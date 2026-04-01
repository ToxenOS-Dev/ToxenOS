// ToxenOS/kernel/ext2.c
// Ext2 filesystem driver
//
// Supports:
//   - Read files and directories
//   - Write / append to existing files
//   - Create files and directories
//   - Remove files
//   - Direct, single, double, and triple indirect blocks
//   - Inode and block bitmap allocation
//
// Ext2 disk layout:
//   Block 0        — boot block (unused)
//   Block 1        — superblock (always at byte offset 1024)
//   Block 2+       — block group descriptors
//   Then per block group: block bitmap, inode bitmap, inode table, data blocks

#include <stdint.h>
#include "../include/ext2.h"
#include "../include/ata.h"
#include "../include/vga.h"

// ── helpers ──────────────────────────────────────────────────────────────────

static int e2_strlen(const char* s) { int i=0; while(s[i]) i++; return i; }
static void e2_strcpy(char* d, const char* s, int max) {
    int i=0; while(s[i]&&i<max-1){d[i]=s[i];i++;} d[i]=0;
}
static int e2_strcmp(const char* a, const char* b) {
    int i; for(i=0;a[i]&&b[i];i++) if(a[i]!=b[i]) return 1; return a[i]!=b[i];
}
static void e2_memset(void* p, uint8_t v, uint32_t n) {
    uint8_t* b=(uint8_t*)p; for(uint32_t i=0;i<n;i++) b[i]=v;
}
static void e2_memcpy(void* d, const void* s, uint32_t n) {
    uint8_t* dd=(uint8_t*)d; const uint8_t* ss=(const uint8_t*)s;
    for(uint32_t i=0;i<n;i++) dd[i]=ss[i];
}

// ── on-disk structures ────────────────────────────────────────────────────────

typedef struct {
    uint32_t inodes_count;
    uint32_t blocks_count;
    uint32_t r_blocks_count;
    uint32_t free_blocks_count;
    uint32_t free_inodes_count;
    uint32_t first_data_block;
    uint32_t log_block_size;      // block size = 1024 << log_block_size
    uint32_t log_frag_size;
    uint32_t blocks_per_group;
    uint32_t frags_per_group;
    uint32_t inodes_per_group;
    uint32_t mtime;
    uint32_t wtime;
    uint16_t mnt_count;
    uint16_t max_mnt_count;
    uint16_t magic;               // 0xEF53
    uint16_t state;
    uint16_t errors;
    uint16_t minor_rev_level;
    uint32_t lastcheck;
    uint32_t checkinterval;
    uint32_t creator_os;
    uint32_t rev_level;
    uint16_t def_resuid;
    uint16_t def_resgid;
    // ext2 rev 1 fields
    uint32_t first_ino;
    uint16_t inode_size;
    uint16_t block_group_nr;
    uint32_t feature_compat;
    uint32_t feature_incompat;
    uint32_t feature_ro_compat;
    uint8_t  uuid[16];
    uint8_t  volume_name[16];
    uint8_t  last_mounted[64];
    uint32_t algo_bitmap;
    uint8_t  pad[820];
} __attribute__((packed)) ext2_superblock_t;

typedef struct {
    uint32_t block_bitmap;
    uint32_t inode_bitmap;
    uint32_t inode_table;
    uint16_t free_blocks_count;
    uint16_t free_inodes_count;
    uint16_t used_dirs_count;
    uint16_t pad;
    uint8_t  reserved[12];
} __attribute__((packed)) ext2_group_desc_t;

typedef struct {
    uint16_t mode;
    uint16_t uid;
    uint32_t size;
    uint32_t atime;
    uint32_t ctime;
    uint32_t mtime;
    uint32_t dtime;
    uint16_t gid;
    uint16_t links_count;
    uint32_t blocks;       // 512-byte blocks used
    uint32_t flags;
    uint32_t osd1;
    uint32_t block[15];    // 12 direct + 1 indirect + 1 dindirect + 1 tindirect
    uint32_t generation;
    uint32_t file_acl;
    uint32_t dir_acl;
    uint32_t faddr;
    uint8_t  osd2[12];
} __attribute__((packed)) ext2_inode_t;

typedef struct {
    uint32_t inode;
    uint16_t rec_len;
    uint8_t  name_len;
    uint8_t  file_type;
    char     name[255];
} __attribute__((packed)) ext2_dirent_t;

#define EXT2_MAGIC      0xEF53
#define EXT2_S_IFREG    0x8000
#define EXT2_S_IFDIR    0x4000
#define EXT2_FT_REG     1
#define EXT2_FT_DIR     2
#define EXT2_ROOT_INO   2

// ── filesystem state ──────────────────────────────────────────────────────────

typedef struct {
    ext2_superblock_t  sb;
    uint32_t           block_size;
    uint32_t           inodes_per_group;
    uint32_t           blocks_per_group;
    uint32_t           inode_size;
    uint32_t           groups_count;
    char               mountpoint[64];
    int                mounted;
    uint8_t            drive;
} ext2_fs_t;

static ext2_fs_t ext2_fs;

// ── block I/O ─────────────────────────────────────────────────────────────────

static int ext2_read_block(uint32_t block, uint8_t* buf)
{
    uint32_t sectors = ext2_fs.block_size / 512;
    return ata_read_drive(ext2_fs.drive, block * sectors, buf, sectors);
}

static int ext2_write_block(uint32_t block, const uint8_t* buf)
{
    uint32_t sectors = ext2_fs.block_size / 512;
    return ata_write_drive(ext2_fs.drive, block * sectors, buf, sectors);
}

// ── group descriptor ──────────────────────────────────────────────────────────

static int ext2_read_group_desc(uint32_t group, ext2_group_desc_t* gd)
{
    // Group descriptors start at block 2 (or block 1 if block_size > 1024)
    uint32_t gd_block  = (ext2_fs.block_size == 1024) ? 2 : 1;
    uint32_t gd_offset = group * sizeof(ext2_group_desc_t);
    uint32_t block     = gd_block + gd_offset / ext2_fs.block_size;
    uint32_t offset    = gd_offset % ext2_fs.block_size;

    static uint8_t buf[4096];
    if (ext2_read_block(block, buf) < 0) return -1;
    e2_memcpy(gd, buf + offset, sizeof(ext2_group_desc_t));
    return 0;
}

static int ext2_write_group_desc(uint32_t group, const ext2_group_desc_t* gd)
{
    uint32_t gd_block  = (ext2_fs.block_size == 1024) ? 2 : 1;
    uint32_t gd_offset = group * sizeof(ext2_group_desc_t);
    uint32_t block     = gd_block + gd_offset / ext2_fs.block_size;
    uint32_t offset    = gd_offset % ext2_fs.block_size;

    static uint8_t buf[4096];
    if (ext2_read_block(block, buf) < 0) return -1;
    e2_memcpy(buf + offset, gd, sizeof(ext2_group_desc_t));
    return ext2_write_block(block, buf);
}

// ── inode I/O ─────────────────────────────────────────────────────────────────

static int ext2_read_inode(uint32_t ino, ext2_inode_t* inode)
{
    uint32_t group  = (ino - 1) / ext2_fs.inodes_per_group;
    uint32_t index  = (ino - 1) % ext2_fs.inodes_per_group;

    ext2_group_desc_t gd;
    if (ext2_read_group_desc(group, &gd) < 0) return -1;

    uint32_t inode_table_block = gd.inode_table;
    uint32_t inode_offset      = index * ext2_fs.inode_size;
    uint32_t block             = inode_table_block + inode_offset / ext2_fs.block_size;
    uint32_t offset            = inode_offset % ext2_fs.block_size;

    static uint8_t buf[4096];
    if (ext2_read_block(block, buf) < 0) return -1;
    e2_memcpy(inode, buf + offset, sizeof(ext2_inode_t));
    return 0;
}

static int ext2_write_inode(uint32_t ino, const ext2_inode_t* inode)
{
    uint32_t group  = (ino - 1) / ext2_fs.inodes_per_group;
    uint32_t index  = (ino - 1) % ext2_fs.inodes_per_group;

    ext2_group_desc_t gd;
    if (ext2_read_group_desc(group, &gd) < 0) return -1;

    uint32_t inode_table_block = gd.inode_table;
    uint32_t inode_offset      = index * ext2_fs.inode_size;
    uint32_t block_num         = inode_table_block + inode_offset / ext2_fs.block_size;
    uint32_t offset            = inode_offset % ext2_fs.block_size;

    static uint8_t buf[4096];
    if (ext2_read_block(block_num, buf) < 0) return -1;
    e2_memcpy(buf + offset, inode, sizeof(ext2_inode_t));
    return ext2_write_block(block_num, buf);
}

// ── block allocation ──────────────────────────────────────────────────────────

static uint32_t ext2_alloc_block()
{
    static uint8_t bitmap[4096];

    for (uint32_t g = 0; g < ext2_fs.groups_count; g++) {
        ext2_group_desc_t gd;
        if (ext2_read_group_desc(g, &gd) < 0) continue;
        if (gd.free_blocks_count == 0) continue;

        if (ext2_read_block(gd.block_bitmap, bitmap) < 0) continue;

        for (uint32_t i = 0; i < ext2_fs.blocks_per_group; i++) {
            if (!((bitmap[i/8] >> (i%8)) & 1)) {
                bitmap[i/8] |= (1 << (i%8));
                ext2_write_block(gd.block_bitmap, bitmap);
                gd.free_blocks_count--;
                ext2_write_group_desc(g, &gd);
                ext2_fs.sb.free_blocks_count--;

                uint32_t blk = ext2_fs.sb.first_data_block +
                               g * ext2_fs.blocks_per_group + i;

                // Zero the new block
                e2_memset(bitmap, 0, ext2_fs.block_size);
                ext2_write_block(blk, bitmap);
                return blk;
            }
        }
    }
    return 0;
}

static void ext2_free_block(uint32_t blk)
{
    if (!blk) return;
    uint32_t g = (blk - ext2_fs.sb.first_data_block) / ext2_fs.blocks_per_group;
    uint32_t i = (blk - ext2_fs.sb.first_data_block) % ext2_fs.blocks_per_group;

    ext2_group_desc_t gd;
    if (ext2_read_group_desc(g, &gd) < 0) return;

    static uint8_t bitmap[4096];
    if (ext2_read_block(gd.block_bitmap, bitmap) < 0) return;
    bitmap[i/8] &= ~(1 << (i%8));
    ext2_write_block(gd.block_bitmap, bitmap);
    gd.free_blocks_count++;
    ext2_write_group_desc(g, &gd);
    ext2_fs.sb.free_blocks_count++;
}

// ── inode allocation ──────────────────────────────────────────────────────────

static uint32_t ext2_alloc_inode()
{
    static uint8_t bitmap[4096];

    for (uint32_t g = 0; g < ext2_fs.groups_count; g++) {
        ext2_group_desc_t gd;
        if (ext2_read_group_desc(g, &gd) < 0) continue;
        if (gd.free_inodes_count == 0) continue;

        if (ext2_read_block(gd.inode_bitmap, bitmap) < 0) continue;

        for (uint32_t i = 0; i < ext2_fs.inodes_per_group; i++) {
            if (!((bitmap[i/8] >> (i%8)) & 1)) {
                bitmap[i/8] |= (1 << (i%8));
                ext2_write_block(gd.inode_bitmap, bitmap);
                gd.free_inodes_count--;
                ext2_write_group_desc(g, &gd);
                ext2_fs.sb.free_inodes_count--;
                return g * ext2_fs.inodes_per_group + i + 1;
            }
        }
    }
    return 0;
}

static void ext2_free_inode(uint32_t ino)
{
    uint32_t group = (ino - 1) / ext2_fs.inodes_per_group;
    uint32_t index = (ino - 1) % ext2_fs.inodes_per_group;

    ext2_group_desc_t gd;
    if (ext2_read_group_desc(group, &gd) < 0) return;

    static uint8_t bitmap[4096];
    if (ext2_read_block(gd.inode_bitmap, bitmap) < 0) return;
    bitmap[index/8] &= ~(1 << (index%8));
    ext2_write_block(gd.inode_bitmap, bitmap);
    gd.free_inodes_count++;
    ext2_write_group_desc(group, &gd);
    ext2_fs.sb.free_inodes_count++;
}

// ── indirect block resolution ─────────────────────────────────────────────────

#define PTRS_PER_BLOCK (ext2_fs.block_size / 4)

static uint32_t ext2_get_block(ext2_inode_t* inode, uint32_t idx, int alloc)
{
    static uint8_t ibuf[4096], dibuf[4096], tibuf[4096];
    uint32_t ppb = ext2_fs.block_size / 4;

    // Direct blocks (0-11)
    if (idx < 12) {
        if (!inode->block[idx] && alloc) {
            inode->block[idx] = ext2_alloc_block();
        }
        return inode->block[idx];
    }
    idx -= 12;

    // Single indirect (12 .. 12+ppb-1)
    if (idx < ppb) {
        if (!inode->block[12] && alloc)
            inode->block[12] = ext2_alloc_block();
        if (!inode->block[12]) return 0;
        ext2_read_block(inode->block[12], ibuf);
        uint32_t* ptrs = (uint32_t*)ibuf;
        if (!ptrs[idx] && alloc) {
            ptrs[idx] = ext2_alloc_block();
            ext2_write_block(inode->block[12], ibuf);
        }
        return ptrs[idx];
    }
    idx -= ppb;

    // Double indirect
    if (idx < ppb * ppb) {
        if (!inode->block[13] && alloc)
            inode->block[13] = ext2_alloc_block();
        if (!inode->block[13]) return 0;
        ext2_read_block(inode->block[13], dibuf);
        uint32_t* l1 = (uint32_t*)dibuf;
        uint32_t  i1 = idx / ppb;
        uint32_t  i2 = idx % ppb;
        if (!l1[i1] && alloc) {
            l1[i1] = ext2_alloc_block();
            ext2_write_block(inode->block[13], dibuf);
        }
        if (!l1[i1]) return 0;
        ext2_read_block(l1[i1], ibuf);
        uint32_t* l2 = (uint32_t*)ibuf;
        if (!l2[i2] && alloc) {
            l2[i2] = ext2_alloc_block();
            ext2_write_block(l1[i1], ibuf);
        }
        return l2[i2];
    }
    idx -= ppb * ppb;

    // Triple indirect
    if (!inode->block[14] && alloc)
        inode->block[14] = ext2_alloc_block();
    if (!inode->block[14]) return 0;
    ext2_read_block(inode->block[14], tibuf);
    uint32_t* l1 = (uint32_t*)tibuf;
    uint32_t  i1 = idx / (ppb * ppb);
    uint32_t  i2 = (idx / ppb) % ppb;
    uint32_t  i3 = idx % ppb;
    if (!l1[i1] && alloc) {
        l1[i1] = ext2_alloc_block();
        ext2_write_block(inode->block[14], tibuf);
    }
    if (!l1[i1]) return 0;
    ext2_read_block(l1[i1], dibuf);
    uint32_t* l2 = (uint32_t*)dibuf;
    if (!l2[i2] && alloc) {
        l2[i2] = ext2_alloc_block();
        ext2_write_block(l1[i1], dibuf);
    }
    if (!l2[i2]) return 0;
    ext2_read_block(l2[i2], ibuf);
    uint32_t* l3 = (uint32_t*)ibuf;
    if (!l3[i3] && alloc) {
        l3[i3] = ext2_alloc_block();
        ext2_write_block(l2[i2], ibuf);
    }
    return l3[i3];
}

// ── path helpers ──────────────────────────────────────────────────────────────

static const char* ext2_strip_mount(const char* path)
{
    int i = 0;
    while (ext2_fs.mountpoint[i] && path[i] == ext2_fs.mountpoint[i]) i++;
    if (!path[i]) return "/";
    return path + i;
}

static void ext2_split_path(const char* path, char* parent, char* name)
{
    int len = e2_strlen(path);
    int slash = -1;
    for (int i = len-1; i >= 0; i--) if (path[i]=='/') { slash=i; break; }
    if (slash <= 0) {
        e2_strcpy(parent, "/", 256);
        const char* n = path; if(*n=='/') n++;
        e2_strcpy(name, n, 256);
    } else {
        for (int i=0; i<slash; i++) parent[i]=path[i];
        parent[slash]=0;
        e2_strcpy(name, path+slash+1, 256);
    }
}

// ── directory lookup ──────────────────────────────────────────────────────────

// Search directory inode for name, return child inode number or 0
static uint32_t ext2_dir_lookup(ext2_inode_t* dir, const char* name)
{
    static uint8_t buf[4096];
    uint32_t name_len = (uint32_t)e2_strlen(name);
    uint32_t offset   = 0;

    while (offset < dir->size) {
        uint32_t block_idx    = offset / ext2_fs.block_size;
        uint32_t block_offset = offset % ext2_fs.block_size;

        uint32_t blk = ext2_get_block(dir, block_idx, 0);
        if (!blk) break;
        ext2_read_block(blk, buf);

        while (block_offset < ext2_fs.block_size) {
            ext2_dirent_t* de = (ext2_dirent_t*)(buf + block_offset);
            if (!de->rec_len) break;
            if (de->inode && de->name_len == name_len) {
                int match = 1;
                for (uint32_t i = 0; i < name_len; i++)
                    if (de->name[i] != name[i]) { match=0; break; }
                if (match) return de->inode;
            }
            block_offset += de->rec_len;
            offset       += de->rec_len;
        }
        // align to next block if we didn't naturally get there
        uint32_t next_block = (block_idx + 1) * ext2_fs.block_size;
        if (offset < next_block) offset = next_block;
    }
    return 0;
}

// Walk full path, return inode number or 0
static uint32_t ext2_lookup(const char* path)
{
    if (!e2_strcmp(path, "/")) return EXT2_ROOT_INO;

    const char* p = path;
    if (*p == '/') p++;

    uint32_t cur_ino = EXT2_ROOT_INO;

    while (*p) {
        char component[256];
        int len = 0;
        while (p[len] && p[len] != '/') len++;
        for (int i=0; i<len; i++) component[i]=p[i];
        component[len] = 0;
        p += len;
        if (*p == '/') p++;

        ext2_inode_t inode;
        if (ext2_read_inode(cur_ino, &inode) < 0) return 0;
        if ((inode.mode & 0xF000) != EXT2_S_IFDIR) return 0;

        cur_ino = ext2_dir_lookup(&inode, component);
        if (!cur_ino) return 0;
    }
    return cur_ino;
}

// Append a dirent to a directory
static int ext2_dir_append(uint32_t dir_ino, ext2_inode_t* dir,
                           uint32_t child_ino, uint8_t ftype, const char* name)
{
    static uint8_t buf[4096];
    uint32_t name_len    = (uint32_t)e2_strlen(name);
    uint32_t needed      = (8 + name_len + 3) & ~3;  // aligned
    uint32_t offset      = 0;

    // Walk existing entries looking for slack space
    while (offset < dir->size) {
        uint32_t block_idx    = offset / ext2_fs.block_size;
        uint32_t block_offset = offset % ext2_fs.block_size;

        uint32_t blk = ext2_get_block(dir, block_idx, 1);
        if (!blk) return -1;
        ext2_read_block(blk, buf);

        while (block_offset + 8 <= ext2_fs.block_size) {
            ext2_dirent_t* de = (ext2_dirent_t*)(buf + block_offset);
            if (!de->rec_len) break;

            uint32_t actual = (8 + de->name_len + 3) & ~3;
            uint32_t slack  = de->rec_len - actual;

            if (slack >= needed) {
                // Shrink existing entry and add new one after it
                uint16_t old_rec = de->rec_len;
                de->rec_len      = (uint16_t)actual;

                ext2_dirent_t* ne = (ext2_dirent_t*)(buf + block_offset + actual);
                ne->inode     = child_ino;
                ne->rec_len   = (uint16_t)(old_rec - actual);
                ne->name_len  = (uint8_t)name_len;
                ne->file_type = ftype;
                for (uint32_t i=0; i<name_len; i++) ne->name[i] = name[i];

                ext2_write_block(blk, buf);
                return 0;
            }
            block_offset += de->rec_len;
            offset       += de->rec_len;
        }
        offset = (block_idx + 1) * ext2_fs.block_size;
    }

    // Need a new block for the directory
    uint32_t new_block_idx = dir->size / ext2_fs.block_size;
    uint32_t blk = ext2_get_block(dir, new_block_idx, 1);
    if (!blk) return -1;

    e2_memset(buf, 0, ext2_fs.block_size);
    ext2_dirent_t* de = (ext2_dirent_t*)buf;
    de->inode     = child_ino;
    de->rec_len   = (uint16_t)ext2_fs.block_size;
    de->name_len  = (uint8_t)name_len;
    de->file_type = ftype;
    for (uint32_t i=0; i<name_len; i++) de->name[i] = name[i];

    ext2_write_block(blk, buf);
    dir->size += ext2_fs.block_size;
    ext2_write_inode(dir_ino, dir);
    return 0;
}

// Remove a dirent by marking its inode as 0 and merging rec_len
static int ext2_dir_remove(uint32_t dir_ino, ext2_inode_t* dir, const char* name)
{
    static uint8_t buf[4096];
    uint32_t name_len = (uint32_t)e2_strlen(name);
    uint32_t offset   = 0;

    while (offset < dir->size) {
        uint32_t block_idx    = offset / ext2_fs.block_size;
        uint32_t block_offset = offset % ext2_fs.block_size;

        uint32_t blk = ext2_get_block(dir, block_idx, 0);
        if (!blk) break;
        ext2_read_block(blk, buf);

        ext2_dirent_t* prev = 0;
        while (block_offset < ext2_fs.block_size) {
            ext2_dirent_t* de = (ext2_dirent_t*)(buf + block_offset);
            if (!de->rec_len) break;
            if (de->inode && de->name_len == name_len) {
                int match = 1;
                for (uint32_t i=0; i<name_len; i++)
                    if (de->name[i] != name[i]) { match=0; break; }
                if (match) {
                    if (prev) prev->rec_len += de->rec_len;
                    else      de->inode = 0;
                    ext2_write_block(blk, buf);
                    (void)dir_ino;
                    return 0;
                }
            }
            prev = de;
            block_offset += de->rec_len;
            offset       += de->rec_len;
        }
        offset = (block_idx + 1) * ext2_fs.block_size;
    }
    return -1;
}

// ── open file descriptors ─────────────────────────────────────────────────────

#define EXT2_MAX_FDS 32

typedef struct {
    int          used;
    uint32_t     ino;
    ext2_inode_t inode;
    uint32_t     position;
} ext2_fd_t;

static ext2_fd_t ext2_fds[EXT2_MAX_FDS];

// ── mount ─────────────────────────────────────────────────────────────────────

static int ext2_mount_fn(const char* device)
{
    // device format: "N:/X:" where N=drive number, /X:=mountpoint
    ext2_fs.drive = (device && device[0] >= '0' && device[0] <= '9')
                    ? (uint8_t)(device[0] - '0') : ATA_DRIVE_SLAVE;
    if (device && device[1] == ':')
        e2_strcpy(ext2_fs.mountpoint, device + 2, 64);
    else
        e2_strcpy(ext2_fs.mountpoint, "/E:", 64);

    // Superblock is always at byte offset 1024 (LBA 2 on 512-byte sectors)
    static uint8_t buf[1024];
    if (ata_read_drive(ext2_fs.drive, 2, buf, 2) < 0) return -1;
    ext2_superblock_t* sb = (ext2_superblock_t*)buf;
    if (sb->magic != EXT2_MAGIC) return -1;

    e2_memcpy(&ext2_fs.sb, sb, sizeof(ext2_superblock_t));

    ext2_fs.block_size      = 1024 << sb->log_block_size;
    ext2_fs.inodes_per_group = sb->inodes_per_group;
    ext2_fs.blocks_per_group = sb->blocks_per_group;
    ext2_fs.inode_size      = (sb->rev_level >= 1) ? sb->inode_size : 128;
    ext2_fs.groups_count    = (sb->blocks_count + sb->blocks_per_group - 1)
                               / sb->blocks_per_group;
    ext2_fs.mounted = 1;

    for (int i=0; i<EXT2_MAX_FDS; i++) ext2_fds[i].used = 0;

    return 0;
}

// ── open ──────────────────────────────────────────────────────────────────────

static int ext2_open_fn(const char* path, int flags)
{
    if (!ext2_fs.mounted) return -1;
    const char* local = ext2_strip_mount(path);
    uint32_t ino = ext2_lookup(local);

    if (!ino) {
        if (!(flags & VFS_O_CREATE)) return -1;

        // Create new file
        ino = ext2_alloc_inode();
        if (!ino) return -1;

        ext2_inode_t new_inode;
        e2_memset(&new_inode, 0, sizeof(new_inode));
        new_inode.mode       = EXT2_S_IFREG | 0644;
        new_inode.links_count = 1;
        ext2_write_inode(ino, &new_inode);

        char parent_path[256], name[256];
        ext2_split_path(local, parent_path, name);
        uint32_t parent_ino = ext2_lookup(parent_path);
        if (!parent_ino) parent_ino = EXT2_ROOT_INO;

        ext2_inode_t parent;
        ext2_read_inode(parent_ino, &parent);
        ext2_dir_append(parent_ino, &parent, ino, EXT2_FT_REG, name);
    }

    for (int i=0; i<EXT2_MAX_FDS; i++) {
        if (!ext2_fds[i].used) {
            ext2_fds[i].used     = 1;
            ext2_fds[i].ino      = ino;
            ext2_fds[i].position = 0;
            ext2_read_inode(ino, &ext2_fds[i].inode);
            return i;
        }
    }
    return -1;
}

// ── close ─────────────────────────────────────────────────────────────────────

static int ext2_close_fn(int fd)
{
    if (!ext2_fs.mounted) return -1;
    if (fd<0||fd>=EXT2_MAX_FDS||!ext2_fds[fd].used) return -1;
    ext2_fds[fd].used = 0;
    return 0;
}

// ── read ──────────────────────────────────────────────────────────────────────

static int ext2_read_fn(int fd, uint8_t* buf, uint32_t size)
{
    if (!ext2_fs.mounted) return -1;
    if (fd<0||fd>=EXT2_MAX_FDS||!ext2_fds[fd].used) return -1;
    ext2_fd_t* f = &ext2_fds[fd];

    if (f->position >= f->inode.size) return 0;
    if (f->position + size > f->inode.size)
        size = f->inode.size - f->position;

    static uint8_t block_buf[4096];
    uint32_t done = 0;

    while (done < size) {
        uint32_t block_idx = (f->position + done) / ext2_fs.block_size;
        uint32_t block_off = (f->position + done) % ext2_fs.block_size;

        uint32_t blk = ext2_get_block(&f->inode, block_idx, 0);
        if (!blk) break;
        ext2_read_block(blk, block_buf);

        uint32_t can = ext2_fs.block_size - block_off;
        if (can > size - done) can = size - done;
        e2_memcpy(buf + done, block_buf + block_off, can);
        done += can;
    }

    f->position += done;
    return (int)done;
}

// ── write ─────────────────────────────────────────────────────────────────────

static int ext2_write_fn(int fd, const uint8_t* buf, uint32_t size)
{
    if (!ext2_fs.mounted) return -1;
    if (fd<0||fd>=EXT2_MAX_FDS||!ext2_fds[fd].used) return -1;
    ext2_fd_t* f = &ext2_fds[fd];

    static uint8_t block_buf[4096];
    uint32_t done = 0;

    while (done < size) {
        uint32_t block_idx = (f->position + done) / ext2_fs.block_size;
        uint32_t block_off = (f->position + done) % ext2_fs.block_size;

        uint32_t blk = ext2_get_block(&f->inode, block_idx, 1);
        if (!blk) break;

        ext2_read_block(blk, block_buf);
        uint32_t can = ext2_fs.block_size - block_off;
        if (can > size - done) can = size - done;
        e2_memcpy(block_buf + block_off, buf + done, can);
        ext2_write_block(blk, block_buf);
        done += can;
    }

    f->position  += done;
    if (f->position > f->inode.size) f->inode.size = f->position;
    ext2_write_inode(f->ino, &f->inode);
    return (int)done;
}

// ── readdir ───────────────────────────────────────────────────────────────────

static int ext2_readdir_fn(const char* path, char* out, uint32_t index)
{
    if (!ext2_fs.mounted) return -1;
    const char* local = ext2_strip_mount(path);
    uint32_t ino = ext2_lookup(local);
    if (!ino) return -1;

    ext2_inode_t inode;
    if (ext2_read_inode(ino, &inode) < 0) return -1;
    if ((inode.mode & 0xF000) != EXT2_S_IFDIR) return -1;

    static uint8_t buf[4096];
    uint32_t count  = 0;
    uint32_t offset = 0;

    while (offset < inode.size) {
        uint32_t block_idx = offset / ext2_fs.block_size;
        uint32_t block_off = offset % ext2_fs.block_size;

        uint32_t blk = ext2_get_block(&inode, block_idx, 0);
        if (!blk) break;
        ext2_read_block(blk, buf);

        while (block_off < ext2_fs.block_size) {
            ext2_dirent_t* de = (ext2_dirent_t*)(buf + block_off);
            if (!de->rec_len) break;
            if (de->inode && de->name[0] != '.') {
                if (count == index) {
                    for (int i=0; i<de->name_len; i++) out[i]=de->name[i];
                    out[de->name_len] = 0;
                    return 0;
                }
                count++;
            }
            block_off += de->rec_len;
            offset    += de->rec_len;
        }
        offset = (block_idx + 1) * ext2_fs.block_size;
    }
    return -1;
}

// ── stat ──────────────────────────────────────────────────────────────────────

static int ext2_stat_fn(const char* path, uint32_t* size)
{
    if (!ext2_fs.mounted) return -1;
    const char* local = ext2_strip_mount(path);
    uint32_t ino = ext2_lookup(local);
    if (!ino) return -1;

    ext2_inode_t inode;
    if (ext2_read_inode(ino, &inode) < 0) return -1;
    *size = inode.size;
    return 0;
}

// ── isdir ─────────────────────────────────────────────────────────────────────

static int ext2_isdir_fn(const char* path)
{
    print("[EXT2] isdir mounted="); print_hex(ext2_fs.mounted);
    print(" path="); print(path); print("\n");
    if (!ext2_fs.mounted) return -1;
    const char* local = ext2_strip_mount(path);
    print("[EXT2] local="); print(local); print("\n");
    if (!e2_strcmp(local, "/")) return 1;
    uint32_t ino = ext2_lookup(local);
    if (!ino) return -1;

    ext2_inode_t inode;
    if (ext2_read_inode(ino, &inode) < 0) return -1;
    return ((inode.mode & 0xF000) == EXT2_S_IFDIR) ? 1 : 0;
}

// ── mkdir ─────────────────────────────────────────────────────────────────────

static int ext2_mkdir_fn(const char* path)
{
    if (!ext2_fs.mounted) return -1;
    const char* local = ext2_strip_mount(path);
    if (ext2_lookup(local)) return -1;  // already exists

    uint32_t ino = ext2_alloc_inode();
    if (!ino) return -1;

    ext2_inode_t new_inode;
    e2_memset(&new_inode, 0, sizeof(new_inode));
    new_inode.mode        = EXT2_S_IFDIR | 0755;
    new_inode.links_count = 2;  // . and parent
    ext2_write_inode(ino, &new_inode);

    // Add . and .. entries
    ext2_dir_append(ino, &new_inode, ino,      EXT2_FT_DIR, ".");

    char parent_path[256], name[256];
    ext2_split_path(local, parent_path, name);
    uint32_t parent_ino = ext2_lookup(parent_path);
    if (!parent_ino) parent_ino = EXT2_ROOT_INO;

    ext2_dir_append(ino, &new_inode, parent_ino, EXT2_FT_DIR, "..");

    ext2_inode_t parent;
    ext2_read_inode(parent_ino, &parent);
    ext2_dir_append(parent_ino, &parent, ino, EXT2_FT_DIR, name);

    return 0;
}

// ── remove ────────────────────────────────────────────────────────────────────

static int ext2_remove_fn(const char* path)
{
    if (!ext2_fs.mounted) return -1;
    const char* local = ext2_strip_mount(path);
    uint32_t ino = ext2_lookup(local);
    if (!ino) return -1;

    ext2_inode_t inode;
    ext2_read_inode(ino, &inode);

    // Free all data blocks
    uint32_t block_count = (inode.size + ext2_fs.block_size - 1) / ext2_fs.block_size;
    for (uint32_t i=0; i<block_count; i++) {
        uint32_t blk = ext2_get_block(&inode, i, 0);
        if (blk) ext2_free_block(blk);
    }
    // Free indirect blocks themselves
    if (inode.block[12]) ext2_free_block(inode.block[12]);
    if (inode.block[13]) ext2_free_block(inode.block[13]);
    if (inode.block[14]) ext2_free_block(inode.block[14]);

    ext2_free_inode(ino);

    char parent_path[256], name[256];
    ext2_split_path(local, parent_path, name);
    uint32_t parent_ino = ext2_lookup(parent_path);
    if (!parent_ino) parent_ino = EXT2_ROOT_INO;

    ext2_inode_t parent;
    ext2_read_inode(parent_ino, &parent);
    ext2_dir_remove(parent_ino, &parent, name);

    return 0;
}

// ── driver registration ───────────────────────────────────────────────────────

static fs_driver_t ext2_driver = {
    .name    = "ext2",
    .mount   = ext2_mount_fn,
    .open    = ext2_open_fn,
    .close   = ext2_close_fn,
    .read    = ext2_read_fn,
    .write   = ext2_write_fn,
    .readdir = ext2_readdir_fn,
    .stat    = ext2_stat_fn,
    .mkdir   = ext2_mkdir_fn,
    .remove  = ext2_remove_fn,
    .isdir   = ext2_isdir_fn,
};

fs_driver_t* ext2_init()
{
    return &ext2_driver;
}
