// ToxenOS/kernel/virtio_net.c
// virtio legacy network driver (PCI 0x1AF4:0x1000)
#include <stdint.h>
#include "../include/virtio_net.h"
#include "../include/pci.h"
#include "../include/mm.h"
#include "../include/vga.h"
#include "../include/syscall.h"

static inline void outb(uint16_t p,uint8_t  v){__asm__ volatile("outb %0,%1"::"a"(v),"Nd"(p));}
static inline void outw(uint16_t p,uint16_t v){__asm__ volatile("outw %0,%1"::"a"(v),"Nd"(p));}
static inline void outl(uint16_t p,uint32_t v){__asm__ volatile("outl %0,%1"::"a"(v),"Nd"(p));}
static inline uint8_t  inb(uint16_t p){uint8_t  r;__asm__ volatile("inb %1,%0":"=a"(r):"Nd"(p));return r;}
static inline uint16_t inw(uint16_t p){uint16_t r;__asm__ volatile("inw %1,%0":"=a"(r):"Nd"(p));return r;}
static inline uint32_t inl(uint16_t p){uint32_t r;__asm__ volatile("inl %1,%0":"=a"(r):"Nd"(p));return r;}
static inline void mb(){__asm__ volatile("mfence":::"memory");}

uint8_t virtio_net_mac[ETH_ALEN];
int     virtio_net_ready = 0;

static uint32_t io_base;
static uint16_t vq_size[2];   // actual queue sizes reported by device

// Each virtqueue is allocated separately so we can give the device
// the correct page-aligned physical address.
// We use kmalloc_aligned to get 4096-aligned kernel memory.

static uint32_t* vq_desc[2];   // descriptor table  (vq_size[i] * 16 bytes)
static uint16_t* vq_avail[2];  // available ring     (6 + vq_size[i]*2 bytes)
static uint16_t* vq_used[2];   // used ring          (6 + vq_size[i]*8 bytes)
static uint32_t  vq_phys[2];   // physical base address of the full virtqueue

// Receive buffers — one per RX descriptor
#define RX_BUF_SIZE  (sizeof(virtio_net_hdr_t) + ETH_MAX_FRAME + 4)
static uint8_t* rx_bufs[VIRTQ_SIZE];

// TX bounce buffer
static uint8_t tx_buf[sizeof(virtio_net_hdr_t) + ETH_MAX_FRAME + 4];

static uint16_t rx_last_used = 0;
static uint16_t tx_last_used = 0;

// ── virtqueue memory layout ───────────────────────────────────────────────────
// virtio legacy requires all three rings in one contiguous block:
//   [desc table][avail ring][padding to 4096][used ring]
// Total size = 4096 + ceil((6 + size*8) / 4096)*4096

static int setup_vq(int idx)
{
    uint16_t sz = vq_size[idx];
    if (sz == 0) return -1;

    // Compute sizes
    uint32_t desc_bytes  = (uint32_t)sz * 16;
    uint32_t avail_bytes = 6 + (uint32_t)sz * 2;
    uint32_t used_bytes  = 6 + (uint32_t)sz * 8;

    // used ring starts at next 4096-byte boundary after desc+avail
    uint32_t used_offset = (desc_bytes + avail_bytes + 4095) & ~4095u;
    uint32_t total_bytes = used_offset + used_bytes;
    total_bytes = (total_bytes + 4095) & ~4095u;  // round up to page

    // Allocate one contiguous block
    uint8_t* mem = (uint8_t*)kmalloc_aligned(total_bytes, 4096);
    if (!mem) return -1;

    // Zero it
    for (uint32_t i = 0; i < total_bytes; i++) mem[i] = 0;

    vq_phys[idx]  = (uint32_t)mem;   // identity mapped: virt == phys
    vq_desc[idx]  = (uint32_t*)mem;
    vq_avail[idx] = (uint16_t*)(mem + desc_bytes);
    vq_used[idx]  = (uint16_t*)(mem + used_offset);

    // Tell device: select queue, then write page number
    outw((uint16_t)(io_base + VIRTIO_PCI_QUEUE_SELECT), (uint16_t)idx);
    outl((uint16_t)(io_base + VIRTIO_PCI_QUEUE_ADDR), vq_phys[idx] >> 12);

    // Verify: read back the address we just wrote
    outw((uint16_t)(io_base + VIRTIO_PCI_QUEUE_SELECT), (uint16_t)idx);
    uint32_t readback = inl((uint16_t)(io_base + VIRTIO_PCI_QUEUE_ADDR));
    if (readback != vq_phys[idx] >> 12) {
        klog("virtio-net: QUEUE_ADDR readback mismatch!\n");
    }

    return 0;
}

// ── RX fill ───────────────────────────────────────────────────────────────────
static void rx_fill()
{
    uint16_t sz = vq_size[0];
    uint32_t* desc  = vq_desc[0];
    uint16_t* avail = vq_avail[0];

    for (uint16_t i = 0; i < sz; i++) {
        // Allocate an RX buffer
        if (!rx_bufs[i])
            rx_bufs[i] = (uint8_t*)kmalloc(RX_BUF_SIZE);

        // Descriptor: write-only, size = full buffer
        desc[i*4 + 0] = (uint32_t)rx_bufs[i];  // addr low
        desc[i*4 + 1] = 0;                       // addr high (32-bit: 0)
        desc[i*4 + 2] = RX_BUF_SIZE;             // len
        desc[i*4 + 3] = VIRTQ_DESC_F_WRITE;      // flags + next (no chain)

        // Add to avail ring
        uint16_t avail_idx = avail[1];  // avail->idx
        avail[2 + (avail_idx % sz)] = i;
        mb();
        avail[1] = avail_idx + 1;       // bump idx
    }
    mb();
    outw((uint16_t)(io_base + VIRTIO_PCI_QUEUE_NOTIFY), 0);  // notify RX queue
}

// ── Public API ────────────────────────────────────────────────────────────────
int virtio_net_init()
{
    pci_device_t* dev = pci_find(PCI_VENDOR_VIRTIO, PCI_DEV_VIRTIO_NET);
    if (!dev) { klog("virtio-net: not found\n"); return -1; }

    pci_enable(dev);
    io_base = pci_bar_io(dev, 0);

    // Check PCI revision: 0 = legacy virtio, 1 = transitional/modern
    uint8_t rev = (uint8_t)pci_read(dev->bus, dev->slot, dev->func, 0x08);
    if (rev != 0) {
        klog("virtio-net: modern device (rev=");
        char rc[3]; rc[0]='0'+(rev/10); rc[1]='0'+(rev%10); rc[2]=0;
        klog(rc); klog(") - need modern init\n");
        // For now fall through and try legacy anyway
    } else {
        klog("virtio-net: legacy device\n");
    }

    // 1. Reset
    outb((uint16_t)(io_base + VIRTIO_PCI_STATUS), 0);
    // 2. ACK
    outb((uint16_t)(io_base + VIRTIO_PCI_STATUS), VIRTIO_STATUS_ACK);
    // 3. DRIVER
    outb((uint16_t)(io_base + VIRTIO_PCI_STATUS),
         VIRTIO_STATUS_ACK | VIRTIO_STATUS_DRIVER);

    // 4. Negotiate features — just ask for MAC
    uint32_t hf = inl((uint16_t)(io_base + VIRTIO_PCI_HOST_FEATURES));
    outl((uint16_t)(io_base + VIRTIO_PCI_GUEST_FEATURES), hf & VIRTIO_NET_F_MAC);

    // 5. Read MAC
    for (int i = 0; i < ETH_ALEN; i++)
        virtio_net_mac[i] = inb((uint16_t)(io_base + VIRTIO_PCI_NET_MAC + i));

    // 6. Setup queues — read actual size from device
    for (int i = 0; i < 2; i++) {
        outw((uint16_t)(io_base + VIRTIO_PCI_QUEUE_SELECT), (uint16_t)i);
        vq_size[i] = inw((uint16_t)(io_base + VIRTIO_PCI_QUEUE_SIZE));
        if (vq_size[i] == 0 || vq_size[i] > VIRTQ_SIZE) vq_size[i] = VIRTQ_SIZE;
    }

    klog("virtio-net: qsize=");
    char qs[4]; qs[0]='0'+(char)(vq_size[0]/10); qs[1]='0'+(char)(vq_size[0]%10); qs[2]='\n'; qs[3]=0;
    klog(qs);

    if (setup_vq(0) < 0 || setup_vq(1) < 0) {
        klog("virtio-net: vq setup failed\n"); return -1;
    }

    // Fill RX queue
    for (int i = 0; i < VIRTQ_SIZE; i++) rx_bufs[i] = 0;
    rx_fill();
    rx_last_used = 0;
    tx_last_used = 0;

    // 7. DRIVER_OK
    outb((uint16_t)(io_base + VIRTIO_PCI_STATUS),
         VIRTIO_STATUS_ACK | VIRTIO_STATUS_DRIVER | VIRTIO_STATUS_DRIVER_OK);

    virtio_net_ready = 1;

    klog("virtio-net: MAC=");
    const char* hx="0123456789ABCDEF";
    for (int i=0;i<ETH_ALEN;i++){
        char mb2[3];mb2[0]=hx[(virtio_net_mac[i]>>4)&0xF];mb2[1]=hx[virtio_net_mac[i]&0xF];mb2[2]=0;
        klog(mb2); if(i<5)klog(":");
    }
    klog("\nvirtio-net: ready\n");
    return 0;
}

int virtio_net_send(const uint8_t* frame, uint16_t len)
{
    if (!virtio_net_ready || len > ETH_MAX_FRAME) return -1;

    uint16_t sz    = vq_size[1];
    uint32_t* desc  = vq_desc[1];
    uint16_t* avail = vq_avail[1];
    uint16_t* used  = vq_used[1];

    // Build TX buffer: virtio header (all zeros) + frame
    for (uint32_t i = 0; i < sizeof(virtio_net_hdr_t); i++) tx_buf[i] = 0;
    for (uint16_t i = 0; i < len; i++) tx_buf[sizeof(virtio_net_hdr_t)+i] = frame[i];
    uint16_t total = (uint16_t)(sizeof(virtio_net_hdr_t) + len);

    uint16_t idx = avail[1] % sz;  // next descriptor slot

    desc[idx*4+0] = (uint32_t)tx_buf;
    desc[idx*4+1] = 0;
    desc[idx*4+2] = total;
    desc[idx*4+3] = 0;  // read-only, no chain

    avail[2 + (avail[1] % sz)] = idx;
    mb();
    avail[1]++;
    mb();
    outw((uint16_t)(io_base + VIRTIO_PCI_QUEUE_NOTIFY), 1);

    // Poll for completion
    uint32_t timeout = 1000000;
    while (used[1] == tx_last_used && --timeout);
    tx_last_used = used[1];
    return 0;
}

int virtio_net_recv(uint8_t* buf)
{
    if (!virtio_net_ready) return 0;

    uint16_t  sz   = vq_size[0];
    uint16_t* used = vq_used[0];
    uint16_t* avail= vq_avail[0];
    uint32_t* desc = vq_desc[0];

    // used[0]=flags, used[1]=idx
    if (used[1] == rx_last_used) return 0;

    uint16_t used_slot = rx_last_used % sz;
    // used ring elements start at used[2]: each is {id(u32), len(u32)}
    uint32_t* ue    = (uint32_t*)(used + 2);
    uint16_t  didx  = (uint16_t)ue[used_slot*2 + 0];
    uint32_t  rlen  = ue[used_slot*2 + 1];
    rx_last_used++;

    if (rlen <= sizeof(virtio_net_hdr_t)) goto recycle;

    {
        uint32_t flen = rlen - sizeof(virtio_net_hdr_t);
        if (flen > ETH_MAX_FRAME) flen = ETH_MAX_FRAME;
        uint8_t* src = rx_bufs[didx] + sizeof(virtio_net_hdr_t);
        for (uint32_t i = 0; i < flen; i++) buf[i] = src[i];

        // Recycle descriptor
recycle:
        desc[didx*4+0] = (uint32_t)rx_bufs[didx];
        desc[didx*4+1] = 0;
        desc[didx*4+2] = RX_BUF_SIZE;
        desc[didx*4+3] = VIRTQ_DESC_F_WRITE;

        avail[2 + (avail[1] % sz)] = didx;
        mb();
        avail[1]++;
        mb();
        outw((uint16_t)(io_base + VIRTIO_PCI_QUEUE_NOTIFY), 0);

        if (rlen <= sizeof(virtio_net_hdr_t)) return 0;
        return (int)(rlen - sizeof(virtio_net_hdr_t));
    }
}

uint32_t virtio_net_rx_used() {
    if (!virtio_net_ready) return 0;
    return (uint32_t)vq_used[0][1];
}
