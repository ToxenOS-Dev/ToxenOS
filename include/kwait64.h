#ifndef KWAIT64_H
#define KWAIT64_H

// M+11C: the ONE explicit condition-wait primitive drivers use to wait for a
// completion interrupt. A driver never hand-rolls its own sleep protocol.
//
//   kwait64_wait(chan, cond, arg, timeout_ticks)
//       atomically (against a completion IRQ):
//           evaluate cond   ->  mark the caller BLOCKED on chan  ->
//           arm a deadline  ->  scheduler hand-off
//       and returns KWAIT64_OK as soon as cond is true, KWAIT64_TIMEOUT once the
//       deadline passes with cond still false, or KWAIT64_WOULDBLOCK if the
//       calling context may not wait at all.
//
//   kwait64_signal(chan)
//       called by the completion side AFTER it updated the state cond reads.
//
// `cond` runs with interrupts disabled: side-effect free, no blocking.
//
// Where a wait is legal (classification, see kwait64_classify):
//   PROCESS   a real process, not in an interrupt handler, not preempt-disabled:
//             blocks through the scheduler. (Syscalls enter through an interrupt
//             gate, so IF=0 at entry is the NORMAL state of a blockable process;
//             the non-blockable contexts are the ones tracked explicitly below.)
//   BOOT      idle/boot context (no process) that entered with IF=1, i.e. where
//             interrupts are intentionally permitted: check with interrupts off,
//             `sti; hlt`, `cli`, recheck (the sti/hlt adjacency cannot lose a
//             wake-up). The caller's interrupt state is restored on return.
//   WOULDBLOCK
//             an interrupt handler; a preempt-disabled section (e.g. exit-time
//             cleanup); or the idle/boot context entered with IF=0 (a genuine
//             critical section -- kwait never secretly enables interrupts).
//
// Why each race terminates (with cond true/false as the completion updates it):
//   completion before the wait      -> the first cond check succeeds.
//   completion during the handoff   -> interrupts are off from the check until the
//                                      switch, so the message stays pending in the
//                                      LAPIC; the waiter is already BLOCKED when it
//                                      is delivered, so signal finds and wakes it.
//   completion after the block      -> signal wakes the blocked waiter.
//   timeout racing a completion     -> on resume cond is re-evaluated BEFORE the
//                                      deadline, so a completion that landed always
//                                      wins; only cond==false && now>=deadline times out.
#include <stdint.h>

#define KWAIT64_OK          0
#define KWAIT64_TIMEOUT     (-110)
#define KWAIT64_WOULDBLOCK  (-11)

typedef int (*kwait64_cond_fn)(void* arg);

typedef enum {
    KWAIT64_CTX_PROCESS = 0,
    KWAIT64_CTX_BOOT,
    KWAIT64_CTX_WOULDBLOCK,
} kwait64_ctx_t;

// Pure classification (inputs are the facts about the calling context).
kwait64_ctx_t kwait64_classify(int in_irq, int has_process, int preempt_disabled, int entered_if_set);

// 1 if kwait64_wait() from the CURRENT context would wait (PROCESS or BOOT), 0 if it would return
// KWAIT64_WOULDBLOCK. No side effects. Drivers call it BEFORE submitting a request.
int  kwait64_can_wait(void);

int  kwait64_wait(void* chan, kwait64_cond_fn cond, void* arg, uint64_t timeout_ticks);
void kwait64_signal(void* chan);

// Stats (per system): waits satisfied without blocking, blocked in a process,
// hlt-waited in the boot context, timeouts, would-blocks.
typedef struct {
    uint32_t immediate, blocked, boot_hlt_rounds, timeouts, wouldblock, signals;
} kwait64_stats_t;
void kwait64_get_stats(kwait64_stats_t* out);

// Self-test hook: called inside a timer interrupt (interrupt context).
void timer64_set_test_hook(void (*fn)(void));

#endif // KWAIT64_H
