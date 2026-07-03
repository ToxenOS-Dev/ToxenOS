// kernel/ata64.c — Milestone 4: minimal ATA PIO driver for the 64-bit
// kernel. Mirrors kernel/ata.c's primary-channel/master-drive PIO logic
// (ata_init_channel/ata_do_read) exactly, but deliberately drops the
// secondary channel, slave-drive wake-up, write path, and the
// AHCI/NVMe/VirtIO/ramdisk redirection layer -- none of that is needed
// to prove the 64-bit kernel can read a file, and porting it would
// drag in three unported, not-yet-LP64-safe drivers for code paths
// that would never execute.
#include <stdint.h>
#include "../include/ata64.h"

#define ATA_DATA        0x1F0
#define ATA_ERROR       0x1F1
#define ATA_SECTOR_CNT  0x1F2
#define ATA_LBA_LO      0x1F3
#define ATA_LBA_MID     0x1F4
#define ATA_LBA_HI      0x1F5
#define ATA_DRIVE_SEL   0x1F6
#define ATA_STATUS      0x1F7
#define ATA_ALT_STATUS  0x3F6

#define ATA_STATUS_ERR  0x01
#define ATA_STATUS_DRQ  0x08
#define ATA_STATUS_BSY  0x80
#define ATA_CMD_READ    0x20
#define ATA_CMD_WRITE   0x30
#define ATA_CMD_FLUSH   0xE7

#define ATA_SEL_MASTER  0xE0

static inline void outb(uint16_t port, uint8_t val) {
    __asm__ volatile ("outb %0,%1" :: "a"(val), "Nd"(port));
}
static inline void outw(uint16_t port, uint16_t val) {
    __asm__ volatile ("outw %0,%1" :: "a"(val), "Nd"(port));
}
static inline uint8_t inb(uint16_t port) {
    uint8_t r; __asm__ volatile ("inb %1,%0" : "=a"(r) : "Nd"(port)); return r;
}
static inline uint16_t inw(uint16_t port) {
    uint16_t r; __asm__ volatile ("inw %1,%0" : "=a"(r) : "Nd"(port)); return r;
}

static void ata64_delay(void) {
    inb(ATA_ALT_STATUS); inb(ATA_ALT_STATUS);
    inb(ATA_ALT_STATUS); inb(ATA_ALT_STATUS);
}

static int ata64_wait_not_busy(void) {
    uint32_t timeout = 500000;
    while (timeout--) {
        uint8_t s = inb(ATA_STATUS);
        if (s == 0xFF) return -1;
        if (!(s & ATA_STATUS_BSY)) return 0;
    }
    return -1;
}

static int ata64_wait_drq(void) {
    uint32_t timeout = 500000;
    while (timeout--) {
        uint8_t s = inb(ATA_STATUS);
        if (s == 0xFF) return -1;
        if (s & ATA_STATUS_DRQ) return 0;
    }
    return -1;
}

void ata64_init(void) {
    outb(ATA_ALT_STATUS, 0x04);  // SRST=1
    ata64_delay();
    outb(ATA_ALT_STATUS, 0x00);  // SRST=0
    ata64_delay();

    outb(ATA_DRIVE_SEL, ATA_SEL_MASTER);
    ata64_delay();
    ata64_wait_not_busy();
}

int ata64_read(uint32_t lba, uint8_t* buf, uint32_t sectors) {
    for (uint32_t s = 0; s < sectors; s++) {
        uint32_t cur = lba + s;

        if (ata64_wait_not_busy() < 0) return -1;

        int drq_ok = 0;
        for (int retry = 0; retry < 5; retry++) {
            inb(ATA_ERROR);  // clear stale error
            outb(ATA_DRIVE_SEL, ATA_SEL_MASTER | ((cur >> 24) & 0x0F));
            ata64_delay();
            ata64_wait_not_busy();

            outb(ATA_SECTOR_CNT, 1);
            outb(ATA_LBA_LO,  cur & 0xFF);
            outb(ATA_LBA_MID, (cur >> 8) & 0xFF);
            outb(ATA_LBA_HI,  (cur >> 16) & 0xFF);
            outb(ATA_STATUS, ATA_CMD_READ);
            ata64_delay();

            uint8_t st = inb(ATA_STATUS);
            if (st & ATA_STATUS_ERR) { inb(ATA_ERROR); continue; }
            if (ata64_wait_drq() == 0) { drq_ok = 1; break; }
        }

        if (!drq_ok) {
            outb(ATA_DRIVE_SEL, ATA_SEL_MASTER); ata64_delay();
            return -1;
        }

        uint16_t* p = (uint16_t*)(buf + s * 512);
        for (int i = 0; i < 256; i++) p[i] = inw(ATA_DATA);
    }

    outb(ATA_DRIVE_SEL, ATA_SEL_MASTER);
    ata64_delay();
    return (int)sectors;
}

// Milestone 19: ATA PIO write -- mirrors ata64_read's retry logic but
// uses WRITE_SECTORS (0x30) and transfers data OUT via outw instead of
// IN via inw. A FLUSH_CACHE (0xE7) is issued after all sectors to
// ensure data reaches the disk (required for QEMU's virtio-backed raw
// images, which may otherwise defer the write).
int ata64_write(uint32_t lba, const uint8_t* buf, uint32_t sectors) {
    for (uint32_t s = 0; s < sectors; s++) {
        uint32_t cur = lba + s;

        if (ata64_wait_not_busy() < 0) return -1;

        int drq_ok = 0;
        for (int retry = 0; retry < 5; retry++) {
            inb(ATA_ERROR);
            outb(ATA_DRIVE_SEL, ATA_SEL_MASTER | ((cur >> 24) & 0x0F));
            ata64_delay();
            ata64_wait_not_busy();

            outb(ATA_SECTOR_CNT, 1);
            outb(ATA_LBA_LO,  cur & 0xFF);
            outb(ATA_LBA_MID, (cur >> 8) & 0xFF);
            outb(ATA_LBA_HI,  (cur >> 16) & 0xFF);
            outb(ATA_STATUS, ATA_CMD_WRITE);
            ata64_delay();

            uint8_t st = inb(ATA_STATUS);
            if (st & ATA_STATUS_ERR) { inb(ATA_ERROR); continue; }
            if (ata64_wait_drq() == 0) { drq_ok = 1; break; }
        }

        if (!drq_ok) {
            outb(ATA_DRIVE_SEL, ATA_SEL_MASTER); ata64_delay();
            return -1;
        }

        const uint16_t* p = (const uint16_t*)(buf + s * 512);
        for (int i = 0; i < 256; i++) outw(ATA_DATA, p[i]);

        if (ata64_wait_not_busy() < 0) return -1;
    }

    // Flush cache so the write reaches the backing image
    if (ata64_wait_not_busy() < 0) return -1;
    outb(ATA_DRIVE_SEL, ATA_SEL_MASTER);
    ata64_delay();
    outb(ATA_STATUS, ATA_CMD_FLUSH);
    ata64_delay();
    ata64_wait_not_busy();

    outb(ATA_DRIVE_SEL, ATA_SEL_MASTER);
    ata64_delay();
    return (int)sectors;
}
