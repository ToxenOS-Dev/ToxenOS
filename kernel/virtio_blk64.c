// kernel/virtio_blk64.c — Milestone 28: legacy-transport VirtIO block
// driver. See include/virtio_blk64.h for the design rationale.
#include <stdint.h>
#include "../include/virtio_blk64.h"
#include "../include/pci64.h"
#include "../include/physmem64.h"
#include "../include/heap64.h"
#include "../include/blockdev64.h"
#include "../include/klog.h"

#define VIRTIO_REG_DEVICE_FEATURES 0x00
#define VIRTIO_REG_GUEST_FEATURES  0x04
#define VIRTIO_REG_QUEUE_ADDRESS   0x08
#define VIRTIO_REG_QUEUE_SIZE      0x0C
#define VIRTIO_REG_QUEUE_SELECT    0x0E
#define VIRTIO_REG_QUEUE_NOTIFY    0x10
#define VIRTIO_REG_STATUS          0x12
#define VIRTIO_REG_ISR             0x13
#define VIRTIO_REG_CONFIG          0x14

#define VIRTIO_STATUS_ACKNOWLEDGE 0x01
#define VIRTIO_STATUS_DRIVER      0x02
#define VIRTIO_STATUS_DRIVER_OK   0x04

#define VRING_DESC_F_NEXT  1u
#define VRING_DESC_F_WRITE 2u

#define VIRTIO_BLK_T_IN  0u
#define VIRTIO_BLK_T_OUT 1u

#define VIRTIO_MAX_SECTORS_PER_CMD 8 // 4096 bytes -- matches the single-page control/data buffers below

static inline void outb(uint16_t port, uint8_t val) { __asm__ volatile ("outb %0,%1" :: "a"(val), "Nd"(port)); }
static inline void outw(uint16_t port, uint16_t val) { __asm__ volatile ("outw %0,%1" :: "a"(val), "Nd"(port)); }
static inline void outl(uint16_t port, uint32_t val) { __asm__ volatile ("outl %0,%1" :: "a"(val), "Nd"(port)); }
static inline uint8_t  inb(uint16_t port) { uint8_t r;  __asm__ volatile ("inb %1,%0" : "=a"(r) : "Nd"(port)); return r; }
static inline uint16_t inw(uint16_t port) { uint16_t r; __asm__ volatile ("inw %1,%0" : "=a"(r) : "Nd"(port)); return r; }

typedef struct {
    uint64_t addr;
    uint32_t len;
    uint16_t flags;
    uint16_t next;
} __attribute__((packed)) vring_desc_t;

typedef struct {
    uint32_t type;
    uint32_t reserved;
    uint64_t sector;
} __attribute__((packed)) virtio_blk_req_hdr_t;

typedef struct {
    uint16_t io_base;
    uint16_t queue_size;
    uint8_t* desc_virt;
    uint8_t* avail_virt;
    uint8_t* used_virt;
    uint64_t queue_phys;
    uint32_t queue_pages;
    uint16_t used_idx_seen;
    uint8_t* ctrl_virt;  // one page: [0..15]=request header, [16]=status byte
    uint64_t ctrl_phys;
    uint8_t* data_virt;  // one page: up to 8 sectors (4096 bytes) of transfer data
    uint64_t data_phys;
    blockdev64_t bd;
} virtio_blk_dev_t;

static uint32_t align4k(uint32_t x) { return (x + 4095u) & ~4095u; }

// Builds and submits a single 3-descriptor chain (header -> data ->
// status), notifies the device, and polls the used ring for exactly
// one new entry -- bounded by an iteration-count timeout (matching
// kernel/ata64.c's/kernel/ahci64.c's/kernel/nvme64.c's own precedent).
// Only ever one request outstanding at a time, so "used.idx advanced
// by exactly 1" is a sufficient completion check without tracking
// individual descriptor/request IDs.
static int virtio_blk_issue(virtio_blk_dev_t* vb, uint64_t lba, uint32_t sector_count, int is_write) {
    virtio_blk_req_hdr_t* hdr = (virtio_blk_req_hdr_t*)vb->ctrl_virt;
    hdr->type = is_write ? VIRTIO_BLK_T_OUT : VIRTIO_BLK_T_IN;
    hdr->reserved = 0;
    hdr->sector = lba;
    uint8_t* status = vb->ctrl_virt + 16;
    *status = 0xFF; // sentinel the device must overwrite with 0 on success

    vring_desc_t* desc = (vring_desc_t*)vb->desc_virt;
    desc[0].addr = vb->ctrl_phys;
    desc[0].len  = sizeof(virtio_blk_req_hdr_t);
    desc[0].flags = VRING_DESC_F_NEXT;
    desc[0].next = 1;

    desc[1].addr = vb->data_phys;
    desc[1].len  = sector_count * 512u;
    desc[1].flags = VRING_DESC_F_NEXT | (is_write ? 0u : VRING_DESC_F_WRITE);
    desc[1].next = 2;

    desc[2].addr = vb->ctrl_phys + 16;
    desc[2].len  = 1;
    desc[2].flags = VRING_DESC_F_WRITE;
    desc[2].next = 0;

    uint16_t* avail_idx  = (uint16_t*)(vb->avail_virt + 2);
    uint16_t* avail_ring = (uint16_t*)(vb->avail_virt + 4);
    uint16_t idx = *avail_idx;
    avail_ring[idx % vb->queue_size] = 0; // always chain head = descriptor 0 (single outstanding request)
    *avail_idx = (uint16_t)(idx + 1);

    outw((uint16_t)(vb->io_base + VIRTIO_REG_QUEUE_NOTIFY), 0);

    volatile uint16_t* used_idx_ptr = (volatile uint16_t*)(vb->used_virt + 2);
    uint16_t target = (uint16_t)(vb->used_idx_seen + 1);
    uint32_t timeout = 20000000;
    while (timeout-- && *used_idx_ptr != target) { }
    if (*used_idx_ptr != target) {
        klog("virtio_blk64: command timeout\n");
        return -1;
    }
    vb->used_idx_seen = target;

    if (*status != 0) {
        klog("virtio_blk64: device reported an I/O error\n");
        return -1;
    }
    return 0;
}

static int virtio_bd_read(blockdev64_t* dev, uint64_t lba, uint8_t* buf, uint32_t count) {
    virtio_blk_dev_t* vb = (virtio_blk_dev_t*)dev->driver_data;
    uint32_t done = 0;
    while (done < count) {
        uint32_t chunk = count - done;
        if (chunk > VIRTIO_MAX_SECTORS_PER_CMD) chunk = VIRTIO_MAX_SECTORS_PER_CMD;
        if (virtio_blk_issue(vb, lba + done, chunk, 0) < 0) return -1;
        for (uint32_t i = 0; i < chunk * 512u; i++) buf[(uint64_t)done * 512 + i] = vb->data_virt[i];
        done += chunk;
    }
    return (int)count;
}

static int virtio_bd_write(blockdev64_t* dev, uint64_t lba, const uint8_t* buf, uint32_t count) {
    virtio_blk_dev_t* vb = (virtio_blk_dev_t*)dev->driver_data;
    uint32_t done = 0;
    while (done < count) {
        uint32_t chunk = count - done;
        if (chunk > VIRTIO_MAX_SECTORS_PER_CMD) chunk = VIRTIO_MAX_SECTORS_PER_CMD;
        for (uint32_t i = 0; i < chunk * 512u; i++) vb->data_virt[i] = buf[(uint64_t)done * 512 + i];
        if (virtio_blk_issue(vb, lba + done, chunk, 1) < 0) return -1;
        done += chunk;
    }
    return (int)count;
}

static const blockdev64_ops_t virtio_bd_ops = {
    .read  = virtio_bd_read,
    .write = virtio_bd_write,
};

static void build_name(char* out, int idx) {
    int i = 0;
    const char* p = "vblk";
    while (p[i]) { out[i] = p[i]; i++; }
    out[i++] = (char)('0' + (idx % 10));
    out[i] = 0;
}

static int virtio_blk_init_device(pci64_device_t* dev, int idx) {
    pci64_enable_device(dev);

    pci64_bar_t* bar0 = &dev->bar[0];
    if (bar0->type != PCI64_BAR_IO) {
        klog("virtio_blk64: BAR0 is not an I/O BAR -- modern-only device, unsupported this milestone\n");
        return -1;
    }
    uint16_t io_base = (uint16_t)bar0->address;

    outb((uint16_t)(io_base + VIRTIO_REG_STATUS), 0); // reset
    outb((uint16_t)(io_base + VIRTIO_REG_STATUS), VIRTIO_STATUS_ACKNOWLEDGE);
    outb((uint16_t)(io_base + VIRTIO_REG_STATUS), VIRTIO_STATUS_ACKNOWLEDGE | VIRTIO_STATUS_DRIVER);
    outl((uint16_t)(io_base + VIRTIO_REG_GUEST_FEATURES), 0); // negotiate nothing -- see header comment

    outw((uint16_t)(io_base + VIRTIO_REG_QUEUE_SELECT), 0);
    uint16_t qsz = inw((uint16_t)(io_base + VIRTIO_REG_QUEUE_SIZE));
    if (qsz == 0) {
        klog("virtio_blk64: queue 0 reports size 0 -- skipping device\n");
        return -1;
    }

    uint32_t desc_bytes  = (uint32_t)qsz * 16u;
    uint32_t avail_bytes = 6u + (uint32_t)qsz * 2u;
    uint32_t used_offset = align4k(desc_bytes + avail_bytes);
    uint32_t used_bytes  = 6u + (uint32_t)qsz * 8u;
    uint32_t total_bytes = used_offset + used_bytes;
    uint32_t pages = (total_bytes + 4095u) / 4096u;

    uint64_t queue_phys = physmem64_alloc_pages(pages);
    if (!queue_phys) {
        klog("virtio_blk64: queue memory allocation failed\n");
        return -1;
    }
    uint64_t ctrl_phys = physmem64_alloc_page();
    uint64_t data_phys = ctrl_phys ? physmem64_alloc_page() : 0;
    if (!ctrl_phys || !data_phys) {
        klog("virtio_blk64: control/data buffer allocation failed\n");
        if (ctrl_phys) physmem64_free_page(ctrl_phys);
        physmem64_free_pages(queue_phys, pages);
        return -1;
    }

    virtio_blk_dev_t* vb = (virtio_blk_dev_t*)kmalloc(sizeof(virtio_blk_dev_t));
    if (!vb) {
        klog("virtio_blk64: out of memory\n");
        physmem64_free_page(ctrl_phys);
        physmem64_free_page(data_phys);
        physmem64_free_pages(queue_phys, pages);
        return -1;
    }

    uint8_t* queue_virt = (uint8_t*)physmem64_to_virt(queue_phys);
    vb->io_base      = io_base;
    vb->queue_size   = qsz;
    vb->desc_virt    = queue_virt;
    vb->avail_virt   = queue_virt + desc_bytes;
    vb->used_virt    = queue_virt + used_offset;
    vb->queue_phys   = queue_phys;
    vb->queue_pages  = pages;
    vb->used_idx_seen = 0;
    vb->ctrl_virt    = (uint8_t*)physmem64_to_virt(ctrl_phys);
    vb->ctrl_phys    = ctrl_phys;
    vb->data_virt    = (uint8_t*)physmem64_to_virt(data_phys);
    vb->data_phys    = data_phys;

    outl((uint16_t)(io_base + VIRTIO_REG_QUEUE_ADDRESS), (uint32_t)(queue_phys >> 12));
    outb((uint16_t)(io_base + VIRTIO_REG_STATUS), VIRTIO_STATUS_ACKNOWLEDGE | VIRTIO_STATUS_DRIVER | VIRTIO_STATUS_DRIVER_OK);

    uint64_t capacity = 0;
    for (int i = 0; i < 8; i++) ((uint8_t*)&capacity)[i] = inb((uint16_t)(io_base + VIRTIO_REG_CONFIG + i));
    if (capacity == 0) {
        klog("virtio_blk64: device reports zero capacity -- skipping\n");
        kfree(vb);
        physmem64_free_page(ctrl_phys);
        physmem64_free_page(data_phys);
        physmem64_free_pages(queue_phys, pages);
        return -1;
    }

    vb->bd.type               = BLOCKDEV64_TYPE_VIRTIO;
    vb->bd.logical_block_size = 512;
    vb->bd.block_count        = capacity;
    vb->bd.ops                = &virtio_bd_ops;
    vb->bd.driver_data        = vb;
    build_name(vb->bd.name, idx);

    if (blockdev64_register(&vb->bd) < 0) {
        kfree(vb);
        physmem64_free_page(ctrl_phys);
        physmem64_free_page(data_phys);
        physmem64_free_pages(queue_phys, pages);
        return -1;
    }
    return 0;
}

int virtio_blk64_init(void) {
    int total = 0;
    int idx = 0;
    pci64_device_t* dev = 0;
    while ((dev = pci64_find_device(PCI64_VENDOR_VIRTIO, PCI64_DEVICE_VIRTIO_BLK_LEGACY, dev)) != 0) {
        if (virtio_blk_init_device(dev, idx) == 0) total++;
        idx++;
    }
    return total;
}
