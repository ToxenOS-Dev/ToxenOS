#ifndef FONT_H
#define FONT_H

#include <stdint.h>

void font_draw_char(uint32_t x, uint32_t y, char c, uint32_t color);
void font_draw_string(uint32_t x, uint32_t y, const char* str, uint32_t color);

#define FONT_WIDTH  8
#define FONT_HEIGHT 16

#endif