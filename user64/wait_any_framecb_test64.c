// ToxenOS/user64/wait_any_framecb_test64.c — M+12B compositor
// regression: a frame-callback grant is a REAL queue_event() producer
// (grant_frame_callback() -> queue_event(WM_MSG_FRAME), dispatched from
// dispatch_frame_callbacks(), see the M+12B design audit's own
// producer-by-producer trace) that the pre-M+12B loop ran AFTER its own
// flush -- meaning a WM_MSG_FRAME queued behind an already-backed-up
// outbound pipe had no wake source registered for it at all under the
// old busy-poll loop's own timing, let alone under a naive first-draft
// WAIT_ANY interest set that only looked at req_r. M+12B relocated
// dispatch_frame_callbacks() to run BEFORE the final flush specifically
// so this case is covered by the exact same evt_w-conditional
// registration as every other queued critical event.
//
// Same client-observable, bounded-poll self-verification discipline as
// wait_any_evtpipe_test64.c (see that file's own header comment for why
// this never uses a real blocking read or an unbounded wait): requests
// exactly one frame callback, then -- BEFORE the compositor's pacing
// gate can possibly grant it (COMPOSITOR_PACING_TICKS elapsing takes
// real wall-clock time; this burst is a handful of fast syscalls) --
// fires the same alternating-commit burst wait_any_evtpipe_test64.c
// uses to back up the real outbound pipe, without ever reading it. By
// the time the frame callback is actually granted, its own
// WM_MSG_FRAME message queues behind that backlog. Drains afterward
// and asserts WM_MSG_FRAME specifically arrives within a bounded
// deadline.
#include <stdint.h>
#include "tox64.h"
#include "wmclient64.h"

#define WIN_W 8
#define WIN_H 8
#define NUM_COMMITS 15
#define DRAIN_DEADLINE_TICKS 300 // generous (~3s) -- a real stall must be diagnosable, not an infinite hang

void _start(void) {
    wm_client_t c;
    if (wm_connect(&c) < 0) { wmc_put("wait_any_framecb_test64: wm_connect FAILED\n"); sys_exit(1); }

    wm_window_t win;
    uint32_t wid = wm_create_window(&c, WIN_W, WIN_H, "FrameCB", &win);
    if (wid == 0) { wmc_put("wait_any_framecb_test64: create_window FAILED\n"); sys_exit(1); }

    wm_buffer_t bufA, bufB;
    if (wm_attach_buffer(&c, wid, WIN_W, WIN_H, &bufA) < 0) { wmc_put("wait_any_framecb_test64: attach A FAILED\n"); sys_exit(1); }
    if (wm_attach_buffer(&c, wid, WIN_W, WIN_H, &bufB) < 0) { wmc_put("wait_any_framecb_test64: attach B FAILED\n"); sys_exit(1); }
    for (uint32_t i = 0; i < (uint32_t)WIN_W * WIN_H; i++) { bufA.pixels[i] = 0x00778899u; bufB.pixels[i] = 0x00AABBCCu; }

    // One real commit first -- establishes uses_committed_buffers on
    // the compositor side (dispatch_frame_callbacks()'s own eligibility
    // check requires it) before requesting a callback.
    if (wm_commit_buffer(&c, &bufA, &win, 0, 0, WIN_W, WIN_H) < 0) {
        wmc_put("wait_any_framecb_test64: initial commit FAILED\n"); sys_exit(1);
    }

    if (wm_request_frame(&c, &win) < 0) {
        wmc_put("wait_any_framecb_test64: wm_request_frame FAILED\n"); sys_exit(1);
    }

    // The burst -- see this file's own header comment on why this
    // reliably backs up the real pipe before the pacing gate can grant
    // the callback above.
    wmc_put("wait_any_framecb_test64: firing commit burst, not draining events\n");
    for (int i = 0; i < NUM_COMMITS; i++) {
        wm_buffer_t* buf = (i % 2 == 0) ? &bufA : &bufB;
        if (wm_commit_buffer(&c, buf, &win, 0, 0, WIN_W, WIN_H) < 0) {
            wmc_put("wait_any_framecb_test64: commit FAILED mid-burst\n");
            sys_exit(1);
        }
    }

    wmc_put("wait_any_framecb_test64: draining, watching for WM_MSG_FRAME\n");
    int got_frame = 0;
    int release_count = 0;
    uint64_t deadline = sys_get_ticks() + DRAIN_DEADLINE_TICKS;
    while (!got_frame && sys_get_ticks() < deadline) {
        wm_msg_t m;
        int64_t n = sys_handle_try_read(c.evt_r, (char*)&m, sizeof(m));
        if (n == (int64_t)sizeof(m)) {
            if (m.type == WM_MSG_FRAME) got_frame = 1;
            else if (m.type == WM_MSG_BUFFER_RELEASED) release_count++;
            continue;
        }
        if (n == 0) break; // EOF -- compositor gone
        sys_sleep_ticks(1);
    }

    wmc_put("wait_any_framecb_test64: got_frame=");
    wmc_put_u64((uint64_t)got_frame);
    wmc_put(" release_count_seen=");
    wmc_put_u64((uint64_t)release_count);
    wmc_put("\n");

    wm_destroy_window(&c, &win);

    if (got_frame) {
        wmc_put("wait_any_framecb_test64: PASS\n");
        sys_exit(42);
    }
    wmc_put("wait_any_framecb_test64: FAIL -- WM_MSG_FRAME never arrived (frame-callback evt_w registration bug?)\n");
    sys_exit(1);
}
