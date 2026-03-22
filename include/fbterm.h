#ifndef FBTERM_H
#define FBTERM_H

#include <stdint.h>

// Font cell size (matches font.c)
#define FBTERM_CHAR_W   8
#define FBTERM_CHAR_H   16

// Number of virtual TTYs
#define FBTERM_TTY_COUNT  4

// Initialise the framebuffer terminal.
// Must be called after fb_init().
void fbterm_init(void);

// Write a single character to the active TTY at the current cursor,
// advancing the cursor and scrolling when necessary.
void fbterm_putchar(char c);

// Erase the character immediately left of the cursor (backspace visual).
void fbterm_erase(void);

// Clear the active TTY to the current background color.
void fbterm_clear(void);

// Set foreground+background using a VGA color attribute byte
// (same format the shell already uses: 0x07, 0x0C, 0x0A …).
// High nibble = background, low nibble = foreground.
void fbterm_set_color(uint8_t vga_attr);

// Switch active TTY (0-based). Saves old TTY pixels, restores new ones.
void fbterm_switch_tty(int tty);

// Return the currently active TTY index.
int fbterm_current_tty(void);

// How many columns / rows fit at the current resolution.
int fbterm_cols(void);
int fbterm_rows(void);

// Draw the small TTY indicator badge in the top-right corner.
void fbterm_draw_indicator(void);

// Per-process TTY assignment (mirrors the old tty_for_pid array).
extern int fbterm_pid_tty[];

#endif
