# ToxenOS GPU/Display Architecture

Status: current through **M+9B** (direct scanout / composition bypass) and the
post-M+9B wmclient64 message-demux fix. This document is the audit trail for
future milestones (starting with M+10, atomic display state) — read it before
changing any display64/compositor64/virtio_gpu64 code.

## 1. Layering

```
user64/gfx_demo64.c, user64/gfx_interactive64.c   (WM clients)
        │  wmclient64.h (shared client library: connect, buffers, fullscreen, events)
        ▼
user64/compositor64.c                              (the one process that owns display64)
        │  SYS64_DISPLAY_*, SYS64_CURSOR_*, SYS64_DISPLAY_DIRECT_*  (kernel/syscall64.c)
        ▼
kernel/display64.c                                  (generic display core — no VirtIO knowledge)
        │  three independently-registered backend interfaces:
        │    - GPU presentation backend   (display64_set_gpu_backend)
        │    - cursor backend             (display64_set_cursor_backend)
        │    - direct-scanout backend     (display64_set_direct_scanout_backend)
        ▼
kernel/virtio_gpu64.c                                (the only file that knows VirtIO GPU commands)
        │  gpu64_buffer_wrap/release (kernel/gpu64.c) — memobj64 ⇄ VirtIO resource glue
        ▼
rust/toxenos_rs/src/virtio_gpu.rs                    (virtqueue transport, command encoding)
```

`display64.c` never links against anything VirtIO-specific. Every backend is
a plain function-pointer table registered once at boot; `display64.c` is
generic core, the backend owns the specifics. This split predates M+9B (the
GPU presentation and cursor backends are M+4/M+7) and M+9B's direct-scanout
backend follows the identical pattern.

A framebuffer-only boot (no VirtIO GPU) still works: `display64_init()`'s
legacy Multiboot framebuffer path is untouched by any of this, and every
backend query (`display64_direct_scanout_supported()`,
`display64_cursor_available()`) safely reports "unsupported" rather than
crashing or assuming a backend exists.

## 2. Single-owner presentation privilege

`display64_acquire()`/`release()`/`owner_pid()` (Milestone 29) enforce that
at most one process holds the display handle at a time — this is
compositor64's own exclusive access to the physical output, not a
per-backend concept. It predates and is orthogonal to everything below.

## 3. The three display64 backends

### 3.1 GPU presentation backend (M+4)

`display64_set_gpu_backend(vaddr, width, height, stride, flush_fn)`. Once
registered, it becomes the active backend for every pixel-writing primitive
(`display64_put_pixel`, `fill_rect`, `blit_row`, …) — writes land in a
CPU-side buffer at `vaddr`; `display64_gpu_flush_rect()` pushes a rect to the
real device via `flush_fn`. On flush failure, the backend is permanently
disabled for the rest of the boot session and this one rect is mirrored into
the legacy framebuffer if available (no GPU-reset machinery — a deliberate
M+4 scope limit).

`kernel/virtio_gpu64.c`'s `virtio_gpu64_init_compositor_backend()` registers
this backend at boot, unconditionally probed like every other PCI driver.

### 3.2 Cursor backend (M+7)

`display64_set_cursor_backend(set_image_fn, move_fn, set_visible_fn)`.
Independent of the GPU presentation backend (decoupled registration, though
in practice both come from `virtio_gpu64.c`). compositor64 calls
`display64_cursor_available()` once at startup and picks ONE cursor path for
its entire run: hardware cursor via this backend, or its own complete
software-composited cursor (drawn into the backbuffer like any other damage).
The two paths are never mixed at runtime.

Pixel format: `0xAARRGGBB` logical, alpha in the top byte — matches
`VIRTIO_GPU_FORMAT_B8G8R8A8_UNORM` byte-for-byte on this little-endian host.

### 3.3 Direct-scanout backend (M+9B)

Covered in detail in §5 below — this is the newest, most complex backend.

## 4. M+9A — generic compositor fullscreen

Protocol (`user64/wmproto64.h`): `WM_MSG_SET_FULLSCREEN` (client→compositor,
`pressed`=want-fullscreen) replies with exactly one `WM_MSG_CONFIGURE`
(`w`,`h`,`pressed`=authoritative current fullscreen state) before the
transaction's own `WM_MSG_ACK`.

Compositor-side (`user64/compositor64.c`, `WM_MSG_SET_FULLSCREEN` handler):
entering fullscreen saves the window's pre-fullscreen geometry
(`saved_x/y/content_w/content_h`), sets `w->is_fullscreen=1`, resizes to the
full display, raises + focuses + activates the window. Exiting restores the
saved geometry exactly (never re-derived). This is a *per-window* boolean
(`window_t.is_fullscreen`) — there is no global "the fullscreen window"
singleton anywhere in the compositor.

This feature is generic WM protocol, not GfxDemo-specific: both
`gfx_demo64.c` and `gfx_interactive64.c` bind F11 to it via the exact same
`wm_set_fullscreen()` client-library call
(`user64/wmclient64.h`). Fullscreen composited rendering works completely
independently of direct scanout (M+9B) — a client can be fullscreen and
purely composited forever if direct scanout is ineligible or unsupported.

## 5. M+9B — direct scanout / composition bypass

**Goal**: let an eligible fullscreen client's committed buffer become the
primary VirtIO-GPU scanout source directly, bypassing compositor64's
`g_backbuffer`/`g_frontbuffer` composition entirely for that client's
frames, while composited fullscreen (§4) remains the permanent, always-
available fallback. This is explicitly *not* claimed as "zero-copy" beyond
what the debug counters actually prove (steady-state direct-mode compositor/
front-mirror byte counts are 0 — the client→GPU VirtIO transfer itself is
real, measured cost, reported separately).

### 5.1 Eligibility (`direct_scanout_eligible()`, compositor64.c)

Re-checked on every commit of a fullscreen window. All of:
- a direct-scanout backend is registered at all (`display64_direct_scanout_supported()`)
- the window is fullscreen
- it has a committed buffer
- the buffer's dimensions exactly match the display (no scaling/rotation)
- the window is topmost (nothing above it in z-order)
- the *software* cursor path is not active (hardware cursor active, or no
  cursor at all, is compatible; software cursor forces composited fallback
  since it needs to be drawn into the composited scene)

Ineligibility (at entry, or discovered on a later commit) is never fatal —
the caller falls through to the ordinary composited commit/damage path.
**Direct-eligibility failure must never prevent fullscreen itself** — this
was explicitly re-verified this cycle by code audit of the
`WM_MSG_COMMIT_BUFFER` handler: `direct_scanout_try_enter_or_update()`
returning 0 always falls through to normal `damage_content_rect()`/release
handling; the client's ACK is sent unconditionally before the direct attempt
ever runs.

### 5.2 The 2-slot resource cache (`kernel/display64.c`)

`DISPLAY64_DIRECT_MAX_SLOTS = 2` — sized for exactly one double-buffered
fullscreen client's own pair (GfxDemo alternates two buffers even while
fullscreen, for the same tear-free reason it does in windowed mode; §16 of
the original M+9B spec explicitly forbids `RESOURCE_CREATE_2D`/`DESTROY`
every frame just because the client alternates buffers). Slots are keyed by
`identity` (the client buffer's own `shm_token`, opaque to display64.c), with
per-slot states `SLOT_EMPTY` / `SLOT_BOUND` / `SLOT_ACTIVE`. See
`include/display64.h`'s own M+9B section for the exact state-machine
transition table (bind → BOUND, present(switch_active=1) → ACTIVE, leave →
BOUND, unbind → EMPTY). Idempotent re-bind of an already-cached identity is
a cheap no-op (reuses the resource) rather than a second
`RESOURCE_CREATE_2D`.

**Fixed this cycle**: `display64_leave_direct_scanout()` only ever demoted
the currently-ACTIVE slot back to BOUND; nothing unbound a window's *other*
cached-but-inactive slot (its non-active alternation buffer) on exit. A
double-buffering client leaked exactly one cache slot per fullscreen
session, eventually exhausting the 2-slot cache. Fixed in
`user64/compositor64.c`'s `direct_scanout_force_leave()`: after releasing
the active slot, it now sweeps every buffer the window owns and defensively
unbinds each one's token too (`display64_direct_scanout_unbind()` is already
a safe no-op for an uncached or still-active token). Verified via a 130-cycle
bidirectional regression (both windows toggling fullscreen in both starting
orders) with zero leaked slots.

### 5.3 VirtIO command ordering (`kernel/virtio_gpu64.c`)

Matches the Linux `virtio_gpu_primary_plane_update()` reference exactly:
`TRANSFER_TO_HOST_2D` (damage rect) → `SET_SCANOUT` *only if the active
resource actually changed* → `RESOURCE_FLUSH` unconditionally. Never a blind
`SET_SCANOUT` every frame.

### 5.4 Presented-state model (`user64/compositor64.c`)

`presented_mode_t { PRESENTED_COMPOSITED, PRESENTED_DIRECT }` plus
`g_direct_window_idx` (the ARRAY INDEX, not `window_id`, of the current
direct-scanout owner). `g_frontbuffer` never mirrors direct frames.

Buffer ownership state machine per commit: client-writable → compositor-
owned (`owned_by_compositor`/`committed_slot`) → display-scanout-owned
(`scanout_owned_slot`). A buffer is never released before hardware is
confirmed done with it (`display64_direct_present`/`leave` success is the
only trigger for `notify_buffer_released()` on that path).

**Fixed this cycle** (two bugs found via manual/automated regression before
this session's cross-window work):
1. First-entry release: the original code only released a commit's
   `old_slot` when it equaled `scanout_owned_slot`, which is false on the
   very first transition into direct mode (nothing is scanout-owned yet) —
   permanently skipped releasing the last composited buffer on every
   fullscreen entry. Fixed: release `old_slot` unconditionally whenever
   `old_slot != slot`.
2. Duplicate release on exit: `direct_scanout_force_leave()`'s own release
   left `w->committed_slot` stale, so the client's next ordinary commit
   named the same already-released slot as `old_slot` and released it a
   second time, slowly filling the client's own `pending[]`/event queue
   over repeated toggle cycles. Fixed with `window_t.last_scanout_released_slot`,
   a one-shot marker consumed by the normal `WM_MSG_COMMIT_BUFFER` release
   check.

### 5.5 Transitions

**COMPOSITED → DIRECT** (`direct_scanout_try_enter_or_update()`): bind →
present(switch_active=1) → on success, mark `PRESENTED_DIRECT`, release the
old (composited) buffer, record `scanout_owned_slot`. Any failure at bind or
present leaves state exactly as it was (no partial transition) and falls
through to composited handling for this commit.

**DIRECT → COMPOSITED** (`direct_scanout_force_leave()`, the *only*
sanctioned way out of `PRESENTED_DIRECT`): force a full composited redraw of
the current scene into the backbuffer and present it through the *ordinary*
path first (must succeed before scanout switches back) → 
`display64_leave_direct_scanout()` (switches VirtIO scanout back, flushes) →
only on success, mark `PRESENTED_COMPOSITED` and release the direct buffer +
sweep any other cached slots (§5.2). A failure at either step leaves the old
direct buffer alive and visible — never a half-transitioned state.

Called from every exit point M+9B identified: a failed eligibility recheck
on the next commit, `raise_window()` raising a different window,
`close_window()` on the direct-owned window, hardware cursor becoming
unavailable, and `WM_MSG_SET_FULLSCREEN`'s own exit branch.

### 5.6 Fault injection (§17 of the original M+9B spec)

`M9B_FAULT_INJECT`-gated one-shot fault points (`m9b_fault_point_t`:
`BIND`/`TRANSFER`/`SET_SCANOUT`/`RESOURCE_FLUSH`) exist in
`include/display64.h`/`kernel/display64.c` but are compiled out by default
and have not yet been exercised by an automated test in this codebase — flag
this as an open item for M+10 (which has its own, larger fault-injection
requirement).

## 6. wmclient64.h — client-side message demux (post-M+9B fix)

Not display64/virtio_gpu64 architecture per se, but load-bearing for
every M+9A/M+9B manual test path (F11 delivery, fullscreen toggling) and
worth recording here since it was found and fixed via the same investigation.

**Bug found**: a physical-input trace proved keyboard64 and compositor64
both delivered a real F11 press correctly, but GfxDemo's own client-side
`wm_poll_key()` never observed it — traced to `wmclient64.h`'s per-wait-
helper `pending[]` queue silently dropping messages once its small fixed
capacity filled (no error, log, or backpressure), triggered by a burst of
`WM_MSG_KEY_EVENT` messages arriving while `wm_wait_frame()` was blocked.

**Fix** (now the permanent architecture, not temporary instrumentation):
- One shared receive/demux path: `wmc_recv_classified()` (the only place a
  message is read off the wire; applies keycode-repeat suppression) and
  `wmc_classify_or_queue()` (the only place an unclaimed message is
  disposed of), used by every wait helper (`wm_wait_frame`,
  `wm_wait_buffer_released`, `wmc_recv_expecting`, `wm_wait_event`) instead
  of four independently-reimplemented drain loops.
- `WM_MSG_KEY_EVENT` lives in its own ordered ring (`key_ring[]`,
  `WMC_KEY_RING_MAX=32`), completely separate from the generic `pending[]`
  used for FRAME/BUFFER_RELEASED/CONFIGURE/etc. — an earlier attempt at this
  fix coalesced KEY_EVENT into one press-slot + one release-slot in the
  generic queue, which was **wrong**: it destroyed ordering for any
  multi-key sequence (W+D, Ctrl+C, Shift+A). The ring preserves strict
  arrival order for everything except a repeated MAKE for an
  already-logically-down key (real typematic repeat), which is suppressed
  at the single read chokepoint before it ever reaches any queue.
- `WM_MSG_FOCUS` is coalesced state (one slot, newest wins) since no client
  needs gain/loss history. `WM_MSG_ACK` is discarded (no consumer anywhere
  in the library). Every other type is queued, never dropped; the one
  remaining overflow path (should never trigger under this protocol's real
  traffic) evicts the oldest entry rather than silently discarding the
  newest arrival.
- Verified via synthetic ordering tests (W+D, Ctrl+C, Shift+A, rapid
  distinct letters, held-key repeat, F11) showing exact delivery order, plus
  a heavy synthetic stress battery with zero message loss.

## 7. Known limitations / open items for M+10

- No occlusion/visibility-aware frame-callback gating exists
  (`dispatch_frame_callbacks()`'s own comment documents this as deliberate,
  not-built-speculatively policy) — every window with a pending frame request
  gets granted regardless of whether it's actually visible.
- M+9B fault injection points exist but are untested by automation.
- Mode setting is entirely fixed (`g_disp_w`/`g_disp_h` set once at boot from
  the VirtIO GPU's own reported scanout geometry) — no resolution-change path
  exists anywhere in this stack.
- Every current display64 mutation (GPU flush, cursor set/move/visible,
  direct-scanout bind/present/leave/unbind) is an independent, immediately-
  applied operation with no shared validate-before-mutate stage and no
  atomic multi-field commit — exactly the gap M+10 (atomic display state)
  is scoped to close. See M+10's own required audit (its §1) for the
  itemized list of every current mutation path this document's §3 describes
  at the architecture level.
