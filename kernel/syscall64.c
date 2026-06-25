// kernel/syscall64.c — Milestone 5: minimal x86_64 syscall dispatcher.
// Reached from isr64_dispatch (kernel/interrupt64.c) for vector 128
// (int 0x80), exactly like the resumable #BP path -- same trapframe64_t,
// same POP_GPRS/iretq return to ring3 afterward.
//
// Milestone 7: process-aware. sys64_exit/sys64_getpid now go through
// kernel/userproc64.c's real process tracking instead of a hardcoded
// halt / hardcoded pid=1.
#include <stdint.h>
#include "../include/syscall64.h"
#include "../include/userproc64.h"
#include "../include/klog.h"

#define SYS64_WRITE_MAX 256

// No copy-from-user/pointer-validation infrastructure exists yet (that's
// a 32-bit-only mechanism today) -- safe for now only because ring3/ring0
// still share the exact same page tables (no real per-process address
// space yet). Once that lands, this needs real validation before it's
// safe to keep.
//
// Deliberately does NOT gate on userproc64_current_pid() the way
// sys64_exit/sys64_getpid do: writing to the kernel log has no
// process-state dependency, and the still-independently-working
// Milestone 3B/5 RING3_TEST64_RUN stub calls this exact syscall without
// ever going through userproc64_run (no tracked process at all) -- that
// path must keep producing its original output untouched.
static uint64_t sys64_write(const char* buf, uint64_t len) {
    if (len > SYS64_WRITE_MAX) len = SYS64_WRITE_MAX;

    char tmp[SYS64_WRITE_MAX + 1];
    for (uint64_t i = 0; i < len; i++) tmp[i] = buf[i];
    tmp[len] = 0;

    klog(tmp);
    return len;
}

static void sys64_exit(int code) {
    userproc64_exit_current(code);  // never returns
}

static uint64_t sys64_getpid(void) {
    int pid = userproc64_current_pid();
    return (pid < 0) ? (uint64_t)-1 : (uint64_t)pid;
}

void syscall64_dispatch(trapframe64_t* tf) {
    switch (tf->rax) {
    case SYS64_WRITE:
        tf->rax = sys64_write((const char*)tf->rdi, tf->rsi);
        break;
    case SYS64_EXIT:
        sys64_exit((int)tf->rdi);  // never returns
        break;
    case SYS64_GETPID:
        tf->rax = sys64_getpid();
        break;
    default:
        tf->rax = (uint64_t)-1;
        break;
    }
}
