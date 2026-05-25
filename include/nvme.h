#pragma once
#include <stdint.h>

int      nvme_init(void);
int      nvme_read_drive(uint8_t drive, uint32_t lba, uint8_t* buf, uint32_t sectors);
int      nvme_write_drive(uint8_t drive, uint32_t lba, const uint8_t* buf, uint32_t sectors);
uint32_t nvme_get_sectors(uint8_t drive);
