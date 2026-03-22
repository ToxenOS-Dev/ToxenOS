#ifndef IDT_H
#define IDT_H

#include <stdint.h>

void idt_init();
void idt_set_gate(int n, uint32_t handler);
void idt_set_gate_user(int n, uint32_t handler);
void exception_handler(int interrupt, uint32_t error_code);
void page_fault_handler(uint32_t error_code, uint32_t cr2);

#endif
