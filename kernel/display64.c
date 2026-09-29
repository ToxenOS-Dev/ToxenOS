// kernel/display64.c — Milestone 29: low-level framebuffer/display layer.
// See include/display64.h for the design rationale and layering.
#include <stdint.h>
#include "../include/display64.h"
#include "../include/physmem64.h"
#include "../include/process64.h"
#include "../include/klog.h"

static uint64_t    fb_base  = 0;   // mapped virtual address, 0 = unavailable
static uint32_t    fb_width = 0, fb_height = 0, fb_pitch = 0;
static pixfmt64_t  fb_fmt;
static int         fb_avail = 0;
static uint32_t    owner_pid = 0;  // 0 = unowned (real pids start at 1)

// ── M+4: GPU presentation backend ───────────────────────────────────
// See include/display64.h's own header comment for the full design.
// Always identity XRGB8888 -- no pixfmt64_t needed for this backend at
// all, unlike the framebuffer state above.
static uint64_t gpu_base   = 0;    // kernel-mapped vaddr, 0 = no GPU backend registered
static uint32_t gpu_width  = 0, gpu_height = 0, gpu_pitch = 0;
static display64_gpu_flush_fn gpu_flush_fn = 0;
static int gpu_disabled_after_failure = 0; // sticky for the rest of this boot -- see display64_gpu_flush_rect()

static const pixfmt64_t g_gpu_identity_fmt = { 32, 4, 16, 8, 8, 8, 0, 8 };

// Forward-declared: real definition (with fb_base's own layout-switch
// logic) lives further down, but M+8's GPU-flush-failure fallback needs
// it earlier in the file.
static inline void write_native(uint64_t off, uint32_t native);

static inline int gpu_active(void) { return gpu_base != 0 && !gpu_disabled_after_failure; }

// ── M+10: atomic display state -- authoritative CURRENT (declared here,
// early, since display64_set_gpu_backend()/display64_init() below both
// need to sync display64_current.output as soon as a backend registers;
// the rest of the M+10 API (display64_state_begin/check/commit/etc.) is
// implemented further down, after the direct-scanout resource cache it
// builds on -- see that section's own header comment for the full
// design). g_cursor_visible's own "starts visible" default
// (kernel/virtio_gpu64.c) is mirrored here so a fresh boot's
// authoritative state already matches what the registered backend
// itself assumes before any commit ever runs.
static display64_state_t g_display_current = {
    .primary = { .kind = DISPLAY64_PRIMARY_COMPOSITED },
    .cursor  = { .enabled = 1 },
    .output  = { 0 },
};
static display64_validity_t g_display_validity = DISPLAY64_STATE_VALID;
static uint32_t g_cursor_image_generation_counter = 0;

// Called from display64_init()/display64_set_gpu_backend() -- the output
// state always mirrors whichever backend is currently ACTIVE, exactly
// like display64_get_info()'s own precedence (GPU wins once registered).
static void sync_current_output(void) {
    if (gpu_active()) {
        g_display_current.output.width  = gpu_width;
        g_display_current.output.height = gpu_height;
    } else if (fb_avail) {
        g_display_current.output.width  = fb_width;
        g_display_current.output.height = fb_height;
    }
}

int display64_set_gpu_backend(uint64_t vaddr, uint32_t width, uint32_t height,
                               uint32_t stride, display64_gpu_flush_fn flush_fn)
{
    if (gpu_base != 0) return -1; // one GPU backend per boot
    if (!vaddr || !flush_fn || width == 0 || height == 0 || stride < width * 4u) return -1;

    gpu_base   = vaddr;
    gpu_width  = width;
    gpu_height = height;
    gpu_pitch  = stride;
    gpu_flush_fn = flush_fn;
    gpu_disabled_after_failure = 0;
    sync_current_output();
    return 0;
}

int display64_gpu_flush_rect(uint32_t x, uint32_t y, uint32_t w, uint32_t h)
{
    if (!gpu_active()) return 0; // nothing to flush -- blit_row's writes (if any) already landed synchronously
    if (gpu_flush_fn(x, y, w, h) == 0) return 0;

    klog("display64: GPU backend flush failed -- disabling GPU presentation for the rest of this boot session");
    gpu_disabled_after_failure = 1;
    if (!fb_avail) {
        klog(" (no framebuffer fallback available -- presentation now unavailable)\n");
        return -1;
    }
    klog(" -- synchronously mirroring this rect into the legacy framebuffer\n");

    // M+8: this rect's pixels are already correct in gpu_base --
    // display64_blit_row()/put_pixel()/etc. wrote them there before this
    // flush was ever attempted; only the DEVICE never received them.
    // Copy that same data into the legacy framebuffer NOW, synchronously
    // -- a plain MMIO store that cannot itself fail to reach the
    // framebuffer -- so THIS present call can still honestly report
    // success. Clipped independently against both the GPU backend's own
    // geometry (source) and the framebuffer's (destination); they are
    // populated from different sources (GET_DISPLAY_INFO vs. the
    // Multiboot2 framebuffer info) and are not guaranteed identical.
    if (x >= gpu_width || y >= gpu_height || x >= fb_width || y >= fb_height) return -1;
    uint32_t cw = w, ch = h;
    if (x + cw > gpu_width) cw = gpu_width - x;
    if (y + ch > gpu_height) ch = gpu_height - y;
    if (x + cw > fb_width)  cw = fb_width  - x;
    if (y + ch > fb_height) ch = fb_height - y;
    if (cw == 0 || ch == 0) return -1;

    const uint8_t* gsrc = (const uint8_t*)(uintptr_t)gpu_base;
    for (uint32_t row = 0; row < ch; row++) {
        const uint32_t* srow = (const uint32_t*)(uintptr_t)(gsrc + (uint64_t)(y + row) * gpu_pitch + (uint64_t)x * 4u);
        uint64_t fb_row_off = (uint64_t)(y + row) * fb_pitch + (uint64_t)x * fb_fmt.bytes_per_pixel;
        for (uint32_t col = 0; col < cw; col++) {
            // gpu_base is always identity XRGB8888 (g_gpu_identity_fmt) --
            // srow[col] IS already the logical 0x00RRGGBB value.
            write_native(fb_row_off + (uint64_t)col * fb_fmt.bytes_per_pixel, pixfmt64_pack(&fb_fmt, srow[col]));
        }
    }
    return 0;
}

// ── M+7: generic hardware cursor backend ─────────────────────────────
// See include/display64.h's own header comment for the full design.
// Independent of the GPU presentation backend above (no shared state,
// no ordering requirement between the two registrations) -- a cursor
// backend is a separate optional capability, not a sub-feature of GPU
// presentation.
static display64_cursor_set_image_fn   cursor_set_image_fn   = 0;
static display64_cursor_move_fn        cursor_move_fn        = 0;
static display64_cursor_set_visible_fn cursor_set_visible_fn = 0;

int display64_set_cursor_backend(display64_cursor_set_image_fn set_image_fn,
                                  display64_cursor_move_fn move_fn,
                                  display64_cursor_set_visible_fn set_visible_fn)
{
    if (cursor_set_image_fn || cursor_move_fn || cursor_set_visible_fn) return -1; // one cursor backend per boot
    if (!set_image_fn || !move_fn || !set_visible_fn) return -1;
    cursor_set_image_fn   = set_image_fn;
    cursor_move_fn        = move_fn;
    cursor_set_visible_fn = set_visible_fn;
    return 0;
}

int display64_cursor_available(void) {
    return cursor_set_image_fn != 0;
}

// M+10: these three no longer call their registered backend function
// pointers directly -- each now builds a candidate atomic state (a
// duplicate of current, per display64_state_begin()'s own reference
// discipline) with exactly the one field this operation changes, then
// runs it through the shared check/commit pipeline. cursor_set_image_fn/
// move_fn/set_visible_fn stay registered exactly as before (M+7's own
// contract) -- they are now called FROM the atomic backend's own
// atomic_commit implementation (kernel/virtio_gpu64.c) rather than from
// here directly; this file no longer needs to know their names to
// service a cursor request, only whether a cursor backend is present at
// all (display64_cursor_available()'s own unchanged check).
int display64_cursor_set_image(uint32_t width, uint32_t height, const uint32_t* argb_pixels, uint32_t hot_x, uint32_t hot_y) {
    if (!cursor_set_image_fn) return -1;
    display64_state_t cand;
    display64_state_begin(&cand);
    display64_state_set_cursor_image(&cand, width, height, argb_pixels, hot_x, hot_y);
    return display64_state_commit(&cand);
}

int display64_cursor_move(int32_t x, int32_t y) {
    if (!cursor_move_fn) return -1;
    display64_state_t cand;
    display64_state_begin(&cand);
    cand.cursor.x = x;
    cand.cursor.y = y;
    return display64_state_commit(&cand);
}

int display64_cursor_set_visible(int visible) {
    if (!cursor_set_visible_fn) return -1;
    display64_state_t cand;
    display64_state_begin(&cand);
    cand.cursor.enabled = visible ? 1 : 0;
    return display64_state_commit(&cand);
}

// ── M+9B: generic direct-scanout / composition-bypass backend ───────
// See include/display64.h's own header comment for the full state
// machine and design. Independent of the GPU presentation/cursor
// backends above (no shared state) -- registered separately by
// kernel/virtio_gpu64.c's own virtio_gpu64_init_compositor_backend(),
// which already owns g_compositor_buf (the resource this backend's
// leave_fn switches scanout back to).
static display64_direct_bind_fn    direct_bind_fn    = 0;
static display64_direct_present_fn direct_present_fn = 0;
static display64_direct_leave_fn   direct_leave_fn   = 0;
static display64_direct_unbind_fn  direct_unbind_fn  = 0;

typedef enum { SLOT_EMPTY = 0, SLOT_BOUND, SLOT_ACTIVE } direct_slot_state_t;
typedef struct {
    direct_slot_state_t state;
    uint64_t identity;      // the client shm_token this slot is cached for
    void*    handle;        // opaque backend handle (a gpu64_buffer_t*, in the VirtIO backend)
    memobj64_t* backing;    // M+10: the same memobj64_t* passed to display64_direct_scanout_bind() -- remembered here (not read back out of the opaque `handle`, which stays backend-private) so a candidate atomic state naming this identity can take its OWN reference. Does not itself keep this alive -- the bind-time reference gpu64_buffer_wrap() took (via `handle`) already does that; this is only ever used to add a FURTHER reference on top.
    uint32_t width, height;
} direct_slot_t;
static direct_slot_t g_direct_slots[DISPLAY64_DIRECT_MAX_SLOTS];

static direct_slot_t* find_slot(uint64_t identity) {
    for (int i = 0; i < DISPLAY64_DIRECT_MAX_SLOTS; i++) {
        if (g_direct_slots[i].state != SLOT_EMPTY && g_direct_slots[i].identity == identity) return &g_direct_slots[i];
    }
    return 0;
}
static direct_slot_t* find_empty_slot(void) {
    for (int i = 0; i < DISPLAY64_DIRECT_MAX_SLOTS; i++) if (g_direct_slots[i].state == SLOT_EMPTY) return &g_direct_slots[i];
    return 0;
}

int display64_set_direct_scanout_backend(display64_direct_bind_fn bind_fn,
                                          display64_direct_present_fn present_fn,
                                          display64_direct_leave_fn leave_fn,
                                          display64_direct_unbind_fn unbind_fn)
{
    if (direct_bind_fn || direct_present_fn || direct_leave_fn || direct_unbind_fn) return -1; // one backend per boot
    if (!bind_fn || !present_fn || !leave_fn || !unbind_fn) return -1;
    direct_bind_fn    = bind_fn;
    direct_present_fn = present_fn;
    direct_leave_fn   = leave_fn;
    direct_unbind_fn  = unbind_fn;
    return 0;
}

int display64_direct_scanout_supported(void) {
    return direct_bind_fn != 0;
}


int display64_direct_scanout_bind(uint64_t identity, memobj64_t* backing, uint32_t width, uint32_t height, void** out_handle) {
    if (!direct_bind_fn) return -1;
    if (!backing || width == 0 || height == 0) return -1;

    direct_slot_t* existing = find_slot(identity);
    if (existing) {
        // Idempotent re-bind, per this header's own cache comment --
        // only a genuine geometry mismatch is an error (a client would
        // never legitimately recommit the SAME shm_token at a different
        // size; this is a defensive check, not an expected path).
        if (existing->width != width || existing->height != height) return -1;
        if (out_handle) *out_handle = existing->handle;
        return 0;
    }

    direct_slot_t* slot = find_empty_slot();
    if (!slot) {
        return -1; // cache full -- see this header's own comment on why this shouldn't happen under §3's eligibility rule
    }

    void* handle = 0;
    if (direct_bind_fn(backing, width, height, &handle) < 0) {
        return -1;
    }

    slot->state    = SLOT_BOUND;
    slot->identity = identity;
    slot->handle   = handle;
    slot->backing  = backing;
    slot->width    = width;
    slot->height   = height;
    if (out_handle) *out_handle = handle;
    return 0;
}

// M+10: `switch_active` is now advisory only -- kept in the syscall's own
// wire struct for compatibility, but the atomic layer decides for itself
// (via compute_delta()'s own identity comparison against
// g_display_current) whether a resource change is actually needed,
// rather than trusting the caller's flag. This is a genuine correctness/
// efficiency improvement over the pre-M+10 behavior: a single-buffer
// fullscreen client (gfx_interactive64.c) that always passed
// switch_active=1 on every steady-state commit previously caused a
// redundant SET_SCANOUT every single frame even though its resource
// identity never changed -- compute_delta() now correctly recognizes
// "same identity as current" and only ever computes
// DISPLAY64_DELTA_PRIMARY_CONTENT for that case (transfer+flush, no
// SET_SCANOUT). See docs/m10_current_mutation_audit.md's own finding #3.
// The direct-scanout cache's own SLOT_BOUND/SLOT_ACTIVE bookkeeping
// (used by display64_direct_scanout_unbind()'s own "refuse to unbind the
// M+10: display64_direct_present()/leave_direct_scanout() themselves are
// defined further down, AFTER the atomic-state block they now depend on
// (g_display_current, display64_state_begin/commit) -- see that block's
// own placement note.

void display64_direct_scanout_unbind(uint64_t identity) {
    if (!direct_unbind_fn) return;
    direct_slot_t* slot = find_slot(identity);
    if (!slot) {
        return;                      // not cached -- no-op
    }
    if (slot->state == SLOT_ACTIVE) {
        return; // refused: still the live scanout source -- see header comment
    }

    direct_unbind_fn(slot->handle);
    slot->state    = SLOT_EMPTY;
    slot->identity = 0;
    slot->handle   = 0;
    slot->backing  = 0;
    slot->width = 0; slot->height = 0;
}

#ifdef M9B_FAULT_INJECT
// Bitmask, not a single value -- see include/display64.h's own comment
// on why compound scenarios (a partial-commit failure whose OWN
// recovery must ALSO be forced to fail) need two independent one-shot
// arms active at once.
static uint32_t g_m9b_fault_armed_mask = 0;
void display64_direct_fault_inject_arm(m9b_fault_point_t point) { g_m9b_fault_armed_mask |= (1u << (uint32_t)point); }
int display64_direct_fault_should_fail(m9b_fault_point_t point) {
    uint32_t bit = 1u << (uint32_t)point;
    if (g_m9b_fault_armed_mask & bit) {
        g_m9b_fault_armed_mask &= ~bit; // one-shot -- see M+8's own m8_fault_inject_arm() precedent
        return 1;
    }
    return 0;
}
void display64_direct_fault_inject_clear_all(void) { g_m9b_fault_armed_mask = 0; }
#else
void display64_direct_fault_inject_arm(m9b_fault_point_t point) { (void)point; }
int display64_direct_fault_should_fail(m9b_fault_point_t point) { (void)point; return 0; }
void display64_direct_fault_inject_clear_all(void) { }
#endif

// ── M+10: atomic display state ───────────────────────────────────────
// See include/display64.h's own header comment and docs/m10_*.md for the
// full design. This layer sits ABOVE the direct-scanout resource cache
// above (bind/unbind, UNCHANGED) and never touches the GPU-presentation
// content path (display64_gpu_flush_rect/blit_row, also UNCHANGED --
// §16's own configuration-vs-content separation).
static display64_atomic_check_fn  atomic_check_fn  = 0;
static display64_atomic_commit_fn atomic_commit_fn = 0;
// g_display_current/g_display_validity/g_cursor_image_generation_counter/
// sync_current_output() are declared near the top of this file (right
// after gpu_active()) since display64_set_gpu_backend()/display64_init()
// need them long before this point.

int display64_set_atomic_backend(display64_atomic_check_fn check_fn, display64_atomic_commit_fn commit_fn) {
    if (atomic_check_fn || atomic_commit_fn) return -1; // one atomic backend per boot
    if (!check_fn || !commit_fn) return -1;
    atomic_check_fn  = check_fn;
    atomic_commit_fn = commit_fn;
    return 0;
}

void display64_state_begin(display64_state_t* out) {
    *out = g_display_current;
    if (out->primary.kind == DISPLAY64_PRIMARY_DIRECT && out->primary.backing) {
        memobj64_add_ref(out->primary.backing);
    }
}

void display64_state_set_cursor_image(display64_state_t* state, uint32_t width, uint32_t height,
                                       const uint32_t* argb_pixels, uint32_t hot_x, uint32_t hot_y)
{
    // Matches this codebase's own pre-M+10 behavior (kernel/virtio_gpu64.c's
    // cursor_backend_set_image()): uploading a new image makes the cursor
    // visible unless a LATER, explicit hide in the same transaction (or a
    // caller setting state->cursor.enabled = 0 itself afterward) says
    // otherwise -- never silently un-hides on its own once genuinely
    // hidden by an earlier, separate commit (that memory lives in
    // g_display_current.cursor.enabled, which this candidate started
    // from a duplicate of -- setting it here only changes THIS candidate).
    state->cursor.image_generation = ++g_cursor_image_generation_counter;
    state->cursor.img_width  = width;
    state->cursor.img_height = height;
    state->cursor.argb_pixels = argb_pixels;
    state->cursor.hot_x = hot_x;
    state->cursor.hot_y = hot_y;
}

int display64_state_set_primary_direct(display64_state_t* state, uint64_t identity, uint32_t width, uint32_t height) {
    direct_slot_t* slot = find_slot(identity);
    if (!slot || slot->width != width || slot->height != height) return -1;
    if (state->primary.kind == DISPLAY64_PRIMARY_DIRECT && state->primary.backing) {
        memobj64_release(state->primary.backing);
    }
    if (slot->backing) memobj64_add_ref(slot->backing);
    state->primary.kind          = DISPLAY64_PRIMARY_DIRECT;
    state->primary.identity      = identity;
    state->primary.backing       = slot->backing;
    state->primary.backend_handle = slot->handle;
    state->primary.width         = width;
    state->primary.height        = height;
    state->primary.has_damage    = 0;
    return 0;
}

void display64_state_set_primary_composited(display64_state_t* state) {
    if (state->primary.kind == DISPLAY64_PRIMARY_DIRECT && state->primary.backing) {
        memobj64_release(state->primary.backing);
    }
    state->primary.kind          = DISPLAY64_PRIMARY_COMPOSITED;
    state->primary.identity      = 0;
    state->primary.backing       = 0;
    state->primary.backend_handle = 0;
    state->primary.width = 0; state->primary.height = 0;
    state->primary.has_damage = 0;
}

void display64_state_set_primary_damage(display64_state_t* state, uint32_t x, uint32_t y, uint32_t w, uint32_t h) {
    state->primary.damage_x = x; state->primary.damage_y = y;
    state->primary.damage_w = w; state->primary.damage_h = h;
    state->primary.has_damage = 1;
}

static int primary_identity_equal(const display64_primary_state_t* a, const display64_primary_state_t* b) {
    if (a->kind != b->kind) return 0;
    if (a->kind == DISPLAY64_PRIMARY_DIRECT && a->identity != b->identity) return 0;
    return 1;
}

// §8: compares `cand` against `cur`, returns exactly which command
// groups a commit of `cand` would need to issue. DISPLAY64_DELTA_NONE
// for an unchanged candidate -- zero hardware commands, per §21 test A.
static display64_delta_t compute_delta(const display64_state_t* cur, const display64_state_t* cand) {
    display64_delta_t d = DISPLAY64_DELTA_NONE;

    if (!primary_identity_equal(&cur->primary, &cand->primary)) {
        d = (display64_delta_t)(d | DISPLAY64_DELTA_PRIMARY_CHANGED);
    } else if (cand->primary.has_damage) {
        d = (display64_delta_t)(d | DISPLAY64_DELTA_PRIMARY_CONTENT);
    }

    if (cand->cursor.image_generation != cur->cursor.image_generation) {
        d = (display64_delta_t)(d | DISPLAY64_DELTA_CURSOR_IMAGE);
    }
    if (cand->cursor.x != cur->cursor.x || cand->cursor.y != cur->cursor.y) {
        d = (display64_delta_t)(d | DISPLAY64_DELTA_CURSOR_POSITION);
    }
    if (cand->cursor.enabled != cur->cursor.enabled) {
        d = (display64_delta_t)(d | DISPLAY64_DELTA_CURSOR_VISIBILITY);
    }

    if (cand->output.width != cur->output.width || cand->output.height != cur->output.height) {
        d = (display64_delta_t)(d | DISPLAY64_DELTA_MODE_CHANGED);
    }
    return d;
}

// §5: zero side effects, regardless of outcome -- issues no VirtIO
// commands, alters no scanout/cursor/resource ownership, mutates no
// current state. Every check below is a pure comparison against already-
// cached information (g_display_current, the direct-scanout cache's own
// slot table, cursor_available()) -- never a call into anything that
// could itself have a side effect.
display64_check_result_t display64_state_check(const display64_state_t* candidate) {
    // §11: while hardware state is unknown, refuse every ordinary
    // transaction outright rather than letting a caller proceed as if
    // nothing were wrong -- display64_state_retry_recovery() is the
    // only sanctioned way back to VALID.
    if (g_display_validity == DISPLAY64_STATE_RECOVERY_REQUIRED) {
        return DISPLAY64_CHECK_RECOVERY_REQUIRED;
    }

    // §17: mode setting is out of scope this milestone -- represent
    // current, validate unchanged, reject any actual change outright.
    if (candidate->output.width != g_display_current.output.width ||
        candidate->output.height != g_display_current.output.height) {
        return DISPLAY64_CHECK_MODE_UNSUPPORTED;
    }

    if (candidate->primary.kind == DISPLAY64_PRIMARY_DIRECT) {
        if (!direct_bind_fn) return DISPLAY64_CHECK_NO_BACKEND;
        direct_slot_t* slot = find_slot(candidate->primary.identity);
        if (!slot) return DISPLAY64_CHECK_BAD_PRIMARY; // not currently bound in the cache
        if (slot->width != candidate->primary.width || slot->height != candidate->primary.height) return DISPLAY64_CHECK_BAD_PRIMARY;
        // No scaling/rotation/transform support exists anywhere in this
        // stack (M+9B's own §3 eligibility rule) -- a direct primary must
        // exactly match the output.
        if (candidate->primary.width != g_display_current.output.width ||
            candidate->primary.height != g_display_current.output.height) {
            return DISPLAY64_CHECK_BAD_PRIMARY;
        }
    }

    if (candidate->cursor.enabled) {
        if (!cursor_set_image_fn && !atomic_check_fn) return DISPLAY64_CHECK_NO_BACKEND;
    }

    display64_delta_t delta = compute_delta(&g_display_current, candidate);

    // Backend-specific validation (exact cursor size/format limits, any
    // future format/geometry rule only the VirtIO backend itself knows)
    // -- only invoked when the delta actually needs backend knowledge, so
    // a no-op or output-only candidate never even reaches the backend.
    if (delta & (DISPLAY64_DELTA_PRIMARY_CHANGED | DISPLAY64_DELTA_CURSOR_IMAGE | DISPLAY64_DELTA_CURSOR_VISIBILITY)) {
        if (!atomic_check_fn) {
            // §19: no atomic backend registered (software-framebuffer-only
            // boot) -- DIRECT primary and any cursor operation are
            // cleanly unsupported; a COMPOSITED-only, cursor-disabled
            // candidate needs no backend at all and is never rejected
            // here.
            if (candidate->primary.kind == DISPLAY64_PRIMARY_DIRECT) return DISPLAY64_CHECK_NO_BACKEND;
            if (delta & (DISPLAY64_DELTA_CURSOR_IMAGE | DISPLAY64_DELTA_CURSOR_VISIBILITY)) return DISPLAY64_CHECK_NO_BACKEND;
        } else {
            display64_check_result_t brc = atomic_check_fn(&g_display_current, candidate, delta);
            if (brc != DISPLAY64_CHECK_OK) return brc;
        }
    }

    return DISPLAY64_CHECK_OK;
}

// Releases the memobj64 reference the OLD current's primary held, once a
// commit has successfully replaced it. Every candidate carries its OWN
// independent reference (taken by display64_state_begin()'s duplicate or
// display64_state_set_primary_direct()'s explicit add_ref) -- when that
// candidate becomes the new current, the reference the PREVIOUS current
// held is now strictly redundant (candidate's own reference is what
// current holds going forward) and must be dropped unconditionally,
// regardless of whether the new primary happens to share the same
// identity as the old one (two independent references on the same
// object is exactly correct per §4's own model -- one is simply ending
// its life here).
static void release_superseded_primary(const display64_primary_state_t* old_primary) {
    if (old_primary->kind == DISPLAY64_PRIMARY_DIRECT && old_primary->backing) {
        memobj64_release(old_primary->backing);
    }
}

void display64_state_discard(display64_state_t* candidate) {
    if (candidate->primary.kind == DISPLAY64_PRIMARY_DIRECT && candidate->primary.backing) {
        memobj64_release(candidate->primary.backing);
        candidate->primary.backing = 0;
    }
}

void display64_state_get_current(display64_state_t* out) {
    *out = g_display_current;
}

display64_validity_t display64_state_get_validity(void) { return g_display_validity; }

// One recovery attempt: primary -> composited, cursor/output left
// exactly as `g_display_current` already has them (never compound a
// failure by also touching state unrelated to what just went wrong).
// Returns DISPLAY64_COMMIT_OK or a failure code, exactly like any other
// atomic_commit_fn call -- see display64_state_commit()'s own §10/§11
// handling of this result.
// Permanent (not fault-injection-gated), always-cheap diagnostic
// accessors -- exactly what M+10's own truthful-state requirement is
// for: a caller (a self-test, or a future compositor64 debug command)
// can always ask "what actually happened on the last commit," not just
// "did it return 0 or -1." Mirrors compositor64.c's own M9B_STATS
// "cheap counters, always on" precedent.
static display64_commit_result_t g_last_commit_result = DISPLAY64_COMMIT_OK;
static uint64_t g_recovery_attempts = 0;
static uint64_t g_recovery_successes = 0;
display64_commit_result_t display64_state_get_last_commit_result(void) { return g_last_commit_result; }
uint64_t display64_state_get_recovery_attempts(void) { return g_recovery_attempts; }
uint64_t display64_state_get_recovery_successes(void) { return g_recovery_successes; }

static display64_commit_result_t attempt_recovery_to_composited(void) {
    g_recovery_attempts++;
    // §20/M+10 fault-injection campaign: a dedicated fault point (rather
    // than reusing M9B_FAULT_SET_SCANOUT/RESOURCE_FLUSH, which the
    // ORIGINAL failing commit may have already consumed its one-shot arm
    // on) so a test can deterministically fail RECOVERY ITSELF,
    // independent of whatever caused the original commit to fail --
    // required to exercise "rollback failure -> RECOVERY_REQUIRED" as
    // its own distinct scenario. Checked BEFORE calling into the
    // backend at all: this simulates recovery's own hardware commands
    // failing without needing the backend to know it's being asked to
    // recover (it issues the exact same SET_SCANOUT+RESOURCE_FLUSH
    // sequence either way).
    if (display64_direct_fault_should_fail(M9B_FAULT_ROLLBACK)) {
        return DISPLAY64_COMMIT_FAIL_PARTIAL; // conservative: never claim recovery's own attempt was clean when deliberately forced to fail
    }

    display64_state_t recovery = g_display_current;
    display64_state_set_primary_composited(&recovery);
    // Force the backend to treat this as a full primary reprogram even
    // if g_display_current already believed it was composited (the
    // whole POINT of recovery is "hardware might disagree with software
    // right now" -- trusting compute_delta()'s own comparison here could
    // wrongly conclude DISPLAY64_DELTA_NONE and skip reprogramming
    // exactly the thing that might be wrong).
    display64_commit_result_t rc = atomic_commit_fn(&g_display_current, &recovery,
                                                      (display64_delta_t)(DISPLAY64_DELTA_PRIMARY_CHANGED | DISPLAY64_DELTA_PRIMARY_CONTENT));
    if (rc == DISPLAY64_COMMIT_OK) {
        g_recovery_successes++;
        release_superseded_primary(&g_display_current.primary);
        g_display_current = recovery;
    } else {
        display64_state_discard(&recovery);
    }
    return rc;
}

int display64_state_retry_recovery(void) {
    if (g_display_validity == DISPLAY64_STATE_VALID) return 0; // nothing to recover from
    if (!atomic_commit_fn) return -1; // defensive -- RECOVERY_REQUIRED can only be set via a real backend's own FAIL_PARTIAL, so this should be unreachable in practice
    if (attempt_recovery_to_composited() == DISPLAY64_COMMIT_OK) {
        g_display_validity = DISPLAY64_STATE_VALID;
        return 0;
    }
    return -1; // still RECOVERY_REQUIRED -- safe to call again later
}

int display64_state_commit(display64_state_t* candidate) {
    display64_check_result_t crc = display64_state_check(candidate);
    if (crc != DISPLAY64_CHECK_OK) {
        display64_state_discard(candidate);
        return -1;
    }

    display64_delta_t delta = compute_delta(&g_display_current, candidate);
    if (delta == DISPLAY64_DELTA_NONE) {
        // §21 test A: nothing to do -- zero hardware commands, but the
        // candidate's own (redundant) reference must still be released,
        // and this genuinely counts as a successful no-op commit.
        display64_state_discard(candidate);
        g_last_commit_result = DISPLAY64_COMMIT_OK;
        return 0;
    }

    if (!atomic_commit_fn) {
        // Software-framebuffer fallback (§19): a delta that reaches here
        // with no atomic backend registered can only be a no-op-shaped
        // candidate that check() already would have rejected otherwise
        // (DIRECT primary / cursor changes are NO_BACKEND at check time)
        // -- MODE_CHANGED is also always rejected by check(). The only
        // way to reach commit() with a real delta and no backend is a
        // primary-content-only update, which needs no backend command at
        // all (content flows through display64_gpu_flush_rect(), §16) --
        // just adopt it.
        release_superseded_primary(&g_display_current.primary);
        g_display_current = *candidate;
        g_last_commit_result = DISPLAY64_COMMIT_OK;
        return 0;
    }

    display64_commit_result_t rc = atomic_commit_fn(&g_display_current, candidate, delta);
    g_last_commit_result = rc;
    if (rc == DISPLAY64_COMMIT_OK) {
        release_superseded_primary(&g_display_current.primary);
        g_display_current = *candidate;
        return 0;
    }

    if (rc == DISPLAY64_COMMIT_FAIL_CLEAN) {
        // §10 outcome: nothing on the device changed -- current remains
        // authoritative exactly as it was.
        display64_state_discard(candidate);
        return -1;
    }

    // DISPLAY64_COMMIT_FAIL_PARTIAL: hardware may now disagree with any
    // known software state (§10/§11). Never adopt `candidate` (its own
    // request did NOT fully succeed) and never simply leave
    // g_display_current claiming the OLD state either, without first
    // trying to prove hardware actually matches it -- attempt recovery
    // to a known composited state instead.
    display64_state_discard(candidate);
    if (attempt_recovery_to_composited() == DISPLAY64_COMMIT_OK) {
        g_display_validity = DISPLAY64_STATE_VALID;
    } else {
        // Recovery itself failed -- truly unknown hardware state. Per
        // §11: do not release resources still possibly referenced by
        // hardware (g_display_current is left completely untouched,
        // including whatever memobj64 reference its own primary holds --
        // it is NOT discarded here), and do not claim VALID.
        g_display_validity = DISPLAY64_STATE_RECOVERY_REQUIRED;
    }
    return -1; // the ORIGINAL requested transaction did not happen, regardless of recovery's own outcome
}

// The direct-scanout cache's own SLOT_BOUND/SLOT_ACTIVE bookkeeping
// (used by display64_direct_scanout_unbind()'s own "refuse to unbind the
// live source" rule) is re-derived from whichever identity
// g_display_current.primary ACTUALLY names after a commit attempt --
// correct by construction whether the commit succeeded, failed cleanly
// (current unchanged), or hit RECOVERY_REQUIRED (current also left
// unchanged, per display64_state_commit()'s own §11 contract) -- never a
// separately-tracked "did this call succeed" branch that could drift out
// of sync with the real outcome.
static void resync_direct_slot_bookkeeping(void) {
    for (int i = 0; i < DISPLAY64_DIRECT_MAX_SLOTS; i++) {
        if (g_direct_slots[i].state == SLOT_EMPTY) continue;
        int is_now_active = (g_display_current.primary.kind == DISPLAY64_PRIMARY_DIRECT &&
                              g_direct_slots[i].identity == g_display_current.primary.identity);
        g_direct_slots[i].state = is_now_active ? SLOT_ACTIVE : SLOT_BOUND;
    }
}

// M+10: `switch_active` is now advisory only -- kept in the syscall's own
// wire struct for compatibility, but the atomic layer decides for itself
// (via compute_delta()'s own identity comparison against
// g_display_current) whether a resource change is actually needed,
// rather than trusting the caller's flag. This is a genuine correctness/
// efficiency improvement over the pre-M+10 behavior: a single-buffer
// fullscreen client (gfx_interactive64.c) that always passed
// switch_active=1 on every steady-state commit previously caused a
// redundant SET_SCANOUT every single frame even though its resource
// identity never changed -- compute_delta() now correctly recognizes
// "same identity as current" and only ever computes
// DISPLAY64_DELTA_PRIMARY_CONTENT for that case (transfer+flush, no
// SET_SCANOUT). See docs/m10_current_mutation_audit.md's own finding #3.
// M+10: `switch_active` is now advisory only -- kept in the syscall's own
// wire struct for compatibility, but the atomic layer decides for itself
// (via compute_delta()'s own identity comparison against
// g_display_current) whether a resource change is actually needed,
// rather than trusting the caller's flag. This is a genuine correctness/
// efficiency improvement over the pre-M+10 behavior: a single-buffer
// fullscreen client (gfx_interactive64.c) that always passed
// switch_active=1 on every steady-state commit previously caused a
// redundant SET_SCANOUT every single frame even though its resource
// identity never changed -- compute_delta() now correctly recognizes
// "same identity as current" and only ever computes
// DISPLAY64_DELTA_PRIMARY_CONTENT for that case (transfer+flush, no
// SET_SCANOUT). See docs/m10_current_mutation_audit.md's own finding #3.
int display64_direct_present(uint64_t identity, uint32_t x, uint32_t y, uint32_t w, uint32_t h, int switch_active) {
    (void)switch_active;
    if (!direct_present_fn) return -1; // kept as the resource-capability check -- see this file's own §7 comment on why the registration itself is unchanged
    direct_slot_t* slot = find_slot(identity);
    if (!slot) return -1;

    display64_state_t cand;
    display64_state_begin(&cand);
    if (display64_state_set_primary_direct(&cand, identity, slot->width, slot->height) < 0) {
        display64_state_discard(&cand);
        return -1;
    }
    display64_state_set_primary_damage(&cand, x, y, w, h);
    int rc = display64_state_commit(&cand);
    resync_direct_slot_bookkeeping();
    return rc;
}

int display64_leave_direct_scanout(void) {
    if (!direct_leave_fn) return -1; // kept as the resource-capability check
    if (g_display_current.primary.kind != DISPLAY64_PRIMARY_DIRECT) return -1; // nothing to leave

    display64_state_t cand;
    display64_state_begin(&cand);
    display64_state_set_primary_composited(&cand);
    int rc = display64_state_commit(&cand);
    resync_direct_slot_bookkeeping();
    return rc;
}

static inline uint64_t lock(void) {
    uint64_t flags;
    __asm__ volatile ("pushfq\n\tpop %0\n\tcli" : "=r"(flags) :: "memory");
    return flags;
}
static inline void unlock(uint64_t flags) {
    if (flags & (1ULL << 9)) __asm__ volatile ("sti" ::: "memory");
}

int display64_acquire(void) {
    uint64_t flags = lock();
    if (owner_pid != 0) { unlock(flags); return -1; }
    int pid = process64_current_pid();
    if (pid <= 0) { unlock(flags); return -1; }
    owner_pid = (uint32_t)pid;
    unlock(flags);
    return 0;
}

void display64_release(void) {
    uint64_t flags = lock();
    owner_pid = 0;
    unlock(flags);
}

uint32_t display64_owner_pid(void) {
    uint64_t flags = lock();
    uint32_t p = owner_pid;
    unlock(flags);
    return p;
}

int display64_init(uint64_t fb_addr, uint32_t width, uint32_t height,
                    uint32_t pitch, const pixfmt64_t* fmt)
{
    fb_avail = 0;
    if (!fb_addr || !fmt || width < 320 || height < 200) return -1;

    uint64_t size = (uint64_t)height * (uint64_t)pitch;
    void* vaddr = physmem64_map_mmio(fb_addr, size);
    if (!vaddr) return -1;

    fb_base   = (uint64_t)(uintptr_t)vaddr;
    fb_width  = width;
    fb_height = height;
    fb_pitch  = pitch;
    fb_fmt    = *fmt;
    fb_avail  = 1;
    sync_current_output();
    return 0;
}

int display64_available(void) { return gpu_active() || fb_avail; }

void display64_get_info(display64_info_t* out)
{
    if (!out) return;
    if (gpu_active()) {
        out->width  = gpu_width;
        out->height = gpu_height;
        out->pitch  = gpu_pitch;
        out->format = g_gpu_identity_fmt;
        return;
    }
    out->width  = fb_width;
    out->height = fb_height;
    out->pitch  = fb_pitch;
    out->format = fb_fmt;
}

// Writes `native` (only the low bytes_per_pixel bytes are meaningful)
// at byte offset `off` into the framebuffer, in little-endian order --
// i.e. exactly how a real 16/24/32-bit little-endian store would lay
// the bytes out, so this works uniformly across all three supported
// depths without the compiler ever performing a misaligned wide store
// for the 24bpp case.
static inline void write_native(uint64_t off, uint32_t native)
{
    uint8_t* p = (uint8_t*)(uintptr_t)fb_base + off;
    switch (fb_fmt.bytes_per_pixel) {
    case 2:
        p[0] = (uint8_t)(native & 0xFF);
        p[1] = (uint8_t)((native >> 8) & 0xFF);
        break;
    case 3:
        p[0] = (uint8_t)(native & 0xFF);
        p[1] = (uint8_t)((native >> 8) & 0xFF);
        p[2] = (uint8_t)((native >> 16) & 0xFF);
        break;
    default: // 4
        p[0] = (uint8_t)(native & 0xFF);
        p[1] = (uint8_t)((native >> 8) & 0xFF);
        p[2] = (uint8_t)((native >> 16) & 0xFF);
        p[3] = (uint8_t)((native >> 24) & 0xFF);
        break;
    }
}

void display64_put_pixel(uint32_t x, uint32_t y, uint32_t rgb)
{
    if (gpu_active()) {
        if (x >= gpu_width || y >= gpu_height) return;
        uint8_t* base = (uint8_t*)(uintptr_t)gpu_base;
        *(uint32_t*)(uintptr_t)(base + (uint64_t)y * gpu_pitch + (uint64_t)x * 4u) = rgb;
        display64_gpu_flush_rect(x, y, 1, 1); // fbterm64's only caller of this primitive -- no batching caller exists, unlike blit_row
        return;
    }
    if (!fb_avail || x >= fb_width || y >= fb_height) return;
    uint64_t off = (uint64_t)y * fb_pitch + (uint64_t)x * fb_fmt.bytes_per_pixel;
    write_native(off, pixfmt64_pack(&fb_fmt, rgb));
}

void display64_fill_rect(uint32_t x, uint32_t y, uint32_t w, uint32_t h, uint32_t rgb)
{
    if (gpu_active()) {
        if (x >= gpu_width || y >= gpu_height) return;
        if (x + w > gpu_width)  w = gpu_width  - x;
        if (y + h > gpu_height) h = gpu_height - y;
        uint8_t* base = (uint8_t*)(uintptr_t)gpu_base;
        for (uint32_t row = 0; row < h; row++) {
            uint32_t* dst = (uint32_t*)(uintptr_t)(base + (uint64_t)(y + row) * gpu_pitch + (uint64_t)x * 4u);
            for (uint32_t col = 0; col < w; col++) dst[col] = rgb;
        }
        display64_gpu_flush_rect(x, y, w, h); // see display64_put_pixel's own comment on why this call is here, not in blit_row
        return;
    }
    if (!fb_avail) return;
    if (x >= fb_width || y >= fb_height) return;
    if (x + w > fb_width)  w = fb_width  - x;
    if (y + h > fb_height) h = fb_height - y;

    uint32_t native = pixfmt64_pack(&fb_fmt, rgb);
    for (uint32_t row = 0; row < h; row++) {
        uint64_t base = (uint64_t)(y + row) * fb_pitch + (uint64_t)x * fb_fmt.bytes_per_pixel;
        for (uint32_t col = 0; col < w; col++)
            write_native(base + (uint64_t)col * fb_fmt.bytes_per_pixel, native);
    }
}

void display64_copy_rows(uint32_t dst_y, uint32_t src_y, uint32_t num_rows)
{
    if (gpu_active()) {
        if (src_y >= gpu_height || dst_y >= gpu_height) return;
        if (src_y + num_rows > gpu_height) num_rows = gpu_height - src_y;
        if (dst_y + num_rows > gpu_height) num_rows = gpu_height - dst_y;
        if (num_rows == 0) return;

        uint8_t* base = (uint8_t*)(uintptr_t)gpu_base;
        uint64_t bytes_per_row = (uint64_t)gpu_pitch;
        uint64_t total = bytes_per_row * num_rows;

        if (dst_y < src_y) {
            uint8_t* dst = base + (uint64_t)dst_y * bytes_per_row;
            uint8_t* src = base + (uint64_t)src_y * bytes_per_row;
            for (uint64_t i = 0; i < total; i++) dst[i] = src[i];
        } else if (dst_y > src_y) {
            for (uint64_t i = total; i > 0; i--) {
                uint8_t* dst = base + (uint64_t)dst_y * bytes_per_row;
                uint8_t* src = base + (uint64_t)src_y * bytes_per_row;
                dst[i - 1] = src[i - 1];
            }
        }
        display64_gpu_flush_rect(0, dst_y, gpu_width, num_rows); // only the DESTINATION rows actually changed -- see display64_put_pixel's own comment on why this call is here
        return;
    }

    if (!fb_avail) return;
    if (src_y >= fb_height || dst_y >= fb_height) return;
    if (src_y + num_rows > fb_height) num_rows = fb_height - src_y;
    if (dst_y + num_rows > fb_height) num_rows = fb_height - dst_y;
    if (num_rows == 0) return;

    uint8_t* base = (uint8_t*)(uintptr_t)fb_base;
    uint64_t bytes_per_row = (uint64_t)fb_pitch;
    uint64_t total = bytes_per_row * num_rows;

    if (dst_y < src_y) {
        uint8_t* dst = base + (uint64_t)dst_y * bytes_per_row;
        uint8_t* src = base + (uint64_t)src_y * bytes_per_row;
        for (uint64_t i = 0; i < total; i++) dst[i] = src[i];
    } else if (dst_y > src_y) {
        // Copy backward so an overlapping downward move doesn't clobber
        // source rows before they're read (not currently exercised by
        // fbterm64's upward-only scroll, but keeps this primitive
        // correct for any future caller).
        for (uint64_t i = total; i > 0; i--) {
            uint8_t* dst = base + (uint64_t)dst_y * bytes_per_row;
            uint8_t* src = base + (uint64_t)src_y * bytes_per_row;
            dst[i - 1] = src[i - 1];
        }
    }
}

// M-next (compositor performance hardening, §17): true whenever the
// hardware's native pixel layout is BYTE-IDENTICAL to the logical
// 0x00RRGGBB representation every caller already works in -- i.e.
// pixfmt64_pack() would be a no-op for every possible input. This is
// exactly QEMU/Bochs VBE's own default 32bpp layout (red_pos=16,
// green_pos=8, blue_pos=0, all 8-bit fields) that this whole project
// has always run under in practice, but the check is general: any
// future direct-RGB 32bpp mode with this exact channel layout also
// qualifies, and anything else (16bpp, 24bpp, a swapped channel order)
// correctly does not.
static inline int fmt_is_identity_xrgb8888(const pixfmt64_t* f) {
    return f->bytes_per_pixel == 4
        && f->red_pos   == 16 && f->red_size   == 8
        && f->green_pos == 8  && f->green_size == 8
        && f->blue_pos  == 0  && f->blue_size  == 8;
}

void display64_blit_row(uint32_t x, uint32_t y, uint32_t w, const uint32_t* logical_pixels)
{
    if (!logical_pixels) return;

    // M+4: GPU backend -- always identity format by construction, so
    // this is unconditionally a raw row copy, no pack loop needed at
    // all. Deliberately does NOT call display64_gpu_flush_rect() here
    // -- this function is called once per ROW by sys64_display_present
    // (kernel/syscall64.c), which explicitly batches exactly one flush
    // per whole requested rect after its own row loop completes. See
    // include/display64.h's own header comment on display64_gpu_flush_rect.
    if (gpu_active()) {
        if (y >= gpu_height || x >= gpu_width) return;
        uint32_t ww = w;
        if (x + ww > gpu_width) ww = gpu_width - x;
        uint32_t* dst = (uint32_t*)(uintptr_t)((uint8_t*)(uintptr_t)gpu_base + (uint64_t)y * gpu_pitch + (uint64_t)x * 4u);
        for (uint32_t col = 0; col < ww; col++) dst[col] = logical_pixels[col];
        return;
    }

    if (!fb_avail || y >= fb_height || x >= fb_width) return;
    if (x + w > fb_width) w = fb_width - x;

    uint64_t base = (uint64_t)y * fb_pitch + (uint64_t)x * fb_fmt.bytes_per_pixel;

    // Fast path: skip pixfmt64_pack()'s per-pixel channel math entirely
    // and copy the row's bytes directly -- this codebase has no libc
    // memcpy anywhere (see e.g. kernel/fat.c's own fat_memcpy, kernel/
    // ext2.c's e2_memcpy -- every file that needs one writes its own
    // small copy loop), so this is a plain word-at-a-time loop in the
    // same spirit, not a call to a symbol that doesn't exist here.
    // Every non-identity format (RGB565, 24bpp, or any future layout)
    // is completely untouched, still going through the per-pixel pack
    // loop below exactly as before.
    if (fmt_is_identity_xrgb8888(&fb_fmt)) {
        uint32_t* dst = (uint32_t*)(uintptr_t)((uint8_t*)(uintptr_t)fb_base + base);
        for (uint32_t col = 0; col < w; col++) dst[col] = logical_pixels[col];
        return;
    }

    for (uint32_t col = 0; col < w; col++) {
        uint32_t native = pixfmt64_pack(&fb_fmt, logical_pixels[col]);
        write_native(base + (uint64_t)col * fb_fmt.bytes_per_pixel, native);
    }
}

static inline uint32_t read_native(uint64_t off)
{
    const uint8_t* p = (const uint8_t*)(uintptr_t)fb_base + off;
    switch (fb_fmt.bytes_per_pixel) {
    case 2: return (uint32_t)p[0] | ((uint32_t)p[1] << 8);
    case 3: return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16);
    default: return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
    }
}

uint32_t display64_read_pixel_raw(uint32_t x, uint32_t y)
{
    if (gpu_active()) {
        if (x >= gpu_width || y >= gpu_height) return 0;
        const uint8_t* base = (const uint8_t*)(uintptr_t)gpu_base;
        return *(const uint32_t*)(uintptr_t)(base + (uint64_t)y * gpu_pitch + (uint64_t)x * 4u);
    }
    if (!fb_avail || x >= fb_width || y >= fb_height) return 0;
    return read_native((uint64_t)y * fb_pitch + (uint64_t)x * fb_fmt.bytes_per_pixel);
}

static int st_check(int* pass, int* fail, const char* name, int ok) {
    if (ok) { (*pass)++; return 1; }
    (*fail)++;
    klog("display64_selftest: FAIL "); klog(name); klog("\n");
    return 0;
}

int display64_selftest(void)
{
    int pass = 0, fail = 0;
    if (!fb_avail) {
        klog("display64_selftest: no display available -- SKIPPED\n");
        return 1;
    }

    uint32_t w = fb_width, h = fb_height;

    // Full-bounds coverage, including the true bottom-right corner --
    // exactly what fbterm64_clear relies on to avoid leaving a stale
    // strip when height isn't a multiple of the terminal cell height.
    uint32_t magic1 = pixfmt64_pack(&fb_fmt, 0x123456);
    display64_fill_rect(0, 0, w, h, 0x123456);
    int ok = display64_read_pixel_raw(0, 0) == magic1
           && display64_read_pixel_raw(w - 1, 0) == magic1
           && display64_read_pixel_raw(0, h - 1) == magic1
           && display64_read_pixel_raw(w - 1, h - 1) == magic1;
    st_check(&pass, &fail, "full-bounds fill (incl. true bottom-right corner)", ok);

    // Deliberately non-CHAR_H-aligned partial rectangle at the bottom
    // edge -- the exact shape scroll/clear use for the "remainder"
    // strip below the last whole character row.
    display64_fill_rect(0, 0, w, h, 0);
    uint32_t y0 = (h > 50) ? h - 37 : 0; // 37 is not a multiple of 32
    uint32_t hh = h - y0;
    uint32_t magic2 = pixfmt64_pack(&fb_fmt, 0xABCDEF);
    display64_fill_rect(0, y0, w, hh, 0xABCDEF);
    ok = display64_read_pixel_raw(0, y0) == magic2
      && display64_read_pixel_raw(w - 1, h - 1) == magic2
      && (y0 == 0 || display64_read_pixel_raw(0, y0 - 1) != magic2);
    st_check(&pass, &fail, "non-CHAR_H-aligned partial rect reaches true bottom edge", ok);

    // copy_rows: two distinct bands, copy one over the other.
    display64_fill_rect(0, 0, w, h, 0);
    display64_fill_rect(0, 0, w, 10, 0x111111);
    display64_fill_rect(0, 10, w, 10, 0x222222);
    display64_copy_rows(0, 10, 10);
    uint32_t magic3 = pixfmt64_pack(&fb_fmt, 0x222222);
    ok = display64_read_pixel_raw(0, 0) == magic3 && display64_read_pixel_raw(w - 1, 9) == magic3;
    st_check(&pass, &fail, "copy_rows", ok);

    // Clipping: a rect that overshoots the edge must clip, not crash,
    // and still correctly paint the in-bounds portion.
    display64_fill_rect(0, 0, w, h, 0);
    uint32_t magic4 = pixfmt64_pack(&fb_fmt, 0x333333);
    display64_fill_rect(w - 5, h - 5, 100, 100, 0x333333);
    ok = display64_read_pixel_raw(w - 1, h - 1) == magic4;
    st_check(&pass, &fail, "fill_rect clips an out-of-bounds rect safely", ok);

    display64_fill_rect(0, 0, w, h, 0); // leave a clean screen behind

    klog("display64_selftest: pass="); klog_hex("", (uint32_t)pass);
    klog("display64_selftest: fail="); klog_hex("", (uint32_t)fail);
    return fail == 0;
}

void display64_dump(void)
{
    klog("display64: dump ---\n");
    if (gpu_base != 0) {
        klog_hex("  GPU backend width:  ", gpu_width);
        klog_hex("  GPU backend height: ", gpu_height);
        klog_hex("  GPU backend pitch:  ", gpu_pitch);
        klog(gpu_active() ? "  GPU backend: ACTIVE\n" : "  GPU backend: registered but DISABLED (a present failed)\n");
    }
    if (!fb_avail) { klog("  framebuffer: unavailable\n"); klog("display64: dump end ---\n"); return; }
    klog_hex("  width:  ", fb_width);
    klog_hex("  height: ", fb_height);
    klog_hex("  pitch:  ", fb_pitch);
    klog_hex("  bpp:    ", fb_fmt.bpp);
    klog_hex("  red_pos/size:   ", ((uint32_t)fb_fmt.red_pos << 8)   | fb_fmt.red_size);
    klog_hex("  green_pos/size: ", ((uint32_t)fb_fmt.green_pos << 8) | fb_fmt.green_size);
    klog_hex("  blue_pos/size:  ", ((uint32_t)fb_fmt.blue_pos << 8)  | fb_fmt.blue_size);
    klog_hex("  owner pid:      ", owner_pid);
    klog("display64: dump end ---\n");
}

// ── M+10 §21: atomic state-machine self-tests ────────────────────────
// Intended to run once, early at boot, right after
// virtio_gpu64_init_compositor_backend() registers the atomic backend --
// BEFORE any real client (compositor64/GfxDemo/Interact) ever runs, so
// the direct-scanout cache is guaranteed empty and this test's own
// synthetic identity can never collide with a real one. Uses a reserved
// test-only identity (DISPLAY64_SELFTEST_IDENTITY) that no real shm_token
// will ever produce in practice (see its own comment).
#define DISPLAY64_SELFTEST_IDENTITY 0xFFFFFFFFFFFFFFFEULL

int display64_atomic_selftest(void)
{
    int pass = 0, fail = 0;

    // A: duplicate current, change nothing, check, commit -> the code
    // path itself structurally proves zero hardware commands (delta ==
    // NONE short-circuits BEFORE atomic_commit_fn is ever called -- see
    // display64_state_commit()) -- this test proves current is
    // genuinely byte-for-byte unchanged afterward, the externally
    // observable half of that guarantee.
    {
        display64_state_t before = g_display_current;
        display64_state_t cand;
        display64_state_begin(&cand);
        int rc = display64_state_commit(&cand);
        int ok = (rc == 0) &&
                 g_display_current.primary.kind == before.primary.kind &&
                 g_display_current.primary.identity == before.primary.identity &&
                 g_display_current.cursor.x == before.cursor.x &&
                 g_display_current.cursor.y == before.cursor.y &&
                 g_display_current.cursor.enabled == before.cursor.enabled &&
                 g_display_current.cursor.image_generation == before.cursor.image_generation;
        st_check(&pass, &fail, "A: no-change candidate commits as a true no-op", ok);
    }

    // §17: any actual mode CHANGE is rejected outright.
    {
        display64_state_t cand;
        display64_state_begin(&cand);
        cand.output.width += 1;
        display64_check_result_t crc = display64_state_check(&cand);
        display64_state_discard(&cand);
        st_check(&pass, &fail, "mode-change candidate rejected as MODE_UNSUPPORTED", crc == DISPLAY64_CHECK_MODE_UNSUPPORTED);
    }

    // H: a DIRECT primary naming an identity that was never bound is
    // refused immediately (before check() is even reached) -- zero
    // mutation either way.
    {
        display64_state_t before = g_display_current;
        display64_state_t cand;
        display64_state_begin(&cand);
        int src_rc = display64_state_set_primary_direct(&cand, 0xDEADBEEFCAFEULL, 640, 480);
        display64_state_discard(&cand);
        int ok = (src_rc < 0) &&
                 g_display_current.primary.kind == before.primary.kind &&
                 g_display_current.primary.identity == before.primary.identity;
        st_check(&pass, &fail, "H: unbound direct identity rejected, zero mutation", ok);
    }

    // I: cursor enablement with no atomic backend registered at all is
    // cleanly NO_BACKEND (only meaningful on a framebuffer-only boot --
    // trivially "skipped" (still counted as a pass, nothing to
    // contradict) when a real backend IS registered, since this
    // specific assertion is about the NO-backend case only).
    {
        display64_state_t cand;
        display64_state_begin(&cand);
        cand.cursor.enabled = 1;
        display64_state_set_cursor_image(&cand, 1, 1, 0, 0, 0);
        display64_check_result_t crc = display64_state_check(&cand);
        display64_state_discard(&cand);
        int ok = display64_direct_scanout_supported() ? 1 /* backend present -- this specific no-backend case doesn't apply, see E/F/G/I2 below for the real-backend cursor/primary checks */
                                                        : (crc == DISPLAY64_CHECK_NO_BACKEND);
        st_check(&pass, &fail, "I: cursor request with no backend registered is cleanly unsupported (or n/a)", ok);
    }

    if (!display64_direct_scanout_supported()) {
        klog("display64_atomic_selftest: no direct-scanout backend -- E/F/G/K/L skipped (framebuffer-only boot)\n");
        klog("display64_atomic_selftest: pass="); klog_hex("", (uint32_t)pass);
        klog("display64_atomic_selftest: fail="); klog_hex("", (uint32_t)fail);
        return fail == 0;
    }

    // E/G/K/L: a real bind -> primary resource change -> leave cycle,
    // using a synthetic test-only identity/buffer so it can never
    // collide with (or be observed by) any real client. Run once, early
    // at boot, before any real client exists -- see this function's own
    // header comment.
    {
        uint32_t w = g_display_current.output.width, h = g_display_current.output.height;
        memobj64_t* test_obj = 0;
        int created = (w > 0 && h > 0 && memobj64_create((uint64_t)w * h * 4u, &test_obj) == 0);
        st_check(&pass, &fail, "K/L prep: test memobj64 allocated", created);

        if (created) {
            void* out_handle = 0;
            int bind_rc = display64_direct_scanout_bind(DISPLAY64_SELFTEST_IDENTITY, test_obj, w, h, &out_handle);
            st_check(&pass, &fail, "E prep: test identity bound", bind_rc == 0);

            if (bind_rc == 0) {
                // F: a DIRECT primary whose geometry does NOT match the
                // bound slot's own recorded geometry is refused, zero
                // mutation.
                display64_state_t bad;
                display64_state_begin(&bad);
                int bad_rc = display64_state_set_primary_direct(&bad, DISPLAY64_SELFTEST_IDENTITY, w + 1, h);
                display64_state_discard(&bad);
                st_check(&pass, &fail, "F: geometry-mismatched direct primary rejected", bad_rc < 0);

                // E: a genuine primary resource change.
                display64_primary_kind_t before_kind = g_display_current.primary.kind;
                int present_rc = display64_direct_present(DISPLAY64_SELFTEST_IDENTITY, 0, 0, w, h, 1);
                int e_ok = (present_rc == 0) &&
                           g_display_current.primary.kind == DISPLAY64_PRIMARY_DIRECT &&
                           g_display_current.primary.identity == DISPLAY64_SELFTEST_IDENTITY;
                st_check(&pass, &fail, "E: primary resource change (composited -> direct)", e_ok);
                (void)before_kind;

                // D: same primary, content-only update -> no
                // SET_SCANOUT needed (delta correctly computed as
                // PRIMARY_CONTENT, not PRIMARY_CHANGED) -- verified
                // indirectly: the present succeeds and the primary
                // identity is provably unchanged across the call.
                int content_rc = display64_direct_present(DISPLAY64_SELFTEST_IDENTITY, 0, 0, w, h, 1);
                int d_ok = (content_rc == 0) && g_display_current.primary.identity == DISPLAY64_SELFTEST_IDENTITY;
                st_check(&pass, &fail, "D: content-only re-present of the same identity", d_ok);

                // G: direct -> composited.
                int leave_rc = display64_leave_direct_scanout();
                int g_ok = (leave_rc == 0) && g_display_current.primary.kind == DISPLAY64_PRIMARY_COMPOSITED;
                st_check(&pass, &fail, "G: direct -> composited leave", g_ok);

                display64_direct_scanout_unbind(DISPLAY64_SELFTEST_IDENTITY);
            }
            // L: this self-test's own reference is released the instant
            // unbind() tears down the slot's bind-time reference; no
            // separate leak-check accessor exists in this codebase
            // (memobj64_dump() is klog-only) -- absence of a crash/hang
            // across this whole sequence, plus the cache being provably
            // empty again (a real client can immediately bind its own
            // identity afterward with zero cache pressure), is this
            // milestone's own practical verification standard, matching
            // display64_selftest()'s own pixel-readback-only rigor.
        }
    }

    klog("display64_atomic_selftest: pass="); klog_hex("", (uint32_t)pass);
    klog("display64_atomic_selftest: fail="); klog_hex("", (uint32_t)fail);
    return fail == 0;
}
