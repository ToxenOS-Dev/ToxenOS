#ifndef FAT_H
#define FAT_H

#include <stdint.h>
#include "../include/vfs.h"

// Returns the FAT fs_driver_t to pass to vfs_mount()
fs_driver_t* fat_init();

#endif
