// ToxenOS/kernel/txfs.c
// TxFS — ToxenOS native filesystem
//
// Improvements over v1:
//   - txfs_mkdir: correctly resolves any-depth parent, not always root
//   - txfs_open (create): same fix; robust parent extraction
//   - txfs_remove: finds file in its actual parent dir, not always root
//   - Directories: entries spread across multiple blocks (no more 15-entry cap)
//   - TxFS v2: uint64_t size + triple indirect = 4TB addressable, 16EB size field
//   - free_blocks/free_inodes properly maintained on remove
//   - txfs_alloc_block: always clears the new block on every allocation

#include <stdint.h>
#include "../include/txfs.h"
#include "../include/ata.h"
#include "../include/mm.h"
#include "../include/vga.h"
#include "../include/timer.h"

static char txfs_mountpoint[64] = "/C:";

static const char* txfs_strip_mount(const char* path)
{
    int i = 0;
    while (txfs_mountpoint[i] && path[i] == txfs_mountpoint[i]) i++;
    if (!path[i]) return "/";
    return path + i;
}

// --- string helpers ----------------------------------------------------------

static int txfs_strlen(const char* s)
{
    int i = 0; while (s[i]) i++; return i;
}

static void txfs_strcpy(char* dst, const char* src, int max)
{
    int i = 0;
    while (src[i] && i < max - 1) { dst[i] = src[i]; i++; }
    dst[i] = 0;
}

static int txfs_strcmp(const char* a, const char* b)
{
    int i;
    for (i = 0; a[i] && b[i]; i++)
        if (a[i] != b[i]) return 1;
    return a[i] != b[i];
}

// --- block I/O ---------------------------------------------------------------

static uint8_t block_buf[TXFS_BLOCK_SIZE];

static int txfs_read_block(uint32_t block, uint8_t* buf)
{
    uint32_t sectors = TXFS_BLOCK_SIZE / 512;
    return ata_read(block * sectors, buf, sectors);
}

static int txfs_write_block(uint32_t block, const uint8_t* buf)
{
    uint32_t sectors = TXFS_BLOCK_SIZE / 512;
    return ata_write(block * sectors, buf, sectors);
}

// --- superblock --------------------------------------------------------------

static txfs_superblock_t sb;

static int txfs_read_super()
{
    if (txfs_read_block(TXFS_BLOCK_SUPER, (uint8_t*)&sb) < 0) return -1;
    if (sb.magic != TXFS_MAGIC) return -1;
    return 0;
}

static int txfs_write_super()
{
    return txfs_write_block(TXFS_BLOCK_SUPER, (uint8_t*)&sb);
}

// --- bitmap helpers ----------------------------------------------------------

static uint8_t inode_bitmap[TXFS_BLOCK_SIZE];
static uint8_t block_bitmap[TXFS_BLOCK_SIZE];

static int bitmap_test(uint8_t* bm, uint32_t bit)
{
    return (bm[bit / 8] >> (bit % 8)) & 1;
}

static void bitmap_set(uint8_t* bm, uint32_t bit)
{
    bm[bit / 8] |= (1 << (bit % 8));
}

static void bitmap_clear(uint8_t* bm, uint32_t bit)
{
    bm[bit / 8] &= ~(1 << (bit % 8));
}

static int bitmap_alloc(uint8_t* bm, uint32_t max)
{
    for (uint32_t i = 0; i < max; i++)
        if (!bitmap_test(bm, i)) { bitmap_set(bm, i); return i; }
    return -1;
}

// --- inode I/O ---------------------------------------------------------------

#define INODES_PER_BLOCK    16  // 4096 / 256 = 16

static int txfs_read_inode(uint32_t num, txfs_inode_t* inode)
{
    uint32_t block  = TXFS_BLOCK_INODES + (num / INODES_PER_BLOCK);
    uint32_t offset = (num % INODES_PER_BLOCK) * sizeof(txfs_inode_t);

    if (txfs_read_block(block, block_buf) < 0) return -1;

    uint8_t* src = block_buf + offset;
    uint8_t* dst = (uint8_t*)inode;
    for (uint32_t i = 0; i < sizeof(txfs_inode_t); i++)
        dst[i] = src[i];

    return 0;
}

static int txfs_write_inode(uint32_t num, const txfs_inode_t* inode)
{
    uint32_t block  = TXFS_BLOCK_INODES + (num / INODES_PER_BLOCK);
    uint32_t offset = (num % INODES_PER_BLOCK) * sizeof(txfs_inode_t);

    if (txfs_read_block(block, block_buf) < 0) return -1;

    uint8_t* dst = block_buf + offset;
    uint8_t* src = (uint8_t*)inode;
    for (uint32_t i = 0; i < sizeof(txfs_inode_t); i++)
        dst[i] = src[i];

    return txfs_write_block(block, block_buf);
}

// --- block allocation --------------------------------------------------------

static uint8_t zero_block[TXFS_BLOCK_SIZE];

static int txfs_alloc_block()
{
    if (txfs_read_block(TXFS_BLOCK_BBITMAP, block_bitmap) < 0) return -1;

    int b = bitmap_alloc(block_bitmap, sb.total_blocks);
    if (b < 0) return -1;

    txfs_write_block(TXFS_BLOCK_BBITMAP, block_bitmap);
    sb.free_blocks--;
    txfs_write_super();

    // always clear new block so stale data never leaks
    txfs_write_block((uint32_t)(b + TXFS_BLOCK_DATA), zero_block);

    return b + TXFS_BLOCK_DATA;
}

static void txfs_free_block(uint32_t block_num)
{
    if (block_num < TXFS_BLOCK_DATA) return;
    txfs_read_block(TXFS_BLOCK_BBITMAP, block_bitmap);
    bitmap_clear(block_bitmap, block_num - TXFS_BLOCK_DATA);
    txfs_write_block(TXFS_BLOCK_BBITMAP, block_bitmap);
    sb.free_blocks++;
    txfs_write_super();
}

static int txfs_alloc_inode()
{
    if (txfs_read_block(TXFS_BLOCK_IBITMAP, inode_bitmap) < 0) return -1;

    int i = bitmap_alloc(inode_bitmap, sb.total_inodes);
    if (i < 0) return -1;

    txfs_write_block(TXFS_BLOCK_IBITMAP, inode_bitmap);
    sb.free_inodes--;
    txfs_write_super();
    return i;
}

static void txfs_free_inode(uint32_t inum)
{
    txfs_read_block(TXFS_BLOCK_IBITMAP, inode_bitmap);
    bitmap_clear(inode_bitmap, inum);
    txfs_write_block(TXFS_BLOCK_IBITMAP, inode_bitmap);
    sb.free_inodes++;
    txfs_write_super();
}

// --- indirect block helpers --------------------------------------------------

// TXFS_PTRS_PER_BLOCK: a 4096-byte block holds 1024 uint32_t pointers
#define TXFS_PTRS_PER_BLOCK  (TXFS_BLOCK_SIZE / 4)

// Return the physical block number for logical block index idx within inode.
// If alloc=1, allocates missing indirect/data blocks.
// Returns 0 if the block doesn't exist and alloc=0.
static uint32_t txfs_get_block(txfs_inode_t* inode, uint32_t idx, int alloc)
{
    if (idx < TXFS_DIRECT_BLOCKS) {
        if (!inode->blocks[idx] && alloc) {
            int b = txfs_alloc_block();
            if (b < 0) return 0;
            inode->blocks[idx] = (uint32_t)b;
        }
        return inode->blocks[idx];
    }

    // ── Single-indirect ──────────────────────────────────────────────────────
    uint32_t after_direct = idx - TXFS_DIRECT_BLOCKS;
    if (after_direct < TXFS_PTRS_PER_BLOCK) {
        if (!inode->indirect) {
            if (!alloc) return 0;
            int b = txfs_alloc_block(); if (b < 0) return 0;
            inode->indirect = (uint32_t)b;
        }
        uint32_t ptrs[TXFS_PTRS_PER_BLOCK];
        txfs_read_block(inode->indirect, (uint8_t*)ptrs);
        if (!ptrs[after_direct]) {
            if (!alloc) return 0;
            int b = txfs_alloc_block(); if (b < 0) return 0;
            ptrs[after_direct] = (uint32_t)b;
            txfs_write_block(inode->indirect, (uint8_t*)ptrs);
        }
        return ptrs[after_direct];
    }

    // ── Double-indirect ───────────────────────────────────────────────────────
    uint32_t after_single = after_direct - TXFS_PTRS_PER_BLOCK;
    if (after_single < TXFS_PTRS_PER_BLOCK * TXFS_PTRS_PER_BLOCK) {
        uint32_t dbl_idx = after_single;
        uint32_t l1 = dbl_idx / TXFS_PTRS_PER_BLOCK;
        uint32_t l2 = dbl_idx % TXFS_PTRS_PER_BLOCK;
        if (!inode->dindirect) {
            if (!alloc) return 0;
            int b = txfs_alloc_block(); if (b < 0) return 0;
            inode->dindirect = (uint32_t)b;
        }
        uint32_t l1p[TXFS_PTRS_PER_BLOCK];
        txfs_read_block(inode->dindirect, (uint8_t*)l1p);
        if (!l1p[l1]) {
            if (!alloc) return 0;
            int b = txfs_alloc_block(); if (b < 0) return 0;
            l1p[l1] = (uint32_t)b; txfs_write_block(inode->dindirect, (uint8_t*)l1p);
        }
        uint32_t l2p[TXFS_PTRS_PER_BLOCK];
        txfs_read_block(l1p[l1], (uint8_t*)l2p);
        if (!l2p[l2]) {
            if (!alloc) return 0;
            int b = txfs_alloc_block(); if (b < 0) return 0;
            l2p[l2] = (uint32_t)b; txfs_write_block(l1p[l1], (uint8_t*)l2p);
        }
        return l2p[l2];
    }

    // ── Triple-indirect ───────────────────────────────────────────────────────
    // Range: adds 1024^3 × 4KB ≈ 4TB
    uint32_t after_double = after_single - TXFS_PTRS_PER_BLOCK * TXFS_PTRS_PER_BLOCK;
    if (after_double >= TXFS_PTRS_PER_BLOCK * TXFS_PTRS_PER_BLOCK * TXFS_PTRS_PER_BLOCK)
        return 0; // beyond triple-indirect (>4TB)

    uint32_t t1 = after_double / (TXFS_PTRS_PER_BLOCK * TXFS_PTRS_PER_BLOCK);
    uint32_t t2 = (after_double / TXFS_PTRS_PER_BLOCK) % TXFS_PTRS_PER_BLOCK;
    uint32_t t3 = after_double % TXFS_PTRS_PER_BLOCK;

    if (!inode->tindirect) {
        if (!alloc) return 0;
        int b = txfs_alloc_block(); if (b < 0) return 0;
        inode->tindirect = (uint32_t)b;
    }
    uint32_t p1[TXFS_PTRS_PER_BLOCK]; txfs_read_block(inode->tindirect, (uint8_t*)p1);
    if (!p1[t1]) {
        if (!alloc) return 0;
        int b = txfs_alloc_block(); if (b < 0) return 0;
        p1[t1]=(uint32_t)b; txfs_write_block(inode->tindirect,(uint8_t*)p1);
    }
    uint32_t p2[TXFS_PTRS_PER_BLOCK]; txfs_read_block(p1[t1],(uint8_t*)p2);
    if (!p2[t2]) {
        if (!alloc) return 0;
        int b = txfs_alloc_block(); if (b < 0) return 0;
        p2[t2]=(uint32_t)b; txfs_write_block(p1[t1],(uint8_t*)p2);
    }
    uint32_t p3[TXFS_PTRS_PER_BLOCK]; txfs_read_block(p2[t2],(uint8_t*)p3);
    if (!p3[t3]) {
        if (!alloc) return 0;
        int b = txfs_alloc_block(); if (b < 0) return 0;
        p3[t3]=(uint32_t)b; txfs_write_block(p2[t2],(uint8_t*)p3);
    }
    return p3[t3];
}

// --- format ------------------------------------------------------------------

int txfs_format(uint32_t total_blocks)
{
    uint8_t* p = (uint8_t*)&sb;
    for (uint32_t i = 0; i < sizeof(sb); i++) p[i] = 0;

    sb.magic        = TXFS_MAGIC;
    sb.version      = TXFS_VERSION;
    sb.block_size   = TXFS_BLOCK_SIZE;
    sb.total_blocks = total_blocks - TXFS_BLOCK_DATA;
    sb.free_blocks  = sb.total_blocks;
    sb.total_inodes = TXFS_MAX_INODES;
    sb.free_inodes  = TXFS_MAX_INODES - 1;
    sb.root_inode   = 0;

    txfs_write_super();

    for (int i = 0; i < TXFS_BLOCK_SIZE; i++)
        inode_bitmap[i] = block_bitmap[i] = 0;

    bitmap_set(inode_bitmap, 0);

    txfs_write_block(TXFS_BLOCK_IBITMAP, inode_bitmap);
    txfs_write_block(TXFS_BLOCK_BBITMAP, block_bitmap);

    txfs_inode_t root;
    p = (uint8_t*)&root;
    for (uint32_t i = 0; i < sizeof(root); i++) p[i] = 0;

    root.mode  = (TXFS_TYPE_DIR << 12) |
                 TXFS_PERM_OWNER_R | TXFS_PERM_OWNER_W | TXFS_PERM_OWNER_X;
    root.links = 1;
    root.size  = 0;

    txfs_write_inode(0, &root);

    return 0;
}

// --- directory helpers -------------------------------------------------------

// Search directory inode for a named entry. Returns child inode number or -1.
static int txfs_dir_lookup(txfs_inode_t* dir, const char* name)
{
    uint8_t  data_buf[TXFS_BLOCK_SIZE];
    uint32_t entries_total = dir->size / sizeof(txfs_dirent_t);
    uint32_t checked = 0;

    for (int b = 0; checked < entries_total; b++) {
        uint32_t blk = txfs_get_block(dir, (uint32_t)b, 0);
        if (!blk) break;
        txfs_read_block(blk, data_buf);

        uint32_t per_block = TXFS_BLOCK_SIZE / sizeof(txfs_dirent_t);
        uint32_t in_this   = entries_total - checked;
        if (in_this > per_block) in_this = per_block;

        for (uint32_t i = 0; i < in_this; i++) {
            txfs_dirent_t* de = (txfs_dirent_t*)(data_buf + i * sizeof(txfs_dirent_t));
            if (de->inode && !txfs_strcmp(de->name, name))
                return (int)de->inode;
        }
        checked += in_this;
    }
    return -1;
}

// Append a new dirent to a directory, allocating an extra data block if needed.
static int txfs_dir_append(int dir_inum, txfs_inode_t* dir,
                           uint32_t child_inum, uint8_t type, const char* name)
{
    uint32_t per_block   = TXFS_BLOCK_SIZE / sizeof(txfs_dirent_t);
    uint32_t total_slots = dir->size / sizeof(txfs_dirent_t);
    uint32_t block_idx   = total_slots / per_block;
    uint32_t slot_in_blk = total_slots % per_block;

    uint8_t  data_buf[TXFS_BLOCK_SIZE];
    uint32_t blk = txfs_get_block(dir, block_idx, 1);
    if (!blk) return -1;

    txfs_read_block(blk, data_buf);
    txfs_dirent_t* de = (txfs_dirent_t*)(data_buf + slot_in_blk * sizeof(txfs_dirent_t));

    de->inode    = child_inum;
    de->name_len = (uint16_t)txfs_strlen(name);
    de->type     = type;
    txfs_strcpy(de->name, name, 256);

    txfs_write_block(blk, data_buf);
    dir->size += sizeof(txfs_dirent_t);
    txfs_write_inode((uint32_t)dir_inum, dir);

    return 0;
}

// Mark a dirent as deleted (inode=0). Does NOT compact the directory.
static int txfs_dir_remove_entry(int dir_inum, txfs_inode_t* dir, const char* name)
{
    uint8_t  data_buf[TXFS_BLOCK_SIZE];
    uint32_t entries_total = dir->size / sizeof(txfs_dirent_t);
    uint32_t checked = 0;

    for (int b = 0; checked < entries_total; b++) {
        uint32_t blk = txfs_get_block(dir, (uint32_t)b, 0);
        if (!blk) break;
        txfs_read_block(blk, data_buf);

        uint32_t per_block = TXFS_BLOCK_SIZE / sizeof(txfs_dirent_t);
        uint32_t in_this   = entries_total - checked;
        if (in_this > per_block) in_this = per_block;

        for (uint32_t i = 0; i < in_this; i++) {
            txfs_dirent_t* de = (txfs_dirent_t*)(data_buf + i * sizeof(txfs_dirent_t));
            if (de->inode && !txfs_strcmp(de->name, name)) {
                de->inode = 0;
                txfs_write_block(blk, data_buf);
                (void)dir_inum;
                return 0;
            }
        }
        checked += in_this;
    }
    return -1;
}

// --- path helpers ------------------------------------------------------------

// Resolve a full path to an inode number, or -1 if not found.
static int txfs_lookup(const char* path)
{
    if (!txfs_strcmp(path, "/")) return 0;

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

        txfs_inode_t inode;
        if (txfs_read_inode((uint32_t)cur, &inode) < 0) return -1;

        int type = (inode.mode >> 12) & 0xF;
        if (type != TXFS_TYPE_DIR) return -1;

        cur = txfs_dir_lookup(&inode, component);
        if (cur < 0) return -1;
    }

    return cur;
}

// Split /a/b/c into parent="/a/b" and name="c".
static void txfs_split_path(const char* path, char* parent_out, char* name_out)
{
    int len   = txfs_strlen(path);
    int slash = -1;
    for (int i = len - 1; i >= 0; i--) {
        if (path[i] == '/') { slash = i; break; }
    }

    if (slash <= 0) {
        txfs_strcpy(parent_out, "/", 256);
        const char* n = path;
        if (*n == '/') n++;
        txfs_strcpy(name_out, n, 256);
    } else {
        for (int i = 0; i < slash; i++) parent_out[i] = path[i];
        parent_out[slash] = 0;
        txfs_strcpy(name_out, path + slash + 1, 256);
    }
}

// --- VFS driver functions ----------------------------------------------------

#define TXFS_MAX_FDS 32
static txfs_fd_t open_files[TXFS_MAX_FDS];

static int txfs_mount_fn(const char* device)
{
    for (int i = 0; i < TXFS_MAX_FDS; i++)
        open_files[i].used = 0;

    if (txfs_read_super() < 0) {
        // Query the drive's actual size instead of hardcoding 204800.
        // ata_get_sectors(0) returns 512-byte sector count via ATA IDENTIFY.
        // Falls back to 204800 (100 MB) if IDENTIFY fails.
        extern uint32_t ata_get_sectors(uint8_t drive);
        uint32_t total = ata_get_sectors(0);
        if (!total) total = 204800;
        txfs_format(total);
        txfs_read_super();
    }

    return 0;
}

static int txfs_open_fn(const char* path, int flags)
{
    const char* local = txfs_strip_mount(path);
    int inode_num = txfs_lookup(local);

    if (inode_num >= 0) {
        // File exists — check permissions
        txfs_inode_t existing;
        txfs_read_inode((uint32_t)inode_num, &existing);
        uint32_t perm = existing.mode & 0x1FF;
        if ((flags & VFS_O_READ)  && !(perm & TXFS_PERM_OWNER_R)) return -1;
        if ((flags & VFS_O_WRITE) && !(perm & TXFS_PERM_OWNER_W)) return -1;
    }

    if (inode_num < 0) {
        if (!(flags & VFS_O_CREATE)) return -1;

        int new_inum = txfs_alloc_inode();
        if (new_inum < 0) return -1;

        txfs_inode_t inode;
        uint8_t* p = (uint8_t*)&inode;
        for (uint32_t i = 0; i < sizeof(inode); i++) p[i] = 0;
        inode.mode     = (TXFS_TYPE_FILE << 12) | TXFS_PERM_OWNER_R | TXFS_PERM_OWNER_W | TXFS_PERM_OWNER_X;
        inode.links    = 1;
        inode.size     = 0;
        inode.created  = timer_getticks();
        inode.modified = inode.created;
        txfs_write_inode((uint32_t)new_inum, &inode);

        char parent_path[256], filename[256];
        txfs_split_path(local, parent_path, filename);

        int parent_inum = txfs_lookup(parent_path);
        if (parent_inum < 0) parent_inum = 0;

        txfs_inode_t parent;
        txfs_read_inode((uint32_t)parent_inum, &parent);
        txfs_dir_append(parent_inum, &parent, (uint32_t)new_inum, TXFS_TYPE_FILE, filename);

        inode_num = new_inum;
    }

    for (int i = 0; i < TXFS_MAX_FDS; i++) {
        if (!open_files[i].used) {
            open_files[i].used      = 1;
            open_files[i].inode_num = (uint32_t)inode_num;
            txfs_read_inode((uint32_t)inode_num, &open_files[i].inode);
            // For append mode, start writing at end of existing data
            open_files[i].position = (flags & VFS_O_APPEND) ? open_files[i].inode.size : 0;
            return i;
        }
    }

    return -1;
}

static int txfs_close_fn(int fd)
{
    if (fd < 0 || fd >= TXFS_MAX_FDS || !open_files[fd].used) return -1;
    open_files[fd].used = 0;
    return 0;
}

static int txfs_read_fn(int fd, uint8_t* buf, uint32_t size)
{
    if (fd < 0 || fd >= TXFS_MAX_FDS || !open_files[fd].used) return -1;

    txfs_fd_t*    f     = &open_files[fd];
    txfs_inode_t* inode = &f->inode;

    if (f->position >= inode->size) return 0;

    uint32_t to_read = size;
    if (f->position + to_read > inode->size)
        to_read = inode->size - f->position;

    uint32_t done = 0;
    uint8_t  data_buf[TXFS_BLOCK_SIZE];

    while (done < to_read) {
        uint32_t block_idx    = (f->position + done) / TXFS_BLOCK_SIZE;
        uint32_t block_offset = (f->position + done) % TXFS_BLOCK_SIZE;

        uint32_t blk = txfs_get_block(inode, block_idx, 0);
        if (!blk) break;

        txfs_read_block(blk, data_buf);

        uint32_t can_read = TXFS_BLOCK_SIZE - block_offset;
        if (can_read > to_read - done) can_read = to_read - done;

        for (uint32_t i = 0; i < can_read; i++)
            buf[done + i] = data_buf[block_offset + i];

        done += can_read;
    }

    f->position += done;
    return (int)done;
}

static int txfs_write_fn(int fd, const uint8_t* buf, uint32_t size)
{
    if (fd < 0 || fd >= TXFS_MAX_FDS || !open_files[fd].used) return -1;

    txfs_fd_t*    f     = &open_files[fd];
    txfs_inode_t* inode = &f->inode;

    uint32_t done  = 0;
    int      dirty = 0;
    uint8_t  data_buf[TXFS_BLOCK_SIZE];

    while (done < size) {
        uint32_t block_idx    = (f->position + done) / TXFS_BLOCK_SIZE;
        uint32_t block_offset = (f->position + done) % TXFS_BLOCK_SIZE;

        uint32_t blk = txfs_get_block(inode, block_idx, 1);
        if (!blk) break;

        txfs_read_block(blk, data_buf);

        uint32_t can_write = TXFS_BLOCK_SIZE - block_offset;
        if (can_write > size - done) can_write = size - done;

        for (uint32_t i = 0; i < can_write; i++)
            data_buf[block_offset + i] = buf[done + i];

        txfs_write_block(blk, data_buf);
        done += can_write;
        dirty = 1;
    }

    if (dirty) {
        f->position += done;
        if (f->position > inode->size)
            inode->size = f->position;
        inode->modified = timer_getticks();
        txfs_write_inode(f->inode_num, inode);
    }

    return (int)done;
}

static int txfs_mkdir_fn(const char* path)
{
    const char* local = txfs_strip_mount(path);

    if (txfs_lookup(local) >= 0) return -1;  // already exists

    int new_inum = txfs_alloc_inode();
    if (new_inum < 0) return -1;

    txfs_inode_t inode;
    uint8_t* p = (uint8_t*)&inode;
    for (uint32_t i = 0; i < sizeof(inode); i++) p[i] = 0;

    inode.mode     = (TXFS_TYPE_DIR << 12) |
                     TXFS_PERM_OWNER_R | TXFS_PERM_OWNER_W | TXFS_PERM_OWNER_X;
    inode.links    = 1;
    inode.size     = 0;
    inode.created  = timer_getticks();
    inode.modified = inode.created;
    txfs_write_inode((uint32_t)new_inum, &inode);

    char parent_path[256], dirname[256];
    txfs_split_path(local, parent_path, dirname);

    int parent_inum = txfs_lookup(parent_path);
    if (parent_inum < 0) parent_inum = 0;

    txfs_inode_t parent;
    txfs_read_inode((uint32_t)parent_inum, &parent);
    txfs_dir_append(parent_inum, &parent, (uint32_t)new_inum, TXFS_TYPE_DIR, dirname);

    return 0;
}

static int txfs_remove_fn(const char* path)
{
    const char* local = txfs_strip_mount(path);
    int inode_num = txfs_lookup(local);
    if (inode_num < 0) return -1;

    txfs_inode_t inode;
    txfs_read_inode((uint32_t)inode_num, &inode);

    // free direct blocks
    for (int i = 0; i < TXFS_DIRECT_BLOCKS; i++) {
        if (inode.blocks[i])
            txfs_free_block(inode.blocks[i]);
    }

    // free indirect block and all blocks it points to
    if (inode.indirect) {
        uint32_t ptrs[TXFS_PTRS_PER_BLOCK];
        txfs_read_block(inode.indirect, (uint8_t*)ptrs);
        for (uint32_t i = 0; i < TXFS_PTRS_PER_BLOCK; i++) {
            if (ptrs[i]) txfs_free_block(ptrs[i]);
        }
        txfs_free_block(inode.indirect);
    }

    txfs_free_inode((uint32_t)inode_num);

    // remove dirent from the correct parent
    char parent_path[256], name[256];
    txfs_split_path(local, parent_path, name);

    int parent_inum = txfs_lookup(parent_path);
    if (parent_inum < 0) parent_inum = 0;

    txfs_inode_t parent;
    txfs_read_inode((uint32_t)parent_inum, &parent);
    txfs_dir_remove_entry(parent_inum, &parent, name);

    return 0;
}

static int txfs_isdir_fn(const char* path)
{
    const char* local = txfs_strip_mount(path);
    int inode_num = txfs_lookup(local);
    if (inode_num < 0) return -1;

    txfs_inode_t inode;
    txfs_read_inode((uint32_t)inode_num, &inode);
    int type = (inode.mode >> 12) & 0xF;
    return (type == TXFS_TYPE_DIR) ? 1 : 0;
}

static int txfs_readdir_fn(const char* path, char* out, uint32_t index)
{
    const char* local = txfs_strip_mount(path);
    int inode_num = txfs_lookup(local);
    if (inode_num < 0) return -1;

    txfs_inode_t inode;
    if (txfs_read_inode((uint32_t)inode_num, &inode) < 0) return -1;

    int type = (inode.mode >> 12) & 0xF;
    if (type != TXFS_TYPE_DIR) return -1;

    uint8_t  data_buf[TXFS_BLOCK_SIZE];
    uint32_t entries_total = inode.size / sizeof(txfs_dirent_t);
    uint32_t count   = 0;
    uint32_t checked = 0;

    for (int b = 0; checked < entries_total; b++) {
        uint32_t blk = txfs_get_block(&inode, (uint32_t)b, 0);
        if (!blk) break;
        txfs_read_block(blk, data_buf);

        uint32_t per_block = TXFS_BLOCK_SIZE / sizeof(txfs_dirent_t);
        uint32_t in_this   = entries_total - checked;
        if (in_this > per_block) in_this = per_block;

        for (uint32_t i = 0; i < in_this; i++) {
            txfs_dirent_t* de = (txfs_dirent_t*)(data_buf + i * sizeof(txfs_dirent_t));
            if (de->inode) {
                if (count == index) {
                    txfs_strcpy(out, de->name, 256);
                    return 0;
                }
                count++;
            }
        }
        checked += in_this;
    }

    return -1;
}

static int txfs_stat_fn(const char* path, uint32_t* size)
{
    const char* local = txfs_strip_mount(path);
    int inode_num = txfs_lookup(local);
    if (inode_num < 0) return -1;

    txfs_inode_t inode;
    if (txfs_read_inode((uint32_t)inode_num, &inode) < 0) return -1;

    // Clamp uint64_t size to uint32_t for VFS compat (files >4GB: 0xFFFFFFFF)
    *size = inode.size > 0xFFFFFFFFULL ? 0xFFFFFFFFu : (uint32_t)inode.size;
    return 0;
}

static int txfs_chmod_fn(const char* path, uint32_t new_perm)
{
    const char* local = txfs_strip_mount(path);
    int inode_num = txfs_lookup(local);
    if (inode_num < 0) return -1;
    txfs_inode_t inode;
    txfs_read_inode((uint32_t)inode_num, &inode);
    // Keep type bits (upper 4 bits of high nibble), replace permission bits
    inode.mode = (inode.mode & 0xFFFFF000u) | (new_perm & 0x1FF);
    txfs_write_inode((uint32_t)inode_num, &inode);
    return 0;
}

static int txfs_getmode_fn(const char* path)
{
    const char* local = txfs_strip_mount(path);
    int inode_num = txfs_lookup(local);
    if (inode_num < 0) return -1;
    txfs_inode_t inode;
    txfs_read_inode((uint32_t)inode_num, &inode);
    return (int)(inode.mode & 0x1FF);  // return permission bits only
}

// --- TxFS Snapshots ----------------------------------------------------------
// Snapshots live in a dedicated area AFTER the normal filesystem space.
// Disk layout: blocks 0-25599 = TxFS filesystem, blocks 25600+ = snapshot area
// Snapshot area block 0 = directory (up to 10 snapshot entries)
// Snapshot area blocks 1..N = metadata snapshots (TXFS_META_BLOCKS each)

#define SNAP_AREA_START   25600   // first block of snapshot area
#define SNAP_MAX          10      // max snapshots
#define TXFS_META_BLOCKS  36      // blocks 0-35 = superblock+bitmaps+inode table

typedef struct {
    char     name[64];
    uint32_t timestamp;
    int      used;
    uint32_t pad[2];
} txfs_snap_entry_t;  // 80 bytes

typedef struct {
    uint32_t          magic;                       // 0x534E4150 "SNAP"
    txfs_snap_entry_t entries[SNAP_MAX];           // 10 * 80 = 800 bytes
    uint8_t           pad[4096 - 4 - SNAP_MAX*80]; // pad to 1 block
} txfs_snap_dir_t;

static txfs_snap_dir_t snap_dir;
static int snap_dir_loaded = 0;

static void snap_load_dir(void) {
    if (!snap_dir_loaded) {
        uint8_t buf[TXFS_BLOCK_SIZE];
        if (txfs_read_block(SNAP_AREA_START, buf) == 0) {
            txfs_snap_dir_t* d = (txfs_snap_dir_t*)buf;
            if (d->magic == 0x534E4150u) {
                for (int i = 0; i < SNAP_MAX; i++) snap_dir.entries[i] = d->entries[i];
                snap_dir.magic = d->magic;
            } else {
                snap_dir.magic = 0x534E4150u;
                for (int i = 0; i < SNAP_MAX; i++) snap_dir.entries[i].used = 0;
            }
        }
        snap_dir_loaded = 1;
    }
}

static void snap_save_dir(void) {
    uint8_t buf[TXFS_BLOCK_SIZE];
    uint8_t* p = (uint8_t*)&snap_dir;
    for (int i = 0; i < TXFS_BLOCK_SIZE; i++) buf[i] = p[i];
    txfs_write_block(SNAP_AREA_START, buf);
}

static int snap_strcpy(char* d, const char* s, int max) {
    int i = 0; while (s[i] && i < max-1) { d[i]=s[i]; i++; } d[i]=0; return i;
}
static int snap_streq(const char* a, const char* b) {
    int i=0; while(a[i]&&b[i]&&a[i]==b[i])i++; return a[i]==b[i];
}

int txfs_snap_create(const char* name, uint32_t timestamp) {
    snap_load_dir();
    // Find free slot
    int slot = -1;
    for (int i = 0; i < SNAP_MAX; i++) {
        if (!snap_dir.entries[i].used) { slot = i; break; }
        if (snap_streq(snap_dir.entries[i].name, name)) { slot = i; break; } // overwrite same name
    }
    if (slot < 0) return -1; // no free slots

    // Save metadata blocks (0 to TXFS_META_BLOCKS-1) to snapshot area
    uint32_t snap_start = SNAP_AREA_START + 1 + (uint32_t)slot * TXFS_META_BLOCKS;
    uint8_t buf[TXFS_BLOCK_SIZE];
    for (int b = 0; b < TXFS_META_BLOCKS; b++) {
        if (txfs_read_block((uint32_t)b, buf) < 0) return -1;
        if (txfs_write_block(snap_start + (uint32_t)b, buf) < 0) return -1;
    }

    snap_dir.entries[slot].used = 1;
    snap_dir.entries[slot].timestamp = timestamp;
    snap_strcpy(snap_dir.entries[slot].name, name, 64);
    snap_save_dir();
    return slot;
}

int txfs_snap_restore(const char* name) {
    snap_load_dir();
    int slot = -1;
    for (int i = 0; i < SNAP_MAX; i++)
        if (snap_dir.entries[i].used && snap_streq(snap_dir.entries[i].name, name)) { slot = i; break; }
    if (slot < 0) return -1;

    uint32_t snap_start = SNAP_AREA_START + 1 + (uint32_t)slot * TXFS_META_BLOCKS;
    uint8_t buf[TXFS_BLOCK_SIZE];
    for (int b = 0; b < TXFS_META_BLOCKS; b++) {
        if (txfs_read_block(snap_start + (uint32_t)b, buf) < 0) return -1;
        if (txfs_write_block((uint32_t)b, buf) < 0) return -1;
    }
    // Reload superblock after restore
    txfs_read_super();
    snap_dir_loaded = 0;
    return 0;
}

int txfs_snap_delete(const char* name) {
    snap_load_dir();
    for (int i = 0; i < SNAP_MAX; i++) {
        if (snap_dir.entries[i].used && snap_streq(snap_dir.entries[i].name, name)) {
            snap_dir.entries[i].used = 0;
            snap_save_dir();
            return 0;
        }
    }
    return -1;
}

// List: copies name+timestamp of each used slot into out[]. Returns count.
int txfs_snap_list(char out[][64], uint32_t* timestamps, int max) {
    snap_load_dir();
    int count = 0;
    for (int i = 0; i < SNAP_MAX && count < max; i++) {
        if (snap_dir.entries[i].used) {
            snap_strcpy(out[count], snap_dir.entries[i].name, 64);
            if (timestamps) timestamps[count] = snap_dir.entries[i].timestamp;
            count++;
        }
    }
    return count;
}

// --- driver registration -----------------------------------------------------

static fs_driver_t txfs_driver = {
    .name    = "txfs",
    .mount   = txfs_mount_fn,
    .open    = txfs_open_fn,
    .close   = txfs_close_fn,
    .read    = txfs_read_fn,
    .write   = txfs_write_fn,
    .readdir = txfs_readdir_fn,
    .stat    = txfs_stat_fn,
    .mkdir   = txfs_mkdir_fn,
    .remove  = txfs_remove_fn,
    .isdir   = txfs_isdir_fn,
    .chmod   = txfs_chmod_fn,
    .getmode = txfs_getmode_fn,
};

void txfs_diskstats(uint32_t* total_kb, uint32_t* free_kb)
{
    *total_kb = (sb.total_blocks * TXFS_BLOCK_SIZE) / 1024;
    *free_kb  = (sb.free_blocks  * TXFS_BLOCK_SIZE) / 1024;
}

fs_driver_t* txfs_init()
{
    return &txfs_driver;
}
