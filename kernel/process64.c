// kernel/process64.c — Milestone 24: unified process model + preemptive
// scheduler.
//
// Design overview
// ----------------
// Every process is represented by a suspended (or actively running)
// kernel stack. Whenever a process is NOT the one currently on the
// CPU, its kernel_rsp points at a stack layout of exactly this shape,
// from low address (== kernel_rsp) to high address:
//
//   [ r15..rbx (6 callee-saved slots, zeroed or real) ]
//   [ return address = process64_resume_trapframe      ]
//   [ trapframe64_t (real or fake -- see below)         ]
//
// This is EXACTLY the layout context_switch64 (kernel/switch64.asm,
// reused unchanged from the old ring-0 task scheduler) expects to
// restore: it pops the 6 callee-saved registers, then executes a plain
// `ret`, which jumps to process64_resume_trapframe (kernel/isr64.asm) --
// a small routine that is simply COMMON_TAIL's own epilogue (restore
// every GPR, discard vector/error_code, iretq) pulled out into its own
// callable target. iretq then drops into ring3 using whatever is in the
// trapframe.
//
// For a process that was PREEMPTED mid-flight, that trapframe is 100%
// real: the timer IRQ landed on this process's own TSS.RSP0 kernel
// stack, COMMON_TAIL pushed a genuine trapframe64_t there, and
// process64_tick() was reached via a normal (if deep) C call chain
// sitting just above it. Switching away from it is just
// context_switch64 saving the CURRENT rsp (wherever it happens to be in
// that call chain) into kernel_rsp; switching back to it later resumes
// that exact call chain, which eventually unwinds back out through
// COMMON_TAIL's own POP_GPRS/iretq -- process64_resume_trapframe is
// never actually reached for this case, since the frame was pushed by a
// real interrupt stub, not fabricated. For a BRAND NEW process (never
// run), bootstrap_process_stack() below fabricates the same layout by
// hand, with a synthetic trapframe pointing at the process's real entry
// point/user stack -- process64_resume_trapframe genuinely IS the first
// thing that runs for it.
//
// This is the same trick the deleted Milestone 3 task64_t scheduler
// used for ring-0-only tasks (context_switch64 + a hand-built stack for
// first activation); the only new piece is fabricating a full ring3
// trapframe instead of a bare `ret`-to-entry-function, which is what
// makes this generalize to real, isolated, preemptible USER processes.
//
// ── CR3 / TSS.RSP0 lifecycle ─────────────────────────────────────────
// perform_switch() always updates CR3 and TSS.RSP0 to the TARGET
// process BEFORE calling context_switch64 -- both are plain register/
// MSR-adjacent state changes with no privilege transition of their own,
// so doing them slightly "early" (while still nominally running as the
// OLD process, for the next few instructions until the RSP swap
// actually happens) is safe: nothing between here and the RSP swap
// takes a ring3->ring0 transition (that's the only time TSS.RSP0 is
// consulted) or relies on CR3 still pointing at the old tables (the
// kernel's own code/data is mapped identically in every process's
// tables, so continuing to execute is fine regardless of which CR3 is
// loaded).
//
// This ordering is exactly what makes process exit safe: an exiting
// process's address space is torn down synchronously, in its own call
// to perform_switch(), immediately after CR3 has already moved to the
// NEXT process -- by the time paging64_destroy_as() frees the exiting
// process's page-table/user pages, CR3 no longer references them at
// all, even though the CPU is still, for a few more instructions,
// executing on the exiting process's own (not-yet-abandoned) kernel
// stack. Its kernel stack itself is never touched at exit time (still
// "in use" until context_switch64 actually swaps away from it) -- only
// reused once a later process64_wait() reaps the slot.
//
// ── Idle ─────────────────────────────────────────────────────────────
// There is no process64_t for "idle" -- kernel_main64's own execution
// (boot stack, boot pml4/CR3, ring0) doubles as the fallback whenever no
// real process is READY, tracked as current_idx == -1 with its own
// dedicated idle_kernel_rsp save slot instead of a table entry.
// Switching TO idle always explicitly reloads the boot pml4 and a
// neutral TSS.RSP0 (stack_top64) -- idle never takes a ring3->ring0
// transition of its own (it's ring0-only, running `hlt` in a loop), so
// TSS.RSP0's value while idle is technically unused, but is still set
// for hygiene/future-proofing.
#include <stdint.h>
#include "../include/process64.h"
#include "../include/exec64.h"
#include "../include/paging64.h"
#include "../include/tss64.h"
#include "../include/isr64.h"
#include "../include/gdt64.h"
#include "../include/physmem64.h"
#include "../include/pipe64.h"
#include "../include/shm64.h"
#include "../include/vfs64.h"
#include "../include/input64.h"
#include "../include/display64.h"
#include "../include/klog.h"

extern void context_switch64(uint64_t* old_rsp_ptr, uint64_t* new_rsp_ptr);

// boot64.asm's static, identity-mapped (VA==PA) boot page tables/stack --
// idle's own CR3/RSP0 targets.
extern uint64_t pml4[512];
extern uint8_t  stack_top64[];

static process64_t procs[PROCESS64_MAX];
static uint8_t     kernel_stacks[PROCESS64_MAX][PROCESS64_KSTACK_SIZE] __attribute__((aligned(16)));
static int         current_idx = -1;   // -1 == idle/boot context is current
static uint32_t    next_pid = 1;
static uint64_t    idle_kernel_rsp = 0;
static uint64_t    g_switch_count = 0; // diagnostic/self-test only -- counts real switches

// ── Small helpers ────────────────────────────────────────────────────
static void copy_str(char* dst, const char* src, int max) {
    int i = 0;
    if (src) { while (src[i] && i < max - 1) { dst[i] = src[i]; i++; } }
    dst[i] = 0;
}

static void dec_to_str(uint64_t val, char* out) {
    char tmp[24];
    int n = 0;
    if (val == 0) tmp[n++] = '0';
    while (val > 0 && n < 24) { tmp[n++] = (char)('0' + (val % 10)); val /= 10; }
    int j = 0;
    while (n > 0) out[j++] = tmp[--n];
    out[j] = 0;
}

static void hex_to_str(uint64_t val, char* out) {
    const char* h = "0123456789ABCDEF";
    out[0] = '0'; out[1] = 'x';
    for (int i = 0; i < 16; i++) { out[2 + (15 - i)] = h[val & 0xF]; val >>= 4; }
    out[18] = 0;
}

// Milestone 26: same pushfq/cli/restore-flags critical-section pattern
// already used by physmem64.c/heap64.c -- protects the procs[] table
// scan+mutate in process64_wake_one/wake_all against being itself
// preempted mid-scan by a timer tick.
static inline uint64_t process64_lock(void) {
    uint64_t flags;
    __asm__ volatile ("pushfq\n\tpop %0\n\tcli" : "=r"(flags) :: "memory");
    return flags;
}
static inline void process64_unlock(uint64_t flags) {
    if (flags & (1ULL << 9)) __asm__ volatile ("sti" ::: "memory");
}

// Milestone 26: defined below (near process64_init) -- forward-declared
// here since perform_switch's cleanup path (above process64_init in
// this file) needs to call it.
static void close_all_handles(process64_t* p);

// ── Fresh-process stack bootstrap ───────────────────────────────────
// Builds the [callee-saved][return addr][trapframe64_t] layout described
// above by hand, for a process that has never run yet.
static void bootstrap_process_stack(process64_t* p, uint64_t entry, uint64_t user_stack_top) {
    uint8_t* top = p->kernel_stack + p->kernel_stack_size;
    trapframe64_t* tf = ((trapframe64_t*)top) - 1;

    uint8_t* z = (uint8_t*)tf;
    for (uint64_t i = 0; i < sizeof(trapframe64_t); i++) z[i] = 0;

    tf->rip    = entry;
    tf->cs     = USER_CODE64_SEL | 3;
    tf->rflags = 0x202; // reserved bit1=1, IF=1
    tf->rsp    = user_stack_top;
    tf->ss     = USER_DATA64_SEL | 3;

    uint64_t* sp = (uint64_t*)tf;
    *(--sp) = (uint64_t)process64_resume_trapframe; // popped by context_switch64's `ret`
    *(--sp) = 0; // rbx
    *(--sp) = 0; // rbp
    *(--sp) = 0; // r12
    *(--sp) = 0; // r13
    *(--sp) = 0; // r14
    *(--sp) = 0; // r15

    p->kernel_rsp = (uint64_t)sp;
}

// ── The switch primitive ─────────────────────────────────────────────
// `to_cleanup` (if non-NULL) is the process whose address space/fds
// should be released as part of this switch -- always the process that
// just called sys64_exit/faulted, always distinct from `new_idx`'s
// process (never itself, since a ZOMBIE is never selected as a switch
// target). See the file header comment for why this ordering is safe.
static void perform_switch(int old_idx, int new_idx, process64_t* to_cleanup) {
    uint64_t* old_rsp_ptr = (old_idx >= 0) ? &procs[old_idx].kernel_rsp : &idle_kernel_rsp;
    uint64_t* new_rsp_ptr = (new_idx >= 0) ? &procs[new_idx].kernel_rsp : &idle_kernel_rsp;

    if (new_idx >= 0) {
        paging64_switch_to(procs[new_idx].as.pml4_phys);
        tss64_set_kernel_stack((uint64_t)(procs[new_idx].kernel_stack + procs[new_idx].kernel_stack_size));
    } else {
        paging64_switch_to((uint64_t)pml4);
        tss64_set_kernel_stack((uint64_t)stack_top64);
    }

    if (to_cleanup) {
        // Milestone 26: uservm64_teardown now ALSO clears the PTEs of
        // (and drops this mapping's reference to) every shared-memory
        // region -- it must run BEFORE paging64_destroy_as, which
        // otherwise cannot tell a shared page from a privately-owned
        // one and would free it out from under any other process still
        // mapping/holding the same shm64_t object. Private anon
        // heap/mmap/image/stack pages are untouched by uservm64_teardown
        // (as before) -- paging64_destroy_as's generic "free every still
        // -present page" sweep is what reclaims those.
        uservm64_teardown(&to_cleanup->vm, &to_cleanup->as);
        paging64_destroy_as(&to_cleanup->as);
        // Milestone 27: close_all_handles now also releases open files/
        // directories (HANDLE64_FILE/HANDLE64_DIR) -- the old separate
        // fds[]/txfs64_close cleanup loop that used to run here is gone,
        // folded into this one pass over the unified handle table.
        close_all_handles(to_cleanup);
    }

    current_idx = new_idx;
    g_switch_count++;
    context_switch64(old_rsp_ptr, new_rsp_ptr);
    // Resumes here once something later switches back to old_idx --
    // EXCEPT when new_idx was a brand-new process, which jumps straight
    // into ring3 via process64_resume_trapframe instead and never
    // "returns" through this specific call at all. Either way there is
    // nothing left to do on this path once we reach here again.
}

// Finds the next READY process starting after `from_idx`, wrapping
// around. Returns -1 if none exists anywhere (including from_idx's own
// slot, which is never READY while it's RUNNING).
static int pick_next_ready(int from_idx) {
    for (int i = 1; i <= PROCESS64_MAX; i++) {
        int cand = (from_idx + i) % PROCESS64_MAX;
        if (procs[cand].state == PROCESS64_READY) return cand;
    }
    return -1;
}

// Used by exit/block paths, where the caller is DEFINITELY not staying
// current -- -1 correctly means "switch to idle" here (unlike
// process64_tick(), which has its own interpretation).
static void yield_to_next_or_idle(process64_t* to_cleanup) {
    int next = pick_next_ready(current_idx);
    if (next >= 0) procs[next].state = PROCESS64_RUNNING;
    perform_switch(current_idx, next, to_cleanup);
}

static process64_t* find_child(uint32_t parent_pid, uint32_t pid) {
    for (int i = 0; i < PROCESS64_MAX; i++) {
        if (procs[i].state != PROCESS64_UNUSED &&
            procs[i].pid == pid && procs[i].parent_pid == parent_pid) {
            return &procs[i];
        }
    }
    return 0;
}

// Milestone 26: arbitrary lookup by pid alone (not restricted to a
// specific parent) -- used only by process64_spawn to find ITS caller
// (the new process's parent) so its handle table can be inherited.
static process64_t* find_by_pid(uint32_t pid) {
    for (int i = 0; i < PROCESS64_MAX; i++) {
        if (procs[i].state != PROCESS64_UNUSED && procs[i].pid == pid) return &procs[i];
    }
    return 0;
}

static void reap(process64_t* p) {
    p->state = PROCESS64_UNUSED;
    p->pid = 0;
    p->parent_pid = 0;
}

// ── Public API ───────────────────────────────────────────────────────
void process64_init(void) {
    for (int i = 0; i < PROCESS64_MAX; i++) {
        procs[i].state = PROCESS64_UNUSED;
        procs[i].pid = 0;
        procs[i].wait_chan = 0;
        for (int h = 0; h < PROCESS64_MAX_HANDLES; h++) {
            procs[i].handles[h].kind = HANDLE64_UNUSED;
            procs[i].handles[h].obj  = 0;
        }
    }
    current_idx = -1;
    idle_kernel_rsp = 0;
    next_pid = 1;
    g_switch_count = 0;
}

// Milestone 26/27: releases every open handle in `p`'s table -- closing
// a pipe end (waking the opposite side / dropping the pipe's refcount),
// releasing a shared-memory reference (freeing its physical pages only
// if this was the last reference anywhere), or releasing an open file/
// directory (kfree'ing its vfs64_file_t only if this was the last
// reference -- e.g. spawn-inherited by a still-running child).
// Independent of uservm64_teardown/paging64_destroy_as, which handle
// this process's own VIRTUAL MAPPINGS of shared-memory objects (a
// separate reference -- see kernel/shm64.c) -- a process can hold a
// handle without ever mapping it, or map-then-close the handle while
// keeping the mapping, so both paths must run at exit.
static void close_all_handles(process64_t* p) {
    for (int i = 0; i < PROCESS64_MAX_HANDLES; i++) {
        switch (p->handles[i].kind) {
        case HANDLE64_PIPE_READ:  pipe64_close_read((pipe64_t*)p->handles[i].obj); break;
        case HANDLE64_PIPE_WRITE: pipe64_close_write((pipe64_t*)p->handles[i].obj); break;
        case HANDLE64_SHM:        shm64_release((shm64_t*)p->handles[i].obj); break;
        case HANDLE64_FILE:
        case HANDLE64_DIR:        vfs64_file_release((vfs64_file_t*)p->handles[i].obj); break;
        case HANDLE64_INPUT:      input64_release(); break;
        case HANDLE64_DISPLAY:    display64_release(); break;
        default: break;
        }
        p->handles[i].kind = HANDLE64_UNUSED;
        p->handles[i].obj  = 0;
    }
}

int process64_spawn(const char* path, const char* args, uint32_t parent_pid, uint32_t* pid_out) {
    int idx = -1;
    for (int i = 0; i < PROCESS64_MAX; i++) {
        if (procs[i].state == PROCESS64_UNUSED) { idx = i; break; }
    }
    if (idx < 0) {
        klog("process64: spawn: no free process slots\n");
        return -1;
    }

    process64_t* p = &procs[idx];

    if (paging64_create_as(&p->as) < 0) {
        klog("process64: spawn: failed to create address space\n");
        return -1;
    }

    uint64_t entry, stack_top;
    if (exec64_load(&p->as, path, &entry, &stack_top) < 0) {
        paging64_destroy_as(&p->as);
        return -1;
    }

    p->pid             = next_pid++;
    p->parent_pid      = parent_pid;
    p->exit_code       = 0;
    p->wait_chan       = 0;
    uservm64_init(&p->vm);
    for (int i = 0; i < PROCESS64_MAX_HANDLES; i++) {
        p->handles[i].kind = HANDLE64_UNUSED;
        p->handles[i].obj  = 0;
    }

    // Milestone 26/27: explicit, whole-table handle inheritance -- see
    // the header comment on process64_spawn for why this (rather than
    // an explicit transfer syscall) is enough for this milestone. Each
    // inherited handle bumps its object's refcount: the parent's and
    // child's handle-table entries are now two INDEPENDENT references
    // to the same pipe end / shared-memory object / open file or
    // directory.
    if (parent_pid != 0) {
        process64_t* parent = find_by_pid(parent_pid);
        if (parent) {
            for (int i = 0; i < PROCESS64_MAX_HANDLES; i++) {
                switch (parent->handles[i].kind) {
                case HANDLE64_PIPE_READ:
                    pipe64_add_ref_read((pipe64_t*)parent->handles[i].obj);
                    p->handles[i] = parent->handles[i];
                    break;
                case HANDLE64_PIPE_WRITE:
                    pipe64_add_ref_write((pipe64_t*)parent->handles[i].obj);
                    p->handles[i] = parent->handles[i];
                    break;
                case HANDLE64_SHM:
                    shm64_add_ref((shm64_t*)parent->handles[i].obj);
                    p->handles[i] = parent->handles[i];
                    break;
                case HANDLE64_FILE:
                case HANDLE64_DIR:
                    vfs64_file_add_ref((vfs64_file_t*)parent->handles[i].obj);
                    p->handles[i] = parent->handles[i];
                    break;
                default:
                    break;
                }
            }
        }
    }

    copy_str(p->args, args, PROCESS64_ARGS_MAX);
    copy_str(p->path, path, PROCESS64_PATH_MAX);

    p->kernel_stack      = kernel_stacks[idx];
    p->kernel_stack_size = PROCESS64_KSTACK_SIZE;
    bootstrap_process_stack(p, entry, stack_top);

    char pidbuf[24];
    dec_to_str(p->pid, pidbuf);
    klog("process64: spawned pid=");
    klog(pidbuf);
    klog(" path=");
    klog(path);
    klog("\n");

    if (pid_out) *pid_out = p->pid;
    p->state = PROCESS64_READY; // scheduler picks it up later; never run here
    return 0;
}

process64_t* process64_current(void) {
    return current_idx >= 0 ? &procs[current_idx] : 0;
}

int process64_current_pid(void) {
    return current_idx >= 0 ? (int)procs[current_idx].pid : -1;
}

// ── Generalized blocking (Milestone 26) ─────────────────────────────
// See include/process64.h's header comment for the full contract.
void process64_block_on(void* chan) {
    process64_t* cur = &procs[current_idx];
    cur->state = PROCESS64_BLOCKED;
    cur->wait_chan = chan;
    yield_to_next_or_idle(0);
    // Resumes here once woken (state was set back to READY and later
    // scheduled by process64_tick or another yield point) -- interrupts
    // are whatever the CALLER had them as before its own first call into
    // this function (unaffected by however long this process was off-
    // CPU); only the caller's own saved-flags restore re-enables them.
}

void process64_wake_one(void* chan) {
    uint64_t flags = process64_lock();
    for (int i = 0; i < PROCESS64_MAX; i++) {
        if (procs[i].state == PROCESS64_BLOCKED && procs[i].wait_chan == chan) {
            procs[i].state = PROCESS64_READY;
            procs[i].wait_chan = 0;
            break;
        }
    }
    process64_unlock(flags);
}

void process64_wake_all(void* chan) {
    uint64_t flags = process64_lock();
    for (int i = 0; i < PROCESS64_MAX; i++) {
        if (procs[i].state == PROCESS64_BLOCKED && procs[i].wait_chan == chan) {
            procs[i].state = PROCESS64_READY;
            procs[i].wait_chan = 0;
        }
    }
    process64_unlock(flags);
}

void process64_log_waiters(const char* label, void* chan) {
    for (int i = 0; i < PROCESS64_MAX; i++) {
        if (procs[i].state == PROCESS64_BLOCKED && procs[i].wait_chan == chan) {
            char pidbuf[24];
            dec_to_str(procs[i].pid, pidbuf);
            klog(label);
            klog(pidbuf);
            klog(" ");
        }
    }
}

static void exit_current(int code, const char* why) {
    int idx = current_idx;
    if (idx < 0) return; // never called with no current process

    process64_t* self = &procs[idx];
    self->exit_code = code;
    self->state = PROCESS64_ZOMBIE;

    char pidbuf[24], codebuf[24];
    dec_to_str(self->pid, pidbuf);
    dec_to_str((uint64_t)(uint32_t)code, codebuf);
    klog(why);
    klog(" pid=");
    klog(pidbuf);
    klog(" code=");
    klog(codebuf);
    klog("\n");

    // Milestone 26: channel identity for "waiting on THIS child" is the
    // child's own (stable, unique while alive) process64_t* -- see
    // process64_wait below. wake_all rather than wake_one purely out of
    // caution (at most one process can legally be waiting per child
    // under the current parent-only-wait rule, so they're equivalent
    // here); harmless no-op if nobody happens to be waiting yet.
    process64_wake_all(self);
    yield_to_next_or_idle(self);
    for (;;) { } // unreachable -- self is ZOMBIE, never selected as a switch target again
}

void process64_exit_current(int code) {
    exit_current(code, "process64: exited,");
}

void process64_fault_current(void) {
    exit_current(-1, "process64: faulted,");
}

int process64_wait(uint32_t pid) {
    if (current_idx < 0) {
        // Called from kernel/debug code, not from a real process -- the
        // only processes it may reap are ones spawned with parent_pid 0.
        process64_t* child = find_child(0, pid);
        if (!child) return -1;
        while (child->state != PROCESS64_ZOMBIE) __asm__ volatile ("hlt");
        int code = child->exit_code;
        reap(child);
        return code;
    }

    process64_t* cur = &procs[current_idx];
    process64_t* child = find_child(cur->pid, pid);
    if (!child) return -1;

    // Milestone 26: the condition check and the block MUST be atomic --
    // without this cli, a timer tick could preempt this process right
    // after the while-condition reads "not ZOMBIE yet" but before
    // process64_block_on marks it BLOCKED; if the child happened to run
    // to completion during that preemption, its exit_current() would
    // call process64_wake_all(child) and find nobody BLOCKED yet (this
    // process hasn't set that state), permanently losing the wakeup.
    // process64_block_on() never touches RFLAGS itself, so this one
    // cli/restore-flags pair covers every recheck of the loop, not just
    // the first -- see include/process64.h's contract comment.
    uint64_t flags = process64_lock();
    while (child->state != PROCESS64_ZOMBIE) {
        process64_block_on(child); // channel identity = the child's own process64_t*
    }
    process64_unlock(flags);

    int code = child->exit_code;
    reap(child);
    return code;
}

void process64_tick(void) {
    int next = pick_next_ready(current_idx);
    if (next < 0) return; // nothing else ready -- let current (or idle) keep running

    if (current_idx >= 0) procs[current_idx].state = PROCESS64_READY;
    procs[next].state = PROCESS64_RUNNING;
    perform_switch(current_idx, next, 0);
}

// ── Diagnostics ──────────────────────────────────────────────────────
static const char* state_name(process64_state_t s) {
    switch (s) {
    case PROCESS64_UNUSED:  return "UNUSED";
    case PROCESS64_READY:   return "READY";
    case PROCESS64_RUNNING: return "RUNNING";
    case PROCESS64_BLOCKED: return "BLOCKED";
    case PROCESS64_ZOMBIE:  return "ZOMBIE";
    }
    return "?";
}

void process64_dump(void) {
    klog("process64: dump ---\n");
    if (current_idx < 0) {
        klog("  current: idle\n");
    } else {
        char pidbuf[24];
        dec_to_str(procs[current_idx].pid, pidbuf);
        klog("  current: pid=");
        klog(pidbuf);
        klog("\n");
    }
    for (int i = 0; i < PROCESS64_MAX; i++) {
        process64_t* p = &procs[i];
        if (p->state == PROCESS64_UNUSED) continue;
        char pidbuf[24], ppidbuf[24], cr3buf[19];
        dec_to_str(p->pid, pidbuf);
        dec_to_str(p->parent_pid, ppidbuf);
        hex_to_str(p->as.pml4_phys, cr3buf);
        klog("  pid="); klog(pidbuf);
        klog(" parent="); klog(ppidbuf);
        klog(" state="); klog(state_name(p->state));
        klog(" cr3="); klog(cr3buf);
        klog(" path="); klog(p->path);
        klog("\n");
    }
    klog("process64: dump end ---\n");
}

// ── Self-test suite ──────────────────────────────────────────────────
// Every case below runs from the kernel/idle context (current_idx < 0),
// which is what makes process64_wait() poll-via-hlt instead of block a
// real process -- see its own header comment. Real ring3-side blocking
// (a process calling sys_wait and actually being marked BLOCKED, then
// woken by its child's exit) is still genuinely exercised: the nested
// case below spawns a worker with args="nest", and THAT worker (running
// in ring3, as pid N) itself calls sys_wait on ITS OWN child (pid N+1)
// from inside its own syscall handler -- exactly the real blocking path,
// just one level removed from this top-level kernel-side test.
//
// Requires `make populate PACKAGE_DEBUG64=1` for sched_worker64.nex64
// (and exec_fault_test.nex64) to exist on the disk image.
#define SCHED_WORKER_PATH "/sched_worker64.nex64"
#define SCHED_FAULT_PATH  "/exec64_fault_test.nex64"

static int test_single_spawn_wait(void) {
    uint32_t pid = 0;
    if (process64_spawn(SCHED_WORKER_PATH, "", 0, &pid) < 0) return 0;
    if (pid == 0) return 0;

    int code = process64_wait(pid);
    if (code != 42) return 0;

    // Reaped -- the slot must be fully vacated, not just marked done.
    process64_t* gone = find_child(0, pid);
    return gone == 0;
}

static int test_concurrent_preemption_and_isolation(void) {
    uint32_t pid_a = 0, pid_b = 0;
    int spawn_a, spawn_b;
    process64_t *a, *b;
    int a_resident, b_resident;
    uint64_t switches_before;

    // The kernel/idle context is itself preemptible: interrupts are
    // enabled throughout boot, and loading two binaries from disk (the
    // two spawns below) takes long enough in wall-clock time that a
    // timer tick is essentially always pending by the time it's safe to
    // service one again. A cli/sti pair around JUST the two spawns is
    // not enough -- re-enabling interrupts (sti) right after them lets
    // that pending tick fire immediately, which can preempt THIS
    // function (kernel/idle is just another switch target) and not
    // resume it until BOTH newly-READY workers have run to completion
    // (idle is only resumed once nothing else is runnable) -- defeating
    // a "check they're both still resident" read placed after the sti.
    // The whole spawn-then-observe sequence below must stay atomic, so
    // the critical section covers the state reads too, not just the
    // spawns. See kernel/physmem64.c/heap64.c for the same
    // save/restore-flags technique used for their own critical sections.
    uint64_t flags;
    __asm__ volatile ("pushfq\n\tpop %0\n\tcli" : "=r"(flags) :: "memory");

    spawn_a = process64_spawn(SCHED_WORKER_PATH, "", 0, &pid_a);
    spawn_b = process64_spawn(SCHED_WORKER_PATH, "", 0, &pid_b);
    a = find_child(0, pid_a);
    b = find_child(0, pid_b);
    a_resident = a && a->state != PROCESS64_ZOMBIE;
    b_resident = b && b->state != PROCESS64_ZOMBIE;
    switches_before = g_switch_count;

    if (flags & (1ULL << 9)) __asm__ volatile ("sti" ::: "memory");

    if (spawn_a < 0 || spawn_b < 0) return 0;
    if (pid_a == 0 || pid_b == 0 || pid_b == pid_a) return 0;
    // Both must be genuinely resident (not yet exited) immediately after
    // both spawns return -- proves spawn does not block on completion.
    if (!a_resident || !b_resident) return 0;

    // Wait for B first (spawned second) -- exercises "wait for a
    // specific pid regardless of spawn/completion order", not just
    // FIFO.
    int code_b = process64_wait(pid_b);
    int code_a = process64_wait(pid_a);

    uint64_t switches_after = g_switch_count;

    // Two busy-looping workers sharing one CPU for ~8M iterations each
    // can only both finish if the scheduler repeatedly switched between
    // them -- a handful of switches would mean one just got lucky
    // (e.g. finished, then the other ran alone); real round-robin
    // preemption at a ~10ms quantum produces many more than that.
    int real_preemption = (switches_after - switches_before) >= 8;

    return code_a == 42 && code_b == 42 && real_preemption;
}

static int test_nested_spawn(void) {
    uint32_t pid = 0;
    if (process64_spawn(SCHED_WORKER_PATH, "nest", 0, &pid) < 0) return 0;
    if (pid == 0) return 0;

    int code = process64_wait(pid);
    // 42 is only possible if the outer worker's OWN nested sys_spawn +
    // (real, ring3-blocking) sys_wait on its child also succeeded --
    // see user64/sched_worker64.c.
    return code == 42;
}

static int test_exit_status_propagation(void) {
    uint32_t pid = 0;
    if (process64_spawn(SCHED_FAULT_PATH, "", 0, &pid) < 0) return 0;
    if (pid == 0) return 0;

    int code = process64_wait(pid);
    // exec_fault_test.nex64 deliberately NULL-derefs; process64_fault_current()
    // hardcodes exit_code -1 -- a DIFFERENT value than the success path's
    // 42, proving exit status genuinely propagates rather than always
    // reading back some fixed constant.
    return code == -1;
}

static int test_pid_and_slot_reuse(void) {
    uint32_t pid1 = 0, pid2 = 0;

    if (process64_spawn(SCHED_WORKER_PATH, "", 0, &pid1) < 0) return 0;
    process64_t* p1 = find_child(0, pid1);
    if (!p1) return 0;
    int slot1 = (int)(p1 - procs);
    if (process64_wait(pid1) != 42) return 0;

    if (process64_spawn(SCHED_WORKER_PATH, "", 0, &pid2) < 0) return 0;
    process64_t* p2 = find_child(0, pid2);
    if (!p2) return 0;
    int slot2 = (int)(p2 - procs);
    if (process64_wait(pid2) != 42) return 0;

    // Pids are never numerically reused (a monotonic counter -- see
    // process64_spawn); the underlying TABLE SLOT is what gets recycled.
    return pid2 == pid1 + 1 && slot2 == slot1;
}

static int test_repeated_cycles_no_leak(void) {
    physmem64_stats_t before, after;
    physmem64_stats(&before);

    for (int i = 0; i < 20; i++) {
        uint32_t pid = 0;
        if (process64_spawn(SCHED_WORKER_PATH, "", 0, &pid) < 0) return 0;
        if (pid == 0) return 0;
        if (process64_wait(pid) != 42) return 0;
    }

    physmem64_stats(&after);
    return after.used_pages == before.used_pages && after.free_pages == before.free_pages;
}

#define PROCESS64_TEST(name, expr) do {          \
    int _r = (expr);                             \
    klog("process64_selftest: " name " ");       \
    klog(_r ? "PASS\n" : "FAIL\n");              \
    if (_r) pass++; else fail++;                 \
} while (0)

int process64_selftest(void) {
    int pass = 0, fail = 0;
    klog("process64_selftest: starting\n");

    PROCESS64_TEST("single spawn/wait (also: exit status, one runnable)", test_single_spawn_wait());
    PROCESS64_TEST("concurrent residency + preemption + isolation", test_concurrent_preemption_and_isolation());
    PROCESS64_TEST("nested process creation", test_nested_spawn());
    PROCESS64_TEST("exit status propagation (fault path)", test_exit_status_propagation());
    PROCESS64_TEST("pid monotonic, slot reused", test_pid_and_slot_reuse());
    PROCESS64_TEST("repeated spawn/wait cycles, no page leak", test_repeated_cycles_no_leak());

    char passbuf[24], failbuf[24];
    dec_to_str((uint64_t)pass, passbuf);
    dec_to_str((uint64_t)fail, failbuf);
    klog("process64_selftest: pass=");
    klog(passbuf);
    klog(" fail=");
    klog(failbuf);
    klog("\n");
    return fail == 0;
}
