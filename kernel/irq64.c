// kernel/irq64.c — Milestone 2 IRQ dispatch, mirrors kernel/irq.c.
#include <stdint.h>
#include "../include/irq64.h"
#include "../include/isr64.h"
#include "../include/pic.h"

static void (*irq64_handlers[16])(void) = { 0 };

void irq64_register(uint8_t irq, void (*handler)(void))
{
    irq64_handlers[irq] = handler;
}

void irq64_dispatch(trapframe64_t* tf)
{
    uint8_t irq = (uint8_t)(tf->vector - 32);
    pic_send_eoi(irq);
    if (irq64_handlers[irq])
        irq64_handlers[irq]();
}
