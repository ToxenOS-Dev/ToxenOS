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

static int ata_wait()
{
    // wait for BSY to clear
    uint32_t timeout = 100000;
    while (timeout--)
    {
        uint8_t status = inb(ATA_PRIMARY_STATUS);
        if (status & ATA_STATUS_ERR) return -1;
        if (!(status & ATA_STATUS_BSY)) return 0;
    }
    return -1;  // timeout
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

int ata_init()
{
    outb(ATA_PRIMARY_DRIVE, 0xA0);

    outb(ATA_PRIMARY_SECTOR_COUNT, 0);
    outb(ATA_PRIMARY_LBA_LOW,      0);
    outb(ATA_PRIMARY_LBA_MID,      0);
    outb(ATA_PRIMARY_LBA_HIGH,     0);

    outb(ATA_PRIMARY_COMMAND, 0xEC);

    uint8_t status = inb(ATA_PRIMARY_STATUS);
    if (status == 0) return -1;

    if (ata_wait() < 0) return -1;

    for (int i = 0; i < 256; i++)
        inw(ATA_PRIMARY_DATA);

    return 0;
}

int ata_read(uint32_t lba, uint8_t* buf, uint32_t sectors)
{
    for (uint32_t s = 0; s < sectors; s++)
    {
        uint32_t cur_lba = lba + s;

        // wait for drive ready
        if (ata_wait() < 0) return -1;

        // select drive + LBA high bits
        outb(ATA_PRIMARY_DRIVE,        0xE0 | ((cur_lba >> 24) & 0x0F));
        outb(ATA_PRIMARY_SECTOR_COUNT, 1);
        outb(ATA_PRIMARY_LBA_LOW,      cur_lba & 0xFF);
        outb(ATA_PRIMARY_LBA_MID,      (cur_lba >> 8) & 0xFF);
        outb(ATA_PRIMARY_LBA_HIGH,     (cur_lba >> 16) & 0xFF);
        outb(ATA_PRIMARY_COMMAND,      ATA_CMD_READ);

        // wait for data ready
        if (ata_wait_drq() < 0) return -1;

        // read 256 words = 512 bytes
        uint16_t* ptr = (uint16_t*)(buf + s * 512);
        for (int i = 0; i < 256; i++)
            ptr[i] = inw(ATA_PRIMARY_DATA);
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

        // write 256 words = 512 bytes
        const uint16_t* ptr = (const uint16_t*)(buf + s * 512);
        for (int i = 0; i < 256; i++)
            outw(ATA_PRIMARY_DATA, ptr[i]);

        // flush cache
        outb(ATA_PRIMARY_COMMAND, 0xE7);
        ata_wait();
    }

    return sectors;
}