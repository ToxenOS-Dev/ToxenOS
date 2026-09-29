#ifndef VIRTIO_IRQ64_H
#define VIRTIO_IRQ64_H

// M+11C: interrupt-driven VirtIO -- per-device IRQ-mode state machine and the
// orchestration that moves a device from "initialised" to "MSI-X live" (or
// falls back to polling) using the M+11A irq64 layer and the M+11B
// pci_irq64 layer.
//
// Setup order (VirtIO 1.3 3.1.1 / 4.1.5.1.2 / 4.1.4.3.2 + M+11B ARMED state):
//   S1  reset, ACKNOWLEDGE, DRIVER, features, FEATURES_OK          (device begin)
//   S2  pci64_irq_alloc
//   S3  pci64_irq_request  (handlers bound; set BOUND)
//   S4  pci64_irq_prepare  (MSI-X enabled, function MASKED, table programmed; set ARMED)
//   S5  queues: queue_msix_vector written + READ BACK, then queue_enable
//         (config_msix_vector is never written: NO_VECTOR)
//   S6  pci64_irq_activate (INTx off, entries unmasked, MASKALL cleared LAST -> LIVE)
//   S7  DRIVER_OK (first notification only after this)             (device finish)
// Any failure before S6's boundary: free the PCI set (exact restore), reset the
// device (unmaps every vector) and RE-INITIALISE IN POLL MODE. A failure after
// the boundary keeps the set sealed (M+11B) and falls back to polling only after
// the device is reset/quiesced and verified silent; otherwise FAULTED.
//
// Ownership (see rust/toxenos_rs/src/virtio_pci.rs QueueOwner): a device that
// completes setup is born IRQ-OWNED; there is exactly one normal completion
// owner per queue, never both. Demotion is DEVICE-WIDE.
//
// Post-live demotion (IRQ -> POLL) happens only after the device is PROVEN silent,
// for the device as a whole (one masked or unmapped queue is not proof):
//   P1  every IRQ-owned queue's MSI-X entry read back masked AND every
//       queue_msix_vector reads NO_VECTOR;
//   P2  the complete PCI IRQ set quiesced (function mask read back);
//   P3  a device reset completed (status read back 0) AND every selector reads
//       NO_VECTOR (input only: a GPU reset would destroy every resource/scanout).
// After a proof: every queue IRQ-owner -> NONE, used.idx reconciled, NONE ->
// POLL-owner, polling resumes.
//
// FAULTED (no proof obtainable) preserves SOFTWARE ownership and prevents unsafe reuse:
// no polling and no normal operation, the sealed IRQ set / descriptors / handlers /
// contexts stay pinned, one loud diagnostic, PS/2 takes over for input. The gate/reset
// that was attempted is best-effort. It does NOT promise that the system stays live if
// the hardware keeps generating MSI-X traffic -- this kernel has no interrupt-remapping
// subsystem that could contain a misbehaving device.
#include <stdint.h>
#include "pci64.h"
#include "pci_irq64.h"
#include "irq64.h"

typedef enum { VIRTIO_DEV_INPUT = 0, VIRTIO_DEV_GPU = 1, VIRTIO_DEV_COUNT = 2 } virtio_dev_t;

typedef enum {
    VIRTIO_IRQ_POLLING = 0,   // timer/manual poll owns completion discovery
    VIRTIO_IRQ_SWITCHING,     // setup in progress: device quiesced, nothing racing
    VIRTIO_IRQ_LIVE,          // MSI-X owns completion discovery
    VIRTIO_IRQ_QUIESCED,      // MSI-X set sealed, hardware delivery stopped
    VIRTIO_IRQ_FAULTED,       // ownership uncertain: preserve sealed resources, stop
} virtio_irq_mode_t;

#define VIRTIO_IRQ_MAX_Q 2

typedef struct {
    const char*        name;
    pci64_device_t*    pdev;
    pci64_irq_set_t*   set;
    int                nq;
    int                irq[VIRTIO_IRQ_MAX_Q];     // logical ToxenOS IRQ handles
    uint16_t           entry[VIRTIO_IRQ_MAX_Q];   // device-local MSI-X table entries
    virtio_irq_mode_t  mode;
    int                setup_result;              // 0 = live, 1 = polling fallback, <0 = failed
    int                fail_step;                 // step at which a fallback happened (0 = none)
    int                last_proof;                // 1..3 demotion proof, 0 = none/faulted
    uint32_t           demotions, faulted, lost_irq, storm_warn, storm_masked;
    uint32_t           timer_polls_while_live;    // MUST stay 0
} virtio_irq_dev_t;

// ── pure decision: is the device verifiably silent? (P1/P2/P3, device-wide) ──
// P1: EVERY IRQ-owned queue entry hardware-masked AND EVERY queue_msix_vector
//     reads NO_VECTOR.  P2: the whole PCI IRQ set quiesced (function mask
//     verified).  P3: a device reset completed AND all selectors read
//     NO_VECTOR.  One masked/unmapped queue is NOT proof for the device.
// Returns 1/2/3, or 0 (unverifiable -> FAULTED).
int virtio_irq_silence_proof(int p1_entries_masked_all, int p1_selectors_unmapped_all,
                             int p2_set_quiesced, int p3_reset_and_unmapped);

// ── injectable operation tables (real = pci64_* / Rust; fakes in the self-test) ──
typedef struct {
    int  (*alloc)(pci64_device_t*, int n, pci64_irq_set_t**);
    int  (*request)(pci64_irq_set_t*, int i, irq64_handler_t, void*, const char*);
    int  (*prepare)(pci64_irq_set_t*);
    int  (*activate)(pci64_irq_set_t*);
    int  (*free_set)(pci64_irq_set_t*);      // legal from BOUND/ARMED; SEALED after the boundary
    int  (*quiesce)(pci64_irq_set_t*);
    int  (*vector)(const pci64_irq_set_t*, int i, pci64_irq_vector_t*);
    pci_irqset_state_t (*state)(const pci64_irq_set_t*);
} virtio_pci_ops_t;

typedef struct {
    int  (*begin)(void* info);                        // S1
    int  (*queues)(const uint32_t* vec, int n);       // S5 (vec[i] = MSI-X entry, 0xFFFF = NO_VECTOR)
    int  (*finish)(void);                             // S7
    void (*abort)(void);                              // reset the device, drop partial state
    int  (*poll_init)(void* info);                    // full POLL-mode initialisation (fallback)
    int  (*quiesce_verify)(void);                     // post-live cleanup: device reset verified silent
} virtio_dev_ops_t;

extern const virtio_pci_ops_t virtio_pci_real_ops;

// Runs the setup order above. Returns 0 (LIVE, IRQ-owned), 1 (fell back to a
// working POLL-mode device), or negative (device unusable). `force_poll`
// skips the IRQ attempt (VIRTIO_IRQ_FORCE_POLL); `fail_at` (1..7) injects a
// failure just before that step (VIRTIO_IRQ_FAIL_AT and the self-tests).
int virtio_irq_init_device(virtio_irq_dev_t* d, virtio_dev_t which, pci64_device_t* pdev, void* info,
                           int nq, irq64_handler_t* handlers, const char** names,
                           const virtio_pci_ops_t* pci, const virtio_dev_ops_t* dev,
                           int force_poll, int fail_at);

virtio_irq_dev_t* virtio_irq_dev(virtio_dev_t which);
const char* virtio_irq_mode_name(virtio_irq_mode_t m);

// Called by the Rust drivers (rust/toxenos_rs/src/ffi.rs). `dev`: 0 input, 1 GPU.
int  virtio_irq64_mask_all(uint32_t dev);
int  virtio_irq64_mask_one(uint32_t dev, uint32_t queue);
int  virtio_irq64_quiesce_set(uint32_t dev);
int  virtio_irq64_test_fail_reset(void);   // 1 only in a VIRTIO_IRQ_TEST_FAIL_RESET build
int  virtio_irq64_test_fail_unmap(void);   // 1 only in a VIRTIO_IRQ_TEST_FAIL_UNMAP build
void virtio_irq64_note(uint32_t dev, uint32_t event);

// Boot/diagnostic reporting (one summary line per device; counters).
void virtio_irq64_report(void);
void virtio_irq64_report_full(void);
void virtio_irq64_selftest_run(void);   // deterministic boot-time tests (kernel/virtio_irq64_selftest.c)
void virtio_irq64_post_init_check(void); // real-device ownership checks (after both drivers initialised)

// Compile-time test switches (no runtime boot arg):
//   -DVIRTIO_IRQ_FORCE_POLL          skip IRQ setup: behave like pre-M+11C
//   -DVIRTIO_IRQ_FAIL_AT=N           inject a pre-/post-live failure at setup step N
//   -DVIRTIO_IRQ_TEST_DROP_CTL_IRQ   mask the GPU control entry after init: a real command
//                                    then times out, is recovered and the device is demoted
//   -DVIRTIO_IRQ_TEST_FAIL_MASK      the per-entry mask proof (P1) is unavailable -> demotion needs P2 (set quiesce)
//   -DVIRTIO_IRQ_TEST_FAULT_DEMOTE   P1 and P2 are unavailable: the GPU (no P3) ends FAULTED, the input device uses P3
//   -DVIRTIO_IRQ_TEST_FAIL_UNMAP     the queue selector unmap cannot be verified -> P1 unavailable (P2/P3 or FAULTED)
//   -DVIRTIO_IRQ_TEST_FAIL_RESET     P3 (device reset) is unavailable too: with FAULT_DEMOTE the input device ends FAULTED
//   -DVIRTIO_IRQ_TEST_DEMOTE_INPUT   demote the (IRQ-live) input device right after init and report the result
//   -DVIRTIO_IRQ_DEBUG_WATCHDOG      debug-only 1 Hz health probe (never in a production build)

#endif // VIRTIO_IRQ64_H
