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

// M+7A: device registry -- see include/input64.h's own header comment.
static input64_device_t* device_list = 0;

// M+7A: pointer-source policy state -- separate from device_list on
// purpose (a fast flag check, not a list walk, from IRQ12 context).
static int g_abs_pointer_active = 0;
static input64_abs_range_t g_abs_range = {0, 0, 0, 0};

int input64_register_device(input64_device_t* dev) {
    if (!dev) return -1;
    for (input64_device_t* d = device_list; d; d = d->next) {
        int same = 1;
        for (int i = 0; i < 16 && same; i++) {
            if (dev->name[i] != d->name[i]) same = 0;
            if (dev->name[i] == 0 && d->name[i] == 0) break;
        }
        if (same) return -1; // duplicate name
    }
    dev->next = device_list;
    device_list = dev;
    return 0;
}

input64_device_t* input64_find_device(const char* name) {
    for (input64_device_t* d = device_list; d; d = d->next) {
        int same = 1;
        for (int i = 0; i < 16; i++) {
            if (d->name[i] != name[i]) { same = 0; break; }
            if (d->name[i] == 0) break;
        }
        if (same) return d;
    }
    return 0;
}

input64_device_t* input64_iter_device(input64_device_t* prev) {
    return prev ? prev->next : device_list;
}

void input64_set_abs_pointer_active(int active, const input64_abs_range_t* range) {
    g_abs_pointer_active = active ? 1 : 0;
    if (active && range) g_abs_range = *range;
}

int input64_abs_pointer_active(void) { return g_abs_pointer_active; }

int input64_get_abs_range(input64_abs_range_t* out) {
    if (!g_abs_pointer_active || !out) return -1;
    *out = g_abs_range;
    return 0;
}

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

    // M+12B: was process64_wake_one -- IRQ-safe, but did not request an
    // IRQ-exit reschedule (see include/irq64.h's own stated convention:
    // "a device handler that wakes a waiter calls
    // irq64_request_reschedule()"), so a blocked reader became READY
    // immediately yet might not actually run until the next 100Hz timer
    // tick (~10ms) rather than promptly at this IRQ's exit. Both
    // keyboard64.c's and mouse64.c's callers are real IRQ handlers, so
    // process64_wake (documented IRQ-safe) is a safe drop-in that fixes
    // this -- needed for M+12B's WAIT_ANY-blocked compositor to actually
    // feel responsive rather than merely correct.
    process64_wake(&wait_chan);
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

int input64_try_read(input64_event_t* out) {
    uint64_t flags = lock();
    if (count == 0) { unlock(flags); return -2; }
    *out = queue[head];
    head = (head + 1) % INPUT64_QUEUE_CAP;
    count--;
    unlock(flags);
    return 0;
}

// M+12B: see include/input64.h's own header comments on these two.
int input64_ready(void) {
    uint64_t flags = lock();
    int ready = (count > 0);
    unlock(flags);
    return ready;
}

void* input64_wait_chan(void) { return &wait_chan; }

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
