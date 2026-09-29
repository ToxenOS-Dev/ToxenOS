#ifndef DISPLAY64_H
#define DISPLAY64_H

#include <stdint.h>
#include "pixfmt64.h"
#include "memobj64.h"

// Milestone 29: the low-level display/framebuffer layer beneath
// kernel/fbterm64.c. Owns the framebuffer's physical mapping, pitch, and
// pixel format; fbterm64 is now just ONE client of this layer (a text
// renderer), not the owner of framebuffer geometry/pixel-format logic --
// a future Milestone 30 compositor is another client, going through the
// present operation (kernel/syscall64.c's SYS64_DISPLAY_PRESENT) instead
// of these direct kernel-side calls.
//
// MMIO mapping: reuses kernel/physmem64.c's physmem64_map_mmio() (the
// same cache-disabled physical direct-map extension Milestone 28's
// AHCI/NVMe/VirtIO drivers use for their controller registers) instead
// of kernel/fbterm64.c's old ad hoc low-identity-map extension -- one
// physical-mapping mechanism for the whole kernel instead of two. Like
// every other physmem64_map_mmio caller, display64_init() MUST run
// before the first process64_spawn() (see physmem64.h's ordering
// comment); it already does, since console64_init() runs during early
// boot, long before any process exists.
//
// All coordinates/rectangles are clamped to the actual display_width/
// display_height internally -- callers get silently-clipped, not
// undefined, behavior for an out-of-range rectangle, which is what lets
// fbterm64_clear()/scrolling correctly cover the true framebuffer height
// even when it is not an exact multiple of the terminal's character
// cell height (the Milestone 21-28 "bottom strip" bug).

typedef struct {
    uint32_t width, height, pitch;
    pixfmt64_t format;
} display64_info_t;

// Validates `fmt` is already-checked (see pixfmt64_from_mb2) and maps
// the framebuffer physical range via physmem64_map_mmio(). Returns 0 on
// success, -1 on failure (bad geometry, or the MMIO mapping failed) --
// on failure display64_available() reports 0 and every other call here
// is a safe no-op, mirroring the old fbterm64_available() gating so
// console64 can fall back to vgaterm64 exactly as before.
int display64_init(uint64_t fb_addr, uint32_t width, uint32_t height,
                    uint32_t pitch, const pixfmt64_t* fmt);
int display64_available(void);
void display64_get_info(display64_info_t* out);

// Writes one pixel, converting the logical 0x00RRGGBB color to the
// real hardware layout via pixfmt64_pack(). No-op if (x,y) is out of
// bounds or the display isn't available.
void display64_put_pixel(uint32_t x, uint32_t y, uint32_t rgb);

// Fills an axis-aligned rectangle with one logical color, clipped to the
// actual display bounds -- the primitive fbterm64_clear()/scrolling use
// to correctly cover the full framebuffer height.
void display64_fill_rect(uint32_t x, uint32_t y, uint32_t w, uint32_t h, uint32_t rgb);

// Moves `num_rows` full scanlines from src_y to dst_y via a raw byte
// copy (no per-pixel pack/unpack -- the bytes are already in the
// correct hardware format, only their row position changes). Used by
// fbterm64's scroll. Overlapping ranges are handled correctly (copies
// in the safe direction for an upward scroll, dst_y < src_y).
void display64_copy_rows(uint32_t dst_y, uint32_t src_y, uint32_t num_rows);

// Writes one scanline of `w` logical 0x00RRGGBB pixels (packed here,
// hardware format written out), clipped to display bounds. Used by
// SYS64_DISPLAY_PRESENT to stream a validated userspace row directly
// into the framebuffer without an intermediate full-frame kernel copy.
void display64_blit_row(uint32_t x, uint32_t y, uint32_t w, const uint32_t* logical_pixels);

// Logs width/height/pitch/bpp and the parsed RGB field positions/sizes
// via klog(). Debug use only.
void display64_dump(void);

// Reads back the raw native (hardware-format) pixel value at (x,y), or
// 0 if out of bounds/unavailable -- the read-side counterpart to
// display64_put_pixel/blit_row's writes, used by display64_selftest()
// to verify what was actually written rather than merely that nothing
// crashed. Also useful in general as a display diagnostic.
uint32_t display64_read_pixel_raw(uint32_t x, uint32_t y);

// Milestone 29: exercises fill_rect/copy_rows/clipping against the
// REAL active display via direct pixel read-back, including a
// deliberately non-CHAR_H-aligned partial rectangle (the exact shape
// kernel/fbterm64.c's scroll/clear use to cover a framebuffer whose
// height isn't an exact multiple of the terminal's 32px cell height --
// see that file's header comment on the "bottom strip" bug this fixed).
// QEMU's fixed VBE mode table only ever offered this environment a
// 1024x768x32 framebuffer (an exact multiple of 32) no matter what
// GRUB gfxmode/gfxpayload was requested, so this test proves the
// PRIMITIVE handles an arbitrary, non-aligned rectangle correctly
// directly, rather than relying on booting a genuinely different
// resolution to exercise it end-to-end. Leaves the screen cleared to
// black when done. Returns 1 if every case passed (or if no display is
// available -- vacuously, there is nothing to test).
int display64_selftest(void);

// ── M+4: generic GPU presentation backend ───────────────────────────
// display64 has always owned exactly one physical output; this adds a
// SECOND kind of backend it can drive that output through, alongside
// (never replacing the mapping of) the legacy Multiboot framebuffer
// display64_init() sets up above. display64.c itself never links
// against anything VirtIO-specific -- a backend registers a plain
// kernel-virtual pointer plus geometry, and a function pointer for
// "push this rect to the real device"; this is the exact same
// "generic core, driver owns the specifics" split Revision 2 §13
// already established between C and Rust, applied one layer up
// between generic display and a specific presentation backend.
//
// Once a GPU backend is registered, it becomes the ACTIVE backend for
// display64_available()/display64_get_info()/every pixel-writing
// primitive below -- see kernel/virtio_gpu64.c's own
// virtio_gpu64_init_compositor_backend() for why GPU presentation wins
// whenever it successfully initializes (Revision 2 §08's fallback
// policy), and kernel/kernel64.c for where that decision actually gets
// made (unconditionally, at boot, mirroring every other storage/PCI
// driver's own "always probe, no-op cleanly if absent" convention --
// this is NOT a manual debug flag). The legacy framebuffer mapping is
// never torn down or overwritten by this -- display64_acquire/release/
// owner_pid's single-owner semantics are completely unaffected by
// which backend is active, since they never touch backend state at
// all.
//
// The GPU backend is always the identity XRGB8888 layout (byte-for-
// byte the same as this codebase's logical 0x00RRGGBB color -- see
// Revision 2's own note on why VIRTIO_GPU_FORMAT_B8G8R8X8_UNORM was
// chosen for exactly this reason), so no pixfmt64_pack() conversion is
// ever needed on this path -- every primitive's GPU branch is a raw
// memory write/copy, unconditionally.
typedef int (*display64_gpu_flush_fn)(uint32_t x, uint32_t y, uint32_t w, uint32_t h);

// Registers the GPU backend. `vaddr` must be a kernel-mapped pointer to
// at least `height`*`stride` bytes (logical 0x00RRGGBB pixels,
// `stride` >= `width`*4); `flush_fn` is called by
// display64_gpu_flush_rect() below to push a rect to the real device --
// never called by this file for any other reason. Returns 0, or -1
// (bad geometry/NULL args, or a GPU backend is already registered --
// at most one per boot). Safe to call from kernel-only driver code
// only (no process context needed, matching display64_init() itself).
int display64_set_gpu_backend(uint64_t vaddr, uint32_t width, uint32_t height,
                               uint32_t stride, display64_gpu_flush_fn flush_fn);

// Called by SYS64_DISPLAY_PRESENT (kernel/syscall64.c) exactly once per
// present call, AFTER its row-copy loop has written every row of the
// requested rect via display64_blit_row() -- NOT called per row, since
// that would turn one damage-sized present into one VirtIO command per
// scanline, defeating the entire point of driving M-next's damage
// rectangles through this path (Revision 2 §05's own milestone-3
// finding on small-region overhead). A safe no-op if the active
// backend is the legacy framebuffer (blit_row's writes already landed
// directly in the physical framebuffer -- nothing further to flush) or
// if no display is available at all.
//
// M+8: returns 0 if this exact rect is now known to be part of the
// visible output, -1 if it could not be made visible at all -- the
// honest, specific meaning §1 of the M+8 spec requires ("the requested
// rectangle became part of the visible output before this call
// returned"), not merely "a fallback was enabled for next time." A safe
// 0 (trivial success) if the active backend is the legacy framebuffer
// already (blit_row's writes already landed directly in the physical
// framebuffer -- nothing further to flush) or if no GPU backend is
// registered at all.
//
// On flush_fn failure: logs once via klog(), permanently disables the
// GPU backend for the rest of this boot session (never attempts to
// re-enable it -- see this milestone's own brief against building "a
// huge GPU-reset system" for a single failed present), and -- THIS rect
// only -- synchronously mirrors its already-correct pixels (blit_row
// already wrote them into the GPU backend's own CPU-side buffer before
// this flush was ever attempted; only the DEVICE never saw them) into
// the legacy framebuffer, if one is available. That mirror write is a
// plain, synchronous MMIO store that cannot itself fail to reach the
// framebuffer, so returning 0 after it succeeds is honest by the same
// definition of SUCCESS above -- not "fallback was merely enabled," but
// "this exact rectangle is now genuinely visible." Returns -1 if no
// framebuffer fallback exists (a real, documented limitation of a
// single-VirtIO-GPU-device boot) or if the rect falls outside the
// framebuffer's own geometry.
int display64_gpu_flush_rect(uint32_t x, uint32_t y, uint32_t w, uint32_t h);

// ── M+7: generic hardware cursor backend ─────────────────────────────
// The same "generic core, driver owns the specifics" split the GPU
// presentation backend above already established, applied to the
// cursor: display64.c never links against anything VirtIO-specific, and
// a caller (compositor64, through the SYS64_CURSOR_* syscalls below)
// only ever asks for semantic operations -- set the cursor image, move
// it, show/hide it -- never anything shaped like
// VIRTIO_GPU_CMD_UPDATE_CURSOR/MOVE_CURSOR, a virtqueue descriptor, or a
// VirtIO resource ID. Those stay exclusively in
// kernel/virtio_gpu64.c/rust/toxenos_rs/src/virtio_gpu.rs.
//
// Optional, independently of the GPU presentation backend: a cursor
// backend may be registered, left unregistered (no hardware cursor
// queue, or setup failed), or -- in principle -- registered without a
// GPU presentation backend existing (not a case any current driver
// produces, but display64.c itself enforces no such coupling). Whoever
// calls display64_cursor_move()/set_image()/set_visible() without a
// backend registered gets -1 back, cheaply and safely -- exactly the
// signal compositor64 needs to keep using its own complete, unmodified
// software cursor path.
//
// Pixel format: same identity logical-to-native mapping the GPU
// backend's own framebuffer already uses, EXCEPT with alpha -- a
// logical 0xAARRGGBB pixel, alpha in the top byte, is byte-for-byte
// VIRTIO_GPU_FORMAT_B8G8R8A8_UNORM on this little-endian host (see
// include/virtio_gpu64.h's own note on that constant). display64.c
// itself never interprets the pixel bytes at all -- they pass straight
// through to whichever backend is registered.
typedef int (*display64_cursor_set_image_fn)(uint32_t width, uint32_t height, const uint32_t* argb_pixels, uint32_t hot_x, uint32_t hot_y);
typedef int (*display64_cursor_move_fn)(int32_t x, int32_t y);
typedef int (*display64_cursor_set_visible_fn)(int visible);

// Registers the three cursor operations as one backend -- all three
// non-NULL, or the call is rejected (a backend that can only do SOME of
// these isn't a coherent capability). Returns 0, or -1 (a NULL function
// pointer, or a cursor backend is already registered -- at most one per
// boot, same precedent display64_set_gpu_backend() already sets).
int display64_set_cursor_backend(display64_cursor_set_image_fn set_image_fn,
                                  display64_cursor_move_fn move_fn,
                                  display64_cursor_set_visible_fn set_visible_fn);

// Whether a cursor backend is currently registered -- what
// SYS64_CURSOR_AVAILABLE reports back to userspace.
int display64_cursor_available(void);

// Thin pass-throughs to the registered backend's own function pointers.
// Each is a safe, cheap no-op returning -1 if no backend is registered --
// callers (kernel/syscall64.c's SYS64_CURSOR_* handlers) never need to
// check display64_cursor_available() first, though compositor64 itself
// does anyway, once, at startup, to decide which cursor path to use for
// the rest of its own run.
int display64_cursor_set_image(uint32_t width, uint32_t height, const uint32_t* argb_pixels, uint32_t hot_x, uint32_t hot_y);
int display64_cursor_move(int32_t x, int32_t y);
int display64_cursor_set_visible(int visible);

// ── Milestone 29: single-owner present privilege ────────────────────
// The physical framebuffer is a privileged global resource -- no
// process maps its MMIO directly (see kernel/syscall64.c's
// SYS64_DISPLAY_PRESENT, which copies through a validated userspace
// pointer entirely inside the kernel). This is the access-control half
// of that: only one process may hold an open HANDLE64_DISPLAY handle
// at a time, exactly mirroring kernel/input64.c's single-consumer input
// policy and for the same reason -- ToxenOS has no credential system
// yet, so "first (and only) owner wins" stands in for real access
// control, while still shaping the ABI around the intended future
// model where one compositor process owns presentation.
int display64_acquire(void);
void display64_release(void);
uint32_t display64_owner_pid(void); // 0 if unowned

// ── M+9B: generic direct-scanout / composition-bypass backend ───────
// Same "generic core, driver owns the specifics" split as the GPU
// presentation and cursor backends above, applied to composition
// bypass: display64.c never learns a VirtIO resource ID or command
// name. A backend (kernel/virtio_gpu64.c) registers four operations;
// compositor64 (via the SYS64_DISPLAY_DIRECT_* syscalls below) only
// ever asks for "bind this client backing," "present it," "leave
// direct mode," "unbind." A framebuffer-only boot (no backend
// registered) makes display64_direct_scanout_supported() report 0, and
// every other call here a safe -1 -- fullscreen continues through
// normal composition unconditionally in that case
// (DIRECT_SCANOUT_UNSUPPORTED, per this milestone's own §2).
//
// Exactly one fullscreen client may ever be the ACTIVE scanout source
// at a time (this milestone's own eligibility rule), but that client's
// own buffer identity can legitimately alternate between two committed
// slots (GfxDemo double-buffers even while fullscreen, for the same
// tear-free reason it does in windowed mode) -- §16 explicitly forbids
// RESOURCE_CREATE_2D/ATTACH_BACKING/DESTROY every single frame merely
// because the client alternates which of its own two buffers it just
// committed. So this file keeps a small, fixed-size CACHE of bound
// resources (DISPLAY64_DIRECT_MAX_SLOTS, currently 2 -- one double-
// buffered pair's worth), keyed by `identity` (the client buffer's own
// shm_token, an opaque uint64_t as far as this file is concerned): a
// bind() for a token already cached is a cheap idempotent success
// (reuses the existing resource, matching §16's own "lazily create,
// keep while it exists, reuse across commits" model) rather than a
// second RESOURCE_CREATE_2D. At most one cached slot is ever the
// ACTIVE scanout source; the other stays bound (its VirtIO resource
// alive, content refreshable via present() with switch_active=0) so
// switching back to it later needs only SET_SCANOUT, never a rebind.
// The state machine per SLOT (independent of whatever compositor64
// itself believes):
//
//   (no slot for this identity)
//     -- display64_direct_scanout_bind(identity, ...) --> BOUND (not
//        yet the active primary scanout source; the backend's own
//        driver_resource_id already exists and the client's memobj64
//        backing is already attached, but VirtIO scanout 0 has not
//        been repointed yet). Calling bind() again with the SAME
//        identity and matching width/height is a no-op success.
//   BOUND (any slot)
//     -- display64_direct_present(identity, ..., switch_active=1)
//        succeeds --> that slot becomes ACTIVE; whichever OTHER slot
//        was previously ACTIVE (if any) simply becomes BOUND again --
//        still cached/alive, not torn down (§16) -- exactly one slot
//        is ACTIVE at a time.
//   BOUND or ACTIVE
//     -- display64_direct_present(identity, ..., switch_active=0)
//        succeeds --> unchanged ACTIVE/BOUND status, buffer content
//        refreshed (TRANSFER+FLUSH only, no SET_SCANOUT).
//   ACTIVE (whichever slot currently is)
//     -- display64_leave_direct_scanout() succeeds --> that slot
//        becomes BOUND (scanout 0 now points back at the normal
//        compositor resource; the slot's own resource is still bound/
//        alive -- see this milestone's own §11 on why the buffer must
//        outlive the switch-back). No slot is ACTIVE afterward.
//   BOUND (never activated, or no longer needed)
//     -- display64_direct_scanout_unbind(identity) --> that slot is
//        released (GPU resource + the memobj64 reference bind() took)
//        and freed for reuse by a future, unrelated identity.
//
// Unbinding a slot that is currently ACTIVE is refused (-1, no-op) --
// exactly the ordering enforcement this milestone's own §11 requires
// (switch scanout away FIRST, only then may that buffer be released),
// enforced once here rather than trusted to every caller. Binding a
// brand-new (uncached) identity while the cache is already full (both
// slots occupied by two OTHER identities) also fails with -1 --
// compositor64's own eligibility/ownership bookkeeping is expected to
// unbind a slot it no longer needs (e.g. a window's OLD buffer pair,
// after a resize or after that window stops being the direct-scanout
// owner) before a third identity would ever need a slot; §3's own
// "extremely conservative, exactly one fullscreen client" rule means
// this should never actually happen in the first eligible client's
// steady state.
#define DISPLAY64_DIRECT_MAX_SLOTS 2

typedef int (*display64_direct_bind_fn)(memobj64_t* backing, uint32_t width, uint32_t height, void** out_handle);
// `switch_active`: 1 if this call must also (re)point the primary
// scanout at `handle`'s own resource before flushing (the Linux/
// virtio_gpu_primary_plane_update reference this milestone's own audit
// studied only ever does this when the visible framebuffer object
// actually changed -- never unconditionally every frame); 0 to just
// push updated pixel content through an already-bound resource.
typedef int (*display64_direct_present_fn)(void* handle, uint32_t width, uint32_t height,
                                            uint32_t x, uint32_t y, uint32_t w, uint32_t h,
                                            int switch_active);
typedef int (*display64_direct_leave_fn)(void);
typedef void (*display64_direct_unbind_fn)(void* handle);

// Registers the four direct-scanout operations as one backend -- all
// four non-NULL, or the call is rejected, same coherent-capability
// requirement display64_set_cursor_backend() already enforces. Returns
// 0, or -1 (a NULL function pointer, or a backend is already
// registered).
int display64_set_direct_scanout_backend(display64_direct_bind_fn bind_fn,
                                          display64_direct_present_fn present_fn,
                                          display64_direct_leave_fn leave_fn,
                                          display64_direct_unbind_fn unbind_fn);

// Whether a direct-scanout backend is registered AT ALL -- what
// SYS64_DISPLAY_DIRECT_QUERY reports. This is capability, not current
// state (a registered backend can still have zero bound slots);
// compositor64's own eligibility check (§3) is layered entirely on top
// of this.
int display64_direct_scanout_supported(void);

// Binds `backing` (typically shm64_find_by_token(token)->obj for a
// client's committed buffer) as a direct-scanout resource sized
// width x height, cached under `identity` (the same shm_token). A
// second bind() with an identity already cached (matching width/
// height) is a cheap no-op success reusing the existing resource --
// see this header's own cache comment above. Fails (-1) if no backend
// is registered, the identity is new AND the cache is already full, or
// the backend's own bind_fn fails (resource creation/backing-attach
// failure) -- nothing is left bound on any failure path. `out_handle`
// is optional (NULL-able) -- when given, *out_handle is set for fault-
// injection/debug identity comparison only; compositor64 must treat it
// as opaque and never dereference it (indeed, ToxenOS's own
// SYS64_DISPLAY_DIRECT_BIND never even passes it back across the
// syscall boundary at all -- there is nothing a caller could
// legitimately do with a raw kernel pointer).
int display64_direct_scanout_bind(uint64_t identity, memobj64_t* backing, uint32_t width, uint32_t height, void** out_handle);

// Presents the resource cached under `identity` (which must already be
// bound) at its own already-remembered width/height. Returns 0 only if
// the pixels named by (x,y,w,h) are now genuinely part of the visible
// output (same honest-SUCCESS contract display64_gpu_flush_rect()
// already established for M+8) -- -1 on ANY failure (no backend, this
// identity isn't bound, or the backend's own present_fn failed at any
// VirtIO command in its sequence). A failure leaves the state machine
// exactly where it was before the call: if `switch_active` was
// requested and failed, the CURRENTLY active source (composited or a
// prior direct buffer/slot) remains authoritative -- this function
// never claims a switch happened when it didn't.
int display64_direct_present(uint64_t identity, uint32_t x, uint32_t y, uint32_t w, uint32_t h, int switch_active);

// Switches the primary scanout back to the normal compositor
// resource and flushes the full frame -- the ONLY sanctioned way out
// of an ACTIVE slot. Returns 0 once the compositor's own resource is
// confirmed genuinely visible again, or -1 (no backend, no slot
// currently ACTIVE, or the switch-back/flush itself failed -- in which
// case the old direct slot remains ACTIVE and alive, never torn down
// on a failed leave).
int display64_leave_direct_scanout(void);

// Releases the slot cached under `identity`. Refused (a no-op, does
// not crash) if that slot is currently ACTIVE -- see this header's own
// state-machine comment above for why. Safe to call for an identity
// that isn't cached at all (no-op).
void display64_direct_scanout_unbind(uint64_t identity);

// ── M+9B debug fault injection (§17) ─────────────────────────────────
// Deterministically forces the NEXT call to the named stage to fail,
// exactly once, then clears itself -- same one-shot-arm-and-consume
// discipline M+8's own m8_fault_inject_arm() established, reused here
// rather than inventing a second mechanism. Gated behind
// M9B_FAULT_INJECT at compile time; the enum is still declared
// unconditionally so callers compile either way (each injection point
// below checks display64_direct_fault_should_fail() itself, which is a
// permanent no-op returning 0 when the macro is undefined).
//
// M+10 reuses this exact mechanism (still gated behind M9B_FAULT_INJECT --
// one fault-injection knob for the whole display stack, not a second one)
// and extends the point list with the cursor/rollback stages its own
// atomic commit path can independently fail at.
typedef enum {
    M9B_FAULT_NONE = 0,
    M9B_FAULT_BIND,             // memobj/gpu64_buffer_wrap or resource-create/attach-backing
    M9B_FAULT_TRANSFER,         // TRANSFER_TO_HOST_2D
    M9B_FAULT_SET_SCANOUT,      // SET_SCANOUT
    M9B_FAULT_RESOURCE_FLUSH,   // RESOURCE_FLUSH
    M9B_FAULT_CURSOR_TRANSFER,  // M+10: cursor image bytes -> cursor resource
    M9B_FAULT_CURSOR_UPDATE,    // M+10: UPDATE_CURSOR
    M9B_FAULT_CURSOR_MOVE,      // M+10: MOVE_CURSOR
    M9B_FAULT_ROLLBACK,         // M+10: the recovery-commit's own SET_SCANOUT/RESOURCE_FLUSH, forced to fail to exercise RECOVERY_REQUIRED
} m9b_fault_point_t;
// M+10 fault-injection campaign: arming is now a BITMASK, not a single
// value -- required to test compound scenarios (a partial commit
// failure whose OWN recovery attempt must ALSO be forced to fail,
// exercising RECOVERY_REQUIRED) where two INDEPENDENT points each need
// their own one-shot arm active at the same time. Arming the same point
// twice before it fires is a harmless no-op (the bit is already set).
void display64_direct_fault_inject_arm(m9b_fault_point_t point);
int display64_direct_fault_should_fail(m9b_fault_point_t point); // consumes (clears) only THIS point's own bit if armed
void display64_direct_fault_inject_clear_all(void); // test-harness reset between scenarios -- clears every armed bit without requiring each to fire first

// ── M+10: atomic display state ───────────────────────────────────────
// See docs/m10_linux_vs_toxenos_audit.md and
// docs/m10_current_mutation_audit.md for the full design rationale --
// summary here.
//
// Every display64 mutation before M+10 (cursor move/image/visibility,
// direct-scanout present/leave) was an independent, immediately-applied
// operation: no shared validate-before-mutate stage, no way to express
// "primary + cursor change together" as one all-or-nothing unit, and no
// way to tell "nothing happened" apart from "some hardware commands
// landed before a later one failed" (the current mutation audit's own
// top finding). This section adds a small, ToxenOS-native atomic layer
// ABOVE the existing GPU-presentation backend (content path, UNCHANGED --
// see §16 below) and the existing direct-scanout resource cache
// (bind/unbind resource EXISTENCE, UNCHANGED -- a candidate state only
// ever NAMES an already-bound identity, it never binds/unbinds one
// itself).
//
// Deliberately NOT a Linux DRM/KMS clone: one fixed primary, one fixed
// cursor, one fixed output -- no object graph, no property blobs, no
// connector/CRTC IDs, no userspace atomic-commit syscall (the existing
// SYS64_CURSOR_*/SYS64_DISPLAY_DIRECT_* syscalls are UNCHANGED; this
// model lives entirely inside kernel/display64.c + kernel/virtio_gpu64.c,
// each existing public function below internally does begin+mutate+
// check+commit in one synchronous call -- see this header's own
// per-function comments).
typedef enum {
    DISPLAY64_PRIMARY_COMPOSITED = 0, // the compositor's own always-live primary resource
    DISPLAY64_PRIMARY_DIRECT,          // a client-backed resource, already bound in the direct-scanout cache, named by identity
} display64_primary_kind_t;

typedef struct {
    display64_primary_kind_t kind;
    uint64_t identity;       // DIRECT only -- the direct-scanout cache's own identity (shm_token). 0 for COMPOSITED.
    memobj64_t* backing;     // DIRECT only -- THIS CANDIDATE's own reference (see §4), independent of the cache's own. NULL for COMPOSITED.
    void* backend_handle;    // DIRECT only -- the SAME opaque handle the backend's own bind returned for this identity (display64_state_set_primary_direct() copies it out of the cache) -- lets atomic_commit_fn issue commands against this resource without display64.c exposing its private slot cache. Opaque to display64.c itself; never dereferenced here.
    uint32_t width, height;  // DIRECT only -- must match the cached slot's own recorded geometry
    // Content-update-only fields (§16): a transaction whose kind/identity
    // are UNCHANGED from current but names a nonzero-area damage rect is
    // a CONTENT update on the already-selected primary (transfer+flush
    // only, no SET_SCANOUT) -- never meaningful together with a kind/
    // identity CHANGE in the same transaction (a resource change always
    // transfers the FULL new resource regardless of these fields).
    uint32_t damage_x, damage_y, damage_w, damage_h;
    int has_damage;
} display64_primary_state_t;

typedef struct {
    int enabled;
    // image_generation == 0 means "same image as current" (no transfer
    // needed this commit). Only display64_state_set_cursor_image() may
    // set these three fields to a nonzero generation + real pixels --
    // never assign them directly.
    uint32_t image_generation;
    uint32_t img_width, img_height;
    const uint32_t* argb_pixels; // borrowed for the duration of check()/commit() only -- the backend copies bytes synchronously; never retained past commit() returning, never touched again afterward
    uint32_t hot_x, hot_y;
    int32_t x, y;
} display64_cursor_state_t;

typedef struct {
    uint32_t width, height; // the one mode this milestone supports -- see display64_state_check()'s own §17 MODE_UNSUPPORTED rule
} display64_output_state_t;

typedef struct {
    display64_primary_state_t primary;
    display64_cursor_state_t  cursor;
    display64_output_state_t  output;
} display64_state_t;

// Delta categories (§8) -- a bitmask computed by comparing a candidate
// against current, telling the backend's own atomic_commit exactly which
// VirtIO command groups are actually required. A no-change transaction
// computes DISPLAY64_DELTA_NONE and issues zero hardware commands.
typedef enum {
    DISPLAY64_DELTA_NONE              = 0,
    DISPLAY64_DELTA_PRIMARY_CHANGED   = 1 << 0, // primary.kind or .identity differs from current -- transfer + SET_SCANOUT + flush
    DISPLAY64_DELTA_PRIMARY_CONTENT   = 1 << 1, // same primary, has_damage -- transfer + flush only, no SET_SCANOUT
    DISPLAY64_DELTA_CURSOR_IMAGE      = 1 << 2, // image_generation differs from current -- transfer + UPDATE_CURSOR
    DISPLAY64_DELTA_CURSOR_POSITION   = 1 << 3, // x/y differ from current, image/visibility unchanged -- MOVE_CURSOR only
    DISPLAY64_DELTA_CURSOR_VISIBILITY = 1 << 4, // enabled differs from current
    DISPLAY64_DELTA_MODE_CHANGED      = 1 << 5, // output width/height differs from current -- always rejected by check() today, see §17
} display64_delta_t;

// §5: possible display64_state_check() outcomes -- always zero side
// effects regardless of which of these is returned.
typedef enum {
    DISPLAY64_CHECK_OK                 = 0,
    DISPLAY64_CHECK_NO_BACKEND         = -1, // requested primary/cursor kind needs a backend that isn't registered
    DISPLAY64_CHECK_BAD_PRIMARY        = -2, // DIRECT identity not bound in the cache, or geometry mismatch
    DISPLAY64_CHECK_BAD_CURSOR         = -3, // unsupported size/format/hotspot/coordinates, or no cursor backend
    DISPLAY64_CHECK_MODE_UNSUPPORTED   = -4, // §17: any actual mode CHANGE is rejected outright this milestone
    DISPLAY64_CHECK_CURSOR_CONFLICT    = -5, // software-cursor policy cannot coexist with the requested primary path
    DISPLAY64_CHECK_RECOVERY_REQUIRED  = -6, // §11: current is DISPLAY64_STATE_RECOVERY_REQUIRED -- every ordinary transaction is refused until display64_state_retry_recovery() succeeds; this is the ONLY sanctioned way out
} display64_check_result_t;

// §10/§11: the two truthful post-commit outcomes.
typedef enum {
    DISPLAY64_STATE_VALID = 0,             // current is authoritative and matches real hardware
    DISPLAY64_STATE_RECOVERY_REQUIRED = 1, // a commit failed mid-sequence AND recovery to a known composited state also failed -- see display64_state_commit()'s own comment
} display64_validity_t;

// Backend result for one atomic_commit_fn call (§10) -- NOT the same as
// display64_state_commit()'s own return value; this is what the BACKEND
// reports about hardware progress, which display64.c's own commit logic
// uses to decide VALID vs RECOVERY_REQUIRED.
typedef enum {
    DISPLAY64_COMMIT_OK           = 0,  // every required command succeeded
    DISPLAY64_COMMIT_FAIL_CLEAN   = -1, // failed before altering any visible hardware state (e.g. the first command of the whole delta failed) -- current remains authoritative as-is, no recovery needed
    DISPLAY64_COMMIT_FAIL_PARTIAL = -2, // one or more hardware commands already landed before a later one in the SAME delta failed -- display64.c must attempt recovery (see display64_state_commit())
} display64_commit_result_t;

// Duplicates CURRENT into *out, taking a fresh reference (§4) on a DIRECT
// primary's backing if current has one. The caller mutates *out directly
// via the setters below (or its own field writes for cursor position/
// visibility/output, which need no reference bookkeeping). Must be
// followed by exactly one of display64_state_commit()/discard() --
// never left dangling (its reference would leak).
void display64_state_begin(display64_state_t* out);

// Sets `state->cursor` to a NEW image -- the only correct way to
// populate image_generation/img_width/img_height/argb_pixels (bumps
// image_generation so check()/commit() can tell this candidate actually
// changes the image, vs merely re-describing the current one unchanged).
void display64_state_set_cursor_image(display64_state_t* state, uint32_t width, uint32_t height,
                                       const uint32_t* argb_pixels, uint32_t hot_x, uint32_t hot_y);

// Sets `state->primary` to the DIRECT resource already bound under
// `identity` in the direct-scanout cache (this does NOT bind -- call
// display64_direct_scanout_bind() first, exactly as before). Takes this
// candidate's own reference on the cached slot's backing. Returns -1
// (state left unchanged) if `identity` isn't currently bound.
int display64_state_set_primary_direct(display64_state_t* state, uint64_t identity, uint32_t width, uint32_t height);

// Sets `state->primary` back to the compositor's own resource, releasing
// any DIRECT reference this candidate was previously holding.
void display64_state_set_primary_composited(display64_state_t* state);

// Marks a content-only damage rect against the primary `state` ALREADY
// names (§16) -- meaningless combined with a kind/identity change in the
// same transaction (see display64_primary_state_t's own comment).
void display64_state_set_primary_damage(display64_state_t* state, uint32_t x, uint32_t y, uint32_t w, uint32_t h);

// §5: pure validation against current display64 state and registered
// backend capability. Issues NO VirtIO commands, alters NO scanout/
// cursor/resource ownership, sends NO BUFFER_RELEASED, mutates NO
// current state -- regardless of outcome. Returns DISPLAY64_CHECK_OK or
// one of the failure codes above.
display64_check_result_t display64_state_check(const display64_state_t* candidate);

// §6/§7/§8/§9/§10: computes the delta against current, issues ONLY the
// commands the delta requires (via the registered atomic backend), and
// on success adopts `candidate` as the new current (references transfer
// -- do not call display64_state_discard() on a candidate after a
// successful commit). Internally re-validates via display64_state_check()
// first (a commit is never attempted against an invalid candidate).
//
// On check failure: candidate is discarded internally, current is
// completely unchanged, returns -1. Query nothing further -- this is
// outcome "never attempted," not a hardware failure.
//
// On DISPLAY64_COMMIT_FAIL_CLEAN: current is unchanged (truthfully --
// nothing on the device changed), candidate is discarded, returns -1.
//
// On DISPLAY64_COMMIT_FAIL_PARTIAL: hardware may now be inconsistent with
// any known software state. display64.c attempts ONE recovery commit
// (primary = composited, cursor/output left as current's own, forcing a
// full primary reprogram) against the backend:
//   - recovery succeeds: hardware is NOW known-composited; that recovered
//     state becomes the new current, display64_state_get_validity()
//     reports VALID -- but this function still returns -1, since the
//     ORIGINAL requested transaction did not happen and the caller must
//     know that.
//   - recovery itself fails: display64_state_get_validity() reports
//     DISPLAY64_STATE_RECOVERY_REQUIRED. No resource still possibly
//     referenced by hardware is released in this case (see
//     display64_state_discard()'s own comment) until a later successful
//     recovery.
// Returns 0 on success, -1 on any failure (check display64_state_get_validity()).
int display64_state_commit(display64_state_t* candidate);

// Releases any reference `candidate` holds (a DIRECT primary's memobj64
// reference) WITHOUT committing. Call on any begin()'d candidate that
// will never be passed to display64_state_commit() (an aborted
// transaction, or one that failed display64_state_check() before the
// caller even tries to commit it).
void display64_state_discard(display64_state_t* candidate);

// Read-only copy of current -- does NOT take any reference (the copy's
// own `primary.backing` is a borrowed, informational pointer only; never
// pass this copy to display64_state_commit()/discard()).
void display64_state_get_current(display64_state_t* out);

display64_validity_t display64_state_get_validity(void);

// §11: the ONLY sanctioned way out of DISPLAY64_STATE_RECOVERY_REQUIRED.
// While validity is RECOVERY_REQUIRED, display64_state_check() refuses
// every ordinary transaction (DISPLAY64_CHECK_RECOVERY_REQUIRED) rather
// than letting a caller proceed as if hardware were in a known state --
// this is the explicit "next known-good recovery attempt" the M+10
// fault-injection campaign requires. Attempts the same full-composited
// recovery display64_state_commit() itself runs after a FAIL_PARTIAL;
// on success, current becomes that known-good composited state and
// validity returns to VALID; on failure, validity stays
// RECOVERY_REQUIRED (safe to call again later -- idempotent either way,
// no resource is released or current touched on a failed attempt).
// Returns 0 on success, -1 on failure. A no-op success (returns 0
// immediately) if validity is already VALID -- nothing to recover from.
int display64_state_retry_recovery(void);

// Registers the atomic backend -- both non-NULL, or the call is
// rejected, same coherent-capability rule every other display64 backend
// registration already enforces. At most one per boot.
typedef display64_check_result_t (*display64_atomic_check_fn)(const display64_state_t* old_state, const display64_state_t* new_state, display64_delta_t delta);
typedef display64_commit_result_t (*display64_atomic_commit_fn)(const display64_state_t* old_state, const display64_state_t* new_state, display64_delta_t delta);
int display64_set_atomic_backend(display64_atomic_check_fn check_fn, display64_atomic_commit_fn commit_fn);

// Runs the M+10 state-machine self-tests (§21, items A-L) against
// whatever backend (VirtIO atomic, or none -- the software-framebuffer
// fallback path, §19) is actually registered this boot. Logs each case
// and a final tally via klog(). Returns 1 if every case passed.
int display64_atomic_selftest(void);

// Permanent, always-cheap diagnostics -- what actually happened on the
// last display64_state_commit() call (OK, or exactly which failure
// classification), and how many recovery attempts have run/succeeded
// since boot. Not fault-injection-gated: costs nothing, and answers
// "what really happened" for any future caller (a self-test, or a
// compositor64 debug command), not just the commit's own 0/-1 return.
display64_commit_result_t display64_state_get_last_commit_result(void);
uint64_t display64_state_get_recovery_attempts(void);
uint64_t display64_state_get_recovery_successes(void);

#endif // DISPLAY64_H
