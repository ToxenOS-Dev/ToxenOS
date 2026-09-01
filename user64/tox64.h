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
// SYS64_READ (5) / SYS64_CLOSE (6) retired Milestone 27 -- an open
// file is read/written/closed through the generic SYS64_HANDLE_READ/
// WRITE/CLOSE below, same as a pipe end; see sys_read/sys_close.
#define SYS64_STAT   7
#define SYS64_SPAWN  8
#define SYS64_WAIT   9
#define SYS64_GETCH  10
#define SYS64_GET_ARGS 11
#define SYS64_CLEAR 12
#define SYS64_SETCOLOR 13
#define SYS64_READDIR 14
#define SYS64_MKDIR      15
#define SYS64_MKFILE     16
#define SYS64_WRITE_FILE 17
#define SYS64_DELETE     18
#define SYS64_RMDIR      19
#define SYS64_RENAME     20
#define SYS64_BRK    21
#define SYS64_MMAP   22
#define SYS64_MUNMAP 23
#define SYS64_PIPE_CREATE  24
#define SYS64_HANDLE_READ  25
#define SYS64_HANDLE_WRITE 26
#define SYS64_HANDLE_CLOSE 27
#define SYS64_SHM_CREATE   28
#define SYS64_SHM_MAP      29
#define SYS64_READDIR_NEXT 30
// Return value -2 means the path is protected (kernel refused the op).
#define SYS64_ERR_PROTECTED ((int64_t)-2)
// Return value -3 from SYS64_DELETE means the folder is not empty.
#define SYS64_ERR_NOTEMPTY  ((int64_t)-3)
// Return value -4 from SYS64_RENAME means the destination already exists.
#define SYS64_ERR_EXISTS    ((int64_t)-4)

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
// failure (bad pointer, bad handle, file not found, etc.). Milestone
// 27: sys_open returns a handle from the same unified per-process
// handle table pipes/shared-memory use (works for directories too --
// see sys_readdir_next); sys_read/sys_close are no longer separate
// syscalls of their own, just this file-specific NAME kept stable for
// every existing caller -- they now route through the generic
// SYS64_HANDLE_READ/SYS64_HANDLE_CLOSE (see sys_handle_read/
// sys_handle_close below), which do the exact same thing for a
// HANDLE64_FILE as these always did.
static inline int64_t sys_open(const char* path) {
    return (int64_t)SYSCALL1(SYS64_OPEN, path);
}

static inline int64_t sys_read(int fd, char* buf, uint64_t len) {
    return (int64_t)SYSCALL3(SYS64_HANDLE_READ, fd, buf, len);
}

static inline int64_t sys_close(int fd) {
    return (int64_t)SYSCALL1(SYS64_HANDLE_CLOSE, fd);
}

// Milestone 11: is_dir_out may be NULL if the caller doesn't care about
// the entry's type, just its size.
static inline int64_t sys_stat(const char* path, uint64_t* size_out, int* is_dir_out) {
    return (int64_t)SYSCALL3(SYS64_STAT, path, size_out, is_dir_out);
}

// Milestone 24: asynchronous -- returns the child's pid as soon as it
// has been loaded into its own address space and left runnable; the
// child does not run during this call and is not guaranteed to have
// made any progress by the time this returns (the scheduler picks it up
// on a later tick). Returns -1 on failure (wrong arch, missing file,
// etc.) -- no process is created in that case. Callers that want to run
// something and see its result should always follow this with sys_wait.
// Milestone 11: args is a single raw string the child can retrieve via
// sys_get_args -- may be NULL, meaning "no args" (not real argv[]).
static inline int64_t sys_spawn(const char* path, const char* args) {
    return (int64_t)SYSCALL2(SYS64_SPAWN, path, args);
}

// Milestone 24: genuinely blocks the caller until `pid` -- which must be
// one of this process's own children -- exits, then returns its real
// exit code. Returns -1 if `pid` is not a matching child (including
// after it has already been reaped by an earlier sys_wait call).
static inline int64_t sys_wait(uint32_t pid) {
    return (int64_t)SYSCALL1(SYS64_WAIT, pid);
}

// Milestone 10: minimal stdin path. Non-blocking -- returns -1 if no key
// is currently buffered (kernel/keyboard_buffer64.c, filled by IRQ1).
static inline int64_t sys_getch(void) {
    return (int64_t)SYSCALL0(SYS64_GETCH);
}

// Milestone 11: retrieves the single raw argument string this process
// was spawned with (mirrors the 32-bit tox_get_args model -- ToxenOS has
// never had a real argv[]). Returns the copied length, or -1 on failure
// (no current process, or max_len == 0).
static inline int64_t sys_get_args(char* buf, uint64_t max_len) {
    return (int64_t)SYSCALL2(SYS64_GET_ARGS, buf, max_len);
}

// Milestone 14: clears the VGA text console (the shell's `clear`/`cls`
// builtin) -- no args, no failure case.
static inline void sys_clear(void) {
    SYSCALL0(SYS64_CLEAR);
}

// Milestone 15: sets the active VGA text attribute (fg low nibble, bg
// high nibble) used by every subsequent sys_write call -- mirrors
// 32-bit ToxenOS's stateful set_color(). No failure case.
static inline void sys_set_color(uint8_t color) {
    SYSCALL1(SYS64_SETCOLOR, color);
}

// Milestone 15: retrieves the name of the index'th non-empty entry of
// the directory at path into out (>=256 bytes). Returns 0 on success,
// -1 once index runs past the last entry or path isn't a directory.
static inline int64_t sys_readdir(const char* path, char* out, uint32_t index) {
    return (int64_t)SYSCALL3(SYS64_READDIR, path, out, index);
}

// Milestone 25: brk/mmap/munmap. brk/mmap return raw userspace
// addresses, which live in the canonical high half (bit 63 always set)
// -- callers MUST compare against (uint64_t)-1 for failure, never treat
// the result as signed/negative (a valid address numerically looks
// "negative" if naively cast to int64_t).
//
// sys_brk(0) queries the current break without changing it. Otherwise
// requests the break become exactly new_brk (byte granular); returns
// the resulting break on success, or (uint64_t)-1 if new_brk is out of
// the allowed heap range or physical memory ran out (the heap is left
// exactly as it was on failure).
static inline uint64_t sys_brk(uint64_t new_brk) {
    return SYSCALL1(SYS64_BRK, new_brk);
}

// Allocates a page-rounded anonymous, private, read-write region of at
// least `size` bytes at an address the kernel chooses. Returns that
// address, or (uint64_t)-1 on failure.
static inline uint64_t sys_mmap(uint64_t size) {
    return SYSCALL1(SYS64_MMAP, size);
}

// Unmaps a region previously returned by sys_mmap -- addr/size must
// match a live mapping exactly (no partial unmap). Returns 0 or -1.
// Also the correct call for unmapping a shared-memory mapping returned
// by sys_shm_map -- the kernel tells the two apart internally.
static inline int64_t sys_munmap(uint64_t addr, uint64_t size) {
    return (int64_t)SYSCALL2(SYS64_MUNMAP, addr, size);
}

// Milestone 26: IPC -- pipes and shared memory, referred to by small
// per-process handle numbers (never a raw pointer or global ID). A
// handle created before sys_spawn is automatically inherited by the
// child at the SAME handle number -- see include/handle64.h and
// kernel/process64.c's process64_spawn for the exact inheritance rule.

// Creates a new pipe. On success, *read_h and *write_h are set to this
// process's own handle numbers for the two ends. Returns 0 or -1.
static inline int64_t sys_pipe_create(int* read_h, int* write_h) {
    return (int64_t)SYSCALL2(SYS64_PIPE_CREATE, read_h, write_h);
}

// Reads/writes through a pipe handle (must be the matching end -- a
// write handle passed to sys_handle_read, or vice versa, fails with
// -1). Blocks (a real scheduler block, not a busy-wait) exactly as
// documented on kernel/pipe64.c's pipe64_read/pipe64_write: a read
// blocks while the pipe is empty and a writer remains, returning fewer
// bytes than requested (possibly 0) only once it's genuinely empty AND
// every writer has closed (EOF); a write blocks while the pipe is full
// and a reader remains, returning -1 if no reader is left at all.
static inline int64_t sys_handle_read(int h, char* buf, uint64_t len) {
    return (int64_t)SYSCALL3(SYS64_HANDLE_READ, h, buf, len);
}
static inline int64_t sys_handle_write(int h, const char* buf, uint64_t len) {
    return (int64_t)SYSCALL3(SYS64_HANDLE_WRITE, h, buf, len);
}

// Closes any handle kind (a pipe end or a shared-memory object) --
// releases its reference. Returns 0 or -1 (bad/already-closed handle).
static inline int64_t sys_handle_close(int h) {
    return (int64_t)SYSCALL1(SYS64_HANDLE_CLOSE, h);
}

// Creates a new shared-memory object of at least `size` bytes (rounded
// up to whole pages) and returns a handle to it -- NOT yet mapped
// anywhere. Returns the handle (>= 0), or -1 on failure.
static inline int64_t sys_shm_create(uint64_t size) {
    return (int64_t)SYSCALL1(SYS64_SHM_CREATE, size);
}

// Maps the shared-memory object referenced by `h` into this process's
// own address space at a kernel-chosen address -- writable != 0 for
// read/write, 0 for a read-only mapping (a write to it, by userspace
// OR by the kernel copying into it on this process's behalf, is
// rejected/faults). Returns that address, or (uint64_t)-1 on failure
// -- compare the raw return value against -1 exactly like sys_brk/
// sys_mmap, never treat it as signed.
static inline uint64_t sys_shm_map(int h, int writable) {
    return SYSCALL2(SYS64_SHM_MAP, h, writable);
}

// Milestone 27: reads the NEXT entry of a directory handle returned by
// sys_open() on a directory path, advancing that handle's OWN private
// enumeration cursor (out >= 256 bytes). Returns 0 with `out` filled,
// or -1 once past the last entry. Two opens of the same directory (in
// one process or across processes) never interfere with each other --
// unlike sys_readdir's path+index form, which re-scans from the start
// every call, this remembers where it left off.
static inline int64_t sys_readdir_next(int h, char* out) {
    return (int64_t)SYSCALL2(SYS64_READDIR_NEXT, h, out);
}

// Milestone 19: write/create/delete. Return 0 = success, -1 = generic
// failure, SYS64_ERR_PROTECTED (-2) = path is kernel-protected.
static inline int64_t sys_mkdir(const char* path) {
    return (int64_t)SYSCALL1(SYS64_MKDIR, path);
}
static inline int64_t sys_mkfile(const char* path) {
    return (int64_t)SYSCALL1(SYS64_MKFILE, path);
}
static inline int64_t sys_write_file(const char* path, const char* data, uint64_t len) {
    return (int64_t)SYSCALL3(SYS64_WRITE_FILE, path, data, len);
}
static inline int64_t sys_delete(const char* path) {
    return (int64_t)SYSCALL1(SYS64_DELETE, path);
}
static inline int64_t sys_rename(const char* src, const char* dest) {
    return (int64_t)SYSCALL2(SYS64_RENAME, src, dest);
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
