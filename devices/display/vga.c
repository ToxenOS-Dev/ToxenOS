#include "devices/display/vga.h"

static volatile uint16_t* const VGA_MEMORY = (uint16_t*)0xB8000;

/* Timer-driven blink state */
static uint32_t timer_ticks = 0;
static int cursor_visible = 0;

/* Port I/O */
static inline void outb(uint16_t port, uint8_t value) {
    __asm__ volatile ("outb %0, %1" : : "a"(value), "Nd"(port));
}

static inline uint16_t vga_entry(char c, uint8_t color) {
    return (uint16_t)c | ((uint16_t)color << 8);
}

void vga_disable_hw_cursor(void) {
    outb(0x3D4, 0x0A);
    outb(0x3D5, 0x20);
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

/* Cursor helpers */
void vga_cursor_hide(int x, int y) {
    vga_put_char(' ', x, y, VGA_WHITE);
}

void vga_cursor_show(int x, int y) {
    vga_put_char('_', x, y, VGA_WHITE);
}

/* Timer-based blink (IRQ0) */
void vga_blink_cursor(int x, int y) {
    timer_ticks++;

    /* Blink rate: every 10 ticks */
    if (timer_ticks % 10 == 0) {
        cursor_visible = !cursor_visible;

        if (cursor_visible)
            vga_cursor_show(x, y);
        else
            vga_cursor_hide(x, y);
    }
}
