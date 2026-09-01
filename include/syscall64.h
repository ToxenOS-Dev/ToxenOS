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
#define SYS64_OPEN   4   // (const char* path)                      -> per-process fd (0..3) or -1
#define SYS64_READ   5   // (int fd, char* buf, uint64_t len)       -> bytes read, 0=EOF, -1=error
#define SYS64_CLOSE  6   // (int fd)                                -> 0 or -1
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
// SYS64_HANDLE_READ/WRITE work on a PIPE_READ/PIPE_WRITE handle only
// (wrong kind -> -1). Semantics match kernel/pipe64.c's pipe64_read/
// pipe64_write exactly: blocks the calling process (real scheduler
// block, not polling) as needed, returns fewer bytes than requested
// only at true EOF, returns -1 for a broken pipe (no peer left).
#define SYS64_HANDLE_READ  25 // (int handle, char* buf, uint64_t len)    -> bytes read, 0=EOF, -1=error
#define SYS64_HANDLE_WRITE 26 // (int handle, const char* buf, uint64_t len) -> bytes written, -1=error/broken pipe
// Closes ANY handle kind (pipe end or shared-memory) -- releases its
// reference, same cleanup process exit performs automatically for
// every handle still open at that point.
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

void syscall64_dispatch(trapframe64_t* tf);

#endif // SYSCALL64_H
