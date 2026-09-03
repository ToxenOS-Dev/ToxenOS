// ToxenOS/user64/compositor_stall_test64.c — Milestone 32.1: real
// cross-process regression test for the compositor event-delivery
// deadlock fixed this milestone (user64/compositor64.c's queue_event/
// flush_conn_queue). Mirrors user64/pipe_test64.c's/
// user64/service_test64.c's self-spawning-driver pattern.
//
// With NO args, this program is the DRIVER: it spawns a disposable
// TEST compositor instance (/compositor64_test.nex64 -- built from the
// exact same source as the real compositor64.nex64, with one extra
// -DCOMPOSITOR64_TEST_MODE flag that makes it recognize
// WM_MSG_TEST_SHUTDOWN so this driver can end it cleanly afterward
// instead of leaking a permanently-running process that holds the
// exclusive display/input handles), then three client roles:
//
//   'n' (normal)  -- connects, does K real create+destroy-window round
//                    trips, proving ongoing request/reply liveness
//                    with the compositor for the WHOLE test.
//   's' (stalled) -- connects, then fires a burst of raw WM_MSG_SET_TITLE
//                    requests against a window_id that can never exist
//                    (guaranteed WM_ERR_BAD_WINDOW replies with zero
//                    side effects -- deliberately NOT CREATE_WINDOW,
//                    which would consume real, globally shared window
//                    slots and starve the concurrently-running normal
//                    client for an unrelated reason) WITHOUT EVER
//                    reading a single reply. Once the burst exceeds the
//                    compositor's bounded per-connection queue, the
//                    compositor's defined overflow policy (disconnect
//                    the stalled connection) must kick in -- detected
//                    here by a subsequent write failing with a broken
//                    pipe. Signals "flood attempted" to the driver over
//                    an inherited plain pipe once done.
//   'l' (late)    -- spawned by the driver only AFTER 's' has already
//                    signaled, proving the compositor still accepts
//                    brand new connections while a stalled client
//                    exists/is being torn down. Also sends
//                    WM_MSG_TEST_SHUTDOWN once connected, ending the
//                    disposable test compositor.
//
// The driver's own exit code is OK_EXIT (42) iff every one of these
// holds; a distinct non-42 code identifies which stage failed, purely
// for interactive debugging (kernel/pipe64.c-style self-tests only
// check for exact equality with OK_EXIT).
#include <stdint.h>
#include "tox64.h"
#include "wmclient64.h"

#define SELF_PATH  "/compositor_stall_test64.nex64"
#define COMP_PATH  "/compositor64_test.nex64"
#define OK_EXIT    42

#define WIN_W 40
#define WIN_H 30

static int my_strlen(const char* s) { int i = 0; while (s[i]) i++; return i; }
static void put(const char* s) { sys_write(s, (uint64_t)my_strlen(s)); }

static void zero_msg(wm_msg_t* m) {
    for (uint64_t i = 0; i < sizeof(*m); i++) ((char*)m)[i] = 0;
}

static void put_int(const char* prefix, int v) {
    char buf[32]; int n = 0;
    if (v == 0) buf[n++] = '0';
    int neg = v < 0; if (neg) v = -v;
    char tmp[16]; int tn = 0;
    while (v > 0) { tmp[tn++] = (char)('0' + (v % 10)); v /= 10; }
    if (neg) buf[n++] = '-';
    while (tn > 0) buf[n++] = tmp[--tn];
    buf[n++] = '\n'; buf[n] = 0;
    put(prefix); put(buf);
}

// ── Role 'n': normal client -- proves ongoing liveness ───────────────
#define NORMAL_ROUNDS 20

static int run_normal(void) {
    wm_client_t c;
    if (wm_connect(&c) < 0) { put("compositor_stall_test64: normal client: connect failed\n"); return 0; }

    int ok_count = 0;
    for (int i = 0; i < NORMAL_ROUNDS; i++) {
        uint32_t win = wm_create_window(&c, WIN_W, WIN_H, "Normal");
        if (win == 0) { put_int("compositor_stall_test64: normal client: create_window failed at round", i); break; }
        if (wm_destroy_window(&c, win) < 0) { put_int("compositor_stall_test64: normal client: destroy_window failed at round", i); break; }
        ok_count++;
    }
    return ok_count == NORMAL_ROUNDS;
}

// ── Role 's': stalled client -- floods, never reads, expects disconnect ─
#define FLOOD_COUNT 64 // well past CONN_EVT_QUEUE_MAX (32) in compositor64.c

static int run_stalled(int ready_w) {
    wm_client_t c;
    if (wm_connect(&c) < 0) return 0;

    int saw_broken_pipe = 0;
    for (int i = 0; i < FLOOD_COUNT; i++) {
        wm_msg_t m; zero_msg(&m);
        // WM_MSG_SET_TITLE against a window_id that can never exist --
        // always produces exactly one queued WM_ERR_BAD_WINDOW reply,
        // with NO side effect on shared compositor state (unlike
        // WM_MSG_CREATE_WINDOW, which would consume real, GLOBALLY
        // shared window slots and starve the concurrently-running
        // normal client for an unrelated reason). This isolates the
        // test to exactly what it means to exercise: this ONE
        // connection's own outgoing reply queue overflowing.
        m.type = WM_MSG_SET_TITLE;
        m.version = WM_PROTO_VERSION;
        m.client_id = c.client_id;
        m.window_id = 0xFFFFFFFFu;
        // Deliberately never call wmc_recv_expecting/wm_wait_event --
        // the whole point is a client that stops draining its event
        // pipe. wmc_send is a raw blocking sys_handle_write on req_w;
        // if it ever reports broken pipe, the compositor has already
        // torn this connection down mid-flood -- treat that as the
        // expected outcome and stop early rather than writing into a
        // handle that's about to be reused for something else.
        if (wmc_send(&c, &m) < 0) { saw_broken_pipe = 1; break; }
    }

    if (!saw_broken_pipe) {
        // Didn't observe it mid-flood -- check explicitly with one more
        // request now that every queued reply has had a chance to
        // overflow the compositor's bounded per-connection queue.
        wm_msg_t m; zero_msg(&m);
        m.type = WM_MSG_SET_TITLE;
        m.version = WM_PROTO_VERSION;
        m.client_id = c.client_id;
        if (wmc_send(&c, &m) < 0) saw_broken_pipe = 1;
    }

    uint8_t one = 'R';
    sys_handle_write(ready_w, (const char*)&one, 1);
    sys_handle_close(ready_w);

    return saw_broken_pipe;
}

// ── Role 'l': late client -- connects after the flood, ends the test compositor ─
static int run_late(void) {
    wm_client_t c;
    if (wm_connect(&c) < 0) return 0;

    uint32_t win = wm_create_window(&c, WIN_W, WIN_H, "Late");
    int ok = (win != 0);

    wm_msg_t m; zero_msg(&m);
    m.type = WM_MSG_TEST_SHUTDOWN;
    m.version = WM_PROTO_VERSION;
    wmc_send(&c, &m); // no reply expected -- see wmproto64.h

    return ok;
}

// ── Driver ────────────────────────────────────────────────────────────
static int run_driver(void) {
    int64_t comp_pid = sys_spawn(COMP_PATH, "");
    if (comp_pid < 0) { put("compositor_stall_test64: cannot spawn test compositor\n"); return 1; }

    int ready_r, ready_w;
    if (sys_pipe_create(&ready_r, &ready_w) < 0) { sys_wait((uint32_t)comp_pid); return 2; }

    int64_t n_pid = sys_spawn(SELF_PATH, "n");
    if (n_pid < 0) { put("compositor_stall_test64: cannot spawn normal client\n"); return 3; }

    char s_args[4];
    s_args[0] = 's';
    s_args[1] = (char)('0' + ready_w);
    s_args[2] = 0;
    int64_t s_pid = sys_spawn(SELF_PATH, s_args);
    if (s_pid < 0) { put("compositor_stall_test64: cannot spawn stalled client\n"); return 4; }

    sys_handle_close(ready_w); // driver only reads the ready signal

    uint8_t r;
    int64_t got = sys_handle_read(ready_r, (char*)&r, 1); // blocks until 's' finishes its flood
    sys_handle_close(ready_r);
    if (got != 1) { put("compositor_stall_test64: ready signal not received\n"); return 5; }

    // Milestone 32.1: proves the compositor accepts a BRAND NEW
    // connection while the stalled client exists / is being torn down
    // -- spawned only now, deliberately overlapping with 's' still
    // possibly mid-teardown.
    int64_t l_pid = sys_spawn(SELF_PATH, "l");
    if (l_pid < 0) { put("compositor_stall_test64: cannot spawn late client\n"); return 6; }

    int n_ok = (int)sys_wait((uint32_t)n_pid) == OK_EXIT;
    int s_ok = (int)sys_wait((uint32_t)s_pid) == OK_EXIT;
    int l_ok = (int)sys_wait((uint32_t)l_pid) == OK_EXIT;
    int c_ok = (int)sys_wait((uint32_t)comp_pid) == 0; // clean exit via WM_MSG_TEST_SHUTDOWN

    if (!n_ok) { put("compositor_stall_test64: normal client FAILED\n"); return 7; }
    if (!s_ok) { put("compositor_stall_test64: stalled client was never disconnected\n"); return 8; }
    if (!l_ok) { put("compositor_stall_test64: late client FAILED\n"); return 9; }
    if (!c_ok) { put("compositor_stall_test64: test compositor did not exit cleanly\n"); return 10; }

    return OK_EXIT;
}

void _start(void) {
    char args[16];
    sys_get_args(args, sizeof(args));

    int code;
    if (args[0] == 0) {
        code = run_driver();
    } else if (args[0] == 'n') {
        code = run_normal() ? OK_EXIT : 1;
    } else if (args[0] == 's') {
        int ready_w = args[1] - '0';
        code = run_stalled(ready_w) ? OK_EXIT : 1;
    } else if (args[0] == 'l') {
        code = run_late() ? OK_EXIT : 1;
    } else {
        code = 1;
    }

    sys_exit(code);
    for (;;) { } // unreachable
}
