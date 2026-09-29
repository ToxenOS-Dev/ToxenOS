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

// M-next (compositor performance hardening, §17 of the GPU/display
// architecture proposal): the smallest possible userspace time source
// for software frame pacing -- exposes the existing 100Hz PIT tick
// counter (kernel/timer64.c's ticks64, already driving the scheduler)
// read-only, with no new blocking primitive, no new handle kind, and
// no change to how the timer itself works. Deliberately NOT a sleep/
// yield syscall -- the compositor still busy-polls exactly as
// documented in its own file header; this only lets it CHEAPLY check
// "has enough real time passed to render a frame yet" instead of
// rendering on every dirty event immediately.
#define SYS64_GET_TICKS 43 // () -> uint64_t PIT ticks since boot (100Hz, i.e. 10ms/tick) -- never fails

// M+1B: generic GPU buffer syscalls -- literal ports of the SYS64_SHM_*
// create/token/open_token/size quartet (include/shm64.h's own header
// comment on shm64_t::token has the full cross-process handoff
// rationale; it applies identically here). `device_idx` selects a
// registered gpu64_device_t the same way Revision 2 originally proposed
// (include/gpu64.h's gpu64_by_index()) -- index 0 is whichever device
// registered first, exactly like blockdev64's own "no fixed slot"
// registry. No SYS64_GPU_BUFFER_MAP exists yet -- see include/gpu64.h's
// header comment for why this milestone doesn't need one.
#define SYS64_GPU_BUFFER_CREATE     44 // (uint32_t device_idx, uint64_t size, uint32_t usage) -> handle, or (uint64_t)-1
#define SYS64_GPU_BUFFER_TOKEN      45 // (int handle) -> uint64_t token, or (uint64_t)-1
#define SYS64_GPU_BUFFER_OPEN_TOKEN 46 // (uint64_t token) -> handle, or -1
#define SYS64_GPU_BUFFER_SIZE       47 // (int handle) -> uint64_t byte size (whole pages), or (uint64_t)-1

// M+4 investigation: a real, genuinely-blocking sleep (kernel/process64.c's
// process64_sleep_ticks) -- the CALLER stops running and does not
// consume any CPU for at least `ticks` PIT ticks (kernel/timer64.c,
// 100Hz/10ms each), unlike a busy-wait loop. Added specifically because
// this codebase had no way for a process to pace itself without
// spinning (see user64/gfx_demo64.c's own before/M+4-after and the M+4
// investigation report on how an always-runnable busy-looping process
// starves every OTHER process, including the compositor, under this
// kernel's plain round-robin scheduler -- regardless of which display
// presentation backend is active). `ticks == 0` still yields for at
// least one reschedule point (wakes on the very next tick) rather than
// being a true no-op. Never fails.
#define SYS64_SLEEP_TICKS 48 // (uint64_t ticks) -> 0, never fails

// M+7: generic hardware-cursor control, gated on ownership of a
// HANDLE64_DISPLAY handle (same access-control precedent as
// SYS64_DISPLAY_PRESENT -- only the process that opened the display,
// i.e. the compositor, may control the hardware cursor). Deliberately
// generic: no VirtIO-specific concept (a command type, a virtqueue, a
// resource ID) ever crosses this boundary -- see include/display64.h's
// own header comment on display64_cursor_set_image/move/set_visible,
// which these four calls are thin, validated wrappers over.
//
// SYS64_CURSOR_AVAILABLE: whether a hardware cursor backend is
// currently registered (kernel/virtio_gpu64.c's virtio_gpu64_init_cursor()
// succeeded this boot) -- 0 if not (no such device, no cursor
// virtqueue, or any setup step failed). A compositor should check this
// once at startup and fall back to its own software cursor path if it
// comes back 0; never fatal either way.
#define SYS64_CURSOR_AVAILABLE 49 // (int handle) -> 1 or 0

// SYS64_CURSOR_SET_IMAGE uploads new cursor pixel data and establishes
// it as the displayed cursor image (with a hotspot) -- called once at
// startup and again only if the image/hotspot ever changes, never on
// ordinary pointer movement. `width`/`height` must currently equal
// exactly the hardware's one persistent cursor resource size (64x64 as
// of this milestone -- see include/virtio_gpu64.h's own
// VIRTIO_GPU64_CURSOR_DIM); any other size is rejected (-1), which is
// how a caller discovers the required size is not what it assumed
// without the kernel needing a separate query call. The request struct
// is packed through one pointer, same precedent as
// SYS64_DISPLAY_PRESENT's own request struct:
//   typedef struct {
//       uint64_t pixels_ptr; // user pointer, tightly packed 0xAARRGGBB pixels, width*height*4 bytes, row-major
//       uint32_t width, height;
//       uint32_t hot_x, hot_y;
//   } cursor64_set_image_req_t;
// Returns 0, or -1 (bad handle/pointer, wrong size, no cursor backend
// registered, or the underlying device call failed).
#define SYS64_CURSOR_SET_IMAGE 50 // (int handle, const cursor64_set_image_req_t* req) -> 0 or -1

// SYS64_CURSOR_MOVE is the cheap, common-case operation -- repositions
// the already-uploaded cursor image, with no resource re-upload
// anywhere on this path. `x`/`y` are logical screen coordinates
// (identical coordinate space to SYS64_DISPLAY_PRESENT's own rect
// origin) -- a negative value is clamped to 0 by the driver rather than
// rejected, since it's cheaper and safer than forcing every caller to
// pre-clamp. Returns 0, or -1 (bad handle, or no cursor backend
// registered).
#define SYS64_CURSOR_MOVE 51 // (int handle, int32_t x, int32_t y) -> 0 or -1

// SYS64_CURSOR_SET_VISIBLE shows/hides the hardware cursor without
// touching its uploaded image or remembered position -- e.g. a
// compositor falling back to software cursor after a runtime failure
// calls this with 0 first, so the hardware cursor plane doesn't keep
// showing a stale position while software cursor drawing resumes.
// Returns 0, or -1 (bad handle, or no cursor backend registered).
#define SYS64_CURSOR_SET_VISIBLE 52 // (int handle, uint32_t visible) -> 0 or -1

// M+7A: SYS64_INPUT_GET_ABS_RANGE lets a consumer (compositor64) learn
// the currently-active absolute pointer device's own device-space
// range ONCE at startup, so it can normalize INPUT64_EVENT_POINTER_ABS's
// raw (a,b) into its own logical display coordinates without any driver
// ever hardcoding a display resolution -- see include/input64.h's own
// input64_get_abs_range() comment. `req` must point at:
//   typedef struct {
//       int32_t min_x, max_x, min_y, max_y;
//   } input64_abs_range_req_t;
// Returns 0 with *req filled, or -1 (bad handle/pointer, or no absolute
// pointer device is currently active -- e.g. plain PS/2-only boot).
#define SYS64_INPUT_GET_ABS_RANGE 53 // (int handle, input64_abs_range_req_t* req) -> 0 or -1

// M+9B: direct-scanout / composition-bypass syscalls -- same
// single-owner-display access control every other SYS64_DISPLAY_*/
// SYS64_CURSOR_* call already enforces (`handle` must be this
// process's own open HANDLE64_DISPLAY). See include/display64.h's own
// header comment for the full per-slot state machine and cache model
// these five thin wrappers expose; compositor64 is the only intended
// caller. A small, fixed-size cache of bound resources lives in
// display64.c itself (system-wide, not per-process), keyed by each
// client buffer's own shm_token -- see DISPLAY64_DIRECT_MAX_SLOTS --
// sized for one double-buffered fullscreen client's own buffer pair,
// matching this milestone's eligibility rule that exactly one
// fullscreen client may ever be the ACTIVE scanout source.
//
// SYS64_DISPLAY_DIRECT_QUERY: is a direct-scanout backend registered
// at all (capability, not current state)? A framebuffer-only boot (no
// VirtIO-GPU) always reports 0 here -- compositor64 must treat that as
// DIRECT_SCANOUT_UNSUPPORTED and never attempt to bind.
#define SYS64_DISPLAY_DIRECT_QUERY 54 // (int handle) -> 1 or 0

// SYS64_DISPLAY_DIRECT_BIND: resolves `req->shm_token` (a client's own
// already-attached committed buffer's token -- the SAME token
// compositor64 already validated via WM_MSG_ATTACH_BUFFER) to its
// memobj64 backing and binds/caches it as a direct-scanout resource
// sized req->width x req->height, keyed by that same token. A second
// bind of a token already cached (matching size) is a cheap no-op
// success reusing the existing resource -- see include/display64.h's
// own cache comment for why (GfxDemo-style clients double-buffer even
// while fullscreen; this is what avoids RESOURCE_CREATE_2D/
// ATTACH_BACKING/DESTROY every single frame). Does NOT make it the
// active scanout yet -- see SYS64_DISPLAY_DIRECT_PRESENT's own
// switch_active flag. Struct-by-pointer, same reason
// SYS64_DISPLAY_PRESENT's own request is (this codebase's syscall ABI
// tops out at 3 register arguments -- see user64/tox64.h's
// SYSCALL0..3 -- so anything needing more fields goes through
// copy_from_user64() instead of a 4th/5th/6th register):
//   typedef struct {
//       uint64_t shm_token;
//       uint32_t width, height;
//   } display64_direct_bind_req_t;
// Returns 0, or -1 (bad handle/pointer, unsupported backend, the
// token is new AND the small cache is already full, bad token, or the
// backend's own resource-create/attach-backing failed -- nothing left
// bound on any failure).
#define SYS64_DISPLAY_DIRECT_BIND 55 // (int handle, const display64_direct_bind_req_t* req) -> 0 or -1

// SYS64_DISPLAY_DIRECT_PRESENT: TRANSFER_TO_HOST_2D the damaged rect
// of the resource cached under `req->shm_token`, then (only if
// req->switch_active != 0) SET_SCANOUT to make THAT resource the
// active one, then RESOURCE_FLUSH -- exact ordering per this
// milestone's own Linux virtio_gpu_primary_plane_update reference
// audit. Struct-by-pointer, same reason as SYS64_DISPLAY_DIRECT_BIND
// above:
//   typedef struct {
//       uint64_t shm_token;
//       uint32_t x, y, w, h;
//       uint32_t switch_active;
//   } display64_direct_present_req_t;
// Returns 0 only if (x,y,w,h) is now genuinely part of the visible
// output (same honest-SUCCESS contract SYS64_DISPLAY_PRESENT already
// established for M+8) -- -1 on ANY failure (including `shm_token` not
// currently bound), in which case whatever was previously the active
// source remains authoritative (never a faked success).
#define SYS64_DISPLAY_DIRECT_PRESENT 56 // (int handle, const display64_direct_present_req_t* req) -> 0 or -1

// SYS64_DISPLAY_DIRECT_LEAVE: switches scanout back to the normal
// compositor resource and flushes it -- the only sanctioned way out of
// the ACTIVE state (whichever cached slot currently holds it). The
// slot itself stays bound/cached afterward, exactly like the OTHER
// (non-active) slot always does -- see SYS64_DISPLAY_DIRECT_UNBIND for
// the separate, explicit teardown step. compositor64 must have already
// recomposited and run its own normal SYS64_DISPLAY_PRESENT call before
// this (see
// include/display64.h's own §11-ordering comment) so the compositor
// resource's content is already fresh by the time scanout switches
// back to it. Returns 0, or -1 (not currently ACTIVE, or the
// switch-back/flush itself failed -- the old direct buffer stays
// ACTIVE and alive on failure, never torn down).
#define SYS64_DISPLAY_DIRECT_LEAVE 57 // (int handle) -> 0 or -1

// SYS64_DISPLAY_DIRECT_UNBIND: releases the slot cached under
// `shm_token` -- its GPU resource and memobj64 reference. Refused
// (-1, safe no-op) if that slot is currently the ACTIVE scanout source
// -- callers must SYS64_DISPLAY_DIRECT_LEAVE first. Safe to call for a
// token that isn't cached at all (no-op, returns 0).
#define SYS64_DISPLAY_DIRECT_UNBIND 58 // (int handle, uint64_t shm_token) -> 0 or -1

// M+10A: read-only debug snapshot of display64's M+10 atomic state --
// added specifically for the compositor's own liveness dump (spec §5:
// "display64 (atomic state validity, current primary source, VALID/
// RECOVERY_REQUIRED)"). Deliberately NOT the full display64_state_t
// (cursor/output fields, backend_handle, damage rect) -- only the three
// fields a liveness dump actually needs, so this stays a narrow
// diagnostic addition rather than a second public state-marshaling API.
// Same access-control gate as every other SYS64_DISPLAY_* call.
#define SYS64_DISPLAY_DEBUG_STATE 59 // (int handle, display64_debug_state_req_t* out) -> 0 or -1

// M+12B: bounded multi-handle wait -- the one primitive ToxenOS was
// missing to let a process (the compositor, principally) block on
// "whichever of several handles becomes ready first" instead of
// busy-polling every one of them every iteration (SYS64_HANDLE_TRY_READ/
// SYS64_HANDLE_TRY_WRITE/SYS64_SERVICE_ACCEPT's own non-blocking
// contracts are exactly what made that polling possible, but never gave
// a way to SLEEP between polls). Supported handle kinds are exactly the
// four that already have a kernel-side wait-channel concept:
// HANDLE64_PIPE_READ, HANDLE64_PIPE_WRITE, HANDLE64_INPUT,
// HANDLE64_SERVICE_LISTEN -- any other kind (or an out-of-range/UNUSED
// handle) anywhere in the list fails the WHOLE call with -1, no partial
// registration. `count` must be 1..PROCESS64_WAIT_ANY_MAX
// (== PROCESS64_MAX_HANDLES, include/process64.h) -- a process can
// never usefully name more handles than its own table holds. Duplicate
// handles/channels in the list are harmless, just redundant.
//
// `timeout_ticks`: 0 = poll once, never block (mirrors
// SYS64_HANDLE_TRY_READ's non-blocking contract exactly); UINT64_MAX
// (SYS64_WAIT_FOREVER) = block with no deadline; anything else = a real
// deadline `now + timeout_ticks`.
//
// Returns 0 if something MAY now be ready (immediately, or after a real
// block/wake) -- the caller must re-poll every handle it cares about
// with the ordinary non-blocking calls to find out what, exactly, since
// this call deliberately reports no more than "something changed, go
// look" (see this milestone's own design notes on why a ready-handle
// bitmask is deferred, not a free addition later). Returns
// SYS64_ERR_TIMEOUT if the deadline passed with nothing ready, or -1 for
// any invalid argument (bad pointer, count out of range, an unsupported
// or invalid handle anywhere in the list).
#define SYS64_HANDLE_WAIT_ANY 60 // (const wait_any_req64_t* req) -> 0, -1, or SYS64_ERR_TIMEOUT
#define SYS64_ERR_TIMEOUT ((uint64_t)-3)
#define SYS64_WAIT_FOREVER ((uint64_t)-1)

typedef struct {
    const int* handles;      // user pointer to `count` handle numbers
    uint32_t   count;
    uint64_t   timeout_ticks;
} wait_any_req64_t;

void syscall64_dispatch(trapframe64_t* tf);

#endif // SYSCALL64_H
