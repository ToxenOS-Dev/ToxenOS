// ToxenOS/kernel/e1000.c
// Intel e1000 NIC driver — MMIO, no interrupts (polled)
#include <stdint.h>
#include "../include/e1000.h"
#include "../include/pci.h"
#include "../include/mm.h"
#include "../include/vga.h"
#include "../include/syscall.h"
#include "../include/paging.h"

uint8_t e1000_mac[6];
int     e1000_ready = 0;

static uint32_t mmio_base;  // virtual address of BAR0 MMIO region

// ── MMIO helpers ──────────────────────────────────────────────────────────────
static inline uint32_t e1000_read(uint32_t reg) {
    return *(volatile uint32_t*)(mmio_base + reg);
}
static inline void e1000_write(uint32_t reg, uint32_t val) {
    *(volatile uint32_t*)(mmio_base + reg) = val;
}

// ── Descriptor rings ─────────────────────────────────────────────────────────
static e1000_rx_desc_t* rx_descs;
static e1000_tx_desc_t* tx_descs;
static uint8_t*          rx_bufs[E1000_NUM_RX_DESC];
static uint8_t*          tx_bufs[E1000_NUM_TX_DESC];

static uint32_t rx_tail = E1000_NUM_RX_DESC - 1;
static uint32_t tx_tail = 0;

// ── EEPROM read ───────────────────────────────────────────────────────────────
static uint16_t eeprom_read(uint8_t addr) {
    e1000_write(E1000_EERD, (uint32_t)addr << 8 | 1);
    uint32_t val;
    do { val = e1000_read(E1000_EERD); } while (!(val & (1 << 4)));
    return (uint16_t)(val >> 16);
}

// ── Init ──────────────────────────────────────────────────────────────────────
int e1000_init()
{
    // e1000 vendor 0x8086, device 0x100E (82540EM — what QEMU emulates)
    pci_device_t* dev = pci_find(0x8086, 0x100E);
    if (!dev) {
        // Also try 0x100F (82545EM)
        dev = pci_find(0x8086, 0x100F);
        if (!dev) { klog("e1000: not found\n"); return -1; }
    }

    pci_enable(dev);

    // BAR0 is MMIO for e1000 (bit 0 clear = memory BAR)
    uint32_t bar0 = dev->bar[0] & ~0xFu;
    mmio_base = bar0;  // identity mapped

    // Map the MMIO region (16KB) into the kernel page directory
    extern uint32_t kernel_directory[];
    for (uint32_t off = 0; off < 0x20000; off += 0x1000)
        paging_map(kernel_directory, bar0 + off, bar0 + off,
                   PAGE_PRESENT | PAGE_WRITABLE);

    klog("e1000: MMIO=0x");
    {
        const char* hx="0123456789ABCDEF";
        char buf[9]; buf[8]=0; uint32_t v=bar0;
        for(int i=7;i>=0;i--){buf[i]=hx[v&0xF];v>>=4;} klog(buf); klog("\n");
    }

    // Reset
    e1000_write(E1000_CTRL, e1000_read(E1000_CTRL) | E1000_CTRL_RST);
    // Wait for reset to clear
    uint32_t timeout = 100000;
    while ((e1000_read(E1000_CTRL) & E1000_CTRL_RST) && --timeout);

    // Link up
    e1000_write(E1000_CTRL, e1000_read(E1000_CTRL) | E1000_CTRL_SLU | E1000_CTRL_ASDE);

    // Read MAC from EEPROM
    uint16_t w0 = eeprom_read(0);
    uint16_t w1 = eeprom_read(1);
    uint16_t w2 = eeprom_read(2);
    e1000_mac[0] = (uint8_t)(w0 & 0xFF);
    e1000_mac[1] = (uint8_t)(w0 >> 8);
    e1000_mac[2] = (uint8_t)(w1 & 0xFF);
    e1000_mac[3] = (uint8_t)(w1 >> 8);
    e1000_mac[4] = (uint8_t)(w2 & 0xFF);
    e1000_mac[5] = (uint8_t)(w2 >> 8);

    // Set receive address
    uint32_t ral = (uint32_t)e1000_mac[0] | ((uint32_t)e1000_mac[1]<<8) |
                   ((uint32_t)e1000_mac[2]<<16) | ((uint32_t)e1000_mac[3]<<24);
    uint32_t rah = (uint32_t)e1000_mac[4] | ((uint32_t)e1000_mac[5]<<8) | (1u<<31);
    e1000_write(E1000_RAL0, ral);
    e1000_write(E1000_RAH0, rah);

    // Allocate and init RX descriptors
    rx_descs = (e1000_rx_desc_t*)kmalloc_aligned(
                   sizeof(e1000_rx_desc_t) * E1000_NUM_RX_DESC, 16);
    for (int i = 0; i < E1000_NUM_RX_DESC; i++) {
        rx_bufs[i] = (uint8_t*)kmalloc(E1000_RX_BUF_SIZE);
        rx_descs[i].addr   = (uint64_t)(uint32_t)rx_bufs[i];
        rx_descs[i].status = 0;
    }
    e1000_write(E1000_RDBAL, (uint32_t)rx_descs);
    e1000_write(E1000_RDBAH, 0);
    e1000_write(E1000_RDLEN, sizeof(e1000_rx_desc_t) * E1000_NUM_RX_DESC);
    e1000_write(E1000_RDH, 0);
    e1000_write(E1000_RDT, E1000_NUM_RX_DESC - 1);
    e1000_write(E1000_RCTL,
        E1000_RCTL_EN | E1000_RCTL_BAM | E1000_RCTL_UPE | E1000_RCTL_MPE);

    // Allocate and init TX descriptors
    tx_descs = (e1000_tx_desc_t*)kmalloc_aligned(
                   sizeof(e1000_tx_desc_t) * E1000_NUM_TX_DESC, 16);
    for (int i = 0; i < E1000_NUM_TX_DESC; i++) {
        tx_bufs[i] = (uint8_t*)kmalloc(2048);
        tx_descs[i].addr   = (uint64_t)(uint32_t)tx_bufs[i];
        tx_descs[i].status = E1000_TXD_STAT_DD;  // mark as done (free)
    }
    e1000_write(E1000_TDBAL, (uint32_t)tx_descs);
    e1000_write(E1000_TDBAH, 0);
    e1000_write(E1000_TDLEN, sizeof(e1000_tx_desc_t) * E1000_NUM_TX_DESC);
    e1000_write(E1000_TDH, 0);
    e1000_write(E1000_TDT, 0);
    e1000_write(E1000_TCTL,
        E1000_TCTL_EN | E1000_TCTL_PSP |
        (0x10 << E1000_TCTL_CT_SHIFT) |
        (0x40 << E1000_TCTL_COLD_SHIFT));
    e1000_write(E1000_TIPG, 0x0060200A);

    // Disable interrupts (polled mode)
    e1000_write(E1000_IMC, 0xFFFFFFFF);

    e1000_ready = 1;
    klog("e1000: MAC=");
    {
        const char* hx="0123456789ABCDEF";
        for(int i=0;i<6;i++){
            char mb[3];mb[0]=hx[(e1000_mac[i]>>4)&0xF];mb[1]=hx[e1000_mac[i]&0xF];mb[2]=0;
            klog(mb); if(i<5) klog(":");
        }
    }
    klog("\ne1000: ready\n");
    return 0;
}

// ── Send ──────────────────────────────────────────────────────────────────────
int e1000_send(const uint8_t* data, uint16_t len)
{
    if (!e1000_ready || len > 1518) return -1;

    uint32_t idx = tx_tail % E1000_NUM_TX_DESC;

    // Wait for descriptor to be free
    uint32_t to = 100000;
    while (!(tx_descs[idx].status & E1000_TXD_STAT_DD) && --to);
    if (!to) return -1;

    // Copy data into TX buffer
    for (uint16_t i = 0; i < len; i++) tx_bufs[idx][i] = data[i];

    tx_descs[idx].length = len;
    tx_descs[idx].cmd    = E1000_TXD_CMD_EOP | E1000_TXD_CMD_FCS | E1000_TXD_CMD_RS;
    tx_descs[idx].status = 0;

    tx_tail = (tx_tail + 1) % E1000_NUM_TX_DESC;
    e1000_write(E1000_TDT, tx_tail);

    // Wait for send to complete
    to = 100000;
    while (!(tx_descs[idx].status & E1000_TXD_STAT_DD) && --to);

    return 0;
}

// ── Receive ───────────────────────────────────────────────────────────────────
int e1000_recv(uint8_t* buf)
{
    if (!e1000_ready) return 0;

    uint32_t idx = (rx_tail + 1) % E1000_NUM_RX_DESC;
    if (!(rx_descs[idx].status & E1000_RXD_STAT_DD)) return 0;

    uint16_t len = rx_descs[idx].length;
    for (uint16_t i = 0; i < len; i++) buf[i] = rx_bufs[idx][i];

    // Hand descriptor back to NIC
    rx_descs[idx].status = 0;
    rx_tail = idx;
    e1000_write(E1000_RDT, rx_tail);

    return len;
}
