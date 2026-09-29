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
#include "../include/klog.h"
#include "../include/syscall64.h"
#include "../include/process64.h"
#include "../include/uservm64.h"
#include "../include/usercopy64.h"
#include "../include/vfs64.h"
#include "../include/keyboard_buffer64.h"
#include "../include/console64.h"
#include "../include/pipe64.h"
#include "../include/shm64.h"
#include "../include/gpu64.h"
#include "../include/input64.h"
#include "../include/display64.h"
#include "../include/paging64.h"
#include "../include/heap64.h"
#include "../include/service64.h"
#include "../include/klog.h"
#include "../include/tsc64.h"
#include "../include/virtio_gpu64.h"
#include "../include/timer64.h"

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

// Milestone 27: shared by every syscall that allocates a new handle
// (open a file/directory, create a pipe, create shared memory) -- see
// include/handle64.h. Defined here (before its first use) rather than
// down by the IPC syscalls, now that file opens need it too.
static int find_free_handle(process64_t* cur) {
    for (int i = 0; i < PROCESS64_MAX_HANDLES; i++) {
        if (cur->handles[i].kind == HANDLE64_UNUSED) return i;
    }
    return -1;
}

// Milestone 27: opens `path` through the VFS (kernel/vfs64.c), works
// for both regular files (HANDLE64_FILE) and directories (HANDLE64_DIR)
// -- the returned handle is a small per-process integer in the SAME
// table pipes and shared memory use, not a separate fd namespace.
// Reading/writing/closing it goes through SYS64_HANDLE_READ/WRITE/
// CLOSE like any other handle; enumerating a directory handle's
// entries goes through SYS64_READDIR_NEXT.
static uint64_t sys64_open(uint64_t path_ptr) {
    process64_t* cur = process64_current();
    if (!cur) return (uint64_t)-1;

    char path[SYS64_PATH_MAX];
    if (copy_user_cstr64(path, path_ptr, sizeof(path), 0) < 0) return (uint64_t)-1;

    int slot = find_free_handle(cur);
    if (slot < 0) return (uint64_t)-1;

    vfs64_file_t* f;
    if (vfs64_open(path, &f) < 0) return (uint64_t)-1;

    cur->handles[slot].kind = f->node.is_dir ? HANDLE64_DIR : HANDLE64_FILE;
    cur->handles[slot].obj  = f;
    return (uint64_t)slot;
}

static uint64_t sys64_stat(uint64_t path_ptr, uint64_t size_out_ptr, uint64_t type_out_ptr) {
    if (!process64_current()) return (uint64_t)-1;

    char path[SYS64_PATH_MAX];
    if (copy_user_cstr64(path, path_ptr, sizeof(path), 0) < 0) return (uint64_t)-1;

    vfs64_node_t node;
    if (vfs64_lookup(path, &node) < 0) return (uint64_t)-1;
    if (copy_to_user64(size_out_ptr, &node.size, sizeof(node.size)) < 0) return (uint64_t)-1;
    if (type_out_ptr && copy_to_user64(type_out_ptr, &node.is_dir, sizeof(node.is_dir)) < 0) return (uint64_t)-1;
    return 0;
}

// Milestone 15: path+index directory enumeration (the `ls` command) --
// stateless (re-resolves and re-scans from the start every call), so
// concurrent callers or repeated calls on the same path never
// interfere with each other. Milestone 27: goes through
// vfs64_readdir_path instead of a txfs64 call directly. out_ptr must
// point at a user buffer of at least 256 bytes. See SYS64_READDIR_NEXT
// for the newer handle-based alternative (an open directory with its
// own private enumeration cursor).
static uint64_t sys64_readdir(uint64_t path_ptr, uint64_t out_ptr, uint32_t index) {
    if (!process64_current()) return (uint64_t)-1;

    char path[SYS64_PATH_MAX];
    if (copy_user_cstr64(path, path_ptr, sizeof(path), 0) < 0) return (uint64_t)-1;

    char name[VFS64_NAME_MAX];
    if (vfs64_readdir_path(path, index, name) < 0) return (uint64_t)-1;

    uint64_t len = 0;
    while (name[len] && len < sizeof(name) - 1) len++;
    if (copy_to_user64(out_ptr, name, len + 1) < 0) return (uint64_t)-1;
    return 0;
}

// Milestone 27: reads the NEXT entry of an open directory handle,
// advancing its own private cursor (vfs64_file_t::cursor) -- two
// separate opens of the SAME directory, or two different processes
// each iterating their own handle, never interfere with each other.
// Returns 0 with `name_out` (>=256 bytes) filled, or -1 once past the
// last entry or `h` isn't an open directory.
static uint64_t sys64_readdir_next(int h, uint64_t name_out_ptr) {
    process64_t* cur = process64_current();
    if (!cur) return (uint64_t)-1;
    if (h < 0 || h >= PROCESS64_MAX_HANDLES || cur->handles[h].kind != HANDLE64_DIR) return (uint64_t)-1;

    vfs64_file_t* f = (vfs64_file_t*)cur->handles[h].obj;
    char name[VFS64_NAME_MAX];
    if (vfs64_readdir(&f->node, (uint32_t)f->cursor, name) < 0) return (uint64_t)-1;
    f->cursor++;

    uint64_t len = 0;
    while (name[len] && len < sizeof(name) - 1) len++;
    if (copy_to_user64(name_out_ptr, name, len + 1) < 0) return (uint64_t)-1;
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

static int sys64_str_starts_with(const char* s, const char* pre) {
    int i = 0;
    while (pre[i] && s[i] == pre[i]) i++;
    return pre[i] == 0 && (s[i] == 0 || s[i] == '/');
}

static int sys64_str_eq_k(const char* a, const char* b) {
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
        if (sys64_str_eq_k(path, exact[i])) return 1;
    for (int i = 0; prefix[i]; i++)
        if (sys64_str_starts_with(path, prefix[i])) return 1;
    return 0;
}

#define SYS64_WRITE_FILE_MAX 4096  // max content per write call this milestone

static uint64_t sys64_mkdir(uint64_t path_ptr) {
    if (!process64_current()) return (uint64_t)-1;
    char path[SYS64_PATH_MAX];
    if (copy_user_cstr64(path, path_ptr, sizeof(path), 0) < 0) return (uint64_t)-1;
    if (sys64_is_protected(path)) return SYS64_PATH_PROTECTED;
    return vfs64_mkdir(path) < 0 ? (uint64_t)-1 : 0;
}

static uint64_t sys64_mkfile(uint64_t path_ptr) {
    if (!process64_current()) return (uint64_t)-1;
    char path[SYS64_PATH_MAX];
    if (copy_user_cstr64(path, path_ptr, sizeof(path), 0) < 0) return (uint64_t)-1;
    if (sys64_is_protected(path)) return SYS64_PATH_PROTECTED;
    return vfs64_create_file(path) < 0 ? (uint64_t)-1 : 0;
}

static uint64_t sys64_write_file(uint64_t path_ptr, uint64_t data_ptr, uint64_t len) {
    if (!process64_current()) return (uint64_t)-1;
    char path[SYS64_PATH_MAX];
    if (copy_user_cstr64(path, path_ptr, sizeof(path), 0) < 0) return (uint64_t)-1;
    if (sys64_is_protected(path)) return SYS64_PATH_PROTECTED;
    if (len > SYS64_WRITE_FILE_MAX) return (uint64_t)-1;
    uint8_t data[SYS64_WRITE_FILE_MAX];
    if (len > 0 && copy_from_user64(data, data_ptr, len) < 0) return (uint64_t)-1;
    return vfs64_write_file(path, data, (uint32_t)len) < 0 ? (uint64_t)-1 : 0;
}

static uint64_t sys64_delete(uint64_t path_ptr) {
    if (!process64_current()) return (uint64_t)-1;
    char path[SYS64_PATH_MAX];
    if (copy_user_cstr64(path, path_ptr, sizeof(path), 0) < 0) return (uint64_t)-1;
    if (sys64_is_protected(path)) return SYS64_PATH_PROTECTED;
    int r = vfs64_unlink(path);
    if (r == 0) return 0;
    if (r == -2) {
        // target is a directory — remove it only if empty
        return vfs64_rmdir(path) == 0 ? 0 : (uint64_t)-3;
    }
    return (uint64_t)-1;
}

static uint64_t sys64_rmdir(uint64_t path_ptr) {
    if (!process64_current()) return (uint64_t)-1;
    char path[SYS64_PATH_MAX];
    if (copy_user_cstr64(path, path_ptr, sizeof(path), 0) < 0) return (uint64_t)-1;
    if (sys64_is_protected(path)) return SYS64_PATH_PROTECTED;
    return vfs64_rmdir(path) < 0 ? (uint64_t)-1 : 0;
}

static uint64_t sys64_rename(uint64_t src_ptr, uint64_t dest_ptr) {
    if (!process64_current()) return (uint64_t)-1;
    char src[SYS64_PATH_MAX], dest[SYS64_PATH_MAX];
    if (copy_user_cstr64(src,  src_ptr,  sizeof(src),  0) < 0) return (uint64_t)-1;
    if (copy_user_cstr64(dest, dest_ptr, sizeof(dest),  0) < 0) return (uint64_t)-1;
    if (sys64_is_protected(src) || sys64_is_protected(dest)) return SYS64_PATH_PROTECTED;
    int r = vfs64_rename(src, dest);
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

// Milestone 27: the generic read/write path -- a pipe end and an open
// file are both just "a handle you can read/write bytes through",
// dispatched here by kind rather than exposing two separate ABIs. A
// HANDLE64_FILE read/write is POSITIONED at the open-file object's own
// cursor (vfs64_file_t::cursor), which this call advances by however
// many bytes actually transferred -- growth (if writing past the
// current end of file) is handled by kernel/vfs64.c/kernel/txfs64.c
// underneath, transparently to the caller.
static uint64_t sys64_handle_read(int h, uint64_t buf_ptr, uint64_t len) {
    process64_t* cur = process64_current();
    if (!cur) return (uint64_t)-1;
    if (h < 0 || h >= PROCESS64_MAX_HANDLES) return (uint64_t)-1;

    if (cur->handles[h].kind == HANDLE64_PIPE_READ) {
        if (len > SYS64_PIPE_MAX) len = SYS64_PIPE_MAX;
        uint8_t kbuf[SYS64_PIPE_MAX];
        int64_t n = pipe64_read((pipe64_t*)cur->handles[h].obj, kbuf, len);
        if (n < 0) return (uint64_t)-1;
        if (n > 0 && copy_to_user64(buf_ptr, kbuf, (uint64_t)n) < 0) return (uint64_t)-1;
        return (uint64_t)n;
    }
    if (cur->handles[h].kind == HANDLE64_FILE) {
        vfs64_file_t* f = (vfs64_file_t*)cur->handles[h].obj;
        if (len > SYS64_READ_MAX) len = SYS64_READ_MAX;
        uint8_t kbuf[SYS64_READ_MAX];
        int n = vfs64_read(&f->node, f->cursor, kbuf, (uint32_t)len);
        if (n < 0) return (uint64_t)-1;
        if (n > 0 && copy_to_user64(buf_ptr, kbuf, (uint64_t)n) < 0) return (uint64_t)-1;
        f->cursor += (uint64_t)n;
        return (uint64_t)n;
    }
    // Milestone 29: a HANDLE64_INPUT read blocks (real scheduler block,
    // never polling -- see input64_read_blocking) for exactly ONE
    // structured event, then copies it out. `len` must be at least one
    // event's worth; this deliberately doesn't batch multiple events
    // per call.
    if (cur->handles[h].kind == HANDLE64_INPUT) {
        if (len < sizeof(input64_event_t)) return (uint64_t)-1;
        input64_event_t ev;
        if (input64_read_blocking(&ev) < 0) return (uint64_t)-1;
        if (copy_to_user64(buf_ptr, &ev, sizeof(ev)) < 0) return (uint64_t)-1;
        return (uint64_t)sizeof(ev);
    }
    return (uint64_t)-1;
}

// Milestone 30: non-blocking counterpart, used by kernel/../user64's
// compositor to multiplex its input handle against one pipe per
// connected client (see include/syscall64.h's header comment on
// SYS64_HANDLE_TRY_READ). HANDLE64_FILE reads never block regardless,
// so that case just delegates to the normal path.
static uint64_t sys64_handle_try_read(int h, uint64_t buf_ptr, uint64_t len) {
    process64_t* cur = process64_current();
    if (!cur) return (uint64_t)-1;
    if (h < 0 || h >= PROCESS64_MAX_HANDLES) return (uint64_t)-1;

    if (cur->handles[h].kind == HANDLE64_PIPE_READ) {
        if (len > SYS64_PIPE_MAX) len = SYS64_PIPE_MAX;
        uint8_t kbuf[SYS64_PIPE_MAX];
        int64_t n = pipe64_try_read((pipe64_t*)cur->handles[h].obj, kbuf, len);
        if (n == -2) return SYS64_ERR_WOULDBLOCK;
        if (n < 0) return (uint64_t)-1;
        if (n > 0 && copy_to_user64(buf_ptr, kbuf, (uint64_t)n) < 0) return (uint64_t)-1;
        return (uint64_t)n;
    }
    if (cur->handles[h].kind == HANDLE64_INPUT) {
        if (len < sizeof(input64_event_t)) return (uint64_t)-1;
        input64_event_t ev;
        int r = input64_try_read(&ev);
        if (r == -2) return SYS64_ERR_WOULDBLOCK;
        if (r < 0) return (uint64_t)-1;
        if (copy_to_user64(buf_ptr, &ev, sizeof(ev)) < 0) return (uint64_t)-1;
        return (uint64_t)sizeof(ev);
    }
    if (cur->handles[h].kind == HANDLE64_FILE) {
        return sys64_handle_read(h, buf_ptr, len);
    }
    return (uint64_t)-1;
}

static uint64_t sys64_handle_write(int h, uint64_t buf_ptr, uint64_t len) {
    process64_t* cur = process64_current();
    if (!cur) return (uint64_t)-1;
    if (h < 0 || h >= PROCESS64_MAX_HANDLES) return (uint64_t)-1;

    if (cur->handles[h].kind == HANDLE64_PIPE_WRITE) {
        if (len > SYS64_PIPE_MAX) len = SYS64_PIPE_MAX;
        uint8_t kbuf[SYS64_PIPE_MAX];
        if (len > 0 && copy_from_user64(kbuf, buf_ptr, len) < 0) return (uint64_t)-1;
        int64_t n = pipe64_write((pipe64_t*)cur->handles[h].obj, kbuf, len);
        return (n < 0) ? (uint64_t)-1 : (uint64_t)n;
    }
    if (cur->handles[h].kind == HANDLE64_FILE) {
        vfs64_file_t* f = (vfs64_file_t*)cur->handles[h].obj;
        if (len > SYS64_READ_MAX) len = SYS64_READ_MAX;
        uint8_t kbuf[SYS64_READ_MAX];
        if (len > 0 && copy_from_user64(kbuf, buf_ptr, len) < 0) return (uint64_t)-1;
        int n = vfs64_write(&f->node, f->cursor, kbuf, (uint32_t)len);
        if (n < 0) return (uint64_t)-1;
        f->cursor += (uint64_t)n;
        return (uint64_t)n;
    }
    return (uint64_t)-1;
}

// Milestone 32.1: see include/syscall64.h's header comment on
// SYS64_HANDLE_TRY_WRITE. Mirrors sys64_handle_try_read's structure.
static uint64_t sys64_handle_try_write(int h, uint64_t buf_ptr, uint64_t len) {
    process64_t* cur = process64_current();
    if (!cur) return (uint64_t)-1;
    if (h < 0 || h >= PROCESS64_MAX_HANDLES) return (uint64_t)-1;

    if (cur->handles[h].kind == HANDLE64_PIPE_WRITE) {
        if (len > SYS64_PIPE_MAX) len = SYS64_PIPE_MAX;
        uint8_t kbuf[SYS64_PIPE_MAX];
        if (len > 0 && copy_from_user64(kbuf, buf_ptr, len) < 0) return (uint64_t)-1;
        int64_t n = pipe64_try_write((pipe64_t*)cur->handles[h].obj, kbuf, len);
        if (n == -2) return SYS64_ERR_WOULDBLOCK;
        return (n < 0) ? (uint64_t)-1 : (uint64_t)n;
    }
    if (cur->handles[h].kind == HANDLE64_FILE) {
        return sys64_handle_write(h, buf_ptr, len);
    }
    return (uint64_t)-1;
}

static uint64_t sys64_handle_close(int h) {
    process64_t* cur = process64_current();
    if (!cur) return (uint64_t)-1;
    if (h < 0 || h >= PROCESS64_MAX_HANDLES) return (uint64_t)-1;

    switch (cur->handles[h].kind) {
    case HANDLE64_PIPE_READ:  pipe64_close_read((pipe64_t*)cur->handles[h].obj); break;
    case HANDLE64_PIPE_WRITE: pipe64_close_write((pipe64_t*)cur->handles[h].obj); break;
    case HANDLE64_SHM:        shm64_release((shm64_t*)cur->handles[h].obj); break;
    case HANDLE64_GPU_BUFFER: gpu64_buffer_release((gpu64_buffer_t*)cur->handles[h].obj); break;
    case HANDLE64_FILE:
    case HANDLE64_DIR:        vfs64_file_release((vfs64_file_t*)cur->handles[h].obj); break;
    case HANDLE64_INPUT:      input64_release(); break;
    case HANDLE64_DISPLAY:    display64_release(); break;
    case HANDLE64_SERVICE_LISTEN: service64_unpublish((service64_t*)cur->handles[h].obj); break;
    default: return (uint64_t)-1;
    }
    cur->handles[h].kind = HANDLE64_UNUSED;
    cur->handles[h].obj = 0;
    return 0;
}

// ── M+12B: SYS64_HANDLE_WAIT_ANY ─────────────────────────────────────
// See include/syscall64.h's own header comment for the full contract.
// Supported handle kinds are exactly the four that already have a
// wait-channel concept -- resolve_wait_any_chan/wait_any_handle_ready
// dispatch on handle KIND the same way sys64_handle_try_read/
// sys64_handle_close above already do; any other kind is simply
// unsupported here (returns -1 from resolve_wait_any_chan, which fails
// the whole call before anything blocks).
static int resolve_wait_any_chan(process64_t* cur, int h, void** chan_out) {
    if (h < 0 || h >= PROCESS64_MAX_HANDLES) return -1;
    switch (cur->handles[h].kind) {
    case HANDLE64_PIPE_READ:      *chan_out = &((pipe64_t*)cur->handles[h].obj)->read_chan;  return 0;
    case HANDLE64_PIPE_WRITE:     *chan_out = &((pipe64_t*)cur->handles[h].obj)->write_chan; return 0;
    case HANDLE64_INPUT:          *chan_out = input64_wait_chan(); return 0;
    case HANDLE64_SERVICE_LISTEN: *chan_out = service64_wait_chan((service64_t*)cur->handles[h].obj); return 0;
    default: return -1; // SHM/FILE/DIR/DISPLAY/GPU_BUFFER/UNUSED -- no wait-channel concept, unsupported
    }
}

// Side-effect-free -- see pipe64_read_ready/pipe64_write_ready/
// input64_ready/service64_has_pending's own header comments. `h` must
// already have been validated by resolve_wait_any_chan (same kind
// switch, so an unsupported kind here just means "never ready," which
// never actually happens since resolve_wait_any_chan already rejected
// it before this is ever called for that handle).
static int wait_any_handle_ready(process64_t* cur, int h) {
    switch (cur->handles[h].kind) {
    case HANDLE64_PIPE_READ:      return pipe64_read_ready((pipe64_t*)cur->handles[h].obj);
    case HANDLE64_PIPE_WRITE:     return pipe64_write_ready((pipe64_t*)cur->handles[h].obj);
    case HANDLE64_INPUT:          return input64_ready();
    case HANDLE64_SERVICE_LISTEN: return service64_has_pending((service64_t*)cur->handles[h].obj);
    default: return 0;
    }
}

static inline uint64_t wait_any_syscall_lock(void) {
    uint64_t flags;
    __asm__ volatile ("pushfq\n\tpop %0\n\tcli" : "=r"(flags) :: "memory");
    return flags;
}
static inline void wait_any_syscall_unlock(uint64_t flags) {
    if (flags & (1ULL << 9)) __asm__ volatile ("sti" ::: "memory");
}

// The exact "hold cli across check -> maybe block -> recheck" discipline
// sys64_service_connect (above) already uses for ITS own blocking case,
// generalized from one condition to a bounded set of them. Every
// readiness check happens under the SAME critical section that also
// protects the eventual process64_block_on_any call, so no IRQ-driven
// wake (input64_push, pipe64_*'s own wake_all-on-space-freed) can land
// in the gap between "decided nothing is ready" and "actually blocked"
// -- see this milestone's own design notes for the full race analysis.
static uint64_t sys64_handle_wait_any(uint64_t req_ptr) {
    process64_t* cur = process64_current();
    if (!cur) return (uint64_t)-1;

    wait_any_req64_t req;
    if (copy_from_user64(&req, req_ptr, sizeof(req)) < 0) return (uint64_t)-1;
    if (req.count == 0 || req.count > PROCESS64_WAIT_ANY_MAX) return (uint64_t)-1;

    int hbuf[PROCESS64_WAIT_ANY_MAX];
    if (copy_from_user64(hbuf, (uint64_t)req.handles, (uint64_t)req.count * sizeof(int)) < 0) return (uint64_t)-1;

    void* chans[PROCESS64_WAIT_ANY_MAX];
    uint64_t flags = wait_any_syscall_lock();

    // All-or-nothing validation, same discipline as
    // sys64_service_accept's own rollback-on-bad-pointer precedent: an
    // invalid handle/kind ANYWHERE in the list fails the whole call
    // before anything is registered or blocked.
    for (uint32_t i = 0; i < req.count; i++) {
        if (resolve_wait_any_chan(cur, hbuf[i], &chans[i]) < 0) {
            wait_any_syscall_unlock(flags);
            return (uint64_t)-1;
        }
    }

    uint64_t deadline = (req.timeout_ticks == 0 || req.timeout_ticks == SYS64_WAIT_FOREVER)
                       ? 0 : (timer64_get_ticks() + req.timeout_ticks);
    for (;;) {
        int ready = 0;
        for (uint32_t i = 0; i < req.count; i++) {
            if (wait_any_handle_ready(cur, hbuf[i])) { ready = 1; break; }
        }
        if (ready) { wait_any_syscall_unlock(flags); return 0; }
        if (req.timeout_ticks == 0) { wait_any_syscall_unlock(flags); return SYS64_ERR_TIMEOUT; }
        if (deadline != 0 && timer64_get_ticks() >= deadline) { wait_any_syscall_unlock(flags); return SYS64_ERR_TIMEOUT; }
        process64_block_on_any(chans, (int)req.count, deadline);   // returns still under cli, per process64_block_on's own contract
    }
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

// Milestone 30: see include/syscall64.h's header comment on
// SYS64_SHM_TOKEN/SYS64_SHM_OPEN_TOKEN and include/shm64.h's on
// shm64_t::token for the full cross-process handoff rationale.
static uint64_t sys64_shm_token(int h) {
    process64_t* cur = process64_current();
    if (!cur) return (uint64_t)-1;
    if (h < 0 || h >= PROCESS64_MAX_HANDLES || cur->handles[h].kind != HANDLE64_SHM) return (uint64_t)-1;
    return ((shm64_t*)cur->handles[h].obj)->token;
}

static uint64_t sys64_shm_size(int h) {
    process64_t* cur = process64_current();
    if (!cur) return (uint64_t)-1;
    if (h < 0 || h >= PROCESS64_MAX_HANDLES || cur->handles[h].kind != HANDLE64_SHM) return (uint64_t)-1;
    return (uint64_t)((shm64_t*)cur->handles[h].obj)->obj->npages * 0x1000ULL;
}

static uint64_t sys64_shm_open_token(uint64_t token) {
    process64_t* cur = process64_current();
    if (!cur) return (uint64_t)-1;

    shm64_t* s = shm64_find_by_token(token);
    if (!s) return (uint64_t)-1;

    int slot = find_free_handle(cur);
    if (slot < 0) return (uint64_t)-1;

    shm64_add_ref(s);
    cur->handles[slot].kind = HANDLE64_SHM;
    cur->handles[slot].obj = s;
    return (uint64_t)slot;
}

// ── M+1B: generic GPU buffer syscalls -- literal ports of the shm64
// four above (create/token/open_token/size), same validation shape,
// same handle-kind check pattern. SYS64_GPU_BUFFER_MAP is deliberately
// not implemented this milestone -- see include/gpu64.h's own header
// comment for why nothing in M+1B's required tests or its null driver
// needs a CPU mapping of the GPU-side view.
static uint64_t sys64_gpu_buffer_create(uint32_t device_idx, uint64_t size, uint32_t usage) {
    process64_t* cur = process64_current();
    if (!cur) return (uint64_t)-1;

    gpu64_device_t* dev = gpu64_by_index(device_idx);
    if (!dev) return (uint64_t)-1;

    int slot = find_free_handle(cur);
    if (slot < 0) return (uint64_t)-1;

    gpu64_buffer_t* buf;
    if (gpu64_buffer_create(dev, size, usage, &buf) < 0) return (uint64_t)-1;

    cur->handles[slot].kind = HANDLE64_GPU_BUFFER;
    cur->handles[slot].obj = buf;
    return (uint64_t)slot;
}

static uint64_t sys64_gpu_buffer_token(int h) {
    process64_t* cur = process64_current();
    if (!cur) return (uint64_t)-1;
    if (h < 0 || h >= PROCESS64_MAX_HANDLES || cur->handles[h].kind != HANDLE64_GPU_BUFFER) return (uint64_t)-1;
    return ((gpu64_buffer_t*)cur->handles[h].obj)->token;
}

static uint64_t sys64_gpu_buffer_size(int h) {
    process64_t* cur = process64_current();
    if (!cur) return (uint64_t)-1;
    if (h < 0 || h >= PROCESS64_MAX_HANDLES || cur->handles[h].kind != HANDLE64_GPU_BUFFER) return (uint64_t)-1;
    return (uint64_t)((gpu64_buffer_t*)cur->handles[h].obj)->backing->npages * 0x1000ULL;
}

static uint64_t sys64_gpu_buffer_open_token(uint64_t token) {
    process64_t* cur = process64_current();
    if (!cur) return (uint64_t)-1;

    gpu64_buffer_t* buf = gpu64_buffer_find_by_token(token);
    if (!buf) return (uint64_t)-1;

    int slot = find_free_handle(cur);
    if (slot < 0) return (uint64_t)-1;

    gpu64_buffer_add_ref(buf);
    cur->handles[slot].kind = HANDLE64_GPU_BUFFER;
    cur->handles[slot].obj = buf;
    return (uint64_t)slot;
}

// ── Milestone 32: named local-service registry ──────────────────────
// See include/syscall64.h's header comments on SYS64_SERVICE_LISTEN/
// ACCEPT/CONNECT and include/service64.h for the full design.

static inline uint64_t service64_syscall_lock(void) {
    uint64_t flags;
    __asm__ volatile ("pushfq\n\tpop %0\n\tcli" : "=r"(flags) :: "memory");
    return flags;
}
static inline void service64_syscall_unlock(uint64_t flags) {
    if (flags & (1ULL << 9)) __asm__ volatile ("sti" ::: "memory");
}

static uint64_t sys64_service_listen(uint64_t name_ptr) {
    process64_t* cur = process64_current();
    if (!cur) return (uint64_t)-1;

    char name[SERVICE64_NAME_MAX];
    uint64_t len = 0;
    if (copy_user_cstr64(name, name_ptr, sizeof(name), &len) < 0) return (uint64_t)-1;
    if (len == 0 || !service64_name_valid(name)) return (uint64_t)-1;

    int slot = find_free_handle(cur);
    if (slot < 0) return (uint64_t)-1;

    service64_t* svc;
    if (service64_listen(name, cur->pid, &svc) < 0) return (uint64_t)-1;

    cur->handles[slot].kind = HANDLE64_SERVICE_LISTEN;
    cur->handles[slot].obj = svc;
    return (uint64_t)slot;
}

static uint64_t sys64_service_accept(int h, uint64_t out_ptr) {
    process64_t* cur = process64_current();
    if (!cur) return (uint64_t)-1;
    if (h < 0 || h >= PROCESS64_MAX_HANDLES || cur->handles[h].kind != HANDLE64_SERVICE_LISTEN) return (uint64_t)-1;

    service64_endpoints_t ep;
    int r = service64_accept((service64_t*)cur->handles[h].obj, cur, &ep);
    if (r == -2) return SYS64_ERR_WOULDBLOCK;
    if (r < 0) return (uint64_t)-1;

    if (copy_to_user64(out_ptr, &ep, sizeof(ep)) < 0) {
        // Bad user pointer -- undo the two handles we just installed
        // (mirrors sys64_pipe_create's own rollback-on-bad-pointer
        // pattern) rather than leaking a live connection the caller can
        // never reach.
        pipe64_close_read((pipe64_t*)cur->handles[ep.recv].obj);
        pipe64_close_write((pipe64_t*)cur->handles[ep.send].obj);
        cur->handles[ep.recv].kind = HANDLE64_UNUSED;
        cur->handles[ep.send].kind = HANDLE64_UNUSED;
        return (uint64_t)-1;
    }
    return 0;
}

static uint64_t sys64_service_connect(uint64_t name_ptr, uint64_t blocking, uint64_t out_ptr) {
    process64_t* cur = process64_current();
    if (!cur) return (uint64_t)-1;

    char name[SERVICE64_NAME_MAX];
    uint64_t len = 0;
    if (copy_user_cstr64(name, name_ptr, sizeof(name), &len) < 0) return (uint64_t)-1;
    if (len == 0 || !service64_name_valid(name)) return (uint64_t)-1;

    // Milestone 26's block_on contract: interrupts held disabled by the
    // CALLER across the entire check-then-maybe-block-then-recheck
    // sequence, never released until the condition truly holds (or we
    // give up) -- see kernel/pipe64.c's pipe64_read for the identical
    // discipline. Held continuously through the service64_connect()
    // call too, so `svc` cannot be unpublished by a preempted process
    // between "found it" and "actually connected to it".
    uint64_t flags = service64_syscall_lock();
    service64_t* svc;
    for (;;) {
        svc = service64_find_by_name(name);
        if (svc) break;
        if (!blocking) { service64_syscall_unlock(flags); return (uint64_t)-1; }
        service64_wait_for_registration();
    }

    service64_endpoints_t ep;
    int rc = service64_connect(svc, cur, &ep);
    service64_syscall_unlock(flags);
    if (rc < 0) return (uint64_t)-1;

    if (copy_to_user64(out_ptr, &ep, sizeof(ep)) < 0) {
        pipe64_close_write((pipe64_t*)cur->handles[ep.send].obj);
        pipe64_close_read((pipe64_t*)cur->handles[ep.recv].obj);
        cur->handles[ep.send].kind = HANDLE64_UNUSED;
        cur->handles[ep.recv].kind = HANDLE64_UNUSED;
        return (uint64_t)-1;
    }
    return 0;
}

// ── Milestone 32: explicit-inheritance spawn ────────────────────────
typedef struct {
    uint64_t args_ptr;
    uint64_t inherit_ptr;
    uint32_t inherit_count;
} spawn_ex_req64_t;

static uint64_t sys64_spawn_ex(uint64_t path_ptr, uint64_t req_ptr) {
    process64_t* parent = process64_current();
    if (!parent) return (uint64_t)-1;

    char path[SYS64_PATH_MAX];
    if (copy_user_cstr64(path, path_ptr, sizeof(path), 0) < 0) return (uint64_t)-1;

    spawn_ex_req64_t req;
    if (copy_from_user64(&req, req_ptr, sizeof(req)) < 0) return (uint64_t)-1;

    char args[SYS64_ARGS_MAX];
    args[0] = 0;
    if (req.args_ptr && copy_user_cstr64(args, req.args_ptr, sizeof(args), 0) < 0) return (uint64_t)-1;

    if (req.inherit_count > PROCESS64_MAX_HANDLES) return (uint64_t)-1;
    int32_t inherit[PROCESS64_MAX_HANDLES];
    if (req.inherit_count > 0) {
        if (!req.inherit_ptr) return (uint64_t)-1;
        if (copy_from_user64(inherit, req.inherit_ptr, (uint64_t)req.inherit_count * sizeof(int32_t)) < 0) return (uint64_t)-1;
    }

    uint32_t child_pid = 0;
    if (process64_spawn_ex(path, args, parent->pid, inherit, (int)req.inherit_count, &child_pid) < 0) return (uint64_t)-1;
    return (uint64_t)child_pid;
}

// ── Milestone 29: structured input + userspace display present ──────
// See include/syscall64.h's header comment on SYS64_INPUT_OPEN/
// SYS64_DISPLAY_OPEN/SYS64_DISPLAY_PRESENT for the full design.

static uint64_t sys64_input_open(void) {
    process64_t* cur = process64_current();
    if (!cur) return (uint64_t)-1;

    int slot = find_free_handle(cur);
    if (slot < 0) return (uint64_t)-1;
    if (input64_acquire() < 0) return (uint64_t)-1;

    cur->handles[slot].kind = HANDLE64_INPUT;
    cur->handles[slot].obj  = (void*)1; // singleton -- kind alone identifies it
    return (uint64_t)slot;
}

// M+7A: see include/syscall64.h's own header comment on
// SYS64_INPUT_GET_ABS_RANGE. Requires a HANDLE64_INPUT handle -- the
// same ownership check SYS64_INPUT_OPEN's own handle already implies --
// so only the process that opened the input stream can query this.
typedef struct {
    int32_t min_x, max_x, min_y, max_y;
} input64_abs_range_req_t;

static uint64_t sys64_input_get_abs_range(int handle, uint64_t req_ptr) {
    process64_t* cur = process64_current();
    if (!cur || handle < 0 || handle >= PROCESS64_MAX_HANDLES || cur->handles[handle].kind != HANDLE64_INPUT) {
        return (uint64_t)-1;
    }
    input64_abs_range_t range;
    if (input64_get_abs_range(&range) < 0) return (uint64_t)-1;

    input64_abs_range_req_t out;
    out.min_x = range.min_x; out.max_x = range.max_x;
    out.min_y = range.min_y; out.max_y = range.max_y;
    if (copy_to_user64(req_ptr, &out, sizeof(out)) < 0) return (uint64_t)-1;
    return 0;
}

static uint64_t sys64_display_open(uint64_t width_out_ptr, uint64_t height_out_ptr, uint64_t format_out_ptr) {
    process64_t* cur = process64_current();
    if (!cur) return (uint64_t)-1;
    if (!display64_available()) return (uint64_t)-1;

    int slot = find_free_handle(cur);
    if (slot < 0) return (uint64_t)-1;
    if (display64_acquire() < 0) return (uint64_t)-1;

    display64_info_t info;
    display64_get_info(&info);
    uint32_t fmt = DISPLAY64_FORMAT_LOGICAL_XRGB8888;

    if (copy_to_user64(width_out_ptr, &info.width, sizeof(info.width)) < 0 ||
        copy_to_user64(height_out_ptr, &info.height, sizeof(info.height)) < 0 ||
        (format_out_ptr && copy_to_user64(format_out_ptr, &fmt, sizeof(fmt)) < 0)) {
        display64_release();
        return (uint64_t)-1;
    }

    cur->handles[slot].kind = HANDLE64_DISPLAY;
    cur->handles[slot].obj  = (void*)1;
    return (uint64_t)slot;
}

typedef struct {
    uint64_t buf_ptr;
    uint32_t pitch;
    uint32_t x, y, w, h;
} display64_present_req_t;

// M+9B: request structs for the direct-scanout syscalls -- same
// struct-by-pointer convention as display64_present_req_t above (this
// codebase's syscall ABI tops out at 3 register args; see
// user64/tox64.h's SYSCALL0..3). Independently duplicated in
// user64/tox64.h, kept in sync manually -- same precedent as every
// other request struct and SYS64_* constant in this codebase.
typedef struct {
    uint64_t shm_token;
    uint32_t width, height;
} display64_direct_bind_req_t;

typedef struct {
    uint64_t shm_token;
    uint32_t x, y, w, h;
    uint32_t switch_active;
} display64_direct_present_req_t;

#define SYS64_DISPLAY_MAX_DIM 4096 // sanity cap -- real display width/height already enforce the true bound

// M+4 item 12 / M+4 investigation: define to have SYS64_DISPLAY_PRESENT
// accumulate total time spent inside itself (the row-copy loop plus,
// when active, the GPU backend's transfer+flush) and periodically klog
// a summary -- backend-agnostic instrumentation living at the ONE call
// site both backends share, so the SAME real compositor workload can be
// measured under software presentation and VirtIO-GPU presentation with
// no other code difference between the two runs. Originally built on
// the 100Hz PIT tick (SYS64_GET_TICKS); upgraded to kernel/tsc64.c's
// RDTSC-based clock after manual interactive testing found real lag the
// 10ms tick granularity could not explain -- most individual present
// calls complete well under one tick, so a tick-based total was
// frequently just "0" regardless of the real cost. Also dumps the
// VirtIO-GPU driver's own per-command counters (commands, poll
// iterations, blocked time -- kernel/virtio_gpu64.c's
// virtio_gpu64_debug_stats_report()) alongside, since the interesting
// question isn't just "how long did presenting take" but "how much of
// that time was VirtIO command/poll overhead specifically." Matches
// this codebase's existing convention of gating development-only
// diagnostics behind an explicit #define rather than always-on logging.
// Not left enabled by default.
// #define SYS64_DISPLAY_PRESENT_PERF_STATS 1

#ifdef SYS64_DISPLAY_PRESENT_PERF_STATS
static uint64_t g_present_stat_cycles = 0;
static uint64_t g_present_stat_calls = 0;
static uint64_t g_present_stat_pixels = 0;
static uint64_t g_present_stat_window_start_tick = 0;
static void present_perf_report_if_due(void) {
    if (g_present_stat_calls < 200) return; // report every N present calls -- klog_hex() always terminates its own line, one field per line
    extern uint64_t timer64_get_ticks(void);
    uint64_t window_ms = (timer64_get_ticks() - g_present_stat_window_start_tick) * 10; // 100Hz = 10ms/tick
    uint64_t us_total = tsc64_cycles_to_us(g_present_stat_cycles);
    klog_hex("sys64_display_present perf: calls=          ", (uint32_t)g_present_stat_calls);
    klog_hex("sys64_display_present perf: total_us=       ", (uint32_t)us_total);
    klog_hex("sys64_display_present perf: avg_us/call=    ", (uint32_t)(g_present_stat_calls ? us_total / g_present_stat_calls : 0));
    klog_hex("sys64_display_present perf: pixels=         ", (uint32_t)g_present_stat_pixels);
    klog_hex("sys64_display_present perf: window_ms=      ", (uint32_t)window_ms);
    virtio_gpu64_debug_stats_report(window_ms);
    virtio_gpu64_debug_stats_reset();
    g_present_stat_cycles = 0; g_present_stat_calls = 0; g_present_stat_pixels = 0;
    g_present_stat_window_start_tick = timer64_get_ticks();
}
#endif

static uint64_t sys64_display_present(int handle, uint64_t req_ptr) {
    process64_t* cur = process64_current();
    if (!cur) return (uint64_t)-1;
    if (handle < 0 || handle >= PROCESS64_MAX_HANDLES || cur->handles[handle].kind != HANDLE64_DISPLAY)
        return (uint64_t)-1;

    display64_present_req_t req;
    if (copy_from_user64(&req, req_ptr, sizeof(req)) < 0) return (uint64_t)-1;
    if (req.w == 0 || req.h == 0 || req.w > SYS64_DISPLAY_MAX_DIM || req.h > SYS64_DISPLAY_MAX_DIM) return (uint64_t)-1;

    display64_info_t info;
    display64_get_info(&info);
    if (req.x >= info.width || req.y >= info.height) return (uint64_t)-1;
    if (req.w > info.width - req.x || req.h > info.height - req.y) return (uint64_t)-1;

    uint32_t pitch = req.pitch ? req.pitch : req.w * 4u;
    if (pitch < req.w * 4u) return (uint64_t)-1; // pitch too small to hold w pixels

    // Validate the ENTIRE source span up front (same whole-range-then-
    // direct-access pattern as copy_from_user64/copy_to_user64 --
    // Milestone 25) -- works correctly for a virtually contiguous
    // buffer backed by non-contiguous physical pages with no special
    // casing, since validation and the subsequent reads both go through
    // this process's own live page tables via ordinary virtual
    // addressing, never a physical-address walk.
    uint64_t span = (uint64_t)(req.h - 1) * pitch + (uint64_t)req.w * 4u;
    if (paging64_check_user_range(&cur->as, req.buf_ptr, span, 0) < 0) return (uint64_t)-1;

    uint32_t* rowbuf = (uint32_t*)kmalloc((uint64_t)req.w * 4u);
    if (!rowbuf) return (uint64_t)-1;

#ifdef SYS64_DISPLAY_PRESENT_PERF_STATS
    uint64_t perf_start = tsc64_read();
#endif

    for (uint32_t row = 0; row < req.h; row++) {
        const uint32_t* src = (const uint32_t*)(uintptr_t)(req.buf_ptr + (uint64_t)row * pitch);
        for (uint32_t col = 0; col < req.w; col++) rowbuf[col] = src[col];
        display64_blit_row(req.x, req.y + row, req.w, rowbuf);
    }

    kfree(rowbuf);

    // M+4: one flush per whole present call, after every row has been
    // written -- a safe no-op when the active backend is the legacy
    // framebuffer (blit_row's writes already landed directly in the
    // physical framebuffer). See include/display64.h's own header
    // comment on why this is batched here rather than called from
    // inside display64_blit_row itself.
    // M+8: its return value is now this whole syscall's own honest
    // success/failure verdict -- see display64_gpu_flush_rect()'s own
    // header comment for exactly what SUCCESS means here. Previously
    // discarded entirely, which is precisely the gap that made it
    // impossible for compositor64 to ever distinguish a real
    // presentation failure from success.
    if (display64_gpu_flush_rect(req.x, req.y, req.w, req.h) < 0) return (uint64_t)-1;

#ifdef SYS64_DISPLAY_PRESENT_PERF_STATS
    g_present_stat_cycles += tsc64_read() - perf_start;
    g_present_stat_calls++;
    g_present_stat_pixels += (uint64_t)req.w * req.h;
    present_perf_report_if_due();
#endif

    return 0;
}

// Shared by SYS64_DISPLAY_DIRECT_*, SYS64_CURSOR_* below, and every
// other syscall restricted to whichever process holds the one open
// HANDLE64_DISPLAY handle -- moved above both call sites rather than
// duplicated (originally defined only right before the M+7 cursor
// syscalls further down, which M+9B's own direct-scanout calls also
// need).
static inline int cursor64_caller_owns_display(process64_t* cur, int handle) {
    return handle >= 0 && handle < PROCESS64_MAX_HANDLES && cur->handles[handle].kind == HANDLE64_DISPLAY;
}

// ── M+9B: direct-scanout / composition-bypass syscalls ────────────────
// All four share SYS64_DISPLAY_PRESENT's own access-control check --
// only the process holding the open HANDLE64_DISPLAY handle (the
// compositor) may call any of these. Genuinely thin: each one
// validates its own inputs then calls straight into the display64.c
// state machine, which owns every real invariant (§7-11) itself -- see
// include/display64.h's own header comment.
static uint64_t sys64_display_direct_query(int handle) {
    process64_t* cur = process64_current();
    if (!cur || !cursor64_caller_owns_display(cur, handle)) return 0;
    return (uint64_t)display64_direct_scanout_supported();
}

static uint64_t sys64_display_direct_bind(int handle, uint64_t req_ptr) {
    process64_t* cur = process64_current();
    if (!cur || !cursor64_caller_owns_display(cur, handle)) return (uint64_t)-1;

    display64_direct_bind_req_t req;
    if (copy_from_user64(&req, req_ptr, sizeof(req)) < 0) return (uint64_t)-1;
    if (req.width == 0 || req.height == 0) return (uint64_t)-1;

    // Same token-resolution primitive SYS64_SHM_OPEN_TOKEN already uses
    // (shm tokens are cross-process-shareable by design -- M+5's whole
    // client/compositor model depends on it) -- but no new handle/ref is
    // taken on the shm64_t itself: gpu64_buffer_wrap() (reached via
    // display64_direct_scanout_bind() -> the registered bind_fn) takes
    // its OWN independent reference directly on the memobj64_t backing,
    // per include/gpu64.h's own documented contract, so this shm64_t
    // borrow only needs to survive this one function call.
    shm64_t* s = shm64_find_by_token(req.shm_token);
    if (!s) return (uint64_t)-1;

    void* out_handle = 0;
    if (display64_direct_scanout_bind(req.shm_token, s->obj, req.width, req.height, &out_handle) < 0) return (uint64_t)-1;
    return 0;
}

static uint64_t sys64_display_direct_present(int handle, uint64_t req_ptr) {
    process64_t* cur = process64_current();
    if (!cur || !cursor64_caller_owns_display(cur, handle)) return (uint64_t)-1;

    display64_direct_present_req_t req;
    if (copy_from_user64(&req, req_ptr, sizeof(req)) < 0) return (uint64_t)-1;

    // No client-facing handle number or geometry is passed here at all
    // -- display64.c already remembers both (per shm_token identity)
    // from the bind() call, and re-deriving/re-validating them per
    // present would just be a second place they could drift out of
    // sync (see display64_direct_present()'s own header comment).
    if (display64_direct_present(req.shm_token, req.x, req.y, req.w, req.h, (int)req.switch_active) < 0) {
        return (uint64_t)-1;
    }
    return 0;
}

static uint64_t sys64_display_direct_leave(int handle) {
    process64_t* cur = process64_current();
    if (!cur || !cursor64_caller_owns_display(cur, handle)) return (uint64_t)-1;
    return (uint64_t)(int64_t)display64_leave_direct_scanout();
}

static uint64_t sys64_display_direct_unbind(int handle, uint64_t shm_token) {
    process64_t* cur = process64_current();
    if (!cur || !cursor64_caller_owns_display(cur, handle)) return (uint64_t)-1;
    display64_direct_scanout_unbind(shm_token);
    return 0;
}

// M+10A: see include/syscall64.h's own SYS64_DISPLAY_DEBUG_STATE comment
// -- three fields only, read via the existing display64_state_get_current()/
// display64_state_get_validity() accessors (§5-§10 of the M+10 header),
// never mutating anything.
typedef struct {
    uint32_t validity;         // display64_validity_t: 0 = VALID, 1 = RECOVERY_REQUIRED
    uint32_t primary_kind;     // display64_primary_kind_t: 0 = COMPOSITED, 1 = DIRECT
    uint64_t primary_identity; // DIRECT only -- 0 for COMPOSITED
} display64_debug_state_req_t;

static uint64_t sys64_display_debug_state(int handle, uint64_t out_ptr) {
    process64_t* cur = process64_current();
    if (!cur || !cursor64_caller_owns_display(cur, handle)) return (uint64_t)-1;

    display64_state_t st;
    display64_state_get_current(&st);

    display64_debug_state_req_t out;
    out.validity = (uint32_t)display64_state_get_validity();
    out.primary_kind = (uint32_t)st.primary.kind;
    out.primary_identity = st.primary.identity;

    if (copy_to_user64(out_ptr, &out, sizeof(out)) < 0) return (uint64_t)-1;
    return 0;
}

// ── M+7: generic hardware cursor control ─────────────────────────────
// See include/syscall64.h's own header comment on SYS64_CURSOR_AVAILABLE/
// SET_IMAGE/MOVE/SET_VISIBLE for the full design. All four require the
// caller to hold a HANDLE64_DISPLAY handle -- the same ownership check
// SYS64_DISPLAY_PRESENT already uses -- so only the process that opened
// the display (the compositor) may drive the hardware cursor.
static uint64_t sys64_cursor_available(int handle) {
    process64_t* cur = process64_current();
    if (!cur || !cursor64_caller_owns_display(cur, handle)) return 0;
    return (uint64_t)display64_cursor_available();
}

typedef struct {
    uint64_t pixels_ptr;
    uint32_t width, height;
    uint32_t hot_x, hot_y;
} cursor64_set_image_req_t;

static uint64_t sys64_cursor_set_image(int handle, uint64_t req_ptr) {
    process64_t* cur = process64_current();
    if (!cur || !cursor64_caller_owns_display(cur, handle)) return (uint64_t)-1;

    cursor64_set_image_req_t req;
    if (copy_from_user64(&req, req_ptr, sizeof(req)) < 0) return (uint64_t)-1;
    if (req.width == 0 || req.height == 0) return (uint64_t)-1;

    // Overflow-checked span (the width*height multiplication itself can
    // never overflow a uint64_t -- two uint32_t operands top out well
    // under UINT64_MAX -- but the subsequent *4 for byte count can, for
    // a large enough width/height, so that step is what's actually
    // guarded here; same discipline as virtio_gpu64_init_compositor_backend's
    // own geometry-overflow check).
    uint64_t pixel_count = (uint64_t)req.width * (uint64_t)req.height;
    if (pixel_count > (0xFFFFFFFFFFFFFFFFULL / 4u)) return (uint64_t)-1;
    uint64_t span = pixel_count * 4u;

    uint32_t* kbuf = (uint32_t*)kmalloc(span);
    if (!kbuf) return (uint64_t)-1;
    if (copy_from_user64(kbuf, req.pixels_ptr, span) < 0) { kfree(kbuf); return (uint64_t)-1; }

    // display64_cursor_set_image() itself (via the registered driver
    // callback) is what actually rejects a width/height that doesn't
    // match the hardware's one persistent cursor resource size -- this
    // syscall handler never hardcodes that number, keeping it a
    // driver-owned detail exactly like every other VirtIO-specific fact.
    int rc = display64_cursor_set_image(req.width, req.height, kbuf, req.hot_x, req.hot_y);
    kfree(kbuf);
    return (uint64_t)(int64_t)rc;
}

static uint64_t sys64_cursor_move(int handle, int32_t x, int32_t y) {
    process64_t* cur = process64_current();
    if (!cur || !cursor64_caller_owns_display(cur, handle)) return (uint64_t)-1;
    return (uint64_t)(int64_t)display64_cursor_move(x, y);
}

static uint64_t sys64_cursor_set_visible(int handle, uint32_t visible) {
    process64_t* cur = process64_current();
    if (!cur || !cursor64_caller_owns_display(cur, handle)) return (uint64_t)-1;
    return (uint64_t)(int64_t)display64_cursor_set_visible((int)visible);
}

// M-next: see include/syscall64.h's SYS64_GET_TICKS comment. No header
// exists for timer64.c (kernel64.c itself declares timer64_init/
// timer64_handler the same inline-extern way) -- matching that existing
// convention here rather than introducing a new header for one function.
extern uint64_t timer64_get_ticks(void);
static uint64_t sys64_get_ticks(void) {
    return timer64_get_ticks();
}

// M+4 investigation: see include/syscall64.h's own header comment on
// SYS64_SLEEP_TICKS. Deliberately does NOT gate on process64_current()
// the way most syscalls do -- process64_sleep_ticks() itself requires a
// real current process (documented in its own header comment) and
// every caller of a syscall is, by construction, a real ring-3 process;
// there is no untracked-stub precedent here the way sys64_write() has.
static uint64_t sys64_sleep_ticks(uint64_t ticks) {
    process64_sleep_ticks(ticks);
    return 0;
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
    case SYS64_READDIR_NEXT:
        tf->rax = sys64_readdir_next((int)tf->rdi, tf->rsi);
        break;
    case SYS64_INPUT_OPEN:
        tf->rax = sys64_input_open();
        break;
    case SYS64_DISPLAY_OPEN:
        tf->rax = sys64_display_open(tf->rdi, tf->rsi, tf->rdx);
        break;
    case SYS64_DISPLAY_PRESENT:
        tf->rax = sys64_display_present((int)tf->rdi, tf->rsi);
        break;
    case SYS64_HANDLE_TRY_READ:
        tf->rax = sys64_handle_try_read((int)tf->rdi, tf->rsi, tf->rdx);
        break;
    case SYS64_SHM_TOKEN:
        tf->rax = sys64_shm_token((int)tf->rdi);
        break;
    case SYS64_SHM_OPEN_TOKEN:
        tf->rax = sys64_shm_open_token(tf->rdi);
        break;
    case SYS64_SHM_SIZE:
        tf->rax = sys64_shm_size((int)tf->rdi);
        break;
    case SYS64_SERVICE_LISTEN:
        tf->rax = sys64_service_listen(tf->rdi);
        break;
    case SYS64_SERVICE_ACCEPT:
        tf->rax = sys64_service_accept((int)tf->rdi, tf->rsi);
        break;
    case SYS64_SERVICE_CONNECT:
        tf->rax = sys64_service_connect(tf->rdi, tf->rsi, tf->rdx);
        break;
    case SYS64_SPAWN_EX:
        tf->rax = sys64_spawn_ex(tf->rdi, tf->rsi);
        break;
    case SYS64_HANDLE_TRY_WRITE:
        tf->rax = sys64_handle_try_write((int)tf->rdi, tf->rsi, tf->rdx);
        break;
    case SYS64_GET_TICKS:
        tf->rax = sys64_get_ticks();
        break;
    case SYS64_GPU_BUFFER_CREATE:
        tf->rax = sys64_gpu_buffer_create((uint32_t)tf->rdi, tf->rsi, (uint32_t)tf->rdx);
        break;
    case SYS64_GPU_BUFFER_TOKEN:
        tf->rax = sys64_gpu_buffer_token((int)tf->rdi);
        break;
    case SYS64_GPU_BUFFER_OPEN_TOKEN:
        tf->rax = sys64_gpu_buffer_open_token(tf->rdi);
        break;
    case SYS64_GPU_BUFFER_SIZE:
        tf->rax = sys64_gpu_buffer_size((int)tf->rdi);
        break;
    case SYS64_SLEEP_TICKS:
        tf->rax = sys64_sleep_ticks(tf->rdi);
        break;
    case SYS64_CURSOR_AVAILABLE:
        tf->rax = sys64_cursor_available((int)tf->rdi);
        break;
    case SYS64_CURSOR_SET_IMAGE:
        tf->rax = sys64_cursor_set_image((int)tf->rdi, tf->rsi);
        break;
    case SYS64_CURSOR_MOVE:
        tf->rax = sys64_cursor_move((int)tf->rdi, (int32_t)tf->rsi, (int32_t)tf->rdx);
        break;
    case SYS64_CURSOR_SET_VISIBLE:
        tf->rax = sys64_cursor_set_visible((int)tf->rdi, (uint32_t)tf->rsi);
        break;

    case SYS64_INPUT_GET_ABS_RANGE:
        tf->rax = sys64_input_get_abs_range((int)tf->rdi, tf->rsi);
        break;

    case SYS64_DISPLAY_DIRECT_QUERY:
        tf->rax = sys64_display_direct_query((int)tf->rdi);
        break;
    case SYS64_DISPLAY_DIRECT_BIND:
        tf->rax = sys64_display_direct_bind((int)tf->rdi, tf->rsi);
        break;
    case SYS64_DISPLAY_DIRECT_PRESENT:
        tf->rax = sys64_display_direct_present((int)tf->rdi, tf->rsi);
        break;
    case SYS64_DISPLAY_DIRECT_LEAVE:
        tf->rax = sys64_display_direct_leave((int)tf->rdi);
        break;
    case SYS64_DISPLAY_DIRECT_UNBIND:
        tf->rax = sys64_display_direct_unbind((int)tf->rdi, tf->rsi);
        break;
    case SYS64_DISPLAY_DEBUG_STATE:
        tf->rax = sys64_display_debug_state((int)tf->rdi, tf->rsi);
        break;
    case SYS64_HANDLE_WAIT_ANY:
        tf->rax = sys64_handle_wait_any(tf->rdi);
        break;
    default:
        tf->rax = (uint64_t)-1;
        break;
    }
}
