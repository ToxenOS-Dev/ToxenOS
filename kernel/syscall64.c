// kernel/syscall64.c — Milestone 5: minimal x86_64 syscall dispatcher.
// Reached from isr64_dispatch (kernel/interrupt64.c) for vector 128
// (int 0x80), exactly like the resumable #BP path -- same trapframe64_t,
// same POP_GPRS/iretq return to ring3 afterward.
#include <stdint.h>
#include "../include/syscall64.h"
#include "../include/klog.h"

#define SYS64_WRITE_MAX 256

// No copy-from-user/pointer-validation infrastructure exists yet (that's
// a 32-bit-only mechanism today) -- safe for now only because the sole
// caller is our own hardcoded, trusted ring3 stub and ring3/ring0 still
// share the exact same page tables (no per-process address space yet).
// Once real, untrusted userland exists (Milestone 6+), this needs real
// validation before it's safe to keep.
static uint64_t sys64_write(const char* buf, uint64_t len) {
    if (len > SYS64_WRITE_MAX) len = SYS64_WRITE_MAX;

    char tmp[SYS64_WRITE_MAX + 1];
    for (uint64_t i = 0; i < len; i++) tmp[i] = buf[i];
    tmp[len] = 0;

    klog(tmp);
    return len;
}

static void sys64_exit(int code) {
    char numbuf[24];
    int n = 0;
    uint32_t v = (uint32_t)code;
    if (v == 0) { numbuf[n++] = '0'; }
    while (v > 0 && n < 24) { numbuf[n++] = (char)('0' + (v % 10)); v /= 10; }
    char rev[24];
    int j = 0;
    while (n > 0) rev[j++] = numbuf[--n];
    rev[j] = 0;

    klog("ring3 process exited, code=");
    klog(rev);
    klog("\n*** halting ***\n");

    __asm__ volatile ("cli");
    for (;;) { __asm__ volatile ("hlt"); }
}

static uint64_t sys64_getpid(void) {
    return 1;  // hardcoded -- no real process table for ring3 contexts yet
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
