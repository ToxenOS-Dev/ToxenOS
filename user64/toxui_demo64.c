// ToxenOS/user64/toxui_demo64.c — Milestone 33: the dedicated ToxUI
// graphical demo. Connects to the compositor exactly like every other
// Milestone 30+ graphical client (via user64/wmclient64.h's
// wm_connect/wm_create_window/wm_attach_surface -- completely
// unmodified by this milestone), then draws everything through
// userlib/toxui instead of hand-rolled framebuffer code, into a
// tox_surface_t that directly WRAPS the same shared-memory surface
// wm_attach_surface() already mapped -- no copy, no format conversion
// (see userlib/toxui/tox_surface.h's header comment on why
// WM_FORMAT_XRGB8888 and TOX_SURFACE_XRGB8888 are the exact same
// layout).
//
// Demonstrates, in one window: a PNG loaded from the real ToxenOS VFS,
// transparent-PNG compositing over an opaque background, scaled PNG
// drawing, cropped (source-rectangle) PNG drawing, rounded rectangles,
// lines/a circle, anti-aliased TrueType text at multiple pixel sizes,
// text measurement/alignment (right-aligning a label using
// tox_measure_text), and a clipped region (a rectangle drawn far
// larger than its clip, visibly cut off).
#include <stdint.h>
#include "tox64.h"
#include "wmclient64.h"
#include "tox_surface.h"
#include "tox_draw.h"
#include "tox_image.h"
#include "tox_font.h"
#include "tox_text.h"

#define WIN_W 480
#define WIN_H 420
#define ICON_PATH "/toxui_test_assets/test_rgba_alpha.png"
#define FONT_PATH "/system_manager/system_data/display_interface/fonts/DejaVuSans.ttf"

static int my_strlen(const char* s) { int i = 0; while (s[i]) i++; return i; }
static void put(const char* s) { sys_write(s, (uint64_t)my_strlen(s)); }

void _start(void) {
    wm_client_t c;
    if (wm_connect(&c) < 0) { put("toxui_demo64: wm_connect failed\n"); sys_exit(1); }

    wm_window_t win_state;
    uint32_t win = wm_create_window(&c, WIN_W, WIN_H, "ToxUI Demo", &win_state);
    if (win == 0) { put("toxui_demo64: wm_create_window failed\n"); sys_exit(1); }

    uint32_t stride;
    uint32_t* pixels = wm_attach_surface(&c, win, WIN_W, WIN_H, &stride);
    if (!pixels) { put("toxui_demo64: wm_attach_surface failed\n"); sys_exit(1); }

    tox_surface_t surf;
    tox_surface_init(&surf, pixels, WIN_W, WIN_H, stride);

    tox_image_t icon;
    int have_icon = (tox_image_load(ICON_PATH, &icon) == 0);
    if (!have_icon) put("toxui_demo64: tox_image_load FAILED (icon)\n");

    tox_font_t font;
    int have_font = (tox_font_load(FONT_PATH, &font) == 0);
    if (!have_font) put("toxui_demo64: tox_font_load FAILED\n");

    // Background.
    tox_fill_rect(&surf, 0, 0, WIN_W, WIN_H, TOX_RGB(40, 44, 52));

    // Transparent-PNG compositing at native size, over the background --
    // the corners of test_rgba_alpha.png are fully transparent, so this
    // must show the background color around the circle, never a black box.
    if (have_icon) tox_draw_image_native(&surf, &icon, 16, 16);

    // The SAME image, scaled up 2x, proving bilinear scaling works
    // (and still alpha-composites correctly at a non-native size).
    if (have_icon) tox_draw_image(&surf, &icon, 96, 16, 128, 128, 0, 0, (int32_t)icon.width, (int32_t)icon.height);

    // A CROPPED sub-rectangle of the source image (just its top-left
    // quadrant), drawn at native crop size elsewhere in the window.
    if (have_icon) {
        int32_t half_w = (int32_t)icon.width / 2, half_h = (int32_t)icon.height / 2;
        tox_draw_image(&surf, &icon, 240, 16, half_w, half_h, 0, 0, half_w, half_h);
    }

    // Shapes: rounded rectangle, plain rectangle outline, a line, a circle.
    tox_fill_rounded_rect(&surf, 16, 160, 140, 60, 12, TOX_RGB(90, 140, 220));
    tox_draw_rect(&surf, 166, 160, 140, 60, TOX_RGB(220, 90, 90));
    tox_draw_line(&surf, 320, 160, 456, 220, TOX_RGB(240, 200, 60));
    tox_draw_circle(&surf, 388, 190, 25, TOX_RGB(60, 220, 140));

    // A clipped fill -- drawn far larger than the clip rectangle, must
    // visibly cut off at its bounds rather than covering the window.
    tox_surface_set_clip(&surf, 16, 240, 200, 40);
    tox_fill_rect(&surf, -50, 200, 600, 200, TOX_RGB(200, 80, 200));
    tox_surface_clip_reset(&surf);

    // Anti-aliased scalable text at several sizes -- explicitly NOT the
    // old 8x16 bitmap font (kernel/fbterm64.c's/compositor64.c's font
    // data is never referenced anywhere in this file).
    if (have_font) {
        tox_draw_text(&surf, &font, 14, 16, 310, "ToxUI 14px: The quick brown fox jumps.", TOX_RGB(230, 230, 230));
        tox_draw_text(&surf, &font, 22, 16, 340, "ToxUI 22px scalable text", TOX_RGB(255, 255, 255));
        tox_draw_text(&surf, &font, 32, 16, 380, "ToxUI 32px", TOX_RGB(255, 220, 120));

        // Right-aligned label via tox_measure_text -- proves real
        // proportional-width measurement, not a guess.
        tox_text_extent_t ext;
        tox_measure_text(&font, 14, "right-aligned", &ext);
        tox_draw_text(&surf, &font, 14, WIN_W - 16 - ext.width, 310, "right-aligned", TOX_RGB(150, 200, 255));
    }

    wm_commit(&c, win, 0, 0, WIN_W, WIN_H);

    for (;;) {
        wm_msg_t ev;
        if (wm_wait_event(&c, &ev) < 0) break; // compositor gone -- exit cleanly
        if (ev.type == WM_MSG_CLOSE_REQUEST) {
            wm_destroy_window(&c, &win_state);
            break;
        }
    }

    if (have_font) tox_font_free(&font);
    if (have_icon) tox_image_free(&icon);
    sys_exit(0);
}
