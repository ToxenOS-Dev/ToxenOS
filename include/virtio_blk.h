#ifndef VIRTIO_BLK_H
#define VIRTIO_BLK_H

#include <stdint.h>

int      virtio_blk_init(void);
int      virtio_blk_read(uint8_t drive, uint32_t lba, uint8_t* buf, uint32_t sectors);
int      virtio_blk_write(uint8_t drive, uint32_t lba, const uint8_t* buf, uint32_t sectors);
uint32_t virtio_blk_sectors(uint8_t drive);

#endif
