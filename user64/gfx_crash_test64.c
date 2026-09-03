// ToxenOS/user64/gfx_crash_test64.c — Milestone 32: manual/interactive
// test fixture proving the compositor survives a connected client
// crashing. Connects via the generic client library exactly like any
// other graphical client, creates a window, commits one frame so it is
// visibly present, then deliberately faults (a real #GP via a write to
// address 0) -- kernel/process64.c's fault path treats this exactly
// like any other unhandled exception (process64_fault_current), which
// is what exercises the compositor's existing EOF-on-client-request-pipe
// disconnect cleanup (user64/compositor64.c's main loop) without any
// new mechanism of its own. Not part of the normal boot roster --
// spawn it interactively from the shell (`make populate
// PACKAGE_DEBUG64=1`) while a graphical session is running to observe
// its window disappear and the rest of the desktop keep running.
#include <stdint.h>
#include "tox64.h"
#include "wmclient64.h"

#define WIN_W 120
#define WIN_H 90

void _start(void) {
    wm_client_t c;
    if (wm_connect(&c) < 0) sys_exit(1);

    uint32_t win = wm_create_window(&c, WIN_W, WIN_H, "CrashMe");
    if (win == 0) sys_exit(1);

    uint32_t stride;
    uint32_t* surface = wm_attach_surface(&c, win, WIN_W, WIN_H, &stride);
    if (!surface) sys_exit(1);

    for (uint32_t i = 0; i < WIN_W * WIN_H; i++) surface[i] = 0x00FF0000; // solid red
    wm_commit(&c, win, 0, 0, WIN_W, WIN_H);

    // Deliberate fault -- never returns.
    volatile int* bad = (volatile int*)0;
    *bad = 1;

    for (;;) { } // unreachable
}
