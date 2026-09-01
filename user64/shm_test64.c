// ToxenOS/user64/shm_test64.c — Milestone 26: shared-memory syscall ABI
// end-to-end test, driven by kernel/shm64.c's self-test
// (test_ring3_driver). Mirrors user64/pipe_test64.c's self-contained
// driver+worker structure.
//
// With NO args, this process is the DRIVER: it creates real
// shared-memory objects via sys_shm_create/sys_shm_map, spawns further
// instances of ITSELF (as their real parent, so handle inheritance
// applies) with a small role tag telling each child what to do, and
// checks the results (including each child's own exit code). Exits 42
// iff every scenario passes.
//
// Role args (always "<letter>:<handle>[:<extra>]"):
//   "S:<h>"        - map handle h read-write, verify the pattern the
//                    driver already wrote is visible byte-for-byte,
//                    exit 42/1.
//   "V:<h>:<addr>" - like "S", but first does a throwaway 1-page
//                    sys_mmap (so its OWN shm mapping lands at a
//                    different address than the driver's, which never
//                    does this), then asserts its resulting address is
//                    numerically different from the decimal `addr`
//                    (the driver's own mapped address) before
//                    checking the pattern.
//   "O:<h>"        - maps handle h READ-ONLY, then deliberately writes
//                    to it -- expected to fault (kernel/paging64.c's
//                    hardware-enforced PAGE_WRITABLE bit), which
//                    process64_fault_current() reports as exit code
//                    -1. If the write somehow "succeeds" (bug), exits
//                    1 explicitly instead.
//   "C:<h>:<p>"    - combined IPC role: blocks reading a 1-byte signal
//                    from inherited pipe handle p, THEN maps shm
//                    handle h read-only and verifies the pattern --
//                    proves the shared bytes were only supposed to be
//                    trusted once signaled, not polled.
#include <stdint.h>
#include "tox64.h"

#define SELF_PATH "/shm_test64.nex64"
#define OK_EXIT   42
#define PAGE_SIZE 0x1000ULL

static int my_strlen(const char* s) { int i = 0; while (s[i]) i++; return i; }
static void put(const char* s) { sys_write(s, (uint64_t)my_strlen(s)); }

static uint64_t parse_u64(const char* s) {
    uint64_t v = 0;
    while (*s >= '0' && *s <= '9') { v = v * 10 + (uint64_t)(*s - '0'); s++; }
    return v;
}

static void append_u64(char* out, int* pos, uint64_t v) {
    char rev[24]; int rn = 0;
    if (v == 0) rev[rn++] = '0';
    while (v > 0) { rev[rn++] = (char)('0' + (v % 10)); v /= 10; }
    while (rn > 0) out[(*pos)++] = rev[--rn];
}

static void write_pattern(uint8_t* p) {
    for (int i = 0; i < 4096; i++) p[i] = (uint8_t)(i ^ 0x5A);
}

static int check_pattern(const uint8_t* p) {
    for (int i = 0; i < 4096; i++) if (p[i] != (uint8_t)(i ^ 0x5A)) return 0;
    return 1;
}

// ── Worker roles ─────────────────────────────────────────────────────
static int run_share_and_verify(int h) {
    uint64_t addr = sys_shm_map(h, 1);
    if (addr == (uint64_t)-1) return 0;
    return check_pattern((const uint8_t*)addr);
}

static int run_different_va_and_verify(int h, uint64_t other_addr) {
    // A throwaway anon mapping first, purely to bias this process's own
    // mmap-region gap search away from the driver's (which never does
    // this) -- proves the shm mapping's virtual address is a
    // per-process choice, independent of the object's identity.
    if (sys_mmap(PAGE_SIZE) == (uint64_t)-1) return 0;

    uint64_t addr = sys_shm_map(h, 1);
    if (addr == (uint64_t)-1) return 0;
    if (addr == other_addr) { put("shm_test64: VAs unexpectedly matched\n"); return 0; }
    return check_pattern((const uint8_t*)addr);
}

static int run_readonly_reject(int h) {
    uint64_t addr = sys_shm_map(h, 0);
    if (addr == (uint64_t)-1) return 0;
    // Deliberate write to a read-only shared mapping -- the hardware
    // page tables (PAGE_WRITABLE clear) must fault this process. If we
    // ever reach the line after it, permission enforcement is broken.
    *(volatile uint8_t*)addr = 0x42;
    return 0; // only reached if the fault did NOT happen -- a bug
}

static int run_combined_signal(int h, int pipe_read_h, int pipe_write_h) {
    // Whole-table spawn inheritance means this process also inherited
    // the driver's write end -- close it (same discipline as
    // user64/pipe_test64.c's roles) rather than leave a redundant
    // reference open until process exit.
    sys_handle_close(pipe_write_h);

    uint8_t sig;
    int64_t n = sys_handle_read(pipe_read_h, (char*)&sig, 1);
    if (n != 1) return 0;

    uint64_t addr = sys_shm_map(h, 0);
    if (addr == (uint64_t)-1) return 0;
    return check_pattern((const uint8_t*)addr);
}

// ── Driver scenarios ─────────────────────────────────────────────────
static int scenario_share_and_different_vas(void) {
    int64_t h = sys_shm_create(PAGE_SIZE);
    if (h < 0) return 0;
    uint64_t addr = sys_shm_map((int)h, 1);
    if (addr == (uint64_t)-1) { sys_handle_close((int)h); return 0; }
    write_pattern((uint8_t*)addr);

    char args[32];
    int pos = 0;
    args[pos++] = 'V'; args[pos++] = ':';
    append_u64(args, &pos, (uint64_t)h);
    args[pos++] = ':';
    append_u64(args, &pos, addr);
    args[pos] = 0;

    int64_t pid = sys_spawn(SELF_PATH, args);
    if (pid < 0) { sys_munmap(addr, PAGE_SIZE); sys_handle_close((int)h); return 0; }

    int code = (int)sys_wait((uint32_t)pid);
    sys_munmap(addr, PAGE_SIZE);
    sys_handle_close((int)h);
    return code == OK_EXIT;
}

static int scenario_readonly_rejects_write(void) {
    int64_t h = sys_shm_create(PAGE_SIZE);
    if (h < 0) return 0;
    uint64_t addr = sys_shm_map((int)h, 1);
    if (addr == (uint64_t)-1) { sys_handle_close((int)h); return 0; }
    write_pattern((uint8_t*)addr);

    char args[16];
    int pos = 0;
    args[pos++] = 'O'; args[pos++] = ':';
    append_u64(args, &pos, (uint64_t)h);
    args[pos] = 0;

    int64_t pid = sys_spawn(SELF_PATH, args);
    if (pid < 0) { sys_munmap(addr, PAGE_SIZE); sys_handle_close((int)h); return 0; }

    int code = (int)sys_wait((uint32_t)pid);
    sys_munmap(addr, PAGE_SIZE);
    sys_handle_close((int)h);
    return code == -1; // process64_fault_current()'s hardcoded exit code
}

static int scenario_unmap_while_child_uses(void) {
    int64_t h = sys_shm_create(PAGE_SIZE);
    if (h < 0) return 0;
    uint64_t addr = sys_shm_map((int)h, 1);
    if (addr == (uint64_t)-1) { sys_handle_close((int)h); return 0; }
    write_pattern((uint8_t*)addr);

    // Drop OUR mapping (but not the handle) BEFORE the child even maps
    // its own -- the object must stay alive purely because the handle
    // reference (inherited by the child, and still held here) does.
    if (sys_munmap(addr, PAGE_SIZE) < 0) { sys_handle_close((int)h); return 0; }

    char args[16];
    int pos = 0;
    args[pos++] = 'S'; args[pos++] = ':';
    append_u64(args, &pos, (uint64_t)h);
    args[pos] = 0;

    int64_t pid = sys_spawn(SELF_PATH, args);
    if (pid < 0) { sys_handle_close((int)h); return 0; }

    int code = (int)sys_wait((uint32_t)pid);
    sys_handle_close((int)h);
    return code == OK_EXIT;
}

static int scenario_combined_shm_and_pipe_signal(void) {
    int64_t h = sys_shm_create(PAGE_SIZE);
    if (h < 0) return 0;
    uint64_t addr = sys_shm_map((int)h, 1);
    if (addr == (uint64_t)-1) { sys_handle_close((int)h); return 0; }
    write_pattern((uint8_t*)addr);

    int rh, wh;
    if (sys_pipe_create(&rh, &wh) < 0) { sys_munmap(addr, PAGE_SIZE); sys_handle_close((int)h); return 0; }

    char args[32];
    int pos = 0;
    args[pos++] = 'C'; args[pos++] = ':';
    append_u64(args, &pos, (uint64_t)h);
    args[pos++] = ':';
    append_u64(args, &pos, (uint64_t)rh);
    args[pos++] = ':';
    append_u64(args, &pos, (uint64_t)wh);
    args[pos] = 0;

    int64_t pid = sys_spawn(SELF_PATH, args);
    sys_handle_close(rh); // this process only signals -- child inherited its own read end
    if (pid < 0) { sys_handle_close(wh); sys_munmap(addr, PAGE_SIZE); sys_handle_close((int)h); return 0; }

    // Data is already written into shared memory ABOVE, well before the
    // signal -- the pipe byte is what tells the child it's now safe to
    // trust it, not a race with the write itself.
    uint8_t sig = 1;
    int wrote_ok = (sys_handle_write(wh, (const char*)&sig, 1) == 1);
    sys_handle_close(wh);

    int code = (int)sys_wait((uint32_t)pid);
    sys_munmap(addr, PAGE_SIZE);
    sys_handle_close((int)h);
    return wrote_ok && code == OK_EXIT;
}

static int run_driver(void) {
    if (!scenario_share_and_different_vas())      { put("shm_test64: scenario1 FAILED\n"); return 1; }
    if (!scenario_readonly_rejects_write())        { put("shm_test64: scenario2 FAILED\n"); return 2; }
    if (!scenario_unmap_while_child_uses())        { put("shm_test64: scenario3 FAILED\n"); return 3; }
    if (!scenario_combined_shm_and_pipe_signal())  { put("shm_test64: scenario4 FAILED\n"); return 4; }
    return OK_EXIT;
}

void _start(void) {
    char args[64];
    sys_get_args(args, sizeof(args));

    int code;
    if (args[0] == 0) {
        code = run_driver();
    } else {
        char role = args[0];
        // args[1] is always ':' -- fields start at args[2].
        const char* p = args + 2;
        uint64_t h = parse_u64(p);
        while (*p >= '0' && *p <= '9') p++;

        int ok;
        switch (role) {
        case 'S':
            ok = run_share_and_verify((int)h);
            break;
        case 'V': {
            if (*p == ':') p++;
            uint64_t other = parse_u64(p);
            ok = run_different_va_and_verify((int)h, other);
            break;
        }
        case 'O':
            ok = run_readonly_reject((int)h);
            break;
        case 'C': {
            if (*p == ':') p++;
            uint64_t ph = parse_u64(p);
            while (*p >= '0' && *p <= '9') p++;
            if (*p == ':') p++;
            uint64_t wh = parse_u64(p);
            ok = run_combined_signal((int)h, (int)ph, (int)wh);
            break;
        }
        default:
            ok = 0;
            break;
        }
        code = ok ? OK_EXIT : 1;
    }

    sys_exit(code);
    for (;;) { } // unreachable
}
