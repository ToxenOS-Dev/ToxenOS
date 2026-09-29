#ifndef PCI_IRQ64_H
#define PCI_IRQ64_H

// M+11B: driver-facing PCI MSI / MSI-X API on top of the irq64 core.
//
// A PCI driver deals ONLY in logical IRQ handles and (for MSI-X) the
// device-local table entry index. It never sees CPU vectors, the LAPIC
// message format, the capability layout or the table layout.
//
// Lifecycle (a "sealed IRQ set"):
//
//   ALLOCATED --all handlers bound--> BOUND --prepare--> ARMED --activate--> LIVE --quiesce--> QUIESCED
//                                                                      \--gate failed--> FAULTED
//
//   ALLOCATED  logical IRQs + CPU vectors reserved, hardware silent.
//              pci64_irq_request() binds one handler per vector.
//   BOUND      every handler installed, hardware silent.
//   ARMED      (M+11C) MSI-X: PCI MSI-X Enable = 1 with the Function Mask
//              (MASKALL) SET, every table entry masked and the used entries
//              programmed and read back. MSI: address/data/MME programmed,
//              MSI Enable still 0. NO message can reach the CPU, INTx has
//              not been touched, and NOTHING is sealed: disarm/free restore
//              the exact pre-prepare device state. This is the state in
//              which a device driver may write device-side selectors that
//              the VirtIO spec only defines while MSI-X is enabled
//              (config_msix_vector / queue_msix_vector).
//   LIVE       MSI/MSI-X has been exposed to the device.
//   QUIESCED   delivery was stopped and the gate read back as expected.
//   FAULTED    a gate after the boundary failed or could not be verified:
//              the hardware interrupt state is UNCERTAIN.
//
// THE BOUNDARY. Going live is the single write that clears MASKALL (MSI-X) /
// sets MSI Enable (MSI). IMMEDIATELY BEFORE issuing it the set's descriptors
// are SEALED, because once the write is issued a message may have escaped
// even if the readback later fails. The state only becomes LIVE if the
// readback verifies the write; a boundary attempt whose result cannot be
// verified ends QUIESCED (verified re-gate) or FAULTED (unverifiable) --
// always sealed, never freed, never left "LIVE by default".
//   * failure BEFORE the boundary: nothing could have escaped; the set is
//     ARMED/unsealed (activate) or BOUND (prepare/enable); disarm/free legal.
//   * boundary verified: LIVE, sealed.
//   * boundary attempted but unverified: QUIESCED or FAULTED, sealed.
//
// Once a set has EVER attempted the boundary (states LIVE, QUIESCED, FAULTED)
// everything on the interrupt side is PINNED until reboot: the CPU vector, the
// logical IRQ descriptor, the vector->irq mapping, the handler function, the
// handler context and the set object itself. A message the device already
// emitted may still arrive after quiescing; if any of those were released or
// rebound it would dispatch to freed memory or the wrong owner.
// pci64_irq_free() is therefore legal only in ALLOCATED/BOUND/ARMED and
// returns PCI64_IRQ_ERR_SEALED otherwise. No re-enable, no rebinding, no retry.
//
// DEVICE-QUIESCE CONTRACT: before pci64_irq_enable(), the owning driver
// must have the device QUIESCED -- device interrupt generation disabled at
// the device, no queue/engine intentionally producing events, handler
// already bound. pci64_irq_enable() then establishes the routing; only
// after it returns success may the driver enable device-specific interrupt
// causes. Causes/pending bits latched earlier WILL fire at go-live, so
// handlers must tolerate that. M+11C VirtIO must obey the same contract.
//
// PCI Command.BME (bus mastering, i.e. MSI's memory write) must already be
// set by the driver (pci64_enable_device); enable() refuses otherwise and
// changes only Command bit 10 (INTx Disable).
//
// MSI policy: at most ONE plain MSI vector is ever enabled (MME=0). MMC/MME
// are decoded and validated, but multi-message MSI is not exposed.
#include <stdint.h>
#include "pci64.h"
#include "irq64.h"

#define PCI64_IRQ_MSIX 0x1u
#define PCI64_IRQ_MSI  0x2u
#define PCI64_IRQ_ANY  (PCI64_IRQ_MSIX | PCI64_IRQ_MSI)

#define PCI64_IRQ_MAX_SET 64   // vectors per set (MSI-X tables are capped to this)

#define PCI64_IRQ_ERR_NODEV          (-19)   // forced PIC / no usable MSI or MSI-X capability
#define PCI64_IRQ_ERR_IO             (-5)    // config/table readback disagreed, injected failure
#define PCI64_IRQ_ERR_NOMEM          (-12)
#define PCI64_IRQ_ERR_INVAL          (-22)
#define PCI64_IRQ_ERR_NOSPC          (-28)
#define PCI64_IRQ_ERR_ALREADY_ACTIVE (-101)  // MSI or MSI-X already enabled at discovery
#define PCI64_IRQ_ERR_ALREADY_OWNED  (-102)  // ToxenOS already owns an IRQ set on this device
#define PCI64_IRQ_ERR_MALFORMED      (-103)  // capability list/capability/table metadata malformed
#define PCI64_IRQ_ERR_BADSTATE       (-104)
#define PCI64_IRQ_ERR_SEALED         (-105)  // set has been LIVE: cannot be freed
#define PCI64_IRQ_ERR_NO_BUS_MASTER  (-106)
#define PCI64_IRQ_ERR_COMPOSE        (-107)  // MSI message could not be composed (dest/vector)
#define PCI64_IRQ_ERR_FAULTED        (-108)
#define PCI64_IRQ_ERR_NO_DECODE      (-109)  // Command.MEM clear: MSI-X table BAR not decoding

typedef enum { PCI64_IRQ_TYPE_NONE = 0, PCI64_IRQ_TYPE_MSI = 1, PCI64_IRQ_TYPE_MSIX = 2 } pci64_irq_type_t;

typedef enum {
    PCI_IRQSET_ALLOCATED = 0,
    PCI_IRQSET_BOUND,
    PCI_IRQSET_ARMED,
    PCI_IRQSET_LIVE,
    PCI_IRQSET_QUIESCED,
    PCI_IRQSET_FAULTED,
} pci_irqset_state_t;

typedef struct pci64_irq_set pci64_irq_set_t;

typedef struct {
    int      irq;         // logical ToxenOS IRQ handle
    uint16_t dev_index;   // device-local MSI-X table entry index (MSI: message number 0).
                          // This is what a device (e.g. VirtIO queue_msix_vector) is told;
                          // it is NOT the CPU vector.
} pci64_irq_vector_t;

// Reserves between `min` and `max` vectors (MSI-X preferred when allowed,
// else one MSI). Nothing is programmed into the device. Returns the vector
// count (>= min), or a negative PCI64_IRQ_ERR_*. Fails with ALREADY_ACTIVE
// if the device arrives with MSI or MSI-X enabled, and ALREADY_OWNED if a
// set already exists; with NODEV in forced-PIC mode.
int pci64_irq_alloc(pci64_device_t* dev, int min, int max, unsigned flags, pci64_irq_set_t** out);

pci64_irq_type_t   pci64_irq_type(const pci64_irq_set_t* set);
pci_irqset_state_t pci64_irq_state(const pci64_irq_set_t* set);
int                pci64_irq_count(const pci64_irq_set_t* set);
int                pci64_irq_vector(const pci64_irq_set_t* set, int i, pci64_irq_vector_t* out);

// Binds vector i's handler (each vector exactly once). Hardware stays silent.
int pci64_irq_request(pci64_irq_set_t* set, int i, irq64_handler_t h, void* ctx, const char* name);

// BOUND -> ARMED (see kernel/pci_irq64.c). Snapshots everything it will touch;
// on any failure the exact snapshot is restored and the set stays BOUND.
int pci64_irq_prepare(pci64_irq_set_t* set);

// ARMED -> LIVE. Sets INTx Disable, unmasks the used entries (the function
// stays masked), seals, then clears MASKALL LAST. Failure before the boundary
// undoes only what activate itself did and leaves the set ARMED/unsealed;
// failure at/after the boundary is QUIESCED (verified gate) or FAULTED.
int pci64_irq_activate(pci64_irq_set_t* set);

// ARMED -> BOUND: restores the exact pre-prepare device state (legal only
// while unsealed).
int pci64_irq_disarm(pci64_irq_set_t* set);

// BOUND -> LIVE: prepare + activate; on a pre-boundary failure the set is
// disarmed back to BOUND. Sealed outcomes are as for activate.
int pci64_irq_enable(pci64_irq_set_t* set);

// LIVE -> QUIESCED. Returns 0 if the hardware gate was verified; on
// gate/readback failure the set becomes FAULTED and an error is returned.
// Never releases anything either way. (Relies on the driver-quiesce contract.)
int pci64_irq_quiesce(pci64_irq_set_t* set);

// Legal in ALLOCATED/BOUND/ARMED (an ARMED set is disarmed first, restoring the
// device exactly). Otherwise (the boundary was ever attempted)
// PCI64_IRQ_ERR_SEALED and nothing is released.
int pci64_irq_free(pci64_irq_set_t* set);

// Read-only diagnostic: pending state of vector i (MSI-X PBA bit / MSI
// pending bit). 1/0, or a negative error if unavailable. Never writes.
int pci64_irq_pending(const pci64_irq_set_t* set, int i);

// Read-only boot-time inspection of every enumerated device's MSI/MSI-X
// capabilities (decode + BAR/table/PBA validation; maps and READS the MSI-X
// table's vector-control words). Never writes config space or any table.
void pci64_irq_inspect_all(void);
void pci64_irq_inspect_dev(pci64_device_t* dev);

// Deterministic boot-time tests (kernel/pci_irq64_selftest.c). Fake config
// space + private irq64 table; the only real-hardware step is read-only.
void pci_irq64_selftest_run(void);
// Real-MSI test against QEMU's `edu` device (kernel/msi64_edu_test.c);
// compiled to a no-op unless built with MSI64_DEFS=-DMSI64_EDU_TEST.
void msi64_edu_test_run(void);

// ── self-test hooks ──────────────────────────────────────────────────
typedef struct {
    void*    (*map)(uint64_t phys, uint64_t len);
    uint32_t (*r32)(void* base, uint32_t off);
    void     (*w32)(void* base, uint32_t off, uint32_t v);
} pci_mmio_ops_t;
const pci_mmio_ops_t* pci_irq64_set_mmio_ops(const pci_mmio_ops_t* ops);   // NULL = real MMIO
void pci_irq64_set_quiet(int quiet);            // silence boot-log lines (self-test fake devices)
uint32_t pci_irq64_diag_count(void);            // FAULTED diagnostics emitted so far
void pci_irq64_test_arm(int fail_at_step);     // 0 disarms; counts forward-path steps
int  pci_irq64_test_steps(void);

#endif // PCI_IRQ64_H
