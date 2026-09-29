//! Milestone 31: the C/Rust integration self-test. Called once from C
//! (kernel/kernel64.c, behind the existing RUST64_SELFTEST_RUN debug
//! flag convention this codebase already uses for every other
//! milestone's self-test) via `toxenos_rust_selftest()`.
use crate::console;
use crate::ffi;
use alloc::boxed::Box;
use alloc::vec::Vec;

/// Must stay byte-for-byte in sync with include/rustffi64.h's
/// `toxenos_rust_test_struct_t`.
#[repr(C)]
pub struct TestStruct {
    pub a: i32,
    pub b: u32,
    pub c: u64,
}

/// C prepares `s`, calls this, and checks the documented transformation
/// afterward -- see include/rustffi64.h's comment on
/// `toxenos_rust_touch_struct`. Proves C->Rust struct-layout ABI
/// compatibility bidirectionally (not just a Rust-internal check that
/// only proves Rust agrees with itself).
///
/// # Safety
/// `s` must be a valid, non-null, uniquely-owned-for-the-duration-of-
/// this-call pointer to a `TestStruct` -- exactly the C-side contract
/// documented in rustffi64.h. This is the crate's `extern "C"` boundary
/// function, called directly by C; it is unsafe by construction (a raw
/// pointer with no Rust-side lifetime), not because it does anything
/// unusual with it.
#[no_mangle]
pub unsafe extern "C" fn toxenos_rust_touch_struct(s: *mut TestStruct) -> i32 {
    if s.is_null() {
        return -1;
    }
    let s = &mut *s;
    s.a += 1;
    s.b = s.b.wrapping_mul(2);
    s.c = s.a as u64 + s.b as u64;
    0
}

/// Milestone 31 controlled panic test: deliberately panics so the
/// C side (kernel/kernel64.c's `RUST64_PANIC_TEST_RUN` debug flag,
/// mutually exclusive with normal boot -- a panic halts forever by
/// design) can verify `panic.rs`'s handler reports useful diagnostics
/// through klog() and then reaches `kernel64_halt_forever()`, exactly
/// like an unhandled C-side kernel exception would. Never called during
/// normal boot or by the main self-test suite above.
///
/// # Safety
/// Takes no arguments and never returns -- there is no additional
/// precondition beyond "the kernel is far enough along to log a
/// message," true from the same point RUST64_SELFTEST_RUN already runs.
#[no_mangle]
pub unsafe extern "C" fn toxenos_rust_panic_test() -> ! {
    console::log_line("toxenos_rs: panic_test: about to panic on purpose");
    let v: [u32; 4] = [1, 2, 3, 4];
    // core::hint::black_box hides the index from the optimizer so rustc
    // can't prove out-of-bounds access at compile time (and refuse to
    // build, as it correctly does for a literal `v[4]`) -- this must
    // remain a genuine RUNTIME panic to test the panic handler at all.
    let bad_index: usize = core::hint::black_box(4);
    let _ = v[bad_index]; // triggers a real core::panicking::panic_bounds_check
    loop {}
}

/// Milestone 31 controlled allocation-FAILURE test (distinct from the
/// panic test above): requests an allocation far larger than physical
/// memory could ever satisfy, to observe what stable no_std Rust
/// actually does when `GlobalAlloc::alloc` returns null. See
/// kernel/kernel64.c's `RUST64_ALLOC_FAIL_TEST_RUN` debug flag.
///
/// # Safety
/// No preconditions beyond the heap being initialized.
#[no_mangle]
pub unsafe extern "C" fn toxenos_rust_alloc_fail_test() -> ! {
    console::log_line("toxenos_rs: alloc_fail_test: requesting an impossible allocation");
    let v: Vec<u8> = alloc::vec![0u8; usize::MAX / 2];
    console::log_line("toxenos_rs: alloc_fail_test: UNEXPECTEDLY survived allocation");
    crate::klog_fmt!("  len = {}", v.len());
    loop {}
}

fn check(pass: &mut u32, fail: &mut u32, bit: u32, name: &str, ok: bool) {
    if ok {
        *pass += 1;
        crate::klog_fmt!("toxenos_rs: selftest: {} PASS", name);
    } else {
        *fail |= bit;
        crate::klog_fmt!("toxenos_rs: selftest: {} FAIL", name);
    }
}

/// Runs every Milestone 31 self-test case. See
/// include/rustffi64.h's header comment on `toxenos_rust_selftest` for
/// the return-value contract.
///
/// # Safety
/// This is the crate's `extern "C"` entry point, called directly by C
/// with no arguments -- there is no additional precondition beyond "the
/// kernel heap (kmalloc/kfree) is already initialized," which is always
/// true by the time kernel64.c reaches this call (see kernel/kernel64.c's
/// existing boot-ordering comments on every other Milestone 22+
/// self-test needing the same thing).
#[no_mangle]
pub unsafe extern "C" fn toxenos_rust_selftest() -> i32 {
    console::log_line("toxenos_rs: selftest: starting");
    let mut pass: u32 = 0;
    let mut fail: u32 = 0;

    // 1. Rust console output (already exercised by the log_line calls
    // in this function, but check it succeeds structurally: a
    // multi-segment formatted line via the klog_fmt! macro).
    crate::klog_fmt!("toxenos_rs: selftest: format check {} {} {:#x}", 1, "two", 3);
    check(&mut pass, &mut fail, 1, "console output", true);

    // 2. Rust calling a C kernel function directly (klog_hex) -- proves
    // the reverse FFI direction from what C->Rust already demonstrates
    // just by having called this function at all.
    console::log_hex("toxenos_rs: selftest: klog_hex direct call = ", 0xC0FFEE);
    check(&mut pass, &mut fail, 2, "Rust calling C (klog_hex)", true);

    // 3. C/Rust struct ABI, exercised the OTHER direction: Rust
    // constructs the struct, a raw C-callable function (this same
    // toxenos_rust_touch_struct, called here as an ordinary Rust
    // function since it's already in this crate) transforms it in
    // place, Rust checks the result matches the documented contract.
    {
        let mut s = TestStruct { a: 10, b: 7, c: 0 };
        // SAFETY: `s` is a valid, uniquely-owned local value.
        let r = toxenos_rust_touch_struct(&mut s as *mut TestStruct);
        let ok = r == 0 && s.a == 11 && s.b == 14 && s.c == 25;
        check(&mut pass, &mut fail, 4, "struct ABI round-trip", ok);
    }

    // 4. pci.rs's own struct-layout self-test (a SEPARATE, dedicated
    // FFI struct -- see include/rustffi64.h's toxenos_pci_info_t).
    check(&mut pass, &mut fail, 8, "pci struct ABI", crate::pci::selftest());

    // 5. Heap allocation via kmalloc/kfree (the #[global_allocator] in
    // alloc_api.rs) through a single `Box`.
    {
        let boxed = Box::new(42u64);
        let ok = *boxed == 42;
        drop(boxed); // explicit: exercises kfree_aligned's release path deterministically here, not at function end
        check(&mut pass, &mut fail, 16, "Box heap allocation", ok);
    }

    // 6. Multiple `alloc`-crate allocations via `Vec` (push triggers at
    // least one reallocation as it grows, exercising kmalloc_aligned
    // more than once for a single logical object).
    {
        let mut v: Vec<u32> = Vec::new();
        for i in 0..64u32 {
            v.push(i);
        }
        let sum: u32 = v.iter().sum();
        let ok = v.len() == 64 && sum == (0..64u32).sum::<u32>();
        drop(v);
        check(&mut pass, &mut fail, 32, "Vec multiple allocations", ok);
    }

    // 7. Physical-page allocation (Milestone 23's physmem64), independent
    // of the kmalloc-backed heap tested above.
    {
        // SAFETY: physmem64_alloc_page/free_page have no preconditions
        // beyond the allocator being initialized (always true here).
        let phys = ffi::physmem64_alloc_page();
        let ok = phys != 0;
        if ok {
            ffi::physmem64_free_page(phys);
        }
        check(&mut pass, &mut fail, 64, "physmem64 page alloc/free", ok);
    }

    // 8. mmio.rs's volatile-register abstraction, exercised over a
    // plain heap buffer (there is no such thing as a "safe to touch"
    // piece of real device MMIO to test against without an owned
    // device -- see mmio::selftest's header comment for why that's
    // still a valid test of the abstraction's read/write/offset logic).
    check(&mut pass, &mut fail, 128, "mmio abstraction", crate::mmio::selftest());

    // 9. M+2's virtio_pci notify-address bounds/overflow defense,
    // exercised synthetically (no real device needed -- see that
    // module's own selftest_bounds_synthetic() doc comment).
    check(&mut pass, &mut fail, 256, "virtio_pci notify bounds check", crate::virtio_pci::selftest_bounds_synthetic());

    crate::klog_fmt!("toxenos_rs: selftest: pass={} fail_mask={:#x}", pass, fail);
    if fail == 0 {
        console::log_line("toxenos_rs: selftest: all passed");
    }
    fail as i32
}
