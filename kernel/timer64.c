// kernel/timer64.c — Milestone 2 minimal IRQ0 handler; Milestone 24 now
// reprograms the PIT to a real 100Hz instead of free-running at the
// legacy default (~18.2Hz), and drives the real process scheduler
// (kernel/process64.c) instead of the old ring-0 task demo. No
// fbterm_tick/usb_hid_poll/net_poll calls — none of those exist/apply
// at this milestone.
#include <stdint.h>
#include "../include/klog.h"
#include "../include/process64.h"

#define PIT_CHANNEL0  0x40
#define PIT_COMMAND   0x43
#define PIT_BASE_FREQ 1193182

static inline void outb(uint16_t port, uint8_t value) {
    __asm__ volatile ("outb %0, %1" : : "a"(value), "Nd"(port));
}

static volatile uint64_t ticks64 = 0;

// Milestone 24: programs PIT channel 0 for mode 3 (square wave) at the
// given frequency -- the 64-bit analogue of the 32-bit kernel/timer.c's
// timer_init(). Must run before `sti` (kernel/kernel64.c).
void timer64_init(uint32_t frequency) {
    uint32_t divisor = PIT_BASE_FREQ / frequency;
    outb(PIT_COMMAND, 0x36);
    outb(PIT_CHANNEL0, (uint8_t)(divisor & 0xFF));
    outb(PIT_CHANNEL0, (uint8_t)((divisor >> 8) & 0xFF));
}

void timer64_handler(void)
{
    ticks64++;
    if (ticks64 % 100 == 0)
        klog("[timer64] heartbeat\n");
    if (ticks64 % PROCESS64_TICK_DIVISOR == 0)
        process64_tick();
}
