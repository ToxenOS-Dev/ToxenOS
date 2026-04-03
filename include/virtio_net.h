#ifndef VIRTIO_NET_H
#define VIRTIO_NET_H

#include <stdint.h>

// ── virtio legacy I/O register offsets (from BAR0 base) ──────────────────────
#define VIRTIO_PCI_HOST_FEATURES    0x00  // features the device supports (read)
#define VIRTIO_PCI_GUEST_FEATURES   0x04  // features the driver accepts (write)
#define VIRTIO_PCI_QUEUE_ADDR       0x08  // virtqueue physical addr >> 12 (write)
#define VIRTIO_PCI_QUEUE_SIZE       0x0C  // virtqueue size (read)
#define VIRTIO_PCI_QUEUE_SELECT     0x0E  // select which virtqueue (write)
#define VIRTIO_PCI_QUEUE_NOTIFY     0x10  // notify device of new buffers (write)
#define VIRTIO_PCI_STATUS           0x12  // device status register
#define VIRTIO_PCI_ISR              0x13  // interrupt status (read clears)
#define VIRTIO_PCI_NET_MAC          0x14  // 6-byte MAC address

// ── virtio device status bits ─────────────────────────────────────────────────
#define VIRTIO_STATUS_RESET         0x00
#define VIRTIO_STATUS_ACK           0x01
#define VIRTIO_STATUS_DRIVER        0x02
#define VIRTIO_STATUS_DRIVER_OK     0x04
#define VIRTIO_STATUS_FEATURES_OK   0x08
#define VIRTIO_STATUS_FAILED        0x80

// ── virtio net feature bits ───────────────────────────────────────────────────
#define VIRTIO_NET_F_MAC            (1 << 5)   // device has a MAC address

// ── virtqueue descriptor flags ────────────────────────────────────────────────
#define VIRTQ_DESC_F_NEXT           0x01  // buffer continues via next field
#define VIRTQ_DESC_F_WRITE          0x02  // write-only (device writes to it)

// ── virtqueue sizes ───────────────────────────────────────────────────────────
#define VIRTQ_SIZE       64   // must be power of 2
#define VIRTQ_RX         0    // receive queue index
#define VIRTQ_TX         1    // transmit queue index

// ── Ethernet / network constants ──────────────────────────────────────────────
#define ETH_ALEN         6    // MAC address length
#define ETH_MAX_FRAME    1514 // max Ethernet frame (excl. FCS)
#define ETH_MIN_FRAME    60   // min Ethernet frame

// virtio-net packet header (prepended to every TX/RX packet)
typedef struct __attribute__((packed)) {
    uint8_t  flags;
    uint8_t  gso_type;
    uint16_t hdr_len;
    uint16_t gso_size;
    uint16_t csum_start;
    uint16_t csum_offset;
} virtio_net_hdr_t;

// virtqueue descriptor
typedef struct __attribute__((packed)) {
    uint64_t addr;   // physical address of buffer
    uint32_t len;    // buffer length
    uint16_t flags;
    uint16_t next;   // index of next descriptor (if VIRTQ_DESC_F_NEXT)
} virtq_desc_t;

// virtqueue available ring (driver → device)
typedef struct __attribute__((packed)) {
    uint16_t flags;
    uint16_t idx;
    uint16_t ring[VIRTQ_SIZE];
} virtq_avail_t;

// used ring element
typedef struct __attribute__((packed)) {
    uint32_t id;
    uint32_t len;
} virtq_used_elem_t;

// virtqueue used ring (device → driver)
typedef struct __attribute__((packed)) {
    uint16_t          flags;
    uint16_t          idx;
    virtq_used_elem_t ring[VIRTQ_SIZE];
} virtq_used_t;

// one complete virtqueue (descriptor table + avail ring + used ring)
// aligned to 4KB so VIRTIO_PCI_QUEUE_ADDR = phys >> 12 works
typedef struct __attribute__((aligned(4096))) {
    virtq_desc_t  desc[VIRTQ_SIZE];
    virtq_avail_t avail;
    uint8_t       _pad[4096 - sizeof(virtq_desc_t)*VIRTQ_SIZE - sizeof(virtq_avail_t)];
    virtq_used_t  used;
} virtqueue_t;

// ── Public API ────────────────────────────────────────────────────────────────

// Initialise the virtio-net device. Returns 0 on success, -1 if not found.
int  virtio_net_init();

// Send an Ethernet frame. Returns 0 on success.
int  virtio_net_send(const uint8_t* frame, uint16_t len);

// Poll for a received frame. Returns frame length, or 0 if nothing available.
// buf must be at least ETH_MAX_FRAME bytes.
int  virtio_net_recv(uint8_t* buf);

// Our MAC address (set after virtio_net_init)
extern uint8_t virtio_net_mac[ETH_ALEN];

// 1 if the driver is up
extern int virtio_net_ready;

#endif
