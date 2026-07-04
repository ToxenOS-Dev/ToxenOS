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

// ── Milestone 19: write-side implementation ───────────────────────────────
// Ported from tools/txfs_write.c's host-side logic, adapted to use the
// kernel's ata64 read/write API instead of fseek/fread/fwrite.
// Block/inode allocation follows the host tool exactly: the bitmap is in
// block TXFS64_BLOCK_BBITMAP (=3) for data blocks and TXFS64_BLOCK_IBITMAP
// (=2) for inodes; data blocks are numbered starting at TXFS64_BLOCK_DATA
// (=36) (absolute physical block number = logical_bit + TXFS64_BLOCK_DATA).

// Keep a static zero block for newly-allocated block initialization --
// BSS-initialized to all zeroes, never written by userland.
static uint8_t txfs64_zero_block[TXFS64_BLOCK_SIZE];

static int txfs64_write_block(uint32_t block, const uint8_t* buf) {
    uint32_t sectors = TXFS64_BLOCK_SIZE / 512;
    return ata64_write(TXFS64_LBA_OFFSET + block * sectors, buf, sectors);
}

// Write superblock back after modifying free_blocks or free_inodes.
static void txfs64_write_super(void) {
    txfs64_write_block(TXFS64_BLOCK_SUPER, (const uint8_t*)&sb);
}

// Write one inode back to disk.
static int txfs64_write_inode(uint32_t num, const txfs64_inode_t* inode) {
    uint8_t buf[TXFS64_BLOCK_SIZE];
    uint32_t block  = TXFS64_BLOCK_INODES + (num / INODES_PER_BLOCK);
    uint32_t offset = (num % INODES_PER_BLOCK) * sizeof(txfs64_inode_t);
    if (txfs64_read_block(block, buf) < 0) return -1;
    const uint8_t* src = (const uint8_t*)inode;
    for (uint32_t i = 0; i < sizeof(txfs64_inode_t); i++) buf[offset + i] = src[i];
    return txfs64_write_block(block, buf);
}

// Bitmap bit test/set/clear helpers.
static int  txfs64_btest(const uint8_t* m, int i) { return (m[i/8] >> (i%8)) & 1; }
static void txfs64_bset (uint8_t* m, int i) { m[i/8] |=  (uint8_t)(1 << (i%8)); }
static void txfs64_bclr (uint8_t* m, int i) { m[i/8] &= (uint8_t)~(1 << (i%8)); }

// Returns the absolute physical block number of a newly-allocated data
// block (bit + TXFS64_BLOCK_DATA), already zeroed. Returns -1 if full.
static int txfs64_alloc_block(void) {
    if (txfs64_read_super() < 0) return -1;
    if (!sb.free_blocks) return -1;

    uint8_t bmap[TXFS64_BLOCK_SIZE];
    if (txfs64_read_block(TXFS64_BLOCK_BBITMAP, bmap) < 0) return -1;

    int bit = -1;
    for (int i = 0; i < (int)sb.total_blocks; i++) {
        if (!txfs64_btest(bmap, i)) { bit = i; break; }
    }
    if (bit < 0) return -1;

    txfs64_bset(bmap, bit);
    txfs64_write_block(TXFS64_BLOCK_BBITMAP, bmap);
    sb.free_blocks--;
    txfs64_write_super();

    uint32_t phys = (uint32_t)bit + TXFS64_BLOCK_DATA;
    txfs64_write_block(phys, txfs64_zero_block);
    return (int)phys;
}

static void txfs64_free_block(uint32_t phys_block) {
    if (phys_block < TXFS64_BLOCK_DATA) return;
    uint8_t bmap[TXFS64_BLOCK_SIZE];
    if (txfs64_read_block(TXFS64_BLOCK_BBITMAP, bmap) < 0) return;
    txfs64_bclr(bmap, (int)(phys_block - TXFS64_BLOCK_DATA));
    txfs64_write_block(TXFS64_BLOCK_BBITMAP, bmap);
    if (txfs64_read_super() == 0) { sb.free_blocks++; txfs64_write_super(); }
}

// Returns allocated inode number, or -1.
static int txfs64_alloc_inode(void) {
    if (txfs64_read_super() < 0) return -1;
    if (!sb.free_inodes) return -1;

    uint8_t imap[TXFS64_BLOCK_SIZE];
    if (txfs64_read_block(TXFS64_BLOCK_IBITMAP, imap) < 0) return -1;

    int bit = -1;
    for (int i = 0; i < TXFS64_MAX_INODES; i++) {
        if (!txfs64_btest(imap, i)) { bit = i; break; }
    }
    if (bit < 0) return -1;

    txfs64_bset(imap, bit);
    txfs64_write_block(TXFS64_BLOCK_IBITMAP, imap);
    sb.free_inodes--;
    txfs64_write_super();
    return bit;
}

static void txfs64_free_inode(uint32_t num) {
    uint8_t imap[TXFS64_BLOCK_SIZE];
    if (txfs64_read_block(TXFS64_BLOCK_IBITMAP, imap) < 0) return;
    txfs64_bclr(imap, (int)num);
    txfs64_write_block(TXFS64_BLOCK_IBITMAP, imap);
    if (txfs64_read_super() == 0) { sb.free_inodes++; txfs64_write_super(); }
}

// Append a new directory entry to the directory at dir_inum.
static int txfs64_dir_append(uint32_t dir_inum, uint32_t child_inum,
                             uint8_t type, const char* name) {
    txfs64_inode_t dir;
    if (txfs64_read_inode(dir_inum, &dir) < 0) return -1;

    uint32_t per_block   = TXFS64_BLOCK_SIZE / sizeof(txfs64_dirent_t);
    uint32_t total_slots = (uint32_t)(dir.size / sizeof(txfs64_dirent_t));
    uint32_t block_idx   = total_slots / per_block;
    uint32_t slot_in_blk = total_slots % per_block;

    // Allocate a new data block if this is the first entry in a new block.
    uint32_t blk = txfs64_get_block(&dir, block_idx);
    if (!blk) {
        if (block_idx >= TXFS64_DIRECT_BLOCKS) return -1;  // too many entries
        int new_blk = txfs64_alloc_block();
        if (new_blk < 0) return -1;
        dir.blocks[block_idx] = (uint32_t)new_blk;
        blk = (uint32_t)new_blk;
    }

    uint8_t buf[TXFS64_BLOCK_SIZE];
    if (txfs64_read_block(blk, buf) < 0) return -1;

    txfs64_dirent_t* de = (txfs64_dirent_t*)(buf + slot_in_blk * sizeof(txfs64_dirent_t));
    de->inode    = child_inum;
    de->name_len = 0;
    de->type     = type;
    txfs64_strcpy(de->name, name, 255);

    if (txfs64_write_block(blk, buf) < 0) return -1;

    dir.size += sizeof(txfs64_dirent_t);
    return txfs64_write_inode(dir_inum, &dir);
}

// Zero out a directory entry named `name` in dir_inum. Does NOT reduce
// dir.size -- the entry count stays the same and the slot is simply
// skipped by readdir/lookup when its inode field is 0.
static int txfs64_dir_remove_entry(uint32_t dir_inum, const char* name) {
    txfs64_inode_t dir;
    if (txfs64_read_inode(dir_inum, &dir) < 0) return -1;

    uint8_t buf[TXFS64_BLOCK_SIZE];
    uint32_t entries_total = (uint32_t)(dir.size / sizeof(txfs64_dirent_t));
    uint32_t checked = 0;

    for (uint32_t b = 0; checked < entries_total; b++) {
        uint32_t blk = txfs64_get_block(&dir, b);
        if (!blk) break;
        if (txfs64_read_block(blk, buf) < 0) break;

        uint32_t per_block = TXFS64_BLOCK_SIZE / sizeof(txfs64_dirent_t);
        uint32_t in_this   = entries_total - checked;
        if (in_this > per_block) in_this = per_block;

        for (uint32_t i = 0; i < in_this; i++) {
            txfs64_dirent_t* de = (txfs64_dirent_t*)(buf + i * sizeof(txfs64_dirent_t));
            if (de->inode && !txfs64_strcmp(de->name, name)) {
                de->inode = 0;
                return txfs64_write_block(blk, buf);
            }
        }
        checked += in_this;
    }
    return -1;
}

// Splits an absolute internal path ("/a/b/c") into parent ("/a/b") and
// leaf ("c"). Root path ("/") produces parent="" and leaf="", caller
// must check. Returns 0 on success, -1 if path is malformed.
static int txfs64_split_path(const char* path, char* parent, int pmax,
                              char* leaf, int lmax) {
    int len = 0;
    while (path[len]) len++;

    int last_slash = -1;
    for (int i = len - 1; i >= 0; i--) {
        if (path[i] == '/') { last_slash = i; break; }
    }
    if (last_slash < 0) return -1;

    if (last_slash == 0) {
        parent[0] = '/'; parent[1] = 0;
    } else {
        int j = 0;
        while (j < last_slash && j < pmax - 1) { parent[j] = path[j]; j++; }
        parent[j] = 0;
    }

    int k = 0, s = last_slash + 1;
    while (path[s] && k < lmax - 1) leaf[k++] = path[s++];
    leaf[k] = 0;
    return 0;
}

int txfs64_mkdir(const char* path) {
    char parent[256], leaf[256];
    if (txfs64_split_path(path, parent, sizeof(parent), leaf, sizeof(leaf)) < 0) return -1;
    if (!leaf[0]) return -1;

    int parent_inum = txfs64_resolve_path(parent);
    if (parent_inum < 0) return -1;

    // Fail if name already exists
    txfs64_inode_t parent_inode;
    if (txfs64_read_inode((uint32_t)parent_inum, &parent_inode) < 0) return -1;
    if (txfs64_dir_lookup(&parent_inode, leaf) >= 0) return -1;  // already exists

    int new_inum = txfs64_alloc_inode();
    if (new_inum < 0) return -1;

    txfs64_inode_t new_dir = {0};
    new_dir.mode  = (TXFS64_TYPE_DIR << 12) | 0x1C0;
    new_dir.links = 1;
    new_dir.size  = 0;
    if (txfs64_write_inode((uint32_t)new_inum, &new_dir) < 0) {
        txfs64_free_inode((uint32_t)new_inum);
        return -1;
    }

    if (txfs64_dir_append((uint32_t)parent_inum, (uint32_t)new_inum,
                          TXFS64_TYPE_DIR, leaf) < 0) {
        txfs64_free_inode((uint32_t)new_inum);
        return -1;
    }
    return 0;
}

int txfs64_create_file(const char* path) {
    char parent[256], leaf[256];
    if (txfs64_split_path(path, parent, sizeof(parent), leaf, sizeof(leaf)) < 0) return -1;
    if (!leaf[0]) return -1;

    int parent_inum = txfs64_resolve_path(parent);
    if (parent_inum < 0) return -1;

    txfs64_inode_t parent_inode;
    if (txfs64_read_inode((uint32_t)parent_inum, &parent_inode) < 0) return -1;
    if (txfs64_dir_lookup(&parent_inode, leaf) >= 0) return -1;  // already exists

    int new_inum = txfs64_alloc_inode();
    if (new_inum < 0) return -1;

    txfs64_inode_t new_file = {0};
    new_file.mode  = (TXFS64_TYPE_FILE << 12) | 0x1C0;
    new_file.links = 1;
    new_file.size  = 0;
    if (txfs64_write_inode((uint32_t)new_inum, &new_file) < 0) {
        txfs64_free_inode((uint32_t)new_inum);
        return -1;
    }

    if (txfs64_dir_append((uint32_t)parent_inum, (uint32_t)new_inum,
                          TXFS64_TYPE_FILE, leaf) < 0) {
        txfs64_free_inode((uint32_t)new_inum);
        return -1;
    }
    return 0;
}

// Limited to TXFS64_DIRECT_BLOCKS * TXFS64_BLOCK_SIZE (48 KB) per file.
int txfs64_write_file(const char* path, const uint8_t* data, uint32_t len) {
    int inum = txfs64_resolve_path(path);
    if (inum < 0) return -1;

    txfs64_inode_t inode;
    if (txfs64_read_inode((uint32_t)inum, &inode) < 0) return -1;
    if (((inode.mode >> 12) & 0xF) != TXFS64_TYPE_FILE) return -1;

    uint32_t blocks_needed = (len + TXFS64_BLOCK_SIZE - 1) / TXFS64_BLOCK_SIZE;
    if (blocks_needed > TXFS64_DIRECT_BLOCKS) return -1;  // file too large

    // Free all existing data blocks first
    for (int i = 0; i < TXFS64_DIRECT_BLOCKS; i++) {
        if (inode.blocks[i]) { txfs64_free_block(inode.blocks[i]); inode.blocks[i] = 0; }
    }

    uint8_t blk_buf[TXFS64_BLOCK_SIZE];
    uint32_t done = 0;
    for (uint32_t b = 0; b < blocks_needed; b++) {
        int new_blk = txfs64_alloc_block();
        if (new_blk < 0) {
            inode.size = (uint64_t)done;
            txfs64_write_inode((uint32_t)inum, &inode);
            return -1;
        }
        inode.blocks[b] = (uint32_t)new_blk;

        // Build block: copy data then zero-pad to block boundary
        uint32_t chunk = len - done;
        if (chunk > TXFS64_BLOCK_SIZE) chunk = TXFS64_BLOCK_SIZE;
        uint32_t i;
        for (i = 0; i < chunk; i++) blk_buf[i] = data[done + i];
        for (; i < TXFS64_BLOCK_SIZE; i++) blk_buf[i] = 0;
        txfs64_write_block((uint32_t)new_blk, blk_buf);
        done += chunk;
    }

    inode.size = (uint64_t)len;
    return txfs64_write_inode((uint32_t)inum, &inode);
}

int txfs64_unlink(const char* path) {
    char parent[256], leaf[256];
    if (txfs64_split_path(path, parent, sizeof(parent), leaf, sizeof(leaf)) < 0) return -1;
    if (!leaf[0]) return -1;

    int inum = txfs64_resolve_path(path);
    if (inum < 0) return -1;

    txfs64_inode_t inode;
    if (txfs64_read_inode((uint32_t)inum, &inode) < 0) return -1;
    if (((inode.mode >> 12) & 0xF) != TXFS64_TYPE_FILE) return -2;

    // Free data blocks
    for (int i = 0; i < TXFS64_DIRECT_BLOCKS; i++) {
        if (inode.blocks[i]) { txfs64_free_block(inode.blocks[i]); inode.blocks[i] = 0; }
    }

    int parent_inum = txfs64_resolve_path(parent);
    if (parent_inum >= 0) txfs64_dir_remove_entry((uint32_t)parent_inum, leaf);
    txfs64_free_inode((uint32_t)inum);
    return 0;
}

// Rename one directory entry in place without touching the inode or data blocks.
static int txfs64_dir_rename_entry(uint32_t dir_inum,
                                   const char* old_name, const char* new_name) {
    txfs64_inode_t dir;
    if (txfs64_read_inode(dir_inum, &dir) < 0) return -1;

    uint8_t buf[TXFS64_BLOCK_SIZE];
    uint32_t entries_total = (uint32_t)(dir.size / sizeof(txfs64_dirent_t));
    uint32_t checked = 0;

    for (uint32_t b = 0; checked < entries_total; b++) {
        uint32_t blk = txfs64_get_block(&dir, b);
        if (!blk) break;
        if (txfs64_read_block(blk, buf) < 0) break;

        uint32_t per_block = TXFS64_BLOCK_SIZE / sizeof(txfs64_dirent_t);
        uint32_t in_this   = entries_total - checked;
        if (in_this > per_block) in_this = per_block;

        for (uint32_t i = 0; i < in_this; i++) {
            txfs64_dirent_t* de = (txfs64_dirent_t*)(buf + i * sizeof(txfs64_dirent_t));
            if (de->inode && !txfs64_strcmp(de->name, old_name)) {
                txfs64_strcpy(de->name, new_name, 255);
                return txfs64_write_block(blk, buf);
            }
        }
        checked += in_this;
    }
    return -1;
}

int txfs64_rename(const char* src_path, const char* dest_path) {
    int src_inum = txfs64_resolve_path(src_path);
    if (src_inum < 0) return -1;

    if (txfs64_resolve_path(dest_path) >= 0) return -4;  // dest already exists

    char src_parent[256], src_leaf[256];
    char dest_parent[256], dest_leaf[256];
    if (txfs64_split_path(src_path,  src_parent,  256, src_leaf,  256) < 0) return -1;
    if (txfs64_split_path(dest_path, dest_parent, 256, dest_leaf, 256) < 0) return -1;
    if (!src_leaf[0] || !dest_leaf[0]) return -1;

    int src_pinum  = txfs64_resolve_path(src_parent);
    int dest_pinum = txfs64_resolve_path(dest_parent);
    if (src_pinum < 0 || dest_pinum < 0) return -1;

    if (src_pinum == dest_pinum)
        return txfs64_dir_rename_entry((uint32_t)src_pinum, src_leaf, dest_leaf);

    // Cross-directory: relink the inode under the new parent and unlink old entry.
    txfs64_inode_t inode;
    if (txfs64_read_inode((uint32_t)src_inum, &inode) < 0) return -1;
    uint8_t type = (uint8_t)((inode.mode >> 12) & 0xF);

    if (txfs64_dir_append((uint32_t)dest_pinum, (uint32_t)src_inum,
                          type, dest_leaf) < 0) return -1;
    txfs64_dir_remove_entry((uint32_t)src_pinum, src_leaf);
    return 0;
}

int txfs64_dir_is_empty(const char* path) {
    int inum = txfs64_resolve_path(path);
    if (inum < 0) return 0;

    txfs64_inode_t inode;
    if (txfs64_read_inode((uint32_t)inum, &inode) < 0) return 0;
    if (((inode.mode >> 12) & 0xF) != TXFS64_TYPE_DIR) return 0;

    uint8_t buf[TXFS64_BLOCK_SIZE];
    uint32_t total = (uint32_t)(inode.size / sizeof(txfs64_dirent_t));
    uint32_t checked = 0;
    for (uint32_t b = 0; checked < total; b++) {
        uint32_t blk = txfs64_get_block(&inode, b);
        if (!blk) break;
        if (txfs64_read_block(blk, buf) < 0) break;
        uint32_t per = TXFS64_BLOCK_SIZE / sizeof(txfs64_dirent_t);
        uint32_t in_this = total - checked;
        if (in_this > per) in_this = per;
        for (uint32_t i = 0; i < in_this; i++) {
            txfs64_dirent_t* de = (txfs64_dirent_t*)(buf + i * sizeof(txfs64_dirent_t));
            if (de->inode) return 0;  // found live entry
        }
        checked += in_this;
    }
    return 1;  // no live entries found
}

int txfs64_rmdir(const char* path) {
    char parent[256], leaf[256];
    if (txfs64_split_path(path, parent, sizeof(parent), leaf, sizeof(leaf)) < 0) return -1;
    if (!leaf[0]) return -1;

    if (!txfs64_dir_is_empty(path)) return -1;  // refuse non-empty

    int inum = txfs64_resolve_path(path);
    if (inum < 0) return -1;

    txfs64_inode_t inode;
    if (txfs64_read_inode((uint32_t)inum, &inode) < 0) return -1;
    if (((inode.mode >> 12) & 0xF) != TXFS64_TYPE_DIR) return -1;

    // Free any data blocks (typically none for a just-emptied dir)
    for (int i = 0; i < TXFS64_DIRECT_BLOCKS; i++) {
        if (inode.blocks[i]) { txfs64_free_block(inode.blocks[i]); inode.blocks[i] = 0; }
    }

    int parent_inum = txfs64_resolve_path(parent);
    if (parent_inum >= 0) txfs64_dir_remove_entry((uint32_t)parent_inum, leaf);
    txfs64_free_inode((uint32_t)inum);
    return 0;
}
