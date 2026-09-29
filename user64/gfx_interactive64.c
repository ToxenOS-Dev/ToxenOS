// ToxenOS/user64/gfx_interactive64.c — Milestone 30: interactive
// client. Connects via the generic client library, creates a window,
// and BLOCKS waiting for compositor events (wm_wait_event -- a real
// scheduler block on the event pipe, never polling), visibly reacting
// to keyboard and pointer input: the background tint reflects the
// currently-held modifier keys, pressing a key stamps a colored mark
// whose color depends on the translated ASCII character, and pointer
// button presses "paint" a small square at the click position.
//
// M+9A follow-up: fullscreen is a generic WM protocol feature -- F11
// toggles it here too, through the exact same wm_set_fullscreen() call
// GfxDemo uses. This also migrated the surface model from the legacy
// single-surface WM_MSG_ATTACH_SURFACE/WM_MSG_COMMIT to the same
// committed-buffer model GfxDemo already uses for its own fullscreen
// pair. Unlike GfxDemo, this client is a pure reactive painter with no
// animation and no frame-pacing need at all -- it never calls
// wm_wait_frame()/wm_request_frame(), just wm_commit_buffer() whenever
// real input actually changes something, gated on that one buffer's own
// owned_by_compositor flag.
//
// M+10A protocol redesign: buffer ownership and fullscreen/configure
// state are now authoritative state maintained by the dispatcher (see
// wmclient64.h's own top comment), not raw messages this file used to
// manually cross-reference by token/compare against a synchronous
// wm_set_fullscreen() reply. F11 is now fire-and-forget -- the resulting
// WM_MSG_CONFIGURE arrives through this file's own existing
// wm_wait_event() loop exactly like any other event (a new case in the
// same switch, below), which fits this client's already-reactive
// architecture more naturally than the old synchronous call ever did:
// a CONFIGURE this client didn't itself request (there is none today,
// but the protocol no longer assumes otherwise) would be handled
// identically.
#include <stdint.h>
#include "tox64.h"
#include "wmclient64.h"

#define WIN_W_DEFAULT 220
#define WIN_H_DEFAULT 160

// Same raw PS/2 Set-1 F11 scancode as gfx_demo64.c -- see that file's
// own header comment on why it has no named INPUT64_KEY_* constant.
#define KEY_F11 0x57

static uint32_t* g_surface;
static uint32_t g_win_w = WIN_W_DEFAULT, g_win_h = WIN_H_DEFAULT;
static uint32_t g_modifiers = 0;

static uint32_t bg_color(void) {
    uint32_t r = (g_modifiers & INPUT64_MOD_CTRL)  ? 0x60 : 0x20;
    uint32_t g = (g_modifiers & INPUT64_MOD_SHIFT) ? 0x60 : 0x20;
    uint32_t b = (g_modifiers & INPUT64_MOD_ALT)   ? 0x60 : 0x20;
    return (r << 16) | (g << 8) | b;
}

static void redraw_background(void) {
    uint32_t c = bg_color();
    for (uint32_t i = 0; i < g_win_w * g_win_h; i++) g_surface[i] = c;
}

// M+10A follow-up audit (point 3): centralizes the same defensive check
// gfx_demo64.c applies before every render -- `active`/g_win_w/g_win_h
// are always kept in sync by the CONFIGURE handler, but this is a cheap
// belt-and-suspenders guard against ever committing/rendering a mismatch
// rather than trusting that invariant by construction alone. Never
// fires in practice; if it ever does, the commit is simply skipped
// rather than writing or presenting beyond the buffer's own allocation.
static int interact_commit(wm_client_t* c, wm_buffer_t* active, wm_window_t* win) {
    if (!wm_buffer_fits(active, g_win_w, g_win_h)) {
        wmc_put("*** Interact INVARIANT VIOLATION: buffer size mismatch -- commit skipped ***\n");
        return -1;
    }
    return wm_commit_buffer(c, active, win, 0, 0, g_win_w, g_win_h);
}

static void paint_square(int32_t cx, int32_t cy, uint32_t color) {
    for (int32_t y = cy - 6; y <= cy + 6; y++) {
        if (y < 0 || (uint32_t)y >= g_win_h) continue;
        for (int32_t x = cx - 6; x <= cx + 6; x++) {
            if (x < 0 || (uint32_t)x >= g_win_w) continue;
            g_surface[(uint32_t)y * g_win_w + (uint32_t)x] = color;
        }
    }
}

void _start(void) {
    wm_client_t c;
    if (wm_connect(&c) < 0) sys_exit(1);

    wm_window_t win;
    uint32_t win_id = wm_create_window(&c, WIN_W_DEFAULT, WIN_H_DEFAULT, "Interact", &win);
    if (win_id == 0) sys_exit(1);

    // Single buffer per mode -- no double-buffering needed since this
    // client never animates, only redraws in direct response to a real
    // event.
    wm_buffer_t normal_buf;
    if (wm_attach_buffer(&c, win_id, WIN_W_DEFAULT, WIN_H_DEFAULT, &normal_buf) < 0) sys_exit(1);

    // The fullscreen buffer, attached lazily the first time a CONFIGURE
    // actually reports fullscreen -- see gfx_demo64.c's own identical
    // comment on why (sized to whatever it reports, never guessed).
    wm_buffer_t fs_buf;
    int fs_attached = 0;
    int fs_attach_failed = 0; // M+10A follow-up audit: a genuine attach failure is permanent -- see gfx_demo64.c's own identical comment
    int dirty = 0; // a redraw arrived while the active buffer was still owned by the compositor; flush on release

    wm_buffer_t* active = &normal_buf;
    g_surface = active->pixels;
    g_win_w = WIN_W_DEFAULT; g_win_h = WIN_H_DEFAULT;

    redraw_background();
    if (interact_commit(&c, active, &win) < 0) sys_exit(1);

    for (;;) {
        wm_msg_t ev;
        if (wm_wait_event(&c, &ev) < 0) sys_exit(1); // compositor gone -- exit cleanly
        if (ev.window_id != win_id) continue; // not for us (shouldn't happen, defensive)

        // M+10A: `active`'s own owned_by_compositor flag is already
        // correct at this point regardless of what `ev` turned out to
        // be -- wm_wait_event() applies state (via wmc_apply_message())
        // BEFORE handing the raw message back, so a BUFFER_RELEASED for
        // `active`'s own token has already updated this by the time we
        // ever see it below. No manual token comparison needed.
        int writable = !active->owned_by_compositor;
        int changed = 0;
        switch (ev.type) {
        case WM_MSG_BUFFER_RELEASED:
            if (dirty && !active->owned_by_compositor) {
                dirty = 0;
                redraw_background();
                interact_commit(&c, active, &win);
            }
            break;
        case WM_MSG_CONFIGURE: {
            // M+10A: fullscreen is now asynchronous -- this is where a
            // transition this client itself requested (or, in principle,
            // one it didn't) actually takes effect, exactly like any
            // other unsolicited state update. `win` itself was already
            // updated by wm_wait_event()'s own dispatch before `ev` was
            // handed back; this case just reacts to that update.
            if (win.is_fullscreen && !fs_attached && !fs_attach_failed) {
                if (wm_attach_buffer(&c, win_id, win.content_w, win.content_h, &fs_buf) == 0) {
                    fs_attached = 1;
                } else {
                    fs_attach_failed = 1; // permanent -- never retried, matches gfx_demo64.c's own fix
                }
                // If attach failed (e.g. WM_ERR_NO_RESOURCES), the
                // effective-fullscreen check below keeps this client on
                // the normal buffer -- same "never fake fullscreen
                // without a correctly-sized buffer" rule gfx_demo64.c's
                // own comment documents.
            }
            int effective_fullscreen = win.is_fullscreen && fs_attached;
            active = effective_fullscreen ? &fs_buf : &normal_buf;
            g_win_w = effective_fullscreen ? win.content_w : WIN_W_DEFAULT;
            g_win_h = effective_fullscreen ? win.content_h : WIN_H_DEFAULT;
            g_surface = active->pixels;
            writable = !active->owned_by_compositor; // re-check: `active` just changed
            if (writable) redraw_background(); else dirty = 1;
            changed = 1;
            break;
        }
        case WM_MSG_KEY_EVENT:
            if (ev.pressed && ev.key_code == KEY_F11) {
                wm_set_fullscreen(&c, &win, !win.is_fullscreen); // fire-and-forget -- see this file's own header comment
            } else {
                // Every key tap still delivers a paired press+release;
                // this switch already fully drains BOTH through
                // wm_wait_event() every iteration, so an unused
                // release/keycode falling through to no case above is
                // already safe -- it's still consumed here, just
                // ignored, never left queued anywhere.
                g_modifiers = ev.modifiers; // logical state, not a buffer write -- always safe
                if (writable) {
                    redraw_background();
                    if (ev.pressed && ev.ascii) {
                        uint32_t color = (ev.ascii * 2654435761u) & 0xFFFFFF; // cheap hash -> color
                        paint_square((int32_t)(g_win_w / 2), (int32_t)(g_win_h / 2), color);
                    }
                } else {
                    dirty = 1;
                }
                changed = 1;
            }
            break;
        case WM_MSG_POINTER_BUTTON:
            if (ev.pressed) {
                if (writable) {
                    uint32_t color = (ev.button == INPUT64_BTN_LEFT) ? 0x00FF6060 :
                                      (ev.button == INPUT64_BTN_RIGHT) ? 0x006060FF : 0x0060FF60;
                    paint_square(ev.x, ev.y, color);
                } else {
                    dirty = 1;
                }
                changed = 1;
            }
            break;
        case WM_MSG_FOCUS:
            if (writable) redraw_background(); else dirty = 1;
            changed = 1;
            break;
        case WM_MSG_CLOSE_REQUEST:
            wm_destroy_window(&c, &win);
            sys_exit(42);
            break;
        default:
            break;
        }

        if (changed && writable) {
            interact_commit(&c, active, &win);
        }
    }
}
