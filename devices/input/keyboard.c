#include "devices/input/keyboard.h"
#include "devices/display/vga.h"
#include "runtime/shell/shell.h"
#include <stdint.h>

int cursor_x = 5;
int cursor_y = 3;

static int shift_pressed = 0;

/* I/O */
static inline uint8_t inb(uint16_t port) {
    uint8_t ret;
    __asm__ volatile ("inb %1, %0" : "=a"(ret) : "Nd"(port));
    return ret;
}

/* Keymaps (no arrows here) */
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
    uint8_t sc = inb(0x60);

    /* key release */
    if (sc & 0x80) {
        if (sc == 0xAA || sc == 0xB6)
            shift_pressed = 0;
        return;
    }

    /* arrows */
    if (sc == 0x48) { shell_input_char('\x01'); return; } /* UP */
    if (sc == 0x50) { shell_input_char('\x02'); return; } /* DOWN */

    /* shift */
    if (sc == 42 || sc == 54) {
        shift_pressed = 1;
        return;
    }

    char c = shift_pressed ? keymap_shift[sc] : keymap[sc];
    if (!c) return;

    /* filter */
    if (c < 32 || c > 126) {
        if (c != '\b' && c != '\n') return;
    }

    /* BACKSPACE */
    if (c == '\b') {
        if (cursor_x > 5) {
            vga_cursor_hide(cursor_x, cursor_y);
            cursor_x--;
            vga_put_char(' ', cursor_x, cursor_y, VGA_WHITE);
            shell_input_char('\b');
        }
        return;
    }

    /* ENTER */
    if (c == '\n') {
        vga_cursor_hide(cursor_x, cursor_y);
        shell_input_char('\n');
        return;
    }

    /* NORMAL */
    vga_cursor_hide(cursor_x, cursor_y);
    vga_put_char(c, cursor_x++, cursor_y, VGA_WHITE);
    shell_input_char(c);
}

void keyboard_init(void) {}
