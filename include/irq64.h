#ifndef IRQ64_H
#define IRQ64_H

// M+11A: generic x86 interrupt core (descriptor table + chip abstraction),
// deliberately a small subset of Linux's genirq:
//
//   logical IRQ   index into the descriptor table. 0-15 are the legacy
//                 ISA IRQs (so existing "IRQ1 = keyboard" code keeps its
//                 numbering); 16-255 are reserved for dynamic sources
//                 (MSI/MSI-X, M+11B -- NOT implemented here).
//   hwirq         chip-specific input: ISA IRQ line for the PIC, GSI for
//                 the IOAPIC.
//   vector        CPU vector; owned via vector64's allocator. Legacy ISA
//                 IRQs use the fixed vector 0x20 + isa_irq in BOTH modes.
//
// A chip provides mask/unmask/eoi (and, for the 8259 only, a spurious
// check). The FLOW is chosen by the descriptor's trigger type:
//
//   edge : spurious check -> chip EOI -> handler.   EOI comes FIRST
//          because a handler may context-switch away (the timer's does,
//          via process64_tick) and never return here until much later; a
//          late EOI would block every lower-priority interrupt meanwhile.
//   level: spurious check -> handler -> chip EOI, and if the handler
//          keeps answering NONE the line is masked (storm guard) before
//          the EOI, so an unserviced level source cannot livelock the CPU.
//
// Handlers return IRQ64_RET_HANDLED when their device raised the IRQ and
// was serviced, IRQ64_RET_NONE when it did not (spurious/shared/level
// that isn't ours).
#include <stdint.h>
#include "vector64.h"
#include "isr64.h"      // trapframe64_t (irq64_exit_build_ctx)

#define IRQ64_MAX 256
#define IRQ64_STORM_THRESHOLD 100   // consecutive NONE on a level line before it is masked

typedef enum { IRQ64_RET_NONE = 0, IRQ64_RET_HANDLED = 1 } irq64_ret_t;
typedef irq64_ret_t (*irq64_handler_t)(void* ctx);

typedef enum { IRQ64_TRIG_EDGE = 0, IRQ64_TRIG_LEVEL = 1 } irq64_trigger_t;
typedef enum { IRQ64_POL_HIGH = 0, IRQ64_POL_LOW = 1 } irq64_polarity_t;

typedef enum { IRQ64_MODE_PIC = 0, IRQ64_MODE_APIC = 1 } irq64_mode_t;

// M+11B: interrupt source kind (diagnostics + chip selection).
typedef enum {
    IRQ64_SRC_NONE = 0,
    IRQ64_SRC_PIC,
    IRQ64_SRC_IOAPIC,
    IRQ64_SRC_MSI,
    IRQ64_SRC_MSIX,
} irq64_src_t;

// errno-style results for the M+11B message-signalled API.
#define IRQ64_E_INVAL   (-22)
#define IRQ64_E_BUSY    (-16)
#define IRQ64_E_NODEV   (-19)
#define IRQ64_E_NOSPC   (-28)
#define IRQ64_E_NOTSUP  (-95)

struct irq64_desc;
typedef struct irq64_chip {
    const char* name;
    void (*mask)(struct irq64_desc*);
    void (*unmask)(struct irq64_desc*);
    void (*eoi)(struct irq64_desc*);
    // Optional. Nonzero = this delivery was spurious; the chip has already
    // performed whatever partial EOI its hardware requires (8259 IRQ15:
    // master only; IRQ7: none). The core then neither runs a handler nor
    // sends another EOI.
    int  (*spurious)(struct irq64_desc*);
    // M+11B (all zero-initialised for existing chips):
    uint8_t src;                                            // irq64_src_t
    // Optional checked enable/disable used by irq64_enable()/irq64_disable();
    // returns 0 or a negative error, and only 0 if the hardware state
    // really changed. NULL -> the void mask/unmask above are used.
    int  (*ctl)(struct irq64_desc*, int enable);
} irq64_chip_t;

typedef struct irq64_desc {
    const irq64_chip_t* chip;
    irq64_handler_t     handler;
    void*               ctx;
    const char*         name;
    uint32_t            hwirq;       // ISA line (PIC) or GSI (IOAPIC)
    int16_t             vector;      // -1 = none
    uint8_t             trigger;
    uint8_t             polarity;
    uint8_t             requested;
    uint8_t             masked;      // chip-level mask state as last set by the core
    uint8_t             storm_masked;
    uint32_t            count;       // deliveries
    uint32_t            handled;
    uint32_t            unhandled;   // handler NONE or no handler
    uint32_t            spurious;
    uint32_t            consecutive_unhandled;
    uint32_t            storm_masks;
    // M+11B message-signalled interrupts:
    uint8_t             src;         // irq64_src_t
    uint8_t             allocated;   // logical-IRQ slot in use (message IRQs)
    uint8_t             can_mask;    // 1 = irq64_disable() really masks the hardware
    uint8_t             sealed;      // has crossed the live boundary: pinned until reboot
    uint16_t            bdf;         // bus<<8 | slot<<3 | func (message IRQs)
    uint16_t            dev_index;   // MSI-X table entry / MSI message number
    uint8_t             dest_apic;   // xAPIC destination the message targets
    void*               chip_data;   // irq64_msg_owner_t* for message IRQs
    uint32_t            eoi_count;   // chip EOIs issued for this descriptor
} irq64_desc_t;

typedef struct irq64_table {
    irq64_desc_t desc[IRQ64_MAX];
    int16_t      vec_to_irq[256];    // -1 = no descriptor
    vector64_map_t vmap;
    // Mode hooks (NULL in a bare table):
    //   route: program the chip for a legacy ISA IRQ (resolve GSI /
    //          trigger / polarity, fill desc, leave the source MASKED);
    //          0 on success.
    //   orphan_eoi: EOI for a vector that has no descriptor at all.
    int  (*route)(struct irq64_table*, irq64_desc_t*, uint8_t isa_irq, uint8_t vector);
    void (*orphan_eoi)(uint8_t vector);
    uint32_t dispatched;
    uint32_t orphan_vectors;
    // M+11B: message-signalled interrupts need the LAPIC (APIC mode only);
    // dest_apic is the BSP's xAPIC ID that messages target.
    int      apic_mode;
    uint32_t dest_apic;
} irq64_table_t;

// ── Table primitives (pure logic over a table; used by the self-tests) ─
void irq64_table_init(irq64_table_t* t, const irq64_chip_t* legacy_chip);
int  irq64_table_request_legacy(irq64_table_t* t, uint8_t isa_irq,
                                irq64_handler_t h, void* ctx, const char* name);
int  irq64_table_free_legacy(irq64_table_t* t, uint8_t isa_irq);
void irq64_table_dispatch(irq64_table_t* t, uint8_t vector);

// ── M+11B: message-signalled (MSI / MSI-X) descriptors ───────────────────
// Table-level primitives (used by kernel/pci_irq64.c and the self-tests).
// A message descriptor gets a logical IRQ >= 16 and ONE dynamically
// allocated CPU vector; the vector -> irq mapping is installed immediately,
// while the descriptor has no handler and the hardware is silent (delivery
// would just be counted as unhandled and EOI'd). MSI is edge-triggered and
// EOIs through the LAPIC; nothing here touches the IOAPIC.
typedef struct irq64_msg_owner {
    void* owner;
    // Hardware mask/unmask of one message (MSI-X entry mask bit / MSI
    // per-vector mask bit). 0 on success, negative on failure.
    int (*ctl)(void* owner, uint16_t dev_index, int enable);
} irq64_msg_owner_t;

int  irq64_table_msg_alloc(irq64_table_t* t, irq64_src_t src, uint16_t bdf, uint16_t dev_index,
                           int can_mask, irq64_msg_owner_t* owner, int* out_irq);
int  irq64_table_msg_bind(irq64_table_t* t, int irq, irq64_handler_t h, void* ctx, const char* name);
// Marks the descriptor SEALED (pinned until reboot): it can no longer be
// freed or rebound, and its handler/context/vector mapping stay valid.
int  irq64_table_msg_seal(irq64_table_t* t, int irq);
// -IRQ64_E_BUSY once sealed; otherwise releases the vector + logical IRQ.
int  irq64_table_msg_free(irq64_table_t* t, int irq);
// Core-internal: the CPU vector of a message IRQ (for the MSI composer
// only -- drivers never see it), or -1.
int  irq64_table_msg_vector(const irq64_table_t* t, int irq);
// Checked hardware enable/disable of one IRQ. -IRQ64_E_NOTSUP if the
// descriptor cannot be hardware-masked (plain MSI without a mask bit): the
// descriptor is then left enabled, never pretending otherwise.
int  irq64_table_ctl(irq64_table_t* t, int irq, int enable);
// EOI hook for message chips (default lapic64_eoi); self-tests substitute a counter.
void irq64_msg_set_eoi_hook(void (*fn)(void));

// ── M+11C: reschedule on IRQ exit ────────────────────────────────────────
// A device handler that wakes a waiter calls irq64_request_reschedule() (via
// process64_wake). Nothing switches inside the handler. After the generic
// dispatch has completed -- descriptor handler returned, EOI already sent --
// and BEFORE control returns to the assembly stub that restores the
// registers and executes iretq, irq64_dispatch() asks whether the
// INTERRUPTED context may legally be preempted. If so the scheduler switches
// (exactly as the timer path already does); if not, need_resched stays
// pending until a later legal IRQ exit. need_resched != a context switch.
//
// Schedulable interrupt origins in the current ToxenOS architecture:
//   ring 3 (user mode)                       -> YES  (any process)
//   ring 0 in the idle/boot context          -> YES  (no process to protect)
//   ring 0 inside a process (kernel mode)    -> NO   (deferred; syscalls run IF=0
//                                                     so this only ever happens in
//                                                     explicit sti windows)
//   preempt-disabled section / switch in progress -> NO
//   this interrupt nested inside another one          -> NO (the OUTERMOST exit decides)
typedef struct {
    int from_user;          // interrupted CS.RPL == 3
    int has_current;        // a real process (not idle/boot) is current
    int switching;          // the scheduler is mid-switch
    int preempt_disabled;   // preempt-disable depth > 0
    int nested;             // still inside an outer interrupt handler after this one returns
} irq64_exit_ctx_t;

int  irq64_exit_may_resched(const irq64_exit_ctx_t* c);        // pure decision
void irq64_request_reschedule(void);
int  irq64_need_resched(void);
int  irq64_in_irq(void);                                        // nonzero while inside irq64_dispatch
int  irq64_irq_ctx_suspend(void);                               // scheduler: save+clear around a context switch
void irq64_irq_ctx_resume(int saved);
uint32_t irq64_resched_switches(void);                          // stats
// Core: if need_resched is set and the context is schedulable, clear it and
// call do_switch. Returns 1 if a switch was performed. (do_switch is injectable
// for the self-tests; production passes process64_irq_exit_reschedule.)
int  irq64_exit_resched_point(const irq64_exit_ctx_t* c, void (*do_switch)(void));
void irq64_clear_need_resched(void);
// Self-test observability: the context of the latest IRQ-exit reschedule decision, and how many were made.
void irq64_get_last_exit_ctx(irq64_exit_ctx_t* out, uint32_t* decisions);
// The dispatch glue's context builder (exported so a test can drive it with a synthetic frame).
void irq64_exit_build_ctx(const trapframe64_t* tf, irq64_exit_ctx_t* out);

irq64_table_t* irq64_cur_table(void);
// Self-test only: swaps the table the global API and dispatcher use
// (caller must hold IF=0). Returns the previous table.
irq64_table_t* irq64_test_swap_table(irq64_table_t* t);
int  irq64_enable(int irq);
int  irq64_disable(int irq);

// ── Global core ───────────────────────────────────────────────────────
// Brings up the whole interrupt subsystem: remaps + masks the 8259s,
// discovers ACPI/MADT, decides PIC vs LAPIC+IOAPIC (transactionally,
// with rollback), installs the matching chip. Must run with IF=0 after
// idt64_init()/tss64_init() and before any irq64_request*().
void irq64_init(uint64_t mb_info_addr);
irq64_mode_t irq64_mode(void);
const char*  irq64_mode_name(void);

// Claims the fixed vector 0x20+isa_irq, routes it (IOAPIC mode: through
// any Interrupt Source Override), and unmasks it. 0 on success; negative
// on duplicate request / routing failure.
int  irq64_request_legacy(uint8_t isa_irq, irq64_handler_t h, void* ctx, const char* name);
int  irq64_free_legacy(uint8_t isa_irq);

const irq64_desc_t* irq64_get_desc(int irq);
void irq64_dump(void);                  // per-IRQ counters to klog
// Deterministic boot-time tests of the whole interrupt foundation
// (kernel/irq64_selftest.c); logs one PASS/FAIL line per group plus a
// summary. Runs with IF=0 after irq64_init().
void irq64_selftest_run(void);


#endif // IRQ64_H
