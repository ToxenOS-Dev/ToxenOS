#ifndef PROCESS64_H
#define PROCESS64_H

#include <stdint.h>
#include "paging64.h"
#include "uservm64.h"
#include "handle64.h"

// Milestone 24: unified process model + preemptive scheduler.
//
// Replaces BOTH of the previous, separate models: Milestone 3's
// task64_t (ring-0-only, no address space, cooperative-ish demo
// scheduler) and Milestones 7-9's userproc64_t (real per-process
// address spaces, but synchronous/run-to-completion -- sys_spawn
// blocked the caller until the child finished, so only one user
// process was ever resident at a time). There is now exactly one
// process concept: real address space, real preemption, real
// concurrency. See kernel/process64.c for the full design writeup.

#define PROCESS64_MAX         8
#define PROCESS64_KSTACK_SIZE (16u * 1024u)
#define PROCESS64_ARGS_MAX    128
#define PROCESS64_PATH_MAX    256

// Every Nth timer tick invokes the scheduler (kernel/timer64.c calls
// process64_tick()). The PIT is now reprogrammed to 100Hz
// (timer64_init), so a divisor of 1 gives a genuine ~10ms quantum.
#define PROCESS64_TICK_DIVISOR 1

typedef enum {
    PROCESS64_UNUSED,   // free slot -- must be the zero value (procs[] starts zeroed)
    PROCESS64_READY,    // runnable, waiting for the scheduler to pick it
    PROCESS64_RUNNING,  // currently on the CPU (at most one, single-core)
    PROCESS64_BLOCKED,  // blocked on wait_chan (see process64_block_on/wake_*)
    PROCESS64_ZOMBIE,   // exited/faulted; exit_code valid; address space already
                        // released; slot itself still held until the parent reaps it
} process64_state_t;

typedef struct {
    uint32_t            pid;
    uint32_t            parent_pid;     // 0 = spawned directly by the kernel/idle context, not a real process
    process64_state_t   state;
    uint64_t            kernel_rsp;     // saved kernel stack pointer while NOT running -- see kernel/process64.c
    uint8_t*            kernel_stack;   // this process's own dedicated RSP0 stack
    uint64_t            kernel_stack_size;
    paging64_as_t        as;            // this process's own address space
    int                 exit_code;
    // Milestone 26: generalized from "waiting_for_pid" -- valid while
    // state == BLOCKED, an opaque event identity (a stable pointer: a
    // child process64_t*, or the address of a byte inside a pipe64_t/
    // future kernel object) rather than a pid. sys_wait blocking on a
    // child, pipe read/write blocking, and any future blocking kernel
    // object (keyboard, sockets, GUI messages) all share this ONE
    // mechanism instead of a special case per syscall -- see
    // process64_block_on/process64_wake_one/process64_wake_all.
    void*               wait_chan;
    uservm64_state_t    vm;             // Milestone 25: heap (brk) + anonymous/shared mmap state
    // Milestone 26: pipe/shm kernel-object handles. Milestone 27: also
    // open files/directories (HANDLE64_FILE/HANDLE64_DIR, obj =
    // vfs64_file_t*) -- the old separate fds[] array (a raw txfs64 fd
    // per slot, with no "kind" and no reference counting) is gone; a
    // process now has exactly ONE integer namespace for everything it
    // has open, not two.
    handle64_t          handles[PROCESS64_MAX_HANDLES];
    char                args[PROCESS64_ARGS_MAX];     // copied BY VALUE at spawn time
    char                path[PROCESS64_PATH_MAX];     // copied BY VALUE -- debug/diagnostics only
} process64_t;

void process64_init(void);

// Loads `path` into a brand-new process (own address space, own kernel
// stack), leaves it READY, and returns 0 with *pid_out set on success.
// Returns -1 if the file failed to load (wrong arch, missing,
// malformed) -- no process/slot is consumed in that case. Does NOT
// run it and does NOT block -- the scheduler picks it up on a later
// tick or reschedule point. `parent_pid` should be an existing
// process's pid (real spawns), or 0 for a process spawned directly by
// kernel/debug code with no user-space parent (only process64_wait()
// called with no current process can ever reap such a process).
//
// Milestone 26: if `parent_pid` names a real, currently-tracked
// process, every open handle in ITS handle table (pipe ends, shared-
// memory objects) is copied into the SAME slot number in the new
// child's table, and the referenced object's refcount is bumped once
// per inherited handle -- explicit, whole-table, spawn-time
// inheritance (deliberately simple, mirroring how Unix fork()
// preserves fd numbers across the call): a parent that creates a pipe
// or shared-memory object BEFORE spawning can rely on the child seeing
// the exact same handle numbers afterward. There is no other way for a
// handle to cross process boundaries this milestone (no explicit
// transfer/passing syscall) -- see the Milestone 26 summary for why
// that is sufficient for now.
int process64_spawn(const char* path, const char* args, uint32_t parent_pid, uint32_t* pid_out);

// Milestone 32: the explicit-inheritance counterpart. Identical to
// process64_spawn in every respect EXCEPT which of the parent's
// handles the child receives: instead of the whole table, only the
// slots explicitly named in `inherit` (an array of `inherit_count`
// indices into the PARENT's own handle table) are copied -- each one
// into the SAME slot number in the child (preserving process64_spawn's
// "child sees the exact same handle number" property for whichever
// handles ARE listed), with the same per-kind refcount bump either way
// applies (kernel/process64.c's shared inherit_handle_slot helper).
// `inherit` may be NULL iff `inherit_count` is 0 (inherit nothing --
// the new default-safe case: a child starts with an empty handle
// table unless the parent explicitly hands it something). An
// out-of-range or already-UNUSED index in `inherit` is silently
// skipped, not an error -- mirrors HANDLE64_INPUT/DISPLAY/
// SERVICE_LISTEN never being inheritable by ANY spawn path regardless
// of whether their slot number happens to appear in the list.
// process64_spawn itself (whole-table inheritance) is UNCHANGED and
// kept working forever -- existing test/demo binaries that rely on it
// (pipe_test64, shm_test64, vfs_test64, brk_mmap_test64, the process64
// self-test workers) are not migrated; only NEW graphical/system
// userspace (init64, shell64) uses this explicit-list form. See the
// Milestone 32 summary for the full rationale.
int process64_spawn_ex(const char* path, const char* args, uint32_t parent_pid,
                        const int32_t* inherit, int inherit_count, uint32_t* pid_out);

// The current process, or NULL if the CPU is running the idle/boot
// context (no tracked process -- e.g. kernel_main64's own idle loop).
process64_t* process64_current(void);
int process64_current_pid(void); // -1 if idle

// Called by sys64_exit. Marks the current process ZOMBIE, releases its
// address space and every open handle (files, pipes, shared memory --
// safe: CR3 has already moved off its tables by the time this runs;
// see kernel/process64.c), wakes its parent if blocked specifically on
// this pid, and switches to
// whatever the scheduler picks next. Never returns.
void process64_exit_current(int code);

// Called by the fault path (kernel/interrupt64.c). Identical to
// process64_exit_current(-1) except it logs differently. Never returns.
void process64_fault_current(void);

// Waits for the process `pid`, which must be a child of the caller
// (process64_current(), or -- if called with no current process, i.e.
// from kernel/debug code -- a process spawned with parent_pid == 0).
// If `pid` has not exited yet, blocks the caller (a real process blocks
// via the scheduler; the no-current-process case polls via hlt) until
// it does. Reaps it (frees the table slot) before returning its exit
// code. Returns -1 if `pid` is not a matching child.
int process64_wait(uint32_t pid);

// Called from the timer ISR (kernel/timer64.c) every
// PROCESS64_TICK_DIVISOR ticks. If another process is READY, preempts
// whatever is currently running (or idle) in favor of it round-robin;
// otherwise leaves the current context running untouched (does NOT
// switch to idle just because nothing ELSE happens to be ready).
void process64_tick(void);

// ── Generalized blocking (Milestone 26) ─────────────────────────────
// A small BSD-style "sleep on a channel" primitive: `chan` is any
// stable address that both the blocker and the waker agree identifies
// the same event -- a child process64_t* (process64_wait, below), or
// the address of a byte inside a pipe64_t (kernel/pipe64.c's
// read_chan/write_chan). This is the ONE mechanism every blocking
// kernel object uses, instead of a bespoke state machine per syscall;
// a future keyboard/socket/GUI-message object blocks and wakes the
// exact same way.
//
// Contract (see kernel/pipe64.c for the canonical example): the CALLER
// is responsible for disabling interrupts (the same pushfq/cli/
// restore-on-exit pattern already used by physmem64.c/heap64.c) around
// the ENTIRE "check condition, maybe call process64_block_on, maybe
// loop and recheck" sequence -- process64_block_on() itself never
// touches RFLAGS, so interrupts stay masked across every recheck of the
// loop, and only the caller's own restore (once the loop finally exits)
// re-enables them. This is what prevents a wakeup that happens between
// the condition check and the actual block from being lost forever.
// Must be called with current_idx >= 0 (a real process, never idle).
void process64_block_on(void* chan);

// Moves the first (process64_wake_one) or every (process64_wake_all)
// BLOCKED process whose wait_chan == chan back to READY. Safe to call
// with nobody blocked on `chan` (a no-op). Does not itself switch
// contexts -- the woken process(es) simply become eligible for the
// next scheduling point (a timer tick, or the caller's own next yield).
void process64_wake_one(void* chan);
void process64_wake_all(void* chan);

// Diagnostics helper: klogs "<label><pid> " for every process currently
// BLOCKED on `chan` (nothing logged if none) -- lets kernel/pipe64.c and
// kernel/shm64.c's own dump functions show waiting pids without needing
// direct access to the private procs[] table.
void process64_log_waiters(const char* label, void* chan);

// ── Diagnostics ──────────────────────────────────────────────────────
// Logs the scheduler's current state and every non-UNUSED process's
// pid/state/parent/CR3/path via klog(). For interactive debugging only.
void process64_dump(void);

// Runs the Milestone 24 self-test suite against real NEX64 test
// binaries (multiple resident processes, preemption, isolation, async
// spawn, blocking wait + wakeup, exit status propagation, pid/slot
// reuse, teardown + page reclamation, nested spawn, single/no runnable
// process). Logs each case's result and a final pass/fail tally via
// klog(). Returns 1 if every case passed. Must be run from the
// kernel/idle context (before spawning init64), not from within a
// process.
int process64_selftest(void);

#endif // PROCESS64_H
