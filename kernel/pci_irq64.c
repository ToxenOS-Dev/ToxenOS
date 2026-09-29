// kernel/pci_irq64.c -- M+11B PCI MSI / MSI-X allocation and go-live.
// See include/pci_irq64.h for the lifecycle, the sealed-set invariant and the
// device-quiesce contract.
//
// Go-live boundaries (the one moment the device is exposed):
//   MSI   : the single write that sets MSI Enable (Enable=0 throughout setup).
//   MSI-X : the single write that clears MASKALL (MASKALL|ENABLE throughout
//           setup; every table entry masked while messages are programmed).
// Everything before the boundary is snapshotted and fully restorable.
// The set is marked LIVE and its descriptors SEALED *before* the boundary
// write is issued, so even if that write's readback fails, nothing that a
// stray message could still reach is ever released.
#include <stdint.h>
#include "../include/pci_irq64.h"
#include "../include/pci_cap64.h"
#include "../include/msi_x86_64.h"
#include "../include/heap64.h"
#include "../include/physmem64.h"
#include "../include/klog.h"

// ── MMIO ops ─────────────────────────────────────────────────────────
static void* real_map(uint64_t phys, uint64_t len) { return physmem64_map_mmio(phys, len); }
static uint32_t real_r32(void* base, uint32_t off) { return *(volatile uint32_t*)((volatile char*)base + off); }
static void real_w32(void* base, uint32_t off, uint32_t v) { *(volatile uint32_t*)((volatile char*)base + off) = v; }
static const pci_mmio_ops_t g_real_mmio = { real_map, real_r32, real_w32 };
static const pci_mmio_ops_t* g_mmio = &g_real_mmio;

const pci_mmio_ops_t* pci_irq64_set_mmio_ops(const pci_mmio_ops_t* ops) {
    const pci_mmio_ops_t* old = g_mmio;
    g_mmio = ops ? ops : &g_real_mmio;
    return old;
}

// ── step counter (failure injection for the self-tests) ──────────────
static int g_step_count = 0, g_fail_at = 0;
static int step_fail(void) { g_step_count++; return g_fail_at != 0 && g_step_count == g_fail_at; }
void pci_irq64_test_arm(int fail_at) { g_step_count = 0; g_fail_at = fail_at; }
int  pci_irq64_test_steps(void) { return g_step_count; }

// ── set object ───────────────────────────────────────────────────────
struct pci64_irq_set {
    pci64_device_t*    dev;
    irq64_table_t*     tbl;
    pci64_irq_type_t   type;
    pci_irqset_state_t state;
    int                count;
    pci_msi_cap_t      msi;
    pci_msix_cap_t     msix;
    pci_msix_regions_t reg;
    void*              table_base;
    void*              pba_base;
    irq64_msg_owner_t  owner;
    struct { int irq; uint16_t dev_index; int bound; } vec[PCI64_IRQ_MAX_SET];
    const char*        gate_op;      // diagnostic: gate step that failed
    int                sealed;       // the boundary was attempted (descriptors sealed)
    // Pre-prepare snapshot, alive while ARMED (freed at a verified/attempted
    // boundary or on disarm). Everything prepare/activate can modify.
    struct {
        int      valid;
        uint16_t cmd0, ctrl0;
        uint32_t lo0, hi0, mask0; uint16_t data0;             // MSI fields
        uint32_t* snap_ctrl; uint32_t* snap_ent;              // MSI-X table snapshot
        int      table_snapped, intx_changed, unmasked_bookkeeping;
        msi_x86_msg_t msg[PCI64_IRQ_MAX_SET];
    } snap;
};

// Self-tests silence the boot log for their fake devices but still count the
// FAULTED diagnostics, so "exactly one loud diagnostic per fault" is testable.
static int g_quiet = 0;
static uint32_t g_diag_count = 0;
void pci_irq64_set_quiet(int q) { g_quiet = q; }
uint32_t pci_irq64_diag_count(void) { return g_diag_count; }

// ── formatting for the few log lines ─────────────────────────────────
static char* put_str(char* p, const char* s) { while (*s) *p++ = *s++; return p; }
static char* put_dec(char* p, uint32_t v) {
    char t[12]; int n = 0;
    if (!v) t[n++] = '0';
    while (v) { t[n++] = (char)('0' + v % 10); v /= 10; }
    while (n) *p++ = t[--n];
    return p;
}
static char* put_hex2(char* p, uint32_t v) {
    const char* h = "0123456789abcdef";
    *p++ = h[(v >> 4) & 0xF]; *p++ = h[v & 0xF]; return p;
}
static char* put_bdf(char* p, const pci64_device_t* d) {
    p = put_hex2(p, d->bus); *p++ = ':'; p = put_hex2(p, d->slot); *p++ = '.';
    *p++ = (char)('0' + (d->func & 7)); return p;
}
static uint16_t bdf_of(const pci64_device_t* d) { return (uint16_t)((d->bus << 8) | (d->slot << 3) | (d->func & 7)); }

// ── config helpers (native width) ────────────────────────────────────
#define R16(d, o)  pci64_config_read16((d)->bus, (d)->slot, (d)->func, (uint8_t)(o))
#define R32(d, o)  pci64_config_read32((d)->bus, (d)->slot, (d)->func, (uint8_t)(o))
#define W16(d, o, v) pci64_config_write16((d)->bus, (d)->slot, (d)->func, (uint8_t)(o), (uint16_t)(v))
#define W32(d, o, v) pci64_config_write32((d)->bus, (d)->slot, (d)->func, (uint8_t)(o), (uint32_t)(v))
#define U16(d, o, clr, set) pci64_cfg_update16((d)->bus, (d)->slot, (d)->func, (uint8_t)(o), (uint16_t)(clr), (uint16_t)(set))
#define U32(d, o, clr, set) pci64_cfg_update32((d)->bus, (d)->slot, (d)->func, (uint8_t)(o), (uint32_t)(clr), (uint32_t)(set))

#define PCI_CMD_MEM   0x0002
#define PCI_CMD_BME   0x0004
#define PCI_CMD_INTXD 0x0400

// ── per-vector hardware mask (the irq64 chip's ctl) ──────────────────
static int set_ctl(void* owner, uint16_t idx, int enable) {
    pci64_irq_set_t* s = (pci64_irq_set_t*)owner;
    if (s->state != PCI_IRQSET_LIVE) return PCI64_IRQ_ERR_BADSTATE;
    if (idx >= s->count) return PCI64_IRQ_ERR_INVAL;
    if (s->type == PCI64_IRQ_TYPE_MSIX) {
        uint32_t off = (uint32_t)idx * PCI_MSIX_ENTRY_SIZE + PCI_MSIX_ENTRY_CTRL;
        uint64_t f; __asm__ volatile ("pushfq; pop %0; cli" : "=r"(f) :: "memory");
        uint32_t v = g_mmio->r32(s->table_base, off);
        v = enable ? (v & ~1u) : (v | 1u);
        g_mmio->w32(s->table_base, off, v);
        uint32_t rb = g_mmio->r32(s->table_base, off);        // flush the posted write + verify
        if (f & 0x200) __asm__ volatile ("sti" ::: "memory");
        return ((rb & 1u) == (enable ? 0u : 1u)) ? 0 : PCI64_IRQ_ERR_IO;
    }
    if (!s->msi.per_vec_mask) return IRQ64_E_NOTSUP;
    U32(s->dev, s->msi.mask_off, enable ? 1u : 0u, enable ? 0u : 1u);
    uint32_t rb = R32(s->dev, s->msi.mask_off);
    return ((rb & 1u) == (enable ? 0u : 1u)) ? 0 : PCI64_IRQ_ERR_IO;
}

// ── read-only queries ────────────────────────────────────────────────
pci64_irq_type_t   pci64_irq_type(const pci64_irq_set_t* s)  { return s ? s->type : PCI64_IRQ_TYPE_NONE; }
pci_irqset_state_t pci64_irq_state(const pci64_irq_set_t* s) { return s->state; }
int                pci64_irq_count(const pci64_irq_set_t* s) { return s ? s->count : 0; }
int pci64_irq_vector(const pci64_irq_set_t* s, int i, pci64_irq_vector_t* out) {
    if (!s || i < 0 || i >= s->count || !out) return PCI64_IRQ_ERR_INVAL;
    out->irq = s->vec[i].irq; out->dev_index = s->vec[i].dev_index;
    return 0;
}

int pci64_irq_pending(const pci64_irq_set_t* s, int i) {
    if (!s || i < 0 || i >= s->count) return PCI64_IRQ_ERR_INVAL;
    if (s->type == PCI64_IRQ_TYPE_MSIX) {
        if (!s->pba_base) return IRQ64_E_NOTSUP;
        uint32_t w = g_mmio->r32(s->pba_base, (uint32_t)(i / 32) * 4);
        return (w >> (i % 32)) & 1;
    }
    if (!s->msi.per_vec_mask) return IRQ64_E_NOTSUP;
    return (int)(R32(s->dev, s->msi.pending_off) & 1u);
}

// ── allocation ───────────────────────────────────────────────────────
static void free_vectors(pci64_irq_set_t* s, int upto) {
    for (int i = 0; i < upto; i++) irq64_table_msg_free(s->tbl, s->vec[i].irq);
}

int pci64_irq_alloc(pci64_device_t* dev, int min, int max, unsigned flags, pci64_irq_set_t** out) {
    if (!dev || !out || min < 1 || max < min || !(flags & PCI64_IRQ_ANY)) return PCI64_IRQ_ERR_INVAL;
    *out = 0;
    if (dev->irq_state) return PCI64_IRQ_ERR_ALREADY_OWNED;
    irq64_table_t* tbl = irq64_cur_table();
    if (!tbl->apic_mode) return PCI64_IRQ_ERR_NODEV;            // forced PIC: MSI is not enabled underneath it

    pci_cfg_ro_t ro; pci_cfg_ro_for_dev(dev, &ro);
    int moff = -1, mcnt = 0, xoff = -1, xcnt = 0;
    pci_cap_status_t ms = pci_cap_find(&ro, PCI_CAP_ID_MSI,  0, &moff, &mcnt);
    pci_cap_status_t xs = pci_cap_find(&ro, PCI_CAP_ID_MSIX, 0, &xoff, &xcnt);
    if (ms == PCI_CAP_MALFORMED || xs == PCI_CAP_MALFORMED) {
        // A malformed list makes BOTH capabilities untrustworthy.
        return PCI64_IRQ_ERR_MALFORMED;
    }

    // Already MSI/MSI-X enabled (either capability) -> not ours to take over.
    if (ms == PCI_CAP_FOUND && (R16(dev, moff + 2) & PCI_MSI_CTRL_ENABLE))  return PCI64_IRQ_ERR_ALREADY_ACTIVE;
    if (xs == PCI_CAP_FOUND && (R16(dev, xoff + 2) & PCI_MSIX_CTRL_ENABLE)) return PCI64_IRQ_ERR_ALREADY_ACTIVE;

    pci_msix_cap_t xc; pci_msix_regions_t reg;
    int msix_ok = 0, msi_ok = 0, saw_bad = 0;
    pci_msi_cap_t mc;
    if (xs == PCI_CAP_FOUND) {
        if (xcnt != 1 || pci_msix_decode(&ro, (uint8_t)xoff, &xc) != 0 || pci_msix_resolve(&xc, dev->bar, &reg) != 0) saw_bad = 1;
        else msix_ok = 1;
    }
    if (ms == PCI_CAP_FOUND) {
        if (mcnt != 1 || pci_msi_decode(&ro, (uint8_t)moff, &mc) != 0) saw_bad = 1;
        else msi_ok = 1;
    }

    pci64_irq_type_t type = PCI64_IRQ_TYPE_NONE;
    int n = 0;
    if ((flags & PCI64_IRQ_MSIX) && msix_ok) {
        n = (int)xc.table_size;
        if (n > max) n = max;
        if (n > PCI64_IRQ_MAX_SET) n = PCI64_IRQ_MAX_SET;
        if (n >= min) type = PCI64_IRQ_TYPE_MSIX; else n = 0;
    }
    if (type == PCI64_IRQ_TYPE_NONE && (flags & PCI64_IRQ_MSI) && msi_ok) {
        // Plain MSI: exactly ONE vector, ever (MME stays 0).
        if (min > 1) return PCI64_IRQ_ERR_NOSPC;
        type = PCI64_IRQ_TYPE_MSI; n = 1;
    }
    if (type == PCI64_IRQ_TYPE_NONE) {
        if (saw_bad) return PCI64_IRQ_ERR_MALFORMED;
        return ((flags & PCI64_IRQ_MSIX) && msix_ok) ? PCI64_IRQ_ERR_NOSPC : PCI64_IRQ_ERR_NODEV;
    }

    void* table_base = 0; void* pba_base = 0;
    if (type == PCI64_IRQ_TYPE_MSIX) {
        if (!(R16(dev, 0x04) & PCI_CMD_MEM)) return PCI64_IRQ_ERR_NO_DECODE;
        table_base = g_mmio->map(reg.table_phys, reg.table_len);
        if (!table_base) return PCI64_IRQ_ERR_IO;
        pba_base = g_mmio->map(reg.pba_phys, reg.pba_len);       // diagnostics only; may be NULL
    }

    pci64_irq_set_t* s = (pci64_irq_set_t*)kmalloc(sizeof(*s));
    if (!s) return PCI64_IRQ_ERR_NOMEM;
    char* z = (char*)s; for (uint64_t i = 0; i < sizeof(*s); i++) z[i] = 0;
    s->dev = dev; s->tbl = tbl; s->type = type; s->state = PCI_IRQSET_ALLOCATED;
    if (type == PCI64_IRQ_TYPE_MSIX) { s->msix = xc; s->reg = reg; s->table_base = table_base; s->pba_base = pba_base; }
    else s->msi = mc;
    s->owner.owner = s; s->owner.ctl = set_ctl;

    int can_mask = (type == PCI64_IRQ_TYPE_MSIX) ? 1 : mc.per_vec_mask;
    int got = 0;
    for (int i = 0; i < n; i++) {
        int irq;
        int rc = irq64_table_msg_alloc(tbl, type == PCI64_IRQ_TYPE_MSIX ? IRQ64_SRC_MSIX : IRQ64_SRC_MSI,
                                       bdf_of(dev), (uint16_t)i, can_mask, &s->owner, &irq);
        if (rc != 0) {
            if (i >= min) break;                                  // fewer than asked, but enough
            free_vectors(s, got); kfree(s);
            return rc == IRQ64_E_NODEV ? PCI64_IRQ_ERR_NODEV : PCI64_IRQ_ERR_NOSPC;
        }
        s->vec[i].irq = irq; s->vec[i].dev_index = (uint16_t)i; s->vec[i].bound = 0;
        got++;
    }
    s->count = got;
    dev->irq_state = s;
    *out = s;
    return got;
}

int pci64_irq_request(pci64_irq_set_t* s, int i, irq64_handler_t h, void* ctx, const char* name) {
    if (!s || !h || i < 0 || i >= s->count) return PCI64_IRQ_ERR_INVAL;
    if (s->state != PCI_IRQSET_ALLOCATED) return PCI64_IRQ_ERR_BADSTATE;
    if (s->vec[i].bound) return PCI64_IRQ_ERR_BADSTATE;           // each vector exactly once
    int rc = irq64_table_msg_bind(s->tbl, s->vec[i].irq, h, ctx, name);
    if (rc != 0) return PCI64_IRQ_ERR_BADSTATE;
    s->vec[i].bound = 1;
    int all = 1;
    for (int k = 0; k < s->count; k++) if (!s->vec[k].bound) all = 0;
    if (all) s->state = PCI_IRQSET_BOUND;
    return 0;
}

static void snap_free(pci64_irq_set_t* s) {
    if (s->snap.snap_ctrl) { kfree(s->snap.snap_ctrl); s->snap.snap_ctrl = 0; }
    if (s->snap.snap_ent)  { kfree(s->snap.snap_ent);  s->snap.snap_ent = 0; }
    s->snap.valid = 0;
}

static int disarm_restore(pci64_irq_set_t* s);

int pci64_irq_free(pci64_irq_set_t* s) {
    if (!s) return PCI64_IRQ_ERR_INVAL;
    if (s->state == PCI_IRQSET_ARMED && !s->sealed) {
        int rc = disarm_restore(s);
        if (rc != 0) return rc;
    }
    if (s->state != PCI_IRQSET_ALLOCATED && s->state != PCI_IRQSET_BOUND) return PCI64_IRQ_ERR_SEALED;
    for (int i = 0; i < s->count; i++) {
        s->tbl->desc[s->vec[i].irq].handler = 0;                 // unbound before release
        s->tbl->desc[s->vec[i].irq].requested = 0;
        irq64_table_msg_free(s->tbl, s->vec[i].irq);
    }
    s->dev->irq_state = 0;
    snap_free(s);
    kfree(s);
    return 0;
}

// ── diagnostics ──────────────────────────────────────────────────────
static void log_summary(const pci64_irq_set_t* s, const char* what) {
    char line[128]; char* p = line;
    p = put_str(p, "pci64: "); p = put_bdf(p, s->dev);
    p = put_str(p, s->type == PCI64_IRQ_TYPE_MSIX ? " MSI-X " : " MSI ");
    p = put_str(p, what); p = put_str(p, ", vectors="); p = put_dec(p, (uint32_t)s->count);
    *p++ = '\n'; *p = 0; if (!g_quiet) klog(line);
}

static void log_faulted(const pci64_irq_set_t* s, const char* op) {
    char line[200]; char* p = line;
    p = put_str(p, "pci64_irq: *** FAULTED *** "); p = put_bdf(p, s->dev);
    p = put_str(p, s->type == PCI64_IRQ_TYPE_MSIX ? " MSI-X irq=" : " MSI irq=");
    for (int i = 0; i < s->count && i < 8; i++) { if (i) *p++ = ','; p = put_dec(p, (uint32_t)s->vec[i].irq); }
    p = put_str(p, " -- failed gate: "); p = put_str(p, op);
    p = put_str(p, " (hardware interrupt state uncertain; vectors/handlers stay pinned)\n");
    *p = 0; g_diag_count++; if (!g_quiet) klog(line);
}

// ── gating (used after the live boundary) ────────────────────────────
// Returns 1 if the gate read back as expected.
static int gate(pci64_irq_set_t* s, const char** op) {
    pci64_device_t* d = s->dev;
    if (s->type == PCI64_IRQ_TYPE_MSIX) {
        *op = "set MSI-X MASKALL";
        U16(d, s->msix.off + 2, 0, PCI_MSIX_CTRL_MASKALL);
        return (R16(d, s->msix.off + 2) & PCI_MSIX_CTRL_MASKALL) != 0;
    }
    if (s->msi.per_vec_mask) {
        *op = "set MSI vector-0 mask bit";
        U32(d, s->msi.mask_off, 0, 1u);
        return (R32(d, s->msi.mask_off) & 1u) != 0;
    }
    *op = "clear MSI Enable";
    U16(d, s->msi.off + 2, PCI_MSI_CTRL_ENABLE, 0);
    return (R16(d, s->msi.off + 2) & PCI_MSI_CTRL_ENABLE) == 0;
}

int pci64_irq_quiesce(pci64_irq_set_t* s) {
    if (!s) return PCI64_IRQ_ERR_INVAL;
    if (s->state == PCI_IRQSET_QUIESCED) return 0;
    if (s->state == PCI_IRQSET_FAULTED) return PCI64_IRQ_ERR_FAULTED;   // no retry pretending the device is clean
    if (s->state != PCI_IRQSET_LIVE) return PCI64_IRQ_ERR_BADSTATE;
    const char* op = "";
    if (gate(s, &op)) { s->state = PCI_IRQSET_QUIESCED; return 0; }
    s->state = PCI_IRQSET_FAULTED; s->gate_op = op; log_faulted(s, op);
    return PCI64_IRQ_ERR_IO;
}

// The boundary write was ATTEMPTED but its success could not be verified (or
// a later step failed): the set is sealed; gate the device and classify.
static int fail_after_boundary(pci64_irq_set_t* s) {
    const char* op = "";
    if (gate(s, &op)) s->state = PCI_IRQSET_QUIESCED;
    else { s->state = PCI_IRQSET_FAULTED; s->gate_op = op; log_faulted(s, op); }
    snap_free(s);
    return PCI64_IRQ_ERR_IO;
}

// Seals every descriptor IMMEDIATELY BEFORE the boundary write. The set's
// STATE deliberately stays ARMED until the readback verifies the write.
static void seal_before_boundary(pci64_irq_set_t* s) {
    s->sealed = 1;
    for (int i = 0; i < s->count; i++) irq64_table_msg_seal(s->tbl, s->vec[i].irq);
}

// ── MSI-X ────────────────────────────────────────────────────────────
#define TW(idx, reg, v) g_mmio->w32(s->table_base, (uint32_t)(idx) * PCI_MSIX_ENTRY_SIZE + (reg), (v))
#define TR(idx, reg)    g_mmio->r32(s->table_base, (uint32_t)(idx) * PCI_MSIX_ENTRY_SIZE + (reg))

// Restores the exact pre-prepare state (MSI-X): table under MASKALL first,
// Message Control last, then the INTx bit.
static void msix_restore(pci64_irq_set_t* s) {
    pci64_device_t* d = s->dev;
    const uint32_t off = s->msix.off, T = s->msix.table_size;
    const int n = s->count;
    if (s->snap.table_snapped) {
        for (int i = 0; i < n; i++) {
            // An entry's address/data may only change while the ENTRY is masked.
            TW(i, PCI_MSIX_ENTRY_CTRL,    s->snap.snap_ent[i * 4 + 3] | PCI_MSIX_ENTRY_MASKBIT);
            TW(i, PCI_MSIX_ENTRY_ADDR_LO, s->snap.snap_ent[i * 4 + 0]);
            TW(i, PCI_MSIX_ENTRY_ADDR_HI, s->snap.snap_ent[i * 4 + 1]);
            TW(i, PCI_MSIX_ENTRY_DATA,    s->snap.snap_ent[i * 4 + 2]);
            TW(i, PCI_MSIX_ENTRY_CTRL,    s->snap.snap_ent[i * 4 + 3]);
        }
        for (uint32_t e = (uint32_t)n; e < T; e++)
            if (!(s->snap.snap_ctrl[e] & PCI_MSIX_ENTRY_MASKBIT)) TW(e, PCI_MSIX_ENTRY_CTRL, s->snap.snap_ctrl[e]);
        (void)TR(0, PCI_MSIX_ENTRY_DATA);                        // flush
    }
    W16(d, off + 2, s->snap.ctrl0);
    if (s->snap.intx_changed) { U16(d, 0x04, PCI_CMD_INTXD, 0); s->snap.intx_changed = 0; }
    if (s->snap.unmasked_bookkeeping) {
        for (int i = 0; i < n; i++) s->tbl->desc[s->vec[i].irq].masked = 1;
        s->snap.unmasked_bookkeeping = 0;
    }
}

static int prepare_msix(pci64_irq_set_t* s) {
    pci64_device_t* d = s->dev;
    const uint32_t off = s->msix.off, T = s->msix.table_size;
    const int n = s->count;

    uint16_t cmd0 = R16(d, 0x04);
    if (!(cmd0 & PCI_CMD_BME)) return PCI64_IRQ_ERR_NO_BUS_MASTER;
    if (!(cmd0 & PCI_CMD_MEM)) return PCI64_IRQ_ERR_NO_DECODE;
    for (int i = 0; i < n; i++) {
        int vec = irq64_table_msg_vector(s->tbl, s->vec[i].irq);
        if (msi_x86_compose(s->tbl->dest_apic, vec, &s->snap.msg[i]) != 0) return PCI64_IRQ_ERR_COMPOSE;
    }

    s->snap.cmd0 = cmd0;
    s->snap.ctrl0 = R16(d, off + 2);
    s->snap.snap_ctrl = (uint32_t*)kmalloc((uint64_t)T * 4);
    s->snap.snap_ent  = (uint32_t*)kmalloc((uint64_t)n * 16);
    if (!s->snap.snap_ctrl || !s->snap.snap_ent) { snap_free(s); return PCI64_IRQ_ERR_NOMEM; }
    s->snap.valid = 1; s->snap.table_snapped = 0; s->snap.intx_changed = 0; s->snap.unmasked_bookkeeping = 0;

    // Function-mask + enable in ONE write: enabled but nothing can be sent.
    U16(d, off + 2, 0, PCI_MSIX_CTRL_MASKALL | PCI_MSIX_CTRL_ENABLE);
    if (step_fail()) goto fail;
    { uint16_t rb = R16(d, off + 2);
      if ((rb & (PCI_MSIX_CTRL_MASKALL | PCI_MSIX_CTRL_ENABLE)) != (PCI_MSIX_CTRL_MASKALL | PCI_MSIX_CTRL_ENABLE)) goto fail; }

    // Snapshot (function is masked now): every entry's control word, and the
    // complete 16 bytes of each used entry.
    for (uint32_t e = 0; e < T; e++) s->snap.snap_ctrl[e] = TR(e, PCI_MSIX_ENTRY_CTRL);
    for (int i = 0; i < n; i++) {
        s->snap.snap_ent[i * 4 + 0] = TR(i, PCI_MSIX_ENTRY_ADDR_LO);
        s->snap.snap_ent[i * 4 + 1] = TR(i, PCI_MSIX_ENTRY_ADDR_HI);
        s->snap.snap_ent[i * 4 + 2] = TR(i, PCI_MSIX_ENTRY_DATA);
        s->snap.snap_ent[i * 4 + 3] = TR(i, PCI_MSIX_ENTRY_CTRL);
    }
    s->snap.table_snapped = 1;
    if (step_fail()) goto fail;

    // Mask EVERY entry (stale entries beyond the used ones must not fire).
    for (uint32_t e = 0; e < T; e++)
        if (!(s->snap.snap_ctrl[e] & PCI_MSIX_ENTRY_MASKBIT)) TW(e, PCI_MSIX_ENTRY_CTRL, s->snap.snap_ctrl[e] | PCI_MSIX_ENTRY_MASKBIT);
    if (step_fail()) goto fail;

    // Program the used entries while masked.
    for (int i = 0; i < n; i++) {
        TW(i, PCI_MSIX_ENTRY_ADDR_LO, s->snap.msg[i].address_lo);
        TW(i, PCI_MSIX_ENTRY_ADDR_HI, s->snap.msg[i].address_hi);
        TW(i, PCI_MSIX_ENTRY_DATA,    s->snap.msg[i].data);
    }
    if (step_fail()) goto fail;

    // Flush the posted writes by reading back; verify.
    for (int i = 0; i < n; i++) {
        if (TR(i, PCI_MSIX_ENTRY_ADDR_LO) != s->snap.msg[i].address_lo || TR(i, PCI_MSIX_ENTRY_ADDR_HI) != s->snap.msg[i].address_hi ||
            TR(i, PCI_MSIX_ENTRY_DATA) != s->snap.msg[i].data) goto fail;
    }
    for (uint32_t e = 0; e < T; e++) if (!(TR(e, PCI_MSIX_ENTRY_CTRL) & PCI_MSIX_ENTRY_MASKBIT)) goto fail;
    if (step_fail()) goto fail;

    s->state = PCI_IRQSET_ARMED;
    return 0;

fail:
    msix_restore(s);
    snap_free(s);
    return PCI64_IRQ_ERR_IO;
}

// Undoes ONLY what activate did (INTx bit, entry unmask), leaving the set ARMED.
static void msix_undo_activate(pci64_irq_set_t* s) {
    pci64_device_t* d = s->dev;
    for (int i = 0; i < s->count; i++) {
        TW(i, PCI_MSIX_ENTRY_CTRL, s->snap.snap_ctrl[i] | PCI_MSIX_ENTRY_MASKBIT);   // re-mask (steering-tag bits preserved)
        s->tbl->desc[s->vec[i].irq].masked = 1;
    }
    s->snap.unmasked_bookkeeping = 0;
    if (s->snap.intx_changed) { U16(d, 0x04, PCI_CMD_INTXD, 0); s->snap.intx_changed = 0; }
}

static int activate_msix(pci64_irq_set_t* s) {
    pci64_device_t* d = s->dev;
    const uint32_t off = s->msix.off, T = s->msix.table_size;
    const int n = s->count;

    // Re-verify what prepare established (the device selectors were written
    // in between by the driver; the function must still be masked).
    { uint16_t rb = R16(d, off + 2);
      if ((rb & (PCI_MSIX_CTRL_MASKALL | PCI_MSIX_CTRL_ENABLE)) != (PCI_MSIX_CTRL_MASKALL | PCI_MSIX_CTRL_ENABLE)) return PCI64_IRQ_ERR_IO; }
    for (int i = 0; i < n; i++)
        if (TR(i, PCI_MSIX_ENTRY_ADDR_LO) != s->snap.msg[i].address_lo || TR(i, PCI_MSIX_ENTRY_ADDR_HI) != s->snap.msg[i].address_hi ||
            TR(i, PCI_MSIX_ENTRY_DATA) != s->snap.msg[i].data) return PCI64_IRQ_ERR_IO;
    for (uint32_t e = 0; e < T; e++) if (!(TR(e, PCI_MSIX_ENTRY_CTRL) & PCI_MSIX_ENTRY_MASKBIT)) return PCI64_IRQ_ERR_IO;
    if (step_fail()) return PCI64_IRQ_ERR_IO;

    // INTx Disable: only now, at the final transition.
    U16(d, 0x04, 0, PCI_CMD_INTXD);
    if ((R16(d, 0x04) & PCI_CMD_INTXD) && !(s->snap.cmd0 & PCI_CMD_INTXD)) s->snap.intx_changed = 1;
    if (step_fail()) { msix_undo_activate(s); return PCI64_IRQ_ERR_IO; }

    // Unmask the used entries (the function mask still holds them back).
    for (int i = 0; i < n; i++) {
        TW(i, PCI_MSIX_ENTRY_CTRL, s->snap.snap_ctrl[i] & ~PCI_MSIX_ENTRY_MASKBIT);
        s->tbl->desc[s->vec[i].irq].masked = 0; s->snap.unmasked_bookkeeping = 1;
    }
    for (int i = 0; i < n; i++) if (TR(i, PCI_MSIX_ENTRY_CTRL) & PCI_MSIX_ENTRY_MASKBIT) { msix_undo_activate(s); return PCI64_IRQ_ERR_IO; }
    if (step_fail()) { msix_undo_activate(s); return PCI64_IRQ_ERR_IO; }

    // ── BOUNDARY ─────────────────────────────────────────────────────
    if (step_fail()) { msix_undo_activate(s); return PCI64_IRQ_ERR_IO; }   // last point at which nothing can have escaped
    seal_before_boundary(s);                                    // sealed BEFORE the write is issued
    U16(d, off + 2, PCI_MSIX_CTRL_MASKALL, 0);                  // clearing MASKALL == going live
    { uint16_t rb = R16(d, off + 2);
      if (step_fail() || (rb & PCI_MSIX_CTRL_MASKALL) || !(rb & PCI_MSIX_CTRL_ENABLE)) return fail_after_boundary(s); }
    s->state = PCI_IRQSET_LIVE;                                 // LIVE only after the readback verified it
    snap_free(s);
    log_summary(s, "enabled");
    return 0;
}

// ── MSI ──────────────────────────────────────────────────────────────
static void msi_restore(pci64_irq_set_t* s) {
    pci64_device_t* d = s->dev;
    const pci_msi_cap_t* m = &s->msi;
    if (m->per_vec_mask) W32(d, m->mask_off, s->snap.mask0);
    W16(d, m->data_off, s->snap.data0);
    if (m->is64) W32(d, m->addr_hi_off, s->snap.hi0);
    W32(d, m->addr_lo_off, s->snap.lo0);
    W16(d, m->off + 2, s->snap.ctrl0);
    if (s->snap.intx_changed) { U16(d, 0x04, PCI_CMD_INTXD, 0); s->snap.intx_changed = 0; }
    if (m->per_vec_mask) s->tbl->desc[s->vec[0].irq].masked = 1;
}

static int prepare_msi(pci64_irq_set_t* s) {
    pci64_device_t* d = s->dev;
    const pci_msi_cap_t* m = &s->msi;
    uint16_t cmd0 = R16(d, 0x04);
    if (!(cmd0 & PCI_CMD_BME)) return PCI64_IRQ_ERR_NO_BUS_MASTER;
    int vec = irq64_table_msg_vector(s->tbl, s->vec[0].irq);
    if (msi_x86_compose(s->tbl->dest_apic, vec, &s->snap.msg[0]) != 0) return PCI64_IRQ_ERR_COMPOSE;

    // Snapshot every field this path can modify.
    s->snap.cmd0 = cmd0;
    s->snap.ctrl0 = R16(d, m->off + 2);
    s->snap.lo0 = R32(d, m->addr_lo_off);
    s->snap.hi0 = m->is64 ? R32(d, m->addr_hi_off) : 0;
    s->snap.data0 = R16(d, m->data_off);
    s->snap.mask0 = m->per_vec_mask ? R32(d, m->mask_off) : 0;
    s->snap.valid = 1; s->snap.intx_changed = 0;
    const msi_x86_msg_t* msg = &s->snap.msg[0];

    // MSI Enable stays 0 throughout; MME is programmed to 0 (one vector).
    U16(d, m->off + 2, PCI_MSI_CTRL_MME_MASK, 0);
    if (step_fail()) goto fail;
    if (R16(d, m->off + 2) & (PCI_MSI_CTRL_MME_MASK | PCI_MSI_CTRL_ENABLE)) goto fail;

    if (m->per_vec_mask) {
        U32(d, m->mask_off, 0, 1u);                              // vector 0 masked while programming
        if (step_fail()) goto fail;
    }

    W32(d, m->addr_lo_off, msg->address_lo);
    if (m->is64) W32(d, m->addr_hi_off, msg->address_hi);
    W16(d, m->data_off, msg->data);
    if (step_fail()) goto fail;
    if (R32(d, m->addr_lo_off) != msg->address_lo || (m->is64 && R32(d, m->addr_hi_off) != msg->address_hi) ||
        R16(d, m->data_off) != (uint16_t)msg->data) goto fail;
    if (step_fail()) goto fail;

    s->state = PCI_IRQSET_ARMED;
    return 0;
fail:
    msi_restore(s);
    snap_free(s);
    return PCI64_IRQ_ERR_IO;
}

static void msi_undo_activate(pci64_irq_set_t* s) {
    pci64_device_t* d = s->dev;
    const pci_msi_cap_t* m = &s->msi;
    if (m->per_vec_mask) { U32(d, m->mask_off, 0, 1u); s->tbl->desc[s->vec[0].irq].masked = 1; }
    if (s->snap.intx_changed) { U16(d, 0x04, PCI_CMD_INTXD, 0); s->snap.intx_changed = 0; }
}

static int activate_msi(pci64_irq_set_t* s) {
    pci64_device_t* d = s->dev;
    const pci_msi_cap_t* m = &s->msi;

    // INTx Disable: only at the final transition.
    U16(d, 0x04, 0, PCI_CMD_INTXD);
    if ((R16(d, 0x04) & PCI_CMD_INTXD) && !(s->snap.cmd0 & PCI_CMD_INTXD)) s->snap.intx_changed = 1;
    if (step_fail()) { msi_undo_activate(s); return PCI64_IRQ_ERR_IO; }

    if (m->per_vec_mask) {
        U32(d, m->mask_off, 1u, 0);                              // unmask: MSI Enable is the boundary
        if (R32(d, m->mask_off) & 1u) { msi_undo_activate(s); return PCI64_IRQ_ERR_IO; }
        s->tbl->desc[s->vec[0].irq].masked = 0;
    }
    if (step_fail()) { msi_undo_activate(s); return PCI64_IRQ_ERR_IO; }

    // ── BOUNDARY ─────────────────────────────────────────────────────
    if (step_fail()) { msi_undo_activate(s); return PCI64_IRQ_ERR_IO; }
    seal_before_boundary(s);
    U16(d, m->off + 2, 0, PCI_MSI_CTRL_ENABLE);                  // MSI Enable = 1 LAST
    if (step_fail() || !(R16(d, m->off + 2) & PCI_MSI_CTRL_ENABLE)) return fail_after_boundary(s);
    s->state = PCI_IRQSET_LIVE;
    snap_free(s);
    log_summary(s, "enabled");
    return 0;
}

#undef TW
#undef TR

// ── lifecycle entry points ───────────────────────────────────────────
int pci64_irq_prepare(pci64_irq_set_t* s) {
    if (!s) return PCI64_IRQ_ERR_INVAL;
    if (s->state == PCI_IRQSET_FAULTED) return PCI64_IRQ_ERR_FAULTED;
    if (s->state != PCI_IRQSET_BOUND) return PCI64_IRQ_ERR_BADSTATE;
    return s->type == PCI64_IRQ_TYPE_MSIX ? prepare_msix(s) : prepare_msi(s);
}

int pci64_irq_activate(pci64_irq_set_t* s) {
    if (!s) return PCI64_IRQ_ERR_INVAL;
    if (s->state == PCI_IRQSET_FAULTED) return PCI64_IRQ_ERR_FAULTED;
    if (s->state != PCI_IRQSET_ARMED || s->sealed) return PCI64_IRQ_ERR_BADSTATE;
    return s->type == PCI64_IRQ_TYPE_MSIX ? activate_msix(s) : activate_msi(s);
}

// ARMED -> BOUND: exact restore of the pre-prepare device state.
static int disarm_restore(pci64_irq_set_t* s) {
    if (s->state != PCI_IRQSET_ARMED || s->sealed) return PCI64_IRQ_ERR_BADSTATE;
    if (s->type == PCI64_IRQ_TYPE_MSIX) msix_restore(s); else msi_restore(s);
    snap_free(s);
    s->state = PCI_IRQSET_BOUND;
    return 0;
}

int pci64_irq_disarm(pci64_irq_set_t* s) {
    if (!s) return PCI64_IRQ_ERR_INVAL;
    return disarm_restore(s);
}

int pci64_irq_enable(pci64_irq_set_t* s) {
    if (!s) return PCI64_IRQ_ERR_INVAL;
    if (s->state == PCI_IRQSET_FAULTED) return PCI64_IRQ_ERR_FAULTED;
    if (s->state != PCI_IRQSET_BOUND) return PCI64_IRQ_ERR_BADSTATE;   // incl. ARMED/LIVE/QUIESCED: no re-enable
    int rc = pci64_irq_prepare(s);
    if (rc != 0) return rc;
    rc = pci64_irq_activate(s);
    if (rc != 0 && s->state == PCI_IRQSET_ARMED && !s->sealed) disarm_restore(s);   // pre-boundary: back to BOUND
    return rc;
}

// ── read-only inspection ─────────────────────────────────────────────
// Uses ONLY the read-only config view (a type with no write entries) and
// MMIO reads of the table's vector-control words: it can inspect devices
// owned by production drivers without any chance of modifying them.
void pci64_irq_inspect_dev(pci64_device_t* d) {
    pci_cfg_ro_t ro; pci_cfg_ro_for_dev(d, &ro);
    int moff = -1, xoff = -1, mc = 0, xc = 0;
    pci_cap_status_t ms = pci_cap_find(&ro, PCI_CAP_ID_MSI,  0, &moff, &mc);
    pci_cap_status_t xs = pci_cap_find(&ro, PCI_CAP_ID_MSIX, 0, &xoff, &xc);
    if (ms == PCI_CAP_MALFORMED || xs == PCI_CAP_MALFORMED) {
        char line[80]; char* p = put_str(line, "pci64: inspect "); p = put_bdf(p, d);
        p = put_str(p, " malformed capability list\n"); *p = 0; klog(line);
        return;
    }
    if (ms == PCI_CAP_FOUND) {
        pci_msi_cap_t m;
        char line[120]; char* p = put_str(line, "pci64: inspect "); p = put_bdf(p, d);
        if (mc == 1 && pci_msi_decode(&ro, (uint8_t)moff, &m) == 0) {
            p = put_str(p, m.is64 ? " MSI 64-bit" : " MSI 32-bit");
            p = put_str(p, m.per_vec_mask ? " pvm" : " no-mask");
            p = put_str(p, " max="); p = put_dec(p, 1u << m.mmc_log2);
            p = put_str(p, m.enabled ? " ENABLED" : " off");
        } else p = put_str(p, " MSI capability invalid");
        *p++ = '\n'; *p = 0; klog(line);
    }
    if (xs == PCI_CAP_FOUND) {
        pci_msix_cap_t x; pci_msix_regions_t r;
        char line[160]; char* p = put_str(line, "pci64: inspect "); p = put_bdf(p, d);
        if (xc != 1 || pci_msix_decode(&ro, (uint8_t)xoff, &x) != 0) p = put_str(p, " MSI-X capability invalid");
        else if (pci_msix_resolve(&x, d->bar, &r) != 0) p = put_str(p, " MSI-X table/PBA invalid (BAR check)");
        else {
            p = put_str(p, " MSI-X entries="); p = put_dec(p, x.table_size);
            p = put_str(p, " tbl=bar"); p = put_dec(p, x.table_bir); p = put_str(p, "+0x");
            { const char* h = "0123456789abcdef"; int started = 0;
              for (int i = 7; i >= 0; i--) { int v = (x.table_off >> (i * 4)) & 0xF; if (v || started || i == 0) { *p++ = h[v]; started = 1; } } }
            p = put_str(p, x.enabled ? " ENABLED" : " off");
            void* tb = g_mmio->map(r.table_phys, r.table_len);
            if (tb) {
                uint32_t masked = 0;
                for (uint32_t e = 0; e < x.table_size; e++)
                    if (g_mmio->r32(tb, e * PCI_MSIX_ENTRY_SIZE + PCI_MSIX_ENTRY_CTRL) & 1u) masked++;
                p = put_str(p, " masked="); p = put_dec(p, masked);
            }
        }
        *p++ = '\n'; *p = 0; klog(line);
    }
}

void pci64_irq_inspect_all(void) {
    for (pci64_device_t* d = pci64_iter(0); d; d = d->next) pci64_irq_inspect_dev(d);
}
