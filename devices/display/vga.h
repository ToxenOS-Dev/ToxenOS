#ifndef VGA_H
#define VGA_H

#include <stdint.h>

#define VGA_WIDTH  80
#define VGA_HEIGHT 25

enum vga_color {
    VGA_BLACK = 0,
    VGA_BLUE = 1,
    VGA_GREEN = 2,
    VGA_CYAN = 3,
    VGA_RED = 4,
    VGA_MAGENTA = 5,
    VGA_BROWN = 6,
    VGA_LIGHT_GREY = 7,
    VGA_DARK_GREY = 8,
    VGA_LIGHT_GREEN = 10,
    VGA_LIGHT_RED = 12,
    VGA_WHITE = 15,
};

void vga_clear(void);
void vga_put_char(char c, int x, int y, uint8_t color);
void vga_print(const char* str, int x, int y, uint8_t color);

void vga_disable_hw_cursor(void);

/* Cursor helpers */
void vga_cursor_hide(int x, int y);
void vga_cursor_show(int x, int y);

/* Called from timer IRQ */
void vga_blink_cursor(int x, int y);

#endif
