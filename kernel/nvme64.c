// kernel/nvme64.c — Milestone 28: NVMe driver. See include/nvme64.h
// for the design rationale.
#include <stdint.h>
#include "../include/nvme64.h"
#include "../include/pci64.h"
#include "../include/physmem64.h"
#include "../include/heap64.h"
#include "../include/blockdev64.h"
#include "../include/klog.h"

#define NVME_REG_CAP   0x00
#define NVME_REG_VS    0x08
#define NVME_REG_CC    0x14
#define NVME_REG_CSTS  0x1C
#define NVME_REG_AQA   0x24
#define NVME_REG_ASQ   0x28
#define NVME_REG_ACQ   0x30
#define NVME_DOORBELL_BASE 0x1000

#define NVME_OP_DELETE_IOSQ 0x00
#define NVME_OP_CREATE_IOSQ 0x01
#define NVME_OP_DELETE_IOCQ 0x04
#define NVME_OP_CREATE_IOCQ 0x05
#define NVME_OP_IDENTIFY    0x06

#define NVME_IO_OP_WRITE 0x01
#define NVME_IO_OP_READ  0x02

#define NVME_MAX_SECTORS_PER_CMD 8 // 4096 bytes = exactly one page -- see header comment (PRP1-only design)
#define NVME_QUEUE_DEPTH 16

typedef struct {
    uint32_t cdw0;
    uint32_t nsid;
    uint64_t rsv1;
    uint64_t mptr;
    uint64_t prp1;
    uint64_t prp2;
    uint32_t cdw10, cdw11, cdw12, cdw13, cdw14, cdw15;
} __attribute__((packed)) nvme_cmd_t; // 64 bytes

typedef struct {
    uint32_t dw0, dw1;
    uint16_t sq_head;
    uint16_t sq_id;
    uint16_t cid;
    uint16_t status; // bit0 = phase tag, bits1-15 = status field
} __attribute__((packed)) nvme_cqe_t; // 16 bytes

typedef struct {
    void* sq_virt; uint64_t sq_phys; uint32_t sq_pages;
    void* cq_virt; uint64_t cq_phys; uint32_t cq_pages;
    uint16_t sq_tail;
    uint16_t cq_head;
    uint8_t  phase;
    uint16_t depth;
    uint16_t qid;
} nvme_queue_t;

typedef struct {
    volatile uint8_t* regs;
    uint32_t doorbell_stride;
    nvme_queue_t admin_q;
    nvme_queue_t io_q;
    uint8_t* dma_buf_virt;
    uint64_t dma_buf_phys;
    uint16_t next_cid;
} nvme_ctrl_t;

typedef struct {
    nvme_ctrl_t* ctrl;
    uint32_t nsid;
    blockdev64_t bd;
} nvme_ns_state_t;

static inline uint32_t nvme_r32(nvme_ctrl_t* c, uint32_t off) { return *(volatile uint32_t*)(c->regs + off); }
static inline void     nvme_w32(nvme_ctrl_t* c, uint32_t off, uint32_t v) { *(volatile uint32_t*)(c->regs + off) = v; }
static inline uint64_t nvme_r64(nvme_ctrl_t* c, uint32_t off) { return *(volatile uint64_t*)(c->regs + off); }
static inline void     nvme_w64(nvme_ctrl_t* c, uint32_t off, uint64_t v) { *(volatile uint64_t*)(c->regs + off) = v; }

static void nvme_ring_doorbell(nvme_ctrl_t* c, uint16_t qid, int is_cq, uint32_t value) {
    uint32_t offset = NVME_DOORBELL_BASE + (uint32_t)(2 * qid + is_cq) * c->doorbell_stride;
    nvme_w32(c, offset, value);
}

static void nvme_zero_cmd(nvme_cmd_t* cmd) {
    uint32_t* w = (uint32_t*)cmd;
    for (int i = 0; i < (int)(sizeof(nvme_cmd_t) / 4); i++) w[i] = 0;
}

// Submits `cmd` on `q` and polls its matching completion queue for a
// phase-tag flip -- bounded by an iteration-count timeout (matching
// kernel/ata64.c's/kernel/ahci64.c's own precedent), so a wedged or
// missing controller cannot hang the kernel forever. Returns 0 on a
// successful completion (status code 0), or -1 (timeout or non-zero
// status).
static int nvme_submit(nvme_ctrl_t* c, nvme_queue_t* q, nvme_cmd_t* cmd) {
    cmd->cdw0 = (cmd->cdw0 & 0x0000FFFFu) | ((uint32_t)(c->next_cid++) << 16);

    nvme_cmd_t* sq = (nvme_cmd_t*)q->sq_virt;
    sq[q->sq_tail] = *cmd;
    q->sq_tail = (uint16_t)((q->sq_tail + 1) % q->depth);
    nvme_ring_doorbell(c, q->qid, 0, q->sq_tail);

    volatile nvme_cqe_t* cq = (volatile nvme_cqe_t*)q->cq_virt;
    uint32_t timeout = 20000000;
    while (timeout--) {
        uint16_t status = cq[q->cq_head].status;
        if ((status & 1) == q->phase) {
            uint16_t sc = (uint16_t)((status >> 1) & 0x7FFF);
            q->cq_head = (uint16_t)(q->cq_head + 1);
            if (q->cq_head == q->depth) { q->cq_head = 0; q->phase ^= 1; }
            nvme_ring_doorbell(c, q->qid, 1, q->cq_head);
            if (sc != 0) klog("nvme64: command completed with a non-zero status\n");
            return sc == 0 ? 0 : -1;
        }
    }
    klog("nvme64: command timeout\n");
    return -1;
}

static int nvme_alloc_queue(nvme_queue_t* q, uint16_t qid, uint16_t depth) {
    uint64_t sq_bytes = (uint64_t)depth * sizeof(nvme_cmd_t);
    uint64_t cq_bytes = (uint64_t)depth * sizeof(nvme_cqe_t);
    uint32_t sq_pages = (uint32_t)((sq_bytes + 4095) / 4096);
    uint32_t cq_pages = (uint32_t)((cq_bytes + 4095) / 4096);
    if (sq_pages == 0) sq_pages = 1;
    if (cq_pages == 0) cq_pages = 1;

    uint64_t sq_phys = physmem64_alloc_pages(sq_pages);
    uint64_t cq_phys = sq_phys ? physmem64_alloc_pages(cq_pages) : 0;
    if (!sq_phys || !cq_phys) {
        if (sq_phys) physmem64_free_pages(sq_phys, sq_pages);
        if (cq_phys) physmem64_free_pages(cq_phys, cq_pages);
        return -1;
    }

    q->sq_virt = physmem64_to_virt(sq_phys); q->sq_phys = sq_phys; q->sq_pages = sq_pages;
    q->cq_virt = physmem64_to_virt(cq_phys); q->cq_phys = cq_phys; q->cq_pages = cq_pages;
    q->sq_tail = 0; q->cq_head = 0; q->phase = 1; q->depth = depth; q->qid = qid;
    return 0;
}

static void nvme_free_queue(nvme_queue_t* q) {
    if (q->sq_phys) physmem64_free_pages(q->sq_phys, q->sq_pages);
    if (q->cq_phys) physmem64_free_pages(q->cq_phys, q->cq_pages);
}

static int nvme_identify_controller_nn(nvme_ctrl_t* c, uint32_t* nn_out) {
    nvme_cmd_t cmd; nvme_zero_cmd(&cmd);
    cmd.cdw0 = NVME_OP_IDENTIFY;
    cmd.prp1 = c->dma_buf_phys;
    cmd.cdw10 = 1; // CNS=1 -- identify controller
    if (nvme_submit(c, &c->admin_q, &cmd) < 0) return -1;

    *nn_out = *(uint32_t*)(c->dma_buf_virt + 516); // NN field
    return 0;
}

static int nvme_identify_namespace(nvme_ctrl_t* c, uint32_t nsid, uint64_t* nsze_out, uint32_t* lba_size_out) {
    nvme_cmd_t cmd; nvme_zero_cmd(&cmd);
    cmd.cdw0 = NVME_OP_IDENTIFY;
    cmd.nsid = nsid;
    cmd.prp1 = c->dma_buf_phys;
    cmd.cdw10 = 0; // CNS=0 -- identify namespace
    if (nvme_submit(c, &c->admin_q, &cmd) < 0) return -1;

    uint8_t* id = c->dma_buf_virt;
    uint64_t nsze = *(uint64_t*)(id + 0);
    uint8_t flbas = id[26];
    uint8_t lbads = id[128 + (flbas & 0xF) * 4 + 2];

    *nsze_out = nsze;
    *lba_size_out = 1u << lbads;
    return 0;
}

static int nvme_bd_read(blockdev64_t* dev, uint64_t lba, uint8_t* buf, uint32_t count) {
    nvme_ns_state_t* ns = (nvme_ns_state_t*)dev->driver_data;
    nvme_ctrl_t* c = ns->ctrl;
    uint32_t done = 0;
    while (done < count) {
        uint32_t chunk = count - done;
        if (chunk > NVME_MAX_SECTORS_PER_CMD) chunk = NVME_MAX_SECTORS_PER_CMD;

        nvme_cmd_t cmd; nvme_zero_cmd(&cmd);
        cmd.cdw0 = NVME_IO_OP_READ;
        cmd.nsid = ns->nsid;
        cmd.prp1 = c->dma_buf_phys;
        uint64_t start = lba + done;
        cmd.cdw10 = (uint32_t)start;
        cmd.cdw11 = (uint32_t)(start >> 32);
        cmd.cdw12 = chunk - 1; // 0-based count

        if (nvme_submit(c, &c->io_q, &cmd) < 0) return -1;
        for (uint32_t i = 0; i < chunk * 512u; i++) buf[(uint64_t)done * 512 + i] = c->dma_buf_virt[i];
        done += chunk;
    }
    return (int)count;
}

static int nvme_bd_write(blockdev64_t* dev, uint64_t lba, const uint8_t* buf, uint32_t count) {
    nvme_ns_state_t* ns = (nvme_ns_state_t*)dev->driver_data;
    nvme_ctrl_t* c = ns->ctrl;
    uint32_t done = 0;
    while (done < count) {
        uint32_t chunk = count - done;
        if (chunk > NVME_MAX_SECTORS_PER_CMD) chunk = NVME_MAX_SECTORS_PER_CMD;

        for (uint32_t i = 0; i < chunk * 512u; i++) c->dma_buf_virt[i] = buf[(uint64_t)done * 512 + i];

        nvme_cmd_t cmd; nvme_zero_cmd(&cmd);
        cmd.cdw0 = NVME_IO_OP_WRITE;
        cmd.nsid = ns->nsid;
        cmd.prp1 = c->dma_buf_phys;
        uint64_t start = lba + done;
        cmd.cdw10 = (uint32_t)start;
        cmd.cdw11 = (uint32_t)(start >> 32);
        cmd.cdw12 = chunk - 1;

        if (nvme_submit(c, &c->io_q, &cmd) < 0) return -1;
        done += chunk;
    }
    return (int)count;
}

static const blockdev64_ops_t nvme_bd_ops = {
    .read  = nvme_bd_read,
    .write = nvme_bd_write,
};

static void build_name(char* out, int ctrl_idx, uint32_t nsid) {
    int i = 0;
    const char* p = "nvme";
    while (p[i]) { out[i] = p[i]; i++; }
    out[i++] = (char)('0' + (ctrl_idx % 10));
    out[i++] = 'n';
    out[i++] = (char)('0' + (nsid % 10));
    if (nsid >= 10) out[i++] = (char)('0' + (nsid / 10) % 10);
    out[i] = 0;
}

static int nvme_init_controller(pci64_device_t* dev, int ctrl_idx) {
    pci64_enable_device(dev);

    pci64_bar_t* bar0 = &dev->bar[0];
    if (bar0->type != PCI64_BAR_MEM32 && bar0->type != PCI64_BAR_MEM64) {
        klog("nvme64: BAR0 is not MMIO -- skipping controller\n");
        return 0;
    }
    uint64_t bar_size = bar0->size;
    if (bar_size < 0x2000) bar_size = 0x2000;

    void* regs = physmem64_map_mmio(bar0->address, bar_size);
    if (!regs) { klog("nvme64: failed to map BAR0\n"); return 0; }

    nvme_ctrl_t* c = (nvme_ctrl_t*)kmalloc(sizeof(nvme_ctrl_t));
    if (!c) { klog("nvme64: out of memory\n"); return 0; }
    c->regs = (volatile uint8_t*)regs;
    c->next_cid = 1;
    c->io_q.sq_phys = 0; c->io_q.cq_phys = 0; // so a partial-failure cleanup never double-frees

    uint64_t cap = nvme_r64(c, NVME_REG_CAP);
    c->doorbell_stride = 4u << ((cap >> 32) & 0xF);
    uint32_t mqes = (uint32_t)(cap & 0xFFFFu) + 1;
    uint16_t depth = (uint16_t)(mqes < NVME_QUEUE_DEPTH ? mqes : NVME_QUEUE_DEPTH);
    if (depth < 2) depth = 2;

    // Reset: CC.EN=0, wait CSTS.RDY==0.
    nvme_w32(c, NVME_REG_CC, nvme_r32(c, NVME_REG_CC) & ~1u);
    uint32_t timeout = 5000000;
    while (timeout-- && (nvme_r32(c, NVME_REG_CSTS) & 1)) { }
    if (nvme_r32(c, NVME_REG_CSTS) & 1) {
        klog("nvme64: controller would not reset (CSTS.RDY stuck)\n");
        kfree(c);
        return 0;
    }

    if (nvme_alloc_queue(&c->admin_q, 0, depth) < 0) {
        klog("nvme64: admin queue allocation failed\n");
        kfree(c);
        return 0;
    }

    nvme_w32(c, NVME_REG_AQA, ((uint32_t)(depth - 1) << 16) | (uint32_t)(depth - 1));
    nvme_w64(c, NVME_REG_ASQ, c->admin_q.sq_phys);
    nvme_w64(c, NVME_REG_ACQ, c->admin_q.cq_phys);

    uint32_t cc = 1u              // EN
                | (0u << 4)       // CSS = NVM command set
                | (0u << 7)       // MPS = 0 -> 4KB pages
                | (0u << 11)      // AMS = round robin
                | (6u << 16)      // IOSQES = 2^6 = 64 bytes
                | (4u << 20);     // IOCQES = 2^4 = 16 bytes
    nvme_w32(c, NVME_REG_CC, cc);

    timeout = 5000000;
    while (timeout-- && !(nvme_r32(c, NVME_REG_CSTS) & 1)) { }
    if (!(nvme_r32(c, NVME_REG_CSTS) & 1)) {
        klog("nvme64: controller did not become ready\n");
        nvme_free_queue(&c->admin_q);
        kfree(c);
        return 0;
    }

    uint64_t dma_phys = physmem64_alloc_page();
    if (!dma_phys) {
        klog("nvme64: DMA buffer allocation failed\n");
        nvme_free_queue(&c->admin_q);
        kfree(c);
        return 0;
    }
    c->dma_buf_phys = dma_phys;
    c->dma_buf_virt = (uint8_t*)physmem64_to_virt(dma_phys);

    // One I/O queue pair, qid=1: create the CQ first (the SQ's
    // CREATE_IOSQ command references it by CQID).
    if (nvme_alloc_queue(&c->io_q, 1, depth) < 0) {
        klog("nvme64: I/O queue allocation failed\n");
        physmem64_free_page(dma_phys);
        nvme_free_queue(&c->admin_q);
        kfree(c);
        return 0;
    }

    nvme_cmd_t cmd;
    nvme_zero_cmd(&cmd);
    cmd.cdw0 = NVME_OP_CREATE_IOCQ;
    cmd.prp1 = c->io_q.cq_phys;
    cmd.cdw10 = ((uint32_t)(depth - 1) << 16) | c->io_q.qid;
    cmd.cdw11 = 0x1; // PC=1 (physically contiguous), interrupts disabled (polling)
    if (nvme_submit(c, &c->admin_q, &cmd) < 0) {
        klog("nvme64: CREATE_IOCQ failed\n");
        nvme_free_queue(&c->io_q);
        physmem64_free_page(dma_phys);
        nvme_free_queue(&c->admin_q);
        kfree(c);
        return 0;
    }

    nvme_zero_cmd(&cmd);
    cmd.cdw0 = NVME_OP_CREATE_IOSQ;
    cmd.prp1 = c->io_q.sq_phys;
    cmd.cdw10 = ((uint32_t)(depth - 1) << 16) | c->io_q.qid;
    cmd.cdw11 = ((uint32_t)c->io_q.qid << 16) | 0x1; // CQID | PC=1
    if (nvme_submit(c, &c->admin_q, &cmd) < 0) {
        klog("nvme64: CREATE_IOSQ failed\n");
        nvme_free_queue(&c->io_q);
        physmem64_free_page(dma_phys);
        nvme_free_queue(&c->admin_q);
        kfree(c);
        return 0;
    }

    uint32_t nn = 0;
    if (nvme_identify_controller_nn(c, &nn) < 0 || nn == 0) {
        klog("nvme64: IDENTIFY CONTROLLER failed\n");
        nvme_free_queue(&c->io_q);
        physmem64_free_page(dma_phys);
        nvme_free_queue(&c->admin_q);
        kfree(c);
        return 0;
    }

    int registered = 0;
    for (uint32_t nsid = 1; nsid <= nn; nsid++) {
        uint64_t nsze; uint32_t lba_size;
        if (nvme_identify_namespace(c, nsid, &nsze, &lba_size) < 0 || nsze == 0) continue;
        if (lba_size != 512) {
            klog("nvme64: namespace has a non-512-byte native block size -- unsupported this milestone, skipping\n");
            continue;
        }

        nvme_ns_state_t* ns = (nvme_ns_state_t*)kmalloc(sizeof(nvme_ns_state_t));
        if (!ns) { klog("nvme64: out of memory for namespace state\n"); continue; }
        ns->ctrl = c;
        ns->nsid = nsid;
        ns->bd.type               = BLOCKDEV64_TYPE_NVME;
        ns->bd.logical_block_size = lba_size;
        ns->bd.block_count        = nsze;
        ns->bd.ops                = &nvme_bd_ops;
        ns->bd.driver_data        = ns;
        build_name(ns->bd.name, ctrl_idx, nsid);

        if (blockdev64_register(&ns->bd) == 0) registered++;
        else kfree(ns);
    }

    if (registered == 0) {
        // No usable namespace -- tear everything down rather than
        // leaving an initialized-but-unused controller's DMA memory
        // permanently allocated.
        nvme_free_queue(&c->io_q);
        physmem64_free_page(dma_phys);
        nvme_free_queue(&c->admin_q);
        kfree(c);
    }
    return registered;
}

int nvme64_init(void) {
    int total = 0;
    int ctrl_idx = 0;
    pci64_device_t* dev = 0;
    while ((dev = pci64_find_class(PCI64_CLASS_STORAGE, PCI64_SUBCLASS_NVME, PCI64_PROGIF_NVME, dev)) != 0) {
        total += nvme_init_controller(dev, ctrl_idx);
        ctrl_idx++;
    }
    return total;
}
