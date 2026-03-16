#include <stdint.h>
#include "../include/txfs.h"
#include "../include/ata.h"
#include "../include/mm.h"
#include "../include/vga.h"

static char txfs_mountpoint[64] = "/disk";

static const char* txfs_strip_mount(const char* path)
{
    int i = 0;
    while (txfs_mountpoint[i] && path[i] == txfs_mountpoint[i]) i++;
    if (!path[i]) return "/";
    return path + i;
}

// ─── block I/O ───────────────────────────────────────────────────────────────

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

// ─── superblock ──────────────────────────────────────────────────────────────

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

// ─── bitmap helpers ──────────────────────────────────────────────────────────

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

// ─── inode I/O ───────────────────────────────────────────────────────────────

// 16 inodes per block (4096 / 256 = 16)
#define INODES_PER_BLOCK    16

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

// ─── block allocation ────────────────────────────────────────────────────────

static int txfs_alloc_block()
{
    if (txfs_read_block(TXFS_BLOCK_BBITMAP, block_bitmap) < 0) return -1;

    int b = bitmap_alloc(block_bitmap, sb.total_blocks);
    if (b < 0) return -1;

    txfs_write_block(TXFS_BLOCK_BBITMAP, block_bitmap);
    sb.free_blocks--;
    txfs_write_super();
    return b + TXFS_BLOCK_DATA;
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

// ─── format ──────────────────────────────────────────────────────────────────

int txfs_format(uint32_t total_blocks)
{
    // write superblock
    uint8_t* p = (uint8_t*)&sb;
    for (uint32_t i = 0; i < sizeof(sb); i++) p[i] = 0;

    sb.magic        = TXFS_MAGIC;
    sb.version      = TXFS_VERSION;
    sb.block_size   = TXFS_BLOCK_SIZE;
    sb.total_blocks = total_blocks - TXFS_BLOCK_DATA;
    sb.free_blocks  = sb.total_blocks;
    sb.total_inodes = TXFS_MAX_INODES;
    sb.free_inodes  = TXFS_MAX_INODES - 1;  // root uses inode 0
    sb.root_inode   = 0;

    txfs_write_super();

    // clear bitmaps
    for (int i = 0; i < TXFS_BLOCK_SIZE; i++)
        inode_bitmap[i] = block_bitmap[i] = 0;

    // mark root inode as used
    bitmap_set(inode_bitmap, 0);

    txfs_write_block(TXFS_BLOCK_IBITMAP, inode_bitmap);
    txfs_write_block(TXFS_BLOCK_BBITMAP, block_bitmap);

    // create root inode
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

// ─── path helpers ────────────────────────────────────────────────────────────

static int str_equal(const char* a, const char* b)
{
    int i;
    for (i = 0; a[i] && b[i]; i++)
        if (a[i] != b[i]) return 0;
    return a[i] == b[i];
}

static int str_len(const char* s)
{
    int i = 0;
    while (s[i]) i++;
    return i;
}

static void str_copy(char* dst, const char* src, int max)
{
    int i;
    for (i = 0; i < max - 1 && src[i]; i++)
        dst[i] = src[i];
    dst[i] = 0;
}

static int txfs_strlen(const char* s)
{
    int i = 0;
    while (s[i]) i++;
    return i;
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

// find inode number for a path
static int txfs_lookup(const char* path)
{
    if (str_equal(path, "/")) return 0;  // root

    // skip leading slash
    const char* p = path;
    if (*p == '/') p++;

    int cur_inode = 0;  // start at root

    while (*p)
    {
        // extract next component
        char component[256];
        int  len = 0;
        while (p[len] && p[len] != '/') len++;
        for (int i = 0; i < len; i++) component[i] = p[i];
        component[len] = 0;
        p += len;
        if (*p == '/') p++;

        // read current inode
        txfs_inode_t inode;
        if (txfs_read_inode(cur_inode, &inode) < 0) return -1;

        int type = (inode.mode >> 12) & 0xF;
        if (type != TXFS_TYPE_DIR) return -1;

        // search directory entries
        int found = -1;
        uint32_t offset = 0;
        uint8_t data_buf[TXFS_BLOCK_SIZE];

        for (int b = 0; b < TXFS_DIRECT_BLOCKS && offset < inode.size; b++)
        {
            if (!inode.blocks[b]) break;
            if (txfs_read_block(inode.blocks[b], data_buf) < 0) return -1;

            uint32_t block_offset = 0;
            while (block_offset + sizeof(txfs_dirent_t) <= TXFS_BLOCK_SIZE
                   && offset < inode.size)
            {
                txfs_dirent_t* de = (txfs_dirent_t*)(data_buf + block_offset);
                if (de->inode && str_equal(de->name, component))
                {
                    found = de->inode;
                    break;
                }
                block_offset += sizeof(txfs_dirent_t);
                offset       += sizeof(txfs_dirent_t);
            }
            if (found >= 0) break;
        }

        if (found < 0) return -1;
        cur_inode = found;
    }

    return cur_inode;
}

// ─── VFS driver functions ────────────────────────────────────────────────────

#define TXFS_MAX_FDS 32
static txfs_fd_t open_files[TXFS_MAX_FDS];

static int txfs_mount_fn(const char* device)
{
    for (int i = 0; i < TXFS_MAX_FDS; i++)
        open_files[i].used = 0;

    if (txfs_read_super() < 0)
    {
        // no valid filesystem — format it
        txfs_format(204800);  // 100MB disk
        txfs_read_super();
    }

    return 0;
}

static int txfs_open_fn(const char* path, int flags)
{
    const char* local = txfs_strip_mount(path);
    int inode_num = txfs_lookup(local);

    if (inode_num < 0)
    {
        if (!(flags & VFS_O_CREATE)) return -1;

        // create the file
        // find parent directory
        // simplified: only support files in root for now
        int new_inode = txfs_alloc_inode();
        const char* local = txfs_strip_mount(path);
        print("mkdir local: ");
        print(local);
        print("\n");
        if (new_inode < 0) return -1;

        txfs_inode_t inode;
        uint8_t* p = (uint8_t*)&inode;
        for (uint32_t i = 0; i < sizeof(inode); i++) p[i] = 0;

        inode.mode  = (TXFS_TYPE_FILE << 12) |
                      TXFS_PERM_OWNER_R | TXFS_PERM_OWNER_W;
        inode.links = 1;
        inode.size  = 0;

        txfs_write_inode(new_inode, &inode);

        // add entry to root directory
        txfs_inode_t root;
        txfs_read_inode(0, &root);

        // find or allocate a block for root dir
        if (!root.blocks[0])
        {
            int b = txfs_alloc_block();
            if (b < 0) return -1;
            root.blocks[0] = b;

            // clear the block
            uint8_t zero[TXFS_BLOCK_SIZE];
            for (int i = 0; i < TXFS_BLOCK_SIZE; i++) zero[i] = 0;
            txfs_write_block(b, zero);
        }

        // find free dirent slot in root
        uint8_t data_buf[TXFS_BLOCK_SIZE];
        txfs_read_block(root.blocks[0], data_buf);

        uint32_t slot = root.size / sizeof(txfs_dirent_t);
        txfs_dirent_t* de = (txfs_dirent_t*)(data_buf +
                             slot * sizeof(txfs_dirent_t));

        // get filename from path (skip leading slash)
        const char* name = local;
        if (*name == '/') name++;

        de->inode    = new_inode;
        de->name_len = str_len(name);
        de->type     = TXFS_TYPE_FILE;
        str_copy(de->name, name, 256);

        txfs_write_block(root.blocks[0], data_buf);

        root.size += sizeof(txfs_dirent_t);
        txfs_write_inode(0, &root);

        inode_num = new_inode;
    }

    // find free fd slot
    for (int i = 0; i < TXFS_MAX_FDS; i++)
    {
        if (!open_files[i].used)
        {
            open_files[i].used      = 1;
            open_files[i].inode_num = inode_num;
            open_files[i].position  = 0;
            txfs_read_inode(inode_num, &open_files[i].inode);
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

    uint32_t read = 0;
    uint8_t  data_buf[TXFS_BLOCK_SIZE];

    while (read < to_read)
    {
        uint32_t block_idx    = (f->position + read) / TXFS_BLOCK_SIZE;
        uint32_t block_offset = (f->position + read) % TXFS_BLOCK_SIZE;

        if (block_idx >= TXFS_DIRECT_BLOCKS) break;  // no indirect yet
        if (!inode->blocks[block_idx]) break;

        txfs_read_block(inode->blocks[block_idx], data_buf);

        uint32_t can_read = TXFS_BLOCK_SIZE - block_offset;
        if (can_read > to_read - read) can_read = to_read - read;

        for (uint32_t i = 0; i < can_read; i++)
            buf[read + i] = data_buf[block_offset + i];

        read += can_read;
    }

    f->position += read;
    return read;
}

static int txfs_mkdir_fn(const char* path)
{
    const char* local = txfs_strip_mount(path);
    print("mkdir local: ");
    print(local);
    print("\n");

    int new_inode = txfs_alloc_inode();
    print("mkdir alloc_inode: ");
    print_hex(new_inode);
    print("\n");
    if (new_inode < 0) return -1;

    txfs_inode_t inode;
    uint8_t* p = (uint8_t*)&inode;
    for (uint32_t i = 0; i < sizeof(inode); i++) p[i] = 0;

    inode.mode  = (TXFS_TYPE_DIR << 12) |
                  TXFS_PERM_OWNER_R | TXFS_PERM_OWNER_W | TXFS_PERM_OWNER_X;
    inode.links = 1;
    inode.size  = 0;
    txfs_write_inode(new_inode, &inode);

    // add to root directory (simplified: only root-level dirs)
    txfs_inode_t root;
    txfs_read_inode(0, &root);

    if (!root.blocks[0])
    {
        int b = txfs_alloc_block();
        if (b < 0) return -1;
        root.blocks[0] = b;
        uint8_t zero[TXFS_BLOCK_SIZE];
        for (int i = 0; i < TXFS_BLOCK_SIZE; i++) zero[i] = 0;
        txfs_write_block(b, zero);
    }

    uint8_t data_buf[TXFS_BLOCK_SIZE];
    txfs_read_block(root.blocks[0], data_buf);

    uint32_t slot = root.size / sizeof(txfs_dirent_t);
    txfs_dirent_t* de = (txfs_dirent_t*)(data_buf + slot * sizeof(txfs_dirent_t));

    const char* name = local;
    if (*name == '/') name++;

    de->inode    = new_inode;
    de->name_len = txfs_strlen(name);
    de->type     = TXFS_TYPE_DIR;
    txfs_strcpy(de->name, name, 256);

    txfs_write_block(root.blocks[0], data_buf);
    root.size += sizeof(txfs_dirent_t);
    txfs_write_inode(0, &root);

    return 0;
}

static int txfs_remove_fn(const char* path)
{
    const char* local = txfs_strip_mount(path);
    int inode_num = txfs_lookup(local);
    if (inode_num < 0) return -1;

    // free inode blocks
    txfs_inode_t inode;
    txfs_read_inode(inode_num, &inode);

    txfs_read_block(TXFS_BLOCK_BBITMAP, block_bitmap);
    for (int i = 0; i < TXFS_DIRECT_BLOCKS; i++)
    {
        if (inode.blocks[i])
            bitmap_clear(block_bitmap, inode.blocks[i] - TXFS_BLOCK_DATA);
    }
    txfs_write_block(TXFS_BLOCK_BBITMAP, block_bitmap);

    // free inode
    txfs_read_block(TXFS_BLOCK_IBITMAP, inode_bitmap);
    bitmap_clear(inode_bitmap, inode_num);
    txfs_write_block(TXFS_BLOCK_IBITMAP, inode_bitmap);

    // remove from parent directory
    txfs_inode_t root;
    txfs_read_inode(0, &root);

    uint8_t data_buf[TXFS_BLOCK_SIZE];
    txfs_read_block(root.blocks[0], data_buf);

    const char* name = local;
    if (*name == '/') name++;

    uint32_t offset = 0;
    while (offset + sizeof(txfs_dirent_t) <= TXFS_BLOCK_SIZE)
    {
        txfs_dirent_t* de = (txfs_dirent_t*)(data_buf + offset);
        if (de->inode && !txfs_strcmp(de->name, name))
        {
            de->inode = 0;  // mark as deleted
            txfs_write_block(root.blocks[0], data_buf);
            return 0;
        }
        offset += sizeof(txfs_dirent_t);
    }

    return -1;
}

static int txfs_write_fn(int fd, const uint8_t* buf, uint32_t size)
{
    if (fd < 0 || fd >= TXFS_MAX_FDS || !open_files[fd].used) return -1;

    txfs_fd_t*    f     = &open_files[fd];
    txfs_inode_t* inode = &f->inode;

    uint32_t written = 0;
    uint8_t  data_buf[TXFS_BLOCK_SIZE];

    while (written < size)
    {
        uint32_t block_idx    = (f->position + written) / TXFS_BLOCK_SIZE;
        uint32_t block_offset = (f->position + written) % TXFS_BLOCK_SIZE;

        if (block_idx >= TXFS_DIRECT_BLOCKS) break;

        // allocate block if needed
        if (!inode->blocks[block_idx])
        {
            int b = txfs_alloc_block();
            if (b < 0) break;
            inode->blocks[block_idx] = b;

            // clear it
            for (int i = 0; i < TXFS_BLOCK_SIZE; i++) data_buf[i] = 0;
            txfs_write_block(b, data_buf);
        }

        txfs_read_block(inode->blocks[block_idx], data_buf);

        uint32_t can_write = TXFS_BLOCK_SIZE - block_offset;
        if (can_write > size - written) can_write = size - written;

        for (uint32_t i = 0; i < can_write; i++)
            data_buf[block_offset + i] = buf[written + i];

        txfs_write_block(inode->blocks[block_idx], data_buf);
        written += can_write;
    }

    f->position  += written;
    if (f->position > inode->size)
        inode->size = f->position;

    txfs_write_inode(f->inode_num, inode);
    return written;
}

static int txfs_readdir_fn(const char* path, char* out, uint32_t index)
{
    const char* local = txfs_strip_mount(path);
    int inode_num = txfs_lookup(local);
    if (inode_num < 0) return -1;

    txfs_inode_t inode;
    if (txfs_read_inode(inode_num, &inode) < 0) return -1;

    int type = (inode.mode >> 12) & 0xF;
    if (type != TXFS_TYPE_DIR) return -1;

    uint32_t count  = 0;
    uint8_t  data_buf[TXFS_BLOCK_SIZE];

    for (int b = 0; b < TXFS_DIRECT_BLOCKS; b++)
    {
        if (!inode.blocks[b]) break;
        txfs_read_block(inode.blocks[b], data_buf);

        uint32_t offset = 0;
        while (offset + sizeof(txfs_dirent_t) <= TXFS_BLOCK_SIZE)
        {
            txfs_dirent_t* de = (txfs_dirent_t*)(data_buf + offset);
            if (de->inode)
            {
                if (count == index)
                {
                    str_copy(out, de->name, 256);
                    return 0;
                }
                count++;
            }
            offset += sizeof(txfs_dirent_t);
        }
    }

    return -1;
}


static int txfs_stat_fn(const char* path, uint32_t* size)
{
    const char* local = txfs_strip_mount(path);
    int inode_num = txfs_lookup(local);
    if (inode_num < 0) return -1;

    txfs_inode_t inode;
    if (txfs_read_inode(inode_num, &inode) < 0) return -1;

    *size = inode.size;
    return 0;
}

// ─── driver registration ─────────────────────────────────────────────────────

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
};

fs_driver_t* txfs_init()
{
    return &txfs_driver;
}