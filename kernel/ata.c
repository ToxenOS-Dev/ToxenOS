#include <stdint.h>
#include "../include/ata.h"
#include "../include/vga.h"

static inline void outb(uint16_t port, uint8_t val)
{
    __asm__ volatile("outb %0, %1" : : "a"(val), "Nd"(port));
}

static inline uint8_t inb(uint16_t port)
{
    uint8_t ret;
    __asm__ volatile("inb %1, %0" : "=a"(ret) : "Nd"(port));
    return ret;
}

static inline uint16_t inw(uint16_t port)
{
    uint16_t ret;
    __asm__ volatile("inw %1, %0" : "=a"(ret) : "Nd"(port));
    return ret;
}

static inline void outw(uint16_t port, uint16_t val)
{
    __asm__ volatile("outw %0, %1" : : "a"(val), "Nd"(port));
}

static void ata_delay()
{
    for (int i = 0; i < 16; i++) inb(0x3F6);
}

static int ata_wait()
{
    uint32_t timeout = 100000;
    while (timeout--)
    {
        uint8_t status = inb(ATA_PRIMARY_STATUS);
        if (status & ATA_STATUS_ERR) return -1;
        if (!(status & ATA_STATUS_BSY)) return 0;
    }
    return -1;
}

static int ata_wait_drq()
{
    uint32_t timeout = 100000;
    while (timeout--)
    {
        uint8_t status = inb(ATA_PRIMARY_STATUS);
        if (status & ATA_STATUS_ERR) return -1;
        if (status & ATA_STATUS_DRQ) return 0;
    }
    return -1;
}

// Wait for BSY only — ignores ERR, used for slave drive
static int ata_wait_bsy_only()
{
    uint32_t timeout = 200000;
    while (timeout--)
    {
        uint8_t status = inb(ATA_PRIMARY_STATUS);
        if (status == 0xFF) return -1;
        if (!(status & ATA_STATUS_BSY)) return 0;
    }
    return -1;
}

int ata_init()
{
    // Initialize master (drive 0)
    outb(ATA_PRIMARY_DRIVE, 0xA0);
    ata_delay();
    outb(ATA_PRIMARY_SECTOR_COUNT, 0);
    outb(ATA_PRIMARY_LBA_LOW,      0);
    outb(ATA_PRIMARY_LBA_MID,      0);
    outb(ATA_PRIMARY_LBA_HIGH,     0);
    outb(ATA_PRIMARY_COMMAND, 0xEC);
    uint8_t status = inb(ATA_PRIMARY_STATUS);
    if (status == 0) return -1;
    if (ata_wait() < 0) return -1;
    for (int i = 0; i < 256; i++) inw(ATA_PRIMARY_DATA);

    // Initialize slave (drive 1) — ignore failure
    outb(ATA_PRIMARY_DRIVE, 0xB0);
    ata_delay();
    outb(ATA_PRIMARY_SECTOR_COUNT, 0);
    outb(ATA_PRIMARY_LBA_LOW,      0);
    outb(ATA_PRIMARY_LBA_MID,      0);
    outb(ATA_PRIMARY_LBA_HIGH,     0);
    outb(ATA_PRIMARY_COMMAND, 0xEC);
    ata_delay();
    status = inb(ATA_PRIMARY_STATUS);
    if (status != 0x00 && status != 0xFF) {
        ata_wait_bsy_only();
        for (int i = 0; i < 256; i++) inw(ATA_PRIMARY_DATA);
    }

    // Restore master
    outb(ATA_PRIMARY_DRIVE, 0xA0);
    ata_delay();

    return 0;
}

int ata_read(uint32_t lba, uint8_t* buf, uint32_t sectors)
{
    for (uint32_t s = 0; s < sectors; s++)
    {
        uint32_t cur_lba = lba + s;
        if (ata_wait() < 0) return -1;
        outb(ATA_PRIMARY_DRIVE,        0xE0 | ((cur_lba >> 24) & 0x0F));
        outb(ATA_PRIMARY_SECTOR_COUNT, 1);
        outb(ATA_PRIMARY_LBA_LOW,      cur_lba & 0xFF);
        outb(ATA_PRIMARY_LBA_MID,      (cur_lba >> 8) & 0xFF);
        outb(ATA_PRIMARY_LBA_HIGH,     (cur_lba >> 16) & 0xFF);
        outb(ATA_PRIMARY_COMMAND,      ATA_CMD_READ);
        if (ata_wait_drq() < 0) return -1;
        uint16_t* ptr = (uint16_t*)(buf + s * 512);
        for (int i = 0; i < 256; i++) ptr[i] = inw(ATA_PRIMARY_DATA);
    }
    return sectors;
}

int ata_write(uint32_t lba, const uint8_t* buf, uint32_t sectors)
{
    for (uint32_t s = 0; s < sectors; s++)
    {
        uint32_t cur_lba = lba + s;
        if (ata_wait() < 0) return -1;
        outb(ATA_PRIMARY_DRIVE,        0xE0 | ((cur_lba >> 24) & 0x0F));
        outb(ATA_PRIMARY_SECTOR_COUNT, 1);
        outb(ATA_PRIMARY_LBA_LOW,      cur_lba & 0xFF);
        outb(ATA_PRIMARY_LBA_MID,      (cur_lba >> 8) & 0xFF);
        outb(ATA_PRIMARY_LBA_HIGH,     (cur_lba >> 16) & 0xFF);
        outb(ATA_PRIMARY_COMMAND,      ATA_CMD_WRITE);
        if (ata_wait_drq() < 0) return -1;
        const uint16_t* ptr = (const uint16_t*)(buf + s * 512);
        for (int i = 0; i < 256; i++) outw(ATA_PRIMARY_DATA, ptr[i]);
        outb(ATA_PRIMARY_COMMAND, 0xE7);
        ata_wait();
    }
    return sectors;
}

static int ata_read_drive_bsy_only_drq()
{
    // Wait for DRQ, ignoring ERR — for slave drives
    uint32_t timeout = 200000;
    while (timeout--)
    {
        uint8_t status = inb(ATA_PRIMARY_STATUS);
        if (status == 0xFF) return -1;
        if (status & ATA_STATUS_DRQ) return 0;
        if (!(status & ATA_STATUS_BSY) && !(status & ATA_STATUS_DRQ)) {
            // Not busy and no DRQ — stalled, try a bit longer
        }
    }
    return -1;
}

int ata_read_drive(uint8_t drive, uint32_t lba, uint8_t* buf, uint32_t sectors)
{
    uint8_t drive_base = (drive == ATA_DRIVE_SLAVE) ? 0xB0 : 0xE0;

    // Software reset: assert SRST bit in device control register
    outb(0x3F6, 0x04);  // SRST=1
    ata_delay();
    outb(0x3F6, 0x00);  // SRST=0
    ata_delay();
    ata_wait_bsy_only();

    for (uint32_t s = 0; s < sectors; s++)
    {
        uint32_t cur_lba = lba + s;

        outb(ATA_PRIMARY_DRIVE, drive_base | ((cur_lba >> 24) & 0x0F));
        ata_delay();
        ata_wait_bsy_only();

        outb(ATA_PRIMARY_SECTOR_COUNT, 1);
        outb(ATA_PRIMARY_LBA_LOW,      cur_lba & 0xFF);
        outb(ATA_PRIMARY_LBA_MID,      (cur_lba >> 8) & 0xFF);
        outb(ATA_PRIMARY_LBA_HIGH,     (cur_lba >> 16) & 0xFF);
        outb(ATA_PRIMARY_COMMAND,      ATA_CMD_READ);
        ata_delay();

        if (ata_read_drive_bsy_only_drq() < 0) {
            outb(ATA_PRIMARY_DRIVE, 0xA0); ata_delay();
            return -1;
        }

        uint16_t* ptr = (uint16_t*)(buf + s * 512);
        for (int i = 0; i < 256; i++) ptr[i] = inw(ATA_PRIMARY_DATA);
    }

    outb(ATA_PRIMARY_DRIVE, 0xA0);
    ata_delay();
    return sectors;
}

int ata_write_drive(uint8_t drive, uint32_t lba, const uint8_t* buf, uint32_t sectors)
{
    uint8_t drive_base = (drive == ATA_DRIVE_SLAVE) ? 0xB0 : 0xE0;

    for (uint32_t s = 0; s < sectors; s++)
    {
        uint32_t cur_lba = lba + s;

        outb(ATA_PRIMARY_DRIVE, drive_base | ((cur_lba >> 24) & 0x0F));
        ata_delay();
        if (ata_wait_bsy_only() < 0) {
            outb(ATA_PRIMARY_DRIVE, 0xA0); ata_delay();
            return -1;
        }

        outb(ATA_PRIMARY_SECTOR_COUNT, 1);
        outb(ATA_PRIMARY_LBA_LOW,      cur_lba & 0xFF);
        outb(ATA_PRIMARY_LBA_MID,      (cur_lba >> 8) & 0xFF);
        outb(ATA_PRIMARY_LBA_HIGH,     (cur_lba >> 16) & 0xFF);
        outb(ATA_PRIMARY_COMMAND,      ATA_CMD_WRITE);

        if (ata_wait_drq() < 0) {
            outb(ATA_PRIMARY_DRIVE, 0xA0); ata_delay();
            return -1;
        }

        const uint16_t* ptr = (const uint16_t*)(buf + s * 512);
        for (int i = 0; i < 256; i++) outw(ATA_PRIMARY_DATA, ptr[i]);
        outb(ATA_PRIMARY_COMMAND, 0xE7);
        ata_wait_bsy_only();
    }

    outb(ATA_PRIMARY_DRIVE, 0xA0);
    ata_delay();
    return sectors;
}
