#ifndef E1000_H
#define E1000_H

#include <stdint.h>

// ── e1000 MMIO register offsets ───────────────────────────────────────────────
#define E1000_CTRL      0x0000  // Device Control
#define E1000_STATUS    0x0008  // Device Status
#define E1000_EECD      0x0010  // EEPROM/Flash Control
#define E1000_EERD      0x0014  // EEPROM Read
#define E1000_ICR       0x00C0  // Interrupt Cause Read
#define E1000_IMS       0x00D0  // Interrupt Mask Set
#define E1000_IMC       0x00D8  // Interrupt Mask Clear
#define E1000_RCTL      0x0100  // Receive Control
#define E1000_TCTL      0x0400  // Transmit Control
#define E1000_TIPG      0x0410  // Transmit IPG
#define E1000_RDBAL     0x2800  // RX Descriptor Base Low
#define E1000_RDBAH     0x2804  // RX Descriptor Base High
#define E1000_RDLEN     0x2808  // RX Descriptor Length
#define E1000_RDH       0x2810  // RX Descriptor Head
#define E1000_RDT       0x2818  // RX Descriptor Tail
#define E1000_TDBAL     0x3800  // TX Descriptor Base Low
#define E1000_TDBAH     0x3804  // TX Descriptor Base High
#define E1000_TDLEN     0x3808  // TX Descriptor Length
#define E1000_TDH       0x3810  // TX Descriptor Head
#define E1000_TDT       0x3818  // TX Descriptor Tail
#define E1000_RAL0      0x5400  // Receive Address Low
#define E1000_RAH0      0x5404  // Receive Address High

// ── CTRL bits ─────────────────────────────────────────────────────────────────
#define E1000_CTRL_RST      (1 << 26)
#define E1000_CTRL_ASDE     (1 << 5)
#define E1000_CTRL_SLU      (1 << 6)   // Set Link Up

// ── RCTL bits ─────────────────────────────────────────────────────────────────
#define E1000_RCTL_EN       (1 << 1)
#define E1000_RCTL_SBP      (1 << 2)
#define E1000_RCTL_UPE      (1 << 3)   // Unicast Promiscuous
#define E1000_RCTL_MPE      (1 << 4)   // Multicast Promiscuous
#define E1000_RCTL_BAM      (1 << 15)  // Broadcast Accept
#define E1000_RCTL_BSIZE_2048 0        // Buffer size 2048

// ── TCTL bits ─────────────────────────────────────────────────────────────────
#define E1000_TCTL_EN       (1 << 1)
#define E1000_TCTL_PSP      (1 << 3)   // Pad Short Packets
#define E1000_TCTL_CT_SHIFT 4
#define E1000_TCTL_COLD_SHIFT 12

// ── TX descriptor command bits ────────────────────────────────────────────────
#define E1000_TXD_CMD_EOP   (1 << 0)   // End of Packet
#define E1000_TXD_CMD_FCS   (1 << 1)   // Insert FCS/CRC
#define E1000_TXD_CMD_RS    (1 << 3)   // Report Status

// ── TX descriptor status bits ────────────────────────────────────────────────
#define E1000_TXD_STAT_DD   (1 << 0)   // Descriptor Done

// ── RX descriptor status bits ────────────────────────────────────────────────
#define E1000_RXD_STAT_DD   (1 << 0)   // Descriptor Done
#define E1000_RXD_STAT_EOP  (1 << 1)   // End of Packet

// ── Descriptor sizes ─────────────────────────────────────────────────────────
#define E1000_NUM_RX_DESC   32
#define E1000_NUM_TX_DESC   8
#define E1000_RX_BUF_SIZE   2048

// ── Descriptors ──────────────────────────────────────────────────────────────
typedef struct __attribute__((packed)) {
    uint64_t addr;      // buffer address
    uint16_t length;
    uint16_t checksum;
    uint8_t  status;
    uint8_t  errors;
    uint16_t special;
} e1000_rx_desc_t;

typedef struct __attribute__((packed)) {
    uint64_t addr;      // buffer address
    uint16_t length;
    uint8_t  cso;       // checksum offset
    uint8_t  cmd;       // command
    uint8_t  status;
    uint8_t  css;       // checksum start
    uint16_t special;
} e1000_tx_desc_t;

// ── Public API ────────────────────────────────────────────────────────────────
int  e1000_init();
int  e1000_send(const uint8_t* data, uint16_t len);
int  e1000_recv(uint8_t* buf);   // returns frame length or 0

extern uint8_t e1000_mac[6];
extern int     e1000_ready;

#endif
