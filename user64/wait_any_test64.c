// ToxenOS/user64/wait_any_test64.c — M+12B: SYS64_HANDLE_WAIT_ANY
// syscall ABI end-to-end test, driven by kernel/process64.c's own
// self-test (a new test_ring3_wait_any_driver case in
// process64_selftest). Mirrors user64/pipe_test64.c's own self-
// contained driver+worker pattern exactly: with no args, this program
// IS the driver -- it creates real pipes/services, spawns further
// instances of ITSELF (sys_spawn, whole-table inheritance, so it is
// their real parent) with a tiny role tag as args, and drives each
// scenario to completion. Exits 42 iff every scenario passes.
//
// Every wait_any call here uses a bounded, generous timeout (never
// SYS64_WAIT_FOREVER) -- a self-contained automated test must never be
// able to hang the whole kernel selftest sequence; a bug that breaks
// wakeup detection must show up as a diagnosable TIMEOUT failure, not
// an indefinite hang.
#include <stdint.h>
#include "tox64.h"

#define SELF_PATH "/wait_any_test64.nex64"
#define OK_EXIT   42

// Generous relative to how long the delayed-writer children actually
// sleep (WORKER_DELAY_TICKS) -- gives huge headroom while still
// guaranteeing the whole suite terminates in well under a second even
// if something is broken.
#define WORKER_DELAY_TICKS 15
#define WAIT_TIMEOUT_TICKS 150

static int my_strlen(const char* s) { int i = 0; while (s[i]) i++; return i; }
static void put(const char* s) { sys_write(s, (uint64_t)my_strlen(s)); }

static void build_role_args(char* out, char role, int a, int b) {
    out[0] = role;
    out[1] = (char)('0' + a);
    out[2] = (char)('0' + b);
    out[3] = 0;
}

// ── Worker roles ─────────────────────────────────────────────────────
// 'w': sleep, then write one byte to the inherited write handle `wh`
// (closing the inherited read handle `rh` first, same discipline
// pipe_test64.c already established for whole-table inheritance).
static int run_delayed_writer(int wh, int rh) {
    sys_handle_close(rh);
    sys_sleep_ticks(WORKER_DELAY_TICKS);
    uint8_t b = 'Z';
    return sys_handle_write(wh, (const char*)&b, 1) == 1;
}

// 'x': sleep, then CLOSE the inherited write handle `wh` without ever
// writing to it (closing the inherited read handle `rh` first) --
// the exact "handle closes while waited on" trigger: if this is the
// LAST writer, pipe64_close_write wakes the read side.
static int run_delayed_closer(int wh, int rh) {
    sys_handle_close(rh);
    sys_sleep_ticks(WORKER_DELAY_TICKS);
    return sys_handle_close(wh) == 0;
}

// 'c': sleep, then connect to the named service already published by
// the driver -- the exact "new client connects to an already-published
// listener" trigger for service64_t::wait_chan.
#define WAIT_ANY_TEST_SVC_NAME "wait_any_test.svc"
static int run_delayed_connector(void) {
    sys_sleep_ticks(WORKER_DELAY_TICKS);
    service64_endpoints_t ep;
    return sys_service_connect(WAIT_ANY_TEST_SVC_NAME, 1, &ep) == 0;
}

// ── Driver scenarios ─────────────────────────────────────────────────

// A: multi-channel wake on any member + actual sleep/wake. Blocks on
// TWO pipes' read ends at once; only the second one ever becomes
// ready (via a delayed child write) -- proves wait_any wakes on
// whichever member actually changes, not just the first in the list.
static int scenario_multi_wake_on_any_member(void) {
    int r1, w1, r2, w2;
    if (sys_pipe_create(&r1, &w1) < 0) return 0;
    if (sys_pipe_create(&r2, &w2) < 0) { sys_handle_close(r1); sys_handle_close(w1); return 0; }

    char args[4];
    build_role_args(args, 'w', w2, r2);
    int64_t pid = sys_spawn(SELF_PATH, args);
    if (pid < 0) { sys_handle_close(r1); sys_handle_close(w1); sys_handle_close(r2); sys_handle_close(w2); return 0; }
    sys_handle_close(w2); // driver doesn't write on pipe2 -- only the child does

    int handles[2] = { r1, r2 };
    int64_t rc = sys_handle_wait_any(handles, 2, WAIT_TIMEOUT_TICKS);
    int ok = (rc == 0);

    // r1 must still be empty (nobody ever wrote to it); r2 must have
    // exactly the child's one byte.
    uint8_t buf;
    if (ok && sys_handle_try_read(r1, (char*)&buf, 1) != SYS64_ERR_WOULDBLOCK) ok = 0;
    if (ok) {
        int64_t n = sys_handle_try_read(r2, (char*)&buf, 1);
        if (n != 1 || buf != 'Z') ok = 0;
    }

    sys_handle_close(r1); sys_handle_close(w1); sys_handle_close(r2);
    int code = (int)sys_wait((uint32_t)pid);
    return ok && code == OK_EXIT;
}

// B: handle-closes-while-waited-on. The driver holds ONLY the read
// end; the child holds the only write end and closes it (without
// writing) after a delay -- writers hits 0, pipe64_close_write wakes
// read_chan, wait_any must return, and the read side must then observe
// EOF.
static int scenario_close_while_waiting(void) {
    int r, w;
    if (sys_pipe_create(&r, &w) < 0) return 0;

    char args[4];
    build_role_args(args, 'x', w, r);
    int64_t pid = sys_spawn(SELF_PATH, args);
    if (pid < 0) { sys_handle_close(r); sys_handle_close(w); return 0; }
    sys_handle_close(w); // driver's own write end -- the CHILD's inherited copy must be the last one

    int handles[1] = { r };
    int64_t rc = sys_handle_wait_any(handles, 1, WAIT_TIMEOUT_TICKS);
    int ok = (rc == 0);

    if (ok) {
        uint8_t buf;
        if (sys_handle_read(r, (char*)&buf, 1) != 0) ok = 0; // EOF -- no writer left, nothing ever buffered
    }

    sys_handle_close(r);
    int code = (int)sys_wait((uint32_t)pid);
    return ok && code == OK_EXIT;
}

// C: duplicate handles in the interest set are harmless -- same chan
// listed twice behaves exactly like once, both for the not-ready
// (poll, timeout=0) and the ready case.
static int scenario_duplicate_handles(void) {
    int r, w;
    if (sys_pipe_create(&r, &w) < 0) return 0;
    int ok = 1;

    int dup_handles[2] = { r, r };
    int64_t rc = sys_handle_wait_any(dup_handles, 2, 0); // poll only -- pipe is empty, writer (this process) still open
    if (rc != SYS64_ERR_TIMEOUT) ok = 0;

    uint8_t b = 'D';
    if (ok && sys_handle_write(w, (const char*)&b, 1) != 1) ok = 0;
    if (ok) {
        rc = sys_handle_wait_any(dup_handles, 2, 0);
        if (rc != 0) ok = 0;
    }
    if (ok) {
        uint8_t got;
        if (sys_handle_try_read(r, (char*)&got, 1) != 1 || got != 'D') ok = 0;
    }

    sys_handle_close(r); sys_handle_close(w);
    return ok;
}

// D: an invalid handle ANYWHERE in the list fails the WHOLE call, and
// nothing is left partially registered -- a subsequent legitimate call
// on just the valid handle still behaves normally afterward.
static int scenario_invalid_handles_all_or_nothing(void) {
    int r, w;
    if (sys_pipe_create(&r, &w) < 0) return 0;
    int ok = 1;

    int bad_handles[2] = { r, 9999 }; // 9999 is well out of PROCESS64_MAX_HANDLES range
    int64_t rc = sys_handle_wait_any(bad_handles, 2, WAIT_TIMEOUT_TICKS);
    if (rc != -1) ok = 0;

    // Prove nothing was left registered/blocked: an ordinary poll on
    // just the valid handle must behave completely normally (WOULDBLOCK
    // -- still empty, still one writer open).
    if (ok) {
        int good_handles[1] = { r };
        rc = sys_handle_wait_any(good_handles, 1, 0);
        if (rc != SYS64_ERR_TIMEOUT) ok = 0;
    }

    sys_handle_close(r); sys_handle_close(w);
    return ok;
}

// E: a handle that's already ready must return 0 immediately, never
// actually block -- verified two ways: the return code itself, and a
// tick-count bound (nothing else in this single-process test could
// possibly wake it if it HAD incorrectly blocked, so a large elapsed
// time here would mean it fell through to the timeout path instead of
// detecting immediate readiness).
static int scenario_immediately_ready(void) {
    int r, w;
    if (sys_pipe_create(&r, &w) < 0) return 0;
    int ok = 1;

    uint8_t b = 'E';
    if (sys_handle_write(w, (const char*)&b, 1) != 1) ok = 0;

    uint64_t before = sys_get_ticks();
    int handles[1] = { r };
    int64_t rc = sys_handle_wait_any(handles, 1, WAIT_TIMEOUT_TICKS);
    uint64_t after = sys_get_ticks();

    if (rc != 0) ok = 0;
    if (ok && (after - before) >= 5) ok = 0; // generous bound -- true immediate-ready is ~0 ticks

    sys_handle_close(r); sys_handle_close(w);
    return ok;
}

// F: after a WAIT_ANY call genuinely times out, an ORDINARY blocking
// single-channel operation (sys_handle_read, process64_block_on
// internally) on this SAME process must still behave completely
// normally afterward -- proves wait_is_multi/wait_any_count were
// correctly cleared on timeout, not left stale to corrupt the next,
// unrelated block.
static int scenario_deadline_then_ordinary_reblock(void) {
    int r1, w1, r2, w2;
    if (sys_pipe_create(&r1, &w1) < 0) return 0;
    if (sys_pipe_create(&r2, &w2) < 0) { sys_handle_close(r1); sys_handle_close(w1); return 0; }
    int ok = 1;

    char args[4];
    build_role_args(args, 'w', w2, r2);
    int64_t pid = sys_spawn(SELF_PATH, args);
    if (pid < 0) { sys_handle_close(r1); sys_handle_close(w1); sys_handle_close(r2); sys_handle_close(w2); return 0; }
    sys_handle_close(w2);

    // Short timeout, well BEFORE the child's own WORKER_DELAY_TICKS
    // write on pipe2 -- pipe1 never gets written at all, so this must
    // genuinely time out.
    int handles[1] = { r1 };
    int64_t rc = sys_handle_wait_any(handles, 1, 3);
    if (rc != SYS64_ERR_TIMEOUT) ok = 0;

    // Now an ordinary BLOCKING read on pipe2 -- the child's delayed
    // write hasn't landed yet (WORKER_DELAY_TICKS=15 > the 3-tick
    // timeout above), so this must actually block via
    // process64_block_on, then wake normally once the child writes.
    if (ok) {
        uint8_t buf;
        int64_t n = sys_handle_read(r2, (char*)&buf, 1);
        if (n != 1 || buf != 'Z') ok = 0;
    }

    sys_handle_close(r1); sys_handle_close(w1); sys_handle_close(r2);
    int code = (int)sys_wait((uint32_t)pid);
    return ok && code == OK_EXIT;
}

// G: a new client connecting to an already-published service wakes a
// WAIT_ANY block on that listener's handle -- before M+12B nothing
// ever woke on this at all (service64_accept was always polled).
static int scenario_service_listener_wake(void) {
    int64_t listen_h = sys_service_listen(WAIT_ANY_TEST_SVC_NAME);
    if (listen_h < 0) return 0;
    int ok = 1;

    int64_t pid = sys_spawn(SELF_PATH, "c00"); // role 'c' ignores its two digit args
    if (pid < 0) { sys_handle_close((int)listen_h); return 0; }

    int handles[1] = { (int)listen_h };
    int64_t rc = sys_handle_wait_any(handles, 1, WAIT_TIMEOUT_TICKS);
    if (rc != 0) ok = 0;

    if (ok) {
        service64_endpoints_t ep;
        if (sys_service_accept((int)listen_h, &ep) != 0) ok = 0;
        else { sys_handle_close(ep.send); sys_handle_close(ep.recv); }
    }

    sys_handle_close((int)listen_h); // unpublishes -- safe even if accept above already ran
    int code = (int)sys_wait((uint32_t)pid);
    return ok && code == OK_EXIT;
}

static int run_driver(void) {
    if (!scenario_multi_wake_on_any_member())        { put("wait_any_test64: scenario A (multi-wake-on-any-member) FAILED\n"); return 1; }
    if (!scenario_close_while_waiting())             { put("wait_any_test64: scenario B (close-while-waiting) FAILED\n"); return 2; }
    if (!scenario_duplicate_handles())               { put("wait_any_test64: scenario C (duplicate-handles) FAILED\n"); return 3; }
    if (!scenario_invalid_handles_all_or_nothing())  { put("wait_any_test64: scenario D (invalid-handles) FAILED\n"); return 4; }
    if (!scenario_immediately_ready())               { put("wait_any_test64: scenario E (immediately-ready) FAILED\n"); return 5; }
    if (!scenario_deadline_then_ordinary_reblock())  { put("wait_any_test64: scenario F (deadline-then-reblock) FAILED\n"); return 6; }
    if (!scenario_service_listener_wake())           { put("wait_any_test64: scenario G (service-listener-wake) FAILED\n"); return 7; }
    return OK_EXIT;
}

void _start(void) {
    char args[16];
    sys_get_args(args, sizeof(args));

    int code;
    if (args[0] == 0) {
        code = run_driver();
    } else {
        int a = args[1] - '0';
        int b = args[2] - '0';
        int ok;
        switch (args[0]) {
        case 'w': ok = run_delayed_writer(a, b); break;
        case 'x': ok = run_delayed_closer(a, b); break;
        case 'c': ok = run_delayed_connector(); break;
        default:  ok = 0; break;
        }
        code = ok ? OK_EXIT : 1;
    }

    sys_exit(code);
    for (;;) { } // unreachable
}
