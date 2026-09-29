// ToxenOS/user64/compositor_policy_test64.c — M+12A policy-boundary test,
// IN-GUEST HALF ONLY. See this file's own _start() comment before assuming
// this program alone proves anything: it is NOT a self-contained user64
// test like compositor_gen_test64.c. It creates one window and reports
// every WM_MSG_FOCUS/WM_MSG_CLOSE_REQUEST it observes to the serial log
// with clearly tagged lines -- it cannot generate the pointer input that
// drives those events itself. There is no wm-protocol message and no
// syscall that injects synthetic pointer/keyboard input: input64
// (include/input64.h) is a single exclusive kernel-owned handle held by
// the real compositor for the whole session, and no test-injection API
// exists for it. The click/drag stimulus that makes this test meaningful
// comes from a HOST-SIDE QMP script (scratchpad qclick2.py/qdrag.py in
// this milestone's own acceptance run) driving the real virtio-tablet
// device, exactly the same way a physical mouse would. This program and
// that script are two halves of ONE test; neither half alone proves the
// M+12A policy boundary.
//
// What this half proves, together with the host script: the compositor
// policy boundary (see compositor64.c's own "compositor policy boundary"
// section) really is swappable end to end -- default-policy click/drag/
// close behavior is observable over the real wire, and building the
// SAME test binary against a COMPOSITOR_POLICY_TEST_ALT compositor shows
// the opposite outcomes for the same host-side actions.
//
// Placement (M+12A's third hook, place_new_window) has no wire message
// at all -- WM_MSG_WINDOW_CREATED carries only window_id, never x/y (see
// wmproto64.h). This program's own window_id is printed to the serial
// log; the host script correlates it with compositor64.c's own
// M12A_POLICY_TRACE PLACED dump (also serial-log-only, also debug-only)
// to learn the real on-screen position rather than needing a new
// protocol message solely for this test.
#include <stdint.h>
#include "tox64.h"
#include "wmclient64.h"

#define WIN_W 220
#define WIN_H 160

void _start(void) {
    wm_client_t c;
    if (wm_connect(&c) < 0) { wmc_put("compositor_policy_test64: wm_connect FAILED\n"); sys_exit(1); }

    wm_window_t win;
    uint32_t win_id = wm_create_window(&c, WIN_W, WIN_H, "PolicyTest", &win);
    if (win_id == 0) { wmc_put("compositor_policy_test64: wm_create_window FAILED\n"); sys_exit(1); }

    // This line is the correlation point the host script's own serial-log
    // scrape depends on -- it must appear before the script goes looking
    // for this window_id's M12A_POLICY_TRACE PLACED entry.
    wmc_put("compositor_policy_test64: window_id="); wmc_put_u64((uint64_t)win_id); wmc_put("\n");

    // A committed buffer isn't needed for anything this test observes
    // (activation/titlebar decisions fire on hit-testing the window's
    // OUTER rect and titlebar, both independent of buffer content), so
    // none is attached -- this window stays visually blank, matching
    // "the minimum needed to test the policy boundary" rather than
    // duplicating gfx_demo64/gfx_interactive64's own rendering.

    // Bounded observation window: report every FOCUS/CLOSE_REQUEST this
    // connection receives for a fixed, generous budget, then exit
    // regardless of what arrived -- the PASS/FAIL judgment on whether the
    // right events arrived in the right order belongs to the host script,
    // which alone knows what it actually clicked and when. Never hangs
    // waiting for a specific message: a build/policy combination that
    // legitimately produces NO focus event (COMPOSITOR_POLICY_TEST_ALT's
    // own on_window_activate_request is an intentional no-op) is exactly
    // as valid an observation as one that does.
    uint64_t deadline = sys_get_ticks() + 9000; // ~90s -- generous for host-side QMP scripting/manual latency
    while (sys_get_ticks() < deadline) {
        wm_msg_t m;
        int64_t n = sys_handle_try_read(c.evt_r, (char*)&m, sizeof(m));
        if (n == (int64_t)sizeof(m)) {
            wmc_apply_message(&c, &m);
            if (m.type == WM_MSG_FOCUS) {
                wmc_put("compositor_policy_test64: EVENT FOCUS pressed="); wmc_put_u64((uint64_t)m.pressed); wmc_put("\n");
            } else if (m.type == WM_MSG_CLOSE_REQUEST) {
                wmc_put("compositor_policy_test64: EVENT CLOSE_REQUEST\n");
            }
            continue;
        }
        if (n == 0) { wmc_put("compositor_policy_test64: compositor closed the connection\n"); break; }
        sys_sleep_ticks(1);
    }

    wmc_put("compositor_policy_test64: observation window ended, exiting\n");
    sys_exit(42);
}
