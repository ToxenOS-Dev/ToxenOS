#include "devices/display/vga.h"

static volatile uint16_t* const VGA_MEMORY = (uint16_t*)0xB8000;
static int cursor_visible = 1;

/* Low-level port output */
static inline void outb(uint16_t port, uint8_t value) {
    __asm__ volatile ("outb %0, %1" : : "a"(value), "Nd"(port));
}

/* Disable the hardware VGA cursor */
void vga_disable_hw_cursor(void) {
    outb(0x3D4, 0x0A);
    outb(0x3D5, 0x20);
}

static inline uint16_t vga_entry(char c, uint8_t color) {
    return (uint16_t)c | ((uint16_t)color << 8);
}

void vga_clear(void) {
    for (int y = 0; y < VGA_HEIGHT; y++) {
        for (int x = 0; x < VGA_WIDTH; x++) {
            VGA_MEMORY[y * VGA_WIDTH + x] =
                vga_entry(' ', VGA_LIGHT_GREY);
        }
    }
}

void vga_put_char(char c, int x, int y, uint8_t color) {
    if (x < 0 || x >= VGA_WIDTH || y < 0 || y >= VGA_HEIGHT)
        return;

    VGA_MEMORY[y * VGA_WIDTH + x] = vga_entry(c, color);
}

void vga_print(const char* str, int x, int y, uint8_t color) {
    for (int i = 0; str[i]; i++) {
        vga_put_char(str[i], x + i, y, color);
    }
}

void vga_blink_cursor(int x, int y) {
    static int counter = 0;
    counter++;

    if (counter > 500000) {
        counter = 0;
        cursor_visible = !cursor_visible;

        vga_put_char(
            cursor_visible ? '_' : ' ',
            x,
            y,
            VGA_WHITE
        );
    }
}
