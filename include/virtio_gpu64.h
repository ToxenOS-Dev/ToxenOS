#ifndef VIRTIO_GPU64_H
#define VIRTIO_GPU64_H

#include <stdint.h>
#include "gpu64.h"

// M+3: the VirtIO-GPU 2D driver's C-side bridge. Owns everything the
// ToxenOS object model (gpu64_device_t/gpu64_buffer_t/memobj64_t) needs
// to know about a VirtIO-GPU device; the actual wire protocol lives
// exclusively in Rust (rust/toxenos_rs/src/virtio_gpu.rs) -- this file
// calls it only through the plain-value extern "C" functions declared
// below, never handing it a raw ToxenOS struct pointer, mirroring
// exactly how M+2's virtio_pci64.c/virtio_pci.rs split already works.
//
// Scope (see this milestone's own brief): GET_DISPLAY_INFO,
// RESOURCE_CREATE_2D, RESOURCE_ATTACH_BACKING/DETACH_BACKING,
// TRANSFER_TO_HOST_2D, RESOURCE_FLUSH, SET_SCANOUT, RESOURCE_UNREF.
// No VirGL, no contexts, no interrupts.
//
// M+7 adds the dedicated cursor virtqueue (UPDATE_CURSOR/MOVE_CURSOR) --
// see this header's own "hardware cursor" section below and
// kernel/virtio_gpu64.c's virtio_gpu64_cursor_init() for the full
// design. Still optional: a missing/failed cursor queue leaves
// compositor64 on the unchanged software cursor path.

// ── Rust entry points (rust/toxenos_rs/src/virtio_gpu.rs) ───────────
// Signatures copied by hand from that file's own #[no_mangle] functions
// -- same "duplicated, documented, kept in sync by convention"
// discipline user64/tox64.h and rust/toxenos_rs/src/ffi.rs already use
// at their own boundaries.
#include "virtio_pci64.h" // virtio_pci64_transport_info_t

int32_t toxenos_virtio_gpu_init(const virtio_pci64_transport_info_t* info);   // POLL-mode (begin+queues+finish)
// M+11C phased initialisation, interrupt entry points and diagnostics (rust/toxenos_rs/src/virtio_gpu.rs)
int32_t toxenos_virtio_gpu_init_begin(const virtio_pci64_transport_info_t* info);
int32_t toxenos_virtio_gpu_init_queues(uint32_t vec_ctl, uint32_t vec_cur);
int32_t toxenos_virtio_gpu_init_finish(void);
void    toxenos_virtio_gpu_init_abort(void);
int32_t toxenos_virtio_gpu_ctl_irq(void);
int32_t toxenos_virtio_gpu_cur_irq(void);
int32_t toxenos_virtio_gpu_irq_stats(uint32_t which, uint64_t* out19);
int32_t toxenos_virtio_gpu_owner(uint32_t which);
int32_t toxenos_virtio_gpu_is_faulted(void);
int32_t toxenos_virtio_gpu_demote(void);
int32_t toxenos_virtio_gpu_test_wrong_owner_poll(uint32_t which);
// out must point at 16*5 uint32_t: per scanout, {x,y,width,height,enabled}.
// Returns the number of ENABLED scanouts (>=0), or -1.
int32_t toxenos_virtio_gpu_get_display_info(uint32_t* out);
int32_t toxenos_virtio_gpu_resource_create_2d(uint32_t width, uint32_t height, uint32_t format, uint32_t* resource_id_out);
int32_t toxenos_virtio_gpu_attach_backing(uint32_t resource_id, const uint64_t* phys, const uint32_t* len, uint32_t count);
int32_t toxenos_virtio_gpu_detach_backing(uint32_t resource_id);
int32_t toxenos_virtio_gpu_transfer_to_host_2d(uint32_t resource_id, uint32_t x, uint32_t y, uint32_t w, uint32_t h, uint32_t res_w, uint32_t res_h);
int32_t toxenos_virtio_gpu_resource_flush(uint32_t resource_id, uint32_t x, uint32_t y, uint32_t w, uint32_t h, uint32_t res_w, uint32_t res_h);
int32_t toxenos_virtio_gpu_set_scanout(uint32_t scanout_id, uint32_t resource_id, uint32_t x, uint32_t y, uint32_t w, uint32_t h);
int32_t toxenos_virtio_gpu_resource_unref(uint32_t resource_id);
int32_t toxenos_virtio_gpu_is_ready(void);
// M+7: 1 if the cursor virtqueue was successfully set up (always before
// DRIVER_OK -- see rust/toxenos_rs/src/virtio_gpu.rs's own GpuDevice::new()
// comment), 0 otherwise (no device, or the device/transport didn't offer
// queue 1, or setup failed) -- the single source of truth every other
// cursor entry point in this file checks first.
int32_t toxenos_virtio_gpu_cursor_available(void);
// VIRTIO_GPU_CMD_UPDATE_CURSOR -- sets which resource is the cursor
// image, its hotspot, and its position, via the DEDICATED cursor
// virtqueue (never the control queue). resource_id == 0 hides the
// cursor. Call once at image-creation time; not the per-move path.
int32_t toxenos_virtio_gpu_cursor_update(uint32_t scanout_id, uint32_t resource_id, uint32_t x, uint32_t y, uint32_t hot_x, uint32_t hot_y);
// VIRTIO_GPU_CMD_MOVE_CURSOR -- the cheap, common-case position-only
// update, via the cursor virtqueue. `resource_id` is echoed (matching
// the same wire struct UPDATE_CURSOR uses) but carries no new meaning
// here.
int32_t toxenos_virtio_gpu_cursor_move(uint32_t scanout_id, uint32_t resource_id, uint32_t x, uint32_t y);
// M+4 investigation: out must point at 4 uint64_t: [commands,
// poll_iters_total, poll_iters_max, cycles_total] (raw RDTSC deltas --
// see virtio_gpu64_debug_stats_report() below for the converted form).
int32_t toxenos_virtio_gpu_debug_stats(uint64_t* out);
void toxenos_virtio_gpu_debug_stats_reset(void);

// ── Format ────────────────────────────────────────────────────────
// VIRTIO_GPU_FORMAT_B8G8R8X8_UNORM -- matches ToxenOS's existing logical
// 0x00RRGGBB framebuffer format exactly (same identity
// kernel/display64.c's display64_blit_row() fast path already relies
// on), so test pixels need no conversion. See rust/toxenos_rs/src/
// virtio_gpu.rs's own comment on this same constant.
#define VIRTIO_GPU64_FORMAT_B8G8R8X8_UNORM 2
// M+7: the cursor resource needs alpha (transparency) that the scanout's
// own X8 (no-alpha) format cannot express -- VIRTIO_GPU_FORMAT_B8G8R8A8_UNORM
// per the same Linux uapi/linux/virtio_gpu.h reference this codebase
// already verifies every other format constant against. Byte layout
// (little-endian): byte0=B, byte1=G, byte2=R, byte3=A -- exactly ToxenOS's
// own logical 0xAARRGGBB packed as a little-endian uint32_t, the same
// "identity, no conversion needed" property VIRTIO_GPU64_FORMAT_B8G8R8X8_UNORM
// already relies on for the non-alpha case.
#define VIRTIO_GPU64_FORMAT_B8G8R8A8_UNORM 1

// One discovered scanout, mirroring toxenos_virtio_gpu_get_display_info's
// flat uint32_t[5]-per-entry output.
typedef struct {
    uint32_t x, y, width, height;
    uint32_t enabled;
} virtio_gpu64_display_mode_t;

#define VIRTIO_GPU64_MAX_SCANOUTS 16

// Probes for a modern VirtIO-GPU PCI device (PCI64_DEVICE_VIRTIO_GPU_MODERN
// -- the SAME device ID M+2 already used for transport testing) and, if
// found, initializes the driver and registers it as a real gpu64_device_t
// (kind GPU64_DEVICE_VIRTIO). Returns 0 on success, -1 if no such device
// exists or initialization failed -- NEVER touches the existing
// Multiboot framebuffer / display64 software path either way (see this
// milestone's own "must not destroy the fallback" requirement).
int virtio_gpu64_init(void);

// Fills `out` (VIRTIO_GPU64_MAX_SCANOUTS entries) via GET_DISPLAY_INFO.
// Returns the number of enabled scanouts, or -1. Requires
// virtio_gpu64_init() to have already succeeded.
int virtio_gpu64_get_display_info(virtio_gpu64_display_mode_t* out);

// Gives an ALREADY-CREATED gpu64_buffer_t (from the generic
// gpu64_buffer_create()/gpu64_buffer_wrap()) real VirtIO-GPU scanout
// geometry: creates a matching resource, attaches the buffer's own
// memobj64 backing (via memobj64_get_runs() -- never reading
// base_phys/npages directly), and stores the resulting resource ID on
// the buffer. Deliberately NOT folded into gpu64_buffer_create()'s own
// generic (size, usage) signature, which has no width/height/format --
// inferring image dimensions from a byte count is exactly what this
// milestone's own brief forbids (§05). `buf` must not already have a
// bound resource (driver_resource_id == 0). Returns 0 on success
// (buf->driver_resource_id set to the new, nonzero resource ID), or -1
// (leaves buf completely unmodified -- no partial resource/backing left
// dangling).
int virtio_gpu64_bind_scanout(gpu64_buffer_t* buf, uint32_t width, uint32_t height);

// Reverses virtio_gpu64_bind_scanout(): detaches backing, unrefs the
// resource, clears buf->driver_resource_id back to 0. Safe to call on a
// buffer that was never bound (no-op, returns 0) -- this is what
// virtio_gpu64's own gpu64_device_ops_t.destroy_buffer hook calls
// unconditionally on every buffer it's asked to destroy.
int virtio_gpu64_unbind_scanout(gpu64_buffer_t* buf);

// The item-9 isolated proof: allocates its OWN memobj64-backed buffer
// (NOT going through gpu64_buffer_create() at all -- this is a
// driver-internal test, not a client-facing API call), fills it with an
// obvious test pattern (color bars), and drives create -> attach ->
// transfer -> set_scanout -> flush against scanout 0. Leaves the
// resource bound (visible) on success so the caller can screenshot it;
// the caller is responsible for tearing it down afterward via the
// returned buffer + virtio_gpu64_unbind_scanout()/gpu64_buffer_release().
// Returns the new gpu64_buffer_t* on success (never NULL then), or NULL
// on any failure (nothing left allocated).
gpu64_buffer_t* virtio_gpu64_run_scanout_test(uint32_t width, uint32_t height);

// M+3 item 15: honest performance comparison between the existing
// software present path (display64_blit_row(), the same one
// sys64_display_present's own implementation uses) and VirtIO-GPU's
// TRANSFER_TO_HOST_2D + RESOURCE_FLUSH, for `width`x`height` repeated
// `iterations` times. Logs raw tick counts (100Hz, 10ms/tick) for both
// -- no derived percentage, since tick granularity is too coarse for
// one to be honest. Requires a real device (no-op, logged, if absent).
void virtio_gpu64_perf_compare(uint32_t width, uint32_t height, uint32_t iterations);

// M+4: real compositor integration. Called once, automatically, right
// after virtio_gpu64_init() succeeds (kernel/kernel64.c, unconditionally
// -- no debug flag, matching every storage driver's own "always probe"
// convention) -- reads the ACTUAL discovered output geometry via
// GET_DISPLAY_INFO (never hardcoded), creates ToxenOS's one persistent
// compositor-presentation gpu64_buffer_t at that size, uploads a known-
// safe initial (black) frame, binds+transfers+SET_SCANOUTs+flushes it
// (never pointing scanout at an uninitialized resource), and registers
// it with display64.c as the active presentation backend (see
// display64_set_gpu_backend()). Unlike virtio_gpu64_run_scanout_test()'s
// disposable self-test buffer, this buffer is never torn down in normal
// operation -- it lives for the rest of the boot session, exactly like
// the legacy Multiboot framebuffer's own mapping.
//
// A failure at ANY step is NOT fatal to boot: logged via klog(), and
// display64 is left completely untouched (still on whatever framebuffer-
// or-none state it already had) -- this function makes no partial
// change to display64's active backend on failure. Returns 0 on
// success, or -1 (device not registered, no enabled scanout, or any
// setup step failed). Calling this a second time in the same boot is a
// safe, logged no-op (-1) -- at most one compositor backend per boot.
int virtio_gpu64_init_compositor_backend(void);

// ── M+7: hardware cursor ──────────────────────────────────────────────
// Cursor resource: fixed 64x64, VIRTIO_GPU64_FORMAT_B8G8R8A8_UNORM
// (alpha-capable). This driver never generates or knows the cursor's
// actual pixel shape -- that would cross the same kernel/userspace
// policy boundary Revision 2 §02 already draws for "which cursor shape
// means resize vs. text": only WHICH resource shows at WHICH position is
// this driver's concern. compositor64 (userspace) draws its own existing
// procedural cursor shape (see user64/compositor64.c's bb_draw_cursor())
// into a 64x64 ARGB buffer once, at startup, and uploads it via
// SYS64_CURSOR_SET_IMAGE -- see kernel/virtio_gpu64.c's own
// cursor_backend_set_image() for the receiving end. VIRTIO_GPU64_CURSOR_HOT_X/Y
// document the hotspot compositor64 actually uses (0,0, matching its
// triangle's own top-left-anchored shape) but are not enforced here --
// compositor64 passes its own hotspot with each image upload.
#define VIRTIO_GPU64_CURSOR_DIM   64
#define VIRTIO_GPU64_CURSOR_HOT_X 0
#define VIRTIO_GPU64_CURSOR_HOT_Y 0

// Called once, automatically, right after virtio_gpu64_init_compositor_backend()
// succeeds (kernel/kernel64.c, unconditionally -- no debug flag, same
// "always probe" convention every other driver here uses). Requires the
// cursor virtqueue (toxenos_virtio_gpu_cursor_available()) -- if that,
// or any step of resource creation/upload/initial UPDATE_CURSOR, fails,
// this function logs and returns -1 WITHOUT touching display64's cursor
// backend registration at all (see include/display64.h's own
// display64_set_cursor_backend()) -- compositor64's later
// SYS64_CURSOR_AVAILABLE query then correctly reports "no hardware
// cursor," and it uses its own existing, completely unmodified software
// cursor path. Never fatal to boot either way, exactly like
// virtio_gpu64_init_compositor_backend() itself. Returns 0 on success
// (display64's cursor backend is live), or -1.
int virtio_gpu64_init_cursor(void);

// Returns 1 if virtio_gpu64_init_cursor() succeeded this boot, 0
// otherwise. Exposed separately from the Rust-level
// toxenos_virtio_gpu_cursor_available() because THIS reflects "the
// cursor RESOURCE is live and usable," a stronger condition than merely
// "the transport queue exists" (that alone doesn't mean resource
// creation/upload also succeeded).
int virtio_gpu64_cursor_available(void);

// Deterministic cursor pixel-format proof (current hardening pass) --
// uploads known ARGB pixels through the real display64_cursor_set_image()
// entry point and verifies the exact bytes landing in the resource's
// own backing memory, with no reliance on visual inspection. Returns 1
// if every pixel matched (or if no cursor backend exists -- absence is
// a first-class outcome, same precedent every other selftest here
// uses), 0 on any mismatch.
int virtio_gpu64_cursor_pixel_selftest(void);

// M+10: registers the atomic check/commit backend (display64.h's
// display64_set_atomic_backend()) and runs display64_atomic_selftest()
// once, immediately -- call this from kernel64.c right after
// virtio_gpu64_init_cursor() (cursor must already be set up, since some
// self-test cases exercise it). Never fatal to boot -- a registration or
// self-test failure is logged only, matching every other init step
// here. Returns 0 on successful registration, -1 if
// display64_set_atomic_backend() itself was rejected (e.g. called
// twice).
int virtio_gpu64_init_atomic_backend(void);

// M+10 fault-injection campaign (mandatory before acceptance): exercises
// every fault point individually (primary TRANSFER/SET_SCANOUT/
// RESOURCE_FLUSH, cursor transfer/UPDATE_CURSOR/MOVE_CURSOR), a clean
// pre-mutation failure, a partial failure with successful rollback, and
// a partial failure whose OWN rollback also fails (RECOVERY_REQUIRED,
// then a subsequent successful display64_state_retry_recovery() back to
// VALID). Lives here (not display64.h) because it needs backend-private
// knowledge (VIRTIO_GPU64_CURSOR_DIM, exact command sequences) to
// construct scenarios that reach past display64_state_check() --
// display64.c itself stays VirtIO-agnostic. Only meaningfully runs
// anything when M9B_FAULT_INJECT is defined AND a real VirtIO-GPU
// compositor backend is live; otherwise a vacuous pass (same "absence
// is a first-class outcome" precedent every *_selftest() here uses).
// Logs each case and a final tally via klog(). Returns 1 if every case
// passed. Called automatically from virtio_gpu64_init_atomic_backend().
int display64_atomic_fault_selftest(void);

// Runs this file's own self-test suite: GET_DISPLAY_INFO, create/attach/
// transfer/flush/set_scanout/destroy round trip, and the validation
// cases practical without a real malformed device (invalid resource ID,
// invalid rect, forced allocation failure). Requires a real modern
// VirtIO-GPU device -- there is no synthetic stand-in for a device that
// actually implements the protocol (same limitation M+2's own
// device-dependent self-test documents). Returns 1 if every case passed.
int virtio_gpu64_selftest(void);

// M+4 investigation: logs a human-readable summary of
// toxenos_virtio_gpu_debug_stats()'s counters, converting the raw RDTSC
// cycle total to microseconds via tsc64_cycles_to_us() -- the
// high-resolution clock this investigation added specifically because
// the 100Hz PIT tick could not expose real, manually-observed
// interactive lag. Reports commands/sec over the elapsed window if
// `window_ms` is nonzero (caller-supplied: this function has no clock
// of its own beyond what it's told). Does not reset the counters --
// call virtio_gpu64_debug_stats_reset() (thin wrapper over the Rust
// function of the same name) separately if a caller wants windowed
// rather than cumulative-since-boot numbers.
void virtio_gpu64_debug_stats_report(uint64_t window_ms);
void virtio_gpu64_debug_stats_reset(void);
// M+11C: interrupt-mode summary (BDF/entry/irq/counters) via klog.
void virtio_gpu64_irq_report(void);
// Wrong-owner assertion self-check for both queues. 1 = both refused.
int  virtio_gpu64_owner_selfcheck(void);
int virtio_gpu64_wouldblock_selfcheck(void);   // 1 iff a request from a non-blockable context is refused BEFORE submission
int32_t toxenos_virtio_gpu_read_selector(uint32_t which);   // raw queue_msix_vector (0xFFFF = NO_VECTOR, -1 = none)
// M+11C test switch: masks the control entry (VIRTIO_IRQ_TEST_DROP_CTL_IRQ builds) and issues a command.
void virtio_gpu64_irq_test_drop_ctl(void);

#endif // VIRTIO_GPU64_H
