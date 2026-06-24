// kernel/virtio_blk.c — Legacy VirtIO block device (PCI 0x1AF4:0x1001)
// QEMU: -drive file=disk.img,if=virtio
// Single in-flight synchronous request, polling only (no IRQ).
#include <stdint.h>
#include "../include/pci.h"
#include "../include/klog.h"
#include "../include/memmap.h"

// Virtual → physical for DMA: kernel is higher-half (linked at 0xC0000000)
#define V2P(v)  KVIRT_TO_PHYS((uint32_t)(v))

// ── Legacy VirtIO I/O port offsets (BAR0) ────────────────────────────────
#define VTIO_DEV_FEATURES  0x00
#define VTIO_GUE_FEATURES  0x04
#define VTIO_QUEUE_PFN     0x08   // 32-bit PFN of virtqueue (phys >> 12)
#define VTIO_QUEUE_SIZE    0x0C   // 16-bit, read-only after reset
#define VTIO_QUEUE_SELECT  0x0E
#define VTIO_QUEUE_NOTIFY  0x10
#define VTIO_STATUS        0x12
#define VTIO_ISR           0x13
#define VTIO_CONFIG        0x14   // block: uint64_t capacity in sectors

#define VTIO_S_ACK         0x01
#define VTIO_S_DRIVER      0x02
#define VTIO_S_DRIVER_OK   0x04

#define VRING_F_NEXT       0x01
#define VRING_F_WRITE      0x02

#define VIRTIO_BLK_T_IN    0
#define VIRTIO_BLK_T_OUT   1

// ── Virtqueue descriptor (fixed 16-byte layout) ───────────────────────────
typedef struct {
    uint64_t addr;   // physical address
    uint32_t len;
    uint16_t flags;
    uint16_t next;
} __attribute__((packed)) vdesc_t;

// ── Block request header ──────────────────────────────────────────────────
typedef struct {
    uint32_t type;
    uint32_t reserved;
    uint64_t sector;
} __attribute__((packed)) blk_hdr_t;

// ── Virtqueue memory ──────────────────────────────────────────────────────
// Ring offsets depend on device-reported queue size (read at init).
// Worst-case qsize=256: desc=4096 + avail=516 → used at 8192, used=2052
// → 10244 bytes needed; 12288 (3 pages) is safe for qsize ≤ 256.
// We support up to qsize=512 with 16384 (4 pages) to be future-proof.
#define VQ_MEM_SIZE 16384
static uint8_t vq_mem[VQ_MEM_SIZE] __attribute__((aligned(4096)));

// Ring base pointers — set during init from actual device queue size
static vdesc_t* g_desc;
static uint8_t* g_avail;   // raw pointer; access via AVAIL_IDX / AVAIL_RING
static uint8_t* g_used;    // raw pointer; access via USED_IDX

// Available ring field accessors
#define AVAIL_IDX     (*(volatile uint16_t*)(g_avail + 2))
#define AVAIL_RING(n) (*(uint16_t*)(g_avail + 4 + (n)*2))

// Used ring idx (device writes here on completion)
#define USED_IDX      (*(volatile uint16_t*)(g_used + 2))

// ── Per-request scratch (must not be on stack — needs stable physical addr) ─
static blk_hdr_t g_hdr;
static uint8_t   g_status;

#define MAX_CHUNK 128
static uint8_t g_data[MAX_CHUNK * 512] __attribute__((aligned(512)));

// ── State ─────────────────────────────────────────────────────────────────
static uint16_t g_io_base       = 0;
static int      g_ok            = 0;
static uint16_t g_avail_idx     = 0;
static uint16_t g_last_used     = 0;
static uint32_t g_total_sectors = 0;
static uint16_t g_qsize         = 0;

// ── I/O port helpers ──────────────────────────────────────────────────────
static inline void outb(uint16_t p,uint8_t  v){__asm__ volatile("outb %0,%1"::"a"(v),"Nd"(p));}
static inline void outw(uint16_t p,uint16_t v){__asm__ volatile("outw %0,%1"::"a"(v),"Nd"(p));}
static inline void outl(uint16_t p,uint32_t v){__asm__ volatile("outl %0,%1"::"a"(v),"Nd"(p));}
static inline uint8_t  inb(uint16_t p){uint8_t  v;__asm__ volatile("inb %1,%0":"=a"(v):"Nd"(p));return v;}
static inline uint16_t inw(uint16_t p){uint16_t v;__asm__ volatile("inw %1,%0":"=a"(v):"Nd"(p));return v;}
static inline uint32_t inl(uint16_t p){uint32_t v;__asm__ volatile("inl %1,%0":"=a"(v):"Nd"(p));return v;}

// ── Single synchronous I/O request ───────────────────────────────────────
static int virtio_do_io(uint32_t type, uint64_t sector, uint8_t* buf, uint32_t nsectors) {
    uint32_t data_len = nsectors * 512;

    g_hdr.type     = type;
    g_hdr.reserved = 0;
    g_hdr.sector   = sector;
    g_status       = 0xFF;

    if (type == VIRTIO_BLK_T_OUT)
        for (uint32_t i = 0; i < data_len; i++) g_data[i] = buf[i];

    // Chain: desc[0]=header → desc[1]=data → desc[2]=status
    g_desc[0].addr  = V2P(&g_hdr);
    g_desc[0].len   = sizeof(blk_hdr_t);
    g_desc[0].flags = VRING_F_NEXT;
    g_desc[0].next  = 1;

    g_desc[1].addr  = V2P(g_data);
    g_desc[1].len   = data_len;
    g_desc[1].flags = VRING_F_NEXT | (type == VIRTIO_BLK_T_IN ? VRING_F_WRITE : 0);
    g_desc[1].next  = 2;

    g_desc[2].addr  = V2P(&g_status);
    g_desc[2].len   = 1;
    g_desc[2].flags = VRING_F_WRITE;
    g_desc[2].next  = 0;

    // Post descriptor chain head into available ring
    uint16_t slot = g_avail_idx % g_qsize;
    AVAIL_RING(slot) = 0;                               // head descriptor index
    __asm__ volatile("" ::: "memory");
    AVAIL_IDX = (uint16_t)(g_avail_idx + 1);
    g_avail_idx++;
    __asm__ volatile("" ::: "memory");

    outw((uint16_t)(g_io_base + VTIO_QUEUE_NOTIFY), 0); // kick queue 0

    // Poll for completion — pause hint for KVM; 10M iterations ~ several seconds
    for (int t = 10000000; t > 0; t--) {
        __asm__ volatile("pause" ::: "memory");
        if (USED_IDX != g_last_used) {
            g_last_used = USED_IDX;
            goto done;
        }
    }
    // Timeout: drain any stale used-ring entry so future requests aren't misled
    g_last_used = USED_IDX;
    klog("virtio_blk: I/O timeout\n");
    return -1;

done:
    if (g_status != 0) {
        char msg[64];
        int i = 0;
        static const char *hex = "0123456789ABCDEF";
        const char *pre = "virtio_blk: err st=0x";
        while (*pre) msg[i++] = *pre++;
        msg[i++] = hex[(g_status >> 4) & 0xF];
        msg[i++] = hex[g_status & 0xF];
        const char *s2 = " sec=0x";
        while (*s2) msg[i++] = *s2++;
        // print lower 32 bits of sector as hex (upper bits always 0 on this OS)
        uint32_t lo = (uint32_t)sector;
        for (int shift = 28; shift >= 0; shift -= 4)
            msg[i++] = hex[(lo >> shift) & 0xF];
        msg[i++] = '\n'; msg[i] = 0;
        klog(msg);
        return -1;
    }
    if (type == VIRTIO_BLK_T_IN)
        for (uint32_t i = 0; i < data_len; i++) buf[i] = g_data[i];
    return (int)nsectors;
}

// ── Public API ────────────────────────────────────────────────────────────
int virtio_blk_init(void) {
    pci_device_t* dev = pci_find(PCI_VENDOR_VIRTIO, PCI_DEV_VIRTIO_BLK);
    if (!dev) return -1;

    pci_enable(dev);
    g_io_base = (uint16_t)pci_bar_io(dev, 0);
    if (!g_io_base) { klog("virtio_blk: bad BAR0\n"); return -1; }

    // Reset, then acknowledge
    outb((uint16_t)(g_io_base + VTIO_STATUS), 0);
    outb((uint16_t)(g_io_base + VTIO_STATUS), VTIO_S_ACK | VTIO_S_DRIVER);

    // Feature negotiation: accept nothing (legacy minimal operation)
    outl((uint16_t)(g_io_base + VTIO_GUE_FEATURES), 0);

    // Select queue 0 and read its actual size
    outw((uint16_t)(g_io_base + VTIO_QUEUE_SELECT), 0);
    g_qsize = inw((uint16_t)(g_io_base + VTIO_QUEUE_SIZE));
    if (g_qsize == 0) { klog("virtio_blk: queue size 0\n"); return -1; }

    // Compute ring offsets from actual queue size (per legacy VirtIO spec):
    //   avail ring starts after descriptor table
    //   used  ring starts at next 4096-byte boundary after avail ring
    uint32_t avail_off = (uint32_t)g_qsize * 16;
    uint32_t avail_end = avail_off + 4u + (uint32_t)g_qsize * 2;
    uint32_t used_off  = (avail_end + 4095u) & ~4095u;
    uint32_t used_end  = used_off + 4u + (uint32_t)g_qsize * 8;

    if (used_end > VQ_MEM_SIZE) {
        klog("virtio_blk: queue too large for buffer\n");
        return -1;
    }

    // Zero the virtqueue and set ring pointers
    for (uint32_t i = 0; i < VQ_MEM_SIZE; i++) vq_mem[i] = 0;
    g_desc  = (vdesc_t*)vq_mem;
    g_avail = vq_mem + avail_off;
    g_used  = vq_mem + used_off;

    // Tell device the physical page number of the virtqueue
    outl((uint16_t)(g_io_base + VTIO_QUEUE_PFN), V2P(vq_mem) >> 12);

    outb((uint16_t)(g_io_base + VTIO_STATUS),
         VTIO_S_ACK | VTIO_S_DRIVER | VTIO_S_DRIVER_OK);

    // Read capacity (lower 32 bits; upper 32 ignored on 32-bit OS)
    g_total_sectors = inl((uint16_t)(g_io_base + VTIO_CONFIG));

    g_ok = 1;
    klog("virtio_blk: ready\n");
    return 0;
}

int virtio_blk_read(uint8_t drive, uint32_t lba, uint8_t* buf, uint32_t sectors) {
    (void)drive;
    if (!g_ok) return -1;
    uint32_t done = 0;
    while (done < sectors) {
        uint32_t chunk = sectors - done;
        if (chunk > MAX_CHUNK) chunk = MAX_CHUNK;
        if (virtio_do_io(VIRTIO_BLK_T_IN, (uint64_t)(lba + done),
                         buf + done * 512, chunk) < 0) return -1;
        done += chunk;
    }
    return (int)sectors;
}

int virtio_blk_write(uint8_t drive, uint32_t lba, const uint8_t* buf, uint32_t sectors) {
    (void)drive;
    if (!g_ok) return -1;
    uint32_t done = 0;
    while (done < sectors) {
        uint32_t chunk = sectors - done;
        if (chunk > MAX_CHUNK) chunk = MAX_CHUNK;
        if (virtio_do_io(VIRTIO_BLK_T_OUT, (uint64_t)(lba + done),
                         (uint8_t*)(buf + done * 512), chunk) < 0) return -1;
        done += chunk;
    }
    return (int)sectors;
}

uint32_t virtio_blk_sectors(uint8_t drive) {
    (void)drive;
    return g_ok ? g_total_sectors : 0;
}
