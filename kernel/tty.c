// ToxenOS/kernel/tty.c
#include <stdint.h>
#include "../include/tty.h"
#include "../include/vga.h"
#include "../include/process.h"
#include "../include/fbterm.h"
#include "../include/keyboard.h"

#define TTY_COUNT  FBTERM_TTY_COUNT

// tty_pid: which process is the "foreground" owner of each TTY slot.
// Indexed by TTY number (0..TTY_COUNT-1), not by PID.
static int tty_pid[TTY_COUNT];

void tty_init(void)
{
    for (int i = 0; i < TTY_COUNT; i++)
        tty_pid[i] = -1;
    // kernel process (pid 0) owns TTY 0 at boot
    tty_pid[0] = 0;
    // Set tty on the kernel process struct directly
    processes[0].tty = 0;
}

// Assign a process to a TTY.  Stores the TTY in p->tty — the single source
// of truth.  No parallel arrays involved.
void tty_assign_pid(int pid, int tty)
{
    process_t* p = process_get_by_pid(pid);
    if (p) p->tty = tty;
}

int tty_of_pid(int pid)
{
    process_t* p = process_get_by_pid(pid);
    return p ? p->tty : -1;
}

void tty_set_pid(int tty, int pid) { if (tty >= 0 && tty < TTY_COUNT) tty_pid[tty] = pid; }
int  tty_get_pid(int tty)          { return (tty >= 0 && tty < TTY_COUNT) ? tty_pid[tty] : -1; }

void tty_save_current(void) {}
void tty_restore(int tty)   { (void)tty; }
void tty_draw_indicator(void) { fbterm_draw_indicator(); }
void tty_switch(int tty)      { fbterm_switch_tty(tty); }
void tty_update_cursor(int x, int y) { (void)x; (void)y; }

void tty_handle_switch(int tty)
{
    if (tty >= 0 && tty < TTY_COUNT) tty_switch(tty);
}

int tty_current(void) { return fbterm_current_tty(); }
