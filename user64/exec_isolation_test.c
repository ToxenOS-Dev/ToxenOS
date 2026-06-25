// ToxenOS/user64/exec_isolation_test.c — Milestone 8: per-process memory
// isolation proof. A static (.data/.bss) variable at a fixed virtual
// address: if the kernel's per-process address spaces are real (fresh,
// zeroed pages per process) rather than a shared carve-out, EVERY run of
// this exact binary sees `marker == 0` on entry, no matter how many times
// it has run before -- a leftover nonzero value would mean two "separate"
// processes are secretly sharing the same physical page.
#include <stdint.h>
#include "tox64.h"

static volatile uint64_t marker = 0;

static int my_strlen(const char* s) {
    int i = 0;
    while (s[i]) i++;
    return i;
}

void _start(void) {
    const char* msg;
    if (marker == 0xCAFEBABEull) {
        msg = "exec_isolation_test: LEAKED -- isolation broken (pid=";
    } else {
        msg = "exec_isolation_test: fresh (pid=";
    }
    sys_write(msg, (uint64_t)my_strlen(msg));

    char pidbuf[24];
    int n = 0;
    int v = (int)sys_getpid();
    char tmp[12]; int tn = 0;
    if (v == 0) tmp[tn++] = '0';
    while (v > 0) { tmp[tn++] = (char)('0' + (v % 10)); v /= 10; }
    while (tn > 0) pidbuf[n++] = tmp[--tn];
    pidbuf[n++] = ')'; pidbuf[n++] = '\n'; pidbuf[n] = 0;
    sys_write(pidbuf, (uint64_t)n);

    marker = 0xCAFEBABEull;
    sys_exit(0);

    for (;;) { }  // unreachable
}
