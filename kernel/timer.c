#include <stdint.h>
#include "../include/timer.h"
#include "../include/irq.h"
#include "../include/process.h"
#include "../include/pic.h"

#define PIT_CHANNEL0    0x40
#define PIT_COMMAND     0x43
#define PIT_BASE_FREQ   1193182  // PIT runs at this frequency in Hz

static volatile int schedule_pending = 0;

static inline void outb(uint16_t port, uint8_t value)
{
    __asm__ volatile ("outb %0, %1" : : "a"(value), "Nd"(port));
}

static uint32_t ticks = 0;

static volatile int in_scheduler = 0;

static void timer_handler()
{
    ticks++;
    if (ticks % 10 == 0)
    {
        pic_send_eoi(0);    // ← send EOI BEFORE switching
        scheduler();
    }
}

int timer_schedule_pending()
{
    if (schedule_pending)
    {
        schedule_pending = 0;
        return 1;
    }
    return 0;
}

void timer_init(uint32_t frequency)
{
    uint32_t divisor = PIT_BASE_FREQ / frequency;

    // channel 0, lobyte/hibyte, rate generator
    outb(PIT_COMMAND, 0x36);

    // send divisor low byte then high byte
    outb(PIT_CHANNEL0, (uint8_t)(divisor & 0xFF));
    outb(PIT_CHANNEL0, (uint8_t)((divisor >> 8) & 0xFF));

    irq_register(0, timer_handler);
}

uint32_t timer_getticks()
{
    return ticks;
}