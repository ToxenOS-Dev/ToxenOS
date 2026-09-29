// ToxenOS/user64/wait_any_hardfail_test64.c — M+12B compositor
// regression: a HARD event-pipe write failure discovered DURING the
// final flush (not merely WOULDBLOCK) must mark the connection
// pending_disconnect and have it fully torn down by the SECOND
// disconnect_stalled_conns() sweep -- all BEFORE the pre-WAIT
// invariant check, so the compositor never reaches
// sys_handle_wait_any with known-pending local cleanup. See the M+12B
// design audit's own "final flush can itself create pending disconnect
// work" analysis (the two-sweep tail, g_diag_pending_disconnect_at_wait_count).
//
// This program deliberately closes its OWN read end of ITS event pipe
// (c.evt_r) while leaving its request pipe open -- pipe64_try_write's
// own contract checks `readers == 0` BEFORE any space check, so the
// compositor's very next attempt to write anything into that
// connection's evt_w returns -1 (hard failure), not WOULDBLOCK,
// regardless of how much room is left in the pipe.
//
// Client-observable self-verification (no serial-log dependency for
// the PRIMARY assertion -- see this file's own note on the two
// internal-only aspects this test does NOT and cannot check): once the
// compositor really tears the connection down (disconnect_conn closes
// its own req_r/evt_w for this connection), a FURTHER attempt by this
// client to write on its still-open req_w must fail cleanly (broken
// pipe, no reader left) -- that is the externally-visible proof that
// teardown actually completed, not just that the hard write error was
// silently swallowed. Two internal-only aspects of this scenario
// (g_diag_pending_disconnect_at_wait_count staying exactly 0, and the
// compositor subsequently reaching a genuine WAIT_ANY-blocked idle
// state) are NOT observable from this ring3 client at all -- those are
// verified via COMPOSITOR_WAIT_ANY_TRACE/the serial log during manual
// acceptance, matching this codebase's own established precedent
// (compositor_present_test64.c's own g_diag_present_failure_count
// verdict) for exactly the class of fact only the compositor's own
// internal state can prove.
#include <stdint.h>
#include "tox64.h"
#include "wmclient64.h"

#define WIN_W 8
#define WIN_H 8
#define SETTLE_TICKS 30          // generous headroom past one full compositor loop iteration
#define RECHECK_DEADLINE_TICKS 300 // bounded -- never an unbounded/blocking wait, see this file's own header comment

void _start(void) {
    wm_client_t c;
    if (wm_connect(&c) < 0) { wmc_put("wait_any_hardfail_test64: wm_connect FAILED\n"); sys_exit(1); }

    wm_window_t win;
    uint32_t wid = wm_create_window(&c, WIN_W, WIN_H, "HardFail", &win);
    if (wid == 0) { wmc_put("wait_any_hardfail_test64: create_window FAILED\n"); sys_exit(1); }

    wm_buffer_t bufA, bufB;
    if (wm_attach_buffer(&c, wid, WIN_W, WIN_H, &bufA) < 0) { wmc_put("wait_any_hardfail_test64: attach A FAILED\n"); sys_exit(1); }
    if (wm_attach_buffer(&c, wid, WIN_W, WIN_H, &bufB) < 0) { wmc_put("wait_any_hardfail_test64: attach B FAILED\n"); sys_exit(1); }
    for (uint32_t i = 0; i < (uint32_t)WIN_W * WIN_H; i++) { bufA.pixels[i] = 0x00123456u; bufB.pixels[i] = 0x00654321u; }

    if (wm_commit_buffer(&c, &bufA, &win, 0, 0, WIN_W, WIN_H) < 0) {
        wmc_put("wait_any_hardfail_test64: initial commit FAILED\n"); sys_exit(1);
    }

    // The trigger: break OUR OWN read side of the event pipe. From this
    // point on, ANY compositor write attempt to this connection's evt_w
    // returns -1 (hard failure), unconditionally -- see this file's own
    // header comment on pipe64_try_write's readers==0 check ordering.
    wmc_put("wait_any_hardfail_test64: closing own event-pipe read end\n");
    sys_handle_close(c.evt_r);

    // One more commit on the still-open request pipe -- retires the
    // previously-committed slot, which queues a WM_MSG_BUFFER_RELEASED
    // for THIS connection. Without something queued, flush_conn_queue's
    // own `while (evt_count > 0)` loop would never attempt a write at
    // all, and the hard failure wouldn't be discovered promptly.
    if (wm_commit_buffer(&c, &bufB, &win, 0, 0, WIN_W, WIN_H) < 0) {
        wmc_put("wait_any_hardfail_test64: trigger commit FAILED\n"); sys_exit(1);
    }

    // Give the compositor real scheduler time to: process this request,
    // attempt its final flush (discover the hard failure, mark
    // pending_disconnect), run its second disconnect sweep, and
    // actually tear the connection down (close_window + disconnect_conn).
    sys_sleep_ticks(SETTLE_TICKS);

    // The externally-visible proof teardown really completed: our own
    // request pipe's write end must now observe "no reader" (the
    // compositor closed ITS req_r for this connection in
    // disconnect_conn) -- bounded retry, since exactly how many
    // iterations the compositor needed is not something this client
    // controls or needs to know precisely.
    wmc_put("wait_any_hardfail_test64: checking request pipe now reports broken (teardown proof)\n");
    int torn_down = 0;
    uint64_t deadline = sys_get_ticks() + RECHECK_DEADLINE_TICKS;
    while (!torn_down && sys_get_ticks() < deadline) {
        wm_msg_t m; for (uint64_t i = 0; i < sizeof(m); i++) ((char*)&m)[i] = 0;
        m.type = WM_MSG_COMMIT_BUFFER;
        m.version = WM_PROTO_VERSION;
        m.client_id = c.client_id;
        m.window_id = win.window_id;
        m.shm_token = bufA.token;
        m.x = 0; m.y = 0; m.w = WIN_W; m.h = WIN_H;
        m.generation = win.configure_generation;
        int64_t n = sys_handle_try_write(c.req_w, (const char*)&m, sizeof(m));
        if (n == -1) { torn_down = 1; break; } // broken pipe -- no reader left, exactly what teardown produces
        sys_sleep_ticks(2);
    }

    wmc_put("wait_any_hardfail_test64: torn_down=");
    wmc_put_u64((uint64_t)torn_down);
    wmc_put("\n");

    if (torn_down) {
        wmc_put("wait_any_hardfail_test64: PASS\n");
        sys_exit(42);
    }
    wmc_put("wait_any_hardfail_test64: FAIL -- compositor never tore down the broken connection (sweep #2 bug?)\n");
    sys_exit(1);
}
