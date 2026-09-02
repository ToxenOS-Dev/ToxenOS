//! Milestone 31: freestanding `memcpy`/`memset`/`memcmp`/`memmove`.
//!
//! `core`'s own codegen (slice copies, struct moves, `derive(Clone)`,
//! etc.) can lower to calls to these exact C-runtime-named symbols
//! rather than inlining a loop, same as GCC does for the equivalent C
//! code. On a hosted target these come from libc; this kernel has none.
//! `compiler_builtins`' own "mem" feature is the usual no_std answer,
//! but (checked directly -- see Cargo.toml's dependency comment) it
//! requires nightly-only internals in the version available here, so
//! these are plain, simple, byte-at-a-time freestanding implementations
//! instead. Correctness over speed: nothing in this crate is
//! performance-sensitive yet, and a future real driver needing fast
//! bulk copy can special-case it the same way kernel/ata64.c or any
//! other C file would.
//!
//! `#[no_mangle]` + `extern "C"` so the linker resolves the standard
//! symbol names exactly as `core`'s generated calls expect -- these are
//! genuinely unsafe raw-pointer C ABI functions by nature, not merely by
//! FFI convention.

#[no_mangle]
pub unsafe extern "C" fn memcpy(dest: *mut u8, src: *const u8, n: usize) -> *mut u8 {
    let mut i = 0;
    while i < n {
        *dest.add(i) = *src.add(i);
        i += 1;
    }
    dest
}

#[no_mangle]
pub unsafe extern "C" fn memmove(dest: *mut u8, src: *const u8, n: usize) -> *mut u8 {
    if (dest as usize) < (src as usize) {
        let mut i = 0;
        while i < n {
            *dest.add(i) = *src.add(i);
            i += 1;
        }
    } else {
        let mut i = n;
        while i > 0 {
            i -= 1;
            *dest.add(i) = *src.add(i);
        }
    }
    dest
}

#[no_mangle]
pub unsafe extern "C" fn memset(dest: *mut u8, val: i32, n: usize) -> *mut u8 {
    let b = val as u8;
    let mut i = 0;
    while i < n {
        *dest.add(i) = b;
        i += 1;
    }
    dest
}

#[no_mangle]
pub unsafe extern "C" fn memcmp(a: *const u8, b: *const u8, n: usize) -> i32 {
    let mut i = 0;
    while i < n {
        let av = *a.add(i);
        let bv = *b.add(i);
        if av != bv {
            return av as i32 - bv as i32;
        }
        i += 1;
    }
    0
}
