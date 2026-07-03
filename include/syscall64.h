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
#define SYS64_DELETE     18 // (const char* path) -> 0, -1, or -2
#define SYS64_RMDIR      19 // (const char* path) -> 0, -1, or -2

void syscall64_dispatch(trapframe64_t* tf);

#endif // SYSCALL64_H
