#ifndef VFS64_H
#define VFS64_H

#include <stdint.h>

// Milestone 27: a small VFS layer between kernel/syscall64.c and the
// actual filesystem implementation (kernel/txfs64.c today). Nothing in
// syscall64.c calls txfs64_* directly anymore -- everything goes
// through vfs64_* here, which:
//
//   - normalizes/validates path STRINGS (repeated slashes, "."/"..",
//     overflow) before any backend ever sees them -- backend lookup()
//     implementations only ever need to walk clean, absolute paths
//     against their own directory format;
//   - owns the open-file OBJECT (vfs64_file_t: a node + a cursor + a
//     refcount) that kernel/process64.c's per-process handle table
//     (include/handle64.h, HANDLE64_FILE/HANDLE64_DIR) points at --
//     the underlying filesystem backend has no concept of "open" at
//     all, only nodes reachable by path;
//   - dispatches to whichever backend is mounted at (a prefix of) the
//     requested path via a small ops-table interface, so a second
//     filesystem (FAT, ext2, devfs, ...) can be mounted alongside
//     TxFS64 later without touching syscall64.c or this file's public
//     API, only by registering another vfs64_backend_t.
//
// ToxenOS only ever has one mounted filesystem today (TxFS64 at "/"),
// so the "mount table" below is deliberately tiny -- but it IS a real
// table, scanned by longest-prefix match, not a single hardcoded
// global backend pointer, specifically so adding a second mount later
// is additive.

#define VFS64_PATH_MAX 256
#define VFS64_NAME_MAX 256
#define VFS64_MAX_MOUNTS 4

// A resolved filesystem object: which backend owns it, that backend's
// own identifier for it (TxFS64: an inode number), and a snapshot of
// its size/type from the moment it was resolved. Cheap to copy by
// value. NOT a handle/capability by itself -- see vfs64_file_t for the
// thing that actually gets kept open and referenced from a handle.
typedef struct vfs64_backend_s vfs64_backend_t;

typedef struct {
    const vfs64_backend_t* backend;
    uint32_t inum;
    uint64_t size;
    int      is_dir;
} vfs64_node_t;

// Backend operations, all given an ALREADY-NORMALIZED absolute path (or
// an inode number from a prior lookup on the SAME backend). A backend
// that doesn't support an operation at all (there are none today, but
// a future read-only or device backend might) may leave the function
// pointer NULL -- vfs64.c treats that the same as "the operation
// failed" (-1), never a crash.
struct vfs64_backend_s {
    const char* name; // diagnostics only, e.g. "txfs64"
    int (*lookup)(const char* path, uint32_t* inum_out, uint64_t* size_out, int* is_dir_out);
    int (*read_at)(uint32_t inum, uint64_t offset, uint8_t* buf, uint32_t len);
    int (*write_at)(uint32_t inum, uint64_t offset, const uint8_t* buf, uint32_t len);
    int (*readdir_at)(uint32_t inum, uint32_t index, char* name_out);
    int (*create_file)(const char* path);
    int (*mkdir)(const char* path);
    int (*unlink)(const char* path);   // -2 if target is a directory (mirrors the existing syscall ABI)
    int (*rmdir)(const char* path);
    int (*rename)(const char* src, const char* dest); // -4 if dest exists
    int (*write_file)(const char* path, const uint8_t* data, uint32_t len); // whole-file replace convenience
};

// Registers `ops` as the backend for everything under `mount_point`
// ("/" for the root). Returns 0, or -1 if the mount table is full.
int vfs64_mount(const char* mount_point, const vfs64_backend_t* ops);

// Convenience used once by kernel/kernel64.c at boot: mounts the
// TxFS64 backend at "/" and calls txfs64_mount() to validate the
// on-disk superblock. Returns 0, or -1 (bad magic / mount table full).
int vfs64_init_root_txfs(void);

// Normalizes `in` into `out`: collapses repeated '/', resolves "."
// (dropped) and ".." (pops the previous component, clamped at root --
// ".." above root is a no-op, not an error, matching the "cd .." at
// top level convention user64/toxpath64.h already uses in userspace),
// and ensures the result is absolute (a missing leading '/' is treated
// as present, matching this kernel's long-standing behavior of having
// no relative-path/cwd concept below the syscall boundary). Returns 0
// with `out` filled (always starts with '/'), or -1 if `in` or any
// single component would overflow VFS64_PATH_MAX/VFS64_NAME_MAX.
int vfs64_normalize_path(const char* in, char* out, int out_max);

// Normalizes `path`, finds its mount, and asks that backend to resolve
// it. Returns 0 with `out` filled, or -1.
int vfs64_lookup(const char* path, vfs64_node_t* out);

int vfs64_read(const vfs64_node_t* node, uint64_t offset, uint8_t* buf, uint32_t len);
// May grow the underlying file -- updates node->size in place on
// success so a caller holding this vfs64_node_t (e.g. inside a
// vfs64_file_t) sees the new size immediately without a fresh lookup.
int vfs64_write(vfs64_node_t* node, uint64_t offset, const uint8_t* buf, uint32_t len);

int vfs64_readdir(const vfs64_node_t* node, uint32_t index, char* name_out);
// Path-based convenience (normalizes + looks up + reads one entry) --
// what the legacy path+index SYS64_READDIR syscall uses, kept for
// compatibility alongside the newer handle-based enumeration.
int vfs64_readdir_path(const char* path, uint32_t index, char* name_out);

int vfs64_create_file(const char* path);
int vfs64_mkdir(const char* path);
int vfs64_unlink(const char* path);
int vfs64_rmdir(const char* path);
int vfs64_rename(const char* src, const char* dest);
int vfs64_write_file(const char* path, const uint8_t* data, uint32_t len);

// ── Open-file objects ────────────────────────────────────────────────
// The thing a HANDLE64_FILE/HANDLE64_DIR handle actually points at.
// Each vfs64_open() call creates an INDEPENDENT instance (its own
// cursor, starting at 0) even for the same path opened twice, exactly
// like a fresh file description in Unix -- refcount is used ONLY for
// spawn-inheritance sharing (a parent's and a child's handle-table
// entries pointing at the SAME vfs64_file_t after inheriting, exactly
// like kernel/pipe64.c's/kernel/shm64.c's own reference model), never
// for "multiple independent opens of the same path share state".
typedef struct vfs64_file_s {
    vfs64_node_t node;
    uint64_t     cursor;  // byte offset for files; enumeration index for directories
    uint32_t     refcount;
    struct vfs64_file_s* dbg_next; // intrusive list of every live open-file object, diagnostics only
} vfs64_file_t;

// Resolves `path` and kmalloc's a fresh vfs64_file_t (cursor=0,
// refcount=1). Works for both files and directories -- callers
// distinguish via (*out)->node.is_dir. Returns 0 with *out set, or -1
// (bad path, or out of memory).
int vfs64_open(const char* path, vfs64_file_t** out);
void vfs64_file_add_ref(vfs64_file_t* f);
// -1 reference; kfree's the object once it reaches zero. Never touches
// the underlying filesystem node -- closing a file does not delete it.
void vfs64_file_release(vfs64_file_t* f);

// ── Diagnostics ──────────────────────────────────────────────────────
// Logs every registered mount and every live open-file object
// (backend, inode, cursor, refcount, file/dir) via klog(). Development
// use only, same precedent as pipe64_dump()/shm64_dump(). Never exposes
// a raw pointer -- object identity is shown as an opaque sequence
// number, not an address.
void vfs64_dump(void);

// Runs the Milestone 27 VFS-level self-test suite: path normalization,
// open/read/write/close through the unified handle table, directory
// enumeration through both the path-based and handle-based APIs,
// spawn inheritance of file handles, and mixed file+pipe+shared-memory
// handles in one process. Spawns real NEX64 test binaries where a real
// process/scheduler is genuinely needed. Logs each case and a final
// tally via klog(). Returns 1 if every case passed. Must be run from
// the kernel/idle context, same as the other Milestone 22-26 selftests.
int vfs64_selftest(void);

#endif // VFS64_H
