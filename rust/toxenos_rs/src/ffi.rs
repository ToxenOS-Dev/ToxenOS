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
}
