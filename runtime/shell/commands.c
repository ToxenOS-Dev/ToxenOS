#include "runtime/shell/commands.h"
#include "devices/display/vga.h"
#include "devices/input/keyboard.h"

void cmd_help(void) {
    vga_print("Available commands:", 0, cursor_y++, VGA_LIGHT_GREY);
    vga_print(" help   - show this message", 0, cursor_y++, VGA_LIGHT_GREY);
    vga_print(" clear  - clear the screen", 0, cursor_y++, VGA_LIGHT_GREY);
}

void cmd_clear(void) {
    vga_clear();
    cursor_x = 0;
    cursor_y = 0;
}
