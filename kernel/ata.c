// ToxenOS/kernel/ata.c
// ATA PIO driver — supports all 4 drives across primary and secondary channels
//
// Drive numbers:
//   0 = primary master   (ports 0x1F0, select 0xE0)
//   1 = primary slave    (ports 0x1F0, select 0xF0)
//   2 = secondary master (ports 0x170, select 0xE0)
//   3 = secondary slave  (ports 0x170, select 0xF0)

#include <stdint.h>
#include "../include/ata.h"
#include "../include/vga.h"

static inline void outb(uint16_t port, uint8_t val)
    { __asm__ volatile("outb %0,%1"::"a"(val),"Nd"(port)); }
static inline uint8_t inb(uint16_t port)
    { uint8_t r; __asm__ volatile("inb %1,%0":"=a"(r):"Nd"(port)); return r; }
static inline uint16_t inw(uint16_t port)
    { uint16_t r; __asm__ volatile("inw %1,%0":"=a"(r):"Nd"(port)); return r; }
static inline void outw(uint16_t port, uint16_t val)
    { __asm__ volatile("outw %0,%1"::"a"(val),"Nd"(port)); }

// ── Channel descriptor ────────────────────────────────────────────────────────

typedef struct {
    uint16_t data;        // 0x1F0 or 0x170
    uint16_t error;       // 0x1F1 or 0x171
    uint16_t sec_count;   // 0x1F2 or 0x172
    uint16_t lba_lo;      // 0x1F3 or 0x173
    uint16_t lba_mid;     // 0x1F4 or 0x174
    uint16_t lba_hi;      // 0x1F5 or 0x175
    uint16_t drive_sel;   // 0x1F6 or 0x176
    uint16_t status;      // 0x1F7 or 0x177
    uint16_t alt_status;  // 0x3F6 or 0x376
} ata_channel_t;

static const ata_channel_t channels[2] = {
    // Primary
    { 0x1F0, 0x1F1, 0x1F2, 0x1F3, 0x1F4, 0x1F5, 0x1F6, 0x1F7, 0x3F6 },
    // Secondary
    { 0x170, 0x171, 0x172, 0x173, 0x174, 0x175, 0x176, 0x177, 0x376 },
};

// Get channel index (0=primary, 1=secondary) and drive select byte from drive number
static void ata_decode_drive(uint8_t drive, int* ch_idx, uint8_t* sel)
{
    *ch_idx = (drive >= 2) ? 1 : 0;          // 0,1 = primary; 2,3 = secondary
    *sel    = (drive & 1) ? 0xF0 : 0xE0;     // odd = slave, even = master
}

// ── Timing ────────────────────────────────────────────────────────────────────

static void ata_delay(const ata_channel_t* ch)
{
    // Read alt-status 4 times = ~400ns
    inb(ch->alt_status); inb(ch->alt_status);
    inb(ch->alt_status); inb(ch->alt_status);
}

// ── Status polling ────────────────────────────────────────────────────────────

static int ata_wait_not_busy(const ata_channel_t* ch)
{
    uint32_t timeout = 500000;
    while (timeout--) {
        uint8_t s = inb(ch->status);
        if (s == 0xFF) return -1;
        if (!(s & ATA_STATUS_BSY)) return 0;
    }
    return -1;
}

static int ata_wait_drq(const ata_channel_t* ch)
{
    uint32_t timeout = 500000;
    while (timeout--) {
        uint8_t s = inb(ch->status);
        if (s == 0xFF) return -1;
        if (s & ATA_STATUS_DRQ) return 0;
    }
    return -1;
}

// ── Init ──────────────────────────────────────────────────────────────────────

static void ata_init_channel(const ata_channel_t* ch)
{
    // Software reset
    outb(ch->alt_status, 0x04);  // SRST=1
    ata_delay(ch);
    outb(ch->alt_status, 0x00);  // SRST=0
    ata_delay(ch);

    // Init master
    outb(ch->drive_sel, 0xA0);
    ata_delay(ch);
    ata_wait_not_busy(ch);
    outb(ch->sec_count, 0); outb(ch->lba_lo, 0);
    outb(ch->lba_mid, 0);   outb(ch->lba_hi, 0);
    outb(ch->status, 0xEC); // IDENTIFY
    ata_delay(ch);
    uint8_t s = inb(ch->status);
    if (s && s != 0xFF) {
        ata_wait_not_busy(ch);
        for (int i=0; i<256; i++) inw(ch->data);
    }

    // Init slave
    outb(ch->drive_sel, 0xF0);
    ata_delay(ch);
    s = inb(ch->status);
    if (s && s != 0xFF) {
        outb(ch->sec_count, 0); outb(ch->lba_lo, 0);
        outb(ch->lba_mid, 0);   outb(ch->lba_hi, 0);
        outb(ch->status, 0xEC);
        ata_delay(ch);
        ata_wait_not_busy(ch);
        s = inb(ch->status);
        if (s & ATA_STATUS_DRQ)
            for (int i=0; i<256; i++) inw(ch->data);
    }

    // Restore master
    outb(ch->drive_sel, 0xA0);
    ata_delay(ch);
}

// Wake up slave drives by issuing several reads — QEMU ignores the first few
static void ata_wake_slave(const ata_channel_t* ch)
{
    uint8_t slave_sel = 0xF0;

    // Check slave exists
    outb(ch->drive_sel, slave_sel);
    ata_delay(ch);
    uint8_t s = inb(ch->status);
    if (s == 0x00 || s == 0xFF) {
        outb(ch->drive_sel, 0xA0); ata_delay(ch);
        return;
    }

    // Issue 5 dummy reads at LBA 0 to wake the drive up
    for (int i = 0; i < 5; i++) {
        outb(ch->drive_sel, slave_sel);
        ata_delay(ch);
        ata_wait_not_busy(ch);
        inb(ch->error);
        outb(ch->sec_count, 1);
        outb(ch->lba_lo, 1);  // LBA 1, not 0 — LBA 0 always ABORTs on QEMU slave
        outb(ch->lba_mid, 0);
        outb(ch->lba_hi, 0);
        outb(ch->status, ATA_CMD_READ);
        ata_delay(ch);
        // Wait briefly for DRQ
        uint32_t t = 100000;
        while (t--) {
            s = inb(ch->status);
            if (s & ATA_STATUS_DRQ) {
                for (int j = 0; j < 256; j++) inw(ch->data);
                break;
            }
            if (!(s & ATA_STATUS_BSY)) break;
        }
    }

    outb(ch->drive_sel, 0xA0);
    ata_delay(ch);
}

int ata_init()
{
    ata_init_channel(&channels[0]);  // primary
    ata_init_channel(&channels[1]);  // secondary
    // Wake up slave drives on both channels
    ata_wake_slave(&channels[0]);
    ata_wake_slave(&channels[1]);
    return 0;
}

// ── Core read/write ───────────────────────────────────────────────────────────

static int ata_do_read(const ata_channel_t* ch, uint8_t sel, uint32_t lba,
                       uint8_t* buf, uint32_t sectors)
{
    for (uint32_t s = 0; s < sectors; s++) {
        uint32_t cur = lba + s;

        if (ata_wait_not_busy(ch) < 0) return -1;

        outb(ch->drive_sel, sel | ((cur >> 24) & 0x0F));
        ata_delay(ch);
        if (ata_wait_not_busy(ch) < 0) {
            outb(ch->drive_sel, (sel & 0xF0) == 0xF0 ? 0xA0 : 0xA0);
            ata_delay(ch);
            return -1;
        }

        // Retry loop
        int drq_ok = 0;
        for (int retry = 0; retry < 5; retry++) {
            inb(ch->error);  // clear stale error
            outb(ch->drive_sel, sel | ((cur >> 24) & 0x0F));
            ata_delay(ch);
            ata_wait_not_busy(ch);

            outb(ch->sec_count, 1);
            outb(ch->lba_lo,  cur & 0xFF);
            outb(ch->lba_mid, (cur >> 8) & 0xFF);
            outb(ch->lba_hi,  (cur >> 16) & 0xFF);
            outb(ch->status, ATA_CMD_READ);
            ata_delay(ch);

            uint8_t st = inb(ch->status);
            if (st & ATA_STATUS_ERR) { inb(ch->error); continue; }
            if (ata_wait_drq(ch) == 0) { drq_ok = 1; break; }
        }

        if (!drq_ok) {
            outb(ch->drive_sel, 0xA0); ata_delay(ch);
            return -1;
        }

        uint16_t* p = (uint16_t*)(buf + s * 512);
        for (int i = 0; i < 256; i++) p[i] = inw(ch->data);
    }

    // Restore master on this channel
    outb(ch->drive_sel, 0xA0);
    ata_delay(ch);
    return (int)sectors;
}

static int ata_do_write(const ata_channel_t* ch, uint8_t sel, uint32_t lba,
                        const uint8_t* buf, uint32_t sectors)
{
    for (uint32_t s = 0; s < sectors; s++) {
        uint32_t cur = lba + s;

        if (ata_wait_not_busy(ch) < 0) return -1;

        outb(ch->drive_sel, sel | ((cur >> 24) & 0x0F));
        ata_delay(ch);
        if (ata_wait_not_busy(ch) < 0) {
            outb(ch->drive_sel, 0xA0); ata_delay(ch);
            return -1;
        }

        outb(ch->sec_count, 1);
        outb(ch->lba_lo,  cur & 0xFF);
        outb(ch->lba_mid, (cur >> 8) & 0xFF);
        outb(ch->lba_hi,  (cur >> 16) & 0xFF);
        outb(ch->status, ATA_CMD_WRITE);
        ata_delay(ch);

        if (ata_wait_drq(ch) < 0) {
            outb(ch->drive_sel, 0xA0); ata_delay(ch);
            return -1;
        }

        const uint16_t* p = (const uint16_t*)(buf + s * 512);
        for (int i = 0; i < 256; i++) outw(ch->data, p[i]);

        outb(ch->status, 0xE7);  // FLUSH CACHE
        ata_wait_not_busy(ch);
    }

    outb(ch->drive_sel, 0xA0);
    ata_delay(ch);
    return (int)sectors;
}

// ── Public API ────────────────────────────────────────────────────────────────

// AHCI / NVMe / VirtIO / ramdisk fallback — redirect disk I/O if a faster driver is active
#include "../include/ahci.h"
#include "../include/nvme.h"
#include "../include/virtio_blk.h"
static int      use_ahci    = 0;
static int      use_nvme    = 0;
static int      use_virtio  = 0;
static int      nvme_ready   = 0;
static int      ahci_ready   = 0;
static int      virtio_ready = 0;
static uint8_t *ramdisk_buf  = 0;
static uint32_t ramdisk_size = 0;

void ata_set_ahci(int v)         { use_ahci    = v; }
void ata_set_nvme(int v)         { use_nvme    = v; }
void ata_set_virtio(int v)       { use_virtio  = v; }
void ata_set_nvme_ready(int v)   { nvme_ready   = v; }
void ata_set_ahci_ready(int v)   { ahci_ready   = v; }
void ata_set_virtio_ready(int v) { virtio_ready = v; }
void ata_set_ramdisk(uint8_t *buf, uint32_t size) { ramdisk_buf = buf; ramdisk_size = size; }
int  ata_has_ramdisk(void) { return ramdisk_buf != 0; }

// In-memory read/write for the ramdisk (always drive 0)
static int ramdisk_read(uint32_t lba, uint8_t *buf, uint32_t sectors)
{
    uint32_t off = lba * 512, nbytes = sectors * 512;
    if (off + nbytes > ramdisk_size) return -1;
    for (uint32_t i = 0; i < nbytes; i++) buf[i] = ramdisk_buf[off + i];
    return (int)sectors;
}
static int ramdisk_write(uint32_t lba, const uint8_t *buf, uint32_t sectors)
{
    uint32_t off = lba * 512, nbytes = sectors * 512;
    if (off + nbytes > ramdisk_size) return -1;
    for (uint32_t i = 0; i < nbytes; i++) ramdisk_buf[off + i] = buf[i];
    return (int)sectors;
}

int ata_read(uint32_t lba, uint8_t* buf, uint32_t sectors)
{
    if (ramdisk_buf)  return ramdisk_read(lba, buf, sectors);
    if (use_nvme)     return nvme_read_drive(0, lba, buf, sectors);
    if (use_ahci)     return ahci_read_drive(0, lba, buf, sectors);
    if (use_virtio)   return virtio_blk_read(0, lba, buf, sectors);
    return ata_do_read(&channels[0], 0xE0, lba, buf, sectors);
}

int ata_write(uint32_t lba, const uint8_t* buf, uint32_t sectors)
{
    if (ramdisk_buf)  return ramdisk_write(lba, buf, sectors);
    if (use_nvme)     return nvme_write_drive(0, lba, buf, sectors);
    if (use_ahci)     return ahci_write_drive(0, lba, buf, sectors);
    if (use_virtio)   return virtio_blk_write(0, lba, buf, sectors);
    return ata_do_write(&channels[0], 0xE0, lba, buf, sectors);
}

int ata_read_drive(uint8_t drive, uint32_t lba, uint8_t* buf, uint32_t sectors)
{
    if (drive == 0 && ramdisk_buf) return ramdisk_read(lba, buf, sectors);
    if (use_nvme)   return nvme_read_drive(drive, lba, buf, sectors);
    if (use_ahci)   return ahci_read_drive(drive, lba, buf, sectors);
    if (use_virtio) return virtio_blk_read(drive, lba, buf, sectors);
    if (ramdisk_buf && nvme_ready   && drive == 1) return nvme_read_drive(0, lba, buf, sectors);
    if (ramdisk_buf && ahci_ready   && drive == 1) return ahci_read_drive(0, lba, buf, sectors);
    if (ramdisk_buf && virtio_ready && drive == 1) return virtio_blk_read(0, lba, buf, sectors);
    int ch_idx; uint8_t sel;
    ata_decode_drive(drive, &ch_idx, &sel);
    return ata_do_read(&channels[ch_idx], sel, lba, buf, sectors);
}

int ata_write_drive(uint8_t drive, uint32_t lba, const uint8_t* buf, uint32_t sectors)
{
    if (drive == 0 && ramdisk_buf) return ramdisk_write(lba, buf, sectors);
    if (use_nvme)   return nvme_write_drive(drive, lba, buf, sectors);
    if (use_ahci)   return ahci_write_drive(drive, lba, buf, sectors);
    if (use_virtio) return virtio_blk_write(drive, lba, buf, sectors);
    if (ramdisk_buf && nvme_ready   && drive == 1) return nvme_write_drive(0, lba, buf, sectors);
    if (ramdisk_buf && ahci_ready   && drive == 1) return ahci_write_drive(0, lba, buf, sectors);
    if (ramdisk_buf && virtio_ready && drive == 1) return virtio_blk_write(0, lba, buf, sectors);
    int ch_idx; uint8_t sel;
    ata_decode_drive(drive, &ch_idx, &sel);
    return ata_do_write(&channels[ch_idx], sel, lba, buf, sectors);
}

// Return total 512-byte sectors on drive.
// When ramdisk is the TxFS source, drive 0 = ramdisk and drive 1 = NVMe/AHCI target.
uint32_t ata_get_sectors(uint8_t drive)
{
    if (drive == 0 && ramdisk_buf) return ramdisk_size / 512;
    if (use_nvme)   return nvme_get_sectors(drive);
    if (use_ahci)   return ahci_get_sectors(drive);
    if (use_virtio) return virtio_blk_sectors(drive);
    if (ramdisk_buf && nvme_ready   && drive == 1) return nvme_get_sectors(0);
    if (ramdisk_buf && ahci_ready   && drive == 1) return ahci_get_sectors(0);
    if (ramdisk_buf && virtio_ready && drive == 1) return virtio_blk_sectors(0);

    // Fall through to ATA PIO IDENTIFY
    int ch_idx; uint8_t sel;
    ata_decode_drive(drive, &ch_idx, &sel);
    const ata_channel_t* ch = &channels[ch_idx];

    if (ata_wait_not_busy(ch) < 0) return 0;
    outb(ch->drive_sel, sel);
    ata_delay(ch);
    outb(ch->sec_count, 0); outb(ch->lba_lo, 0);
    outb(ch->lba_mid,   0); outb(ch->lba_hi, 0);
    outb(ch->status, 0xEC); // IDENTIFY
    ata_delay(ch);

    uint8_t s = inb(ch->status);
    if (!s || s == 0xFF) return 0;
    if (ata_wait_not_busy(ch) < 0) return 0;
    if (!(inb(ch->status) & ATA_STATUS_DRQ)) return 0;

    uint16_t id[256];
    for (int i = 0; i < 256; i++) id[i] = inw(ch->data);

    // Words 60-61: 28-bit LBA total sectors
    uint32_t lba28 = (uint32_t)id[60] | ((uint32_t)id[61] << 16);
    return lba28;
}
