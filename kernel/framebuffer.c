#include <stdint.h>
#include "../include/framebuffer.h"

static uint32_t* fb_addr;
static uint32_t  fb_width;
static uint32_t  fb_height;
static uint32_t  fb_pitch;
static uint32_t  fb_bpp;

void fb_init(uint32_t addr, uint32_t w, uint32_t h, uint32_t pitch, uint32_t bpp)
{
    fb_addr   = (uint32_t*)addr;
    fb_width  = w;
    fb_height = h;
    fb_pitch  = pitch;
    fb_bpp    = bpp;

    // clear to black
    fb_clear(0x000000);
}

void fb_put_pixel(uint32_t x, uint32_t y, uint32_t color)
{
    if (x >= fb_width || y >= fb_height) return;
    uint32_t offset = y * (fb_pitch / 4) + x;
    fb_addr[offset] = color;
}

void fb_clear(uint32_t color)
{
    for (uint32_t y = 0; y < fb_height; y++)
        for (uint32_t x = 0; x < fb_width; x++)
            fb_put_pixel(x, y, color);
}

void fb_draw_rect(uint32_t x, uint32_t y, uint32_t w, uint32_t h, uint32_t color)
{
    for (uint32_t dy = 0; dy < h; dy++)
        for (uint32_t dx = 0; dx < w; dx++)
            fb_put_pixel(x + dx, y + dy, color);
}

uint32_t fb_get_width()  { return fb_width; }
uint32_t fb_get_height() { return fb_height; }
uint32_t fb_get_addr() { return (uint32_t)fb_addr; }
uint32_t fb_get_pitch()  { return fb_pitch; }
