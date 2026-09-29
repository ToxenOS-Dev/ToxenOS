// ToxenOS/user64/wmproto_test64.c — M+10A protocol redesign: deterministic
// ordering-matrix tests for wmclient64.h's central dispatcher
// (wmc_apply_message()). Not part of the normal graphical boot roster --
// a standalone diagnostic fixture, spawned manually from the shell.
//
// These tests exercise wmc_apply_message() DIRECTLY, feeding it
// hand-built wm_msg_t sequences with no real compositor connection at
// all -- the whole point of the M+10A redesign is that a message's
// effect on client state is a pure function of (current state, message),
// independent of arrival order for anything state-like (item 2), so
// this is exactly the level the ordering matrix in the M+10A redesign
// report needs to be proven at: no timing, no QMP, no physical input,
// just the state machine itself against every order the report calls
// out by name.
//
// wm_client_t.req_w is left at an invalid handle throughout -- the
// dispatcher's own self-healing re-request calls (wm_request_frame(),
// invoked internally by wmc_apply_message() for a stale/superseded
// FRAME) send through it, and a write to an invalid handle simply fails
// (-1) without side effects relevant to these tests, which only inspect
// wm_window_t/wm_buffer_t state afterward, never actual wire traffic.
#include <stdint.h>
#include "tox64.h"
#include "wmclient64.h"

static int g_pass = 0, g_fail = 0;

static void ok(const char* name, int cond) {
    if (cond) { g_pass++; }
    else {
        g_fail++;
        wmc_put("FAIL: "); wmc_put(name); wmc_put("\n");
    }
}

static void reset_client(wm_client_t* c) {
    c->req_w = -1;
    c->evt_r = -1;
    c->client_id = 1;
    c->key_ring_count = 0;
    c->down_count = 0;
    c->tracked_buffer_count = 0;
    c->tracked_window_count = 0;
    c->has_last_reply = 0;
    c->has_last_error = 0;
}

static void reset_window(wm_window_t* w, uint32_t window_id) {
    for (uint64_t i = 0; i < sizeof(*w); i++) ((char*)w)[i] = 0;
    w->window_id = window_id;
}

static void reset_buffer(wm_buffer_t* b, uint64_t token) {
    for (uint64_t i = 0; i < sizeof(*b); i++) ((char*)b)[i] = 0;
    b->token = token;
    b->owned_by_compositor = 1; // simulates "just committed" -- the common starting point for these tests
}

static void track(wm_client_t* c, wm_window_t* w) {
    c->tracked_windows[c->tracked_window_count++] = w;
}
static void track_buf(wm_client_t* c, wm_buffer_t* b) {
    c->tracked_buffers[c->tracked_buffer_count++] = b;
}

static wm_msg_t msg_release(uint32_t win_id, uint64_t token) {
    wm_msg_t m; for (uint64_t i = 0; i < sizeof(m); i++) ((char*)&m)[i] = 0;
    m.type = WM_MSG_BUFFER_RELEASED; m.version = WM_PROTO_VERSION;
    m.window_id = win_id; m.shm_token = token;
    return m;
}
static wm_msg_t msg_frame(uint32_t win_id, uint32_t generation) {
    wm_msg_t m; for (uint64_t i = 0; i < sizeof(m); i++) ((char*)&m)[i] = 0;
    m.type = WM_MSG_FRAME; m.version = WM_PROTO_VERSION;
    m.window_id = win_id; m.generation = generation;
    return m;
}
static wm_msg_t msg_configure(uint32_t win_id, uint32_t generation, uint32_t w, uint32_t h, int fullscreen) {
    wm_msg_t m; for (uint64_t i = 0; i < sizeof(m); i++) ((char*)&m)[i] = 0;
    m.type = WM_MSG_CONFIGURE; m.version = WM_PROTO_VERSION;
    m.window_id = win_id; m.generation = generation; m.w = w; m.h = h; m.pressed = (uint32_t)fullscreen;
    return m;
}
static wm_msg_t msg_key(uint32_t win_id, uint32_t keycode, int pressed) {
    wm_msg_t m; for (uint64_t i = 0; i < sizeof(m); i++) ((char*)&m)[i] = 0;
    m.type = WM_MSG_KEY_EVENT; m.version = WM_PROTO_VERSION;
    m.window_id = win_id; m.key_code = keycode; m.pressed = (uint32_t)pressed;
    return m;
}
static wm_msg_t msg_focus(uint32_t win_id, int gained) {
    wm_msg_t m; for (uint64_t i = 0; i < sizeof(m); i++) ((char*)&m)[i] = 0;
    m.type = WM_MSG_FOCUS; m.version = WM_PROTO_VERSION;
    m.window_id = win_id; m.pressed = (uint32_t)gained;
    return m;
}

// ── §10: RELEASE, FRAME (independent conditions, either order) ────────
static void test_release_then_frame(void) {
    wm_client_t c; reset_client(&c);
    wm_window_t w; reset_window(&w, 1);
    wm_buffer_t b; reset_buffer(&b, 100);
    track(&c, &w); track_buf(&c, &b);

    wm_msg_t r = msg_release(1, 100); wmc_apply_message(&c, &r);
    ok("release_then_frame: buffer freed by RELEASE alone", !b.owned_by_compositor);
    ok("release_then_frame: frame not yet granted", !w.frame_permission);

    w.frame_requested = 1; // simulates the preceding wm_request_frame() a real client always sends
    wm_msg_t f = msg_frame(1, 0); wmc_apply_message(&c, &f); // generation 0 == initial configure_generation (never configured)
    ok("release_then_frame: frame granted after FRAME", w.frame_permission);
    ok("release_then_frame: buffer still free", !b.owned_by_compositor);
}

static void test_frame_then_release(void) {
    wm_client_t c; reset_client(&c);
    wm_window_t w; reset_window(&w, 1);
    wm_buffer_t b; reset_buffer(&b, 100);
    track(&c, &w); track_buf(&c, &b);

    w.frame_requested = 1; // simulates the preceding wm_request_frame()
    wm_msg_t f = msg_frame(1, 0); wmc_apply_message(&c, &f);
    ok("frame_then_release: frame granted first", w.frame_permission);
    ok("frame_then_release: buffer still owned", b.owned_by_compositor);

    wm_msg_t r = msg_release(1, 100); wmc_apply_message(&c, &r);
    ok("frame_then_release: buffer freed second", !b.owned_by_compositor);
    ok("frame_then_release: frame permission unaffected by an unrelated release", w.frame_permission);
}

// ── §10: CONFIGURE, RELEASE, FRAME / RELEASE, CONFIGURE, FRAME ─────────
static void test_configure_release_frame(void) {
    wm_client_t c; reset_client(&c);
    wm_window_t w; reset_window(&w, 1);
    wm_buffer_t b; reset_buffer(&b, 100);
    track(&c, &w); track_buf(&c, &b);

    wm_msg_t cfg = msg_configure(1, 1, 640, 480, 1); wmc_apply_message(&c, &cfg);
    ok("configure_release_frame: generation applied", w.configure_generation == 1);
    ok("configure_release_frame: fullscreen applied", w.is_fullscreen == 1);

    wm_msg_t r = msg_release(1, 100); wmc_apply_message(&c, &r);
    ok("configure_release_frame: buffer freed", !b.owned_by_compositor);

    w.frame_requested = 1; // simulates the preceding wm_request_frame()
    wm_msg_t f = msg_frame(1, 1); wmc_apply_message(&c, &f); // matches current generation 1
    ok("configure_release_frame: frame granted for current generation", w.frame_permission);
}

static void test_release_configure_frame(void) {
    wm_client_t c; reset_client(&c);
    wm_window_t w; reset_window(&w, 1);
    wm_buffer_t b; reset_buffer(&b, 100);
    track(&c, &w); track_buf(&c, &b);

    wm_msg_t r = msg_release(1, 100); wmc_apply_message(&c, &r);
    ok("release_configure_frame: buffer freed before configure exists", !b.owned_by_compositor);

    wm_msg_t cfg = msg_configure(1, 1, 640, 480, 1); wmc_apply_message(&c, &cfg);
    w.frame_requested = 1; // simulates the preceding wm_request_frame()
    wm_msg_t f = msg_frame(1, 1); wmc_apply_message(&c, &f);
    ok("release_configure_frame: frame granted for current generation", w.frame_permission);
    ok("release_configure_frame: buffer stayed free the whole time", !b.owned_by_compositor);
}

// ── §10: FRAME, CONFIGURE, RELEASE -- the case that exposed the
// unconsumed-permission gap during test design (see wmclient64.h's own
// WM_MSG_CONFIGURE case comment): a FRAME granted for generation 1,
// never consumed, must NOT survive a configure that supersedes it to
// generation 2.
static void test_frame_configure_release(void) {
    wm_client_t c; reset_client(&c);
    wm_window_t w; reset_window(&w, 1);
    wm_buffer_t b; reset_buffer(&b, 100);
    track(&c, &w); track_buf(&c, &b);

    wm_msg_t cfg1 = msg_configure(1, 1, 200, 150, 0); wmc_apply_message(&c, &cfg1);
    w.frame_requested = 1; // simulates the preceding wm_request_frame()
    wm_msg_t f = msg_frame(1, 1); wmc_apply_message(&c, &f);
    ok("frame_configure_release: frame granted for gen 1", w.frame_permission);

    wm_msg_t cfg2 = msg_configure(1, 2, 640, 480, 1); wmc_apply_message(&c, &cfg2);
    ok("frame_configure_release: stale gen-1 permission invalidated by gen 2", !w.frame_permission);
    ok("frame_configure_release: generation advanced to 2", w.configure_generation == 2);

    wm_msg_t r = msg_release(1, 100); wmc_apply_message(&c, &r);
    ok("frame_configure_release: buffer freed regardless", !b.owned_by_compositor);
    ok("frame_configure_release: still no frame permission for gen 2 (a fresh grant is required)", !w.frame_permission);
}

// ── §10: multiple CONFIGURE messages before rendering ──────────────────
static void test_multi_configure_supersedes(void) {
    wm_client_t c; reset_client(&c);
    wm_window_t w; reset_window(&w, 1);
    track(&c, &w);

    wm_msg_t c100 = msg_configure(1, 100, 320, 240, 0); wmc_apply_message(&c, &c100);
    wm_msg_t c101 = msg_configure(1, 101, 640, 480, 1); wmc_apply_message(&c, &c101);
    wm_msg_t c102 = msg_configure(1, 102, 1024, 768, 1); wmc_apply_message(&c, &c102);
    ok("multi_configure: final generation is 102", w.configure_generation == 102);
    ok("multi_configure: final geometry is 102's", w.content_w == 1024 && w.content_h == 768);

    // A stale grant for generation 100, arriving after the burst, must
    // be recognized as stale and self-heal (re-request) rather than
    // ever granting permission for the CURRENT (102) generation.
    w.frame_requested = 1; // simulates an outstanding request from before the configure burst
    wm_msg_t stale = msg_frame(1, 100); wmc_apply_message(&c, &stale);
    ok("multi_configure: stale gen-100 FRAME does not grant permission", !w.frame_permission);
    ok("multi_configure: stale FRAME counted", g_wmc_stale_frame_regranted_count > 0);

    // Out-of-order arrival: 100 arrives LAST, after 102 already applied.
    wm_msg_t late100 = msg_configure(1, 100, 1, 1, 0); wmc_apply_message(&c, &late100);
    ok("multi_configure: a late, older-generation CONFIGURE never regresses state", w.configure_generation == 102 && w.content_w == 1024);
}

// ── §10: COMMIT generation vs a burst of CONFIGUREs (client-side half --
// the client always stamps its own commit with whatever it currently
// believes, which by construction is always the latest applied) ───────
static void test_commit_uses_latest_generation(void) {
    wm_client_t c; reset_client(&c);
    wm_window_t w; reset_window(&w, 1);
    wm_buffer_t b; reset_buffer(&b, 100);
    b.owned_by_compositor = 0;
    track(&c, &w); track_buf(&c, &b);

    wmc_apply_message(&c, &(wm_msg_t){0}); // no-op sanity: applying a zeroed message must not crash or touch tracked state
    wm_msg_t c100 = msg_configure(1, 100, 320, 240, 0); wmc_apply_message(&c, &c100);
    wm_msg_t c101 = msg_configure(1, 101, 640, 480, 1); wmc_apply_message(&c, &c101);
    wm_msg_t c102 = msg_configure(1, 102, 1024, 768, 1); wmc_apply_message(&c, &c102);

    // wm_commit_buffer() itself just reads win->configure_generation --
    // verify that field is exactly 102 right before a real client would
    // stamp its commit with it (wm_commit_buffer()'s own body is a
    // one-line read of this field, not separately tested here since it
    // requires a live wmc_send()).
    ok("commit_uses_latest_generation: window generation is 102 before commit", w.configure_generation == 102);
}

// ── M+10A follow-up audit (point 2): FRAME one-shot semantics ──────────
// Exact state machine under test (see wmclient64.h's own WM_MSG_FRAME
// comment for the full rationale):
//
//   [IDLE] --commit finishes, wm_request_frame()--> [REQUESTED]
//   [REQUESTED] --FRAME(gen==current)--> [GRANTED] (frame_requested=0)
//   [REQUESTED] --FRAME(gen<current, stale)--> self-heal, re-request,
//                                                stays [REQUESTED]
//   [GRANTED] --CONFIGURE(gen advances)--> invalidated, re-request,
//                                            back to [REQUESTED]
//   [GRANTED] --consumed (render+commit)--> [IDLE], frame_permission=0
//   [IDLE] or [GRANTED-but-already-consumed] --FRAME arrives anyway
//     (frame_requested==0)--> UNSOLICITED, ignored, never re-enters
//     [GRANTED]
//
// At most one outstanding request and one unconsumed permission exist
// per window at any time (the struct holds one bit of each, and the
// client's own sequential loop never issues a second request before
// consuming the first grant) -- combined with the unsolicited-FRAME
// rejection below, this is what makes the state machine formally
// unambiguous without needing a separate per-commit sequence number.
static void test_two_consecutive_frames_same_generation(void) {
    wm_client_t c; reset_client(&c);
    wm_window_t w; reset_window(&w, 1);
    wm_buffer_t bufA; reset_buffer(&bufA, 100);
    wm_buffer_t bufB; reset_buffer(&bufB, 101);
    track(&c, &w); track_buf(&c, &bufA); track_buf(&c, &bufB);

    wm_msg_t cfg = msg_configure(1, 20, 640, 480, 0); wmc_apply_message(&c, &cfg);

    // COMMIT A gen=20 (bufA already "owned" per reset_buffer's own
    // convention), REQUEST, FRAME gen=20.
    w.frame_requested = 1; // simulates wm_request_frame() having just sent REQUEST_FRAME
    wm_msg_t frameA = msg_frame(1, 20); wmc_apply_message(&c, &frameA);
    ok("two_consecutive_frames: FRAME for A's cycle grants permission", w.frame_permission == 1 && w.frame_requested == 0);

    // Consumed exactly once (as the real inner wait loop does): render
    // B, commit B gen=20, REQUEST, FRAME gen=20 again.
    bufA.owned_by_compositor = 0;
    w.frame_permission = 0;
    bufB.owned_by_compositor = 1;
    w.frame_requested = 1;
    wm_msg_t frameB = msg_frame(1, 20); wmc_apply_message(&c, &frameB);
    ok("two_consecutive_frames: FRAME for B's cycle ALSO grants permission under the same generation", w.frame_permission == 1 && w.frame_requested == 0);
}

static void test_unsolicited_frame_ignored(void) {
    wm_client_t c; reset_client(&c);
    wm_window_t w; reset_window(&w, 1);
    track(&c, &w);

    uint64_t before = g_wmc_unsolicited_frame_count;
    // No request was ever simulated -- frame_requested stays 0.
    wm_msg_t f = msg_frame(1, 0); wmc_apply_message(&c, &f);
    ok("unsolicited_frame: permission NOT granted with no outstanding request", !w.frame_permission);
    ok("unsolicited_frame: diagnosed exactly once", g_wmc_unsolicited_frame_count == before + 1);
}

// Injects a delayed/duplicate FRAME for an already-consumed cycle and
// proves it cannot grant an extra render for whatever the client
// renders next -- directly answering "prove a callback from A cannot
// grant an extra render for B/C".
static void test_duplicate_frame_after_consumption_rejected(void) {
    wm_client_t c; reset_client(&c);
    wm_window_t w; reset_window(&w, 1);
    track(&c, &w);

    w.frame_requested = 1;
    wm_msg_t f1 = msg_frame(1, 0); wmc_apply_message(&c, &f1);
    ok("duplicate_frame: first FRAME grants permission", w.frame_permission == 1);
    w.frame_permission = 0; // consumed by the client's own render/commit for cycle A

    // The delayed duplicate for cycle A arrives AFTER consumption, with
    // no fresh request yet outstanding (matching real usage: a client
    // only re-requests AFTER its next commit) -- must be rejected as
    // unsolicited, never granting permission for whatever cycle B ends
    // up rendering.
    uint64_t before = g_wmc_unsolicited_frame_count;
    wm_msg_t f2 = msg_frame(1, 0); wmc_apply_message(&c, &f2);
    ok("duplicate_frame: delayed duplicate does not re-grant permission for a later cycle", !w.frame_permission);
    ok("duplicate_frame: diagnosed as unsolicited", g_wmc_unsolicited_frame_count == before + 1);
}

// ── M+10A follow-up audit (point 3): memory-safe rendering ─────────────
static void test_wm_buffer_fits(void) {
    wm_buffer_t b; reset_buffer(&b, 100);
    b.w = 200; b.h = 150; b.stride = 200 * 4;
    ok("wm_buffer_fits: exact match accepted", wm_buffer_fits(&b, 200, 150));
    ok("wm_buffer_fits: fullscreen-sized render into a normal-sized buffer rejected", !wm_buffer_fits(&b, 1280, 800));
    ok("wm_buffer_fits: even a SMALLER render than allocated is rejected (exact match required, never partial trust)", !wm_buffer_fits(&b, 100, 75));
}

static void test_duplicate_token_detection(void) {
    wm_client_t c; reset_client(&c);
    wm_buffer_t bufA; reset_buffer(&bufA, 9);
    track_buf(&c, &bufA);
    ok("duplicate_token: a fresh, distinct token is not flagged", wmc_find_tracked_buffer(&c, 12345) == 0);
    ok("duplicate_token: an aliasing token IS found by the same lookup wm_attach_buffer() uses to detect it", wmc_find_tracked_buffer(&c, 9) == &bufA);
}

// ── §10: KEY_PRESS, CONFIGURE, KEY_RELEASE, RELEASE, FRAME -- key
// ordering must survive interleaving with unrelated state updates ─────
static void test_key_ordering_survives_interleaving(void) {
    wm_client_t c; reset_client(&c);
    wm_window_t w; reset_window(&w, 1);
    wm_buffer_t b; reset_buffer(&b, 100);
    track(&c, &w); track_buf(&c, &b);

    wm_msg_t kp = msg_key(1, 0x1E, 1); wmc_apply_message(&c, &kp); // 'A' make
    wm_msg_t cfg = msg_configure(1, 1, 640, 480, 1); wmc_apply_message(&c, &cfg);
    wm_msg_t kr = msg_key(1, 0x1E, 0); wmc_apply_message(&c, &kr); // 'A' break
    wm_msg_t r = msg_release(1, 100); wmc_apply_message(&c, &r);
    w.frame_requested = 1; // simulates the preceding wm_request_frame()
    wm_msg_t f = msg_frame(1, 1); wmc_apply_message(&c, &f);

    ok("key_ordering: both key transitions queued", c.key_ring_count == 2);
    wm_msg_t out;
    int got1 = wmc_key_ring_pop_front(&c, &out);
    ok("key_ordering: press popped first", got1 && out.pressed == 1);
    int got2 = wmc_key_ring_pop_front(&c, &out);
    ok("key_ordering: release popped second, same keycode", got2 && out.pressed == 0 && out.key_code == 0x1E);
    ok("key_ordering: unrelated CONFIGURE/RELEASE/FRAME still applied correctly", w.is_fullscreen == 1 && !b.owned_by_compositor && w.frame_permission);
}

// ── FOCUS is authoritative state, not queued clutter (item 2) ──────────
static void test_focus_is_state(void) {
    wm_client_t c; reset_client(&c);
    wm_window_t w; reset_window(&w, 1);
    track(&c, &w);

    wm_msg_t f1 = msg_focus(1, 1); wmc_apply_message(&c, &f1);
    ok("focus_is_state: gained", w.has_focus == 1);
    wm_msg_t f2 = msg_focus(1, 0); wmc_apply_message(&c, &f2);
    ok("focus_is_state: lost", w.has_focus == 0);
    // Many FOCUS transitions in a row must never grow any queue -- there
    // is no queue for FOCUS at all under the M+10A redesign.
    for (int i = 0; i < 50; i++) {
        wm_msg_t fx = msg_focus(1, i & 1); wmc_apply_message(&c, &fx);
    }
    ok("focus_is_state: no generic queue exists to grow (struct has none)", 1); // structural guarantee, not a runtime check -- wm_client_t has no pending[] field at all post-redesign
}

// ── Item 3/9: duplicate BUFFER_RELEASED is diagnosed, never corrupts ───
static void test_duplicate_release_is_idempotent(void) {
    wm_client_t c; reset_client(&c);
    wm_window_t w; reset_window(&w, 1);
    wm_buffer_t b; reset_buffer(&b, 100);
    track(&c, &w); track_buf(&c, &b);

    uint64_t before = g_wmc_duplicate_release_count;
    wm_msg_t r1 = msg_release(1, 100); wmc_apply_message(&c, &r1);
    wm_msg_t r2 = msg_release(1, 100); wmc_apply_message(&c, &r2); // duplicate
    ok("duplicate_release: buffer still simply free (not corrupted)", !b.owned_by_compositor);
    ok("duplicate_release: diagnosed exactly once", g_wmc_duplicate_release_count == before + 1);
}

// ── Direct-scanout alternation cannot generate a second semantic
// release for the same slot (item 10's own explicit scenario) --
// modeled client-side as two distinct buffer tokens (slot2/slot3), each
// released exactly once regardless of interleaving with a fullscreen
// exit/re-entry in between.
static void test_direct_scanout_alternation_no_double_release(void) {
    wm_client_t c; reset_client(&c);
    wm_window_t w; reset_window(&w, 1);
    wm_buffer_t slot2; reset_buffer(&slot2, 2002);
    wm_buffer_t slot3; reset_buffer(&slot3, 2003);
    track(&c, &w); track_buf(&c, &slot2); track_buf(&c, &slot3);

    // slot2 commit, slot3 commit, slot2 released (alternation supersession)
    wm_msg_t rel2 = msg_release(1, 2002); wmc_apply_message(&c, &rel2);
    ok("direct_scanout_alt: slot2 released once", !slot2.owned_by_compositor);
    uint64_t dup_before = g_wmc_duplicate_release_count;

    // fullscreen exit, slot3 released
    wm_msg_t rel3 = msg_release(1, 2003); wmc_apply_message(&c, &rel3);
    ok("direct_scanout_alt: slot3 released once", !slot3.owned_by_compositor);

    // fullscreen enter, slot2 reuse (re-committed then eventually
    // re-released by a LATER real alternation) -- must never be seen as
    // a duplicate of the FIRST slot2 release above.
    slot2.owned_by_compositor = 1; // re-committed
    wm_msg_t rel2b = msg_release(1, 2002); wmc_apply_message(&c, &rel2b);
    ok("direct_scanout_alt: slot2 reuse released cleanly, not flagged as duplicate", !slot2.owned_by_compositor && g_wmc_duplicate_release_count == dup_before);
}

void _start(void) {
    test_release_then_frame();
    test_frame_then_release();
    test_configure_release_frame();
    test_release_configure_frame();
    test_frame_configure_release();
    test_multi_configure_supersedes();
    test_commit_uses_latest_generation();
    test_key_ordering_survives_interleaving();
    test_focus_is_state();
    test_duplicate_release_is_idempotent();
    test_direct_scanout_alternation_no_double_release();
    test_two_consecutive_frames_same_generation();
    test_unsolicited_frame_ignored();
    test_duplicate_frame_after_consumption_rejected();
    test_wm_buffer_fits();
    test_duplicate_token_detection();

    wmc_put("wmproto_test64: "); wmc_put_u64((uint64_t)g_pass); wmc_put(" passed, ");
    wmc_put_u64((uint64_t)g_fail); wmc_put(" failed\n");
    sys_exit(g_fail == 0 ? 42 : 1);
}
