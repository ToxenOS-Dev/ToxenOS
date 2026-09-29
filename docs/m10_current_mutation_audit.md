# M+10 §1 — Audit of Every Current display64 State Mutation Path

For each current API, in the order the M+10 spec's §1 groups them: what live
software state changes, what hardware/VirtIO command is issued, whether
validation occurs first, what happens on failure, and whether another
subsystem can observe an intermediate state.

## PRIMARY OUTPUT

### 1. Normal compositor framebuffer presentation
`user64/compositor64.c: present_rect()` → `SYS64_DISPLAY_PRESENT`
(`kernel/syscall64.c`) → `display64_blit_row()` per row → 
`display64_gpu_flush_rect()` once for the whole rect.

- **Software state changed**: none in display64 itself — `blit_row()` writes
  directly into the GPU backend's own CPU-side buffer (`gpu_base`) or the
  legacy framebuffer. There is no "pending" vs "current" distinction; the
  write *is* the mutation.
- **Hardware command**: `TRANSFER_TO_HOST_2D` + `RESOURCE_FLUSH` for the
  damaged rect only (inside `virtio_gpu64.c`'s flush_fn), against whichever
  resource is CURRENTLY the compositor's own primary — no `SET_SCANOUT`
  (this path never changes which resource is scanned out).
- **Validation before mutation**: none. `blit_row()` clips to backend bounds
  and returns silently if out of range; there is no separate "would this
  succeed" check before the CPU-side write happens.
- **On failure**: `display64_gpu_flush_rect()` failure permanently disables
  the GPU backend for the rest of the boot session (`gpu_disabled_after_failure`)
  and mirrors this one rect into the legacy framebuffer as a fallback. This
  is a real, intentional M+4 policy — not a bug — but it means one flush
  failure changes ALL FUTURE mutation behavior (every subsequent present
  goes through the framebuffer-mirror path), a global side effect this
  single rect's own "success/failure" return doesn't fully convey to the
  caller.
- **Intermediate-state observability**: `blit_row()`'s CPU-side write and
  the VirtIO flush are two separate steps with no lock between them. Between
  them, `gpu_base`'s CPU-side buffer has NEW pixels but the actual scanout
  resource still shows OLD pixels — genuinely correct (nothing else reads
  `gpu_base` directly), but it is a real gap between "software state
  changed" and "hardware reflects it" that M+10's check/commit split must
  preserve semantically (content update, not configuration change — see
  M+10 §16).

### 2. Direct scanout bind
`display64_direct_scanout_bind()` → `virtio_gpu64.c`'s `direct_bind_impl`
→ `gpu64_buffer_wrap()` (memobj64 → VirtIO resource) → `RESOURCE_CREATE_2D`
+ `RESOURCE_ATTACH_BACKING`.

- **Software state changed**: on success, a `direct_slot_t` transitions
  `SLOT_EMPTY → SLOT_BOUND` (or is a no-op success if already bound at
  matching geometry — an explicit idempotent-cache-hit path, not a true
  "always mutates" primitive).
- **Hardware command**: resource creation + backing attach only — no
  `SET_SCANOUT` yet (that's a separate, later present() call).
- **Validation before mutation**: minimal — non-null backing, non-zero
  dimensions, a free cache slot (or matching-geometry existing slot) are
  checked BEFORE calling into the backend; but there is no check of whether
  the eventual `SET_SCANOUT`/present would succeed. Compositor64's own
  `direct_scanout_eligible()` check happens BEFORE this call, at a higher
  layer — display64/virtio_gpu64 themselves do no independent policy
  validation.
- **On failure**: `direct_bind_fn` failure leaves the slot untouched
  (`find_empty_slot()`'s slot is never written since the write happens
  after the backend call succeeds) — clean, no partial state.
- **Intermediate-state observability**: none — bind either fully succeeds
  (slot now BOUND, resource exists) or fully fails (nothing changed).

### 3. Direct scanout present (with or without switch_active)
`display64_direct_present()` → `virtio_gpu64.c`'s `direct_present_impl` →
`TRANSFER_TO_HOST_2D` → (if `switch_active`) `SET_SCANOUT` → `RESOURCE_FLUSH`.

- **Software state changed**: on success, at most one slot's state flips to
  `SLOT_ACTIVE` (others revert to `SLOT_BOUND`) if `switch_active` was
  requested; otherwise no slot-state change at all (content-only refresh).
- **Hardware command**: as above — this is the ONE path in the entire
  current codebase that already does real "only issue what changed"
  delta logic (no `SET_SCANOUT` unless the active resource is actually
  changing) — M+10's §7/§8 delta model should generalize this existing,
  already-correct pattern rather than reinvent it.
- **Validation before mutation**: the slot must already be bound
  (`find_slot()`); no other check. Eligibility (§3 of M+9B) was already
  decided by the caller before this was ever invoked.
- **On failure**: any VirtIO command failing inside `direct_present_impl`
  returns -1; `display64_direct_present()` never advances the slot's state
  on that path — old ACTIVE slot (if any) remains ACTIVE, matching the "a
  failed switch never claims success" contract. **However**: if the failure
  happens AFTER `TRANSFER_TO_HOST_2D`/`SET_SCANOUT` already reached the
  device but BEFORE `RESOURCE_FLUSH` (or the flush itself fails), the real
  hardware scanout may already be pointing at the new resource while
  software state (`slot->state`) still says the OLD one is active — this is
  exactly the "truthful vs lying state" gap M+10 §10/§11 must close. This
  is the single most important finding of this audit for M+10's design.
- **Intermediate-state observability**: yes, per the above — a partial
  present() failure can leave software state and real hardware state
  disagreeing, currently completely undetected (no `UNKNOWN` state exists
  anywhere in this codebase today).

### 4. Direct scanout leave
`display64_leave_direct_scanout()` → `virtio_gpu64.c`'s `direct_leave_impl`
→ switches scanout back to the compositor's own resource, flushes.

- **Software state changed**: ACTIVE slot → BOUND, only on success.
- **On failure**: slot stays ACTIVE, "old buffer stays alive" per its own
  comment — this is the ONE mutation path in the current codebase that
  already gets the "leave software state unchanged on hardware failure"
  rule right. But note: like #3, a failure partway through the backend's
  own multi-command sequence (switch + flush) has the same
  software/hardware disagreement risk — `direct_leave_impl`'s internal
  ordering isn't separately audited here since it's inside
  `virtio_gpu64.c`, not `display64.c`, but the same M+10 concern applies.

### 5. Direct scanout unbind
`display64_direct_scanout_unbind()` → `virtio_gpu64.c`'s `direct_unbind_impl`
→ releases the VirtIO resource + memobj64 reference `gpu64_buffer_wrap()`
took at bind time.

- **Software state changed**: slot → EMPTY, only if not currently ACTIVE
  (refused otherwise — no-op, not an error return, matching this
  function's `void` signature).
- **Validation before mutation**: the ACTIVE check IS the validation here —
  this is the one place display64.c enforces an ordering invariant
  (never release a resource still scanned out) unconditionally, not
  something callers must remember.
- **On failure**: `direct_unbind_fn` (a `void`-returning backend hook) has
  no failure signal at all today — release is assumed to always succeed.
  This is a real gap: M+10's resource-reference model (§4) needs an
  explicit answer for whether releasing a backend resource can fail, even
  if the current VirtIO implementation never actually reports one.

## CURSOR

### 6. Cursor image set
`display64_cursor_set_image()` → backend `set_image_fn` → 
`virtio_gpu64.c` transfers the cursor bitmap into a dedicated cursor
resource, `UPDATE_CURSOR`.

- **Software state changed**: none in display64.c itself — this is a pure
  pass-through to the backend, no local state tracked (no "current cursor
  image" cached anywhere in display64.c).
- **Validation before mutation**: only "is a backend registered" — no
  size/format validation exists in display64.c at all today (the backend's
  own fixed `HW_CURSOR_DIM` constant is compositor64's own choice, not
  something display64 enforces).
- **On failure**: caller (`compositor64.c`, boot-time hardware-cursor
  probe) treats failure as "hardware cursor unavailable," falls back to
  software cursor for the ENTIRE session — a one-time, boot-only decision,
  not a per-call retry.
- **Intermediate-state observability**: n/a — no caller currently changes
  the cursor image after boot (only position moves at runtime).

### 7. Cursor position move
`display64_cursor_move()` → backend `move_fn` → `MOVE_CURSOR` only —
already the cheapest possible path, no transfer/flush.

- **Software state changed**: none in display64.c (position isn't cached
  there); compositor64 itself tracks `g_cursor_x/y`.
- **On failure** (`compositor64.c`'s own handling, not display64's): a
  runtime `sys_cursor_move()` failure after having worked at boot triggers
  an immediate, permanent fallback for the rest of the session: hardware
  cursor disabled (`g_hw_cursor_active = 0`), `sys_cursor_set_visible(0)`
  best-effort called, ONE software-cursor damage rect added at the new
  position so the very next composite draws it correctly with no ghost —
  and (M+9B integration) this is also explicitly treated as an
  eligibility loss for direct scanout, since software cursor cannot
  coexist with it.
- **This is the fast, high-frequency path M+10 §14/§15 must not regress.**
  Currently: exactly one MOVE_CURSOR command per motion event, zero
  compositor damage, zero backbuffer copy, zero present call — already
  matches M+10's own required result. The atomic model must preserve this
  exactly, not wrap it in a heavier state-duplication cost per mouse
  packet (§15's own explicit requirement).

### 8. Cursor visibility
`display64_cursor_set_visible()` → backend `set_visible_fn`. Only called
today from the ONE runtime-failure fallback path above
(`sys_cursor_set_visible(g_display_h, 0)`), best-effort (return value not
checked) — no code path currently re-ENABLES it after that.

## OUTPUT

### 9. Mode / geometry
There is no mutation path at all. `g_disp_w`/`g_disp_h` (compositor64) and
`gpu_width`/`gpu_height` (display64) are set exactly once, at boot, from
whatever the VirtIO GPU device itself reports
(`toxenos_rs::virtio_gpu`'s own `scanout[0]` geometry) or from the Multiboot2
framebuffer info for the legacy path. `display64_init()` and
`display64_set_gpu_backend()` are each callable exactly once per boot
(explicitly rejected on a second call) — there is no "change resolution"
operation anywhere in this codebase today. This confirms M+10 §17's own
scoping: mode representation can exist in `display64_state_t`, but
`check()` must reject any actual mode CHANGE as unsupported; nothing needs
to be built to make mode changes work.

## PRESENTED STATE (compositor64, not display64 — but tightly coupled)

### 10. `PRESENTED_COMPOSITED` / `PRESENTED_DIRECT` / `g_direct_window_idx`
Pure compositor64-side bookkeeping, advanced ONLY after the corresponding
display64 call above (present with switch_active, or leave) returns
success — already follows the "advance software state only after
confirmed hardware success" rule for the HAPPY path. The audit finding
from #3/#4 above (a failure partway through a backend's own multi-command
sequence can leave real hardware ahead of what any software state
believes) applies transitively here too: compositor64's
`PRESENTED_COMPOSITED`/`PRESENTED_DIRECT` is only ever as truthful as
display64's own return value, which — per this audit — cannot currently
distinguish "nothing happened" from "some hardware commands landed before
one failed."

## Cross-cutting findings for M+10's design

1. **No mutation path in this codebase does validation separate from
   mutation today.** Every one of #1–#8 either fully succeeds or fully
   fails as one step; there is no existing "check-only" mode to generalize
   — M+10's `display64_state_check()` is genuinely new, not a refactor of
   something that half-exists.
2. **The one already-correct pattern to preserve, not reinvent**: direct
   scanout present's own "only SET_SCANOUT if the resource actually
   changed" delta logic (#3). M+10's delta-based commit (§7/§8) should
   generalize this exact idea to cursor and primary-content updates too,
   not design a new rule from scratch.
3. **The one real gap this audit surfaces that M+10 must close**: no
   existing path can express "some VirtIO commands succeeded, a later one
   in the same logical operation failed" — every current failure return is
   a simple boolean with no partial-progress information. M+10's
   rollback/`UNKNOWN` design (§10/§11) is filling a gap that genuinely does
   not exist today, not replacing existing (if imperfect) machinery.
4. **Resource lifetime today is entirely manual and scattered**: the
   direct-scanout cache (`display64.c`) owns slot lifetime, but the memobj64
   reference itself is taken/released inside `virtio_gpu64.c`'s
   `direct_bind_impl`/`direct_unbind_impl`, invisible to `display64.c`. A
   candidate state's own reference-holding (M+10 §4) needs to decide which
   layer actually owns the refcount for a PENDING (not yet committed)
   resource reference — see the design document's own answer.
