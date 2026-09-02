//! Milestone 31: a minimal volatile-register abstraction so a future
//! Rust driver never needs to write raw pointer arithmetic + manual
//! `read_volatile`/`write_volatile` itself. This does NOT make MMIO
//! access safe -- it can't be: reading or writing a hardware register
//! can have arbitrary side effects the type system knows nothing about,
//! the address must genuinely be backed by device memory (mapped via
//! `physmem64_map_mmio`, never guessed), and nothing here prevents two
//! overlapping `Mmio` regions from aliasing the same registers. The
//! point is to concentrate that unsafety in one small, obviously-named
//! place instead of letting it spread through every driver that touches
//! hardware.
use core::marker::PhantomData;
use core::ptr;

/// A single MMIO register of type `T` (typically `u8`/`u16`/`u32`/`u64`)
/// at a fixed byte offset within a mapped region. Never constructed
/// directly by driver code -- see [`MmioRegion::reg`].
pub struct Mmio<T> {
    addr: *mut T,
    _marker: PhantomData<T>,
}

impl<T: Copy> Mmio<T> {
    /// Reads the register with a volatile load.
    ///
    /// # Safety
    /// `self` must have been constructed over an address that is
    /// genuinely mapped, currently valid device MMIO memory (see
    /// [`MmioRegion::new`]'s safety contract) -- this is not re-checked
    /// on every access.
    #[inline]
    pub unsafe fn read(&self) -> T {
        ptr::read_volatile(self.addr)
    }

    /// Writes the register with a volatile store.
    ///
    /// # Safety
    /// Same contract as [`Mmio::read`], plus whatever additional
    /// preconditions the specific hardware register documents for
    /// writes (side effects, required bit patterns, ordering relative
    /// to other registers, etc.) -- this abstraction has no way to know
    /// or enforce those.
    #[inline]
    pub unsafe fn write(&self, val: T) {
        ptr::write_volatile(self.addr, val)
    }
}

/// A mapped MMIO region: a base virtual address (already produced by
/// `physmem64_map_mmio`, NEVER a raw guessed/physical address) plus its
/// byte length, used only to bounds-check `reg()` offsets.
pub struct MmioRegion {
    base: *mut u8,
    len: usize,
}

impl MmioRegion {
    /// Wraps an already-mapped MMIO base address.
    ///
    /// # Safety
    /// `base` must be a virtual address returned by
    /// `physmem64_map_mmio` (or an offset cleanly within such a
    /// mapping), valid for `len` bytes, for as long as this
    /// `MmioRegion` (and anything derived from it) is used. This
    /// function does not map anything itself -- see
    /// [`crate::dma`]'s analogous split between "obtaining" and
    /// "using" a physical resource.
    pub unsafe fn new(base: *mut u8, len: usize) -> Self {
        MmioRegion { base, len }
    }

    /// Returns a typed register accessor at `offset` bytes from the
    /// region's base. Panics (a deliberate, loud kernel-side bug signal,
    /// not a hardware condition) if the register would fall outside the
    /// mapped region -- this is the one check this module CAN make
    /// safely, so it does.
    pub fn reg<T>(&self, offset: usize) -> Mmio<T> {
        assert!(
            offset + core::mem::size_of::<T>() <= self.len,
            "MmioRegion::reg: offset {} + size {} exceeds mapped region length {}",
            offset,
            core::mem::size_of::<T>(),
            self.len
        );
        // SAFETY: bounds-checked above; the region's own base validity
        // is the caller's responsibility per `MmioRegion::new`'s
        // contract, inherited here.
        let addr = unsafe { self.base.add(offset) } as *mut T;
        Mmio { addr, _marker: PhantomData }
    }
}

/// Milestone 31 self-test for this module, run from
/// `toxenos_rust_selftest` alongside every other case. This can only
/// ever exercise the abstraction's plumbing (typed volatile
/// read/write, offset bounds-checking) over ordinary heap memory --
/// there is no such thing as a "safe to touch" piece of real device
/// MMIO to test against without a specific, already-owned device, and
/// this module's whole point is that touching real MMIO is never
/// safe. A plain `Vec<u8>` buffer is just as valid a target for
/// `ptr::read_volatile`/`write_volatile` as a real mapping -- both are
/// "a valid, live block of memory of the stated length" as far as
/// those operations care -- so it proves the read/write/offset-math
/// logic is correct without needing (or pretending to need) hardware.
pub fn selftest() -> bool {
    // 16 bytes: enough room for a handful of differently-sized,
    // differently-offset registers within one region.
    let mut buf = alloc::vec![0u8; 16];
    // SAFETY: `buf` is a uniquely-owned, live allocation of exactly
    // `buf.len()` bytes for the remainder of this function -- exactly
    // the contract `MmioRegion::new` documents, just satisfied by a
    // heap buffer instead of a `physmem64_map_mmio` mapping.
    let region = unsafe { MmioRegion::new(buf.as_mut_ptr(), buf.len()) };

    // u8 register at offset 0.
    let r8: Mmio<u8> = region.reg(0);
    // u16 register at offset 2 (naturally aligned).
    let r16: Mmio<u16> = region.reg(2);
    // u32 register at offset 4.
    let r32: Mmio<u32> = region.reg(4);
    // u64 register at offset 8 -- reaches exactly to the end (8+8=16).
    let r64: Mmio<u64> = region.reg(8);

    // SAFETY: each `Mmio<T>` above was bounds-checked by `reg()`
    // against `buf`'s real length, and `buf` is not touched by
    // anything else concurrently (single-threaded self-test).
    unsafe {
        r8.write(0xAB);
        r16.write(0xBEEF);
        r32.write(0xDEAD_BEEF);
        r64.write(0x1122_3344_5566_7788);

        let ok = r8.read() == 0xAB
            && r16.read() == 0xBEEF
            && r32.read() == 0xDEAD_BEEF
            && r64.read() == 0x1122_3344_5566_7788;

        // Cross-check directly against the backing buffer's bytes (not
        // just reading back through the same `Mmio` handles) to catch
        // an offset-math bug that happened to be self-consistent.
        let raw_ok = buf[0] == 0xAB
            && u16::from_ne_bytes([buf[2], buf[3]]) == 0xBEEF
            && u32::from_ne_bytes([buf[4], buf[5], buf[6], buf[7]]) == 0xDEAD_BEEF
            && u64::from_ne_bytes([
                buf[8], buf[9], buf[10], buf[11], buf[12], buf[13], buf[14], buf[15],
            ]) == 0x1122_3344_5566_7788;

        ok && raw_ok
    }
}
