// kernel/irqmode64.c -- M+11A PIC -> LAPIC+IOAPIC transactional handover.
// See include/irqmode64.h for the design and architectural notes.
#include "../include/irqmode64.h"
#include "../include/lapic64.h"
#include "../include/ioapic64.h"
#include "../include/pic.h"
#include "../include/klog.h"

#define SVR_ENABLE       0x100u
#define SVR_SPURIOUS_VEC 0xFFu
#define LVT_DELIV_NMI    0x400u
#define LVT_DELIV_EXTINT 0x700u
#define LVT_POL_LOW      (1u << 13)
#define LVT_TRIG_LEVEL   (1u << 15)

const char* irqmode64_reason_str(irqmode_reason_t r) {
    switch (r) {
    case IRQMODE_REASON_OK:                 return "ok";
    case IRQMODE_REASON_FORCED_PIC:         return "forced PIC (compile-time)";
    case IRQMODE_REASON_NO_MADT:            return "no valid MADT";
    case IRQMODE_REASON_NO_IOAPIC:          return "MADT lists no IOAPIC";
    case IRQMODE_REASON_CPU_NO_APIC:        return "CPU has no APIC";
    case IRQMODE_REASON_X2APIC_ACTIVE:      return "x2APIC already active (not switching back to xAPIC)";
    case IRQMODE_REASON_ENABLE_REFUSED:     return "APIC globally disabled and could not be enabled";
    case IRQMODE_REASON_LAPIC_MAP_FAILED:   return "LAPIC MMIO map failed";
    case IRQMODE_REASON_BSP_NOT_IN_MADT:    return "BSP APIC ID not in MADT";
    case IRQMODE_REASON_VERIFY_FAILED:      return "LAPIC readback verify failed";
    case IRQMODE_REASON_IOAPIC_INIT_FAILED: return "IOAPIC init failed";
    }
    return "?";
}

// Snapshot of every piece of state the transaction modifies.
typedef struct {
    uint8_t  imr_master, imr_slave;
    int      have_msr;
    uint64_t apic_base;
    int      have_lapic;
    uint32_t svr, tpr, lint0, lint1, lvt_timer, lvt_thermal, lvt_perf, lvt_error;
} irqmode_snapshot_t;

static void snap_lapic(const irqmode_hw_t* hw, irqmode_snapshot_t* s) {
    s->svr         = hw->lapic_read(LAPIC_REG_SVR);
    s->tpr         = hw->lapic_read(LAPIC_REG_TPR);
    s->lint0       = hw->lapic_read(LAPIC_REG_LINT0);
    s->lint1       = hw->lapic_read(LAPIC_REG_LINT1);
    s->lvt_timer   = hw->lapic_read(LAPIC_REG_LVT_TIMER);
    s->lvt_thermal = hw->lapic_read(LAPIC_REG_LVT_THERMAL);
    s->lvt_perf    = hw->lapic_read(LAPIC_REG_LVT_PERF);
    s->lvt_error   = hw->lapic_read(LAPIC_REG_LVT_ERROR);
    s->have_lapic  = 1;
}

// Restores actual previous values. LAPIC registers first (the APIC must
// still be enabled to accept the writes), then the MSR, then the PICs.
static void rollback(const irqmode_hw_t* hw, const irqmode_snapshot_t* s) {
    if (s->have_lapic) {
        hw->lapic_write(LAPIC_REG_LVT_TIMER,   s->lvt_timer);
        hw->lapic_write(LAPIC_REG_LVT_THERMAL, s->lvt_thermal);
        hw->lapic_write(LAPIC_REG_LVT_PERF,    s->lvt_perf);
        hw->lapic_write(LAPIC_REG_LVT_ERROR,   s->lvt_error);
        hw->lapic_write(LAPIC_REG_LINT0,       s->lint0);
        hw->lapic_write(LAPIC_REG_LINT1,       s->lint1);
        hw->lapic_write(LAPIC_REG_TPR,         s->tpr);
        hw->lapic_write(LAPIC_REG_SVR,         s->svr);
    }
    if (s->have_msr) hw->wrmsr(MSR_IA32_APIC_BASE, s->apic_base);
    hw->pic_set_mask(0, s->imr_master);
    hw->pic_set_mask(1, s->imr_slave);
}

static uint32_t nmi_lvt(uint16_t flags) {
    int pol = 0, trg = 0;
    madt64_decode_flags(flags, &pol, &trg);
    return LVT_DELIV_NMI | (pol ? LVT_POL_LOW : 0) | (trg ? LVT_TRIG_LEVEL : 0);
}

static void fail(irqmode_result_t* out, irqmode_reason_t r) { out->apic = 0; out->reason = r; }

void irqmode64_commit(const irqmode_hw_t* hw, const madt64_info_t* madt,
                      int force_pic, irqmode_result_t* out) {
    out->apic = 0; out->reason = IRQMODE_REASON_OK;
    out->enabled_apic_ourselves = 0; out->rolled_back = 0;
    out->lapic_phys = 0; out->bsp_apic_id = 0; out->madt_lapic_mismatch = 0;

    if (force_pic)                     { fail(out, IRQMODE_REASON_FORCED_PIC); return; }
    if (!madt)                         { fail(out, IRQMODE_REASON_NO_MADT); return; }
    if (madt->n_ioapic <= 0)           { fail(out, IRQMODE_REASON_NO_IOAPIC); return; }

    // ── CPU capability. No hardware state is modified until the snapshot. ──
    // A clear CPUID APIC flag is ambiguous (no APIC vs firmware-disabled),
    // so only conclude "no APIC" if the MSR cannot be consulted.
    if (!hw->cpuid_has_apic() && !hw->msr_supported()) { fail(out, IRQMODE_REASON_CPU_NO_APIC); return; }

    irqmode_snapshot_t s = {0};
    s.imr_master = hw->pic_get_mask(0);
    s.imr_slave  = hw->pic_get_mask(1);
    s.apic_base  = hw->rdmsr(MSR_IA32_APIC_BASE);
    s.have_msr   = 1;

    if (s.apic_base & APIC_BASE_X2APIC_EN) { fail(out, IRQMODE_REASON_X2APIC_ACTIVE); return; }

    uint64_t base = s.apic_base;
    if (!(base & APIC_BASE_GLOBAL_EN)) {
        // Firmware left the xAPIC globally disabled. Enabling it is the
        // architected way to bring it back on P6+/K8+ (xAPIC mode, ID/
        // registers reset to power-on defaults); confirm it took.
        if (!(base & APIC_BASE_ADDR_MASK)) base |= 0xFEE00000ull;
        hw->wrmsr(MSR_IA32_APIC_BASE, base | APIC_BASE_GLOBAL_EN);
        uint64_t rb = hw->rdmsr(MSR_IA32_APIC_BASE);
        if (!(rb & APIC_BASE_GLOBAL_EN) || (rb & APIC_BASE_X2APIC_EN) || !hw->cpuid_has_apic()) {
            hw->wrmsr(MSR_IA32_APIC_BASE, s.apic_base);
            out->rolled_back = 1;
            fail(out, IRQMODE_REASON_ENABLE_REFUSED);
            return;
        }
        out->enabled_apic_ourselves = 1;
    }

    uint64_t phys = hw->rdmsr(MSR_IA32_APIC_BASE) & APIC_BASE_ADDR_MASK;
    out->lapic_phys = phys;
    if (phys != madt->lapic_phys) out->madt_lapic_mismatch = 1;   // MSR is authoritative

    if (hw->lapic_map(phys) != 0) {
        rollback(hw, &s); out->rolled_back = 1;
        fail(out, IRQMODE_REASON_LAPIC_MAP_FAILED); return;
    }

    // LAPIC registers are accessible now: snapshot them BEFORE any write.
    snap_lapic(hw, &s);

    uint32_t bsp_id = hw->lapic_read(LAPIC_REG_ID) >> 24;
    out->bsp_apic_id = bsp_id;
    const madt_lapic_t* me = madt64_find_lapic(madt, bsp_id);
    if (!me) {
        rollback(hw, &s); out->rolled_back = 1;
        fail(out, IRQMODE_REASON_BSP_NOT_IN_MADT); return;
    }

    // ── Modify. All IOAPIC routes are still masked (not yet touched). ────
    hw->pic_set_mask(0, 0xFF);
    hw->pic_set_mask(1, 0xFF);

    uint32_t lint0 = LVT_DELIV_EXTINT | LAPIC_LVT_MASKED;   // ExtINT stays masked in APIC mode
    uint32_t lint1 = LVT_DELIV_NMI;
    for (int i = 0; i < madt->n_nmi; i++) {
        const madt_nmi_t* n = &madt->nmi[i];
        if (n->acpi_id != 0xFF && n->acpi_id != me->acpi_id) continue;
        if (n->lint == 0)      { lint0 = nmi_lvt(n->flags); lint1 = LVT_DELIV_NMI | LAPIC_LVT_MASKED; }
        else if (n->lint == 1) { lint1 = nmi_lvt(n->flags); }
    }
    hw->lapic_write(LAPIC_REG_LINT0, lint0);
    hw->lapic_write(LAPIC_REG_LINT1, lint1);
    hw->lapic_write(LAPIC_REG_LVT_TIMER,   hw->lapic_read(LAPIC_REG_LVT_TIMER)   | LAPIC_LVT_MASKED);
    hw->lapic_write(LAPIC_REG_LVT_THERMAL, hw->lapic_read(LAPIC_REG_LVT_THERMAL) | LAPIC_LVT_MASKED);
    hw->lapic_write(LAPIC_REG_LVT_PERF,    hw->lapic_read(LAPIC_REG_LVT_PERF)    | LAPIC_LVT_MASKED);
    hw->lapic_write(LAPIC_REG_LVT_ERROR,   hw->lapic_read(LAPIC_REG_LVT_ERROR)   | LAPIC_LVT_MASKED);
    hw->lapic_write(LAPIC_REG_TPR, 0);
    hw->lapic_write(LAPIC_REG_SVR, SVR_ENABLE | SVR_SPURIOUS_VEC);

    // ── Verify by readback. ───────────────────────────────────────────────
    uint32_t svr = hw->lapic_read(LAPIC_REG_SVR);
    if ((svr & SVR_ENABLE) == 0 || (svr & 0xFF) != SVR_SPURIOUS_VEC ||
        (lint0 & LAPIC_LVT_MASKED && !(hw->lapic_read(LAPIC_REG_LINT0) & LAPIC_LVT_MASKED))) {
        rollback(hw, &s); out->rolled_back = 1;
        fail(out, IRQMODE_REASON_VERIFY_FAILED); return;
    }

    // ── IOAPICs: map, probe, mask everything. ────────────────────────────
    if (hw->ioapic_init_masked(madt) != 0) {
        rollback(hw, &s); out->rolled_back = 1;
        fail(out, IRQMODE_REASON_IOAPIC_INIT_FAILED); return;
    }

    out->apic = 1;
    out->reason = IRQMODE_REASON_OK;
}

// ── real hardware ops ────────────────────────────────────────────────
static int real_cpuid_has_apic(void) {
    uint32_t a = 1, b, c, d;
    __asm__ volatile ("cpuid" : "+a"(a), "=b"(b), "=c"(c), "=d"(d));
    return (d >> 9) & 1;
}
static int real_msr_supported(void) {
    uint32_t a = 1, b, c, d;
    __asm__ volatile ("cpuid" : "+a"(a), "=b"(b), "=c"(c), "=d"(d));
    uint32_t family = (a >> 8) & 0xF;
    if (family == 0xF) family += (a >> 20) & 0xFF;
    return ((d >> 5) & 1) && family >= 6;
}
static uint64_t real_rdmsr(uint32_t msr) {
    uint32_t lo, hi;
    __asm__ volatile ("rdmsr" : "=a"(lo), "=d"(hi) : "c"(msr));
    return ((uint64_t)hi << 32) | lo;
}
static void real_wrmsr(uint32_t msr, uint64_t v) {
    __asm__ volatile ("wrmsr" :: "c"(msr), "a"((uint32_t)v), "d"((uint32_t)(v >> 32)));
}
static uint8_t real_pic_get(int slave) { return pic_get_mask(slave); }
static void    real_pic_set(int slave, uint8_t m) { pic_set_mask(slave, m); }

const irqmode_hw_t irqmode64_real_hw = {
    real_cpuid_has_apic, real_msr_supported, real_rdmsr, real_wrmsr,
    lapic64_map, lapic64_read, lapic64_write,
    real_pic_get, real_pic_set, ioapic64_init_all_masked,
};
