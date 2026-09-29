// kernel/kwait64.c -- M+11C explicit condition wait. See include/kwait64.h.
#include <stdint.h>
#include "../include/kwait64.h"
#include "../include/process64.h"
#include "../include/irq64.h"

extern uint64_t timer64_get_ticks(void);

static kwait64_stats_t g_st;

kwait64_ctx_t kwait64_classify(int in_irq, int has_process, int preempt_disabled, int entered_if_set) {
    if (in_irq) return KWAIT64_CTX_WOULDBLOCK;
    if (preempt_disabled) return KWAIT64_CTX_WOULDBLOCK;
    if (has_process) return KWAIT64_CTX_PROCESS;
    return entered_if_set ? KWAIT64_CTX_BOOT : KWAIT64_CTX_WOULDBLOCK;
}

// The same classification kwait64_wait() will make, without waiting: lets a driver refuse a request
// BEFORE it submits anything to the device (once the doorbell rings the device owns the buffers and
// will write the response whether or not anyone waits for it).
int kwait64_can_wait(void) {
    uint64_t flags;
    __asm__ volatile ("pushfq; pop %0" : "=r"(flags) :: "memory");
    return kwait64_classify(irq64_in_irq(), process64_current_pid() >= 0,
                            process64_preempt_disabled(), (flags & 0x200) != 0) != KWAIT64_CTX_WOULDBLOCK;
}

void kwait64_get_stats(kwait64_stats_t* out) { *out = g_st; }

void kwait64_signal(void* chan) {
    g_st.signals++;
    process64_wake(chan);
}

int kwait64_wait(void* chan, kwait64_cond_fn cond, void* arg, uint64_t timeout_ticks) {
    uint64_t flags;
    __asm__ volatile ("pushfq; pop %0; cli" : "=r"(flags) :: "memory");
    kwait64_ctx_t ctx = kwait64_classify(irq64_in_irq(), process64_current_pid() >= 0,
                                         process64_preempt_disabled(), (flags & 0x200) != 0);
    int rc;
    if (ctx == KWAIT64_CTX_WOULDBLOCK) {
        g_st.wouldblock++;
        rc = KWAIT64_WOULDBLOCK;
    } else {
        uint64_t deadline = timer64_get_ticks() + timeout_ticks;
        int blocked_once = 0;
        for (;;) {
            if (cond(arg)) { rc = KWAIT64_OK; break; }
            if (timer64_get_ticks() >= deadline) { rc = KWAIT64_TIMEOUT; g_st.timeouts++; break; }
            if (ctx == KWAIT64_CTX_PROCESS) {
                blocked_once = 1;
                process64_block_on_deadline(chan, deadline);   // returns still under cli
            } else {
                g_st.boot_hlt_rounds++;
                __asm__ volatile ("sti; hlt" ::: "memory");     // the sti shadow: no interrupt slips in before hlt
                __asm__ volatile ("cli" ::: "memory");
            }
        }
        if (rc == KWAIT64_OK) { if (blocked_once) g_st.blocked++; else g_st.immediate++; }
    }
    if (flags & 0x200) __asm__ volatile ("sti" ::: "memory");   // restore the caller's interrupt state, no more
    return rc;
}
