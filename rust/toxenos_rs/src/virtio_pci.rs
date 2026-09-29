//! M+2: the reusable modern ("virtio 1.0+") VirtIO-PCI transport --
//! device status/feature negotiation and split-ring virtqueue setup,
//! built entirely on the existing [`crate::mmio`]/[`crate::dma`]
//! abstractions. Every capability region this module touches was
//! already discovered, validated, and mapped by C
//! (kernel/virtio_pci64.c's `virtio_pci64_probe()`) -- see that file's
//! own header comment for why capability PARSING lives only there, not
//! here too. This module owns everything downstream of "here are some
//! already-mapped MMIO regions": the actual register-poking protocol --
//! exactly the part a future Rust VirtIO-GPU driver (Revision 2's own
//! intended first Rust driver) builds directly on top of. Nothing here
//! knows or cares what kind of VirtIO device it's talking to; no
//! GPU-specific concept (a command, a resource ID, a display mode)
//! appears anywhere in this file.
//!
//! Deliberately not here (see this milestone's own scope notes):
//! MSI/MSI-X, interrupt-driven queues, indirect descriptors, packed
//! rings, and anything past a single outstanding descriptor chain at a
//! time -- all left for whenever a real consumer actually needs them.
use crate::console;
use crate::dma::DmaBuffer;
use crate::mmio::MmioRegion;
use core::cell::Cell;
use core::sync::atomic::{compiler_fence, fence, Ordering};

// ── M+11C: named VirtIO ring barriers ────────────────────────────────
// ONE authoritative ring-ordering implementation. C code and the device
// drivers never issue barriers of their own; they call the queue methods
// that use these. Semantics (VirtIO spec 2.7.13/2.7.14):
//
//   virtio_ring_wmb   descriptor / available-ring entry writes are ordered
//                     before the avail.idx store that publishes them, and
//                     that store is ordered before the notification.
//   virtio_ring_rmb   after OBSERVING used.idx, reads of the used entry and
//                     of device-written buffer contents are ordered after it.
//   virtio_ring_mb    full store->load barrier; needed only for notification
//                     suppression (flags/used_event/EVENT_IDX), which this
//                     transport does not use (implemented + kept ready).
//
// x86 implementation: ring memory is ordinary write-back RAM that is
// coherent with the device, and x86-TSO already orders store->store and
// load->load, so wmb/rmb only need to stop the COMPILER from reordering
// (the ring accesses are volatile, but the device-written buffers the
// drivers read afterwards are not). The notify register is strongly
// ordered UC MMIO. `mb` is a real `mfence`.
#[inline(always)]
pub fn virtio_ring_wmb() { compiler_fence(Ordering::Release); }
#[inline(always)]
pub fn virtio_ring_rmb() { compiler_fence(Ordering::Acquire); }
#[inline(always)]
pub fn virtio_ring_mb() { fence(Ordering::SeqCst); }

/// "No vector": the reset value of msix_config / queue_msix_vector, and what a
/// device returns on read when a mapping failed (VirtIO spec 4.1.5.1.2).
pub const VIRTIO_MSI_NO_VECTOR: u16 = 0xFFFF;

// ── FFI mirror of include/virtio_pci64.h ────────────────────────────
// Must stay byte-for-byte in sync with the C definitions -- same
// discipline crate::pci's PciInfoRaw already documents for its own C
// mirror.
#[repr(C)]
#[derive(Clone, Copy)]
pub struct RegionRaw {
    pub present: u32,
    pub mmio_base: u64,
    pub length: u32,
    pub notify_off_multiplier: u32,
}

#[repr(C)]
pub struct TransportInfoRaw {
    pub ok: u32,
    pub common: RegionRaw,
    pub notify: RegionRaw,
    pub isr: RegionRaw,
    pub device: RegionRaw,
    pub pci_cfg: RegionRaw,
}

// ── Device status bits (VirtIO PCI Transport spec, section 2.1) ─────
pub const STATUS_ACKNOWLEDGE: u8 = 1;
pub const STATUS_DRIVER: u8 = 2;
pub const STATUS_DRIVER_OK: u8 = 4;
pub const STATUS_FEATURES_OK: u8 = 8;
pub const STATUS_DEVICE_NEEDS_RESET: u8 = 64;
pub const STATUS_FAILED: u8 = 128;

const VRING_DESC_F_NEXT: u16 = 1;
const VRING_DESC_F_WRITE: u16 = 2;

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum VirtioError {
    /// A `required` feature bit the caller asked for was not offered by
    /// the device -- negotiation aborted before FEATURES_OK, device left
    /// FAILED, per this milestone's "never continue with a
    /// half-initialized transport" instruction.
    RequiredFeatureMissing,
    /// The device cleared FEATURES_OK when read back -- it rejected the
    /// accepted feature subset for a reason this transport has no way to
    /// know; device left FAILED.
    FeaturesRejected,
    /// `queue_size` was 0 (queue doesn't exist / already taken) or not a
    /// power of 2 (the one hard requirement the spec places on it).
    QueueSizeInvalid,
    /// DMA allocation for the descriptor/avail/used rings failed.
    QueueAllocFailed,
    /// `queue_notify_off * notify_off_multiplier` would land outside the
    /// mapped NOTIFY_CFG region -- see [`Transport::notify_addr`].
    NotifyOutOfBounds,
    /// M+11C: the device did not accept the MSI-X vector index written to
    /// queue_msix_vector / msix_config (read back NO_VECTOR or a different
    /// value) -- spec 4.1.5.1.2.2: the driver MUST verify by reading back.
    VectorRejected,
}

fn pages_for(bytes: u64) -> u64 {
    (bytes + 4095) / 4096
}

/// The modern common-configuration register block (virtio_pci_common_cfg,
/// VirtIO PCI Transport spec). Field offsets below are fixed by the spec,
/// confirmed against Linux's own uapi/linux/virtio_pci.h (the spec's
/// reference implementation) -- NOT inferred from kernel/virtio_blk64.c's
/// legacy I/O-port layout, which is an entirely different, older register
/// set this module has nothing to do with.
pub struct CommonCfg {
    region: MmioRegion,
}

impl CommonCfg {
    /// # Safety
    /// `base`/`len` must be the real, already-validated COMMON_CFG
    /// region from a genuine `virtio_pci64_probe()` result.
    unsafe fn new(base: *mut u8, len: usize) -> Self {
        CommonCfg { region: MmioRegion::new(base, len) }
    }

    fn read_device_features(&self, select: u32) -> u32 {
        unsafe {
            self.region.reg::<u32>(0).write(select); // device_feature_select
            self.region.reg::<u32>(4).read() // device_feature
        }
    }
    fn write_driver_features(&self, select: u32, value: u32) {
        unsafe {
            self.region.reg::<u32>(8).write(select);  // driver_feature_select
            self.region.reg::<u32>(12).write(value);  // driver_feature
        }
    }

    /// Reads the full 64-bit device feature bitmap (two 32-bit windows,
    /// select 0 then 1, per spec -- feature bits 64+ don't exist yet in
    /// any real device, so this transport doesn't expose them).
    pub fn read_features64(&self) -> u64 {
        let lo = self.read_device_features(0) as u64;
        let hi = self.read_device_features(1) as u64;
        (hi << 32) | lo
    }
    fn write_features64(&self, features: u64) {
        self.write_driver_features(0, (features & 0xFFFF_FFFF) as u32);
        self.write_driver_features(1, (features >> 32) as u32);
    }

    pub fn status(&self) -> u8 {
        unsafe { self.region.reg::<u8>(20).read() }
    }
    fn set_status(&self, val: u8) {
        unsafe { self.region.reg::<u8>(20).write(val) }
    }

    pub fn num_queues_reported(&self) -> u16 {
        unsafe { self.region.reg::<u16>(18).read() }
    }

    // ── M+11C: MSI-X vector selectors (valid only while the PCI MSI-X capability is enabled) ──
    /// Raw write of `msix_config` (offset 16).
    pub fn write_config_vector_raw(&self, v: u16) { unsafe { self.region.reg::<u16>(16).write(v) } }
    pub fn read_config_vector(&self) -> u16 { unsafe { self.region.reg::<u16>(16).read() } }
    /// Raw select + write of `queue_msix_vector` (offset 26).
    pub fn write_queue_vector_raw(&self, idx: u16, v: u16) {
        self.select_queue(idx);
        unsafe { self.region.reg::<u16>(26).write(v) }
    }
    pub fn read_queue_vector(&self, idx: u16) -> u16 {
        self.select_queue(idx);
        unsafe { self.region.reg::<u16>(26).read() }
    }
    /// Pure verification rule: a mapping succeeded iff the value read back is
    /// the value written (and not NO_VECTOR).
    pub fn vector_accepted(written: u16, read_back: u16) -> bool {
        written != VIRTIO_MSI_NO_VECTOR && read_back == written
    }
    /// Writes queue `idx`'s vector and VERIFIES it by reading back. Spec:
    /// "After mapping an event to vector, the driver MUST verify success by
    /// reading the Vector field value".
    pub fn program_queue_vector(&self, idx: u16, vector: u16) -> Result<(), VirtioError> {
        self.program_with(idx, vector, &|i| self.read_queue_vector(i))
    }
    /// The write-then-VERIFY rule with the readback injectable, so a device that refuses the mapping
    /// (or reads back garbage) is testable without a device that misbehaves on demand.
    pub fn program_with(&self, idx: u16, vector: u16, read_back: &dyn Fn(u16) -> u16) -> Result<(), VirtioError> {
        self.write_queue_vector_raw(idx, vector);
        if Self::vector_accepted(vector, read_back(idx)) { Ok(()) } else { Err(VirtioError::VectorRejected) }
    }
    /// Writes NO_VECTOR (unmap) and verifies the device reports NO_VECTOR.
    pub fn unmap_queue_vector(&self, idx: u16) -> bool {
        self.unmap_with(idx, &|i| self.read_queue_vector(i))
    }
    /// As [`CommonCfg::unmap_queue_vector`] with the readback injectable.
    pub fn unmap_with(&self, idx: u16, read_back: &dyn Fn(u16) -> u16) -> bool {
        self.write_queue_vector_raw(idx, VIRTIO_MSI_NO_VECTOR);
        read_back(idx) == VIRTIO_MSI_NO_VECTOR
    }

    fn select_queue(&self, idx: u16) {
        unsafe { self.region.reg::<u16>(22).write(idx) }
    }
    fn current_queue_size(&self) -> u16 {
        unsafe { self.region.reg::<u16>(24).read() }
    }
    fn set_queue_size(&self, size: u16) {
        unsafe { self.region.reg::<u16>(24).write(size) }
    }
    fn current_queue_notify_off(&self) -> u16 {
        unsafe { self.region.reg::<u16>(30).read() }
    }
    fn set_queue_addrs(&self, desc: u64, avail: u64, used: u64) {
        unsafe {
            self.region.reg::<u64>(32).write(desc);  // queue_desc (lo+hi as one LE u64 -- safe on this little-endian host)
            self.region.reg::<u64>(40).write(avail); // queue_avail
            self.region.reg::<u64>(48).write(used);  // queue_used
        }
    }
    fn set_queue_enable(&self, enable: bool) {
        unsafe { self.region.reg::<u16>(28).write(if enable { 1 } else { 0 }) }
    }

    /// Selects queue `index`, reads its reported size, and clamps it to
    /// `max_size` (a driver-chosen ceiling, e.g. to bound DMA memory use)
    /// if the device offers something larger. `None` if the device
    /// reports 0 (queue doesn't exist, or is already in use).
    pub fn queue_size_for(&self, index: u16, max_size: u16) -> Option<u16> {
        self.select_queue(index);
        let reported = self.current_queue_size();
        if reported == 0 {
            return None;
        }
        Some(reported.min(max_size))
    }
}

/// The full modern transport for one VirtIO device: status/feature
/// negotiation plus everything [`VirtQueue::new`] needs. Constructed
/// from a C-provided, already-mapped [`TransportInfoRaw`] -- this type
/// never maps or unmaps anything itself.
pub struct Transport {
    pub common: CommonCfg,
    notify_base: *mut u8,
    notify_len: usize,
    notify_off_multiplier: u32,
    pub isr: MmioRegion,
    pub device_cfg: Option<MmioRegion>,
}

impl Transport {
    /// # Safety
    /// `info` must be a genuine `TransportInfoRaw` populated by C's
    /// `virtio_pci64_probe()`, with `ok != 0` (every MMIO region it
    /// names real, validated, currently-mapped device memory).
    pub unsafe fn from_raw(info: &TransportInfoRaw) -> Option<Self> {
        if info.ok == 0 {
            return None;
        }
        let common = CommonCfg::new(info.common.mmio_base as *mut u8, info.common.length as usize);
        let isr = MmioRegion::new(info.isr.mmio_base as *mut u8, info.isr.length as usize);
        let device_cfg = if info.device.present != 0 {
            Some(MmioRegion::new(info.device.mmio_base as *mut u8, info.device.length as usize))
        } else {
            None
        };
        Some(Transport {
            common,
            notify_base: info.notify.mmio_base as *mut u8,
            notify_len: info.notify.length as usize,
            notify_off_multiplier: info.notify.notify_off_multiplier,
            isr,
            device_cfg,
        })
    }

    /// Writes device_status = 0 and polls (bounded) for the device to
    /// acknowledge the reset -- the spec allows a device to take some
    /// time here, so this is a real poll, not a fire-and-forget write,
    /// matching every other bounded wait in this transport.
    pub fn reset(&self) {
        let _ = self.reset_verified();
    }

    /// As [`Transport::reset`], returning whether the device really read back 0
    /// (spec: the driver MUST wait for a read of device_status to return 0).
    pub fn reset_verified(&self) -> bool {
        self.reset_verified_with(&|| self.common.status())
    }
    /// As [`Transport::reset_verified`] with the status readback injectable (a device stuck non-zero).
    pub fn reset_verified_with(&self, read_status: &dyn Fn() -> u8) -> bool {
        self.common.set_status(0);
        for _ in 0..100_000u32 {
            if read_status() == 0 {
                return true;
            }
        }
        read_status() == 0
    }

    fn fail(&self) {
        let cur = self.common.status();
        self.common.set_status(cur | STATUS_FAILED);
    }

    /// Runs the full modern status sequence (spec section 3.1) through
    /// FEATURES_OK verification: RESET, ACKNOWLEDGE, DRIVER, feature
    /// read/negotiate, FEATURES_OK, re-read to confirm it stuck. Accepts
    /// every device-offered bit in `required | optional`; a `required`
    /// bit the device does NOT offer aborts before FEATURES_OK is even
    /// set (device left FAILED, `Err` returned) -- this transport never
    /// silently proceeds with fewer than the caller's stated
    /// requirements. Does NOT set DRIVER_OK -- see [`Transport::set_driver_ok`],
    /// kept separate so a caller can finish queue setup first, per spec
    /// ("the driver MUST set DRIVER_OK only after nothing else it does
    /// could report itself as failed").
    pub fn negotiate(&self, required: u64, optional: u64) -> Result<u64, VirtioError> {
        self.reset();
        self.common.set_status(STATUS_ACKNOWLEDGE);
        self.common.set_status(STATUS_ACKNOWLEDGE | STATUS_DRIVER);

        let device_features = self.common.read_features64();
        if (device_features & required) != required {
            self.fail();
            return Err(VirtioError::RequiredFeatureMissing);
        }
        let accepted = device_features & (required | optional);
        self.common.write_features64(accepted);

        self.common.set_status(STATUS_ACKNOWLEDGE | STATUS_DRIVER | STATUS_FEATURES_OK);
        if self.common.status() & STATUS_FEATURES_OK == 0 {
            self.fail();
            return Err(VirtioError::FeaturesRejected);
        }
        Ok(accepted)
    }

    /// Sets DRIVER_OK -- call only once every queue this driver needs is
    /// already set up (see this fn's own note on `negotiate`).
    pub fn set_driver_ok(&self) {
        let cur = self.common.status();
        self.common.set_status(cur | STATUS_DRIVER_OK);
    }

    /// Reads and clears the ISR status byte (clear-on-read, per spec).
    /// Included for completeness/debugging only -- M+2 is polling-based
    /// and never branches on this value; no interrupt infrastructure is
    /// built around it (see this module's own scope note).
    pub fn isr_read_and_clear(&self) -> u8 {
        unsafe { self.isr.reg::<u8>(0).read() }
    }

    /// Computes one queue's notification address: the NOTIFY_CFG
    /// region's own base (already offset by C to the capability's own
    /// `offset` within its BAR) plus `queue_notify_off * notify_off_multiplier`,
    /// bounds-checked against the region's mapped length before it is
    /// ever used. `queue_notify_off` (a `u16`) is widened to `u64`
    /// BEFORE multiplying specifically so the multiplication itself can
    /// never overflow (`u16::MAX as u64 * u32::MAX as u64` fits
    /// comfortably in a `u64`) -- the bounds check below is a separate,
    /// necessary defense against a large-but-non-overflowing result that
    /// still lands outside the actually-mapped region.
    fn notify_addr(&self, queue_notify_off: u16) -> Result<*mut u16, VirtioError> {
        let byte_off = (queue_notify_off as u64) * (self.notify_off_multiplier as u64);
        if byte_off + 2 > self.notify_len as u64 {
            return Err(VirtioError::NotifyOutOfBounds);
        }
        // SAFETY: notify_base/notify_len describe a real mapped region
        // (Transport::from_raw's own contract); byte_off + 2 <= notify_len
        // was just checked above.
        Ok(unsafe { self.notify_base.add(byte_off as usize) as *mut u16 })
    }
}

// ══════════════════════════════════════════════════════════════════════
// M+11C: queue completion ownership, one authoritative used-ring consumer.
// ══════════════════════════════════════════════════════════════════════

/// Which agent is the queue's NORMAL completion owner. Exactly one at any time;
/// `None` is the transient fence used while ownership changes.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
#[repr(u8)]
pub enum QueueOwner {
    Poll = 0,
    Irq = 1,
    None = 2,
}

/// Capacity of the per-queue completion-metadata ring (single-outstanding
/// commands mean at most one entry is ever legitimately queued; the rest is
/// slack for stray/late completions). Explicit `count`, so capacity is the
/// full COMP_CAP -- never COMP_CAP-1.
pub const COMP_CAP: usize = 8;

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum DrainErr {
    /// The caller was not the queue's owner: refused and counted.
    WrongOwner,
}

/// The interrupt/consumer-side state of one virtqueue. It is a `static`-friendly
/// shared object made ONLY of `Cell`s, accessed through `&` references: the
/// completion interrupt handler and the process-context waiter both use it
/// without either holding a `&mut` to anything the other touches (the waiter
/// holds `&mut` to the producer-side `VirtQueue`/device only).
///
/// One authoritative consumer index, `used_consumed`, advanced ONLY by
/// [`QueueShared::drain`] for the current owner. The completion ring holds
/// entries that were consumed from the used ring but not yet retired by the
/// waiter -- there is no separate "observed" index.
pub struct QueueShared {
    used_base: Cell<*mut u8>,
    size: Cell<u16>,
    used_consumed: Cell<u16>,
    owner: Cell<QueueOwner>,
    comp_id: [Cell<u16>; COMP_CAP],
    comp_len: [Cell<u32>; COMP_CAP],
    comp_head: Cell<u8>,
    comp_count: Cell<u8>,
    broken: Cell<bool>,
    inflight: Cell<bool>,
    demote_requested: Cell<bool>,
    // ── counters ──
    pub irq_count: Cell<u64>,
    pub completions: Cell<u64>,
    pub max_per_irq: Cell<u32>,
    pub empty_irqs: Cell<u64>,
    pub consec_empty: Cell<u64>,
    pub max_consec_empty: Cell<u64>,
    pub stale_irqs: Cell<u64>,
    pub wrong_owner: Cell<u64>,
    pub comp_overflow: Cell<u64>,
    pub poll_drains: Cell<u64>,
    pub poll_calls: Cell<u64>,
    pub waits_woken: Cell<u64>,
    pub timeouts: Cell<u64>,
    pub bad_ids: Cell<u64>,
    pub inflight_violations: Cell<u64>,
    pub stale_completions: Cell<u64>,
    pub abandoned_reclaimed: Cell<u64>,
    pub last_irq_tsc: Cell<u64>,
    pub wake_lat_total: Cell<u64>,
    pub wake_lat_max: Cell<u64>,
    pub wake_lat_count: Cell<u64>,
    pub health_probes: Cell<u64>,
}

// SAFETY: ToxenOS is single-CPU; every access is either from process
// context with interrupts arranged by the caller or from the (non-nested)
// completion handler, and the type has no interior references. Shared as
// `&'static` exactly so no `&mut` aliasing exists between handler and waiter.
unsafe impl Sync for QueueShared {}

impl QueueShared {
    pub const fn new() -> Self {
        QueueShared {
            used_base: Cell::new(core::ptr::null_mut()),
            size: Cell::new(0),
            used_consumed: Cell::new(0),
            owner: Cell::new(QueueOwner::None),
            comp_id: [const { Cell::new(0) }; COMP_CAP],
            comp_len: [const { Cell::new(0) }; COMP_CAP],
            comp_head: Cell::new(0),
            comp_count: Cell::new(0),
            broken: Cell::new(false),
            inflight: Cell::new(false),
            demote_requested: Cell::new(false),
            irq_count: Cell::new(0), completions: Cell::new(0), max_per_irq: Cell::new(0),
            empty_irqs: Cell::new(0), consec_empty: Cell::new(0), max_consec_empty: Cell::new(0),
            stale_irqs: Cell::new(0), wrong_owner: Cell::new(0), comp_overflow: Cell::new(0),
            poll_drains: Cell::new(0), poll_calls: Cell::new(0), waits_woken: Cell::new(0),
            timeouts: Cell::new(0), bad_ids: Cell::new(0), inflight_violations: Cell::new(0),
            stale_completions: Cell::new(0), abandoned_reclaimed: Cell::new(0),
            last_irq_tsc: Cell::new(0), wake_lat_total: Cell::new(0), wake_lat_max: Cell::new(0),
            wake_lat_count: Cell::new(0), health_probes: Cell::new(0),
        }
    }

    /// (Re)binds to a freshly created queue's used ring and resets all state.
    pub fn attach(&self, used_base: *mut u8, size: u16, owner: QueueOwner) {
        self.used_base.set(used_base);
        self.size.set(size);
        self.used_consumed.set(0);
        self.owner.set(owner);
        self.comp_head.set(0);
        self.comp_count.set(0);
        self.broken.set(false);
        self.inflight.set(false);
        self.demote_requested.set(false);
        self.irq_count.set(0); self.completions.set(0); self.max_per_irq.set(0);
        self.empty_irqs.set(0); self.consec_empty.set(0); self.max_consec_empty.set(0);
        self.stale_irqs.set(0); self.wrong_owner.set(0); self.comp_overflow.set(0);
        self.poll_drains.set(0); self.poll_calls.set(0); self.waits_woken.set(0);
        self.timeouts.set(0); self.bad_ids.set(0); self.inflight_violations.set(0);
        self.stale_completions.set(0); self.abandoned_reclaimed.set(0);
        self.last_irq_tsc.set(0); self.wake_lat_total.set(0); self.wake_lat_max.set(0);
        self.wake_lat_count.set(0); self.health_probes.set(0);
    }

    /// Test/inspection: start the consumer index at an arbitrary value
    /// (e.g. near the 16-bit wrap).
    pub fn set_used_consumed(&self, v: u16) { self.used_consumed.set(v); }
    pub fn used_consumed(&self) -> u16 { self.used_consumed.get() }
    pub fn size(&self) -> u16 { self.size.get() }
    pub fn owner(&self) -> QueueOwner { self.owner.get() }
    pub fn set_owner(&self, o: QueueOwner) { self.owner.set(o); }
    pub fn broken(&self) -> bool { self.broken.get() }
    pub fn set_broken(&self) { self.broken.set(true); }
    pub fn demote_requested(&self) -> bool { self.demote_requested.get() }
    pub fn request_demote(&self) { self.demote_requested.set(true); }
    /// Chain of the queue is in use by a command (single-outstanding assertion).
    pub fn begin_inflight(&self) { if self.inflight.get() { self.inflight_violations.set(self.inflight_violations.get() + 1); } self.inflight.set(true); }
    pub fn end_inflight(&self) { self.inflight.set(false); }

    /// The device's current `used.idx` (a plain volatile read: NO ordering barrier).
    pub fn used_idx(&self) -> u16 {
        unsafe { core::ptr::read_volatile(self.used_base.get().add(2) as *const u16) }
    }
    /// Unconsumed used entries as the 16-bit modular difference.
    pub fn pending(&self) -> u16 { self.used_idx().wrapping_sub(self.used_consumed.get()) }

    /// THE consumer. Refuses (and counts) unless `who` is the queue's current
    /// owner. Reads used.idx once, then a `virtio_ring_rmb()`, then consumes up
    /// to `max` entries, handing each `(descriptor id, written length)` to
    /// `sink` and advancing `used_consumed` for every one -- so no entry is ever
    /// consumed twice and none is skipped, across 16-bit wraparound. Returns
    /// the number of entries consumed.
    pub fn drain(&self, who: QueueOwner, max: u16, sink: &mut dyn FnMut(u16, u32)) -> Result<u16, DrainErr> {
        if self.owner.get() != who {
            self.wrong_owner.set(self.wrong_owner.get() + 1);
            return Err(DrainErr::WrongOwner);
        }
        let idx = self.used_idx();
        virtio_ring_rmb(); // observe used.idx, THEN read the entry / device-written buffers
        let mut avail = idx.wrapping_sub(self.used_consumed.get());
        let size = self.size.get();
        if avail > size {
            // The device claims more completions than the ring can hold: corrupt. Consume
            // nothing further than one ring's worth and flag the queue.
            self.broken.set(true);
            avail = size;
        }
        let n = avail.min(max);
        for _ in 0..n {
            let pos = self.used_consumed.get();
            let off = 4 + (pos % size) as usize * 8;
            let (id, len) = unsafe {
                let base = self.used_base.get();
                (core::ptr::read_volatile(base.add(off) as *const u32), core::ptr::read_volatile(base.add(off + 4) as *const u32))
            };
            self.used_consumed.set(pos.wrapping_add(1));
            if id >= size as u32 {
                self.bad_ids.set(self.bad_ids.get() + 1);
                continue; // consumed (never re-read) but not delivered
            }
            sink(id as u16, len);
        }
        Ok(n)
    }

    // ── completion-metadata ring (explicit count) ──
    pub fn comp_count(&self) -> u8 { self.comp_count.get() }
    pub fn comp_push(&self, id: u16, len: u32) -> bool {
        let c = self.comp_count.get() as usize;
        if c >= COMP_CAP { return false; }
        let slot = (self.comp_head.get() as usize + c) % COMP_CAP;
        self.comp_id[slot].set(id);
        self.comp_len[slot].set(len);
        self.comp_count.set((c + 1) as u8);
        true
    }
    pub fn comp_pop_front(&self) -> Option<(u16, u32)> {
        let c = self.comp_count.get() as usize;
        if c == 0 { return None; }
        let h = self.comp_head.get() as usize;
        let r = (self.comp_id[h].get(), self.comp_len[h].get());
        self.comp_head.set(((h + 1) % COMP_CAP) as u8);
        self.comp_count.set((c - 1) as u8);
        Some(r)
    }
    pub fn comp_free_slots(&self) -> u16 { (COMP_CAP - self.comp_count.get() as usize) as u16 }

    /// The interrupt-side drain for a queue whose completions are handed to a
    /// process-context waiter through the completion ring. Bounded by the ring's
    /// free slots; an overflow (more completions than the ring can hold) is a
    /// protocol violation for a single-outstanding queue and marks it broken.
    /// Returns the number consumed, or None if this queue is not IRQ-owned
    /// (stale interrupt / wrong owner -- nothing touched).
    pub fn irq_drain_to_ring(&self) -> Option<u16> {
        if self.owner.get() != QueueOwner::Irq {
            self.stale_irqs.set(self.stale_irqs.get() + 1);
            return None;
        }
        self.irq_count.set(self.irq_count.get() + 1);
        let free = self.comp_free_slots();
        let pending = self.pending();
        if pending > free {
            self.comp_overflow.set(self.comp_overflow.get() + 1);
            self.broken.set(true);
        }
        let mut sink = |id: u16, len: u32| { let _ = self.comp_push(id, len); };
        let n = match self.drain(QueueOwner::Irq, free, &mut sink) { Ok(n) => n, Err(_) => 0 };
        self.note_irq_result(n);
        Some(n)
    }

    /// Per-interrupt accounting: progress vs an empty interrupt.
    pub fn note_irq_result(&self, n: u16) {
        if n == 0 {
            self.empty_irqs.set(self.empty_irqs.get() + 1);
            let c = self.consec_empty.get() + 1;
            self.consec_empty.set(c);
            if c > self.max_consec_empty.get() { self.max_consec_empty.set(c); }
        } else {
            self.consec_empty.set(0);
            self.completions.set(self.completions.get() + n as u64);
            if (n as u32) > self.max_per_irq.get() { self.max_per_irq.set(n as u32); }
        }
    }
}

/// A generic split-ring virtqueue's PRODUCER side: initialize, allocate/free a
/// descriptor, submit a chain, notify. The consumer side (used ring) lives in
/// the `&'static QueueShared` it is bound to. No VirtIO-GPU (or any other
/// device-specific) concept appears anywhere in this type.
pub struct VirtQueue {
    index: u16,
    size: u16,
    _desc_buf: DmaBuffer,
    _avail_buf: DmaBuffer,
    _used_buf: DmaBuffer,
    desc: MmioRegion,
    avail: MmioRegion,
    notify_ptr: *mut u16,
    free_head: u16,
    free_count: u16,
    shared: &'static QueueShared,
}

impl VirtQueue {
    /// Poll-mode queue (vector = NO_VECTOR) with its own leaked shared state --
    /// the pre-M+11C constructor, kept for the self-tests and callers that do not
    /// use interrupts.
    pub fn new(transport: &Transport, index: u16, max_size: u16) -> Result<Self, VirtioError> {
        let shared: &'static QueueShared = alloc::boxed::Box::leak(alloc::boxed::Box::new(QueueShared::new()));
        Self::new_with_vector(transport, index, max_size, VIRTIO_MSI_NO_VECTOR, shared, QueueOwner::Poll)
    }

    /// Selects queue `index`, negotiates a size, allocates the descriptor table /
    /// available ring / used ring, programs their physical addresses, computes and
    /// validates the notification address, initialises the available ring
    /// (`flags = 0`, `idx = 0`: interrupts wanted, no suppression, no EVENT_IDX),
    /// and -- per the VirtIO spec ("the driver MUST configure the other virtqueue
    /// fields before enabling the virtqueue with queue_enable") -- writes and
    /// VERIFIES `queue_msix_vector` (if `vector != NO_VECTOR`) BEFORE `queue_enable`.
    /// A vector the device refuses fails construction (`VectorRejected`) with the
    /// queue never enabled. `owner` becomes the queue's normal completion owner
    /// (a queue with a vector is born IRQ-owned).
    pub fn new_with_vector(transport: &Transport, index: u16, max_size: u16, vector: u16,
                           shared: &'static QueueShared, owner: QueueOwner) -> Result<Self, VirtioError> {
        let size = transport
            .common
            .queue_size_for(index, max_size)
            .ok_or(VirtioError::QueueSizeInvalid)?;
        if size == 0 || (size & (size - 1)) != 0 {
            return Err(VirtioError::QueueSizeInvalid);
        }

        let desc_bytes = size as u64 * 16;
        let avail_bytes = 6 + size as u64 * 2;
        let used_bytes = 6 + size as u64 * 8;

        let desc_buf = DmaBuffer::alloc_pages(pages_for(desc_bytes)).ok_or(VirtioError::QueueAllocFailed)?;
        let avail_buf = DmaBuffer::alloc_pages(pages_for(avail_bytes)).ok_or(VirtioError::QueueAllocFailed)?;
        let used_buf = DmaBuffer::alloc_pages(pages_for(used_bytes)).ok_or(VirtioError::QueueAllocFailed)?;

        // SAFETY: each buffer is a freshly, uniquely allocated DmaBuffer of at least
        // the stated length for as long as this VirtQueue lives; the device can write
        // it asynchronously, so every access is a real volatile load/store.
        let desc = unsafe { MmioRegion::new(desc_buf.virt_ptr(), desc_bytes as usize) };
        let avail = unsafe { MmioRegion::new(avail_buf.virt_ptr(), avail_bytes as usize) };

        transport.common.select_queue(index);
        transport.common.set_queue_size(size);
        transport
            .common
            .set_queue_addrs(desc_buf.phys_addr(), avail_buf.phys_addr(), used_buf.phys_addr());

        let notify_off = transport.common.current_queue_notify_off();
        let notify_ptr = transport.notify_addr(notify_off)?;

        for i in 0..size {
            let next = if i + 1 < size { i + 1 } else { 0 };
            unsafe { desc.reg::<u16>(i as usize * 16 + 14).write(next) };
        }
        // Interrupts wanted (no NO_INTERRUPT suppression); EVENT_IDX is not negotiated.
        unsafe { avail.reg::<u16>(0).write(0); avail.reg::<u16>(2).write(0); }

        if vector != VIRTIO_MSI_NO_VECTOR {
            transport.common.program_queue_vector(index, vector)?;   // verified BEFORE queue_enable
        }

        transport.common.set_queue_enable(true);
        shared.attach(used_buf.virt_ptr(), size, owner);

        Ok(VirtQueue {
            index,
            size,
            _desc_buf: desc_buf,
            _avail_buf: avail_buf,
            _used_buf: used_buf,
            desc,
            avail,
            notify_ptr,
            free_head: 0,
            free_count: size,
            shared,
        })
    }

    pub fn size(&self) -> u16 { self.size }
    pub fn index(&self) -> u16 { self.index }
    pub fn shared(&self) -> &'static QueueShared { self.shared }
    /// Descriptors currently on the free list (tests / diagnostics).
    pub fn free_count(&self) -> u16 { self.free_count }

    /// Allocates one free descriptor index, or `None` if the queue is full.
    pub fn alloc_desc(&mut self) -> Option<u16> {
        if self.free_count == 0 {
            return None;
        }
        let idx = self.free_head;
        self.free_head = unsafe { self.desc.reg::<u16>(idx as usize * 16 + 14).read() };
        self.free_count -= 1;
        Some(idx)
    }

    /// Returns a descriptor to the free list. Caller's responsibility to only do
    /// this once the device is genuinely done with it (i.e. it appeared in the
    /// used ring) -- an in-flight/abandoned descriptor must NOT be freed.
    pub fn free_desc(&mut self, idx: u16) {
        unsafe { self.desc.reg::<u16>(idx as usize * 16 + 14).write(self.free_head) };
        self.free_head = idx;
        self.free_count += 1;
    }

    pub fn set_desc(&mut self, idx: u16, addr: u64, len: u32, device_writable: bool, next: Option<u16>) {
        let flags = (if device_writable { VRING_DESC_F_WRITE } else { 0 })
            | (if next.is_some() { VRING_DESC_F_NEXT } else { 0 });
        let base = idx as usize * 16;
        unsafe {
            self.desc.reg::<u64>(base).write(addr);
            self.desc.reg::<u32>(base + 8).write(len);
            self.desc.reg::<u16>(base + 12).write(flags);
            self.desc.reg::<u16>(base + 14).write(next.unwrap_or(0));
        }
    }

    /// Publishes `head` into the available ring: entry written, WMB, then the
    /// `avail.idx` store that makes it visible. Does NOT notify.
    pub fn submit_chain(&mut self, head: u16) {
        unsafe {
            let idx = self.avail.reg::<u16>(2).read();
            let ring_off = 4 + (idx % self.size) as usize * 2;
            self.avail.reg::<u16>(ring_off).write(head);
            virtio_ring_wmb(); // descriptors + avail entry before the idx that publishes them
            self.avail.reg::<u16>(2).write(idx.wrapping_add(1));
        }
    }

    /// MMIO notification (queue index). WMB first: the published `avail.idx`
    /// precedes the doorbell. NEVER call before DRIVER_OK (spec 3.1.1).
    pub fn notify(&self) {
        virtio_ring_wmb();
        unsafe { core::ptr::write_volatile(self.notify_ptr, self.index) };
    }

    /// Poll-mode consumption for the current POLL owner, with the queue's own
    /// bounded busy-wait. Used by early bring-up and the polling fallback only.
    pub fn poll_drain(&self, sink: &mut dyn FnMut(u16, u32)) -> Result<u16, DrainErr> {
        let r = self.shared.drain(QueueOwner::Poll, self.size, sink);
        // `poll_calls` counts polls the ownership rule ALLOWED. A refused attempt on an IRQ-owned
        // queue is a wrong-owner assertion (counted by `drain`), NOT polling.
        if r.is_ok() { self.shared.poll_calls.set(self.shared.poll_calls.get() + 1); }
        if let Ok(n) = r { if n > 0 { self.shared.poll_drains.set(self.shared.poll_drains.get() + n as u64); } }
        r
    }
}

// ── Synthetic bounds-check test -- no real device needed ────────────
// Proves [`Transport::notify_addr`]'s overflow/bounds defense with a
// deliberately hostile `notify_off_multiplier`, using plain heap buffers
// in place of real device MMIO -- exactly [`crate::mmio`]'s own
// selftest's technique (a `Vec<u8>` is just as valid a
// read_volatile/write_volatile target as a real mapping; the point
// being tested is the arithmetic, not real hardware behavior). Also
// exercises the "allocation failure midway through queue setup must not
// leak" property structurally: the DMA buffers `VirtQueue::new`
// allocates before it reaches the notify-bounds check are ordinary
// `DmaBuffer` locals, freed automatically via `Drop` the moment this
// function returns `Err` early -- a property Rust's ownership model
// guarantees here, not something this test needs to separately assert.
/// Builds a [`Transport`] over heap buffers (no device): common cfg pre-seeded with queue_size 16 and
/// queue_notify_off 0, a 64-byte notify region (multiplier 4). Used by the device-free self-tests.
pub fn with_synthetic_transport<R>(f: impl FnOnce(&Transport) -> R) -> Option<R> {
    let mut common_buf = alloc::vec![0u8; 64];
    let mut notify_buf = alloc::vec![0u8; 64];
    let mut isr_buf = alloc::vec![0u8; 4];
    common_buf[24..26].copy_from_slice(&16u16.to_ne_bytes());
    common_buf[30..32].copy_from_slice(&0u16.to_ne_bytes());
    let absent = RegionRaw { present: 0, mmio_base: 0, length: 0, notify_off_multiplier: 0 };
    let info = TransportInfoRaw {
        ok: 1,
        common: RegionRaw { present: 1, mmio_base: common_buf.as_mut_ptr() as u64, length: common_buf.len() as u32, notify_off_multiplier: 0 },
        notify: RegionRaw { present: 1, mmio_base: notify_buf.as_mut_ptr() as u64, length: notify_buf.len() as u32, notify_off_multiplier: 4 },
        isr: RegionRaw { present: 1, mmio_base: isr_buf.as_mut_ptr() as u64, length: isr_buf.len() as u32, notify_off_multiplier: 0 },
        device: absent, pci_cfg: absent,
    };
    // SAFETY: every region is a live, uniquely-owned heap buffer at least as long as its declared length for
    // the whole call (the same technique as selftest_bounds_synthetic).
    let t = unsafe { Transport::from_raw(&info) }?;
    Some(f(&t))
}

pub fn selftest_bounds_synthetic() -> bool {
    let mut common_buf = alloc::vec![0u8; 64];
    let mut notify_buf = alloc::vec![0u8; 16]; // deliberately small
    let mut isr_buf = alloc::vec![0u8; 4];

    // Seed queue_size (offset 24, u16) = 4 (a valid power of 2) and
    // queue_notify_off (offset 30, u16) = 0xFFFF, so queue setup gets
    // far enough to reach the notify-address computation.
    common_buf[24..26].copy_from_slice(&4u16.to_ne_bytes());
    common_buf[30..32].copy_from_slice(&0xFFFFu16.to_ne_bytes());

    let info = TransportInfoRaw {
        ok: 1,
        common: RegionRaw { present: 1, mmio_base: common_buf.as_mut_ptr() as u64, length: common_buf.len() as u32, notify_off_multiplier: 0 },
        // queue_notify_off (0xFFFF) * notify_off_multiplier (0x10000) is
        // ~4GiB -- comfortably nonzero after widening to u64 (so it does
        // NOT wrap), but wildly outside notify_buf's real 16-byte
        // length. Exactly the "large but non-overflowing result that
        // still lands outside the region" case this check exists for.
        notify: RegionRaw { present: 1, mmio_base: notify_buf.as_mut_ptr() as u64, length: notify_buf.len() as u32, notify_off_multiplier: 0x10000 },
        isr: RegionRaw { present: 1, mmio_base: isr_buf.as_mut_ptr() as u64, length: isr_buf.len() as u32, notify_off_multiplier: 0 },
        device: RegionRaw { present: 0, mmio_base: 0, length: 0, notify_off_multiplier: 0 },
        pci_cfg: RegionRaw { present: 0, mmio_base: 0, length: 0, notify_off_multiplier: 0 },
    };

    // SAFETY: every region above is a real, live, uniquely-owned Vec
    // buffer at least as long as its own declared `length`, for the
    // duration of this function -- exactly MmioRegion::new's contract,
    // satisfied by heap memory instead of a real mapping (see this
    // function's own doc comment).
    let transport = match unsafe { Transport::from_raw(&info) } {
        Some(t) => t,
        None => {
            console::log_line("toxenos_rs: virtio_pci selftest_bounds_synthetic: from_raw returned None unexpectedly");
            return false;
        }
    };

    let result = VirtQueue::new(&transport, 0, 256);
    let rejected = matches!(result, Err(VirtioError::NotifyOutOfBounds));
    if !rejected {
        console::log_line("toxenos_rs: virtio_pci selftest_bounds_synthetic: FAILED to reject an out-of-bounds notify address");
    }
    rejected
}

// ── M+2 self-test ────────────────────────────────────────────────────
// Unlike crate::mmio's/crate::pci's own selftest()s, this one genuinely
// needs a real modern VirtIO device behind `info` -- there is no
// synthetic stand-in for "a device that actually implements the status/
// feature-negotiation protocol." kernel/kernel64.c finds one via
// pci64_find_device + virtio_pci64_probe (under a debug flag, since this
// touches real hardware state) and hands the result here, mirroring
// exactly how toxenos_rust_pci_demo already receives a PciInfoRaw from C.
//
// Deliberately stops at "transport + one queue initialized" -- notify()/
// poll_used() are exercised structurally (the queue's own bookkeeping:
// alloc/free descriptors, notify-address computation already succeeded
// inside VirtQueue::new) but never actually notify the real device with
// a request, since that would require inventing a fake, meaningless
// command for whatever device happens to be attached. Per this
// milestone's own instruction: stop at successful initialization rather
// than invent device-specific semantics that belong to a later
// milestone.
fn run_selftest(info: &TransportInfoRaw) -> bool {
    if info.ok == 0 {
        console::log_line("toxenos_rs: virtio_pci selftest: transport not ok (missing common/notify/isr cfg)");
        return false;
    }
    // SAFETY: `info` is a genuine virtio_pci64_probe() result (this
    // function's own caller contract, forwarded from run_selftest's
    // caller below), and info.ok != 0 was just checked.
    let transport = match unsafe { Transport::from_raw(info) } {
        Some(t) => t,
        None => {
            console::log_line("toxenos_rs: virtio_pci selftest: Transport::from_raw returned None");
            return false;
        }
    };

    let accepted = match transport.negotiate(0, u64::MAX) {
        Ok(f) => f,
        Err(e) => {
            crate::klog_fmt!("toxenos_rs: virtio_pci selftest: negotiate FAILED: {:?}", e);
            return false;
        }
    };
    crate::klog_fmt!("toxenos_rs: virtio_pci selftest: negotiated features = {:#x}", accepted);

    if transport.common.status() & STATUS_FEATURES_OK == 0 {
        console::log_line("toxenos_rs: virtio_pci selftest: FEATURES_OK did not stick");
        return false;
    }

    let mut vq = match VirtQueue::new(&transport, 0, 256) {
        Ok(q) => q,
        Err(e) => {
            crate::klog_fmt!("toxenos_rs: virtio_pci selftest: queue 0 setup FAILED: {:?}", e);
            return false;
        }
    };
    crate::klog_fmt!("toxenos_rs: virtio_pci selftest: queue 0 size = {}", vq.size());

    // Structural proof of the descriptor allocator: allocate every
    // descriptor the queue has, confirm the queue reports itself full,
    // free them all back, confirm a fresh allocation succeeds again --
    // never touches the device.
    let mut allocated = alloc::vec::Vec::new();
    while let Some(idx) = vq.alloc_desc() {
        allocated.push(idx);
    }
    let full_count_ok = allocated.len() == vq.size() as usize;
    for idx in &allocated {
        vq.set_desc(*idx, 0, 0, false, None);
        vq.free_desc(*idx);
    }
    let refill_ok = vq.alloc_desc().is_some();

    transport.set_driver_ok();
    let driver_ok_set = transport.common.status() & STATUS_DRIVER_OK != 0;

    let ok = full_count_ok && refill_ok && driver_ok_set;
    if !ok {
        crate::klog_fmt!(
            "toxenos_rs: virtio_pci selftest: full_count_ok={} refill_ok={} driver_ok_set={}",
            full_count_ok, refill_ok, driver_ok_set
        );
    } else {
        console::log_line("toxenos_rs: virtio_pci selftest: transport + queue 0 fully initialized, DRIVER_OK set");
    }
    ok
}

/// # Safety
/// `info` must point to a valid, fully-initialized `TransportInfoRaw`
/// (i.e. a `virtio_pci64_transport_info_t` on the C side, populated by
/// `virtio_pci64_probe`) for the duration of this call. Never retained
/// past it. Returns 0 on success, -1 on failure or a NULL pointer.
#[no_mangle]
pub unsafe extern "C" fn toxenos_rust_virtio_pci_selftest(info: *const TransportInfoRaw) -> i32 {
    if info.is_null() {
        console::log_line("toxenos_rs: virtio_pci selftest: NULL info pointer");
        return -1;
    }
    if run_selftest(&*info) {
        0
    } else {
        -1
    }
}


// ══════════════════════════════════════════════════════════════════════
// M+11C deterministic self-tests (no device): selectors, the single
// authoritative used-ring consumer, ownership, the completion ring.
// Heap buffers stand in for the DMA ring exactly as the existing
// selftest_bounds_synthetic does. Returns a bitmask of FAILED groups.
// ══════════════════════════════════════════════════════════════════════
struct FakeRing {
    buf: alloc::vec::Vec<u8>,
    dev_idx: u16,     // the device's used.idx
    size: u16,
}

impl FakeRing {
    fn new(size: u16) -> Self { FakeRing { buf: alloc::vec![0u8; 6 + size as usize * 8], dev_idx: 0, size } }
    fn base(&mut self) -> *mut u8 { self.buf.as_mut_ptr() }
    /// The "device" completes descriptor `id` with `len` bytes: entry first, then the idx store.
    fn complete(&mut self, id: u32, len: u32) {
        let off = 4 + (self.dev_idx % self.size) as usize * 8;
        unsafe {
            core::ptr::write_volatile(self.base().add(off) as *mut u32, id);
            core::ptr::write_volatile(self.base().add(off + 4) as *mut u32, len);
        }
        self.dev_idx = self.dev_idx.wrapping_add(1);
        let idx = self.dev_idx;
        unsafe { core::ptr::write_volatile(self.base().add(2) as *mut u16, idx); }
    }
    fn set_start(&mut self, v: u16) {
        self.dev_idx = v;
        unsafe { core::ptr::write_volatile(self.base().add(2) as *mut u16, v); }
    }
}

fn leak_shared() -> &'static QueueShared { alloc::boxed::Box::leak(alloc::boxed::Box::new(QueueShared::new())) }

/// Total assertions executed by the three M+11C Rust suites (irq / input / gpu): a suite that
/// silently stopped asserting would show up as a smaller number.
pub static SELFTEST_ASSERTIONS: core::sync::atomic::AtomicU32 = core::sync::atomic::AtomicU32::new(0);
#[inline(always)]
pub fn selftest_count() { SELFTEST_ASSERTIONS.fetch_add(1, core::sync::atomic::Ordering::Relaxed); }

#[no_mangle]
pub extern "C" fn toxenos_virtio_irq_selftest_assertions() -> u32 { SELFTEST_ASSERTIONS.load(core::sync::atomic::Ordering::Relaxed) }

pub fn selftest_irq() -> u32 {
    let mut failed = 0u32;
    let mut check = |group: u32, name: &str, ok: bool| { selftest_count(); if !ok { failed |= 1 << group; crate::klog_fmt!("toxenos_rs: virtio irq selftest FAIL [{}]: {}", group, name); } };

    // ── 0: MSI-X selector programming: write/readback, NO_VECTOR, rejection ──
    {
        let mut cfg = alloc::vec![0u8; 64];
        let common = unsafe { CommonCfg::new(cfg.as_mut_ptr(), cfg.len()) };
        check(0, "valid vector accepted after readback", common.program_queue_vector(1, 3).is_ok());
        check(0, "queue_select written", u16::from_ne_bytes([cfg[22], cfg[23]]) == 1);
        check(0, "queue_msix_vector written at offset 26", u16::from_ne_bytes([cfg[26], cfg[27]]) == 3);
        check(0, "NO_VECTOR is never an accepted mapping", !CommonCfg::vector_accepted(VIRTIO_MSI_NO_VECTOR, VIRTIO_MSI_NO_VECTOR));
        check(0, "matching readback accepted", CommonCfg::vector_accepted(5, 5));
        check(0, "mismatching readback rejected", !CommonCfg::vector_accepted(5, 4));
        check(0, "NO_VECTOR readback rejected", !CommonCfg::vector_accepted(5, VIRTIO_MSI_NO_VECTOR));
        // device refuses: the register reads back NO_VECTOR after the write
        common.write_queue_vector_raw(2, 7);
        cfg[26] = 0xFF; cfg[27] = 0xFF;
        check(0, "refused vector detected by readback", !CommonCfg::vector_accepted(7, common.read_queue_vector(2)));
        check(0, "unmap writes and verifies NO_VECTOR", common.unmap_queue_vector(2));
        check(0, "msix_config raw accessors", { common.write_config_vector_raw(VIRTIO_MSI_NO_VECTOR); common.read_config_vector() == VIRTIO_MSI_NO_VECTOR });
        // the write-then-VERIFY plumbing with a device that misbehaves (readback injected)
        check(0, "program: a device that refuses (reads NO_VECTOR) is an Err", matches!(common.program_with(1, 3, &|_| VIRTIO_MSI_NO_VECTOR), Err(VirtioError::VectorRejected)));
        check(0, "program: a mismatching readback is an Err", matches!(common.program_with(1, 3, &|_| 4), Err(VirtioError::VectorRejected)));
        check(0, "program: NO_VECTOR is never a mapping", matches!(common.program_with(1, VIRTIO_MSI_NO_VECTOR, &|_| VIRTIO_MSI_NO_VECTOR), Err(VirtioError::VectorRejected)));
        check(0, "program: the matching readback is Ok", common.program_with(1, 3, &|_| 3).is_ok());
        check(0, "unmap: a device still reporting a vector is NOT unmapped", !common.unmap_with(1, &|_| 3));
        check(0, "unmap: NO_VECTOR readback is unmapped", common.unmap_with(1, &|_| VIRTIO_MSI_NO_VECTOR));
    }
    // ── 0b: reset verification with a stuck device ──
    {
        let r = with_synthetic_transport(|t| (t.reset_verified_with(&|| 1), t.reset_verified_with(&|| 0), t.reset_verified()));
        check(0, "reset: a device stuck non-zero is NOT reset (bounded, false)", matches!(r, Some((false, true, true))));
    }

    // ── 1: drain: 0, 1, many, jump, wraparound; each entry consumed exactly once ──
    {
        let mut ring = FakeRing::new(16);
        let sh = leak_shared();
        sh.attach(ring.base(), 16, QueueOwner::Irq);
        let mut got = alloc::vec::Vec::new();
        let n0 = sh.drain(QueueOwner::Irq, 16, &mut |id, len| got.push((id, len))).unwrap_or(0xFFFF);
        check(1, "0 completions", n0 == 0 && got.is_empty());
        ring.complete(3, 100);
        let n1 = sh.drain(QueueOwner::Irq, 16, &mut |id, len| got.push((id, len))).unwrap_or(0xFFFF);
        check(1, "1 completion", n1 == 1 && got == [(3u16, 100u32)]);
        got.clear();
        for i in 0..7 { ring.complete(i, 10 + i); }
        let n7 = sh.drain(QueueOwner::Irq, 16, &mut |id, len| got.push((id, len))).unwrap_or(0xFFFF);
        check(1, "many completions in order", n7 == 7 && got.iter().enumerate().all(|(k, &(id, len))| id == k as u16 && len == 10 + k as u32));
        let again = sh.drain(QueueOwner::Irq, 16, &mut |_, _| {}).unwrap_or(0xFFFF);
        check(1, "no entry consumed twice", again == 0);
        // used.idx JUMP: the device completes 12 entries before we look
        got.clear();
        for i in 0..12 { ring.complete(i as u32 % 16, i as u32); }
        let nj = sh.drain(QueueOwner::Irq, 16, &mut |id, _| got.push((id, 0))).unwrap_or(0xFFFF);
        check(1, "idx jump drains every entry", nj == 12 && got.len() == 12 && sh.pending() == 0);
        // 16-bit wraparound
        let mut ring2 = FakeRing::new(16);
        let sh2 = leak_shared();
        sh2.attach(ring2.base(), 16, QueueOwner::Irq);
        ring2.set_start(65530); sh2.set_used_consumed(65530);
        let mut ids = alloc::vec::Vec::new();
        for i in 0..10u32 { ring2.complete(i, 0); }                 // crosses 65535 -> 0
        let nw = sh2.drain(QueueOwner::Irq, 16, &mut |id, _| ids.push(id)).unwrap_or(0xFFFF);
        check(1, "16-bit wraparound", nw == 10 && ids == alloc::vec![0u16,1,2,3,4,5,6,7,8,9] && sh2.used_consumed() == 4 && sh2.pending() == 0);
        // max bound: a partial drain leaves the rest pending, then completes
        let mut ring3 = FakeRing::new(16); let sh3 = leak_shared(); sh3.attach(ring3.base(), 16, QueueOwner::Irq);
        for i in 0..5u32 { ring3.complete(i, 0); }
        let mut part = alloc::vec::Vec::new();
        let np = sh3.drain(QueueOwner::Irq, 2, &mut |id, _| part.push(id)).unwrap_or(0xFFFF);
        check(1, "bounded drain leaves the rest pending", np == 2 && sh3.pending() == 3);
        let nr = sh3.drain(QueueOwner::Irq, 16, &mut |id, _| part.push(id)).unwrap_or(0xFFFF);
        check(1, "remainder drained, each once", nr == 3 && part == alloc::vec![0u16,1,2,3,4]);
        // an out-of-range descriptor id is consumed but never delivered
        let mut ring4 = FakeRing::new(16); let sh4 = leak_shared(); sh4.attach(ring4.base(), 16, QueueOwner::Irq);
        ring4.complete(999, 0); ring4.complete(2, 0);
        let mut ok_ids = alloc::vec::Vec::new();
        let nb = sh4.drain(QueueOwner::Irq, 16, &mut |id, _| ok_ids.push(id)).unwrap_or(0xFFFF);
        check(1, "bad id skipped, counted", nb == 2 && ok_ids == alloc::vec![2u16] && sh4.bad_ids.get() == 1);
        // a device that claims MORE completions than the ring can hold is corrupt: one ring's worth at most, queue flagged
        let mut ring5 = FakeRing::new(16); let sh5 = leak_shared(); sh5.attach(ring5.base(), 16, QueueOwner::Irq);
        for i in 0..16u32 { ring5.complete(i, 0); }
        ring5.set_start(ring5.dev_idx.wrapping_add(24));             // used.idx now 40 ahead of what we consumed
        let mut cnt5 = 0u32;
        let n5 = sh5.drain(QueueOwner::Irq, 64, &mut |_, _| cnt5 += 1).unwrap_or(0xFFFF);
        check(1, "used.idx claiming > ring size: bounded to one ring, queue marked broken", n5 == 16 && cnt5 == 16 && sh5.broken());
        let mut ring6 = FakeRing::new(16); let sh6 = leak_shared(); sh6.attach(ring6.base(), 16, QueueOwner::Irq);
        for i in 0..16u32 { ring6.complete(i, 0); }
        check(1, "exactly one full ring is NOT corruption", sh6.drain(QueueOwner::Irq, 64, &mut |_, _| {}).unwrap_or(0xFFFF) == 16 && !sh6.broken());
    }

    // ── 2: ownership: wrong owner refused + counted, NONE fences, handover reconciles ──
    {
        let mut ring = FakeRing::new(16); let sh = leak_shared(); sh.attach(ring.base(), 16, QueueOwner::Irq);
        ring.complete(1, 0);
        check(2, "POLL drain of an IRQ-owned queue refused", sh.drain(QueueOwner::Poll, 16, &mut |_, _| {}) == Err(DrainErr::WrongOwner));
        check(2, "wrong-owner attempt counted", sh.wrong_owner.get() == 1);
        check(2, "refused drain consumed nothing", sh.pending() == 1);
        sh.set_owner(QueueOwner::None);                              // fence
        check(2, "NONE fences both owners", sh.drain(QueueOwner::Irq, 16, &mut |_, _| {}).is_err() && sh.drain(QueueOwner::Poll, 16, &mut |_, _| {}).is_err() && sh.wrong_owner.get() == 3);
        ring.complete(2, 0);                                         // completes during the transition
        check(2, "used.idx reconciliation sees both", sh.pending() == 2);
        sh.set_owner(QueueOwner::Poll);
        let mut ids = alloc::vec::Vec::new();
        let n = sh.drain(QueueOwner::Poll, 16, &mut |id, _| ids.push(id)).unwrap_or(0xFFFF);
        check(2, "new owner drains exactly what is pending, once", n == 2 && ids == alloc::vec![1u16, 2] && sh.pending() == 0);
        check(2, "IRQ owner now refused", sh.drain(QueueOwner::Irq, 16, &mut |_, _| {}).is_err());
        check(2, "stale IRQ against a POLL-owned queue touches nothing", sh.irq_drain_to_ring().is_none() && sh.stale_irqs.get() == 1);
    }

    // ── 3: completion ring: explicit count (full capacity), FIFO, overflow policy ──
    {
        let sh = leak_shared();
        let mut ring = FakeRing::new(16); sh.attach(ring.base(), 16, QueueOwner::Irq);
        let mut all = true;
        for i in 0..COMP_CAP { all &= sh.comp_push(i as u16, i as u32); }
        check(3, "all COMP_CAP slots usable (not COMP_CAP-1)", all && sh.comp_count() as usize == COMP_CAP);
        check(3, "push onto a full ring refused", !sh.comp_push(99, 0));
        let mut order_ok = true;
        for i in 0..COMP_CAP { order_ok &= sh.comp_pop_front() == Some((i as u16, i as u32)); }
        check(3, "FIFO order", order_ok && sh.comp_pop_front().is_none());
        // wrap the head pointer and refill
        for i in 0..3 { sh.comp_push(i, 0); } sh.comp_pop_front(); sh.comp_pop_front();
        let mut refill = true; for i in 0..(COMP_CAP - 1) { refill &= sh.comp_push(50 + i as u16, 0); }
        check(3, "ring reusable after head wrap", refill && sh.comp_count() as usize == COMP_CAP);
        // IRQ overflow: more completions than free slots -> only the free ones consumed, queue marked broken
        let sh2 = leak_shared(); let mut r2 = FakeRing::new(16); sh2.attach(r2.base(), 16, QueueOwner::Irq);
        for i in 0..12u32 { r2.complete(i % 16, i); }
        let n = sh2.irq_drain_to_ring().unwrap_or(0xFFFF);
        check(3, "overflow: bounded by free slots, counted, queue marked broken", n as usize == COMP_CAP && sh2.comp_overflow.get() == 1 && sh2.broken() && sh2.pending() == 4);
    }

    // ── 4: observed vs consumed: two entries before one IRQ, delayed waiter, nothing consumed twice / lost ──
    {
        let sh = leak_shared(); let mut ring = FakeRing::new(16); sh.attach(ring.base(), 16, QueueOwner::Irq);
        ring.complete(7, 24); ring.complete(9, 24);                  // TWO completions before ONE interrupt
        let n = sh.irq_drain_to_ring().unwrap_or(0xFFFF);
        check(4, "one IRQ drains both entries", n == 2 && sh.pending() == 0 && sh.used_consumed() == 2 && sh.comp_count() == 2);
        // the waiter is delayed: a second (empty) interrupt arrives; nothing is re-consumed and nothing vanishes
        let n2 = sh.irq_drain_to_ring().unwrap_or(0xFFFF);
        check(4, "second IRQ finds nothing new (no double consumption)", n2 == 0 && sh.comp_count() == 2 && sh.empty_irqs.get() == 1);
        check(4, "waiter retires the entries in order", sh.comp_pop_front() == Some((7, 24)) && sh.comp_pop_front() == Some((9, 24)) && sh.comp_pop_front().is_none());
        check(4, "counters: irqs, drained", sh.irq_count.get() == 2 && sh.completions.get() == 2 && sh.max_per_irq.get() == 2);
        ring.complete(1, 24);
        check(4, "later completion consumed exactly once", sh.irq_drain_to_ring().unwrap_or(0xFFFF) == 1 && sh.comp_count() == 1);
    }
    failed
}

/// Bitmask of failed groups (0 = all pass).
#[no_mangle]
pub extern "C" fn toxenos_virtio_irq_selftest_pci() -> u32 { selftest_irq() }
