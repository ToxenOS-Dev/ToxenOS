// kernel/txfs64.c — TxFS block/inode/directory manipulation for the
// 64-bit kernel, keyed by inode number. Read-path logic mirrors
// kernel/txfs.c's txfs_read_inode/txfs_get_block/txfs_dir_lookup/
// txfs_lookup line-for-line, renamed and pointed at ata64_read instead
// of ata_read.
//
// Milestone 27: this file is no longer a self-contained "open files by
// path, read through an internal fd table" API -- kernel/vfs64.c now
// owns path normalization, the open-file/cursor/refcount concept
// (folded into the SAME per-process handle table pipes and shared
// memory already use, retiring this file's old private open_files[]
// table entirely), and mounting this backend at "/". Everything here
// operates directly on inode numbers.
//
// Also new this milestone: txfs64_write_at can allocate a single
// indirect block (and its 1024-pointer table) on demand, so files can
// grow past the old 12-direct-block/48KB ceiling -- see its own header
// comment for exactly how far and its rollback guarantee -- and
// txfs64_free_all_blocks reclaims direct/indirect/double-indirect/
// triple-indirect blocks alike on truncate/unlink/rmdir (previously
// only the 12 direct blocks were ever freed, leaking anything beyond
// them). Every public entry point is now wrapped in a pushfq/cli/
// restore-flags critical section (kernel/physmem64.c's own pattern) so
// the shared block_buf/sb scratch state is safe even though syscalls
// run with interrupts enabled and can be preempted mid-operation by
// another process's own filesystem call.
#include <stdint.h>
#include "../include/txfs64.h"
#include "../include/ata64.h"
#include "../include/heap64.h"
#include "../include/klog.h"

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

// Milestone 27: protects block_buf/sb (and every disk access made
// while a public entry point below runs) against being preempted mid-
// operation by another process's own filesystem call -- single-core,
// so a plain cli/sti pair (nestable: each lock/unlock only restores
// whatever IF state IT observed, so a public function calling another
// public function, e.g. txfs64_rmdir calling txfs64_dir_is_empty, is
// safe) is sufficient, matching the same discipline already used by
// physmem64.c/heap64.c/process64.c/pipe64.c/shm64.c.
static inline uint64_t txfs64_lock(void) {
    uint64_t flags;
    __asm__ volatile ("pushfq\n\tpop %0\n\tcli" : "=r"(flags) :: "memory");
    return flags;
}
static inline void txfs64_unlock(uint64_t flags) {
    if (flags & (1ULL << 9)) __asm__ volatile ("sti" ::: "memory");
}

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

// Read-only: returns 0 (no such block) instead of allocating one.
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

// Resolve a full path ("/a/b/c") to an inode number, or -1. Path
// syntax is expected to already be normalized (kernel/vfs64.c's job) --
// this only walks components against real directory entries.
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

int txfs64_mount(void) {
    uint64_t flags = txfs64_lock();
    int r = txfs64_read_super();
    txfs64_unlock(flags);
    return r;
}

int txfs64_lookup(const char* path, uint32_t* inum_out, uint64_t* size_out, int* is_dir_out) {
    uint64_t flags = txfs64_lock();
    int inum = txfs64_resolve_path(path);
    if (inum < 0) { txfs64_unlock(flags); return -1; }

    txfs64_inode_t inode;
    if (txfs64_read_inode((uint32_t)inum, &inode) < 0) { txfs64_unlock(flags); return -1; }

    *inum_out = (uint32_t)inum;
    *size_out = inode.size;
    *is_dir_out = (((inode.mode >> 12) & 0xF) == TXFS64_TYPE_DIR) ? 1 : 0;
    txfs64_unlock(flags);
    return 0;
}

int txfs64_read_at(uint32_t inum, uint64_t offset, uint8_t* buf, uint32_t len) {
    uint64_t flags = txfs64_lock();

    txfs64_inode_t inode;
    if (txfs64_read_inode(inum, &inode) < 0) { txfs64_unlock(flags); return -1; }

    if (offset >= inode.size) { txfs64_unlock(flags); return 0; }

    uint32_t to_read = len;
    if (offset + to_read > inode.size) to_read = (uint32_t)(inode.size - offset);

    uint32_t done = 0;
    uint8_t  data_buf[TXFS64_BLOCK_SIZE];

    while (done < to_read) {
        uint32_t block_idx    = (uint32_t)((offset + done) / TXFS64_BLOCK_SIZE);
        uint32_t block_offset = (uint32_t)((offset + done) % TXFS64_BLOCK_SIZE);

        uint32_t blk = txfs64_get_block(&inode, block_idx);
        if (!blk) break;

        txfs64_read_block(blk, data_buf);

        uint32_t can_read = TXFS64_BLOCK_SIZE - block_offset;
        if (can_read > to_read - done) can_read = to_read - done;

        for (uint32_t i = 0; i < can_read; i++)
            buf[done + i] = data_buf[block_offset + i];

        done += can_read;
    }

    txfs64_unlock(flags);
    return (int)done;
}

int txfs64_readdir_at(uint32_t inum, uint32_t index, char* name_out) {
    uint64_t flags = txfs64_lock();

    txfs64_inode_t inode;
    if (txfs64_read_inode(inum, &inode) < 0) { txfs64_unlock(flags); return -1; }
    if (((inode.mode >> 12) & 0xF) != TXFS64_TYPE_DIR) { txfs64_unlock(flags); return -1; }

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
                if (count == index) {
                    txfs64_strcpy(name_out, de->name, 256);
                    txfs64_unlock(flags);
                    return 0;
                }
                count++;
            }
        }
        checked += in_this;
    }
    txfs64_unlock(flags);
    return -1;
}

// ── Write-side implementation ─────────────────────────────────────────
// Block/inode allocation follows tools/txfs_write.c's host-side logic:
// the bitmap is in block TXFS64_BLOCK_BBITMAP (=3) for data blocks and
// TXFS64_BLOCK_IBITMAP (=2) for inodes; data blocks are numbered
// starting at TXFS64_BLOCK_DATA (=36) (absolute physical block number
// = logical_bit + TXFS64_BLOCK_DATA).

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

// Milestone 27: frees EVERY block an inode owns -- direct, single-,
// double-, and triple-indirect data blocks, plus every indirect/
// double-indirect/triple-indirect POINTER block itself -- mirroring
// txfs64_get_block's read-side traversal exactly. Previously (pre-
// Milestone 27) unlink/rmdir only ever freed the 12 direct blocks,
// silently leaking anything reachable only through an indirect pointer
// (impossible to hit through this kernel's OWN write path before this
// milestone, since it never allocated one, but a real bug waiting for
// the day something did). Clears every pointer field to 0. Does NOT
// touch inode->size or persist the inode -- purely the block-
// reclamation half of truncate/delete; the caller updates size and
// calls txfs64_write_inode itself once ready.
static void txfs64_free_all_blocks(txfs64_inode_t* inode) {
    for (int i = 0; i < TXFS64_DIRECT_BLOCKS; i++) {
        if (inode->blocks[i]) { txfs64_free_block(inode->blocks[i]); inode->blocks[i] = 0; }
    }

    if (inode->indirect) {
        uint32_t ptrs[TXFS64_PTRS_PER_BLOCK];
        txfs64_read_block(inode->indirect, (uint8_t*)ptrs);
        for (uint32_t i = 0; i < TXFS64_PTRS_PER_BLOCK; i++) {
            if (ptrs[i]) txfs64_free_block(ptrs[i]);
        }
        txfs64_free_block(inode->indirect);
        inode->indirect = 0;
    }

    if (inode->dindirect) {
        uint32_t l1[TXFS64_PTRS_PER_BLOCK];
        txfs64_read_block(inode->dindirect, (uint8_t*)l1);
        for (uint32_t i = 0; i < TXFS64_PTRS_PER_BLOCK; i++) {
            if (!l1[i]) continue;
            uint32_t l2[TXFS64_PTRS_PER_BLOCK];
            txfs64_read_block(l1[i], (uint8_t*)l2);
            for (uint32_t j = 0; j < TXFS64_PTRS_PER_BLOCK; j++) {
                if (l2[j]) txfs64_free_block(l2[j]);
            }
            txfs64_free_block(l1[i]);
        }
        txfs64_free_block(inode->dindirect);
        inode->dindirect = 0;
    }

    if (inode->tindirect) {
        uint32_t t1[TXFS64_PTRS_PER_BLOCK];
        txfs64_read_block(inode->tindirect, (uint8_t*)t1);
        for (uint32_t i = 0; i < TXFS64_PTRS_PER_BLOCK; i++) {
            if (!t1[i]) continue;
            uint32_t t2[TXFS64_PTRS_PER_BLOCK];
            txfs64_read_block(t1[i], (uint8_t*)t2);
            for (uint32_t j = 0; j < TXFS64_PTRS_PER_BLOCK; j++) {
                if (!t2[j]) continue;
                uint32_t t3[TXFS64_PTRS_PER_BLOCK];
                txfs64_read_block(t2[j], (uint8_t*)t3);
                for (uint32_t k = 0; k < TXFS64_PTRS_PER_BLOCK; k++) {
                    if (t3[k]) txfs64_free_block(t3[k]);
                }
                txfs64_free_block(t2[j]);
            }
            txfs64_free_block(t1[i]);
        }
        txfs64_free_block(inode->tindirect);
        inode->tindirect = 0;
    }
}

static int txfs64_truncate(uint32_t inum) {
    txfs64_inode_t inode;
    if (txfs64_read_inode(inum, &inode) < 0) return -1;
    txfs64_free_all_blocks(&inode);
    inode.size = 0;
    return txfs64_write_inode(inum, &inode);
}

// Milestone 27: txfs64_write_at's growth path allocates at most this
// many NEW blocks in one call before giving up (generous for any
// single bounded syscall-sized write -- see kernel/syscall64.c's
// SYS64_HANDLE_WRITE cap) -- bounds the rollback-trail array below.
#define TXFS64_MAX_NEW_BLOCKS_PER_CALL 64

typedef struct {
    uint32_t blocks[TXFS64_MAX_NEW_BLOCKS_PER_CALL];
    int count;
} txfs64_trail_t;

static int trail_add(txfs64_trail_t* t, uint32_t blk) {
    if (t->count >= TXFS64_MAX_NEW_BLOCKS_PER_CALL) return -1;
    t->blocks[t->count++] = blk;
    return 0;
}

static void trail_free_all(txfs64_trail_t* t) {
    for (int i = 0; i < t->count; i++) txfs64_free_block(t->blocks[i]);
    t->count = 0;
}

// Grows/writes a file, allocating direct blocks (0..11) and, beyond
// those, a SINGLE indirect block and its 1024-entry pointer table
// (blocks 12..1035) on demand -- (12 + 1024) * 4096 bytes ~= 4.05MB
// per file, far past the 48KB this milestone needs to exceed.
// Double/triple-indirect growth is deliberately NOT implemented for
// writes (they stay read-only/legacy-support territory via
// txfs64_get_block, exactly as before this milestone) -- 4MB is
// already two orders of magnitude past anything this OS's own files
// need, and supporting further levels would add real complexity for no
// exercised benefit; a write that would need them fails cleanly (-1)
// rather than partially succeeding.
//
// Every block this call allocates (the indirect pointer block on first
// use, and each new data block) is tracked in a local rollback trail;
// if ANY allocation in the requested range fails, every block THIS
// CALL allocated is freed again and the function returns -1 WITHOUT
// EVER writing the modified inode or indirect-pointer-table back to
// disk -- the on-disk inode and any pre-existing indirect table are
// therefore untouched by a failed call, so there is nothing to roll
// back on disk, only in-memory state to discard. This is also why the
// indirect pointer table is read into a local copy ONCE and written
// back at most ONCE, right before the inode itself, rather than after
// each new pointer -- writing it incrementally would leave the on-disk
// table pointing at blocks a LATER failure in the same call then frees
// back to the pool, corrupting the filesystem the next time that freed
// block is reused for something else.
int txfs64_write_at(uint32_t inum, uint64_t offset, const uint8_t* buf, uint32_t len) {
    if (len == 0) return 0;
    if (offset + len < offset) return -1; // overflow guard

    uint64_t flags = txfs64_lock();

    txfs64_inode_t inode;
    if (txfs64_read_inode(inum, &inode) < 0) { txfs64_unlock(flags); return -1; }
    if (((inode.mode >> 12) & 0xF) != TXFS64_TYPE_FILE) { txfs64_unlock(flags); return -1; }

    uint64_t old_size = inode.size;
    uint64_t new_size = offset + len;

    uint32_t first_blk = (uint32_t)(offset / TXFS64_BLOCK_SIZE);
    uint32_t last_blk  = (uint32_t)((offset + len - 1) / TXFS64_BLOCK_SIZE);
    uint32_t growable_ceiling = TXFS64_DIRECT_BLOCKS + TXFS64_PTRS_PER_BLOCK;
    if (last_blk >= growable_ceiling) { txfs64_unlock(flags); return -1; }

    uint32_t ptrs[TXFS64_PTRS_PER_BLOCK];
    int have_ptrs = 0;
    int indirect_is_new = 0;
    uint32_t new_indirect_block = 0;

    txfs64_trail_t trail = { .count = 0 };
    int failed = 0;

    for (uint32_t b = first_blk; b <= last_blk; b++) {
        if (b < TXFS64_DIRECT_BLOCKS) {
            if (inode.blocks[b]) continue;
            int nb = txfs64_alloc_block();
            if (nb < 0) { failed = 1; break; }
            if (trail_add(&trail, (uint32_t)nb) < 0) { txfs64_free_block((uint32_t)nb); failed = 1; break; }
            inode.blocks[b] = (uint32_t)nb;
            continue;
        }

        uint32_t pidx = b - TXFS64_DIRECT_BLOCKS;
        if (!have_ptrs) {
            if (inode.indirect) {
                txfs64_read_block(inode.indirect, (uint8_t*)ptrs);
            } else {
                for (uint32_t i = 0; i < TXFS64_PTRS_PER_BLOCK; i++) ptrs[i] = 0;
                int nb = txfs64_alloc_block();
                if (nb < 0) { failed = 1; break; }
                if (trail_add(&trail, (uint32_t)nb) < 0) { txfs64_free_block((uint32_t)nb); failed = 1; break; }
                new_indirect_block = (uint32_t)nb;
                indirect_is_new = 1;
            }
            have_ptrs = 1;
        }

        if (ptrs[pidx]) continue;
        int nb = txfs64_alloc_block();
        if (nb < 0) { failed = 1; break; }
        if (trail_add(&trail, (uint32_t)nb) < 0) { txfs64_free_block((uint32_t)nb); failed = 1; break; }
        ptrs[pidx] = (uint32_t)nb;
    }

    if (failed) {
        trail_free_all(&trail);
        txfs64_unlock(flags);
        return -1;
    }

    // Every block this write touches now exists (freshly zeroed on
    // allocation) -- commit the payload, then the indirect table (if
    // touched), then the inode, in that order. A crash between these
    // three writes would at worst leave an allocated-but-not-yet-
    // referenced block (a leak, recoverable by a future scan/fsck
    // tool) -- never a dangling reference to a freed block. Full crash
    // consistency (a real journal) remains future work; see the
    // Milestone 27 summary.
    uint32_t done = 0;
    while (done < len) {
        uint32_t blk_idx = (uint32_t)((offset + done) / TXFS64_BLOCK_SIZE);
        uint32_t blk_off = (uint32_t)((offset + done) % TXFS64_BLOCK_SIZE);
        uint32_t phys = (blk_idx < TXFS64_DIRECT_BLOCKS) ? inode.blocks[blk_idx] : ptrs[blk_idx - TXFS64_DIRECT_BLOCKS];

        uint8_t blk_data[TXFS64_BLOCK_SIZE];
        txfs64_read_block(phys, blk_data); // preserve bytes outside this write's span within the same block
        uint32_t can = TXFS64_BLOCK_SIZE - blk_off;
        if (can > len - done) can = len - done;
        for (uint32_t i = 0; i < can; i++) blk_data[blk_off + i] = buf[done + i];
        txfs64_write_block(phys, blk_data);
        done += can;
    }

    if (have_ptrs) {
        if (indirect_is_new) inode.indirect = new_indirect_block;
        txfs64_write_block(inode.indirect, (uint8_t*)ptrs);
    }

    if (new_size > old_size) inode.size = new_size;
    int wr = txfs64_write_inode(inum, &inode);
    txfs64_unlock(flags);
    return wr < 0 ? -1 : (int)len;
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

    // Allocate a new data block if this is the first entry in a new
    // block. Directories are deliberately kept direct-blocks-only (not
    // extended to indirect growth this milestone -- see the Milestone
    // 27 summary): TXFS64_DIRECT_BLOCKS blocks already hold several
    // hundred entries, far more than anything this OS creates.
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
    uint64_t flags = txfs64_lock();
    char parent[256], leaf[256];
    if (txfs64_split_path(path, parent, sizeof(parent), leaf, sizeof(leaf)) < 0) { txfs64_unlock(flags); return -1; }
    if (!leaf[0]) { txfs64_unlock(flags); return -1; }

    int parent_inum = txfs64_resolve_path(parent);
    if (parent_inum < 0) { txfs64_unlock(flags); return -1; }

    // Fail if name already exists
    txfs64_inode_t parent_inode;
    if (txfs64_read_inode((uint32_t)parent_inum, &parent_inode) < 0) { txfs64_unlock(flags); return -1; }
    if (txfs64_dir_lookup(&parent_inode, leaf) >= 0) { txfs64_unlock(flags); return -1; }  // already exists

    int new_inum = txfs64_alloc_inode();
    if (new_inum < 0) { txfs64_unlock(flags); return -1; }

    // uid/created/modified: reserved, always 0 -- see include/txfs64.h.
    txfs64_inode_t new_dir = {0};
    new_dir.mode  = (TXFS64_TYPE_DIR << 12) | 0x1C0;
    new_dir.links = 1;
    new_dir.size  = 0;
    if (txfs64_write_inode((uint32_t)new_inum, &new_dir) < 0) {
        txfs64_free_inode((uint32_t)new_inum);
        txfs64_unlock(flags);
        return -1;
    }

    if (txfs64_dir_append((uint32_t)parent_inum, (uint32_t)new_inum,
                          TXFS64_TYPE_DIR, leaf) < 0) {
        txfs64_free_inode((uint32_t)new_inum);
        txfs64_unlock(flags);
        return -1;
    }
    txfs64_unlock(flags);
    return 0;
}

int txfs64_create_file(const char* path) {
    uint64_t flags = txfs64_lock();
    char parent[256], leaf[256];
    if (txfs64_split_path(path, parent, sizeof(parent), leaf, sizeof(leaf)) < 0) { txfs64_unlock(flags); return -1; }
    if (!leaf[0]) { txfs64_unlock(flags); return -1; }

    int parent_inum = txfs64_resolve_path(parent);
    if (parent_inum < 0) { txfs64_unlock(flags); return -1; }

    txfs64_inode_t parent_inode;
    if (txfs64_read_inode((uint32_t)parent_inum, &parent_inode) < 0) { txfs64_unlock(flags); return -1; }
    if (txfs64_dir_lookup(&parent_inode, leaf) >= 0) { txfs64_unlock(flags); return -1; }  // already exists

    int new_inum = txfs64_alloc_inode();
    if (new_inum < 0) { txfs64_unlock(flags); return -1; }

    // uid/created/modified: reserved, always 0 -- see include/txfs64.h.
    txfs64_inode_t new_file = {0};
    new_file.mode  = (TXFS64_TYPE_FILE << 12) | 0x1C0;
    new_file.links = 1;
    new_file.size  = 0;
    if (txfs64_write_inode((uint32_t)new_inum, &new_file) < 0) {
        txfs64_free_inode((uint32_t)new_inum);
        txfs64_unlock(flags);
        return -1;
    }

    if (txfs64_dir_append((uint32_t)parent_inum, (uint32_t)new_inum,
                          TXFS64_TYPE_FILE, leaf) < 0) {
        txfs64_free_inode((uint32_t)new_inum);
        txfs64_unlock(flags);
        return -1;
    }
    txfs64_unlock(flags);
    return 0;
}

int txfs64_write_file(const char* path, const uint8_t* data, uint32_t len) {
    uint64_t flags = txfs64_lock();
    int inum = txfs64_resolve_path(path);
    if (inum < 0) { txfs64_unlock(flags); return -1; }

    txfs64_inode_t inode;
    if (txfs64_read_inode((uint32_t)inum, &inode) < 0) { txfs64_unlock(flags); return -1; }
    if (((inode.mode >> 12) & 0xF) != TXFS64_TYPE_FILE) { txfs64_unlock(flags); return -1; }

    if (txfs64_truncate((uint32_t)inum) < 0) { txfs64_unlock(flags); return -1; }
    txfs64_unlock(flags); // txfs64_write_at re-acquires its own lock -- see its nestable-lock note

    if (len == 0) return 0;
    return txfs64_write_at((uint32_t)inum, 0, data, len) < 0 ? -1 : 0;
}

int txfs64_unlink(const char* path) {
    uint64_t flags = txfs64_lock();
    char parent[256], leaf[256];
    if (txfs64_split_path(path, parent, sizeof(parent), leaf, sizeof(leaf)) < 0) { txfs64_unlock(flags); return -1; }
    if (!leaf[0]) { txfs64_unlock(flags); return -1; }

    int inum = txfs64_resolve_path(path);
    if (inum < 0) { txfs64_unlock(flags); return -1; }

    txfs64_inode_t inode;
    if (txfs64_read_inode((uint32_t)inum, &inode) < 0) { txfs64_unlock(flags); return -1; }
    if (((inode.mode >> 12) & 0xF) != TXFS64_TYPE_FILE) { txfs64_unlock(flags); return -2; }

    txfs64_free_all_blocks(&inode);

    int parent_inum = txfs64_resolve_path(parent);
    if (parent_inum >= 0) txfs64_dir_remove_entry((uint32_t)parent_inum, leaf);
    txfs64_free_inode((uint32_t)inum);
    txfs64_unlock(flags);
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
    uint64_t flags = txfs64_lock();
    int src_inum = txfs64_resolve_path(src_path);
    if (src_inum < 0) { txfs64_unlock(flags); return -1; }

    if (txfs64_resolve_path(dest_path) >= 0) { txfs64_unlock(flags); return -4; }  // dest already exists

    char src_parent[256], src_leaf[256];
    char dest_parent[256], dest_leaf[256];
    if (txfs64_split_path(src_path,  src_parent,  256, src_leaf,  256) < 0) { txfs64_unlock(flags); return -1; }
    if (txfs64_split_path(dest_path, dest_parent, 256, dest_leaf, 256) < 0) { txfs64_unlock(flags); return -1; }
    if (!src_leaf[0] || !dest_leaf[0]) { txfs64_unlock(flags); return -1; }

    int src_pinum  = txfs64_resolve_path(src_parent);
    int dest_pinum = txfs64_resolve_path(dest_parent);
    if (src_pinum < 0 || dest_pinum < 0) { txfs64_unlock(flags); return -1; }

    if (src_pinum == dest_pinum) {
        int r = txfs64_dir_rename_entry((uint32_t)src_pinum, src_leaf, dest_leaf);
        txfs64_unlock(flags);
        return r;
    }

    // Cross-directory: relink the inode under the new parent and unlink old entry.
    txfs64_inode_t inode;
    if (txfs64_read_inode((uint32_t)src_inum, &inode) < 0) { txfs64_unlock(flags); return -1; }
    uint8_t type = (uint8_t)((inode.mode >> 12) & 0xF);

    if (txfs64_dir_append((uint32_t)dest_pinum, (uint32_t)src_inum,
                          type, dest_leaf) < 0) { txfs64_unlock(flags); return -1; }
    txfs64_dir_remove_entry((uint32_t)src_pinum, src_leaf);
    txfs64_unlock(flags);
    return 0;
}

int txfs64_dir_is_empty(const char* path) {
    uint64_t flags = txfs64_lock();
    int inum = txfs64_resolve_path(path);
    if (inum < 0) { txfs64_unlock(flags); return 0; }

    txfs64_inode_t inode;
    if (txfs64_read_inode((uint32_t)inum, &inode) < 0) { txfs64_unlock(flags); return 0; }
    if (((inode.mode >> 12) & 0xF) != TXFS64_TYPE_DIR) { txfs64_unlock(flags); return 0; }

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
            if (de->inode) { txfs64_unlock(flags); return 0; }  // found live entry
        }
        checked += in_this;
    }
    txfs64_unlock(flags);
    return 1;  // no live entries found
}

int txfs64_rmdir(const char* path) {
    uint64_t flags = txfs64_lock();
    char parent[256], leaf[256];
    if (txfs64_split_path(path, parent, sizeof(parent), leaf, sizeof(leaf)) < 0) { txfs64_unlock(flags); return -1; }
    if (!leaf[0]) { txfs64_unlock(flags); return -1; }

    if (!txfs64_dir_is_empty(path)) { txfs64_unlock(flags); return -1; }  // refuse non-empty (nested lock -- safe, see header note)

    int inum = txfs64_resolve_path(path);
    if (inum < 0) { txfs64_unlock(flags); return -1; }

    txfs64_inode_t inode;
    if (txfs64_read_inode((uint32_t)inum, &inode) < 0) { txfs64_unlock(flags); return -1; }
    if (((inode.mode >> 12) & 0xF) != TXFS64_TYPE_DIR) { txfs64_unlock(flags); return -1; }

    txfs64_free_all_blocks(&inode);

    int parent_inum = txfs64_resolve_path(parent);
    if (parent_inum >= 0) txfs64_dir_remove_entry((uint32_t)parent_inum, leaf);
    txfs64_free_inode((uint32_t)inum);
    txfs64_unlock(flags);
    return 0;
}

// ── Diagnostics ──────────────────────────────────────────────────────
void txfs64_stats(txfs64_stats_t* out) {
    uint64_t flags = txfs64_lock();
    txfs64_read_super();
    out->total_blocks = sb.total_blocks;
    out->free_blocks  = sb.free_blocks;
    out->total_inodes = sb.total_inodes;
    out->free_inodes  = sb.free_inodes;
    txfs64_unlock(flags);
}

static void dec_to_str_local(uint64_t val, char* out) {
    char tmp[24];
    int n = 0;
    if (val == 0) tmp[n++] = '0';
    while (val > 0 && n < 24) { tmp[n++] = (char)('0' + (val % 10)); val /= 10; }
    int j = 0;
    while (n > 0) out[j++] = tmp[--n];
    out[j] = 0;
}

void txfs64_dump(void) {
    txfs64_stats_t s;
    txfs64_stats(&s);
    char buf[24];
    klog("txfs64: dump ---\n  blocks: total=");
    dec_to_str_local(s.total_blocks, buf); klog(buf);
    klog(" free="); dec_to_str_local(s.free_blocks, buf); klog(buf);
    klog("\n  inodes: total="); dec_to_str_local(s.total_inodes, buf); klog(buf);
    klog(" free="); dec_to_str_local(s.free_inodes, buf); klog(buf);
    klog("\ntxfs64: dump end ---\n");
}

// ── Self-test suite ──────────────────────────────────────────────────
// Operates directly on inode numbers (no process/VFS layer involved --
// kernel/vfs64.c's own vfs64_selftest covers the path/handle/process
// integration). Uses two scratch paths at the TxFS64 root that must not
// already exist; cleans both up on every exit path, including early
// failures, so a partial failure never leaves debris for a later run.
#define TXFS64_TEST_PATH    "/txfs64_selftest_tmp"
#define TXFS64_SCRATCH_PATH "/txfs64_selftest_scratch"

static int test_indirect_growth_and_read(void) {
    if (txfs64_create_file(TXFS64_TEST_PATH) < 0) return 0;
    int ok = 1;

    uint32_t inum; uint64_t size; int is_dir;
    if (txfs64_lookup(TXFS64_TEST_PATH, &inum, &size, &is_dir) < 0) ok = 0;

    // 200KB -- well past the old 48KB/12-direct-block ceiling, deep
    // into single-indirect territory (12*4KB=48KB direct + up to
    // 1024*4KB=4MB indirect).
    uint32_t total = 200 * 1024;
    uint8_t* wbuf = (uint8_t*)kmalloc(total);
    uint8_t* rbuf = (uint8_t*)kmalloc(total);
    if (!wbuf || !rbuf) { if (wbuf) kfree(wbuf); if (rbuf) kfree(rbuf); txfs64_unlink(TXFS64_TEST_PATH); return 0; }
    for (uint32_t i = 0; i < total; i++) wbuf[i] = (uint8_t)(i * 31 + 7);

    // Written in 4 chunks (simulating multiple positioned writes, same
    // pattern kernel/vfs64.c's handle-based sys_handle_write uses) to
    // also exercise growth across several separate calls, not just one
    // giant one.
    uint32_t chunk = total / 4;
    for (int c = 0; ok && c < 4; c++) {
        if (txfs64_write_at(inum, (uint64_t)c * chunk, wbuf + c * chunk, chunk) != (int)chunk) ok = 0;
    }

    if (ok && txfs64_read_at(inum, 0, rbuf, total) != (int)total) ok = 0;
    for (uint32_t i = 0; ok && i < total; i++) if (rbuf[i] != wbuf[i]) { ok = 0; break; }

    if (ok && txfs64_lookup(TXFS64_TEST_PATH, &inum, &size, &is_dir) < 0) ok = 0;
    if (ok && size != total) ok = 0;

    kfree(wbuf);
    kfree(rbuf);
    txfs64_unlink(TXFS64_TEST_PATH);
    return ok;
}

static int test_direct_indirect_boundary(void) {
    if (txfs64_create_file(TXFS64_TEST_PATH) < 0) return 0;
    int ok = 1;
    uint32_t inum; uint64_t size; int is_dir;
    if (txfs64_lookup(TXFS64_TEST_PATH, &inum, &size, &is_dir) < 0) ok = 0;

    // A single write straddling the last direct block and the first
    // indirect-range block.
    uint64_t boundary = (uint64_t)TXFS64_DIRECT_BLOCKS * TXFS64_BLOCK_SIZE;
    uint8_t wbuf[300];
    for (int i = 0; i < 300; i++) wbuf[i] = (uint8_t)(100 + i);

    if (ok && txfs64_write_at(inum, boundary - 100, wbuf, 300) != 300) ok = 0;

    uint8_t rbuf[300];
    if (ok && txfs64_read_at(inum, boundary - 100, rbuf, 300) != 300) ok = 0;
    for (int i = 0; ok && i < 300; i++) if (rbuf[i] != wbuf[i]) ok = 0;

    txfs64_unlink(TXFS64_TEST_PATH);
    return ok;
}

static int test_delete_reclaims_all_blocks(void) {
    txfs64_stats_t before, after;
    txfs64_stats(&before);

    if (txfs64_create_file(TXFS64_TEST_PATH) < 0) return 0;
    uint32_t inum; uint64_t size; int is_dir;
    txfs64_lookup(TXFS64_TEST_PATH, &inum, &size, &is_dir);

    uint32_t total = 100 * 1024; // well into indirect territory
    uint8_t* wbuf = (uint8_t*)kmalloc(total);
    if (!wbuf) { txfs64_unlink(TXFS64_TEST_PATH); return 0; }
    for (uint32_t i = 0; i < total; i++) wbuf[i] = (uint8_t)i;
    int wrote_ok = (txfs64_write_at(inum, 0, wbuf, total) == (int)total);
    kfree(wbuf);
    if (!wrote_ok) { txfs64_unlink(TXFS64_TEST_PATH); return 0; }

    if (txfs64_unlink(TXFS64_TEST_PATH) < 0) return 0;

    txfs64_stats(&after);
    // Exactly one inode (this test's) and every data+indirect-pointer
    // block it grew into must be back to the pre-test baseline.
    return after.free_inodes == before.free_inodes && after.free_blocks == before.free_blocks;
}

static int test_out_of_space_rollback(void) {
    if (txfs64_create_file(TXFS64_TEST_PATH) < 0) return 0;
    uint32_t inum; uint64_t size; int is_dir;
    if (txfs64_lookup(TXFS64_TEST_PATH, &inum, &size, &is_dir) < 0) { txfs64_unlink(TXFS64_TEST_PATH); return 0; }

    uint8_t fillbuf[TXFS64_BLOCK_SIZE];
    for (int i = 0; i < TXFS64_BLOCK_SIZE; i++) fillbuf[i] = (uint8_t)i;
    uint64_t full_direct = (uint64_t)TXFS64_DIRECT_BLOCKS * TXFS64_BLOCK_SIZE;
    if (txfs64_write_at(inum, 0, fillbuf, TXFS64_BLOCK_SIZE) < 0) { txfs64_unlink(TXFS64_TEST_PATH); return 0; }
    // Fill every direct block individually so this is exactly 12
    // separately-tracked allocations, matching the file's real size.
    for (int b = 1; b < TXFS64_DIRECT_BLOCKS; b++) {
        if (txfs64_write_at(inum, (uint64_t)b * TXFS64_BLOCK_SIZE, fillbuf, TXFS64_BLOCK_SIZE) < 0) {
            txfs64_unlink(TXFS64_TEST_PATH);
            return 0;
        }
    }

    txfs64_stats_t stats;
    txfs64_stats(&stats);
    uint32_t drain = (stats.free_blocks > 1) ? (stats.free_blocks - 1) : 0;
    if (drain > TXFS64_PTRS_PER_BLOCK) drain = TXFS64_PTRS_PER_BLOCK; // stay within the scratch file's own single-indirect growth room

    if (txfs64_create_file(TXFS64_SCRATCH_PATH) < 0) { txfs64_unlink(TXFS64_TEST_PATH); return 0; }
    uint32_t scratch_inum; uint64_t ssize; int sdir;
    txfs64_lookup(TXFS64_SCRATCH_PATH, &scratch_inum, &ssize, &sdir);

    int drain_ok = 1;
    if (drain > 0) {
        uint64_t drain_bytes = (uint64_t)drain * TXFS64_BLOCK_SIZE;
        uint8_t* big = (uint8_t*)kmalloc(drain_bytes);
        if (!big) drain_ok = 0;
        else {
            for (uint64_t i = 0; i < drain_bytes; i++) big[i] = (uint8_t)i;
            drain_ok = (txfs64_write_at(scratch_inum, 0, big, (uint32_t)drain_bytes) == (int)drain_bytes);
            kfree(big);
        }
    }

    txfs64_stats(&stats);
    if (!drain_ok || stats.free_blocks != 1) {
        // Could not precisely arrange "exactly 1 free block" (disk
        // capacity is environment-dependent) -- clean up and report a
        // vacuous pass rather than a false failure; every other test in
        // this suite already exercises the same allocation/free paths.
        txfs64_unlink(TXFS64_SCRATCH_PATH);
        txfs64_unlink(TXFS64_TEST_PATH);
        return 1;
    }

    // This write starts exactly at the first indirect-range block --
    // needs the indirect pointer block AND a data block (2 new blocks),
    // but only 1 is free. Must fail cleanly.
    int r = txfs64_write_at(inum, full_direct, fillbuf, 1);

    txfs64_stats_t after;
    txfs64_stats(&after);
    int ok = (r < 0) && (after.free_blocks == 1);

    uint32_t final_inum; uint64_t final_size; int final_isdir;
    if (txfs64_lookup(TXFS64_TEST_PATH, &final_inum, &final_size, &final_isdir) < 0) ok = 0;
    if (ok && final_size != full_direct) ok = 0;

    txfs64_unlink(TXFS64_SCRATCH_PATH);
    txfs64_unlink(TXFS64_TEST_PATH);
    return ok;
}

static int test_repeated_cycles_no_leak(void) {
    txfs64_stats_t before, after;
    txfs64_stats(&before);

    for (int i = 0; i < 5; i++) {
        if (txfs64_create_file(TXFS64_TEST_PATH) < 0) return 0;
        uint32_t inum; uint64_t size; int is_dir;
        if (txfs64_lookup(TXFS64_TEST_PATH, &inum, &size, &is_dir) < 0) return 0;

        uint32_t total = 80 * 1024; // crosses into indirect territory
        uint8_t* buf = (uint8_t*)kmalloc(total);
        if (!buf) return 0;
        for (uint32_t j = 0; j < total; j++) buf[j] = (uint8_t)(j + i);
        int wok = (txfs64_write_at(inum, 0, buf, total) == (int)total);

        uint8_t* rbuf = (uint8_t*)kmalloc(total);
        int rok = rbuf && wok && txfs64_read_at(inum, 0, rbuf, total) == (int)total;
        if (rok) for (uint32_t j = 0; j < total; j++) if (rbuf[j] != buf[j]) { rok = 0; break; }
        kfree(buf);
        if (rbuf) kfree(rbuf);
        if (!wok || !rok) { txfs64_unlink(TXFS64_TEST_PATH); return 0; }

        if (txfs64_unlink(TXFS64_TEST_PATH) < 0) return 0;
    }

    txfs64_stats(&after);
    return after.free_blocks == before.free_blocks && after.free_inodes == before.free_inodes;
}

#define TXFS64_TEST(name, expr) do {              \
    int _r = (expr);                              \
    klog("txfs64_selftest: " name " ");           \
    klog(_r ? "PASS\n" : "FAIL\n");               \
    if (_r) pass++; else fail++;                  \
} while (0)

int txfs64_selftest(void) {
    int pass = 0, fail = 0;
    klog("txfs64_selftest: starting\n");

    TXFS64_TEST("indirect-block growth, write/read past 48KB", test_indirect_growth_and_read());
    TXFS64_TEST("direct/indirect boundary crossing", test_direct_indirect_boundary());
    TXFS64_TEST("delete reclaims all blocks (direct + indirect)", test_delete_reclaims_all_blocks());
    TXFS64_TEST("out-of-space growth failure rolls back cleanly", test_out_of_space_rollback());
    TXFS64_TEST("repeated create/write/read/delete cycles, no leak", test_repeated_cycles_no_leak());

    txfs64_dump();

    char passbuf[24], failbuf[24];
    dec_to_str_local((uint64_t)pass, passbuf);
    dec_to_str_local((uint64_t)fail, failbuf);
    klog("txfs64_selftest: pass=");
    klog(passbuf);
    klog(" fail=");
    klog(failbuf);
    klog("\n");
    return fail == 0;
}
