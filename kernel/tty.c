// ToxenOS/kernel/tty.c
// Virtual TTY management — now delegates screen save/restore to fbterm.
#include <stdint.h>
#include "../include/tty.h"
#include "../include/vga.h"
#include "../include/process.h"
#include "../include/fbterm.h"
#include "../include/keyboard.h"

#define TTY_COUNT  FBTERM_TTY_COUNT

static int tty_pid[TTY_COUNT] = {-1, -1, -1, -1};
int        tty_for_pid[MAX_PROCESSES];

// These are still referenced by kernel.c for legacy compatibility.
extern uint16_t* const VGA_MEMORY;
extern int     cursor_x;
extern int     cursor_y;
extern uint8_t current_color;

static inline void outb(uint16_t port, uint8_t value)
{
    __asm__ volatile ("outb %0, %1" : : "a"(value), "Nd"(port));
}

void tty_assign_pid(int pid, int tty)
{
    tty_for_pid[pid] = tty;
    fbterm_pid_tty[pid] = tty;
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

// Save / restore are now no-ops at the VGA level — fbterm keeps per-TTY
// pixel backbuffers and handles this automatically on fbterm_switch_tty().
void tty_save_current(void) {}
void tty_restore(int tty)   { (void)tty; }

void tty_draw_indicator(void)
{
    fbterm_draw_indicator();
}

void tty_switch(int tty)
{
    fbterm_switch_tty(tty);
}

// Called from keyboard ISR: Ctrl+Alt+Fx → switch to TTY (x-1).
void tty_handle_switch(int tty)
{
    if (tty < 0 || tty >= TTY_COUNT) return;
    tty_switch(tty);
}

int tty_current(void)
{
    return fbterm_current_tty();
}

// Hardware cursor update — not needed for framebuffer terminal.
void tty_update_cursor(int x, int y) { (void)x; (void)y; }

void tty_init(void)
{
    for (int i = 0; i < MAX_PROCESSES; i++)
        tty_for_pid[i] = -1;

    tty_for_pid[0] = 0;
}
