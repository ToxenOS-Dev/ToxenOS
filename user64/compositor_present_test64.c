// ToxenOS/user64/compositor_present_test64.c — post-M+12A audit regression
// test for the has_surface/display-handle bug (see that bug's own report:
// WM_MSG_COMMIT_BUFFER used to set window_t.has_surface, which made closing
// ANY committed-buffer window close the compositor's OWN display handle,
// number 0, out from under it -- silently failing every future present, for
// every window, for the rest of the session).
//
// Fully self-contained, unlike compositor_policy_test64.c's own hybrid
// design -- this bug's trigger is ordinary window lifecycle traffic
// (attach/commit/destroy/create/commit), nothing that needs real pointer
// input, so no host-side QMP half is needed here.
//
// A client cannot query sys_display_present()'s own success/failure
// directly (no such message exists, deliberately -- see wmproto64.h's own
// minimal-protocol philosophy), so this program only DRIVES the exact
// sequence that used to trip the bug; the actual PASS/FAIL verdict comes
// from compositor64.c's own g_diag_present_failure_count (a never-reset
// diagnostic counter, same discipline as g_diag_duplicate_release_count),
// read from the serial log via the DBG_PRESENT_FAIL trace a
// COMPOSITOR_PRESENT_FAIL_TRACE build of the compositor prints. Before the
// fix, that counter starts climbing the instant window A closes; after the
// fix, it stays exactly 0 through this whole sequence and for the rest of
// the session. See the milestone's own acceptance report for the two real
// boots (unfixed vs. fixed) that prove this both ways.
#include <stdint.h>
#include "tox64.h"
#include "wmclient64.h"

#define WIN_W 64
#define WIN_H 48

void _start(void) {
    wm_client_t c;
    if (wm_connect(&c) < 0) { wmc_put("compositor_present_test64: wm_connect FAILED\n"); sys_exit(1); }

    // Window A: attach + commit at least once -- this is exactly the
    // precondition the bug needed (has_surface wrongly set to 1 on a
    // committed-buffer window).
    wm_window_t winA;
    uint32_t idA = wm_create_window(&c, WIN_W, WIN_H, "PresentA", &winA);
    if (idA == 0) { wmc_put("compositor_present_test64: create A FAILED\n"); sys_exit(1); }
    wm_buffer_t bufA;
    if (wm_attach_buffer(&c, idA, WIN_W, WIN_H, &bufA) < 0) { wmc_put("compositor_present_test64: attach A FAILED\n"); sys_exit(1); }
    for (uint32_t i = 0; i < (uint64_t)WIN_W * WIN_H; i++) bufA.pixels[i] = 0x00445566u;
    if (wm_commit_buffer(&c, &bufA, &winA, 0, 0, WIN_W, WIN_H) < 0) { wmc_put("compositor_present_test64: commit A FAILED\n"); sys_exit(1); }
    wmc_put("compositor_present_test64: window A created, attached, committed\n");

    // Give the compositor a few pacing ticks to actually present A before
    // closing it -- the bug's own effect (closing the display handle) only
    // matters for presents that happen AFTER this point, so it is not
    // required for A to be visibly presented first, but doing so keeps this
    // sequence realistic (an ordinary client that draws before closing).
    sys_sleep_ticks(10);

    // The exact trigger: close a window that has committed at least once.
    wm_destroy_window(&c, &winA);
    wmc_put("compositor_present_test64: window A destroyed\n");
    sys_sleep_ticks(10); // let the compositor actually process DESTROY_WINDOW + its own full redraw

    // Window B: an ordinary new window, attached and committed AFTER A's
    // close -- before the fix, every present from this point on (this
    // window's own, and everything else's) fails silently.
    wm_window_t winB;
    uint32_t idB = wm_create_window(&c, WIN_W, WIN_H, "PresentB", &winB);
    if (idB == 0) { wmc_put("compositor_present_test64: create B FAILED\n"); sys_exit(1); }
    wm_buffer_t bufB;
    if (wm_attach_buffer(&c, idB, WIN_W, WIN_H, &bufB) < 0) { wmc_put("compositor_present_test64: attach B FAILED\n"); sys_exit(1); }
    for (uint32_t i = 0; i < (uint64_t)WIN_W * WIN_H; i++) bufB.pixels[i] = 0x00778899u;
    if (wm_commit_buffer(&c, &bufB, &winB, 0, 0, WIN_W, WIN_H) < 0) { wmc_put("compositor_present_test64: commit B FAILED\n"); sys_exit(1); }
    wmc_put("compositor_present_test64: window B created, attached, committed\n");

    // Hold a while longer -- gives the compositor's own pacing loop several
    // more full/damage present attempts, which is exactly what made the
    // pre-fix failure count climb continuously rather than staying at some
    // small fixed number.
    sys_sleep_ticks(30);

    wm_destroy_window(&c, &winB);
    wmc_put("compositor_present_test64: window B destroyed, sequence complete\n");
    sys_exit(42);
}
