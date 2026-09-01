#ifndef KMUTEX64_H
#define KMUTEX64_H

#include <stdint.h>

// Milestone 28: the smallest possible SLEEPING mutex, built directly on
// kernel/process64.c's Milestone 26 generalized blocking primitives
// (process64_block_on/wake_one). Exists specifically because
// Milestone 27's TxFS64 locking (a plain pushfq/cli/restore-flags
// critical section, matching physmem64.c/heap64.c/pipe64.c/shm64.c)
// held interrupts disabled across the ENTIRE protected operation,
// including the actual disk I/O -- fine when that I/O was a handful of
// PIO port reads, but AHCI/NVMe/VirtIO-blk poll loops (Milestone 28)
// can legitimately take much longer, and holding cli for that whole
// span would stall the timer (no scheduling) and the keyboard (no IRQ1)
// system-wide. A kmutex64_t instead makes the CALLING PROCESS block via
// the real scheduler while contended -- interrupts stay enabled the
// entire time, so everything else keeps running.
//
// This is deliberately NOT a general-purpose synchronization subsystem:
// no recursion/re-entrancy, no priority inheritance, no read/write
// variants, no SMP support (ToxenOS is single-core; see
// kernel/process64.c's own critical-section precedent for why a plain
// cli-based lock stays sufficient for the SHORT bookkeeping sections
// kmutex64 itself uses internally). Callers that need to call one
// locking function from inside another already-locked one (the one
// case kernel/txfs64.c hit) must factor the inner call into a private
// "_nolock" helper instead -- see kernel/txfs64.c's txfs64_rmdir for
// the pattern -- since re-locking an already-held kmutex64_t from the
// SAME call stack would deadlock (no owner tracking to detect it).
typedef struct {
    volatile int locked;
    uint8_t wait_chan; // dummy byte; its ADDRESS is the wait-channel identity (see process64_block_on)
} kmutex64_t;

void kmutex64_init(kmutex64_t* m);

// Blocks (a real scheduler block -- process64_block_on -- never a
// busy-wait) until `m` is free, then claims it. Safe to call from
// kernel/idle context (current_idx < 0, e.g. early boot before any
// process exists) -- polls via hlt in that case instead, mirroring
// kernel/process64.c's own process64_wait() no-current-process
// fallback; in practice this path is only ever reached before any
// other process exists to hold the lock, so it never actually spins.
void kmutex64_lock(kmutex64_t* m);

// Releases `m` and wakes one blocked waiter, if any.
void kmutex64_unlock(kmutex64_t* m);

#endif // KMUTEX64_H
