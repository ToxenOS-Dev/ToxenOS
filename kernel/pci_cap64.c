// kernel/pci_cap64.c -- M+11B strict capability walking + MSI/MSI-X decode.
// See include/pci_cap64.h.
#include "../include/pci_cap64.h"

static uint8_t  dev_r8 (void* c, uint8_t off) { const pci64_device_t* d = (const pci64_device_t*)c; return pci64_config_read8 (d->bus, d->slot, d->func, off); }
static uint16_t dev_r16(void* c, uint8_t off) { const pci64_device_t* d = (const pci64_device_t*)c; return pci64_config_read16(d->bus, d->slot, d->func, off); }
static uint32_t dev_r32(void* c, uint8_t off) { const pci64_device_t* d = (const pci64_device_t*)c; return pci64_config_read32(d->bus, d->slot, d->func, off); }

void pci_cfg_ro_for_dev(const pci64_device_t* dev, pci_cfg_ro_t* out) {
    out->ctx = (void*)dev; out->r8 = dev_r8; out->r16 = dev_r16; out->r32 = dev_r32;
}

pci_cap_status_t pci_cap_find(const pci_cfg_ro_t* cfg, uint8_t cap_id, uint8_t start_after,
                              int* first_off, int* match_count) {
    if (first_off) *first_off = -1;
    if (match_count) *match_count = 0;

    uint16_t status = cfg->r16(cfg->ctx, 0x06);
    if (status == 0xFFFF) return PCI_CAP_MALFORMED;            // device gone
    if (!(status & 0x10)) return PCI_CAP_NOT_FOUND;            // no capability list

    uint8_t next = cfg->r8(cfg->ctx, 0x34);
    uint8_t visited[PCI_CAP_WALK_MAX];
    int nvisited = 0, matches = 0, passed = (start_after == 0);
    int first = -1;

    while (next != 0) {
        if (next & 3) return PCI_CAP_MALFORMED;                // misaligned pointer
        if (next < 0x40 || next > 0xFC) return PCI_CAP_MALFORMED;
        for (int i = 0; i < nvisited; i++) if (visited[i] == next) return PCI_CAP_MALFORMED;   // loop
        if (nvisited >= PCI_CAP_WALK_MAX) return PCI_CAP_MALFORMED;
        visited[nvisited++] = next;

        uint8_t id = cfg->r8(cfg->ctx, next);
        uint8_t nx = cfg->r8(cfg->ctx, (uint8_t)(next + 1));
        if (id == 0xFF && nx == 0xFF) return PCI_CAP_MALFORMED;

        if (id == cap_id) {
            matches++;
            if (passed && first < 0) first = next;
        }
        if (next == start_after) passed = 1;
        next = nx;
    }
    if (match_count) *match_count = matches;
    if (first >= 0) { if (first_off) *first_off = first; return PCI_CAP_FOUND; }
    return PCI_CAP_NOT_FOUND;
}

int pci_msi_decode(const pci_cfg_ro_t* cfg, uint8_t off, pci_msi_cap_t* o) {
    uint16_t ctrl = cfg->r16(cfg->ctx, (uint8_t)(off + 2));
    int mmc = (ctrl & PCI_MSI_CTRL_MMC_MASK) >> 1;
    int mme = (ctrl & PCI_MSI_CTRL_MME_MASK) >> 4;
    if (mmc > 5) return -1;
    if (mme > 5) return -2;
    o->off = off; o->ctrl = ctrl;
    o->is64 = !!(ctrl & PCI_MSI_CTRL_64BIT);
    o->per_vec_mask = !!(ctrl & PCI_MSI_CTRL_PVM);
    o->mmc_log2 = mmc; o->mme_log2 = mme; o->mme_exceeds_mmc = mme > mmc;
    o->enabled = !!(ctrl & PCI_MSI_CTRL_ENABLE);
    o->addr_lo_off = (uint8_t)(off + 4);
    o->addr_hi_off = o->is64 ? (uint8_t)(off + 8) : 0;
    o->data_off    = o->is64 ? (uint8_t)(off + 12) : (uint8_t)(off + 8);
    if (o->per_vec_mask) {
        o->mask_off    = o->is64 ? (uint8_t)(off + 16) : (uint8_t)(off + 12);
        o->pending_off = (uint8_t)(o->mask_off + 4);
    } else { o->mask_off = 0; o->pending_off = 0; }
    o->len = (uint8_t)((o->is64 ? 14 : 10) + (o->per_vec_mask ? 10 : 0));
    if ((uint32_t)off + o->len > 0x100) return -3;
    return 0;
}

int pci_msix_decode(const pci_cfg_ro_t* cfg, uint8_t off, pci_msix_cap_t* o) {
    if ((uint32_t)off + 12 > 0x100) return -3;
    uint16_t ctrl = cfg->r16(cfg->ctx, (uint8_t)(off + 2));
    uint32_t tbl = cfg->r32(cfg->ctx, (uint8_t)(off + 4));
    uint32_t pba = cfg->r32(cfg->ctx, (uint8_t)(off + 8));
    o->off = off; o->ctrl = ctrl;
    o->table_size = (uint32_t)(ctrl & PCI_MSIX_CTRL_QSIZE) + 1;
    o->enabled = !!(ctrl & PCI_MSIX_CTRL_ENABLE);
    o->func_masked = !!(ctrl & PCI_MSIX_CTRL_MASKALL);
    o->table_bir = (uint8_t)(tbl & 7); o->table_off = tbl & ~7u;
    o->pba_bir = (uint8_t)(pba & 7);   o->pba_off = pba & ~7u;
    if (o->table_bir > 5 || o->pba_bir > 5) return -1;
    return 0;
}

int pci_msix_resolve(const pci_msix_cap_t* cap, const pci64_bar_t bars[6], pci_msix_regions_t* out) {
    if (cap->table_bir > 5 || cap->pba_bir > 5) return -1;
    const pci64_bar_t* tb = &bars[cap->table_bir];
    const pci64_bar_t* pb = &bars[cap->pba_bir];
    if ((tb->type != PCI64_BAR_MEM32 && tb->type != PCI64_BAR_MEM64) || tb->size == 0) return -2;
    if ((pb->type != PCI64_BAR_MEM32 && pb->type != PCI64_BAR_MEM64) || pb->size == 0) return -2;

    uint64_t tlen = (uint64_t)cap->table_size * PCI_MSIX_ENTRY_SIZE;
    uint64_t plen = (((uint64_t)cap->table_size + 63) / 64) * 8;
    if (cap->table_off > tb->size || tlen > tb->size - cap->table_off) return -3;
    if (cap->pba_off > pb->size || plen > pb->size - cap->pba_off) return -4;

    uint64_t tphys = tb->address + cap->table_off;
    uint64_t pphys = pb->address + cap->pba_off;
    if (tphys < tb->address || pphys < pb->address) return -5;
    if (tphys + tlen < tphys || pphys + plen < pphys) return -5;

    if (cap->table_bir == cap->pba_bir) {
        uint64_t t0 = cap->table_off, t1 = t0 + tlen, p0 = cap->pba_off, p1 = p0 + plen;
        if (t0 < p1 && p0 < t1) return -6;
    }
    out->table_phys = tphys; out->table_len = tlen;
    out->pba_phys = pphys;   out->pba_len = plen;
    return 0;
}
