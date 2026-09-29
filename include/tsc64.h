#ifndef TSC64_H
#define TSC64_H

#include <stdint.h>

// M+4 investigation: RDTSC-based cycle-accurate timing, calibrated
// against the existing 100Hz PIT tick (kernel/timer64.c) at boot -- the
// smallest possible high-resolution clock this kernel can offer, needed
// because the PIT's own 10ms granularity cannot expose sub-millisecond
// VirtIO-GPU command latency (manual testing found real interactive lag
// the M+4 perf instrumentation's 10ms ticks completely failed to show).
// Internal diagnostic/instrumentation use only -- NOT exposed to
// userspace, not a general timekeeping subsystem, no new syscall.

// Must run after timer64_init()+sti (needs real PIT ticks advancing to
// calibrate against). Busy-waits for a short, fixed real-time window
// (~100ms) -- a one-time boot cost, not a recurring one.
void tsc64_init(void);

// Raw cycle counter -- a thin wrapper around RDTSC. Always safe to call
// (even before tsc64_init()); only tsc64_cycles_to_us() needs
// calibration to have completed.
static inline uint64_t tsc64_read(void) {
    uint32_t lo, hi;
    __asm__ volatile ("rdtsc" : "=a"(lo), "=d"(hi));
    return ((uint64_t)hi << 32) | lo;
}

// Converts a cycle DELTA (e.g. tsc64_read() - start) to microseconds
// using the boot-time calibration. Returns 0 before tsc64_init() has
// completed (calibration incomplete) rather than dividing by zero.
uint64_t tsc64_cycles_to_us(uint64_t cycles);

#endif // TSC64_H
