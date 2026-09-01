// ToxenOS/user64/brk_mmap_test64.c — Milestone 25: brk/mmap ABI
// end-to-end test, driven by kernel/uservm64.c's self-test.
//
// Default (empty args): runs a comprehensive sequence exercising brk
// growth/shrink/zero-fill/bounds, multi-region mmap, munmap + address
// reuse, and an invalid-pointer syscall rejection -- exits 42 iff every
// step passed.
//
// args == "isolation": grows the heap by exactly one page, writes a
// pid-derived marker at its very first byte, then busy-loops
// re-checking it -- used by the kernel self-test to spawn TWO instances
// of this concurrently and confirm neither corrupts the other's heap
// even though both live at the identical virtual address (USER_HEAP_BASE
// is fixed for every process). Exits 42 iff the marker survived intact.
#include <stdint.h>
#include "tox64.h"

#define OK_EXIT 42

static int str_eq(const char* a, const char* b) {
    int i = 0;
    while (a[i] && b[i]) { if (a[i] != b[i]) return 0; i++; }
    return a[i] == b[i];
}

static int run_isolation_mode(void) {
    uint64_t pid = sys_getpid();
    uint64_t base = sys_brk(0);
    uint64_t grown = sys_brk(base + 0x1000);
    if (grown != base + 0x1000) return 0;

    volatile uint64_t* marker = (volatile uint64_t*)base;
    if (*marker != 0) return 0; // must be fresh/zeroed

    uint64_t val = 0xCAFE000000000000ULL | pid;
    *marker = val;

    for (volatile uint64_t i = 0; i < 6000000ULL; i++) {
        if (*marker != val) return 0; // another process corrupted "our" address
    }
    return 1;
}

static int run_full_test(void) {
    uint64_t b0 = sys_brk(0);

    // 1. Grow by one page -- must be freshly zeroed, then writable.
    uint64_t b1 = sys_brk(b0 + 0x1000);
    if (b1 != b0 + 0x1000) return 0;
    volatile uint8_t* p0 = (volatile uint8_t*)b0;
    for (int i = 0; i < 4096; i++) if (p0[i] != 0) return 0;
    for (int i = 0; i < 4096; i++) p0[i] = (uint8_t)i;
    for (int i = 0; i < 4096; i++) if (p0[i] != (uint8_t)i) return 0;

    // 2. Grow across 3 more pages -- new pages zeroed, old page's data
    // (and the boundary between old and new) intact.
    uint64_t b2 = sys_brk(b1 + 3 * 0x1000);
    if (b2 != b1 + 3 * 0x1000) return 0;
    volatile uint8_t* p1 = (volatile uint8_t*)b1;
    for (uint64_t i = 0; i < 3 * 0x1000ULL; i++) if (p1[i] != 0) return 0;
    for (int i = 0; i < 4096; i++) if (p0[i] != (uint8_t)i) return 0;

    // 3. Shrink back down, then grow again -- must be re-zeroed, not
    // whatever stale data happened to be physically reused.
    if (sys_brk(b1) != b1) return 0;
    if (sys_brk(b1 + 0x1000) != b1 + 0x1000) return 0;
    for (int i = 0; i < 4096; i++) if (p1[i] != 0) return 0;

    // 4. Grow far beyond the heap budget -- must fail cleanly, leaving
    // the break exactly where it was.
    uint64_t before_bad = sys_brk(0);
    if (sys_brk(0xFFFFFFFFFFFF0000ULL) != (uint64_t)-1) return 0;
    if (sys_brk(0) != before_bad) return 0;

    // 5. mmap one page, then a second, larger region -- non-overlapping,
    // independently writable.
    uint64_t m1 = sys_mmap(0x1000);
    if (m1 == (uint64_t)-1) return 0;
    uint64_t m2 = sys_mmap(3 * 0x1000);
    if (m2 == (uint64_t)-1) return 0;
    if (!(m2 >= m1 + 0x1000 || m1 >= m2 + 3 * 0x1000)) return 0;

    volatile uint8_t* mp1 = (volatile uint8_t*)m1;
    volatile uint8_t* mp2 = (volatile uint8_t*)m2;
    mp1[0] = 0xAB;
    mp2[0] = 0xCD;
    if (mp1[0] != 0xAB || mp2[0] != 0xCD) return 0;

    // 6. munmap m1, then mmap the same size again -- the freed range
    // should be reused (first-fit finds it), and must come back zeroed.
    if (sys_munmap(m1, 0x1000) < 0) return 0;
    uint64_t m3 = sys_mmap(0x1000);
    if (m3 != m1) return 0;
    volatile uint8_t* mp3 = (volatile uint8_t*)m3;
    for (int i = 0; i < 4096; i++) if (mp3[i] != 0) return 0;

    // 7. An unmapped/garbage pointer must be rejected by the syscall
    // layer, not crash the process. sys_write returns uint64_t (a valid
    // byte count on success can itself have bit 63 set once truncated to
    // SYS64_WRITE_MAX -- it never does here, but the ABI-correct failure
    // check is still a signed comparison against the syscall's actual
    // (uint64_t)-1 sentinel, not an unsigned ">= 0" which is always true).
    if ((int64_t)sys_write((const char*)0x1, 8) >= 0) return 0;

    sys_munmap(m2, 3 * 0x1000);
    sys_munmap(m3, 0x1000);
    return 1;
}

void _start(void) {
    char args[16];
    sys_get_args(args, sizeof(args));

    int ok = str_eq(args, "isolation") ? run_isolation_mode() : run_full_test();
    sys_exit(ok ? OK_EXIT : 1);

    for (;;) { } // unreachable
}
