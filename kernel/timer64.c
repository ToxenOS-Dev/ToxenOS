// kernel/timer64.c — Milestone 2 minimal IRQ0 handler, now (Milestone 3)
// also driving the kernel-task scheduler. Nothing reprograms the PIT
// divisor, so this free-runs at the legacy default (~18.2Hz). No
// fbterm_tick/usb_hid_poll/net_poll calls — none of those exist/apply
// at this milestone.
#include <stdint.h>
#include "../include/klog.h"
#include "../include/process64.h"

static volatile uint64_t ticks64 = 0;

void timer64_handler(void)
{
    ticks64++;
    if (ticks64 % 100 == 0)
        klog("[timer64] heartbeat\n");
    if (ticks64 % SCHED64_TICK_DIVISOR == 0)
        scheduler64_tick();
}
