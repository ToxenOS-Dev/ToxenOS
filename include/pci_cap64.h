#ifndef PCI_CAP64_H
#define PCI_CAP64_H

// M+11B: strict PCI capability-list walking, MSI / MSI-X capability decode,
// and BAR/BIR/table/PBA validation. Everything here is PURE (no MMIO, no
// writes): it works over a read-only config view, so production devices can
// be inspected with a type that has no write entries at all, and tests can
// drive it over a fake config space.
#include <stdint.h>
#include "pci64.h"

#define PCI_CAP_ID_MSI   0x05
#define PCI_CAP_ID_MSIX  0x11

// Read-only view of one function's config space.
typedef struct {
    void*    ctx;
    uint8_t  (*r8) (void* ctx, uint8_t off);
    uint16_t (*r16)(void* ctx, uint8_t off);
    uint32_t (*r32)(void* ctx, uint8_t off);
} pci_cfg_ro_t;

// A view over a real (or, under a swapped backend, fake) device via the
// native-width pci64_config_* accessors.
void pci_cfg_ro_for_dev(const pci64_device_t* dev, pci_cfg_ro_t* out);

typedef enum {
    PCI_CAP_FOUND = 0,
    PCI_CAP_NOT_FOUND = 1,
    PCI_CAP_MALFORMED = 2,
} pci_cap_status_t;

#define PCI_CAP_WALK_MAX 48

// Walks the standard capability list from the head. Validates: Status
// capabilities-list bit; pointers >= 0x40, <= 0xFC and 4-byte ALIGNED
// (a pointer with low bits set is MALFORMED, not masked); loops and
// self-loops (visited set); a hop cap; a dead device (id and next both
// 0xFF, or Status all-ones). `start_after` (0 = none) skips matches up to
// and including the capability at that offset. *first_off receives the
// first match after start_after; *match_count the number of matches of
// `cap_id` in the WHOLE chain (so callers can reject duplicates).
pci_cap_status_t pci_cap_find(const pci_cfg_ro_t* cfg, uint8_t cap_id, uint8_t start_after,
                              int* first_off, int* match_count);

// ── MSI ──────────────────────────────────────────────────────────────
typedef struct {
    uint8_t  off;
    uint16_t ctrl;
    int      is64;
    int      per_vec_mask;
    int      mmc_log2;       // Multiple Message Capable: 0..5 -> 1..32 vectors
    int      mme_log2;       // Multiple Message Enable as currently programmed
    int      mme_exceeds_mmc;
    int      enabled;
    uint8_t  addr_lo_off, addr_hi_off, data_off, mask_off, pending_off; // 0 = absent
    uint8_t  len;            // capability length in bytes (10/14/20/24)
} pci_msi_cap_t;

#define PCI_MSI_CTRL_ENABLE   0x0001
#define PCI_MSI_CTRL_MMC_MASK 0x000E
#define PCI_MSI_CTRL_MME_MASK 0x0070
#define PCI_MSI_CTRL_64BIT    0x0080
#define PCI_MSI_CTRL_PVM      0x0100

// Decode the MSI capability at `off`. 0 on success; negative: -1 reserved
// MMC encoding (6/7), -2 reserved MME encoding, -3 capability does not fit
// below 0x100.
int pci_msi_decode(const pci_cfg_ro_t* cfg, uint8_t off, pci_msi_cap_t* out);

// ── MSI-X ────────────────────────────────────────────────────────────
typedef struct {
    uint8_t  off;
    uint16_t ctrl;
    uint32_t table_size;     // entries, 1..2048
    int      enabled;
    int      func_masked;
    uint8_t  table_bir, pba_bir;
    uint32_t table_off, pba_off;   // byte offsets within their BARs (low 3 bits stripped)
} pci_msix_cap_t;

#define PCI_MSIX_CTRL_QSIZE   0x07FF
#define PCI_MSIX_CTRL_MASKALL 0x4000
#define PCI_MSIX_CTRL_ENABLE  0x8000
#define PCI_MSIX_ENTRY_SIZE   16
#define PCI_MSIX_ENTRY_ADDR_LO 0x0
#define PCI_MSIX_ENTRY_ADDR_HI 0x4
#define PCI_MSIX_ENTRY_DATA    0x8
#define PCI_MSIX_ENTRY_CTRL    0xC
#define PCI_MSIX_ENTRY_MASKBIT 0x1

// 0 on success; -1 reserved BIR (6/7); -3 capability does not fit.
int pci_msix_decode(const pci_cfg_ro_t* cfg, uint8_t off, pci_msix_cap_t* out);

typedef struct {
    uint64_t table_phys, table_len;
    uint64_t pba_phys,   pba_len;
} pci_msix_regions_t;

// Resolves BAR base + encoded offset for the table and the PBA against the
// device's ALREADY-PROBED BAR sizes (kernel/pci64.c decode_bar, done at
// enumeration -- BARs are never re-sized here). Returns 0, or:
//  -1 BIR out of range   -2 BAR is not memory (unused, I/O, or the high
//  dword slot of a 64-bit BAR)   -3 table outside the BAR   -4 PBA outside
//  the BAR   -5 arithmetic overflow   -6 table and PBA overlap
int pci_msix_resolve(const pci_msix_cap_t* cap, const pci64_bar_t bars[6], pci_msix_regions_t* out);

#endif // PCI_CAP64_H
