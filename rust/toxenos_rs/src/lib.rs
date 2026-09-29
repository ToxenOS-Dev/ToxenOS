//! ToxenOS64 Rust kernel/driver support foundation (Milestone 31).
//!
//! This crate is `#![no_std]` and links as a `staticlib` directly into
//! kernel64.bin alongside the existing C/assembly objects -- see the
//! top-level Makefile's `rust64` build step. It does NOT replace or
//! reimplement any existing C subsystem; it exists purely to give
//! FUTURE ToxenOS drivers/kernel code a clean, explicit place to be
//! written in Rust, with:
//!   - `console`  -- klog()-backed logging (`klog_fmt!` macro),
//!   - `alloc_api`-- a `#[global_allocator]` backed by kmalloc/kfree,
//!   - `mmio`     -- a volatile-register accessor with the unsafe
//!                   boundary concentrated in one place,
//!   - `dma`      -- an owned, physically-contiguous DMA buffer type,
//!   - `pci`      -- a read-only PCI-device-info wrapper,
//!   - `ffi`      -- the ONE module allowed to declare raw `extern "C"`
//!                   C kernel functions; everything else wraps these,
//!   - `panic`    -- an abort-style `#[panic_handler]` that funnels
//!                   into the existing C fatal-halt path,
//!   - `selftest`/`pcidemo` -- Milestone 31's own C/Rust integration
//!                   proof and example component (see their own doc
//!                   comments).
//!
//! `alloc` (the crate, not this module) IS enabled -- see
//! `alloc_api`'s `#[global_allocator]`, which is what makes `Box`/`Vec`
//! usable here at all.
#![no_std]

extern crate alloc;

pub mod alloc_api;
pub mod console;
pub mod dma;
pub mod ffi;
pub mod mem;
pub mod mmio;
pub mod panic;
pub mod pci;
pub mod pcidemo;
pub mod selftest;
pub mod virtio_gpu;
pub mod virtio_input;
pub mod virtio_pci;
