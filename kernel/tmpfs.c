#include <stdint.h>
#include "../include/tmpfs.h"
#include "../include/mm.h"
#include "../include/vga.h"

typedef struct
{
    int      used;
    char     name[TMPFS_NAME_MAX];
    uint8_t* data;
    uint32_t size;
    uint32_t capacity;
    int      is_dir;
} tmpfs_file_t;

static tmpfs_file_t files[TMPFS_MAX_FILES];
static int file_count = 0;

static void str_copy(char* dst, const char* src, int max)
{
    int i;
    for (i = 0; i < max - 1 && src[i]; i++)
        dst[i] = src[i];
    dst[i] = 0;
}

static int str_equal(const char* a, const char* b)
{
    int i;
    for (i = 0; a[i] && b[i]; i++)
        if (a[i] != b[i]) return 0;
    return a[i] == b[i];
}

static int tmpfs_find(const char* path)
{
    for (int i = 0; i < TMPFS_MAX_FILES; i++)
        if (files[i].used && str_equal(files[i].name, path))
            return i;
    return -1;
}

static int tmpfs_mount(const char* device)
{
    for (int i = 0; i < TMPFS_MAX_FILES; i++)
        files[i].used = 0;

    // create root directory
    files[0].used   = 1;
    files[0].is_dir = 1;
    str_copy(files[0].name, "/", TMPFS_NAME_MAX);
    file_count = 1;

    return 0;
}

static int tmpfs_open(const char* path, int flags)
{
    int idx = tmpfs_find(path);

    if (idx == -1)
    {
        if (!(flags & VFS_O_CREATE)) return -1;

        // create new file
        for (int i = 0; i < TMPFS_MAX_FILES; i++)
        {
            if (!files[i].used)
            {
                files[i].used     = 1;
                files[i].is_dir   = 0;
                files[i].size     = 0;
                files[i].capacity = 256;
                files[i].data     = (uint8_t*)kmalloc(256);
                str_copy(files[i].name, path, TMPFS_NAME_MAX);
                file_count++;
                return i;
            }
        }
        return -1;
    }

    return idx;
}

static int tmpfs_close(int fd)
{
    return 0;
}

static int tmpfs_read(int fd, uint8_t* buf, uint32_t size)
{
    if (fd < 0 || fd >= TMPFS_MAX_FILES || !files[fd].used) return -1;

    uint32_t pos = 0;  // simplified — no position tracking per fd yet
    uint32_t to_read = size;
    if (to_read > files[fd].size) to_read = files[fd].size;

    for (uint32_t i = 0; i < to_read; i++)
        buf[i] = files[fd].data[i];

    return to_read;
}

static int tmpfs_write(int fd, const uint8_t* buf, uint32_t size)
{
    if (fd < 0 || fd >= TMPFS_MAX_FILES || !files[fd].used) return -1;

    // grow buffer if needed
    if (files[fd].size + size > files[fd].capacity)
    {
        uint32_t new_cap = files[fd].size + size + 256;
        if (new_cap > TMPFS_MAX_FILESIZE) return -1;

        uint8_t* new_data = (uint8_t*)kmalloc(new_cap);
        for (uint32_t i = 0; i < files[fd].size; i++)
            new_data[i] = files[fd].data[i];

        files[fd].data     = new_data;
        files[fd].capacity = new_cap;
    }

    for (uint32_t i = 0; i < size; i++)
        files[fd].data[files[fd].size + i] = buf[i];

    files[fd].size += size;
    return size;
}

static int tmpfs_readdir(const char* path, char* out, uint32_t index)
{
    uint32_t count = 0;

    for (int i = 0; i < TMPFS_MAX_FILES; i++)
    {
        if (!files[i].used) continue;
        if (str_equal(files[i].name, path)) continue; // skip . entry

        // check if file is in this directory
        // simple check: file starts with path
        int path_len = 0;
        while (path[path_len]) path_len++;

        int match = 1;
        for (int j = 0; j < path_len; j++)
            if (files[i].name[j] != path[j]) { match = 0; break; }

        if (!match) continue;

        if (count == index)
        {
            str_copy(out, files[i].name, TMPFS_NAME_MAX);
            return 0;
        }
        count++;
    }

    return -1;
}

static int tmpfs_stat(const char* path, uint32_t* size)
{
    int idx = tmpfs_find(path);
    if (idx == -1) return -1;

    *size = files[idx].size;
    return 0;
}

static fs_driver_t tmpfs_driver = {
    .name    = "tmpfs",
    .mount   = tmpfs_mount,
    .open    = tmpfs_open,
    .close   = tmpfs_close,
    .read    = tmpfs_read,
    .write   = tmpfs_write,
    .readdir = tmpfs_readdir,
    .stat    = tmpfs_stat,
    .mkdir  = 0,
    .remove = 0,
};

fs_driver_t* tmpfs_init()
{
    return &tmpfs_driver;
}