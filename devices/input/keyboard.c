#include "devices/input/keyboard.h"
#include "devices/display/vga.h"
#include <stdint.h>

/* Cursor position (shared with timer) */
int cursor_x = 5;   /* after "Tox:" */
int cursor_y = 3;

static int shift_pressed = 0;

/* I/O */
static inline uint8_t inb(uint16_t port) {
    uint8_t ret;
    __asm__ volatile ("inb %1, %0" : "=a"(ret) : "Nd"(port));
    return ret;
}

/* Keymaps */
static const char keymap[128] = {
    0,27,'1','2','3','4','5','6','7','8','9','0','-','=', '\b',
    '\t','q','w','e','r','t','y','u','i','o','p','[',']','\n',
    0,'a','s','d','f','g','h','j','k','l',';','\'','`',
    0,'\\','z','x','c','v','b','n','m',',','.','/',0,
    '*',0,' '
};

static const char keymap_shift[128] = {
    0,27,'!','@','#','$','%','^','&','*','(',')','_','+', '\b',
    '\t','Q','W','E','R','T','Y','U','I','O','P','{','}','\n',
    0,'A','S','D','F','G','H','J','K','L',':','"','~',
    0,'|','Z','X','C','V','B','N','M','<','>','?',0,
    '*',0,' '
};

void keyboard_handler(void) {
    uint8_t scancode = inb(0x60);

    /* Key release */
    if (scancode & 0x80) {
        if (scancode == 0xAA || scancode == 0xB6)
            shift_pressed = 0;
        return;
    }

    /* Shift press */
    if (scancode == 42 || scancode == 54) {
        shift_pressed = 1;
        return;
    }

    char c = shift_pressed ? keymap_shift[scancode]
                            : keymap[scancode];

    if (!c)
        return;

    /* ---- FILTER NON-PRINTABLE KEYS ---- */
    if (c < 32 || c > 126) {
        /* Allow backspace and enter only */
        if (c != '\b' && c != '\n')
            return;
    }

    /* BACKSPACE */
    if (c == '\b') {
        if (cursor_x > 5) {
            vga_cursor_hide(cursor_x, cursor_y);
            cursor_x--;
            vga_put_char(' ', cursor_x, cursor_y, VGA_WHITE);
        }
        return;
    }

    /* ENTER */
    if (c == '\n') {
        vga_cursor_hide(cursor_x, cursor_y);
        cursor_y++;
        cursor_x = 5;
        vga_print("Tox:", 0, cursor_y, VGA_BROWN);
        return;
    }

    /* NORMAL CHARACTER */
    vga_cursor_hide(cursor_x, cursor_y);

    vga_put_char(c, cursor_x, cursor_y, VGA_WHITE);
    cursor_x++;

    if (cursor_x >= VGA_WIDTH) {
        cursor_x = 5;
        cursor_y++;
        vga_print("Tox:", 0, cursor_y, VGA_BROWN);
    }
}

void keyboard_init(void) {
    /* Nothing yet */
}
