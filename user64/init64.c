// ToxenOS/user64/init64.c — Milestone 9: the first real userland
// program. Not the real init64 yet (no argument parsing, no real
// service startup) -- it's plumbing that proves the new syscall API
// (file read, spawn, wait) works end-to-end from a real user process:
// prints a banner, stats and reads /hello.ts through the new file
// syscalls, spawns /exec64_test.nex64, waits for it, and prints its
// exit code.
#include <stdint.h>
#include "tox64.h"

// Milestone 10: define to have init64 launch /shell64.nex64 instead of
// the Milestone 9 exec64_test smoke test below -- mutually exclusive for
// now. Default off so the already-verified Milestone 9 happy path stays
// the reproducible reference; flip this on for the Milestone 10
// "init64 launches a real interactive shell" verification run.
// #define INIT64_SHELL_RUN 1

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

#if defined(INIT64_SHELL_RUN)
    int64_t shell_pid = sys_spawn("/shell64.nex64", 0);
    if (shell_pid >= 0) {
        put_line_int("init64: spawned shell64, pid=", shell_pid);
        int64_t code = sys_wait((uint32_t)shell_pid);
        put_line_int("init64: shell64 exited, code=", code);
    } else {
        put("init64: sys_spawn(/shell64.nex64) failed\n");
    }
#else
    int64_t child_pid = sys_spawn("/exec64_test.nex64", 0);
    if (child_pid >= 0) {
        put_line_int("init64: spawned child pid=", child_pid);
        int64_t code = sys_wait((uint32_t)child_pid);
        put_line_int("init64: child exited, code=", code);
    } else {
        put("init64: sys_spawn(/exec64_test.nex64) failed\n");
    }
#endif

    put("init64: done\n");
    sys_exit(0);

    for (;;) { }  // unreachable
}
