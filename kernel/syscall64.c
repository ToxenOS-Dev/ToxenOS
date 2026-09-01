// kernel/syscall64.c — Milestone 5: minimal x86_64 syscall dispatcher.
// Reached from isr64_dispatch (kernel/interrupt64.c) for vector 128
// (int 0x80), exactly like the resumable #BP path -- same trapframe64_t,
// same POP_GPRS/iretq return to ring3 afterward.
//
// Milestone 7: process-aware. sys64_exit/sys64_getpid now go through
// kernel/process64.c's real process tracking instead of a hardcoded
// halt / hardcoded pid=1.
//
// Milestone 9: first userland file/process API (open/read/close/stat/
// spawn/wait), all pointer args going through kernel/usercopy64.c's
// validated copy helpers instead of trusting a raw user pointer.
//
// Milestone 24: sys64_spawn/sys64_wait are no longer synchronous --
// spawn creates a process and returns its pid immediately (the child
// runs concurrently, picked up by the scheduler), and wait genuinely
// blocks the caller (kernel/process64.c's process64_wait) until the
// target child exits, rather than retrieving an already-known result.
#include <stdint.h>
#include "../include/syscall64.h"
#include "../include/process64.h"
#include "../include/uservm64.h"
#include "../include/usercopy64.h"
#include "../include/txfs64.h"
#include "../include/keyboard_buffer64.h"
#include "../include/console64.h"
#include "../include/pipe64.h"
#include "../include/shm64.h"
#include "../include/klog.h"

#define SYS64_WRITE_MAX 256
#define SYS64_READ_MAX  1024
#define SYS64_PIPE_MAX  1024 // per-call cap for SYS64_HANDLE_READ/WRITE on a pipe, same spirit as SYS64_READ_MAX
#define SYS64_PATH_MAX  256
#define SYS64_ARGS_MAX  PROCESS64_ARGS_MAX

// Deliberately does NOT gate on process64_current() the way every other
// Milestone 9 syscall does: writing to the kernel log has no
// process-state dependency, and the still-independently-working
// Milestone 3B/5 RING3_TEST64_RUN stub calls this exact syscall without
// ever going through process64_spawn (no tracked process at all) -- that
// path must keep producing its original output untouched. When there
// IS a tracked process, the pointer is validated like every other
// syscall; the untracked-stub fallback keeps the old direct-copy
// behavior, which is safe there only because that stub is a known,
// trusted, hardcoded test binary, not arbitrary user input.
static uint64_t sys64_write(const char* buf, uint64_t len) {
    if (len > SYS64_WRITE_MAX) len = SYS64_WRITE_MAX;
    char tmp[SYS64_WRITE_MAX + 1];

    if (process64_current()) {
        if (copy_from_user64(tmp, (uint64_t)buf, len) < 0) return (uint64_t)-1;
    } else {
        for (uint64_t i = 0; i < len; i++) tmp[i] = buf[i];
    }

    tmp[len] = 0;
    klog(tmp);
    // Milestone 13: also mirror to the VGA console -- otherwise
    // userland output (shell64, shw, etc.) is only ever visible in the
    // serial log, never in the QEMU graphical window.
    console64_write(tmp, len);
    return len;
}

static void sys64_exit(int code) {
    process64_exit_current(code);  // never returns
}

static uint64_t sys64_getpid(void) {
    int pid = process64_current_pid();
    return (pid < 0) ? (uint64_t)-1 : (uint64_t)pid;
}

static uint64_t sys64_open(uint64_t path_ptr) {
    process64_t* cur = process64_current();
    if (!cur) return (uint64_t)-1;

    char path[SYS64_PATH_MAX];
    if (copy_user_cstr64(path, path_ptr, sizeof(path), 0) < 0) return (uint64_t)-1;

    int slot = -1;
    for (int i = 0; i < PROCESS64_MAX_FDS; i++) {
        if (cur->fds[i] < 0) { slot = i; break; }
    }
    if (slot < 0) return (uint64_t)-1;

    int fd = txfs64_open(path);
    if (fd < 0) return (uint64_t)-1;

    cur->fds[slot] = fd;
    return (uint64_t)slot;
}

static uint64_t sys64_read(int pfd, uint64_t buf_ptr, uint64_t len) {
    process64_t* cur = process64_current();
    if (!cur) return (uint64_t)-1;
    if (pfd < 0 || pfd >= PROCESS64_MAX_FDS || cur->fds[pfd] < 0) return (uint64_t)-1;

    if (len > SYS64_READ_MAX) len = SYS64_READ_MAX;
    uint8_t kbuf[SYS64_READ_MAX];
    int n = txfs64_read(cur->fds[pfd], kbuf, (uint32_t)len);
    if (n < 0) return (uint64_t)-1;
    if (n > 0 && copy_to_user64(buf_ptr, kbuf, (uint64_t)n) < 0) return (uint64_t)-1;
    return (uint64_t)n;
}

static uint64_t sys64_close(int pfd) {
    process64_t* cur = process64_current();
    if (!cur) return (uint64_t)-1;
    if (pfd < 0 || pfd >= PROCESS64_MAX_FDS || cur->fds[pfd] < 0) return (uint64_t)-1;

    txfs64_close(cur->fds[pfd]);
    cur->fds[pfd] = -1;
    return 0;
}

static uint64_t sys64_stat(uint64_t path_ptr, uint64_t size_out_ptr, uint64_t type_out_ptr) {
    if (!process64_current()) return (uint64_t)-1;

    char path[SYS64_PATH_MAX];
    if (copy_user_cstr64(path, path_ptr, sizeof(path), 0) < 0) return (uint64_t)-1;

    uint64_t size;
    int is_dir;
    if (txfs64_stat_type(path, &size, &is_dir) < 0) return (uint64_t)-1;
    if (copy_to_user64(size_out_ptr, &size, sizeof(size)) < 0) return (uint64_t)-1;
    if (type_out_ptr && copy_to_user64(type_out_ptr, &is_dir, sizeof(is_dir)) < 0) return (uint64_t)-1;
    return 0;
}

// Milestone 15: exposes the already-existing txfs64_readdir to
// userland (the new `ls` command) -- same validated-pointer shape as
// every other M9 file syscall. out_ptr must point at a user buffer of
// at least 256 bytes (txfs64_readdir's own contract).
static uint64_t sys64_readdir(uint64_t path_ptr, uint64_t out_ptr, uint32_t index) {
    if (!process64_current()) return (uint64_t)-1;

    char path[SYS64_PATH_MAX];
    if (copy_user_cstr64(path, path_ptr, sizeof(path), 0) < 0) return (uint64_t)-1;

    char name[256];
    if (txfs64_readdir(path, name, index) < 0) return (uint64_t)-1;

    uint64_t len = 0;
    while (name[len] && len < sizeof(name) - 1) len++;
    if (copy_to_user64(out_ptr, name, len + 1) < 0) return (uint64_t)-1;
    return 0;
}

// Milestone 24: no longer synchronous -- creates the child process (its
// own address space, its own kernel stack) and returns its pid as soon
// as loading succeeds. The child does not run during this call at all;
// it is left READY for the scheduler to pick up on a later tick.
static uint64_t sys64_spawn(uint64_t path_ptr, uint64_t args_ptr) {
    process64_t* parent = process64_current();
    if (!parent) return (uint64_t)-1;

    char path[SYS64_PATH_MAX];
    if (copy_user_cstr64(path, path_ptr, sizeof(path), 0) < 0) return (uint64_t)-1;

    char args[SYS64_ARGS_MAX];
    args[0] = 0;
    if (args_ptr && copy_user_cstr64(args, args_ptr, sizeof(args), 0) < 0) return (uint64_t)-1;

    uint32_t child_pid = 0;
    if (process64_spawn(path, args, parent->pid, &child_pid) < 0) return (uint64_t)-1;
    return (uint64_t)child_pid;
}

// Milestone 24: genuinely blocks the caller (kernel/process64.c's
// process64_wait, via the scheduler) until `pid` -- which must be one
// of this process's own children -- actually exits, then reaps it and
// returns its real exit code.
static uint64_t sys64_wait(uint32_t pid) {
    process64_t* cur = process64_current();
    if (!cur) return (uint64_t)-1;

    int code = process64_wait(pid);
    return (uint64_t)(int64_t)code;
}

// Milestone 10: non-blocking by design -- see keyboard_buffer64.h for
// why blocking belongs in userland (tox_readline), not here.
static uint64_t sys64_getch(void) {
    int c = keyboard_buffer64_getch();
    return (c < 0) ? (uint64_t)-1 : (uint64_t)c;
}

// Milestone 14: no process-state dependency (same reasoning as
// sys64_write) -- clearing the screen has nothing to do with which
// process is current, so this works even from the untracked
// RING3_TEST64_RUN stub.
static uint64_t sys64_clear(void) {
    console64_clear();
    return 0;
}

// Milestone 15: same no-process-state-dependency reasoning as
// sys64_clear -- setting the active color has nothing to do with which
// process is current.
static uint64_t sys64_setcolor(uint8_t color) {
    console64_set_color(color);
    return 0;
}

// Milestone 11: retrieves the single raw argument string sys_spawn
// stashed on this process at creation time (see process64_spawn's
// copy_str). Truncates to max_len if the caller's buffer is smaller
// than the stored args -- never overruns the user buffer either way,
// since copy_to_user64 itself validates it first.
static uint64_t sys64_get_args(uint64_t buf_ptr, uint64_t max_len) {
    process64_t* cur = process64_current();
    if (!cur) return (uint64_t)-1;
    if (max_len == 0) return (uint64_t)-1;

    uint64_t len = 0;
    while (cur->args[len] && len < (uint64_t)PROCESS64_ARGS_MAX - 1) len++;
    if (len > max_len - 1) len = max_len - 1;  // truncate to fit the caller's buffer

    // Truncating cur->args[] directly would cut it off mid-string without
    // a NUL where we clamped -- build the (possibly shorter) terminated
    // copy here instead, then copy that out.
    char tmp[PROCESS64_ARGS_MAX];
    for (uint64_t i = 0; i < len; i++) tmp[i] = cur->args[i];
    tmp[len] = 0;

    if (copy_to_user64(buf_ptr, tmp, len + 1) < 0) return (uint64_t)-1;
    return len;
}

// ── Milestone 19: protected-path check + write/create/delete syscalls ────
//
// These paths cannot be deleted, renamed, or overwritten by normal user
// commands. Anything matching the protected_exact list (exact match) or
// the protected_prefix list (path starts with the prefix + "/" or is the
// prefix itself) is refused with return value -2 so callers can print a
// distinct "path is protected" error instead of a generic failure.
//
// Note: exec64 loader messages ("exec64: NEX64 loaded"), process64 lifecycle
// messages ("process64: spawned..."), and timer heartbeats go through
// klog() which writes to the serial ring buffer ONLY -- never to vgaterm64.
// The VGA user-facing shell already stays clean without any additional gating
// on the kernel write path.

#define SYS64_PATH_PROTECTED ((uint64_t)-2)

static int txfs64_str_starts_with(const char* s, const char* pre) {
    int i = 0;
    while (pre[i] && s[i] == pre[i]) i++;
    return pre[i] == 0 && (s[i] == 0 || s[i] == '/');
}

static int txfs64_str_eq_k(const char* a, const char* b) {
    int i = 0;
    while (a[i] && b[i]) { if (a[i] != b[i]) return 0; i++; }
    return a[i] == b[i];
}

static int sys64_is_protected(const char* path) {
    static const char* exact[] = {
        "/",
        "/system_manager",
        "/system_manager/system_tools",
        "/system_manager/user",
        "/system_manager/user/profiles",
        "/system_manager/user/profiles/default",
        "/init64.nex64",
        "/shell64.nex64",
        0
    };
    static const char* prefix[] = {
        // Everything under Command Tools is protected (can't delete binaries)
        "/system_manager/system_tools/command_tools",
        0
    };
    for (int i = 0; exact[i]; i++)
        if (txfs64_str_eq_k(path, exact[i])) return 1;
    for (int i = 0; prefix[i]; i++)
        if (txfs64_str_starts_with(path, prefix[i])) return 1;
    return 0;
}

#define SYS64_WRITE_FILE_MAX 4096  // max content per write call this milestone

static uint64_t sys64_mkdir(uint64_t path_ptr) {
    if (!process64_current()) return (uint64_t)-1;
    char path[SYS64_PATH_MAX];
    if (copy_user_cstr64(path, path_ptr, sizeof(path), 0) < 0) return (uint64_t)-1;
    if (sys64_is_protected(path)) return SYS64_PATH_PROTECTED;
    return txfs64_mkdir(path) < 0 ? (uint64_t)-1 : 0;
}

static uint64_t sys64_mkfile(uint64_t path_ptr) {
    if (!process64_current()) return (uint64_t)-1;
    char path[SYS64_PATH_MAX];
    if (copy_user_cstr64(path, path_ptr, sizeof(path), 0) < 0) return (uint64_t)-1;
    if (sys64_is_protected(path)) return SYS64_PATH_PROTECTED;
    return txfs64_create_file(path) < 0 ? (uint64_t)-1 : 0;
}

static uint64_t sys64_write_file(uint64_t path_ptr, uint64_t data_ptr, uint64_t len) {
    if (!process64_current()) return (uint64_t)-1;
    char path[SYS64_PATH_MAX];
    if (copy_user_cstr64(path, path_ptr, sizeof(path), 0) < 0) return (uint64_t)-1;
    if (sys64_is_protected(path)) return SYS64_PATH_PROTECTED;
    if (len > SYS64_WRITE_FILE_MAX) return (uint64_t)-1;
    uint8_t data[SYS64_WRITE_FILE_MAX];
    if (len > 0 && copy_from_user64(data, data_ptr, len) < 0) return (uint64_t)-1;
    return txfs64_write_file(path, data, (uint32_t)len) < 0 ? (uint64_t)-1 : 0;
}

static uint64_t sys64_delete(uint64_t path_ptr) {
    if (!process64_current()) return (uint64_t)-1;
    char path[SYS64_PATH_MAX];
    if (copy_user_cstr64(path, path_ptr, sizeof(path), 0) < 0) return (uint64_t)-1;
    if (sys64_is_protected(path)) return SYS64_PATH_PROTECTED;
    int r = txfs64_unlink(path);
    if (r == 0) return 0;
    if (r == -2) {
        // target is a directory — remove it only if empty
        return txfs64_rmdir(path) == 0 ? 0 : (uint64_t)-3;
    }
    return (uint64_t)-1;
}

static uint64_t sys64_rmdir(uint64_t path_ptr) {
    if (!process64_current()) return (uint64_t)-1;
    char path[SYS64_PATH_MAX];
    if (copy_user_cstr64(path, path_ptr, sizeof(path), 0) < 0) return (uint64_t)-1;
    if (sys64_is_protected(path)) return SYS64_PATH_PROTECTED;
    return txfs64_rmdir(path) < 0 ? (uint64_t)-1 : 0;
}

static uint64_t sys64_rename(uint64_t src_ptr, uint64_t dest_ptr) {
    if (!process64_current()) return (uint64_t)-1;
    char src[SYS64_PATH_MAX], dest[SYS64_PATH_MAX];
    if (copy_user_cstr64(src,  src_ptr,  sizeof(src),  0) < 0) return (uint64_t)-1;
    if (copy_user_cstr64(dest, dest_ptr, sizeof(dest),  0) < 0) return (uint64_t)-1;
    if (sys64_is_protected(src) || sys64_is_protected(dest)) return SYS64_PATH_PROTECTED;
    int r = txfs64_rename(src, dest);
    if (r == -4) return (uint64_t)-4;
    return r < 0 ? (uint64_t)-1 : 0;
}

// Milestone 25: brk/mmap/munmap on top of kernel/uservm64.c. brk/mmap
// return raw userspace addresses (canonical high-half, bit 63 always
// set) -- (uint64_t)-1 is the ONLY failure value, distinct from any
// address this kernel ever hands out (see include/syscall64.h).
static uint64_t sys64_brk(uint64_t new_brk) {
    process64_t* cur = process64_current();
    if (!cur) return (uint64_t)-1;
    uint64_t actual;
    if (uservm64_brk(&cur->vm, &cur->as, new_brk, &actual) < 0) return (uint64_t)-1;
    return actual;
}

static uint64_t sys64_mmap(uint64_t size) {
    process64_t* cur = process64_current();
    if (!cur) return (uint64_t)-1;
    uint64_t addr;
    if (uservm64_mmap(&cur->vm, &cur->as, size, &addr) < 0) return (uint64_t)-1;
    return addr;
}

static uint64_t sys64_munmap(uint64_t addr, uint64_t size) {
    process64_t* cur = process64_current();
    if (!cur) return (uint64_t)-1;
    return uservm64_munmap(&cur->vm, &cur->as, addr, size) < 0 ? (uint64_t)-1 : 0;
}

// ── Milestone 26: IPC -- pipes and shared memory ────────────────────
// All of these go through the per-process handle table
// (include/handle64.h, process64_t::handles[]) -- never a raw
// pipe64_t*/shm64_t* or a global object ID crossing into userspace.

static int find_free_handle(process64_t* cur) {
    for (int i = 0; i < PROCESS64_MAX_HANDLES; i++) {
        if (cur->handles[i].kind == HANDLE64_UNUSED) return i;
    }
    return -1;
}

static uint64_t sys64_pipe_create(uint64_t read_out_ptr, uint64_t write_out_ptr) {
    process64_t* cur = process64_current();
    if (!cur) return (uint64_t)-1;

    int rslot = find_free_handle(cur);
    if (rslot < 0) return (uint64_t)-1;
    // Reserve rslot's kind immediately so the second find_free_handle
    // scan can't pick the SAME slot for the write end.
    cur->handles[rslot].kind = HANDLE64_PIPE_READ;
    int wslot = find_free_handle(cur);
    if (wslot < 0) { cur->handles[rslot].kind = HANDLE64_UNUSED; return (uint64_t)-1; }

    pipe64_t* p;
    if (pipe64_create(&p) < 0) {
        cur->handles[rslot].kind = HANDLE64_UNUSED;
        return (uint64_t)-1;
    }
    cur->handles[rslot].obj = p;
    cur->handles[wslot].kind = HANDLE64_PIPE_WRITE;
    cur->handles[wslot].obj = p;

    int r = rslot, w = wslot;
    if (copy_to_user64(read_out_ptr, &r, sizeof(r)) < 0 ||
        copy_to_user64(write_out_ptr, &w, sizeof(w)) < 0) {
        // Bad user pointer -- undo everything, leaking neither the
        // pipe object nor the two handle slots.
        pipe64_close_read(p);
        pipe64_close_write(p);
        cur->handles[rslot].kind = HANDLE64_UNUSED;
        cur->handles[wslot].kind = HANDLE64_UNUSED;
        return (uint64_t)-1;
    }
    return 0;
}

static uint64_t sys64_handle_read(int h, uint64_t buf_ptr, uint64_t len) {
    process64_t* cur = process64_current();
    if (!cur) return (uint64_t)-1;
    if (h < 0 || h >= PROCESS64_MAX_HANDLES || cur->handles[h].kind != HANDLE64_PIPE_READ) return (uint64_t)-1;

    if (len > SYS64_PIPE_MAX) len = SYS64_PIPE_MAX;
    uint8_t kbuf[SYS64_PIPE_MAX];
    int64_t n = pipe64_read((pipe64_t*)cur->handles[h].obj, kbuf, len);
    if (n < 0) return (uint64_t)-1;
    if (n > 0 && copy_to_user64(buf_ptr, kbuf, (uint64_t)n) < 0) return (uint64_t)-1;
    return (uint64_t)n;
}

static uint64_t sys64_handle_write(int h, uint64_t buf_ptr, uint64_t len) {
    process64_t* cur = process64_current();
    if (!cur) return (uint64_t)-1;
    if (h < 0 || h >= PROCESS64_MAX_HANDLES || cur->handles[h].kind != HANDLE64_PIPE_WRITE) return (uint64_t)-1;

    if (len > SYS64_PIPE_MAX) len = SYS64_PIPE_MAX;
    uint8_t kbuf[SYS64_PIPE_MAX];
    if (len > 0 && copy_from_user64(kbuf, buf_ptr, len) < 0) return (uint64_t)-1;
    int64_t n = pipe64_write((pipe64_t*)cur->handles[h].obj, kbuf, len);
    return (n < 0) ? (uint64_t)-1 : (uint64_t)n;
}

static uint64_t sys64_handle_close(int h) {
    process64_t* cur = process64_current();
    if (!cur) return (uint64_t)-1;
    if (h < 0 || h >= PROCESS64_MAX_HANDLES) return (uint64_t)-1;

    switch (cur->handles[h].kind) {
    case HANDLE64_PIPE_READ:  pipe64_close_read((pipe64_t*)cur->handles[h].obj); break;
    case HANDLE64_PIPE_WRITE: pipe64_close_write((pipe64_t*)cur->handles[h].obj); break;
    case HANDLE64_SHM:        shm64_release((shm64_t*)cur->handles[h].obj); break;
    default: return (uint64_t)-1;
    }
    cur->handles[h].kind = HANDLE64_UNUSED;
    cur->handles[h].obj = 0;
    return 0;
}

static uint64_t sys64_shm_create(uint64_t size) {
    process64_t* cur = process64_current();
    if (!cur) return (uint64_t)-1;

    int slot = find_free_handle(cur);
    if (slot < 0) return (uint64_t)-1;

    shm64_t* s;
    if (shm64_create(size, &s) < 0) return (uint64_t)-1;

    cur->handles[slot].kind = HANDLE64_SHM;
    cur->handles[slot].obj = s;
    return (uint64_t)slot;
}

static uint64_t sys64_shm_map(int h, int writable) {
    process64_t* cur = process64_current();
    if (!cur) return (uint64_t)-1;
    if (h < 0 || h >= PROCESS64_MAX_HANDLES || cur->handles[h].kind != HANDLE64_SHM) return (uint64_t)-1;

    uint64_t addr;
    if (uservm64_map_shm(&cur->vm, &cur->as, (shm64_t*)cur->handles[h].obj, writable, &addr) < 0) return (uint64_t)-1;
    return addr;
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
        tf->rax = sys64_stat(tf->rdi, tf->rsi, tf->rdx);
        break;
    case SYS64_SPAWN:
        tf->rax = sys64_spawn(tf->rdi, tf->rsi);
        break;
    case SYS64_WAIT:
        tf->rax = sys64_wait((uint32_t)tf->rdi);
        break;
    case SYS64_GETCH:
        tf->rax = sys64_getch();
        break;
    case SYS64_GET_ARGS:
        tf->rax = sys64_get_args(tf->rdi, tf->rsi);
        break;
    case SYS64_CLEAR:
        tf->rax = sys64_clear();
        break;
    case SYS64_SETCOLOR:
        tf->rax = sys64_setcolor((uint8_t)tf->rdi);
        break;
    case SYS64_READDIR:
        tf->rax = sys64_readdir(tf->rdi, tf->rsi, (uint32_t)tf->rdx);
        break;
    case SYS64_MKDIR:
        tf->rax = sys64_mkdir(tf->rdi);
        break;
    case SYS64_MKFILE:
        tf->rax = sys64_mkfile(tf->rdi);
        break;
    case SYS64_WRITE_FILE:
        tf->rax = sys64_write_file(tf->rdi, tf->rsi, tf->rdx);
        break;
    case SYS64_DELETE:
        tf->rax = sys64_delete(tf->rdi);
        break;
    case SYS64_RMDIR:
        tf->rax = sys64_rmdir(tf->rdi);
        break;
    case SYS64_RENAME:
        tf->rax = sys64_rename(tf->rdi, tf->rsi);
        break;
    case SYS64_BRK:
        tf->rax = sys64_brk(tf->rdi);
        break;
    case SYS64_MMAP:
        tf->rax = sys64_mmap(tf->rdi);
        break;
    case SYS64_MUNMAP:
        tf->rax = sys64_munmap(tf->rdi, tf->rsi);
        break;
    case SYS64_PIPE_CREATE:
        tf->rax = sys64_pipe_create(tf->rdi, tf->rsi);
        break;
    case SYS64_HANDLE_READ:
        tf->rax = sys64_handle_read((int)tf->rdi, tf->rsi, tf->rdx);
        break;
    case SYS64_HANDLE_WRITE:
        tf->rax = sys64_handle_write((int)tf->rdi, tf->rsi, tf->rdx);
        break;
    case SYS64_HANDLE_CLOSE:
        tf->rax = sys64_handle_close((int)tf->rdi);
        break;
    case SYS64_SHM_CREATE:
        tf->rax = sys64_shm_create(tf->rdi);
        break;
    case SYS64_SHM_MAP:
        tf->rax = sys64_shm_map((int)tf->rdi, (int)tf->rsi);
        break;
    default:
        tf->rax = (uint64_t)-1;
        break;
    }
}
