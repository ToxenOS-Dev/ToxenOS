#ifndef FBTERM_H
#define FBTERM_H

#include <stdint.h>

#define FBTERM_CHAR_W    8
#define FBTERM_CHAR_H   16
#define FBTERM_TTY_COUNT 1

void fbterm_init(void);
void fbterm_putchar(char c);
void fbterm_erase(void);
void fbterm_clear(void);
void fbterm_set_color(uint8_t vga_attr);
void fbterm_switch_tty(int tty);
void fbterm_tick(void);      // call from timer IRQ to blink cursor
int  fbterm_current_tty(void);
int  fbterm_cols(void);
int  fbterm_rows(void);
void fbterm_draw_indicator(void);

extern int fbterm_pid_tty[];

#endif
