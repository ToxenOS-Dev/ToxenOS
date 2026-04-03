#include <stdint.h>
#include "../include/timer.h"
#include "../include/irq.h"
#include "../include/process.h"
#include "../include/pic.h"
#include "../include/fbterm.h"
#include "../include/net.h"

#define PIT_CHANNEL0    0x40
#define PIT_COMMAND     0x43
#define PIT_BASE_FREQ   1193182

static inline void outb(uint16_t port, uint8_t value)
{
    __asm__ volatile ("outb %0, %1" : : "a"(value), "Nd"(port));
}

static volatile uint32_t ticks = 0;

static void timer_handler()
{
    ticks++;
    fbterm_tick();

    if (ticks % 10 == 0) {
        net_poll();
        scheduler();
    }

    // EOI is sent by irq_handler after this returns
}

int timer_schedule_pending() { return 0; }

void timer_init(uint32_t frequency)
{
    uint32_t divisor = PIT_BASE_FREQ / frequency;
    outb(PIT_COMMAND, 0x36);
    outb(PIT_CHANNEL0, (uint8_t)(divisor & 0xFF));
    outb(PIT_CHANNEL0, (uint8_t)((divisor >> 8) & 0xFF));
    irq_register(0, timer_handler);
}

uint32_t timer_getticks()
{
    return ticks;
}
