#ifndef TMPFS_H
#define TMPFS_H

#include <stdint.h>
#include "../include/vfs.h"

#define TMPFS_MAX_FILES     64
#define TMPFS_MAX_FILESIZE  65536   // 64KB per file
#define TMPFS_NAME_MAX      256

fs_driver_t* tmpfs_init();

#endif