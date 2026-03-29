#ifndef ATA_H
#define ATA_H

#include <stdint.h>

#define ATA_PRIMARY_DATA         0x1F0
#define ATA_PRIMARY_ERROR        0x1F1
#define ATA_PRIMARY_SECTOR_COUNT 0x1F2
#define ATA_PRIMARY_LBA_LOW      0x1F3
#define ATA_PRIMARY_LBA_MID      0x1F4
#define ATA_PRIMARY_LBA_HIGH     0x1F5
#define ATA_PRIMARY_DRIVE        0x1F6
#define ATA_PRIMARY_STATUS       0x1F7
#define ATA_PRIMARY_COMMAND      0x1F7

#define ATA_STATUS_BSY  0x80
#define ATA_STATUS_DRQ  0x08
#define ATA_STATUS_ERR  0x01

#define ATA_CMD_READ    0x20
#define ATA_CMD_WRITE   0x30

#define ATA_DRIVE_MASTER  0  // first drive  (0xE0)
#define ATA_DRIVE_SLAVE   1  // second drive (0xF0)

int  ata_init();
int  ata_read(uint32_t lba, uint8_t* buf, uint32_t sectors);
int  ata_write(uint32_t lba, const uint8_t* buf, uint32_t sectors);

// Drive-selectable versions for FAT and future drivers
int  ata_read_drive(uint8_t drive, uint32_t lba, uint8_t* buf, uint32_t sectors);
int  ata_write_drive(uint8_t drive, uint32_t lba, const uint8_t* buf, uint32_t sectors);

#endif
