//! Milestone 31: a DMA buffer abstraction that keeps the kernel virtual
//! address, the physical/DMA address a device actually needs, the
//! allocation's size, and its ownership all explicit and inseparable --
//! see the milestone's own instruction that DMA cannot be made
//! genuinely safe, only have its unsafety concentrated and documented.
//!
//! Backed by `physmem64_alloc_pages`, which (per include/physmem64.h)
//! guarantees physical contiguity and zero-fills on allocation -- the
//! same primitive Milestone 28's C storage drivers (AHCI/NVMe/VirtIO)
//! use for their own command buffers, so a Rust driver gets identical
//! guarantees.
use crate::ffi;

/// An owned, physically-contiguous DMA-capable buffer of whole 4KB
/// pages. Dropping it frees the physical pages via
/// `physmem64_free_pages` -- there is no way to "leak on purpose" other
/// than `core::mem::forget`, matching how every other kernel allocation
/// wrapper in this codebase behaves.
///
/// Alignment: `physmem64_alloc_pages` always returns page-aligned
/// (4096-byte) physical memory, which covers every alignment
/// requirement this milestone's drivers need (AHCI/NVMe/VirtIO command
/// structures all require at most page alignment). A future driver
/// needing coarser alignment (e.g. a 64KB-aligned ring) is explicitly
/// NOT handled here -- document that as a gap rather than silently
/// under-aligning.
pub struct DmaBuffer {
    virt: *mut u8,
    phys: u64,
    pages: u64,
}

impl DmaBuffer {
    /// Allocates `pages` physically-contiguous, zero-filled 4KB pages.
    /// Returns `None` on allocation failure (mirroring
    /// `physmem64_alloc_pages`'s own `0 == failure` contract) -- never
    /// panics, since a DMA allocation failure is an ordinary, expected
    /// runtime condition a driver must handle, not a programming error.
    pub fn alloc_pages(pages: u64) -> Option<Self> {
        if pages == 0 {
            return None;
        }
        // SAFETY: physmem64_alloc_pages has no preconditions beyond
        // "the allocator is initialized," which is always true by the
        // time any driver (C or Rust) can run.
        let phys = unsafe { ffi::physmem64_alloc_pages(pages) };
        if phys == 0 {
            return None;
        }
        // SAFETY: `phys` was just allocated by physmem64 itself, so it
        // is guaranteed to be within the physical direct-map window
        // physmem64_to_virt covers.
        let virt = unsafe { ffi::physmem64_to_virt(phys) } as *mut u8;
        Some(DmaBuffer { virt, phys, pages })
    }

    /// The kernel virtual address a driver reads/writes through.
    pub fn virt_ptr(&self) -> *mut u8 {
        self.virt
    }

    /// The physical/DMA address to program into a device's registers or
    /// descriptor. Never the same value as [`DmaBuffer::virt_ptr`] on
    /// this higher-half kernel -- a driver must never hand a device the
    /// virtual address by mistake (see this module's own doc comment).
    pub fn phys_addr(&self) -> u64 {
        self.phys
    }

    /// Total size in bytes.
    pub fn len(&self) -> u64 {
        self.pages * 4096
    }

    pub fn is_empty(&self) -> bool {
        false // pages == 0 is rejected by alloc_pages; a live DmaBuffer is never zero-sized
    }

    /// Byte slice over the buffer for CPU-side reads.
    ///
    /// # Safety
    /// The caller must ensure no in-flight DMA write from the device is
    /// concurrently modifying this memory -- this crate has no way to
    /// know a device's completion state; that is the driver's own
    /// protocol-level responsibility (matching every C storage driver's
    /// existing polling-then-read pattern).
    pub unsafe fn as_slice(&self) -> &[u8] {
        core::slice::from_raw_parts(self.virt, self.len() as usize)
    }

    /// Mutable byte slice over the buffer for CPU-side writes (e.g.
    /// building a command descriptor before handing its physical
    /// address to a device).
    ///
    /// # Safety
    /// Same contract as [`DmaBuffer::as_slice`], plus: the caller must
    /// not write into a region the device is concurrently reading in a
    /// way that could observe a torn/partial update.
    pub unsafe fn as_mut_slice(&mut self) -> &mut [u8] {
        core::slice::from_raw_parts_mut(self.virt, self.len() as usize)
    }
}

impl Drop for DmaBuffer {
    fn drop(&mut self) {
        // SAFETY: `self.phys`/`self.pages` were returned together by
        // the same physmem64_alloc_pages call in `alloc_pages` and
        // never mutated afterward, so they still describe exactly the
        // allocation being freed. The driver is responsible for never
        // dropping a DmaBuffer a device might still be actively using
        // -- this type cannot know a device's completion state (same
        // limitation as `as_slice`/`as_mut_slice` above).
        unsafe { ffi::physmem64_free_pages(self.phys, self.pages) };
    }
}
