// ToxenOS/user64/sched_worker64.c — Milestone 24: scheduler test worker.
//
// A single small program used to prove several scheduler properties at
// once when multiple instances of it are spawned concurrently by
// kernel/process64.c's self-test (or interactively via the shell):
//
//   - Real timer-driven preemption: the busy loops below contain NO
//     syscalls that would voluntarily yield -- if two instances run
//     "at once" and their heartbeat lines interleave in the kernel log
//     (rather than one finishing entirely before the other starts),
//     that is only possible if the timer IRQ genuinely preempted one
//     mid-loop in favor of the other.
//   - Address-space isolation: `marker` is a .bss variable at a
//     process-private virtual address (same idea as the Milestone 8
//     user64/exec_isolation_test.c, generalized to two DIFFERENT
//     processes running AT THE SAME TIME instead of the same binary
//     run twice in sequence). It must read 0 on entry -- a leaked
//     nonzero value would mean two "separate" address spaces are
//     secretly sharing the same physical page -- and once this
//     process sets it to a value derived from its OWN pid, it must
//     still read back exactly that value after the second busy loop:
//     a concurrently running SEPARATE instance corrupting it would
//     mean the two "isolated" address spaces actually alias the same
//     physical memory.
//
// With args == "nest", also spawns and waits for one more instance of
// itself (with empty args, so it never recurses more than one level)
// after its own checks pass -- its own exit code folds in whether that
// nested child also succeeded (42), so a single exit-code check from
// whoever spawned the outer instance proves the whole chain worked.
#include <stdint.h>
#include "tox64.h"

#define SELF_PATH   "/sched_worker64.nex64"
#define WORK_ITERS  4000000ULL
#define OK_EXIT     42

static volatile uint64_t marker = 0;

static int my_strlen(const char* s) { int i = 0; while (s[i]) i++; return i; }
static void put(const char* s) { sys_write(s, (uint64_t)my_strlen(s)); }

static void put_u64(uint64_t v) {
    char rev[24]; int rn = 0;
    if (v == 0) rev[rn++] = '0';
    while (v > 0) { rev[rn++] = (char)('0' + (v % 10)); v /= 10; }
    char out[24]; int n = 0;
    while (rn > 0) out[n++] = rev[--rn];
    out[n] = 0;
    put(out);
}

static int str_eq(const char* a, const char* b) {
    int i = 0;
    while (a[i] && b[i]) { if (a[i] != b[i]) return 0; i++; }
    return a[i] == b[i];
}

void _start(void) {
    uint64_t pid = sys_getpid();
    uint64_t want = 0xCAFE000000000000ULL | pid;

    if (marker != 0) {
        put("sched_worker64: LEAKED marker on entry, pid="); put_u64(pid); put("\n");
        sys_exit(1);
    }

    put("sched_worker64: start pid="); put_u64(pid); put("\n");

    for (volatile uint64_t i = 0; i < WORK_ITERS; i++) {
        if (i % 1000000ULL == 0) {
            put("sched_worker64: heartbeat pid="); put_u64(pid);
            put(" i="); put_u64(i); put("\n");
        }
    }

    marker = want;

    for (volatile uint64_t i = 0; i < WORK_ITERS; i++) {
        if (marker != want) {
            put("sched_worker64: ISOLATION BROKEN pid="); put_u64(pid); put("\n");
            sys_exit(2);
        }
    }

    int ok = 1;
    char args[32];
    sys_get_args(args, sizeof(args));
    if (str_eq(args, "nest")) {
        put("sched_worker64: nesting, pid="); put_u64(pid); put("\n");
        int64_t child = sys_spawn(SELF_PATH, "");
        if (child < 0) {
            put("sched_worker64: nested spawn failed\n");
            ok = 0;
        } else {
            int64_t code = sys_wait((uint32_t)child);
            if (code != OK_EXIT) ok = 0;
        }
    }

    put("sched_worker64: done pid="); put_u64(pid); put("\n");
    sys_exit(ok ? OK_EXIT : 3);

    for (;;) { } // unreachable
}
