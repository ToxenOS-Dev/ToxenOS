#ifndef HANDLE64_H
#define HANDLE64_H

// Milestone 26: per-process kernel-object handle table for IPC objects
// (pipes, shared memory). Deliberately separate from the existing
// fds[] array (kernel/txfs64.c file descriptors, include/process64.h) --
// fds[] stores a raw txfs64 integer per slot with no notion of "kind"
// or a reference-counted kernel object behind it, so it cannot cleanly
// represent a pipe end or a shared-memory object (audited before
// starting this milestone; see the Milestone 26 summary). A handle is
// a small per-process integer -- an index into this table -- that
// userspace passes back to the kernel. It is never a raw kernel
// pointer and never a global object ID: two different processes'
// handle 2 can (and usually do) refer to completely different
// objects, and a process can never address another process's object
// by guessing a number, only by inheriting a handle across spawn (see
// kernel/process64.c's process64_spawn) or by both ends independently
// holding a reference to the same kmalloc'd object.
typedef enum {
    HANDLE64_UNUSED = 0,
    HANDLE64_PIPE_READ,
    HANDLE64_PIPE_WRITE,
    HANDLE64_SHM,
} handle64_kind_t;

typedef struct {
    handle64_kind_t kind;
    void* obj; // pipe64_t* (PIPE_READ/PIPE_WRITE) or shm64_t* (SHM); NULL if UNUSED
} handle64_t;

#define PROCESS64_MAX_HANDLES 8

#endif // HANDLE64_H
