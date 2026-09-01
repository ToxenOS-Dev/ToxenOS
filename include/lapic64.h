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

#endif // LAPIC64_H
