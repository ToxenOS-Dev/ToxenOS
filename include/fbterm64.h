#ifndef FBTERM64_H
#define FBTERM64_H

#include <stdint.h>

// Milestone 21: framebuffer terminal backend for ToxenOS64.
// Renders text via a scaled 8x16 bitmap font directly into a linear
// framebuffer (32bpp RGB only). VGA 4-bit color attributes map to
// standard 32-bit RGB values. Font is rendered at 2x scale (16x32
// pixels per character) for comfortable readability at 1024x768.
//
// Call fbterm64_available() to check whether init succeeded before
// routing any output through this backend — console64.c does this
// automatically and falls back to vgaterm64 if unavailable.

void fbterm64_init(uint64_t fb_addr, uint32_t width, uint32_t height,
                   uint32_t pitch, uint8_t bpp);

void fbterm64_write(const char* buf, uint64_t len);
void fbterm64_clear(void);
void fbterm64_set_color(uint8_t attr);
int  fbterm64_available(void);

#endif // FBTERM64_H
