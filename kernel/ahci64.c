// kernel/ahci64.c — Milestone 28: AHCI/SATA driver. See
// include/ahci64.h for the design rationale.
#include <stdint.h>
#include "../include/ahci64.h"
#include "../include/pci64.h"
#include "../include/physmem64.h"
#include "../include/heap64.h"
#include "../include/blockdev64.h"
#include "../include/klog.h"

// ── AHCI register/structure layout (spec-defined, packed) ───────────
typedef struct {
    uint32_t clb, clbu;
    uint32_t fb, fbu;
    uint32_t is, ie;
    uint32_t cmd;
    uint32_t rsv0;
    uint32_t tfd;
    uint32_t sig;
    uint32_t ssts, sctl, serr, sact;
    uint32_t ci;
    uint32_t sntf, fbs;
    uint32_t rsv1[11];
    uint32_t vendor[4];
} __attribute__((packed)) hba_port_t;

typedef struct {
    uint32_t cap, ghc, is, pi, vs;
    uint32_t ccc_ctl, ccc_pts;
    uint32_t em_loc, em_ctl;
    uint32_t cap2, bohc;
    uint8_t  rsv[0xA0 - 0x2C];
    uint8_t  vendor[0x100 - 0xA0];
    hba_port_t ports[32];
} __attribute__((packed)) hba_mem_t;

typedef struct {
    uint8_t fis_type;    // 0x27
    uint8_t pm_c;        // bit7 = C (this is a command FIS)
    uint8_t command;
    uint8_t featurel;
    uint8_t lba0, lba1, lba2;
    uint8_t device;
    uint8_t lba3, lba4, lba5;
    uint8_t featureh;
    uint8_t countl, counth;
    uint8_t icc;
    uint8_t control;
    uint8_t rsv1[4];
} __attribute__((packed)) fis_reg_h2d_t; // 20 bytes

typedef struct {
    uint16_t flags;   // bits0-4: CFL (dwords), bit6: W (write)
    uint16_t prdtl;
    uint32_t prdbc;
    uint32_t ctba, ctbau;
    uint32_t rsv1[4];
} __attribute__((packed)) hba_cmd_header_t; // 32 bytes

typedef struct {
    uint32_t dba, dbau;
    uint32_t rsv0;
    uint32_t dbc_i;   // bits0-21: byte count - 1
} __attribute__((packed)) hba_prdt_entry_t;

typedef struct {
    uint8_t cfis[64];
    uint8_t acmd[16];
    uint8_t rsv[48];
    hba_prdt_entry_t prdt[1]; // one entry -- every transfer goes through one contiguous DMA buffer
} __attribute__((packed)) hba_cmd_table_t;

#define AHCI_CMD_READ_DMA_EX  0x25
#define AHCI_CMD_WRITE_DMA_EX 0x35
#define AHCI_CMD_IDENTIFY     0xEC

#define AHCI_SIG_ATAPI      0xEB140101u
#define AHCI_SIG_ENCLOSURE  0xC33C0101u
#define AHCI_SIG_PM         0x96690101u

#define AHCI_DMA_PAGES             16  // 64KB per-port DMA buffer
#define AHCI_MAX_SECTORS_PER_CMD  128  // 64KB / 512 -- matches the DMA buffer size

typedef struct {
    hba_port_t* preg;
    uint8_t* cmd_list_virt;
    uint8_t* cmd_table_virt;
    uint8_t* dma_buf_virt;
    uint64_t dma_buf_phys;
    blockdev64_t bd;
} ahci_port_state_t;

static int ahci_wait_idle(hba_port_t* preg) {
    uint32_t timeout = 1000000;
    while (timeout--) {
        if (!(preg->tfd & 0x88)) return 0; // BSY|DRQ clear
    }
    return -1;
}

// Issues a command through slot 0 (the only slot this simple driver
// ever uses) and polls for completion -- see include/ahci64.h's header
// comment for why polling is this milestone's deliberate choice.
// Bounded by an iteration-count timeout (not a calibrated real-time
// one, matching kernel/ata64.c's own existing precedent) so a wedged
// or missing controller cannot hang the kernel forever.
static int ahci_issue(ahci_port_state_t* st, uint8_t ata_cmd, uint64_t lba,
                       uint16_t count, int is_write, uint32_t byte_count) {
    hba_port_t* preg = st->preg;
    if (ahci_wait_idle(preg) < 0) {
        klog("ahci64: port busy, refusing to issue command\n");
        return -1;
    }

    hba_cmd_header_t* hdr = (hba_cmd_header_t*)st->cmd_list_virt;
    hba_cmd_table_t* tbl = (hba_cmd_table_t*)st->cmd_table_virt;

    for (uint32_t i = 0; i < sizeof(tbl->cfis); i++) tbl->cfis[i] = 0;
    fis_reg_h2d_t* fis = (fis_reg_h2d_t*)tbl->cfis;
    fis->fis_type = 0x27;
    fis->pm_c     = 0x80;
    fis->command  = ata_cmd;
    fis->device   = 0x40; // LBA mode
    fis->lba0 = (uint8_t)(lba);       fis->lba1 = (uint8_t)(lba >> 8);  fis->lba2 = (uint8_t)(lba >> 16);
    fis->lba3 = (uint8_t)(lba >> 24); fis->lba4 = (uint8_t)(lba >> 32); fis->lba5 = (uint8_t)(lba >> 40);
    fis->countl = (uint8_t)count;
    fis->counth = (uint8_t)(count >> 8);

    tbl->prdt[0].dba  = (uint32_t)(st->dma_buf_phys & 0xFFFFFFFFu);
    tbl->prdt[0].dbau = (uint32_t)(st->dma_buf_phys >> 32);
    tbl->prdt[0].dbc_i = byte_count > 0 ? (byte_count - 1) : 0;

    hdr[0].flags = (uint16_t)(sizeof(fis_reg_h2d_t) / 4) & 0x1F; // CFL
    if (is_write) hdr[0].flags |= (1u << 6);
    hdr[0].prdtl = 1;
    hdr[0].prdbc = 0;

    preg->is = 0xFFFFFFFFu; // clear stale interrupt-status bits before issuing
    preg->ci = 1;           // issue slot 0

    uint32_t timeout = 20000000;
    while (timeout--) {
        if (preg->is & (1u << 30)) { // TFES -- task file error
            klog("ahci64: task file error\n");
            preg->is = (1u << 30);
            return -1;
        }
        if (!(preg->ci & 1)) return 0; // slot cleared -- command complete
    }

    klog("ahci64: command timeout\n");
    return -1;
}

static int ahci_bd_read(blockdev64_t* dev, uint64_t lba, uint8_t* buf, uint32_t count) {
    ahci_port_state_t* st = (ahci_port_state_t*)dev->driver_data;
    uint32_t done = 0;
    while (done < count) {
        uint32_t chunk = count - done;
        if (chunk > AHCI_MAX_SECTORS_PER_CMD) chunk = AHCI_MAX_SECTORS_PER_CMD;
        if (ahci_issue(st, AHCI_CMD_READ_DMA_EX, lba + done, (uint16_t)chunk, 0, chunk * 512) < 0) return -1;
        for (uint32_t i = 0; i < chunk * 512u; i++) buf[(uint64_t)done * 512 + i] = st->dma_buf_virt[i];
        done += chunk;
    }
    return (int)count;
}

static int ahci_bd_write(blockdev64_t* dev, uint64_t lba, const uint8_t* buf, uint32_t count) {
    ahci_port_state_t* st = (ahci_port_state_t*)dev->driver_data;
    uint32_t done = 0;
    while (done < count) {
        uint32_t chunk = count - done;
        if (chunk > AHCI_MAX_SECTORS_PER_CMD) chunk = AHCI_MAX_SECTORS_PER_CMD;
        for (uint32_t i = 0; i < chunk * 512u; i++) st->dma_buf_virt[i] = buf[(uint64_t)done * 512 + i];
        if (ahci_issue(st, AHCI_CMD_WRITE_DMA_EX, lba + done, (uint16_t)chunk, 1, chunk * 512) < 0) return -1;
        done += chunk;
    }
    return (int)count;
}

static const blockdev64_ops_t ahci_bd_ops = {
    .read  = ahci_bd_read,
    .write = ahci_bd_write,
};

static void ahci_stop_port(hba_port_t* preg) {
    preg->cmd &= ~(1u << 0); // ST
    uint32_t timeout = 500000;
    while (timeout-- && (preg->cmd & (1u << 15))) { } // wait CR clear
    preg->cmd &= ~(1u << 4); // FRE
    timeout = 500000;
    while (timeout-- && (preg->cmd & (1u << 14))) { } // wait FR clear
}

static void ahci_start_port(hba_port_t* preg) {
    uint32_t timeout = 500000;
    while (timeout-- && (preg->cmd & (1u << 15))) { } // wait CR clear before restarting
    preg->cmd |= (1u << 4); // FRE
    preg->cmd |= (1u << 0); // ST
}

static void build_name(char* out, const char* prefix, int a, int b) {
    int i = 0;
    while (prefix[i]) { out[i] = prefix[i]; i++; }
    // controller index (single digit is plenty -- QEMU never presents
    // more than a couple of AHCI controllers in this project's configs)
    out[i++] = (char)('0' + (a % 10));
    out[i++] = '.';
    out[i++] = (char)('0' + (b % 10));
    if (b >= 10) out[i++] = (char)('0' + (b / 10) % 10); // harmless extra digit for port >= 10, still unique
    out[i] = 0;
}

// Frees every DMA allocation for a port that failed to finish
// initializing -- called on every early-return path in ahci_init_port
// so a broken/unsupported port never leaks physical pages.
static void ahci_free_port_dma(uint64_t cmd_list_phys, uint64_t fis_phys,
                                uint64_t cmd_table_phys, uint64_t dma_phys) {
    if (cmd_list_phys)  physmem64_free_page(cmd_list_phys);
    if (fis_phys)       physmem64_free_page(fis_phys);
    if (cmd_table_phys) physmem64_free_page(cmd_table_phys);
    if (dma_phys)       physmem64_free_pages(dma_phys, AHCI_DMA_PAGES);
}

static int ahci_init_port(hba_port_t* preg, int ctrl_idx, int port_idx) {
    ahci_stop_port(preg);

    uint64_t cmd_list_phys  = physmem64_alloc_page();
    uint64_t fis_phys       = physmem64_alloc_page();
    uint64_t cmd_table_phys = physmem64_alloc_page();
    uint64_t dma_phys       = physmem64_alloc_pages(AHCI_DMA_PAGES);
    if (!cmd_list_phys || !fis_phys || !cmd_table_phys || !dma_phys) {
        klog("ahci64: DMA allocation failed for a port -- skipping\n");
        ahci_free_port_dma(cmd_list_phys, fis_phys, cmd_table_phys, dma_phys);
        return -1;
    }

    preg->clb  = (uint32_t)(cmd_list_phys & 0xFFFFFFFFu);
    preg->clbu = (uint32_t)(cmd_list_phys >> 32);
    preg->fb   = (uint32_t)(fis_phys & 0xFFFFFFFFu);
    preg->fbu  = (uint32_t)(fis_phys >> 32);
    preg->serr = 0xFFFFFFFFu;
    preg->is   = 0xFFFFFFFFu;

    hba_cmd_header_t* hdr = (hba_cmd_header_t*)physmem64_to_virt(cmd_list_phys);
    for (int i = 0; i < 32; i++) { hdr[i].flags = 0; hdr[i].prdtl = 0; hdr[i].prdbc = 0; hdr[i].ctba = 0; hdr[i].ctbau = 0; }
    hdr[0].ctba  = (uint32_t)(cmd_table_phys & 0xFFFFFFFFu);
    hdr[0].ctbau = (uint32_t)(cmd_table_phys >> 32);
    hdr[0].prdtl = 1;

    ahci_start_port(preg);

    ahci_port_state_t* st = (ahci_port_state_t*)kmalloc(sizeof(ahci_port_state_t));
    if (!st) {
        klog("ahci64: out of memory for port state\n");
        ahci_free_port_dma(cmd_list_phys, fis_phys, cmd_table_phys, dma_phys);
        return -1;
    }
    st->preg           = preg;
    st->cmd_list_virt  = (uint8_t*)physmem64_to_virt(cmd_list_phys);
    st->cmd_table_virt = (uint8_t*)physmem64_to_virt(cmd_table_phys);
    st->dma_buf_virt   = (uint8_t*)physmem64_to_virt(dma_phys);
    st->dma_buf_phys   = dma_phys;

    if (ahci_issue(st, AHCI_CMD_IDENTIFY, 0, 0, 0, 512) < 0) {
        klog("ahci64: IDENTIFY failed -- skipping port\n");
        kfree(st);
        ahci_free_port_dma(cmd_list_phys, fis_phys, cmd_table_phys, dma_phys);
        return -1;
    }

    uint16_t* id = (uint16_t*)st->dma_buf_virt;
    uint64_t sectors;
    if (id[83] & (1u << 10)) { // LBA48 supported
        sectors = ((uint64_t)id[103] << 48) | ((uint64_t)id[102] << 32) |
                  ((uint64_t)id[101] << 16) | id[100];
    } else {
        sectors = ((uint32_t)id[61] << 16) | id[60];
    }
    if (sectors == 0) {
        klog("ahci64: IDENTIFY reported zero sectors -- skipping port\n");
        kfree(st);
        ahci_free_port_dma(cmd_list_phys, fis_phys, cmd_table_phys, dma_phys);
        return -1;
    }

    st->bd.type               = BLOCKDEV64_TYPE_AHCI;
    st->bd.logical_block_size = 512;
    st->bd.block_count        = sectors;
    st->bd.ops                = &ahci_bd_ops;
    st->bd.driver_data        = st;
    build_name(st->bd.name, "ahci", ctrl_idx, port_idx);

    if (blockdev64_register(&st->bd) < 0) {
        kfree(st);
        ahci_free_port_dma(cmd_list_phys, fis_phys, cmd_table_phys, dma_phys);
        return -1;
    }
    return 0;
}

static int ahci_init_controller(pci64_device_t* dev, int ctrl_idx) {
    pci64_enable_device(dev);

    pci64_bar_t* bar5 = &dev->bar[5];
    if (bar5->type != PCI64_BAR_MEM32 && bar5->type != PCI64_BAR_MEM64) {
        klog("ahci64: ABAR (BAR5) is not MMIO -- skipping controller\n");
        return 0;
    }
    uint64_t abar_size = bar5->size;
    if (abar_size < 0x1100) abar_size = 0x1100; // generic regs + 32 ports worth, safety floor

    void* abar = physmem64_map_mmio(bar5->address, abar_size);
    if (!abar) {
        klog("ahci64: failed to map ABAR\n");
        return 0;
    }

    hba_mem_t* hba = (hba_mem_t*)abar;
    hba->ghc |= (1u << 31); // GHC.AE -- enable AHCI mode

    uint32_t pi = hba->pi;
    int registered = 0;
    for (int p = 0; p < 32; p++) {
        if (!(pi & (1u << p))) continue;

        hba_port_t* preg = &hba->ports[p];
        uint32_t ssts = preg->ssts;
        uint8_t det = (uint8_t)(ssts & 0xF);
        uint8_t ipm = (uint8_t)((ssts >> 8) & 0xF);
        if (det != 3 || ipm != 1) continue; // no device / no active link

        uint32_t sig = preg->sig;
        if (sig == AHCI_SIG_ATAPI || sig == AHCI_SIG_ENCLOSURE || sig == AHCI_SIG_PM) continue;

        if (ahci_init_port(preg, ctrl_idx, p) == 0) registered++;
    }
    return registered;
}

int ahci64_init(void) {
    int total = 0;
    int ctrl_idx = 0;
    pci64_device_t* dev = 0;
    while ((dev = pci64_find_class(PCI64_CLASS_STORAGE, PCI64_SUBCLASS_SATA, PCI64_PROGIF_AHCI, dev)) != 0) {
        total += ahci_init_controller(dev, ctrl_idx);
        ctrl_idx++;
    }
    return total;
}
