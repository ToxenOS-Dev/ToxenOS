// kernel/virtio_irq64.c -- M+11C per-device VirtIO interrupt-mode state machine.
// See include/virtio_irq64.h for the design and the setup order.
#include <stdint.h>
#include "../include/virtio_irq64.h"
#include "../include/klog.h"
#include "../include/timer64.h"
#include "../include/input64.h"
#include "../include/virtio_input64.h"
#include "../include/virtio_gpu64.h"
#include "../include/kwait64.h"
#include "../include/process64.h"

#define NO_VECTOR 0xFFFFu

static virtio_irq_dev_t g_dev[VIRTIO_DEV_COUNT] = {
    { .name = "virtio-input" },
    { .name = "virtio-gpu" },
};

virtio_irq_dev_t* virtio_irq_dev(virtio_dev_t which) { return (which >= 0 && which < VIRTIO_DEV_COUNT) ? &g_dev[which] : 0; }

const char* virtio_irq_mode_name(virtio_irq_mode_t m) {
    switch (m) {
    case VIRTIO_IRQ_POLLING:   return "POLLING";
    case VIRTIO_IRQ_SWITCHING: return "SWITCHING";
    case VIRTIO_IRQ_LIVE:      return "LIVE";
    case VIRTIO_IRQ_QUIESCED:  return "QUIESCED";
    case VIRTIO_IRQ_FAULTED:   return "FAULTED";
    }
    return "?";
}

int virtio_irq_silence_proof(int p1_entries_masked_all, int p1_selectors_unmapped_all,
                             int p2_set_quiesced, int p3_reset_and_unmapped) {
    if (p1_entries_masked_all && p1_selectors_unmapped_all) return 1;
    if (p2_set_quiesced) return 2;
    if (p3_reset_and_unmapped) return 3;
    return 0;
}

// ── real PCI ops ─────────────────────────────────────────────────────
static int r_alloc(pci64_device_t* d, int n, pci64_irq_set_t** out) { return pci64_irq_alloc(d, n, n, PCI64_IRQ_MSIX, out); }
static pci_irqset_state_t r_state(const pci64_irq_set_t* s) { return pci64_irq_state(s); }
const virtio_pci_ops_t virtio_pci_real_ops = {
    r_alloc, pci64_irq_request, pci64_irq_prepare, pci64_irq_activate, pci64_irq_free, pci64_irq_quiesce,
    pci64_irq_vector, r_state,
};

// ── formatting ───────────────────────────────────────────────────────
static char* put_str(char* p, const char* s) { while (*s) *p++ = *s++; return p; }
static const char* dname(const virtio_irq_dev_t* d) { return (d && d->name) ? d->name : "virtio-?"; }
static char* put_dec(char* p, uint32_t v) {
    char t[12]; int n = 0;
    if (!v) t[n++] = '0';
    while (v) { t[n++] = (char)('0' + v % 10); v /= 10; }
    while (n) *p++ = t[--n];
    return p;
}
static char* put_hex2(char* p, uint32_t v) { const char* h = "0123456789abcdef"; *p++ = h[(v >> 4) & 0xF]; *p++ = h[v & 0xF]; return p; }
static char* put_bdf(char* p, const pci64_device_t* d) {
    p = put_hex2(p, d->bus); *p++ = ':'; p = put_hex2(p, d->slot); *p++ = '.'; *p++ = (char)('0' + (d->func & 7)); return p;
}

// ── setup orchestration ──────────────────────────────────────────────
static void log_fallback(virtio_irq_dev_t* d, int step, const char* why) {
    char line[160]; char* p = put_str(line, dname(d));
    p = put_str(p, ": MSI-X setup failed at step S"); p = put_dec(p, (uint32_t)step);
    p = put_str(p, " ("); p = put_str(p, why); p = put_str(p, ") -- falling back to POLLING\n");
    *p = 0; klog(line);
}

#define INJECT(n) (fail_at == (n))

int virtio_irq_init_device(virtio_irq_dev_t* d, virtio_dev_t which, pci64_device_t* pdev, void* info,
                           int nq, irq64_handler_t* handlers, const char** names,
                           const virtio_pci_ops_t* pci, const virtio_dev_ops_t* dev,
                           int force_poll, int fail_at) {
    (void)which;
    d->pdev = pdev; d->nq = nq; d->set = 0; d->fail_step = 0; d->last_proof = 0;
    d->mode = VIRTIO_IRQ_SWITCHING;
    for (int i = 0; i < VIRTIO_IRQ_MAX_Q; i++) { d->irq[i] = -1; d->entry[i] = NO_VECTOR; }

    int step = 0;
    const char* why = "";
    pci64_irq_set_t* set = 0;
    uint32_t vec[VIRTIO_IRQ_MAX_Q] = { NO_VECTOR, NO_VECTOR };

    if (force_poll) { why = "forced polling (compile-time)"; step = 0; goto fallback_quiet; }

    // S1: reset, ACKNOWLEDGE, DRIVER, features, FEATURES_OK
    step = 1; if (INJECT(1) || dev->begin(info) != 0) { why = "device negotiation"; goto fail_abort; }

    // S2: allocate the MSI-X vectors (one per queue; config stays NO_VECTOR)
    step = 2; if (INJECT(2) || pci->alloc(pdev, nq, &set) < nq || !set) { why = "pci64_irq_alloc"; set = 0; goto fail_abort; }
    for (int i = 0; i < nq; i++) {
        pci64_irq_vector_t pv;
        if (pci->vector(set, i, &pv) != 0) { why = "pci64_irq_vector"; goto fail_free; }
        d->irq[i] = pv.irq; d->entry[i] = pv.dev_index; vec[i] = pv.dev_index;
    }

    // S3: bind handlers (set BOUND); hardware still silent
    step = 3;
    if (INJECT(3)) { why = "handler binding"; goto fail_free; }
    for (int i = 0; i < nq; i++) if (pci->request(set, i, handlers[i], (void*)(uintptr_t)i, names[i]) != 0) { why = "pci64_irq_request"; goto fail_free; }

    // S4: ARMED -- MSI-X enabled, function masked, table programmed; nothing sealed
    step = 4; if (INJECT(4) || pci->prepare(set) != 0) { why = "pci64_irq_prepare"; goto fail_free; }

    // S5: queue_msix_vector written + read back, then queue_enable (config vector untouched)
    step = 5; if (INJECT(5) || dev->queues(vec, nq) != 0) { why = "queue vector programming/readback"; goto fail_free; }

    // S6: go live (INTx off, unmask entries, MASKALL cleared LAST)
    step = 6;
    if (INJECT(6) || pci->activate(set) != 0) {
        why = "pci64_irq_activate";
        if (pci->state(set) == PCI_IRQSET_ARMED) goto fail_free;      // pre-boundary: nothing escaped, exact rollback
        goto fail_sealed;                                             // boundary attempted: sealed, cannot be freed
    }

    // S7: DRIVER_OK (first notification only after this)
    step = 7;
    if (INJECT(7) || dev->finish() != 0) { why = "DRIVER_OK"; goto fail_sealed; }

    d->set = set;
    d->mode = VIRTIO_IRQ_LIVE;
    d->setup_result = 0;
    {
        char line[160]; char* p = put_str(line, dname(d)); p = put_str(p, " ");
        p = put_bdf(p, pdev); p = put_str(p, " MSI-X LIVE: ");
        for (int i = 0; i < nq; i++) {
            if (i) p = put_str(p, ", ");
            p = put_str(p, "queue"); p = put_dec(p, (uint32_t)i); p = put_str(p, "=entry"); p = put_dec(p, d->entry[i]);
            p = put_str(p, "/irq"); p = put_dec(p, (uint32_t)d->irq[i]);
        }
        p = put_str(p, "\n"); *p = 0; klog(line);
    }
    return 0;

fail_free:
    // Pre-live: the PCI set is free-able (BOUND/ARMED -> exact restore) and nothing was sealed.
    if (set) pci->free_set(set);
    set = 0;
    for (int i = 0; i < VIRTIO_IRQ_MAX_Q; i++) { d->irq[i] = -1; d->entry[i] = NO_VECTOR; }
fail_abort:
    dev->abort();                                   // reset: unmaps every vector, drops partial state
    d->fail_step = step; log_fallback(d, step, why);
    goto fallback_quiet;

fail_sealed:
    // The boundary was attempted: the set is sealed/pinned and cannot be freed. The device is reset
    // (which unmaps every vector) and the set is quiesced; polling resumes only when that is verified.
    d->fail_step = step; log_fallback(d, step, why);
    dev->abort();
    d->set = set;
    if (pci->quiesce(set) != 0 && pci->state(set) != PCI_IRQSET_QUIESCED) {
        d->mode = VIRTIO_IRQ_FAULTED; d->faulted++; d->setup_result = -1;
        char line[200]; char* p = put_str(line, "virtio_irq64: *** FAULTED *** "); p = put_str(p, dname(d));
        p = put_str(p, ": post-live setup failure could not be verified silent -- device stays offline, PCI IRQ set pinned\n"); *p = 0; klog(line);
        return -1;
    }
    // (falls through to the polling re-initialisation)

fallback_quiet:
    d->mode = VIRTIO_IRQ_POLLING;
    if (dev->poll_init(info) != 0) {
        d->setup_result = -1;
        char line[120]; char* p = put_str(line, "virtio_irq64: "); p = put_str(p, dname(d)); p = put_str(p, ": polling fallback initialisation FAILED\n"); *p = 0; klog(line);
        return -1;
    }
    d->setup_result = 1;
    return 1;
}

// ── Rust-facing helpers ──────────────────────────────────────────────
static virtio_irq_dev_t* dev_of(uint32_t dev) { return dev < VIRTIO_DEV_COUNT ? &g_dev[dev] : 0; }

int virtio_irq64_mask_all(uint32_t dev) {
    virtio_irq_dev_t* d = dev_of(dev);
    if (!d || !d->set) return 0;
#if defined(VIRTIO_IRQ_TEST_FAULT_DEMOTE) || defined(VIRTIO_IRQ_TEST_FAIL_MASK)
    return 0;                       // test builds: the per-entry mask cannot be verified -> proof P1 unavailable
#endif
    for (int i = 0; i < d->nq; i++)
        if (irq64_disable(d->irq[i]) != 0) return 0;
    return 1;
}

// Test build (VIRTIO_IRQ_TEST_FAIL_RESET): the device-reset proof P3 cannot be verified.
int virtio_irq64_test_fail_reset(void) {
#ifdef VIRTIO_IRQ_TEST_FAIL_RESET
    return 1;
#else
    return 0;
#endif
}

// Test build (VIRTIO_IRQ_TEST_FAIL_UNMAP): the queue_msix_vector -> NO_VECTOR unmap cannot be verified.
int virtio_irq64_test_fail_unmap(void) {
#ifdef VIRTIO_IRQ_TEST_FAIL_UNMAP
    return 1;
#else
    return 0;
#endif
}

int virtio_irq64_mask_one(uint32_t dev, uint32_t queue) {
    virtio_irq_dev_t* d = dev_of(dev);
    if (!d || !d->set || queue >= (uint32_t)d->nq) return -1;
    return irq64_disable(d->irq[queue]);
}

int virtio_irq64_quiesce_set(uint32_t dev) {
    virtio_irq_dev_t* d = dev_of(dev);
    if (!d || !d->set) return 0;
#ifdef VIRTIO_IRQ_TEST_FAULT_DEMOTE
    return 0;                       // test builds: the function mask cannot be verified -> proof P2 unavailable
#endif
    return pci64_irq_quiesce(d->set) == 0 ? 1 : 0;
}

// Events from Rust: mode bookkeeping + the ONE loud diagnostic per fault.
void virtio_irq64_note(uint32_t dev, uint32_t event) {
    virtio_irq_dev_t* d = dev_of(dev);
    if (!d) return;
    switch (event) {
    case 1: case 2: case 3:            // demoted with proof P1/P2/P3: device-wide, now POLLING
        d->demotions++; d->last_proof = (int)event; d->mode = VIRTIO_IRQ_POLLING;
        // POLL_OWNER again: the timer is the completion owner, so it must start polling now (the hook is
        // otherwise installed only by an init-time fallback). The GPU polls inline in its own waiter.
        if (dev == VIRTIO_DEV_INPUT) timer64_set_poll_hook(virtio_input64_poll);
        { char line[120]; char* p = put_str(line, dname(d)); p = put_str(p, ": IRQ path demoted to POLLING (verified silent, proof P");
          p = put_dec(p, event); p = put_str(p, ")\n"); *p = 0; klog(line); }
        break;
    case 4:                            // FAULTED
        d->faulted++; d->mode = VIRTIO_IRQ_FAULTED;
        if (dev == VIRTIO_DEV_INPUT) input64_set_abs_pointer_active(0, 0);   // PS/2 relative input resumes
        { char line[200]; char* p = put_str(line, "virtio_irq64: *** FAULTED *** "); p = put_str(p, dname(d));
          p = put_str(p, " -- interrupt silence could not be verified; device operations fail, polling NOT resumed, PCI IRQ set stays pinned\n"); *p = 0; klog(line); }
        break;
    case 5: d->storm_warn++; break;
    case 6: d->storm_masked++; break;
    case 7: d->lost_irq++; break;
    }
}

// ── reporting ────────────────────────────────────────────────────────
void virtio_irq64_report(void) {
    for (int k = 0; k < VIRTIO_DEV_COUNT; k++) {
        virtio_irq_dev_t* d = &g_dev[k];
        if (!d->pdev) continue;
        char line[200]; char* p = put_str(line, "virtio_irq64: "); p = put_str(p, dname(d)); p = put_str(p, " ");
        p = put_bdf(p, d->pdev); p = put_str(p, " mode="); p = put_str(p, virtio_irq_mode_name(d->mode));
        for (int i = 0; i < d->nq && d->set; i++) {
            const irq64_desc_t* q = irq64_get_desc(d->irq[i]);
            p = put_str(p, " q"); p = put_dec(p, (uint32_t)i); p = put_str(p, "[entry="); p = put_dec(p, d->entry[i]);
            p = put_str(p, " irq="); p = put_dec(p, (uint32_t)d->irq[i]);
            p = put_str(p, " vec=0x"); p = put_hex2(p, q ? (uint32_t)(q->vector & 0xFF) : 0); p = put_str(p, "]");
        }
        p = put_str(p, "\n"); *p = 0; klog(line);
    }
}

// Full interrupt-mode report: per-device summary, per-queue counters, scheduler/kwait statistics.
void virtio_irq64_report_full(void) {
    virtio_irq64_report();
    virtio_input64_irq_report();
    virtio_gpu64_irq_report();
    kwait64_stats_t k; kwait64_get_stats(&k);
    klog("virtio_irq64: scheduler / kwait statistics ---\n");
    klog_hex("  irq-exit reschedule switches: ", irq64_resched_switches());
    klog_hex("  process wakes:                ", process64_stat_wakes());
    klog_hex("  deadline wakes:               ", process64_stat_deadline_wakes());
    klog_hex("  kwait immediate:              ", k.immediate);
    klog_hex("  kwait blocked (process):      ", k.blocked);
    klog_hex("  kwait boot-hlt rounds:        ", k.boot_hlt_rounds);
    klog_hex("  kwait timeouts:               ", k.timeouts);
    klog_hex("  kwait wouldblock:             ", k.wouldblock);
    klog_hex("  kwait signals:                ", k.signals);
    klog("virtio_irq64: scheduler / kwait statistics end ---\n");
}
