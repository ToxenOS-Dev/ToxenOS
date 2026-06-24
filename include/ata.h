#ifndef ATA_H
#define ATA_H

#include <stdint.h>

// ── Primary channel ports ─────────────────────────────────────────────────────
#define ATA_PRIMARY_DATA         0x1F0
#define ATA_PRIMARY_ERROR        0x1F1
#define ATA_PRIMARY_SECTOR_COUNT 0x1F2
#define ATA_PRIMARY_LBA_LOW      0x1F3
#define ATA_PRIMARY_LBA_MID      0x1F4
#define ATA_PRIMARY_LBA_HIGH     0x1F5
#define ATA_PRIMARY_DRIVE        0x1F6
#define ATA_PRIMARY_STATUS       0x1F7
#define ATA_PRIMARY_COMMAND      0x1F7
#define ATA_PRIMARY_ALT_STATUS   0x3F6

// ── Secondary channel ports ───────────────────────────────────────────────────
#define ATA_SECONDARY_DATA         0x170
#define ATA_SECONDARY_ERROR        0x171
#define ATA_SECONDARY_SECTOR_COUNT 0x172
#define ATA_SECONDARY_LBA_LOW      0x173
#define ATA_SECONDARY_LBA_MID      0x174
#define ATA_SECONDARY_LBA_HIGH     0x175
#define ATA_SECONDARY_DRIVE        0x176
#define ATA_SECONDARY_STATUS       0x177
#define ATA_SECONDARY_COMMAND      0x177
#define ATA_SECONDARY_ALT_STATUS   0x376

// ── Status bits ───────────────────────────────────────────────────────────────
#define ATA_STATUS_BSY  0x80
#define ATA_STATUS_DRQ  0x08
#define ATA_STATUS_ERR  0x01

// ── Commands ──────────────────────────────────────────────────────────────────
#define ATA_CMD_READ    0x20
#define ATA_CMD_WRITE   0x30

// ── Drive numbers ─────────────────────────────────────────────────────────────
// Encoded as: bits[1:0] = position on channel, bit[2] = channel
#define ATA_DRIVE_PRIMARY_MASTER    0  // primary master
#define ATA_DRIVE_PRIMARY_SLAVE     1  // primary slave
#define ATA_DRIVE_SECONDARY_MASTER  2  // secondary master
#define ATA_DRIVE_SECONDARY_SLAVE   3  // secondary slave

// Legacy aliases
#define ATA_DRIVE_MASTER  ATA_DRIVE_PRIMARY_MASTER
#define ATA_DRIVE_SLAVE   ATA_DRIVE_PRIMARY_SLAVE

int  ata_init();
void ata_set_ahci(int use_ahci);        // redirect TxFS I/O through AHCI
void ata_set_nvme(int use_nvme);        // redirect TxFS I/O through NVMe
void ata_set_virtio(int v);             // redirect TxFS I/O through VirtIO block
void ata_set_nvme_ready(int v);         // mark NVMe as probed (for installer drive numbering)
void ata_set_ahci_ready(int v);         // mark AHCI as probed (for installer drive numbering)
void ata_set_virtio_ready(int v);       // mark VirtIO as probed
void ata_set_ramdisk(uint8_t *buf, uint32_t size); // use in-memory buffer for drive 0
int  ata_has_ramdisk(void);

// Legacy functions — always use primary master
int  ata_read(uint32_t lba, uint8_t* buf, uint32_t sectors);
int  ata_write(uint32_t lba, const uint8_t* buf, uint32_t sectors);

// Drive-selectable functions — use ATA_DRIVE_* constants
int      ata_read_drive(uint8_t drive, uint32_t lba, uint8_t* buf, uint32_t sectors);
int      ata_write_drive(uint8_t drive, uint32_t lba, const uint8_t* buf, uint32_t sectors);
uint32_t ata_get_sectors(uint8_t drive);  // total 512-byte sectors via IDENTIFY

#endif
