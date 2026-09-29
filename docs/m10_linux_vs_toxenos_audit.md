# M+10 §0 — Linux Atomic KMS vs ToxenOS Audit

Sources inspected (current upstream `torvalds/linux`, master branch, read for
concepts only — no GPL source copied):
`Documentation/gpu/drm-kms.rst`, `drivers/gpu/drm/drm_atomic.c`,
`drivers/gpu/drm/drm_atomic_helper.c`, `drivers/gpu/drm/drm_atomic_state_helper.c`.

## What Linux atomic KMS actually guarantees

1. **State duplication, not live mutation.** A free-standing
   `drm_atomic_state` container holds duplicated per-object state
   (`drm_plane_state`, `drm_crtc_state`, `drm_connector_state`).
   `drm_atomic_get_*_state()` locks the object and calls a driver
   `atomic_duplicate_state()` hook that `memcpy`s the whole struct, then
   takes a **new reference** on anything the state points to (a plane's
   framebuffer via `drm_framebuffer_get()`, a CRTC's property blobs via
   `drm_property_blob_get()`). Live objects are never edited while a
   candidate is being assembled.
2. **Check is a pure validation pass.** `drm_atomic_check_only()` runs
   every object's `atomic_check()` and issues zero hardware commands.
   `DRM_MODE_ATOMIC_TEST_ONLY` exists specifically so userspace can probe
   feasibility with this guarantee.
3. **Discard-on-failure is just refcounting.** `drm_atomic_commit_clear()`
   calls each object's `atomic_destroy_state()` (which drops the extra
   references duplicate_state took — `drm_framebuffer_put()`, etc.) and
   nullifies the state pointers. There is no larger rollback machinery
   because the design never mutated anything live to begin with — "rolling
   back an update boils down to releasing memory."
4. **Swap-state is the one point of no return.** `drm_atomic_helper_swap_state()`
   exchanges each object's active-state pointer for the new one. Everything
   before this point is reversible by discarding the candidate;
   everything after is committed to hardware.
5. **No hardware rollback exists.** This is the critical finding for M+10.
   The documented helper commit tail is: disable-needed-outputs → commit
   planes → enable-needed-outputs → wait for vblank/flip → cleanup old
   framebuffers. The helper's own comment states the design philosophy
   directly: *"everything below never fails except when the hw goes
   bonghits."* Linux's atomic model assumes `atomic_check` makes hardware
   failure during commit effectively impossible on real hardware, and
   provides **no formal mid-commit rollback path** if that assumption is
   wrong. A real GPU driver that hits a genuine hardware fault mid-commit
   is, in practice, in undefined/degraded territory that the atomic KMS
   core itself does not solve.

## Why ToxenOS cannot just copy this assumption

Linux's "commit cannot realistically fail" assumption holds because real
display hardware's failure modes are rare and mostly caught by
`atomic_check` (unsupported mode, insufficient bandwidth, etc.) — a
programmed register write essentially always "succeeds" from the driver's
point of view. ToxenOS's VirtIO-GPU backend is different: each stage
(`TRANSFER_TO_HOST_2D`, `SET_SCANOUT`, `RESOURCE_FLUSH`, cursor
transfer/update/move) is a real virtqueue round-trip that can fail for
reasons `check` cannot fully predict (a fault-injected failure today; a
genuinely degraded/reset device tomorrow). M+9B already treats every one of
these as a real, independently-failable operation with its own recovery
rule (e.g. "a failed leave must never be treated as having happened").

**Conclusion for M+10's design**: adopt Linux's *check/duplicate/refcount/
discard* model wholesale (§0's item 1-4 above) — it is directly applicable
and battle-tested. Do **not** adopt Linux's "commit cannot fail" assumption.
ToxenOS's M+10 must add an explicit mid-commit failure/rollback contract
(rollback-if-possible, else `UNKNOWN`/`RECOVERY_REQUIRED`) that upstream
Linux atomic KMS does not need and does not provide — this is new,
ToxenOS-native design, not a gap in the Linux study.

## Mapping table

| Linux concept | ToxenOS M+10 equivalent |
|---|---|
| `drm_atomic_state` (whole-transaction container) | `display64_state_t` candidate |
| `drm_plane_state`/`drm_crtc_state` (per-object duplicated state) | one flat `display64_state_t` (primary + cursor + output) — ToxenOS has one plane, one cursor, one output; no object graph is needed |
| `atomic_duplicate_state()` hook | `display64_state_begin()` (duplicate current) |
| framebuffer refcount on duplicate (`drm_framebuffer_get`) | memobj64/gpu64_buffer reference taken by the candidate on any resource it names |
| `atomic_check()` | `display64_state_check()` — zero side effects, same contract |
| `atomic_destroy_state()` / `drm_atomic_commit_clear()` | candidate-discard path — releases the references check-time acquisition took |
| `drm_atomic_helper_swap_state()` (point of no return) | `display64_state_commit()`'s own point of no return — but see below |
| helper commit tail ordering (disable → planes → enable → vblank wait → cleanup old fb) | VirtIO delta-ordering per §9 (ensure resource valid → transfer → switch scanout → flush; cursor transfer → update; motion → move only) |
| **(no Linux equivalent)** | mid-commit failure → rollback-or-`UNKNOWN`/`RECOVERY_REQUIRED` (§10/§11) — ToxenOS-native, not borrowed |

## Explicit non-goals (per M+10's own scope limits, reconfirmed after this study)

- No `drm_mode_object`/property-blob/connector universe — ToxenOS has one
  fixed output, one primary, one cursor. A generic object graph would be
  over-engineering for hardware this codebase doesn't have.
- No async/worker-thread commit queue (`drm_atomic_helper_wait_for_dependencies()`
  and friends) — ToxenOS's compositor is single-threaded; commit is
  synchronous by construction (M+10 §6 already mandates this explicitly).
- No connector/encoder concepts — VirtIO-GPU's single scanout has no
  equivalent split to preserve.
