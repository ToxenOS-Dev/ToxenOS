#ifndef IRQ_H
#define IRQ_H

#include <stdint.h>

void irq_register(uint8_t irq, void (*handler)(void));
void irq_handler(int interrupt);

#endif