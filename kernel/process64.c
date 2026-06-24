// kernel/process64.c — Milestone 3: kernel-task scheduler foundation.
//
// Every task here runs in ring 0 on its own static kernel stack, sharing
// the existing Milestone-1 page tables (no per-task address space, no
// TSS/IST, no ring3 — deferred to a future Milestone 3B). A brand-new
// task is bootstrapped far more simply than the 32-bit fake-iret-frame
// approach: since nothing here ever crosses a privilege level, starting
// a task is just "push its entry function's address as the return
// address context_switch64's `ret` will pop" — no trampoline, no iretq.
#include <stdint.h>
#include "../include/klog.h"
#include "../include/process64.h"

#define BOOT_TASK_ID 0
#define TASK_A_ID    1
#define TASK_B_ID    2

extern void context_switch64(uint64_t* old_rsp_ptr, uint64_t* new_rsp_ptr);

static task64_t tasks[MAX_TASKS64];
static int current_task = BOOT_TASK_ID;

static uint8_t task_stack_a[TASK64_STACK_SIZE] __attribute__((aligned(16)));
static uint8_t task_stack_b[TASK64_STACK_SIZE] __attribute__((aligned(16)));

static volatile uint64_t task_a_count = 0;
static volatile uint64_t task_b_count = 0;

// Deliberately a tight busy loop with NO hlt inside it: this is the
// stronger proof that the scheduler is genuinely preemptive — neither
// task ever voluntarily yields, so the only thing moving execution
// between them is the timer IRQ firing mid-loop and scheduler64_tick()
// deciding to switch. IF stays 1 throughout, so IRQ0/IRQ1 still land
// and preempt this loop normally.
// A brand-new task's first activation never goes through iretq (there is
// no trapframe yet for a task that has never run) — iretq is normally
// what restores RFLAGS.IF on return from an interrupt-gate handler. If a
// task's first activation happens to occur from *inside* the timer IRQ's
// nested call chain (true for every task after the first one, since the
// scheduler only ever runs from inside timer64_handler), it inherits
// whatever IF currently is — 0, because IDT64_INTERRUPT_GATE_K (0x8E)
// gates auto-clear IF on entry. With IF stuck at 0, no further IRQ ever
// fires again, silently freezing the scheduler on whichever task hit
// this first. Each task explicitly re-enables interrupts as its first
// action to guarantee the "every task runs with IF=1" invariant holds
// regardless of how it was started.
static void task64_a_entry(void)
{
    __asm__ volatile ("sti");
    for (;;) {
        task_a_count++;
        if (task_a_count % 2000000 == 0)
            klog("[task64 A] heartbeat\n");
    }
}

static void task64_b_entry(void)
{
    __asm__ volatile ("sti");
    for (;;) {
        task_b_count++;
        if (task_b_count % 2000000 == 0)
            klog("[task64 B] heartbeat\n");
    }
}

// Builds the stack layout context_switch64 expects for a task that has
// never run before. Must mirror context_switch64's pop order exactly
// (pop r15,r14,r13,r12,rbp,rbx — first pop reads the lowest address):
//
//   [highest]  entry_fn   <- popped by `ret`
//              rbx  = 0   <- popped LAST by context_switch64
//              rbp  = 0
//              r12  = 0
//              r13  = 0
//              r14  = 0
//   [lowest]   r15  = 0   <- popped FIRST; task->rsp ends up here
//
// The 6 zero values carry no meaning (callee-saved regs are dead on
// first entry) — only their count (6, matching switch64.asm) and
// entry_fn's position above them matter.
static void task64_bootstrap(task64_t* t, void (*entry_fn)(void))
{
    uint64_t* sp = (uint64_t*)(t->stack_base + t->stack_size);
    *(--sp) = (uint64_t)entry_fn;
    *(--sp) = 0;  // rbx
    *(--sp) = 0;  // rbp
    *(--sp) = 0;  // r12
    *(--sp) = 0;  // r13
    *(--sp) = 0;  // r14
    *(--sp) = 0;  // r15
    t->rsp = (uint64_t)sp;
}

void process64_init(void)
{
    for (int i = 0; i < MAX_TASKS64; i++) {
        tasks[i].state = TASK64_DEAD;
        tasks[i].id    = (uint32_t)i;
        tasks[i].name  = "(unused)";
    }

    // The boot task has no static stack of its own — it's already
    // running on kernel_main64's real stack (linker64.ld's
    // stack_top64). Its rsp is captured by context_switch64 itself the
    // first time process64_start() switches away from it.
    tasks[BOOT_TASK_ID].name = "boot";

    tasks[TASK_A_ID].name       = "A";
    tasks[TASK_A_ID].stack_base = task_stack_a;
    tasks[TASK_A_ID].stack_size = TASK64_STACK_SIZE;
    task64_bootstrap(&tasks[TASK_A_ID], task64_a_entry);

    tasks[TASK_B_ID].name       = "B";
    tasks[TASK_B_ID].stack_base = task_stack_b;
    tasks[TASK_B_ID].stack_size = TASK64_STACK_SIZE;
    task64_bootstrap(&tasks[TASK_B_ID], task64_b_entry);
}

void process64_start(void)
{
    current_task = BOOT_TASK_ID;
    tasks[BOOT_TASK_ID].state = TASK64_READY;  // fallback target if A/B ever die
    tasks[TASK_A_ID].state    = TASK64_READY;
    tasks[TASK_B_ID].state    = TASK64_READY;

    int old = current_task;
    current_task = TASK_A_ID;
    tasks[TASK_A_ID].state = TASK64_RUNNING;
    context_switch64(&tasks[old].rsp, &tasks[current_task].rsp);
    // Only reached again once the scheduler later switches back to
    // BOOT_TASK_ID — execution resumes here, returns into
    // kernel_main64's idle hlt loop exactly where it left off. Same IF
    // hazard as a brand-new task's first activation (see task64_a_entry/
    // task64_b_entry): this resumption never goes through iretq either,
    // so re-assert IF=1 explicitly rather than assume it survived.
    __asm__ volatile ("sti");
}

void scheduler64_tick(void)
{
    int next = current_task;
    for (int i = 1; i <= MAX_TASKS64; i++) {
        int cand = (current_task + i) % MAX_TASKS64;
        if (tasks[cand].state == TASK64_READY || tasks[cand].state == TASK64_RUNNING) {
            next = cand;
            break;
        }
    }
    if (next == current_task) return;  // nothing else runnable

    if (tasks[current_task].state == TASK64_RUNNING)
        tasks[current_task].state = TASK64_READY;
    tasks[next].state = TASK64_RUNNING;

    int old = current_task;
    current_task = next;
    context_switch64(&tasks[old].rsp, &tasks[next].rsp);
}
