// kernel/txfs64.c — Milestone 4: read-only TxFS access for the 64-bit
// kernel. Logic mirrors kernel/txfs.c's read paths (txfs_read_inode,
// txfs_get_block with alloc=0, txfs_dir_lookup, txfs_lookup,
// txfs_read_fn) line-for-line, renamed and pointed at ata64_read
// instead of ata_read. Deliberately excludes everything write/create
// related (txfs_alloc_block/inode, bitmap helpers, txfs_write_fn,
// mkdir/remove/chmod) and the uid/permission-check block inside
// txfs_open_fn -- no allocation ever happens on a read-only path, and
// there's no process/user concept in the 64-bit kernel yet to check
// permissions against.
#include <stdint.h>
#include "../include/txfs64.h"
#include "../include/ata64.h"

#define TXFS64_PTRS_PER_BLOCK (TXFS64_BLOCK_SIZE / 4)
#define TXFS64_LBA_OFFSET     10240u  // GPT layout TxFS partition start

static void txfs64_strcpy(char* dst, const char* src, int max) {
    int i = 0;
    while (src[i] && i < max - 1) { dst[i] = src[i]; i++; }
    dst[i] = 0;
}

static int txfs64_strcmp(const char* a, const char* b) {
    int i;
    for (i = 0; a[i] && b[i]; i++)
        if (a[i] != b[i]) return 1;
    return a[i] != b[i];
}

static uint8_t block_buf[TXFS64_BLOCK_SIZE];
static txfs64_superblock_t sb;

static int txfs64_read_block(uint32_t block, uint8_t* buf) {
    uint32_t sectors = TXFS64_BLOCK_SIZE / 512;
    return ata64_read(TXFS64_LBA_OFFSET + block * sectors, buf, sectors);
}

static int txfs64_read_super(void) {
    if (txfs64_read_block(TXFS64_BLOCK_SUPER, (uint8_t*)&sb) < 0) return -1;
    if (sb.magic != TXFS64_MAGIC) return -1;
    return 0;
}

#define INODES_PER_BLOCK 16  // 4096 / 256

static int txfs64_read_inode(uint32_t num, txfs64_inode_t* inode) {
    uint32_t block  = TXFS64_BLOCK_INODES + (num / INODES_PER_BLOCK);
    uint32_t offset = (num % INODES_PER_BLOCK) * sizeof(txfs64_inode_t);

    if (txfs64_read_block(block, block_buf) < 0) return -1;

    uint8_t* src = block_buf + offset;
    uint8_t* dst = (uint8_t*)inode;
    for (uint32_t i = 0; i < sizeof(txfs64_inode_t); i++) dst[i] = src[i];
    return 0;
}

// Read-only: returns 0 (no such block) instead of allocating one --
// mirrors kernel/txfs.c's txfs_get_block with alloc always false.
static uint32_t txfs64_get_block(txfs64_inode_t* inode, uint32_t idx) {
    if (idx < TXFS64_DIRECT_BLOCKS) return inode->blocks[idx];

    uint32_t after_direct = idx - TXFS64_DIRECT_BLOCKS;
    if (after_direct < TXFS64_PTRS_PER_BLOCK) {
        if (!inode->indirect) return 0;
        uint32_t ptrs[TXFS64_PTRS_PER_BLOCK];
        txfs64_read_block(inode->indirect, (uint8_t*)ptrs);
        return ptrs[after_direct];
    }

    uint32_t after_single = after_direct - TXFS64_PTRS_PER_BLOCK;
    if (after_single < TXFS64_PTRS_PER_BLOCK * TXFS64_PTRS_PER_BLOCK) {
        uint32_t l1 = after_single / TXFS64_PTRS_PER_BLOCK;
        uint32_t l2 = after_single % TXFS64_PTRS_PER_BLOCK;
        if (!inode->dindirect) return 0;
        uint32_t l1p[TXFS64_PTRS_PER_BLOCK];
        txfs64_read_block(inode->dindirect, (uint8_t*)l1p);
        if (!l1p[l1]) return 0;
        uint32_t l2p[TXFS64_PTRS_PER_BLOCK];
        txfs64_read_block(l1p[l1], (uint8_t*)l2p);
        return l2p[l2];
    }

    uint32_t after_double = after_single - TXFS64_PTRS_PER_BLOCK * TXFS64_PTRS_PER_BLOCK;
    uint32_t triple_span = TXFS64_PTRS_PER_BLOCK * TXFS64_PTRS_PER_BLOCK * TXFS64_PTRS_PER_BLOCK;
    if (after_double >= (uint32_t)triple_span) return 0;

    uint32_t t1 = after_double / (TXFS64_PTRS_PER_BLOCK * TXFS64_PTRS_PER_BLOCK);
    uint32_t t2 = (after_double / TXFS64_PTRS_PER_BLOCK) % TXFS64_PTRS_PER_BLOCK;
    uint32_t t3 = after_double % TXFS64_PTRS_PER_BLOCK;

    if (!inode->tindirect) return 0;
    uint32_t p1[TXFS64_PTRS_PER_BLOCK]; txfs64_read_block(inode->tindirect, (uint8_t*)p1);
    if (!p1[t1]) return 0;
    uint32_t p2[TXFS64_PTRS_PER_BLOCK]; txfs64_read_block(p1[t1], (uint8_t*)p2);
    if (!p2[t2]) return 0;
    uint32_t p3[TXFS64_PTRS_PER_BLOCK]; txfs64_read_block(p2[t2], (uint8_t*)p3);
    return p3[t3];
}

static int txfs64_dir_lookup(txfs64_inode_t* dir, const char* name) {
    uint8_t  data_buf[TXFS64_BLOCK_SIZE];
    uint32_t entries_total = (uint32_t)(dir->size / sizeof(txfs64_dirent_t));
    uint32_t checked = 0;

    for (uint32_t b = 0; checked < entries_total; b++) {
        uint32_t blk = txfs64_get_block(dir, b);
        if (!blk) break;
        txfs64_read_block(blk, data_buf);

        uint32_t per_block = TXFS64_BLOCK_SIZE / sizeof(txfs64_dirent_t);
        uint32_t in_this    = entries_total - checked;
        if (in_this > per_block) in_this = per_block;

        for (uint32_t i = 0; i < in_this; i++) {
            txfs64_dirent_t* de = (txfs64_dirent_t*)(data_buf + i * sizeof(txfs64_dirent_t));
            if (de->inode && !txfs64_strcmp(de->name, name))
                return (int)de->inode;
        }
        checked += in_this;
    }
    return -1;
}

// Resolve a full path ("/a/b/c") to an inode number, or -1.
static int txfs64_resolve_path(const char* path) {
    if (!txfs64_strcmp(path, "/")) return 0;

    const char* p = path;
    if (*p == '/') p++;

    int cur = 0;
    while (*p) {
        char component[256];
        int  len = 0;
        while (p[len] && p[len] != '/') len++;
        for (int i = 0; i < len; i++) component[i] = p[i];
        component[len] = 0;
        p += len;
        if (*p == '/') p++;

        txfs64_inode_t inode;
        if (txfs64_read_inode((uint32_t)cur, &inode) < 0) return -1;

        int type = (inode.mode >> 12) & 0xF;
        if (type != TXFS64_TYPE_DIR) return -1;

        cur = txfs64_dir_lookup(&inode, component);
        if (cur < 0) return -1;
    }
    return cur;
}

#define TXFS64_MAX_FDS 8

typedef struct {
    int            used;
    uint32_t       inode_num;
    txfs64_inode_t inode;
    uint32_t       position;
} txfs64_fd_t;

static txfs64_fd_t open_files[TXFS64_MAX_FDS];

int txfs64_mount(void) {
    for (int i = 0; i < TXFS64_MAX_FDS; i++) open_files[i].used = 0;
    return txfs64_read_super();
}

int txfs64_open(const char* path) {
    int inode_num = txfs64_resolve_path(path);
    if (inode_num < 0) return -1;

    for (int i = 0; i < TXFS64_MAX_FDS; i++) {
        if (!open_files[i].used) {
            open_files[i].used      = 1;
            open_files[i].inode_num = (uint32_t)inode_num;
            open_files[i].position  = 0;
            if (txfs64_read_inode((uint32_t)inode_num, &open_files[i].inode) < 0) {
                open_files[i].used = 0;
                return -1;
            }
            return i;
        }
    }
    return -1;
}

int txfs64_read(int fd, uint8_t* buf, uint32_t size) {
    if (fd < 0 || fd >= TXFS64_MAX_FDS || !open_files[fd].used) return -1;

    txfs64_fd_t*    f     = &open_files[fd];
    txfs64_inode_t* inode = &f->inode;

    if (f->position >= inode->size) return 0;

    uint32_t to_read = size;
    if (f->position + to_read > inode->size)
        to_read = (uint32_t)(inode->size - f->position);

    uint32_t done = 0;
    uint8_t  data_buf[TXFS64_BLOCK_SIZE];

    while (done < to_read) {
        uint32_t block_idx    = (f->position + done) / TXFS64_BLOCK_SIZE;
        uint32_t block_offset = (f->position + done) % TXFS64_BLOCK_SIZE;

        uint32_t blk = txfs64_get_block(inode, block_idx);
        if (!blk) break;

        txfs64_read_block(blk, data_buf);

        uint32_t can_read = TXFS64_BLOCK_SIZE - block_offset;
        if (can_read > to_read - done) can_read = to_read - done;

        for (uint32_t i = 0; i < can_read; i++)
            buf[done + i] = data_buf[block_offset + i];

        done += can_read;
    }

    f->position += done;
    return (int)done;
}

int txfs64_close(int fd) {
    if (fd < 0 || fd >= TXFS64_MAX_FDS || !open_files[fd].used) return -1;
    open_files[fd].used = 0;
    return 0;
}

int txfs64_readdir(const char* path, char* out, uint32_t index) {
    int inode_num = txfs64_resolve_path(path);
    if (inode_num < 0) return -1;

    txfs64_inode_t inode;
    if (txfs64_read_inode((uint32_t)inode_num, &inode) < 0) return -1;
    if (((inode.mode >> 12) & 0xF) != TXFS64_TYPE_DIR) return -1;

    uint8_t  data_buf[TXFS64_BLOCK_SIZE];
    uint32_t entries_total = (uint32_t)(inode.size / sizeof(txfs64_dirent_t));
    uint32_t count = 0, checked = 0;

    for (uint32_t b = 0; checked < entries_total; b++) {
        uint32_t blk = txfs64_get_block(&inode, b);
        if (!blk) break;
        txfs64_read_block(blk, data_buf);

        uint32_t per_block = TXFS64_BLOCK_SIZE / sizeof(txfs64_dirent_t);
        uint32_t in_this    = entries_total - checked;
        if (in_this > per_block) in_this = per_block;

        for (uint32_t i = 0; i < in_this; i++) {
            txfs64_dirent_t* de = (txfs64_dirent_t*)(data_buf + i * sizeof(txfs64_dirent_t));
            if (de->inode) {
                if (count == index) { txfs64_strcpy(out, de->name, 256); return 0; }
                count++;
            }
        }
        checked += in_this;
    }
    return -1;
}

int txfs64_stat_type(const char* path, uint64_t* size_out, int* is_dir_out) {
    int inode_num = txfs64_resolve_path(path);
    if (inode_num < 0) return -1;

    txfs64_inode_t inode;
    if (txfs64_read_inode((uint32_t)inode_num, &inode) < 0) return -1;

    *size_out = inode.size;
    if (is_dir_out) *is_dir_out = (((inode.mode >> 12) & 0xF) == TXFS64_TYPE_DIR) ? 1 : 0;
    return 0;
}

int txfs64_stat(const char* path, uint64_t* size_out) {
    return txfs64_stat_type(path, size_out, 0);
}
