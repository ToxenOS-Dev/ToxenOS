#pragma once
#include <stdint.h>

int      ahci_init(void);
int      ahci_read_drive(uint8_t drive, uint32_t lba, uint8_t* buf, uint32_t sectors);
int      ahci_write_drive(uint8_t drive, uint32_t lba, const uint8_t* buf, uint32_t sectors);
uint32_t ahci_get_sectors(uint8_t drive);
