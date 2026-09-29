#ifndef VIRTIO_INPUT64_H
#define VIRTIO_INPUT64_H

#include <stdint.h>
#include "virtio_pci64.h" // virtio_pci64_transport_info_t

// M+7A: the VirtIO-input driver's C-side bridge -- mirrors
// kernel/virtio_gpu64.c's own split exactly: the wire protocol lives
// exclusively in Rust (rust/toxenos_rs/src/virtio_input.rs), this file
// calls it only through plain-value extern "C" functions, and registers
// the result as a real input64_device_t (include/input64.h) the same
// way virtio_gpu64.c registers a gpu64_device_t.
//
// Scope: one absolute pointer device (a QEMU virtio-tablet-pci), polled
// from a timer tick -- no interrupts, no statusq, no EV_REL/keyboard
// handling (see this driver's own header comment in virtio_input.rs for
// the full scope note).

// Modern-transport PCI device ID for VirtIO device type 18 (input): per
// the VirtIO spec's own device-ID convention (0x1040 + device type),
// the same formula this codebase already confirmed correct for
// PCI64_DEVICE_VIRTIO_GPU_MODERN (0x1040 + 16 = 0x1050).
#define PCI64_DEVICE_VIRTIO_INPUT_MODERN 0x1052

// ── Rust entry points (rust/toxenos_rs/src/virtio_input.rs) ─────────
int32_t toxenos_virtio_input_init(const virtio_pci64_transport_info_t* info);   // POLL-mode (begin+queues+finish)
// M+11C phased initialisation + interrupt/poll entry points (rust/toxenos_rs/src/virtio_input.rs)
int32_t toxenos_virtio_input_init_begin(const virtio_pci64_transport_info_t* info);
int32_t toxenos_virtio_input_init_queues(uint32_t vector);
int32_t toxenos_virtio_input_init_finish(void);
void    toxenos_virtio_input_init_abort(void);
int32_t toxenos_virtio_input_irq(int32_t first);
int32_t toxenos_virtio_input_owner(void);
int32_t toxenos_virtio_input_health_probe(void);
int32_t toxenos_virtio_input_irq_stats(uint64_t* out14);
int32_t toxenos_virtio_input_demote(void);
int32_t toxenos_virtio_input_read_selector(void);   // raw queue_msix_vector (0xFFFF = NO_VECTOR, -1 = none)
int32_t toxenos_virtio_input_is_faulted(void);
int32_t toxenos_virtio_input_test_wrong_owner_poll(void);
void    toxenos_virtio_input_poll(void);
int32_t toxenos_virtio_input_get_abs_range(int32_t* min_x, int32_t* max_x, int32_t* min_y, int32_t* max_y);
int32_t toxenos_virtio_input_take_sample(int32_t* x, int32_t* y, uint32_t* buttons);
int32_t toxenos_virtio_input_is_ready(void);
// [received, recycled, polls] -- see rust/toxenos_rs/src/virtio_input.rs's
// own header comment on the receive-queue health invariant this reports.
int32_t toxenos_virtio_input_debug_stats(uint64_t* out);
int32_t toxenos_virtio_input_selftest_ring_wrap(void);

// Probes for a modern VirtIO-input PCI device, initializes the driver,
// and -- if it reports a usable absolute range -- registers it as a
// real input64_device_t AND calls input64_set_abs_pointer_active(1, ...)
// (see include/input64.h's own header comment on the pointer-source
// policy this sets up: PS/2 stops pushing REL/BUTTON events the moment
// this succeeds). Never fatal to boot: returns -1 and touches nothing
// else if no such device exists, or it has no absolute axes (e.g. a
// virtio-mouse instead of a virtio-tablet) -- PS/2 remains the sole
// pointer source in that case, exactly as before this milestone.
int virtio_input64_init(void);

// Called once per timer tick (kernel/timer64.c's timer64_handler) --
// see rust/toxenos_rs/src/virtio_input.rs's own header comment on why a
// periodic non-blocking poll, not real interrupts, is this milestone's
// deliberate choice. A safe no-op if virtio_input64_init() was never
// called or failed.
void virtio_input64_poll(void);
// M+11C: debug-only health probe (VIRTIO_IRQ_DEBUG_WATCHDOG builds); never drains.
void virtio_input64_health_probe(void);
// M+11C test switch (VIRTIO_IRQ_TEST_DEMOTE_INPUT builds): demote the IRQ-live input device and report.
void virtio_input64_irq_test_demote(void);
// Interrupt-mode summary (BDF/entry/irq/counters) via klog.
void virtio_input64_irq_report(void);
// Wrong-owner assertion self-check (POLL drain of an IRQ-owned queue must be refused). 1 = OK.
int  virtio_input64_owner_selfcheck(void);

// Diagnostics: 1 if a device was successfully initialized this boot.
int virtio_input64_available(void);

// Logs the receive-queue health counters (received/recycled/polls) via
// klog() -- development use only.
void virtio_input64_debug_stats_report(void);

// Runs the pure ring-index-wraparound arithmetic self-test (no real
// device needed -- see rust/toxenos_rs/src/virtio_input.rs's own
// selftest_ring_wrap()). Returns 1 if every case passed.
int virtio_input64_selftest(void);

#endif // VIRTIO_INPUT64_H
