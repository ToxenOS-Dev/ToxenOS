// ToxenOS/user64/tox64.h — minimal 64-bit userspace syscall wrappers.
// The 64-bit analogue of user/tox.h, scoped to exactly what Milestones 5
// and 9 added. Constants here must stay in sync with include/syscall64.h.
#ifndef TOX64_H
#define TOX64_H
#include <stdint.h>

#define SYS64_WRITE  1
#define SYS64_EXIT   2
#define SYS64_GETPID 3
#define SYS64_OPEN   4
#define SYS64_READ   5
#define SYS64_CLOSE  6
#define SYS64_STAT   7
#define SYS64_SPAWN  8
#define SYS64_WAIT   9
#define SYS64_GETCH  10

// ABI: rax = syscall number, rdi/rsi/rdx = up to 3 args (see
// kernel/syscall64.c). Return value comes back in rax.
#define SYSCALL0(n) ({ \
    uint64_t _r; __asm__ volatile("int $0x80" : "=a"(_r) : "a"((uint64_t)(n))); _r; })

#define SYSCALL1(n, a) ({ \
    uint64_t _r; __asm__ volatile("int $0x80" : "=a"(_r) : "a"((uint64_t)(n)), "D"((uint64_t)(a))); _r; })

#define SYSCALL2(n, a, b) ({ \
    uint64_t _r; __asm__ volatile("int $0x80" : "=a"(_r) : "a"((uint64_t)(n)), "D"((uint64_t)(a)), "S"((uint64_t)(b))); _r; })

#define SYSCALL3(n, a, b, c) ({ \
    uint64_t _r; __asm__ volatile("int $0x80" : "=a"(_r) : "a"((uint64_t)(n)), "D"((uint64_t)(a)), "S"((uint64_t)(b)), "d"((uint64_t)(c))); _r; })

static inline uint64_t sys_write(const char* buf, uint64_t len) {
    return SYSCALL2(SYS64_WRITE, buf, len);
}

static inline void sys_exit(int code) {
    SYSCALL1(SYS64_EXIT, code);
    for (;;) { }  // unreachable -- sys64_exit halts forever
}

static inline uint64_t sys_getpid(void) {
    return SYSCALL0(SYS64_GETPID);
}

// Milestone 9: first userland file/process API. All return -1 on
// failure (bad pointer, bad fd, file not found, etc.).
static inline int64_t sys_open(const char* path) {
    return (int64_t)SYSCALL1(SYS64_OPEN, path);
}

static inline int64_t sys_read(int fd, char* buf, uint64_t len) {
    return (int64_t)SYSCALL3(SYS64_READ, fd, buf, len);
}

static inline int64_t sys_close(int fd) {
    return (int64_t)SYSCALL1(SYS64_CLOSE, fd);
}

static inline int64_t sys_stat(const char* path, uint64_t* size_out) {
    return (int64_t)SYSCALL2(SYS64_STAT, path, size_out);
}

// Synchronous -- by the time this returns, the child has already run to
// completion (see kernel/syscall64.c's sys64_spawn). Returns the child's
// pid on success, -1 on failure (wrong arch, missing file, etc.).
static inline int64_t sys_spawn(const char* path) {
    return (int64_t)SYSCALL1(SYS64_SPAWN, path);
}

// Retrieves the cached exit code of the most recently completed child
// with this pid -- never actually blocks (there is nothing to block on
// by the time spawn has already returned). Returns -1 if pid doesn't
// match the last completed child.
static inline int64_t sys_wait(uint32_t pid) {
    return (int64_t)SYSCALL1(SYS64_WAIT, pid);
}

// Milestone 10: minimal stdin path. Non-blocking -- returns -1 if no key
// is currently buffered (kernel/keyboard_buffer64.c, filled by IRQ1).
static inline int64_t sys_getch(void) {
    return (int64_t)SYSCALL0(SYS64_GETCH);
}

// Composed userland helper, not a 1:1 syscall wrapper -- hence "tox_"
// instead of "sys_", to keep that distinction visible at call sites.
// Blocks by polling sys_getch (safe: ring3 always resumes with IF=1
// after int 0x80's iretq, so IRQ1 keeps filling the kernel-side buffer
// between polls even though this spins). Echoes each accepted character
// back via sys_write, and handles Enter/Backspace itself:
//   - Enter ('\n'/'\r') ends the line.
//   - Backspace (8 or 127/DEL) erases the previous character; a no-op
//     on an empty line instead of underflowing.
//   - Any other control character (Tab, Esc, ...) is ignored -- no line
//     editing beyond Backspace yet.
//   - Once `max - 1` characters have been accepted, further characters
//     are silently dropped (not written past the buffer) until
//     Enter/Backspace.
// Always NUL-terminates buf and returns the number of characters read
// (not counting the NUL).
static inline int tox_readline(char* buf, int max) {
    int n = 0;
    for (;;) {
        int64_t ci;
        do { ci = sys_getch(); } while (ci < 0);
        char c = (char)ci;

        if (c == '\n' || c == '\r') {
            sys_write("\n", 1);
            break;
        }
        if (c == 8 || c == 127) {
            if (n > 0) {
                n--;
                sys_write("\b \b", 3);
            }
            continue;
        }
        if (c < 32) continue;

        if (n < max - 1) {
            buf[n++] = c;
            sys_write(&c, 1);
        }
    }
    buf[n] = 0;
    return n;
}

#endif // TOX64_H
