// ToxenOS/kernel/virtio_net.c
// virtio legacy network driver (PCI device 0x1AF4:0x1000)
// Uses VIRTQ_SIZE=64 split virtqueues, polled (no MSI-X).
#include <stdint.h>
#include "../include/virtio_net.h"
#include "../include/pci.h"
#include "../include/mm.h"
#include "../include/vga.h"
#include "../include/syscall.h"

// ── Port I/O helpers ──────────────────────────────────────────────────────────

static inline void outb(uint16_t p, uint8_t  v){ __asm__ volatile("outb %0,%1"::"a"(v),"Nd"(p)); }
static inline void outw(uint16_t p, uint16_t v){ __asm__ volatile("outw %0,%1"::"a"(v),"Nd"(p)); }
static inline void outl(uint16_t p, uint32_t v){ __asm__ volatile("outl %0,%1"::"a"(v),"Nd"(p)); }
static inline uint8_t  inb(uint16_t p){ uint8_t  r; __asm__ volatile("inb %1,%0":"=a"(r):"Nd"(p)); return r; }
static inline uint16_t inw(uint16_t p){ uint16_t r; __asm__ volatile("inw %1,%0":"=a"(r):"Nd"(p)); return r; }
static inline uint32_t inl(uint16_t p){ uint32_t r; __asm__ volatile("inl %1,%0":"=a"(r):"Nd"(p)); return r; }

// memory barrier — prevent compiler/CPU reordering around virtqueue updates
static inline void mb() { __asm__ volatile("mfence":::"memory"); }

// ── Driver state ──────────────────────────────────────────────────────────────

uint8_t virtio_net_mac[ETH_ALEN];
int     virtio_net_ready = 0;

static uint32_t    io_base;        // BAR0 I/O port base
static virtqueue_t vq[2];         // vq[0]=RX, vq[1]=TX  (4KB aligned static storage)

// RX buffers — one per descriptor slot
// Each slot holds virtio_net_hdr_t + max Ethernet frame
#define RX_BUF_SIZE  (sizeof(virtio_net_hdr_t) + ETH_MAX_FRAME + 4)
static uint8_t rx_bufs[VIRTQ_SIZE][RX_BUF_SIZE];

// TX bounce buffer (one at a time — we don't need more for now)
static uint8_t tx_buf[sizeof(virtio_net_hdr_t) + ETH_MAX_FRAME + 4];

// Track where we last read in the used ring
static uint16_t rx_last_used = 0;
static uint16_t tx_last_used = 0;

// Next free descriptor indices
static uint16_t rx_free_head = 0;
static uint16_t tx_free_head = 0;

// ── virtqueue setup helpers ───────────────────────────────────────────────────

static void vq_select(int idx)
{
    outw((uint16_t)(io_base + VIRTIO_PCI_QUEUE_SELECT), (uint16_t)idx);
}

static void vq_activate(int idx, virtqueue_t* q)
{
    vq_select(idx);
    uint32_t phys = (uint32_t)q;
    outl((uint16_t)(io_base + VIRTIO_PCI_QUEUE_ADDR), phys >> 12);
}

static void vq_notify(int idx)
{
    outw((uint16_t)(io_base + VIRTIO_PCI_QUEUE_NOTIFY), (uint16_t)idx);
}

// ── RX setup ─────────────────────────────────────────────────────────────────
// Fill all RX descriptor slots with write-only buffers so the device can
// deposit received frames immediately.

static void rx_fill_all()
{
    for (uint16_t i = 0; i < VIRTQ_SIZE; i++) {
        vq[VIRTQ_RX].desc[i].addr  = (uint32_t)rx_bufs[i];
        vq[VIRTQ_RX].desc[i].len   = RX_BUF_SIZE;
        vq[VIRTQ_RX].desc[i].flags = VIRTQ_DESC_F_WRITE;
        vq[VIRTQ_RX].desc[i].next  = 0;

        // Make descriptor available to device
        vq[VIRTQ_RX].avail.ring[vq[VIRTQ_RX].avail.idx % VIRTQ_SIZE] = i;
        vq[VIRTQ_RX].avail.idx++;
    }
    mb();
    vq_notify(VIRTQ_RX);
}

// ── Public API ────────────────────────────────────────────────────────────────

int virtio_net_init()
{
    pci_device_t* dev = pci_find(PCI_VENDOR_VIRTIO, PCI_DEV_VIRTIO_NET);
    if (!dev) {
        klog("virtio-net: device not found\n");
        return -1;
    }

    pci_enable(dev);
    io_base = pci_bar_io(dev, 0);

    klog("virtio-net: I/O base=0x");
    // log hex io_base
    const char* hx = "0123456789ABCDEF";
    char hbuf[9]; hbuf[8] = 0;
    uint32_t v = io_base;
    for (int i = 7; i >= 0; i--) { hbuf[i] = hx[v & 0xF]; v >>= 4; }
    klog(hbuf); klog("\n");

    // Reset device
    outb((uint16_t)(io_base + VIRTIO_PCI_STATUS), VIRTIO_STATUS_RESET);

    // ACK + DRIVER
    outb((uint16_t)(io_base + VIRTIO_PCI_STATUS), VIRTIO_STATUS_ACK);
    outb((uint16_t)(io_base + VIRTIO_PCI_STATUS), VIRTIO_STATUS_ACK | VIRTIO_STATUS_DRIVER);

    // Negotiate features — accept MAC feature only
    uint32_t host_feat = inl((uint16_t)(io_base + VIRTIO_PCI_HOST_FEATURES));
    uint32_t our_feat  = host_feat & VIRTIO_NET_F_MAC;
    outl((uint16_t)(io_base + VIRTIO_PCI_GUEST_FEATURES), our_feat);

    // Read MAC
    for (int i = 0; i < ETH_ALEN; i++)
        virtio_net_mac[i] = inb((uint16_t)(io_base + VIRTIO_PCI_NET_MAC + i));

    klog("virtio-net: MAC=");
    for (int i = 0; i < ETH_ALEN; i++) {
        char mb2[3];
        mb2[0] = hx[(virtio_net_mac[i] >> 4) & 0xF];
        mb2[1] = hx[virtio_net_mac[i] & 0xF];
        mb2[2] = 0;
        klog(mb2);
        if (i < 5) klog(":");
    }
    klog("\n");

    // Zero out virtqueues
    uint8_t* p = (uint8_t*)vq;
    for (uint32_t i = 0; i < sizeof(vq); i++) p[i] = 0;

    // Activate RX queue (index 0)
    vq_activate(VIRTQ_RX, &vq[VIRTQ_RX]);
    // Activate TX queue (index 1)
    vq_activate(VIRTQ_TX, &vq[VIRTQ_TX]);

    // Fill RX descriptors
    rx_fill_all();
    rx_last_used = 0;
    tx_last_used = 0;

    // DRIVER_OK — device is live
    outb((uint16_t)(io_base + VIRTIO_PCI_STATUS),
         VIRTIO_STATUS_ACK | VIRTIO_STATUS_DRIVER | VIRTIO_STATUS_DRIVER_OK);

    virtio_net_ready = 1;
    klog("virtio-net: ready\n");
    return 0;
}

int virtio_net_send(const uint8_t* frame, uint16_t len)
{
    if (!virtio_net_ready) return -1;
    if (len > ETH_MAX_FRAME) return -1;

    // Build TX buffer: virtio header + frame data
    virtio_net_hdr_t* hdr = (virtio_net_hdr_t*)tx_buf;
    uint8_t* hbuf = (uint8_t*)hdr;
    for (uint32_t i = 0; i < sizeof(virtio_net_hdr_t); i++) hbuf[i] = 0;

    uint8_t* payload = tx_buf + sizeof(virtio_net_hdr_t);
    for (uint16_t i = 0; i < len; i++) payload[i] = frame[i];

    uint16_t total = (uint16_t)(sizeof(virtio_net_hdr_t) + len);

    // Use next free TX descriptor
    uint16_t idx = tx_free_head % VIRTQ_SIZE;
    tx_free_head++;

    vq[VIRTQ_TX].desc[idx].addr  = (uint32_t)tx_buf;
    vq[VIRTQ_TX].desc[idx].len   = total;
    vq[VIRTQ_TX].desc[idx].flags = 0;  // read-only for device
    vq[VIRTQ_TX].desc[idx].next  = 0;

    // Put in avail ring
    uint16_t avail_idx = vq[VIRTQ_TX].avail.idx % VIRTQ_SIZE;
    vq[VIRTQ_TX].avail.ring[avail_idx] = idx;
    mb();
    vq[VIRTQ_TX].avail.idx++;
    mb();
    vq_notify(VIRTQ_TX);

    // Wait for TX completion (poll used ring)
    uint32_t timeout = 1000000;
    while (vq[VIRTQ_TX].used.idx == tx_last_used && --timeout) ;
    tx_last_used = vq[VIRTQ_TX].used.idx;

    return 0;
}

int virtio_net_recv(uint8_t* buf)
{
    if (!virtio_net_ready) return 0;

    // Check if device has put anything in the used ring
    if (vq[VIRTQ_RX].used.idx == rx_last_used) return 0;

    uint16_t used_idx = rx_last_used % VIRTQ_SIZE;
    uint16_t desc_idx = (uint16_t)vq[VIRTQ_RX].used.ring[used_idx].id;
    uint32_t recv_len = vq[VIRTQ_RX].used.ring[used_idx].len;

    rx_last_used++;

    if (recv_len <= sizeof(virtio_net_hdr_t)) return 0;

    // Skip the virtio header — give caller the raw Ethernet frame
    uint32_t frame_len = recv_len - sizeof(virtio_net_hdr_t);
    uint8_t* src = rx_bufs[desc_idx] + sizeof(virtio_net_hdr_t);
    for (uint32_t i = 0; i < frame_len; i++) buf[i] = src[i];

    // Recycle the descriptor back to the device
    vq[VIRTQ_RX].avail.ring[vq[VIRTQ_RX].avail.idx % VIRTQ_SIZE] = desc_idx;
    mb();
    vq[VIRTQ_RX].avail.idx++;
    mb();
    vq_notify(VIRTQ_RX);

    return (int)frame_len;
}
