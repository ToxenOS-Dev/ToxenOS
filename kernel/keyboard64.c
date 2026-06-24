// kernel/keyboard64.c — Milestone 2 minimal IRQ1 handler.
// No shift/ctrl tracking, no scancode-to-ASCII table, no input buffer,
// no process coupling (kernel/keyboard.c's full version is none of those
// things at this milestone — there's no scheduler/processes yet). Just
// reads the raw scancode and prints it.
#include <stdint.h>
#include "../include/klog.h"

static inline uint8_t inb(uint16_t port)
{
    uint8_t ret;
    __asm__ volatile ("inb %1, %0" : "=a"(ret) : "Nd"(port));
    return ret;
}

static void hex8_to_str(uint8_t val, char* out)
{
    const char* h = "0123456789ABCDEF";
    out[0] = '0'; out[1] = 'x';
    out[2] = h[(val >> 4) & 0xF];
    out[3] = h[val & 0xF];
    out[4] = 0;
}

void keyboard64_handler(void)
{
    uint8_t sc = inb(0x60);
    char hex[5];
    hex8_to_str(sc, hex);
    klog("[keyboard64] scancode ");
    klog(hex);
    klog("\n");
}
