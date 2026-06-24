// kernel/idt64.c — long-mode IDT bring-up (Milestone 2).
// Mirrors kernel/idt.c's role (build the gate table, lidt) but in the
// real x86_64 16-byte gate format. Exception interpretation/reporting
// lives in kernel/interrupt64.c, not here — this file only builds the table.
#include "../include/idt64.h"
#include "../include/isr64.h"

static idt64_entry_t idt64[256];
static idt_ptr64_t   idtp64;

void idt64_set_gate(int n, uint64_t handler, uint8_t ist, uint8_t type_attr)
{
    idt64[n].offset_low  = (uint16_t)(handler & 0xFFFF);
    idt64[n].selector    = 0x08;              // CODE64_SEL, kernel/boot64.asm
    idt64[n].ist         = ist & 0x7;
    idt64[n].type_attr   = type_attr;
    idt64[n].offset_mid  = (uint16_t)((handler >> 16) & 0xFFFF);
    idt64[n].offset_high = (uint32_t)((handler >> 32) & 0xFFFFFFFFu);
    idt64[n].reserved    = 0;
}

void idt64_init(void)
{
    idtp64.limit = sizeof(idt64) - 1;
    idtp64.base  = (uint64_t)&idt64;

    for (int i = 0; i < 256; i++)
        idt64_set_gate(i, (uint64_t)isr64_default, 0, IDT64_INTERRUPT_GATE_K);

    idt64_set_gate(0,  (uint64_t)isr64_0,  0, IDT64_INTERRUPT_GATE_K);
    idt64_set_gate(1,  (uint64_t)isr64_1,  0, IDT64_INTERRUPT_GATE_K);
    idt64_set_gate(2,  (uint64_t)isr64_2,  0, IDT64_INTERRUPT_GATE_K);
    idt64_set_gate(3,  (uint64_t)isr64_3,  0, IDT64_INTERRUPT_GATE_K);
    idt64_set_gate(4,  (uint64_t)isr64_4,  0, IDT64_INTERRUPT_GATE_K);
    idt64_set_gate(5,  (uint64_t)isr64_5,  0, IDT64_INTERRUPT_GATE_K);
    idt64_set_gate(6,  (uint64_t)isr64_6,  0, IDT64_INTERRUPT_GATE_K);
    idt64_set_gate(7,  (uint64_t)isr64_7,  0, IDT64_INTERRUPT_GATE_K);
    idt64_set_gate(8,  (uint64_t)isr64_8,  0, IDT64_INTERRUPT_GATE_K);
    idt64_set_gate(9,  (uint64_t)isr64_9,  0, IDT64_INTERRUPT_GATE_K);
    idt64_set_gate(10, (uint64_t)isr64_10, 0, IDT64_INTERRUPT_GATE_K);
    idt64_set_gate(11, (uint64_t)isr64_11, 0, IDT64_INTERRUPT_GATE_K);
    idt64_set_gate(12, (uint64_t)isr64_12, 0, IDT64_INTERRUPT_GATE_K);
    idt64_set_gate(13, (uint64_t)isr64_13, 0, IDT64_INTERRUPT_GATE_K);
    idt64_set_gate(14, (uint64_t)isr64_14, 0, IDT64_INTERRUPT_GATE_K);
    idt64_set_gate(15, (uint64_t)isr64_15, 0, IDT64_INTERRUPT_GATE_K);
    idt64_set_gate(16, (uint64_t)isr64_16, 0, IDT64_INTERRUPT_GATE_K);
    idt64_set_gate(17, (uint64_t)isr64_17, 0, IDT64_INTERRUPT_GATE_K);
    idt64_set_gate(18, (uint64_t)isr64_18, 0, IDT64_INTERRUPT_GATE_K);
    idt64_set_gate(19, (uint64_t)isr64_19, 0, IDT64_INTERRUPT_GATE_K);
    idt64_set_gate(20, (uint64_t)isr64_20, 0, IDT64_INTERRUPT_GATE_K);
    idt64_set_gate(21, (uint64_t)isr64_21, 0, IDT64_INTERRUPT_GATE_K);
    idt64_set_gate(22, (uint64_t)isr64_22, 0, IDT64_INTERRUPT_GATE_K);
    idt64_set_gate(23, (uint64_t)isr64_23, 0, IDT64_INTERRUPT_GATE_K);
    idt64_set_gate(24, (uint64_t)isr64_24, 0, IDT64_INTERRUPT_GATE_K);
    idt64_set_gate(25, (uint64_t)isr64_25, 0, IDT64_INTERRUPT_GATE_K);
    idt64_set_gate(26, (uint64_t)isr64_26, 0, IDT64_INTERRUPT_GATE_K);
    idt64_set_gate(27, (uint64_t)isr64_27, 0, IDT64_INTERRUPT_GATE_K);
    idt64_set_gate(28, (uint64_t)isr64_28, 0, IDT64_INTERRUPT_GATE_K);
    idt64_set_gate(29, (uint64_t)isr64_29, 0, IDT64_INTERRUPT_GATE_K);
    idt64_set_gate(30, (uint64_t)isr64_30, 0, IDT64_INTERRUPT_GATE_K);
    idt64_set_gate(31, (uint64_t)isr64_31, 0, IDT64_INTERRUPT_GATE_K);

    idt64_set_gate(32, (uint64_t)irq64_0,  0, IDT64_INTERRUPT_GATE_K);
    idt64_set_gate(33, (uint64_t)irq64_1,  0, IDT64_INTERRUPT_GATE_K);
    idt64_set_gate(34, (uint64_t)irq64_2,  0, IDT64_INTERRUPT_GATE_K);
    idt64_set_gate(35, (uint64_t)irq64_3,  0, IDT64_INTERRUPT_GATE_K);
    idt64_set_gate(36, (uint64_t)irq64_4,  0, IDT64_INTERRUPT_GATE_K);
    idt64_set_gate(37, (uint64_t)irq64_5,  0, IDT64_INTERRUPT_GATE_K);
    idt64_set_gate(38, (uint64_t)irq64_6,  0, IDT64_INTERRUPT_GATE_K);
    idt64_set_gate(39, (uint64_t)irq64_7,  0, IDT64_INTERRUPT_GATE_K);
    idt64_set_gate(40, (uint64_t)irq64_8,  0, IDT64_INTERRUPT_GATE_K);
    idt64_set_gate(41, (uint64_t)irq64_9,  0, IDT64_INTERRUPT_GATE_K);
    idt64_set_gate(42, (uint64_t)irq64_10, 0, IDT64_INTERRUPT_GATE_K);
    idt64_set_gate(43, (uint64_t)irq64_11, 0, IDT64_INTERRUPT_GATE_K);
    idt64_set_gate(44, (uint64_t)irq64_12, 0, IDT64_INTERRUPT_GATE_K);
    idt64_set_gate(45, (uint64_t)irq64_13, 0, IDT64_INTERRUPT_GATE_K);
    idt64_set_gate(46, (uint64_t)irq64_14, 0, IDT64_INTERRUPT_GATE_K);
    idt64_set_gate(47, (uint64_t)irq64_15, 0, IDT64_INTERRUPT_GATE_K);

    __asm__ volatile ("lidt %0" : : "m"(idtp64));
}
