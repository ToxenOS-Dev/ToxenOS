#ifndef IDT_H
#define IDT_H

void idt_init();
void exception_handler(int interrupt);
void idt_set_gate(int n, uint32_t handler);
void idt_set_gate_user(int n, uint32_t handler);

#endif