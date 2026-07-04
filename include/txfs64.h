#ifndef TXFS64_H
#define TXFS64_H

#include <stdint.h>

// Read-only TxFS subset for the x86_64 kernel — Milestone 4 (filesystem
// access). On-disk structs below are duplicated from include/txfs.h
// (NOT included directly, since txfs.h pulls in vfs.h for the
// fs_driver_t-returning txfs_init(), which this milestone doesn't use)
// -- must stay in sync with include/txfs.h: same field order, same
// types, same packing.

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

// Returns an fd >= 0 on success, -1 if the path doesn't resolve to an
// existing file. No O_CREAT, no permission checks (no process/user
// concept exists in the 64-bit kernel yet) -- read-only, single-context.
int txfs64_open(const char* path);
int txfs64_read(int fd, uint8_t* buf, uint32_t size);
int txfs64_close(int fd);

// Lists the index'th non-empty entry of the directory at path into
// out (>=256 bytes). Returns 0 on success, -1 if path/index is invalid.
int txfs64_readdir(const char* path, char* out, uint32_t index);
int txfs64_stat(const char* path, uint64_t* size_out);

// Milestone 11: same lookup as txfs64_stat, plus the entry's type.
int txfs64_stat_type(const char* path, uint64_t* size_out, int* is_dir_out);

// Milestone 19: write/create/delete operations. All validate their
// arguments and return 0 on success, -1 on failure. txfs64_write_file
// replaces the entire contents of an existing file; it does NOT create
// the file (call txfs64_create_file first if needed). File data is
// limited to TXFS64_DIRECT_BLOCKS * TXFS64_BLOCK_SIZE = 48KB per file
// for this milestone (indirect block allocation not yet implemented).
// txfs64_rmdir refuses non-empty directories (returns -1).
int txfs64_mkdir(const char* path);
int txfs64_create_file(const char* path);
int txfs64_write_file(const char* path, const uint8_t* data, uint32_t len);
int txfs64_unlink(const char* path);
int txfs64_rmdir(const char* path);
// Rename/move src_path to dest_path on the same volume. Handles same-dir
// (rename-in-place) and cross-dir (relink). Returns 0, -1 (not found /
// bad path), or -4 (dest already exists).
int txfs64_rename(const char* src_path, const char* dest_path);
// Returns 1 if the directory at path has zero live entries (safe to rmdir).
int txfs64_dir_is_empty(const char* path);

#endif // TXFS64_H
