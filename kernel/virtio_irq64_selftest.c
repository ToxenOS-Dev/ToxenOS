// kernel/virtio_irq64_selftest.c -- M+11C deterministic boot-time tests:
// kwait classification/atomicity, reschedule-on-IRQ-exit decisions, VirtIO
// setup orchestration with fake PCI/device ops (every failure step, post-live
// failure, unverifiable silence -> FAULTED, forced polling), the silence-proof
// decision, and the Rust-side ring/ownership/input/GPU-completion suites.
#include <stdint.h>
#include "../include/virtio_irq64.h"
#include "../include/kwait64.h"
#include "../include/process64.h"
#include "../include/irq64.h"
#include "../include/timer64.h"
#include "../include/klog.h"
#include "../include/virtio_input64.h"
#include "../include/virtio_gpu64.h"

extern uint32_t toxenos_virtio_irq_selftest_pci(void);
extern uint32_t toxenos_virtio_irq_selftest_input(void);
extern uint32_t toxenos_virtio_irq_selftest_gpu(void);
extern uint32_t toxenos_virtio_irq_selftest_assertions(void);

static int g_pass, g_fail, g_group_fail;
static void check(int cond, const char* what) {
    if (cond) { g_pass++; return; }
    g_fail++; g_group_fail++;
    klog("  virtio_irq64 selftest FAIL: "); klog(what); klog("\n");
}
#define CHECK(c) check((c), #c)
static void group_begin(void) { g_group_fail = 0; }
static void group_end(const char* name) {
    klog(g_group_fail ? "virtio_irq64 selftest FAIL: " : "virtio_irq64 selftest PASS: ");
    klog(name); klog("\n");
}

// ═════════════════════ 1. kwait: classification + waits ═══════════════════
static int cond_true(void* a)  { (void)a; return 1; }
static int cond_false(void* a) { (void)a; return 0; }
static int cond_ticks(void* a) { return timer64_get_ticks() >= *(uint64_t*)a; }

// cond evaluations must ALWAYS run with interrupts off (atomic against the completion IRQ): record any
// evaluation that ran with IF=1.
static volatile int g_cond_calls, g_cond_saw_if;
static void note_if(void) { uint64_t f; __asm__ volatile ("pushfq; pop %0" : "=r"(f)); g_cond_calls++; if (f & 0x200) g_cond_saw_if = 1; }
static int cond_rec_ticks(void* a) { note_if(); return timer64_get_ticks() >= *(uint64_t*)a; }
static int cond_rec_false(void* a) { (void)a; note_if(); return 0; }

static volatile int g_irq_probe_armed, g_irq_probe_done, g_irq_probe_rc, g_irq_probe_can, g_irq_probe_nested = -1;
static void irq_probe_hook(void) {
    if (g_irq_probe_armed) {
        g_irq_probe_armed = 0;
        { trapframe64_t tf; for (unsigned i = 0; i < sizeof tf; i++) ((volatile char*)&tf)[i] = 0; tf.cs = 0x08;
          irq64_exit_ctx_t c; irq64_exit_build_ctx(&tf, &c); g_irq_probe_nested = c.nested; }   // we ARE inside a handler here
        g_irq_probe_can = kwait64_can_wait();
        g_irq_probe_rc = kwait64_wait((void*)&g_irq_probe_rc, cond_false, 0, 5);   // from INSIDE the timer interrupt
        g_irq_probe_done = 1;
    }
}

static void test_kwait(void) {
    group_begin();
    // pure classification table
    CHECK(kwait64_classify(1, 1, 0, 1) == KWAIT64_CTX_WOULDBLOCK);   // interrupt handler, even if it interrupted a process
    CHECK(kwait64_classify(1, 0, 0, 1) == KWAIT64_CTX_WOULDBLOCK);
    CHECK(kwait64_classify(0, 1, 1, 1) == KWAIT64_CTX_WOULDBLOCK);   // preempt-disabled (exit cleanup)
    CHECK(kwait64_classify(0, 1, 0, 0) == KWAIT64_CTX_PROCESS);      // process entered with IF=0 (syscall gate): blockable
    CHECK(kwait64_classify(0, 1, 0, 1) == KWAIT64_CTX_PROCESS);
    CHECK(kwait64_classify(0, 0, 0, 1) == KWAIT64_CTX_BOOT);         // idle/boot context, interrupts permitted
    CHECK(kwait64_classify(0, 0, 0, 0) == KWAIT64_CTX_WOULDBLOCK);   // idle/boot with IF=0: genuine critical section

    kwait64_stats_t s0, s1; kwait64_get_stats(&s0);
    uint64_t entry_if; __asm__ volatile ("pushfq; pop %0" : "=r"(entry_if));
    entry_if &= 0x200;
    if (entry_if) {
        // boot wait with interrupts allowed: completion BEFORE the wait -> no hlt at all
        CHECK(kwait64_wait((void*)&s0, cond_true, 0, 10) == KWAIT64_OK);
        kwait64_get_stats(&s1);
        CHECK(s1.boot_hlt_rounds == s0.boot_hlt_rounds && s1.immediate == s0.immediate + 1);
        CHECK(kwait64_can_wait() == 1);                              // boot context with IF=1: waiting is allowed
        // completion AFTER the block: a real timer interrupt satisfies it. Every cond evaluation (before the
        // hlt, and the recheck after it) must have run with interrupts OFF.
        g_cond_calls = 0; g_cond_saw_if = 0;
        uint64_t t0 = timer64_get_ticks(), target = t0 + 3;
        CHECK(kwait64_wait((void*)&target, cond_rec_ticks, &target, 50) == KWAIT64_OK);
        CHECK(timer64_get_ticks() >= target);
        CHECK(g_cond_calls >= 2 && !g_cond_saw_if);                  // checked at least before AND after the hlt, never with IF=1
        kwait64_get_stats(&s1);
        CHECK(s1.boot_hlt_rounds > s0.boot_hlt_rounds);              // it really did sti;hlt
        // timeout: cond never true
        g_cond_calls = 0; g_cond_saw_if = 0;
        t0 = timer64_get_ticks();
        CHECK(kwait64_wait((void*)&s0, cond_rec_false, 0, 3) == KWAIT64_TIMEOUT);
        uint64_t el = timer64_get_ticks() - t0;
        CHECK(el >= 3 && el <= 6);
        CHECK(g_cond_calls >= 2 && !g_cond_saw_if);
        // timeout racing a completion: the completion is in place at the deadline tick -> completion wins
        t0 = timer64_get_ticks(); target = t0 + 3;
        CHECK(kwait64_wait((void*)&target, cond_ticks, &target, 3) == KWAIT64_OK);   // deadline == completion tick: cond checked first
        // the caller's interrupt state is restored (still enabled)
        uint64_t f; __asm__ volatile ("pushfq; pop %0" : "=r"(f));
        CHECK((f & 0x200) != 0);
        // IRQ handler -> WOULDBLOCK (a real wait attempted from inside the timer interrupt)
        kwait64_get_stats(&s0);
        g_irq_probe_done = 0; g_irq_probe_can = -1; g_irq_probe_armed = 1;
        timer64_set_test_hook(irq_probe_hook);
        for (uint64_t spin = 0; !g_irq_probe_done && spin < 2000000; spin++) __asm__ volatile ("pause");
        timer64_set_test_hook(0);
        CHECK(g_irq_probe_done && g_irq_probe_rc == KWAIT64_WOULDBLOCK);
        CHECK(g_irq_probe_can == 0);                                 // the driver-side pre-check agrees: no submission from an IRQ
        CHECK(g_irq_probe_nested == 1);                              // the exit-context builder sees the live IRQ nesting depth
        kwait64_get_stats(&s1);
        CHECK(s1.wouldblock == s0.wouldblock + 1);
        // IF=0 non-blockable caller -> WOULDBLOCK, and kwait does NOT enable interrupts behind its back
        kwait64_get_stats(&s0);
        __asm__ volatile ("cli" ::: "memory");
        uint64_t tk = timer64_get_ticks();
        int can = kwait64_can_wait();
        int rc = kwait64_wait((void*)&s0, cond_false, 0, 100);
        uint64_t f2; __asm__ volatile ("pushfq; pop %0" : "=r"(f2));
        CHECK(rc == KWAIT64_WOULDBLOCK && can == 0 && !(f2 & 0x200) && timer64_get_ticks() == tk);
        __asm__ volatile ("sti" ::: "memory");
        kwait64_get_stats(&s1);
        CHECK(s1.wouldblock == s0.wouldblock + 1);
    }
    // preempt-disabled section -> WOULDBLOCK
    kwait64_get_stats(&s0);
    process64_preempt_disable();
    CHECK(process64_preempt_disabled() == 1 && !process64_can_block() && kwait64_can_wait() == 0);
    CHECK(kwait64_wait((void*)&s0, cond_true, 0, 10) == KWAIT64_WOULDBLOCK);
    process64_preempt_enable();
    CHECK(process64_preempt_disabled() == 0);
    kwait64_get_stats(&s1);
    CHECK(s1.wouldblock == s0.wouldblock + 1);
    // (A normal PROCESS wait that really blocks and is woken by the completion interrupt is exercised, with
    //  counters, by the live GPU command traffic: see "kwait blocked (process)" in the acceptance report.)
    group_end("kwait: context classification, boot wait (before/after/timeout/racing), IF=0 and IRQ callers -> WOULDBLOCK, no secret sti");
}

// ═════════════════════ 2. reschedule on IRQ exit ═════════════════════════
static int g_switch_calls;
static void fake_switch(void) { g_switch_calls++; }

static void test_resched(void) {
    group_begin();
    irq64_exit_ctx_t user   = { 1, 1, 0, 0 };      // interrupted user code of a process
    irq64_exit_ctx_t idle   = { 0, 0, 0, 0 };      // interrupted the idle/boot context
    irq64_exit_ctx_t kproc  = { 0, 1, 0, 0 };      // interrupted a process while it ran kernel code
    irq64_exit_ctx_t nopre  = { 1, 1, 0, 1 };      // preempt-disabled section
    irq64_exit_ctx_t inswch = { 1, 1, 1, 0, 0 };   // scheduler mid-switch
    irq64_exit_ctx_t nested = { 1, 1, 0, 0, 1 };   // this IRQ interrupted another IRQ handler
    irq64_exit_ctx_t nested_idle = { 0, 0, 0, 0, 1 };
    CHECK(irq64_exit_may_resched(&user) == 1);
    CHECK(irq64_exit_may_resched(&idle) == 1);
    CHECK(irq64_exit_may_resched(&kproc) == 0);
    CHECK(irq64_exit_may_resched(&nopre) == 0);
    CHECK(irq64_exit_may_resched(&inswch) == 0);
    CHECK(irq64_exit_may_resched(&nested) == 0 && irq64_exit_may_resched(&nested_idle) == 0);   // only the OUTERMOST exit may switch

    irq64_clear_need_resched(); g_switch_calls = 0;
    uint32_t sw0 = irq64_resched_switches();
    CHECK(irq64_exit_resched_point(&user, fake_switch) == 0 && g_switch_calls == 0);   // nothing requested: never switches
    // a wake requested a reschedule; the IRQ interrupted user code -> IMMEDIATE eligible switch, flag consumed
    irq64_request_reschedule();
    CHECK(irq64_need_resched() == 1);
    CHECK(irq64_exit_resched_point(&user, fake_switch) == 1 && g_switch_calls == 1 && irq64_need_resched() == 0);
    // need_resched != unconditional switch: illegal points leave it PENDING ...
    irq64_request_reschedule();
    CHECK(irq64_exit_resched_point(&kproc, fake_switch) == 0 && irq64_need_resched() == 1 && g_switch_calls == 1);
    CHECK(irq64_exit_resched_point(&nopre, fake_switch) == 0 && irq64_need_resched() == 1);
    CHECK(irq64_exit_resched_point(&inswch, fake_switch) == 0 && irq64_need_resched() == 1);
    CHECK(irq64_exit_resched_point(&nested, fake_switch) == 0 && irq64_need_resched() == 1);
    // ... and it survives until a LEGAL scheduling point, where it is consumed exactly once
    CHECK(irq64_exit_resched_point(&user, fake_switch) == 1 && g_switch_calls == 2 && irq64_need_resched() == 0);
    CHECK(irq64_exit_resched_point(&user, fake_switch) == 0 && g_switch_calls == 2);
    CHECK(irq64_resched_switches() == sw0 + 2);
    // IRQ while NO process is schedulable: the idle context is a legal point, but the scheduler helper must
    // decline to switch when nothing is READY (called for real here: the boot context, no processes yet).
    if (process64_current_pid() < 0) {
        irq64_request_reschedule();
        irq64_exit_resched_point(&idle, process64_irq_exit_reschedule);
        CHECK(process64_current_pid() < 0);                   // still the boot context: no unsafe/phantom switch
    }
    irq64_clear_need_resched();

    // ── the dispatch glue's context builder, driven with synthetic frames ──
    {
        trapframe64_t tf; irq64_exit_ctx_t c;
        for (unsigned i = 0; i < sizeof tf; i++) ((volatile char*)&tf)[i] = 0;
        tf.cs = 0x08;                                                    // ring 0
        irq64_exit_build_ctx(&tf, &c);
        CHECK(!c.from_user && !c.nested && !c.switching && !c.preempt_disabled && c.has_current == (process64_current_pid() >= 0));
        tf.cs = 0x1B;                                                    // ring 3 (user CS | RPL 3)
        irq64_exit_build_ctx(&tf, &c);
        CHECK(c.from_user && !c.nested);
        process64_preempt_disable();
        irq64_exit_build_ctx(&tf, &c);
        CHECK(c.preempt_disabled == 1);                                  // live preempt depth, not a constant
        process64_preempt_disable();
        irq64_exit_build_ctx(&tf, &c);
        CHECK(c.preempt_disabled == 2);
        process64_preempt_enable(); process64_preempt_enable();
        irq64_exit_build_ctx(&tf, &c);
        CHECK(c.preempt_disabled == 0);
    }

    // ── the same decisions through REAL interrupts (timer, 100 Hz): the dispatch glue must build the
    //    context from the live machine state (not only the pure predicate being right) ──
    {
        uint64_t f; __asm__ volatile ("pushfq; pop %0" : "=r"(f));
        if (f & 0x200) {
            irq64_exit_ctx_t last; uint32_t dec0, dec1;
            irq64_get_last_exit_ctx(&last, &dec0);
            // (a) preempt-DISABLED section: a wake requested a reschedule; the interrupt must NOT act on it
            process64_preempt_disable();
            irq64_request_reschedule();
            uint32_t sw_before = irq64_resched_switches();
            for (uint64_t t0 = timer64_get_ticks(); timer64_get_ticks() < t0 + 3; ) __asm__ volatile ("pause");
            irq64_get_last_exit_ctx(&last, &dec1);
            CHECK(dec1 > dec0);                                               // real IRQ exits evaluated the request...
            CHECK(last.preempt_disabled == 1 && !last.from_user && !last.switching);   // ...from the live preempt depth
            CHECK(irq64_need_resched() == 1 && irq64_resched_switches() == sw_before);  // ...and it stayed PENDING, no switch
            process64_preempt_enable();
            // (b) legal point (idle/boot context, preemption enabled): consumed by the next real IRQ exit,
            //     and with nothing schedulable the helper must not switch (still the boot context)
            int had_cur = process64_current_pid();
            for (uint64_t t0 = timer64_get_ticks(); irq64_need_resched() && timer64_get_ticks() < t0 + 5; ) __asm__ volatile ("pause");
            CHECK(irq64_need_resched() == 0);                                 // the deferred request survived to a legal point
            irq64_get_last_exit_ctx(&last, &dec0);
            CHECK(last.preempt_disabled == 0 && dec0 > dec1);
            CHECK(process64_current_pid() == had_cur);                        // no schedulable process -> no unsafe/phantom switch
        }
    }
    irq64_clear_need_resched();
    group_end("reschedule on IRQ exit: legality table, immediate eligible switch, illegal points leave need_resched pending, survives to a legal point (unit + real timer interrupts)");
}

// ═════════════════════ 3. silence-proof decision ═════════════════════════
static void test_silence_proof(void) {
    group_begin();
    CHECK(virtio_irq_silence_proof(1, 1, 0, 0) == 1);       // P1: every entry masked AND every selector unmapped
    CHECK(virtio_irq_silence_proof(1, 1, 1, 1) == 1);       // strongest proof reported first
    CHECK(virtio_irq_silence_proof(0, 0, 1, 0) == 2);       // P2: whole set quiesced
    CHECK(virtio_irq_silence_proof(0, 1, 1, 0) == 2);
    CHECK(virtio_irq_silence_proof(0, 0, 1, 1) == 2);       // both: reported in strength order
    CHECK(virtio_irq_silence_proof(1, 0, 1, 0) == 2);       // P1 incomplete, P2 holds
    CHECK(virtio_irq_silence_proof(0, 0, 0, 1) == 3);       // P3: verified reset + selectors unmapped
    CHECK(virtio_irq_silence_proof(1, 0, 0, 0) == 0);       // masked but NOT unmapped: not proof
    CHECK(virtio_irq_silence_proof(0, 1, 0, 0) == 0);       // unmapped but NOT masked: not proof
    CHECK(virtio_irq_silence_proof(0, 0, 0, 0) == 0);       // nothing verified -> FAULTED
    group_end("device-wide verified-silence proofs (P1/P2/P3); one masked/unmapped queue is not proof");
}

// ═════════════════════ 4. orchestration with fake ops ════════════════════
#define MAXEV 64
static char ev[MAXEV][24]; static int nev;
static void evlog(const char* s) { if (nev < MAXEV) { int i = 0; while (s[i] && i < 23) { ev[nev][i] = s[i]; i++; } ev[nev][i] = 0; nev++; } }
static int ev_has(const char* s) { for (int i = 0; i < nev; i++) { int k = 0; while (s[k] && ev[i][k] == s[k]) k++; if (!s[k] && !ev[i][k]) return 1; } return 0; }
static int ev_idx(const char* s) { for (int i = 0; i < nev; i++) { int k = 0; while (s[k] && ev[i][k] == s[k]) k++; if (!s[k] && !ev[i][k]) return i; } return -1; }

static struct fake_set { pci_irqset_state_t st; int nq; } g_fset;
static pci64_device_t g_fpdev;
static int g_f_activate_mode;           // 0 = ok, 1 = fail pre-boundary (stays ARMED), 2 = fail AFTER boundary (QUIESCED/sealed)
static int g_f_quiesce_ok;
static int g_f_begin_fail, g_f_poll_fail, g_f_finish_fail, g_f_queues_fail;

static int f_alloc(pci64_device_t* d, int n, pci64_irq_set_t** out) { (void)d; evlog("alloc"); g_fset.st = PCI_IRQSET_ALLOCATED; g_fset.nq = n; *out = (pci64_irq_set_t*)&g_fset; return n; }
static int f_request(pci64_irq_set_t* s, int i, irq64_handler_t h, void* c, const char* nm) { (void)s; (void)i; (void)h; (void)c; (void)nm; evlog("request"); g_fset.st = PCI_IRQSET_BOUND; return 0; }
static int f_prepare(pci64_irq_set_t* s) { (void)s; evlog("prepare"); g_fset.st = PCI_IRQSET_ARMED; return 0; }
static int f_activate(pci64_irq_set_t* s) {
    (void)s; evlog("activate");
    if (g_f_activate_mode == 1) return -5;
    if (g_f_activate_mode == 2) { g_fset.st = PCI_IRQSET_QUIESCED; return -5; }
    g_fset.st = PCI_IRQSET_LIVE; return 0;
}
static int f_free(pci64_irq_set_t* s) { (void)s; evlog("free"); if (g_fset.st > PCI_IRQSET_ARMED) return PCI64_IRQ_ERR_SEALED; return 0; }
static int f_quiesce(pci64_irq_set_t* s) { (void)s; evlog("quiesce"); if (!g_f_quiesce_ok) { g_fset.st = PCI_IRQSET_FAULTED; return -5; } g_fset.st = PCI_IRQSET_QUIESCED; return 0; }
static int f_vector(const pci64_irq_set_t* s, int i, pci64_irq_vector_t* o) { (void)s; o->irq = 16 + i; o->dev_index = (uint16_t)i; return 0; }
static pci_irqset_state_t f_state(const pci64_irq_set_t* s) { (void)s; return g_fset.st; }
static const virtio_pci_ops_t fake_pci = { f_alloc, f_request, f_prepare, f_activate, f_free, f_quiesce, f_vector, f_state };

static int d_begin(void* i) { (void)i; evlog("begin"); return g_f_begin_fail ? -1 : 0; }
static int d_queues(const uint32_t* v, int n) { evlog("queues"); g_f_queues_fail = g_f_queues_fail; return (v[0] == 0xFFFF && n) ? 0 : 0; }
static int d_finish(void) { evlog("finish"); return g_f_finish_fail ? -1 : 0; }
static void d_abort(void) { evlog("abort"); }
static int d_poll(void* i) { (void)i; evlog("poll_init"); return g_f_poll_fail ? -1 : 0; }
static const virtio_dev_ops_t fake_dev = { d_begin, d_queues, d_finish, d_abort, d_poll, 0 };

static irq64_ret_t fh(void* c) { (void)c; return IRQ64_RET_NONE; }

static int run_init(int nq, int force_poll, int fail_at, virtio_irq_dev_t* d) {
    nev = 0; g_fset.st = PCI_IRQSET_ALLOCATED;
    d->name = "selftest-fake";                     // injected failures below are NOT real devices
    irq64_handler_t hs[2] = { fh, fh }; const char* nms[2] = { "a", "b" };
    return virtio_irq_init_device(d, VIRTIO_DEV_INPUT, &g_fpdev, 0, nq, hs, nms, &fake_pci, &fake_dev, force_poll, fail_at);
}

static void test_orchestration(void) {
    group_begin();
    static virtio_irq_dev_t d;
    g_f_activate_mode = 0; g_f_quiesce_ok = 1; g_f_begin_fail = g_f_poll_fail = g_f_finish_fail = g_f_queues_fail = 0;

    // normal: exact order; born IRQ-owned; nothing aborted, no polling init
    int rc = run_init(2, 0, 0, &d);
    CHECK(rc == 0 && d.mode == VIRTIO_IRQ_LIVE && d.setup_result == 0 && d.set != 0);
    CHECK(ev_idx("begin") < ev_idx("alloc") && ev_idx("alloc") < ev_idx("request") && ev_idx("request") < ev_idx("prepare"));
    CHECK(ev_idx("prepare") < ev_idx("queues"));            // selectors are programmed while ARMED (MSI-X enabled, function masked)
    CHECK(ev_idx("queues") < ev_idx("activate"));           // ...and BEFORE the go-live boundary
    CHECK(ev_idx("activate") < ev_idx("finish"));           // DRIVER_OK only after activation
    CHECK(!ev_has("abort") && !ev_has("poll_init") && !ev_has("free"));
    CHECK(d.entry[0] == 0 && d.entry[1] == 1 && d.irq[0] == 16 && d.irq[1] == 17);

    // forced polling: no PCI traffic at all
    rc = run_init(2, 1, 0, &d);
    CHECK(rc == 1 && d.mode == VIRTIO_IRQ_POLLING && !ev_has("alloc") && !ev_has("begin") && ev_has("poll_init") && d.set == 0);

    // pre-live failure at EVERY step 1..6: exact rollback (free when a set exists), device reset, POLL re-init
    for (int k = 1; k <= 6; k++) {
        rc = run_init(2, 0, k, &d);
        CHECK(rc == 1 && d.mode == VIRTIO_IRQ_POLLING && d.fail_step == k);
        CHECK(ev_has("abort") && ev_has("poll_init") && ev_idx("abort") < ev_idx("poll_init"));
        CHECK(!ev_has("finish"));                                    // never reached DRIVER_OK on the failed attempt
        CHECK(ev_has("free") == (k >= 3));                           // a set existed and was released iff it had been allocated
        CHECK(!ev_has("quiesce"));                                   // pre-boundary: nothing sealed, nothing to quiesce
        CHECK(d.set == 0);
    }
    // failure at S7 (DRIVER_OK) is POST-live: the set is sealed, cannot be freed, is quiesced, then POLL
    rc = run_init(2, 0, 7, &d);
    CHECK(rc == 1 && d.mode == VIRTIO_IRQ_POLLING && d.fail_step == 7 && ev_has("abort") && ev_has("quiesce") && !ev_has("free") && ev_has("poll_init"));
    CHECK(ev_idx("abort") < ev_idx("poll_init") && ev_idx("quiesce") < ev_idx("poll_init"));   // silence verified BEFORE polling resumes
    CHECK(d.set != 0);                                               // the sealed IRQ set stays pinned
    // real DRIVER_OK failure
    g_f_finish_fail = 1; rc = run_init(2, 0, 0, &d); g_f_finish_fail = 0;
    CHECK(rc == 1 && d.fail_step == 7 && !ev_has("free") && ev_has("quiesce"));
    // boundary attempted but unverified (activate fails AFTER the boundary): sealed, cannot free, quiesce path
    g_f_activate_mode = 2; rc = run_init(2, 0, 0, &d); g_f_activate_mode = 0;
    CHECK(rc == 1 && d.fail_step == 6 && !ev_has("free") && ev_has("abort") && ev_has("quiesce") && ev_has("poll_init"));
    // activate fails BEFORE the boundary (set still ARMED): freed, not quiesced
    g_f_activate_mode = 1; rc = run_init(2, 0, 0, &d); g_f_activate_mode = 0;
    CHECK(rc == 1 && d.fail_step == 6 && ev_has("free") && !ev_has("quiesce"));
    // post-live silence NOT verifiable -> FAULTED: no polling, no normal operation
    g_f_quiesce_ok = 0; rc = run_init(2, 0, 7, &d);
    CHECK(rc < 0 && d.mode == VIRTIO_IRQ_FAULTED && !ev_has("poll_init") && d.faulted == 1 && d.set != 0);
    g_f_quiesce_ok = 1;
    // both attempts fail
    g_f_poll_fail = 1; rc = run_init(2, 0, 1, &d); g_f_poll_fail = 0;
    CHECK(rc < 0);
    // device negotiation fails on the IRQ attempt -> abort + poll
    g_f_begin_fail = 1; rc = run_init(1, 0, 0, &d); g_f_begin_fail = 0;
    CHECK(rc == 1 && d.fail_step == 1);
    group_end("VirtIO setup order (ARMED -> selectors -> activate -> DRIVER_OK), forced poll, pre-live failure at every step, post-live sealed/FAULTED handling");
}

// ═════════════════════ 5. Rust suites ════════════════════════════════════
static void test_rust_suites(void) {
    group_begin();
    uint32_t a = toxenos_virtio_irq_selftest_pci();
    uint32_t b = toxenos_virtio_irq_selftest_input();
    uint32_t c = toxenos_virtio_irq_selftest_gpu();
    CHECK(a == 0); CHECK(b == 0); CHECK(c == 0);
    CHECK(toxenos_virtio_input_selftest_ring_wrap() == 1);
    klog_hex("virtio_irq64 selftest: Rust assertions executed: ", toxenos_virtio_irq_selftest_assertions());
    CHECK(toxenos_virtio_irq_selftest_assertions() >= 97);           // floor: a suite that silently stopped asserting is a failure
    group_end("Rust: MSI-X selectors, used-ring drain (0/1/many/jump/wrap/recycle-once), ownership, completion ring, observed-vs-consumed, input sample ring + decoder, GPU completion paths");
}

void virtio_irq64_selftest_run(void) {
    g_pass = g_fail = 0;
    klog("virtio_irq64 selftest: begin\n");
    test_silence_proof();
    test_kwait();
    test_resched();
    test_orchestration();
    test_rust_suites();
    klog_hex("virtio_irq64 selftest: checks passed: ", (uint32_t)g_pass);
    klog_hex("virtio_irq64 selftest: checks failed: ", (uint32_t)g_fail);
    klog(g_fail ? "virtio_irq64 selftest: RESULT FAIL\n" : "virtio_irq64 selftest: RESULT PASS\n");
}

// A POLLING device either never went live (no PCI IRQ set, no logical irqs) or fell back AFTER the go-live
// boundary (set sealed and verified QUIESCED -- it can never be freed, but it is silent).
static int poll_mode_consistent(const virtio_irq_dev_t* d) {
    if (!d->set) return d->irq[0] < 0 && d->irq[1] < 0;
    return pci64_irq_state(d->set) == PCI_IRQSET_QUIESCED;
}

// Post-initialisation checks against the REAL devices (run after both drivers initialised).
// What is asserted depends on the mode each device actually ended up in; a device that is in neither
// LIVE nor POLLING (absent, FAULTED) is not checked here and the report says so -- never a vacuous PASS.
void virtio_irq64_post_init_check(void) {
    group_begin();
    virtio_irq_dev_t* in = virtio_irq_dev(VIRTIO_DEV_INPUT);
    virtio_irq_dev_t* gp = virtio_irq_dev(VIRTIO_DEV_GPU);
    int live = 0, polling = 0;

    if (in->pdev && in->mode == VIRTIO_IRQ_LIVE) {
        live++;
        CHECK(in->set != 0 && in->irq[0] >= 0);
        CHECK(toxenos_virtio_input_owner() == 1);               // born IRQ-owned
        CHECK(virtio_input64_owner_selfcheck() == 1);           // a POLL drain of the IRQ-owned queue is REFUSED + counted
    } else if (in->pdev && in->mode == VIRTIO_IRQ_POLLING) {
        polling++;
        CHECK(poll_mode_consistent(in));                        // no IRQ resources, or sealed + verified QUIESCED
        CHECK(toxenos_virtio_input_owner() == 0);               // POLL-owned
    }
    if (gp->pdev && gp->mode == VIRTIO_IRQ_LIVE) {
        live++;
        CHECK(gp->set != 0 && gp->irq[0] >= 0 && gp->irq[1] >= 0 && gp->irq[0] != gp->irq[1]);   // dedicated vector per queue
        CHECK(toxenos_virtio_gpu_owner(0) == 1 && toxenos_virtio_gpu_owner(1) == 1);            // device-wide: both IRQ-owned
        CHECK(virtio_gpu64_owner_selfcheck() == 1);
        CHECK(virtio_gpu64_wouldblock_selfcheck() == 1);        // non-blockable context: refused before the doorbell, nothing in flight
    } else if (gp->pdev && gp->mode == VIRTIO_IRQ_POLLING) {
        polling++;
        CHECK(poll_mode_consistent(gp));
        CHECK(toxenos_virtio_gpu_owner(0) == 0 && toxenos_virtio_gpu_owner(1) == 0);            // device-wide: both POLL-owned
    }
    if (!live && !polling) {
        klog("virtio_irq64 selftest SKIPPED: real devices absent or not in LIVE/POLLING mode -- nothing to assert\n");
        return;
    }
    group_end(live && !polling ? "real devices (MSI-X mode): IRQ-owned, dedicated vectors, POLL drain refused and counted (wrong-owner assertion)"
            : polling && !live ? "real devices (POLL mode): POLL-owned; no IRQ set, or a sealed set verified QUIESCED"
            :                    "real devices (mixed per-device modes): each device consistent with its own mode");
}
