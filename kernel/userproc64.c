// kernel/userproc64.c — Milestone 7: real user process lifecycle.
//
// Owns process tracking, per-process kernel stacks (RSP0), per-process
// address spaces (Milestone 8: kernel/paging64.c), and the asymmetric
// enter/resume jump (kernel/userproc64.asm).
//
// Milestone 8: each process now gets its own paging64_as_t -- created
// before loading, activated (CR3) right before ring3 entry, and
// deactivated+destroyed immediately after the process exits or faults,
// before the slot is reclaimed. Still strictly one-process-at-a-time
// (cooperative, run-to-exit) -- this milestone adds isolation, not
// concurrency.
#include <stdint.h>
#include "../include/userproc64.h"
#include "../include/exec64.h"
#include "../include/paging64.h"
#include "../include/tss64.h"
#include "../include/memmap64.h"
#include "../include/klog.h"

#define USERPROC64_KSTACK_SIZE (16u * 1024u)

extern void userproc64_enter(uint64_t* resume_rsp_out, uint64_t user_rip, uint64_t user_rsp);
extern void userproc64_return_to_kernel(uint64_t resume_rsp);

// boot64.asm's static, identity-mapped (VA==PA) boot pml4 -- the address
// space every process's table is built from a copy of, and the address
// space control returns to whenever no process is running.
extern uint64_t pml4[512];

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

static void hex_to_str(uint64_t val, char* out) {
    const char* h = "0123456789ABCDEF";
    out[0] = '0'; out[1] = 'x';
    for (int i = 0; i < 16; i++) { out[2 + (15 - i)] = h[val & 0xF]; val >>= 4; }
    out[18] = 0;
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
    if (current_idx >= 0) {
        klog("userproc64: reentrant userproc64_run call -- refusing\n");
        return -1;
    }

    int idx = -1;
    for (int i = 0; i < MAX_USERPROCS64; i++) {
        if (procs[i].state == USERPROC64_UNUSED) { idx = i; break; }
    }
    if (idx < 0) {
        klog("userproc64: no free process slots\n");
        return -1;
    }

    userproc64_t* p = &procs[idx];

    if (paging64_create_as(&p->as) < 0) {
        klog("userproc64: failed to create address space\n");
        return -1;
    }

    uint64_t entry, stack_top, heap_start;
    if (exec64_load(&p->as, path, &entry, &stack_top, &heap_start) < 0) {
        paging64_destroy_as(&p->as);
        return -1;
    }

    p->pid               = next_pid++;
    p->state             = USERPROC64_READY;
    p->entry             = entry;
    p->user_stack_top    = stack_top;
    p->kernel_stack      = kernel_stacks[idx];
    p->kernel_stack_size = USERPROC64_KSTACK_SIZE;
    p->exit_code         = 0;
    p->path              = path;
    p->heap_start        = heap_start;
    p->heap_end          = heap_start;

    int prev_idx = current_idx;
    current_idx  = idx;
    p->state     = USERPROC64_RUNNING;

    char pidbuf[24], physbuf[19];
    dec_to_str(p->pid, pidbuf);
    hex_to_str(p->as.pt_phys, physbuf);
    klog("userproc64: starting pid=");
    klog(pidbuf);
    klog(" path=");
    klog(path);
    klog(" pt_phys=");
    klog(physbuf);
    klog("\n");

    // RSP0 must point at THIS process's own kernel stack before ring3
    // entry -- a single, never-updated RSP0 stack is unsafe once more
    // than one process can exist across the lifetime of the kernel.
    tss64_set_kernel_stack((uint64_t)(p->kernel_stack + p->kernel_stack_size));

    // Activate the process's own address space right before entering
    // ring3 -- everything up to here (slot bookkeeping, tss64 update)
    // ran under whatever address space was already active (the boot one,
    // or a previous process's about to be restored below), which is
    // always safe since every table carries the same kernel mappings.
    paging64_switch_to(p->as.pml4_phys);

    // Saves the launcher's (this function's) callee-saved regs + rsp
    // into p->kernel_resume_rsp, then iretq's into ring3. Execution
    // resumes right here -- not by falling through, but via
    // userproc64_return_to_kernel jumping back to this exact point --
    // once the process exits or faults.
    userproc64_enter(&p->kernel_resume_rsp, p->entry, p->user_stack_top);

    // The process is done (exited or faulted). Restore the kernel's own
    // address space BEFORE freeing the process's now-unloaded tables --
    // switching CR3 away from p->as is also the TLB flush that makes
    // freeing its pages safe (no stale entry can reference them
    // afterward, since every MOV CR3 on this kernel is an unconditional
    // full flush -- PCID is never enabled).
    // pml4 is a .bootdata-section global -- already VA==PA (identity-
    // mapped), unlike every other phys_of() argument in this codebase.
    // Do NOT phys_of() it (that subtracts KERNEL_VIRT_BASE64 from an
    // already-physical address and produces garbage) -- same convention
    // kernel/ring3_test64.c's own CR3 reload already uses.
    paging64_switch_to((uint64_t)pml4);
    paging64_destroy_as(&p->as);

    int code = p->exit_code;
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
