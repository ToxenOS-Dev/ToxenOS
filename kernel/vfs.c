#include <stdint.h>
#include "../include/vfs.h"
#include "../include/vga.h"

static mount_t          mounts[VFS_MAX_MOUNTS];
static file_descriptor_t fds[VFS_MAX_FDS];
static int              mount_count = 0;

static int string_starts_with(const char* str, const char* prefix)
{
    int i;
    for (i = 0; prefix[i] != 0; i++)
        if (str[i] != prefix[i]) return 0;
    return 1;
}

static int string_length(const char* str)
{
    int i = 0;
    while (str[i]) i++;
    return i;
}

static void string_copy(char* dst, const char* src, int max)
{
    int i;
    for (i = 0; i < max - 1 && src[i]; i++)
        dst[i] = src[i];
    dst[i] = 0;
}

void vfs_init()
{
    for (int i = 0; i < VFS_MAX_MOUNTS; i++)
        mounts[i].mounted = 0;

    for (int i = 0; i < VFS_MAX_FDS; i++)
        fds[i].used = 0;

    mount_count = 0;
}

int vfs_mount(const char* mountpoint, fs_driver_t* driver, const char* device)
{
    if (mount_count >= VFS_MAX_MOUNTS) return -1;

    int slot = -1;
    for (int i = 0; i < VFS_MAX_MOUNTS; i++)
    {
        if (!mounts[i].mounted)
        {
            slot = i;
            break;
        }
    }

    if (slot == -1) return -1;

    if (driver->mount) {
        // Call mount_fn first — only register if it succeeds
        int result = driver->mount(device);
        if (result < 0) return -1;
    }

    string_copy(mounts[slot].mountpoint, mountpoint, 64);
    mounts[slot].driver  = driver;
    mounts[slot].mounted = 1;
    mount_count++;

    return 0;
}

// find best matching mount for a path
static int vfs_find_mount(const char* path)
{
    int best = -1;
    int best_len = -1;

    for (int i = 0; i < VFS_MAX_MOUNTS; i++)
    {
        if (!mounts[i].mounted) continue;

        int len = string_length(mounts[i].mountpoint);
        if (string_starts_with(path, mounts[i].mountpoint) && len > best_len)
        {
            best = i;
            best_len = len;
        }
    }

    return best;
}

int vfs_open(const char* path, int flags)
{
    int mount_idx = vfs_find_mount(path);
    if (mount_idx == -1) return -1;

    // find free vfs fd
    int fd = -1;
    for (int i = 0; i < VFS_MAX_FDS; i++)
    {
        if (!fds[i].used)
        {
            fd = i;
            break;
        }
    }
    if (fd == -1) return -1;

    // call driver open — it returns its own internal fd
    int driver_fd = mounts[mount_idx].driver->open(path, flags);
    if (driver_fd < 0) return -1;

    fds[fd].used      = 1;
    fds[fd].mount_idx = mount_idx;
    fds[fd].position  = 0;
    fds[fd].driver_fd = driver_fd;  // ← store driver's fd separately
    string_copy(fds[fd].path, path, VFS_NAME_MAX);

    return fd;
}

int vfs_read(int fd, uint8_t* buf, uint32_t size)
{
    if (fd < 0 || fd >= VFS_MAX_FDS || !fds[fd].used) return -1;
    int result = mounts[fds[fd].mount_idx].driver->read(fds[fd].driver_fd, buf, size);
    if (result > 0) fds[fd].position += result;
    return result;
}

int vfs_write(int fd, const uint8_t* buf, uint32_t size)
{
    if (fd < 0 || fd >= VFS_MAX_FDS || !fds[fd].used) return -1;
    int result = mounts[fds[fd].mount_idx].driver->write(fds[fd].driver_fd, buf, size);
    if (result > 0) fds[fd].position += result;
    return result;
}

int vfs_close(int fd)
{
    if (fd < 0 || fd >= VFS_MAX_FDS || !fds[fd].used) return -1;
    mounts[fds[fd].mount_idx].driver->close(fds[fd].driver_fd);
    fds[fd].used = 0;
    return 0;
}

int vfs_mkdir(const char* path)
{

    int mount_idx = vfs_find_mount(path);

    if (mount_idx == -1) return -1;

    if (!mounts[mount_idx].driver->mkdir)
        return -1;

    return mounts[mount_idx].driver->mkdir(path);
}

int vfs_remove(const char* path)
{
    int mount_idx = vfs_find_mount(path);
    if (mount_idx == -1) return -1;

    if (!mounts[mount_idx].driver->remove)
        return -1;

    return mounts[mount_idx].driver->remove(path);
}

int vfs_isdir(const char* path)
{
    int mount_idx = vfs_find_mount(path);
    if (mount_idx == -1) return -1;
    if (!mounts[mount_idx].driver->isdir) return -1;
    return mounts[mount_idx].driver->isdir(path);
}

int vfs_readdir(const char* path, char* out, uint32_t index)
{
    int mount_idx = vfs_find_mount(path);
    if (mount_idx == -1) return -1;

    return mounts[mount_idx].driver->readdir(path, out, index);
}

int vfs_stat(const char* path, uint32_t* size)
{
    int mount_idx = vfs_find_mount(path);
    if (mount_idx == -1) return -1;

    return mounts[mount_idx].driver->stat(path, size);
}