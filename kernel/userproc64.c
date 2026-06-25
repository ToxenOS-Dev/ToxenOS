// kernel/userproc64.c — Milestone 7: real user process lifecycle.
//
// Owns process tracking, per-process kernel stacks (RSP0), and the
// asymmetric enter/resume jump (kernel/userproc64.asm). Loading is
// still Milestone 6's exec64_load() (parse+map+CR3 reload only, no
// jump) -- this file just decides what to do with the resulting
// entry/stack_top: create a tracked process, point TSS RSP0 at its own
// stack, and enter ring3.
//
// Still single-shared-address-space (Milestone 6's carved PD_EXEC_IDX,
// not a real per-process PML4) and one-process-at-a-time -- loading a
// second binary over a still-"running" one would silently overwrite
// the first one's pages. Fine for this milestone's cooperative,
// run-to-exit model; a documented limitation, not a bug.
#include <stdint.h>
#include "../include/userproc64.h"
#include "../include/exec64.h"
#include "../include/tss64.h"
#include "../include/klog.h"

#define USERPROC64_KSTACK_SIZE (16u * 1024u)

extern void userproc64_enter(uint64_t* resume_rsp_out, uint64_t user_rip, uint64_t user_rsp);
extern void userproc64_return_to_kernel(uint64_t resume_rsp);

static userproc64_t procs[MAX_USERPROCS64];
static uint8_t kernel_stacks[MAX_USERPROCS64][USERPROC64_KSTACK_SIZE] __attribute__((aligned(16)));
static int current_idx = -1;
static uint32_t next_pid = 1;

static void dec_to_str(uint64_t val, char* out) {
    char tmp[24];
    int n = 0;
    if (val == 0) tmp[n++] = '0';
    while (val > 0 && n < 24) { tmp[n++] = (char)('0' + (val % 10)); val /= 10; }
    int j = 0;
    while (n > 0) out[j++] = tmp[--n];
    out[j] = 0;
}

userproc64_t* userproc64_current(void) {
    if (current_idx < 0) return 0;
    return &procs[current_idx];
}

int userproc64_current_pid(void) {
    if (current_idx < 0) return -1;
    return (int)procs[current_idx].pid;
}

int userproc64_run(const char* path) {
    uint64_t entry, stack_top;
    if (exec64_load(path, &entry, &stack_top) < 0) return -1;

    int idx = -1;
    for (int i = 0; i < MAX_USERPROCS64; i++) {
        if (procs[i].state == USERPROC64_UNUSED) { idx = i; break; }
    }
    if (idx < 0) {
        klog("userproc64: no free process slots\n");
        return -1;
    }

    userproc64_t* p = &procs[idx];
    p->pid               = next_pid++;
    p->state             = USERPROC64_READY;
    p->entry             = entry;
    p->user_stack_top    = stack_top;
    p->kernel_stack      = kernel_stacks[idx];
    p->kernel_stack_size = USERPROC64_KSTACK_SIZE;
    p->exit_code         = 0;
    p->path              = path;

    int prev_idx = current_idx;
    current_idx  = idx;
    p->state     = USERPROC64_RUNNING;

    char pidbuf[24];
    dec_to_str(p->pid, pidbuf);
    klog("userproc64: starting pid=");
    klog(pidbuf);
    klog(" path=");
    klog(path);
    klog("\n");

    // RSP0 must point at THIS process's own kernel stack before ring3
    // entry -- Milestone 3B/6 only ever had one fixed, never-updated
    // RSP0 stack, which is unsafe once more than one process can exist
    // across the lifetime of the kernel.
    tss64_set_kernel_stack((uint64_t)(p->kernel_stack + p->kernel_stack_size));

    // Saves the launcher's (this function's) callee-saved regs + rsp
    // into p->kernel_resume_rsp, then iretq's into ring3. Execution
    // resumes right here -- not by falling through, but via
    // userproc64_return_to_kernel jumping back to this exact point --
    // once the process exits or faults.
    userproc64_enter(&p->kernel_resume_rsp, p->entry, p->user_stack_top);

    int code = p->exit_code;
    // Slot reclaimed; pages and the kernel stack itself are not freed
    // (no per-process address space teardown yet) -- documented TODO,
    // matches Milestone 6/7's "no heap, no real memory management"
    // constraint rather than silently leaking the slot forever.
    p->state    = USERPROC64_UNUSED;
    current_idx = prev_idx;
    return code;
}

void userproc64_exit_current(int code) {
    if (current_idx < 0) {
        // No tracked process -- reachable from the still-independently-
        // working Milestone 3B/5 RING3_TEST64_RUN stub, which calls
        // this same syscall without ever going through userproc64_run.
        // Reproduce that test's exact original behavior byte-for-byte.
        char numbuf[24];
        dec_to_str((uint64_t)(uint32_t)code, numbuf);
        klog("ring3 process exited, code=");
        klog(numbuf);
        klog("\n*** halting ***\n");
        __asm__ volatile ("cli");
        for (;;) { __asm__ volatile ("hlt"); }
    }

    userproc64_t* p = &procs[current_idx];
    p->exit_code = code;
    p->state     = USERPROC64_EXITED;

    char pidbuf[24], codebuf[24];
    dec_to_str(p->pid, pidbuf);
    dec_to_str((uint64_t)(uint32_t)code, codebuf);
    klog("process exited, pid=");
    klog(pidbuf);
    klog(" code=");
    klog(codebuf);
    klog("\n");

    userproc64_return_to_kernel(p->kernel_resume_rsp);
    // unreachable -- jumps back into userproc64_run's stack frame
}

void userproc64_fault_current(void) {
    if (current_idx < 0) return;  // caller already checked current_idx >= 0

    userproc64_t* p = &procs[current_idx];
    p->exit_code = -1;
    p->state     = USERPROC64_FAULTED;

    userproc64_return_to_kernel(p->kernel_resume_rsp);
    // unreachable -- jumps back into userproc64_run's stack frame
}
