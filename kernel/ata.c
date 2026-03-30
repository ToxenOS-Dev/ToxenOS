// ToxenOS/kernel/ata.c
// ATA PIO driver — supports master and slave on primary IDE channel
//
// Key points:
//   - Drive select byte: 0xE0 = master LBA, 0xF0 = slave LBA
//   - After selecting a drive, wait 400ns (4 alt-status reads) before using it
//   - Never check ERR bit right after drive select — it reflects previous state
//   - Software reset resets BOTH drives — only use it during init
//   - Always restore master selection after slave operations

#include <stdint.h>
#include "../include/ata.h"
#include "../include/vga.h"

// ── port I/O ──────────────────────────────────────────────────────────────────

static inline void outb(uint16_t port, uint8_t val)
    { __asm__ volatile("outb %0,%1"::"a"(val),"Nd"(port)); }
static inline uint8_t inb(uint16_t port)
    { uint8_t r; __asm__ volatile("inb %1,%0":"=a"(r):"Nd"(port)); return r; }
static inline uint16_t inw(uint16_t port)
    { uint16_t r; __asm__ volatile("inw %1,%0":"=a"(r):"Nd"(port)); return r; }
static inline void outw(uint16_t port, uint16_t val)
    { __asm__ volatile("outw %0,%1"::"a"(val),"Nd"(port)); }

// 400ns delay — read alternate status register 4 times
static void ata_400ns()
{
    inb(0x3F6); inb(0x3F6); inb(0x3F6); inb(0x3F6);
}

// ── status polling ────────────────────────────────────────────────────────────

// Wait for BSY=0, check ERR
static int ata_wait_ready()
{
    uint32_t timeout = 500000;
    while (timeout--) {
        uint8_t s = inb(0x1F7);
        if (s == 0xFF) return -1;          // no drive
        if (s & 0x01) return -1;           // ERR set
        if (!(s & 0x80)) return 0;         // BSY clear
    }
    return -1;
}

// Wait for BSY=0, ignore ERR — used right after drive select
static int ata_wait_not_busy()
{
    uint32_t timeout = 500000;
    while (timeout--) {
        uint8_t s = inb(0x1F7);
        if (s == 0xFF) return -1;
        if (!(s & 0x80)) return 0;
    }
    return -1;
}

// Wait for DRQ=1 (data ready), ignore ERR
static int ata_wait_drq()
{
    uint32_t timeout = 500000;
    while (timeout--) {
        uint8_t s = inb(0x1F7);
        if (s == 0xFF) return -1;
        if (s & 0x08) return 0;            // DRQ set
    }
    return -1;
}

// ── drive selection ───────────────────────────────────────────────────────────

// ── init ──────────────────────────────────────────────────────────────────────

int ata_init()
{
    // Software reset both drives
    outb(0x3F6, 0x04);  // SRST=1, nIEN=0
    ata_400ns();
    outb(0x3F6, 0x00);  // SRST=0
    ata_400ns();

    // Wait for master to be ready after reset
    outb(0x1F6, 0xA0);
    ata_400ns();
    ata_wait_not_busy();

    // Identify master
    outb(0x1F6, 0xA0);
    ata_400ns();
    outb(0x1F2, 0); outb(0x1F3, 0); outb(0x1F4, 0); outb(0x1F5, 0);
    outb(0x1F7, 0xEC);  // IDENTIFY
    ata_400ns();
    uint8_t s = inb(0x1F7);
    if (s && s != 0xFF) {
        ata_wait_not_busy();
        for (int i=0; i<256; i++) inw(0x1F0);
    }

    // Identify slave
    outb(0x1F6, 0xF0);
    ata_400ns();
    s = inb(0x1F7);
    if (s && s != 0xFF) {
        outb(0x1F2, 0); outb(0x1F3, 0); outb(0x1F4, 0); outb(0x1F5, 0);
        outb(0x1F7, 0xEC);
        ata_400ns();
        ata_wait_not_busy();
        // drain IDENTIFY data if DRQ set
        s = inb(0x1F7);
        if (s & 0x08) {
            for (int i=0; i<256; i++) inw(0x1F0);
        }
    }

    // Leave master selected
    outb(0x1F6, 0xA0);
    ata_400ns();
    return 0;
}

// ── read (master) ─────────────────────────────────────────────────────────────

int ata_read(uint32_t lba, uint8_t* buf, uint32_t sectors)
{
    for (uint32_t s = 0; s < sectors; s++) {
        uint32_t cur = lba + s;

        if (ata_wait_ready() < 0) return -1;
        outb(0x1F6, 0xE0 | ((cur >> 24) & 0x0F));
        ata_400ns();
        if (ata_wait_not_busy() < 0) return -1;

        outb(0x1F2, 1);
        outb(0x1F3, cur & 0xFF);
        outb(0x1F4, (cur >> 8) & 0xFF);
        outb(0x1F5, (cur >> 16) & 0xFF);
        outb(0x1F7, 0x20);  // READ SECTORS
        ata_400ns();

        if (ata_wait_drq() < 0) return -1;

        uint16_t* p = (uint16_t*)(buf + s * 512);
        for (int i=0; i<256; i++) p[i] = inw(0x1F0);
    }
    return (int)sectors;
}

// ── write (master) ────────────────────────────────────────────────────────────

int ata_write(uint32_t lba, const uint8_t* buf, uint32_t sectors)
{
    for (uint32_t s = 0; s < sectors; s++) {
        uint32_t cur = lba + s;

        if (ata_wait_ready() < 0) return -1;
        outb(0x1F6, 0xE0 | ((cur >> 24) & 0x0F));
        ata_400ns();
        if (ata_wait_not_busy() < 0) return -1;

        outb(0x1F2, 1);
        outb(0x1F3, cur & 0xFF);
        outb(0x1F4, (cur >> 8) & 0xFF);
        outb(0x1F5, (cur >> 16) & 0xFF);
        outb(0x1F7, 0x30);  // WRITE SECTORS
        ata_400ns();

        if (ata_wait_drq() < 0) return -1;

        const uint16_t* p = (const uint16_t*)(buf + s * 512);
        for (int i=0; i<256; i++) outw(0x1F0, p[i]);

        outb(0x1F7, 0xE7);  // FLUSH CACHE
        ata_wait_not_busy();
    }
    return (int)sectors;
}

// ── read (any drive) ──────────────────────────────────────────────────────────

int ata_read_drive(uint8_t drive, uint32_t lba, uint8_t* buf, uint32_t sectors)
{
    uint8_t drive_sel = (drive == ATA_DRIVE_SLAVE) ? 0xF0 : 0xE0;

    for (uint32_t s = 0; s < sectors; s++) {
        uint32_t cur = lba + s;

        if (ata_wait_not_busy() < 0) return -1;

        outb(0x1F6, drive_sel | ((cur >> 24) & 0x0F));
        ata_400ns();
        if (ata_wait_not_busy() < 0) {
            outb(0x1F6, 0xA0); ata_400ns();
            return -1;
        }

        // Retry loop — controller sometimes needs a second attempt
        int drq_ok = 0;
        for (int retry = 0; retry < 5; retry++) {
            inb(0x1F1);  // clear stale error
            outb(0x1F6, drive_sel | ((cur >> 24) & 0x0F));
            ata_400ns();
            ata_wait_not_busy();

            outb(0x1F2, 1);
            outb(0x1F3, cur & 0xFF);
            outb(0x1F4, (cur >> 8) & 0xFF);
            outb(0x1F5, (cur >> 16) & 0xFF);
            outb(0x1F7, 0x20);
            ata_400ns();

            uint8_t st = inb(0x1F7);
            if (st & 0x01) { inb(0x1F1); continue; }
            if (ata_wait_drq() == 0) { drq_ok = 1; break; }
        }

        if (!drq_ok) {
            outb(0x1F6, 0xA0); ata_400ns();
            return -1;
        }

        uint16_t* p = (uint16_t*)(buf + s * 512);
        for (int i=0; i<256; i++) p[i] = inw(0x1F0);
    }

    outb(0x1F6, 0xA0);
    ata_400ns();
    return (int)sectors;
}

// ── write (any drive) ─────────────────────────────────────────────────────────

int ata_write_drive(uint8_t drive, uint32_t lba, const uint8_t* buf, uint32_t sectors)
{
    uint8_t drive_sel = (drive == ATA_DRIVE_SLAVE) ? 0xF0 : 0xE0;

    for (uint32_t s = 0; s < sectors; s++) {
        uint32_t cur = lba + s;

        if (ata_wait_not_busy() < 0) return -1;

        outb(0x1F6, drive_sel | ((cur >> 24) & 0x0F));
        ata_400ns();

        if (ata_wait_not_busy() < 0) {
            outb(0x1F6, 0xA0); ata_400ns();
            return -1;
        }

        outb(0x1F2, 1);
        outb(0x1F3, cur & 0xFF);
        outb(0x1F4, (cur >> 8) & 0xFF);
        outb(0x1F5, (cur >> 16) & 0xFF);
        outb(0x1F7, 0x30);  // WRITE SECTORS
        ata_400ns();

        if (ata_wait_drq() < 0) {
            outb(0x1F6, 0xA0); ata_400ns();
            return -1;
        }

        const uint16_t* p = (const uint16_t*)(buf + s * 512);
        for (int i=0; i<256; i++) outw(0x1F0, p[i]);

        outb(0x1F7, 0xE7);
        ata_wait_not_busy();
    }

    outb(0x1F6, 0xA0);
    ata_400ns();
    return (int)sectors;
}
