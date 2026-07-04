// kernel/console64.c — Milestone 21: console abstraction layer.
// Dispatches write/clear/set_color to either the framebuffer terminal
// (fbterm64) or the VGA text-mode terminal (vgaterm64) depending on
// which backend initialized successfully.  All kernel output paths
// (kernel_main64 diagnostics + syscall64's sys64_write/clear/set_color)
// go through this layer, making the display backend a single-point choice.
#include <stdint.h>
#include "../include/console64.h"
#include "../include/fbterm64.h"
#include "../include/vgaterm64.h"

void console64_init(uint64_t fb_addr, uint32_t width, uint32_t height,
                    uint32_t pitch, uint8_t bpp)
{
    // Try framebuffer first; fbterm64_init is a no-op (leaves fb_avail=0)
    // if bpp != 32, dimensions are tiny, or the physical mapping fails.
    fbterm64_init(fb_addr, width, height, pitch, bpp);
    // VGA text mode requires no explicit init — hardware default is valid.
}

void console64_write(const char* buf, uint64_t len)
{
    if (fbterm64_available()) fbterm64_write(buf, len);
    else                      vgaterm64_write(buf, len);
}

void console64_clear(void)
{
    if (fbterm64_available()) fbterm64_clear();
    else                      vgaterm64_clear();
}

void console64_set_color(uint8_t attr)
{
    if (fbterm64_available()) fbterm64_set_color(attr);
    else                      vgaterm64_set_color(attr);
}
