// kernel/kmutex64.c — Milestone 28: minimal sleeping mutex. See
// include/kmutex64.h for the full design rationale.
#include <stdint.h>
#include "../include/kmutex64.h"
#include "../include/process64.h"
#include "../include/klog.h"

static inline uint64_t kmutex64_irqsave(void) {
    uint64_t flags;
    __asm__ volatile ("pushfq\n\tpop %0\n\tcli" : "=r"(flags) :: "memory");
    return flags;
}
static inline void kmutex64_irqrestore(uint64_t flags) {
    if (flags & (1ULL << 9)) __asm__ volatile ("sti" ::: "memory");
}

void kmutex64_init(kmutex64_t* m) {
    m->locked = 0;
    m->wait_chan = 0;
}

void kmutex64_lock(kmutex64_t* m) {
    for (;;) {
        uint64_t flags = kmutex64_irqsave();
        if (!m->locked) {
            m->locked = 1;
            kmutex64_irqrestore(flags);
            return;
        }

        if (process64_current_pid() < 0) {
            // Kernel/idle context can't block via the scheduler (see
            // process64_block_on's contract: it requires a real
            // process) -- poll via hlt instead, exactly like
            // process64_wait()'s own no-current-process fallback.
            if (!(flags & (1ULL << 9))) {
                // Contended while the idle/boot context has interrupts OFF: nothing can
                // ever release it and hlt with IF=0 would halt the CPU silently forever.
                klog("kmutex64: contended in a non-interruptible idle context -- deadlock\n");
                kernel64_halt_forever();
            }
            kmutex64_irqrestore(flags);
            __asm__ volatile ("hlt");
            continue;
        }

        process64_block_on(&m->wait_chan); // still under this call's own cli -- see process64_block_on's contract
        kmutex64_irqrestore(flags);
        // Loop back and recheck -- process64_block_on only guarantees
        // "something changed", not "this waiter specifically now owns
        // the lock" (process64_wake_one could in principle wake a
        // waiter that loses a race to another one also spinning back
        // through this loop, though with a single core and no
        // intervening preemption between the recheck and re-lock that
        // race cannot actually happen here -- rechecking is still the
        // only correct way to use a condition-style wakeup).
    }
}

void kmutex64_unlock(kmutex64_t* m) {
    uint64_t flags = kmutex64_irqsave();
    m->locked = 0;
    process64_wake_one(&m->wait_chan);
    kmutex64_irqrestore(flags);
}
