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

#endif // TOX64_H
