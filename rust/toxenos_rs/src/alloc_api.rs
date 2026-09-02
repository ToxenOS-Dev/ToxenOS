//! Milestone 31: wires Rust's `alloc` crate to the existing Milestone 22
//! kernel heap (kernel/heap64.c's kmalloc/kfree) via `#[global_allocator]`.
//!
//! Always routes through `kmalloc_aligned`/`kfree_aligned` rather than
//! plain `kmalloc`/`kfree`, even for small/unaligned requests: `Layout`'s
//! alignment is not generally knowable to satisfy from plain `kmalloc`
//! alone (its default alignment guarantee is an implementation detail of
//! kernel/heap64.c this crate should not need to assume), and
//! `kmalloc_aligned` already exists as the general-purpose primitive for
//! exactly this. `Layout::align()` is always a nonzero power of two by
//! `Layout`'s own invariant, matching what `kmalloc_aligned` requires.
use crate::ffi;
use core::alloc::{GlobalAlloc, Layout};
use core::ffi::c_void;

pub struct ToxenAllocator;

unsafe impl GlobalAlloc for ToxenAllocator {
    unsafe fn alloc(&self, layout: Layout) -> *mut u8 {
        let size = layout.size() as u64;
        let align = layout.align() as u64;
        if size == 0 {
            // A zero-size allocation must return some non-null, non-
            // dangling-for-dealloc-purposes pointer per GlobalAlloc's
            // contract; align itself is always a valid such pointer.
            return align as *mut u8;
        }
        ffi::kmalloc_aligned(size, align) as *mut u8
    }

    unsafe fn dealloc(&self, ptr: *mut u8, layout: Layout) {
        if layout.size() == 0 {
            return; // never actually allocated -- see `alloc` above
        }
        ffi::kfree_aligned(ptr as *mut c_void);
    }
}

#[global_allocator]
static ALLOCATOR: ToxenAllocator = ToxenAllocator;
