#ifndef IRQMODE64_H
#define IRQMODE64_H

// M+11A: PIC -> LAPIC+IOAPIC handover, as one all-or-nothing transaction.
//
// Every hardware access goes through irqmode_hw_t so the boot self-tests
// can drive the identical decision/commit/rollback code against a fake
// CPU (MSRs, LAPIC registers, 8259 IMRs) with injected failures.
//
// The transaction runs with IF=0. It snapshots EVERYTHING it will modify
// (both 8259 IMRs, IA32_APIC_BASE, and the LAPIC SVR/TPR/LINT0/LINT1/
// Timer/Thermal/PerfMon/Error registers) before the first write, and on
// any failure restores those actual previous values -- not assumed
// defaults. IOAPIC routes stay masked for the whole transaction; a
// rolled-back IOAPIC is left masked (safe: PIC mode never uses it), so
// it is intentionally not part of the snapshot.
//
// Architectural notes (Intel SDM Vol.3 10.4.3/10.12, AMD APM Vol.2):
//  * CPUID.01H:EDX[9] (APIC) reads 0 both on a CPU with no APIC AND on
//    one whose IA32_APIC_BASE[11] (global enable) firmware cleared. These
//    are different situations. If CPUID says no APIC but the CPU is
//    family>=6 with MSR support, we read the MSR: bit11==0 means "present
//    but disabled", and we attempt xAPIC enable ourselves.
//  * IA32_APIC_BASE[10] (x2APIC enable) already set: NOT switched back to
//    xAPIC here (that requires disabling the APIC first, and firmware/
//    other agents may depend on x2APIC mode). Policy: fall back to PIC.
//  * The MSR base address is authoritative over the MADT's LAPIC address.
#include <stdint.h>
#include "madt64.h"

#define MSR_IA32_APIC_BASE   0x1Bu
#define APIC_BASE_BSP        (1ull << 8)
#define APIC_BASE_X2APIC_EN  (1ull << 10)
#define APIC_BASE_GLOBAL_EN  (1ull << 11)
#define APIC_BASE_ADDR_MASK  0x000FFFFFFFFFF000ull

typedef enum {
    IRQMODE_REASON_OK = 0,
    IRQMODE_REASON_FORCED_PIC,
    IRQMODE_REASON_NO_MADT,
    IRQMODE_REASON_NO_IOAPIC,
    IRQMODE_REASON_CPU_NO_APIC,        // genuinely no APIC in this CPU
    IRQMODE_REASON_X2APIC_ACTIVE,
    IRQMODE_REASON_ENABLE_REFUSED,     // firmware-disabled APIC that would not enable
    IRQMODE_REASON_LAPIC_MAP_FAILED,
    IRQMODE_REASON_BSP_NOT_IN_MADT,
    IRQMODE_REASON_VERIFY_FAILED,
    IRQMODE_REASON_IOAPIC_INIT_FAILED,
} irqmode_reason_t;

typedef struct {
    int      (*cpuid_has_apic)(void);
    int      (*msr_supported)(void);      // CPUID MSR bit && family >= 6
    uint64_t (*rdmsr)(uint32_t msr);
    void     (*wrmsr)(uint32_t msr, uint64_t val);
    int      (*lapic_map)(uint64_t phys); // 0 = ok
    uint32_t (*lapic_read)(uint32_t reg);
    void     (*lapic_write)(uint32_t reg, uint32_t val);
    uint8_t  (*pic_get_mask)(int slave);
    void     (*pic_set_mask)(int slave, uint8_t mask);
    int      (*ioapic_init_masked)(const madt64_info_t* m);  // 0 = ok
} irqmode_hw_t;

typedef struct {
    int              apic;            // 1 = LAPIC+IOAPIC committed
    irqmode_reason_t reason;
    int              enabled_apic_ourselves;  // we set IA32_APIC_BASE[11]
    int              rolled_back;     // a transaction started and was undone
    uint64_t         lapic_phys;
    uint32_t         bsp_apic_id;
    int              madt_lapic_mismatch;     // MSR base != MADT address (MSR won)
} irqmode_result_t;

// Decision + transaction. `madt` may be NULL (no/invalid MADT). Caller
// guarantees IF=0. Never leaves the machine half-converted.
void irqmode64_commit(const irqmode_hw_t* hw, const madt64_info_t* madt,
                      int force_pic, irqmode_result_t* out);

extern const irqmode_hw_t irqmode64_real_hw;
const char* irqmode64_reason_str(irqmode_reason_t r);

#endif // IRQMODE64_H
