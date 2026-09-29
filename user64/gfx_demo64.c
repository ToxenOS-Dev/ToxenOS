// ToxenOS/user64/gfx_demo64.c — Milestone 30: graphics demo client.
// Connects to the compositor via the generic client library
// (wmclient64.h), creates a window, and repeatedly renders a changing
// pattern. Never touches a pipe/shm syscall or a wm_msg_t directly.
//
// M+5: the first client migrated to committed, double-buffered surfaces
// (Revision 2 §22 items 1-2) -- chosen specifically because it's
// animated (continuously producing new frames is the strongest test of
// buffer ownership). Two buffers, strict alternation, wait for a
// buffer's own release before writing into it again -- see
// wmclient64.h's own wm_attach_buffer()/wm_commit_buffer() for what
// this wraps.
//
// M+6: pacing is compositor-driven (Revision 3 §22 item 5) -- every
// frame after the first waits for BOTH a frame-callback grant and a
// free buffer.
//
// M+9A: F11 toggles real compositor fullscreen (wmproto64.h's
// WM_MSG_SET_FULLSCREEN/WM_MSG_CONFIGURE). A separate, lazily-attached
// buffer pair is used for fullscreen content, sized to whatever
// WM_MSG_CONFIGURE actually reports -- never guessed.
//
// M+10A protocol redesign: the main loop is now CONDITION-DRIVEN, not a
// sequence of blocking "wait for message X" calls -- see wmclient64.h's
// own top comment for the full architecture rationale (studied against
// upstream Wayland/Weston: a client dispatches whatever is available
// and checks its own prerequisites, rather than reading the wire
// looking for one specific type while queueing everything else).
// Concretely: fullscreen is requested fire-and-forget
// (wm_set_fullscreen() no longer blocks for CONFIGURE), and this file's
// own mode/buffer selection is re-derived from win.is_fullscreen fresh
// on every dispatch iteration -- never computed once per outer loop and
// then trusted through an entire wait, since win.is_fullscreen can
// legitimately change out from under a stale local copy the instant a
// CONFIGURE is dispatched while waiting on something else.
#include <stdint.h>
#include "tox64.h"
#include "wmclient64.h"

#define WIN_W 200
#define WIN_H 150

// Plain Set-1 PS/2 scancode for F11 (0x57) -- not among
// include/input64.h's small set of NAMED logical keycodes
// (arrows/ctrl/etc.), but still a valid, usable raw code per that
// header's own documented contract. Unextended: F11/F12 use codes
// 0x57/0x58 directly on real PS/2 Set-1 hardware.
#define KEY_F11 0x57

// M+10A liveness investigation -- LOW-OVERHEAD, kept from the earlier
// diagnostic pass (see that commit's own rationale: a per-frame full
// dump measurably perturbed the system under physical test). All state
// below is updated SILENTLY every iteration; a full dump only ever
// happens on the stall watchdog firing or an explicit invariant
// violation -- never per frame.
static uint64_t g_gd_iter = 0;
static int g_gd_last_committed_slot = -1;   // -1 none yet; 0/1 = bufs[0/1]; 2/3 = fs_bufs[0/1]
static uint64_t g_gd_last_progress_tick = 0; // set once per loop iteration -- the stall watchdog's own baseline
static uint64_t g_gd_last_heartbeat_tick = 0;
static int g_gd_watchdog_fired = 0;
static int g_gd_invariant_fired = 0;
static int g_gd_buffer_mismatch_reported = 0;

#define GD_WATCHDOG_TICKS 200   // ~2s at the kernel's fixed 100Hz tick rate (kernel/timer64.c)
#define GD_HEARTBEAT_TICKS 100  // ~1s

static void render_frame(uint32_t* px, uint32_t w, uint32_t h, uint32_t phase) {
    for (uint32_t y = 0; y < h; y++) {
        for (uint32_t x = 0; x < w; x++) {
            uint32_t r = (x * 2 + phase) & 0xFF;
            uint32_t g = (y * 2 + phase / 2) & 0xFF;
            uint32_t b = ((x ^ y) + phase) & 0xFF;
            px[y * w + x] = (r << 16) | (g << 8) | b;
        }
    }
    // A moving marker square so distinct frames are visually obvious.
    uint32_t mw = w > 20 ? 20 : w, mh = h > 20 ? 20 : h;
    uint32_t mx = phase % (w > mw ? w - mw : 1);
    uint32_t my = (phase / 3) % (h > mh ? h - mh : 1);
    for (uint32_t y = my; y < my + mh; y++)
        for (uint32_t x = mx; x < mx + mw; x++)
            px[y * w + x] = 0x00FFFFFF;
}

static void dump_gfxdemo_state(wm_client_t* c, wm_window_t* win, int fs_attached,
                                wm_buffer_t* bufs, wm_buffer_t* fs_bufs, const char* tag) {
    wmc_put("GfxDemo is alive and currently waiting for ");
    wmc_put(wm_wait_reason_name(wm_get_wait_reason()));
    wmc_put(" ["); wmc_put(tag); wmc_put("]\n");
    wmc_put("  iter="); wmc_put_u64(g_gd_iter);
    wmc_put(" last_committed_slot=");
    if (g_gd_last_committed_slot < 0) wmc_put("none");
    else wmc_put_u64((uint64_t)g_gd_last_committed_slot);
    wmc_put("\n");
    wmc_put("  win.is_fullscreen="); wmc_put_u64((uint64_t)win->is_fullscreen);
    wmc_put(" win.configure_generation="); wmc_put_u64((uint64_t)win->configure_generation);
    wmc_put(" win.content_w="); wmc_put_u64((uint64_t)win->content_w);
    wmc_put(" win.content_h="); wmc_put_u64((uint64_t)win->content_h);
    wmc_put(" win.frame_permission="); wmc_put_u64((uint64_t)win->frame_permission);
    wmc_put(" win.frame_requested="); wmc_put_u64((uint64_t)win->frame_requested);
    wmc_put("\n");
    wmc_put("  slot0(bufs[0]) writable="); wmc_put_u64((uint64_t)!bufs[0].owned_by_compositor);
    wmc_put(" slot1(bufs[1]) writable="); wmc_put_u64((uint64_t)!bufs[1].owned_by_compositor);
    if (fs_attached) {
        wmc_put(" slot2(fs_bufs[0]) writable="); wmc_put_u64((uint64_t)!fs_bufs[0].owned_by_compositor);
        wmc_put(" slot3(fs_bufs[1]) writable="); wmc_put_u64((uint64_t)!fs_bufs[1].owned_by_compositor);
    } else {
        wmc_put(" slot2/slot3=unattached");
    }
    wmc_put("\n");
    wmc_dump_state(c, tag);
}

// M+10A: liveness bookkeeping shared by BOTH loop levels -- a stall
// happening INSIDE the inner frame/buffer wait loop must be caught
// there directly, not just at the top of the outer loop, since a real
// hang means the outer loop's own top is never reached again at all.
// Cheap: a few integer comparisons and, at most, one short line per
// second or per genuine stall/invariant violation -- no per-iteration
// I/O either way.
static void gd_liveness_tick(wm_client_t* c, wm_window_t* win, int fs_attached,
                              wm_buffer_t* bufs, wm_buffer_t* fs_bufs, const char* tag) {
    uint64_t now = sys_get_ticks();
    if (g_gd_last_progress_tick != 0) {
        uint64_t gap = now - g_gd_last_progress_tick;
        if (gap >= GD_WATCHDOG_TICKS) {
            if (!g_gd_watchdog_fired) {
                g_gd_watchdog_fired = 1;
                wmc_put("*** GfxDemo STALL WATCHDOG: loop has not advanced in >=2s ***\n");
                dump_gfxdemo_state(c, win, fs_attached, bufs, fs_bufs, tag);
            }
        } else {
            g_gd_watchdog_fired = 0;
        }
    }
    g_gd_last_progress_tick = now;

    if (now - g_gd_last_heartbeat_tick >= GD_HEARTBEAT_TICKS) {
        g_gd_last_heartbeat_tick = now;
        wmc_put("GfxDemo heartbeat: iter="); wmc_put_u64(g_gd_iter);
        wmc_put(" wait="); wmc_put(wm_wait_reason_name(wm_get_wait_reason()));
        wmc_put(" mode="); wmc_put(win->is_fullscreen ? "fullscreen" : "normal");
        wmc_put("\n");
    }

    if (!g_gd_invariant_fired && g_wmc_key_lost > 0) {
        g_gd_invariant_fired = 1;
        wmc_put("*** GfxDemo INVARIANT VIOLATION: key_ring lost a KEY_EVENT ***\n");
        dump_gfxdemo_state(c, win, fs_attached, bufs, fs_bufs, tag);
    }
}

void _start(void) {
    wm_client_t c;
    if (wm_connect(&c) < 0) sys_exit(1);

    wm_window_t win;
    uint32_t win_id = wm_create_window(&c, WIN_W, WIN_H, "GfxDemo", &win);
    if (win_id == 0) sys_exit(1);

    // M+5: two attached buffers, never one live surface. Both start
    // owned_by_compositor = 0 (safe to draw into immediately) -- neither
    // has ever been committed yet.
    wm_buffer_t bufs[2];
    if (wm_attach_buffer(&c, win_id, WIN_W, WIN_H, &bufs[0]) < 0) sys_exit(1);
    if (wm_attach_buffer(&c, win_id, WIN_W, WIN_H, &bufs[1]) < 0) sys_exit(1);

    // M+9A: the fullscreen pair, attached lazily the first time a
    // CONFIGURE actually reports fullscreen (sized to whatever it
    // reports, never guessed).
    //
    // M+10A stress-testing finding: the attach attempt below sits inside
    // the inner dispatch loop and is re-evaluated on EVERY iteration
    // until fs_attached becomes true -- under rapid F11 stress this
    // could retry MANY times per second. If the first of the two
    // wm_attach_buffer() calls kept succeeding while the second kept
    // failing (each retry creating a fresh, never-released shm object
    // for fs_bufs[0] alone), a real run produced fs_bufs[0]/fs_bufs[1]
    // with the SAME token and neither ever properly attached -- the
    // client then permanently waited on a buffer release that could
    // never arrive for a slot the compositor never actually held this
    // way. Fixed by attempting the attach EXACTLY ONCE: a genuine
    // failure (e.g. WM_ERR_NO_RESOURCES) now permanently falls back to
    // the normal buffer pair for the rest of the session rather than
    // hammering the compositor with repeated ATTACH_BUFFER requests
    // inside a hot loop -- matches this file's own "never fake
    // fullscreen without a correctly-sized buffer" rule, just extended
    // to "never keep re-trying forever" too.
    wm_buffer_t fs_bufs[2];
    for (uint64_t i = 0; i < sizeof(fs_bufs); i++) ((char*)fs_bufs)[i] = 0;
    int fs_attached = 0;
    int fs_attach_failed = 0;
    int cur_normal = 0, cur_fs = 0;

    // M+6 startup lifecycle -- deliberately does NOT wait for a frame
    // callback before this first frame: neither buffer has ever been
    // committed and no frame has ever been requested yet, so there is
    // nothing to wait for and nothing that could ever grant one.
    uint32_t phase = 0;
    render_frame(bufs[cur_normal].pixels, WIN_W, WIN_H, phase);
    if (wm_commit_buffer(&c, &bufs[cur_normal], &win, 0, 0, WIN_W, WIN_H) < 0) sys_exit(1);
    g_gd_last_committed_slot = cur_normal;
    if (wm_request_frame(&c, &win) < 0) sys_exit(1);
    cur_normal ^= 1;

    for (;;) {
        g_gd_iter++;
        gd_liveness_tick(&c, &win, fs_attached, bufs, fs_bufs, "outer_loop");

        phase = (phase + 3) & 0xFFFF;

        // M+9A: F11 -- fire-and-forget now (item 5 of the M+10A
        // redesign). The eventual CONFIGURE is applied to `win` by the
        // ordinary dispatch path (inside wm_wait_frame()'s own inner
        // loop, below), with no special-casing needed here at all.
        uint32_t keycode;
        int got_key = wm_poll_key(&c, &keycode);
        if (got_key && keycode == KEY_F11) {
            wm_set_fullscreen(&c, &win, !win.is_fullscreen);
        }

        // M+10A: mode/buffer selection is re-derived on EVERY dispatch
        // iteration below, never computed once and trusted through a
        // whole wait -- win.is_fullscreen can legitimately change out
        // from under a stale local copy the instant a CONFIGURE is
        // dispatched while this loop is blocked on something else, and
        // rendering into a buffer chosen under the OLD mode after that
        // would be wrong. This inlines wm_wait_frame()'s own condition
        // (frame permission AND buffer ownership) rather than calling it
        // with a single fixed buffer, specifically so the buffer itself
        // can be re-picked each time too.
        int* cur = 0;
        wm_buffer_t* b = 0;
        int effective_fullscreen = 0;
        for (;;) {
            gd_liveness_tick(&c, &win, fs_attached, bufs, fs_bufs, "inner_wait_loop");
            // If `win.is_fullscreen` is true but the fullscreen pair
            // isn't attached yet (either never requested, or the
            // CONFIGURE confirming it hasn't arrived), fall back to the
            // normal pair -- never fake fullscreen without a
            // correctly-sized buffer.
            if (win.is_fullscreen && !fs_attached && !fs_attach_failed && win.configure_known) {
                if (wm_attach_buffer(&c, win_id, win.content_w, win.content_h, &fs_bufs[0]) == 0 &&
                    wm_attach_buffer(&c, win_id, win.content_w, win.content_h, &fs_bufs[1]) == 0) {
                    fs_attached = 1;
                } else {
                    fs_attach_failed = 1; // permanent -- see this file's own header comment on why this must never retry in a hot loop
                }
            }
            effective_fullscreen = win.is_fullscreen && fs_attached;
            cur = effective_fullscreen ? &cur_fs : &cur_normal;
            wm_buffer_t* pair = effective_fullscreen ? fs_bufs : bufs;

            // M+10A follow-up audit: a commit can legitimately be
            // REJECTED by the compositor now that COMMIT_BUFFER's own
            // generation is really enforced (compositor64.c) -- a
            // rejected commit's buffer bounces straight back to free
            // WITHOUT ever becoming the new current content. Blind
            // alternation (*cur ^= 1 after every attempted commit,
            // regardless of acceptance) would then desync from reality:
            // this client could end up "expecting" a buffer that never
            // actually stopped being the real current content, and
            // deadlock waiting for a release that can only ever come
            // from committing the OTHER buffer -- exactly the freeze a
            // live stress run exposed. Self-correct here, every
            // iteration: if the expected buffer isn't free but its
            // partner IS, use the partner and resync the index to match
            // -- this makes buffer selection robust to an occasional
            // rejected commit instead of trusting alternation blindly.
            if (pair[*cur].owned_by_compositor && !pair[*cur ^ 1].owned_by_compositor) {
                *cur ^= 1;
            }
            b = &pair[*cur];

            if (win.frame_permission && !b->owned_by_compositor) break;
            g_wmc_wait_reason = (!win.frame_permission && b->owned_by_compositor) ? WM_WAIT_FRAME_AND_RELEASE
                              : (!win.frame_permission ? WM_WAIT_FRAME : WM_WAIT_BUFFER_RELEASE);
            if (wmc_dispatch_one(&c, 0) < 0) sys_exit(1);
        }
        win.frame_permission = 0;
        g_wmc_wait_reason = WM_WAIT_NONE;
        uint32_t aw = effective_fullscreen ? win.content_w : WIN_W;
        uint32_t ah = effective_fullscreen ? win.content_h : WIN_H;

        // M+10A follow-up audit (point 3): defensive, should never fire
        // given aw/ah and `b` are always derived together from the same
        // effective_fullscreen snapshot above -- but never trust that by
        // construction alone when the alternative is writing past a
        // buffer's own allocation. If this ever fires, skip the render
        // entirely (never a partial/overflowing write) and re-request a
        // frame so the client still makes progress on the next cycle.
        if (!wm_buffer_fits(b, aw, ah)) {
            if (!g_gd_buffer_mismatch_reported) {
                g_gd_buffer_mismatch_reported = 1;
                wmc_put("*** GfxDemo INVARIANT VIOLATION: buffer size mismatch -- render skipped ***\n");
                dump_gfxdemo_state(&c, &win, fs_attached, bufs, fs_bufs, "buffer_mismatch");
            }
            wm_request_frame(&c, &win);
            continue;
        }

        render_frame(b->pixels, aw, ah, phase);
        if (wm_commit_buffer(&c, b, &win, 0, 0, aw, ah) < 0) sys_exit(1);
        g_gd_last_committed_slot = effective_fullscreen ? (2 + *cur) : (*cur);
        if (wm_request_frame(&c, &win) < 0) sys_exit(1);
        *cur ^= 1;
    }
}
