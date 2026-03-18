#include <stdint.h>
#include "../include/tty.h"
#include "../include/vga.h"
#include "../include/process.h"

#define TTY_COUNT   4
#define VGA_WIDTH   80
#define VGA_HEIGHT  25

// each TTY has its own screen buffer and cursor
static uint16_t tty_buffer[TTY_COUNT][VGA_WIDTH * VGA_HEIGHT];
static int      tty_cursor_x[TTY_COUNT];
static int      tty_cursor_y[TTY_COUNT];
static int      tty_color[TTY_COUNT];
static int tty_pid[TTY_COUNT] = {-1, -1, -1, -1};
int tty_for_pid[MAX_PROCESSES];

static int current_tty = 0;

// TTY indicator colors
static uint8_t tty_indicator_colors[TTY_COUNT] = {
    0x06,  // TTY1 - orange
    0x0B,  // TTY2 - cyan
    0x0A,  // TTY3 - green
    0x0E,  // TTY4 - yellow
};

extern uint16_t* const VGA_MEMORY;
extern int cursor_x;
extern int cursor_y;
extern uint8_t current_color;

static inline void outb(uint16_t port, uint8_t value)
{
    __asm__ volatile ("outb %0, %1" : : "a"(value), "Nd"(port));
}

void tty_update_cursor(int x, int y)
{
    uint16_t pos = y * VGA_WIDTH + x;
    outb(0x3D4, 0x0F);
    outb(0x3D5, (uint8_t)(pos & 0xFF));
    outb(0x3D4, 0x0E);
    outb(0x3D5, (uint8_t)((pos >> 8) & 0xFF));
}

void tty_draw_indicator()
{
    // draw [TTY1] in top right corner
    const char* labels[TTY_COUNT] = {"[TTY1]", "[TTY2]", "[TTY3]", "[TTY4]"};
    const char* label = labels[current_tty];
    uint8_t color = tty_indicator_colors[current_tty];

    int col = VGA_WIDTH - 6;  // 6 chars wide
    for (int i = 0; label[i]; i++)
        VGA_MEMORY[col + i] = label[i] | (color << 8);
}

void tty_assign_pid(int pid, int tty)
{
    tty_for_pid[pid] = tty;
}

int tty_of_pid(int pid)
{
    return tty_for_pid[pid];
}

void tty_set_pid(int tty, int pid)
{
    tty_pid[tty] = pid;
}

int tty_get_pid(int tty)
{
    return tty_pid[tty];
}

void tty_save_current()
{
    // save current VGA state to current TTY buffer
    for (int i = 0; i < VGA_WIDTH * VGA_HEIGHT; i++)
        tty_buffer[current_tty][i] = VGA_MEMORY[i];
    tty_cursor_x[current_tty] = cursor_x;
    tty_cursor_y[current_tty] = cursor_y;
    tty_color[current_tty]    = current_color;
}

void tty_restore(int tty)
{
    // restore TTY buffer to VGA
    for (int i = 0; i < VGA_WIDTH * VGA_HEIGHT; i++)
        VGA_MEMORY[i] = tty_buffer[tty][i];
    cursor_x      = tty_cursor_x[tty];
    cursor_y      = tty_cursor_y[tty];
    current_color = tty_color[tty];
    tty_draw_indicator();
    tty_update_cursor(cursor_x, cursor_y);
}

void tty_switch(int tty)
{
    if (tty == current_tty) return;
    if (tty < 0 || tty >= TTY_COUNT) return;

    tty_save_current();
    current_tty = tty;
    tty_restore(tty);
}

int tty_current()
{
    return current_tty;
}

void tty_init()
{
    for (int i = 0; i < MAX_PROCESSES; i++)
        tty_for_pid[i] = -1;  // unassigned
    
    // clear all TTY buffers
    for (int t = 0; t < TTY_COUNT; t++)
    {
        for (int i = 0; i < VGA_WIDTH * VGA_HEIGHT; i++)
            tty_buffer[t][i] = ' ' | (0x07 << 8);
        tty_cursor_x[t] = 0;
        tty_cursor_y[t] = 2;
        tty_color[t]    = 0x07;
    }
    
    tty_for_pid[0] = 0;
}