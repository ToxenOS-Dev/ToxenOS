// ToxenOS/user64/init64.c — Milestone 9: the first real userland
// program. Not the real init64 yet (no argument parsing, no real
// service startup) -- originally plumbing that proved the new syscall
// API (file read, spawn, wait) works end-to-end from a real user
// process.
//
// Milestone 13: normal boot now means "launch the interactive shell
// immediately, with no diagnostic banner of its own" -- the kernel
// already cleared the VGA screen right before spawning init64
// (kernel/kernel64.c), so the first thing the user should see is
// shell64's own banner and prompt, not init64's old Milestone 9 debug
// spew. That original banner/file-read/exec64_test smoke test is kept
// as a debug alternative behind INIT64_TEST_MODE, not deleted.
#include <stdint.h>
#include "tox64.h"

// Define to run the original Milestone 9 diagnostic banner (pid, stat+
// read /hello.ts, spawn /exec64_test.nex64, wait, print exit code)
// instead of launching the shell -- useful for re-verifying the file/
// spawn/wait syscall path in isolation. Default off: normal boot always
// launches /shell64.nex64.
// Milestone 16: /exec64_test.nex64 is no longer on the disk image by
// default -- rebuild it with `make populate PACKAGE_DEBUG64=1` before
// re-enabling this flag, or the sys_spawn below will fail.
// #define INIT64_TEST_MODE 1

// Milestone 30: define to launch the graphical environment
// (/compositor64.nex64, which owns the Milestone 29 display/input
// handles and spawns its own demo clients) instead of the text shell.
// Default OFF, matching every other milestone's convention of gating
// new, higher-risk functionality behind an explicit flag rather than
// silently changing the default boot path every existing regression
// test relies on -- the text shell remains the unconditional default
// so a compositor bug never destabilizes the rest of the test suite.
// If the compositor process exits (normally or via a fault), init64
// falls back to the text shell rather than leaving the user stranded.
// #define INIT64_GRAPHICAL_MODE 1

static int my_strlen(const char* s) {
    int i = 0;
    while (s[i]) i++;
    return i;
}

static int itoa10(int v, char* out) {
    char tmp[12];
    int n = 0;
    if (v == 0) tmp[n++] = '0';
    while (v > 0) { tmp[n++] = (char)('0' + (v % 10)); v /= 10; }
    int j = 0;
    while (n > 0) out[j++] = tmp[--n];
    out[j] = 0;
    return j;
}

static void put(const char* s) {
    sys_write(s, (uint64_t)my_strlen(s));
}

// sys_write caps each call at SYS64_WRITE_MAX (256) bytes -- unrelated
// to the new file-read syscalls, that cap has been there since
// Milestone 5. Chunk a longer buffer (e.g. a whole file's contents)
// across multiple calls instead of silently showing only the first 256
// bytes of it.
__attribute__((unused))
static void put_buf(const char* buf, int len) {
    const int chunk = 200;
    int off = 0;
    while (off < len) {
        int n = len - off;
        if (n > chunk) n = chunk;
        sys_write(buf + off, (uint64_t)n);
        off += n;
    }
}

__attribute__((unused))
static void put_line_int(const char* prefix, int64_t v) {
    char buf[64];
    int n = 0;
    int neg = v < 0;
    if (neg) { buf[n++] = '-'; v = -v; }
    n += itoa10((int)v, buf + n);
    buf[n++] = '\n';
    buf[n] = 0;
    put(prefix);
    put(buf);
}

void _start(void) {
#if defined(INIT64_TEST_MODE)
    put("init64: starting, pid=");
    char pidbuf[24];
    int n = itoa10((int)sys_getpid(), pidbuf);
    pidbuf[n++] = '\n';
    pidbuf[n] = 0;
    put(pidbuf);

    uint64_t size = 0;
    if (sys_stat("/hello.ts", &size, 0) == 0) {
        put_line_int("init64: /hello.ts size = ", (int64_t)size);
    } else {
        put("init64: sys_stat(/hello.ts) failed\n");
    }

    int64_t fd = sys_open("/hello.ts");
    if (fd >= 0) {
        char buf[512];
        int64_t got = sys_read((int)fd, buf, sizeof(buf) - 1);
        if (got > 0) {
            buf[got] = 0;
            put("init64: /hello.ts contents:\n");
            put_buf(buf, (int)got);
            put("\n");
        }
        sys_close((int)fd);
    } else {
        put("init64: sys_open(/hello.ts) failed\n");
    }

    int64_t child_pid = sys_spawn("/exec64_test.nex64", 0);
    if (child_pid >= 0) {
        put_line_int("init64: spawned child pid=", child_pid);
        int64_t code = sys_wait((uint32_t)child_pid);
        put_line_int("init64: child exited, code=", code);
    } else {
        put("init64: sys_spawn(/exec64_test.nex64) failed\n");
    }
    put("init64: done\n");
#elif defined(INIT64_GRAPHICAL_MODE)
    // Milestone 32: compositor64, a small graphical demo roster, and
    // shell64 are now SIBLINGS -- all spawned directly by init64 via
    // sys_spawn_isolated (SYS64_SPAWN_EX with an empty inherit list, so
    // none of them can see any handle init64 itself might have open),
    // none of them the compositor's own child. Every graphical client's
    // wm_connect() (user64/wmclient64.h) performs a genuinely blocking
    // sys_service_connect internally, so it doesn't matter what order
    // these sys_spawn_isolated calls actually get scheduled in -- each
    // client connects the moment compositor64 publishes WM_SERVICE_NAME,
    // with zero delay loops and zero wasted CPU cycles either way. This
    // replaces Milestone 30's "compositor spawns its own demo clients,
    // init64 blocks on the compositor before even starting the shell"
    // arrangement.
    int64_t comp_pid = sys_spawn_isolated("/compositor64.nex64", 0);
    if (comp_pid < 0) put("init64: sys_spawn(/compositor64.nex64) failed\n");
    sys_spawn_isolated("/gfx_demo64.nex64", 0);
    sys_spawn_isolated("/gfx_interactive64.nex64", 0);
    // Milestone 33: ToxUI's own graphical acceptance demos, same
    // sibling/isolated arrangement as the M30 demos above -- each
    // connects to compositor64 independently via wm_connect().
    sys_spawn_isolated("/toxui_demo64.nex64", 0);
    sys_spawn_isolated("/wallpaper_demo64.nex64", 0);
    int64_t shell_pid = sys_spawn_isolated("/shell64.nex64", 0);
    if (shell_pid < 0) put("init64: sys_spawn(/shell64.nex64) failed\n");

    // init64 stays alive exactly as long as its main children do (same
    // discipline the plain default boot path below always used) --
    // waits on the compositor first (which normally runs for the whole
    // session), then the shell, so a compositor exit is still handled
    // gracefully (the shell -- already running the whole time now, not
    // "falling back" -- simply keeps going) without init64 exiting out
    // from under either of them.
    if (comp_pid >= 0) {
        int64_t code = sys_wait((uint32_t)comp_pid);
        put_line_int("init64: compositor64 exited, code=", code);
    }
    if (shell_pid >= 0) {
        sys_wait((uint32_t)shell_pid);
    }
#else
    // Milestone 13: normal boot -- no banner of our own, just hand off
    // straight to the shell.
    int64_t shell_pid = sys_spawn("/shell64.nex64", 0);
    if (shell_pid < 0) {
        put("init64: sys_spawn(/shell64.nex64) failed\n");
    } else {
        sys_wait((uint32_t)shell_pid);
    }
#endif

    sys_exit(0);

    for (;;) { }  // unreachable
}
