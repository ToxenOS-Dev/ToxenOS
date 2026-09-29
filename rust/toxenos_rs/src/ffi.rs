//! Milestone 31: raw `extern "C"` declarations for the existing C kernel
//! functions Rust code needs. This is the ONLY file in the crate that
//! declares raw C signatures directly -- every other module wraps these
//! in a safer, more explicit Rust API instead of letting handwritten
//! `extern "C"` blocks spread across every future Rust driver (per the
//! milestone's explicit instruction). Signatures are copied by hand from
//! the corresponding C headers (include/klog.h, include/heap64.h,
//! include/physmem64.h) and must be kept in sync with them -- the same
//! "duplicate, documented, kept in sync by convention" discipline this
//! codebase already uses at the user64/kernel boundary (see
//! user64/tox64.h's own header comment).
//!
//! Every function here is `unsafe` to call for the usual FFI reasons
//! (raw pointers, no borrow checking across the boundary, and in most
//! cases genuine hardware/global-kernel-state side effects) even where
//! the C implementation itself cannot itself fail loudly.

use core::ffi::c_void;

extern "C" {
    // include/klog.h
    pub fn klog(msg: *const u8);
    pub fn klog_hex(label: *const u8, val: u32);

    // include/heap64.h
    pub fn kmalloc(size: u64) -> *mut c_void;
    pub fn kfree(ptr: *mut c_void);
    pub fn kmalloc_aligned(size: u64, align: u64) -> *mut c_void;
    pub fn kfree_aligned(ptr: *mut c_void);

    // include/physmem64.h
    pub fn physmem64_alloc_page() -> u64;
    pub fn physmem64_alloc_pages(count: u64) -> u64;
    pub fn physmem64_free_page(phys: u64);
    pub fn physmem64_free_pages(phys: u64, count: u64);
    pub fn physmem64_to_virt(phys: u64) -> *mut c_void;
    pub fn physmem64_to_phys(virt: *const c_void) -> u64;
    pub fn physmem64_map_mmio(phys: u64, size: u64) -> *mut c_void;

    // kernel/interrupt64.c -- see that file's header comment on why this
    // one function was made non-static and exported: Rust's panic
    // handler must funnel into the SAME fatal path a C-side unrecoverable
    // exception uses, not reimplement its own halt loop.
    pub fn kernel64_halt_forever() -> !;

    // include/kwait64.h (M+11C) -- the explicit condition-wait primitive.
    pub fn kwait64_wait(chan: *mut c_void, cond: extern "C" fn(*mut c_void) -> i32, arg: *mut c_void, timeout_ticks: u64) -> i32;
    pub fn kwait64_signal(chan: *mut c_void);
    pub fn kwait64_can_wait() -> i32;
    pub fn timer64_get_ticks() -> u64;

    // include/virtio_irq64.h (M+11C): C-side per-device interrupt bookkeeping and the
    // PCI-side pieces of demotion. `dev`: 0 = input, 1 = GPU.
    pub fn virtio_irq64_mask_all(dev: u32) -> i32;      // 1 iff EVERY queue entry read back masked
    pub fn virtio_irq64_mask_one(dev: u32, queue: u32) -> i32;   // 0 on success (IRQ-context safe)
    pub fn virtio_irq64_quiesce_set(dev: u32) -> i32;   // 1 iff the PCI IRQ set quiesced (verified)
    pub fn virtio_irq64_note(dev: u32, event: u32);
    pub fn virtio_irq64_test_fail_reset() -> i32;      // 1 only in a VIRTIO_IRQ_TEST_FAIL_RESET build
    pub fn virtio_irq64_test_fail_unmap() -> i32;      // 1 only in a VIRTIO_IRQ_TEST_FAIL_UNMAP build

    // kernel/virtio_gpu64.c: the one sleeping GPU command mutex.
    pub fn toxenos_gpu_ctl_lock();
    pub fn toxenos_gpu_ctl_unlock();
}

/// Events reported to C via `virtio_irq64_note`.
pub const VIRQ_EV_DEMOTED_P1: u32 = 1;
pub const VIRQ_EV_DEMOTED_P2: u32 = 2;
pub const VIRQ_EV_DEMOTED_P3: u32 = 3;
pub const VIRQ_EV_FAULTED: u32 = 4;
pub const VIRQ_EV_STORM_WARN: u32 = 5;
pub const VIRQ_EV_STORM_MASKED: u32 = 6;
pub const VIRQ_EV_LOST_IRQ: u32 = 7;
pub const VIRQ_DEV_INPUT: u32 = 0;
pub const VIRQ_DEV_GPU: u32 = 1;
