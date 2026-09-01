#ifndef HANDLE64_H
#define HANDLE64_H

// Milestone 26: per-process kernel-object handle table for IPC objects
// (pipes, shared memory). Milestone 27: also the home for open files
// and directories, retiring the separate txfs64-specific fds[] array
// that used to live on process64_t -- see kernel/vfs64.c and the
// Milestone 27 summary for why one process should not have two
// unrelated integer namespaces for "things it has open". A handle is
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
    HANDLE64_FILE,  // Milestone 27: obj = vfs64_file_t* (regular file)
    HANDLE64_DIR,   // Milestone 27: obj = vfs64_file_t* (directory, cursor == enumeration index)
    // Milestone 29: obj is unused (0) for both of these -- each is a
    // singleton global resource (see kernel/input64.c's/kernel/display64.c's
    // single-owner acquire/release), not a per-open kmalloc'd object like
    // a pipe or file. Deliberately NOT copied by process64_spawn's
    // handle-inheritance switch (kernel/process64.c) -- a child of the
    // process holding one of these does NOT automatically get raw
    // display/input access, matching the "compositor is the sole owner,
    // dispatches to apps via IPC" architecture this milestone sets up
    // for but does not itself implement.
    HANDLE64_INPUT,   // the global structured-input-event stream
    HANDLE64_DISPLAY, // the physical display's present operation
} handle64_kind_t;

typedef struct {
    handle64_kind_t kind;
    void* obj; // pipe64_t* / shm64_t* / vfs64_file_t*, by kind; NULL if UNUSED
} handle64_t;

// Milestone 27: bumped from 8 now that files/directories share this one
// table too (previously fds[] gave files their own separate 4 slots on
// top of this table's 8) -- gives comfortable headroom for realistic
// combined usage (a few open files/dirs plus inherited pipe/shm
// handles) without artificially constraining tests that exercise mixed
// handle kinds at once.
#define PROCESS64_MAX_HANDLES 16

#endif // HANDLE64_H
