#ifndef TXFS64_H
#define TXFS64_H

#include <stdint.h>

// TxFS subset for the x86_64 kernel. On-disk structs below are
// duplicated from include/txfs.h (NOT included directly, since txfs.h
// pulls in vfs.h for the fs_driver_t-returning txfs_init(), which this
// kernel doesn't use) -- must stay in sync with include/txfs.h: same
// field order, same types, same packing.
//
// Milestone 27: this file is now a block/inode/directory manipulation
// library keyed by INODE NUMBER, not a self-contained "open files by
// path, read through a cursor" API -- kernel/vfs64.c is the only
// caller, and owns path normalization, the open-file/cursor concept,
// and mounting this backend at "/". The old txfs64_open/read/close and
// their private open_files[] table are gone: cursor/refcount state now
// lives entirely in kernel/vfs64.c's vfs64_file_t, one per open()
// call, in the SAME per-process handle table pipes/shared-memory
// objects use (include/handle64.h) -- there is no longer a second,
// txfs64-specific fd namespace.
//
// Metadata note: txfs64_inode_t's uid/created/modified fields are
// parsed/preserved but not otherwise meaningful yet -- ToxenOS has no
// process credential system and no RTC-backed clock wired into this
// kernel, so every inode created here always has uid=0/created=0/
// modified=0, and nothing anywhere reads or enforces them. They are
// reserved for a future credentials/timestamp milestone, not
// currently interpreted.

#define TXFS64_MAGIC         0x54584653  // "TXFS"
#define TXFS64_BLOCK_SIZE    4096
#define TXFS64_DIRECT_BLOCKS 12
#define TXFS64_BLOCK_SUPER   1
#define TXFS64_BLOCK_IBITMAP 2           // inode allocation bitmap
#define TXFS64_BLOCK_BBITMAP 3           // data-block allocation bitmap
#define TXFS64_BLOCK_INODES  4           // first inode table block
#define TXFS64_BLOCK_DATA    36          // first data block (32 inode table blocks)
#define TXFS64_MAX_INODES    256         // max allocated inodes (bitmap limit)
#define TXFS64_TYPE_FILE     0x1
#define TXFS64_TYPE_DIR      0x2

typedef struct {
    uint32_t magic;
    uint32_t version;
    uint32_t block_size;
    uint32_t total_blocks;
    uint32_t free_blocks;
    uint32_t total_inodes;
    uint32_t free_inodes;
    uint32_t root_inode;
    uint8_t  pad[4096 - 32];
} __attribute__((packed)) txfs64_superblock_t;

typedef struct {
    uint32_t mode;
    uint32_t uid;
    uint64_t size;
    uint32_t created;
    uint32_t modified;
    uint32_t links;
    uint32_t blocks[TXFS64_DIRECT_BLOCKS];
    uint32_t indirect;
    uint32_t dindirect;
    uint32_t tindirect;
    uint8_t  pad[256 - (4+4+8+4+4+4 + TXFS64_DIRECT_BLOCKS*4 + 4+4+4)];
} __attribute__((packed)) txfs64_inode_t;

typedef struct {
    uint32_t inode;
    uint16_t name_len;
    uint8_t  type;
    char     name[256];
} __attribute__((packed)) txfs64_dirent_t;

// 0 on success (valid TXFS64_MAGIC found at the hardcoded LBA-10240
// partition offset), -1 on bad magic.
int txfs64_mount(void);

// Resolves an ALREADY-NORMALIZED absolute path ("/a/b/c" or "/") to an
// inode number plus its cached size/type. Path syntax normalization
// (repeated slashes, "." / "..", overflow checks) is kernel/vfs64.c's
// job, not this file's -- by the time a path reaches here it is
// assumed clean. Returns 0 with *out fields set, or -1 if any
// component doesn't exist or a non-directory appears where a directory
// was expected.
int txfs64_lookup(const char* path, uint32_t* inum_out, uint64_t* size_out, int* is_dir_out);

// Positioned read/write directly by inode number -- no fd/cursor
// concept here, kernel/vfs64.c's vfs64_file_t owns that. Both always
// re-read the inode from disk first (never a stale cached copy), so
// two independently-opened views of the same file always see the
// CURRENT on-disk size, not whatever it was at open time.
//
// txfs64_write_at grows the file (allocating direct blocks, then a
// single indirect block and its pointer table, on demand) if
// offset+len exceeds the current size -- see kernel/txfs64.c's header
// comment for exactly how far growth extends and its rollback
// guarantee on failure. Returns bytes read/written, or -1.
int txfs64_read_at(uint32_t inum, uint64_t offset, uint8_t* buf, uint32_t len);
int txfs64_write_at(uint32_t inum, uint64_t offset, const uint8_t* buf, uint32_t len);

// Directory enumeration by inode number + 0-based index (skips unused
// slots, same semantics as the old path-based txfs64_readdir). Returns
// 0 with `name_out` (>=256 bytes) filled, or -1 once index runs past
// the last live entry.
int txfs64_readdir_at(uint32_t inum, uint32_t index, char* name_out);

// Write/create/delete operations -- all validate their arguments and
// return 0 on success, -1 on failure (except where noted). Paths are
// normalized by kernel/vfs64.c before reaching here, same as lookup.
// txfs64_rmdir refuses non-empty directories (returns -1).
int txfs64_mkdir(const char* path);
int txfs64_create_file(const char* path);
// Replaces a file's entire contents (truncate-then-write, fully
// reclaiming any indirect blocks the old content had) -- does NOT
// create the file; call txfs64_create_file first if needed. Unlike
// Milestone 19, no longer capped at 12 direct blocks: internally this
// is just txfs64_truncate() + txfs64_write_at(inum, 0, data, len), so
// it inherits the same indirect-block growth and rollback behavior.
int txfs64_write_file(const char* path, const uint8_t* data, uint32_t len);
int txfs64_unlink(const char* path);
int txfs64_rmdir(const char* path);
// Rename/move src_path to dest_path on the same volume. Handles same-dir
// (rename-in-place) and cross-dir (relink). Returns 0, -1 (not found /
// bad path), or -4 (dest already exists).
int txfs64_rename(const char* src_path, const char* dest_path);
// Returns 1 if the directory at path has zero live entries (safe to rmdir).
int txfs64_dir_is_empty(const char* path);

// ── Diagnostics ──────────────────────────────────────────────────────
typedef struct {
    uint32_t total_blocks;
    uint32_t free_blocks;
    uint32_t total_inodes;
    uint32_t free_inodes;
} txfs64_stats_t;
void txfs64_stats(txfs64_stats_t* out);
// Logs superblock stats via klog(). For interactive debugging only.
void txfs64_dump(void);

// Runs the Milestone 27 self-test suite: indirect-block write growth
// past the old 12-block/48KB limit, direct->indirect boundary
// crossing, full block reclamation on delete, out-of-space rollback
// leaking no blocks, and repeated create/write/delete cycles with zero
// block/heap drift. Operates directly on inode numbers (no process/VFS
// layer involved) -- see kernel/vfs64.c's own vfs64_selftest for the
// higher-level path/handle/process integration tests. Logs each case
// and a final tally via klog(). Returns 1 if every case passed.
int txfs64_selftest(void);

#endif // TXFS64_H
