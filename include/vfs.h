#ifndef VFS_H
#define VFS_H

#include <stdint.h>

#define VFS_MAX_MOUNTS   16
#define VFS_MAX_FDS      64   // global pool of file descriptions
#define VFS_PROC_FDS     16   // per-process open FD slots (local fd 0..15)
#define VFS_NAME_MAX     256

typedef struct
{
    char name[32];
    int (*mount)(const char* device);
    int (*open)(const char* path, int flags);
    int (*close)(int fd);
    int (*read)(int fd, uint8_t* buf, uint32_t size);
    int (*write)(int fd, const uint8_t* buf, uint32_t size);
    int (*readdir)(const char* path, char* out, uint32_t index);
    int (*stat)(const char* path, uint32_t* size);
    int (*mkdir)(const char* path);      
    int (*remove)(const char* path); 
    int (*isdir)(const char* path);
} fs_driver_t;

typedef struct
{
    char         mountpoint[64];
    fs_driver_t* driver;
    int          mounted;
} mount_t;

// fd types
#define FD_TYPE_FILE    0
#define FD_TYPE_PIPE_R  1
#define FD_TYPE_PIPE_W  2

typedef struct
{
    int      used;
    int      type;       // FD_TYPE_FILE / PIPE_R / PIPE_W
    int      mount_idx;
    int      driver_fd;
    int      pipe_idx;   // index into pipes[] when type != FD_TYPE_FILE
    char     path[VFS_NAME_MAX];
    uint32_t position;
} file_descriptor_t;

void vfs_init();
int  vfs_mount(const char* mountpoint, fs_driver_t* driver, const char* device);
int  vfs_open(const char* path, int flags);
int  vfs_close(int fd);
int  vfs_read(int fd, uint8_t* buf, uint32_t size);
int  vfs_write(int fd, const uint8_t* buf, uint32_t size);
int  vfs_readdir(const char* path, char* out, uint32_t index);
int  vfs_stat(const char* path, uint32_t* size);
int  vfs_mkdir(const char* path);
int  vfs_remove(const char* path);
int  vfs_isdir(const char* path);
int  vfs_pipe(int* rfd, int* wfd);

// Per-process FD table — maps local fd numbers to global pool slots.
// local_fds[i] = -1 means slot i is closed.
typedef struct {
    int local_fds[VFS_PROC_FDS];
} fd_table_t;

void fd_table_init(fd_table_t* t);
void fd_table_copy(fd_table_t* dst, const fd_table_t* src);
// Allocate next free local fd, pointing at global slot gfd. Returns local fd or -1.
int  fd_table_alloc(fd_table_t* t, int gfd);
// Translate local fd → global fd. Returns -1 if not open.
int  fd_table_get(fd_table_t* t, int local_fd);
// Close a local fd (does not touch global pool).
void fd_table_close_local(fd_table_t* t, int local_fd);
// Close all local fds in a table (called on process exit).
void fd_table_close_all(fd_table_t* t);

// flags for vfs_open
#define VFS_O_READ   0x1
#define VFS_O_WRITE  0x2
#define VFS_O_CREATE 0x4

#endif