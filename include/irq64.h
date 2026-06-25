#ifndef IRQ64_H
#define IRQ64_H

#include <stdint.h>

// IRQ handlers stay parameterless (matching the 32-bit irq_register()
// signature) — they don't need the trap frame, only exception handlers do.
void irq64_register(uint8_t irq, void (*handler)(void));

#endif // IRQ64_H
