#include <stdint.h>
#include "../include/framebuffer.h"
#include "../include/paging.h"
#include "../include/memmap.h"

// ── VGA text mode constants ───────────────────────────────────────────────────
// Used as fallback when no framebuffer is available
#define VGA_TEXT_PHYS   0x000B8000u
#define VGA_TEXT_COLS   80
#define VGA_TEXT_ROWS   25
#define VGA_TEXT_VIRT   (VGA_TEXT_PHYS + KERNEL_VIRT_BASE)

static uint32_t* fb_addr;
static uint32_t  fb_width;
static uint32_t  fb_height;
static uint32_t  fb_pitch;
static uint32_t  fb_bpp;
static int       fb_vga_fallback = 0; // 1 = using VGA text mode

// ── VGA 16-colour palette (index 0-15) → 32-bit RGB ─────────────────────────
static const uint32_t vga_rgb[16] = {
    0x000000, 0x0000AA, 0x00AA00, 0x00AAAA,
    0xAA0000, 0xAA00AA, 0xAA5500, 0xAAAAAA,
    0x555555, 0x5555FF, 0x55FF55, 0x55FFFF,
    0xFF5555, 0xFF55FF, 0xFFFF55, 0xFFFFFF,
};

void fb_init(uint32_t addr, uint32_t w, uint32_t h, uint32_t pitch, uint32_t bpp)
{
    if (!addr || !w || !h) {
        // No framebuffer — fall back to VGA text mode
        fb_vga_fallback = 1;
        fb_width  = VGA_TEXT_COLS * 8;   // pretend 640px wide
        fb_height = VGA_TEXT_ROWS * 16;  // pretend 400px tall
        fb_pitch  = VGA_TEXT_COLS * 8 * 4;
        fb_bpp    = 32;
        fb_addr   = 0;

        // Map VGA text buffer
        extern uint32_t kernel_directory[];
        paging_map(kernel_directory, VGA_TEXT_PHYS, VGA_TEXT_PHYS,
                   PAGE_PRESENT | PAGE_WRITABLE);

        // Clear text buffer (space + white-on-black attribute 0x07)
        volatile uint16_t* vga = (volatile uint16_t*)VGA_TEXT_VIRT;
        for (int i = 0; i < VGA_TEXT_COLS * VGA_TEXT_ROWS; i++)
            vga[i] = 0x0720; // space, white on black
        return;
    }

    fb_vga_fallback = 0;
    fb_addr   = (uint32_t*)addr;
    fb_width  = w;
    fb_height = h;
    fb_pitch  = pitch;
    fb_bpp    = bpp;

    fb_clear(0x000000);
}

void fb_put_pixel(uint32_t x, uint32_t y, uint32_t color)
{
    if (fb_vga_fallback) return; // VGA text mode: pixels not supported
    if (x >= fb_width || y >= fb_height) return;
    uint32_t offset = y * (fb_pitch / 4) + x;
    fb_addr[offset] = color;
}

void fb_clear(uint32_t color)
{
    if (fb_vga_fallback) {
        volatile uint16_t* vga = (volatile uint16_t*)VGA_TEXT_VIRT;
        for (int i = 0; i < VGA_TEXT_COLS * VGA_TEXT_ROWS; i++)
            vga[i] = 0x0720;
        return;
    }
    for (uint32_t y = 0; y < fb_height; y++)
        for (uint32_t x = 0; x < fb_width; x++)
            fb_put_pixel(x, y, color);
}

void fb_draw_rect(uint32_t x, uint32_t y, uint32_t w, uint32_t h, uint32_t color)
{
    if (fb_vga_fallback) return;
    for (uint32_t dy = 0; dy < h; dy++)
        for (uint32_t dx = 0; dx < w; dx++)
            fb_put_pixel(x + dx, y + dy, color);
}

uint32_t fb_get_width()    { return fb_width; }
uint32_t fb_get_height()   { return fb_height; }
uint32_t fb_get_addr()     { return (uint32_t)fb_addr; }
uint32_t fb_get_pitch()    { return fb_pitch; }
int      fb_is_vga_mode()  { return fb_vga_fallback; }
