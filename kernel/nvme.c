// ToxenOS/kernel/nvme.c — NVMe (PCIe SSD) driver
// Supports one NVMe controller, one namespace, read/write via I/O queues.
#include <stdint.h>
#include "../include/pci.h"
#include "../include/klog.h"
#include "../include/paging.h"
#include "../include/memmap.h"

// ── PCI identifiers ───────────────────────────────────────────────────────────
#define PCI_CLASS_STORAGE   0x01
#define PCI_SUBCLASS_NVME   0x08

// ── Controller register offsets (from BAR0) ───────────────────────────────────
#define NVME_CAP        0x00   // Controller Capabilities (64-bit)
#define NVME_VS         0x08   // Version
#define NVME_CC         0x14   // Controller Configuration
#define NVME_CSTS       0x1C   // Controller Status
#define NVME_AQA        0x24   // Admin Queue Attributes
#define NVME_ASQ        0x28   // Admin Submission Queue (64-bit, lo)
#define NVME_ASQ_HI     0x2C   // Admin Submission Queue (hi)
#define NVME_ACQ        0x30   // Admin Completion Queue (lo)
#define NVME_ACQ_HI     0x34   // Admin Completion Queue (hi)

#define NVME_CC_EN      (1u<<0)
#define NVME_CC_CSS     (0u<<4)    // NVM command set
#define NVME_CC_MPS     (0u<<7)    // 4KB memory page size (2^(12+0))
#define NVME_CC_AMS     (0u<<11)   // Round-robin arbitration
#define NVME_CC_IOSQES  (6u<<16)   // I/O SQ entry size = 2^6 = 64B
#define NVME_CC_IOCQES  (4u<<20)   // I/O CQ entry size = 2^4 = 16B
#define NVME_CSTS_RDY   (1u<<0)

// ── Queue depths ──────────────────────────────────────────────────────────────
#define NVME_AQ_DEPTH   16   // admin queue depth
#define NVME_IOQ_DEPTH  16   // I/O queue depth

// ── NVMe opcodes ──────────────────────────────────────────────────────────────
#define NVME_ADMIN_CREATE_IOCQ  0x05
#define NVME_ADMIN_CREATE_IOSQ  0x01
#define NVME_ADMIN_IDENTIFY     0x06
#define NVME_CMD_READ           0x02
#define NVME_CMD_WRITE          0x01

// ── Submission Queue Entry (64 bytes) ─────────────────────────────────────────
typedef struct __attribute__((packed)) {
    uint32_t cdw0;      // opcode[7:0], fuse[9:8], psdt[15:14], cid[31:16]
    uint32_t nsid;
    uint64_t reserved;
    uint64_t mptr;
    uint64_t prp1;
    uint64_t prp2;
    uint32_t cdw10, cdw11, cdw12, cdw13, cdw14, cdw15;
} nvme_sq_entry_t;  // 64 bytes

// ── Completion Queue Entry (16 bytes) ─────────────────────────────────────────
typedef struct __attribute__((packed)) {
    uint32_t result;
    uint32_t reserved;
    uint16_t sq_head;
    uint16_t sq_id;
    uint16_t cid;
    uint16_t status;   // bit 0 = phase, bits 15:1 = status code
} nvme_cq_entry_t;  // 16 bytes

// ── Static buffers (aligned, in first 4GB so upper PRP bits = 0) ─────────────
static nvme_sq_entry_t asq[NVME_AQ_DEPTH]  __attribute__((aligned(4096)));
static nvme_cq_entry_t acq[NVME_AQ_DEPTH]  __attribute__((aligned(4096)));
static nvme_sq_entry_t iosq[NVME_IOQ_DEPTH] __attribute__((aligned(4096)));
static nvme_cq_entry_t iocq[NVME_IOQ_DEPTH] __attribute__((aligned(4096)));
static uint8_t         dma_buf[65536]        __attribute__((aligned(4096)));
static uint8_t         identify_buf[4096]    __attribute__((aligned(4096)));

// ── Driver state ──────────────────────────────────────────────────────────────
static int      nvme_ok       = 0;
static uint32_t nvme_base     = 0;   // BAR0 virtual address (identity mapped)
static uint32_t nvme_nsid     = 1;   // namespace ID (usually 1)
static uint32_t nvme_lba_size = 512; // bytes per LBA (from IDENTIFY namespace)
static uint64_t nvme_lba_count = 0;  // total LBA count
static uint32_t nvme_db_stride = 0;  // doorbell stride in bytes

// Admin queue tracking
static uint16_t asq_tail = 0;
static uint16_t acq_head = 0;
static uint8_t  acq_phase = 1;

// I/O queue tracking
static uint16_t iosq_tail = 0;
static uint16_t iocq_head = 0;
static uint8_t  iocq_phase = 1;
static uint16_t io_cid = 0;

// ── Register access ───────────────────────────────────────────────────────────
#define REG32(off)  (*((volatile uint32_t*)(nvme_base + (off))))
#define REG64LO(off) (*((volatile uint32_t*)(nvme_base + (off))))
#define REG64HI(off) (*((volatile uint32_t*)(nvme_base + (off) + 4)))

static uint32_t db_offset(uint16_t qid, int is_cq) {
    return 0x1000 + ((2*qid + is_cq) * nvme_db_stride);
}

// ── Admin command helpers ─────────────────────────────────────────────────────

static int nvme_wait_ready(int ready) {
    uint32_t to = 2000000;
    while (--to) {
        uint32_t csts = REG32(NVME_CSTS);
        if (ready  && (csts & NVME_CSTS_RDY)) return 0;
        if (!ready && !(csts & NVME_CSTS_RDY)) return 0;
    }
    return -1;
}

// Submit one admin command and wait for completion
static int nvme_admin_cmd(nvme_sq_entry_t* cmd) {
    uint16_t cid = asq_tail;
    cmd->cdw0 = (cmd->cdw0 & 0xFFFF) | ((uint32_t)cid << 16);

    asq[asq_tail] = *cmd;
    asq_tail = (asq_tail + 1) % NVME_AQ_DEPTH;
    REG32(db_offset(0, 0)) = asq_tail; // ring doorbell

    // Poll CQ
    uint32_t to = 2000000;
    while (--to) {
        if ((acq[acq_head].status & 1) == acq_phase) {
            uint16_t sc = (acq[acq_head].status >> 1) & 0x7FFF;
            acq_head = (acq_head + 1) % NVME_AQ_DEPTH;
            if (acq_head == 0) acq_phase ^= 1;
            REG32(db_offset(0, 1)) = acq_head; // update CQ head
            return (sc == 0) ? 0 : -1;
        }
    }
    return -1; // timeout
}

// ── Init ──────────────────────────────────────────────────────────────────────

int nvme_init(void) {
    // Find NVMe controller via PCI
    pci_device_t* dev = 0;
    for (int i = 0; i < pci_device_count; i++) {
        if (pci_devices[i].class_code == PCI_CLASS_STORAGE &&
            pci_devices[i].subclass   == PCI_SUBCLASS_NVME) {
            dev = &pci_devices[i];
            break;
        }
    }
    if (!dev) { klog("NVMe: no controller found\n"); return -1; }

    // BAR0 — might be 64-bit BAR (type=2), use lower 32 bits only
    uint32_t bar0 = dev->bar[0];
    int bar_type = (bar0 >> 1) & 0x3;
    uint32_t base = bar0 & ~0xFu;
    if (bar_type == 2) {
        // 64-bit BAR: upper 32 in bar[1]. We only support lower 4GB.
        if (dev->bar[1] != 0) { klog("NVMe: BAR above 4GB, skipping\n"); return -1; }
    }
    if (!base) { klog("NVMe: invalid BAR0\n"); return -1; }

    pci_enable(dev);

    // Map BAR0 as uncacheable MMIO (PAGE_CD prevents stale cache reads on MMIO)
    extern uint32_t kernel_directory[];
    for (uint32_t off = 0; off < 0x10000; off += 0x1000)
        paging_map(kernel_directory, base+off, base+off,
                   PAGE_PRESENT | PAGE_WRITABLE | PAGE_CD);
    nvme_base = base;

    // Read doorbell stride: CAP[35:32] = DSTRD, stride = 4 << DSTRD
    uint32_t cap_lo = REG32(NVME_CAP);
    uint32_t cap_hi = REG64HI(NVME_CAP);
    uint32_t dstrd  = (cap_hi >> 0) & 0xF;
    nvme_db_stride  = 4u << dstrd;

    // Disable controller
    REG32(NVME_CC) = 0;
    if (nvme_wait_ready(0) < 0) { klog("NVMe: disable timeout\n"); return -1; }

    // Zero queues
    for (int i=0;i<NVME_AQ_DEPTH;i++) {
        uint8_t* p=(uint8_t*)&asq[i]; for(int j=0;j<64;j++) p[j]=0;
        uint8_t* q=(uint8_t*)&acq[i]; for(int j=0;j<16;j++) q[j]=0;
    }
    asq_tail=0; acq_head=0; acq_phase=1;

    // Set admin queue base addresses (physical)
    uint32_t asq_phys = (uint32_t)asq - KERNEL_VIRT_BASE;
    uint32_t acq_phys = (uint32_t)acq - KERNEL_VIRT_BASE;
    REG32(NVME_ASQ)    = asq_phys;  REG32(NVME_ASQ_HI) = 0;
    REG32(NVME_ACQ)    = acq_phys;  REG32(NVME_ACQ_HI) = 0;
    REG32(NVME_AQA)    = ((NVME_AQ_DEPTH-1)<<16) | (NVME_AQ_DEPTH-1);

    // Enable controller with NVM command set
    REG32(NVME_CC) = NVME_CC_EN | NVME_CC_CSS | NVME_CC_MPS |
                     NVME_CC_AMS | NVME_CC_IOSQES | NVME_CC_IOCQES;
    if (nvme_wait_ready(1) < 0) { klog("NVMe: enable timeout\n"); return -1; }

    // IDENTIFY controller
    {
        nvme_sq_entry_t cmd = {0};
        cmd.cdw0 = NVME_ADMIN_IDENTIFY;
        cmd.nsid = 0;
        cmd.prp1 = (uint32_t)identify_buf - KERNEL_VIRT_BASE;
        cmd.cdw10 = 1; // CNS=1: identify controller
        if (nvme_admin_cmd(&cmd) < 0) { klog("NVMe: IDENTIFY failed\n"); return -1; }
        // Model number is at bytes 24..63 of identify data
        klog("NVMe: ");
        char model[41]; int mi=0;
        for(int i=24;i<64&&mi<40;i++) if(identify_buf[i]>' ') model[mi++]=identify_buf[i];
        model[mi]=0;
        // trim trailing spaces
        while(mi>0&&model[mi-1]==' ') model[--mi]=0;
        if(mi>0){klog(model);} else klog("(unknown)");
        klog("\n");
    }

    // IDENTIFY namespace 1
    {
        nvme_sq_entry_t cmd = {0};
        cmd.cdw0 = NVME_ADMIN_IDENTIFY;
        cmd.nsid = nvme_nsid;
        cmd.prp1 = (uint32_t)identify_buf - KERNEL_VIRT_BASE;
        cmd.cdw10 = 0; // CNS=0: identify namespace
        if (nvme_admin_cmd(&cmd) < 0) { klog("NVMe: NS IDENTIFY failed\n"); return -1; }
        // NSIZE at bytes 0-7, NCAP at 8-15, FLBAS at 26
        uint64_t* ns = (uint64_t*)identify_buf;
        nvme_lba_count = ns[0];
        uint8_t flbas = identify_buf[26] & 0xF;
        uint32_t lbads = identify_buf[128 + flbas*4 + 3]; // LBA data size = 2^lbads
        nvme_lba_size = (lbads >= 9) ? (1u << lbads) : 512;
        klog("NVMe: lba_size="); klog(nvme_lba_size==512?"512\n":"4096\n");
    }

    // Create I/O Completion Queue (QTYPE=1)
    {
        uint32_t iocq_phys = (uint32_t)iocq - KERNEL_VIRT_BASE;
        for(int i=0;i<NVME_IOQ_DEPTH;i++){uint8_t* p=(uint8_t*)&iocq[i];for(int j=0;j<16;j++) p[j]=0;}
        iocq_head=0; iocq_phase=1;
        nvme_sq_entry_t cmd = {0};
        cmd.cdw0  = NVME_ADMIN_CREATE_IOCQ;
        cmd.prp1  = iocq_phys;
        cmd.cdw10 = ((NVME_IOQ_DEPTH-1)<<16) | 1; // QSIZE | QID=1
        cmd.cdw11 = 1; // PC=1 (physically contiguous)
        if (nvme_admin_cmd(&cmd) < 0) { klog("NVMe: create IOCQ failed\n"); return -1; }
    }

    // Create I/O Submission Queue (QTYPE=0)
    {
        uint32_t iosq_phys = (uint32_t)iosq - KERNEL_VIRT_BASE;
        for(int i=0;i<NVME_IOQ_DEPTH;i++){uint8_t* p=(uint8_t*)&iosq[i];for(int j=0;j<64;j++) p[j]=0;}
        iosq_tail=0;
        nvme_sq_entry_t cmd = {0};
        cmd.cdw0  = NVME_ADMIN_CREATE_IOSQ;
        cmd.prp1  = iosq_phys;
        cmd.cdw10 = ((NVME_IOQ_DEPTH-1)<<16) | 1; // QSIZE | QID=1
        cmd.cdw11 = (1<<16) | 1; // CQID=1 | PC=1
        if (nvme_admin_cmd(&cmd) < 0) { klog("NVMe: create IOSQ failed\n"); return -1; }
    }

    nvme_ok = 1;
    klog("NVMe: ready\n");
    return 0;
}

// ── I/O ───────────────────────────────────────────────────────────────────────

// copy_limit: max bytes to copy into buf (prevents overflow when nvme_lba_size > 512)
static int nvme_io(uint64_t lba, uint8_t* buf, uint32_t sectors, int write,
                   uint32_t copy_limit) {
    if (!nvme_ok) return -1;

    // Use DMA buffer for each chunk
    while (sectors > 0) {
        uint32_t chunk = sectors > 128 ? 128 : sectors;
        uint32_t bytes = chunk * nvme_lba_size;

        if (write) {
            uint32_t n = bytes < copy_limit ? bytes : copy_limit;
            for (uint32_t i = 0; i < n; i++) dma_buf[i] = buf[i];
        }

        uint16_t cid = ++io_cid;
        nvme_sq_entry_t* cmd = &iosq[iosq_tail];
        uint8_t* cp = (uint8_t*)cmd; for(int i=0;i<64;i++) cp[i]=0;

        cmd->cdw0 = (write ? NVME_CMD_WRITE : NVME_CMD_READ) | ((uint32_t)cid<<16);
        cmd->nsid = nvme_nsid;
        cmd->prp1 = (uint32_t)dma_buf - KERNEL_VIRT_BASE;
        cmd->cdw10= (uint32_t)(lba & 0xFFFFFFFF);
        cmd->cdw11= (uint32_t)(lba >> 32);
        cmd->cdw12= chunk - 1; // 0-based count

        iosq_tail = (iosq_tail + 1) % NVME_IOQ_DEPTH;
        REG32(db_offset(1, 0)) = iosq_tail;

        // Poll IOCQ
        uint32_t to = 2000000;
        while (--to) {
            if ((iocq[iocq_head].status & 1) == iocq_phase) {
                uint16_t sc = (iocq[iocq_head].status >> 1) & 0x7FFF;
                iocq_head = (iocq_head+1) % NVME_IOQ_DEPTH;
                if (iocq_head == 0) iocq_phase ^= 1;
                REG32(db_offset(1, 1)) = iocq_head;
                if (sc != 0) return -1;
                break;
            }
        }
        if (!to) return -1;

        if (!write) {
            uint32_t n = bytes < copy_limit ? bytes : copy_limit;
            for (uint32_t i = 0; i < n; i++) buf[i] = dma_buf[i];
            copy_limit -= n;
        }

        buf     += bytes;
        lba     += chunk;
        sectors -= chunk;
    }
    return 0;
}

// Public interface — same signature as ata_read_drive / ata_write_drive
// NVMe uses 512-byte logical sectors by default (or 4096 for 4Kn drives)
int nvme_read_drive(uint8_t drive, uint32_t lba, uint8_t* buf, uint32_t sectors) {
    (void)drive;
    if (!nvme_ok) return -1;
    uint32_t ratio = nvme_lba_size / 512;
    if (ratio == 0) ratio = 1;
    uint32_t copy_limit = sectors * 512;
    int r = nvme_io(lba / ratio, buf, sectors / ratio + (sectors%ratio?1:0), 0, copy_limit);
    return (r < 0) ? -1 : (int)sectors;
}

int nvme_write_drive(uint8_t drive, uint32_t lba, const uint8_t* buf, uint32_t sectors) {
    (void)drive;
    if (!nvme_ok) return -1;
    uint32_t ratio = nvme_lba_size / 512;
    if (ratio == 0) ratio = 1;
    uint32_t copy_limit = sectors * 512;
    int r = nvme_io(lba / ratio, (uint8_t*)buf, sectors / ratio + (sectors%ratio?1:0), 1,
                    copy_limit);
    return (r < 0) ? -1 : (int)sectors;
}

uint32_t nvme_get_sectors(uint8_t drive) {
    (void)drive;
    if (!nvme_ok) return 0;
    // Convert NVMe LBAs to 512-byte sectors
    return (uint32_t)(nvme_lba_count * (nvme_lba_size / 512));
}
