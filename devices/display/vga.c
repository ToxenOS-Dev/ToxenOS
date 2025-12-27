#include "vga.h"

#define VGA_MEMORY ((volatile uint16_t*)0xB8000)
#define VGA_WIDTH  80
#define VGA_HEIGHT 25

static inline uint16_t vga_entry(char c, uint8_t color) {
    return (uint16_t)c | ((uint16_t)color << 8);
}

void vga_clear(void) {
    for (uint32_t i = 0; i < VGA_WIDTH * VGA_HEIGHT; i++) {
        VGA_MEMORY[i] = vga_entry(' ', 0x07); // light gray on black
    }
}

void vga_print_line(const char* str, uint32_t line) {
    uint32_t offset = line * VGA_WIDTH;

    for (uint32_t i = 0; str[i] && i < VGA_WIDTH; i++) {
        VGA_MEMORY[offset + i] = vga_entry(str[i], 0x0F); // white on black
    }
}
