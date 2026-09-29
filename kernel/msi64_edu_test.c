// kernel/msi64_edu_test.c -- M+11B: REAL MSI test against QEMU's `edu` PCI
// device (hw/misc/edu.c; docs/specs/edu.rst). TEST-ONLY: compiled to a no-op
// unless the kernel is built with MSI64_DEFS=-DMSI64_EDU_TEST, and it only
// ever touches a device with exactly vendor 0x1234 / device 0x11E8, which no
// production ToxenOS driver owns.
//
// edu's documented interface (BAR0, 1 MiB MMIO):
//   0x00 identification (RO)     0xRRrr00ed
//   0x04 liveness check          write x, read ~x
//   0x20 status                  bit 0 = computing, bit 7 = raise IRQ when a factorial finishes
//   0x24 interrupt status (RO)   bits raised by 0x60 writes / factorial completion
//   0x60 interrupt raise         writing v raises an interrupt with value v (MSI if enabled)
//   0x64 interrupt acknowledge   writing v clears those bits of 0x24
// Its MSI capability (probed): 64-bit, NO per-vector masking, 1 vector.
#include <stdint.h>
#include "../include/pci_irq64.h"
#include "../include/irq64.h"

#ifndef MSI64_EDU_TEST
void msi64_edu_test_run(void) {}
#else
#include "../include/pci64.h"
#include "../include/lapic64.h"
#include "../include/ioapic64.h"
#include "../include/physmem64.h"
#include "../include/vector64.h"
#include "../include/klog.h"

#define EDU_VENDOR 0x1234
#define EDU_DEVICE 0x11E8
#define EDU_ID        0x00
#define EDU_LIVE      0x04
#define EDU_STATUS    0x20
#define EDU_IRQ_STAT  0x24
#define EDU_IRQ_RAISE 0x60
#define EDU_IRQ_ACK   0x64

static volatile uint32_t* g_edu;
static inline uint32_t edu_r(uint32_t off) { return *(volatile uint32_t*)((volatile char*)g_edu + off); }
static inline void edu_w(uint32_t off, uint32_t v) { *(volatile uint32_t*)((volatile char*)g_edu + off) = v; }

static volatile uint32_t g_count, g_none, g_last;
static irq64_ret_t edu_handler(void* ctx) {
    (void)ctx;
    uint32_t st = edu_r(EDU_IRQ_STAT);
    if (!st) { g_none++; return IRQ64_RET_NONE; }
    g_last = st;
    edu_w(EDU_IRQ_ACK, st);                 // acknowledge at the device
    g_count++;
    return IRQ64_RET_HANDLED;
}

// Atomic snapshot of the LAPIC EOI counter and every descriptor's counters
// (IF=0 so no interrupt can land between the reads and skew the comparison).
typedef struct { uint64_t lapic_eoi, desc_eoi, desc_count; uint32_t io_ops, orphans; } snap_t;
static void snap(const irq64_table_t* t, snap_t* s) {
    uint64_t f; __asm__ volatile ("pushfq; pop %0; cli" : "=r"(f) :: "memory");
    s->lapic_eoi = lapic64_eoi_count; s->io_ops = ioapic64_op_count; s->orphans = t->orphan_vectors;
    s->desc_eoi = 0; s->desc_count = 0;
    for (int i = 0; i < IRQ64_MAX; i++) { s->desc_count += t->desc[i].count; s->desc_eoi += t->desc[i].eoi_count; }
    if (f & 0x200) __asm__ volatile ("sti" ::: "memory");
}

static int g_fails;
static void chk(int ok, const char* what) { if (!ok) { g_fails++; klog("MSI64 EDU TEST FAIL: "); klog(what); klog("\n"); } }
#define CK(c) chk((c), #c)

void msi64_edu_test_run(void) {
    // Exactly one edu, and nothing else claims it.
    pci64_device_t* dev = pci64_find_device(EDU_VENDOR, EDU_DEVICE, 0);
    if (!dev) { klog("MSI64 EDU TEST: no edu device attached -- skipped\n"); return; }
    if (pci64_find_device(EDU_VENDOR, EDU_DEVICE, dev)) { klog("MSI64 EDU TEST: more than one edu -- refusing\n"); return; }
    if (irq64_mode() != IRQ64_MODE_APIC) { klog("MSI64 EDU TEST: not in APIC mode -- skipped\n"); return; }
    if (dev->bar[0].type != PCI64_BAR_MEM32 && dev->bar[0].type != PCI64_BAR_MEM64) { klog("MSI64 EDU TEST: edu BAR0 not MMIO -- skipped\n"); return; }

    pci64_enable_device(dev);                                       // IO|MEM|BME (driver responsibility, per the API contract)
    g_edu = (volatile uint32_t*)physmem64_map_mmio(dev->bar[0].address, dev->bar[0].size);
    if (!g_edu) { klog("MSI64 EDU TEST: BAR0 map failed\n"); return; }

    CK((edu_r(EDU_ID) & 0xFF) == 0xED);
    edu_w(EDU_LIVE, 0x12345678u); CK(edu_r(EDU_LIVE) == ~0x12345678u);

    // QUIESCE the device before touching interrupt routing (API contract).
    edu_w(EDU_STATUS, 0);                                           // no factorial-completion interrupt
    edu_w(EDU_IRQ_ACK, 0xFFFFFFFFu);
    CK(edu_r(EDU_IRQ_STAT) == 0);

    pci64_irq_set_t* set = 0;
    int n = pci64_irq_alloc(dev, 1, 4, PCI64_IRQ_ANY, &set);        // edu has MSI only: exactly one vector
    CK(n == 1 && set && pci64_irq_type(set) == PCI64_IRQ_TYPE_MSI);
    if (!set || n != 1) { klog("MSI64 EDU TEST: FAIL (alloc)\n"); return; }
    pci64_irq_vector_t pv; pci64_irq_vector(set, 0, &pv);
    CK(pci64_irq_request(set, 0, edu_handler, 0, "edu-msi") == 0 && pci64_irq_state(set) == PCI_IRQSET_BOUND);
    CK(pci64_irq_enable(set) == 0 && pci64_irq_state(set) == PCI_IRQSET_LIVE);

    irq64_table_t* t = irq64_cur_table();
    const irq64_desc_t* d = irq64_get_desc(pv.irq);
    CK(d && d->src == IRQ64_SRC_MSI && d->trigger == IRQ64_TRIG_EDGE && d->sealed);
    int vec = d ? d->vector : -1;
    CK(vec >= 0x30 && vec != 0x80 && vector64_is_dynamic_device_vector(vec));
    CK(pci64_irq_free(set) == PCI64_IRQ_ERR_SEALED);                // live -> pinned

    snap_t s0; snap(t, &s0);
    uint32_t desc_c0 = d->count;
    uint64_t rte0 = 0; const irq64_desc_t* timer = irq64_get_desc(0);
    int have_rte = timer && ioapic64_read_rte(timer->hwirq, &rte0) == 0;

    // Real interrupts, repeatedly, through the device's documented raise/ack interface.
    const int N = 1000; int lost = 0;
    g_count = 0; g_none = 0;
    for (int i = 0; i < N; i++) {
        uint32_t want = (uint32_t)i + 1;
        uint32_t v = (i % 7 == 0) ? 0x80u : (1u << (i % 5));        // vary the raised value
        edu_w(EDU_IRQ_RAISE, v);
        uint64_t spin = 0;
        while (g_count < want && spin++ < 20000000ull) __asm__ volatile ("pause");
        if (g_count < want) { lost++; if (lost >= 3) break; g_count = want; }   // resync so one loss is counted once
    }
    // Nothing further may arrive (no duplicates / stray deliveries).
    for (volatile int i = 0; i < 2000000; i++) __asm__ volatile ("pause");

    snap_t s1; snap(t, &s1);
    uint64_t deliv = d->count - desc_c0;
    CK(lost == 0);
    CK(deliv == (uint32_t)N);                                       // every raise -> exactly one delivery to this descriptor
    CK(d->handled >= (uint32_t)N && d->unhandled == 0 && g_none == 0);
    CK(d->eoi_count == d->count);                                   // one LAPIC EOI per delivery
    CK(s1.orphans == s0.orphans);
    CK(s1.io_ops == s0.io_ops);                                     // IOAPIC never touched
    if (have_rte) { uint64_t rte1 = 0; CK(ioapic64_read_rte(timer->hwirq, &rte1) == 0 && (rte1 & ~(3ull << 12 | 1ull << 14)) == (rte0 & ~(3ull << 12 | 1ull << 14))); }
    // Global EOI accounting: every dispatched interrupt (timer, keyboard, mouse, MSI) EOI'd exactly once through the LAPIC.
    CK((s1.lapic_eoi - s0.lapic_eoi) == (s1.desc_eoi - s0.desc_eoi));   // LAPIC EOIs == descriptor EOIs, atomically sampled
    CK((s1.desc_eoi - s0.desc_eoi) == (s1.desc_count - s0.desc_count)); // every dispatched interrupt EOI'd exactly once
    CK(edu_r(EDU_IRQ_STAT) == 0);

    // Quiesce (driver first quiets the device, then the layer gates MSI).
    edu_w(EDU_IRQ_ACK, 0xFFFFFFFFu);
    CK(pci64_irq_quiesce(set) == 0 && pci64_irq_state(set) == PCI_IRQSET_QUIESCED);
    uint32_t before = d->count;
    edu_w(EDU_IRQ_RAISE, 1);                                        // MSI is gated now: nothing may arrive on the vector
    for (volatile int i = 0; i < 2000000; i++) __asm__ volatile ("pause");
    CK(d->count == before);
    edu_w(EDU_IRQ_ACK, 0xFFFFFFFFu);
    CK(pci64_irq_free(set) == PCI64_IRQ_ERR_SEALED);                // quiesced: still pinned

    klog_hex("MSI64 EDU TEST: delivered ", (uint32_t)deliv);
    klog_hex("MSI64 EDU TEST: raised    ", (uint32_t)N);
    klog_hex("MSI64 EDU TEST: lost      ", (uint32_t)lost);
    klog_hex("MSI64 EDU TEST: descriptor eoi_count ", d->eoi_count);
    klog_hex("MSI64 EDU TEST: lapic EOI delta ", (uint32_t)(s1.lapic_eoi - s0.lapic_eoi));
    klog_hex("MSI64 EDU TEST: ioapic ops during test ", s1.io_ops - s0.io_ops);
    klog(g_fails ? "MSI64 EDU TEST: RESULT FAIL\n" : "MSI64 EDU TEST: RESULT PASS\n");
}
#endif
