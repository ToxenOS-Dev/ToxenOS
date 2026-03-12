#ifndef VFS_H
#define VFS_H

#include <stdint.h>

#define VFS_MAX_MOUNTS  16
#define VFS_MAX_FDS     64
#define VFS_NAME_MAX    256

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
} fs_driver_t;

typedef struct
{
    char         mountpoint[64];
    fs_driver_t* driver;
    int          mounted;
} mount_t;

typedef struct
{
    int    used;
    int    mount_idx;
    char   path[VFS_NAME_MAX];
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

// flags for vfs_open
#define VFS_O_READ   0x1
#define VFS_O_WRITE  0x2
#define VFS_O_CREATE 0x4

#endif