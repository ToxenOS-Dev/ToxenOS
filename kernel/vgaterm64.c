// kernel/vgaterm64.c — Milestone 13: scrolling VGA text-mode console.
// See include/vgaterm64.h for the rationale (making userland sys_write
// output visible in the QEMU window, not just serial). Deliberately
// separate from kernel64.c's boot-diagnostic vga_puts() -- that one is
// a simple non-scrolling one-shot used only for the boot banner; this
// one is the real, persistent, scrolling console that takes over once
// the normal boot path clears the screen and hands off to userland.
#include <stdint.h>
#include "../include/vgaterm64.h"

static uint16_t* const VGA = (uint16_t*)0xB8000;
#define VGA_COLS 80
#define VGA_ROWS 25

// Milestone 15: light gray on black is the new baseline (matches
// 32-bit ToxenOS's actual default), replacing the original hardcoded
// white. cur_attr is the "current color" set via vgaterm64_set_color()
// (mirrors 32-bit's stateful set_color()) -- blank_attr is deliberately
// a separate, fixed constant so a cleared screen always comes back
// neutral regardless of whatever color happened to be active when
// vgaterm64_clear() was called.
#define VGA_DEFAULT_ATTR 0x07
static uint8_t cur_attr = VGA_DEFAULT_ATTR;
static const uint8_t blank_attr = VGA_DEFAULT_ATTR;

static int cur_row = 0;
static int cur_col = 0;

static inline void outb(uint16_t port, uint8_t val) {
    __asm__ volatile ("outb %0, %1" :: "a"(val), "Nd"(port));
}

// Moves the real blinking hardware cursor to (cur_row, cur_col) -- pure
// cosmetic feedback so the user can see where they're typing.
static void update_hw_cursor(void) {
    uint16_t pos = (uint16_t)(cur_row * VGA_COLS + cur_col);
    outb(0x3D4, 0x0F);
    outb(0x3D5, (uint8_t)(pos & 0xFF));
    outb(0x3D4, 0x0E);
    outb(0x3D5, (uint8_t)((pos >> 8) & 0xFF));
}

static void blank_row(int row) {
    for (int c = 0; c < VGA_COLS; c++) {
        VGA[row * VGA_COLS + c] = ((uint16_t)blank_attr << 8) | ' ';
    }
}

static void scroll_if_needed(void) {
    if (cur_row < VGA_ROWS) return;
    for (int r = 1; r < VGA_ROWS; r++) {
        for (int c = 0; c < VGA_COLS; c++) {
            VGA[(r - 1) * VGA_COLS + c] = VGA[r * VGA_COLS + c];
        }
    }
    blank_row(VGA_ROWS - 1);
    cur_row = VGA_ROWS - 1;
}

static void put_char(char c) {
    if (c == '\n') {
        cur_col = 0;
        cur_row++;
        scroll_if_needed();
        return;
    }
    if (c == '\r') {
        cur_col = 0;
        return;
    }
    if (c == '\b') {
        if (cur_col > 0) {
            cur_col--;
            VGA[cur_row * VGA_COLS + cur_col] = ((uint16_t)blank_attr << 8) | ' ';
        }
        return;
    }
    if (c == '\t') {
        cur_col = (cur_col + 8) & ~7;
        if (cur_col >= VGA_COLS) { cur_col = 0; cur_row++; scroll_if_needed(); }
        return;
    }

    VGA[cur_row * VGA_COLS + cur_col] = ((uint16_t)cur_attr << 8) | (uint8_t)c;
    cur_col++;
    if (cur_col >= VGA_COLS) {
        cur_col = 0;
        cur_row++;
        scroll_if_needed();
    }
}

void vgaterm64_write(const char* buf, uint64_t len) {
    for (uint64_t i = 0; i < len; i++) put_char(buf[i]);
    update_hw_cursor();
}

void vgaterm64_clear(void) {
    for (int r = 0; r < VGA_ROWS; r++) blank_row(r);
    cur_row = 0;
    cur_col = 0;
    update_hw_cursor();
}

void vgaterm64_set_color(uint8_t attr) {
    cur_attr = attr;
}
