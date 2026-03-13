#ifndef VGA_H
#define VGA_H

void print(const char* str);
void put_char(char c);
void clear_screen();
void print_hex(uint32_t val);
void erase_char();
void set_color(uint8_t color);

#endif