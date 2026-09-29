// ToxenOS/user64/wmclient64.h — Milestone 30: reusable compositor
// client library. Hides connection bootstrap, shared-memory surface
// creation/mapping, and protocol message construction from graphical
// applications -- see user64/gfx_demo64.c/user64/gfx_interactive64.c
// for how little an application needs to know to use this.
//
// A client program using this library never touches sys_get_args,
// sys_pipe_*, sys_shm_*, or wm_msg_t directly for the common path.
//
// ── M+10A protocol redesign: ONE authoritative dispatcher ──────────────
// Studied against upstream Wayland/Weston/wlroots (see the M+10A
// protocol report for the full comparison): libwayland never has
// multiple independent call sites each doing their own "read the wire,
// filter for message X, shove everything else in a side array" loop --
// there is exactly one dispatcher, and every blocking convenience call
// is built ON TOP of it by polling authoritative per-object state, never
// by re-reading the wire itself.
//
// The PRE-M+10A version of this header had exactly the anti-pattern
// upstream avoids: wmc_recv_expecting()/wm_wait_buffer_released()/
// wm_wait_frame() each read the wire directly and pushed whatever they
// didn't want into a shared, size-bounded, EVICTABLE pending[] array --
// including WM_MSG_FRAME/BUFFER_RELEASED/CONFIGURE, none of which are
// actually events (each names a fact that supersedes or idempotently
// updates prior state, exactly like Wayland's wl_buffer.release/
// wl_surface.frame/xdg_surface.configure). Static audit proved this
// design could permanently strand or evict a liveness-critical message:
// compositor64.c's own last_scanout_released_slot bug (a single scalar
// representing a fact that can independently be true for TWO different
// buffer slots at once) could fire a genuine duplicate BUFFER_RELEASED
// with no consumer, permanently occupying one of only WMC_PENDING_MAX
// queue slots -- and once enough of those accumulated, the oldest-
// eviction fallback could discard a FRAME/BUFFER_RELEASED/CONFIGURE a
// future wait call would need, which then blocks forever, because the
// compositor has no reason to ever resend a message it already sent.
//
// This version has exactly one raw-wire entry point (wmc_dispatch_one(),
// via wmc_recv_classified() for the read itself) and one place that
// updates state from a received message (wmc_apply_message()). Every
// blocking helper below is now shaped like Wayland's own
// wl_display_roundtrip() idiom:
//
//     while (!condition) wmc_dispatch_one(c, NULL);
//
// never like the old:
//
//     while (recv() != message_I_want) push_other_message_into_fifo();
//
// State vs. ordered events (see the M+10A report's own invariant B):
//   STATE (idempotent, supersedable, never queued as a message):
//     - buffer ownership (wm_buffer_t.owned_by_compositor, per token)
//     - window configure/fullscreen state + generation (wm_window_t)
//     - frame-callback permission + generation (wm_window_t)
//     - focus (wm_window_t.has_focus)
//   ORDERED EVENT STREAM (ordering is the whole point, never coalesced):
//     - WM_MSG_KEY_EVENT -- key_ring[], unchanged from the earlier
//       wmclient64 fix (see that fix's own comment on why coalescing
//       distinct press/release transitions is wrong)
//     - everything else (pointer motion/button/wheel, close request) --
//       handed back VERBATIM by wmc_dispatch_one()'s own out-param to
//       whichever caller is driving it. wm_wait_event() (the one
//       reactive-loop entry point that actually wants this, see
//       gfx_interactive64.c) is nothing more than "pop a queued key if
//       there is one, else dispatch one message and return it" -- no
//       separate queue is needed for these types because their one
//       consumer IS the same call site that dispatches them; nothing is
//       ever "queued for someone else to find later" the way the old
//       generic pending[] was.
//
// ACK/WELCOME/WINDOW_CREATED/ERROR are one-shot command replies, never
// ordered-stream data -- a single last-reply slot (wmc_wait_reply())
// replaces both the old ACK-is-always-discarded special case AND the
// old bug where an unexpected WM_MSG_ERROR was never recognized as a
// terminal reply at all, so a command that failed with an error instead
// of its expected reply type would hang forever.
#ifndef WMCLIENT64_H
#define WMCLIENT64_H
#include <stdint.h>
#include "tox64.h"
#include "wmproto64.h"

// key_ring[] sizing: see this header's own top comment and the earlier
// wmclient64 fix -- KEY_EVENT gets its own small ordered ring, sized
// generously for any realistic human input burst between two drains
// (repeat suppression already removes the only unbounded-producer case
// a real keyboard can generate). Under an intentionally-absurd synthetic
// storm this can still fill -- handled as an oldest-evicted overflow
// (see wmc_key_ring_push()), never by corrupting ordering to "fix" it.
// This is the ONE bounded, evictable queue this header still has, and
// it is limited to genuine ordered-event-stream data -- see this
// header's own top comment for why that is a fundamentally different
// case from the removed generic pending[].
#define WMC_KEY_RING_MAX 32
// Small fixed set of "currently down" keycodes this library dedups
// typematic repeats against -- generous for any realistic shortcut set
// (F11 today) without a general 256-key table. If more than this many
// DISTINCT keys are ever held at once, repeat suppression simply stops
// applying to the overflow key; every message still gets applied
// normally either way, so this is a missed optimization for an
// unrealistic case, never a correctness or data-loss issue.
#define WMC_TRACKED_DOWN_MAX 4
// Small fixed registries the dispatcher updates by ID -- generous for
// any realistic client (GfxDemo needs 4 buffers/1 window; Interact needs
// 2 buffers/1 window). The caller owns the actual wm_buffer_t/wm_window_t
// memory (same convention wm_buffer_t already established); these arrays
// only ever hold POINTERS into memory the caller guarantees outlives the
// connection (both current clients' buffers/window state live in their
// own never-returning _start() stack frame).
#define WMC_MAX_TRACKED_BUFFERS 4
#define WMC_MAX_TRACKED_WINDOWS 2

// ── M+10A: per-window authoritative state ──────────────────────────────
// Registered once by wm_create_window() (which fills window_id and
// zeroes everything else) and updated ONLY by wmc_apply_message() from
// then on -- application code reads these fields, never writes them
// (the one exception is frame_requested, which wm_request_frame() sets
// the instant it actually sends, to avoid a redundant send -- see that
// function's own comment).
typedef struct {
    uint32_t window_id;
    // WM_MSG_CONFIGURE state -- see wmproto64.h's own comment. Applied
    // only if the incoming generation is strictly greater than this one
    // (or none has ever been applied yet), so a burst of CONFIGURE 40,
    // 41, 42 delivered in any read granularity always converges on 42's
    // geometry/fullscreen state, never an intermediate one.
    uint32_t configure_generation;
    uint32_t content_w, content_h;
    int is_fullscreen;
    int configure_known; // has at least one CONFIGURE ever been applied
    // WM_MSG_FRAME permission -- one-shot, exactly like Wayland's own
    // wl_surface.frame callback: sending FRAME(generation) is a real
    // frame permission ONLY when generation matches configure_generation
    // at the moment it is dispatched (see wmc_apply_message() below for
    // the stale-generation case, which the dispatcher self-heals rather
    // than ever surfacing to application code). Cleared to 0 the instant
    // a caller consumes it (wm_wait_frame()).
    int frame_permission;
    int frame_requested; // a WM_MSG_REQUEST_FRAME is currently outstanding -- avoids a redundant send, not a correctness requirement
    int has_focus;
} wm_window_t;

// ── M+5: committed, double-buffered surfaces (Revision 2 §22) ───────
// A wm_buffer_t is one attached-but-not-necessarily-committed buffer --
// distinct from wm_attach_surface()'s single always-live surface. The
// client holds its own shm handle/mapping for the buffer's ENTIRE life
// (same "deliberately leaks the handle, released automatically on exit"
// convention wm_attach_surface() already uses) -- "committed" is a
// PROTOCOL-level contract (don't write it while the compositor owns it),
// not something the kernel's own page permissions enforce; the client's
// mapping stays writable throughout, exactly like a real Wayland
// client's own buffer contract.
//
// M+10A protocol redesign: owned_by_compositor is now updated in EXACTLY
// one place -- wmc_apply_message()'s own WM_MSG_BUFFER_RELEASED case,
// looked up by token against wm_client_t's tracked_buffers[] registry
// (populated automatically by wm_attach_buffer()). No caller ever reads
// a raw BUFFER_RELEASED message and flips this flag itself anymore.
typedef struct {
    int shm_handle;               // this process's own handle, held for the buffer's entire life
    uint64_t token;                // the wire identity of this buffer (== what WM_MSG_BUFFER_RELEASED names)
    uint32_t* pixels;              // mapped WRITABLE address
    uint32_t w, h, stride;
    int owned_by_compositor;       // true from wm_commit_buffer() until this buffer's own WM_MSG_BUFFER_RELEASED is applied
} wm_buffer_t;

// ── M+10A: wait-reason (retained from the earlier liveness investigation) ──
// Still the cheap, permanent "what is this client currently blocked on"
// signal a client's own liveness dump reads -- updated by the same
// condition-polling loops that replaced the old raw-wire waiters.
typedef enum {
    WM_WAIT_NONE = 0,
    WM_WAIT_FRAME,             // wm_wait_frame(): blocked specifically on frame permission (buffer already free)
    WM_WAIT_BUFFER_RELEASE,    // wm_wait_frame()/wm_wait_buffer_released(): blocked specifically on buffer ownership (frame permission already valid, or the buffer-only wait)
    WM_WAIT_FRAME_AND_RELEASE, // wm_wait_frame(): blocked on BOTH conditions, neither satisfied yet
    WM_WAIT_REPLY,             // wmc_wait_reply(): blocked on a one-shot command reply (WELCOME/WINDOW_CREATED/ACK)
    WM_WAIT_OTHER,             // wm_wait_event() or any other dispatch-driven wait
} wm_wait_reason_t;
static wm_wait_reason_t g_wmc_wait_reason = WM_WAIT_NONE;
static inline wm_wait_reason_t wm_get_wait_reason(void) { return g_wmc_wait_reason; }

// Permanent (not debug-build-gated) diagnostics -- cheap counters/one-
// shot latched reports, same "always on, silent unless something is
// actually wrong" precedent compositor64.c's own M9B_STATS/M+10A
// diagnostics already established. See item 9 of the M+10A protocol
// redesign for the exact invariants these are meant to prove.
static uint64_t g_wmc_received = 0;              // every message ever read off the wire (wmc_recv_classified)
static uint64_t g_wmc_key_queued = 0;             // KEY_EVENT appended to key_ring[]
static uint64_t g_wmc_key_dequeued = 0;           // KEY_EVENT removed from key_ring[] by any consumer
static uint64_t g_wmc_key_ring_high_water = 0;
static uint64_t g_wmc_key_overflow_attempts = 0;  // key_ring[] was full at push() time
static uint64_t g_wmc_key_lost = 0;               // MUST stay 0 under real (non-synthetic-storm) use
static uint64_t g_wmc_duplicate_release_count = 0; // a BUFFER_RELEASED arrived for a token already CLIENT_OWNED -- diagnosed, never corrupts state (idempotent)
static int g_wmc_duplicate_release_reported = 0;
static uint64_t g_wmc_stale_frame_regranted_count = 0; // a FRAME arrived whose generation had already been superseded -- self-healed via an automatic re-request, never satisfies a stale wait
static uint64_t g_wmc_unsolicited_frame_count = 0; // a FRAME arrived with no outstanding request -- ignored, never grants permission
static int g_wmc_unsolicited_frame_reported = 0;
static uint64_t g_wmc_duplicate_token_count = 0; // wm_attach_buffer() produced a token already held by another live tracked buffer -- see that function's own comment
static int g_wmc_duplicate_token_reported = 0;
static int g_wmc_eviction_reported = 0; // latches the one-shot key_ring[] overflow report

static inline const char* wmc_msg_type_name(uint32_t t);
static inline void wmc_put(const char* s);
static inline void wmc_put_u64(uint64_t v);

typedef struct {
    int req_w;      // this process's own handle: write end, client -> compositor
    int evt_r;      // this process's own handle: read end, compositor -> client
    uint32_t client_id;

    wm_msg_t key_ring[WMC_KEY_RING_MAX]; // ordered KEY_EVENT queue -- see this header's own top comment
    int key_ring_count;
    uint32_t down_keys[WMC_TRACKED_DOWN_MAX]; // repeat-suppression state -- see wmc_recv_classified()
    int down_count;

    wm_buffer_t* tracked_buffers[WMC_MAX_TRACKED_BUFFERS]; // registered by wm_attach_buffer()
    int tracked_buffer_count;
    wm_window_t* tracked_windows[WMC_MAX_TRACKED_WINDOWS]; // registered by wm_create_window()
    int tracked_window_count;

    // One-shot command-reply slot -- see wmc_wait_reply(). Safe as a
    // single slot (not a queue) because every command/reply exchange in
    // this library is synchronous from the caller's own perspective: a
    // client never has two outstanding ATTACH_BUFFER/CREATE_WINDOW/etc
    // requests at once, so there is never more than one "the next
    // ACK/WELCOME/WINDOW_CREATED/ERROR belongs to THIS wait" claim active
    // at a time.
    wm_msg_t last_reply;
    int has_last_reply;
    int32_t last_error_code; // WM_MSG_ERROR's own x field
    int has_last_error;
} wm_client_t;

static inline int wmc_strlen(const char* s) { int i = 0; while (s[i]) i++; return i; }

static inline int wmc_send(wm_client_t* c, const wm_msg_t* m) {
    int64_t n = sys_handle_write(c->req_w, (const char*)m, sizeof(*m));
    return (n == (int64_t)sizeof(*m)) ? 0 : -1;
}

// Blocks for the next raw message, completely unfiltered -- the ONLY
// function in this header that touches sys_handle_read directly.
static inline int wmc_recv_raw(wm_client_t* c, wm_msg_t* out) {
    int64_t n = sys_handle_read(c->evt_r, (char*)out, sizeof(*out));
    return (n == (int64_t)sizeof(*out)) ? 0 : -1;
}

// wmc_recv_classified(): applies keycode-repeat suppression (unchanged
// from the earlier wmclient64 fix) before anything else ever sees the
// message. A WM_MSG_KEY_EVENT press for a keycode already tracked as
// down is discarded here, before it could ever reach the dispatcher --
// this is what makes "exactly one logical press per physical down-edge"
// true regardless of how many raw MAKE codes the host/PS2 layer
// generates while a key is held. A release always clears the
// down-tracking and IS returned normally.
static inline int wmc_recv_classified(wm_client_t* c, wm_msg_t* out) {
    for (;;) {
        if (wmc_recv_raw(c, out) < 0) return -1;
        g_wmc_received++;

        if (out->type == WM_MSG_KEY_EVENT && out->pressed) {
            int already_down = 0;
            for (int i = 0; i < c->down_count; i++) {
                if (c->down_keys[i] == out->key_code) { already_down = 1; break; }
            }
            if (already_down) { continue; }
            if (c->down_count < WMC_TRACKED_DOWN_MAX) c->down_keys[c->down_count++] = out->key_code;
        } else if (out->type == WM_MSG_KEY_EVENT && !out->pressed) {
            for (int i = 0; i < c->down_count; i++) {
                if (c->down_keys[i] == out->key_code) { c->down_keys[i] = c->down_keys[--c->down_count]; break; }
            }
        }
        return 0;
    }
}

// wmc_key_ring_push()/wmc_key_ring_pop_front(): unchanged from the
// earlier wmclient64 fix -- ordered, never coalesced, oldest-evicted on
// overflow (a real-storm-only case, never normal traffic).
static inline void wmc_key_ring_push(wm_client_t* c, const wm_msg_t* m) {
    if (c->key_ring_count < WMC_KEY_RING_MAX) {
        c->key_ring[c->key_ring_count++] = *m;
        g_wmc_key_queued++;
        if ((uint64_t)c->key_ring_count > g_wmc_key_ring_high_water) g_wmc_key_ring_high_water = (uint64_t)c->key_ring_count;
        return;
    }
    g_wmc_key_overflow_attempts++;
    g_wmc_key_lost++;
    if (!g_wmc_eviction_reported) {
        g_wmc_eviction_reported = 1;
        wmc_put("*** M10A: key_ring[] overflow -- oldest KEY_EVENT evicted ***\n");
    }
    for (int i = 1; i < WMC_KEY_RING_MAX; i++) c->key_ring[i - 1] = c->key_ring[i];
    c->key_ring[WMC_KEY_RING_MAX - 1] = *m;
}

static inline int wmc_key_ring_pop_front(wm_client_t* c, wm_msg_t* out) {
    if (c->key_ring_count == 0) return 0;
    *out = c->key_ring[0];
    for (int i = 1; i < c->key_ring_count; i++) c->key_ring[i - 1] = c->key_ring[i];
    c->key_ring_count--;
    g_wmc_key_dequeued++;
    return 1;
}

static inline wm_buffer_t* wmc_find_tracked_buffer(wm_client_t* c, uint64_t token) {
    for (int i = 0; i < c->tracked_buffer_count; i++) {
        if (c->tracked_buffers[i]->token == token) return c->tracked_buffers[i];
    }
    return 0;
}

static inline wm_window_t* wmc_find_tracked_window(wm_client_t* c, uint32_t window_id) {
    for (int i = 0; i < c->tracked_window_count; i++) {
        if (c->tracked_windows[i]->window_id == window_id) return c->tracked_windows[i];
    }
    return 0;
}

static inline int wm_request_frame(wm_client_t* c, wm_window_t* win);

// wmc_apply_message(): the ONE place any message's effect on client
// state is ever applied -- see this header's own top comment. Every
// blocking helper in this file reaches state exclusively through here,
// by calling wmc_dispatch_one() (below), never by reading the wire
// itself. Idempotent for every state-like type: applying the same
// CONFIGURE/FRAME/BUFFER_RELEASED/FOCUS fact twice is always safe.
static inline void wmc_apply_message(wm_client_t* c, const wm_msg_t* m) {
    switch (m->type) {
    case WM_MSG_KEY_EVENT:
        wmc_key_ring_push(c, m);
        break;

    case WM_MSG_BUFFER_RELEASED: {
        wm_buffer_t* buf = wmc_find_tracked_buffer(c, m->shm_token);
        if (buf) {
            // Item 3: CLIENT_OWNED/COMPOSITOR_OWNED has exactly one
            // authoritative meaning here. A duplicate is diagnosed but
            // never corrupts anything -- the assignment below is
            // idempotent either way.
            if (!buf->owned_by_compositor) {
                g_wmc_duplicate_release_count++;
                if (!g_wmc_duplicate_release_reported) {
                    g_wmc_duplicate_release_reported = 1;
                    wmc_put("*** M10A: duplicate BUFFER_RELEASED for an already-writable buffer ***\n");
                }
            }
            buf->owned_by_compositor = 0;
        }
        break;
    }

    case WM_MSG_CONFIGURE: {
        wm_window_t* w = wmc_find_tracked_window(c, m->window_id);
        if (w && (!w->configure_known || m->generation > w->configure_generation)) {
            w->configure_generation = m->generation;
            w->content_w = m->w;
            w->content_h = m->h;
            w->is_fullscreen = (int)m->pressed;
            w->configure_known = 1;
            // Item 4's OTHER half: a FRAME granted for the OLD generation
            // may already be sitting here as live, not-yet-consumed
            // permission (m->generation == the generation live when it
            // was granted, checked at THAT time -- see the WM_MSG_FRAME
            // case below). A newer CONFIGURE superseding that generation
            // must invalidate it -- an unconsumed grant from before this
            // transition must never satisfy a render for content
            // belonging to the NEW one. Since holding live permission
            // means the client clearly wanted to render, immediately
            // ask for a fresh grant for the new generation rather than
            // leaving it stuck having never re-asked.
            if (w->frame_permission) {
                w->frame_permission = 0;
                w->frame_requested = 0;
                wm_request_frame(c, w);
            }
        }
        // else: stale/duplicate CONFIGURE (generation not newer than
        // one already applied) -- correctly superseded, ignored.
        break;
    }

    case WM_MSG_FRAME: {
        wm_window_t* w = wmc_find_tracked_window(c, m->window_id);
        if (w) {
            // M+10A follow-up audit (point 2): a FRAME with no
            // outstanding request is unsolicited -- honoring it anyway
            // would let a stray or duplicate grant hand out permission
            // for a render the client never actually asked for at this
            // moment, which is exactly the gap between "true in practice
            // because the real compositor structurally sends at most one
            // grant per request" and "formally true regardless of what
            // arrives on the wire." This is the client's own independent
            // enforcement of that invariant -- combined with the fact
            // that at most one request/permission is ever outstanding
            // per window (this same struct's own sequential use, never
            // concurrent), it makes a per-commit sequence number
            // unnecessary: configure_generation handles the one real
            // cross-cycle ambiguity (a fullscreen/configure transition),
            // and this check handles a wire-level duplicate/stray grant.
            if (!w->frame_requested) {
                g_wmc_unsolicited_frame_count++;
                if (!g_wmc_unsolicited_frame_reported) {
                    g_wmc_unsolicited_frame_reported = 1;
                    wmc_put("*** M10A: unsolicited FRAME (no outstanding request) -- ignored ***\n");
                }
            } else if (!w->configure_known || m->generation == w->configure_generation) {
                w->frame_permission = 1;
                w->frame_requested = 0;
            } else {
                // Item 4 (earlier redesign): a callback granted before a
                // since-superseded configure generation must never
                // satisfy a waiter for the CURRENT one. The compositor
                // already cleared its own frame_callback_pending when it
                // sent this grant, so without an immediate re-request
                // nothing would ever ask for a fresh one -- self-healed
                // here, transparently, rather than ever surfacing "stale"
                // to application code.
                g_wmc_stale_frame_regranted_count++;
                w->frame_requested = 0;
                wm_request_frame(c, w);
            }
        }
        break;
    }

    case WM_MSG_FOCUS: {
        wm_window_t* w = wmc_find_tracked_window(c, m->window_id);
        if (w) w->has_focus = (int)m->pressed;
        break;
    }

    case WM_MSG_WELCOME:
    case WM_MSG_WINDOW_CREATED:
    case WM_MSG_ACK:
        c->last_reply = *m;
        c->has_last_reply = 1;
        break;

    case WM_MSG_ERROR:
        c->last_error_code = m->x;
        c->has_last_error = 1;
        break;

    default:
        // Genuine ordered-event-stream types with no authoritative state
        // of their own (POINTER_MOTION/BUTTON/WHEEL, CLOSE_REQUEST) --
        // nothing to update here; wmc_dispatch_one()'s own out-param
        // already hands the raw message back to whichever caller is
        // driving the dispatch loop, which is this library's only
        // notion of "queuing" left for these types. See this header's
        // own top comment.
        break;
    }
}

// wmc_dispatch_one(): the ONE point where a fresh message enters this
// process AND has its effect applied. `out` may be NULL for a caller
// that only wants progress made (state updated) and doesn't care what
// the message was -- every condition-polling wait below uses that form.
// Returns 0, or -1 on disconnect/error.
static inline int wmc_dispatch_one(wm_client_t* c, wm_msg_t* out) {
    wm_msg_t m;
    if (wmc_recv_classified(c, &m) < 0) return -1;
    wmc_apply_message(c, &m);
    if (out) *out = m;
    return 0;
}

// wmc_wait_reply(): blocks until a one-shot command reply of EXACTLY
// `wanted_type` arrives, or until WM_MSG_ERROR arrives (a terminal
// failure for ANY outstanding command -- closing the gap the pre-M+10A
// design had: an unexpected ERROR was never recognized as a reply at
// all, so a command that failed would hang forever waiting for a reply
// type that would never come). Used by wm_connect/wm_create_window/
// wm_attach_surface/wm_attach_buffer. Returns 0 with *out filled, or -1
// on disconnect/error/WM_MSG_ERROR.
static inline int wmc_wait_reply(wm_client_t* c, uint32_t wanted_type, wm_msg_t* out) {
    c->has_last_reply = 0;
    c->has_last_error = 0;
    g_wmc_wait_reason = WM_WAIT_REPLY;
    for (;;) {
        if (wmc_dispatch_one(c, 0) < 0) { g_wmc_wait_reason = WM_WAIT_NONE; return -1; }
        if (c->has_last_error) { c->has_last_error = 0; g_wmc_wait_reason = WM_WAIT_NONE; return -1; }
        if (c->has_last_reply) {
            wm_msg_t got = c->last_reply;
            c->has_last_reply = 0;
            if (got.type == wanted_type) { *out = got; g_wmc_wait_reason = WM_WAIT_NONE; return 0; }
            // An unexpected reply type while waiting for a specific one
            // -- shouldn't happen given this library's own
            // one-outstanding-request-at-a-time usage; keep waiting
            // rather than treating it as fatal.
        }
    }
}

// Connects to whichever compositor currently owns WM_SERVICE_NAME --
// see wmproto64.h's header comment. Sends HELLO and blocks for WELCOME.
// Returns 0 with *out filled, or -1.
static inline int wm_connect(wm_client_t* out) {
    service64_endpoints_t ep;
    if (sys_service_connect(WM_SERVICE_NAME, 1 /* blocking */, &ep) < 0) return -1;

    out->req_w = ep.send;
    out->evt_r = ep.recv;
    out->client_id = 0;
    out->key_ring_count = 0;
    out->down_count = 0;
    out->tracked_buffer_count = 0;
    out->tracked_window_count = 0;
    out->has_last_reply = 0;
    out->has_last_error = 0;

    wm_msg_t hello; for (uint64_t i = 0; i < sizeof(hello); i++) ((char*)&hello)[i] = 0;
    hello.type = WM_MSG_HELLO;
    hello.version = WM_PROTO_VERSION;
    if (wmc_send(out, &hello) < 0) return -1;

    wm_msg_t reply;
    if (wmc_wait_reply(out, WM_MSG_WELCOME, &reply) < 0) return -1;
    out->client_id = reply.client_id;
    return 0;
}

// Requests a new window of content size w x h with the given title
// (truncated to WM_TITLE_MAX-1 chars). Blocks for WINDOW_CREATED, then
// registers `out_win` (zeroed except window_id) into the dispatcher's
// own tracked-windows registry -- from this point on, `out_win`'s
// fields are updated automatically by CONFIGURE/FRAME/FOCUS traffic.
// Returns the window_id, or 0 on failure (0 is never a valid window_id;
// `out_win` is left zeroed in that case).
static inline uint32_t wm_create_window(wm_client_t* c, uint32_t w, uint32_t h, const char* title, wm_window_t* out_win) {
    for (uint64_t i = 0; i < sizeof(*out_win); i++) ((char*)out_win)[i] = 0;

    wm_msg_t m; for (uint64_t i = 0; i < sizeof(m); i++) ((char*)&m)[i] = 0;
    m.type = WM_MSG_CREATE_WINDOW;
    m.version = WM_PROTO_VERSION;
    m.client_id = c->client_id;
    m.w = w; m.h = h;
    int i = 0; while (title[i] && i < WM_TITLE_MAX - 1) { m.title[i] = title[i]; i++; }
    if (wmc_send(c, &m) < 0) return 0;

    wm_msg_t reply;
    if (wmc_wait_reply(c, WM_MSG_WINDOW_CREATED, &reply) < 0) return 0;

    out_win->window_id = reply.window_id;
    if (c->tracked_window_count < WMC_MAX_TRACKED_WINDOWS) {
        c->tracked_windows[c->tracked_window_count++] = out_win;
    }
    return reply.window_id;
}

// Creates a w x h x 4 (WM_FORMAT_XRGB8888) shared-memory surface,
// attaches it to `window_id`, and maps it WRITABLE into this process's
// own address space. Blocks for the compositor's ACK. Returns a
// pointer to the mapped pixel buffer (row-major, tightly packed,
// w*4 bytes per row) on success, or 0 on failure. `stride_out` (if
// non-NULL) is set to the actual stride (always w*4 here).
static inline uint32_t* wm_attach_surface(wm_client_t* c, uint32_t window_id,
                                           uint32_t w, uint32_t h, uint32_t* stride_out)
{
    if (w == 0 || h == 0 || w > 4096 || h > 4096) return 0;
    uint64_t size = (uint64_t)w * (uint64_t)h * 4ULL;

    int64_t shm_h = sys_shm_create(size);
    if (shm_h < 0) return 0;
    int64_t token = sys_shm_token((int)shm_h);
    if (token < 0) { sys_handle_close((int)shm_h); return 0; }
    uint64_t addr = sys_shm_map((int)shm_h, 1 /* writable */);
    if (addr == (uint64_t)-1) { sys_handle_close((int)shm_h); return 0; }

    wm_msg_t m; for (uint64_t i = 0; i < sizeof(m); i++) ((char*)&m)[i] = 0;
    m.type = WM_MSG_ATTACH_SURFACE;
    m.version = WM_PROTO_VERSION;
    m.client_id = c->client_id;
    m.window_id = window_id;
    m.shm_token = (uint64_t)token;
    m.w = w; m.h = h;
    m.stride = w * 4;
    m.format = WM_FORMAT_XRGB8888;
    if (wmc_send(c, &m) < 0) { sys_handle_close((int)shm_h); return 0; }

    wm_msg_t reply;
    if (wmc_wait_reply(c, WM_MSG_ACK, &reply) < 0) {
        sys_handle_close((int)shm_h);
        return 0;
    }

    if (stride_out) *stride_out = w * 4;
    // Deliberately leaks the shm handle for this milestone's simple
    // single-surface-per-window model -- it stays alive exactly as
    // long as this process runs, released automatically on exit like
    // every other handle (kernel/process64.c's close_all_handles); a
    // window that outlives its client is never possible since the
    // compositor removes windows on client disconnect (see
    // compositor64.c's connection cleanup).
    return (uint32_t*)(uintptr_t)addr;
}

// Tells the compositor that the rectangle (x,y,w,h) of window_id's
// surface has changed and should be recomposited. No reply.
static inline int wm_commit(wm_client_t* c, uint32_t window_id, int32_t x, int32_t y, uint32_t w, uint32_t h) {
    wm_msg_t m; for (uint64_t i = 0; i < sizeof(m); i++) ((char*)&m)[i] = 0;
    m.type = WM_MSG_COMMIT;
    m.version = WM_PROTO_VERSION;
    m.client_id = c->client_id;
    m.window_id = window_id;
    m.x = x; m.y = y; m.w = w; m.h = h;
    return wmc_send(c, &m);
}

// Creates a w x h x 4 (WM_FORMAT_XRGB8888) shared-memory buffer and
// attaches it to window_id WITHOUT displaying it -- call this once per
// buffer (typically twice, for double buffering) before ever committing
// either one. Blocks for the compositor's ACK, then registers `out`
// into the dispatcher's own tracked-buffers registry -- from this point
// on, out->owned_by_compositor is updated automatically by
// BUFFER_RELEASED traffic, never manually by caller code. Returns 0
// with *out filled (owned_by_compositor = 0, safe to draw into
// immediately), or -1.
static inline int wm_attach_buffer(wm_client_t* c, uint32_t window_id,
                                    uint32_t w, uint32_t h, wm_buffer_t* out)
{
    if (w == 0 || h == 0 || w > 4096 || h > 4096) return -1;
    uint64_t size = (uint64_t)w * (uint64_t)h * 4ULL;

    int64_t shm_h = sys_shm_create(size);
    if (shm_h < 0) return -1;
    int64_t token = sys_shm_token((int)shm_h);
    if (token < 0) { sys_handle_close((int)shm_h); return -1; }
    uint64_t addr = sys_shm_map((int)shm_h, 1 /* writable */);
    if (addr == (uint64_t)-1) { sys_handle_close((int)shm_h); return -1; }

    wm_msg_t m; for (uint64_t i = 0; i < sizeof(m); i++) ((char*)&m)[i] = 0;
    m.type = WM_MSG_ATTACH_BUFFER;
    m.version = WM_PROTO_VERSION;
    m.client_id = c->client_id;
    m.window_id = window_id;
    m.shm_token = (uint64_t)token;
    m.w = w; m.h = h;
    m.stride = w * 4;
    m.format = WM_FORMAT_XRGB8888;
    if (wmc_send(c, &m) < 0) { sys_handle_close((int)shm_h); return -1; }

    wm_msg_t reply;
    if (wmc_wait_reply(c, WM_MSG_ACK, &reply) < 0) { sys_handle_close((int)shm_h); return -1; }

    out->shm_handle = (int)shm_h;
    out->token = (uint64_t)token;
    out->pixels = (uint32_t*)(uintptr_t)addr;
    out->w = w; out->h = h; out->stride = w * 4;
    out->owned_by_compositor = 0;

    // M+10A follow-up audit (point 3): a fresh token aliasing one
    // already held by another SIMULTANEOUSLY-LIVE tracked buffer would
    // mean two wm_buffer_t slots silently referring to the same
    // underlying shm object -- aliasing is never valid for this
    // library's model (every buffer is an independent double-buffering
    // slot) and was exactly the observable symptom of a real bug found
    // during M+10A stress testing (an unbounded retry loop that leaked
    // shm objects and left two buffer slots with the same token).
    // Diagnosed loudly, once -- this must never fire now that the retry
    // loop that caused it is gone, so if it ever does, that is itself
    // proof of a regression of that same bug class.
    if (wmc_find_tracked_buffer(c, out->token)) {
        g_wmc_duplicate_token_count++;
        if (!g_wmc_duplicate_token_reported) {
            g_wmc_duplicate_token_reported = 1;
            wmc_put("*** M10A: wm_attach_buffer() produced a token aliasing an already-live buffer ***\n");
        }
    }

    if (c->tracked_buffer_count < WMC_MAX_TRACKED_BUFFERS) {
        c->tracked_buffers[c->tracked_buffer_count++] = out;
    }
    return 0;
}

// M+10A follow-up audit (point 3): a cheap, defensive check a caller
// should make immediately before rendering `render_w` x `render_h`
// pixels (tightly packed, matching this library's own attach
// convention of stride == w*4, see wm_attach_buffer() above) into
// `buf->pixels`. Returns 1 iff the buffer was actually allocated at
// exactly this size -- never "close enough" -- so a caller can refuse
// to render rather than ever writing beyond the backing shm mapping's
// own allocation. This exists specifically to make a fullscreen-vs-
// normal buffer-pair mismatch (e.g. a stale local copy of which pair is
// "current") a loud, cheap, caught bug instead of a silent overflow.
static inline int wm_buffer_fits(const wm_buffer_t* buf, uint32_t render_w, uint32_t render_h) {
    if (render_w != buf->w || render_h != buf->h) return 0;
    // Belt-and-suspenders: recompute the actual byte requirement against
    // the buffer's own recorded stride rather than trusting w/h alone.
    uint64_t required = (uint64_t)buf->stride * (uint64_t)render_h;
    uint64_t allocated = (uint64_t)buf->stride * (uint64_t)buf->h;
    return required <= allocated;
}

// Commits `buf` (which must already be attached, and NOT currently
// owned_by_compositor) as `win`'s new displayed frame, with damage rect
// (x,y,w,h) in content-local coordinates. No reply -- marks
// buf->owned_by_compositor = 1 immediately; the caller must not write
// into `buf->pixels` again until it is next observed writable (see
// wm_wait_buffer_released()/wm_wait_frame()).
//
// M+10A protocol redesign: stamps the outgoing message with `win`'s own
// current configure_generation -- the compositor can now tell exactly
// which CONFIGURE this content answers (see wmproto64.h's own
// WM_MSG_COMMIT_BUFFER comment).
static inline int wm_commit_buffer(wm_client_t* c, wm_buffer_t* buf, wm_window_t* win,
                                    int32_t x, int32_t y, uint32_t w, uint32_t h)
{
    wm_msg_t m; for (uint64_t i = 0; i < sizeof(m); i++) ((char*)&m)[i] = 0;
    m.type = WM_MSG_COMMIT_BUFFER;
    m.version = WM_PROTO_VERSION;
    m.client_id = c->client_id;
    m.window_id = win->window_id;
    m.shm_token = buf->token;
    m.x = x; m.y = y; m.w = w; m.h = h;
    m.generation = win->configure_generation;
    if (wmc_send(c, &m) < 0) return -1;
    buf->owned_by_compositor = 1;
    return 0;
}

// Blocks until `buf` is observed writable -- a no-op if it already is.
// Item 3: this is now a pure condition-poll over authoritative state,
// never a raw-wire reader itself. Only appropriate for a client that
// doesn't otherwise need to process events on this same connection
// (see wm_wait_event() for a reactive client's own loop-driving call).
// Returns 0, or -1 on disconnect/error.
static inline int wm_wait_buffer_released(wm_client_t* c, wm_buffer_t* buf) {
    g_wmc_wait_reason = WM_WAIT_BUFFER_RELEASE;
    while (buf->owned_by_compositor) {
        if (wmc_dispatch_one(c, 0) < 0) { g_wmc_wait_reason = WM_WAIT_NONE; return -1; }
    }
    g_wmc_wait_reason = WM_WAIT_NONE;
    return 0;
}

// ── M+6: compositor-driven frame pacing (Revision 3 §22 item 5) ─────
// Requests exactly one future frame-callback grant for `win` -- see
// wmproto64.h's own WM_MSG_REQUEST_FRAME comment. No reply; the reply IS
// the eventual WM_MSG_FRAME wmc_apply_message() applies to
// win->frame_permission. Idempotent: a no-op if one is already
// outstanding (win->frame_requested), so callers may call this freely
// without their own bookkeeping.
static inline int wm_request_frame(wm_client_t* c, wm_window_t* win) {
    if (win->frame_requested) return 0;
    wm_msg_t m; for (uint64_t i = 0; i < sizeof(m); i++) ((char*)&m)[i] = 0;
    m.type = WM_MSG_REQUEST_FRAME;
    m.version = WM_PROTO_VERSION;
    m.client_id = c->client_id;
    m.window_id = win->window_id;
    if (wmc_send(c, &m) < 0) return -1;
    win->frame_requested = 1;
    return 0;
}

// Blocks until BOTH `win`'s frame-callback permission AND `buf`'s own
// ownership are simultaneously true -- see wmproto64.h's own
// WM_MSG_REQUEST_FRAME/WM_MSG_FRAME comment for why a well-behaved
// animated client needs BOTH before rendering its next frame. A pure
// condition-poll over authoritative state (win->frame_permission,
// buf->owned_by_compositor) -- correct regardless of which condition
// becomes true first, since each is applied the instant it's seen
// regardless of what this call happens to be doing. Consumes the
// one-shot frame permission on success (win->frame_permission = 0).
// A no-op wait (returns immediately) if both conditions already hold.
// Only appropriate for a client that doesn't otherwise need to process
// events on this same connection. Returns 0, or -1 on disconnect/error.
static inline int wm_wait_frame(wm_client_t* c, wm_window_t* win, wm_buffer_t* buf) {
    for (;;) {
        int frame_ready = win->frame_permission;
        int buf_ready = !buf->owned_by_compositor;
        if (frame_ready && buf_ready) break;
        g_wmc_wait_reason = (!frame_ready && !buf_ready) ? WM_WAIT_FRAME_AND_RELEASE
                          : (!frame_ready ? WM_WAIT_FRAME : WM_WAIT_BUFFER_RELEASE);
        if (wmc_dispatch_one(c, 0) < 0) { g_wmc_wait_reason = WM_WAIT_NONE; return -1; }
    }
    win->frame_permission = 0;
    g_wmc_wait_reason = WM_WAIT_NONE;
    return 0;
}

static inline int wm_set_title(wm_client_t* c, wm_window_t* win, const char* title) {
    wm_msg_t m; for (uint64_t i = 0; i < sizeof(m); i++) ((char*)&m)[i] = 0;
    m.type = WM_MSG_SET_TITLE;
    m.version = WM_PROTO_VERSION;
    m.client_id = c->client_id;
    m.window_id = win->window_id;
    int i = 0; while (title[i] && i < WM_TITLE_MAX - 1) { m.title[i] = title[i]; i++; }
    return wmc_send(c, &m);
}

// M+10A protocol redesign (item 5): requests entering (fullscreen=1) or
// exiting (fullscreen=0) real compositor fullscreen for `win` -- see
// wmproto64.h's own WM_MSG_SET_FULLSCREEN/WM_MSG_CONFIGURE comments.
// FIRE-AND-FORGET: unlike the pre-M+10A version, this never blocks for
// a reply. The transition is asynchronous, exactly like Wayland's own
// xdg_surface configure/ack_configure/commit handshake -- the caller
// observes the result (new geometry, current fullscreen state, the
// generation to stamp its next commit with) purely through `win`'s own
// fields, updated by the ordinary dispatch path the instant the
// eventual CONFIGURE arrives, with no special-casing needed by the
// caller's own main loop. No synchronous behavior is required for
// correctness -- a caller that wants to keep rendering/dispatching
// between the request and the compositor's reply simply keeps doing
// so; nothing here stops the world waiting for one specific message.
// Returns the wmc_send() result (0 or -1) -- NOT whether the transition
// itself succeeded, which is knowable only once win->is_fullscreen
// actually reflects it.
static inline int wm_set_fullscreen(wm_client_t* c, wm_window_t* win, int fullscreen) {
    wm_msg_t m; for (uint64_t i = 0; i < sizeof(m); i++) ((char*)&m)[i] = 0;
    m.type = WM_MSG_SET_FULLSCREEN;
    m.version = WM_PROTO_VERSION;
    m.client_id = c->client_id;
    m.window_id = win->window_id;
    m.pressed = fullscreen ? 1u : 0u;
    return wmc_send(c, &m);
}

static inline int wm_destroy_window(wm_client_t* c, wm_window_t* win) {
    wm_msg_t m; for (uint64_t i = 0; i < sizeof(m); i++) ((char*)&m)[i] = 0;
    m.type = WM_MSG_DESTROY_WINDOW;
    m.version = WM_PROTO_VERSION;
    m.client_id = c->client_id;
    m.window_id = win->window_id;
    return wmc_send(c, &m);
}

// wm_poll_key(): the ONE shared, non-blocking key-drain rule for a
// client that can't simply block in wm_wait_event() because it also has
// its own separate pacing/wait to run (e.g. gfx_demo64.c's
// wm_wait_frame() call). A pure peek of whatever's already queued in
// key_ring[] -- fresh KEY_EVENTs only ever enter it via
// wmc_apply_message() (called from whatever dispatch call happens to be
// running, e.g. wm_wait_frame()'s own inner loop), so this never itself
// touches the wire.
//
// Drains the WHOLE ring in one call, in order -- not just the first
// press. The FIRST (oldest, FIFO-order) queued press wins. Returns 1
// with *out_keycode filled, or 0 if no press was queued at all. Never
// blocks.
static inline int wm_poll_key(wm_client_t* c, uint32_t* out_keycode) {
    int got = 0;
    wm_msg_t m;
    while (wmc_key_ring_pop_front(c, &m)) {
        if (!got && m.pressed) { *out_keycode = m.key_code; got = 1; }
    }
    return got;
}

// wm_wait_event(): blocks until the next event arrives -- draining
// key_ring[] first (oldest queued KEY_EVENT, if any, preserving its
// exact arrival order relative to every other queued key event), then
// dispatching exactly one fresh message and handing it back verbatim.
// This is what a reactive application's own event loop should call
// (see user64/gfx_interactive64.c) -- state (buffer ownership, configure,
// frame permission, focus) is ALSO applied before this returns, via the
// same wmc_dispatch_one() every other wait helper uses, so a reactive
// client sees both the raw event AND already-updated state for it.
// Returns 0 with *out filled, or -1 on disconnect/error.
static inline int wm_wait_event(wm_client_t* c, wm_msg_t* out) {
    if (wmc_key_ring_pop_front(c, out)) return 0;
    g_wmc_wait_reason = WM_WAIT_OTHER;
    int rc = wmc_dispatch_one(c, out);
    g_wmc_wait_reason = WM_WAIT_NONE;
    return rc;
}

// ── M+10A: diagnostic dump ─────────────────────────────────────────────
// Self-contained, direct-to-serial via sys_write. Read-only: never
// mutates state, never drops or reorders anything while dumping. Callers
// are expected to invoke this rarely (a stall watchdog firing, an
// invariant violation, an explicit request) -- never per frame; see the
// M+10A liveness investigation's own low-overhead lesson.
static inline const char* wmc_msg_type_name(uint32_t t) {
    switch (t) {
    case WM_MSG_HELLO: return "HELLO";
    case WM_MSG_WELCOME: return "WELCOME";
    case WM_MSG_CREATE_WINDOW: return "CREATE_WINDOW";
    case WM_MSG_WINDOW_CREATED: return "WINDOW_CREATED";
    case WM_MSG_ATTACH_SURFACE: return "ATTACH_SURFACE";
    case WM_MSG_COMMIT: return "COMMIT";
    case WM_MSG_KEY_EVENT: return "KEY_EVENT";
    case WM_MSG_POINTER_MOTION: return "POINTER_MOTION";
    case WM_MSG_POINTER_BUTTON: return "POINTER_BUTTON";
    case WM_MSG_POINTER_WHEEL: return "POINTER_WHEEL";
    case WM_MSG_FOCUS: return "FOCUS";
    case WM_MSG_CLOSE_REQUEST: return "CLOSE_REQUEST";
    case WM_MSG_ACK: return "ACK";
    case WM_MSG_ERROR: return "ERROR";
    case WM_MSG_ATTACH_BUFFER: return "ATTACH_BUFFER";
    case WM_MSG_COMMIT_BUFFER: return "COMMIT_BUFFER";
    case WM_MSG_BUFFER_RELEASED: return "BUFFER_RELEASED";
    case WM_MSG_REQUEST_FRAME: return "REQUEST_FRAME";
    case WM_MSG_FRAME: return "FRAME";
    case WM_MSG_SET_TITLE: return "SET_TITLE";
    case WM_MSG_SET_FULLSCREEN: return "SET_FULLSCREEN";
    case WM_MSG_CONFIGURE: return "CONFIGURE";
    case WM_MSG_DESTROY_WINDOW: return "DESTROY_WINDOW";
    default: return "?";
    }
}
static inline const char* wm_wait_reason_name(wm_wait_reason_t r) {
    switch (r) {
    case WM_WAIT_NONE: return "NONE";
    case WM_WAIT_FRAME: return "FRAME";
    case WM_WAIT_BUFFER_RELEASE: return "BUFFER_RELEASE";
    case WM_WAIT_FRAME_AND_RELEASE: return "FRAME_AND_RELEASE";
    case WM_WAIT_REPLY: return "REPLY";
    case WM_WAIT_OTHER: return "OTHER";
    default: return "?";
    }
}
static inline void wmc_put(const char* s) { sys_write(s, (uint64_t)wmc_strlen(s)); }
static inline void wmc_put_u64(uint64_t v) {
    char buf[24]; int p = 0;
    if (v == 0) buf[p++] = '0';
    else { char tmp[24]; int t = 0; while (v) { tmp[t++] = (char)('0' + (v % 10)); v /= 10; } while (t) buf[p++] = tmp[--t]; }
    buf[p] = 0;
    wmc_put(buf);
}

// Dumps every window/buffer this client has registered, plus the
// permanent diagnostic counters -- the client-side half of a liveness
// investigation (see gfx_demo64.c's own stall watchdog for the other
// half, and compositor64.c's own dump_window_liveness() for the
// compositor-side view of the same window).
static inline void wmc_dump_state(wm_client_t* c, const char* tag) {
    wmc_put("wmc_dump ["); wmc_put(tag); wmc_put("]: wait_reason=");
    wmc_put(wm_wait_reason_name(g_wmc_wait_reason));
    wmc_put(" key_ring_count="); wmc_put_u64((uint64_t)c->key_ring_count);
    wmc_put("\n");
    for (int i = 0; i < c->tracked_window_count; i++) {
        wm_window_t* w = c->tracked_windows[i];
        wmc_put("  window[window_id="); wmc_put_u64((uint64_t)w->window_id);
        wmc_put("] configure_generation="); wmc_put_u64((uint64_t)w->configure_generation);
        wmc_put(" is_fullscreen="); wmc_put_u64((uint64_t)w->is_fullscreen);
        wmc_put(" content_w="); wmc_put_u64((uint64_t)w->content_w);
        wmc_put(" content_h="); wmc_put_u64((uint64_t)w->content_h);
        wmc_put(" frame_permission="); wmc_put_u64((uint64_t)w->frame_permission);
        wmc_put(" frame_requested="); wmc_put_u64((uint64_t)w->frame_requested);
        wmc_put(" has_focus="); wmc_put_u64((uint64_t)w->has_focus);
        wmc_put("\n");
    }
    for (int i = 0; i < c->tracked_buffer_count; i++) {
        wm_buffer_t* b = c->tracked_buffers[i];
        wmc_put("  buffer["); wmc_put_u64((uint64_t)i); wmc_put("] token="); wmc_put_u64(b->token);
        wmc_put(" owned_by_compositor="); wmc_put_u64((uint64_t)b->owned_by_compositor);
        wmc_put("\n");
    }
    for (int i = 0; i < c->key_ring_count; i++) {
        wmc_put("  key_ring["); wmc_put_u64((uint64_t)i); wmc_put("] = KEY_EVENT pressed=");
        wmc_put_u64((uint64_t)c->key_ring[i].pressed);
        wmc_put("\n");
    }
    wmc_put("  counters: received="); wmc_put_u64(g_wmc_received);
    wmc_put(" key_queued="); wmc_put_u64(g_wmc_key_queued);
    wmc_put(" key_dequeued="); wmc_put_u64(g_wmc_key_dequeued);
    wmc_put(" key_ring_high_water="); wmc_put_u64(g_wmc_key_ring_high_water);
    wmc_put(" key_overflow_attempts="); wmc_put_u64(g_wmc_key_overflow_attempts);
    wmc_put(" KEY_LOST="); wmc_put_u64(g_wmc_key_lost);
    wmc_put(" DUPLICATE_RELEASE="); wmc_put_u64(g_wmc_duplicate_release_count);
    wmc_put(" stale_frame_regranted="); wmc_put_u64(g_wmc_stale_frame_regranted_count);
    wmc_put(" unsolicited_frame="); wmc_put_u64(g_wmc_unsolicited_frame_count);
    wmc_put(" DUPLICATE_TOKEN="); wmc_put_u64(g_wmc_duplicate_token_count);
    wmc_put("\n");
}

#endif // WMCLIENT64_H
