#include <stdint.h>
#include "../include/pic.h"
#include "../include/irq.h"

static void (*irq_handlers[16])(void) = { 0 };

void irq_register(uint8_t irq, void (*handler)(void))
{
    irq_handlers[irq] = handler;
}

void irq_handler(int interrupt)
{
    uint8_t irq = interrupt - 32;

    pic_send_eoi(irq);      // ← send EOI first, before handler

    if (irq_handlers[irq])
        irq_handlers[irq]();
}