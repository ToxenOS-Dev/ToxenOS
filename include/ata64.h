#ifndef ATA64_H
#define ATA64_H

#include <stdint.h>

// Minimal ATA PIO driver for the x86_64 kernel — Milestone 4 (read-only
// filesystem access). A deliberate subset of kernel/ata.c's API, not a
// verbatim reuse: the 32-bit driver pulls in AHCI/NVMe/VirtIO and
// redirects through them at runtime, which would force linking three
// unported drivers for code paths that would never execute here.
// Primary channel (0x1F0-0x1F7/0x3F6), master drive only.
void ata64_init(void);
int  ata64_read(uint32_t lba, uint8_t* buf, uint32_t sectors);
// Milestone 19: write path (mirrors read; blocks/sectors same layout).
int  ata64_write(uint32_t lba, const uint8_t* buf, uint32_t sectors);

#endif // ATA64_H
