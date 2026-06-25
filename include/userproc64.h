#ifndef USERPROC64_H
#define USERPROC64_H

#include <stdint.h>
#include "paging64.h"

// Milestone 7: real (one-at-a-time) 64-bit user process lifecycle, built
// on top of Milestone 3B's ring3 entry, Milestone 5's syscall layer, and
// Milestone 6's NEX64/ELF64 loader. Deliberately separate from
// kernel/process64.h's task64_t (Milestone 3A ring-0 kernel tasks) --
// the two concepts don't need to merge for this milestone.
//
// Milestone 8: each process owns its own paging64_as_t (see
// kernel/paging64.c) instead of sharing one global carve-out, plus a
// heap_start/heap_end hint (no pages mapped there yet -- just plumbing
// for a future brk-style syscall).
//
// Milestone 9: a tiny per-process file descriptor table (thin wrapper
// over kernel/txfs64.c's own global fd table) and "last completed
// child" fields for sys_spawn/sys_wait (see kernel/userproc64.c's
// userproc64_run -- spawn is synchronous, so by the time it returns the
// child's result is already known and just needs somewhere to live
// until the parent calls sys_wait).

#define USERPROC64_MAX_FDS 4

// Milestone 11: single raw argument string, mirroring the 32-bit
// tox_get_args model (ToxenOS has never had a real argv[] array, 32-bit
// or 64-bit) -- not real argv/argc.
#define USERPROC64_ARGS_MAX 128

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
    paging64_as_t       as;                 // this process's own address space
    uint64_t            heap_start;         // next free page-aligned vaddr after segments
    uint64_t            heap_end;           // == heap_start until something maps heap pages
    int                 fds[USERPROC64_MAX_FDS];  // underlying txfs64 fd, or -1 if unused
    uint32_t            last_child_pid;     // most recently completed child (sys_spawn/sys_wait)
    int                 last_child_exit_code;
    int                 has_child_result;   // 1 if last_child_* is valid and not yet sys_wait'd
    char                args[USERPROC64_ARGS_MAX];  // copied BY VALUE at spawn time, see sys_get_args
} userproc64_t;

#define MAX_USERPROCS64 4

// Loads `path` (NEX64 primary, ELF64 fallback -- see kernel/exec64.c),
// creates a tracked process, points TSS RSP0 at its own kernel stack,
// and enters ring3. Blocks (from the caller's point of view) until the
// process exits or faults, then returns its exit code. Returns -1 if
// the file failed to load (wrong arch, missing, malformed, etc.) --
// no process is created in that case. If `pid_out` is non-NULL, it is
// filled with the new process's pid once assigned (left untouched on a
// load failure).
//
// Milestone 9: safe to call NESTED, from inside another process's own
// syscall handler (sys_spawn) -- it saves and restores the caller's own
// CR3/RSP0/current_idx, so a parent calling this from a trap context
// resumes correctly once the (synchronous, run-to-completion) child
// finishes.
//
// Milestone 11: `args` (may be NULL, meaning "no args") is copied BY
// VALUE into the new process's own args[] field -- never stored as a
// pointer, since the caller's own buffer (typically a kernel-side
// syscall stack buffer, or another process's memory entirely) will not
// outlive this call the way the new process's own state does.
int userproc64_run(const char* path, uint32_t* pid_out, const char* args);

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
