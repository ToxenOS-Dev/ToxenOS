// ToxenOS/user64/compositor_gen_test64.c — M+10A follow-up audit (point
// 1): integration test proving compositor64.c's own COMMIT_BUFFER
// generation enforcement. Unlike wmproto_test64.c's pure client-side
// state-machine tests, this needs a REAL running compositor -- it
// exercises the actual wire protocol end to end. Spawned automatically
// by init64.c once the graphical session is up; prints PASS/FAIL and
// exits 42 on full success, matching wmproto_test64.c's own convention.
//
// A client cannot query compositor internals directly (no such message
// exists, deliberately -- see wmproto64.h's own minimal-protocol
// philosophy), so this proves rejection/acceptance OBSERVABLY: a
// REJECTED commit must release ITS OWN just-committed buffer back
// immediately (the compositor never adopts it), while an ACCEPTED
// commit instead releases the PREVIOUSLY committed buffer (old_slot).
// Watching which buffer's BUFFER_RELEASED arrives after each commit
// distinguishes "rejected" from "accepted" without any introspection.
#include <stdint.h>
#include "tox64.h"
#include "wmclient64.h"

#define WIN_W 64
#define WIN_H 48

static int g_pass = 0, g_fail = 0;
static void ok(const char* name, int cond) {
    if (cond) g_pass++;
    else { g_fail++; wmc_put("FAIL: "); wmc_put(name); wmc_put("\n"); }
}

// Sends COMMIT_BUFFER with an EXPLICIT (possibly wrong) generation,
// bypassing wm_commit_buffer()'s own "always stamp with the window's
// own current configure_generation" behavior -- needed to construct the
// stale/impossible scenarios this test exists to prove compositor64.c
// rejects correctly.
static int commit_with_generation(wm_client_t* c, wm_buffer_t* buf, wm_window_t* win, uint32_t generation) {
    wm_msg_t m; for (uint64_t i = 0; i < sizeof(m); i++) ((char*)&m)[i] = 0;
    m.type = WM_MSG_COMMIT_BUFFER;
    m.version = WM_PROTO_VERSION;
    m.client_id = c->client_id;
    m.window_id = win->window_id;
    m.shm_token = buf->token;
    m.w = WIN_W; m.h = WIN_H;
    m.generation = generation;
    if (wmc_send(c, &m) < 0) return -1;
    buf->owned_by_compositor = 1; // matches wm_commit_buffer()'s own real behavior: ownership transfers the instant the message is sent, regardless of what the compositor decides
    return 0;
}

// Non-blocking dispatch, LOCAL to this test file only -- a real client
// should always use the library's own blocking wmc_dispatch_one() (that
// IS the correct behavior: block until genuine progress is possible).
// This diagnostic harness needs the opposite: a way to give up after a
// bounded WALL-CLOCK time, never an iteration count -- bounding
// iterations alone cannot bound time when the underlying primitive is a
// blocking read, since a SINGLE such call can block forever if the
// expected message genuinely never arrives (exactly the failure mode
// this whole audit exists to catch), which would otherwise stall the
// entire boot sequence (init64.c waits on this process before spawning
// anything else).
static int try_dispatch_one(wm_client_t* c, wm_msg_t* out) {
    wm_msg_t m;
    int64_t n = sys_handle_try_read(c->evt_r, (char*)&m, sizeof(m));
    if (n != (int64_t)sizeof(m)) return 0; // nothing available yet (or EOF/error -- no progress either way)
    wmc_apply_message(c, &m);
    if (out) *out = m;
    return 1;
}

#define WAIT_TIMEOUT_TICKS 300 // ~3s at the kernel's fixed 100Hz tick rate -- generous for a same-machine round trip, never near the M+10A stall watchdog's own 2s threshold by coincidence, just a bounded diagnostic budget

// Blocks (via bounded polling, genuinely time-bounded) until a
// BUFFER_RELEASED for ANY tracked buffer arrives, and returns which one.
static wm_buffer_t* wait_for_any_release(wm_client_t* c, uint64_t timeout_ticks) {
    uint64_t deadline = sys_get_ticks() + timeout_ticks;
    while (sys_get_ticks() < deadline) {
        wm_msg_t m;
        if (try_dispatch_one(c, &m)) {
            if (m.type == WM_MSG_BUFFER_RELEASED) return wmc_find_tracked_buffer(c, m.shm_token);
            continue; // got a different message -- keep polling within the same deadline
        }
        sys_sleep_ticks(1);
    }
    wmc_put("  (wait_for_any_release: timed out without a release)\n");
    return 0;
}

// Same time-bounded discipline for a CONFIGURE reaching at least
// `min_generation`. Returns 1 on success, 0 on timeout.
static int wait_for_configure_at_least(wm_client_t* c, wm_window_t* win, uint32_t min_generation, uint64_t timeout_ticks) {
    uint64_t deadline = sys_get_ticks() + timeout_ticks;
    while (sys_get_ticks() < deadline) {
        if (win->configure_generation >= min_generation) return 1;
        if (!try_dispatch_one(c, 0)) sys_sleep_ticks(1);
    }
    return win->configure_generation >= min_generation;
}

void _start(void) {
    wm_client_t c;
    if (wm_connect(&c) < 0) sys_exit(1);
    wm_window_t win;
    uint32_t win_id = wm_create_window(&c, WIN_W, WIN_H, "GenTest", &win);
    if (win_id == 0) sys_exit(1);

    wm_buffer_t bufA, bufB;
    if (wm_attach_buffer(&c, win_id, WIN_W, WIN_H, &bufA) < 0) sys_exit(1);
    if (wm_attach_buffer(&c, win_id, WIN_W, WIN_H, &bufB) < 0) sys_exit(1);

    // Startup commit at the pre-CONFIGURE generation (0 on both sides,
    // per the redesign's own "initial generation" contract) -- must be
    // ACCEPTED, since it IS current by definition. Its own ACK is never
    // explicitly drained here -- a bounded loop calling wmc_dispatch_one()
    // more times than there are actually queued messages would itself
    // block forever on each excess call (bounding the ITERATION COUNT
    // does not bound the wall-clock time of one blocking read). The ACK
    // is simply left to be picked up, harmlessly, by a later dispatch
    // call below -- nothing is currently waiting on it.
    if (commit_with_generation(&c, &bufA, &win, 0) < 0) sys_exit(1);

    // Enter fullscreen -> CONFIGURE(gen=N). Exit -> CONFIGURE(gen=N+1).
    // Real generations, not literal 40/41 -- the redesign's own contract
    // only requires them to be distinct and increasing, which real
    // SET_FULLSCREEN round trips already guarantee.
    wm_set_fullscreen(&c, &win, 1);
    int got_cfg1 = wait_for_configure_at_least(&c, &win, 1, WAIT_TIMEOUT_TICKS);
    ok("setup: CONFIGURE for fullscreen-enter arrived", got_cfg1);
    uint32_t gen1 = win.configure_generation;
    if (!got_cfg1 || gen1 == 0) {
        wmc_put("compositor_gen_test64: aborting early -- no initial CONFIGURE\n");
        wmc_put_u64((uint64_t)g_pass); wmc_put(" passed, "); wmc_put_u64((uint64_t)(g_fail + 1)); wmc_put(" failed\n");
        sys_exit(1);
    }

    wm_set_fullscreen(&c, &win, 0);
    int got_cfg2 = wait_for_configure_at_least(&c, &win, gen1 + 1, WAIT_TIMEOUT_TICKS);
    ok("setup: CONFIGURE for fullscreen-exit arrived", got_cfg2);
    uint32_t gen2 = win.configure_generation;
    ok("setup: two distinct increasing generations observed", gen2 > gen1);
    if (!got_cfg2 || gen2 <= gen1) {
        wmc_put("compositor_gen_test64: aborting early -- second CONFIGURE never arrived\n");
        wmc_put_u64((uint64_t)g_pass); wmc_put(" passed, "); wmc_put_u64((uint64_t)(g_fail + 1)); wmc_put(" failed\n");
        sys_exit(1);
    }

    // ── CONFIGURE gen1, CONFIGURE gen2, [stale] COMMIT gen1 ──
    // A stale commit (commit.gen < current) must be rejected: release
    // ITS OWN buffer (B) immediately, leave the real current content
    // (A, from the startup commit) completely untouched.
    if (commit_with_generation(&c, &bufB, &win, gen1) < 0) sys_exit(1);
    wm_buffer_t* r1 = wait_for_any_release(&c, WAIT_TIMEOUT_TICKS);
    ok("stale_commit(gen<current): the STALE buffer (B) bounces back, never adopted", r1 == &bufB);
    ok("stale_commit(gen<current): the real current buffer (A) is untouched", bufA.owned_by_compositor == 1);

    // ── impossible: COMMIT gen2+100 (commit.gen > current) ──
    // Same rejection path, distinctly diagnosed compositor-side.
    if (commit_with_generation(&c, &bufB, &win, gen2 + 100) < 0) sys_exit(1);
    wm_buffer_t* r2 = wait_for_any_release(&c, WAIT_TIMEOUT_TICKS);
    ok("impossible_commit(gen>current): the offending buffer (B) bounces back", r2 == &bufB);
    ok("impossible_commit(gen>current): buffer A is still untouched", bufA.owned_by_compositor == 1);

    // ── valid: COMMIT gen2 (commit.gen == current) ──
    // Must be ACCEPTED -- proven by the PREVIOUSLY committed buffer (A)
    // being released as old_slot, which only happens when B genuinely
    // became the new committed_slot.
    if (commit_with_generation(&c, &bufB, &win, gen2) < 0) sys_exit(1);
    wm_buffer_t* r3 = wait_for_any_release(&c, WAIT_TIMEOUT_TICKS);
    ok("valid_commit(gen==current): the OLD buffer (A) is released as superseded", r3 == &bufA);

    // ── late stale: CONFIGURE gen2 already current, COMMIT gen1 arrives late ──
    // Must still be rejected -- and must not disturb B's now-current status.
    if (commit_with_generation(&c, &bufA, &win, gen1) < 0) sys_exit(1);
    wm_buffer_t* r4 = wait_for_any_release(&c, WAIT_TIMEOUT_TICKS);
    ok("late_stale_commit: the late/stale buffer (A) bounces straight back", r4 == &bufA);

    wmc_put("compositor_gen_test64: "); wmc_put_u64((uint64_t)g_pass); wmc_put(" passed, ");
    wmc_put_u64((uint64_t)g_fail); wmc_put(" failed\n");
    sys_exit(g_fail == 0 ? 42 : 1);
}
