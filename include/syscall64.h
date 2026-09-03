#ifndef SYSCALL64_H
#define SYSCALL64_H

#include "isr64.h"

// Milestone 5: minimal x86_64 syscall layer via int 0x80. ABI: rax =
// syscall number, rdi/rsi/rdx = up to 3 args -- already sitting in the
// first three System V C argument registers, so syscall64_dispatch can
// read them straight off the saved trapframe64_t with no shuffling.
// Return value goes back into tf->rax, restored into the real rax by
// the time ring3 resumes.
#define SYS64_WRITE  1   // (const char* buf, uint64_t len) -> bytes written
#define SYS64_EXIT   2   // (int code) -> never returns
#define SYS64_GETPID 3   // () -> real pid, or -1 if no current process

// Milestone 9: first userland file/process API. All pointer args are
// user pointers, validated by kernel/usercopy64.c before use -- a bad
// pointer fails the syscall (-1), it never reaches the kernel raw.
//
// Milestone 27: SYS64_OPEN now returns a handle from the SAME unified
// per-process handle table pipes/shared-memory use (include/handle64.h),
// not a separate small fds[] namespace -- works for both files
// (HANDLE64_FILE) and directories (HANDLE64_DIR). SYS64_READ (5) and
// SYS64_CLOSE (6) are RETIRED as distinct syscall numbers: reading/
// writing/closing an open file now goes through the generic
// SYS64_HANDLE_READ/WRITE/CLOSE below (which dispatch on handle kind),
// exactly like a pipe end. Both numbers are reserved -- never reused
// for something else, to keep old disassembly/notes unambiguous.
#define SYS64_OPEN   4   // (const char* path) -> handle, or -1
// #define SYS64_READ  5  -- retired, see SYS64_HANDLE_READ
// #define SYS64_CLOSE 6  -- retired, see SYS64_HANDLE_CLOSE
#define SYS64_STAT   7   // (const char* path, uint64_t* size_out, int* is_dir_out [nullable]) -> 0 or -1
#define SYS64_SPAWN  8   // (const char* path, const char* args [nullable]) -> child pid or -1
#define SYS64_WAIT   9   // (uint32_t pid)                          -> child exit code or -1

// Milestone 10: minimal stdin path. No pointer args, so no
// usercopy64 validation is needed here -- the only thing crossing the
// boundary is a single integer return value.
#define SYS64_GETCH  10  // () -> next buffered ASCII char (0-255), or -1 if none available yet

// Milestone 11: single raw argument string (mirrors the 32-bit
// tox_get_args model -- no real argv[] anywhere in ToxenOS, 32-bit or
// 64-bit). A process retrieves whatever sys_spawn passed it.
#define SYS64_GET_ARGS 11 // (char* buf, uint64_t max_len) -> length copied, or -1

// Milestone 14: lets userland (the shell's `clear`/`cls` builtin) clear
// the VGA text console -- no pointer args, just forwards to
// vgaterm64_clear().
#define SYS64_CLEAR 12 // () -> always 0

// Milestone 15: colored terminal + directory listing.
#define SYS64_SETCOLOR 13 // (uint8_t color) -> always 0
#define SYS64_READDIR  14 // (const char* path, char* out [>=256 bytes], uint32_t index) -> 0 or -1

// Milestone 19: write/create/delete filesystem operations. All paths
// are user pointers validated before use. Protected system paths return
// -2 (caller should print a "path is protected" error). -1 = generic
// failure (path not found, dir not empty, disk full, etc.).
#define SYS64_MKDIR      15 // (const char* path) -> 0, -1, or -2
#define SYS64_MKFILE     16 // (const char* path) -> 0, -1, or -2
#define SYS64_WRITE_FILE 17 // (const char* path, const char* data, uint64_t len) -> 0, -1, or -2
#define SYS64_DELETE     18 // (const char* path) -> 0, -1, -2, or -3
#define SYS64_RMDIR      19 // (const char* path) -> 0, -1, or -2

// Milestone 20: rename/move. Both src and dest are validated; -4 if dest exists.
#define SYS64_RENAME     20 // (const char* src, const char* dest) -> 0, -1, -2, or -4

// Milestone 25: brk/mmap/munmap (include/uservm64.h). brk/mmap return
// USERSPACE ADDRESSES, which live in the canonical high half (bit 63
// always set) -- callers MUST compare the raw uint64_t return value
// against (uint64_t)-1 for failure, never treat it as a signed/negative
// value. munmap has no address-shaped return value, so it stays a
// normal 0/-1 result like every other syscall.
#define SYS64_BRK    21 // (uint64_t new_brk; 0 = query) -> new break, or (uint64_t)-1
#define SYS64_MMAP   22 // (uint64_t size)               -> mapped address, or (uint64_t)-1
#define SYS64_MUNMAP 23 // (uint64_t addr, uint64_t size) -> 0 or -1

// Milestone 26: IPC -- pipes and shared memory. Both are referred to by
// a small per-process HANDLE number (include/handle64.h), never a raw
// pipe/shm pointer or a global object ID -- see kernel/process64.c's
// handle table. Handles are inherited (whole-table, same slot numbers)
// by every child a process spawns AFTER creating them; there is no
// other way to move a handle between processes this milestone.
//
// SYS64_PIPE_CREATE writes both ends out via pointers (same pattern as
// SYS64_STAT's size_out/type_out) rather than packing two small ints
// into one rax, since a pipe genuinely has two independent handles.
#define SYS64_PIPE_CREATE  24 // (int* read_h_out, int* write_h_out)      -> 0 or -1
// Milestone 27: the generic read/write path for anything handle-shaped
// -- dispatches on the handle's kind. PIPE_READ/PIPE_WRITE: semantics
// match kernel/pipe64.c's pipe64_read/pipe64_write exactly (blocks the
// calling process -- a real scheduler block, not polling -- as needed,
// returns fewer bytes than requested only at true EOF, returns -1 for
// a broken pipe with no peer left). HANDLE64_FILE: a POSITIONED read/
// write at the open file's own cursor (advanced by however many bytes
// actually transferred), growing the file on write past its current
// end exactly like SYS64_WRITE_FILE, via kernel/vfs64.c/txfs64.c. Any
// other handle kind (SHM, DIR, or a mismatched pipe end) fails (-1).
#define SYS64_HANDLE_READ  25 // (int handle, char* buf, uint64_t len)    -> bytes read, 0=EOF, -1=error
#define SYS64_HANDLE_WRITE 26 // (int handle, const char* buf, uint64_t len) -> bytes written, -1=error/broken pipe
// Closes ANY handle kind (pipe end, shared-memory, open file, or open
// directory) -- releases its reference, same cleanup process exit
// performs automatically for every handle still open at that point.
#define SYS64_HANDLE_CLOSE 27 // (int handle) -> 0 or -1
// Creates a new shared-memory object of at least `size` bytes
// (rounded up to whole pages), returning a SHM handle -- NOT yet
// mapped anywhere (see SYS64_SHM_MAP). -1 on failure (bad size, or no
// single contiguous physical run big enough).
#define SYS64_SHM_CREATE   28 // (uint64_t size) -> handle, or (uint64_t)-1
// Maps the object referenced by `handle` (must be a SHM handle) into
// the caller's OWN address space, kernel-choosing the address --
// writable != 0 for read/write, 0 for read-only (enforced by the
// hardware page tables: a read-only mapping's pages genuinely cannot
// be written by userspace OR by the kernel copying into them via
// copy_to_user64, see kernel/paging64.c's paging64_check_user_range).
// Unmapping a shared-memory region uses the EXISTING SYS64_MUNMAP --
// kernel/uservm64.c tells shared and anonymous regions apart
// internally, so no separate "shm unmap" syscall is needed.
#define SYS64_SHM_MAP      29 // (int handle, int writable) -> mapped address, or (uint64_t)-1

// Milestone 27: reads the NEXT entry of an open directory handle
// (SYS64_OPEN on a directory path), advancing that handle's OWN
// private enumeration cursor -- two opens of the same directory (in
// one process or across processes) never interfere with each other.
// name_out must point at a user buffer of at least 256 bytes. Returns
// 0 with name_out filled, or -1 once past the last entry or `handle`
// isn't an open directory. The older path+index SYS64_READDIR (14)
// keeps working unchanged for existing callers (ls.c) -- both go
// through kernel/vfs64.c underneath.
#define SYS64_READDIR_NEXT 30 // (int handle, char* name_out) -> 0 or -1

// Milestone 29: structured input events and the userspace display-
// present interface. Both new handle kinds (HANDLE64_INPUT,
// HANDLE64_DISPLAY -- include/handle64.h) are process-wide SINGLETONS:
// only one process may hold each open at a time (see kernel/input64.c's/
// kernel/display64.c's acquire/release), and neither is inherited
// across sys_spawn -- a deliberate access-control stand-in until
// ToxenOS has real credentials, shaped around the eventual model where
// one compositor/input-server process owns both and fans out to
// applications via IPC (not implemented this milestone).
//
// SYS64_INPUT_OPEN returns a HANDLE64_INPUT handle, or -1 if the input
// stream is already owned by another process. Reading it goes through
// the EXISTING generic SYS64_HANDLE_READ: `len` must be at least
// sizeof(input64_event_t) (see include/input64.h) -- as many whole
// events as fit are copied out (never a partial event); the call BLOCKS
// (a real scheduler block, never polling) while the queue is empty.
// Closing it (SYS64_HANDLE_CLOSE) releases ownership, same as process
// exit.
#define SYS64_INPUT_OPEN 31 // () -> handle, or -1

// SYS64_DISPLAY_OPEN returns a HANDLE64_DISPLAY handle plus the
// display's actual geometry and logical pixel format (currently always
// DISPLAY64_FORMAT_LOGICAL_XRGB8888 -- see below), or -1 if no display
// is available or it's already owned by another process.
#define SYS64_DISPLAY_OPEN 32 // (uint32_t* width_out, uint32_t* height_out, uint32_t* format_out) -> handle, or -1
#define DISPLAY64_FORMAT_LOGICAL_XRGB8888 1

// SYS64_DISPLAY_PRESENT copies a rectangle of caller-supplied pixels
// (logical 0x00RRGGBB, DISPLAY64_FORMAT_LOGICAL_XRGB8888) from the
// caller's own memory -- ordinary private or shared-memory-backed
// userspace memory, validated through the SAME paging64_check_user_range
// machinery as copy_from_user64/copy_to_user64 (Milestone 25), so a
// virtually-contiguous buffer backed by non-contiguous physical pages
// works correctly with no special-casing -- into the physical
// framebuffer, converting to the real hardware pixel layout inside the
// kernel (kernel/display64.c). One call performs the entire rectangle's
// blit (never one syscall per pixel); passing the full display
// dimensions presents a whole frame, a smaller w/h presents only a
// damaged sub-rectangle. `handle` must be a HANDLE64_DISPLAY this
// process owns. The request struct is packed into a single user
// pointer since it has more fields than the 3-register syscall ABI
// allows in separate arguments:
//   typedef struct {
//       uint64_t buf_ptr;  // user pointer, logical XRGB8888, row-major
//       uint32_t pitch;    // bytes per row in buf_ptr (0 = tightly packed, w*4)
//       uint32_t x, y;     // destination rect top-left on the display
//       uint32_t w, h;     // destination rect size
//   } display64_present_req_t;
// Returns 0, or -1 (bad handle/pointer, or the rect doesn't fit the
// display).
#define SYS64_DISPLAY_PRESENT 33 // (int handle, const display64_present_req_t* req) -> 0 or -1

// Milestone 30: non-blocking counterpart to SYS64_HANDLE_READ, for a
// HANDLE64_PIPE_READ or HANDLE64_INPUT handle only (HANDLE64_FILE reads
// never block anyway, so it behaves identically to SYS64_HANDLE_READ
// there). Needed because ToxenOS has no select()/poll() equivalent and
// no threads -- a userspace compositor multiplexing its input handle
// against one pipe per connected client has no other way to wait on
// "whichever is ready first" than polling each non-blockingly. Returns
// bytes read (>=0, 0 = EOF), (uint64_t)-1 on error/bad handle, or
// (uint64_t)-2 if nothing is available right now (would-block).
#define SYS64_HANDLE_TRY_READ 34 // (int handle, char* buf, uint64_t len) -> bytes, 0=EOF, -1=error, -2=would-block
#define SYS64_ERR_WOULDBLOCK ((uint64_t)-2)

// Milestone 30: cross-process shared-memory handoff by opaque token
// (include/shm64.h's header comment on shm64_t::token has the full
// rationale -- handle numbers are per-process and spawn-inheritance-only,
// which cannot get a client's window-surface handle to an unrelated,
// already-running compositor process). SYS64_SHM_TOKEN queries the
// stable token for a SHM handle the caller already owns (for sending to
// another process over a pipe); SYS64_SHM_OPEN_TOKEN looks up a live
// object by that token and creates a NEW handle to it in the CALLING
// process's own table (bumping refcount). Neither ever exposes a
// pointer or physical address -- the token is an opaque monotonic
// counter value with no meaning outside these two calls.
#define SYS64_SHM_TOKEN      35 // (int handle) -> uint64_t token, or (uint64_t)-1
#define SYS64_SHM_OPEN_TOKEN 36 // (uint64_t token) -> handle, or -1

// Milestone 30: a process that opened a shm object by token (i.e. did
// NOT create it) has no other way to learn its actual byte size --
// necessary so a compositor can bound its OWN reads of a client-claimed
// surface (width/height/stride in the wm protocol are just numbers the
// client sent; the compositor must clamp against the object's real
// mapped size before touching it, or a malicious/buggy client could
// crash the compositor with an out-of-bounds read into its own
// unmapped address space).
#define SYS64_SHM_SIZE 37 // (int handle) -> uint64_t byte size (whole pages), or (uint64_t)-1

// Milestone 32: generic named local-service registry (include/service64.h)
// -- replaces the Milestone 30 compositor-specific bootstrap (spawn-arg-
// encoded inherited pipe handles). The kernel has no idea what a
// "compositor" or "window" is; a service is just a short textual name
// one process publishes and others discover and connect to, with the
// kernel creating a fresh bidirectional pipe pair per connection.
//
// All three take/return a service64_endpoints_t { int send; int recv; }
// (see include/service64.h) packed through a single user pointer, same
// struct-packing precedent as SYS64_DISPLAY_PRESENT's request struct --
// "send"/"recv" are always from the CALLER's own point of view, so the
// exact same struct shape serves both SYS64_SERVICE_CONNECT's result
// (the new client's ends) and SYS64_SERVICE_ACCEPT's (the server's ends
// for that one client).
//
// Name validation (kernel/service64.c's service64_name_valid): 1 to
// SERVICE64_NAME_MAX-1 (31) bytes, every byte printable non-space ASCII
// (0x21-0x7E). The name is copied into kernel-owned storage immediately
// (copy_user_cstr64, which independently rejects an unterminated
// string, a bad/unmapped pointer, or anything past the buffer) -- no
// user pointer is ever retained past the syscall returning.
//
// SYS64_SERVICE_LISTEN publishes `name` as a new service owned by the
// calling process. Fails (-1) if that name is already published (the
// SECOND registration fails, not the first) or on allocation failure.
// The returned handle (HANDLE64_SERVICE_LISTEN) is a single-owner
// resource, like HANDLE64_INPUT/DISPLAY -- never inherited by ANY
// spawn path. Closing it (explicitly, or automatically on process
// exit/fault) unpublishes the name immediately, so a replacement
// process can register the exact same name right afterward -- a stale
// registration can never permanently block a restart.
#define SYS64_SERVICE_LISTEN 38 // (const char* name) -> handle, or -1

// SYS64_SERVICE_ACCEPT dequeues the oldest not-yet-accepted connection
// on a service this process owns (`handle` must be a
// HANDLE64_SERVICE_LISTEN this process holds). Never blocks: returns 0
// with `out` filled, SYS64_ERR_WOULDBLOCK if nothing is pending right
// now, or -1 (bad handle, or a connection IS pending but this process
// has fewer than two free handle slots -- the pending connection is
// left queued for a later retry, not dropped). Deliberately
// non-blocking rather than a blocking accept: a real server process
// (a compositor) already has other things to multiplex every loop
// iteration (its input handle, every existing client's pipe) via the
// EXISTING SYS64_HANDLE_TRY_READ non-blocking-poll pattern -- this
// fits that exact same style with zero new synchronization model.
#define SYS64_SERVICE_ACCEPT 39 // (int handle, service64_endpoints_t* out) -> 0, -1, or SYS64_ERR_WOULDBLOCK

// SYS64_SERVICE_CONNECT attempts to connect to `name`. `blocking` == 0:
// fails immediately (-1) if no such service is currently published --
// a clean, instant "service not available" result, safe to call as a
// one-shot probe. `blocking` != 0: if no such service exists yet, the
// caller genuinely BLOCKS (a real scheduler block via
// kernel/process64.c's process64_block_on -- never polling) until SOME
// service is published, rechecking whether it's the wanted name each
// time, exactly like every other blocking primitive in this kernel
// (kernel/pipe64.c's pipe64_read is the precedent). This is what lets
// an application call a single "connect to the compositor" library
// call that works correctly no matter what order it and the compositor
// happen to be scheduled/spawned in, with no retry loop and no wasted
// CPU cycles -- see the Milestone 32 summary's discussion of why this
// was chosen over a giant delay loop. A blocking connect to a service
// that will genuinely never exist (e.g. calling it in text-only mode
// with no compositor ever started) blocks forever, same tradeoff as
// any other indefinite kernel block in this codebase -- callers that
// need a bounded probe should pass `blocking` == 0 instead.
#define SYS64_SERVICE_CONNECT 40 // (const char* name, uint64_t blocking, service64_endpoints_t* out) -> 0 or -1

// Milestone 32: explicit-inheritance spawn -- see include/process64.h's
// header comment on process64_spawn_ex for the full rationale. Unlike
// SYS64_SPAWN (which copies the ENTIRE parent handle table into the
// child, unchanged, kept forever for backward compatibility with
// existing test/demo binaries that rely on it), SYS64_SPAWN_EX's child
// starts with an EMPTY handle table except for whatever slots the
// caller explicitly lists -- the new default-safe primitive graphical/
// system userspace (init64, shell64) uses so that launching one program
// can never unintentionally leak references to the launcher's own open
// files, pipes, shared memory, or service connections.
//
// The request struct (packed through one pointer, same precedent as
// SYS64_DISPLAY_PRESENT) is:
//   typedef struct {
//       uint64_t args_ptr;      // nullable, same meaning as SYS64_SPAWN's args
//       uint64_t inherit_ptr;   // nullable iff inherit_count == 0: array of int32_t parent handle numbers
//       uint32_t inherit_count; // 0..PROCESS64_MAX_HANDLES
//   } spawn_ex_req64_t;
// An out-of-range or already-unused parent handle number in the list is
// silently skipped (not an error) -- see process64_spawn_ex.
#define SYS64_SPAWN_EX 41 // (const char* path, const spawn_ex_req64_t* req) -> child pid, or -1

// Milestone 32.1: non-blocking counterpart to SYS64_HANDLE_WRITE, for a
// HANDLE64_PIPE_WRITE handle only (HANDLE64_FILE writes never block
// anyway, so it behaves identically to SYS64_HANDLE_WRITE there).
// Mirrors SYS64_HANDLE_TRY_READ's contract exactly, just for the write
// side -- added specifically so a process that both reads and writes
// several handles in one non-blocking poll loop (a compositor
// delivering an event to a client, most notably) never has to guess
// whether a write would fit before risking a blocking one. Deliberately
// ALL-OR-NOTHING: either the whole write is buffered immediately and
// `len` is returned, or NOTHING is written and SYS64_ERR_WOULDBLOCK is
// returned -- a partial write would desync fixed-size message framing
// the same way a short read would. Returns -1 for a genuinely broken
// pipe (no reader left at all), matching SYS64_HANDLE_WRITE.
#define SYS64_HANDLE_TRY_WRITE 42 // (int handle, const char* buf, uint64_t len) -> bytes, -1=error/broken pipe, or SYS64_ERR_WOULDBLOCK

void syscall64_dispatch(trapframe64_t* tf);

#endif // SYSCALL64_H
