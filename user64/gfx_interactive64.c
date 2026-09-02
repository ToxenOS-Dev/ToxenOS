// ToxenOS/user64/gfx_interactive64.c — Milestone 30: interactive
// client. Connects via the generic client library, creates a window,
// and BLOCKS waiting for compositor events (wm_wait_event -- a real
// scheduler block on the event pipe, never polling), visibly reacting
// to keyboard and pointer input: the background tint reflects the
// currently-held modifier keys, pressing a key stamps a colored mark
// whose color depends on the translated ASCII character, and pointer
// button presses "paint" a small square at the click position.
#include <stdint.h>
#include "tox64.h"
#include "wmclient64.h"

#define WIN_W 220
#define WIN_H 160

static uint32_t* g_surface;
static uint32_t g_modifiers = 0;

static uint32_t bg_color(void) {
    uint32_t r = (g_modifiers & INPUT64_MOD_CTRL)  ? 0x60 : 0x20;
    uint32_t g = (g_modifiers & INPUT64_MOD_SHIFT) ? 0x60 : 0x20;
    uint32_t b = (g_modifiers & INPUT64_MOD_ALT)   ? 0x60 : 0x20;
    return (r << 16) | (g << 8) | b;
}

static void redraw_background(void) {
    uint32_t c = bg_color();
    for (uint32_t i = 0; i < WIN_W * WIN_H; i++) g_surface[i] = c;
}

static void paint_square(int32_t cx, int32_t cy, uint32_t color) {
    for (int32_t y = cy - 6; y <= cy + 6; y++) {
        if (y < 0 || y >= WIN_H) continue;
        for (int32_t x = cx - 6; x <= cx + 6; x++) {
            if (x < 0 || x >= WIN_W) continue;
            g_surface[(uint32_t)y * WIN_W + (uint32_t)x] = color;
        }
    }
}

void _start(void) {
    wm_client_t c;
    if (wm_connect(&c) < 0) sys_exit(1);

    uint32_t win = wm_create_window(&c, WIN_W, WIN_H, "Interact");
    if (win == 0) sys_exit(1);

    uint32_t stride;
    g_surface = wm_attach_surface(&c, win, WIN_W, WIN_H, &stride);
    if (!g_surface) sys_exit(1);

    redraw_background();
    wm_commit(&c, win, 0, 0, WIN_W, WIN_H);

    for (;;) {
        wm_msg_t ev;
        if (wm_wait_event(&c, &ev) < 0) sys_exit(1); // compositor gone -- exit cleanly
        if (ev.window_id != win) continue; // not for us (shouldn't happen, defensive)

        int changed = 0;
        switch (ev.type) {
        case WM_MSG_KEY_EVENT:
            g_modifiers = ev.modifiers;
            redraw_background();
            if (ev.pressed && ev.ascii) {
                uint32_t color = (ev.ascii * 2654435761u) & 0xFFFFFF; // cheap hash -> color
                paint_square(WIN_W / 2, WIN_H / 2, color);
            }
            changed = 1;
            break;
        case WM_MSG_POINTER_BUTTON:
            if (ev.pressed) {
                uint32_t color = (ev.button == INPUT64_BTN_LEFT) ? 0x00FF6060 :
                                  (ev.button == INPUT64_BTN_RIGHT) ? 0x006060FF : 0x0060FF60;
                paint_square(ev.x, ev.y, color);
                changed = 1;
            }
            break;
        case WM_MSG_FOCUS:
            redraw_background();
            changed = 1;
            break;
        case WM_MSG_CLOSE_REQUEST:
            wm_destroy_window(&c, win);
            sys_exit(42);
            break;
        default:
            break;
        }

        if (changed) wm_commit(&c, win, 0, 0, WIN_W, WIN_H);
    }
}
