// ToxenOS/user64/wallpaper_demo64.c — Milestone 33: loads the real
// ToxenOS wallpaper PNG from the filesystem
// (/system_manager/system_data/display_interface/backgrounds/
// toxenos-default.png -- see the Makefile's populate target and
// assets/README.md) and displays it filling an ordinary graphical
// client window via tox_draw_image_fit's TOX_FIT_COVER mode.
//
// Deliberately just another ordinary compositor client, like every
// other Milestone 30+ demo -- the compositor itself is NOT responsible
// for decoding or knowing about the wallpaper (see this milestone's
// own "the future desktop shell should ultimately own wallpaper
// policy" note); this program exists purely to prove the PNG-load ->
// scale/cover -> composite pipeline works end to end with a real,
// large, filesystem-resident asset, not to BE the desktop shell's
// actual wallpaper renderer.
//
// The window size here is NOT the image's own dimensions -- proving
// tox_draw_image_fit's cover mode correctly scales/crops REGARDLESS of
// source or destination size, per this milestone's explicit
// requirement that 1920x1080 (or any other size) is never hardcoded
// into the image API.
#include <stdint.h>
#include "tox64.h"
#include "wmclient64.h"
#include "tox_surface.h"
#include "tox_draw.h"
#include "tox_image.h"

#define WIN_W 640
#define WIN_H 400
#define WALLPAPER_PATH "/system_manager/system_data/display_interface/backgrounds/toxenos-default.png"

static int my_strlen(const char* s) { int i = 0; while (s[i]) i++; return i; }
static void put(const char* s) { sys_write(s, (uint64_t)my_strlen(s)); }

void _start(void) {
    wm_client_t c;
    if (wm_connect(&c) < 0) { put("wallpaper_demo64: wm_connect failed\n"); sys_exit(1); }

    wm_window_t win_state;
    uint32_t win = wm_create_window(&c, WIN_W, WIN_H, "Wallpaper", &win_state);
    if (win == 0) { put("wallpaper_demo64: wm_create_window failed\n"); sys_exit(1); }

    uint32_t stride;
    uint32_t* pixels = wm_attach_surface(&c, win, WIN_W, WIN_H, &stride);
    if (!pixels) { put("wallpaper_demo64: wm_attach_surface failed\n"); sys_exit(1); }

    tox_surface_t surf;
    tox_surface_init(&surf, pixels, WIN_W, WIN_H, stride);

    tox_image_t wallpaper;
    if (tox_image_load(WALLPAPER_PATH, &wallpaper) < 0) {
        put("wallpaper_demo64: tox_image_load FAILED\n");
        tox_fill_rect(&surf, 0, 0, WIN_W, WIN_H, TOX_RGB(180, 20, 20)); // visibly distinct failure indicator
        wm_commit(&c, win, 0, 0, WIN_W, WIN_H);
    } else {
        tox_draw_image_fit(&surf, &wallpaper, 0, 0, WIN_W, WIN_H, TOX_FIT_COVER);
        wm_commit(&c, win, 0, 0, WIN_W, WIN_H);
        tox_image_free(&wallpaper);
    }

    for (;;) {
        wm_msg_t ev;
        if (wm_wait_event(&c, &ev) < 0) break;
        if (ev.type == WM_MSG_CLOSE_REQUEST) {
            wm_destroy_window(&c, &win_state);
            break;
        }
    }
    sys_exit(0);
}
