// ToxenOS/user64/fpu_smoke_test64.c — Milestone 33: real cross-process
// regression test proving kernel/process64.c's eager FXSAVE/FXRSTOR
// context switch (added this milestone so userlib/toxui's floating-
// point code -- TrueType rasterization in particular -- can exist at
// all) genuinely prevents floating-point register corruption across
// preemption between multiple CONCURRENT processes each doing
// different floating-point work at once. Deliberately independent of
// ToxUI itself (a single graphical demo process doing font rendering
// would never exercise the "two unrelated processes' FP state
// interleaved by the scheduler" case this is specifically about) --
// kept as a permanent regression test, driven by kernel/process64.c's
// self-test suite exactly like every other ring3-driver case in this
// codebase (see kernel/pipe64.c's/kernel/service64.c's own
// test_ring3_driver for the precedent).
#include <stdint.h>
#include "tox64.h"

static int my_strlen(const char* s) { int i = 0; while (s[i]) i++; return i; }
static void put(const char* s) { sys_write(s, (uint64_t)my_strlen(s)); }

static double sqrt_newton(double x) {
    double guess = x;
    for (int i = 0; i < 40; i++) guess = 0.5 * (guess + x / guess);
    return guess;
}

#define ITERS 200000

static int run_role(double base, double expect_lo, double expect_hi) {
    for (int i = 0; i < ITERS; i++) {
        double v = sqrt_newton(base + (double)(i % 7));
        double target = base + (double)(i % 7);
        double back = v * v;
        double diff = back - target;
        if (diff < 0) diff = -diff;
        if (diff > 0.0001) return 0; // corrupted / wrong result
    }
    (void)expect_lo; (void)expect_hi;
    return 1;
}

void _start(void) {
    char args[8];
    sys_get_args(args, sizeof(args));

    if (args[0] == 0) {
        int64_t pid_a = sys_spawn("/fpu_smoke_test64.nex64", "a");
        int64_t pid_b = sys_spawn("/fpu_smoke_test64.nex64", "b");
        int64_t pid_c = sys_spawn("/fpu_smoke_test64.nex64", "c");
        int ok_self = run_role(2.0, 0, 0);
        int ok_a = (int)sys_wait((uint32_t)pid_a) == 42;
        int ok_b = (int)sys_wait((uint32_t)pid_b) == 42;
        int ok_c = (int)sys_wait((uint32_t)pid_c) == 42;
        if (!ok_self) put("fpu_smoke_test64: SELF corrupted\n");
        if (!ok_a) put("fpu_smoke_test64: child A corrupted\n");
        if (!ok_b) put("fpu_smoke_test64: child B corrupted\n");
        if (!ok_c) put("fpu_smoke_test64: child C corrupted\n");
        int all_ok = ok_self && ok_a && ok_b && ok_c;
        put(all_ok ? "fpu_smoke_test64: PASS\n" : "fpu_smoke_test64: FAIL\n");
        sys_exit(all_ok ? 42 : 1);
    } else {
        double base = (args[0] == 'a') ? 3.0 : (args[0] == 'b') ? 5.0 : 7.0;
        int ok = run_role(base, 0, 0);
        sys_exit(ok ? 42 : 1);
    }
    for (;;) { }
}
