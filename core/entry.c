#include "devices/display/vga.h"

void kernel_main(void) {
    vga_disable_hw_cursor();
    vga_clear();

    /* Centered title */
    int title_len = 18; // "Welcome To ToxenOS"
    int title_x = (VGA_WIDTH - title_len) / 2;

    vga_print("Welcome To ", title_x, 0, VGA_WHITE);
    vga_print("Toxen", title_x + 11, 0, VGA_BROWN); // orange/brown
    vga_print("OS",    title_x + 16, 0, VGA_WHITE);

    /* Centered version */
    int version_x = (VGA_WIDTH - 6) / 2;
    vga_print("v0.0.1", version_x, 1, VGA_LIGHT_GREY);

    /* Prompt */
    vga_print("Tox:", 0, 3, VGA_BROWN);

    /* Blinking cursor loop */
    while (1) {
        vga_blink_cursor(5, 3);
    }
}
