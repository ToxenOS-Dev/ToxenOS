// ToxenOS/user64/wait_any_evtpipe_test64.c — M+12B compositor
// regression: outbound event-pipe backpressure must never be able to
// stall the compositor's own WAIT_ANY sleep. See the M+12B design
// audit's own "outbound event-pipe wake model" section for the full
// analysis this drives: flush_conn_queue() can leave a connection's
// evt_count > 0 after hitting SYS64_ERR_WOULDBLOCK on the real pipe
// (PIPE64_BUF_SIZE bytes, independent of CONN_EVT_QUEUE_MAX's own
// 32-message cap) -- the compositor must include that connection's
// evt_w in its WAIT_ANY interest set whenever that happens, or a
// queued event could be stuck until some UNRELATED wake happened to
// occur.
//
// Client-observable self-verification, no serial-log dependency: this
// program deliberately never reads its own event pipe while it fires a
// burst of fire-and-forget WM_MSG_COMMIT_BUFFER messages (each one
// after the first retires the PREVIOUSLY committed buffer slot, which
// queues exactly one WM_MSG_BUFFER_RELEASED event -- see
// notify_buffer_released()'s own call site inside the COMMIT_BUFFER
// handler), reliably exceeding the real pipe's byte capacity (256
// bytes -- a handful of messages) while staying safely under
// CONN_EVT_QUEUE_MAX (32) so this never trips queue_event()'s own
// overflow-to-pending_disconnect path (that is a DIFFERENT scenario,
// covered by wait_any_hardfail_test64.c). Then it drains its event
// pipe with a bounded, non-blocking poll loop (never a real blocking
// read -- see this file's own note on why an automated test must never
// be able to hang the whole boot sequence) and asserts every expected
// BUFFER_RELEASED event eventually arrives.
#include <stdint.h>
#include "tox64.h"
#include "wmclient64.h"

#define WIN_W 8
#define WIN_H 8
#define NUM_COMMITS 15                    // (NUM_COMMITS - 1) expected BUFFER_RELEASED events
#define EXPECT_RELEASES (NUM_COMMITS - 1)
#define DRAIN_DEADLINE_TICKS 300          // generous (~3s) -- a real stall must be diagnosable, not an infinite hang

void _start(void) {
    wm_client_t c;
    if (wm_connect(&c) < 0) { wmc_put("wait_any_evtpipe_test64: wm_connect FAILED\n"); sys_exit(1); }

    wm_window_t win;
    uint32_t wid = wm_create_window(&c, WIN_W, WIN_H, "EvtPipe", &win);
    if (wid == 0) { wmc_put("wait_any_evtpipe_test64: create_window FAILED\n"); sys_exit(1); }

    wm_buffer_t bufA, bufB;
    if (wm_attach_buffer(&c, wid, WIN_W, WIN_H, &bufA) < 0) { wmc_put("wait_any_evtpipe_test64: attach A FAILED\n"); sys_exit(1); }
    if (wm_attach_buffer(&c, wid, WIN_W, WIN_H, &bufB) < 0) { wmc_put("wait_any_evtpipe_test64: attach B FAILED\n"); sys_exit(1); }
    for (uint32_t i = 0; i < (uint32_t)WIN_W * WIN_H; i++) { bufA.pixels[i] = 0x00112233u; bufB.pixels[i] = 0x00445566u; }

    // The burst: alternate committing A/B with NO reads of c.evt_r in
    // between -- this is what makes the outbound pipe genuinely fill
    // faster than this client drains it, the exact precondition
    // flush_conn_queue()'s own WOULDBLOCK path needs to ever trigger.
    wmc_put("wait_any_evtpipe_test64: firing commit burst, not draining events\n");
    for (int i = 0; i < NUM_COMMITS; i++) {
        wm_buffer_t* buf = (i % 2 == 0) ? &bufA : &bufB;
        if (wm_commit_buffer(&c, buf, &win, 0, 0, WIN_W, WIN_H) < 0) {
            wmc_put("wait_any_evtpipe_test64: commit FAILED mid-burst\n");
            sys_exit(1);
        }
    }

    // Drain: bounded non-blocking poll, never a real blocking read --
    // if the compositor really were stuck (the exact bug class this
    // test exists to catch), this must time out with a clear,
    // diagnosable FAILURE rather than hang the whole kernel selftest
    // sequence forever.
    wmc_put("wait_any_evtpipe_test64: draining, counting BUFFER_RELEASED\n");
    int release_count = 0;
    uint64_t deadline = sys_get_ticks() + DRAIN_DEADLINE_TICKS;
    while (release_count < EXPECT_RELEASES && sys_get_ticks() < deadline) {
        wm_msg_t m;
        int64_t n = sys_handle_try_read(c.evt_r, (char*)&m, sizeof(m));
        if (n == (int64_t)sizeof(m)) {
            if (m.type == WM_MSG_BUFFER_RELEASED) release_count++;
            continue; // more may already be queued -- keep draining without sleeping
        }
        if (n == 0) break; // EOF -- compositor gone, definitely a failure
        sys_sleep_ticks(1); // would-block -- give the compositor a real scheduler slot
    }

    wmc_put("wait_any_evtpipe_test64: release_count=");
    wmc_put_u64((uint64_t)release_count);
    wmc_put(" expected=");
    wmc_put_u64((uint64_t)EXPECT_RELEASES);
    wmc_put("\n");

    wm_destroy_window(&c, &win);

    if (release_count == EXPECT_RELEASES) {
        wmc_put("wait_any_evtpipe_test64: PASS\n");
        sys_exit(42);
    }
    wmc_put("wait_any_evtpipe_test64: FAIL -- stuck event(s) never arrived (WAIT_ANY evt_w registration bug?)\n");
    sys_exit(1);
}
