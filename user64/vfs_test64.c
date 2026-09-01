// ToxenOS/user64/vfs_test64.c — Milestone 27: VFS + unified handle
// syscall ABI end-to-end test, driven by kernel/vfs64.c's self-test
// (test_ring3_driver). Mirrors user64/pipe_test64.c/shm_test64.c's
// self-contained driver+worker structure.
//
// With NO args, this process is the DRIVER: it exercises real file
// I/O through sys_open/sys_handle_read/sys_handle_write/sys_close and
// spawns further instances of ITSELF (as their real parent, so
// Milestone 26 handle inheritance applies) to prove cross-process
// file-handle behavior that a single process can't demonstrate alone.
// Exits 42 iff every scenario passes.
//
// Role args:
//   "i<h>" - reads inherited file handle h, expects the exact string
//            this driver wrote before spawning, exits 42/1.
//   "s"    - opens TEST_FILE itself (a FRESH, non-inherited handle),
//            reads it, expects the same string, exits 42/1.
#include <stdint.h>
#include "tox64.h"

#define SELF_PATH  "/vfs_test64.nex64"
#define TEST_FILE  "/vfs_test64_tmp"
#define OK_EXIT    42
#define MSG        "hello from parent"
#define MSG_LEN    18

static int my_strlen(const char* s) { int i = 0; while (s[i]) i++; return i; }
static void put(const char* s) { sys_write(s, (uint64_t)my_strlen(s)); }

static int str_eq(const char* a, const char* b) {
    int i = 0;
    while (a[i] && b[i]) { if (a[i] != b[i]) return 0; i++; }
    return a[i] == b[i];
}

// ── Worker roles ─────────────────────────────────────────────────────
static int run_inherited_reader(int h) {
    char buf[32];
    int64_t n = sys_handle_read(h, buf, sizeof(buf) - 1);
    if (n < 0) return 0;
    buf[n] = 0;
    return str_eq(buf, MSG);
}

static int run_simultaneous_reader(void) {
    int64_t fd = sys_open(TEST_FILE);
    if (fd < 0) return 0;
    char buf[32];
    int64_t n = sys_handle_read((int)fd, buf, sizeof(buf) - 1);
    sys_close((int)fd);
    if (n < 0) return 0;
    buf[n] = 0;
    return str_eq(buf, MSG);
}

// ── Driver scenarios ─────────────────────────────────────────────────
// Creates TEST_FILE and writes MSG into it via a real open handle, then
// opens a SECOND, independent handle to the same path (proving each
// sys_open() call produces its own file description with its own
// cursor -- the write handle's cursor is now at EOF, but this fresh
// one starts at 0) and spawns a child that inherits ONLY that second
// handle. Closing both of THIS process's own copies right after spawn
// (before the child even runs) and still getting a correct read back
// in the child proves the underlying open-file object's reference
// count survived a close in one process because the child's inherited
// copy kept it alive.
static int scenario_inherit_and_refcount(void) {
    if (sys_mkfile(TEST_FILE) < 0) return 0;

    int64_t wfd = sys_open(TEST_FILE);
    if (wfd < 0) return 0;
    if (sys_handle_write((int)wfd, MSG, MSG_LEN) != MSG_LEN) { sys_close((int)wfd); return 0; }

    int64_t rfd = sys_open(TEST_FILE);
    if (rfd < 0) { sys_close((int)wfd); return 0; }

    char args[8];
    args[0] = 'i'; args[1] = (char)('0' + (int)rfd); args[2] = 0;
    int64_t pid = sys_spawn(SELF_PATH, args);
    if (pid < 0) { sys_close((int)wfd); sys_close((int)rfd); return 0; }

    sys_close((int)wfd);
    sys_close((int)rfd);

    int code = (int)sys_wait((uint32_t)pid);
    return code == OK_EXIT;
}

// Parent and child each independently sys_open() TEST_FILE (no
// inheritance involved) and read it AT THE SAME TIME (both processes
// resident, real preemption between them) -- proves two simultaneously
// open, unrelated file descriptions of the same path don't interfere.
static int scenario_simultaneous_independent_opens(void) {
    char args[4];
    args[0] = 's'; args[1] = 0;
    int64_t pid = sys_spawn(SELF_PATH, args);
    if (pid < 0) return 0;

    int64_t fd = sys_open(TEST_FILE);
    int ok = 0;
    if (fd >= 0) {
        char buf[32];
        int64_t n = sys_handle_read((int)fd, buf, sizeof(buf) - 1);
        sys_close((int)fd);
        if (n >= 0) { buf[n] = 0; ok = str_eq(buf, MSG); }
    }

    int code = (int)sys_wait((uint32_t)pid);
    return ok && code == OK_EXIT;
}

// A file handle, a pipe (both ends), and a mapped shared-memory object
// all open in ONE process at once -- proves the unified handle table
// doesn't let different object kinds collide or corrupt each other's
// slot/state.
static int scenario_mixed_handles(void) {
    int64_t fd = sys_open(TEST_FILE);
    if (fd < 0) return 0;

    int rh, wh;
    if (sys_pipe_create(&rh, &wh) < 0) { sys_close((int)fd); return 0; }

    int64_t sh = sys_shm_create(0x1000);
    if (sh < 0) { sys_close((int)fd); sys_handle_close(rh); sys_handle_close(wh); return 0; }
    uint64_t addr = sys_shm_map((int)sh, 1);
    if (addr == (uint64_t)-1) {
        sys_close((int)fd); sys_handle_close(rh); sys_handle_close(wh); sys_handle_close((int)sh);
        return 0;
    }

    char buf[32];
    int64_t n = sys_handle_read((int)fd, buf, sizeof(buf) - 1);
    int file_ok = (n >= 0);

    uint8_t sig = 0xAB;
    int pipe_ok = (sys_handle_write(wh, (const char*)&sig, 1) == 1);
    uint8_t got = 0;
    pipe_ok = pipe_ok && (sys_handle_read(rh, (char*)&got, 1) == 1) && got == 0xAB;

    *(volatile uint8_t*)addr = 0xCD;
    int shm_ok = (*(volatile uint8_t*)addr == 0xCD);

    sys_close((int)fd);
    sys_handle_close(rh);
    sys_handle_close(wh);
    sys_munmap(addr, 0x1000);
    sys_handle_close((int)sh);

    return file_ok && pipe_ok && shm_ok;
}

// Bad paths and out-of-range/unused handle numbers must fail cleanly
// (-1), never crash the process or silently succeed.
static int scenario_invalid_rejected(void) {
    if (sys_open("/this_path_does_not_exist_at_all_12345") >= 0) return 0;

    char buf[8];
    if (sys_handle_read(1000, buf, 4) >= 0) return 0;
    if (sys_handle_write(1000, buf, 4) >= 0) return 0;
    if (sys_handle_close(1000) >= 0) return 0;
    if (sys_handle_read(-1, buf, 4) >= 0) return 0;

    return 1;
}

// Cross-checks the newer handle-based directory enumeration
// (sys_open + sys_readdir_next, its own private cursor) against the
// older path+index form (sys_readdir) -- both must see the same number
// of entries in "/".
static int scenario_dir_enum(void) {
    int64_t dh = sys_open("/");
    if (dh < 0) return 0;

    int count_next = 0;
    char name[256];
    while (sys_readdir_next((int)dh, name) == 0) count_next++;
    sys_close((int)dh);

    int count_path = 0;
    while (sys_readdir("/", name, (uint32_t)count_path) == 0) count_path++;

    return count_next > 0 && count_next == count_path;
}

static int run_driver(void) {
    if (!scenario_inherit_and_refcount())          { put("vfs_test64: scenario1 FAILED\n"); return 1; }
    if (!scenario_simultaneous_independent_opens()) { put("vfs_test64: scenario2 FAILED\n"); return 2; }
    if (!scenario_mixed_handles())                  { put("vfs_test64: scenario3 FAILED\n"); return 3; }
    if (!scenario_invalid_rejected())               { put("vfs_test64: scenario4 FAILED\n"); return 4; }
    if (!scenario_dir_enum())                       { put("vfs_test64: scenario5 FAILED\n"); return 5; }
    sys_delete(TEST_FILE);
    return OK_EXIT;
}

void _start(void) {
    char args[16];
    sys_get_args(args, sizeof(args));

    int code;
    if (args[0] == 0) {
        code = run_driver();
    } else if (args[0] == 'i') {
        int h = args[1] - '0';
        code = run_inherited_reader(h) ? OK_EXIT : 1;
    } else if (args[0] == 's') {
        code = run_simultaneous_reader() ? OK_EXIT : 1;
    } else {
        code = 1;
    }

    sys_exit(code);
    for (;;) { } // unreachable
}
