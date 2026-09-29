//! M+3: the VirtIO-GPU 2D driver -- the first real ToxenOS GPU/display
//! driver, built entirely on M+2's reusable transport
//! ([`crate::virtio_pci`]). Owns the VirtIO-GPU wire protocol
//! EXCLUSIVELY: no other file, C or Rust, ever builds or interprets a
//! `virtio_gpu_ctrl_hdr` or any command/response payload. The generic
//! ToxenOS object model (`gpu64_device_t`/`gpu64_buffer_t`/`memobj64_t`)
//! stays in C (`kernel/virtio_gpu64.c`) exactly as M+1B/M+2 established --
//! this module is called from there via a small set of `extern "C"`
//! functions that take only plain values (resource IDs, physical
//! address/length pairs, rectangles), never a raw `gpu64_buffer_t*` or
//! `memobj64_t*`. Same reasoning M+2's own header comment gives for why
//! Rust never touches `pci64_device_t*` directly.
//!
//! Deliberately NOT here (see this milestone's own scope notes):
//! VirGL/Venus/3D, generic GPU command submission, fences (no
//! `VIRTIO_GPU_FLAG_FENCE` is ever set -- every command is a single
//! synchronous round trip, matching M+2's "only ONE request in flight"
//! design), MSI/MSI-X/interrupts, and hardware/EDID probing beyond
//! `GET_DISPLAY_INFO`.
//!
//! M+7: the cursor virtqueue (queue 1) is now used -- see
//! [`GpuDevice::new`]'s own note on why it's set up before `DRIVER_OK`
//! and treated as an optional capability, never a hard requirement.
//! Still polling/synchronous like every other queue here: cursor
//! commands are submitted as a single device-readable descriptor (no
//! response payload the driver needs to read, per spec) and polled for
//! completion the same way [`GpuDevice::send_command`] already does for
//! the control queue.
use crate::console;
use crate::dma::DmaBuffer;
use crate::ffi;
use crate::virtio_pci::{DrainErr, QueueOwner, QueueShared, Transport, TransportInfoRaw, VirtQueue, VirtioError, VIRTIO_MSI_NO_VECTOR};
use core::ffi::c_void;

// ── M+11C: interrupt-driven completion ────────────────────────────────
// Each queue's consumer state is a `&'static QueueShared` (see virtio_pci.rs):
// the completion interrupt handler drains the used ring into a small
// completion-metadata ring and wakes the waiter; the waiter (process context)
// retires its own entry, validates it, parses the response and recycles the
// descriptors. The handler never parses a GPU response. One authoritative
// consumer index per queue -- "observed" and "consumed" are the same thing.
static GPU_CTL_Q: QueueShared = QueueShared::new();
static GPU_CUR_Q: QueueShared = QueueShared::new();

/// Command timeout in timer ticks (100 Hz): 2 s. On expiry the ring is inspected once
/// (a controlled recovery inspection, not normal polling) to distinguish a LOST INTERRUPT
/// from a device that simply has not answered.
const CMD_TIMEOUT_TICKS: u64 = 200;
const STORM_WARN: u64 = 1_000;
const STORM_DEMOTE: u64 = 100_000;
const MAX_ABANDONED: usize = 4;

#[derive(Clone, Copy)]
struct Abandoned {
    head: u16,
    tail: u16, // 0xFFFF = none
}

// ── VirtIO-GPU wire protocol (spec-mandated; confirmed against Linux's
// own uapi/linux/virtio_gpu.h, the spec's reference implementation) ──
const CMD_GET_DISPLAY_INFO: u32 = 0x0100;
const CMD_RESOURCE_CREATE_2D: u32 = 0x0101;
const CMD_RESOURCE_UNREF: u32 = 0x0102;
const CMD_SET_SCANOUT: u32 = 0x0103;
const CMD_RESOURCE_FLUSH: u32 = 0x0104;
const CMD_TRANSFER_TO_HOST_2D: u32 = 0x0105;
const CMD_RESOURCE_ATTACH_BACKING: u32 = 0x0106;
const CMD_RESOURCE_DETACH_BACKING: u32 = 0x0107;
// M+7: cursor virtqueue command types (spec section 5.7.6.9 / Linux
// uapi/linux/virtio_gpu.h) -- sent ONLY via the cursor queue (index 1),
// never the control queue, even though they share the same struct shape
// as every control-queue command's own ctrl_hdr-prefixed layout.
const CMD_UPDATE_CURSOR: u32 = 0x0300;
const CMD_MOVE_CURSOR: u32 = 0x0301;

const RESP_OK_NODATA: u32 = 0x1100;
const RESP_OK_DISPLAY_INFO: u32 = 0x1101;
const RESP_ERR_FIRST: u32 = 0x1200; // every type >= this is a VIRTIO_GPU_RESP_ERR_*

/// In-memory byte layout matches ToxenOS's existing logical `0x00RRGGBB`
/// framebuffer format EXACTLY -- the same identity this codebase's own
/// `display64_blit_row()` fast path already keys on -- so test pixels
/// need no conversion. Chosen per this milestone's own instruction to
/// start with the format ToxenOS already uses, not build a format
/// matrix.
pub const FORMAT_B8G8R8X8_UNORM: u32 = 2;

/// Bit 32 of the generic (non-GPU-specific) VirtIO feature space --
/// required for ANY modern-transport device to leave FEATURES_OK set,
/// unrelated to virtio-gpu itself.
const VIRTIO_F_VERSION_1: u64 = 1u64 << 32;

/// IMPORTANT correction from M+2: the transport smoke test accepted
/// every feature the device offered (`required=0, optional=u64::MAX`).
/// A real driver must not do that. This driver implements NONE of
/// VIRTIO_GPU_F_VIRGL/EDID/RESOURCE_UUID/RESOURCE_BLOB/CONTEXT_INIT's
/// semantics, so none of those bits are ever named here -- an unnamed
/// bit is never accepted by [`crate::virtio_pci::Transport::negotiate`]
/// regardless of whether the device offers it. `VIRTIO_F_VERSION_1` is
/// the one bit actually required, by the transport itself, not by
/// anything virtio-gpu-specific. `SUPPORTED` is everything this driver
/// knows the meaning of; `REQUIRED` is the subset it cannot function
/// without; `OPTIONAL` (derived, never hand-duplicated) is the rest --
/// today those happen to be the same single bit, but the three stay
/// named separately so a future feature this driver supports-but-
/// doesn't-strictly-need slots into `SUPPORTED` without touching
/// `REQUIRED`.
const REQUIRED_FEATURES: u64 = VIRTIO_F_VERSION_1;
const SUPPORTED_FEATURES: u64 = VIRTIO_F_VERSION_1;
const OPTIONAL_FEATURES: u64 = SUPPORTED_FEATURES & !REQUIRED_FEATURES;

const MAX_BACKING_ENTRIES: usize = 8; // generous for a single-run (today's only memobj64 kind) or small scatter-gather backing; a real cap, not unbounded

/// Largest response this driver ever expects
/// (`virtio_gpu_resp_display_info`: 24-byte hdr + 16 * 24-byte
/// `virtio_gpu_display_one` = 408 bytes) -- the control queue's
/// response buffer is sized to this once, for every command.
const RESP_BUF_LEN: usize = 24 + 16 * 24;
/// Largest request this driver ever builds (`RESOURCE_ATTACH_BACKING`'s
/// header + MAX_BACKING_ENTRIES mem entries).
const REQ_BUF_LEN: usize = 32 + MAX_BACKING_ENTRIES * 16;

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum GpuError {
    Transport(VirtioError),
    Timeout,
    /// The used-ring completion didn't name the descriptor chain this
    /// call submitted -- should never happen with one request in flight
    /// at a time; checked anyway per this milestone's own "do not
    /// assume response success" instruction.
    StaleCompletion,
    ShortResponse,
    UnexpectedResponseType(u32),
    ErrorResponse(u32),
    InvalidArgument,
    TooManyEntries,
    /// M+11C: the calling context may not wait for a completion interrupt.
    WouldBlock,
    /// M+11C: the device is FAULTED (interrupt state uncertain) -- operations fail cleanly.
    Faulted,
    /// M+11C: the queue is being demoted / has no completion owner right now.
    Quiescing,
    /// M+11C: protocol violation on the queue (more completions than commands, ring corrupt).
    QueueBroken,
}

impl From<VirtioError> for GpuError {
    fn from(e: VirtioError) -> Self {
        GpuError::Transport(e)
    }
}

/// One discovered scanout (`struct virtio_gpu_display_one`).
#[derive(Debug, Clone, Copy, Default)]
pub struct DisplayMode {
    pub x: u32,
    pub y: u32,
    pub width: u32,
    pub height: u32,
    pub enabled: bool,
}

// ── Raw byte read/write helpers ─────────────────────────────────────
// This driver builds/parses fixed, spec-defined little-endian layouts
// directly (matching kernel/virtio_pci64.c's own "read fields
// individually, no struct overlay" discipline, and virtio_blk64.c's own
// convention of never needing a serialization crate for this) -- x86_64
// is little-endian, so a native write IS the wire format.
unsafe fn w32(base: *mut u8, off: usize, v: u32) {
    core::ptr::write_unaligned(base.add(off) as *mut u32, v);
}
unsafe fn w64(base: *mut u8, off: usize, v: u64) {
    core::ptr::write_unaligned(base.add(off) as *mut u64, v);
}
unsafe fn r32(base: *const u8, off: usize) -> u32 {
    core::ptr::read_unaligned(base.add(off) as *const u32)
}

fn write_ctrl_hdr(base: *mut u8, cmd_type: u32) {
    unsafe {
        w32(base, 0, cmd_type); // type
        w32(base, 4, 0); // flags -- never VIRTIO_GPU_FLAG_FENCE (see this module's own header comment)
        w64(base, 8, 0); // fence_id
        w32(base, 16, 0); // ctx_id -- no contexts (M+1B deferred gpu64_context_t; this driver has no use for one either)
        // bytes 20..24: ring_idx + padding[3], all zero
        core::ptr::write_bytes(base.add(20), 0, 4);
    }
}

fn validate_rect(x: u32, y: u32, w: u32, h: u32, res_w: u32, res_h: u32) -> Result<(), GpuError> {
    if w == 0 || h == 0 {
        return Err(GpuError::InvalidArgument);
    }
    let x_end = (x as u64).checked_add(w as u64).ok_or(GpuError::InvalidArgument)?;
    let y_end = (y as u64).checked_add(h as u64).ok_or(GpuError::InvalidArgument)?;
    if x_end > res_w as u64 || y_end > res_h as u64 {
        return Err(GpuError::InvalidArgument);
    }
    Ok(())
}

/// The VirtIO-GPU device: M+2's transport plus the control queue
/// (virtqueue 0) and a small, reusable, single-outstanding-request
/// command mechanism. M+7 adds the optional cursor queue (virtqueue 1)
/// alongside it -- see [`GpuBuild`] for why it is `Option`-wrapped.
pub struct GpuDevice {
    transport: Transport,
    cq: VirtQueue,
    req_buf: DmaBuffer,
    resp_buf: DmaBuffer,
    next_resource_id: u32,
    cursor_q: Option<VirtQueue>,
    cursor_req_buf: Option<DmaBuffer>,
    ctl_abandoned: [Option<Abandoned>; MAX_ABANDONED],
    cur_abandoned: [Option<Abandoned>; MAX_ABANDONED],
    faulted: bool,
}

/// Partially constructed device between the phased-init calls.
struct GpuBuild {
    transport: Transport,
    cq: Option<VirtQueue>,
    req_buf: Option<DmaBuffer>,
    resp_buf: Option<DmaBuffer>,
    cursor_q: Option<VirtQueue>,
    cursor_req_buf: Option<DmaBuffer>,
}

impl GpuDevice {
    /// Poll-mode convenience constructor (the pre-M+11C behaviour): every queue POLL-owned.
    ///
    /// # Safety
    /// `info` must be a genuine `virtio_pci64_probe()` result for a real modern VirtIO-GPU device.
    pub unsafe fn new(info: &TransportInfoRaw) -> Result<Self, GpuError> {
        let mut b = GpuBuild::begin(info)?;
        b.queues(VIRTIO_MSI_NO_VECTOR, VIRTIO_MSI_NO_VECTOR)?;
        b.finish()
    }

    pub fn status(&self) -> u8 {
        self.transport.common.status()
    }


    /// Reclaims the descriptors of a previously abandoned (timed-out) command whose
    /// completion has now arrived, or counts a stale/unknown completion.
    fn reclaim_stale(&mut self, ctl: bool, id: u16) {
        let (q, list, shared): (&mut VirtQueue, &mut [Option<Abandoned>; MAX_ABANDONED], &'static QueueShared) =
            if ctl { (&mut self.cq, &mut self.ctl_abandoned, &GPU_CTL_Q) } else { (self.cursor_q.as_mut().unwrap(), &mut self.cur_abandoned, &GPU_CUR_Q) };
        reclaim_chain(q, list, shared, id);
    }

    /// Marks a timed-out command's descriptors as ABANDONED: never recycled while the device might
    /// still write into them; reclaimed only if its completion later arrives.
    fn abandon(&mut self, ctl: bool, head: u16, tail: u16) {
        let (list, shared) = if ctl { (&mut self.ctl_abandoned, &GPU_CTL_Q) } else { (&mut self.cur_abandoned, &GPU_CUR_Q) };
        abandon_chain(list, shared, head, tail);
    }

    /// Waits for the completion of the command whose chain head is `head` on queue `ctl`
    /// (control or cursor). POLL-owned: bounded drain-as-owner loop (bring-up / fallback only).
    /// IRQ-owned: `kwait64_wait` on the completion ring, no busy-wait. Never frees an in-flight
    /// descriptor. Returns the device-written length, or the error (descriptors stay abandoned on
    /// Timeout).
    fn wait_on(&mut self, ctl: bool, head: u16, tail: u16) -> Result<u32, GpuError> {
        let shared: &'static QueueShared = if ctl { &GPU_CTL_Q } else { &GPU_CUR_Q };
        let mut poll_budget: u64 = 20_000_000;
        let mut recovered = false;
        loop {
            if self.faulted { return Err(GpuError::Faulted); }
            if shared.broken() { return Err(GpuError::QueueBroken); }
            while let Some((id, len)) = shared.comp_pop_front() {
                if id == head { return Ok(len); }
                self.reclaim_stale(ctl, id);
            }
            match shared.owner() {
                QueueOwner::Poll => {
                    {
                        let q: &VirtQueue = if ctl { &self.cq } else { self.cursor_q.as_ref().unwrap() };
                        let mut sink = |id: u16, len: u32| { let _ = shared.comp_push(id, len); };
                        let _ = q.poll_drain(&mut sink);
                    }
                    if shared.comp_count() == 0 {
                        poll_budget -= 1;
                        if poll_budget == 0 {
                            shared.timeouts.set(shared.timeouts.get() + 1);
                            self.abandon(ctl, head, tail);
                            return Err(GpuError::Timeout);
                        }
                    }
                }
                QueueOwner::Irq => {
                    let rc = unsafe {
                        ffi::kwait64_wait(shared as *const QueueShared as *mut c_void, cond_ring_nonempty,
                                          shared as *const QueueShared as *mut c_void, CMD_TIMEOUT_TICKS)
                    };
                    if rc == 0 {
                        shared.waits_woken.set(shared.waits_woken.get() + 1);
                        let last = shared.last_irq_tsc.get();
                        if last != 0 {
                            let now = unsafe { core::arch::x86_64::_rdtsc() };
                            let lat = now.wrapping_sub(last);
                            shared.wake_lat_total.set(shared.wake_lat_total.get() + lat);
                            shared.wake_lat_count.set(shared.wake_lat_count.get() + 1);
                            if lat > shared.wake_lat_max.get() { shared.wake_lat_max.set(lat); }
                            shared.last_irq_tsc.set(0);
                        }
                    } else if rc == -11 {
                        // Unreachable after precheck_submit() (the context cannot change mid-command), but if it
                        // ever happens the device already owns the buffers: abandon them, never reuse early.
                        self.abandon(ctl, head, tail);
                        return Err(GpuError::WouldBlock);
                    } else {
                        shared.timeouts.set(shared.timeouts.get() + 1);
                        if shared.pending() > 0 && !recovered {
                            // The ring shows a completion the interrupt never announced: a LOST interrupt.
                            recovered = true;
                            unsafe { ffi::virtio_irq64_note(ffi::VIRQ_DEV_GPU, ffi::VIRQ_EV_LOST_IRQ); }
                            self.demote_to_poll();
                            continue;                      // ownership is POLL now (or the device is FAULTED)
                        }
                        self.abandon(ctl, head, tail);
                        return Err(GpuError::Timeout);
                    }
                }
                QueueOwner::None => return Err(GpuError::Quiescing),
            }
        }
    }

    /// Device-wide verified-silence demotion (both queues change ownership together; there is
    /// never a mixed controlq=POLL / cursorq=IRQ state). Proofs, in order:
    ///  P1  EVERY IRQ-owned queue entry read back masked AND EVERY queue_msix_vector reads NO_VECTOR;
    ///  P2  the whole PCI IRQ set quiesced (function mask read back).
    /// (P3, a device reset, is not offered for the GPU: it would destroy every resource/scanout.)
    /// No proof -> FAULTED: no polling, operations fail, the PCI IRQ set stays pinned.
    fn demote_to_poll(&mut self) -> u32 {
        GPU_CTL_Q.set_owner(QueueOwner::None);              // old owners stopped
        GPU_CUR_Q.set_owner(QueueOwner::None);
        let masked = unsafe { ffi::virtio_irq64_mask_all(ffi::VIRQ_DEV_GPU) } == 1;
        let mut unmapped = self.transport.common.unmap_queue_vector(0);
        if self.cursor_q.is_some() { unmapped = self.transport.common.unmap_queue_vector(1) && unmapped; }
        if unsafe { ffi::virtio_irq64_test_fail_unmap() } != 0 { unmapped = false; }     // test build: unmap not verifiable
        let proof = if masked && unmapped { 1 }
                    else if unsafe { ffi::virtio_irq64_quiesce_set(ffi::VIRQ_DEV_GPU) } == 1 { 2 }
                    else { 0 };
        if proof == 0 {
            self.faulted = true;
            unsafe { ffi::virtio_irq64_note(ffi::VIRQ_DEV_GPU, ffi::VIRQ_EV_FAULTED); }
            return 0;
        }
        crate::virtio_pci::virtio_ring_mb();
        GPU_CTL_Q.set_owner(QueueOwner::Poll);
        if self.cursor_q.is_some() { GPU_CUR_Q.set_owner(QueueOwner::Poll); }
        // Reconcile: the new owner drains whatever completed during the transition.
        {
            let mut s1 = |id: u16, len: u32| { let _ = GPU_CTL_Q.comp_push(id, len); };
            let _ = self.cq.poll_drain(&mut s1);
            if let Some(cq) = self.cursor_q.as_ref() {
                let mut s2 = |id: u16, len: u32| { let _ = GPU_CUR_Q.comp_push(id, len); };
                let _ = cq.poll_drain(&mut s2);
            }
        }
        unsafe { ffi::virtio_irq64_note(ffi::VIRQ_DEV_GPU, if proof == 1 { ffi::VIRQ_EV_DEMOTED_P1 } else { ffi::VIRQ_EV_DEMOTED_P2 }); }
        proof
    }

    /// The reusable request/response round trip every command below
    /// builds on: submits a 2-descriptor chain (request, already written
    /// into `self.req_buf` by the caller -- device-readable; response --
    /// device-writable), notifies, waits for the completion (interrupt-driven
    /// when the queue is IRQ-owned), verifies the completion names the chain just
    /// submitted, and returns the response's own `type` field. Descriptors are freed
    /// on success and on protocol failures; on TIMEOUT they are ABANDONED (the
    /// device may still write the response buffer), never recycled early.
    fn send_command(&mut self, req_len: usize) -> Result<u32, GpuError> {
        debug_assert!(req_len <= REQ_BUF_LEN, "request larger than this driver's own documented maximum");
        if self.faulted { return Err(GpuError::Faulted); }
        if GPU_CTL_Q.demote_requested() && GPU_CTL_Q.owner() == QueueOwner::Irq {
            self.demote_to_poll();                        // a storm was flagged by the interrupt handler
            if self.faulted { return Err(GpuError::Faulted); }
        }
        precheck_submit(&GPU_CTL_Q)?;                     // refuse BEFORE the device owns any buffer
        let cmd_start = unsafe { core::arch::x86_64::_rdtsc() };

        let head = self.cq.alloc_desc().ok_or(GpuError::InvalidArgument)?;
        let tail = match self.cq.alloc_desc() {
            Some(t) => t,
            None => {
                self.cq.free_desc(head);
                return Err(GpuError::InvalidArgument);
            }
        };

        self.cq.set_desc(head, self.req_buf.phys_addr(), req_len as u32, false, Some(tail));
        self.cq.set_desc(tail, self.resp_buf.phys_addr(), RESP_BUF_LEN as u32, true, None);

        GPU_CTL_Q.begin_inflight();
        self.cq.submit_chain(head);
        self.cq.notify();

        let r = self.wait_on(true, head, tail);
        let poll_iters = 0u64;
        record_command_stats(cmd_start, poll_iters);
        GPU_CTL_Q.end_inflight();
        let written_len = match r {
            Ok(l) => l,
            Err(e) => {
                // Timeout: the chain was ABANDONED (kept until its completion arrives). Every other failure
                // (broken / faulted / quiescing queue) leaves the queue unusable, so its descriptors are
                // never handed out again either -- no path frees an in-flight chain.
                return Err(e);
            }
        };
        self.cq.free_desc(head);
        self.cq.free_desc(tail);

        if (written_len as usize) < 24 {
            return Err(GpuError::ShortResponse);
        }
        let resp_type = unsafe { r32(self.resp_buf.virt_ptr(), 0) };
        if resp_type >= RESP_ERR_FIRST {
            return Err(GpuError::ErrorResponse(resp_type));
        }
        Ok(resp_type)
    }

    /// True once the cursor queue was successfully set up in `new()`.
    /// The only thing C code (and, through it, compositor64) needs to
    /// know before attempting any cursor operation -- see this struct's
    /// own field comment.
    pub fn cursor_available(&self) -> bool {
        self.cursor_q.is_some()
    }

    /// Writes a `virtio_gpu_update_cursor` struct (56 bytes: 24-byte
    /// `ctrl_hdr` + 16-byte `cursor_pos` + resource_id + hot_x + hot_y +
    /// 4 bytes padding) -- the SAME struct shape both
    /// `VIRTIO_GPU_CMD_UPDATE_CURSOR` and `VIRTIO_GPU_CMD_MOVE_CURSOR`
    /// use per spec; only the command type and which fields the device
    /// actually reads differ (MOVE_CURSOR only changes position -- the
    /// hotspot was already established by an earlier UPDATE_CURSOR --
    /// but this driver fills the full struct identically either way,
    /// matching Linux's own reference driver behavior rather than
    /// guessing which fields are safe to leave stale).
    fn write_cursor_cmd(base: *mut u8, cmd_type: u32, scanout_id: u32, resource_id: u32, x: u32, y: u32, hot_x: u32, hot_y: u32) {
        write_ctrl_hdr(base, cmd_type);
        unsafe {
            w32(base, 24, scanout_id);
            w32(base, 28, x);
            w32(base, 32, y);
            w32(base, 36, 0); // cursor_pos struct's own trailing padding
            w32(base, 40, resource_id);
            w32(base, 44, hot_x);
            w32(base, 48, hot_y);
            w32(base, 52, 0); // trailing padding
        }
    }

    /// Submits one cursor-queue command: a single device-readable descriptor (no response
    /// buffer), notified and completed exactly like the control queue's round trip -- SYNCHRONOUS,
    /// one outstanding command; the request buffer is reused only after the completion was
    /// verified. On timeout the descriptor is abandoned, never reused early.
    fn cursor_send(&mut self, req_len: usize) -> Result<(), GpuError> {
        if self.faulted { return Err(GpuError::Faulted); }
        if self.cursor_q.is_none() || self.cursor_req_buf.is_none() { return Err(GpuError::InvalidArgument); }
        precheck_submit(&GPU_CUR_Q)?;                     // refuse BEFORE the device owns any buffer
        let head = { let cq = self.cursor_q.as_mut().unwrap(); cq.alloc_desc().ok_or(GpuError::InvalidArgument)? };
        {
            let buf_phys = self.cursor_req_buf.as_ref().unwrap().phys_addr();
            let cq = self.cursor_q.as_mut().unwrap();
            cq.set_desc(head, buf_phys, req_len as u32, false, None);
            GPU_CUR_Q.begin_inflight();
            cq.submit_chain(head);
            cq.notify();
        }
        let r = self.wait_on(false, head, 0xFFFF);
        GPU_CUR_Q.end_inflight();
        match r {
            Ok(_) => { self.cursor_q.as_mut().unwrap().free_desc(head); Ok(()) }
            Err(e) => Err(e),
        }
    }

    /// `VIRTIO_GPU_CMD_UPDATE_CURSOR` -- establishes which resource is
    /// the cursor image, its hotspot, and its initial position, in one
    /// command. Call once when the cursor image is first created, and
    /// again only if the image or hotspot ever changes (never per
    /// ordinary pointer movement -- use `cursor_move` for that). Passing
    /// `resource_id = 0` hides the cursor, per spec.
    pub fn cursor_update(&mut self, scanout_id: u32, resource_id: u32, x: u32, y: u32, hot_x: u32, hot_y: u32) -> Result<(), GpuError> {
        if self.cursor_q.is_none() {
            return Err(GpuError::InvalidArgument);
        }
        let base = self.cursor_req_buf.as_ref().unwrap().virt_ptr();
        Self::write_cursor_cmd(base, CMD_UPDATE_CURSOR, scanout_id, resource_id, x, y, hot_x, hot_y);
        self.cursor_send(56)
    }

    /// `VIRTIO_GPU_CMD_MOVE_CURSOR` -- the cheap, common-case operation:
    /// changes only the on-screen position of the already-established
    /// cursor image. `resource_id` is echoed for spec-completeness
    /// (matching `cursor_update`'s own struct shape) but the device is
    /// not expected to need it once the image is already set.
    pub fn cursor_move(&mut self, scanout_id: u32, resource_id: u32, x: u32, y: u32) -> Result<(), GpuError> {
        if self.cursor_q.is_none() {
            return Err(GpuError::InvalidArgument);
        }
        let base = self.cursor_req_buf.as_ref().unwrap().virt_ptr();
        Self::write_cursor_cmd(base, CMD_MOVE_CURSOR, scanout_id, resource_id, x, y, 0, 0);
        self.cursor_send(56)
    }

    /// `VIRTIO_GPU_CMD_GET_DISPLAY_INFO` -- the first command this
    /// driver ever sends, and the first end-to-end proof that
    /// ToxenOS -> gpu64 -> this driver -> the transport -> QEMU's device
    /// -> a real response actually works. Does not change anything --
    /// read-only, exactly like the spec describes it.
    pub fn get_display_info(&mut self) -> Result<[DisplayMode; 16], GpuError> {
        write_ctrl_hdr(self.req_buf.virt_ptr(), CMD_GET_DISPLAY_INFO);
        let resp_type = self.send_command(24)?;
        if resp_type != RESP_OK_DISPLAY_INFO {
            return Err(GpuError::UnexpectedResponseType(resp_type));
        }
        let mut modes = [DisplayMode::default(); 16];
        for (i, m) in modes.iter_mut().enumerate() {
            let off = 24 + i * 24;
            unsafe {
                let base = self.resp_buf.virt_ptr();
                m.x = r32(base, off);
                m.y = r32(base, off + 4);
                m.width = r32(base, off + 8);
                m.height = r32(base, off + 12);
                m.enabled = r32(base, off + 16) != 0;
            }
        }
        Ok(modes)
    }

    /// `VIRTIO_GPU_CMD_RESOURCE_CREATE_2D`. Allocates the next resource
    /// ID from a strictly monotonic counter (never reused, ever -- the
    /// simplest possible correct answer to "avoid accidental reuse
    /// while a resource is still live," and 0 is never handed out since
    /// the counter starts at 1 and the spec treats 0 as invalid).
    /// Validates `width`/`height` are nonzero and their product doesn't
    /// overflow before ever building the command.
    pub fn resource_create_2d(&mut self, width: u32, height: u32, format: u32) -> Result<u32, GpuError> {
        if width == 0 || height == 0 {
            return Err(GpuError::InvalidArgument);
        }
        (width as u64).checked_mul(height as u64).ok_or(GpuError::InvalidArgument)?;

        let resource_id = self.next_resource_id;
        self.next_resource_id = self.next_resource_id.checked_add(1).ok_or(GpuError::InvalidArgument)?;

        unsafe {
            let base = self.req_buf.virt_ptr();
            write_ctrl_hdr(base, CMD_RESOURCE_CREATE_2D);
            w32(base, 24, resource_id);
            w32(base, 28, format);
            w32(base, 32, width);
            w32(base, 36, height);
        }
        let resp_type = self.send_command(40)?;
        if resp_type != RESP_OK_NODATA {
            return Err(GpuError::UnexpectedResponseType(resp_type));
        }
        Ok(resource_id)
    }

    /// `VIRTIO_GPU_CMD_RESOURCE_ATTACH_BACKING`. `runs` is exactly what
    /// `memobj64_get_runs()` produces on the C side (physical address,
    /// length) -- this function consumes the run abstraction (an
    /// arbitrary-length slice) rather than assuming a single contiguous
    /// range, even though today's `memobj64_t` only ever produces one
    /// run. Validates entry count, rejects a zero-length entry, and
    /// checks the SUM of every entry's length doesn't overflow --
    /// exactly the "first hardware-side proof of the memobj64
    /// abstraction" this milestone calls for.
    pub fn resource_attach_backing(&mut self, resource_id: u32, runs: &[(u64, u32)]) -> Result<(), GpuError> {
        if runs.is_empty() || runs.len() > MAX_BACKING_ENTRIES {
            return Err(GpuError::TooManyEntries);
        }
        let mut total: u64 = 0;
        for &(_addr, len) in runs {
            if len == 0 {
                return Err(GpuError::InvalidArgument);
            }
            total = total.checked_add(len as u64).ok_or(GpuError::InvalidArgument)?;
        }
        let _ = total; // covered-bytes check above is the point; the value itself isn't sent over the wire

        unsafe {
            let base = self.req_buf.virt_ptr();
            write_ctrl_hdr(base, CMD_RESOURCE_ATTACH_BACKING);
            w32(base, 24, resource_id);
            w32(base, 28, runs.len() as u32);
            for (i, &(addr, len)) in runs.iter().enumerate() {
                let off = 32 + i * 16;
                w64(base, off, addr);
                w32(base, off + 8, len);
                w32(base, off + 12, 0); // padding
            }
        }
        let req_len = 32 + runs.len() * 16;
        let resp_type = self.send_command(req_len)?;
        if resp_type != RESP_OK_NODATA {
            return Err(GpuError::UnexpectedResponseType(resp_type));
        }
        Ok(())
    }

    /// `VIRTIO_GPU_CMD_RESOURCE_DETACH_BACKING` -- spec-correct teardown
    /// ordering: a resource's backing must be detached before it is
    /// unreffed (see [`GpuDevice::resource_unref`]'s own comment).
    pub fn resource_detach_backing(&mut self, resource_id: u32) -> Result<(), GpuError> {
        unsafe {
            let base = self.req_buf.virt_ptr();
            write_ctrl_hdr(base, CMD_RESOURCE_DETACH_BACKING);
            w32(base, 24, resource_id);
            w32(base, 28, 0);
        }
        let resp_type = self.send_command(32)?;
        if resp_type != RESP_OK_NODATA {
            return Err(GpuError::UnexpectedResponseType(resp_type));
        }
        Ok(())
    }

    /// `VIRTIO_GPU_CMD_TRANSFER_TO_HOST_2D`. `res_w`/`res_h` are the
    /// resource's own known dimensions (from whoever created it), used
    /// ONLY to validate `(x, y, w, h)` fits inside the resource before
    /// the command is ever built -- never sent over the wire (the
    /// protocol has no separate stride field for basic 2D resources; a
    /// tightly-packed `width * 4` row stride, which is exactly what
    /// ToxenOS's own backbuffer already uses, is the protocol's own
    /// implicit assumption). `offset` is the byte offset into the
    /// resource's LINEAR backing where this rect begins --
    /// `y * res_w * 4 + x * 4` for a sub-rect, or 0 for a full-buffer
    /// transfer.
    pub fn transfer_to_host_2d(&mut self, resource_id: u32, x: u32, y: u32, w: u32, h: u32, res_w: u32, res_h: u32) -> Result<(), GpuError> {
        validate_rect(x, y, w, h, res_w, res_h)?;
        let offset = (y as u64)
            .checked_mul(res_w as u64)
            .and_then(|v| v.checked_add(x as u64))
            .and_then(|v| v.checked_mul(4))
            .ok_or(GpuError::InvalidArgument)?;

        unsafe {
            let base = self.req_buf.virt_ptr();
            write_ctrl_hdr(base, CMD_TRANSFER_TO_HOST_2D);
            w32(base, 24, x);
            w32(base, 28, y);
            w32(base, 32, w);
            w32(base, 36, h);
            w64(base, 40, offset);
            w32(base, 48, resource_id);
            w32(base, 52, 0);
        }
        let resp_type = self.send_command(56)?;
        if resp_type != RESP_OK_NODATA {
            return Err(GpuError::UnexpectedResponseType(resp_type));
        }
        Ok(())
    }

    /// `VIRTIO_GPU_CMD_RESOURCE_FLUSH`. Per Revision 2's own corrected
    /// terminology: this is DEVICE COMMAND COMPLETION -- the device
    /// finished processing the flush -- NOT VSync and NOT real vblank.
    /// No "vsync" abstraction is introduced here or anywhere in this
    /// module; this function's own name says exactly what it does.
    pub fn resource_flush(&mut self, resource_id: u32, x: u32, y: u32, w: u32, h: u32, res_w: u32, res_h: u32) -> Result<(), GpuError> {
        validate_rect(x, y, w, h, res_w, res_h)?;
        unsafe {
            let base = self.req_buf.virt_ptr();
            write_ctrl_hdr(base, CMD_RESOURCE_FLUSH);
            w32(base, 24, x);
            w32(base, 28, y);
            w32(base, 32, w);
            w32(base, 36, h);
            w32(base, 40, resource_id);
            w32(base, 44, 0);
        }
        let resp_type = self.send_command(48)?;
        if resp_type != RESP_OK_NODATA {
            return Err(GpuError::UnexpectedResponseType(resp_type));
        }
        Ok(())
    }

    /// `VIRTIO_GPU_CMD_SET_SCANOUT`. `resource_id == 0` disables the
    /// scanout per spec (not used by this milestone's own test, which
    /// always names a real resource).
    pub fn set_scanout(&mut self, scanout_id: u32, resource_id: u32, x: u32, y: u32, w: u32, h: u32) -> Result<(), GpuError> {
        if w == 0 || h == 0 {
            return Err(GpuError::InvalidArgument);
        }
        unsafe {
            let base = self.req_buf.virt_ptr();
            write_ctrl_hdr(base, CMD_SET_SCANOUT);
            w32(base, 24, x);
            w32(base, 28, y);
            w32(base, 32, w);
            w32(base, 36, h);
            w32(base, 40, scanout_id);
            w32(base, 44, resource_id);
        }
        let resp_type = self.send_command(48)?;
        if resp_type != RESP_OK_NODATA {
            return Err(GpuError::UnexpectedResponseType(resp_type));
        }
        Ok(())
    }

    /// `VIRTIO_GPU_CMD_RESOURCE_UNREF`. Caller's own responsibility to
    /// have already detached any backing and cleared any scanout
    /// pointing at this resource first (spec ordering) -- see
    /// `kernel/virtio_gpu64.c`'s destroy path, which always calls
    /// [`GpuDevice::resource_detach_backing`] immediately before this.
    pub fn resource_unref(&mut self, resource_id: u32) -> Result<(), GpuError> {
        unsafe {
            let base = self.req_buf.virt_ptr();
            write_ctrl_hdr(base, CMD_RESOURCE_UNREF);
            w32(base, 24, resource_id);
            w32(base, 28, 0);
        }
        let resp_type = self.send_command(32)?;
        if resp_type != RESP_OK_NODATA {
            return Err(GpuError::UnexpectedResponseType(resp_type));
        }
        Ok(())
    }
}

// ── M+4 investigation: RDTSC-based command-latency instrumentation ──
// Diagnostic-only counters, always recorded (a few integer ops per
// command is negligible next to an actual VirtIO round trip, so there's
// no need to gate this behind a build-time flag the way kernel64.c's
// debug flags gate expensive/disruptive diagnostics) -- lets a
// PRODUCTION boot's own real interactive workload be measured directly,
// which is exactly what this milestone's own manually-observed-lag
// investigation needs. `_rdtsc()` is a plain baseline x86 instruction
// available on every x86_64 target unconditionally (no target-feature
// gate) -- converting the accumulated cycle count to real time happens
// on the C side (kernel/tsc64.c's PIT-calibrated cycles-per-ms), not
// duplicated here. Single-threaded discipline, same as GPU_DEVICE's own
// comment above (this driver has no real interrupt handlers).
static mut STAT_COMMANDS: u64 = 0;
static mut STAT_POLL_ITERS_TOTAL: u64 = 0;
static mut STAT_POLL_ITERS_MAX: u64 = 0;
static mut STAT_CYCLES_TOTAL: u64 = 0;

fn record_command_stats(cmd_start_cycles: u64, poll_iters: u64) {
    let elapsed = unsafe { core::arch::x86_64::_rdtsc() }.wrapping_sub(cmd_start_cycles);
    unsafe {
        STAT_COMMANDS += 1;
        STAT_POLL_ITERS_TOTAL += poll_iters;
        if poll_iters > STAT_POLL_ITERS_MAX { STAT_POLL_ITERS_MAX = poll_iters; }
        STAT_CYCLES_TOTAL += elapsed;
    }
}

impl GpuBuild {
    /// Phase 1: reset, ACKNOWLEDGE, DRIVER, features, FEATURES_OK (spec 3.1.1 steps 1-6).
    unsafe fn begin(info: &TransportInfoRaw) -> Result<Self, GpuError> {
        let transport = Transport::from_raw(info).ok_or(GpuError::InvalidArgument)?;
        let accepted = transport.negotiate(REQUIRED_FEATURES, OPTIONAL_FEATURES)?;
        crate::klog_fmt!("toxenos_rs: virtio_gpu: negotiated features = {:#x}", accepted);
        Ok(GpuBuild { transport, cq: None, req_buf: None, resp_buf: None, cursor_q: None, cursor_req_buf: None })
    }

    /// Phase 2: create the control queue (0) and the optional cursor queue (1). A queue with an
    /// MSI-X entry index is born IRQ-owned; its `queue_msix_vector` is written and verified BEFORE
    /// `queue_enable`. `NO_VECTOR` -> POLL-owned. A vector the device refuses fails the control
    /// queue outright; a failing cursor queue only disables the hardware cursor.
    fn queues(&mut self, vec_ctl: u16, vec_cur: u16) -> Result<(), GpuError> {
        let owner = |v: u16| if v == VIRTIO_MSI_NO_VECTOR { QueueOwner::Poll } else { QueueOwner::Irq };
        let cq = VirtQueue::new_with_vector(&self.transport, 0, 256, vec_ctl, &GPU_CTL_Q, owner(vec_ctl))?;
        self.req_buf = Some(DmaBuffer::alloc_pages(1).ok_or(GpuError::InvalidArgument)?);
        self.resp_buf = Some(DmaBuffer::alloc_pages(1).ok_or(GpuError::InvalidArgument)?);
        self.cq = Some(cq);
        match VirtQueue::new_with_vector(&self.transport, 1, 16, vec_cur, &GPU_CUR_Q, owner(vec_cur)) {
            Ok(q) => match DmaBuffer::alloc_pages(1) {
                Some(buf) => { self.cursor_q = Some(q); self.cursor_req_buf = Some(buf); }
                None => { crate::klog_fmt!("toxenos_rs: virtio_gpu: cursor request buffer allocation failed -- hardware cursor disabled"); }
            },
            Err(VirtioError::VectorRejected) => return Err(GpuError::Transport(VirtioError::VectorRejected)),
            Err(_) => { crate::klog_fmt!("toxenos_rs: virtio_gpu: cursor virtqueue unavailable -- hardware cursor disabled, software cursor remains"); }
        }
        Ok(())
    }

    /// Phase 3: DRIVER_OK. (The GPU sends nothing until its first command.)
    fn finish(mut self) -> Result<GpuDevice, GpuError> {
        self.transport.set_driver_ok();
        Ok(GpuDevice {
            transport: self.transport,
            cq: self.cq.take().ok_or(GpuError::InvalidArgument)?,
            req_buf: self.req_buf.take().ok_or(GpuError::InvalidArgument)?,
            resp_buf: self.resp_buf.take().ok_or(GpuError::InvalidArgument)?,
            next_resource_id: 1, // 0 is reserved/invalid per spec -- never allocated
            cursor_q: self.cursor_q.take(),
            cursor_req_buf: self.cursor_req_buf.take(),
            ctl_abandoned: [None; MAX_ABANDONED],
            cur_abandoned: [None; MAX_ABANDONED],
            faulted: false,
        })
    }
}

/// What a retired completion entry is, relative to the command being waited for.
#[derive(Debug, PartialEq, Eq)]
enum CompKind { Ours, Abandoned(usize), Stale }

/// Everything that can make a command impossible must be known BEFORE anything is submitted: once the
/// doorbell rings the device owns the buffers and will write the response whether or not anyone waits.
/// (IRQ-owned queue in a context that may not wait, a broken queue, or a queue mid-handover.)
fn precheck_submit(shared: &'static QueueShared) -> Result<(), GpuError> {
    if shared.broken() { return Err(GpuError::QueueBroken); }
    match shared.owner() {
        QueueOwner::Irq => if unsafe { ffi::kwait64_can_wait() } == 0 { Err(GpuError::WouldBlock) } else { Ok(()) },
        QueueOwner::Poll => Ok(()),
        QueueOwner::None => Err(GpuError::Quiescing),
    }
}

/// A timed-out command's descriptors stay allocated (the device may still write into them). If
/// there is no room left to remember one, the queue can no longer be trusted: it is marked broken.
fn abandon_chain(list: &mut [Option<Abandoned>; MAX_ABANDONED], shared: &'static QueueShared, head: u16, tail: u16) {
    for slot in list.iter_mut() {
        if slot.is_none() { *slot = Some(Abandoned { head, tail }); return; }
    }
    shared.set_broken();
}

/// A completion arrived for `id`: if it is an abandoned command's head, ITS descriptors are freed now
/// (the device is provably done with them); otherwise it is counted as a stale/unknown completion and
/// nothing is freed.
fn reclaim_chain(q: &mut VirtQueue, list: &mut [Option<Abandoned>; MAX_ABANDONED], shared: &'static QueueShared, id: u16) {
    for slot in list.iter_mut() {
        if let Some(a) = *slot {
            if a.head == id {
                q.free_desc(a.head);
                if a.tail != 0xFFFF { q.free_desc(a.tail); }
                *slot = None;
                shared.abandoned_reclaimed.set(shared.abandoned_reclaimed.get() + 1);
                return;
            }
        }
    }
    shared.stale_completions.set(shared.stale_completions.get() + 1);
}

/// Pure classification used by the waiter: an entry naming the awaited head is ours; one naming an
/// abandoned (timed-out) command's head is reclaimed; anything else is stale.
fn classify_completion(id: u16, head: u16, abandoned: &[Option<Abandoned>]) -> CompKind {
    if id == head { return CompKind::Ours; }
    for (i, a) in abandoned.iter().enumerate() {
        if let Some(a) = a { if a.head == id { return CompKind::Abandoned(i); } }
    }
    CompKind::Stale
}

extern "C" fn cond_ring_nonempty(arg: *mut c_void) -> i32 {
    let q = unsafe { &*(arg as *const QueueShared) };
    // Wake also when ownership changed under the waiter (demotion / broken queue).
    (q.comp_count() > 0 || q.owner() != QueueOwner::Irq || q.broken()) as i32
}

// ── C bridge -- plain values only, never a raw ToxenOS object pointer ─
// One process-wide instance: exactly one VirtIO-GPU device is ever
// probed this milestone (matching gpu64's own "device index 0, or
// whichever registers first" convention). A raw pointer wrapped in a
// tiny newtype instead of a real `Mutex` -- ToxenOS is single-threaded
// cooperative/preemptive-but-not-SMP, and every call into this driver
// happens from ordinary kernel control flow (boot-time probing, or a
// syscall handler), never a real interrupt handler (M+2/M+3 are both
// polling-only) -- so there is no genuine concurrent-access hazard to
// guard against, only the single-threaded discipline every other
// kernel-side singleton in this codebase (e.g. kernel/display64.c's own
// file-scope statics) already relies on.
static mut GPU_DEVICE: Option<GpuDevice> = None;

fn with_device<R>(f: impl FnOnce(&mut GpuDevice) -> Result<R, GpuError>) -> Result<R, GpuError> {
    // M+11C: ONE sleeping mutex serialises every GPU command. Once a command can block waiting
    // for its completion interrupt, another process (e.g. SYS64_GPU_BUFFER_CREATE, or a client
    // exit releasing its buffers) could otherwise enter the driver mid-command and share the
    // request/response buffers. The mutex is a kmutex64 (a SLEEPING lock: contenders block in
    // the scheduler, never spin), never taken from an interrupt handler; the handler only
    // touches the `&'static QueueShared` statics, never the device object.
    unsafe { ffi::toxenos_gpu_ctl_lock(); }
    // SAFETY: the mutex above makes access exclusive; raw-pointer access (addr_of_mut!) avoids
    // ever materialising a reference with a lifetime broader than this call.
    let ptr = core::ptr::addr_of_mut!(GPU_DEVICE);
    let r = match unsafe { (*ptr).as_mut() } {
        Some(dev) => if dev.faulted { Err(GpuError::Faulted) } else { f(dev) },
        None => Err(GpuError::InvalidArgument),
    };
    unsafe { ffi::toxenos_gpu_ctl_unlock(); }
    r
}

fn log_err(context: &str, e: GpuError) {
    crate::klog_fmt!("toxenos_rs: virtio_gpu: {} FAILED: {:?}", context, e);
}

static mut GPU_BUILD: Option<GpuBuild> = None;

/// Phase 1 (spec 3.1.1 steps 1-6): reset/ACKNOWLEDGE/DRIVER/features/FEATURES_OK.
///
/// # Safety
/// `info` must be a genuine `virtio_pci64_probe()` result for a real modern VirtIO-GPU device.
#[no_mangle]
pub unsafe extern "C" fn toxenos_virtio_gpu_init_begin(info: *const TransportInfoRaw) -> i32 {
    if info.is_null() { console::log_line("toxenos_rs: virtio_gpu: NULL info pointer"); return -1; }
    match GpuBuild::begin(&*info) {
        Ok(b) => { *core::ptr::addr_of_mut!(GPU_BUILD) = Some(b); 0 }
        Err(e) => { log_err("init_begin", e); -1 }
    }
}

/// Phase 2: create the queues with MSI-X entry indices (0xFFFF = NO_VECTOR -> POLL-owned).
#[no_mangle]
pub unsafe extern "C" fn toxenos_virtio_gpu_init_queues(vec_ctl: u32, vec_cur: u32) -> i32 {
    let bp = core::ptr::addr_of_mut!(GPU_BUILD);
    match (*bp).as_mut() {
        Some(b) => match b.queues(vec_ctl as u16, vec_cur as u16) {
            Ok(()) => 0,
            Err(e) => { log_err("init_queues", e); -1 }
        },
        None => -1,
    }
}

/// Phase 3: DRIVER_OK; installs the device.
#[no_mangle]
pub unsafe extern "C" fn toxenos_virtio_gpu_init_finish() -> i32 {
    let bp = core::ptr::addr_of_mut!(GPU_BUILD);
    match (*bp).take() {
        Some(b) => match b.finish() {
            Ok(dev) => {
                let irq = GPU_CTL_Q.owner() == QueueOwner::Irq;
                let cur = dev.cursor_q.is_some();
                *core::ptr::addr_of_mut!(GPU_DEVICE) = Some(dev);
                crate::klog_fmt!("toxenos_rs: virtio_gpu: transport + control queue initialized, DRIVER_OK set (owner = {}, cursor = {})", if irq { "IRQ" } else { "POLL" }, cur);
                0
            }
            Err(e) => { log_err("init_finish", e); -1 }
        },
        None => -1,
    }
}

/// Abort: reset the device (unmapping every vector) and drop any partial state.
#[no_mangle]
pub unsafe extern "C" fn toxenos_virtio_gpu_init_abort() {
    if let Some(b) = (*core::ptr::addr_of_mut!(GPU_BUILD)).take() { b.transport.reset(); }
    if let Some(d) = (*core::ptr::addr_of_mut!(GPU_DEVICE)).take() { d.transport.reset(); }
}

/// Poll-mode convenience entry point (the pre-M+11C behaviour).
///
/// # Safety
/// As [`toxenos_virtio_gpu_init_begin`].
#[no_mangle]
pub unsafe extern "C" fn toxenos_virtio_gpu_init(info: *const TransportInfoRaw) -> i32 {
    if toxenos_virtio_gpu_init_begin(info) < 0 { return -1; }
    if toxenos_virtio_gpu_init_queues(VIRTIO_MSI_NO_VECTOR as u32, VIRTIO_MSI_NO_VECTOR as u32) < 0 { toxenos_virtio_gpu_init_abort(); return -1; }
    toxenos_virtio_gpu_init_finish()
}

// ── completion interrupts ──────────────────────────────────────────────
fn gpu_irq(shared: &'static QueueShared, queue: u32) -> i32 {
    shared.last_irq_tsc.set(unsafe { core::arch::x86_64::_rdtsc() });
    match shared.irq_drain_to_ring() {
        None => -1,
        Some(n) => {
            let c = shared.consec_empty.get();
            if c == STORM_WARN {
                console::log_line("toxenos_rs: virtio_gpu: WARNING many consecutive empty interrupts (possible storm)");
                unsafe { ffi::virtio_irq64_note(ffi::VIRQ_DEV_GPU, ffi::VIRQ_EV_STORM_WARN); }
            } else if c == STORM_DEMOTE {
                // Broken path: silence THIS entry now (cheap, IRQ-safe) and ask the next
                // process-context command to complete the device-wide demotion.
                console::log_line("toxenos_rs: virtio_gpu: interrupt path declared broken -- demotion requested");
                unsafe { ffi::virtio_irq64_mask_one(ffi::VIRQ_DEV_GPU, queue); ffi::virtio_irq64_note(ffi::VIRQ_DEV_GPU, ffi::VIRQ_EV_STORM_MASKED); }
                shared.request_demote();
            }
            if n > 0 { unsafe { ffi::kwait64_signal(shared as *const QueueShared as *mut c_void); } }
            n as i32
        }
    }
}

/// Control-queue MSI-X interrupt body. Returns entries consumed (0 = empty), -1 if not IRQ-owned.
#[no_mangle]
pub extern "C" fn toxenos_virtio_gpu_ctl_irq() -> i32 { gpu_irq(&GPU_CTL_Q, 0) }
/// Cursor-queue MSI-X interrupt body.
#[no_mangle]
pub extern "C" fn toxenos_virtio_gpu_cur_irq() -> i32 { gpu_irq(&GPU_CUR_Q, 1) }

/// out[0..19] = [irq_count, completions, max_per_irq, empty_irqs, max_consec_empty, stale_irqs,
///  wrong_owner, poll_drains, poll_calls, waits_woken, timeouts, comp_overflow, inflight_violations,
///  stale_completions, abandoned_reclaimed, wake_lat_total_cycles, wake_lat_max_cycles, wake_lat_count, owner]
/// `which`: 0 = control queue, 1 = cursor queue.
#[no_mangle]
pub extern "C" fn toxenos_virtio_gpu_irq_stats(which: u32, out: *mut u64) -> i32 {
    if out.is_null() { return -1; }
    let q: &QueueShared = if which == 0 { &GPU_CTL_Q } else { &GPU_CUR_Q };
    unsafe {
        *out.add(0) = q.irq_count.get();  *out.add(1) = q.completions.get(); *out.add(2) = q.max_per_irq.get() as u64;
        *out.add(3) = q.empty_irqs.get(); *out.add(4) = q.max_consec_empty.get(); *out.add(5) = q.stale_irqs.get();
        *out.add(6) = q.wrong_owner.get(); *out.add(7) = q.poll_drains.get(); *out.add(8) = q.poll_calls.get();
        *out.add(9) = q.waits_woken.get(); *out.add(10) = q.timeouts.get(); *out.add(11) = q.comp_overflow.get();
        *out.add(12) = q.inflight_violations.get(); *out.add(13) = q.stale_completions.get(); *out.add(14) = q.abandoned_reclaimed.get();
        *out.add(15) = q.wake_lat_total.get(); *out.add(16) = q.wake_lat_max.get(); *out.add(17) = q.wake_lat_count.get();
        *out.add(18) = q.owner() as u64;
    }
    0
}

/// 0 = POLL, 1 = IRQ, 2 = none/fenced; -1 for an unknown queue.
#[no_mangle]
pub extern "C" fn toxenos_virtio_gpu_owner(which: u32) -> i32 {
    if which == 0 { GPU_CTL_Q.owner() as i32 } else if which == 1 { GPU_CUR_Q.owner() as i32 } else { -1 }
}

#[no_mangle]
pub extern "C" fn toxenos_virtio_gpu_is_faulted() -> i32 {
    let ptr = core::ptr::addr_of!(GPU_DEVICE);
    match unsafe { (*ptr).as_ref() } { Some(d) => d.faulted as i32, None => 0 }
}

/// Device-wide verified demotion on demand (tests, diagnostics). Returns the proof id or 0 (FAULTED).
#[no_mangle]
pub extern "C" fn toxenos_virtio_gpu_demote() -> i32 {
    match with_device_raw(|dev| Ok(dev.demote_to_poll())) { Ok(p) => p as i32, Err(_) => -1 }
}

/// Raw `queue_msix_vector` of queue `which` (0 control, 1 cursor): 0xFFFF = NO_VECTOR, -1 = no such queue.
/// Diagnostics/tests: proves after a demotion that EVERY queue is unmapped, not just one.
#[no_mangle]
pub extern "C" fn toxenos_virtio_gpu_read_selector(which: u32) -> i32 {
    match with_device_raw(|dev| Ok(if which == 0 { dev.transport.common.read_queue_vector(0) as i32 }
                                   else if which == 1 && dev.cursor_q.is_some() { dev.transport.common.read_queue_vector(1) as i32 }
                                   else { -1 })) { Ok(v) => v, Err(_) => -1 }
}

/// Test hook: a POLL drain of an IRQ-owned queue must be refused and counted. 1 = refused, 0 = allowed.
#[no_mangle]
pub extern "C" fn toxenos_virtio_gpu_test_wrong_owner_poll(which: u32) -> i32 {
    let ptr = core::ptr::addr_of!(GPU_DEVICE);
    let dev = match unsafe { (*ptr).as_ref() } { Some(d) => d, None => return -1 };
    let (q, shared): (&VirtQueue, &'static QueueShared) = if which == 0 { (&dev.cq, &GPU_CTL_Q) } else { match dev.cursor_q.as_ref() { Some(c) => (c, &GPU_CUR_Q), None => return -1 } };
    if shared.owner() == QueueOwner::Poll { return -1; }
    let mut sink = |_id: u16, _len: u32| {};
    match q.poll_drain(&mut sink) { Err(DrainErr::WrongOwner) => 1, Ok(_) => 0 }
}

/// Like `with_device` but WITHOUT the sleeping mutex, for a caller that already holds it.
fn with_device_raw<R>(f: impl FnOnce(&mut GpuDevice) -> Result<R, GpuError>) -> Result<R, GpuError> {
    unsafe { ffi::toxenos_gpu_ctl_lock(); }
    let ptr = core::ptr::addr_of_mut!(GPU_DEVICE);
    let r = match unsafe { (*ptr).as_mut() } { Some(dev) => f(dev), None => Err(GpuError::InvalidArgument) };
    unsafe { ffi::toxenos_gpu_ctl_unlock(); }
    r
}

/// Fills `out` (a caller-owned array of at least 16 entries, one C
/// `struct { u32 x,y,width,height,enabled; }` each -- see
/// include/virtio_gpu64.h's own mirror) with every discovered scanout.
/// Returns the number of ENABLED scanouts (>= 0), or -1 on failure.
#[no_mangle]
pub unsafe extern "C" fn toxenos_virtio_gpu_get_display_info(out: *mut u32) -> i32 {
    if out.is_null() {
        return -1;
    }
    let r = with_device(|dev| dev.get_display_info());
    match r {
        Ok(modes) => {
            let mut enabled_count = 0i32;
            for (i, m) in modes.iter().enumerate() {
                let base = out.add(i * 5);
                core::ptr::write(base, m.x);
                core::ptr::write(base.add(1), m.y);
                core::ptr::write(base.add(2), m.width);
                core::ptr::write(base.add(3), m.height);
                core::ptr::write(base.add(4), m.enabled as u32);
                if m.enabled {
                    enabled_count += 1;
                    crate::klog_fmt!(
                        "toxenos_rs: virtio_gpu: scanout[{}] {}x{} at ({},{})",
                        i, m.width, m.height, m.x, m.y
                    );
                }
            }
            enabled_count
        }
        Err(e) => {
            log_err("get_display_info", e);
            -1
        }
    }
}

#[no_mangle]
pub extern "C" fn toxenos_virtio_gpu_resource_create_2d(width: u32, height: u32, format: u32, resource_id_out: *mut u32) -> i32 {
    let r = with_device(|dev| dev.resource_create_2d(width, height, format));
    match r {
        Ok(id) => {
            unsafe { core::ptr::write(resource_id_out, id) };
            0
        }
        Err(e) => {
            log_err("resource_create_2d", e);
            -1
        }
    }
}

/// `phys`/`len` are two caller-owned parallel arrays of `count` entries
/// each -- exactly `memobj64_get_runs()`'s own output shape, kept as
/// plain arrays across the FFI boundary rather than a shared struct
/// definition (see this module's own header comment on why no ToxenOS
/// object crosses this boundary).
#[no_mangle]
pub unsafe extern "C" fn toxenos_virtio_gpu_attach_backing(resource_id: u32, phys: *const u64, len: *const u32, count: u32) -> i32 {
    if phys.is_null() || len.is_null() || count == 0 || count as usize > MAX_BACKING_ENTRIES {
        return -1;
    }
    let mut runs = [(0u64, 0u32); MAX_BACKING_ENTRIES];
    for i in 0..count as usize {
        runs[i] = (core::ptr::read(phys.add(i)), core::ptr::read(len.add(i)));
    }
    let r = with_device(|dev| dev.resource_attach_backing(resource_id, &runs[..count as usize]));
    match r {
        Ok(()) => 0,
        Err(e) => {
            log_err("resource_attach_backing", e);
            -1
        }
    }
}

#[no_mangle]
pub extern "C" fn toxenos_virtio_gpu_detach_backing(resource_id: u32) -> i32 {
    match with_device(|dev| dev.resource_detach_backing(resource_id)) {
        Ok(()) => 0,
        Err(e) => {
            log_err("resource_detach_backing", e);
            -1
        }
    }
}

#[no_mangle]
pub extern "C" fn toxenos_virtio_gpu_transfer_to_host_2d(resource_id: u32, x: u32, y: u32, w: u32, h: u32, res_w: u32, res_h: u32) -> i32 {
    match with_device(|dev| dev.transfer_to_host_2d(resource_id, x, y, w, h, res_w, res_h)) {
        Ok(()) => 0,
        Err(e) => {
            log_err("transfer_to_host_2d", e);
            -1
        }
    }
}

#[no_mangle]
pub extern "C" fn toxenos_virtio_gpu_resource_flush(resource_id: u32, x: u32, y: u32, w: u32, h: u32, res_w: u32, res_h: u32) -> i32 {
    match with_device(|dev| dev.resource_flush(resource_id, x, y, w, h, res_w, res_h)) {
        Ok(()) => 0,
        Err(e) => {
            log_err("resource_flush", e);
            -1
        }
    }
}

#[no_mangle]
pub extern "C" fn toxenos_virtio_gpu_set_scanout(scanout_id: u32, resource_id: u32, x: u32, y: u32, w: u32, h: u32) -> i32 {
    match with_device(|dev| dev.set_scanout(scanout_id, resource_id, x, y, w, h)) {
        Ok(()) => 0,
        Err(e) => {
            log_err("set_scanout", e);
            -1
        }
    }
}

#[no_mangle]
pub extern "C" fn toxenos_virtio_gpu_resource_unref(resource_id: u32) -> i32 {
    match with_device(|dev| dev.resource_unref(resource_id)) {
        Ok(()) => 0,
        Err(e) => {
            log_err("resource_unref", e);
            -1
        }
    }
}

// ── M+7: hardware cursor bridge ──────────────────────────────────────
#[no_mangle]
pub extern "C" fn toxenos_virtio_gpu_cursor_available() -> i32 {
    match with_device(|dev| Ok(dev.cursor_available())) {
        Ok(true) => 1,
        Ok(false) => 0,
        Err(_) => 0, // no device at all -- same as "no cursor," not an error C needs to distinguish
    }
}

#[no_mangle]
pub extern "C" fn toxenos_virtio_gpu_cursor_update(scanout_id: u32, resource_id: u32, x: u32, y: u32, hot_x: u32, hot_y: u32) -> i32 {
    match with_device(|dev| dev.cursor_update(scanout_id, resource_id, x, y, hot_x, hot_y)) {
        Ok(()) => 0,
        Err(e) => {
            log_err("cursor_update", e);
            -1
        }
    }
}

#[no_mangle]
pub extern "C" fn toxenos_virtio_gpu_cursor_move(scanout_id: u32, resource_id: u32, x: u32, y: u32) -> i32 {
    match with_device(|dev| dev.cursor_move(scanout_id, resource_id, x, y)) {
        Ok(()) => 0,
        Err(e) => {
            log_err("cursor_move", e);
            -1
        }
    }
}

/// M+4 investigation: fills `out[0..4]` with
/// `[commands, poll_iters_total, poll_iters_max, cycles_total]` --
/// see this module's own "RDTSC-based command-latency instrumentation"
/// comment. Cycle values are raw RDTSC deltas; the caller (C side,
/// kernel/tsc64.c) converts to real time. Always succeeds (writes
/// whatever has accumulated so far, all zero if no command has ever
/// been sent).
#[no_mangle]
pub extern "C" fn toxenos_virtio_gpu_debug_stats(out: *mut u64) -> i32 {
    if out.is_null() { return -1; }
    unsafe {
        *out.add(0) = STAT_COMMANDS;
        *out.add(1) = STAT_POLL_ITERS_TOTAL;
        *out.add(2) = STAT_POLL_ITERS_MAX;
        *out.add(3) = STAT_CYCLES_TOTAL;
    }
    0
}

/// M+4 investigation: zeroes every counter [`toxenos_virtio_gpu_debug_stats`]
/// reports, so a caller can measure one specific window of activity
/// (e.g. "the next 5 seconds of dragging") instead of a cumulative
/// since-boot total.
#[no_mangle]
pub extern "C" fn toxenos_virtio_gpu_debug_stats_reset() {
    unsafe {
        STAT_COMMANDS = 0;
        STAT_POLL_ITERS_TOTAL = 0;
        STAT_POLL_ITERS_MAX = 0;
        STAT_CYCLES_TOTAL = 0;
    }
}

/// Diagnostics: returns 1 if a device was successfully initialized by
/// [`toxenos_virtio_gpu_init`], 0 otherwise.
#[no_mangle]
pub extern "C" fn toxenos_virtio_gpu_is_ready() -> i32 {
    // SAFETY: single-threaded, see GPU_DEVICE's/with_device's own comment.
    let ptr = core::ptr::addr_of!(GPU_DEVICE);
    if unsafe { (*ptr).is_some() } {
        1
    } else {
        0
    }
}


// ══════════════════════════════════════════════════════════════════════
// M+11C GPU completion self-tests (no device): the completion ring and the
// waiter's view of it. Returns a bitmask of FAILED groups.
// ══════════════════════════════════════════════════════════════════════
struct TestRing { buf: alloc::vec::Vec<u8>, idx: u16, size: u16 }
impl TestRing {
    fn new(size: u16) -> Self { TestRing { buf: alloc::vec![0u8; 6 + size as usize * 8], idx: 0, size } }
    fn base(&mut self) -> *mut u8 { self.buf.as_mut_ptr() }
    fn complete(&mut self, id: u32, len: u32) {
        let off = 4 + (self.idx % self.size) as usize * 8;
        unsafe { core::ptr::write_volatile(self.base().add(off) as *mut u32, id); core::ptr::write_volatile(self.base().add(off + 4) as *mut u32, len); }
        self.idx = self.idx.wrapping_add(1);
        let i = self.idx; unsafe { core::ptr::write_volatile(self.base().add(2) as *mut u16, i); }
    }
}
fn test_shared() -> &'static QueueShared { alloc::boxed::Box::leak(alloc::boxed::Box::new(QueueShared::new())) }

pub fn selftest_gpu() -> u32 {
    let mut failed = 0u32;
    let mut check = |g: u32, name: &str, ok: bool| { crate::virtio_pci::selftest_count(); if !ok { failed |= 1 << g; crate::klog_fmt!("toxenos_rs: virtio_gpu irq selftest FAIL [{}]: {}", g, name); } };
    let waiter_cond = |sh: &'static QueueShared| cond_ring_nonempty(sh as *const QueueShared as *mut c_void) != 0;

    // 0: command completes BEFORE the waiter sleeps -> cond already true; entry retired exactly once
    {
        let mut r = TestRing::new(256); let sh = test_shared(); sh.attach(r.base(), 256, QueueOwner::Irq);
        r.complete(4, 24);                                      // device completes, IRQ runs, only then the waiter checks
        sh.irq_drain_to_ring();
        check(0, "completion before sleep: condition already true, no block needed", waiter_cond(sh));
        check(0, "retired once", sh.comp_pop_front() == Some((4, 24)) && sh.comp_pop_front().is_none());
    }
    // 1: completion WHILE sleeping -> cond false until the IRQ handler ran, then true (the wake)
    {
        let mut r = TestRing::new(256); let sh = test_shared(); sh.attach(r.base(), 256, QueueOwner::Irq);
        check(1, "nothing yet: the waiter would block", !waiter_cond(sh));
        r.complete(2, 24);
        check(1, "device completion alone does not satisfy the (IRQ-owned) waiter", !waiter_cond(sh));   // only the IRQ path discovers it
        sh.irq_drain_to_ring();
        check(1, "after the interrupt handler: condition true", waiter_cond(sh) && sh.comp_pop_front() == Some((2, 24)));
    }
    // 2: several completions in one interrupt: stale/abandoned first, then ours
    {
        let mut r = TestRing::new(256); let sh = test_shared(); sh.attach(r.base(), 256, QueueOwner::Irq);
        r.complete(10, 24); r.complete(11, 24);
        sh.irq_drain_to_ring();
        let list = [Some(Abandoned { head: 10, tail: 12 }), None, None, None];
        let mut kinds = alloc::vec::Vec::new();
        while let Some((id, _)) = sh.comp_pop_front() { kinds.push(classify_completion(id, 11, &list)); }
        check(2, "abandoned entry reclaimed, ours delivered, in one IRQ", kinds == alloc::vec![CompKind::Abandoned(0), CompKind::Ours]);
        check(2, "unknown id is stale", classify_completion(99, 11, &list) == CompKind::Stale);
        check(2, "head match wins", classify_completion(11, 11, &list) == CompKind::Ours);
    }
    // 3: cursor queue: independent QueueShared, same completion path, completion recognised by head
    {
        let mut r = TestRing::new(16); let sh = test_shared(); sh.attach(r.base(), 16, QueueOwner::Irq);
        r.complete(0, 0);
        sh.irq_drain_to_ring();
        check(3, "cursor completion delivered through its own queue state", sh.comp_pop_front() == Some((0, 0)) && sh.completions.get() == 1);
    }
    // 4: ownership change under a waiter wakes it (cond true when the owner is no longer IRQ)
    {
        let mut r = TestRing::new(256); let sh = test_shared(); sh.attach(r.base(), 256, QueueOwner::Irq);
        check(4, "IRQ-owned, nothing pending: waiter blocks", !waiter_cond(sh));
        sh.set_owner(QueueOwner::None);
        check(4, "demotion fence wakes the waiter to re-evaluate", waiter_cond(sh));
        sh.set_owner(QueueOwner::Irq); sh.set_broken();
        check(4, "a broken queue wakes the waiter", waiter_cond(sh));
    }
    // 5: descriptors of an abandoned command are never recycled early: list capacity, then broken
    {
        let mut list: [Option<Abandoned>; MAX_ABANDONED] = [None; MAX_ABANDONED];
        for i in 0..MAX_ABANDONED { list[i] = Some(Abandoned { head: i as u16, tail: 0xFFFF }); }
        check(5, "abandoned list full", list.iter().all(|a| a.is_some()));
        check(5, "reclaim by head", classify_completion(2, 99, &list) == CompKind::Abandoned(2));
    }
    // 6: the REAL descriptor lifecycle of an abandoned (timed-out) command, on a genuine VirtQueue built
    //    over a synthetic transport (heap buffers stand in for the device registers): the abandoned
    //    chain is never handed out again until its completion actually arrives.
    {
        let sh = test_shared();
        let r = crate::virtio_pci::with_synthetic_transport(|t| {
            let q = VirtQueue::new_with_vector(t, 0, 16, 0, sh, QueueOwner::Irq).ok();
            match q {
                None => check(6, "synthetic queue constructed", false),
                Some(mut q) => {
                    let free0 = q.free_count();
                    let (head, tail) = match (q.alloc_desc(), q.alloc_desc()) {
                        (Some(h), Some(t)) => (h, t),
                        _ => { check(6, "two descriptors allocatable", false); return; }
                    };
                    check(6, "a 2-descriptor command holds exactly 2", free0 == 16 && q.free_count() == free0 - 2);
                    let mut list: [Option<Abandoned>; MAX_ABANDONED] = [None; MAX_ABANDONED];
                    abandon_chain(&mut list, sh, head, tail);                       // the command timed out
                    check(6, "abandoning frees nothing and does not break the queue", q.free_count() == free0 - 2 && !sh.broken());
                    let mut got = alloc::vec::Vec::new();
                    while let Some(d) = q.alloc_desc() { got.push(d); }
                    check(6, "an abandoned chain is NEVER re-allocated while abandoned",
                          got.len() == (free0 - 2) as usize && !got.contains(&head) && !got.contains(&tail) && q.alloc_desc().is_none());
                    reclaim_chain(&mut q, &mut list, sh, got[0]);                   // some unknown/stale completion
                    check(6, "a stale completion frees nothing and is counted", q.free_count() == 0 && sh.stale_completions.get() == 1 && list[0].is_some());
                    reclaim_chain(&mut q, &mut list, sh, head);                     // the late completion of the abandoned command
                    check(6, "the late completion frees exactly its head+tail",
                          q.free_count() == 2 && sh.abandoned_reclaimed.get() == 1 && list.iter().all(|a| a.is_none()));
                    let (r1, r2) = match (q.alloc_desc(), q.alloc_desc()) {
                        (Some(a), Some(b)) => (a, b),
                        _ => { check(6, "reclaimed descriptors allocatable", false); return; }
                    };
                    check(6, "the reclaimed descriptors are reusable now, and only those",
                          r1 != r2 && (r1 == head || r1 == tail) && (r2 == head || r2 == tail) && q.alloc_desc().is_none());
                    reclaim_chain(&mut q, &mut list, sh, head);
                    check(6, "a duplicate completion never double-frees", q.free_count() == 0 && sh.stale_completions.get() == 2);
                    // single-descriptor chain (tail == 0xFFFF): exactly one descriptor is returned
                    q.free_desc(r1);
                    let d1 = match q.alloc_desc() { Some(d) => d, None => { check(6, "a descriptor allocatable", false); return; } };
                    abandon_chain(&mut list, sh, d1, 0xFFFF);
                    reclaim_chain(&mut q, &mut list, sh, d1);
                    check(6, "a 1-descriptor chain frees exactly 1", q.free_count() == 1 && sh.abandoned_reclaimed.get() == 2);
                }
            }
        });
        check(6, "synthetic transport constructed", r.is_some());
        // no room to remember one more abandoned chain -> the queue can no longer be trusted
        let sh2 = test_shared(); let mut list2: [Option<Abandoned>; MAX_ABANDONED] = [None; MAX_ABANDONED];
        for i in 0..MAX_ABANDONED { abandon_chain(&mut list2, sh2, i as u16, 0xFFFF); }
        check(6, "MAX_ABANDONED chains remembered without breaking", !sh2.broken() && list2.iter().all(|a| a.is_some()));
        abandon_chain(&mut list2, sh2, 99, 0xFFFF);
        check(6, "one more than fits -> queue marked broken", sh2.broken());
    }
    // 7: the submit pre-check, decided BEFORE anything is handed to the device
    {
        let mut ra = TestRing::new(16); let sa = test_shared(); sa.attach(ra.base(), 16, QueueOwner::Poll);
        check(7, "POLL-owned queue: submission allowed", precheck_submit(sa).is_ok());
        sa.set_broken();
        check(7, "broken queue: refused before submission", precheck_submit(sa) == Err(GpuError::QueueBroken));
        let mut rb = TestRing::new(16); let sb = test_shared(); sb.attach(rb.base(), 16, QueueOwner::None);
        check(7, "queue mid-handover (owner NONE): refused", precheck_submit(sb) == Err(GpuError::Quiescing));
        let mut rc = TestRing::new(16); let sc = test_shared(); sc.attach(rc.base(), 16, QueueOwner::Irq);
        check(7, "IRQ-owned queue, context that may wait (boot, IF=1): allowed", precheck_submit(sc).is_ok());
        sc.set_broken();
        check(7, "IRQ-owned but broken: refused", precheck_submit(sc) == Err(GpuError::QueueBroken));
    }
    failed
}

#[no_mangle]
pub extern "C" fn toxenos_virtio_irq_selftest_gpu() -> u32 { selftest_gpu() }
