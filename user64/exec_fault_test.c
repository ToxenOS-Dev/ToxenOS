// ToxenOS/user64/exec_fault_test.c — Milestone 7: deliberate user-mode
// page fault, to exercise the pid-aware fault termination path
// (kernel/interrupt64.c's terminate_faulting_user_or_halt ->
// userproc64_fault_current). More representative than another ud2/#UD
// test: page_fault64_handler's CR2/error-code decode already exists
// and this proves it correctly attributes the fault to the right pid
// and returns control to the kernel afterward instead of halting the
// whole machine.
#include <stdint.h>
#include "tox64.h"

static int my_strlen(const char* s) {
    int i = 0;
    while (s[i]) i++;
    return i;
}

void _start(void) {
    const char* msg = "exec_fault_test: about to fault (NULL write)\n";
    sys_write(msg, (uint64_t)my_strlen(msg));

    *(volatile uint64_t*)0 = 0xDEADBEEFu;  // deliberate NULL-pointer write

    for (;;) { }  // unreachable -- the page fault never returns here
}
