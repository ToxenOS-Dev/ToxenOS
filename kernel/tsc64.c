// kernel/tsc64.c — M+4 investigation: RDTSC calibrated against the
// existing 100Hz PIT tick. See include/tsc64.h for the full rationale.
#include "../include/tsc64.h"
#include "../include/klog.h"

extern uint64_t timer64_get_ticks(void);

static uint64_t g_cycles_per_ms = 0; // 0 = not calibrated yet

void tsc64_init(void) {
    // Align to a tick boundary first so the calibration window starts
    // right after a real PIT edge, not partway through one already in
    // progress -- halves the worst-case boundary error for a given
    // window length.
    uint64_t t0 = timer64_get_ticks();
    while (timer64_get_ticks() == t0) { }

    uint64_t start_tick = timer64_get_ticks();
    uint64_t start_cycles = tsc64_read();

    // 10 real ticks (100ms at 100Hz) -- long enough that the ±1-tick
    // boundary uncertainty is a small fraction of the window, short
    // enough not to visibly delay boot.
    while (timer64_get_ticks() - start_tick < 10) { }
    uint64_t end_cycles = tsc64_read();
    uint64_t elapsed_ticks = timer64_get_ticks() - start_tick;

    uint64_t elapsed_ms = elapsed_ticks * 10; // 100Hz = 10ms/tick
    if (elapsed_ms == 0) elapsed_ms = 1; // unreachable given the loop above, but never divide by zero
    g_cycles_per_ms = (end_cycles - start_cycles) / elapsed_ms;

    klog_hex("tsc64: calibrated cycles_per_ms = ", (uint32_t)g_cycles_per_ms);
}

uint64_t tsc64_cycles_to_us(uint64_t cycles) {
    if (g_cycles_per_ms == 0) return 0;
    return (cycles * 1000) / g_cycles_per_ms;
}
