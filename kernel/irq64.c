// kernel/irq64.c -- M+11A generic interrupt core. See include/irq64.h for
// the design (descriptor table, chip abstraction, edge/level flows).
#include <stdint.h>
#include "../include/irq64.h"
#include "../include/isr64.h"
#include "../include/pic.h"
#include "../include/pic_chip64.h"
#include "../include/lapic64.h"
#include "../include/ioapic64.h"
#include "../include/acpi64.h"
#include "../include/madt64.h"
#include "../include/irqmode64.h"
#include "../include/klog.h"

// ── table logic (hardware-free) ──────────────────────────────────────
void irq64_table_init(irq64_table_t* t, const irq64_chip_t* legacy_chip) {
    for (int i = 0; i < IRQ64_MAX; i++) {
        irq64_desc_t* d = &t->desc[i];
        d->chip = 0; d->handler = 0; d->ctx = 0; d->name = 0;
        d->hwirq = (uint32_t)i; d->vector = -1;
        d->trigger = IRQ64_TRIG_EDGE; d->polarity = IRQ64_POL_HIGH;
        d->requested = 0; d->masked = 1; d->storm_masked = 0;
        d->count = d->handled = d->unhandled = d->spurious = 0;
        d->consecutive_unhandled = d->storm_masks = 0;
        d->src = IRQ64_SRC_NONE; d->allocated = 0; d->can_mask = 1; d->sealed = 0;
        d->bdf = 0; d->dev_index = 0; d->dest_apic = 0; d->chip_data = 0; d->eoi_count = 0;
    }
    for (int v = 0; v < 256; v++) t->vec_to_irq[v] = -1;
    irq_vector_map_init(&t->vmap);
    t->route = 0; t->orphan_eoi = 0; t->dispatched = t->orphan_vectors = 0;
    t->apic_mode = 0; t->dest_apic = 0;
    // The 16 legacy descriptors always exist, so a stray delivery on an
    // un-requested legacy vector still gets a proper chip EOI.
    for (int i = 0; i < 16; i++) {
        t->desc[i].chip = legacy_chip;
        t->desc[i].src = legacy_chip ? legacy_chip->src : IRQ64_SRC_NONE;
        t->desc[i].vector = (int16_t)(VECTOR64_LEGACY_BASE + i);
        t->vec_to_irq[VECTOR64_LEGACY_BASE + i] = (int16_t)i;
    }
}

int irq64_table_request_legacy(irq64_table_t* t, uint8_t isa_irq,
                               irq64_handler_t h, void* ctx, const char* name) {
    if (isa_irq > 15 || !h) return -1;
    irq64_desc_t* d = &t->desc[isa_irq];
    if (d->requested) return -2;
    int vec = irq_vector_claim_legacy(&t->vmap, isa_irq, isa_irq);
    if (vec < 0) return -3;
    if (t->route && t->route(t, d, isa_irq, (uint8_t)vec) != 0) {
        irq_vector_release_legacy(&t->vmap, vec);
        return -4;
    }
    d->handler = h; d->ctx = ctx; d->name = name;
    d->vector = (int16_t)vec;
    d->requested = 1;
    d->consecutive_unhandled = 0; d->storm_masked = 0;
    d->masked = 0;
    d->chip->unmask(d);
    return 0;
}

int irq64_table_free_legacy(irq64_table_t* t, uint8_t isa_irq) {
    if (isa_irq > 15) return -1;
    irq64_desc_t* d = &t->desc[isa_irq];
    if (!d->requested) return -2;
    d->chip->mask(d);
    d->masked = 1;
    d->requested = 0; d->handler = 0; d->ctx = 0; d->name = 0;
    irq_vector_release_legacy(&t->vmap, VECTOR64_LEGACY_BASE + isa_irq);
    return 0;
}

static inline void do_eoi(irq64_desc_t* d) { d->chip->eoi(d); d->eoi_count++; }

static irq64_ret_t run_handler(irq64_desc_t* d) {
    irq64_ret_t r = IRQ64_RET_NONE;
    if (d->handler) r = d->handler(d->ctx);
    if (r == IRQ64_RET_HANDLED) { d->handled++; d->consecutive_unhandled = 0; }
    else { d->unhandled++; d->consecutive_unhandled++; }
    return r;
}

void irq64_table_dispatch(irq64_table_t* t, uint8_t vector) {
    t->dispatched++;
    int irq = t->vec_to_irq[vector];
    if (irq < 0) {
        // No descriptor: never expected (only descriptor-owned vectors are
        // ever routed). EOI so the ISR bit cannot wedge the priority class.
        t->orphan_vectors++;
        if (t->orphan_eoi) t->orphan_eoi(vector);
        return;
    }
    irq64_desc_t* d = &t->desc[irq];
    d->count++;

    if (d->chip->spurious && d->chip->spurious(d)) { d->spurious++; return; }

    if (d->trigger == IRQ64_TRIG_EDGE) {
        do_eoi(d);                  // before the handler: it may context-switch away
        run_handler(d);
        return;
    }

    // Level: EOI only after the handler has had the chance to deassert the
    // source; runaway lines are masked first so the EOI cannot re-fire them.
    run_handler(d);
    if (d->consecutive_unhandled >= IRQ64_STORM_THRESHOLD) {
        d->chip->mask(d);
        d->masked = 1; d->storm_masked = 1; d->storm_masks++;
        klog("irq64: level IRQ storm -- line masked\n");
    }
    do_eoi(d);
}

// ── IOAPIC chip ──────────────────────────────────────────────────────
static void io_c_mask(irq64_desc_t* d)   { ioapic64_mask(d->hwirq); }
static void io_c_unmask(irq64_desc_t* d) { ioapic64_unmask(d->hwirq); }
static void io_c_eoi(irq64_desc_t* d)    { (void)d; lapic64_eoi(); }
static const irq64_chip_t ioapic_chip = { "IOAPIC", io_c_mask, io_c_unmask, io_c_eoi, 0, IRQ64_SRC_IOAPIC, 0 };

// ── global state + mode setup ────────────────────────────────────────
static irq64_table_t g_tbl;
static irq64_table_t* g_cur = &g_tbl;   // M+11B: swappable ONLY by self-tests (IF=0)
static irq64_mode_t  g_mode = IRQ64_MODE_PIC;
static madt64_info_t g_madt;
static irqmode_result_t g_res;

static void orphan_eoi_apic(uint8_t vector) { (void)vector; lapic64_eoi(); }

static int apic_route(irq64_table_t* t, irq64_desc_t* d, uint8_t isa, uint8_t vec) {
    (void)t;
    uint32_t gsi; int pol, trg;
    if (madt64_resolve_isa(&g_madt, isa, &gsi, &pol, &trg) != 0) return -1;
    ioapic64_rte_t r = {0};
    r.vector = vec; r.polarity = (uint8_t)pol; r.trigger = (uint8_t)trg;
    r.masked = 1; r.dest = (uint8_t)g_res.bsp_apic_id;
    if (ioapic64_route(gsi, &r) != 0) { klog("irq64: no IOAPIC covers the GSI for this ISA IRQ\n"); return -1; }
    d->hwirq = gsi; d->trigger = (uint8_t)trg; d->polarity = (uint8_t)pol;
    return 0;
}

static int pic_route(irq64_table_t* t, irq64_desc_t* d, uint8_t isa, uint8_t vec) {
    (void)t; (void)vec;
    d->hwirq = isa; d->trigger = IRQ64_TRIG_EDGE; d->polarity = IRQ64_POL_HIGH;
    return 0;
}

// The PIC-mode LAPIC "virtual wire" writes LAPIC MMIO; only legal when the
// xAPIC is actually present and enabled.
static int virtual_wire_allowed(void) {
    if (!irqmode64_real_hw.msr_supported()) return 0;
    uint64_t b = irqmode64_real_hw.rdmsr(MSR_IA32_APIC_BASE);
    return (b & APIC_BASE_GLOBAL_EN) && !(b & APIC_BASE_X2APIC_EN);
}

#ifdef IRQ64_FORCE_PIC
#define IRQ64_FORCE_PIC_VAL 1
#else
#define IRQ64_FORCE_PIC_VAL 0
#endif

void irq64_init(uint64_t mb_info_addr) {
    // The 8259s are ALWAYS remapped off the exception vectors and fully
    // masked first: in APIC mode they stay that way, in PIC mode each
    // line is unmasked only when its handler is requested.
    pic_remap();
    pic_mask_all();

    const madt64_info_t* madt = 0;
    if (!IRQ64_FORCE_PIC_VAL) {
        const acpi_sdt_header_t* mt = acpi64_find_madt(mb_info_addr);
        klog("irq64: ACPI: "); klog(acpi64_last_status()); klog("\n");
        if (mt) {
            int err = madt64_parse(mt, mt->length, &g_madt);
            if (err == 0) madt = &g_madt;
            else klog_hex("irq64: MADT parse failed, err=", (uint32_t)(-err));
        }
    }

    irqmode64_commit(&irqmode64_real_hw, madt, IRQ64_FORCE_PIC_VAL, &g_res);

    if (g_res.apic) {
        g_mode = IRQ64_MODE_APIC;
        irq64_table_init(&g_tbl, &ioapic_chip);
        g_tbl.route = apic_route;
        g_tbl.orphan_eoi = orphan_eoi_apic;
        g_tbl.apic_mode = 1;
        g_tbl.dest_apic = g_res.bsp_apic_id;
        klog("IRQ mode: LAPIC + IOAPIC\n");
        if (g_res.enabled_apic_ourselves) klog("irq64: xAPIC was firmware-disabled; enabled by ToxenOS\n");
        if (g_res.madt_lapic_mismatch) klog("irq64: MADT LAPIC address differs from IA32_APIC_BASE (MSR used)\n");
    } else {
        g_mode = IRQ64_MODE_PIC;
        irq64_table_init(&g_tbl, &pic_chip64);
        g_tbl.route = pic_route;
        g_tbl.orphan_eoi = 0;
        if (virtual_wire_allowed()) lapic64_virtual_wire_init();
        klog("IRQ mode: PIC\n");
        klog("irq64: PIC reason: "); klog(irqmode64_reason_str(g_res.reason)); klog("\n");
        if (g_res.rolled_back) klog("irq64: APIC transaction rolled back to snapshot\n");
    }
}

irq64_mode_t irq64_mode(void) { return g_mode; }
const char* irq64_mode_name(void) { return g_mode == IRQ64_MODE_APIC ? "LAPIC + IOAPIC" : "PIC"; }

int irq64_request_legacy(uint8_t isa_irq, irq64_handler_t h, void* ctx, const char* name) {
    uint64_t f;
    __asm__ volatile ("pushfq; pop %0; cli" : "=r"(f) :: "memory");
    int r = irq64_table_request_legacy(g_cur, isa_irq, h, ctx, name);
    if (f & 0x200) __asm__ volatile ("sti" ::: "memory");
    if (r != 0) klog_hex("irq64: request_legacy failed, irq=", isa_irq);
    return r;
}

int irq64_free_legacy(uint8_t isa_irq) {
    return irq64_table_free_legacy(g_cur, isa_irq);
}

const irq64_desc_t* irq64_get_desc(int irq) {
    return (irq >= 0 && irq < IRQ64_MAX) ? &g_cur->desc[irq] : 0;
}

// ── diagnostics ──────────────────────────────────────────────────────
static char* put_str(char* p, const char* s) { while (*s) *p++ = *s++; return p; }
static char* put_dec(char* p, uint32_t v) {
    char t[12]; int n = 0;
    if (!v) t[n++] = '0';
    while (v) { t[n++] = (char)('0' + v % 10); v /= 10; }
    while (n) *p++ = t[--n];
    return p;
}
static char* put_hex(char* p, uint32_t v, int digits) {
    const char* h = "0123456789abcdef";
    for (int i = digits - 1; i >= 0; i--) *p++ = h[(v >> (i * 4)) & 0xF];
    return p;
}
static char* put_bdf(char* p, uint16_t bdf) {
    p = put_hex(p, bdf >> 8, 2); *p++ = ':'; p = put_hex(p, (bdf >> 3) & 0x1F, 2); *p++ = '.';
    return put_hex(p, bdf & 7, 1);
}
static const char* src_name(uint8_t s) {
    return s == IRQ64_SRC_PIC ? "PIC" : s == IRQ64_SRC_IOAPIC ? "IOAPIC" :
           s == IRQ64_SRC_MSI ? "MSI" : s == IRQ64_SRC_MSIX ? "MSIX" : "?";
}

void irq64_dump(void) {
    klog("irq64: counters ("); klog(irq64_mode_name()); klog(")\n");
    for (int i = 0; i < IRQ64_MAX; i++) {
        const irq64_desc_t* d = &g_cur->desc[i];
        if (!d->requested && d->count == 0 && !d->allocated) continue;
        char line[160]; char* p = line;
        p = put_str(p, "  irq "); p = put_dec(p, (uint32_t)i);
        p = put_str(p, "  src="); p = put_str(p, src_name(d->src));
        if (d->src == IRQ64_SRC_MSI || d->src == IRQ64_SRC_MSIX) {
            // The CPU vector is exposed here for diagnostics ONLY.
            p = put_str(p, "  bdf="); p = put_bdf(p, d->bdf);
            p = put_str(p, "  devvec="); p = put_dec(p, d->dev_index);
            p = put_str(p, "  vec=0x"); p = put_hex(p, (uint32_t)(d->vector & 0xFF), 2);
            p = put_str(p, "  dest="); p = put_dec(p, d->dest_apic);
            p = put_str(p, d->sealed ? "  sealed" : "");
        } else {
            p = put_str(p, "  "); p = put_str(p, d->name ? d->name : "?");
        }
        p = put_str(p, "  en="); p = put_dec(p, (d->requested && !d->masked) ? 1u : 0u);
        p = put_str(p, "  count="); p = put_dec(p, d->count);
        p = put_str(p, "  handled="); p = put_dec(p, d->handled);
        p = put_str(p, "  unhandled="); p = put_dec(p, d->unhandled);
        p = put_str(p, "  eoi="); p = put_dec(p, d->eoi_count);
        *p++ = '\n'; *p = 0;
        klog(line);
    }
    klog_hex("  orphan vectors=", g_cur->orphan_vectors);
    klog_hex("  lapic eoi=", (uint32_t)lapic64_eoi_count);
    klog_hex("  lapic spurious(0xFF)=", (uint32_t)lapic64_spurious_count);
}

irq64_table_t* irq64_cur_table(void) { return g_cur; }
irq64_table_t* irq64_test_swap_table(irq64_table_t* t) { irq64_table_t* o = g_cur; g_cur = t ? t : &g_tbl; return o; }
int irq64_enable(int irq)  { return irq64_table_ctl(g_cur, irq, 1); }
int irq64_disable(int irq) { return irq64_table_ctl(g_cur, irq, 0); }

// ── reschedule on IRQ exit (M+11C) ───────────────────────────────────
static volatile int g_need_resched = 0;
static volatile int g_in_irq = 0;
static uint32_t g_resched_switches = 0;

void irq64_request_reschedule(void) { g_need_resched = 1; }
int  irq64_need_resched(void) { return g_need_resched; }
int  irq64_in_irq(void) { return g_in_irq; }
// The interrupt-nesting depth belongs to the CONTEXT: when the scheduler switches away from inside
// an interrupt handler (the timer path does), the incoming context is NOT in interrupt context.
// perform_switch() saves+clears it around the stack switch and restores it when this context resumes.
int  irq64_irq_ctx_suspend(void) { int v = g_in_irq; g_in_irq = 0; return v; }
void irq64_irq_ctx_resume(int v) { g_in_irq = v; }
uint32_t irq64_resched_switches(void) { return g_resched_switches; }
void irq64_clear_need_resched(void) { g_need_resched = 0; }

int irq64_exit_may_resched(const irq64_exit_ctx_t* c) {
    if (c->switching) return 0;
    if (c->nested) return 0;                  // only the outermost interrupt exit may reschedule
    if (c->preempt_disabled > 0) return 0;
    if (c->from_user) return 1;               // user code is always preemptible
    return !c->has_current;                   // kernel code: only the idle/boot context (no process to protect)
}

int irq64_exit_resched_point(const irq64_exit_ctx_t* c, void (*do_switch)(void)) {
    if (!g_need_resched) return 0;
    if (!irq64_exit_may_resched(c)) return 0;     // illegal here: stays pending for the next safe point
    g_need_resched = 0;
    g_resched_switches++;
    if (do_switch) do_switch();
    return 1;
}

extern int  process64_current_pid(void);
extern int  process64_preempt_disabled(void);
extern void process64_irq_exit_reschedule(void);

// The context the most recent IRQ-exit reschedule decision was made from (self-test observability:
// proves the glue below builds it from the LIVE machine state, not only that the pure predicate is right).
static irq64_exit_ctx_t g_last_exit_ctx;
static uint32_t g_exit_decisions;
void irq64_get_last_exit_ctx(irq64_exit_ctx_t* out, uint32_t* decisions) { *out = g_last_exit_ctx; *decisions = g_exit_decisions; }

// Builds the reschedule-decision context from the LIVE machine state. `tf` is the interrupted frame.
// Called after the handler returned and g_in_irq was decremented: a nonzero depth NOW means this
// interrupt nested inside another one (only the outermost exit may reschedule).
void irq64_exit_build_ctx(const trapframe64_t* tf, irq64_exit_ctx_t* c) {
    c->nested = (g_in_irq != 0);
    c->from_user = ((tf->cs & 3) == 3);
    c->has_current = process64_current_pid() >= 0;
    c->switching = 0;                             // IF=0 across perform_switch: a switch cannot be observed mid-way
    c->preempt_disabled = process64_preempt_disabled();
}

void irq64_dispatch(trapframe64_t* tf) {
    g_in_irq++;
    irq64_table_dispatch(g_cur, (uint8_t)tf->vector);
    g_in_irq--;
    if (g_need_resched) {
        irq64_exit_ctx_t c;
        irq64_exit_build_ctx(tf, &c);
        g_last_exit_ctx = c; g_exit_decisions++;
        irq64_exit_resched_point(&c, process64_irq_exit_reschedule);
    }
}
