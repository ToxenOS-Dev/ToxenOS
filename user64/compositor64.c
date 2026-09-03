// ToxenOS/user64/compositor64.c — Milestone 30: userspace compositor
// and window manager. An ordinary ring-3 process (no kernel window-
// management policy at all) that owns the exclusive display and input
// handles from Milestone 29, accepts client connections over per-
// client pipe pairs (Milestone 26 IPC), composites client shared-
// memory surfaces (Milestone 26 SHM + this milestone's cross-process
// token handoff) into its own userspace backbuffer in z-order, and
// presents the result through the Milestone 29 display interface.
//
// Milestone 32: this process no longer spawns its clients (or knows
// what programs might ever connect to it) -- it publishes the generic
// named service WM_SERVICE_NAME (include/service64.h) and accepts
// whichever independently-launched processes show up, exactly like any
// other userspace system service. See this file's own
// accept_one_client() and user64/wmclient64.h's wm_connect().
//
// Multiplexing: ToxenOS has no select()/poll()/epoll() and no threads,
// so the main loop polls its input handle, the service listener, and
// every connected client's request pipe via SYS64_HANDLE_TRY_READ/
// SYS64_SERVICE_ACCEPT (both non-blocking) each iteration, composing+
// presenting a frame only when something actually changed (a window
// committed damage, a window was created/destroyed, the cursor moved,
// a button state changed). This is a deliberate, documented scope
// choice -- a real select()-equivalent primitive is future work; the
// busy-poll costs CPU but never starves other processes (Milestone 24's
// timer preemption is unconditional).
#include <stdint.h>
#include "tox64.h"
#include "wmproto64.h"

// ── Minimal 8x16 bitmap font, ASCII 32-127 ──────────────────────────
// Same dataset kernel/fbterm64.c uses, duplicated here as plain
// userspace data -- a userspace compositor cannot link against a
// kernel module, and this milestone's guidance is explicit that
// duplicating the existing font as generic data (rather than either
// depending on fbterm64 internals or building real font rendering) is
// the acceptable middle ground for title-bar text.
static const uint8_t g_font8x16[96][16] = {
    {0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00},
    {0x00,0x00,0x18,0x3C,0x3C,0x3C,0x18,0x18,0x18,0x00,0x18,0x18,0x00,0x00,0x00,0x00},
    {0x00,0x00,0x66,0x66,0x66,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00},
    {0x00,0x00,0x36,0x36,0x7F,0x36,0x36,0x36,0x7F,0x36,0x36,0x00,0x00,0x00,0x00,0x00},
    {0x00,0x18,0x18,0x3E,0x63,0x61,0x60,0x3E,0x03,0x43,0x63,0x3E,0x18,0x18,0x00,0x00},
    {0x00,0x00,0x00,0x00,0x61,0x63,0x06,0x0C,0x18,0x30,0x63,0x43,0x00,0x00,0x00,0x00},
    {0x00,0x00,0x1C,0x36,0x36,0x1C,0x3B,0x6E,0x66,0x66,0x66,0x3B,0x00,0x00,0x00,0x00},
    {0x00,0x00,0x18,0x18,0x18,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00},
    {0x00,0x00,0x0C,0x18,0x30,0x30,0x30,0x30,0x30,0x30,0x18,0x0C,0x00,0x00,0x00,0x00},
    {0x00,0x00,0x30,0x18,0x0C,0x0C,0x0C,0x0C,0x0C,0x0C,0x18,0x30,0x00,0x00,0x00,0x00},
    {0x00,0x00,0x00,0x00,0x00,0x66,0x3C,0xFF,0x3C,0x66,0x00,0x00,0x00,0x00,0x00,0x00},
    {0x00,0x00,0x00,0x00,0x00,0x18,0x18,0x7E,0x18,0x18,0x00,0x00,0x00,0x00,0x00,0x00},
    {0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x18,0x18,0x18,0x30,0x00,0x00,0x00},
    {0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x7E,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00},
    {0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x18,0x18,0x00,0x00,0x00,0x00},
    {0x00,0x00,0x00,0x00,0x01,0x03,0x06,0x0C,0x18,0x30,0x60,0x40,0x00,0x00,0x00,0x00},
    {0x00,0x00,0x3E,0x63,0x63,0x63,0x6B,0x6B,0x63,0x63,0x63,0x3E,0x00,0x00,0x00,0x00},
    {0x00,0x00,0x18,0x38,0x58,0x18,0x18,0x18,0x18,0x18,0x18,0x7E,0x00,0x00,0x00,0x00},
    {0x00,0x00,0x3E,0x63,0x03,0x06,0x0C,0x18,0x30,0x60,0x63,0x7F,0x00,0x00,0x00,0x00},
    {0x00,0x00,0x3E,0x63,0x03,0x03,0x1E,0x03,0x03,0x03,0x63,0x3E,0x00,0x00,0x00,0x00},
    {0x00,0x00,0x06,0x0E,0x1E,0x36,0x66,0x66,0x7F,0x06,0x06,0x0F,0x00,0x00,0x00,0x00},
    {0x00,0x00,0x7F,0x60,0x60,0x60,0x7E,0x03,0x03,0x03,0x63,0x3E,0x00,0x00,0x00,0x00},
    {0x00,0x00,0x1C,0x30,0x60,0x60,0x7E,0x63,0x63,0x63,0x63,0x3E,0x00,0x00,0x00,0x00},
    {0x00,0x00,0x7F,0x63,0x03,0x06,0x06,0x0C,0x0C,0x18,0x18,0x18,0x00,0x00,0x00,0x00},
    {0x00,0x00,0x3E,0x63,0x63,0x63,0x3E,0x63,0x63,0x63,0x63,0x3E,0x00,0x00,0x00,0x00},
    {0x00,0x00,0x3E,0x63,0x63,0x63,0x3F,0x03,0x03,0x03,0x06,0x3C,0x00,0x00,0x00,0x00},
    {0x00,0x00,0x00,0x00,0x18,0x18,0x00,0x00,0x00,0x18,0x18,0x00,0x00,0x00,0x00,0x00},
    {0x00,0x00,0x00,0x00,0x18,0x18,0x00,0x00,0x00,0x18,0x18,0x30,0x00,0x00,0x00,0x00},
    {0x00,0x00,0x00,0x06,0x0C,0x18,0x30,0x60,0x30,0x18,0x0C,0x06,0x00,0x00,0x00,0x00},
    {0x00,0x00,0x00,0x00,0x00,0x7E,0x00,0x00,0x7E,0x00,0x00,0x00,0x00,0x00,0x00,0x00},
    {0x00,0x00,0x00,0x60,0x30,0x18,0x0C,0x06,0x0C,0x18,0x30,0x60,0x00,0x00,0x00,0x00},
    {0x00,0x00,0x3E,0x63,0x63,0x06,0x0C,0x0C,0x0C,0x00,0x0C,0x0C,0x00,0x00,0x00,0x00},
    {0x00,0x00,0x3E,0x63,0x63,0x6F,0x6B,0x6B,0x6F,0x60,0x60,0x3E,0x00,0x00,0x00,0x00},
    {0x00,0x00,0x08,0x1C,0x36,0x63,0x63,0x7F,0x63,0x63,0x63,0x63,0x00,0x00,0x00,0x00},
    {0x00,0x00,0x7E,0x33,0x33,0x33,0x3E,0x33,0x33,0x33,0x33,0x7E,0x00,0x00,0x00,0x00},
    {0x00,0x00,0x1E,0x33,0x61,0x60,0x60,0x60,0x60,0x61,0x33,0x1E,0x00,0x00,0x00,0x00},
    {0x00,0x00,0x7C,0x36,0x33,0x33,0x33,0x33,0x33,0x33,0x36,0x7C,0x00,0x00,0x00,0x00},
    {0x00,0x00,0x7F,0x33,0x31,0x34,0x3C,0x34,0x30,0x31,0x33,0x7F,0x00,0x00,0x00,0x00},
    {0x00,0x00,0x7F,0x33,0x31,0x34,0x3C,0x34,0x30,0x30,0x30,0x78,0x00,0x00,0x00,0x00},
    {0x00,0x00,0x1E,0x33,0x61,0x60,0x60,0x6F,0x63,0x63,0x33,0x1D,0x00,0x00,0x00,0x00},
    {0x00,0x00,0x63,0x63,0x63,0x63,0x7F,0x63,0x63,0x63,0x63,0x63,0x00,0x00,0x00,0x00},
    {0x00,0x00,0x3C,0x18,0x18,0x18,0x18,0x18,0x18,0x18,0x18,0x3C,0x00,0x00,0x00,0x00},
    {0x00,0x00,0x1F,0x06,0x06,0x06,0x06,0x06,0x06,0x66,0x66,0x3C,0x00,0x00,0x00,0x00},
    {0x00,0x00,0x73,0x33,0x36,0x36,0x3C,0x36,0x36,0x33,0x33,0x73,0x00,0x00,0x00,0x00},
    {0x00,0x00,0x78,0x30,0x30,0x30,0x30,0x30,0x30,0x31,0x33,0x7F,0x00,0x00,0x00,0x00},
    {0x00,0x00,0x63,0x77,0x7F,0x6B,0x63,0x63,0x63,0x63,0x63,0x63,0x00,0x00,0x00,0x00},
    {0x00,0x00,0x63,0x63,0x73,0x7B,0x6F,0x67,0x63,0x63,0x63,0x63,0x00,0x00,0x00,0x00},
    {0x00,0x00,0x1C,0x36,0x63,0x63,0x63,0x63,0x63,0x63,0x36,0x1C,0x00,0x00,0x00,0x00},
    {0x00,0x00,0x7E,0x33,0x33,0x33,0x3E,0x30,0x30,0x30,0x30,0x78,0x00,0x00,0x00,0x00},
    {0x00,0x00,0x1C,0x36,0x63,0x63,0x63,0x63,0x6B,0x6B,0x36,0x1C,0x06,0x00,0x00,0x00},
    {0x00,0x00,0x7E,0x33,0x33,0x33,0x3E,0x36,0x33,0x33,0x33,0x73,0x00,0x00,0x00,0x00},
    {0x00,0x00,0x1E,0x33,0x33,0x30,0x1C,0x06,0x03,0x33,0x33,0x1E,0x00,0x00,0x00,0x00},
    {0x00,0x00,0xFF,0xDB,0x99,0x18,0x18,0x18,0x18,0x18,0x18,0x3C,0x00,0x00,0x00,0x00},
    {0x00,0x00,0x63,0x63,0x63,0x63,0x63,0x63,0x63,0x63,0x63,0x3E,0x00,0x00,0x00,0x00},
    {0x00,0x00,0x63,0x63,0x63,0x63,0x63,0x63,0x63,0x36,0x1C,0x08,0x00,0x00,0x00,0x00},
    {0x00,0x00,0x63,0x63,0x63,0x63,0x6B,0x6B,0x6B,0x7F,0x36,0x36,0x00,0x00,0x00,0x00},
    {0x00,0x00,0x63,0x63,0x36,0x36,0x1C,0x1C,0x36,0x36,0x63,0x63,0x00,0x00,0x00,0x00},
    {0x00,0x00,0xC3,0xC3,0xC3,0x66,0x3C,0x18,0x18,0x18,0x18,0x3C,0x00,0x00,0x00,0x00},
    {0x00,0x00,0x7F,0x63,0x43,0x06,0x0C,0x18,0x30,0x61,0x63,0x7F,0x00,0x00,0x00,0x00},
    {0x00,0x00,0x3C,0x30,0x30,0x30,0x30,0x30,0x30,0x30,0x30,0x3C,0x00,0x00,0x00,0x00},
    {0x00,0x00,0x00,0x00,0x40,0x60,0x30,0x18,0x0C,0x06,0x03,0x01,0x00,0x00,0x00,0x00},
    {0x00,0x00,0x3C,0x0C,0x0C,0x0C,0x0C,0x0C,0x0C,0x0C,0x0C,0x3C,0x00,0x00,0x00,0x00},
    {0x08,0x1C,0x36,0x63,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00},
    {0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0xFF,0x00,0x00,0x00,0x00},
    {0x00,0x18,0x18,0x0C,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00},
    {0x00,0x00,0x00,0x00,0x00,0x3E,0x63,0x03,0x3F,0x63,0x63,0x3F,0x00,0x00,0x00,0x00},
    {0x00,0x00,0x70,0x30,0x30,0x3E,0x33,0x33,0x33,0x33,0x33,0x6E,0x00,0x00,0x00,0x00},
    {0x00,0x00,0x00,0x00,0x00,0x1E,0x33,0x60,0x60,0x60,0x33,0x1E,0x00,0x00,0x00,0x00},
    {0x00,0x00,0x0E,0x06,0x06,0x3E,0x66,0x66,0x66,0x66,0x66,0x3B,0x00,0x00,0x00,0x00},
    {0x00,0x00,0x00,0x00,0x00,0x3E,0x63,0x63,0x7F,0x60,0x63,0x3E,0x00,0x00,0x00,0x00},
    {0x00,0x00,0x1C,0x36,0x30,0x30,0x78,0x30,0x30,0x30,0x30,0x78,0x00,0x00,0x00,0x00},
    {0x00,0x00,0x00,0x00,0x00,0x3B,0x66,0x66,0x66,0x66,0x3E,0x06,0x66,0x3C,0x00,0x00},
    {0x00,0x00,0x70,0x30,0x30,0x36,0x3B,0x33,0x33,0x33,0x33,0x73,0x00,0x00,0x00,0x00},
    {0x00,0x00,0x18,0x18,0x00,0x38,0x18,0x18,0x18,0x18,0x18,0x3C,0x00,0x00,0x00,0x00},
    {0x00,0x00,0x06,0x06,0x00,0x0E,0x06,0x06,0x06,0x06,0x06,0x66,0x66,0x3C,0x00,0x00},
    {0x00,0x00,0x70,0x30,0x30,0x33,0x36,0x3C,0x36,0x33,0x33,0x73,0x00,0x00,0x00,0x00},
    {0x00,0x00,0x38,0x18,0x18,0x18,0x18,0x18,0x18,0x18,0x18,0x3C,0x00,0x00,0x00,0x00},
    {0x00,0x00,0x00,0x00,0x00,0x66,0x7F,0x6B,0x6B,0x6B,0x6B,0x63,0x00,0x00,0x00,0x00},
    {0x00,0x00,0x00,0x00,0x00,0x6E,0x33,0x33,0x33,0x33,0x33,0x33,0x00,0x00,0x00,0x00},
    {0x00,0x00,0x00,0x00,0x00,0x1E,0x33,0x33,0x33,0x33,0x33,0x1E,0x00,0x00,0x00,0x00},
    {0x00,0x00,0x00,0x00,0x00,0x6E,0x33,0x33,0x33,0x33,0x3E,0x30,0x30,0x78,0x00,0x00},
    {0x00,0x00,0x00,0x00,0x00,0x3B,0x66,0x66,0x66,0x66,0x3E,0x06,0x06,0x0F,0x00,0x00},
    {0x00,0x00,0x00,0x00,0x00,0x6E,0x3B,0x33,0x30,0x30,0x30,0x78,0x00,0x00,0x00,0x00},
    {0x00,0x00,0x00,0x00,0x00,0x3E,0x63,0x60,0x3E,0x03,0x63,0x3E,0x00,0x00,0x00,0x00},
    {0x00,0x00,0x08,0x18,0x18,0x7E,0x18,0x18,0x18,0x18,0x1A,0x0C,0x00,0x00,0x00,0x00},
    {0x00,0x00,0x00,0x00,0x00,0x63,0x63,0x63,0x63,0x63,0x63,0x3F,0x00,0x00,0x00,0x00},
    {0x00,0x00,0x00,0x00,0x00,0x63,0x63,0x63,0x63,0x36,0x1C,0x08,0x00,0x00,0x00,0x00},
    {0x00,0x00,0x00,0x00,0x00,0x63,0x63,0x6B,0x6B,0x6B,0x7F,0x36,0x00,0x00,0x00,0x00},
    {0x00,0x00,0x00,0x00,0x00,0x63,0x36,0x1C,0x1C,0x1C,0x36,0x63,0x00,0x00,0x00,0x00},
    {0x00,0x00,0x00,0x00,0x00,0x63,0x63,0x63,0x63,0x63,0x3F,0x03,0x06,0x3C,0x00,0x00},
    {0x00,0x00,0x00,0x00,0x00,0x7F,0x63,0x06,0x0C,0x18,0x33,0x7F,0x00,0x00,0x00,0x00},
    {0x00,0x00,0x0E,0x18,0x18,0x18,0x70,0x18,0x18,0x18,0x18,0x0E,0x00,0x00,0x00,0x00},
    {0x00,0x00,0x18,0x18,0x18,0x18,0x00,0x18,0x18,0x18,0x18,0x18,0x00,0x00,0x00,0x00},
    {0x00,0x00,0x70,0x18,0x18,0x18,0x0E,0x18,0x18,0x18,0x18,0x70,0x00,0x00,0x00,0x00},
    {0x00,0x00,0x3B,0x6E,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00},
    {0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00},
};

static int wmc_strlen(const char* s) { int i = 0; while (s[i]) i++; return i; }
static void put(const char* s) { sys_write(s, (uint64_t)wmc_strlen(s)); }
static void append_u64(char* out, int* pos, uint64_t v) {
    char rev[24]; int rn = 0;
    if (v == 0) rev[rn++] = '0';
    while (v > 0) { rev[rn++] = (char)('0' + (v % 10)); v /= 10; }
    while (rn > 0) out[(*pos)++] = rev[--rn];
}
static void put_kv(const char* label, uint64_t v) {
    char line[64]; int pos = 0;
    while (*label) line[pos++] = *label++;
    append_u64(line, &pos, v);
    line[pos++] = '\n'; line[pos] = 0;
    put(line);
}

// ── Layout constants ─────────────────────────────────────────────────
#define TITLEBAR_H 20
#define BORDER 2
#define MAX_CLIENTS 8
#define MAX_WINDOWS 16
#define BG_COLOR 0x2E3B4E
#define BORDER_COLOR_FOCUSED   0x4A90D9
#define BORDER_COLOR_UNFOCUSED 0x555555
#define TITLE_TEXT_COLOR 0xFFFFFF
#define CURSOR_COLOR 0xFFFF00

// Milestone 32.1: bounded per-connection outgoing event queue -- see
// queue_event()'s header comment for the full delivery/coalescing/
// backpressure design this exists to support. A plain fixed array (no
// heap allocation): nothing to leak, disconnect just needs to reset
// evt_count.
#define CONN_EVT_QUEUE_MAX 32

typedef struct {
    int in_use;
    int req_r;         // compositor's own handle: read end of client's request pipe
    int evt_w;         // compositor's own handle: write end of client's event pipe
    uint32_t client_id;
    int said_hello;
    wm_msg_t evt_queue[CONN_EVT_QUEUE_MAX]; // FIFO, oldest at index 0
    int evt_count;
    // Milestone 32.1: set by queue_event() when this connection's
    // outgoing queue overflowed with a critical (non-coalescible) event
    // it could not absorb, or by flush_conn_queue() when the event pipe
    // turns out to be broken. The ACTUAL disconnect_conn() call is
    // always deferred to one safe point in the main loop (see
    // disconnect_stalled_conns()) -- queue_event()/flush_conn_queue()
    // are called from deep inside per-event/per-window loops elsewhere
    // in this file, and never tearing a connection down from there
    // avoids any question of whether some caller might still touch
    // g_windows[]/g_conns[] state for this connection after the call.
    int pending_disconnect;
} conn_t;

typedef struct {
    int in_use;
    uint32_t window_id;
    int conn_index;
    int32_t x, y;                 // top-left of the DECORATED window (outside border)
    uint32_t content_w, content_h;
    char title[WM_TITLE_MAX + 1];
    int has_surface;
    int shm_handle;                // compositor's own handle
    uint64_t surface_addr;         // mapped (read-only) in the compositor's own AS
    uint32_t surface_stride;
    uint64_t surface_bytes;        // actual mapped size -- bounds every read
} window_t;

static conn_t g_conns[MAX_CLIENTS];
static window_t g_windows[MAX_WINDOWS];
static int g_zorder[MAX_WINDOWS];
static int g_zorder_count = 0;
static int g_focused = -1;
static int g_dragging = -1;
static int32_t g_drag_off_x = 0, g_drag_off_y = 0;

static uint32_t g_next_window_id = 1;
static uint32_t g_next_client_id = 1;

static int g_display_h = -1, g_input_h = -1;
static uint32_t g_disp_w = 0, g_disp_h = 0;
static uint32_t* g_backbuffer = 0;

static int32_t g_cursor_x = 0, g_cursor_y = 0;

static inline uint32_t win_total_w(const window_t* w) { return w->content_w + 2u * BORDER; }
static inline uint32_t win_total_h(const window_t* w) { return w->content_h + 2u * BORDER + TITLEBAR_H; }

// ── Backbuffer drawing primitives ───────────────────────────────────
static void bb_fill_rect(int32_t x, int32_t y, uint32_t w, uint32_t h, uint32_t color) {
    int32_t x0 = x < 0 ? 0 : x;
    int32_t y0 = y < 0 ? 0 : y;
    int64_t x1 = (int64_t)x + w; if (x1 > (int64_t)g_disp_w) x1 = g_disp_w;
    int64_t y1 = (int64_t)y + h; if (y1 > (int64_t)g_disp_h) y1 = g_disp_h;
    for (int32_t yy = y0; yy < y1; yy++)
        for (int32_t xx = x0; xx < x1; xx++)
            g_backbuffer[(uint32_t)yy * g_disp_w + (uint32_t)xx] = color;
}

static void bb_put_pixel(int32_t x, int32_t y, uint32_t color) {
    if (x < 0 || y < 0 || (uint32_t)x >= g_disp_w || (uint32_t)y >= g_disp_h) return;
    g_backbuffer[(uint32_t)y * g_disp_w + (uint32_t)x] = color;
}

static void bb_draw_glyph(int32_t x, int32_t y, unsigned char ch, uint32_t color) {
    const uint8_t* glyph = g_font8x16[(ch >= 32 && ch <= 127) ? ch - 32u : 0u];
    for (int r = 0; r < 16; r++) {
        uint8_t bits = glyph[r];
        for (int c = 0; c < 8; c++) {
            if (bits & (0x80u >> c)) bb_put_pixel(x + c, y + r, color);
        }
    }
}

static void bb_draw_text(int32_t x, int32_t y, const char* s, uint32_t color) {
    int32_t cx = x;
    while (*s) { bb_draw_glyph(cx, y, (unsigned char)*s, color); cx += 8; s++; }
}

// ── Window/connection lookups ────────────────────────────────────────
static window_t* find_window_owned_by(int conn_index, uint32_t window_id) {
    if (window_id == 0) return 0;
    for (int i = 0; i < MAX_WINDOWS; i++) {
        if (g_windows[i].in_use && g_windows[i].window_id == window_id && g_windows[i].conn_index == conn_index)
            return &g_windows[i];
    }
    return 0;
}

static int window_index_of(const window_t* w) { return (int)(w - g_windows); }

static void zorder_remove(int idx) {
    int j = 0;
    for (int i = 0; i < g_zorder_count; i++) {
        if (g_zorder[i] != idx) g_zorder[j++] = g_zorder[i];
    }
    g_zorder_count = j;
}

static void raise_window(int idx) {
    zorder_remove(idx);
    g_zorder[g_zorder_count++] = idx;
}

// Milestone 32.1: queues `m` for delivery to `conn_index`, replacing
// the Milestone 30 design that wrote directly (and blockingly) to the
// client's event pipe -- see this file's own header comment and the
// Milestone 32 summary for why that could freeze the ENTIRE compositor
// (a single-threaded process that also owns input, service accepts,
// and presentation) on nothing worse than one client that stopped
// reading. Actual delivery happens later, non-blockingly, in
// flush_conn_queue() -- called once per main-loop iteration for every
// connection, so under normal conditions (a client draining its event
// pipe promptly, as every well-behaved client does) a queued event is
// written out to the real pipe within the same iteration it was
// generated in, with no observable delay.
//
// Two message types are deliberately COALESCIBLE -- queued at most
// ONCE per window at any time, folding a newer occurrence into the
// existing queued one instead of appending a second entry:
//   - WM_MSG_POINTER_MOTION: only the LATEST position before the
//     client next reads matters; older positions carry no information
//     a client would ever act on differently. Coalescing this is safe
//     and is what keeps a fast mouse from ever being able to fill the
//     queue on its own.
//   - WM_MSG_POINTER_WHEEL: coalesced by SUMMING the new delta (m->x,
//     see wmproto64.h) into the existing queued entry, rather than
//     replacing it -- this is the one coalescible type where simply
//     keeping "the latest" would silently lose scroll distance; summing
//     preserves the total even though the event COUNT is reduced.
// Every other type (WM_MSG_KEY_EVENT, WM_MSG_POINTER_BUTTON,
// WM_MSG_FOCUS, WM_MSG_CLOSE_REQUEST, and every control/reply message
// -- WELCOME/WINDOW_CREATED/ACK/ERROR) is CRITICAL: each one is a
// discrete state transition or a reply a blocking client library call
// is waiting on, so none of these may ever be silently dropped while
// there is any alternative. They are appended to the FIFO tail and
// never coalesced or reordered.
//
// Backpressure policy: the queue is bounded (CONN_EVT_QUEUE_MAX). If a
// COALESCIBLE event has nowhere to coalesce into and the queue is
// full, it is simply dropped -- always safe, since (by definition of
// being coalescible) a fresher equivalent event will follow soon and
// supersede it. If a CRITICAL event cannot be appended because the
// queue is genuinely full of other undelivered critical events, that
// is this milestone's defined threshold for "this client has stopped
// draining for too long": the connection is flagged for disconnect
// (see conn_t::pending_disconnect) rather than the event being lost or
// this function ever blocking. A connection already flagged is a lost
// cause for further delivery anyway, so any event queued to it past
// that point is simply dropped -- the flag guarantees disconnect_conn()
// runs on the very next main-loop pass regardless.
static void queue_event(int conn_index, const wm_msg_t* m) {
    if (conn_index < 0 || !g_conns[conn_index].in_use) return;
    conn_t* c = &g_conns[conn_index];
    if (c->pending_disconnect) return; // already a lost cause -- about to be torn down

    if (m->type == WM_MSG_POINTER_MOTION) {
        for (int i = 0; i < c->evt_count; i++) {
            if (c->evt_queue[i].type == WM_MSG_POINTER_MOTION && c->evt_queue[i].window_id == m->window_id) {
                c->evt_queue[i] = *m;
                return;
            }
        }
    } else if (m->type == WM_MSG_POINTER_WHEEL) {
        for (int i = 0; i < c->evt_count; i++) {
            if (c->evt_queue[i].type == WM_MSG_POINTER_WHEEL && c->evt_queue[i].window_id == m->window_id) {
                c->evt_queue[i].x += m->x; // wheel delta accumulator, see wmproto64.h
                return;
            }
        }
    }

    if (c->evt_count < CONN_EVT_QUEUE_MAX) {
        c->evt_queue[c->evt_count++] = *m;
        return;
    }

    // Queue genuinely full and coalescing didn't apply (either a fresh
    // coalescible type with no existing entry, or -- the real backpressure
    // case -- a critical, never-coalesced event).
    if (m->type == WM_MSG_POINTER_MOTION || m->type == WM_MSG_POINTER_WHEEL) {
        return; // safe to drop -- see header comment
    }
    c->pending_disconnect = 1;
}

// Milestone 32.1: drains as much of `ci`'s outgoing queue as the real
// pipe currently has room for, via non-blocking writes only -- never
// blocks regardless of how backed up the client is. Called once per
// main-loop iteration for every connected client.
static void flush_conn_queue(int ci) {
    conn_t* c = &g_conns[ci];
    while (c->evt_count > 0) {
        int64_t n = sys_handle_try_write(c->evt_w, (const char*)&c->evt_queue[0], sizeof(c->evt_queue[0]));
        if (n == (int64_t)sizeof(c->evt_queue[0])) {
            for (int i = 1; i < c->evt_count; i++) c->evt_queue[i - 1] = c->evt_queue[i];
            c->evt_count--;
            continue; // keep draining -- more may fit
        }
        if (n < 0 && n != SYS64_ERR_WOULDBLOCK) {
            // Broken pipe -- the client's read end is already gone (it
            // closed its own handle, or faulted, without the compositor
            // having seen EOF on the request pipe yet). Nothing more can
            // ever be delivered here; let the deferred-disconnect sweep
            // tear it down on the next pass.
            c->pending_disconnect = 1;
        }
        break; // would-block (pipe currently full) -- try again next iteration
    }
}

static void send_ack(int conn_index, uint32_t window_id) {
    wm_msg_t m; for (uint64_t i = 0; i < sizeof(m); i++) ((char*)&m)[i] = 0;
    m.type = WM_MSG_ACK; m.version = WM_PROTO_VERSION; m.window_id = window_id;
    queue_event(conn_index, &m);
}
static void send_error(int conn_index, uint32_t window_id, int32_t code) {
    wm_msg_t m; for (uint64_t i = 0; i < sizeof(m); i++) ((char*)&m)[i] = 0;
    m.type = WM_MSG_ERROR; m.version = WM_PROTO_VERSION; m.window_id = window_id; m.x = code;
    queue_event(conn_index, &m);
}

// Milestone 30: `notify_new` controls whether the NEWLY-focused
// window's own connection is told about it. This must be 0 when called
// from WM_MSG_CREATE_WINDOW's handler: that handler still owes this
// exact connection its WINDOW_CREATED reply, sent right after this
// call returns, on the SAME event pipe -- an unsolicited FOCUS message
// injected first would race ahead of it, and the client library's
// wm_create_window() (which just reads the next message and expects it
// to be WINDOW_CREATED) would misinterpret the FOCUS message as its
// reply and fail. A brand-new window can safely assume it starts
// focused without an explicit notification; every OTHER focus
// transition (click-to-focus, losing focus to another window) calls
// this with notify_new=1 since no reply is pending on that connection
// at that moment.
static void focus_window_ex(int idx, int notify_new) {
    if (g_focused == idx) return;
    if (g_focused >= 0 && g_windows[g_focused].in_use) {
        wm_msg_t m; for (uint64_t i = 0; i < sizeof(m); i++) ((char*)&m)[i] = 0;
        m.type = WM_MSG_FOCUS; m.version = WM_PROTO_VERSION;
        m.window_id = g_windows[g_focused].window_id; m.pressed = 0;
        queue_event(g_windows[g_focused].conn_index, &m);
    }
    g_focused = idx;
    if (idx >= 0 && notify_new) {
        wm_msg_t m; for (uint64_t i = 0; i < sizeof(m); i++) ((char*)&m)[i] = 0;
        m.type = WM_MSG_FOCUS; m.version = WM_PROTO_VERSION;
        m.window_id = g_windows[idx].window_id; m.pressed = 1;
        queue_event(g_windows[idx].conn_index, &m);
    }
}
static void focus_window(int idx) { focus_window_ex(idx, 1); }

static void free_window_surface(window_t* w) {
    if (!w->has_surface) return;
    sys_munmap(w->surface_addr, w->surface_bytes);
    sys_handle_close(w->shm_handle);
    w->has_surface = 0;
}

static void close_window(int idx) {
    window_t* w = &g_windows[idx];
    free_window_surface(w);
    zorder_remove(idx);
    if (g_focused == idx) focus_window(-1);
    if (g_dragging == idx) g_dragging = -1;
    w->in_use = 0;
}

static void disconnect_conn(int conn_index) {
    for (int i = 0; i < MAX_WINDOWS; i++) {
        if (g_windows[i].in_use && g_windows[i].conn_index == conn_index) close_window(i);
    }
    sys_handle_close(g_conns[conn_index].req_r);
    sys_handle_close(g_conns[conn_index].evt_w);
    g_conns[conn_index].in_use = 0;
    g_conns[conn_index].evt_count = 0;         // Milestone 32.1: nothing to free (fixed array), just reset
    g_conns[conn_index].pending_disconnect = 0;
}

// ── Hit testing ──────────────────────────────────────────────────────
// Iterates z-order TOP TO BOTTOM (reverse of draw order) so the
// topmost visible window wins.
static int hit_test_any(int32_t px, int32_t py) {
    for (int i = g_zorder_count - 1; i >= 0; i--) {
        int idx = g_zorder[i];
        window_t* w = &g_windows[idx];
        if (px >= w->x && py >= w->y && px < w->x + (int32_t)win_total_w(w) && py < w->y + (int32_t)win_total_h(w))
            return idx;
    }
    return -1;
}
static int hit_test_titlebar(const window_t* w, int32_t px, int32_t py) {
    int32_t tx0 = w->x + BORDER, ty0 = w->y + BORDER;
    return px >= tx0 && py >= ty0 && px < tx0 + (int32_t)w->content_w && py < ty0 + TITLEBAR_H;
}
static int hit_test_content(const window_t* w, int32_t px, int32_t py) {
    int32_t cx0 = w->x + BORDER, cy0 = w->y + BORDER + TITLEBAR_H;
    return px >= cx0 && py >= cy0 && px < cx0 + (int32_t)w->content_w && py < cy0 + (int32_t)w->content_h;
}

// ── Composition ──────────────────────────────────────────────────────
static void compose_window(const window_t* w, int focused) {
    uint32_t border_color = focused ? BORDER_COLOR_FOCUSED : BORDER_COLOR_UNFOCUSED;
    bb_fill_rect(w->x, w->y, win_total_w(w), win_total_h(w), border_color);
    bb_fill_rect(w->x + BORDER, w->y + BORDER, w->content_w, TITLEBAR_H, focused ? 0x3A5A85 : 0x3A3A3A);
    bb_draw_text(w->x + BORDER + 4, w->y + BORDER + 2, w->title, TITLE_TEXT_COLOR);

    if (!w->has_surface) {
        bb_fill_rect(w->x + BORDER, w->y + BORDER + TITLEBAR_H, w->content_w, w->content_h, 0x000000);
        return;
    }

    // Blit the client's surface, clipped to BOTH the display bounds and
    // the surface's ACTUAL mapped byte size -- a malicious/buggy client
    // cannot make the compositor read past what it genuinely mapped
    // (see WM_MSG_ATTACH_SURFACE's validation in handle_client_message).
    for (uint32_t row = 0; row < w->content_h; row++) {
        uint64_t row_off = (uint64_t)row * w->surface_stride;
        if (row_off + (uint64_t)w->content_w * 4 > w->surface_bytes) break;
        int32_t dst_y = w->y + BORDER + TITLEBAR_H + (int32_t)row;
        if (dst_y < 0 || (uint32_t)dst_y >= g_disp_h) continue;
        const uint32_t* src_row = (const uint32_t*)(uintptr_t)(w->surface_addr + row_off);
        for (uint32_t col = 0; col < w->content_w; col++) {
            int32_t dst_x = w->x + BORDER + (int32_t)col;
            if (dst_x < 0 || (uint32_t)dst_x >= g_disp_w) continue;
            g_backbuffer[(uint32_t)dst_y * g_disp_w + (uint32_t)dst_x] = src_row[col];
        }
    }
}

static void bb_draw_cursor(int32_t x, int32_t y) {
    // Simple filled arrow-ish triangle, small enough to stay legible --
    // drawn fresh into the backbuffer every frame, never mutating any
    // window's surface underneath it.
    for (int32_t r = 0; r < 12; r++) {
        for (int32_t c = 0; c <= r; c++) {
            bb_put_pixel(x + c, y + r, CURSOR_COLOR);
        }
    }
}

static void compose_and_present(void) {
    bb_fill_rect(0, 0, g_disp_w, g_disp_h, BG_COLOR);
    for (int i = 0; i < g_zorder_count; i++) {
        int idx = g_zorder[i];
        compose_window(&g_windows[idx], idx == g_focused);
    }
    bb_draw_cursor(g_cursor_x, g_cursor_y);

    display64_present_req_t req;
    req.buf_ptr = (uint64_t)(uintptr_t)g_backbuffer;
    req.pitch = 0;
    req.x = 0; req.y = 0; req.w = g_disp_w; req.h = g_disp_h;
    sys_display_present(g_display_h, &req);
}

// ── Client message handling ─────────────────────────────────────────
static int find_free_window_slot(void) {
    for (int i = 0; i < MAX_WINDOWS; i++) if (!g_windows[i].in_use) return i;
    return -1;
}

static void handle_client_message(int conn_index, const wm_msg_t* msg, int* dirty) {
    conn_t* c = &g_conns[conn_index];

    if (msg->version != WM_PROTO_VERSION) { send_error(conn_index, msg->window_id, WM_ERR_BAD_VERSION); return; }

    if (!c->said_hello) {
        if (msg->type != WM_MSG_HELLO) return; // ignore anything before HELLO
        c->said_hello = 1;
        c->client_id = g_next_client_id++;
        wm_msg_t reply; for (uint64_t i = 0; i < sizeof(reply); i++) ((char*)&reply)[i] = 0;
        reply.type = WM_MSG_WELCOME; reply.version = WM_PROTO_VERSION; reply.client_id = c->client_id;
        queue_event(conn_index, &reply);
        return;
    }

    switch (msg->type) {
    case WM_MSG_CREATE_WINDOW: {
        if (msg->w == 0 || msg->h == 0 || msg->w > 4096 || msg->h > 4096) {
            send_error(conn_index, 0, WM_ERR_BAD_SIZE); break;
        }
        int idx = find_free_window_slot();
        if (idx < 0) { send_error(conn_index, 0, WM_ERR_NO_RESOURCES); break; }
        window_t* w = &g_windows[idx];
        for (uint64_t i = 0; i < sizeof(*w); i++) ((char*)w)[i] = 0;
        w->in_use = 1;
        w->window_id = g_next_window_id++;
        w->conn_index = conn_index;
        w->content_w = msg->w; w->content_h = msg->h;
        int ti = 0; while (ti < WM_TITLE_MAX && msg->title[ti]) { w->title[ti] = msg->title[ti]; ti++; }
        w->title[ti] = 0;
        w->x = (int32_t)(40 + 30 * (idx % 6));
        w->y = (int32_t)(40 + 30 * (idx % 6));
        g_zorder[g_zorder_count++] = idx;
        focus_window_ex(idx, 0); // don't notify -- WINDOW_CREATED reply is still pending on this same connection

        wm_msg_t reply; for (uint64_t i = 0; i < sizeof(reply); i++) ((char*)&reply)[i] = 0;
        reply.type = WM_MSG_WINDOW_CREATED; reply.version = WM_PROTO_VERSION; reply.window_id = w->window_id;
        queue_event(conn_index, &reply);
        *dirty = 1;
        break;
    }
    case WM_MSG_DESTROY_WINDOW: {
        window_t* w = find_window_owned_by(conn_index, msg->window_id);
        if (!w) { send_error(conn_index, msg->window_id, WM_ERR_BAD_WINDOW); break; }
        close_window(window_index_of(w));
        *dirty = 1;
        break;
    }
    case WM_MSG_ATTACH_SURFACE: {
        window_t* w = find_window_owned_by(conn_index, msg->window_id);
        if (!w) { send_error(conn_index, msg->window_id, WM_ERR_BAD_WINDOW); break; }
        if (msg->w == 0 || msg->h == 0 || msg->w > 4096 || msg->h > 4096) {
            send_error(conn_index, msg->window_id, WM_ERR_BAD_SIZE); break;
        }
        if (msg->format != WM_FORMAT_XRGB8888 || msg->stride < msg->w * 4) {
            send_error(conn_index, msg->window_id, WM_ERR_BAD_SURFACE); break;
        }
        uint64_t required = (uint64_t)(msg->h - 1) * msg->stride + (uint64_t)msg->w * 4;

        int64_t shm_h = sys_shm_open_token(msg->shm_token);
        if (shm_h < 0) { send_error(conn_index, msg->window_id, WM_ERR_BAD_SURFACE); break; }
        int64_t actual = sys_shm_size((int)shm_h);
        if (actual < 0 || (uint64_t)actual < required) {
            sys_handle_close((int)shm_h);
            send_error(conn_index, msg->window_id, WM_ERR_BAD_SURFACE);
            break;
        }
        uint64_t addr = sys_shm_map((int)shm_h, 0 /* read-only -- the compositor never writes a client's surface */);
        if (addr == (uint64_t)-1) {
            sys_handle_close((int)shm_h);
            send_error(conn_index, msg->window_id, WM_ERR_BAD_SURFACE);
            break;
        }

        free_window_surface(w); // release any previous surface (re-attach case)
        w->shm_handle = (int)shm_h;
        w->surface_addr = addr;
        w->surface_stride = msg->stride;
        w->surface_bytes = (uint64_t)actual;
        w->content_w = msg->w; w->content_h = msg->h;
        w->has_surface = 1;

        send_ack(conn_index, msg->window_id);
        *dirty = 1;
        break;
    }
    case WM_MSG_COMMIT: {
        window_t* w = find_window_owned_by(conn_index, msg->window_id);
        if (!w) { send_error(conn_index, msg->window_id, WM_ERR_BAD_WINDOW); break; }
        *dirty = 1; // Milestone 30: always fully recomposites on any damage -- see file header
        break;
    }
    case WM_MSG_SET_TITLE: {
        window_t* w = find_window_owned_by(conn_index, msg->window_id);
        if (!w) { send_error(conn_index, msg->window_id, WM_ERR_BAD_WINDOW); break; }
        int ti = 0; while (ti < WM_TITLE_MAX && msg->title[ti]) { w->title[ti] = msg->title[ti]; ti++; }
        w->title[ti] = 0;
        *dirty = 1;
        break;
    }
#ifdef COMPOSITOR64_TEST_MODE
    case WM_MSG_TEST_SHUTDOWN:
        // Milestone 32.1: see wmproto64.h's header comment on
        // WM_MSG_TEST_SHUTDOWN -- only a test build reacts to this.
        // sys_exit releases display/input/every handle via the normal
        // process-exit path (kernel/process64.c's close_all_handles),
        // exactly as if this process had exited any other way.
        sys_exit(0);
        break; // unreachable
#endif
    default:
        send_error(conn_index, msg->window_id, WM_ERR_BAD_MESSAGE);
        break;
    }
}

// ── Input event handling ─────────────────────────────────────────────
static void handle_input_event(const input64_event_t* ev, int* dirty) {
    if (ev->type == (uint32_t)INPUT64_EVENT_KEY) {
        if (g_focused < 0) return;
        window_t* w = &g_windows[g_focused];
        wm_msg_t m; for (uint64_t i = 0; i < sizeof(m); i++) ((char*)&m)[i] = 0;
        m.type = WM_MSG_KEY_EVENT; m.version = WM_PROTO_VERSION; m.window_id = w->window_id;
        m.key_code = (uint32_t)ev->a; m.pressed = ev->pressed; m.modifiers = ev->modifiers; m.ascii = ev->ascii;
        queue_event(w->conn_index, &m);
        return;
    }

    if (ev->type == (uint32_t)INPUT64_EVENT_POINTER_MOVE) {
        int32_t nx = g_cursor_x + ev->a, ny = g_cursor_y + ev->b;
        if (nx < 0) nx = 0;
        if (ny < 0) ny = 0;
        if (nx >= (int32_t)g_disp_w) nx = (int32_t)g_disp_w - 1;
        if (ny >= (int32_t)g_disp_h) ny = (int32_t)g_disp_h - 1;
        g_cursor_x = nx; g_cursor_y = ny;
        *dirty = 1;

        if (g_dragging >= 0) {
            g_windows[g_dragging].x = g_cursor_x - g_drag_off_x;
            g_windows[g_dragging].y = g_cursor_y - g_drag_off_y;
            return;
        }
        int idx = hit_test_any(g_cursor_x, g_cursor_y);
        if (idx >= 0 && hit_test_content(&g_windows[idx], g_cursor_x, g_cursor_y)) {
            window_t* w = &g_windows[idx];
            wm_msg_t m; for (uint64_t i = 0; i < sizeof(m); i++) ((char*)&m)[i] = 0;
            m.type = WM_MSG_POINTER_MOTION; m.version = WM_PROTO_VERSION; m.window_id = w->window_id;
            m.x = g_cursor_x - (w->x + BORDER);
            m.y = g_cursor_y - (w->y + BORDER + TITLEBAR_H);
            queue_event(w->conn_index, &m);
        }
        return;
    }

    if (ev->type == (uint32_t)INPUT64_EVENT_POINTER_BUTTON) {
        uint32_t btn = (uint32_t)ev->a;
        *dirty = 1;
        if (ev->pressed) {
            if (g_dragging < 0) {
                int idx = hit_test_any(g_cursor_x, g_cursor_y);
                if (idx >= 0) {
                    raise_window(idx);
                    focus_window(idx);
                    window_t* w = &g_windows[idx];
                    if (hit_test_titlebar(w, g_cursor_x, g_cursor_y)) {
                        if (btn == INPUT64_BTN_MIDDLE) {
                            wm_msg_t m; for (uint64_t i = 0; i < sizeof(m); i++) ((char*)&m)[i] = 0;
                            m.type = WM_MSG_CLOSE_REQUEST; m.version = WM_PROTO_VERSION; m.window_id = w->window_id;
                            queue_event(w->conn_index, &m);
                        } else if (btn == INPUT64_BTN_LEFT) {
                            g_dragging = idx;
                            g_drag_off_x = g_cursor_x - w->x;
                            g_drag_off_y = g_cursor_y - w->y;
                        }
                    } else if (hit_test_content(w, g_cursor_x, g_cursor_y)) {
                        wm_msg_t m; for (uint64_t i = 0; i < sizeof(m); i++) ((char*)&m)[i] = 0;
                        m.type = WM_MSG_POINTER_BUTTON; m.version = WM_PROTO_VERSION; m.window_id = w->window_id;
                        m.button = btn; m.pressed = 1;
                        m.x = g_cursor_x - (w->x + BORDER);
                        m.y = g_cursor_y - (w->y + BORDER + TITLEBAR_H);
                        queue_event(w->conn_index, &m);
                    }
                }
            }
        } else {
            if (g_dragging >= 0) {
                g_dragging = -1;
            } else {
                int idx = hit_test_any(g_cursor_x, g_cursor_y);
                if (idx >= 0 && hit_test_content(&g_windows[idx], g_cursor_x, g_cursor_y)) {
                    window_t* w = &g_windows[idx];
                    wm_msg_t m; for (uint64_t i = 0; i < sizeof(m); i++) ((char*)&m)[i] = 0;
                    m.type = WM_MSG_POINTER_BUTTON; m.version = WM_PROTO_VERSION; m.window_id = w->window_id;
                    m.button = btn; m.pressed = 0;
                    m.x = g_cursor_x - (w->x + BORDER);
                    m.y = g_cursor_y - (w->y + BORDER + TITLEBAR_H);
                    queue_event(w->conn_index, &m);
                }
            }
        }
        return;
    }

    if (ev->type == (uint32_t)INPUT64_EVENT_POINTER_WHEEL) {
        int idx = hit_test_any(g_cursor_x, g_cursor_y);
        if (idx >= 0 && hit_test_content(&g_windows[idx], g_cursor_x, g_cursor_y)) {
            window_t* w = &g_windows[idx];
            wm_msg_t m; for (uint64_t i = 0; i < sizeof(m); i++) ((char*)&m)[i] = 0;
            m.type = WM_MSG_POINTER_WHEEL; m.version = WM_PROTO_VERSION; m.window_id = w->window_id;
            m.x = ev->a;
            queue_event(w->conn_index, &m);
        }
        return;
    }
}

// ── Client bootstrap (service-accept side) ──────────────────────────
// Milestone 32: compositor64 no longer spawns its own clients (or
// knows anything about which programs might connect to it) -- it just
// publishes WM_SERVICE_NAME and accepts whoever shows up, exactly like
// any other named service would. See user64/wmclient64.h's wm_connect()
// for the client side.
static int find_free_conn_slot(void) {
    for (int i = 0; i < MAX_CLIENTS; i++) if (!g_conns[i].in_use) return i;
    return -1;
}

// Non-blocking: called once per main-loop iteration, same multiplexing
// style as every other handle this loop already polls via
// sys_handle_try_read (see this file's own header comment on why --
// ToxenOS has no select()/poll() equivalent). Accepts at most one new
// connection per call; the loop calls this repeatedly so a burst of
// simultaneous connects still drains within a few iterations rather
// than only ever accepting one per frame.
static int accept_one_client(int listen_h) {
    int conn_idx = find_free_conn_slot();
    if (conn_idx < 0) return -1; // no room -- leave it queued, try again once a slot frees up

    service64_endpoints_t ep;
    int64_t r = sys_service_accept(listen_h, &ep);
    if (r < 0) return -1; // WOULDBLOCK or error -- nothing to do this iteration

    conn_t* c = &g_conns[conn_idx];
    c->in_use = 1;
    c->req_r = ep.recv; // reads the client's requests
    c->evt_w = ep.send; // writes events/replies to the client
    c->client_id = 0;
    c->said_hello = 0;
    c->evt_count = 0;
    c->pending_disconnect = 0;
    return conn_idx;
}

// Milestone 32.1: disconnects every connection flagged during this
// iteration's input/request processing (queue_event()'s overflow
// policy, or flush_conn_queue()'s broken-pipe detection). Deferred to
// this single, safe point in the main loop -- see conn_t::
// pending_disconnect's header comment for why nothing tears a
// connection down reentrantly from inside event-generation code.
static void disconnect_stalled_conns(int* dirty) {
    for (int ci = 0; ci < MAX_CLIENTS; ci++) {
        if (g_conns[ci].in_use && g_conns[ci].pending_disconnect) {
            disconnect_conn(ci);
            *dirty = 1;
        }
    }
}

void _start(void) {
    put("compositor64: starting\n");

    uint32_t fmt = 0;
    int64_t dh = sys_display_open(&g_disp_w, &g_disp_h, &fmt);
    if (dh < 0) { put("compositor64: sys_display_open FAILED -- no display available\n"); sys_exit(1); }
    g_display_h = (int)dh;
    put_kv("compositor64: display width=", g_disp_w);
    put_kv("compositor64: display height=", g_disp_h);

    int64_t ih = sys_input_open();
    if (ih < 0) { put("compositor64: sys_input_open FAILED\n"); sys_exit(1); }
    g_input_h = (int)ih;

    uint64_t bb_size = (uint64_t)g_disp_w * g_disp_h * 4;
    uint64_t bb_addr = sys_mmap(bb_size);
    if (bb_addr == (uint64_t)-1) { put("compositor64: backbuffer sys_mmap FAILED\n"); sys_exit(1); }
    g_backbuffer = (uint32_t*)(uintptr_t)bb_addr;

    g_cursor_x = (int32_t)(g_disp_w / 2);
    g_cursor_y = (int32_t)(g_disp_h / 2);

    // Milestone 32: publish the well-known service name instead of
    // spawning any clients ourselves -- an arbitrary independently
    // launched process (init64, shell64, or anything else) discovers
    // and connects to this compositor entirely on its own from here on.
    int64_t listen_h = sys_service_listen(WM_SERVICE_NAME);
    if (listen_h < 0) {
        put("compositor64: sys_service_listen FAILED -- is another compositor already running?\n");
        sys_exit(1);
    }

    put("compositor64: entering main loop\n");
    compose_and_present();

    for (;;) {
        int dirty = 0;

        input64_event_t ev;
        while (sys_handle_try_read(g_input_h, (char*)&ev, sizeof(ev)) == (int64_t)sizeof(ev)) {
            handle_input_event(&ev, &dirty);
        }

        while (accept_one_client((int)listen_h) >= 0) { /* drain any/all pending connects this iteration */ }

        for (int ci = 0; ci < MAX_CLIENTS; ci++) {
            if (!g_conns[ci].in_use) continue;
            for (;;) {
                wm_msg_t msg;
                int64_t n = sys_handle_try_read(g_conns[ci].req_r, (char*)&msg, sizeof(msg));
                if (n == (int64_t)sizeof(msg)) {
                    handle_client_message(ci, &msg, &dirty);
                    continue;
                }
                if (n == 0) { // EOF -- client exited or crashed
                    disconnect_conn(ci);
                    dirty = 1;
                }
                break; // would-block, error, or just handled EOF -- move to next connection
            }
        }

        // Milestone 32.1: tear down anything queue_event()/
        // flush_conn_queue() flagged above BEFORE flushing queues below
        // -- a connection already marked pending_disconnect has nothing
        // further written to or read from its (about to be closed)
        // handles.
        disconnect_stalled_conns(&dirty);

        // Milestone 32.1: deliver whatever is queued, via non-blocking
        // writes only -- this is what guarantees the loop above (input
        // draining, accepting, request processing) can NEVER be blocked
        // by one client's full or stalled event pipe. See queue_event()
        // for the full delivery/coalescing/backpressure design.
        for (int ci = 0; ci < MAX_CLIENTS; ci++) {
            if (g_conns[ci].in_use) flush_conn_queue(ci);
        }

        if (dirty) compose_and_present();
    }
}
