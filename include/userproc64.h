#ifndef USERPROC64_H
#define USERPROC64_H

#include <stdint.h>

// Milestone 7: real (one-at-a-time) 64-bit user process lifecycle, built
// on top of Milestone 3B's ring3 entry, Milestone 5's syscall layer, and
// Milestone 6's NEX64/ELF64 loader. Deliberately separate from
// kernel/process64.h's task64_t (Milestone 3A ring-0 kernel tasks) --
// the two concepts don't need to merge for this milestone.

typedef enum {
    USERPROC64_UNUSED,   // zero value -- slot never used or already freed
    USERPROC64_READY,    // loaded, not yet entered
    USERPROC64_RUNNING,
    USERPROC64_EXITED,   // sys_exit
    USERPROC64_FAULTED,  // unhandled exception while running
} userproc64_state_t;

typedef struct {
    uint32_t            pid;
    userproc64_state_t  state;
    uint64_t            entry;              // user RIP
    uint64_t            user_stack_top;     // user RSP
    uint8_t*            kernel_stack;       // this process's own RSP0 stack
    uint64_t            kernel_stack_size;
    uint64_t            kernel_resume_rsp;  // where userproc64_enter saved the launcher
    int                 exit_code;
    const char*         path;               // debug only
} userproc64_t;

#define MAX_USERPROCS64 4

// Loads `path` (NEX64 primary, ELF64 fallback -- see kernel/exec64.c),
// creates a tracked process, points TSS RSP0 at its own kernel stack,
// and enters ring3. Blocks (from the caller's point of view) until the
// process exits or faults, then returns its exit code. Returns -1 if
// the file failed to load (wrong arch, missing, malformed, etc.) --
// no process is created in that case.
int userproc64_run(const char* path);

// NULL if no user process is currently running.
userproc64_t* userproc64_current(void);

// -1 if no user process is currently running.
int userproc64_current_pid(void);

// Called by sys64_exit. Marks the current process EXITED, stores the
// exit code, prints "process exited, pid=X code=Y", and jumps back to
// userproc64_run's caller. Never returns. If there is no tracked
// current process (the Milestone 3B/5 RING3_TEST64_RUN stub still
// calls this same syscall without going through userproc64_run), falls
// back to that test's original exact halt-forever behavior instead.
void userproc64_exit_current(int code);

// Called by the fault path (kernel/interrupt64.c) when a ring3 fault
// is attributed to a tracked process. Marks it FAULTED and jumps back
// to userproc64_run's caller, exactly like a clean exit. Never returns.
void userproc64_fault_current(void);

#endif // USERPROC64_H
