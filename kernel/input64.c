// kernel/input64.c — Milestone 29: generic input-event queue.
// See include/input64.h for the design (single global bounded ring
// buffer, oldest-dropped overflow, scheduler-blocking consumer).
#include "../include/input64.h"
#include "../include/process64.h"
#include "../include/klog.h"

#define INPUT64_QUEUE_CAP 256

static input64_event_t queue[INPUT64_QUEUE_CAP];
static uint32_t head = 0, tail = 0, count = 0;
static uint32_t seq_counter = 0;
static uint32_t overflow_count = 0;
static uint32_t owner_pid = 0;     // 0 = unowned (real pids start at 1)
static uint8_t  wait_chan;          // dummy byte; its ADDRESS is the wait-channel identity

static inline uint64_t lock(void) {
    uint64_t flags;
    __asm__ volatile ("pushfq\n\tpop %0\n\tcli" : "=r"(flags) :: "memory");
    return flags;
}
static inline void unlock(uint64_t flags) {
    if (flags & (1ULL << 9)) __asm__ volatile ("sti" ::: "memory");
}

void input64_init(void) {
    head = tail = count = 0;
    seq_counter = 0;
    overflow_count = 0;
    owner_pid = 0;
}

void input64_push(const input64_event_t* ev) {
    uint64_t flags = lock();

    if (count == INPUT64_QUEUE_CAP) {
        // Overflow: drop the oldest buffered event to make room for
        // this one -- see include/input64.h's header comment for why.
        head = (head + 1) % INPUT64_QUEUE_CAP;
        count--;
        overflow_count++;
    }

    input64_event_t e = *ev;
    e.seq = ++seq_counter;
    queue[tail] = e;
    tail = (tail + 1) % INPUT64_QUEUE_CAP;
    count++;

    process64_wake_one(&wait_chan);
    unlock(flags);
}

int input64_read_blocking(input64_event_t* out) {
    uint64_t flags = lock();
    while (count == 0) {
        if (process64_current_pid() < 0) { unlock(flags); return -1; }
        process64_block_on(&wait_chan);
    }
    *out = queue[head];
    head = (head + 1) % INPUT64_QUEUE_CAP;
    count--;
    unlock(flags);
    return 0;
}

int input64_acquire(void) {
    uint64_t flags = lock();
    if (owner_pid != 0) { unlock(flags); return -1; }
    int pid = process64_current_pid();
    if (pid <= 0) { unlock(flags); return -1; }
    owner_pid = (uint32_t)pid;
    unlock(flags);
    return 0;
}

void input64_release(void) {
    uint64_t flags = lock();
    owner_pid = 0;
    unlock(flags);
}

void input64_stats(input64_stats_t* out) {
    if (!out) return;
    uint64_t flags = lock();
    out->queued         = count;
    out->capacity        = INPUT64_QUEUE_CAP;
    out->overflow_count  = overflow_count;
    out->owner_pid       = owner_pid;
    unlock(flags);
}

void input64_dump(void) {
    input64_stats_t s;
    input64_stats(&s);
    klog("input64: dump ---\n");
    klog_hex("  queued:    ", s.queued);
    klog_hex("  capacity:  ", s.capacity);
    klog_hex("  overflow:  ", s.overflow_count);
    klog_hex("  owner pid: ", s.owner_pid);
    klog("input64: dump end ---\n");
}
