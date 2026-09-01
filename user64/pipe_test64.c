// ToxenOS/user64/pipe_test64.c — Milestone 26: pipe syscall ABI
// end-to-end test, driven by kernel/pipe64.c's self-test
// (test_ring3_driver).
//
// A single self-contained program, mirroring user64/sched_worker64.c's
// nested-spawn pattern: with NO args, it acts as the DRIVER -- it
// creates real pipes via sys_pipe_create, spawns further instances of
// ITSELF (so it is their real parent and Milestone 26 handle
// inheritance actually triggers) with a tiny role tag as args telling
// each child which side of which scenario to play, and communicates
// with them via sys_handle_read/sys_handle_write. Exits 42 iff every
// scenario passes.
//
// Milestone 26 spawn inheritance is WHOLE-TABLE: a child inherits
// EVERY open handle the parent has at spawn time, not just the one it
// was told about. Exactly like the classic Unix pipe()+fork() idiom,
// each side MUST explicitly close the end it doesn't use -- a child
// that only reads but keeps an inherited copy of the write end open
// would keep the pipe's writer count above zero forever, and a reader
// looping "until EOF" would then block forever even after the real
// writer closes. Role args therefore always carry BOTH handle numbers
// so the child can close the one it doesn't need, exactly as the
// driver itself does on its own side (see each scenario below).
//
// Role args, always exactly 3 characters "<role><use><close>":
//   "r<rh><wh>" - read from <rh> until EOF (closing inherited <wh>
//                 first), expect 'A' bytes.
//   "w<wh><rh>" - write 10 'B' bytes to <wh> (closing inherited <rh>
//                 first), then exit.
//   "b<wh><rh>" - write PIPE_BIG_LEN bytes of an incrementing (mod
//                 256) pattern to <wh> (closing inherited <rh> first)
//                 -- deliberately several times larger than the
//                 pipe's own ring buffer, so this WILL block on a
//                 full pipe partway through unless the other end
//                 keeps draining it.
#include <stdint.h>
#include "tox64.h"

#define SELF_PATH  "/pipe_test64.nex64"
#define OK_EXIT    42

// Must match kernel/pipe64.h's PIPE64_BUF_SIZE -- userspace has no
// access to kernel headers, same precedent as tox64.h's own syscall
// number constants being duplicated from include/syscall64.h.
#define PIPE_BUF_SIZE 256
#define PIPE_BIG_LEN  (3 * PIPE_BUF_SIZE)

static int my_strlen(const char* s) { int i = 0; while (s[i]) i++; return i; }
static void put(const char* s) { sys_write(s, (uint64_t)my_strlen(s)); }

static void build_role_args(char* out, char role, int use_h, int close_h) {
    out[0] = role;
    out[1] = (char)('0' + use_h);
    out[2] = (char)('0' + close_h);
    out[3] = 0;
}

// ── Worker roles ─────────────────────────────────────────────────────
static int run_readchild(int rh, int wh) {
    sys_handle_close(wh); // don't hold the write end open -- see file header
    uint8_t buf[32];
    uint64_t total = 0;
    for (;;) {
        int64_t n = sys_handle_read(rh, (char*)buf, sizeof(buf));
        if (n < 0) return 0; // error -- should never happen here
        if (n == 0) break;   // EOF
        for (int64_t i = 0; i < n; i++) if (buf[i] != 'A') return 0;
        total += (uint64_t)n;
    }
    return total == 10;
}

static int run_writechild(int wh, int rh) {
    sys_handle_close(rh); // don't hold the read end open
    uint8_t buf[10];
    for (int i = 0; i < 10; i++) buf[i] = 'B';
    return sys_handle_write(wh, (const char*)buf, 10) == 10;
}

static int run_bigwriter(int wh, int rh) {
    sys_handle_close(rh); // don't hold the read end open
    uint8_t buf[PIPE_BIG_LEN];
    for (int i = 0; i < PIPE_BIG_LEN; i++) buf[i] = (uint8_t)i;
    // A single sys_handle_write call for the WHOLE thing -- the kernel
    // (pipe64_write) is what must loop/block internally to deliver it
    // all through a ring buffer 1/3 this size.
    return sys_handle_write(wh, (const char*)buf, PIPE_BIG_LEN) == PIPE_BIG_LEN;
}

// ── Driver scenarios ─────────────────────────────────────────────────
static int scenario_parent_writes_child_reads(void) {
    int rh, wh;
    if (sys_pipe_create(&rh, &wh) < 0) return 0;

    char args[4];
    build_role_args(args, 'r', rh, wh);
    int64_t pid = sys_spawn(SELF_PATH, args);
    if (pid < 0) { sys_handle_close(rh); sys_handle_close(wh); return 0; }

    sys_handle_close(rh); // this process only writes

    uint8_t data[10];
    for (int i = 0; i < 10; i++) data[i] = 'A';
    int wrote_ok = (sys_handle_write(wh, (const char*)data, 10) == 10);
    sys_handle_close(wh); // last writer gone -- child's read hits EOF

    int code = (int)sys_wait((uint32_t)pid);
    return wrote_ok && code == OK_EXIT;
}

static int scenario_child_writes_parent_reads(void) {
    int rh, wh;
    if (sys_pipe_create(&rh, &wh) < 0) return 0;

    char args[4];
    build_role_args(args, 'w', wh, rh);
    int64_t pid = sys_spawn(SELF_PATH, args);
    if (pid < 0) { sys_handle_close(rh); sys_handle_close(wh); return 0; }

    sys_handle_close(wh); // this process only reads

    uint8_t buf[16];
    int64_t n = sys_handle_read(rh, (char*)buf, 10);
    int read_ok = (n == 10);
    for (int i = 0; read_ok && i < 10; i++) if (buf[i] != 'B') read_ok = 0;

    sys_handle_close(rh);
    int code = (int)sys_wait((uint32_t)pid);
    return read_ok && code == OK_EXIT;
}

static int scenario_blocking_both_ways(void) {
    int rh, wh;
    if (sys_pipe_create(&rh, &wh) < 0) return 0;

    char args[4];
    build_role_args(args, 'b', wh, rh);
    int64_t pid = sys_spawn(SELF_PATH, args);
    if (pid < 0) { sys_handle_close(rh); sys_handle_close(wh); return 0; }

    sys_handle_close(wh); // this process only reads

    // Deliberately slow (small reads) so the child's single big write
    // genuinely fills the ring buffer and blocks -- at least twice,
    // since PIPE_BIG_LEN == 3x its capacity -- before this side lets it
    // continue by draining more.
    uint64_t got = 0;
    int ok = 1;
    uint8_t expect = 0;
    while (got < PIPE_BIG_LEN) {
        uint8_t chunk[16];
        uint64_t want = PIPE_BIG_LEN - got;
        if (want > sizeof(chunk)) want = sizeof(chunk);
        int64_t n = sys_handle_read(rh, (char*)chunk, want);
        if (n <= 0) { ok = 0; break; }
        for (int64_t i = 0; i < n; i++) {
            if (chunk[i] != expect) ok = 0;
            expect = (uint8_t)(expect + 1);
        }
        got += (uint64_t)n;
    }
    if (got == PIPE_BIG_LEN) {
        uint8_t tmp[4];
        if (sys_handle_read(rh, (char*)tmp, sizeof(tmp)) != 0) ok = 0; // EOF once child exits
    }

    sys_handle_close(rh);
    int code = (int)sys_wait((uint32_t)pid);
    return ok && code == OK_EXIT;
}

static int run_driver(void) {
    if (!scenario_parent_writes_child_reads()) { put("pipe_test64: scenario1 FAILED\n"); return 1; }
    if (!scenario_child_writes_parent_reads()) { put("pipe_test64: scenario2 FAILED\n"); return 2; }
    if (!scenario_blocking_both_ways())        { put("pipe_test64: scenario3 FAILED\n"); return 3; }
    return OK_EXIT;
}

void _start(void) {
    char args[16];
    sys_get_args(args, sizeof(args));

    int code;
    if (args[0] == 0) {
        code = run_driver();
    } else {
        int use_h = args[1] - '0';
        int close_h = args[2] - '0';
        int ok;
        switch (args[0]) {
        case 'r': ok = run_readchild(use_h, close_h); break;
        case 'w': ok = run_writechild(use_h, close_h); break;
        case 'b': ok = run_bigwriter(use_h, close_h); break;
        default:  ok = 0; break;
        }
        code = ok ? OK_EXIT : 1;
    }

    sys_exit(code);
    for (;;) { } // unreachable
}
