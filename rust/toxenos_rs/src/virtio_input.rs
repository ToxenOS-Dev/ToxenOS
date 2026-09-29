//! M+7A/M+11C: the VirtIO-input driver -- the first real ToxenOS consumer of
//! an absolute pointing device, built on the reusable transport
//! [`crate::virtio_pci`]. Owns the VirtIO-input wire protocol EXCLUSIVELY (spec
//! section 5.8; struct/constant values confirmed against Linux's public uapi
//! headers, independently implemented -- no driver source copied).
//!
//! M+11C: the eventq is completed by a PCI MSI-X interrupt instead of a
//! 100 Hz timer poll. The device is created either
//!   * IRQ-owned  (`init_queues` with an MSI-X entry index): the eventq's
//!                 `queue_msix_vector` is programmed and verified while the PCI
//!                 MSI-X function is ARMED, and the interrupt handler
//!                 ([`toxenos_virtio_input_irq`]) is the queue's only completion
//!                 owner -- the timer never touches it; or
//!   * POLL-owned (NO_VECTOR): the pre-M+11C behaviour, used for the fallback
//!                 and forced-poll builds.
//!
//! The interrupt handler is bounded and allocation-free: it drains EVERY visible
//! used entry (at most the 16 posted buffers), decodes into fixed storage, emits
//! ONE sample per SYN_REPORT into a 16-slot ring, recycles every buffer and kicks
//! once. Per-SYN_REPORT samples mean a fast press+release can no longer collapse
//! into a single latest-state sample.
use crate::console;
use crate::dma::DmaBuffer;
use crate::ffi;
use crate::virtio_pci::{DrainErr, QueueOwner, QueueShared, Transport, TransportInfoRaw, VirtQueue, VIRTIO_MSI_NO_VECTOR};

const EV_SYN: u16 = 0x00;
const EV_KEY: u16 = 0x01;
const EV_ABS: u16 = 0x03;
const SYN_REPORT: u16 = 0x00;
const ABS_X: u16 = 0x00;
const ABS_Y: u16 = 0x01;
const BTN_LEFT: u16 = 0x110;
const BTN_RIGHT: u16 = 0x111;
const BTN_MIDDLE: u16 = 0x112;
const CFG_ABS_INFO: u8 = 0x12;

const VIRTIO_F_VERSION_1: u64 = 1u64 << 32;
const REQUIRED_FEATURES: u64 = VIRTIO_F_VERSION_1;

const NUM_SLOTS: u16 = 16;
const SLOT_BYTES: usize = 8;

/// Samples a single drain can produce: every posted buffer could be a SYN_REPORT.
pub const SAMPLE_CAP: usize = NUM_SLOTS as usize;

/// Storm policy (consecutive empty interrupts with no progress at all).
const STORM_WARN: u64 = 1_000;
const STORM_DEMOTE: u64 = 100_000;

static INPUT_EVQ: QueueShared = QueueShared::new();

#[derive(Clone, Copy, Default, Debug, PartialEq, Eq)]
pub struct Sample {
    pub x: i32,
    pub y: i32,
    pub buttons: u32,
}

/// Fixed-capacity FIFO of samples with an EXPLICIT count (so the effective
/// capacity really is SAMPLE_CAP, never SAMPLE_CAP-1). A push onto a full ring is
/// refused and counted, never a silent overwrite.
pub struct SampleRing {
    s: [Sample; SAMPLE_CAP],
    head: usize,
    count: usize,
    pub overflow: u64,
    pub pushed: u64,
}

impl SampleRing {
    pub const fn new() -> Self {
        SampleRing { s: [Sample { x: 0, y: 0, buttons: 0 }; SAMPLE_CAP], head: 0, count: 0, overflow: 0, pushed: 0 }
    }
    pub fn len(&self) -> usize { self.count }
    pub fn push(&mut self, v: Sample) -> bool {
        if self.count == SAMPLE_CAP {
            self.overflow += 1;
            return false;
        }
        let slot = (self.head + self.count) % SAMPLE_CAP;
        self.s[slot] = v;
        self.count += 1;
        self.pushed += 1;
        true
    }
    pub fn pop(&mut self) -> Option<Sample> {
        if self.count == 0 { return None; }
        let v = self.s[self.head];
        self.head = (self.head + 1) % SAMPLE_CAP;
        self.count -= 1;
        Some(v)
    }
}

/// Pure event decoder: accumulates EV_ABS / EV_KEY into a work-in-progress
/// state and emits one [`Sample`] on every SYN_REPORT. The absolute-axis state
/// persists across frames (a real absolute device only re-reports a changed axis).
pub struct EventDecoder {
    wip_x: i32,
    wip_y: i32,
    wip_buttons: u32,
}

impl EventDecoder {
    pub const fn new() -> Self { EventDecoder { wip_x: 0, wip_y: 0, wip_buttons: 0 } }
    pub fn feed(&mut self, ev_type: u16, code: u16, value: u32) -> Option<Sample> {
        match ev_type {
            t if t == EV_ABS => {
                if code == ABS_X { self.wip_x = value as i32; }
                else if code == ABS_Y { self.wip_y = value as i32; }
                None
            }
            t if t == EV_KEY => {
                let bit = if code == BTN_LEFT { 0x01u32 } else if code == BTN_RIGHT { 0x02u32 } else if code == BTN_MIDDLE { 0x04u32 } else { 0 };
                if bit != 0 { if value != 0 { self.wip_buttons |= bit; } else { self.wip_buttons &= !bit; } }
                None
            }
            t if t == EV_SYN && code == SYN_REPORT => Some(Sample { x: self.wip_x, y: self.wip_y, buttons: self.wip_buttons }),
            _ => None,
        }
    }
}

fn axis_range_from_cfg(device_cfg: &crate::mmio::MmioRegion, axis: u8) -> Option<(i32, i32)> {
    unsafe {
        device_cfg.reg::<u8>(0).write(CFG_ABS_INFO);
        device_cfg.reg::<u8>(1).write(axis);
        let size = device_cfg.reg::<u8>(2).read();
        if size < 8 { return None; }
        let min = device_cfg.reg::<u32>(8).read() as i32;
        let max = device_cfg.reg::<u32>(12).read() as i32;
        Some((min, max))
    }
}

// ── receive-queue accounting (single-threaded / IRQ discipline, see below) ──
static mut STAT_RECEIVED: u64 = 0;
static mut STAT_RECYCLED: u64 = 0;
static mut STAT_SAMPLES: u64 = 0;
static mut STAT_SAMPLE_OVERFLOW: u64 = 0;
static mut STAT_TIMER_POLLS: u64 = 0;

/// Pure ring-walk logic (kept from M+7A for the exhaustive wraparound proof).
fn positions_between(last_seen: u16, current: u16, out: &mut alloc::vec::Vec<u16>) {
    let mut pos = last_seen;
    while pos != current {
        out.push(pos);
        pos = pos.wrapping_add(1);
    }
}

pub fn selftest_ring_wrap() -> bool {
    let mut ok = true;
    let mut buf = alloc::vec::Vec::new();
    buf.clear(); positions_between(5, 5, &mut buf); if !buf.is_empty() { ok = false; }
    buf.clear(); positions_between(65534, 1, &mut buf); if buf != [65534u16, 65535u16, 0u16] { ok = false; }
    buf.clear(); positions_between(65535, 0, &mut buf); if buf != [65535u16] { ok = false; }
    buf.clear(); positions_between(0, 3, &mut buf); if buf != [0u16, 1u16, 2u16] { ok = false; }
    buf.clear(); positions_between(12345, 12345u16.wrapping_sub(1), &mut buf); if buf.len() != 65535 { ok = false; }
    ok
}

/// The whole drain body, independent of any real device so it can be tested over a synthetic ring: consumes
/// every visible entry for owner `who` (bounded by the queue's posted buffers), decodes each event through
/// `read`, emits one sample per SYN_REPORT, and reports which descriptors to recycle. Allocation-free.
fn drain_core(shared: &QueueShared, who: QueueOwner, read: &dyn Fn(u16) -> (u16, u16, u32),
              decoder: &mut EventDecoder, ring: &mut SampleRing, ids: &mut [u16; NUM_SLOTS as usize])
              -> Result<(u16, usize), DrainErr> {
    let mut cnt = 0usize;
    let n = shared.drain(who, NUM_SLOTS, &mut |id, _len| {
        unsafe { STAT_RECEIVED += 1; }
        let (t, c, v) = read(id);
        if let Some(sample) = decoder.feed(t, c, v) {
            if ring.push(sample) { unsafe { STAT_SAMPLES += 1; } } else { unsafe { STAT_SAMPLE_OVERFLOW += 1; } }
        }
        if cnt < ids.len() { ids[cnt] = id; cnt += 1; }
    })?;
    Ok((n, cnt))
}

struct InputBuild {
    transport: Transport,
}

struct InputDevice {
    transport: Transport,
    eventq: VirtQueue,
    buf: DmaBuffer,
    decoder: EventDecoder,
    ring: SampleRing,
    have_abs: bool,
    abs_min_x: i32,
    abs_max_x: i32,
    abs_min_y: i32,
    abs_max_y: i32,
    kicked: bool,
    faulted: bool,
    info_copy: TransportInfoRaw,
}

impl InputDevice {
    /// Decodes the 8-byte event in slot `id` (device-written; the caller has already
    /// executed the ring `rmb`).
    fn read_event(buf: &DmaBuffer, id: u16) -> (u16, u16, u32) {
        let off = (id as usize % NUM_SLOTS as usize) * SLOT_BYTES;
        unsafe {
            let base = buf.virt_ptr().add(off);
            (
                core::ptr::read_unaligned(base as *const u16),
                core::ptr::read_unaligned(base.add(2) as *const u16),
                core::ptr::read_unaligned(base.add(4) as *const u32),
            )
        }
    }

    /// Drains every visible completed receive buffer for owner `who`. Bounded
    /// (<= NUM_SLOTS entries), allocation-free. Returns the number consumed, or
    /// `Err(WrongOwner)` (refused and counted) if `who` does not own the queue.
    fn drain(&mut self, who: QueueOwner) -> Result<u16, DrainErr> {
        let shared = self.eventq.shared();
        let mut ids = [0u16; NUM_SLOTS as usize];
        let buf = &self.buf;
        let (n, cnt) = drain_core(shared, who, &|id| Self::read_event(buf, id), &mut self.decoder, &mut self.ring, &mut ids)?;
        for i in 0..cnt {
            self.eventq.submit_chain(ids[i]);      // recycle every consumed buffer exactly once
            unsafe { STAT_RECYCLED += 1; }
        }
        if cnt > 0 && self.kicked { self.eventq.notify(); }
        Ok(n)
    }

    unsafe fn build_queues(b: InputBuild, vector: u16, info: &TransportInfoRaw) -> Option<Self> {
        let transport = b.transport;
        let owner = if vector == VIRTIO_MSI_NO_VECTOR { QueueOwner::Poll } else { QueueOwner::Irq };
        let mut eventq = VirtQueue::new_with_vector(&transport, 0, NUM_SLOTS, vector, &INPUT_EVQ, owner).ok()?;
        let buf = DmaBuffer::alloc_pages(1)?;
        // Post every receive slot as a device-writable descriptor (NO notification: spec 3.1.1
        // forbids notifying before DRIVER_OK; the kick happens in finish()).
        for i in 0..NUM_SLOTS {
            let d = eventq.alloc_desc()?;
            debug_assert_eq!(d, i);
            eventq.set_desc(d, buf.phys_addr() + (i as u64) * SLOT_BYTES as u64, SLOT_BYTES as u32, true, None);
            eventq.submit_chain(d);
        }
        let (mut amin_x, mut amax_x, mut amin_y, mut amax_y) = (0, 0, 0, 0);
        let have_abs = match &transport.device_cfg {
            Some(cfg) => match (axis_range_from_cfg(cfg, ABS_X as u8), axis_range_from_cfg(cfg, ABS_Y as u8)) {
                (Some((minx, maxx)), Some((miny, maxy))) => { amin_x = minx; amax_x = maxx; amin_y = miny; amax_y = maxy; true }
                _ => false,
            },
            None => false,
        };
        Some(InputDevice {
            transport, eventq, buf, decoder: EventDecoder::new(), ring: SampleRing::new(),
            have_abs, abs_min_x: amin_x, abs_max_x: amax_x, abs_min_y: amin_y, abs_max_y: amax_y,
            kicked: false, faulted: false,
            info_copy: TransportInfoRaw { ok: info.ok, common: info.common, notify: info.notify, isr: info.isr, device: info.device, pci_cfg: info.pci_cfg },
        })
    }
}

static mut INPUT_BUILD: Option<InputBuild> = None;
static mut INPUT_INFO: Option<TransportInfoRaw> = None;
static mut INPUT_DEVICE: Option<InputDevice> = None;

fn dev_ptr() -> *mut Option<InputDevice> { core::ptr::addr_of_mut!(INPUT_DEVICE) }

// ── phased initialisation (driven by kernel/virtio_input64.c + virtio_irq64.c) ──

/// Phase 1: reset, ACKNOWLEDGE, DRIVER, features, FEATURES_OK (spec 3.1.1 steps 1-6).
///
/// # Safety
/// `info` must be a genuine `virtio_pci64_probe()` result for a real modern VirtIO-input device.
#[no_mangle]
pub unsafe extern "C" fn toxenos_virtio_input_init_begin(info: *const TransportInfoRaw) -> i32 {
    if info.is_null() { console::log_line("toxenos_rs: virtio_input: NULL info pointer"); return -1; }
    let transport = match Transport::from_raw(&*info) { Some(t) => t, None => return -1 };
    if transport.negotiate(REQUIRED_FEATURES, 0).is_err() { return -1; }
    *core::ptr::addr_of_mut!(INPUT_INFO) = Some(TransportInfoRaw { ok: (*info).ok, common: (*info).common, notify: (*info).notify, isr: (*info).isr, device: (*info).device, pci_cfg: (*info).pci_cfg });
    *core::ptr::addr_of_mut!(INPUT_BUILD) = Some(InputBuild { transport });
    0
}

/// Phase 2: create the eventq with MSI-X entry `vector` (0xFFFF = NO_VECTOR -> POLL-owned). The
/// vector is written and read back BEFORE queue_enable; a device that refuses it fails this call.
#[no_mangle]
pub unsafe extern "C" fn toxenos_virtio_input_init_queues(vector: u32) -> i32 {
    let bp = core::ptr::addr_of_mut!(INPUT_BUILD);
    let ip = core::ptr::addr_of!(INPUT_INFO);
    let b = match (*bp).take() { Some(b) => b, None => return -1 };
    let info = match (*ip).as_ref() { Some(i) => i, None => return -1 };
    match InputDevice::build_queues(b, vector as u16, info) {
        Some(dev) => { *dev_ptr() = Some(dev); 0 }
        None => { -1 }
    }
}

/// Phase 3: DRIVER_OK, then the first (and only) notification. Never before DRIVER_OK.
#[no_mangle]
pub extern "C" fn toxenos_virtio_input_init_finish() -> i32 {
    let p = dev_ptr();
    match unsafe { (*p).as_mut() } {
        Some(dev) => {
            dev.transport.set_driver_ok();
            dev.kicked = true;
            dev.eventq.notify();
            let owner_irq = INPUT_EVQ.owner() == QueueOwner::Irq;
            crate::klog_fmt!("toxenos_rs: virtio_input: eventq live, owner = {}, abs-capable = {}", if owner_irq { "IRQ" } else { "POLL" }, dev.have_abs);
            0
        }
        None => -1,
    }
}

/// Failure/abort: reset the device (which unmaps every vector) and drop everything.
#[no_mangle]
pub extern "C" fn toxenos_virtio_input_init_abort() {
    unsafe {
        if let Some(b) = (*core::ptr::addr_of_mut!(INPUT_BUILD)).take() { b.transport.reset(); }
        if let Some(d) = (*dev_ptr()).take() { d.transport.reset(); }
    }
}

/// Poll-mode convenience (the pre-M+11C entry point): begin + queues(NO_VECTOR) + finish.
///
/// # Safety
/// As [`toxenos_virtio_input_init_begin`].
#[no_mangle]
pub unsafe extern "C" fn toxenos_virtio_input_init(info: *const TransportInfoRaw) -> i32 {
    if toxenos_virtio_input_init_begin(info) < 0 { return -1; }
    if toxenos_virtio_input_init_queues(VIRTIO_MSI_NO_VECTOR as u32) < 0 { toxenos_virtio_input_init_abort(); return -1; }
    toxenos_virtio_input_init_finish()
}

// ── interrupt path ─────────────────────────────────────────────────────

/// The eventq MSI-X interrupt body (called from the C thunk in hard-IRQ context). Returns
/// the number of used entries consumed (0 = an empty interrupt), or -1 if the queue is not
/// IRQ-owned (a stale interrupt: nothing touched).
#[no_mangle]
pub extern "C" fn toxenos_virtio_input_irq(first: i32) -> i32 {
    let p = dev_ptr();
    let dev = match unsafe { (*p).as_mut() } { Some(d) => d, None => return -1 };
    let shared = dev.eventq.shared();
    if shared.owner() != QueueOwner::Irq {
        if first != 0 { shared.stale_irqs.set(shared.stale_irqs.get() + 1); }
        return -1;
    }
    if first != 0 { shared.irq_count.set(shared.irq_count.get() + 1); }   // one INTERRUPT, however many drain passes
    let n = match dev.drain(QueueOwner::Irq) { Ok(n) => n, Err(_) => return -1 };
    if first != 0 { shared.note_irq_result(n); }
    else if n > 0 { shared.completions.set(shared.completions.get() + n as u64); }
    // Storm policy: conservative. Warn once, then (much later) declare the path broken and demote.
    let c = shared.consec_empty.get();
    if c == STORM_WARN {
        console::log_line("toxenos_rs: virtio_input: WARNING many consecutive empty interrupts (possible storm)");
        unsafe { ffi::virtio_irq64_note(ffi::VIRQ_DEV_INPUT, ffi::VIRQ_EV_STORM_WARN); }
    } else if c == STORM_DEMOTE {
        console::log_line("toxenos_rs: virtio_input: interrupt path declared broken -- demoting to POLL");
        unsafe { ffi::virtio_irq64_note(ffi::VIRQ_DEV_INPUT, ffi::VIRQ_EV_STORM_MASKED); }
        demote_device(dev);
    }
    n as i32
}

/// Timer poll body: only meaningful while the queue is POLL-owned. A call against an
/// IRQ-owned queue is REFUSED and counted as a wrong-owner assertion.
#[no_mangle]
pub extern "C" fn toxenos_virtio_input_poll() {
    let p = dev_ptr();
    let dev = match unsafe { (*p).as_mut() } { Some(d) => d, None => return };
    if dev.faulted { return; }
    let shared = dev.eventq.shared();
    if let Ok(n) = dev.drain(QueueOwner::Poll) {
        // Counted only when the ownership rule ALLOWED the poll; a refused call is a wrong-owner
        // assertion (counted by the drain itself), not polling.
        unsafe { STAT_TIMER_POLLS += 1; }
        shared.poll_calls.set(shared.poll_calls.get() + 1);
        if n > 0 { shared.poll_drains.set(shared.poll_drains.get() + n as u64); }
    }
}

/// Pops the OLDEST pending sample (one per SYN_REPORT). Returns 1 with the fields filled,
/// 0 if none, -1 if no device.
#[no_mangle]
pub unsafe extern "C" fn toxenos_virtio_input_take_sample(x: *mut i32, y: *mut i32, buttons: *mut u32) -> i32 {
    let p = dev_ptr();
    match (*p).as_mut() {
        Some(dev) => match dev.ring.pop() {
            Some(s) => { core::ptr::write(x, s.x); core::ptr::write(y, s.y); core::ptr::write(buttons, s.buttons); 1 }
            None => 0,
        },
        None => -1,
    }
}

// ── demotion (device-wide; input has exactly one queue) ────────────────
/// Verified-silence demotion. Order: fence ownership (NONE), then prove silence:
///  P1  the entry is hardware-masked (read back) AND queue_msix_vector reads NO_VECTOR;
///  P2  the whole PCI IRQ set quiesced (function mask read back);
///  P3  a device reset completed AND the selector reads NO_VECTOR (then the device is
///      re-initialised POLL-owned from scratch).
/// Anything short of a proof is FAULTED: no polling, device unusable, PCI IRQ set stays pinned.
/// Returns the proof id (1..3) or 0 for FAULTED.
fn demote_device(dev: &mut InputDevice) -> u32 {
    let shared = dev.eventq.shared();
    shared.set_owner(QueueOwner::None);               // old owner stopped
    let masked = unsafe { ffi::virtio_irq64_mask_all(ffi::VIRQ_DEV_INPUT) } == 1;
    let unmapped = dev.transport.common.unmap_queue_vector(0) && unsafe { ffi::virtio_irq64_test_fail_unmap() } == 0;
    let proof = if masked && unmapped {
        1
    } else if unsafe { ffi::virtio_irq64_quiesce_set(ffi::VIRQ_DEV_INPUT) } == 1 {
        2
    } else if reset_proof(dev) {
        3
    } else {
        0
    };
    match proof {
        1 | 2 => {
            virtio_ring_mb_barrier();
            shared.set_owner(QueueOwner::Poll);        // new owner takes over...
            let _ = dev.drain(QueueOwner::Poll);        // ...and reconciles/drains immediately
            unsafe { ffi::virtio_irq64_note(ffi::VIRQ_DEV_INPUT, if proof == 1 { ffi::VIRQ_EV_DEMOTED_P1 } else { ffi::VIRQ_EV_DEMOTED_P2 }); }
        }
        3 => {
            // Device reset: every vector unmapped and the device stopped. Re-create the eventq POLL-owned.
            let info = TransportInfoRaw { ok: dev.info_copy.ok, common: dev.info_copy.common, notify: dev.info_copy.notify, isr: dev.info_copy.isr, device: dev.info_copy.device, pci_cfg: dev.info_copy.pci_cfg };
            unsafe {
                let ok = toxenos_virtio_input_init_begin(&info) == 0 && toxenos_virtio_input_init_queues(VIRTIO_MSI_NO_VECTOR as u32) == 0 && toxenos_virtio_input_init_finish() == 0;
                if ok { ffi::virtio_irq64_note(ffi::VIRQ_DEV_INPUT, ffi::VIRQ_EV_DEMOTED_P3); }
                else { ffi::virtio_irq64_note(ffi::VIRQ_DEV_INPUT, ffi::VIRQ_EV_FAULTED); return 0; }
            }
        }
        _ => {
            dev.faulted = true;
            unsafe { ffi::virtio_irq64_note(ffi::VIRQ_DEV_INPUT, ffi::VIRQ_EV_FAULTED); }
        }
    }
    proof
}

/// P3: the device reset completed (status read back 0) AND the selector reads NO_VECTOR.
/// (A VIRTIO_IRQ_TEST_FAIL_RESET build forces the verification to fail after performing the reset.)
fn reset_proof(dev: &InputDevice) -> bool {
    let reset_ok = dev.transport.reset_verified();
    let selector = dev.transport.common.read_queue_vector(0);
    let forced_fail = unsafe { ffi::virtio_irq64_test_fail_reset() } != 0;
    p3_holds(reset_ok, selector, forced_fail)
}

/// The P3 decision: the reset completed (verified) AND the selector reads NO_VECTOR (and no test
/// build declared the verification failed).
pub fn p3_holds(reset_ok: bool, selector: u16, forced_fail: bool) -> bool {
    reset_ok && selector == VIRTIO_MSI_NO_VECTOR && !forced_fail
}

fn virtio_ring_mb_barrier() { crate::virtio_pci::virtio_ring_mb(); }

/// Raw `queue_msix_vector` of the event queue (0xFFFF = NO_VECTOR), -1 if no device. Diagnostics/tests.
#[no_mangle]
pub extern "C" fn toxenos_virtio_input_read_selector() -> i32 {
    let p = dev_ptr();
    match unsafe { (*p).as_mut() } { Some(d) => d.transport.common.read_queue_vector(0) as i32, None => -1 }
}

/// C-callable demotion trigger (used by tests and the diagnostic paths).
#[no_mangle]
pub extern "C" fn toxenos_virtio_input_demote() -> i32 {
    let p = dev_ptr();
    match unsafe { (*p).as_mut() } { Some(d) => demote_device(d) as i32, None => -1 }
}

#[no_mangle]
pub extern "C" fn toxenos_virtio_input_is_faulted() -> i32 {
    let p = dev_ptr();
    match unsafe { (*p).as_ref() } { Some(d) => d.faulted as i32, None => 0 }
}

// ── queries ────────────────────────────────────────────────────────────
#[no_mangle]
pub unsafe extern "C" fn toxenos_virtio_input_get_abs_range(min_x: *mut i32, max_x: *mut i32, min_y: *mut i32, max_y: *mut i32) -> i32 {
    let p = dev_ptr();
    match (*p).as_ref() {
        Some(dev) if dev.have_abs => {
            core::ptr::write(min_x, dev.abs_min_x); core::ptr::write(max_x, dev.abs_max_x);
            core::ptr::write(min_y, dev.abs_min_y); core::ptr::write(max_y, dev.abs_max_y);
            0
        }
        _ => -1,
    }
}

#[no_mangle]
pub extern "C" fn toxenos_virtio_input_is_ready() -> i32 {
    if unsafe { (*dev_ptr()).is_some() } { 1 } else { 0 }
}

/// 0 = POLL, 1 = IRQ, 2 = none/fenced.
#[no_mangle]
pub extern "C" fn toxenos_virtio_input_owner() -> i32 { INPUT_EVQ.owner() as i32 }

/// Legacy 3-value report kept for the existing diagnostics: `[received, recycled, polls]`.
#[no_mangle]
pub extern "C" fn toxenos_virtio_input_debug_stats(out: *mut u64) -> i32 {
    if out.is_null() { return -1; }
    unsafe {
        *out.add(0) = STAT_RECEIVED;
        *out.add(1) = STAT_RECYCLED;
        *out.add(2) = STAT_TIMER_POLLS;
    }
    0
}

/// M+11C counters: fills out[0..14] =
/// [irq_count, completions, max_per_irq, empty_irqs, max_consec_empty, stale_irqs, wrong_owner,
///  poll_drains, poll_calls, samples, sample_overflow, received, recycled, timer_polls]
#[no_mangle]
pub extern "C" fn toxenos_virtio_input_irq_stats(out: *mut u64) -> i32 {
    if out.is_null() { return -1; }
    let q = &INPUT_EVQ;
    unsafe {
        *out.add(0) = q.irq_count.get();
        *out.add(1) = q.completions.get();
        *out.add(2) = q.max_per_irq.get() as u64;
        *out.add(3) = q.empty_irqs.get();
        *out.add(4) = q.max_consec_empty.get();
        *out.add(5) = q.stale_irqs.get();
        *out.add(6) = q.wrong_owner.get();
        *out.add(7) = q.poll_drains.get();
        *out.add(8) = q.poll_calls.get();
        *out.add(9) = STAT_SAMPLES;
        *out.add(10) = STAT_SAMPLE_OVERFLOW;
        *out.add(11) = STAT_RECEIVED;
        *out.add(12) = STAT_RECYCLED;
        *out.add(13) = STAT_TIMER_POLLS;
    }
    0
}

/// DEBUG watchdog probe (only ever called under VIRTIO_IRQ_DEBUG_WATCHDOG): counts a health
/// probe and returns the number of unconsumed used entries WITHOUT draining them.
#[no_mangle]
pub extern "C" fn toxenos_virtio_input_health_probe() -> i32 {
    let q = &INPUT_EVQ;
    q.health_probes.set(q.health_probes.get() + 1);
    q.pending() as i32
}

#[no_mangle]
pub extern "C" fn toxenos_virtio_input_selftest_ring_wrap() -> i32 {
    if selftest_ring_wrap() { 1 } else { 0 }
}

/// Test hook: feed the owner check with a specific caller (proves a POLL drain of an IRQ-owned
/// queue is refused and counted). Returns 1 if refused, 0 if it was (wrongly) allowed, -1 if no device.
#[no_mangle]
pub extern "C" fn toxenos_virtio_input_test_wrong_owner_poll() -> i32 {
    let p = dev_ptr();
    let dev = match unsafe { (*p).as_mut() } { Some(d) => d, None => return -1 };
    if dev.eventq.shared().owner() == QueueOwner::Poll { return -1; }
    match dev.drain(QueueOwner::Poll) { Err(DrainErr::WrongOwner) => 1, Ok(_) => 0 }
}


// ══════════════════════════════════════════════════════════════════════
// M+11C input self-tests (no device). Returns a bitmask of FAILED groups.
// ══════════════════════════════════════════════════════════════════════
pub fn selftest_input() -> u32 {
    let mut failed = 0u32;
    let mut check = |g: u32, name: &str, ok: bool| { crate::virtio_pci::selftest_count(); if !ok { failed |= 1 << g; crate::klog_fmt!("toxenos_rs: virtio_input selftest FAIL [{}]: {}", g, name); } };

    // 0: several raw events before SYN_REPORT -> ONE coherent sample, emitted once
    {
        let mut d = EventDecoder::new();
        check(0, "ABS_X alone emits nothing", d.feed(EV_ABS, ABS_X, 100).is_none());
        check(0, "ABS_Y alone emits nothing", d.feed(EV_ABS, ABS_Y, 200).is_none());
        check(0, "BTN press alone emits nothing", d.feed(EV_KEY, BTN_LEFT, 1).is_none());
        let s = d.feed(EV_SYN, SYN_REPORT, 0);
        check(0, "SYN_REPORT emits the coherent sample", s == Some(Sample { x: 100, y: 200, buttons: 1 }));
        check(0, "unknown event ignored", d.feed(0x7F, 1, 1).is_none());
        check(0, "axis state persists across frames", d.feed(EV_SYN, SYN_REPORT, 0) == Some(Sample { x: 100, y: 200, buttons: 1 }));
    }
    // 1: press+release in separate reports are TWO samples (no collapse to latest-only)
    {
        let mut d = EventDecoder::new(); let mut ring = SampleRing::new();
        d.feed(EV_ABS, ABS_X, 5); d.feed(EV_KEY, BTN_LEFT, 1);
        ring.push(d.feed(EV_SYN, SYN_REPORT, 0).unwrap_or_default());
        d.feed(EV_KEY, BTN_LEFT, 0);
        ring.push(d.feed(EV_SYN, SYN_REPORT, 0).unwrap_or_default());
        let a = ring.pop().unwrap_or_default(); let b = ring.pop().unwrap_or_default();
        check(1, "press then release both observable", a.buttons == 1 && b.buttons == 0 && ring.pop().is_none());
    }
    // 2: the sample ring really holds SAMPLE_CAP (16) entries: explicit count, not head==tail
    {
        let mut ring = SampleRing::new();
        let mut all = true;
        for i in 0..SAMPLE_CAP { all &= ring.push(Sample { x: i as i32, y: 0, buttons: 0 }); }
        check(2, "16 samples fit (capacity is 16, not 15)", all && ring.len() == SAMPLE_CAP);
        check(2, "17th refused and counted, never a silent overwrite", !ring.push(Sample::default()) && ring.overflow == 1);
        let mut fifo = true; for i in 0..SAMPLE_CAP { fifo &= ring.pop() == Some(Sample { x: i as i32, y: 0, buttons: 0 }); }
        check(2, "FIFO, then empty", fifo && ring.pop().is_none());
        for i in 0..5 { ring.push(Sample { x: i, y: 1, buttons: 0 }); } for _ in 0..3 { ring.pop(); }
        let mut wrap = true; for i in 0..(SAMPLE_CAP - 2) { wrap &= ring.push(Sample { x: 100 + i as i32, y: 2, buttons: 0 }); }
        check(2, "usable after head wrap", wrap && ring.len() == SAMPLE_CAP);
    }
    // 3: full drain_core over a synthetic ring: burst, recycle exactly once, all-16-SYN worst case, wraparound
    {
        // synthetic 16-slot event buffer: slot i holds one 8-byte event
        let evbuf = core::cell::RefCell::new(alloc::vec![0u8; NUM_SLOTS as usize * SLOT_BYTES]);
        let set_ev = |slot: usize, t: u16, c: u16, v: u32| {
            let o = slot * SLOT_BYTES;
            let mut b = evbuf.borrow_mut();
            b[o..o + 2].copy_from_slice(&t.to_ne_bytes()); b[o + 2..o + 4].copy_from_slice(&c.to_ne_bytes()); b[o + 4..o + 8].copy_from_slice(&v.to_ne_bytes());
        };
        // burst: ABS_X, ABS_Y, SYN, BTN_LEFT down, SYN  (5 raw events -> 2 samples)
        set_ev(0, EV_ABS, ABS_X, 11); set_ev(1, EV_ABS, ABS_Y, 22); set_ev(2, EV_SYN, SYN_REPORT, 0);
        set_ev(3, EV_KEY, BTN_LEFT, 1); set_ev(4, EV_SYN, SYN_REPORT, 0);
        let mut ring_mem = FakeRingShim::new(16);
        let sh: &'static QueueShared = alloc::boxed::Box::leak(alloc::boxed::Box::new(QueueShared::new()));
        sh.attach(ring_mem.base(), 16, QueueOwner::Irq);
        for id in 0..5u32 { ring_mem.complete(id); }
        let mut dec = EventDecoder::new(); let mut ring = SampleRing::new(); let mut ids = [0u16; NUM_SLOTS as usize];
        let read = |id: u16| { let o = id as usize * SLOT_BYTES; let b = evbuf.borrow(); (u16::from_ne_bytes([b[o], b[o+1]]), u16::from_ne_bytes([b[o+2], b[o+3]]), u32::from_ne_bytes([b[o+4], b[o+5], b[o+6], b[o+7]])) };
        let (n, cnt) = drain_core(sh, QueueOwner::Irq, &read, &mut dec, &mut ring, &mut ids).unwrap_or((0xFFFF, 0));
        check(3, "burst: 5 events consumed in one drain", n == 5 && cnt == 5 && sh.pending() == 0);
        check(3, "burst: buffers to recycle == consumed ids, in order, once", ids[..5] == [0u16, 1, 2, 3, 4]);
        check(3, "burst: two coherent samples", ring.len() == 2 && ring.pop() == Some(Sample { x: 11, y: 22, buttons: 0 }) && ring.pop() == Some(Sample { x: 11, y: 22, buttons: 1 }));
        // worst case: ALL 16 buffers are SYN_REPORT in one snapshot -> 16 samples, no overflow
        for slot in 0..16 { set_ev(slot, EV_SYN, SYN_REPORT, 0); }
        for id in 0..16u32 { ring_mem.complete(id); }
        let mut ids2 = [0u16; NUM_SLOTS as usize];
        let ov0 = unsafe { STAT_SAMPLE_OVERFLOW };
        let (n2, c2) = drain_core(sh, QueueOwner::Irq, &read, &mut dec, &mut ring, &mut ids2).unwrap_or((0xFFFF, 0));
        check(3, "16 SYN_REPORTs in one snapshot -> 16 samples, no overflow", n2 == 16 && c2 == 16 && ring.len() == 16 && ring.overflow == 0 && unsafe { STAT_SAMPLE_OVERFLOW } == ov0);
        // wrong owner: refused, nothing decoded
        let mut ids3 = [0u16; NUM_SLOTS as usize];
        ring_mem.complete(0);
        check(3, "POLL drain of an IRQ-owned queue refused", drain_core(sh, QueueOwner::Poll, &read, &mut dec, &mut ring, &mut ids3).is_err() && sh.wrong_owner.get() == 1);
        // 16-bit wraparound of the used index across a drain
        let mut rw = FakeRingShim::new(16);
        let shw: &'static QueueShared = alloc::boxed::Box::leak(alloc::boxed::Box::new(QueueShared::new()));
        shw.attach(rw.base(), 16, QueueOwner::Irq);
        rw.set_start(65533); shw.set_used_consumed(65533);
        for id in 0..6u32 { rw.complete(id); }
        let mut d4 = EventDecoder::new(); let mut r4 = SampleRing::new(); let mut i4 = [0u16; NUM_SLOTS as usize];
        let (nw, cw) = drain_core(shw, QueueOwner::Irq, &read, &mut d4, &mut r4, &mut i4).unwrap_or((0xFFFF, 0));
        check(3, "wraparound drain consumes all 6, once", nw == 6 && cw == 6 && shw.pending() == 0 && i4[..6] == [0u16, 1, 2, 3, 4, 5]);
    }
    // 4: the P3 (device reset) silence proof needs BOTH a verified reset and an unmapped selector
    {
        check(4, "P3: verified reset + NO_VECTOR selector", p3_holds(true, VIRTIO_MSI_NO_VECTOR, false));
        check(4, "P3: reset not verified is no proof", !p3_holds(false, VIRTIO_MSI_NO_VECTOR, false));
        check(4, "P3: a selector still mapped is no proof", !p3_holds(true, 0, false));
        check(4, "P3: a forced-failure test build is no proof", !p3_holds(true, VIRTIO_MSI_NO_VECTOR, true));
    }
    failed
}

// Minimal fake used ring for the input tests (a copy of the pci-side helper, kept local so this
// module's tests do not depend on that module's private test scaffolding).
struct FakeRingShim { buf: alloc::vec::Vec<u8>, idx: u16, size: u16 }
impl FakeRingShim {
    fn new(size: u16) -> Self { FakeRingShim { buf: alloc::vec![0u8; 6 + size as usize * 8], idx: 0, size } }
    fn base(&mut self) -> *mut u8 { self.buf.as_mut_ptr() }
    fn set_start(&mut self, v: u16) { self.idx = v; let i = self.idx; unsafe { core::ptr::write_volatile(self.base().add(2) as *mut u16, i); } }
    fn complete(&mut self, id: u32) {
        let off = 4 + (self.idx % self.size) as usize * 8;
        unsafe { core::ptr::write_volatile(self.base().add(off) as *mut u32, id); core::ptr::write_volatile(self.base().add(off + 4) as *mut u32, 8); }
        self.idx = self.idx.wrapping_add(1);
        let i = self.idx; unsafe { core::ptr::write_volatile(self.base().add(2) as *mut u16, i); }
    }
}

#[no_mangle]
pub extern "C" fn toxenos_virtio_irq_selftest_input() -> u32 { selftest_input() }
