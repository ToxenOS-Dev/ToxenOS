// ToxenOS/user64/exec_badptr_test.c — Milestone 9: user-pointer
// validation proof. Passes a NULL pointer straight to sys_write and
// checks that the syscall rejects it (returns the failure sentinel)
// instead of ever dereferencing it -- if kernel/usercopy64.c's
// validate-before-touch design works, this process survives and exits
// cleanly; if it didn't, the kernel would take a page fault trying to
// read through the NULL pointer on the kernel's own behalf.
#include <stdint.h>
#include "tox64.h"

static int my_strlen(const char* s) {
    int i = 0;
    while (s[i]) i++;
    return i;
}

void _start(void) {
    uint64_t bad = sys_write((const char*)0, 8);

    const char* msg = (bad == (uint64_t)-1)
        ? "exec_badptr_test: NULL pointer correctly rejected\n"
        : "exec_badptr_test: BUG -- bad pointer was not rejected\n";
    sys_write(msg, (uint64_t)my_strlen(msg));

    sys_exit(0);

    for (;;) { }  // unreachable
}
