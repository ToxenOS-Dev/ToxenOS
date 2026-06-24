// kernel/timer64.c — Milestone 2 minimal IRQ0 handler.
// Just a liveness proof: no fbterm_tick/usb_hid_poll/net_poll/scheduler
// calls (none of those exist/apply at this milestone), and nothing
// reprograms the PIT divisor, so this free-runs at the legacy default
// (~18.2Hz).
#include <stdint.h>
#include "../include/klog.h"

static volatile uint64_t ticks64 = 0;

void timer64_handler(void)
{
    ticks64++;
    if (ticks64 % 100 == 0)
        klog("[timer64] heartbeat\n");
}
