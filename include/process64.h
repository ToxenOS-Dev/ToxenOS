#ifndef PROCESS64_H
#define PROCESS64_H

#include <stdint.h>

// Milestone 3: kernel-task scheduler foundation. Every task runs in
// ring 0, sharing the existing static Milestone-1 page tables — no
// per-task address space, no TSS/IST, no ring3 yet (deferred to a
// future Milestone 3B). See kernel/process64.c for the design notes.

typedef enum {
    TASK64_DEAD,    // unused slot — must be the zero value, since
                    // tasks[] starts zero-initialized and an
                    // accidentally-scheduled DEAD slot must never run
    TASK64_READY,
    TASK64_RUNNING,
} task64_state_t;

typedef struct {
    uint64_t        rsp;         // the ONLY field context_switch64 touches
    task64_state_t  state;
    uint32_t        id;
    const char*     name;        // debug/log only
    uint8_t*        stack_base;
    uint64_t        stack_size;
} task64_t;

#define MAX_TASKS64        4
#define TASK64_STACK_SIZE  (16u * 1024u)

// Every Nth timer tick triggers a scheduler decision (see
// kernel/timer64.c). IRQ0 free-runs at the legacy ~18.2Hz (nothing
// reprograms the PIT this milestone), so 4 gives a ~220ms quantum: slow
// enough that each task's heartbeat prints in readable clusters, fast
// enough to show many switches in a few seconds of runtime.
#define SCHED64_TICK_DIVISOR 4

void process64_init(void);
void process64_start(void);     // captures the boot context, switches into Task A
void scheduler64_tick(void);    // called from timer64_handler()

#endif // PROCESS64_H
