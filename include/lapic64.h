#ifndef LAPIC64_H
#define LAPIC64_H

// Milestone 24: 64-bit port of kernel/kernel.c's lapic_virtual_wire_init
// (32-bit). On real UEFI/APIC hardware the 8259 PIC is often fully
// masked by firmware and LAPIC LINT0 may not be in ExtINT mode -- without
// this, no legacy PIC interrupt (timer IRQ0, keyboard IRQ1) ever reaches
// the CPU after `sti`, even though QEMU's default machine leaves PIC
// routing intact and never exposes the gap. Must run before pic_remap()
// and before `sti` (kernel/kernel64.c), same ordering as the 32-bit
// kernel.
void lapic64_virtual_wire_init(void);


// M+11A: register access + EOI for APIC-mode interrupt delivery.
#include <stdint.h>
#define LAPIC_REG_ID      0x020
#define LAPIC_REG_TPR     0x080
#define LAPIC_REG_EOI     0x0B0
#define LAPIC_REG_SVR     0x0F0
#define LAPIC_REG_LVT_TIMER   0x320
#define LAPIC_REG_LVT_THERMAL 0x330
#define LAPIC_REG_LVT_PERF    0x340
#define LAPIC_REG_LINT0   0x350
#define LAPIC_REG_LINT1   0x360
#define LAPIC_REG_LVT_ERROR   0x370
#define LAPIC_LVT_MASKED  0x10000u

int      lapic64_map(uint64_t phys);          // 0 on success
uint32_t lapic64_read(uint32_t reg);
void     lapic64_write(uint32_t reg, uint32_t val);
uint32_t lapic64_id(void);                    // xAPIC ID (bits 31:24 of the ID reg)
// Writes 0 to the EOI register. NEVER call this for the 0xFF spurious
// vector (no ISR bit is set for it -- an EOI would retire a real,
// unrelated in-service interrupt).
void     lapic64_eoi(void);
extern volatile uint64_t lapic64_eoi_count;       // stats/self-test
extern volatile uint64_t lapic64_spurious_count;  // bumped by isr64.asm's 0xFF stub

#endif // LAPIC64_H
