// kernel/timer64.c — Milestone 2 minimal IRQ0 handler; Milestone 24 now
// reprograms the PIT to a real 100Hz instead of free-running at the
// legacy default (~18.2Hz), and drives the real process scheduler
// (kernel/process64.c) instead of the old ring-0 task demo. No
// fbterm_tick/net_poll calls — those don't exist/apply at this
// milestone. M+7A adds virtio_input64_poll() below -- a periodic,
// non-blocking receive-queue drain, same architectural shape the old
// 32-bit-only kernel/usb_hid.c already established for its own xHCI
// interrupt-transfer polling (kernel/timer.c's usb_hid_poll() call),
// just never ported to this 64-bit kernel until now. This remains the
// only "polling" this milestone's VirtIO-input driver does -- see that
// driver's own header comment on why this satisfies "no interrupt-
// driven queues" without ever busy-waiting.
#include <stdint.h>
#include "../include/klog.h"
#include "../include/process64.h"
#include "../include/virtio_input64.h"
#include "../include/irq64.h"
#include "../include/kwait64.h"
#include "../include/virtio_irq64.h"

#define PIT_CHANNEL0  0x40
#define PIT_COMMAND   0x43
#define PIT_BASE_FREQ 1193182

static inline void outb(uint16_t port, uint8_t value) {
    __asm__ volatile ("outb %0, %1" : : "a"(value), "Nd"(port));
}

static volatile uint64_t ticks64 = 0;

static irq64_ret_t timer64_irq(void* ctx);

// Milestone 24: programs PIT channel 0 for mode 3 (square wave) at the
// given frequency -- the 64-bit analogue of the 32-bit kernel/timer.c's
// timer_init(). Must run before `sti` (kernel/kernel64.c).
void timer64_init(uint32_t frequency) {
    uint32_t divisor = PIT_BASE_FREQ / frequency;
    outb(PIT_COMMAND, 0x36);
    outb(PIT_CHANNEL0, (uint8_t)(divisor & 0xFF));
    outb(PIT_CHANNEL0, (uint8_t)((divisor >> 8) & 0xFF));
    // M+11A: IRQ0 is requested here, after the PIT is programmed, so the
    // timer owns its own interrupt registration. In APIC mode the ISA IRQ0
    // -> GSI mapping (typically GSI2 via an Interrupt Source Override) is
    // resolved by the core, not assumed.
    irq64_request_legacy(0, timer64_irq, 0, "timer");
}

// M+4 second stale-line/lag investigation: define to klog a per-process
// scheduler-tick accounting report (kernel/process64.c's
// process64_dump_sched_accounting()) every 5 real seconds, from inside
// the timer IRQ itself -- deliberately NOT hooked into the kernel idle
// loop or any one process's own code path, since if any process (the
// compositor's own busy-poll, in particular) is permanently READY, the
// idle path may rarely or never run, making a report hooked there
// unreliable exactly when it matters most. Not left enabled by default.
// #define TIMER64_SCHED_ACCOUNTING_RUN 1

static void (*g_timer_test_hook)(void) = 0;
static void (*g_timer_poll_hook)(void) = 0;
void timer64_set_poll_hook(void (*fn)(void)) { g_timer_poll_hook = fn; }
void (*timer64_get_poll_hook(void))(void) { return g_timer_poll_hook; }
void timer64_set_test_hook(void (*fn)(void)) { g_timer_test_hook = fn; }

static void timer64_handler(void)
{
    ticks64++;
    if (g_timer_test_hook) g_timer_test_hook();     // self-test only (interrupt-context probes)
    if (ticks64 % 100 == 0)
        klog("[timer64] heartbeat\n");
#ifdef IRQ64_STATS_DUMP
    if (ticks64 % 1000 == 0)
        { irq64_dump(); virtio_irq64_report_full(); }     // M+11A/M+11C acceptance aid: per-IRQ counters every 10s (off by default)
#endif
    if (g_timer_poll_hook) g_timer_poll_hook();       // M+11C: only while a queue is POLL-owned (else NULL)
#ifdef VIRTIO_IRQ_DEBUG_WATCHDOG
    if (ticks64 % 100 == 0) virtio_input64_health_probe();   // debug builds only -- production has NO ring read here
#endif
#ifdef TIMER64_SCHED_ACCOUNTING_RUN
    if (ticks64 % 500 == 0)
        process64_dump_sched_accounting();
#endif
    if (ticks64 % PROCESS64_TICK_DIVISOR == 0)
        process64_tick();
}

// M+11A: thin irq64 thunk. The PIT is edge-triggered, so the core has
// already sent EOI before this runs -- required, because process64_tick()
// below may context-switch away and not return to this frame for a long
// time.
static irq64_ret_t timer64_irq(void* ctx)
{
    (void)ctx;
    timer64_handler();
    return IRQ64_RET_HANDLED;
}

// M-next: read-only accessor for SYS64_GET_TICKS (kernel/syscall64.c)
// -- see that syscall's own header comment for why this exists instead
// of a real sleep/yield primitive.
uint64_t timer64_get_ticks(void) {
    return ticks64;
}
