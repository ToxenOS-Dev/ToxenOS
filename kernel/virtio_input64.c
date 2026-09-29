// kernel/virtio_input64.c — M+7A: VirtIO-input driver, C-side bridge.
// See include/virtio_input64.h for the full design rationale.
#include <stdint.h>
#include "../include/virtio_input64.h"
#include "../include/virtio_pci64.h"
#include "../include/pci64.h"
#include "../include/input64.h"
#include "../include/klog.h"
#include "../include/irq64.h"
#include "../include/virtio_irq64.h"
#include "../include/timer64.h"

static int g_ready = 0;
static uint32_t g_prev_buttons = 0;
static uint32_t g_poll_while_live = 0;      // timer poll bodies reached while the queue is IRQ-owned: MUST stay 0
static uint32_t g_samples_emitted = 0;

#if defined(VIRTIO_IRQ_FORCE_POLL)
#define INPUT_FORCE_POLL 1
#else
#define INPUT_FORCE_POLL 0
#endif
#ifdef VIRTIO_IRQ_FAIL_AT
#define INPUT_FAIL_AT VIRTIO_IRQ_FAIL_AT
#else
#define INPUT_FAIL_AT 0
#endif

int virtio_input64_available(void) { return g_ready; }

void virtio_input64_debug_stats_report(void) {
    uint64_t stats[3];
    if (toxenos_virtio_input_debug_stats(stats) < 0) {
        klog("virtio_input64: debug_stats: not available\n");
        return;
    }
    klog("virtio_input64: debug_stats ---\n");
    klog_hex("  received: ", (uint32_t)stats[0]);
    klog_hex("  recycled: ", (uint32_t)stats[1]);
    klog_hex("  polls:    ", (uint32_t)stats[2]);
    if (stats[0] != stats[1]) {
        klog("virtio_input64: WARNING -- received != recycled, receive queue is leaking\n");
    }
    klog("virtio_input64: debug_stats end ---\n");
}

int virtio_input64_selftest(void) {
    int r = toxenos_virtio_input_selftest_ring_wrap();
    klog("virtio_input64_selftest: ring-index wraparound arithmetic ");
    klog(r ? "PASS\n" : "FAIL\n");
    return r;
}

static int in_begin(void* info) { return toxenos_virtio_input_init_begin((const virtio_pci64_transport_info_t*)info); }
static int in_queues(const uint32_t* vec, int n) { (void)n; return toxenos_virtio_input_init_queues(vec[0]); }
static int in_finish(void) { return toxenos_virtio_input_init_finish(); }
static void in_abort(void) { toxenos_virtio_input_init_abort(); }
static int in_poll_init(void* info) { return toxenos_virtio_input_init((const virtio_pci64_transport_info_t*)info); }

// One decoded sample (one per SYN_REPORT) -> input64 events, with the existing button-diff logic.
static void emit_sample(int32_t x, int32_t y, uint32_t buttons) {
    g_samples_emitted++;
    input64_event_t ev = {0};
    ev.type    = INPUT64_EVENT_POINTER_ABS;
    ev.a       = x;
    ev.b       = y;
    ev.buttons = buttons;
    input64_push(&ev);
    if (buttons != g_prev_buttons) {
        static const uint32_t bits[3] = { INPUT64_BTN_LEFT, INPUT64_BTN_RIGHT, INPUT64_BTN_MIDDLE };
        uint32_t changed = buttons ^ g_prev_buttons;
        for (int i = 0; i < 3; i++) {
            if (!(changed & bits[i])) continue;
            input64_event_t bev = {0};
            bev.type    = INPUT64_EVENT_POINTER_BUTTON;
            bev.a       = (int32_t)bits[i];
            bev.pressed = (buttons & bits[i]) ? 1u : 0u;
            bev.buttons = buttons;
            input64_push(&bev);
        }
        g_prev_buttons = buttons;
    }
}

static void flush_samples(void) {
    int32_t x, y; uint32_t b;
    for (int i = 0; i < 32 && toxenos_virtio_input_take_sample(&x, &y, &b) > 0; i++) emit_sample(x, y, b);
}

// The eventq MSI-X interrupt (hard IRQ): bounded, allocation-free. Drains EVERY visible completion
// (at most the 16 posted buffers per pass; another pass if a pass was full), then converts each
// SYN_REPORT sample into input64 events.
irq64_ret_t virtio_input64_irq(void* ctx) {
    (void)ctx;
    int total = 0;
    for (int pass = 0; pass < 4; pass++) {
        int n = toxenos_virtio_input_irq(pass == 0);
        if (n < 0) {
            if (toxenos_virtio_input_is_faulted()) input64_set_abs_pointer_active(0, 0);
            return total > 0 ? IRQ64_RET_HANDLED : IRQ64_RET_NONE;
        }
        total += n;
        flush_samples();
        if (n < 16) break;
    }
    return total > 0 ? IRQ64_RET_HANDLED : IRQ64_RET_NONE;
}

int virtio_input64_init(void) {
    pci64_device_t* dev = pci64_find_device(PCI64_VENDOR_VIRTIO, PCI64_DEVICE_VIRTIO_INPUT_MODERN, 0);
    if (!dev) {
        klog("virtio_input64: no modern VirtIO-input device found -- PS/2 remains the sole pointer source\n");
        return -1;
    }
    pci64_enable_device(dev);

    virtio_pci64_transport_info_t info;
    if (virtio_pci64_probe(dev, &info) < 0 || !info.ok) {
        klog("virtio_input64: device has no usable modern transport\n");
        return -1;
    }

    // M+11C: interrupt-driven setup with reset-and-retry POLL fallback (see include/virtio_irq64.h).
    static const virtio_dev_ops_t in_ops = { in_begin, in_queues, in_finish, in_abort, in_poll_init, 0 };
    irq64_handler_t handlers[1] = { virtio_input64_irq };
    const char* names[1] = { "virtio-input" };
    int rc = virtio_irq_init_device(virtio_irq_dev(VIRTIO_DEV_INPUT), VIRTIO_DEV_INPUT, dev, &info, 1, handlers, names,
                                    &virtio_pci_real_ops, &in_ops, INPUT_FORCE_POLL, INPUT_FAIL_AT);
    if (rc < 0) {
        klog("virtio_input64: Rust driver init failed\n");
        return -1;
    }
    if (rc == 1) timer64_set_poll_hook(virtio_input64_poll);   // POLL-owned: the timer is the completion owner
    else         timer64_set_poll_hook(0);                     // IRQ-owned: NO timer involvement

    int32_t min_x, max_x, min_y, max_y;
    if (toxenos_virtio_input_get_abs_range(&min_x, &max_x, &min_y, &max_y) < 0) {
        // A real device without usable ABS_INFO (e.g. a plain
        // virtio-mouse, relative-only) -- the eventq is live and
        // polling is harmless, but there is no absolute range to drive
        // a POINTER_ABS/pointer-source-preference decision with, so
        // this driver stays passive: registered as available for
        // diagnostics, but never claims the pointer away from PS/2.
        klog("virtio_input64: device has no absolute axes -- staying passive, PS/2 remains preferred\n");
        g_ready = 1;
        return 0;
    }

    static input64_device_t dev_desc;
    dev_desc.name[0]='v'; dev_desc.name[1]='i'; dev_desc.name[2]='r'; dev_desc.name[3]='t';
    dev_desc.name[4]='i'; dev_desc.name[5]='o'; dev_desc.name[6]='p'; dev_desc.name[7]=0;
    dev_desc.kind = INPUT64_DEVICE_VIRTIO_POINTER;
    dev_desc.capabilities = INPUT64_CAP_ABS | INPUT64_CAP_KEY;
    dev_desc.abs_range.min_x = min_x; dev_desc.abs_range.max_x = max_x;
    dev_desc.abs_range.min_y = min_y; dev_desc.abs_range.max_y = max_y;
    input64_register_device(&dev_desc);

    // §8's policy: a working absolute VirtIO pointer is preferred over
    // PS/2 the moment it's confirmed live -- see include/input64.h's
    // own header comment on input64_set_abs_pointer_active(). Decided
    // once, here, at boot -- never re-arbitrated live this milestone.
    input64_set_abs_pointer_active(1, &dev_desc.abs_range);

    g_ready = 1;
    klog("virtio_input64: absolute pointer device live -- preferred over PS/2 for pointer motion\n");
    return 0;
}

void virtio_input64_poll(void) {
    if (!g_ready) return;
    if (toxenos_virtio_input_owner() != 0) {          // POLL_OWNER only
        g_poll_while_live++;                           // an ownership violation: counted (asserted 0), never drained
        return;
    }
    toxenos_virtio_input_poll();
    flush_samples();
}

void virtio_input64_health_probe(void) {
    if (g_ready) (void)toxenos_virtio_input_health_probe();
}

// The boot self-check deliberately attempts ONE forbidden POLL drain of the IRQ-owned queue; the driver
// must refuse it and count it. `g_expected_wrong_owner` lets the report separate that deliberate refusal
// from any real (unexpected) ownership violation.
static uint32_t g_expected_wrong_owner = 0;
int virtio_input64_owner_selfcheck(void) {
    int r = toxenos_virtio_input_test_wrong_owner_poll();
    if (r == 1) g_expected_wrong_owner++;
    return r;
}

void virtio_input64_irq_report(void) {
    uint64_t st[14];
    virtio_irq_dev_t* d = virtio_irq_dev(VIRTIO_DEV_INPUT);
    if (!g_ready || toxenos_virtio_input_irq_stats(st) < 0) return;
    klog("virtio_input64: irq counters ---\n");
    klog(d->mode == VIRTIO_IRQ_LIVE     ? "  mode: IRQ (MSI-X)\n"
       : d->mode == VIRTIO_IRQ_FAULTED  ? "  mode: FAULTED\n"
       : d->set                         ? "  mode: POLL (sealed IRQ set, quiesced)\n"
       :                                  "  mode: POLL\n");
    klog_hex("  irq_count:            ", (uint32_t)st[0]);
    klog_hex("  completions_drained:  ", (uint32_t)st[1]);
    klog_hex("  max_completions/irq:  ", (uint32_t)st[2]);
    klog_hex("  empty_irqs:           ", (uint32_t)st[3]);
    klog_hex("  samples_emitted:      ", (uint32_t)st[9]);
    klog_hex("  sample_overflow:      ", (uint32_t)st[10]);
    klog_hex("  timer_poll_calls:     ", (uint32_t)st[13]);
    klog_hex("  timer_polls_while_live:", g_poll_while_live);
    klog_hex("  wrong_owner_count:    ", (uint32_t)st[6]);
    klog_hex("  wrong_owner_unexpected:", (uint32_t)(st[6] > g_expected_wrong_owner ? st[6] - g_expected_wrong_owner : 0));
    klog_hex("  stale_irqs:           ", (uint32_t)st[5]);
    klog_hex("  received/recycled:    ", (uint32_t)st[11]);
    klog_hex("                        ", (uint32_t)st[12]);
    klog("virtio_input64: irq counters end ---\n");
}

// VIRTIO_IRQ_TEST_DEMOTE_INPUT: demote the IRQ-live input device on demand (verified-silence proof
// P1/P2/P3, or FAULTED) and report which one; the tablet then either keeps delivering through timer
// polling (proof) or the device is offline and PS/2 relative input takes over (FAULTED).
void virtio_input64_irq_test_demote(void) {
    virtio_irq_dev_t* d = virtio_irq_dev(VIRTIO_DEV_INPUT);
    if (!g_ready || !d->set || d->mode != VIRTIO_IRQ_LIVE) { klog("virtio_input64: demote test: device not IRQ-live, skipped\n"); return; }
    klog("virtio_input64: TEST: demoting the IRQ-live input device\n");
    int proof = toxenos_virtio_input_demote();
    klog_hex("virtio_input64: demote test: proof=", (uint32_t)proof);
    klog_hex("virtio_input64: demote test: owner=", (uint32_t)toxenos_virtio_input_owner());
    klog(d->mode == VIRTIO_IRQ_POLLING ? "virtio_input64: demote test: device demoted to POLLING (verified silent)\n"
        : d->mode == VIRTIO_IRQ_FAULTED ? "virtio_input64: demote test: device FAULTED (silence could not be verified)\n"
        : "virtio_input64: demote test: device still LIVE (unexpected)\n");
    if (d->mode == VIRTIO_IRQ_FAULTED)
        klog(toxenos_virtio_input_is_faulted() == 1 ? "virtio_input64: demote test: driver faulted flag set\n"
                                                     : "virtio_input64: demote test: *** DRIVER FAULTED FLAG NOT SET ***\n");
    if (d->mode == VIRTIO_IRQ_POLLING) {
        int s0 = toxenos_virtio_input_read_selector();
        const irq64_desc_t* e0 = irq64_get_desc(d->irq[0]);
        klog_hex("virtio_input64: demote test: selector=", (uint32_t)s0);
        klog(s0 == 0xFFFF ? "virtio_input64: demote test: queue selector reads NO_VECTOR\n"
                          : "virtio_input64: demote test: *** QUEUE SELECTOR IS STILL MAPPED ***\n");
        klog((e0 && e0->masked) ? "virtio_input64: demote test: MSI-X entry masked\n"
                                : "virtio_input64: demote test: MSI-X entry not masked (proof P2/P3)\n");
        // POLL_OWNER must actually be polled again: the timer hook is installed, and a poll is ALLOWED
        // by the ownership rule (counted as a real timer poll, not as a wrong-owner refusal).
        uint64_t st0[14], st1[14];
        (void)toxenos_virtio_input_irq_stats(st0);
        virtio_input64_poll();
        (void)toxenos_virtio_input_irq_stats(st1);
        klog(timer64_get_poll_hook() == virtio_input64_poll ? "virtio_input64: demote test: timer poll hook installed\n"
                                                             : "virtio_input64: demote test: *** NO TIMER POLL HOOK AFTER DEMOTION ***\n");
        klog((st1[13] == st0[13] + 1 && st1[6] == st0[6]) ? "virtio_input64: demote test: poll allowed by the ownership rule\n"
                                                           : "virtio_input64: demote test: *** POLL REFUSED AFTER DEMOTION ***\n");
    }
}
