// ToxenOS/user64/service_test64.c — Milestone 32: real cross-process
// syscall-ABI test, driven by kernel/service64.c's self-test
// (test_ring3_driver) -- mirrors user64/pipe_test64.c's/
// user64/shm_test64.c's own "self-spawning driver" pattern exactly.
//
// Covers the two things Milestone 32 added that genuinely need TWO
// real scheduled processes to prove (everything else is covered by
// kernel/service64.c's own standalone self-test cases against synthetic
// process64_t values):
//
//   1. A REAL blocking sys_service_connect (blocking=1) issued BEFORE
//      the service exists, genuinely woken once a second process
//      publishes it -- not just "the syscall path works when the
//      service already exists" (which would prove far less).
//   2. SYS64_SPAWN_EX's explicit-list inheritance: a child spawned with
//      exactly ONE parent handle number listed must be able to use
//      THAT handle and must NOT be able to use another handle the
//      parent also had open at spawn time (proving it truly did not
//      get the whole table, unlike plain SYS64_SPAWN).
//
// With NO args, this program is the DRIVER, exiting 42 iff every
// scenario passes. With args, it plays one of two child roles (see
// _start below).
#include <stdint.h>
#include "tox64.h"

#define SELF_PATH "/service_test64.nex64"
#define OK_EXIT   42
#define TEST_SERVICE_NAME "svc.ring3.test"

static int my_strlen(const char* s) { int i = 0; while (s[i]) i++; return i; }
static void put(const char* s) { sys_write(s, (uint64_t)my_strlen(s)); }

// ── Role 'c': blocking-connect child ────────────────────────────────
// args = "c<ready_write_handle>" (one digit -- PROCESS64_MAX_HANDLES is
// 16, but the driver always hands this role a single-digit slot number
// by construction below). Signals readiness over an ALREADY-INHERITED
// plain pipe (proving the driver's own sys_pipe_create + ordinary
// SYS64_SPAWN whole-table inheritance still works unchanged, exactly
// as every earlier milestone's ring3 driver already relies on), THEN
// calls a genuinely blocking sys_service_connect for a service that
// (deterministically, thanks to the readiness handshake) does not
// exist yet. If the connect ever returns without correctly completing
// the request/reply exchange below, this returns non-42.
static int run_connect_child(int ready_w) {
    uint8_t one = 'R';
    sys_handle_write(ready_w, (const char*)&one, 1);
    sys_handle_close(ready_w);

    service64_endpoints_t ep;
    if (sys_service_connect(TEST_SERVICE_NAME, 1 /* blocking */, &ep) < 0) return 0;

    // Real bidirectional traffic over the freshly-established pipes --
    // proves the connection isn't just "handles exist" but genuinely
    // carries data both ways.
    uint8_t buf[4];
    if (sys_handle_write(ep.send, "ping", 4) != 4) return 0;
    if (sys_handle_read(ep.recv, (char*)buf, 4) != 4) return 0;
    if (buf[0] != 'p' || buf[1] != 'o' || buf[2] != 'n' || buf[3] != 'g') return 0;
    return 1;
}

// ── Role 's': spawn_ex explicit-inheritance child ───────────────────
// args = "s<inherited_slot><not_inherited_slot>" (two digits). Must be
// able to use the FIRST (explicitly listed) handle and must NOT be
// able to use the SECOND (a real handle the parent had open at spawn
// time, deliberately left off the inherit list).
static int run_spawnex_child(int inherited_slot, int not_inherited_slot) {
    uint8_t z = 'Z';
    if (sys_handle_write(inherited_slot, (const char*)&z, 1) != 1) return 0;
    // Must fail cleanly -- this slot was never in the inherit list, so
    // it must still read as UNUSED in this (fresh, otherwise-empty)
    // child's own handle table, exactly like any other never-opened
    // handle number.
    if (sys_handle_write(not_inherited_slot, (const char*)&z, 1) != -1) return 0;
    return 1;
}

// ── Driver scenario 1: nonexistent service, non-blocking, clean fail ─
static int scenario_connect_nonexistent_nonblocking(void) {
    service64_endpoints_t ep;
    return sys_service_connect("svc.ring3.nonexistent", 0 /* non-blocking */, &ep) < 0;
}

// ── Driver scenario 2: genuine blocking connect + accept ────────────
static int scenario_blocking_connect_then_register(void) {
    int ready_r, ready_w;
    if (sys_pipe_create(&ready_r, &ready_w) < 0) return 0;

    char args[8];
    args[0] = 'c';
    args[1] = (char)('0' + ready_w);
    args[2] = 0;
    int64_t pid = sys_spawn(SELF_PATH, args); // plain sys_spawn -- whole-table inherit is fine/expected here
    if (pid < 0) { sys_handle_close(ready_r); sys_handle_close(ready_w); return 0; }
    sys_handle_close(ready_w); // this process only reads the ready signal

    uint8_t r;
    if (sys_handle_read(ready_r, (char*)&r, 1) != 1) { sys_handle_close(ready_r); sys_wait((uint32_t)pid); return 0; }
    sys_handle_close(ready_r);

    // The child has signaled it is about to call the blocking connect;
    // a short deliberate delay (same documented "no sleep syscall yet"
    // tradeoff user64/gfx_demo64.c already uses for animation pacing)
    // makes it overwhelmingly likely it is genuinely blocked inside
    // sys_service_connect by the time this process registers the
    // service below, rather than merely proving the non-blocking-
    // already-exists path.
    for (volatile int i = 0; i < 200000; i++) { }

    int64_t listen_h = sys_service_listen(TEST_SERVICE_NAME);
    if (listen_h < 0) { sys_wait((uint32_t)pid); return 0; }

    // Non-blocking accept-poll, same style as user64/compositor64.c's
    // main loop -- bounded, not an uncontrolled busy loop.
    service64_endpoints_t sep;
    int accepted = 0;
    for (int tries = 0; tries < 1000000 && !accepted; tries++) {
        int64_t r2 = sys_service_accept((int)listen_h, &sep);
        if (r2 == 0) { accepted = 1; break; }
        if (r2 != SYS64_ERR_WOULDBLOCK) break;
    }
    if (!accepted) { sys_handle_close((int)listen_h); sys_wait((uint32_t)pid); return 0; }

    uint8_t buf[4];
    int ok = (sys_handle_read(sep.recv, (char*)buf, 4) == 4 &&
              buf[0] == 'p' && buf[1] == 'i' && buf[2] == 'n' && buf[3] == 'g');
    if (ok) ok = (sys_handle_write(sep.send, "pong", 4) == 4);

    sys_handle_close((int)listen_h);
    int code = (int)sys_wait((uint32_t)pid);
    return ok && code == OK_EXIT;
}

// ── Driver scenario 3: duplicate registration + restart after exit ──
static int scenario_duplicate_and_restart(void) {
    int64_t h1 = sys_service_listen("svc.ring3.restart");
    if (h1 < 0) return 0;
    int64_t h2 = sys_service_listen("svc.ring3.restart");
    int dup_rejected = (h2 < 0);

    sys_handle_close((int)h1); // unpublish
    int64_t h3 = sys_service_listen("svc.ring3.restart"); // must succeed immediately after
    int restart_ok = (h3 >= 0);
    if (restart_ok) sys_handle_close((int)h3);

    return dup_rejected && restart_ok;
}

// ── Driver scenario 4: SYS64_SPAWN_EX explicit inheritance ──────────
static int scenario_spawn_ex_explicit_inherit(void) {
    int rA, wA, rB, wB;
    if (sys_pipe_create(&rA, &wA) < 0) return 0;
    if (sys_pipe_create(&rB, &wB) < 0) { sys_handle_close(rA); sys_handle_close(wA); return 0; }

    // Inherit ONLY wA -- rA, rB, and wB must all be absent from the
    // child's own handle table, even though rB/wB are real, currently
    // open handles in THIS process at spawn time.
    int32_t inherit_list[1] = { wA };
    spawn_ex_req_t req;
    req.args_ptr = 0;
    req.inherit_ptr = (uint64_t)(uintptr_t)inherit_list;
    req.inherit_count = 1;

    char args[8];
    args[0] = 's';
    args[1] = (char)('0' + wA);
    args[2] = (char)('0' + rB); // a real handle this process has, deliberately never listed
    args[3] = 0;
    req.args_ptr = (uint64_t)(uintptr_t)args;

    int64_t pid = sys_spawn_ex(SELF_PATH, &req);
    int ok = pid >= 0;

    uint8_t got = 0;
    if (ok) ok = (sys_handle_read(rA, (char*)&got, 1) == 1 && got == 'Z');
    if (ok) {
        int code = (int)sys_wait((uint32_t)pid);
        ok = (code == OK_EXIT);
    } else if (pid >= 0) {
        sys_wait((uint32_t)pid);
    }

    // This process's OWN handles must be completely unaffected by what
    // the child could/couldn't see -- rB/wB still fully usable here.
    uint8_t z = 'z';
    if (ok) ok = (sys_handle_write(wB, (const char*)&z, 1) == 1);
    if (ok) { uint8_t g2 = 0; ok = (sys_handle_read(rB, (char*)&g2, 1) == 1 && g2 == 'z'); }

    sys_handle_close(rA); sys_handle_close(wA);
    sys_handle_close(rB); sys_handle_close(wB);
    return ok;
}

static int run_driver(void) {
    if (!scenario_connect_nonexistent_nonblocking())   { put("service_test64: scenario1 FAILED\n"); return 1; }
    if (!scenario_blocking_connect_then_register())     { put("service_test64: scenario2 FAILED\n"); return 2; }
    if (!scenario_duplicate_and_restart())              { put("service_test64: scenario3 FAILED\n"); return 3; }
    if (!scenario_spawn_ex_explicit_inherit())          { put("service_test64: scenario4 FAILED\n"); return 4; }
    return OK_EXIT;
}

void _start(void) {
    char args[16];
    sys_get_args(args, sizeof(args));

    int code;
    if (args[0] == 0) {
        code = run_driver();
    } else if (args[0] == 'c') {
        int ready_w = args[1] - '0';
        code = run_connect_child(ready_w) ? OK_EXIT : 1;
    } else if (args[0] == 's') {
        int inherited = args[1] - '0';
        int not_inherited = args[2] - '0';
        code = run_spawnex_child(inherited, not_inherited) ? OK_EXIT : 1;
    } else {
        code = 1;
    }

    sys_exit(code);
    for (;;) { } // unreachable
}
