#ifndef RUSTFFI64_H
#define RUSTFFI64_H
#include <stdint.h>
#include "pci64.h"
#include "virtio_pci64.h"

// Milestone 31: purpose-built, explicit-layout structs for the C/Rust
// FFI boundary. Deliberately NOT a raw `pci64_device_t*` handed to Rust
// to interpret directly -- see rust/toxenos_rs/src/pci.rs's header
// comment. Keeping a dedicated boundary type means:
//   - pci64_device_t's own internal layout (or pci64_bar_t's, or
//     pci64_device_s's intrusive `next` pointer) can change later
//     without touching the Rust side at all;
//   - pci64_bar_type_t (a C enum, whose underlying integer size is not
//     guaranteed by the C standard) never crosses the boundary --
//     bar_types[] below is an explicit uint32_t array instead;
//   - ownership is unambiguous: `pci64_fill_rust_info` only ever reads
//     `dev` and writes into a caller-owned `out`, so there is nothing
//     for Rust to accidentally free or retain past the call.
typedef struct {
    uint8_t  bus, slot, func;
    uint16_t vendor_id, device_id;
    uint8_t  class_code, subclass, prog_if, revision;
    uint8_t  header_type;
    uint8_t  irq_line, irq_pin;
    uint32_t bar_types[6]; // pci64_bar_type_t's values, widened to an explicit-size integer: 0=none,1=io,2=mem32,3=mem64
    uint64_t bar_addrs[6]; // meaningful only where bar_types[i] != 0
    uint64_t bar_sizes[6];
} toxenos_pci_info_t;

// Flattens a live pci64_device_t into `out` -- a pure field-by-field
// copy, no allocation, no failure mode, `dev` is only ever read.
void pci64_fill_rust_info(const pci64_device_t* dev, toxenos_pci_info_t* out);

// ── Milestone 31: C/Rust boundary self-test ─────────────────────────
// A small struct BOTH languages agree on the layout of, used to prove
// C->Rust struct-passing ABI compatibility isn't accidental (as opposed
// to a Rust-internal check that only proves Rust agrees with itself).
typedef struct {
    int32_t  a;
    uint32_t b;
    uint64_t c;
} toxenos_rust_test_struct_t;

// Defined in Rust (rust/toxenos_rs/src/selftest.rs). C fills `s` with
// known values, calls this, and verifies the documented transformation
// was applied: a_out = a_in + 1; b_out = b_in * 2; c_out = (uint64_t)a_out + b_out.
// Returns 0.
int32_t toxenos_rust_touch_struct(toxenos_rust_test_struct_t* s);

// Defined in Rust. Runs the Milestone 31 Rust-side self-test suite
// (console output, kmalloc/kfree-backed heap alloc/free, `alloc` crate
// Vec/Box usage, the pci.rs struct-ABI check) and logs each case via
// klog(). Returns 0 if every case passed, nonzero (a bitmask of failed
// cases) otherwise.
int32_t toxenos_rust_selftest(void);

// Defined in Rust. Deliberately panics (an out-of-bounds array index)
// to exercise the panic handler's diagnostic output and fatal-halt
// path. Never returns -- see kernel/kernel64.c's RUST64_PANIC_TEST_RUN
// debug flag, mutually exclusive with normal boot.
__attribute__((noreturn)) void toxenos_rust_panic_test(void);

// Defined in Rust. Requests an allocation far larger than physical
// memory could satisfy, to observe stable no_std Rust's actual
// out-of-memory behavior. Never returns (either it aborts, or -- if it
// somehow "succeeds" against a misconfigured allocator -- loops
// forever after logging that unexpected outcome). See
// kernel/kernel64.c's RUST64_ALLOC_FAIL_TEST_RUN debug flag.
__attribute__((noreturn)) void toxenos_rust_alloc_fail_test(void);

// Defined in Rust. Wraps `info` (already populated by
// pci64_fill_rust_info) in Rust types and logs a human-readable summary
// via klog(). Read-only: never touches the device's actual registers,
// never retains `info` past this call. Returns 0.
int32_t toxenos_rust_pci_demo(const toxenos_pci_info_t* info);

// M+2: defined in Rust (rust/toxenos_rs/src/virtio_pci.rs). `info` must
// be a genuine virtio_pci64_probe() result (already-mapped MMIO
// regions) -- this function runs the real modern status/feature
// negotiation sequence and sets up virtqueue 0 against WHATEVER real
// device `info` describes, structurally proving the descriptor
// allocator, then sets DRIVER_OK. Never sends the device an actual
// command (see that module's own header comment on why this milestone
// stops there). Returns 0 on success, -1 on failure.
int32_t toxenos_rust_virtio_pci_selftest(const virtio_pci64_transport_info_t* info);

#endif // RUSTFFI64_H
