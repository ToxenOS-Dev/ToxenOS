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
// Milestone 33: bumped from 16KB after a real, reproducible kernel-mode
// stack overflow was found (via a hardware write watchpoint) while
// reading a large file (DejaVuSans.ttf, 757KB) through TxFS64: a single,
// uninterrupted syscall call chain -- sys64_handle_read -> vfs64_read ->
// txfs64_read_at (its own 4KB `data_buf[TXFS64_BLOCK_SIZE]`) ->
// txfs64_get_block (its own 4KB `ptrs[TXFS64_PTRS_PER_BLOCK]` for the
// single-indirect case, 8-12KB for double/triple-indirect) ->
// txfs64_read_block -- combined with this kernel's universal -O0 build
// (every local/parameter gets its own stack slot, no reuse) drove the
// stack roughly 10KB past its old 16KB budget, overflowing into the
// next-lower .bss data (kernel/process64.c's own `procs[]` array) and
// corrupting a live process64_t's own fields. Never triggered before
// this milestone because no prior test ever read a file anywhere near
// this large (every previous file was well under one block). 64KB
// gives ample headroom for this call chain (including the deeper
// double/triple-indirect paths, never yet exercised by a real file)
// plus future growth, at a total cost of 8*64KB=512KB static (still
// negligible against real RAM).
#define PROCESS64_KSTACK_SIZE (64u * 1024u)
#define PROCESS64_ARGS_MAX    128
#define PROCESS64_PATH_MAX    256

// Every Nth timer tick invokes the scheduler (kernel/timer64.c calls
// process64_tick()). The PIT is now reprogrammed to 100Hz
// (timer64_init), so a divisor of 1 gives a genuine ~10ms quantum.
#define PROCESS64_TICK_DIVISOR 1

// M+12B: bounded multi-handle WAIT_ANY -- tied to PROCESS64_MAX_HANDLES
// (handle64.h, included above) rather than a separately-hardcoded number,
// because the interest set a process passes to SYS64_HANDLE_WAIT_ANY is
// always a subset of the handles IT ITSELF currently holds (each entry is
// resolved from cur->handles[h] -- see kernel/syscall64.c's
// resolve_wait_any_chan), and no process can ever hold more handles than
// its own table has slots. This makes PROCESS64_WAIT_ANY_MAX a structural
// bound, not a tuning choice: it is automatically sufficient for any
// caller, and automatically tracks PROCESS64_MAX_HANDLES if that limit
// ever changes later.
#define PROCESS64_WAIT_ANY_MAX PROCESS64_MAX_HANDLES

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
    // M+12B: bounded multi-channel wait (see process64_block_on_any,
    // below). wait_is_multi selects which of wait_chan/wait_any_chans[]
    // is meaningful while state==BLOCKED -- 0 (the overwhelmingly common
    // case, unchanged from every pre-M+12B blocker) means wait_chan;
    // nonzero means wait_any_chans[0..wait_any_count). Ownership
    // invariant: ONLY process64_block_on_any sets this to 1; every path
    // that clears BLOCKED state (process64_wake_one/wake_all/wake, and
    // process64_tick's deadline-expiry sweep) unconditionally clears it
    // back to 0 alongside wait_chan/sleep_until_tick, and
    // process64_block_on/process64_block_on_deadline defensively clear it
    // on entry too -- so a stale 1 can never survive into a later,
    // unrelated single-channel block. See kernel/process64.c's
    // wait_matches() for the one place this is actually consulted.
    int                 wait_is_multi;
    void*               wait_any_chans[PROCESS64_WAIT_ANY_MAX];
    int                 wait_any_count;
    // M+4 investigation: nonzero while state==BLOCKED via
    // process64_sleep_ticks() -- the PIT tick (kernel/timer64.c's
    // ticks64) at or after which process64_tick() wakes this process on
    // its own, with no wait_chan/wake_one/wake_all involved (nothing
    // else ever "wakes" a sleeper; the timer tick that reaches the
    // deadline is the only wake source). Zero whenever not sleeping --
    // a real tick count of exactly 0 can never be a valid deadline
    // (sleep_ticks(0) still requires waiting for the NEXT tick, per
    // process64_sleep_ticks()'s own contract), so this doubles safely
    // as the "not sleeping" sentinel alongside state.
    uint64_t            sleep_until_tick;
    // M+4 second stale-line/lag investigation: incremented once per PIT
    // tick (kernel/timer64.c, 100Hz) for whichever process was RUNNING
    // at that tick, sampled BEFORE process64_tick()'s own switch
    // decision -- a coarse (10ms-resolution) per-process "how much of
    // the CPU did this process actually get" accounting, requested to
    // determine whether interactive lag is scheduler contention (many
    // processes fighting for the same CPU) rather than any single
    // subsystem's own cost. Never reset except by
    // process64_sched_accounting_reset() (kernel-internal diagnostic
    // only, no syscall).
    uint64_t            ticks_running;
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
    // Milestone 33: this process's own saved x87/SSE/MXCSR register
    // state, eagerly saved/restored across every context switch (see
    // kernel/process64.c's perform_switch) -- required once userspace
    // floating point exists at all (ToxUI's font rasterizer), since
    // this is genuine CPU-global register state that would otherwise
    // silently corrupt across preemption between two processes that
    // both touch it. Must be 16-byte aligned for FXSAVE/FXRSTOR; a
    // fixed in-struct array (no separate allocation) keeps this
    // reachable from close_all_handles-style cleanup for free (there is
    // nothing to free -- it's just reinitialized at the next spawn into
    // this slot).
    uint8_t             fpu_state[512] __attribute__((aligned(16)));
} process64_t;

void process64_init(void);

// ── M+11C: wake / deadline-block / IRQ-exit reschedule ─────────────────
// process64_wake: like process64_wake_one, but ALSO records the woken process
// as the preferred switch target and asks the generic IRQ-exit path to
// consider rescheduling (irq64_request_reschedule). IRQ-safe. Nothing
// switches inside the caller.
void process64_wake(void* chan);
// Like process64_block_on but ALSO arms a wake-up deadline (a timer tick
// count): woken by process64_wake*/deadline, whichever comes first. BOTH wake
// paths clear BOTH the channel and the deadline, so neither can go stale.
// Same contract as process64_block_on (caller holds cli).
void process64_block_on_deadline(void* chan, uint64_t deadline_tick);
// IRQ-exit scheduler hook (called ONLY by irq64_dispatch after
// irq64_exit_may_resched() approved): switches to the woken process (or the
// next READY one), leaving the interrupted context READY.
void process64_irq_exit_reschedule(void);
// Preempt-disable depth (0 = preemptible). Exit-time resource cleanup runs
// with it raised: it cannot block and must not be switched away from.
void process64_preempt_disable(void);
void process64_preempt_enable(void);
int  process64_preempt_disabled(void);
// True if the CURRENT context may block via the scheduler: a real process,
// not in an interrupt handler, not preempt-disabled.
int  process64_can_block(void);
uint32_t process64_stat_wakes(void);
uint32_t process64_stat_deadline_wakes(void);
// M+12B: see wait_is_multi's own ownership-invariant comment above --
// should stay 0 forever; a defensive backstop, not a normal counter.
uint32_t process64_stat_stale_multiwait(void);

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

// M+12B: like process64_block_on, but the caller is woken by a wake on
// ANY of chans[0..count) (count must be 1..PROCESS64_WAIT_ANY_MAX), or by
// `deadline_tick` if nonzero (same "0 == no deadline" sentinel as
// process64_block_on_deadline). Does NOT report which chan matched --
// callers always re-check every condition they care about themselves on
// return (same discipline pipe64_read/pipe64_write already use), so
// there is nothing to race or lose track of. Same caller-holds-cli
// contract as process64_block_on.
void process64_block_on_any(void* const* chans, int count, uint64_t deadline_tick);

// M+4 investigation: a real, blocking sleep -- genuinely gives up the
// CPU for at least `ticks` PIT ticks (kernel/timer64.c, 100Hz/10ms each)
// via process64_tick()'s own per-tick deadline check, never a busy-wait
// loop. Added specifically so a userspace process that wants to pace
// itself (e.g. an animation loop) no longer has to spin on the CPU to
// do it -- see user64/gfx_demo64.c's own before/after and this
// milestone's investigation report for why a busy-looping "pacing" loop
// starves every OTHER process (including the compositor) under this
// kernel's plain round-robin scheduler, regardless of which display
// presentation backend is active. `ticks == 0` still yields the CPU for
// at least one reschedule point (wakes on the very next tick) rather
// than being a true no-op -- this codebase has no separate zero-cost
// yield primitive, and a caller that wants "give up my remaining
// quantum" gets that from sleep_ticks(0) at effectively the same real-
// time cost as one PIT tick. Must be called with current_idx >= 0 (a
// real process, never idle/kernel-only context).
void process64_sleep_ticks(uint64_t ticks);

// M+4 second stale-line/lag investigation: logs every non-UNUSED
// process's pid/path/state and its ticks_running accumulated since the
// last call to this function (or boot, if never called), then zeroes
// every process's counter -- so successive calls report WINDOWED, not
// cumulative, per-process CPU share. Also logs how many ticks in the
// window went to the idle/kernel context (no process running) via
// `idle_ticks`, which the caller supplies (this file has no standalone
// concept of "seconds elapsed" beyond the tick counter itself).
void process64_dump_sched_accounting(void);

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
