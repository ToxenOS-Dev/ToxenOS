#include <stdint.h>

#define VGA ((volatile uint16_t*)0xB8000)

void kernel_main(void) {
    // Clear screen
    for (int i = 0; i < 80 * 25; i++) {
        VGA[i] = (0x07 << 8) | ' ';
    }

    const char* line1 = "Welcome To ToxenOS";
    const char* line2 = "v0.0.1";

    for (int i = 0; line1[i]; i++) {
        VGA[i] = (0x0F << 8) | line1[i];
    }

    for (int i = 0; line2[i]; i++) {
        VGA[80 + i] = (0x0F << 8) | line2[i];
    }

    while (1) {
        __asm__ volatile ("hlt");
    }
}
