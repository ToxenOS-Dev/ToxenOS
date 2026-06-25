// kernel/keyboard64.c — IRQ1 handler.
// Milestone 2: just read the raw scancode and log its hex value (no
// scancode-to-ASCII table, no input buffer, no process coupling -- there
// was no scheduler/processes yet).
// Milestone 10: forwards every scancode into kernel/keyboard_buffer64.c,
// which does the actual translation/buffering -- this file is now just
// "read port 0x60, hand it off."
#include <stdint.h>
#include "../include/keyboard_buffer64.h"

static inline uint8_t inb(uint16_t port)
{
    uint8_t ret;
    __asm__ volatile ("inb %1, %0" : "=a"(ret) : "Nd"(port));
    return ret;
}

void keyboard64_handler(void)
{
    uint8_t sc = inb(0x60);
    keyboard_buffer64_on_scancode(sc);
}
