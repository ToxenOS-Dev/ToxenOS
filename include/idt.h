#ifndef IDT_H
#define IDT_H

void idt_init();
void exception_handler(int interrupt);

#endif