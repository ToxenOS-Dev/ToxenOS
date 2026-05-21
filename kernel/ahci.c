// ToxenOS/kernel/ahci.c — AHCI SATA driver
// Provides ahci_read_drive/ahci_write_drive as drop-in ATA replacements.
// Detects AHCI via PCI (class 0x01, subclass 0x06), maps ABAR, inits ports.
#include <stdint.h>
#include "../include/pci.h"
#include "../include/klog.h"
#include "../include/paging.h"
#include "../include/memmap.h"

// ── PCI class codes ────────────────────────────────────────────────────────
#define PCI_CLASS_STORAGE    0x01
#define PCI_SUBCLASS_SATA    0x06

// ── HBA register offsets ─────────────────────────────────────────────────
#define HBA_GHC              0x04   // Global Host Control
#define HBA_IS               0x08   // Interrupt Status
#define HBA_PI               0x0C   // Ports Implemented
#define HBA_GHC_AE           (1u<<31) // AHCI Enable
#define HBA_GHC_HR           (1u<<0)  // HBA Reset

// Port register offsets (from port base = ABAR + 0x100 + port*0x80)
#define PORT_CLB             0x00   // Command List Base
#define PORT_FB              0x08   // FIS Base
#define PORT_IS              0x10   // Interrupt Status
#define PORT_IE              0x14   // Interrupt Enable
#define PORT_CMD             0x18   // Command and Status
#define PORT_TFD             0x20   // Task File Data
#define PORT_SIG             0x24   // Signature
#define PORT_SSTS            0x28   // SATA Status (SCR0:SStatus)
#define PORT_SCTL            0x2C   // SATA Control
#define PORT_SERR            0x30   // SATA Error
#define PORT_CI              0x38   // Command Issue

#define PORT_CMD_ST          0x0001  // Start DMA engine
#define PORT_CMD_FRE         0x0010  // FIS Receive Enable
#define PORT_CMD_FR          0x4000  // FIS Receive Running
#define PORT_CMD_CR          0x8000  // Command List Running

#define PORT_SSTS_DET_PRESENT 0x3   // Device present + PHY established
#define SIG_SATA             0x00000101 // SATA drive signature

// ATA commands
#define ATA_CMD_READ_DMA_EX  0x25
#define ATA_CMD_WRITE_DMA_EX 0x35

// FIS type
#define FIS_TYPE_REG_H2D     0x27

// ── Structures ────────────────────────────────────────────────────────────

typedef struct __attribute__((packed)) {
    uint8_t  fis_type;    // 0x27
    uint8_t  c;           // bit 7 = command (1)
    uint8_t  command;
    uint8_t  featurel;
    uint8_t  lba0, lba1, lba2, device;
    uint8_t  lba3, lba4, lba5, featureh;
    uint8_t  countl, counth;
    uint8_t  icc, control;
    uint8_t  rsv[4];
} fis_h2d_t;  // 20 bytes

typedef struct __attribute__((packed)) {
    // DW0
    uint8_t  cfl;         // Command FIS length in dwords (5 for 20-byte FIS)
    uint8_t  flags;       // bit 6=prefetch, bit 7=write
    uint16_t prdtl;       // PRDT entry count
    // DW1
    uint32_t prdbc;       // Physical region byte count (filled by HBA)
    // DW2-3
    uint32_t ctba;        // Command table base address (physical)
    uint32_t ctbau;       // Upper 32 bits (0 for 32-bit)
    // DW4-7
    uint32_t rsv[4];
} hba_cmd_hdr_t;  // 32 bytes

typedef struct __attribute__((packed)) {
    uint32_t dba;         // Data base address (physical)
    uint32_t dbau;        // Upper 32 bits
    uint32_t rsv;
    uint32_t dbc_i;       // Byte count (bits 21:0), bit31=interrupt on completion
} prdt_entry_t;

typedef struct __attribute__((packed)) {
    uint8_t       cfis[64];    // Command FIS (20 bytes used, rest 0)
    uint8_t       acmd[16];    // ATAPI command (unused)
    uint8_t       rsv[48];
    prdt_entry_t  prdt[8];    // Physical Region Descriptor Table
} hba_cmd_tbl_t;

// ── Static buffers (BSS — physical addr = virtual - 0xC0000000) ──────────
#define MAX_AHCI_PORTS 4

static hba_cmd_hdr_t cmd_list[32]       __attribute__((aligned(1024)));
static uint8_t       fis_recv[256]      __attribute__((aligned(256)));
static hba_cmd_tbl_t cmd_table          __attribute__((aligned(128)));
static uint8_t       dma_buf[65536]     __attribute__((aligned(4096)));

// ── AHCI state ─────────────────────────────────────────────────────────────
static int      ahci_ok     = 0;
static uint32_t abar        = 0;   // virtual = physical (identity mapped MMIO)
static int      ahci_port_n = -1;  // first active port index

#define PREG(port, off)  (*((volatile uint32_t*)(abar + 0x100 + (port)*0x80 + (off))))
#define HREG(off)        (*((volatile uint32_t*)(abar + (off))))

// ── Helpers ────────────────────────────────────────────────────────────────

static void ahci_stop_cmd(int port) {
    PREG(port, PORT_CMD) &= ~(uint32_t)PORT_CMD_ST;
    PREG(port, PORT_CMD) &= ~(uint32_t)PORT_CMD_FRE;
    uint32_t tries = 500000;
    while ((PREG(port, PORT_CMD) & (PORT_CMD_FR | PORT_CMD_CR)) && --tries);
}

static void ahci_start_cmd(int port) {
    uint32_t tries = 500000;
    while ((PREG(port, PORT_CMD) & PORT_CMD_CR) && --tries);
    PREG(port, PORT_CMD) |= PORT_CMD_FRE;
    PREG(port, PORT_CMD) |= PORT_CMD_ST;
}

#define PORT_SACT 0x34

static int ahci_find_slot(int port) {
    uint32_t slots = PREG(port, PORT_SACT) | PREG(port, PORT_CI);
    for (int i = 0; i < 32; i++) {
        if (!(slots & (1u << i))) return i;
    }
    return -1;
}

// ── Init ───────────────────────────────────────────────────────────────────

int ahci_init(void) {
    // Find AHCI controller via PCI
    pci_device_t* dev = 0;
    for (int i = 0; i < pci_device_count; i++) {
        if (pci_devices[i].class_code == PCI_CLASS_STORAGE &&
            pci_devices[i].subclass   == PCI_SUBCLASS_SATA) {
            dev = &pci_devices[i];
            break;
        }
    }
    if (!dev) { klog("AHCI: no controller found\n"); return -1; }

    // BAR5 = ABAR (AHCI Base Address Register)
    uint32_t bar5 = dev->bar[5] & ~0xFu;
    if (!bar5) { klog("AHCI: invalid ABAR\n"); return -1; }

    // Enable PCI bus mastering
    pci_enable(dev);

    // Map ABAR (4KB per port × 32 ports + 0x100 header = ~4MB max, map 64KB to be safe)
    extern uint32_t kernel_directory[];
    for (uint32_t off = 0; off < 0x10000; off += 0x1000)
        paging_map(kernel_directory, bar5 + off, bar5 + off,
                   PAGE_PRESENT | PAGE_WRITABLE);

    abar = bar5;

    // Enable AHCI mode
    HREG(HBA_GHC) |= HBA_GHC_AE;

    // Find first implemented port with a drive
    uint32_t pi = HREG(HBA_PI);
    for (int p = 0; p < 32; p++) {
        if (!(pi & (1u << p))) continue;
        uint32_t det = PREG(p, PORT_SSTS) & 0xF;
        uint32_t ipm = (PREG(p, PORT_SSTS) >> 8) & 0xF;
        if (det != PORT_SSTS_DET_PRESENT || ipm != 1) continue;
        if (PREG(p, PORT_SIG) == 0xEB140101) continue; // skip ATAPI

        // Found a SATA drive
        ahci_port_n = p;
        break;
    }
    if (ahci_port_n < 0) { klog("AHCI: no SATA drive found\n"); return -1; }

    int port = ahci_port_n;
    ahci_stop_cmd(port);

    // Set up command list and FIS buffers (physical addresses)
    uint32_t clb_phys  = (uint32_t)cmd_list - KERNEL_VIRT_BASE;
    uint32_t fb_phys   = (uint32_t)fis_recv - KERNEL_VIRT_BASE;
    PREG(port, PORT_CLB)  = clb_phys;
    PREG(port, PORT_FB)   = fb_phys;

    // Clear errors
    PREG(port, PORT_SERR) = 0xFFFFFFFF;
    PREG(port, PORT_IS)   = 0xFFFFFFFF;
    HREG(HBA_IS)          = 0xFFFFFFFF;

    // Zero buffers
    for (int i = 0; i < (int)sizeof(cmd_list); i++)  ((uint8_t*)cmd_list)[i] = 0;
    for (int i = 0; i < (int)sizeof(fis_recv); i++)  fis_recv[i] = 0;

    ahci_start_cmd(port);

    ahci_ok = 1;
    klog("AHCI: initialised\n");
    return 0;
}

// ── Read / Write ───────────────────────────────────────────────────────────

static int ahci_issue(int port, uint64_t lba, uint8_t* buf,
                      uint32_t sector_count, int write) {
    int slot = ahci_find_slot(port);
    if (slot < 0) return -1;

    // Build command header
    hba_cmd_hdr_t* hdr = &cmd_list[slot];
    for (int i = 0; i < (int)sizeof(hba_cmd_hdr_t); i++) ((uint8_t*)hdr)[i] = 0;
    hdr->cfl   = sizeof(fis_h2d_t) / 4;  // 5 dwords
    hdr->flags = write ? (1u<<6) : 0;
    hdr->prdtl = 1;
    hdr->ctba  = (uint32_t)(&cmd_table) - KERNEL_VIRT_BASE;
    hdr->ctbau = 0;

    // Build command table
    for (int i = 0; i < (int)sizeof(hba_cmd_tbl_t); i++) ((uint8_t*)&cmd_table)[i] = 0;

    // Set up PRDT
    uint32_t buf_phys = (uint32_t)buf - KERNEL_VIRT_BASE;
    cmd_table.prdt[0].dba   = buf_phys;
    cmd_table.prdt[0].dbau  = 0;
    cmd_table.prdt[0].dbc_i = (sector_count * 512 - 1) | (1u << 31);

    // Build FIS
    fis_h2d_t* fis = (fis_h2d_t*)cmd_table.cfis;
    fis->fis_type = FIS_TYPE_REG_H2D;
    fis->c        = 0x80; // command bit
    fis->command  = write ? ATA_CMD_WRITE_DMA_EX : ATA_CMD_READ_DMA_EX;
    fis->device   = 0x40; // LBA mode
    fis->lba0     = (uint8_t)(lba      );
    fis->lba1     = (uint8_t)(lba >>  8);
    fis->lba2     = (uint8_t)(lba >> 16);
    fis->lba3     = (uint8_t)(lba >> 24);
    fis->lba4     = (uint8_t)(lba >> 32);
    fis->lba5     = (uint8_t)(lba >> 40);
    fis->countl   = (uint8_t)(sector_count     );
    fis->counth   = (uint8_t)(sector_count >> 8);

    // Wait for port to be idle
    uint32_t to = 1000000;
    while ((PREG(port, PORT_TFD) & 0x88) && --to); // BSY or DRQ
    if (!to) return -1;

    // Issue command
    PREG(port, PORT_IS)  = 0xFFFFFFFF;
    PREG(port, PORT_CI)  = 1u << slot;

    // Poll for completion
    to = 2000000;
    while (--to) {
        if (!(PREG(port, PORT_CI) & (1u << slot))) break;
        if (PREG(port, PORT_IS)  & (1u << 30)) return -1; // TFES error
    }
    if (!to) return -1;
    if (PREG(port, PORT_IS) & (1u << 30)) return -1;

    return 0;
}

// Public interface — same signature as ata_read_drive/ata_write_drive

int ahci_read_drive(uint8_t drive, uint32_t lba, uint8_t* buf, uint32_t sectors) {
    (void)drive; // single drive for now
    if (!ahci_ok) return -1;
    // Use DMA buffer if buf isn't physically contiguous / aligned
    while (sectors > 0) {
        uint32_t chunk = sectors > 128 ? 128 : sectors;
        if (ahci_issue(ahci_port_n, lba, dma_buf, chunk, 0) < 0) return -1;
        for (uint32_t i = 0; i < chunk * 512; i++) buf[i] = dma_buf[i];
        buf += chunk * 512;
        lba += chunk;
        sectors -= chunk;
    }
    return 0;
}

int ahci_write_drive(uint8_t drive, uint32_t lba, const uint8_t* buf, uint32_t sectors) {
    (void)drive;
    if (!ahci_ok) return -1;
    while (sectors > 0) {
        uint32_t chunk = sectors > 128 ? 128 : sectors;
        for (uint32_t i = 0; i < chunk * 512; i++) dma_buf[i] = buf[i];
        if (ahci_issue(ahci_port_n, lba, dma_buf, chunk, 1) < 0) return -1;
        buf += chunk * 512;
        lba += chunk;
        sectors -= chunk;
    }
    return 0;
}

uint32_t ahci_get_sectors(uint8_t drive) {
    (void)drive;
    if (!ahci_ok) return 0;
    // Issue IDENTIFY to get sector count
    // For now return a large number — real IDENTIFY takes more setup
    return 0; // caller falls back to legacy ATA for sector count
}
