//! Milestone 31: kernel panic handling. Abort-style only -- this crate
//! is built with `-C panic=abort` (and Cargo.toml's `panic = "abort"`
//! profile setting agrees), so there is no unwinder, no `eh_personality`
//! lang item, and no possibility of a caller "catching" a Rust panic.
//! A panic here is exactly as fatal as an unhandled C-side kernel
//! exception, and funnels into the SAME halt path
//! (kernel/interrupt64.c's `kernel64_halt_forever`) rather than
//! reimplementing one.
//!
//! Deliberately allocation-free: `console::log_line` only ever touches a
//! fixed-size stack buffer, never the heap, so a panic triggered BY an
//! allocation failure can still be reported without risking a second
//! failure (or infinite recursion) trying to allocate a message buffer.
use crate::console;
use crate::ffi;
use core::panic::PanicInfo;

#[panic_handler]
fn panic(info: &PanicInfo) -> ! {
    console::log_line("*** RUST PANIC ***");

    if let Some(loc) = info.location() {
        // core::fmt IS available (it's part of `core`, not `alloc`), so
        // the fixed-size KlogWriter can format this directly without
        // any heap allocation.
        crate::klog_fmt!("  at {}:{}:{}", loc.file(), loc.line(), loc.column());
    }

    // PanicInfo's message is always available on this edition (no
    // `#[cfg]` gate needed) and formats without allocating.
    crate::klog_fmt!("  {}", info.message());

    // SAFETY: kernel64_halt_forever() takes no arguments, never returns,
    // and its only side effects (klog + cli + hlt loop) are the exact
    // same ones any other fatal kernel path already performs from
    // arbitrary contexts (interrupt handlers included) -- there is no
    // additional precondition for this call beyond "the kernel is about
    // to become unrecoverable," which is already true here.
    unsafe { ffi::kernel64_halt_forever() }
}
