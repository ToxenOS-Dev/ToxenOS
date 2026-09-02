// ToxenOS/user64/gfx_demo64.c — Milestone 30: graphics demo client.
// Connects to the compositor via the generic client library
// (wmclient64.h), creates a window, and repeatedly renders a changing
// pattern into its shared-memory surface, committing after every
// frame. Never touches a pipe/shm syscall or a wm_msg_t directly.
#include <stdint.h>
#include "tox64.h"
#include "wmclient64.h"

#define WIN_W 200
#define WIN_H 150

static void render_frame(uint32_t* px, uint32_t phase) {
    for (uint32_t y = 0; y < WIN_H; y++) {
        for (uint32_t x = 0; x < WIN_W; x++) {
            uint32_t r = (x * 2 + phase) & 0xFF;
            uint32_t g = (y * 2 + phase / 2) & 0xFF;
            uint32_t b = ((x ^ y) + phase) & 0xFF;
            px[y * WIN_W + x] = (r << 16) | (g << 8) | b;
        }
    }
    // A moving marker square so distinct frames are visually obvious,
    // not just a static gradient.
    uint32_t mx = phase % (WIN_W - 20);
    uint32_t my = (phase / 3) % (WIN_H - 20);
    for (uint32_t y = my; y < my + 20; y++)
        for (uint32_t x = mx; x < mx + 20; x++)
            px[y * WIN_W + x] = 0x00FFFFFF;
}

void _start(void) {
    wm_client_t c;
    if (wm_connect(&c) < 0) sys_exit(1);

    uint32_t win = wm_create_window(&c, WIN_W, WIN_H, "GfxDemo");
    if (win == 0) sys_exit(1);

    uint32_t stride;
    uint32_t* surface = wm_attach_surface(&c, win, WIN_W, WIN_H, &stride);
    if (!surface) sys_exit(1);

    for (uint32_t phase = 0; ; phase = (phase + 3) & 0xFFFF) {
        render_frame(surface, phase);
        wm_commit(&c, win, 0, 0, WIN_W, WIN_H);
        // Deliberate busy-ish pacing loop -- ToxenOS has no sleep
        // syscall yet; a real animation rate limiter is future work.
        for (volatile int i = 0; i < 300000; i++) { }
    }
}
