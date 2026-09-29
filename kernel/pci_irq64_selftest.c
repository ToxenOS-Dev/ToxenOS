// kernel/pci_irq64_selftest.c -- M+11B deterministic boot-time tests for the
// PCI MSI / MSI-X foundation. Everything runs against a FAKE config space
// (native-width backend, byte-lane accurate incl. write-1-to-clear Status),
// a FAKE MSI-X table, and a PRIVATE irq64 table -- production hardware and
// the live interrupt table are never touched. The only real-hardware step
// is the read-only inspection at the end, run under a write-blocking spy.
#include <stdint.h>
#include "../include/pci_irq64.h"
#include "../include/pci_cap64.h"
#include "../include/msi_x86_64.h"
#include "../include/vector64.h"
#include "../include/ioapic64.h"
#include "../include/lapic64.h"
#include "../include/heap64.h"
#include "../include/klog.h"

static int g_pass, g_fail, g_group_fail;
static void check(int cond, const char* what) {
    if (cond) { g_pass++; return; }
    g_fail++; g_group_fail++;
    klog("  pci_irq64 selftest FAIL: "); klog(what); klog("\n");
}
#define CHECK(c) check((c), #c)
static void group_begin(void) { g_group_fail = 0; }
static void group_end(const char* name) {
    klog(g_group_fail ? "pci_irq64 selftest FAIL: " : "pci_irq64 selftest PASS: ");
    klog(name); klog("\n");
}
static void zero(void* p, unsigned n) { volatile uint8_t* b = (volatile uint8_t*)p; for (unsigned i = 0; i < n; i++) b[i] = 0; }
static int eq(const void* a, const void* b, unsigned n) { const uint8_t* x = a; const uint8_t* y = b; for (unsigned i = 0; i < n; i++) if (x[i] != y[i]) return 0; return 1; }
static void cp(void* d, const void* s, unsigned n) { uint8_t* x = d; const uint8_t* y = s; for (unsigned i = 0; i < n; i++) x[i] = y[i]; }
static inline uint64_t rflags(void) { uint64_t f; __asm__ volatile ("pushfq; pop %0" : "=r"(f)); return f; }

// ═══════════════════ fake devices: config space + MSI-X table ═══════════════
#define NFAKE 4
#define MSI_OFF  0x50
#define MSIX_OFF 0x70
#define FBUS  0xEE
#define TBL_ENTRIES 64
static uint8_t  fcs[NFAKE][256];
static uint8_t  fwm[NFAKE][256];                 // per-byte writable-bit mask
static uint32_t ftab[NFAKE][TBL_ENTRIES * 4];
static uint32_t fpba[NFAKE][8];
static pci64_device_t fdev[NFAKE];
static int if_violations, tbl_order_violations;

typedef struct { uint8_t kind, width, dev, msien, maskall; uint16_t off; uint32_t val; uint32_t gen; } ev_t;
static uint32_t g_read_gen;   // lock generation observed by the most recent raw READ
static ev_t ev[2048]; static int nev;
static void ev_reset(void) { nev = 0; }
static void ev_add(int kind, int dev, uint16_t off, int width, uint32_t val) {
    if (nev < 2048) { ev[nev].kind = (uint8_t)kind; ev[nev].dev = (uint8_t)dev; ev[nev].off = off; ev[nev].width = (uint8_t)width; ev[nev].val = val; ev[nev].gen = pci64_cfg_lock_generation();
        ev[nev].msien = fcs[dev][MSI_OFF + 2] & 1;                 // MSI Enable / MSI-X MASKALL as they stood BEFORE this write
        ev[nev].maskall = (fcs[dev][MSIX_OFF + 3] & 0x40) ? 1 : 0;
        nev++; }
}
static int cfg_writes(void) { int n = 0; for (int i = 0; i < nev; i++) if (ev[i].kind == 0) n++; return n; }

// Filters: return 1 to accept the write, 0 to drop it (stuck bit / dead register).
static int (*f_cfg_filter)(int dev, uint8_t off, int width, uint32_t val) = 0;
static int (*f_tbl_filter)(int dev, uint32_t off, uint32_t val) = 0;
// Observers (M+11C boundary tests): see the software state at the exact instant of a config write / read.
static void (*f_cfg_wr_obs)(int dev, uint8_t off, int width, uint32_t val) = 0;   // before the write applies
static void (*f_cfg_rd_obs)(int dev, uint8_t off, int width) = 0;                 // before the read returns

static inline void chk_if(void) { if (rflags() & 0x200) if_violations++; }
static uint8_t  fr8 (uint8_t b, uint8_t s, uint8_t f, uint8_t o) { (void)b; (void)s; chk_if(); g_read_gen = pci64_cfg_lock_generation(); return fcs[f & 3][o]; }
static uint16_t fr16(uint8_t b, uint8_t s, uint8_t f, uint8_t o) { (void)b; (void)s; chk_if(); g_read_gen = pci64_cfg_lock_generation(); if (f_cfg_rd_obs) f_cfg_rd_obs(f & 3, o, 2); return (uint16_t)(fcs[f & 3][o] | (fcs[f & 3][o + 1] << 8)); }
static uint32_t fr32(uint8_t b, uint8_t s, uint8_t f, uint8_t o) { (void)b; (void)s; chk_if(); g_read_gen = pci64_cfg_lock_generation(); return (uint32_t)fcs[f & 3][o] | ((uint32_t)fcs[f & 3][o + 1] << 8) | ((uint32_t)fcs[f & 3][o + 2] << 16) | ((uint32_t)fcs[f & 3][o + 3] << 24); }
static void fwr(int d, uint8_t off, int width, uint32_t v) {
    chk_if();
    ev_add(0, d, off, width, v);
    if (f_cfg_wr_obs) f_cfg_wr_obs(d, off, width, v);
    if (f_cfg_filter && !f_cfg_filter(d, off, width, v)) return;
    for (int i = 0; i < width; i++) {
        uint8_t byte = (uint8_t)(v >> (8 * i)); int idx = off + i;
        if (idx == 6 || idx == 7) fcs[d][idx] = (uint8_t)(fcs[d][idx] & ~byte);       // Status is write-1-to-clear
        else fcs[d][idx] = (uint8_t)((fcs[d][idx] & ~fwm[d][idx]) | (byte & fwm[d][idx]));
    }
}
static void fw8 (uint8_t b, uint8_t s, uint8_t f, uint8_t o, uint8_t v)  { (void)b; (void)s; fwr(f & 3, o, 1, v); }
static void fw16(uint8_t b, uint8_t s, uint8_t f, uint8_t o, uint16_t v) { (void)b; (void)s; fwr(f & 3, o, 2, v); }
static void fw32(uint8_t b, uint8_t s, uint8_t f, uint8_t o, uint32_t v) { (void)b; (void)s; fwr(f & 3, o, 4, v); }
static const pci64_cfg_backend_t fake_be = { fr8, fr16, fr32, fw8, fw16, fw32 };

#define FTAB_BASE(i) (0xFEB00000ull + 0x10000ull * (i))
static void* fm_map(uint64_t phys, uint64_t len) {
    (void)len;
    for (int i = 0; i < NFAKE; i++) {
        if (phys == FTAB_BASE(i)) return ftab[i];
        if (phys == FTAB_BASE(i) + 0x800) return fpba[i];
    }
    return 0;
}
static int tbl_dev(void* base) { for (int i = 0; i < NFAKE; i++) if (base == (void*)ftab[i]) return i; return -1; }
static uint32_t fm_r32(void* base, uint32_t off) { return ((uint32_t*)base)[off / 4]; }
static void fm_w32(void* base, uint32_t off, uint32_t v) {
    int d = tbl_dev(base);
    if (d >= 0) {
        ev_add(1, d, (uint16_t)off, 4, v);
        uint32_t reg = off % 16, entry = off / 16;
        // The spec: an entry's address/data may only change while it is masked.
        if (reg < 12 && !(ftab[d][entry * 4 + 3] & 1u)) tbl_order_violations++;
        if (f_tbl_filter && !f_tbl_filter(d, off, v)) return;
    }
    ((uint32_t*)base)[off / 4] = v;
}
static const pci_mmio_ops_t fake_mmio = { fm_map, fm_r32, fm_w32 };

// Layout: PM cap @0x40, MSI @0x50, MSI-X @0x70.
static void fdev_init(int d, int msi, int is64, int pvm, int mmc, int msix, int nmsix) {
    zero(fcs[d], 256); zero(ftab[d], sizeof(ftab[d])); zero(fpba[d], sizeof(fpba[d]));
    for (int i = 0; i < 256; i++) fwm[d][i] = 0xFF;
    fcs[d][0] = 0x34; fcs[d][1] = 0x12; fcs[d][2] = 0xE8; fcs[d][3] = 0x11;
    fcs[d][4] = 0x07; fcs[d][5] = 0x00;                       // Command: IO|MEM|BME
    fcs[d][6] = 0x10; fcs[d][7] = 0x00;                       // Status: capabilities list
    fcs[d][0x34] = 0x40;
    fcs[d][0x40] = 0x01; fcs[d][0x41] = (uint8_t)(msi ? MSI_OFF : (msix ? MSIX_OFF : 0));
    if (msi) {
        uint16_t ctrl = (uint16_t)((mmc << 1) | (is64 ? 0x80 : 0) | (pvm ? 0x100 : 0));
        fcs[d][MSI_OFF] = 0x05; fcs[d][MSI_OFF + 1] = (uint8_t)(msix ? MSIX_OFF : 0);
        fcs[d][MSI_OFF + 2] = (uint8_t)ctrl; fcs[d][MSI_OFF + 3] = (uint8_t)(ctrl >> 8);
        fwm[d][MSI_OFF + 2] = 0x71; fwm[d][MSI_OFF + 3] = 0x00;    // only Enable + MME writable
        if (pvm) {                                                // mask reg with non-trivial neighbours
            int mo = is64 ? MSI_OFF + 16 : MSI_OFF + 12;
            fcs[d][mo] = 0xAA; fcs[d][mo + 1] = 0xAA; fcs[d][mo + 2] = 0xAA; fcs[d][mo + 3] = 0xAA;
        }
    }
    if (msix) {
        fcs[d][MSIX_OFF] = 0x11; fcs[d][MSIX_OFF + 1] = 0;
        uint16_t ctrl = (uint16_t)(nmsix - 1);
        fcs[d][MSIX_OFF + 2] = (uint8_t)ctrl; fcs[d][MSIX_OFF + 3] = (uint8_t)(ctrl >> 8);
        fwm[d][MSIX_OFF + 2] = 0x00; fwm[d][MSIX_OFF + 3] = 0xC0;  // only Enable + MASKALL writable
        fcs[d][MSIX_OFF + 4] = 0x01;                              // table: BIR 1, offset 0
        fcs[d][MSIX_OFF + 8] = 0x01; fcs[d][MSIX_OFF + 9] = 0x08; // PBA: BIR 1, offset 0x800
    }
    pci64_device_t* dv = &fdev[d];
    zero(dv, sizeof(*dv));
    dv->bus = FBUS; dv->slot = 0; dv->func = (uint8_t)d; dv->vendor_id = 0x1234; dv->device_id = 0x11E8;
    for (int i = 0; i < 6; i++) dv->bar[i].type = PCI64_BAR_NONE;
    dv->bar[1].type = PCI64_BAR_MEM32; dv->bar[1].address = FTAB_BASE(d); dv->bar[1].size = 0x1000;
}
static uint16_t f_ctrl_msi(int d)  { return (uint16_t)(fcs[d][MSI_OFF + 2] | (fcs[d][MSI_OFF + 3] << 8)); }
static uint16_t f_ctrl_msix(int d) { return (uint16_t)(fcs[d][MSIX_OFF + 2] | (fcs[d][MSIX_OFF + 3] << 8)); }
static uint16_t f_cmd(int d)       { return (uint16_t)(fcs[d][4] | (fcs[d][5] << 8)); }
static uint32_t f_dw(int d, int off) { return (uint32_t)fcs[d][off] | ((uint32_t)fcs[d][off + 1] << 8) | ((uint32_t)fcs[d][off + 2] << 16) | ((uint32_t)fcs[d][off + 3] << 24); }   // direct read: not a driver access

// ═══════════════════ private irq64 table + counters ═════════════════════════
static irq64_table_t g_pt;
static irq64_table_t* g_saved_tbl;
static int g_eoi_hook_count;
static void eoi_hook(void) { g_eoi_hook_count++; }

static void pt_reset(void) {
    for (int i = 0; i < NFAKE; i++) if (fdev[i].irq_state) { kfree(fdev[i].irq_state); fdev[i].irq_state = 0; }
    irq64_table_init(&g_pt, 0);
    g_pt.apic_mode = 1; g_pt.dest_apic = 3;      // not zero: the composer must use the real destination
    ev_reset(); f_cfg_filter = 0; f_tbl_filter = 0; tbl_order_violations = 0;
    pci_irq64_test_arm(0);
}

static int hA_calls, hB_calls, hC_calls; static irq64_ret_t hA_ret, hB_ret;
static int ctxA, ctxB, ctxC;
static irq64_ret_t hA(void* c) { (void)c; hA_calls++; return hA_ret; }
static irq64_ret_t hB(void* c) { (void)c; hB_calls++; return hB_ret; }
static irq64_ret_t hC(void* c) { (void)c; hC_calls++; return IRQ64_RET_HANDLED; }
static void counters_reset(void) { hA_calls = hB_calls = hC_calls = 0; hA_ret = hB_ret = IRQ64_RET_HANDLED; }

static int vec_of(pci64_irq_set_t* s, int i) { pci64_irq_vector_t v; if (pci64_irq_vector(s, i, &v)) return -1; return irq64_table_msg_vector(&g_pt, v.irq); }
static int irq_of(pci64_irq_set_t* s, int i) { pci64_irq_vector_t v; if (pci64_irq_vector(s, i, &v)) return -1; return v.irq; }

// alloc+bind all with handler `h`
static pci64_irq_set_t* mk_bound(int d, unsigned flags, int min, int max, irq64_handler_t h, void* ctx) {
    pci64_irq_set_t* s = 0;
    int n = pci64_irq_alloc(&fdev[d], min, max, flags, &s);
    if (n < 0 || !s) return 0;
    for (int i = 0; i < n; i++) if (pci64_irq_request(s, i, h, ctx, "t") != 0) return 0;
    return s;
}
static int all_msg_vectors_free(void) {
    for (int i = 16; i < IRQ64_MAX; i++) if (g_pt.desc[i].allocated) return 0;
    for (int v = 0; v < 256; v++) { if (g_pt.vmap.state[v] == VEC_ALLOCATED) return 0; if (v >= 0x30 && g_pt.vec_to_irq[v] != -1) return 0; }
    return 1;
}

// ═════════════════════════ T1: native-width config backend ═══════════════════
static void test_config_backend(void) {
    group_begin();
    fdev_init(0, 1, 1, 0, 0, 0, 0);
    ev_reset();
    uint64_t entry_if = rflags() & 0x200;

    // Writing Command must not clear Status (write-1-to-clear).
    fcs[0][6] = 0x10; fcs[0][7] = 0xA0;                       // Status: cap list + two RW1C error bits
    pci64_config_write16(FBUS, 0, 0, 0x04, 0x0406);
    CHECK(fcs[0][6] == 0x10 && fcs[0][7] == 0xA0);            // Status untouched
    CHECK(f_cmd(0) == 0x0406);
    CHECK(nev == 1 && ev[0].width == 2 && ev[0].off == 4);    // ONE native 16-bit write
    // Negative control: the OLD style 32-bit RMW covering Status DOES clear it in this model.
    pci64_config_write32(FBUS, 0, 0, 0x04, 0x0406u | (0xA010u << 16));
    CHECK(fcs[0][7] == 0x00);

    // Update helper: preserves unrelated bits, one critical section.
    for (int pass = 0; pass < 2; pass++) {
        fdev_init(0, 1, 1, 0, 0, 0, 0);
        fcs[0][4] = 0x07; fcs[0][5] = 0x01; fcs[0][7] = 0x80;
        ev_reset(); if_violations = 0;
        if (pass == 1) __asm__ volatile ("sti" ::: "memory"); else __asm__ volatile ("cli" ::: "memory");
        uint16_t old = pci64_cfg_update16(FBUS, 0, 0, 0x04, 0x0002, 0x0400);
        uint64_t if_after = rflags() & 0x200;
        if (pass == 1) __asm__ volatile ("cli" ::: "memory");
        CHECK(old == 0x0107);
        CHECK(f_cmd(0) == (uint16_t)((0x0107 & ~0x0002) | 0x0400));
        CHECK(fcs[0][7] == 0x80);                              // Status still intact
        CHECK(nev == 1 && ev[0].width == 2);                   // exactly one write, native width
        CHECK(if_violations == 0);                             // raw read AND write both ran with IF=0
        CHECK(ev[0].gen == g_read_gen);                        // ...inside the SAME critical section (no gap between them)
        CHECK((if_after != 0) == (pass == 1));                 // entry IF restored exactly
    }
    if (entry_if) __asm__ volatile ("sti" ::: "memory"); else __asm__ volatile ("cli" ::: "memory");

    // update8/update32 keep their own width.
    fdev_init(0, 1, 1, 0, 0, 0, 0); ev_reset();
    pci64_cfg_update8(FBUS, 0, 0, 0x3C, 0x0F, 0xA0);
    CHECK(nev == 1 && ev[0].width == 1);
    fcs[0][0x60] = 0xF0; fcs[0][0x61] = 0x0F; fcs[0][0x62] = 0xFF; fcs[0][0x63] = 0x00; ev_reset();
    pci64_cfg_update32(FBUS, 0, 0, 0x60, 0x000000FFu, 0x01000000u);
    CHECK(f_dw(0, 0x60) == ((0x00FF0FF0u & ~0xFFu) | 0x01000000u) && nev == 1 && ev[0].width == 4);

    // MSI Message Control write leaves capability ID / next pointer alone.
    fdev_init(0, 1, 1, 0, 0, 0, 0); ev_reset();
    pci64_cfg_update16(FBUS, 0, 0, MSI_OFF + 2, 0, 0x0001);
    CHECK(nev == 1 && ev[0].off == MSI_OFF + 2 && ev[0].width == 2);
    CHECK(fcs[0][MSI_OFF] == 0x05 && fcs[0][MSI_OFF + 1] == 0);
    // 16-bit MSI data write leaves adjacent fields alone, both layouts.
    fdev_init(0, 1, 1, 0, 0, 0, 0);                            // 64-bit: data @+12, reserved @+14
    fcs[0][MSI_OFF + 14] = 0xCD; fcs[0][MSI_OFF + 15] = 0xEF; fcs[0][MSI_OFF + 11] = 0x77; ev_reset();
    pci64_config_write16(FBUS, 0, 0, MSI_OFF + 12, 0x1234);
    CHECK(fcs[0][MSI_OFF + 12] == 0x34 && fcs[0][MSI_OFF + 13] == 0x12);
    CHECK(fcs[0][MSI_OFF + 14] == 0xCD && fcs[0][MSI_OFF + 15] == 0xEF && fcs[0][MSI_OFF + 11] == 0x77);
    CHECK(nev == 1 && ev[0].width == 2);
    fdev_init(0, 1, 0, 1, 0, 0, 0);                            // 32-bit + mask: data @+8, reserved @+10, mask @+12
    fcs[0][MSI_OFF + 10] = 0xAB; fcs[0][MSI_OFF + 11] = 0xBC; ev_reset();
    pci64_config_write16(FBUS, 0, 0, MSI_OFF + 8, 0x4321);
    CHECK(fcs[0][MSI_OFF + 10] == 0xAB && fcs[0][MSI_OFF + 11] == 0xBC && fcs[0][MSI_OFF + 12] == 0xAA);

    // Misaligned accesses are rejected and counted, never masked to a neighbour.
    ev_reset(); uint32_t m0 = pci64_cfg_misaligned_count();
    CHECK(pci64_config_read16(FBUS, 0, 0, 0x05) == 0xFFFF);
    CHECK(pci64_config_read32(FBUS, 0, 0, 0x06) == 0xFFFFFFFFu);
    pci64_config_write16(FBUS, 0, 0, 0x05, 0x1111);
    pci64_config_write32(FBUS, 0, 0, 0x06, 0x22222222u);
    CHECK(pci64_cfg_misaligned_count() == m0 + 4 && nev == 0);
    CHECK(pci64_config_read8(FBUS, 0, 0, 0x05) == fcs[0][5]);  // bytes are fine at any offset
    group_end("PCI config backend: native widths, atomic update in one IF=0 section, W1C Status safe, no adjacent-field writes, misalignment rejected");
}

// ═════════════════════════ T2: strict capability walker ═════════════════════
static void put_cap(int d, int off, int id, int next) { fcs[d][off] = (uint8_t)id; fcs[d][off + 1] = (uint8_t)next; }
static int find(int d, int id, int after, int* first, int* cnt) {
    pci_cfg_ro_t ro; pci_cfg_ro_for_dev(&fdev[d], &ro);
    int f, c; int st = pci_cap_find(&ro, (uint8_t)id, (uint8_t)after, &f, &c);
    if (first) *first = f;
    if (cnt) *cnt = c;
    return st;
}
// Reference implementation of the OLD walker's contract (for compatibility checks).
static int old_find(int d, int id, int start_after) {
    if (!(fcs[d][6] & 0x10)) return -1;
    uint8_t next = (start_after == 0) ? (uint8_t)(fcs[d][0x34] & 0xFC) : (uint8_t)(fcs[d][start_after + 1] & 0xFC);
    int guard = 0;
    while (next != 0 && guard++ < 48) {
        if (next < 0x40) return -1;
        if (fcs[d][next] == id) return next;
        next = (uint8_t)(fcs[d][next + 1] & 0xFC);
    }
    return -1;
}
static void test_cap_walker(void) {
    group_begin();
    int first, cnt;
    fdev_init(0, 1, 1, 0, 0, 1, 4);                            // PM -> MSI -> MSI-X
    CHECK(find(0, 0x05, 0, &first, &cnt) == PCI_CAP_FOUND && first == MSI_OFF && cnt == 1);
    CHECK(find(0, 0x11, 0, &first, &cnt) == PCI_CAP_FOUND && first == MSIX_OFF && cnt == 1);   // both present
    CHECK(find(0, 0x01, 0, &first, &cnt) == PCI_CAP_FOUND && first == 0x40);
    CHECK(find(0, 0x09, 0, &first, &cnt) == PCI_CAP_NOT_FOUND);

    // Resume semantics: two vendor caps.
    fdev_init(0, 0, 0, 0, 0, 0, 0);
    put_cap(0, 0x40, 0x01, 0x70); put_cap(0, 0x70, 0x09, 0x80); put_cap(0, 0x80, 0x09, 0);
    CHECK(find(0, 0x09, 0, &first, &cnt) == PCI_CAP_FOUND && first == 0x70 && cnt == 2);
    CHECK(find(0, 0x09, 0x70, &first, &cnt) == PCI_CAP_FOUND && first == 0x80);
    CHECK(find(0, 0x09, 0x80, &first, &cnt) == PCI_CAP_NOT_FOUND);
    CHECK(pci64_find_capability(&fdev[0], 0x09, 0) == 0x70);
    CHECK(pci64_find_capability(&fdev[0], 0x09, 0x70) == 0x80);
    CHECK(pci64_find_capability(&fdev[0], 0x09, 0x80) == -1);

    // Malformed: misaligned head, misaligned next, below 0x40, above 0xFC, loop, self-loop.
    fdev_init(0, 0, 0, 0, 0, 0, 0); fcs[0][0x34] = 0x41;
    CHECK(find(0, 0x05, 0, 0, 0) == PCI_CAP_MALFORMED);
    CHECK(pci64_find_capability(&fdev[0], 0x05, 0) == -1);      // wrapper: -1, not a masked-and-found offset
    fdev_init(0, 0, 0, 0, 0, 0, 0); put_cap(0, 0x40, 0x01, 0x53);
    CHECK(find(0, 0x05, 0, 0, 0) == PCI_CAP_MALFORMED);
    fdev_init(0, 0, 0, 0, 0, 0, 0); put_cap(0, 0x40, 0x01, 0x3C);
    CHECK(find(0, 0x05, 0, 0, 0) == PCI_CAP_MALFORMED);
    fdev_init(0, 0, 0, 0, 0, 0, 0); put_cap(0, 0x40, 0x01, 0xFE);
    CHECK(find(0, 0x05, 0, 0, 0) == PCI_CAP_MALFORMED);
    fdev_init(0, 0, 0, 0, 0, 0, 0); put_cap(0, 0x40, 0x01, 0x50); put_cap(0, 0x50, 0x09, 0x40);
    CHECK(find(0, 0x05, 0, 0, 0) == PCI_CAP_MALFORMED);         // 40 -> 50 -> 40
    fdev_init(0, 0, 0, 0, 0, 0, 0); put_cap(0, 0x40, 0x01, 0x40);
    CHECK(find(0, 0x05, 0, 0, 0) == PCI_CAP_MALFORMED);         // self-loop
    fdev_init(0, 0, 0, 0, 0, 0, 0); fcs[0][0x34] = 0xFC; put_cap(0, 0xFC, 0x05, 0);
    CHECK(find(0, 0x05, 0, &first, 0) == PCI_CAP_FOUND && first == 0xFC);    // 0xFC is the last legal slot
    // Dead device / all-ones.
    fdev_init(0, 0, 0, 0, 0, 0, 0); put_cap(0, 0x40, 0xFF, 0xFF);
    CHECK(find(0, 0x05, 0, 0, 0) == PCI_CAP_MALFORMED);
    fdev_init(0, 0, 0, 0, 0, 0, 0); fcs[0][6] = 0xFF; fcs[0][7] = 0xFF;
    CHECK(find(0, 0x05, 0, 0, 0) == PCI_CAP_MALFORMED);
    // Status capabilities bit clear.
    fdev_init(0, 1, 1, 0, 0, 0, 0); fcs[0][6] = 0x00;
    CHECK(find(0, 0x05, 0, 0, 0) == PCI_CAP_NOT_FOUND);
    // Full 48-slot chain is legal and walkable.
    fdev_init(0, 0, 0, 0, 0, 0, 0); fcs[0][0x34] = 0x40;
    for (int o = 0x40; o <= 0xFC; o += 4) put_cap(0, o, o == 0xFC ? 0x05 : 0x01, o == 0xFC ? 0 : o + 4);
    CHECK(find(0, 0x05, 0, &first, &cnt) == PCI_CAP_FOUND && first == 0xFC && cnt == 1);
    // Duplicate MSI capabilities are visible to callers.
    fdev_init(0, 1, 1, 0, 0, 0, 0); put_cap(0, 0x40, 0x01, MSI_OFF); put_cap(0, MSI_OFF, 0x05, 0x60); put_cap(0, 0x60, 0x05, 0);
    CHECK(find(0, 0x05, 0, &first, &cnt) == PCI_CAP_FOUND && cnt == 2);

    // Compatibility: identical to the old walker on well-formed lists.
    fdev_init(0, 1, 1, 0, 0, 1, 4);
    int same = 1;
    static const int ids[] = { 0x01, 0x05, 0x11, 0x09, 0x10 };
    static const int afters[] = { 0, 0x40, MSI_OFF, MSIX_OFF };
    for (unsigned i = 0; i < sizeof(ids) / sizeof(ids[0]); i++)
        for (unsigned j = 0; j < sizeof(afters) / sizeof(afters[0]); j++)
            if (pci64_find_capability(&fdev[0], (uint8_t)ids[i], (uint8_t)afters[j]) != old_find(0, ids[i], afters[j])) same = 0;
    CHECK(same);
    group_end("strict capability walker (malformed/misaligned/loop/dead), MSI+MSI-X discovery, pci64_find_capability compatibility");
}

// ═════════════════════════ T3/T4: decode + BAR/table validation ══════════════
static void test_decode(void) {
    group_begin();
    pci_cfg_ro_t ro; pci_msi_cap_t m; pci_msix_cap_t x;
    // MSI layouts.
    fdev_init(0, 1, 0, 0, 0, 0, 0); pci_cfg_ro_for_dev(&fdev[0], &ro);
    CHECK(pci_msi_decode(&ro, MSI_OFF, &m) == 0 && !m.is64 && !m.per_vec_mask && m.data_off == MSI_OFF + 8 && m.addr_hi_off == 0 && m.mask_off == 0 && m.len == 10);
    fdev_init(0, 1, 1, 0, 0, 0, 0);
    CHECK(pci_msi_decode(&ro, MSI_OFF, &m) == 0 && m.is64 && !m.per_vec_mask && m.addr_hi_off == MSI_OFF + 8 && m.data_off == MSI_OFF + 12 && m.len == 14);
    fdev_init(0, 1, 0, 1, 0, 0, 0);
    CHECK(pci_msi_decode(&ro, MSI_OFF, &m) == 0 && !m.is64 && m.per_vec_mask && m.data_off == MSI_OFF + 8 && m.mask_off == MSI_OFF + 12 && m.pending_off == MSI_OFF + 16 && m.len == 20);
    fdev_init(0, 1, 1, 1, 0, 0, 0);
    CHECK(pci_msi_decode(&ro, MSI_OFF, &m) == 0 && m.is64 && m.per_vec_mask && m.mask_off == MSI_OFF + 16 && m.pending_off == MSI_OFF + 20 && m.len == 24);
    // MMC: 0..5 valid (1..32 vectors), 6/7 reserved.
    for (int mmc = 0; mmc < 8; mmc++) {
        fdev_init(0, 1, 1, 0, mmc, 0, 0);
        int rc = pci_msi_decode(&ro, MSI_OFF, &m);
        if (mmc <= 5) CHECK(rc == 0 && m.mmc_log2 == mmc && (1 << m.mmc_log2) == (1 << mmc));
        else CHECK(rc == -1);
    }
    // MME: 0..5 valid, 6/7 reserved; MME > MMC flagged.
    for (int mme = 0; mme < 8; mme++) {
        fdev_init(0, 1, 1, 0, 2, 0, 0);
        fcs[0][MSI_OFF + 2] = (uint8_t)((fcs[0][MSI_OFF + 2] & ~0x70) | (mme << 4));
        int rc = pci_msi_decode(&ro, MSI_OFF, &m);
        if (mme <= 5) CHECK(rc == 0 && m.mme_log2 == mme && m.mme_exceeds_mmc == (mme > 2));
        else CHECK(rc == -2);
    }
    fdev_init(0, 1, 1, 1, 0, 0, 0);
    CHECK(pci_msi_decode(&ro, 0xF8, &m) == -3);                 // 24-byte capability does not fit below 0x100
    fdev_init(0, 1, 1, 0, 0, 0, 0); fcs[0][MSI_OFF + 2] |= 1;
    CHECK(pci_msi_decode(&ro, MSI_OFF, &m) == 0 && m.enabled);
    // MSI-X.
    fdev_init(0, 0, 0, 0, 0, 1, 2048); pci_cfg_ro_for_dev(&fdev[0], &ro);
    CHECK(pci_msix_decode(&ro, MSIX_OFF, &x) == 0 && x.table_size == 2048 && x.table_bir == 1 && x.table_off == 0 && x.pba_bir == 1 && x.pba_off == 0x800);
    fdev_init(0, 0, 0, 0, 0, 1, 1);
    CHECK(pci_msix_decode(&ro, MSIX_OFF, &x) == 0 && x.table_size == 1);
    fcs[0][MSIX_OFF + 4] = 0x0B; fcs[0][MSIX_OFF + 5] = 0x30;    // BIR 3, offset 0x3008 & ~7 -> 0x3008
    CHECK(pci_msix_decode(&ro, MSIX_OFF, &x) == 0 && x.table_bir == 3 && x.table_off == 0x3008);
    fcs[0][MSIX_OFF + 4] = 0x06; CHECK(pci_msix_decode(&ro, MSIX_OFF, &x) == -1);   // reserved BIR
    fcs[0][MSIX_OFF + 4] = 0x07; CHECK(pci_msix_decode(&ro, MSIX_OFF, &x) == -1);
    fdev_init(0, 0, 0, 0, 0, 1, 4); fcs[0][MSIX_OFF + 8] = 0x06; CHECK(pci_msix_decode(&ro, MSIX_OFF, &x) == -1);   // reserved PBA BIR
    CHECK(pci_msix_decode(&ro, 0xF8, &x) == -3);
    fdev_init(0, 0, 0, 0, 0, 1, 4); fcs[0][MSIX_OFF + 3] = 0xC0;
    CHECK(pci_msix_decode(&ro, MSIX_OFF, &x) == 0 && x.enabled && x.func_masked);

    // BAR / BIR / table / PBA resolution.
    pci64_bar_t bars[6]; pci_msix_regions_t r; pci_msix_cap_t c;
    for (int i = 0; i < 6; i++) { bars[i].type = PCI64_BAR_NONE; bars[i].address = 0; bars[i].size = 0; bars[i].prefetchable = 0; }
    bars[1].type = PCI64_BAR_MEM32; bars[1].address = 0xFEB00000ull; bars[1].size = 0x1000;
    bars[2].type = PCI64_BAR_IO;    bars[2].address = 0xC000;       bars[2].size = 0x40;
    bars[4].type = PCI64_BAR_MEM64; bars[4].address = 0x100000000ull; bars[4].size = 0x4000;   // bars[5] = high dword slot (NONE)
    c.table_size = 3; c.table_bir = 1; c.table_off = 0; c.pba_bir = 1; c.pba_off = 0x800;
    CHECK(pci_msix_resolve(&c, bars, &r) == 0 && r.table_phys == 0xFEB00000ull && r.table_len == 48 && r.pba_phys == 0xFEB00800ull && r.pba_len == 8);
    c.table_bir = 4; c.pba_bir = 4; c.table_off = 0x2000; c.pba_off = 0x3000; c.table_size = 65;
    CHECK(pci_msix_resolve(&c, bars, &r) == 0 && r.table_phys == 0x100002000ull && r.table_len == 1040 && r.pba_len == 16);   // 64-bit BAR, high address preserved
    c.table_bir = 5; CHECK(pci_msix_resolve(&c, bars, &r) == -2);            // names the HIGH dword slot of a 64-bit BAR
    c.table_bir = 2; CHECK(pci_msix_resolve(&c, bars, &r) == -2);            // I/O BAR
    c.table_bir = 0; CHECK(pci_msix_resolve(&c, bars, &r) == -2);            // unused BAR
    c.table_bir = 6; CHECK(pci_msix_resolve(&c, bars, &r) == -1);
    c.table_bir = 1; c.pba_bir = 1; c.table_off = 0; c.pba_off = 0x800; c.table_size = 2048;
    CHECK(pci_msix_resolve(&c, bars, &r) == -3);                             // 32 KiB table in a 4 KiB BAR
    c.table_size = 3; c.table_off = 0x2000; CHECK(pci_msix_resolve(&c, bars, &r) == -3);   // offset beyond BAR
    c.table_off = 0; c.pba_off = 0xFF8; c.table_size = 256; CHECK(pci_msix_resolve(&c, bars, &r) == -4);  // table just fits the BAR, PBA does not
    c.table_size = 3; c.pba_off = 0x1000; CHECK(pci_msix_resolve(&c, bars, &r) == -4);     // PBA outside
    c.pba_off = 0x20; CHECK(pci_msix_resolve(&c, bars, &r) == -6);           // PBA overlaps the table
    bars[3].type = PCI64_BAR_MEM64; bars[3].address = 0xFFFFFFFFFFFFF000ull; bars[3].size = 0x2000;
    c.table_bir = 3; c.pba_bir = 3; c.table_off = 0x1000; c.pba_off = 0x1800; c.table_size = 3;
    CHECK(pci_msix_resolve(&c, bars, &r) == -5);                             // base + offset wraps 2^64
    bars[3].type = PCI64_BAR_MEM32; bars[3].size = 0;
    CHECK(pci_msix_resolve(&c, bars, &r) == -2);                             // zero-sized BAR
    group_end("MSI/MSI-X capability decode (MMC/MME/BIR/offsets) + BAR/BIR/table/PBA validation (32/64-bit, I/O, high slot, bounds, overflow)");
}

// ═════════════════════════ T5: composer + vector predicate ═══════════════════
static void test_composer(void) {
    group_begin();
    msi_x86_msg_t m; int dyn = 0, ok_all = 1;
    for (int v = -1; v <= 256; v++) {
        int pred = vector64_is_dynamic_device_vector(v);
        int rc = msi_x86_compose(5, v, &m);
        if (pred) dyn++;
        if ((rc == 0) != pred) ok_all = 0;
    }
    CHECK(dyn == 190);
    CHECK(ok_all);                                                  // composer accepts EXACTLY the dynamic vectors
    CHECK(msi_x86_compose(0, 0x80, &m) == MSI_X86_ERR_VECTOR && m.address_lo == 0 && m.data == 0);   // the syscall vector
    CHECK(msi_x86_compose(0, 0x7F, &m) == 0 && msi_x86_compose(0, 0x81, &m) == 0);
    CHECK(msi_x86_compose(0, 0x30, &m) == 0 && msi_x86_compose(0, 0xEE, &m) == 0);
    for (int v = 0x00; v <= 0x1F; v++) CHECK(msi_x86_compose(0, v, &m) == MSI_X86_ERR_VECTOR);   // exceptions
    for (int v = 0x20; v <= 0x2F; v++) CHECK(msi_x86_compose(0, v, &m) == MSI_X86_ERR_VECTOR);   // fixed legacy
    for (int v = 0xEF; v <= 0xFE; v++) CHECK(msi_x86_compose(0, v, &m) == MSI_X86_ERR_VECTOR);   // system/IPI
    CHECK(msi_x86_compose(0, 0xFF, &m) == MSI_X86_ERR_VECTOR);                                   // LAPIC spurious
    CHECK(msi_x86_compose(0, 0x100, &m) == MSI_X86_ERR_VECTOR && msi_x86_compose(0, -5, &m) == MSI_X86_ERR_VECTOR);
    // Bits.
    CHECK(msi_x86_compose(5, 0x31, &m) == 0);
    CHECK(m.address_lo == 0xFEE05000u && m.address_hi == 0 && m.data == 0x31);
    CHECK((m.address_lo & 0x0000000Cu) == 0);                      // RH = 0, DM = 0 (physical)
    CHECK((m.data & 0xFFFFFF00u) == 0);                            // Fixed delivery, edge, no level bit
    CHECK(msi_x86_compose(0, 0x31, &m) == 0 && m.address_lo == 0xFEE00000u);   // dest 0 accepted (not special-cased)
    CHECK(msi_x86_compose(0xFE, 0x31, &m) == 0 && m.address_lo == 0xFEEFE000u);
    CHECK(msi_x86_compose(0xFF, 0x31, &m) == MSI_X86_ERR_DEST && m.address_lo == 0 && m.data == 0);   // broadcast
    CHECK(msi_x86_compose(0x100, 0x31, &m) == MSI_X86_ERR_DEST);
    CHECK(msi_x86_compose(0xFFFFFFFFu, 0x31, &m) == MSI_X86_ERR_DEST);
    // The allocator only ever hands out vectors the predicate accepts.
    vector64_map_t vm; irq_vector_map_init(&vm);
    int all_ok = 1; for (int i = 0; i < 190; i++) { int v = irq_vector_alloc(&vm, i); if (!vector64_is_dynamic_device_vector(v)) all_ok = 0; }
    CHECK(all_ok);
    group_end("x86 MSI composer + shared vector predicate: exhaustive 256-vector check, 0x80 rejected, bit layout, destination limits");
}

// ═════════════════════════ T6: allocation / IRQ integration / flows ══════════
static void test_msi_flow(void) {
    group_begin();
    pt_reset(); counters_reset();
    fdev_init(0, 1, 1, 0, 0, 0, 0);                              // plain 64-bit MSI, no per-vector mask (edu-like)
    pci64_irq_set_t* s = 0;
    uint32_t io0 = ioapic64_op_count; g_eoi_hook_count = 0;

    CHECK(pci64_irq_alloc(&fdev[0], 1, 4, PCI64_IRQ_ANY, &s) == 1 && s);          // MSI: one vector even when 4 are offered
    CHECK(pci64_irq_type(s) == PCI64_IRQ_TYPE_MSI && pci64_irq_state(s) == PCI_IRQSET_ALLOCATED);
    pci64_irq_vector_t pv; CHECK(pci64_irq_vector(s, 0, &pv) == 0 && pv.irq >= 16 && pv.dev_index == 0);
    int vec = vec_of(s, 0);
    CHECK(vec >= 0x30 && vec != 0x80 && vector64_is_dynamic_device_vector(vec));
    CHECK(g_pt.vec_to_irq[vec] == pv.irq && irq_vector_state(&g_pt.vmap, vec) == VEC_ALLOCATED);
    CHECK(cfg_writes() == 0);                                     // alloc touches no device register
    CHECK(pci64_irq_enable(s) == PCI64_IRQ_ERR_BADSTATE && cfg_writes() == 0);   // ALLOCATED: enable refused
    CHECK(pci64_irq_request(s, 0, hA, &ctxA, "A") == 0 && pci64_irq_state(s) == PCI_IRQSET_BOUND);
    CHECK(pci64_irq_request(s, 0, hA, &ctxA, "A") == PCI64_IRQ_ERR_BADSTATE);      // each vector bound once
    CHECK(pci64_irq_alloc(&fdev[0], 1, 1, PCI64_IRQ_ANY, &(pci64_irq_set_t*){0}) == PCI64_IRQ_ERR_ALREADY_OWNED);

    // The synthetic delivery BEFORE go-live is harmless: counted unhandled? (handler is bound; hardware silent)
    ev_reset();
    CHECK(pci64_irq_enable(s) == 0 && pci64_irq_state(s) == PCI_IRQSET_LIVE);
    CHECK(f_ctrl_msi(0) & 1);                                     // MSI Enable set
    CHECK(((f_ctrl_msi(0) >> 4) & 7) == 0);                       // MME = 0: one vector
    CHECK(f_dw(0, MSI_OFF + 4) == 0xFEE03000u && f_dw(0, MSI_OFF + 8) == 0);       // destination = real BSP id (3), not 0
    CHECK((f_dw(0, MSI_OFF + 12) & 0xFFFF) == (uint32_t)vec);
    CHECK(f_cmd(0) & 0x0400);                                     // INTx Disable set at the transition
    CHECK(g_pt.desc[pv.irq].sealed);
    // Ordering: address/data writes, then INTx disable, then Enable LAST.
    int i_intx = -1, i_en = -1, last_msg = -1;
    for (int i = 0; i < nev; i++) {
        if (ev[i].off == 4 && ev[i].width == 2) i_intx = i;
        if (ev[i].off == MSI_OFF + 2 && (ev[i].val & 1)) i_en = i;
        if (ev[i].off == MSI_OFF + 4 || ev[i].off == MSI_OFF + 8 || ev[i].off == MSI_OFF + 12) last_msg = i;
    }
    CHECK(i_intx > last_msg && i_en > i_intx && i_en == nev - 1);
    { int en_writes = 0, early = 0;
      for (int i = 0; i < nev; i++) { if (ev[i].off == MSI_OFF + 2 && (ev[i].val & 1)) en_writes++; if (i < nev - 1 && ev[i].msien) early = 1; }
      CHECK(en_writes == 1);                                    // MSI Enable is written exactly once: the final boundary write
      CHECK(!early); }                                          // every other write happened while MSI Enable was still 0

    // Dispatch: LAPIC-EOI path, handler NONE/HANDLED, no IOAPIC.
    irq64_table_dispatch(&g_pt, (uint8_t)vec);
    CHECK(hA_calls == 1 && g_pt.desc[pv.irq].count == 1 && g_pt.desc[pv.irq].eoi_count == 1 && g_pt.desc[pv.irq].handled == 1);
    hA_ret = IRQ64_RET_NONE; irq64_table_dispatch(&g_pt, (uint8_t)vec);
    CHECK(hA_calls == 2 && g_pt.desc[pv.irq].unhandled == 1 && g_pt.desc[pv.irq].eoi_count == 2 && g_pt.desc[pv.irq].storm_masks == 0);
    // (message chips call the EOI hook the harness installed instead of the real LAPIC)
    CHECK(g_pt.desc[pv.irq].src == IRQ64_SRC_MSI && g_pt.desc[pv.irq].trigger == IRQ64_TRIG_EDGE);
    CHECK(ioapic64_op_count == io0);                              // no IOAPIC involvement at all

    // Plain MSI has no hardware mask: disable is refused, descriptor stays enabled, no config writes.
    ev_reset();
    CHECK(irq64_table_ctl(&g_pt, pv.irq, 0) == IRQ64_E_NOTSUP);
    CHECK(g_pt.desc[pv.irq].masked == 0 && g_pt.desc[pv.irq].can_mask == 0 && cfg_writes() == 0 && (f_ctrl_msi(0) & 1));

    // Sealed: free refused, nothing released, handler/context pinned.
    int free_before = irq_vector_free_count(&g_pt.vmap);
    CHECK(pci64_irq_free(s) == PCI64_IRQ_ERR_SEALED);
    CHECK(irq_vector_free_count(&g_pt.vmap) == free_before && g_pt.vec_to_irq[vec] == pv.irq);
    CHECK(g_pt.desc[pv.irq].handler == hA && g_pt.desc[pv.irq].allocated);
    CHECK(pci64_irq_enable(s) == PCI64_IRQ_ERR_BADSTATE && pci64_irq_request(s, 0, hB, &ctxB, "B") == PCI64_IRQ_ERR_BADSTATE);
    CHECK(irq64_table_msg_bind(&g_pt, pv.irq, hB, &ctxB, "B") == IRQ64_E_BUSY);    // rebinding refused at the core too
    CHECK(irq64_table_msg_free(&g_pt, pv.irq) == IRQ64_E_BUSY);

    // Quiesce (driver contract: device already quiet): plain MSI is gated by clearing Enable.
    CHECK(pci64_irq_quiesce(s) == 0 && pci64_irq_state(s) == PCI_IRQSET_QUIESCED && !(f_ctrl_msi(0) & 1));
    CHECK(pci64_irq_quiesce(s) == 0);                             // idempotent
    CHECK(pci64_irq_free(s) == PCI64_IRQ_ERR_SEALED && pci64_irq_enable(s) == PCI64_IRQ_ERR_BADSTATE);
    CHECK(irq64_table_msg_vector(&g_pt, pv.irq) == vec && g_pt.desc[pv.irq].handler == hA);
    // Stale message after quiesce: still dispatches ONLY to the pinned original owner.
    hA_ret = IRQ64_RET_HANDLED; int a0 = hA_calls;
    irq64_table_dispatch(&g_pt, (uint8_t)vec);
    CHECK(hA_calls == a0 + 1);
    group_end("plain MSI end-to-end: alloc/bind/enable order, MME=0, real destination, edge+LAPIC-EOI dispatch, NONE/HANDLED, -ENOTSUP mask, sealed after live");
}

static void test_msi_mask(void) {
    group_begin();
    pt_reset(); counters_reset();
    fdev_init(0, 1, 1, 1, 0, 0, 0);                              // 64-bit MSI WITH per-vector masking; mask reg = 0xAAAAAAAA
    pci64_irq_set_t* s = mk_bound(0, PCI64_IRQ_MSI, 1, 1, hA, &ctxA);
    CHECK(s != 0);
    int mo = MSI_OFF + 16;
    ev_reset();
    CHECK(pci64_irq_enable(s) == 0);
    { int en_writes = 0, early = 0;
      for (int i = 0; i < nev; i++) { if (ev[i].off == MSI_OFF + 2 && (ev[i].val & 1)) en_writes++; if (i < nev - 1 && ev[i].msien) early = 1; }
      CHECK(en_writes == 1 && !early && ev[nev - 1].off == MSI_OFF + 2); }   // Enable last even with a per-vector mask
    CHECK((f_dw(0, mo) & 1) == 0 && (f_dw(0, mo) & ~1u) == 0xAAAAAAAAu);   // vector 0 unmasked, neighbours preserved
    int irq = irq_of(s, 0);
    CHECK(g_pt.desc[irq].can_mask == 1 && g_pt.desc[irq].masked == 0);
    ev_reset();
    CHECK(irq64_table_ctl(&g_pt, irq, 0) == 0);                   // real hardware mask
    CHECK((f_dw(0, mo) & 1) == 1 && (f_dw(0, mo) & ~1u) == 0xAAAAAAAAu && g_pt.desc[irq].masked == 1);
    int touched_ctrl = 0; for (int i = 0; i < nev; i++) if (ev[i].off == MSI_OFF + 2) touched_ctrl = 1;
    CHECK(!touched_ctrl && (f_ctrl_msi(0) & 1));                  // MSI Enable is NOT used as a runtime mask
    CHECK(irq64_table_ctl(&g_pt, irq, 1) == 0 && (f_dw(0, mo) & 1) == 0 && g_pt.desc[irq].masked == 0);
    fcs[0][MSI_OFF + 20] = 0x01;                                  // pending register bit 0
    CHECK(pci64_irq_pending(s, 0) == 1);
    group_end("MSI with per-vector mask: real mask-register toggling, neighbours preserved, Enable never used as a mask, pending read");
}

static void test_msix_flow(void) {
    group_begin();
    pt_reset(); counters_reset();
    fdev_init(0, 0, 0, 0, 0, 1, 8);
    // Stale firmware/previous-owner table contents: some unmasked, steering-tag bits set.
    for (int e = 0; e < 8; e++) { ftab[0][e * 4] = 0xFEE0AA00u + (uint32_t)e; ftab[0][e * 4 + 1] = 0; ftab[0][e * 4 + 2] = 0x40 + (uint32_t)e; ftab[0][e * 4 + 3] = (e & 1) ? 1u : 0u; }
    ftab[0][1 * 4 + 3] = 0x00010001u;                             // entry 1: masked WITH a steering tag
    ftab[0][0 * 4 + 3] = 0x00020000u;                             // entry 0: unmasked WITH a steering tag
    uint32_t io0 = ioapic64_op_count;

    pci64_irq_set_t* s = mk_bound(0, PCI64_IRQ_ANY, 1, 3, hA, &ctxA);
    CHECK(s && pci64_irq_type(s) == PCI64_IRQ_TYPE_MSIX && pci64_irq_count(s) == 3);
    int vecs[3], irqs[3];
    for (int i = 0; i < 3; i++) { vecs[i] = vec_of(s, i); irqs[i] = irq_of(s, i); pci64_irq_vector_t v; pci64_irq_vector(s, i, &v); CHECK(v.dev_index == (uint16_t)i && g_pt.desc[irqs[i]].src == IRQ64_SRC_MSIX); }
    CHECK(vecs[0] != vecs[1] && vecs[1] != vecs[2] && vecs[0] != vecs[2] && irqs[0] != irqs[1]);
    ev_reset();
    CHECK(pci64_irq_enable(s) == 0 && pci64_irq_state(s) == PCI_IRQSET_LIVE);
    CHECK(tbl_order_violations == 0);                             // address/data only ever written while the entry was masked
    CHECK((f_ctrl_msix(0) & 0x8000) && !(f_ctrl_msix(0) & 0x4000));   // ENABLE set, MASKALL clear
    for (int i = 0; i < 3; i++) {
        CHECK(ftab[0][i * 4] == (0xFEE00000u | (3u << 12)) && ftab[0][i * 4 + 1] == 0 && ftab[0][i * 4 + 2] == (uint32_t)vecs[i]);
        CHECK((ftab[0][i * 4 + 3] & 1u) == 0);                    // used entries unmasked
    }
    CHECK(ftab[0][3] == 0x00020000u);                             // steering tag preserved on entry 0
    for (int e = 3; e < 8; e++) CHECK(ftab[0][e * 4 + 3] & 1u);   // every OTHER entry masked (stale entries cannot fire)
    CHECK(f_cmd(0) & 0x0400);
    // Ordering of the go-live sequence.
    int i_first_cfg = -1, i_first_tbl = -1, i_intx = -1, i_last_cfg = -1, i_first_unmask = -1, last_msg = -1;
    for (int i = 0; i < nev; i++) {
        if (ev[i].kind == 0 && i_first_cfg < 0) i_first_cfg = i;
        if (ev[i].kind == 1 && i_first_tbl < 0) i_first_tbl = i;
        if (ev[i].kind == 0 && ev[i].off == 4) i_intx = i;
        if (ev[i].kind == 0 && ev[i].off == MSIX_OFF + 2) i_last_cfg = i;
        if (ev[i].kind == 1 && (ev[i].off % 16) < 12 && (ev[i].off / 16) < 3) last_msg = i;
        if (ev[i].kind == 1 && (ev[i].off % 16) == 12 && (ev[i].off / 16) < 3 && !(ev[i].val & 1) && i_first_unmask < 0) i_first_unmask = i;
    }
    CHECK(i_first_cfg >= 0 && i_first_cfg < i_first_tbl);                       // MASKALL|ENABLE before any table write
    CHECK(ev[i_first_cfg].off == MSIX_OFF + 2 && (ev[i_first_cfg].val & 0xC000) == 0xC000);   // one write sets both
    CHECK(i_intx > last_msg && i_first_unmask > i_intx);                          // INTx disable after messages, before unmask
    CHECK(i_last_cfg == nev - 1 && !(ev[nev - 1].val & 0x4000));                // clearing MASKALL is the LAST write
    { int clears = 0, unmasked_table_write = 0;
      for (int i = 0; i < nev; i++) {
          if (ev[i].kind == 0 && ev[i].off == MSIX_OFF + 2 && !(ev[i].val & 0x4000)) clears++;
          if (ev[i].kind == 1 && !ev[i].maskall) unmasked_table_write = 1;
      }
      CHECK(clears == 1);                                       // MASKALL is cleared exactly once, at the boundary
      CHECK(!unmasked_table_write); }                           // every table write happened under MASKALL
    // Dispatch to each vector reaches its own descriptor, LAPIC EOI, no IOAPIC.
    g_eoi_hook_count = 0;
    for (int i = 0; i < 3; i++) irq64_table_dispatch(&g_pt, (uint8_t)vecs[i]);
    CHECK(hA_calls == 3 && g_eoi_hook_count == 3 && ioapic64_op_count == io0);
    for (int i = 0; i < 3; i++) CHECK(g_pt.desc[irqs[i]].count == 1 && g_pt.desc[irqs[i]].eoi_count == 1);
    // Per-entry mask semantics: one table bit, function mask untouched.
    ev_reset();
    CHECK(irq64_table_ctl(&g_pt, irqs[1], 0) == 0);
    CHECK((ftab[0][1 * 4 + 3] & 1u) && !(ftab[0][0 * 4 + 3] & 1u) && !(ftab[0][2 * 4 + 3] & 1u));
    CHECK(!(f_ctrl_msix(0) & 0x4000) && cfg_writes() == 0 && g_pt.desc[irqs[1]].masked == 1);
    CHECK(irq64_table_ctl(&g_pt, irqs[1], 1) == 0 && !(ftab[0][1 * 4 + 3] & 1u) && g_pt.desc[irqs[1]].masked == 0);
    // PBA is read-only diagnostics.
    fpba[0][0] = 0x2; CHECK(pci64_irq_pending(s, 1) == 1 && pci64_irq_pending(s, 0) == 0);
    // Quiesce: function mask.
    CHECK(pci64_irq_quiesce(s) == 0 && (f_ctrl_msix(0) & 0x4000) && pci64_irq_state(s) == PCI_IRQSET_QUIESCED);
    CHECK(pci64_irq_free(s) == PCI64_IRQ_ERR_SEALED);
    CHECK(g_pt.desc[irqs[0]].handler == hA && g_pt.vec_to_irq[vecs[2]] == irqs[2]);
    group_end("MSI-X end-to-end: MASKALL|ENABLE first, masked-only message writes, all other entries masked, INTx late, MASKALL clear LAST, per-entry mask");
}

// ── failure injection at every forward step ────────────────────────────
typedef struct { uint8_t cs[256]; uint32_t tab[TBL_ENTRIES * 4]; uint32_t pba[8]; } snap_t;
static void snap_take(int d, snap_t* s) { cp(s->cs, fcs[d], 256); cp(s->tab, ftab[d], sizeof(s->tab)); cp(s->pba, fpba[d], sizeof(s->pba)); }
static int snap_same(int d, const snap_t* s) { return eq(s->cs, fcs[d], 256) && eq(s->tab, ftab[d], sizeof(s->tab)); }

static void inject_all_steps(const char* label, int msix, int is64, int pvm, int preset_maskall) {
    (void)label;
    int sealed_seen = 0, bound_seen = 0, total = 0;
    for (int k = 1; k < 80; k++) {
        pt_reset(); counters_reset();
        if (msix) {
            fdev_init(0, 0, 0, 0, 0, 1, 6);
            for (int e = 0; e < 6; e++) { ftab[0][e * 4] = 0xDEAD0000u + (uint32_t)e; ftab[0][e * 4 + 1] = 0x11 + (uint32_t)e; ftab[0][e * 4 + 2] = 0x22 + (uint32_t)e; ftab[0][e * 4 + 3] = (e % 3 == 0) ? 0u : 1u; }
            if (preset_maskall) fcs[0][MSIX_OFF + 3] = 0x40;
        } else {
            fdev_init(0, 1, is64, pvm, 0, 0, 0);
            fcs[0][MSI_OFF + 4] = 0x11; fcs[0][MSI_OFF + 5] = 0x22; fcs[0][MSI_OFF + 6] = 0x33; fcs[0][MSI_OFF + 7] = 0x44;   // stale message
        }
        snap_t sn; snap_take(0, &sn);
        pci64_irq_set_t* s = mk_bound(0, msix ? PCI64_IRQ_MSIX : PCI64_IRQ_MSI, 1, msix ? 3 : 1, hA, &ctxA);
        if (!s) { CHECK(0); return; }
        ev_reset();
        pci_irq64_test_arm(k);
        int rc = pci64_irq_enable(s);
        int steps = pci_irq64_test_steps();
        pci_irq64_test_arm(0);
        CHECK(tbl_order_violations == 0);                           // forward path AND rollback only touch masked entries
        if (rc == 0) { total = steps; CHECK(pci64_irq_state(s) == PCI_IRQSET_LIVE); break; }
        if (pci64_irq_state(s) == PCI_IRQSET_BOUND) {
            bound_seen++;
            CHECK(snap_same(0, &sn));                               // config space AND table exactly as before
            CHECK(!g_pt.desc[irq_of(s, 0)].sealed && g_pt.desc[irq_of(s, 0)].masked == (g_pt.desc[irq_of(s, 0)].can_mask ? 1 : 0));
            if (msix) for (int i = 0; i < pci64_irq_count(s); i++) CHECK(g_pt.desc[irq_of(s, i)].masked == 1);
            CHECK(pci64_irq_free(s) == 0);                          // pre-live failure: free is legal...
            CHECK(all_msg_vectors_free() && fdev[0].irq_state == 0);   // ...and everything is released
        } else {
            sealed_seen++;
            CHECK(pci64_irq_state(s) == PCI_IRQSET_QUIESCED);       // gate succeeded (cooperative fake)
            CHECK(g_pt.desc[irq_of(s, 0)].sealed && pci64_irq_free(s) == PCI64_IRQ_ERR_SEALED);
            CHECK(g_pt.desc[irq_of(s, 0)].handler == hA && g_pt.vec_to_irq[vec_of(s, 0)] == irq_of(s, 0));
            if (msix) CHECK(f_ctrl_msix(0) & 0x4000);
            else if (pvm) CHECK(f_dw(0, is64 ? MSI_OFF + 16 : MSI_OFF + 12) & 1);
            else CHECK(!(f_ctrl_msi(0) & 1));
        }
    }
    CHECK(total > 5 && total < 60);
    CHECK(sealed_seen == 1);                                        // exactly the post-boundary checkpoint
    CHECK(bound_seen == total - 1);
}

static void test_failure_injection(void) {
    group_begin();
    inject_all_steps("msi plain 64",   0, 1, 0, 0);
    inject_all_steps("msi plain 32",   0, 0, 0, 0);
    inject_all_steps("msi pvm 64",     0, 1, 1, 0);
    inject_all_steps("msi pvm 32",     0, 0, 1, 0);
    inject_all_steps("msix",           1, 0, 0, 0);
    inject_all_steps("msix maskall",   1, 0, 0, 1);                 // MASKALL already set (Enable clear) is restored
    group_end("failure injected at EVERY forward step (MSI x4 layouts, MSI-X x2): pre-boundary == exact snapshot restore + free; boundary+ == sealed");
}

static int flt_drop_msi_readback(int d, uint8_t off, int width, uint32_t val) { (void)d; (void)width; (void)val; return off != MSI_OFF + 4; }   // address-low dead
static int flt_tbl_dead_entry0(int d, uint32_t off, uint32_t val) { (void)d; (void)val; return off != 0; }                                     // entry-0 address write dropped
static int flt_intx_stuck(int d, uint8_t off, int width, uint32_t val) { (void)d; (void)width; (void)val; return off != 4; }                     // Command writes ignored (no INTx Disable support)
static void test_readback_failures(void) {
    group_begin();
    snap_t sn;
    // Dead message-address register: readback disagrees -> rollback.
    pt_reset(); counters_reset(); fdev_init(0, 1, 1, 0, 0, 0, 0); snap_take(0, &sn);
    pci64_irq_set_t* s = mk_bound(0, PCI64_IRQ_MSI, 1, 1, hA, &ctxA);
    f_cfg_filter = flt_drop_msi_readback;
    CHECK(pci64_irq_enable(s) == PCI64_IRQ_ERR_IO && pci64_irq_state(s) == PCI_IRQSET_BOUND);
    f_cfg_filter = 0; CHECK(snap_same(0, &sn) && !(f_ctrl_msi(0) & 1));
    CHECK(pci64_irq_free(s) == 0 && all_msg_vectors_free());
    // MSI-X: a table write that does not stick.
    pt_reset(); fdev_init(0, 0, 0, 0, 0, 1, 4); snap_take(0, &sn);
    s = mk_bound(0, PCI64_IRQ_MSIX, 1, 2, hA, &ctxA);
    f_tbl_filter = flt_tbl_dead_entry0;
    CHECK(pci64_irq_enable(s) == PCI64_IRQ_ERR_IO && pci64_irq_state(s) == PCI_IRQSET_BOUND);
    f_tbl_filter = 0; CHECK(snap_same(0, &sn) && !(f_ctrl_msix(0) & 0x8000));
    CHECK(pci64_irq_free(s) == 0 && all_msg_vectors_free());
    // A device with no INTx-Disable bit: tolerated; on a later failure Command is left exactly as it was.
    pt_reset(); fdev_init(0, 1, 1, 0, 0, 0, 0); snap_take(0, &sn);
    s = mk_bound(0, PCI64_IRQ_MSI, 1, 1, hA, &ctxA);
    f_cfg_filter = flt_intx_stuck;
    CHECK(pci64_irq_enable(s) == 0 && !(f_cmd(0) & 0x0400));       // succeeded even though bit 10 never stuck
    // BME clear: refused before ANY write.
    pt_reset(); fdev_init(0, 1, 1, 0, 0, 0, 0); fcs[0][4] = 0x03; snap_take(0, &sn);
    s = mk_bound(0, PCI64_IRQ_MSI, 1, 1, hA, &ctxA); ev_reset();
    CHECK(pci64_irq_enable(s) == PCI64_IRQ_ERR_NO_BUS_MASTER && cfg_writes() == 0 && pci64_irq_state(s) == PCI_IRQSET_BOUND && snap_same(0, &sn));
    pt_reset(); fdev_init(0, 0, 0, 0, 0, 1, 4); fcs[0][4] = 0x03;
    s = mk_bound(0, PCI64_IRQ_MSIX, 1, 2, hA, &ctxA); ev_reset();
    CHECK(pci64_irq_enable(s) == PCI64_IRQ_ERR_NO_BUS_MASTER && nev == 0);
    // Unroutable destination: refused before any write.
    pt_reset(); fdev_init(0, 1, 1, 0, 0, 0, 0); g_pt.dest_apic = 0xFF;
    s = mk_bound(0, PCI64_IRQ_MSI, 1, 1, hA, &ctxA); ev_reset();
    CHECK(pci64_irq_enable(s) == PCI64_IRQ_ERR_COMPOSE && nev == 0 && pci64_irq_state(s) == PCI_IRQSET_BOUND);
    g_pt.dest_apic = 0x100; CHECK(pci64_irq_enable(s) == PCI64_IRQ_ERR_COMPOSE && nev == 0);
    group_end("readback failures (dead address register, unsticky table write, no-INTx-bit device), no-BME, bad destination: safe rollback / clean refusal");
}

// ── FAULTED state ──────────────────────────────────────────────────────
static int g_boundary_crossed;
static int flt_msi_stuck_enable(int d, uint8_t off, int width, uint32_t val) {   // once live, MSI Enable can never be cleared
    (void)d; (void)width;
    if (off == MSI_OFF + 2 && (val & 1)) g_boundary_crossed = 1;
    if (off == MSI_OFF + 2 && g_boundary_crossed && !(val & 1)) return 0;
    return 1;
}
static int flt_msix_stuck_maskall(int d, uint8_t off, int width, uint32_t val) {  // once MASKALL is cleared it can never be set again
    (void)d; (void)width;
    if (off == MSIX_OFF + 2 && !(val & 0x4000)) g_boundary_crossed = 1;
    if (off == MSIX_OFF + 2 && g_boundary_crossed && (val & 0x4000)) return 0;
    return 1;
}
static int flt_msi_pvm_stuck(int d, uint8_t off, int width, uint32_t val) {
    (void)d; (void)width;
    if (off == MSI_OFF + 2 && (val & 1)) g_boundary_crossed = 1;
    if (off == MSI_OFF + 16 && g_boundary_crossed && (val & 1)) return 0;
    return 1;
}

static void faulted_common(pci64_irq_set_t* s, int dev, int other_dev) {
    int irq = irq_of(s, 0), vec = vec_of(s, 0);
    uint32_t diag0 = pci_irq64_diag_count();
    CHECK(pci64_irq_state(s) == PCI_IRQSET_FAULTED);
    // Everything stays pinned.
    CHECK(g_pt.desc[irq].allocated && g_pt.desc[irq].sealed && g_pt.desc[irq].handler == hA && g_pt.desc[irq].ctx == &ctxA);
    CHECK(g_pt.vec_to_irq[vec] == irq && irq_vector_state(&g_pt.vmap, vec) == VEC_ALLOCATED && irq64_table_msg_vector(&g_pt, irq) == vec);
    CHECK(pci64_irq_free(s) == PCI64_IRQ_ERR_SEALED && pci64_irq_enable(s) == PCI64_IRQ_ERR_FAULTED);
    CHECK(pci64_irq_request(s, 0, hB, &ctxB, "B") == PCI64_IRQ_ERR_BADSTATE);
    CHECK(irq64_table_msg_bind(&g_pt, irq, hB, &ctxB, "B") == IRQ64_E_BUSY);
    CHECK(pci64_irq_quiesce(s) == PCI64_IRQ_ERR_FAULTED);           // no retry pretending the device is clean
    CHECK(pci64_irq_state(s) == PCI_IRQSET_FAULTED);
    CHECK(pci_irq64_diag_count() == diag0);                        // repeated attempts do not spam the diagnostic
    CHECK(fdev[dev].irq_state == s);
    // A NEW owner gets a DIFFERENT vector; a stale message on the old one reaches only the old owner.
    pci64_irq_set_t* c = mk_bound(other_dev, PCI64_IRQ_MSI, 1, 1, hC, &ctxC);
    CHECK(c != 0);
    if (c) {
        CHECK(vec_of(c, 0) != vec && irq_of(c, 0) != irq);
        int a0 = hA_calls, c0 = hC_calls;
        irq64_table_dispatch(&g_pt, (uint8_t)vec);
        CHECK(hA_calls == a0 + 1 && hC_calls == c0);
        CHECK(g_pt.desc[irq].handler == hA && g_pt.desc[irq].ctx == &ctxA);
    }
}

static int steps_msi_plain(void) {
    pt_reset(); counters_reset(); fdev_init(0, 1, 1, 0, 0, 0, 0);
    pci64_irq_set_t* p = mk_bound(0, PCI64_IRQ_MSI, 1, 1, hA, &ctxA);
    pci_irq64_test_arm(1000); pci64_irq_enable(p);
    int n = pci_irq64_test_steps(); pci_irq64_test_arm(0);
    return n;
}

static void test_faulted(void) {
    group_begin();
    // MSI (plain): Enable cannot be cleared after go-live.
    pt_reset(); counters_reset(); g_boundary_crossed = 0;
    fdev_init(0, 1, 1, 0, 0, 0, 0); fdev_init(1, 1, 1, 0, 0, 0, 0);
    pci64_irq_set_t* s = mk_bound(0, PCI64_IRQ_MSI, 1, 1, hA, &ctxA);
    f_cfg_filter = flt_msi_stuck_enable;
    uint32_t dg = pci_irq64_diag_count();
    CHECK(pci64_irq_enable(s) == 0);
    CHECK(pci64_irq_quiesce(s) == PCI64_IRQ_ERR_IO);
    CHECK(pci_irq64_diag_count() == dg + 1);                        // exactly ONE loud diagnostic for the fault
    f_cfg_filter = 0;
    faulted_common(s, 0, 1);

    // MSI-X: MASKALL cannot be set after go-live.
    pt_reset(); counters_reset(); g_boundary_crossed = 0;
    fdev_init(0, 0, 0, 0, 0, 1, 4); fdev_init(1, 1, 1, 0, 0, 0, 0);
    s = mk_bound(0, PCI64_IRQ_MSIX, 1, 1, hA, &ctxA);
    f_cfg_filter = flt_msix_stuck_maskall;
    CHECK(pci64_irq_enable(s) == 0);
    CHECK(pci64_irq_quiesce(s) == PCI64_IRQ_ERR_IO);
    f_cfg_filter = 0;
    faulted_common(s, 0, 1);

    // MSI with a per-vector mask that will not set.
    pt_reset(); counters_reset(); g_boundary_crossed = 0;
    fdev_init(0, 1, 1, 1, 0, 0, 0); fdev_init(1, 1, 1, 0, 0, 0, 0);
    s = mk_bound(0, PCI64_IRQ_MSI, 1, 1, hA, &ctxA);
    f_cfg_filter = flt_msi_pvm_stuck;
    CHECK(pci64_irq_enable(s) == 0 && pci64_irq_quiesce(s) == PCI64_IRQ_ERR_IO);
    f_cfg_filter = 0;
    faulted_common(s, 0, 1);

    // FAULTED reached straight from enable(): failure AFTER the boundary and the re-gate ALSO fails.
    int steps_probe = steps_msi_plain();
    pt_reset(); counters_reset(); g_boundary_crossed = 0;
    fdev_init(0, 1, 1, 0, 0, 0, 0); fdev_init(1, 1, 1, 0, 0, 0, 0);
    s = mk_bound(0, PCI64_IRQ_MSI, 1, 1, hA, &ctxA);
    f_cfg_filter = flt_msi_stuck_enable;
    pci_irq64_test_arm(steps_probe);                                // fail the post-boundary verification
    CHECK(pci64_irq_enable(s) == PCI64_IRQ_ERR_IO);
    pci_irq64_test_arm(0); f_cfg_filter = 0;
    faulted_common(s, 0, 1);
    group_end("FAULTED state: gate failure after LIVE (MSI, MSI-X, MSI+mask, and from enable() itself) pins vector/desc/handler/ctx, blocks free/enable/quiesce/rebind, stale IRQ reaches only the original owner");
}

// ── QUIESCED stale interrupt vs new owner ──────────────────────────────
static void test_stale_after_quiesce(void) {
    group_begin();
    pt_reset(); counters_reset();
    fdev_init(0, 1, 1, 0, 0, 0, 0); fdev_init(1, 1, 1, 0, 0, 0, 0); fdev_init(2, 1, 1, 0, 0, 0, 0);
    pci64_irq_set_t* a = mk_bound(0, PCI64_IRQ_MSI, 1, 1, hA, &ctxA);
    pci64_irq_set_t* b = mk_bound(1, PCI64_IRQ_MSI, 1, 1, hB, &ctxB);   // allocated + bound, never enabled
    CHECK(a && b && pci64_irq_enable(a) == 0 && pci64_irq_quiesce(a) == 0 && pci64_irq_state(a) == PCI_IRQSET_QUIESCED);
    int vec_a = vec_of(a, 0);
    pci64_irq_set_t* c = mk_bound(2, PCI64_IRQ_MSI, 1, 1, hC, &ctxC);
    CHECK(c && vec_of(c, 0) != vec_a && vec_of(b, 0) != vec_a && vec_of(c, 0) != vec_of(b, 0));
    irq64_table_dispatch(&g_pt, (uint8_t)vec_a);
    CHECK(hA_calls == 1 && hB_calls == 0 && hC_calls == 0);
    CHECK(pci64_irq_free(a) == PCI64_IRQ_ERR_SEALED);
    CHECK(pci64_irq_free(b) == 0 && pci64_irq_free(c) == 0);          // never-live sets DO free
    // A freed vector may be reused -- a SEALED one never is.
    pci64_irq_set_t* d2 = mk_bound(1, PCI64_IRQ_MSI, 1, 1, hB, &ctxB);
    CHECK(d2 && vec_of(d2, 0) != vec_a);
    group_end("stale interrupt after QUIESCED dispatches only to the pinned original owner; sealed vectors never reused, unsealed ones are");
}

// ── allocation policy: already-active / owned / malformed / PIC / limits ──
static void test_alloc_policy(void) {
    group_begin();
    snap_t sn; pci64_irq_set_t* s;
    // MSI already enabled -> ALREADY_ACTIVE for either request; nothing changes.
    pt_reset(); fdev_init(0, 1, 1, 0, 0, 1, 4); fcs[0][MSI_OFF + 2] |= 1; snap_take(0, &sn);
    CHECK(pci64_irq_alloc(&fdev[0], 1, 1, PCI64_IRQ_ANY, &s) == PCI64_IRQ_ERR_ALREADY_ACTIVE);
    CHECK(pci64_irq_alloc(&fdev[0], 1, 1, PCI64_IRQ_MSIX, &s) == PCI64_IRQ_ERR_ALREADY_ACTIVE);   // MSI-X request, MSI active
    CHECK(pci64_irq_alloc(&fdev[0], 1, 1, PCI64_IRQ_MSI, &s) == PCI64_IRQ_ERR_ALREADY_ACTIVE);
    CHECK(snap_same(0, &sn) && all_msg_vectors_free() && fdev[0].irq_state == 0);
    // MSI-X already enabled.
    pt_reset(); fdev_init(0, 1, 1, 0, 0, 1, 4); fcs[0][MSIX_OFF + 3] |= 0x80; snap_take(0, &sn);
    CHECK(pci64_irq_alloc(&fdev[0], 1, 1, PCI64_IRQ_MSI, &s) == PCI64_IRQ_ERR_ALREADY_ACTIVE);    // MSI request, MSI-X active
    CHECK(pci64_irq_alloc(&fdev[0], 1, 4, PCI64_IRQ_ANY, &s) == PCI64_IRQ_ERR_ALREADY_ACTIVE);
    CHECK(snap_same(0, &sn) && all_msg_vectors_free());
    // MASKALL set but Enable clear is NOT active.
    pt_reset(); fdev_init(0, 0, 0, 0, 0, 1, 4); fcs[0][MSIX_OFF + 3] = 0x40;
    CHECK(pci64_irq_alloc(&fdev[0], 1, 2, PCI64_IRQ_ANY, &s) == 2 && s);
    // Second allocation on an owned device.
    pci64_irq_set_t* s2;
    CHECK(pci64_irq_alloc(&fdev[0], 1, 1, PCI64_IRQ_ANY, &s2) == PCI64_IRQ_ERR_ALREADY_OWNED);
    // Forced PIC.
    pt_reset(); fdev_init(0, 1, 1, 0, 0, 1, 4); g_pt.apic_mode = 0;
    CHECK(pci64_irq_alloc(&fdev[0], 1, 1, PCI64_IRQ_ANY, &s) == PCI64_IRQ_ERR_NODEV && all_msg_vectors_free() && fdev[0].irq_state == 0);
    g_pt.apic_mode = 1;
    // No capabilities at all.
    pt_reset(); fdev_init(0, 0, 0, 0, 0, 0, 0); CHECK(pci64_irq_alloc(&fdev[0], 1, 1, PCI64_IRQ_ANY, &s) == PCI64_IRQ_ERR_NODEV);
    // Malformed list -> MALFORMED (both capabilities untrusted).
    pt_reset(); fdev_init(0, 1, 1, 0, 0, 1, 4); fcs[0][0x34] = 0x42;
    CHECK(pci64_irq_alloc(&fdev[0], 1, 1, PCI64_IRQ_ANY, &s) == PCI64_IRQ_ERR_MALFORMED);
    // Reserved MMC / bad table BIR -> that capability unusable.
    pt_reset(); fdev_init(0, 1, 1, 0, 7, 0, 0); CHECK(pci64_irq_alloc(&fdev[0], 1, 1, PCI64_IRQ_ANY, &s) == PCI64_IRQ_ERR_MALFORMED);
    pt_reset(); fdev_init(0, 0, 0, 0, 0, 1, 4); fcs[0][MSIX_OFF + 4] = 0x06; CHECK(pci64_irq_alloc(&fdev[0], 1, 1, PCI64_IRQ_ANY, &s) == PCI64_IRQ_ERR_MALFORMED);
    pt_reset(); fdev_init(0, 0, 0, 0, 0, 1, 4); fdev[0].bar[1].size = 0x20;                    // table does not fit its BAR
    CHECK(pci64_irq_alloc(&fdev[0], 1, 1, PCI64_IRQ_ANY, &s) == PCI64_IRQ_ERR_MALFORMED);
    // MSI-X falls back to MSI when the table is bad and MSI is fine.
    pt_reset(); fdev_init(0, 1, 1, 0, 0, 1, 4); fdev[0].bar[1].type = PCI64_BAR_IO;
    CHECK(pci64_irq_alloc(&fdev[0], 1, 4, PCI64_IRQ_ANY, &s) == 1 && pci64_irq_type(s) == PCI64_IRQ_TYPE_MSI);
    // MSI capped at ONE vector, even for an MMC=5 (32-vector) device.
    pt_reset(); fdev_init(0, 1, 1, 0, 5, 0, 0);
    CHECK(pci64_irq_alloc(&fdev[0], 2, 4, PCI64_IRQ_MSI, &s) == PCI64_IRQ_ERR_NOSPC);
    CHECK(pci64_irq_alloc(&fdev[0], 1, 32, PCI64_IRQ_MSI, &s) == 1);
    // MSI-X: count = min(max, table size).
    pt_reset(); fdev_init(0, 0, 0, 0, 0, 1, 3);
    CHECK(pci64_irq_alloc(&fdev[0], 1, 16, PCI64_IRQ_MSIX, &s) == 3);
    pt_reset(); fdev_init(0, 0, 0, 0, 0, 1, 3);
    CHECK(pci64_irq_alloc(&fdev[0], 4, 8, PCI64_IRQ_MSIX, &s) == PCI64_IRQ_ERR_NOSPC);
    // Vector exhaustion mid-allocation rolls back completely.
    pt_reset(); fdev_init(0, 0, 0, 0, 0, 1, 40);
    for (int i = 0; i < 190 - 2; i++) irq_vector_alloc(&g_pt.vmap, 200);                       // leave 2 free
    CHECK(pci64_irq_alloc(&fdev[0], 3, 40, PCI64_IRQ_MSIX, &s) == PCI64_IRQ_ERR_NOSPC);
    { int leaked = 0; for (int i = 16; i < IRQ64_MAX; i++) if (g_pt.desc[i].allocated) leaked++; CHECK(leaked == 0); CHECK(irq_vector_free_count(&g_pt.vmap) == 2); }
    CHECK(pci64_irq_alloc(&fdev[0], 1, 40, PCI64_IRQ_MSIX, &s) == 2);                          // shrinks to what is available
    // All allocated vectors are distinct, dynamic, never 0x80.
    pt_reset(); fdev_init(0, 0, 0, 0, 0, 1, 32);
    CHECK(pci64_irq_alloc(&fdev[0], 32, 32, PCI64_IRQ_MSIX, &s) == 32);
    int distinct = 1, good = 1;
    for (int i = 0; i < 32; i++) { int v = vec_of(s, i); if (v == 0x80 || !vector64_is_dynamic_device_vector(v)) good = 0;
        for (int j = i + 1; j < 32; j++) if (vec_of(s, j) == v) distinct = 0; }
    CHECK(distinct && good);
    group_end("allocation policy: ALREADY_ACTIVE/OWNED, forced-PIC -ENODEV, malformed metadata, MSI capped at 1, MSI-X sizing, exhaustion rollback, vector uniqueness");
}

// ═════════════ M+11C: ARMED state, precise boundary, no delivery while ARMED ═══════
// Spec model of "would this entry's message reach the CPU right now?".
static int would_send_msix(int d, int entry) {
    uint16_t c = f_ctrl_msix(d);
    return (c & 0x8000) && !(c & 0x4000) && !(ftab[d][entry * 4 + 3] & 1u);
}
static int would_send_msi(int d) {
    return (f_ctrl_msi(d) & 1) != 0;   // (per-vector mask checked separately by the pvm tests)
}
static void fake_vio_write(int d, uint16_t off, uint16_t val) {   // stands in for a VirtIO selector write
    ev_add(2, d, off, 2, val);
}

static void test_armed_lifecycle(void) {
    group_begin();
    pt_reset(); counters_reset();
    fdev_init(0, 0, 0, 0, 0, 1, 6);
    for (int e = 0; e < 6; e++) { ftab[0][e * 4] = 0xDEAD0000u + (uint32_t)e; ftab[0][e * 4 + 1] = 0x11; ftab[0][e * 4 + 2] = 0x22 + (uint32_t)e; ftab[0][e * 4 + 3] = (e % 2) ? 1u : 0x00030000u; }
    snap_t orig; snap_take(0, &orig);
    pci64_irq_set_t* s = mk_bound(0, PCI64_IRQ_MSIX, 2, 3, hA, &ctxA);
    CHECK(s && pci64_irq_state(s) == PCI_IRQSET_BOUND && cfg_writes() == 0);
    CHECK(pci64_irq_activate(s) == PCI64_IRQ_ERR_BADSTATE && pci64_irq_disarm(s) == PCI64_IRQ_ERR_BADSTATE);   // not ARMED yet
    int irq0 = irq_of(s, 0);

    // BOUND -> ARMED
    ev_reset();
    CHECK(pci64_irq_prepare(s) == 0 && pci64_irq_state(s) == PCI_IRQSET_ARMED);
    CHECK((f_ctrl_msix(0) & 0xC000) == 0xC000);                     // MSI-X Enable AND function mask
    CHECK(!(f_cmd(0) & 0x0400));                                    // INTx not touched yet
    CHECK(!g_pt.desc[irq0].sealed);                                 // ARMED is NOT sealed
    for (int e = 0; e < 6; e++) CHECK(ftab[0][e * 4 + 3] & 1u);     // every entry masked
    for (int e = 0; e < 3; e++) CHECK(!would_send_msix(0, e));      // no message can reach the CPU while ARMED
    CHECK(tbl_order_violations == 0);
    CHECK(pci64_irq_prepare(s) == PCI64_IRQ_ERR_BADSTATE);          // prepare only from BOUND
    CHECK(pci64_irq_enable(s) == PCI64_IRQ_ERR_BADSTATE);           // enable is BOUND-only (no re-arm/re-enable)
    CHECK(pci64_irq_quiesce(s) == PCI64_IRQ_ERR_BADSTATE);
    CHECK(pci64_irq_request(s, 0, hB, &ctxB, "B") == PCI64_IRQ_ERR_BADSTATE);

    // Device-side selector programming happens NOW; MASKALL must hold throughout.
    ev_reset();
    fake_vio_write(0, 0x100, 0); fake_vio_write(0, 0x102, 1);       // e.g. queue_msix_vector for two queues
    int selectors_under_mask = 1;
    for (int i = 0; i < nev; i++) if (ev[i].kind == 2 && !(ev[i].maskall && (fcs[0][MSIX_OFF + 3] & 0x80))) selectors_under_mask = 0;
    CHECK(nev == 2 && selectors_under_mask);
    for (int e = 0; e < 3; e++) CHECK(!would_send_msix(0, e));

    // ARMED -> BOUND -> (exact restore) ; free from ARMED
    CHECK(pci64_irq_disarm(s) == 0 && pci64_irq_state(s) == PCI_IRQSET_BOUND && snap_same(0, &orig));
    CHECK(pci64_irq_prepare(s) == 0);
    CHECK(pci64_irq_free(s) == 0 && snap_same(0, &orig) && all_msg_vectors_free() && fdev[0].irq_state == 0);   // free from ARMED restores exactly

    // BOUND -> ARMED -> LIVE
    pt_reset(); counters_reset(); fdev_init(0, 0, 0, 0, 0, 1, 6);
    s = mk_bound(0, PCI64_IRQ_MSIX, 2, 3, hA, &ctxA);
    irq0 = irq_of(s, 0);
    CHECK(pci64_irq_prepare(s) == 0);
    ev_reset();
    CHECK(pci64_irq_activate(s) == 0 && pci64_irq_state(s) == PCI_IRQSET_LIVE);
    CHECK(g_pt.desc[irq0].sealed && (f_cmd(0) & 0x0400));
    for (int e = 0; e < 3; e++) CHECK(would_send_msix(0, e));       // NOW a message can escape
    CHECK(ev[nev - 1].kind == 0 && ev[nev - 1].off == MSIX_OFF + 2 && !(ev[nev - 1].val & 0x4000));   // MASKALL clear is the LAST write
    CHECK(pci64_irq_free(s) == PCI64_IRQ_ERR_SEALED && pci64_irq_disarm(s) == PCI64_IRQ_ERR_BADSTATE && pci64_irq_activate(s) == PCI64_IRQ_ERR_BADSTATE);

    // Plain MSI: ARMED programs address/data with Enable still 0, activate sets Enable last.
    pt_reset(); counters_reset(); fdev_init(0, 1, 1, 0, 0, 0, 0);
    snap_take(0, &orig);
    s = mk_bound(0, PCI64_IRQ_MSI, 1, 1, hA, &ctxA);
    CHECK(pci64_irq_prepare(s) == 0 && pci64_irq_state(s) == PCI_IRQSET_ARMED && !would_send_msi(0) && !(f_cmd(0) & 0x400));
    CHECK(f_dw(0, MSI_OFF + 4) == 0xFEE03000u);                     // message already programmed
    CHECK(pci64_irq_disarm(s) == 0 && snap_same(0, &orig));
    CHECK(pci64_irq_enable(s) == 0 && pci64_irq_state(s) == PCI_IRQSET_LIVE && would_send_msi(0));
    group_end("ARMED state: BOUND->ARMED->LIVE, MASKALL held during selector programming, no delivery while ARMED, disarm/free restore exactly");
}

// Failure injected at every step of PREPARE alone, and of ACTIVATE alone.
static void inject_prepare_activate(int msix, int is64, int pvm) {
    int total_p = 0, total_a = 0;
    // prepare-only: any failure -> BOUND with the exact original device state
    for (int k = 1; k < 40; k++) {
        pt_reset(); counters_reset();
        if (msix) { fdev_init(0, 0, 0, 0, 0, 1, 5); for (int e = 0; e < 5; e++) { ftab[0][e * 4] = 0xBAD00000u + (uint32_t)e; ftab[0][e * 4 + 3] = (e % 2) ? 1u : 0u; } }
        else fdev_init(0, 1, is64, pvm, 0, 0, 0);
        snap_t sn; snap_take(0, &sn);
        pci64_irq_set_t* s = mk_bound(0, msix ? PCI64_IRQ_MSIX : PCI64_IRQ_MSI, 1, msix ? 3 : 1, hA, &ctxA);
        pci_irq64_test_arm(k);
        int rc = pci64_irq_prepare(s);
        int steps = pci_irq64_test_steps();
        pci_irq64_test_arm(0);
        if (rc == 0) { total_p = steps; CHECK(pci64_irq_state(s) == PCI_IRQSET_ARMED); break; }
        CHECK(pci64_irq_state(s) == PCI_IRQSET_BOUND && snap_same(0, &sn) && !g_pt.desc[irq_of(s, 0)].sealed);
        CHECK(tbl_order_violations == 0);
        CHECK(pci64_irq_free(s) == 0 && all_msg_vectors_free());
    }
    CHECK(total_p >= 3);
    // activate-only: pre-boundary failure -> ARMED, UNSEALED, device == its post-prepare state; then disarm restores the original
    int sealed_seen = 0, armed_seen = 0;
    for (int k = 1; k < 40; k++) {
        pt_reset(); counters_reset();
        if (msix) { fdev_init(0, 0, 0, 0, 0, 1, 5); for (int e = 0; e < 5; e++) { ftab[0][e * 4] = 0xBAD00000u + (uint32_t)e; ftab[0][e * 4 + 3] = (e % 2) ? 1u : 0u; } }
        else fdev_init(0, 1, is64, pvm, 0, 0, 0);
        snap_t orig; snap_take(0, &orig);
        pci64_irq_set_t* s = mk_bound(0, msix ? PCI64_IRQ_MSIX : PCI64_IRQ_MSI, 1, msix ? 3 : 1, hA, &ctxA);
        CHECK(pci64_irq_prepare(s) == 0);
        snap_t armed; snap_take(0, &armed);
        pci_irq64_test_arm(k);
        int rc = pci64_irq_activate(s);
        int steps = pci_irq64_test_steps();
        pci_irq64_test_arm(0);
        if (rc == 0) { total_a = steps; CHECK(pci64_irq_state(s) == PCI_IRQSET_LIVE && g_pt.desc[irq_of(s, 0)].sealed); break; }
        if (pci64_irq_state(s) == PCI_IRQSET_ARMED) {
            armed_seen++;
            CHECK(!g_pt.desc[irq_of(s, 0)].sealed);                    // nothing could have escaped: unsealed
            CHECK(snap_same(0, &armed));                                // activate undid exactly what it did
            if (msix) for (int e = 0; e < pci64_irq_count(s); e++) CHECK(!would_send_msix(0, e));
            CHECK(pci64_irq_disarm(s) == 0 && snap_same(0, &orig));    // full rollback still available
            CHECK(pci64_irq_free(s) == 0 && all_msg_vectors_free());
        } else {
            sealed_seen++;                                              // the boundary was attempted and could not be verified
            CHECK(g_pt.desc[irq_of(s, 0)].sealed);
            CHECK(pci64_irq_state(s) == PCI_IRQSET_QUIESCED);           // verified re-gate (cooperative fake)
            CHECK(pci64_irq_free(s) == PCI64_IRQ_ERR_SEALED && all_msg_vectors_free() == 0);
        }
    }
    CHECK(total_a >= 3 && sealed_seen == 1 && armed_seen == total_a - 1);
}

// Boundary observers/filters (see test_boundary_cases).
static pci64_irq_set_t* g_obs_set; static int g_obs_msix;
static int g_obs_writes, g_obs_sealed_at_write, g_obs_state_at_write, g_obs_reads, g_obs_state_at_read;
static int is_golive_write(uint8_t off, uint32_t val) {
    return g_obs_msix ? (off == MSIX_OFF + 2 && (val & 0x8000) && !(val & 0x4000))     // Enable set, MASKALL clear
                      : (off == MSI_OFF + 2 && (val & 1));                             // Enable set
}
static void obs_boundary_write(int d, uint8_t off, int width, uint32_t val) {
    (void)d; (void)width;
    if (!is_golive_write(off, val)) return;
    g_obs_writes++;
    g_obs_sealed_at_write = g_pt.desc[irq_of(g_obs_set, 0)].sealed;
    g_obs_state_at_write = pci64_irq_state(g_obs_set);
}
static void obs_boundary_read(int d, uint8_t off, int width) {
    (void)d; (void)width;
    if (g_obs_writes && !g_obs_reads && off == (g_obs_msix ? MSIX_OFF + 2 : MSI_OFF + 2)) {
        g_obs_reads = 1; g_obs_state_at_read = pci64_irq_state(g_obs_set);
    }
}
static int flt_drop_golive_msix(int d, uint8_t off, int width, uint32_t val) { (void)d; (void)width; return !(off == MSIX_OFF + 2 && !(val & 0x4000)); }   // MASKALL can never be cleared
static int flt_drop_golive_msi(int d, uint8_t off, int width, uint32_t val)  { (void)d; (void)width; return !(off == MSI_OFF + 2 && (val & 1)); }        // Enable can never be set

static void test_boundary_cases(void) {
    group_begin();
    inject_prepare_activate(1, 0, 0);
    inject_prepare_activate(0, 1, 0);
    inject_prepare_activate(0, 0, 1);
    // (1) failure before the boundary -> ARMED/unsealed; (2) success -> LIVE/sealed  [covered above per step];
    // (3) uncertain boundary with a re-gate that ALSO fails -> FAULTED, sealed, one diagnostic.
    pt_reset(); counters_reset(); g_boundary_crossed = 0;
    fdev_init(0, 0, 0, 0, 0, 1, 4);
    pci64_irq_set_t* s = mk_bound(0, PCI64_IRQ_MSIX, 1, 2, hA, &ctxA);
    CHECK(pci64_irq_prepare(s) == 0);
    pci_irq64_test_arm(1000);
    { int probe_rc = pci64_irq_activate(s); (void)probe_rc; }         // count activate's steps on a scratch run
    int asteps = pci_irq64_test_steps(); pci_irq64_test_arm(0);
    pt_reset(); counters_reset(); g_boundary_crossed = 0;
    fdev_init(0, 0, 0, 0, 0, 1, 4);
    s = mk_bound(0, PCI64_IRQ_MSIX, 1, 2, hA, &ctxA);
    CHECK(pci64_irq_prepare(s) == 0);
    f_cfg_filter = flt_msix_stuck_maskall;
    uint32_t dg = pci_irq64_diag_count();
    pci_irq64_test_arm(asteps);                                        // fail the boundary verification; the re-gate is stuck
    CHECK(pci64_irq_activate(s) == PCI64_IRQ_ERR_IO);
    pci_irq64_test_arm(0); f_cfg_filter = 0;
    CHECK(pci64_irq_state(s) == PCI_IRQSET_FAULTED && g_pt.desc[irq_of(s, 0)].sealed && pci_irq64_diag_count() == dg + 1);
    CHECK(pci64_irq_free(s) == PCI64_IRQ_ERR_SEALED && pci64_irq_disarm(s) == PCI64_IRQ_ERR_BADSTATE);
    CHECK(g_pt.vec_to_irq[vec_of(s, 0)] == irq_of(s, 0) && g_pt.desc[irq_of(s, 0)].handler == hA);

    // (4) ORDER at the boundary, observed at the exact instants: when the go-live write is issued every
    //     descriptor is ALREADY sealed while the set is still ARMED; at the very next config read (the
    //     verification readback) the set is STILL ARMED -- LIVE is claimed only after the readback verified.
    for (int msix = 1; msix >= 0; msix--) {
        pt_reset(); counters_reset(); g_boundary_crossed = 0;
        if (msix) fdev_init(0, 0, 0, 0, 0, 1, 4); else fdev_init(0, 1, 1, 0, 0, 0, 0);
        s = mk_bound(0, msix ? PCI64_IRQ_MSIX : PCI64_IRQ_MSI, 1, msix ? 2 : 1, hA, &ctxA);
        CHECK(pci64_irq_prepare(s) == 0);
        g_obs_set = s; g_obs_msix = msix; g_obs_writes = g_obs_sealed_at_write = g_obs_reads = 0;
        g_obs_state_at_write = g_obs_state_at_read = -1;
        f_cfg_wr_obs = obs_boundary_write; f_cfg_rd_obs = obs_boundary_read;
        CHECK(pci64_irq_activate(s) == 0);
        f_cfg_wr_obs = 0; f_cfg_rd_obs = 0;
        CHECK(g_obs_writes == 1);                                              // exactly one go-live write
        CHECK(g_obs_sealed_at_write == 1 && g_obs_state_at_write == PCI_IRQSET_ARMED);   // sealed BEFORE the write, state not yet LIVE
        CHECK(g_obs_reads >= 1 && g_obs_state_at_read == PCI_IRQSET_ARMED);    // readback happens while still ARMED
        CHECK(pci64_irq_state(s) == PCI_IRQSET_LIVE);
    }

    // (5) a device that IGNORES the go-live write: the readback proves it, so the set must NOT be LIVE --
    //     it is sealed (the write was attempted), gated (function masked again) and QUIESCED.
    for (int msix = 1; msix >= 0; msix--) {
        pt_reset(); counters_reset(); g_boundary_crossed = 0;
        if (msix) fdev_init(0, 0, 0, 0, 0, 1, 4); else fdev_init(0, 1, 1, 0, 0, 0, 0);
        s = mk_bound(0, msix ? PCI64_IRQ_MSIX : PCI64_IRQ_MSI, 1, msix ? 2 : 1, hA, &ctxA);
        CHECK(pci64_irq_prepare(s) == 0);
        f_cfg_filter = msix ? flt_drop_golive_msix : flt_drop_golive_msi;
        int rc = pci64_irq_activate(s);
        f_cfg_filter = 0;
        CHECK(rc == PCI64_IRQ_ERR_IO);
        CHECK(pci64_irq_state(s) == PCI_IRQSET_QUIESCED);                      // verified silent, NOT live
        CHECK(g_pt.desc[irq_of(s, 0)].sealed);                                 // boundary attempted: pinned
        CHECK(msix ? !would_send_msix(0, 0) : !would_send_msi(0));             // and nothing can reach the CPU
        CHECK(pci64_irq_free(s) == PCI64_IRQ_ERR_SEALED);
    }
    group_end("boundary cases: pre-boundary -> ARMED/unsealed; verified -> LIVE/sealed; unverified -> QUIESCED/FAULTED sealed; seal BEFORE the go-live write, LIVE only after the readback; a device ignoring the go-live write is never LIVE");
}

// ── lifecycle matrix (every API call in every state) ───────────────────
typedef struct { int request, enable, quiesce, free_; } row_t;
static row_t probe_state(int state) {
    row_t r;
    pt_reset(); counters_reset(); f_cfg_filter = 0; g_boundary_crossed = 0;
    fdev_init(0, 1, 1, 0, 0, 0, 0);
    pci64_irq_set_t* s = 0; pci64_irq_alloc(&fdev[0], 1, 1, PCI64_IRQ_MSI, &s);
    if (state >= PCI_IRQSET_BOUND) pci64_irq_request(s, 0, hA, &ctxA, "A");
    if (state == PCI_IRQSET_ARMED) pci64_irq_prepare(s);
    if (state >= PCI_IRQSET_LIVE) {
        if (state == PCI_IRQSET_FAULTED) f_cfg_filter = flt_msi_stuck_enable;
        pci64_irq_enable(s);
        if (state == PCI_IRQSET_QUIESCED) pci64_irq_quiesce(s);
        if (state == PCI_IRQSET_FAULTED) { pci64_irq_quiesce(s); f_cfg_filter = 0; }
    }
    // Order matters (state-preserving calls first, destructive ones last).
    if (state == PCI_IRQSET_ALLOCATED) {
        r.enable  = pci64_irq_enable(s);
        r.quiesce = pci64_irq_quiesce(s);
        r.request = pci64_irq_request(s, 0, hB, &ctxB, "B");
    } else {
        r.request = pci64_irq_request(s, 0, hB, &ctxB, "B");
        r.enable  = pci64_irq_enable(s);
        r.quiesce = pci64_irq_quiesce(s);
    }
    r.free_   = pci64_irq_free(s);
    return r;
}
static void test_lifecycle_matrix(void) {
    group_begin();
    row_t a = probe_state(PCI_IRQSET_ALLOCATED);
    CHECK(a.request == 0 && a.enable == PCI64_IRQ_ERR_BADSTATE && a.quiesce == PCI64_IRQ_ERR_BADSTATE && a.free_ == 0);
    row_t b = probe_state(PCI_IRQSET_BOUND);
    CHECK(b.request == PCI64_IRQ_ERR_BADSTATE && b.enable == 0 && b.quiesce == 0 && b.free_ == PCI64_IRQ_ERR_SEALED);   // enable then quiesce succeed in this order
    row_t l = probe_state(PCI_IRQSET_LIVE);
    CHECK(l.request == PCI64_IRQ_ERR_BADSTATE && l.enable == PCI64_IRQ_ERR_BADSTATE && l.quiesce == 0 && l.free_ == PCI64_IRQ_ERR_SEALED);
    row_t q = probe_state(PCI_IRQSET_QUIESCED);
    CHECK(q.request == PCI64_IRQ_ERR_BADSTATE && q.enable == PCI64_IRQ_ERR_BADSTATE && q.quiesce == 0 && q.free_ == PCI64_IRQ_ERR_SEALED);
    row_t r = probe_state(PCI_IRQSET_ARMED);
    CHECK(r.request == PCI64_IRQ_ERR_BADSTATE && r.enable == PCI64_IRQ_ERR_BADSTATE && r.quiesce == PCI64_IRQ_ERR_BADSTATE && r.free_ == 0);
    row_t f = probe_state(PCI_IRQSET_FAULTED);
    CHECK(f.request == PCI64_IRQ_ERR_BADSTATE && f.enable == PCI64_IRQ_ERR_FAULTED && f.quiesce == PCI64_IRQ_ERR_FAULTED && f.free_ == PCI64_IRQ_ERR_SEALED);
    // Bad arguments never mutate state.
    pt_reset(); fdev_init(0, 1, 1, 0, 0, 0, 0);
    pci64_irq_set_t* s = 0; pci64_irq_alloc(&fdev[0], 1, 1, PCI64_IRQ_MSI, &s);
    CHECK(pci64_irq_request(s, 5, hA, 0, "x") == PCI64_IRQ_ERR_INVAL && pci64_irq_request(s, 0, 0, 0, "x") == PCI64_IRQ_ERR_INVAL);
    CHECK(pci64_irq_state(s) == PCI_IRQSET_ALLOCATED);
    CHECK(pci64_irq_alloc(0, 1, 1, PCI64_IRQ_ANY, &s) == PCI64_IRQ_ERR_INVAL && pci64_irq_alloc(&fdev[1], 0, 1, PCI64_IRQ_ANY, &s) == PCI64_IRQ_ERR_INVAL);
    group_end("lifecycle matrix: every API call in ALLOCATED/BOUND/ARMED/LIVE/QUIESCED/FAULTED");
}

// ═════════════════════ T7: read-only inspection of REAL devices ══════════════
static const pci64_cfg_backend_t* spy_old;
static int spy_writes, spy_reads;
static uint8_t  sp_r8 (uint8_t b, uint8_t s, uint8_t f, uint8_t o) { spy_reads++; return spy_old->read8(b, s, f, o); }
static uint16_t sp_r16(uint8_t b, uint8_t s, uint8_t f, uint8_t o) { spy_reads++; return spy_old->read16(b, s, f, o); }
static uint32_t sp_r32(uint8_t b, uint8_t s, uint8_t f, uint8_t o) { spy_reads++; return spy_old->read32(b, s, f, o); }
static void sp_w8 (uint8_t b, uint8_t s, uint8_t f, uint8_t o, uint8_t v)  { (void)b; (void)s; (void)f; (void)o; (void)v; spy_writes++; }   // BLOCKED
static void sp_w16(uint8_t b, uint8_t s, uint8_t f, uint8_t o, uint16_t v) { (void)b; (void)s; (void)f; (void)o; (void)v; spy_writes++; }
static void sp_w32(uint8_t b, uint8_t s, uint8_t f, uint8_t o, uint32_t v) { (void)b; (void)s; (void)f; (void)o; (void)v; spy_writes++; }
static const pci64_cfg_backend_t spy_be = { sp_r8, sp_r16, sp_r32, sp_w8, sp_w16, sp_w32 };
static const pci_mmio_ops_t* spy_mmio_old; static int spy_mmio_writes, spy_mmio_reads;
static void* spm_map(uint64_t p, uint64_t l) { return spy_mmio_old->map(p, l); }
static uint32_t spm_r32(void* b, uint32_t o) { spy_mmio_reads++; return spy_mmio_old->r32(b, o); }
static void spm_w32(void* b, uint32_t o, uint32_t v) { (void)b; (void)o; (void)v; spy_mmio_writes++; }   // BLOCKED
static const pci_mmio_ops_t spy_mmio = { spm_map, spm_r32, spm_w32 };

static void test_readonly_inspection(const pci64_cfg_backend_t* real_be) {
    group_begin();
    // (a) fake device under a write-trapping check: inspection performs zero writes.
    pt_reset(); fdev_init(0, 1, 1, 0, 0, 1, 4); ev_reset();
    const pci64_cfg_backend_t* prev_be = pci64_cfg_set_backend(&fake_be);
    const pci_mmio_ops_t* om = pci_irq64_set_mmio_ops(&fake_mmio);
    pci64_irq_inspect_dev(&fdev[0]);
    CHECK(nev == 0);
    pci_irq64_set_mmio_ops(om);
    pci64_cfg_set_backend(prev_be);

    // (b) REAL devices: reads pass through, every write is blocked and counted.
    int count = pci64_device_count();
    if (count > 0) {
        spy_old = real_be; spy_writes = spy_reads = 0;
        pci64_cfg_set_backend(&spy_be);
        spy_mmio_old = pci_irq64_set_mmio_ops(&spy_mmio); spy_mmio_writes = spy_mmio_reads = 0;
        pci64_irq_inspect_all();
        pci_irq64_set_mmio_ops(spy_mmio_old);
        pci64_cfg_set_backend(real_be);
        CHECK(spy_writes == 0 && spy_mmio_writes == 0);
        CHECK(spy_reads > 0);
        // Internal consistency of every real device's MSI-X metadata.
        int bad = 0, seen_msix = 0;
        for (pci64_device_t* d = pci64_iter(0); d; d = d->next) {
            pci_cfg_ro_t ro; pci_cfg_ro_for_dev(d, &ro);
            int off, cnt; if (pci_cap_find(&ro, PCI_CAP_ID_MSIX, 0, &off, &cnt) != PCI_CAP_FOUND) continue;
            pci_msix_cap_t x; pci_msix_regions_t r; seen_msix++;
            if (pci_msix_decode(&ro, (uint8_t)off, &x) != 0 || pci_msix_resolve(&x, d->bar, &r) != 0) bad++;
        }
        CHECK(bad == 0);
        klog_hex("pci_irq64 selftest: real MSI-X devices inspected read-only: ", (uint32_t)seen_msix);
    }
    group_end("read-only inspection: zero config/MMIO writes (fake + real devices under a write-blocking spy), real MSI-X metadata self-consistent");
}

// ═══════════════════════════════ driver ═══════════════════════════════════
void pci_irq64_selftest_run(void) {
    g_pass = g_fail = 0;
    klog("pci_irq64 selftest: begin\n");
    uint64_t entry_flags = rflags();
    pci_irq64_set_quiet(1);

    const pci64_cfg_backend_t* real_be = pci64_cfg_set_backend(&fake_be);
    for (int i = 0; i < NFAKE; i++) { fdev_init(i, 0, 0, 0, 0, 0, 0); }
    if_violations = 0;
    test_config_backend();                     // toggles IF itself; runs against the LIVE table (never swapped here)

    __asm__ volatile ("cli" ::: "memory");     // everything below swaps the global irq table: IF must stay 0
    g_saved_tbl = irq64_test_swap_table(&g_pt);
    irq64_msg_set_eoi_hook(eoi_hook);
    const pci_mmio_ops_t* om = pci_irq64_set_mmio_ops(&fake_mmio);
    uint32_t io0 = ioapic64_op_count;

    test_cap_walker();
    test_decode();
    test_composer();
    test_msi_flow();
    test_msi_mask();
    test_msix_flow();
    test_failure_injection();
    test_readback_failures();
    test_faulted();
    test_stale_after_quiesce();
    test_alloc_policy();
    test_lifecycle_matrix();
    test_armed_lifecycle();
    test_boundary_cases();
    group_begin(); CHECK(if_violations == 0); CHECK(ioapic64_op_count == io0);
    group_end("every raw config access ran with IF=0; the IOAPIC was never touched by any MSI/MSI-X path");

    pt_reset();                                // release any leaked test sets
    pci_irq64_set_mmio_ops(om);
    irq64_msg_set_eoi_hook(0);
    irq64_test_swap_table(g_saved_tbl);
    pci64_cfg_set_backend(real_be);
    test_readonly_inspection(real_be);         // real devices: read-only
    if (irq64_mode() == IRQ64_MODE_PIC) {      // the LIVE table in a forced-PIC (or no-APIC) boot
        pci64_device_t* d = pci64_iter(0);
        if (d) {
            group_begin();
            pci64_irq_set_t* s = 0;
            CHECK(pci64_irq_alloc(d, 1, 1, PCI64_IRQ_ANY, &s) == PCI64_IRQ_ERR_NODEV && d->irq_state == 0);
            group_end("PIC mode on the live table: MSI/MSI-X allocation returns -ENODEV, nothing enabled underneath the PIC fallback");
        }
    }
    pci_irq64_set_quiet(0);
    if (entry_flags & 0x200) __asm__ volatile ("sti" ::: "memory");

    klog_hex("pci_irq64 selftest: checks passed: ", (uint32_t)g_pass);
    klog_hex("pci_irq64 selftest: checks failed: ", (uint32_t)g_fail);
    klog(g_fail ? "pci_irq64 selftest: RESULT FAIL\n" : "pci_irq64 selftest: RESULT PASS\n");
}
