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
#define SYS64_INPUT_OPEN 31
#define SYS64_DISPLAY_OPEN 32
#define SYS64_DISPLAY_PRESENT 33
#define DISPLAY64_FORMAT_LOGICAL_XRGB8888 1
#define SYS64_HANDLE_TRY_READ 34
#define SYS64_SHM_TOKEN 35
#define SYS64_SHM_OPEN_TOKEN 36
#define SYS64_SHM_SIZE 37
#define SYS64_SERVICE_LISTEN 38
#define SYS64_SERVICE_ACCEPT 39
#define SYS64_SERVICE_CONNECT 40
#define SYS64_SPAWN_EX 41
#define SYS64_HANDLE_TRY_WRITE 42
#define SYS64_ERR_WOULDBLOCK ((int64_t)-2)
// Return value -2 means the path is protected (kernel refused the op).
#define SYS64_ERR_PROTECTED ((int64_t)-2)
// Return value -3 from SYS64_DELETE means the folder is not empty.
#define SYS64_ERR_NOTEMPTY  ((int64_t)-3)
// Return value -4 from SYS64_RENAME means the destination already exists.
#define SYS64_ERR_EXISTS    ((int64_t)-4)

// ABI: rax = syscall number, rdi/rsi/rdx = up to 3 args (see
// kernel/syscall64.c). Return value comes back in rax.
//
// Milestone 33 bug fix: every one of these is missing a "memory"
// clobber. Several syscalls write through a POINTER argument (e.g.
// SYS64_STAT's size_out) -- from the compiler's point of view, an
// inline asm block with no "memory" clobber is free to assume it does
// not touch memory at all beyond its declared outputs, meaning a value
// the CALLER already had in a register or cached from an earlier load
// (like a local variable initialized right before the call) can be
// reused AFTER the call instead of being reloaded from the memory the
// syscall actually wrote into. This was always wrong, but never
// visibly broke anything until userlib/toxui (the first userspace code
// in this project built above the Makefile-wide default -O0, see
// UFLAGS64_TOXUI) actually got bitten by it: at -O0 GCC reloads
// variables from memory conservatively often enough that the missing
// clobber's absence went unnoticed; at -O2 it does not. Found via
// tox_image.c's read_whole_file() reading back a stale `size = 0`
// after a successful sys_stat that had genuinely written 135 into that
// same stack slot moments earlier. Adding "memory" makes every syscall
// a full compiler-level memory barrier -- correct and conservative,
// exactly what a real syscall (which can read/write arbitrary user
// memory through its pointer arguments) should always be treated as.
#define SYSCALL0(n) ({ \
    uint64_t _r; __asm__ volatile("int $0x80" : "=a"(_r) : "a"((uint64_t)(n)) : "memory"); _r; })

#define SYSCALL1(n, a) ({ \
    uint64_t _r; __asm__ volatile("int $0x80" : "=a"(_r) : "a"((uint64_t)(n)), "D"((uint64_t)(a)) : "memory"); _r; })

#define SYSCALL2(n, a, b) ({ \
    uint64_t _r; __asm__ volatile("int $0x80" : "=a"(_r) : "a"((uint64_t)(n)), "D"((uint64_t)(a)), "S"((uint64_t)(b)) : "memory"); _r; })

#define SYSCALL3(n, a, b, c) ({ \
    uint64_t _r; __asm__ volatile("int $0x80" : "=a"(_r) : "a"((uint64_t)(n)), "D"((uint64_t)(a)), "S"((uint64_t)(b)), "d"((uint64_t)(c)) : "memory"); _r; })

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

// Milestone 32.1: non-blocking counterpart to sys_handle_write, for a
// pipe write handle (a file handle just behaves like sys_handle_write).
// Deliberately all-or-nothing -- either the whole write is buffered
// immediately and `len` is returned, or NOTHING is written and
// SYS64_ERR_WOULDBLOCK is returned. Returns -1 for a genuinely broken
// pipe (no reader left). Needed by any process that must never risk
// blocking on a write to one handle while it still has other handles
// to service (see user64/compositor64.c's per-client event delivery).
static inline int64_t sys_handle_try_write(int h, const char* buf, uint64_t len) {
    return (int64_t)SYSCALL3(SYS64_HANDLE_TRY_WRITE, h, buf, len);
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
// Milestone 29: structured input events -- mirrors include/input64.h's
// input64_event_t layout exactly (kept in sync by convention, same as
// every other struct duplicated in this file). See that header for the
// full field/type-code documentation.
#define INPUT64_EVENT_KEY            1
#define INPUT64_EVENT_POINTER_MOVE   2
#define INPUT64_EVENT_POINTER_BUTTON 3
#define INPUT64_EVENT_POINTER_WHEEL  4

#define INPUT64_MOD_SHIFT    0x01u
#define INPUT64_MOD_CTRL     0x02u
#define INPUT64_MOD_ALT      0x04u
#define INPUT64_MOD_CAPSLOCK 0x08u

#define INPUT64_BTN_LEFT   0x01u
#define INPUT64_BTN_RIGHT  0x02u
#define INPUT64_BTN_MIDDLE 0x04u

#define INPUT64_KEY_EXTENDED   0x100u
#define INPUT64_KEY_UP         (INPUT64_KEY_EXTENDED | 0x48u)
#define INPUT64_KEY_DOWN       (INPUT64_KEY_EXTENDED | 0x50u)
#define INPUT64_KEY_LEFT       (INPUT64_KEY_EXTENDED | 0x4Bu)
#define INPUT64_KEY_RIGHT      (INPUT64_KEY_EXTENDED | 0x4Du)
#define INPUT64_KEY_ENTER      0x1Cu
#define INPUT64_KEY_BACKSPACE  0x0Eu
#define INPUT64_KEY_ESCAPE     0x01u

typedef struct {
    uint32_t type;
    uint32_t seq;
    int32_t  a;
    int32_t  b;
    uint32_t pressed;
    uint32_t modifiers;
    uint32_t ascii;
    uint32_t buttons;
} input64_event_t;

// Opens the global structured-input-event stream. Fails (-1) if it's
// already owned by another process -- see include/syscall64.h's header
// comment on SYS64_INPUT_OPEN for the single-consumer rationale.
static inline int64_t sys_input_open(void) {
    return (int64_t)SYSCALL0(SYS64_INPUT_OPEN);
}

// Milestone 29: userspace display-present interface. The compositor
// (Milestone 30) is the intended sole caller -- see
// include/syscall64.h's header comment on SYS64_DISPLAY_OPEN/PRESENT.
typedef struct {
    uint64_t buf_ptr;
    uint32_t pitch;
    uint32_t x, y, w, h;
} display64_present_req_t;

// Opens the physical display. Fails (-1) if no display is available or
// it's already owned by another process. format_out may be NULL.
static inline int64_t sys_display_open(uint32_t* width_out, uint32_t* height_out, uint32_t* format_out) {
    return (int64_t)SYSCALL3(SYS64_DISPLAY_OPEN, width_out, height_out, format_out);
}

// Presents one rectangle of logical XRGB8888 pixels from the caller's
// own memory (private or shared-memory-backed) to the physical display
// in a single bulk kernel-side blit. `handle` must be a display handle
// this process owns (see sys_display_open). Returns 0 or -1.
static inline int64_t sys_display_present(int handle, const display64_present_req_t* req) {
    return (int64_t)SYSCALL2(SYS64_DISPLAY_PRESENT, handle, req);
}

// Milestone 30: non-blocking counterpart to sys_handle_read, for a pipe
// or input handle (a file handle just behaves like sys_handle_read).
// Returns bytes read (>=0, 0=EOF), -1 on error, or SYS64_ERR_WOULDBLOCK
// if nothing is available right now -- needed because ToxenOS has no
// select()/poll() equivalent, so a process multiplexing several handles
// (e.g. a compositor's input handle plus one pipe per client) must poll
// each non-blockingly in turn.
static inline int64_t sys_handle_try_read(int h, char* buf, uint64_t len) {
    return (int64_t)SYSCALL3(SYS64_HANDLE_TRY_READ, h, buf, len);
}

// Milestone 30: cross-process shared-memory handoff by opaque token --
// see include/shm64.h's header comment on shm64_t::token. Never exposes
// a pointer or physical address.
static inline int64_t sys_shm_token(int h) {
    return (int64_t)SYSCALL1(SYS64_SHM_TOKEN, h);
}
static inline int64_t sys_shm_open_token(uint64_t token) {
    return (int64_t)SYSCALL1(SYS64_SHM_OPEN_TOKEN, token);
}
// Actual mapped byte size (whole pages) of a SHM handle -- required to
// validate a claimed width/height/stride against reality before a
// compositor touches a client's surface. Returns -1 on failure.
static inline int64_t sys_shm_size(int h) {
    return (int64_t)SYSCALL1(SYS64_SHM_SIZE, h);
}

// Milestone 32: generic named local-service registry -- see
// include/syscall64.h's header comments on SYS64_SERVICE_LISTEN/
// ACCEPT/CONNECT for the full design. `send`/`recv` are always from the
// CALLER's own point of view (write end / read end respectively), so
// the same struct serves both sys_service_connect's result (a new
// client's own ends) and sys_service_accept's (a server's own ends for
// one client).
typedef struct {
    int send;
    int recv;
} service64_endpoints_t;

// Publishes `name` (1..31 printable, non-space ASCII bytes) as a new
// service owned by this process. Fails (-1) if that name is already
// published or on allocation failure. The returned handle is a
// single-owner resource -- closing it (explicitly, or automatically on
// process exit/fault) unpublishes the name immediately, so a
// replacement process can register the exact same name right after.
static inline int64_t sys_service_listen(const char* name) {
    return (int64_t)SYSCALL1(SYS64_SERVICE_LISTEN, name);
}

// Dequeues the oldest not-yet-accepted connection on a service this
// process owns. Never blocks: returns 0 with `out` filled,
// SYS64_ERR_WOULDBLOCK if nothing is pending, or -1 (bad handle, or a
// connection IS pending but this process has fewer than two free
// handle slots -- left queued for a later retry).
static inline int64_t sys_service_accept(int listen_h, service64_endpoints_t* out) {
    return (int64_t)SYSCALL2(SYS64_SERVICE_ACCEPT, listen_h, out);
}

// Connects to `name`. `blocking` == 0: fails immediately (-1) if no
// such service is currently published -- a clean, instant "service not
// available" result. `blocking` != 0: genuinely blocks (a real
// scheduler block, never polling) until SOME service is published,
// rechecking whether it's the wanted name, for as long as necessary --
// see include/syscall64.h's header comment on SYS64_SERVICE_CONNECT
// for why this is the preferred way for an application to find an
// already-or-not-yet-running server with zero retry logic of its own.
static inline int64_t sys_service_connect(const char* name, int blocking, service64_endpoints_t* out) {
    return (int64_t)SYSCALL3(SYS64_SERVICE_CONNECT, name, blocking, out);
}

// Milestone 32: explicit-inheritance spawn -- the new default-safe
// alternative to sys_spawn (which copies the caller's ENTIRE handle
// table into the child, kept unchanged for existing callers that rely
// on it). `inherit_ptr`/`inherit_count` name exactly which of THIS
// process's own handle numbers the child should receive, each landing
// at the SAME slot number in the child; an out-of-range or already-
// unused number is silently skipped, not an error. Pass
// inherit_count == 0 (inherit_ptr may then be NULL) for a child that
// starts with a completely empty handle table -- the normal case for
// launching an unrelated sibling program (a compositor, a shell, a
// graphical client) that should never see the launcher's own open
// files/pipes/shared-memory/service connections.
typedef struct {
    uint64_t args_ptr;
    uint64_t inherit_ptr;
    uint32_t inherit_count;
} spawn_ex_req_t;

static inline int64_t sys_spawn_ex(const char* path, const spawn_ex_req_t* req) {
    return (int64_t)SYSCALL2(SYS64_SPAWN_EX, path, req);
}

// Composed userland helper (not a 1:1 syscall wrapper): sys_spawn_ex
// with an empty inherit list -- the common case for launching a fully
// independent sibling process. `args` may be NULL, same meaning as
// sys_spawn's.
static inline int64_t sys_spawn_isolated(const char* path, const char* args) {
    spawn_ex_req_t req;
    req.args_ptr = (uint64_t)(uintptr_t)args;
    req.inherit_ptr = 0;
    req.inherit_count = 0;
    return sys_spawn_ex(path, &req);
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
