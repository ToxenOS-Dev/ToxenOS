// kernel/syscall64.c — Milestone 5: minimal x86_64 syscall dispatcher.
// Reached from isr64_dispatch (kernel/interrupt64.c) for vector 128
// (int 0x80), exactly like the resumable #BP path -- same trapframe64_t,
// same POP_GPRS/iretq return to ring3 afterward.
//
// Milestone 7: process-aware. sys64_exit/sys64_getpid now go through
// kernel/userproc64.c's real process tracking instead of a hardcoded
// halt / hardcoded pid=1.
//
// Milestone 9: first userland file/process API (open/read/close/stat/
// spawn/wait), all pointer args going through kernel/usercopy64.c's
// validated copy helpers instead of trusting a raw user pointer.
#include <stdint.h>
#include "../include/syscall64.h"
#include "../include/userproc64.h"
#include "../include/usercopy64.h"
#include "../include/txfs64.h"
#include "../include/keyboard_buffer64.h"
#include "../include/klog.h"

#define SYS64_WRITE_MAX 256
#define SYS64_READ_MAX  1024
#define SYS64_PATH_MAX  256

// Deliberately does NOT gate on userproc64_current() the way every other
// Milestone 9 syscall does: writing to the kernel log has no
// process-state dependency, and the still-independently-working
// Milestone 3B/5 RING3_TEST64_RUN stub calls this exact syscall without
// ever going through userproc64_run (no tracked process at all) -- that
// path must keep producing its original output untouched. When there
// IS a tracked process, the pointer is validated like every other
// syscall; the untracked-stub fallback keeps the old direct-copy
// behavior, which is safe there only because that stub is a known,
// trusted, hardcoded test binary, not arbitrary user input.
static uint64_t sys64_write(const char* buf, uint64_t len) {
    if (len > SYS64_WRITE_MAX) len = SYS64_WRITE_MAX;
    char tmp[SYS64_WRITE_MAX + 1];

    if (userproc64_current()) {
        if (copy_from_user64(tmp, (uint64_t)buf, len) < 0) return (uint64_t)-1;
    } else {
        for (uint64_t i = 0; i < len; i++) tmp[i] = buf[i];
    }

    tmp[len] = 0;
    klog(tmp);
    return len;
}

static void sys64_exit(int code) {
    userproc64_exit_current(code);  // never returns
}

static uint64_t sys64_getpid(void) {
    int pid = userproc64_current_pid();
    return (pid < 0) ? (uint64_t)-1 : (uint64_t)pid;
}

static uint64_t sys64_open(uint64_t path_ptr) {
    userproc64_t* cur = userproc64_current();
    if (!cur) return (uint64_t)-1;

    char path[SYS64_PATH_MAX];
    if (copy_user_cstr64(path, path_ptr, sizeof(path), 0) < 0) return (uint64_t)-1;

    int slot = -1;
    for (int i = 0; i < USERPROC64_MAX_FDS; i++) {
        if (cur->fds[i] < 0) { slot = i; break; }
    }
    if (slot < 0) return (uint64_t)-1;

    int fd = txfs64_open(path);
    if (fd < 0) return (uint64_t)-1;

    cur->fds[slot] = fd;
    return (uint64_t)slot;
}

static uint64_t sys64_read(int pfd, uint64_t buf_ptr, uint64_t len) {
    userproc64_t* cur = userproc64_current();
    if (!cur) return (uint64_t)-1;
    if (pfd < 0 || pfd >= USERPROC64_MAX_FDS || cur->fds[pfd] < 0) return (uint64_t)-1;

    if (len > SYS64_READ_MAX) len = SYS64_READ_MAX;
    uint8_t kbuf[SYS64_READ_MAX];
    int n = txfs64_read(cur->fds[pfd], kbuf, (uint32_t)len);
    if (n < 0) return (uint64_t)-1;
    if (n > 0 && copy_to_user64(buf_ptr, kbuf, (uint64_t)n) < 0) return (uint64_t)-1;
    return (uint64_t)n;
}

static uint64_t sys64_close(int pfd) {
    userproc64_t* cur = userproc64_current();
    if (!cur) return (uint64_t)-1;
    if (pfd < 0 || pfd >= USERPROC64_MAX_FDS || cur->fds[pfd] < 0) return (uint64_t)-1;

    txfs64_close(cur->fds[pfd]);
    cur->fds[pfd] = -1;
    return 0;
}

static uint64_t sys64_stat(uint64_t path_ptr, uint64_t size_out_ptr) {
    if (!userproc64_current()) return (uint64_t)-1;

    char path[SYS64_PATH_MAX];
    if (copy_user_cstr64(path, path_ptr, sizeof(path), 0) < 0) return (uint64_t)-1;

    uint64_t size;
    if (txfs64_stat(path, &size) < 0) return (uint64_t)-1;
    if (copy_to_user64(size_out_ptr, &size, sizeof(size)) < 0) return (uint64_t)-1;
    return 0;
}

// Synchronous: by the time this returns, the child has already run to
// completion (kernel/userproc64.c's userproc64_run is fully blocking --
// no concurrent scheduling of multiple user processes exists yet). The
// exit code is therefore already known and stashed on the PARENT for
// sys64_wait to retrieve, rather than anything actually blocking.
static uint64_t sys64_spawn(uint64_t path_ptr) {
    userproc64_t* parent = userproc64_current();
    if (!parent) return (uint64_t)-1;

    char path[SYS64_PATH_MAX];
    if (copy_user_cstr64(path, path_ptr, sizeof(path), 0) < 0) return (uint64_t)-1;

    uint32_t child_pid = 0;
    int exit_code = userproc64_run(path, &child_pid);
    if (child_pid == 0) return (uint64_t)-1;  // load failed -- no process created

    parent->last_child_pid      = child_pid;
    parent->last_child_exit_code = exit_code;
    parent->has_child_result     = 1;
    return (uint64_t)child_pid;
}

static uint64_t sys64_wait(uint32_t pid) {
    userproc64_t* cur = userproc64_current();
    if (!cur) return (uint64_t)-1;
    if (!cur->has_child_result || cur->last_child_pid != pid) return (uint64_t)-1;

    cur->has_child_result = 0;
    return (uint64_t)(int64_t)cur->last_child_exit_code;
}

// Milestone 10: non-blocking by design -- see keyboard_buffer64.h for
// why blocking belongs in userland (tox_readline), not here.
static uint64_t sys64_getch(void) {
    int c = keyboard_buffer64_getch();
    return (c < 0) ? (uint64_t)-1 : (uint64_t)c;
}

void syscall64_dispatch(trapframe64_t* tf) {
    switch (tf->rax) {
    case SYS64_WRITE:
        tf->rax = sys64_write((const char*)tf->rdi, tf->rsi);
        break;
    case SYS64_EXIT:
        sys64_exit((int)tf->rdi);  // never returns
        break;
    case SYS64_GETPID:
        tf->rax = sys64_getpid();
        break;
    case SYS64_OPEN:
        tf->rax = sys64_open(tf->rdi);
        break;
    case SYS64_READ:
        tf->rax = sys64_read((int)tf->rdi, tf->rsi, tf->rdx);
        break;
    case SYS64_CLOSE:
        tf->rax = sys64_close((int)tf->rdi);
        break;
    case SYS64_STAT:
        tf->rax = sys64_stat(tf->rdi, tf->rsi);
        break;
    case SYS64_SPAWN:
        tf->rax = sys64_spawn(tf->rdi);
        break;
    case SYS64_WAIT:
        tf->rax = sys64_wait((uint32_t)tf->rdi);
        break;
    case SYS64_GETCH:
        tf->rax = sys64_getch();
        break;
    default:
        tf->rax = (uint64_t)-1;
        break;
    }
}
