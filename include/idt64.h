#ifndef IDT64_H
#define IDT64_H

#include <stdint.h>

// x86_64 long-mode IDT gate descriptor — 16 bytes. The 32-bit format's
// single "zero" byte becomes `ist` (low 3 bits select an Interrupt Stack
// Table entry; 0 means "don't switch stacks" — milestone 2 has no TSS/IST
// yet, so every gate uses 0), and offset_high grows from 16 to 32 bits
// plus a 32-bit reserved field, to address a full 64-bit handler.
typedef struct {
    uint16_t offset_low;
    uint16_t selector;     // 0x08 = CODE64_SEL (kernel/boot64.asm)
    uint8_t  ist;
    uint8_t  type_attr;
    uint16_t offset_mid;
    uint32_t offset_high;
    uint32_t reserved;
} __attribute__((packed)) idt64_entry_t;

typedef struct {
    uint16_t limit;
    uint64_t base;
} __attribute__((packed)) idt_ptr64_t;

#define IDT64_INTERRUPT_GATE_K 0x8E  // present, ring0, 64-bit interrupt gate
#define IDT64_INTERRUPT_GATE_U 0xEE  // same, but DPL=3 -- callable from ring3 (int 0x80)

void idt64_set_gate(int n, uint64_t handler, uint8_t ist, uint8_t type_attr);
void idt64_init(void);

#endif // IDT64_H
