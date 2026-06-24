#ifndef TXFS_H
#define TXFS_H

#include <stdint.h>
#include "../include/vfs.h"

#define TXFS_MAGIC          0x54584653  // "TXFS"
#define TXFS_VERSION        2           // v2: uint64_t size + triple indirect
#define TXFS_BLOCK_SIZE     4096
// 256 inodes still fits the existing 32-block inode table region
// (TXFS_BLOCK_INODES..TXFS_BLOCK_DATA = 32 blocks * 16 inodes/block = 512 max)
// — bumped from 128 because packaging both .nex and .elf for every bundled
// command (PACKAGE_ELF=1 dev builds) roughly doubles the file count.
#define TXFS_MAX_INODES     256
#define TXFS_DIRECT_BLOCKS  12

// block layout
#define TXFS_BLOCK_BOOT     0   // reserved
#define TXFS_BLOCK_SUPER    1   // superblock
#define TXFS_BLOCK_IBITMAP  2   // inode bitmap
#define TXFS_BLOCK_BBITMAP  3   // block bitmap
#define TXFS_BLOCK_INODES   4   // inode table starts here (32 blocks)
#define TXFS_BLOCK_DATA     36  // data blocks start here

// file types
#define TXFS_TYPE_FILE      0x1
#define TXFS_TYPE_DIR       0x2
#define TXFS_TYPE_SYMLINK   0x3

// permissions
#define TXFS_PERM_OWNER_R   0x100
#define TXFS_PERM_OWNER_W   0x080
#define TXFS_PERM_OWNER_X   0x040
#define TXFS_PERM_GROUP_R   0x020
#define TXFS_PERM_GROUP_W   0x010
#define TXFS_PERM_GROUP_X   0x008
#define TXFS_PERM_OTHER_R   0x004
#define TXFS_PERM_OTHER_W   0x002
#define TXFS_PERM_OTHER_X   0x001

typedef struct
{
    uint32_t magic;
    uint32_t version;
    uint32_t block_size;
    uint32_t total_blocks;
    uint32_t free_blocks;
    uint32_t total_inodes;
    uint32_t free_inodes;
    uint32_t root_inode;
    uint8_t  pad[4096 - 32];    // pad to block size
} __attribute__((packed)) txfs_superblock_t;

typedef struct
{
    uint32_t mode;              // type + permissions
    uint32_t uid;
    uint64_t size;              // v2: 64-bit size (16 exabyte max)
    uint32_t created;
    uint32_t modified;
    uint32_t links;
    uint32_t blocks[TXFS_DIRECT_BLOCKS];
    uint32_t indirect;
    uint32_t dindirect;
    uint32_t tindirect;         // v2: triple-indirect (4TB range)
    uint8_t  pad[256 - (4+4+8+4+4+4 + TXFS_DIRECT_BLOCKS*4 + 4+4+4)];
} __attribute__((packed)) txfs_inode_t;

typedef struct
{
    uint32_t inode;
    uint16_t name_len;
    uint8_t  type;
    char     name[256];
} __attribute__((packed)) txfs_dirent_t;

// open file handle
typedef struct
{
    int         used;
    uint32_t    inode_num;
    txfs_inode_t inode;
    uint32_t    position;
} txfs_fd_t;

void         txfs_set_lba_offset(uint32_t off);
fs_driver_t* txfs_init();
int          txfs_format(uint32_t total_blocks);
void         txfs_diskstats(uint32_t* total_kb, uint32_t* free_kb);
int          txfs_snap_create(const char* name, uint32_t timestamp);
int          txfs_snap_restore(const char* name);
int          txfs_snap_delete(const char* name);
int          txfs_snap_list(char out[][64], uint32_t* timestamps, int max);

#endif