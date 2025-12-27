#ifndef VGA_H
#define VGA_H

#include <stdint.h>

void vga_clear(void);
void vga_print_line(const char* str, uint32_t line);

#endif
