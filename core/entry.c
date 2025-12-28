#include "devices/display/vga.h"
#include "devices/input/keyboard.h"
#include "runtime/shell/shell.h"
#include "cpu/interrupts/idt.h"

void kernel_main(void) {
    vga_disable_hw_cursor();
    vga_clear();

    int title_x = (VGA_WIDTH - 18) / 2;
    vga_print("Welcome To ", title_x, 0, VGA_WHITE);
    vga_print("Toxen", title_x + 11, 0, VGA_BROWN);
    vga_print("OS",    title_x + 16, 0, VGA_WHITE);

    int version_x = (VGA_WIDTH - 6) / 2;
    vga_print("v0.0.1", version_x, 1, VGA_LIGHT_GREY);

    keyboard_init();
    shell_init();
    idt_init();

    while (1) {
        __asm__ volatile ("hlt");
    }
}


