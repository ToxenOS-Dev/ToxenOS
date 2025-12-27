#include "devices/display/vga.h"

void kernel_main(void) {
    vga_clear();
    vga_print_line("Welcome To ToxenOS", 0);
    vga_print_line("v0.0.1", 1);

    while (1) {
        __asm__ volatile ("hlt");
    }
}
