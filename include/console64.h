#ifndef CONSOLE64_H
#define CONSOLE64_H

#include <stdint.h>

// Milestone 21: console abstraction layer for ToxenOS64.
// Selects between the framebuffer terminal (fbterm64) and the VGA text-mode
// terminal (vgaterm64) as the visible output backend. Kernel code and the
// syscall layer route all display writes through this interface rather than
// calling either backend directly, so the display backend can be swapped
// without touching callers.
//
// console64_init() should be called early in kernel_main64 once the
// multiboot2 framebuffer info is available. If no valid framebuffer is found
// (bpp != 32, zero size, or fb mapping fails), the VGA terminal is used and
// console64_write/clear/set_color behave identically to the vgaterm64 calls.

void console64_init(uint64_t fb_addr, uint32_t width, uint32_t height,
                    uint32_t pitch, uint8_t bpp);

void console64_write(const char* buf, uint64_t len);
void console64_clear(void);
void console64_set_color(uint8_t attr);

#endif // CONSOLE64_H
