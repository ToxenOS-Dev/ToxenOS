#ifndef SYSCALL64_H
#define SYSCALL64_H

#include "isr64.h"

// Milestone 5: minimal x86_64 syscall layer via int 0x80. ABI: rax =
// syscall number, rdi/rsi/rdx = up to 3 args -- already sitting in the
// first three System V C argument registers, so syscall64_dispatch can
// read them straight off the saved trapframe64_t with no shuffling.
// Return value goes back into tf->rax, restored into the real rax by
// the time ring3 resumes.
#define SYS64_WRITE  1   // (const char* buf, uint64_t len) -> bytes written
#define SYS64_EXIT   2   // (int code) -> never returns
#define SYS64_GETPID 3   // () -> pid (hardcoded 1 for now)

void syscall64_dispatch(trapframe64_t* tf);

#endif // SYSCALL64_H
