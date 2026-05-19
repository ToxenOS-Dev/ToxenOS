#include <stdint.h>
#include "../include/vfs.h"
#include "../include/vga.h"
#include "../include/pipe.h"
#include "../include/process.h"

static mount_t           mounts[VFS_MAX_MOUNTS];
static file_descriptor_t fds[VFS_MAX_FDS];   // global file description pool
static int               mount_count = 0;

// ── Per-process FD table ──────────────────────────────────────────────────────

void fd_table_init(fd_table_t* t)
{
    for (int i = 0; i < VFS_PROC_FDS; i++)
        t->local_fds[i] = -1;
}

void fd_table_copy(fd_table_t* dst, const fd_table_t* src)
{
    for (int i = 0; i < VFS_PROC_FDS; i++)
        dst->local_fds[i] = src->local_fds[i];
}

int fd_table_alloc(fd_table_t* t, int gfd)
{
    for (int i = 0; i < VFS_PROC_FDS; i++) {
        if (t->local_fds[i] == -1) {
            t->local_fds[i] = gfd;
            return i;
        }
    }
    return -1;
}

int fd_table_get(fd_table_t* t, int local_fd)
{
    if (local_fd < 0 || local_fd >= VFS_PROC_FDS) return -1;
    return t->local_fds[local_fd];
}

void fd_table_close_local(fd_table_t* t, int local_fd)
{
    if (local_fd < 0 || local_fd >= VFS_PROC_FDS) return;
    t->local_fds[local_fd] = -1;
}

void fd_table_close_all(fd_table_t* t)
{
    for (int i = 0; i < VFS_PROC_FDS; i++) {
        if (t->local_fds[i] >= 0) {
            vfs_close(t->local_fds[i]);
            t->local_fds[i] = -1;
        }
    }
}

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

    for (int i = 0; i < VFS_MAX_FDS; i++) {
        fds[i].used     = 0;
        fds[i].type     = FD_TYPE_FILE;
        fds[i].pipe_idx = -1;
    }

    pipe_init();
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
    fds[fd].type      = FD_TYPE_FILE;
    fds[fd].pipe_idx  = -1;
    fds[fd].mount_idx = mount_idx;
    fds[fd].driver_fd = driver_fd;
    string_copy(fds[fd].path, path, VFS_NAME_MAX);
    // For append mode, start position at end of file
    if ((flags & VFS_O_APPEND) && mounts[mount_idx].driver->stat) {
        uint32_t fsize = 0;
        mounts[mount_idx].driver->stat(path, &fsize);
        fds[fd].position = fsize;
    } else {
        fds[fd].position = 0;
    }

    return fd;
}

int vfs_read(int fd, uint8_t* buf, uint32_t size)
{
    if (fd < 0 || fd >= VFS_MAX_FDS || !fds[fd].used) return -1;
    if (fds[fd].type == FD_TYPE_PIPE_R)
        return pipe_read(fds[fd].pipe_idx, buf, size);
    int result = mounts[fds[fd].mount_idx].driver->read(fds[fd].driver_fd, buf, size);
    if (result > 0) fds[fd].position += result;
    return result;
}

int vfs_write(int fd, const uint8_t* buf, uint32_t size)
{
    if (fd < 0 || fd >= VFS_MAX_FDS || !fds[fd].used) return -1;
    if (fds[fd].type == FD_TYPE_PIPE_W)
        return pipe_write(fds[fd].pipe_idx, buf, size);
    int result = mounts[fds[fd].mount_idx].driver->write(fds[fd].driver_fd, buf, size);
    if (result > 0) fds[fd].position += result;
    return result;
}

int vfs_close(int fd)
{
    if (fd < 0 || fd >= VFS_MAX_FDS || !fds[fd].used) return -1;
    if (fds[fd].type == FD_TYPE_PIPE_R) {
        pipe_close_read(fds[fd].pipe_idx);
        fds[fd].used = 0;
        return 0;
    }
    if (fds[fd].type == FD_TYPE_PIPE_W) {
        pipe_close_write(fds[fd].pipe_idx);
        fds[fd].used = 0;
        return 0;
    }
    mounts[fds[fd].mount_idx].driver->close(fds[fd].driver_fd);
    fds[fd].used = 0;
    return 0;
}

// Allocate a pipe and return two VFS fds: rfd (read) and wfd (write).
int vfs_pipe(int* rfd, int* wfd)
{
    int pidx = pipe_alloc();
    if (pidx < 0) return -1;

    // find two free fd slots
    int r = -1, w = -1;
    for (int i = 0; i < VFS_MAX_FDS && (r == -1 || w == -1); i++) {
        if (!fds[i].used) {
            if (r == -1) r = i;
            else         w = i;
        }
    }
    if (r == -1 || w == -1) {
        pipe_close_read(pidx);
        pipe_close_write(pidx);
        return -1;
    }

    fds[r].used     = 1;
    fds[r].type     = FD_TYPE_PIPE_R;
    fds[r].pipe_idx = pidx;
    fds[r].path[0]  = 0;

    fds[w].used     = 1;
    fds[w].type     = FD_TYPE_PIPE_W;
    fds[w].pipe_idx = pidx;
    fds[w].path[0]  = 0;

    *rfd = r;
    *wfd = w;
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

int vfs_chmod(const char* path, uint32_t mode)
{
    int mount_idx = vfs_find_mount(path);
    if (mount_idx == -1) return -1;
    if (!mounts[mount_idx].driver->chmod) return -1;
    return mounts[mount_idx].driver->chmod(path, mode);
}

int vfs_getmode(const char* path)
{
    int mount_idx = vfs_find_mount(path);
    if (mount_idx == -1) return -1;
    if (!mounts[mount_idx].driver->getmode) return -1;
    return mounts[mount_idx].driver->getmode(path);
}