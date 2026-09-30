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
#include "output64.h"
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

// M-next (§17.4): software frame pacing interval, in the existing
// 100Hz scheduler timer's own ticks (kernel/timer64.c, read via the new
// SYS64_GET_TICKS -- see include/syscall64.h). 2 ticks = 20ms = 50Hz:
// the closest rate to "~60Hz" this timer's 10ms granularity can
// actually express as a whole-tick interval -- deliberately not claimed
// to be exact, and deliberately never called "VSync" anywhere in this
// file. See the GPU/display architecture proposal's §09 for why that
// word is reserved for a real display refresh signal, which this
// software timer is not and does not pretend to be.
#define COMPOSITOR_PACING_TICKS 2
// M+4 stale-line investigation: define to FORCE every frame through
// compose_and_present_full() (bypassing compose_and_present_damage()
// entirely) -- a controlled diagnostic to determine whether an
// observed visual artifact is inherent to a client's own rendered
// content or specific to the damage-rect present path. Temporary,
// not left enabled by default.
// #define COMPOSITOR_FORCE_FULL_REDRAW_DIAGNOSTIC 1

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

// M+5: one attached (not necessarily committed) client buffer -- see
// wmproto64.h's own header comment on WM_MSG_ATTACH_BUFFER/
// COMMIT_BUFFER/BUFFER_RELEASED for the full protocol this backs.
// Compositor-side counterpart to the client's own wm_buffer_t
// (wmclient64.h) -- same conceptual fields, independent struct, since
// the two sides never share memory for the bookkeeping itself, only for
// the pixels.
// M+5 is deliberately double-buffered per SIZE, not an arbitrary-depth
// swapchain -- M+9A raised this from 2 to 4 so a fullscreen-capable
// client can hold its normal-size pair AND a separate output-sized pair
// at once (WM_MSG_ATTACH_BUFFER has no "resize" or "detach" concept, and
// window_t.content_w/content_h always comes from whichever buffer was
// actually committed -- see WM_MSG_COMMIT_BUFFER's own comment -- so two
// different target sizes genuinely need two different attached buffers,
// never a reinterpreted/resized single pair). Still not an arbitrary
// swapchain: a client picks which of its own attached buffers to commit
// each frame, exactly as before, just from a slightly larger fixed set.
#define WM_MAX_BUFFERS_PER_WINDOW 4
typedef struct {
    int in_use;
    uint64_t token;                // this buffer's wire identity (== its shm64 token)
    int shm_handle;                 // compositor's own handle
    uint64_t addr;                   // mapped (read-only) in the compositor's own AS
    uint32_t w, h, stride;
    uint64_t bytes;                  // actual mapped size -- bounds every read, exactly like the legacy surface_bytes
} wm_buffer_t;

typedef struct {
    int in_use;
    uint32_t window_id;
    int conn_index;
    int32_t x, y;                 // top-left of the DECORATED window (outside border)
    uint32_t content_w, content_h;
    char title[WM_TITLE_MAX + 1];
    // OWNERSHIP INVARIANT (post-M+12A audit -- see the historical bug this
    // documents in WM_MSG_COMMIT_BUFFER's own handler comment): has_surface
    // means EXACTLY ONE thing -- "a real WM_MSG_ATTACH_SURFACE populated
    // shm_handle/surface_addr/surface_bytes below, and free_window_surface()
    // must release them on close." It is the LEGACY single-surface model's
    // own ownership flag and NOTHING ELSE. It must NEVER be (ab)used as a
    // generic "this window has displayable pixels" flag -- a committed-buffer
    // window is fully represented by uses_committed_buffers/committed_slot
    // below; it has no need for, and must never touch, has_surface.
    int has_surface;
    int shm_handle;                // compositor's own handle
    uint64_t surface_addr;         // mapped (read-only) in the compositor's own AS
    uint32_t surface_stride;
    uint64_t surface_bytes;        // actual mapped size -- bounds every read
    // M+5: committed-buffer state -- coexists with (never mixed with)
    // the legacy has_surface/surface_* fields above. A window uses
    // EXACTLY ONE model, decided by whichever of WM_MSG_ATTACH_SURFACE
    // or WM_MSG_ATTACH_BUFFER its client sends first for it.
    int uses_committed_buffers;
    wm_buffer_t buffers[WM_MAX_BUFFERS_PER_WINDOW];
    int committed_slot;            // index into buffers[] currently displayed, or -1 if nothing committed yet
    // M+6: at most one outstanding WM_MSG_FRAME grant per window -- see
    // WM_MSG_REQUEST_FRAME's own wmproto64.h comment. Only ever set for
    // a window with uses_committed_buffers -- legacy single-surface
    // windows never touch this field. Zeroed for free along with the
    // rest of window_t whenever WM_MSG_CREATE_WINDOW reuses this slot.
    int frame_callback_pending;
    // M+9A: real fullscreen state -- see win_border_off()/win_title_off()
    // below for how this collapses decorations to nothing everywhere a
    // window's own geometry is computed, with no separate per-call-site
    // special case. Distinct from "maximize" (not implemented): fullscreen
    // covers the ENTIRE output, no decorations, nothing rendered above it.
    int is_fullscreen;
    // Restore state, captured the instant fullscreen is entered and
    // applied back the instant it's exited -- content_w/content_h/x/y
    // are overwritten to the full-output geometry while fullscreen is
    // active, so the pre-fullscreen geometry must be preserved
    // separately, not derived from anything still live on the window.
    int32_t saved_x, saved_y;
    uint32_t saved_content_w, saved_content_h;
    // M+9B: index into buffers[] the display hardware is currently (or
    // was, mid-transition) directly scanning out, or -1. While set,
    // notify_buffer_released() must never fire for THIS slot even if a
    // newer commit supersedes it -- see WM_MSG_COMMIT_BUFFER's own
    // handler and direct_scanout_force_leave() for the only two places
    // that clear it, always immediately after confirming hardware no
    // longer references it.
    int scanout_owned_slot;
    // M+10A protocol redesign: this REPLACES the original M+9B scalar
    // `last_scanout_released_slot`, which tracked "the one slot released
    // outside the normal COMMIT_BUFFER path" as a SINGLE value -- but
    // direct-scanout alternation (direct_scanout_try_enter_or_update())
    // and fullscreen exit (direct_scanout_force_leave()) are two
    // INDEPENDENT out-of-band release sites, and either can release a
    // DIFFERENT slot before the other's own release is ever consumed by
    // a matching future commit. A scalar can only remember one of them;
    // the second write silently discards the first, and when that
    // forgotten slot is eventually re-committed as an old_slot, the
    // ordinary COMMIT_BUFFER suppression check (below) no longer
    // recognizes it as already-released and fires a genuine SECOND
    // BUFFER_RELEASED for it. Confirmed via static audit (M+10A protocol
    // report): under the OLD pending[]-as-liveness-state design this
    // duplicate had no consumer and sat stranded forever, one of only
    // WMC_PENDING_MAX slots, until enough accumulated to evict a message
    // a future wait call genuinely needed -- a delayed freeze, not an
    // immediate one. Fixed by tracking release state PER SLOT instead of
    // as one scalar -- see notify_buffer_released()'s own comment, which
    // is now the ONE place this array is read or written.
    int slot_release_ground_truth[WM_MAX_BUFFERS_PER_WINDOW]; // 1 = compositor has already told the client this slot is free since its last commit
    // M+10A: liveness-investigation tracking only -- never read by any
    // presentation/protocol logic, purely diagnostic. Updated whenever a
    // message actually arrives FROM this window's own connection (see
    // touch_window_activity(), below), so "how long since we last heard
    // from this window" is directly observable from the compositor's own
    // side without needing to see into the client's address space.
    uint64_t last_activity_tick;
    uint32_t last_msg_type_received;
    int watchdog_fired; // M+10A §6: latched so the watchdog dumps once per stall, not every main-loop iteration; cleared by touch_window_activity()
    // M+10A: distinguishes a continuously frame-paced client (GfxDemo --
    // calls wm_request_frame() every iteration, so it should NEVER go
    // quiet for long while alive) from a purely reactive one
    // (gfx_interactive64 -- never calls wm_request_frame() at all, per
    // that file's own header comment, and is legitimately silent for as
    // long as nobody touches it). Set once REQUEST_FRAME is ever seen
    // from this window; the watchdog only ever watches windows with this
    // set, so an idle reactive client can never produce a false-positive
    // "stall".
    int ever_requested_frame;
    // M+10A protocol redesign: per-window monotonic CONFIGURE counter --
    // see wmproto64.h's own WM_MSG_CONFIGURE comment. Starts at 0;
    // incremented before every CONFIGURE this window is ever sent, so
    // the first is generation 1. WM_MSG_FRAME is stamped with whatever
    // this currently holds at grant time (see grant_frame_callback()).
    uint32_t configure_generation;
} window_t;

static conn_t g_conns[MAX_CLIENTS];
static window_t g_windows[MAX_WINDOWS];
static int g_zorder[MAX_WINDOWS];
static int g_zorder_count = 0;

// ── Seat64: input focus/grab state ───────────────────────────────────
// Reference audit (Linux drivers/input/input.c, Documentation/input/
// input.rst; wlroots include/wlr/types/wlr_seat.h, types/seat/
// wlr_seat_keyboard.c, tinywl/tinywl.c) before this split:
//
// 1. Linux's own input core carries no window or focus concept at all
//    -- evdev emits raw (type, code, value) events; deciding who a key
//    or click belongs to is entirely a userspace compositor's job.
//    input64/keyboard64 already match this (they push a raw
//    input64_event_t with no window_id), so nothing moves there.
//
// 2. wlroots keeps wlr_seat_pointer_state and wlr_seat_keyboard_state
//    as two SEPARATE structs, each with its own focused_surface/grab.
//    A cursor motion event only ever touches pointer_state; keyboard
//    routing only ever reads keyboard_state.focused_surface. The two
//    are updated by different call sites for different reasons.
//
// g_keyboard_focus below is that keyboard_state.focused_surface
// equivalent -- the ONE authoritative target for KEY_EVENT (see
// handle_input_event's own INPUT64_EVENT_KEY branch, which reads
// nothing else). g_active_window is a separate concept: which window's
// decorations paint focused-color and which one a closing window
// checks itself against -- this file's own compose_window() reads
// g_active_window, never g_keyboard_focus, for border color. Pointer
// hit-testing needs no persisted "pointer_focus" field of its own:
// hit_test_any() is recomputed fresh from g_cursor_x/y on every motion
// AND button event already, which is already the wlroots-equivalent
// behavior of "pointer target follows the surface under the cursor" --
// this file never had a variable conflating that with keyboard focus,
// so nothing changes there. g_dragging (below) already *is* this
// file's pointer grab: while set, POINTER_MOTION moves the dragged
// window instead of being hit-tested/forwarded, exactly like wlroots'
// separate pointer_state.grab -- keeping its existing name rather than
// wrapping it in an unused struct field per this audit's own "do not
// add complexity merely to fill the struct" instruction. ToxenOS has
// no keyboard grab today (no modal dialogs, no drag-and-drop text
// state) -- ordinary click-to-focus is the ONLY thing that ever
// changes g_keyboard_focus, so a keyboard_grab field would have no
// caller; omitted rather than added unused.
static int g_keyboard_focus = -1; // KEY_EVENT routing target ONLY
static int g_active_window = -1;  // decoration/activation state ONLY -- see compose_window()

static int g_dragging = -1;
static int32_t g_drag_off_x = 0, g_drag_off_y = 0;

static uint32_t g_next_window_id = 1;
static uint32_t g_next_client_id = 1;

static int g_input_h = -1; // M+12H: g_display_h retired -- the display handle now lives entirely inside output64.cpp's own Output instance
static uint32_t g_disp_w = 0, g_disp_h = 0;
static uint32_t* g_backbuffer = 0;

// ── M+8: front/presented state ───────────────────────────────────────
// g_backbuffer (above) is the existing, unchanged, mutable COMPOSED/back
// state -- compositor64 keeps rendering into it exactly as before. NEW:
// g_frontbuffer is the same size/format, but is NEVER rendered into
// directly -- it only ever receives an exact copy of a rect AFTER
// present_rect() proves that rect actually reached the visible display.
// It represents "what compositor64 has positive evidence the display
// currently contains," not a second render target and not a swappable
// pointer -- see present_rect()'s own header comment for why a naive
// front/back pointer swap is wrong for this codebase's retained
// partial-damage compositor (the inactive buffer would be stale outside
// whatever was damaged that frame).
static uint32_t* g_frontbuffer = 0;
static int g_front_valid = 0;   // 1 once the first full successful present has established a complete front image
static int g_front_partial = 0; // 1 while g_frontbuffer reflects a mixture of generations (some regions still pending retry after a failed present)

// Bumped once per actual composite pass (a new logical scene state was
// drawn into g_backbuffer); g_presented_generation only ever advances TO
// a composed generation once EVERY region that pass required has
// actually, successfully reached the display -- see present_rect()/
// present_damage()'s own comments. Compositor-local diagnostics only,
// never exposed through any syscall/public ABI.
static uint64_t g_composed_generation = 0;
static uint64_t g_presented_generation = 0;

static int32_t g_cursor_x = 0, g_cursor_y = 0;

// M+7: true once sys_cursor_available()+sys_cursor_set_image() both
// succeeded at startup (see _start()) -- the single flag every cursor
// code path below checks. Never reads back from the kernel repeatedly;
// decided once, at boot, and only ever cleared at runtime by the
// fallback path in handle_input_event() if a later sys_cursor_move()
// call unexpectedly fails (a hardware cursor that worked at startup but
// stops working later, e.g. a hypothetical future device hot-unplug --
// not something VirtIO-GPU actually does today, but the fallback exists
// so a driver-side failure of any kind degrades gracefully rather than
// silently drawing nothing).
static int g_hw_cursor_active = 0;

// M+7A: the active absolute pointer device's own device-space range
// (garbage/unused unless g_have_abs_range is set) -- fetched once at
// startup via sys_input_get_abs_range(), exactly like g_disp_w/h are
// fetched once from sys_display_open(). Used only to normalize
// INPUT64_EVENT_POINTER_ABS's raw (a,b) into logical display
// coordinates in handle_input_event() -- see that function's own
// comment.
static int g_have_abs_range = 0;
static input64_abs_range_t g_abs_range = {0, 0, 0, 0};

// M+9A: the ONE place border/titlebar geometry collapses to zero for a
// fullscreen window -- every other border/titlebar-offset call site in
// this file goes through these two helpers instead of the raw BORDER/
// TITLEBAR_H constants, so a fullscreen window naturally gets "content
// fills the entire window rect, no decorations" everywhere (composition,
// hit-testing, damage, pointer-coordinate translation) with no separate
// per-call-site special case.
static inline int32_t win_border_off(const window_t* w) { return w->is_fullscreen ? 0 : (int32_t)BORDER; }
static inline int32_t win_title_off(const window_t* w) { return w->is_fullscreen ? 0 : (int32_t)TITLEBAR_H; }

static inline uint32_t win_total_w(const window_t* w) { return w->content_w + 2u * (uint32_t)win_border_off(w); }
static inline uint32_t win_total_h(const window_t* w) { return w->content_h + 2u * (uint32_t)win_border_off(w) + (uint32_t)win_title_off(w); }

// ── M+9B: direct scanout / composition bypass ────────────────────────
// See kernel/display64.h's own header comment for the full kernel-side
// state machine this layers on top of. Presented state generalizes
// M+8's own g_frontbuffer/g_presented_generation model (§6): while
// PRESENTED_DIRECT, g_frontbuffer/g_backbuffer are NOT the visible
// output -- they remain the last known COMPOSITED image (never claimed
// current), and the eligible fullscreen window's own committed client
// buffer is authoritative instead. Cached once at startup --
// sys_display_direct_query() never needs re-checking at runtime
// (matches g_hw_cursor_active's own "decided once at boot" precedent).
static int g_direct_supported = 0;

typedef enum { PRESENTED_COMPOSITED = 0, PRESENTED_DIRECT } presented_mode_t;
static presented_mode_t g_presented_mode = PRESENTED_COMPOSITED;
static int g_direct_window_idx = -1; // window_t index currently owning direct scanout, or -1

#define M9B_STATS 1 // cheap uint64_t counters -- always on, unlike COMPOSITOR_PERF_STATS's periodic klog (§18/§20 need these read back, not just logged)
#ifdef M9B_STATS
static uint64_t g_stat_direct_attempts = 0;
static uint64_t g_stat_direct_successes = 0;
static uint64_t g_stat_direct_failures = 0;
static uint64_t g_stat_direct_frames = 0;
static uint64_t g_stat_composed_to_direct = 0;
static uint64_t g_stat_direct_to_composed = 0;
static uint64_t g_stat_direct_transfer_bytes = 0;
static uint64_t g_stat_compositor_bytes_while_direct = 0;
static uint64_t g_stat_front_mirror_bytes_while_direct = 0;
static uint64_t g_stat_deferred_scanout_releases = 0;
static uint64_t g_stat_completed_scanout_releases = 0;
#endif

// Extremely conservative eligibility (§3) -- ALL must hold, checked
// fresh on every commit (never cached beyond this one call):
static int direct_scanout_eligible(window_t* w, int idx) {
    if (!g_direct_supported) {
        return 0;
    }
    if (!w->is_fullscreen) {
        return 0;
    }
    if (!w->uses_committed_buffers || w->committed_slot < 0) {
        return 0;
    }
    if (w->content_w != g_disp_w || w->content_h != g_disp_h) {
        return 0;
    }
    if (g_zorder_count == 0 || g_zorder[g_zorder_count - 1] != idx) {
        return 0;
    }
    if (!g_hw_cursor_active) {
        return 0;
    }
    return 1;
}

#ifdef M9B_STATS
// §18: every counter this milestone's own debug-counter list asks for.
// compositor_bytes_while_direct/front_mirror_bytes_while_direct are
// deliberately never incremented anywhere -- the main loop's own pacing
// block (in _start()) skips the entire normal composite/present
// pipeline outright while PRESENTED_DIRECT (§15), so those two are
// PROVABLY zero by construction, not merely observed to be zero; this
// dump reports them as the fixed 0 they can only ever be, exactly like
// §20's own "expected direct steady state" says they must. Called once
// at the end of every direct_scanout_force_leave() -- a natural, low-
// frequency point to see a completed direct-mode session's totals.
static void m9b_dump_stats(void) {
    put("M9B stats:\n");
    put_kv("  direct_scanout_attempts=", g_stat_direct_attempts);
    put_kv("  direct_scanout_successes=", g_stat_direct_successes);
    put_kv("  direct_scanout_failures=", g_stat_direct_failures);
    put_kv("  direct_scanout_frames=", g_stat_direct_frames);
    put_kv("  composed_to_direct=", g_stat_composed_to_direct);
    put_kv("  direct_to_composed=", g_stat_direct_to_composed);
    put_kv("  direct_transfer_bytes=", g_stat_direct_transfer_bytes);
    put_kv("  compositor_bytes_while_direct=", g_stat_compositor_bytes_while_direct);   // provably 0 -- see this function's own comment
    put_kv("  front_mirror_bytes_while_direct=", g_stat_front_mirror_bytes_while_direct); // provably 0 -- see this function's own comment
    put_kv("  deferred_scanout_releases=", g_stat_deferred_scanout_releases);
    put_kv("  completed_scanout_releases=", g_stat_completed_scanout_releases);
}
#endif

// ── M-next: damage tracking (§17 of the GPU/display architecture
// proposal) ──────────────────────────────────────────────────────────
// Replaces Milestone 30's single global `dirty` boolean (which forced
// a full-screen recompose+present on EVERY window commit, EVERY mouse
// packet, and EVERY z-order/focus change) with a small, bounded list
// of screen-space damage rectangles. Deliberately NOT a real region
// library (no scanline trees, no set subtraction) -- just a fixed
// array with simple pairwise overlap coalescing, matching the
// milestone's own explicit "small, simple... not a giant Wayland-style
// region library" guidance. `g_full_redraw` is the safe fallback this
// milestone equally insists on: any case that's awkward to reason
// about precisely (the damage list overflowing, a client disconnect
// tearing down several windows at once) just sets this flag and lets
// the existing, already-correct full-screen path handle it, rather
// than risking an incorrect "clever" partial redraw.
//
// Rects are stored half-open ([x0,x1) x [y0,y1)) rather than (x,y,w,h)
// -- every operation this file actually needs (clip, overlap test,
// union) is simpler in that form; conversion to (x,y,w,h) happens only
// at the two points that need it (bb_fill_rect, sys_display_present).
typedef struct { int32_t x0, y0, x1, y1; } rect_t;

#define MAX_DAMAGE_RECTS 24
static rect_t g_damage[MAX_DAMAGE_RECTS];
static int g_damage_count = 0;
// M+4 second stale-line investigation: counts how many times
// add_damage_rect() has hit the MAX_DAMAGE_RECTS cap and fallen back to
// a full redraw, since boot -- always tracked (one integer increment on
// an already-rare path costs nothing) so this can be checked without a
// separate debug flag. Overflow itself is NOT a coverage bug (the
// fallback always covers strictly more than whatever was being
// tracked), but a nonzero count under normal interactive use would mean
// MAX_DAMAGE_RECTS is being hit far more often than intended.
static uint64_t g_damage_overflow_count = 0;
// Starts true: the very first frame has nothing meaningful to diff
// against yet, so it must be a full draw -- exactly what Milestone 30's
// own code already did unconditionally right after "entering main loop".
static int g_full_redraw = 1;

// M-next: define to have the compositor periodically klog frames-
// presented / sys_display_present-calls-made / approximate pixel area
// presented (once per second, off the same tick source frame pacing
// uses) -- temporary measurement instrumentation for comparing this
// milestone's before/after behavior under idle/move/drag, not left
// enabled by default. Matches this codebase's existing convention of
// gating development-only diagnostics behind an explicit #define
// (kernel/kernel64.c's *_RUN_TESTS flags) rather than always-on logging.
// Defined here (rather than down by report_perf_stats_if_due(), which
// used to be this flag's only consumer) because M+4's investigation
// added a counter add_damage_rect() itself increments, well above that
// point in the file.
// #define COMPOSITOR_PERF_STATS 1

#ifdef COMPOSITOR_PERF_STATS
// M+4 investigation: declared here (ahead of add_damage_rect() below,
// which increments the first one directly) rather than down by
// report_perf_stats_if_due() -- see that function's own comment on why
// this breakdown exists.
static uint64_t g_stat_total_damage_adds = 0;
static uint64_t g_stat_cursor_damage_adds = 0;
static uint64_t g_stat_drag_damage_adds = 0;
// M+4 size-dependent-lag investigation: declared here (ahead of
// compose_window() below, which increments the first one directly) --
// see report_perf_stats_if_due()'s own comment on why these exist.
static uint64_t g_stat_composited_pixels = 0;
static uint64_t g_stat_windows_touched = 0;
// M+6: declared here (ahead of notify_buffer_released()/
// grant_frame_callback()/the WM_MSG_COMMIT_BUFFER handler below, all of
// which increment one of these directly) rather than down by
// report_perf_stats_if_due() -- see that function's own comment on why
// this breakdown exists.
static uint64_t g_stat_frame_requests = 0;
static uint64_t g_stat_frame_callbacks_sent = 0;
static uint64_t g_stat_commit_buffer = 0;
static uint64_t g_stat_buffer_releases = 0;
#endif

static inline int rect_valid(rect_t r) { return r.x1 > r.x0 && r.y1 > r.y0; }

static rect_t rect_clip_to_display(rect_t r) {
    if (r.x0 < 0) r.x0 = 0;
    if (r.y0 < 0) r.y0 = 0;
    if (r.x1 > (int32_t)g_disp_w) r.x1 = (int32_t)g_disp_w;
    if (r.y1 > (int32_t)g_disp_h) r.y1 = (int32_t)g_disp_h;
    return r;
}

// Overlap-or-touching (inclusive edges) -- used only to decide whether
// two damage rects are worth merging into one; a true "adjacent but not
// touching" test isn't needed here (see the coalescing comment below).
static inline int rect_overlaps_or_touches(rect_t a, rect_t b) {
    return a.x0 <= b.x1 && b.x0 <= a.x1 && a.y0 <= b.y1 && b.y0 <= a.y1;
}
// Strict overlap -- used to decide whether a WINDOW needs redrawing for
// a given damage rect (a window merely touching the rect's edge has no
// actual pixels inside it that need repainting).
static inline int rect_intersects(rect_t a, rect_t b) {
    return a.x0 < b.x1 && b.x0 < a.x1 && a.y0 < b.y1 && b.y0 < a.y1;
}
// M+4 clipped-composition fix: the actual overlap rect (may be
// invalid/empty per rect_valid() -- caller must check before using).
static inline rect_t rect_intersect(rect_t a, rect_t b) {
    rect_t r;
    r.x0 = a.x0 > b.x0 ? a.x0 : b.x0;
    r.y0 = a.y0 > b.y0 ? a.y0 : b.y0;
    r.x1 = a.x1 < b.x1 ? a.x1 : b.x1;
    r.y1 = a.y1 < b.y1 ? a.y1 : b.y1;
    return r;
}
static inline uint64_t rect_area(rect_t r) {
    if (!rect_valid(r)) return 0;
    return (uint64_t)(r.x1 - r.x0) * (uint64_t)(r.y1 - r.y0);
}

static inline rect_t rect_union(rect_t a, rect_t b) {
    rect_t r;
    r.x0 = a.x0 < b.x0 ? a.x0 : b.x0;
    r.y0 = a.y0 < b.y0 ? a.y0 : b.y0;
    r.x1 = a.x1 > b.x1 ? a.x1 : b.x1;
    r.y1 = a.y1 > b.y1 ? a.y1 : b.y1;
    return r;
}

static void mark_full_redraw(void) {
    g_full_redraw = 1;
    g_damage_count = 0; // the individual rects are moot once a full redraw is already guaranteed
}

// Adds `r` (already in SCREEN coordinates) to the pending damage set.
// Safe to call with a rect that's partially or fully off-screen, empty,
// or in an unexpected order -- always clips and validates first.
static void add_damage_rect(rect_t r) {
#ifdef COMPOSITOR_PERF_STATS
    g_stat_total_damage_adds++;
#endif
    if (g_full_redraw) return; // already doing a full redraw this cycle -- nothing more to track
    r = rect_clip_to_display(r);
    if (!rect_valid(r)) return; // fully off-screen or zero-size after clipping

    // Coalesce into the first existing rect that overlaps or touches --
    // keeps the list small for the two cases that matter most (a small
    // cursor stepping a few pixels at a time; a dragged window's
    // incremental moves), without a real region library. Not maximal
    // (a later rect that would now ALSO overlap the just-grown one
    // isn't re-checked against everything again) -- fine, since §17's
    // own guidance is "coalesce where useful", not "always produce the
    // provably minimal rect set".
    for (int i = 0; i < g_damage_count; i++) {
        if (rect_overlaps_or_touches(g_damage[i], r)) {
            g_damage[i] = rect_union(g_damage[i], r);
            return;
        }
    }

    if (g_damage_count >= MAX_DAMAGE_RECTS) {
        // Too many genuinely disjoint damaged regions to track
        // individually this frame -- the conservative fallback §17
        // explicitly calls for, not a bug: falling back to one full
        // redraw is always correct, just not maximally efficient.
        g_damage_overflow_count++;
        mark_full_redraw();
        return;
    }
    g_damage[g_damage_count++] = r;
}

static inline void add_damage_xywh(int32_t x, int32_t y, uint32_t w, uint32_t h) {
    rect_t r = { x, y, x + (int32_t)w, y + (int32_t)h };
    add_damage_rect(r);
}

static inline rect_t window_rect(const window_t* w) {
    rect_t r = { w->x, w->y, w->x + (int32_t)win_total_w(w), w->y + (int32_t)win_total_h(w) };
    return r;
}

static inline void add_damage_window(const window_t* w) {
    add_damage_rect(window_rect(w));
}

// Shared by WM_MSG_COMMIT and WM_MSG_COMMIT_BUFFER (M+5) -- both name a
// damage rect in the SAME content-local coordinate space with the SAME
// validation needs. (x,y,rw,rh) is the client's own claimed rect, never
// trusted blindly: a malicious or buggy client could send negative
// coordinates, a huge w/h that overflows, or a rect left over from a
// since-shrunk surface. int64_t intermediates avoid 32-bit wraparound
// the same way userlib/toxui/tox_draw.c's own clip_rect() does;
// clamping against content_w/content_h is "clip to the client surface"
// AND "clip to the window bounds" at once, since for a compositor
// client the two are the same rectangle. Clipping to the DISPLAY
// happens separately, inside add_damage_rect(), after translating to
// screen space here. A rect that clips away to nothing is silently a
// no-op, not an error.
static void damage_content_rect(const window_t* w, int32_t x, int32_t y, uint32_t rw, uint32_t rh) {
    int64_t cx0 = x, cy0 = y;
    int64_t cx1 = (int64_t)x + rw;
    int64_t cy1 = (int64_t)y + rh;
    if (cx0 < 0) cx0 = 0;
    if (cy0 < 0) cy0 = 0;
    if (cx1 > (int64_t)w->content_w) cx1 = w->content_w;
    if (cy1 > (int64_t)w->content_h) cy1 = w->content_h;
    if (cx1 > cx0 && cy1 > cy0) {
        rect_t r;
        r.x0 = w->x + win_border_off(w) + (int32_t)cx0;
        r.y0 = w->y + win_border_off(w) + win_title_off(w) + (int32_t)cy0;
        r.x1 = w->x + win_border_off(w) + (int32_t)cx1;
        r.y1 = w->y + win_border_off(w) + win_title_off(w) + (int32_t)cy1;
        add_damage_rect(r);
    }
}

// Small filled triangle, matching bb_draw_cursor's own drawn shape and
// size exactly -- kept as one named constant pair so the "old position"
// and "new position" damage rects this file adds on every pointer move
// are guaranteed to cover exactly what bb_draw_cursor will actually
// touch, never more or less by accident.
#define CURSOR_W 12
#define CURSOR_H 12
static inline rect_t cursor_rect_at(int32_t x, int32_t y) {
    rect_t r = { x, y, x + CURSOR_W, y + CURSOR_H };
    return r;
}

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

// M+9B: forward-declared -- real definition lives much further down
// (after compose_and_present_full(), which it calls); raise_window()
// below is one of several exit-condition call sites that need it ahead
// of that point in the file, same "forward-declare across a large file"
// precedent kernel/display64.c's own write_native() already set.
static void direct_scanout_force_leave(void);

static void raise_window(int idx) {
    // M+9B §11: another window becoming visible above the direct-
    // scanned one ends direct mode -- this is the ONE place ANY window
    // (click-to-focus, WM_MSG_SET_FULLSCREEN's own entry, window
    // creation) is ever raised to the top, so it is the correct single
    // choke point for this exit condition rather than duplicating the
    // check at every raise_window() call site.
    if (g_presented_mode == PRESENTED_DIRECT && idx != g_direct_window_idx) direct_scanout_force_leave();
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

// set_keyboard_focus_ex(): the ONE authoritative keyboard-routing
// transition, per this file's own seat64 audit above -- the wlroots
// equivalent of wlr_seat_keyboard_enter() (old surface leaves, new
// surface enters, exactly one seat-wide focused_surface at a time).
// This function ONLY touches g_keyboard_focus and WM_MSG_FOCUS
// (semantics: keyboard focus gained/lost -- see wmproto64.h's own
// comment). It deliberately does NOT touch decorations/damage or
// g_active_window -- that's activate_window()'s separate job, called
// alongside this one everywhere ToxenOS currently wants both (which is
// everywhere today; nothing yet needs them to diverge, but routing and
// decoration are no longer the same variable, so they CAN diverge
// later without another refactor).
//
// `notify_new` controls whether the NEWLY-focused window's own
// connection is told about it. This must be 0 when called from
// WM_MSG_CREATE_WINDOW's handler: that handler still owes this exact
// connection its WINDOW_CREATED reply, sent right after this call
// returns, on the SAME event pipe -- an unsolicited FOCUS message
// injected first would race ahead of it, and the client library's
// wm_create_window() (which just reads the next message and expects it
// to be WINDOW_CREATED) would misinterpret the FOCUS message as its
// reply and fail. A brand-new window can safely assume it starts
// focused without an explicit notification; every OTHER focus
// transition (click-to-focus, losing focus to another window) calls
// this with notify_new=1 since no reply is pending on that connection
// at that moment.
static void set_keyboard_focus_ex(int idx, int notify_new) {
    if (g_keyboard_focus == idx) return;
    if (g_keyboard_focus >= 0 && g_windows[g_keyboard_focus].in_use) {
        wm_msg_t m; for (uint64_t i = 0; i < sizeof(m); i++) ((char*)&m)[i] = 0;
        m.type = WM_MSG_FOCUS; m.version = WM_PROTO_VERSION;
        m.window_id = g_windows[g_keyboard_focus].window_id; m.pressed = 0;
        queue_event(g_windows[g_keyboard_focus].conn_index, &m);
    }
    g_keyboard_focus = idx;
    if (idx >= 0) {
        if (notify_new) {
            wm_msg_t m; for (uint64_t i = 0; i < sizeof(m); i++) ((char*)&m)[i] = 0;
            m.type = WM_MSG_FOCUS; m.version = WM_PROTO_VERSION;
            m.window_id = g_windows[idx].window_id; m.pressed = 1;
            queue_event(g_windows[idx].conn_index, &m);
        }
    }
}
static void set_keyboard_focus(int idx) { set_keyboard_focus_ex(idx, 1); }

// activate_window(): the separate visual/decoration half of what used
// to be one focus_window_ex() call. Touches ONLY g_active_window and
// the damage needed to repaint the losing/gaining window's border
// (compose_window()'s BORDER_COLOR_FOCUSED/UNFOCUSED reads
// g_active_window, never g_keyboard_focus). Sends no protocol message
// -- clients already learn about activation-adjacent state changes via
// WM_MSG_FOCUS from set_keyboard_focus(), and per this file's own seat
// audit there is no current ToxenOS feature (no taskbar, no separate
// "active but not focused" window) that needs a second client-visible
// notification for this.
static void activate_window(int idx) {
    if (g_active_window == idx) return;
    if (g_active_window >= 0 && g_windows[g_active_window].in_use) {
        add_damage_window(&g_windows[g_active_window]);
    }
    g_active_window = idx;
    if (idx >= 0) add_damage_window(&g_windows[idx]);
}

// Releases the LEGACY WM_MSG_ATTACH_SURFACE surface only -- has_surface's
// only legitimate meaning (see window_t's own field comment). Never called
// for, and a correct no-op on, a pure committed-buffer window: has_surface
// stays false for its whole life, so shm_handle/surface_addr/surface_bytes
// (still their window-creation zero defaults) are never touched here.
static void free_window_surface(window_t* w) {
    if (!w->has_surface) return;
    sys_munmap(w->surface_addr, w->surface_bytes);
    sys_handle_close(w->shm_handle);
    w->has_surface = 0;
}

// M+6 targeted diagnostic (temporary, not a permanent feature): verifies
// the intended ordering commit N -> present/consume N -> FRAME(for N+1)
// actually holds, given that presentation pacing (last_frame_tick, main
// loop) and frame-callback pacing (last_callback_tick, main loop) are
// two INDEPENDENT elapsed-time gates on two independent clocks -- by
// design, so wiring in callbacks never changes existing presentation
// latency (see dispatch_frame_callbacks()'s own comment) -- but that
// independence is exactly what could let a callback for N+1 be granted
// before N was ever actually composited, if the two clocks ever drift
// out of phase (e.g. an unrelated window's damage causing an "extra"
// present that resets last_frame_tick without a matching reset of
// last_callback_tick). Logs three events with monotonically increasing
// per-window sequence numbers so they can be correlated after the fact:
// COMMIT (a new buffer became committed_slot), PRESENT (the FIRST
// compose_window() call that actually reads a given commit's pixels),
// and FRAME_GRANT (a callback was sent, with the commit/presented seq
// pair AT THAT INSTANT attached so a mismatch is directly visible in
// the log without needing to hand-correlate three separate counters).
// Parallel arrays keyed by window slot index, exactly like
// COMPOSITOR_DAMAGE_TRACE's own cursor/drag trackers (further below) --
// nothing here touches window_t's own layout.
// #define M6_FRAME_TRACE 1

#ifdef M6_FRAME_TRACE
static uint64_t g_trace_commit_seq[MAX_WINDOWS];
static uint64_t g_trace_presented_seq[MAX_WINDOWS];
#endif

// M+5: notifies the client that a buffer it previously committed has
// been retired by a NEWER commit and may now be drawn into again --
// WM_MSG_COMMIT_BUFFER's own retire path (below) calls this, and ONLY
// this: attaching a buffer (WM_MSG_ATTACH_BUFFER) is a one-time setup
// the client does once per buffer and never repeats, so a retired
// buffer must stay attached (mapped, in_use) forever, ready for the
// client's NEXT commit of that same buffer -- this is the whole point
// of double buffering being an alternation, not a one-shot handoff.
// Does NOT touch buffers[slot].in_use/addr/handle -- only
// free_window_buffers() (window/connection teardown, below) ever does
// that, since only real teardown actually revokes the attachment.
// M+10A protocol redesign (item 3: buffer ownership per slot,
// authoritative, idempotent): `slot_release_ground_truth[]` is now the
// ONE authoritative record of "has this slot been told BUFFER_RELEASED
// since its last commit" -- this function is the ONLY place that ever
// reads or writes it, which is what makes it authoritative (the old
// design had this same fact tracked THREE ways -- committed_slot,
// scanout_owned_slot, and the single-scalar last_scanout_released_slot
// -- and the scalar version is what produced the real duplicate-release
// bug the M+10A audit found). A caller asking to release an
// already-released slot is a genuine protocol-logic bug (should be
// structurally impossible after this redesign, since every out-of-band
// release path now goes through this same idempotent check) -- counted
// and reported once, but never allowed to reach the wire: there is no
// consumer for a second release of an already-free buffer, and under
// the new per-token client-side ownership model it would be a no-op at
// best, so it simply never gets sent.
static uint64_t g_diag_duplicate_release_count = 0;
static int g_diag_duplicate_release_reported = 0;
// M+10A protocol redesign diagnostics (item 9) -- see COMMIT_BUFFER's own
// generation check and grant_frame_callback()'s own stale-permission note.
static uint64_t g_diag_stale_commit_generation_count = 0;
static int g_diag_stale_commit_generation_reported = 0;
static uint64_t g_diag_impossible_commit_generation_count = 0;
static int g_diag_impossible_commit_generation_reported = 0;
// Post-M+12A audit (the has_surface/display-handle bug): never-reset count of
// every sys_display_present() failure not caused by M8_FAULT_INJECT -- see
// present_rect()'s own comment. Must stay exactly 0 for an ordinary session.
static uint64_t g_diag_present_failure_count = 0;
// M+12B: never-reset count of times the pre-WAIT_ANY invariant check
// found an in-use connection still pending_disconnect immediately
// before what would otherwise have been a blocking sys_handle_wait_any
// call -- see the main loop's own tail comment on the two-sweep
// disconnect_stalled_conns() model for why this is proven structurally
// impossible under the current call graph (every event a teardown can
// itself produce targets the connection ALREADY being torn down, never
// a different surviving one). A pure defensive backstop: must stay
// exactly 0 through an ordinary session -- its staying 0 is itself part
// of this milestone's own manual-acceptance evidence, not just a
// theoretical argument.
// __attribute__((unused)): unused under a COMPOSITOR_LEGACY_POLL_MODE
// build, which never reaches the M+12B tail at all -- same precedent as
// M+12A's own g_default_policy under COMPOSITOR_POLICY_TEST_ALT.
static uint64_t g_diag_pending_disconnect_at_wait_count __attribute__((unused)) = 0;
static int g_diag_pending_disconnect_at_wait_reported __attribute__((unused)) = 0;

static void notify_buffer_released(window_t* w, int slot) {
    if (!w->buffers[slot].in_use) return;
    if (w->slot_release_ground_truth[slot]) {
        g_diag_duplicate_release_count++;
        if (!g_diag_duplicate_release_reported) {
            g_diag_duplicate_release_reported = 1;
            put("*** M10A: notify_buffer_released() asked to release an already-free slot (suppressed) ***\n");
            put_kv("  window_id=", w->window_id);
            put_kv("  slot=", (uint64_t)slot);
            put_kv("  committed_slot=", (uint64_t)(w->committed_slot < 0 ? 0xFFFFFFFFu : (uint32_t)w->committed_slot));
            put_kv("  scanout_owned_slot=", (uint64_t)(w->scanout_owned_slot < 0 ? 0xFFFFFFFFu : (uint32_t)w->scanout_owned_slot));
        }
        return; // never send a duplicate -- see this function's own header comment
    }
    w->slot_release_ground_truth[slot] = 1;

    wm_msg_t m; for (uint64_t i = 0; i < sizeof(m); i++) ((char*)&m)[i] = 0;
    m.type = WM_MSG_BUFFER_RELEASED;
    m.version = WM_PROTO_VERSION;
    m.window_id = w->window_id;
    m.shm_token = w->buffers[slot].token;
    queue_event(w->conn_index, &m);
#ifdef COMPOSITOR_PERF_STATS
    g_stat_buffer_releases++;
#endif
}

// M+6: grants one pending frame-callback request -- sends WM_MSG_FRAME
// and clears frame_callback_pending. Only ever called by
// dispatch_frame_callbacks() below, which already checked w->in_use, so
// this never sends into a window that's mid-teardown or already closed
// (a closed window's slot has in_use=0 immediately, well before its
// memory is ever reused by a future WM_MSG_CREATE_WINDOW).
static void grant_frame_callback(window_t* w) {
    wm_msg_t m; for (uint64_t i = 0; i < sizeof(m); i++) ((char*)&m)[i] = 0;
    m.type = WM_MSG_FRAME;
    m.version = WM_PROTO_VERSION;
    m.window_id = w->window_id;
    // M+10A protocol redesign: stamp with the AUTHORITATIVE current
    // generation at the instant of granting, not whatever the client
    // believed when it sent REQUEST_FRAME -- see wmproto64.h's own
    // WM_MSG_FRAME comment. This is what lets the client recognize a
    // grant that was already outstanding before a fullscreen/configure
    // transition as stale relative to its own newer state.
    m.generation = w->configure_generation;
    queue_event(w->conn_index, &m);
    w->frame_callback_pending = 0;
#ifdef COMPOSITOR_PERF_STATS
    g_stat_frame_callbacks_sent++;
#endif
#ifdef M6_FRAME_TRACE
    {
        int idx = window_index_of(w);
        put("M6TRACE FRAME_GRANT\n");
        put_kv("  win=", w->window_id);
        put_kv("  after_commit_seq=", g_trace_commit_seq[idx]);
        put_kv("  presented_seq=", g_trace_presented_seq[idx]);
        put_kv("  tick=", sys_get_ticks());
        if (g_trace_presented_seq[idx] != g_trace_commit_seq[idx]) {
            put("M6TRACE *** MISMATCH: callback granted before latest commit was presented ***\n");
        }
    }
#endif
}

// M+6: dispatches every outstanding frame-callback request. Called from
// the main loop on its OWN elapsed-time gate (COMPOSITOR_PACING_TICKS,
// same interval M-next's presentation pacing already uses) -- a
// deliberately SEPARATE gate from presentation pacing's own
// last_frame_tick, not reusing it, for two reasons: a window requesting
// frames during a lull with no damage to present still gets granted on
// schedule instead of waiting for unrelated damage to show up first, and
// wiring this in never changes the EXISTING presentation-pacing latency
// behavior every other window already depends on (input-driven damage
// still presents as soon as it arrives, exactly as before this
// milestone). This is what keeps a fast client's own
// REQUEST_FRAME -> FRAME -> REQUEST_FRAME sequence from ever becoming a
// new busy loop: no matter how fast the client re-requests, callbacks
// are never granted faster than once per pacing interval per window.
//
// No occlusion/visibility check: nothing in this codebase tracks window
// minimization or occlusion today, so there is nothing "trivial" to
// hook into here. Documented as future policy, not built speculatively
// -- see Revision 3 §22 item 5's own note.
static void dispatch_frame_callbacks(void) {
    for (int i = 0; i < MAX_WINDOWS; i++) {
        window_t* w = &g_windows[i];
        if (w->in_use && w->uses_committed_buffers && w->frame_callback_pending) {
            grant_frame_callback(w);
        }
    }
}

// M+10A §2/§3/§5: comprehensive per-window liveness dump -- this is the
// compositor-side half of the freeze investigation (GfxDemo's own
// gfx_demo64.c/wmclient64.h side already dumps its OWN believed state;
// cross-referencing the two is how the "first missing stage" gets found,
// per the milestone's own §9 instruction). Never mutates anything --
// purely diagnostic reads, same discipline as the M+9/M+10 fault-
// injection self-tests' own read-only assertions.
static const char* msg_type_name(uint32_t t) {
    switch (t) {
    case WM_MSG_HELLO: return "HELLO";
    case WM_MSG_CREATE_WINDOW: return "CREATE_WINDOW";
    case WM_MSG_DESTROY_WINDOW: return "DESTROY_WINDOW";
    case WM_MSG_ATTACH_SURFACE: return "ATTACH_SURFACE";
    case WM_MSG_COMMIT: return "COMMIT";
    case WM_MSG_SET_TITLE: return "SET_TITLE";
    case WM_MSG_ATTACH_BUFFER: return "ATTACH_BUFFER";
    case WM_MSG_COMMIT_BUFFER: return "COMMIT_BUFFER";
    case WM_MSG_REQUEST_FRAME: return "REQUEST_FRAME";
    case WM_MSG_SET_FULLSCREEN: return "SET_FULLSCREEN";
    case WM_MSG_TEST_SHUTDOWN: return "TEST_SHUTDOWN";
    default: return "?";
    }
}

static void put_kvi(const char* label, int v) {
    if (v < 0) { put(label); put("-1\n"); return; }
    put_kv(label, (uint64_t)v);
}

static void put_slot_kv(int slot, const char* field, uint64_t v) {
    char line[80]; int pos = 0;
    const char* p = "  slot["; while (*p) line[pos++] = *p++;
    append_u64(line, &pos, (uint64_t)slot);
    line[pos++] = ']'; line[pos++] = ' ';
    p = field; while (*p) line[pos++] = *p++;
    append_u64(line, &pos, v);
    line[pos++] = '\n'; line[pos] = 0;
    put(line);
}

static void dump_window_liveness(window_t* w, const char* tag) {
    int idx = window_index_of(w);
    put("=== M10A compositor liveness dump ["); put(tag); put("] ===\n");
    put_kv("  window_id=", w->window_id);
    put_kvi("  window_idx=", idx);
    put_kv("  is_fullscreen=", (uint64_t)w->is_fullscreen);
    put_kv("  uses_committed_buffers=", (uint64_t)w->uses_committed_buffers);
    put_kv("  frame_callback_pending=", (uint64_t)w->frame_callback_pending);
    put_kvi("  committed_slot=", w->committed_slot);
    put_kvi("  scanout_owned_slot=", w->scanout_owned_slot);
    put_kv("  configure_generation=", (uint64_t)w->configure_generation);
    if (w->conn_index >= 0 && g_conns[w->conn_index].in_use) {
        put_kv("  conn_evt_count=", (uint64_t)g_conns[w->conn_index].evt_count);
        put_kv("  conn_pending_disconnect=", (uint64_t)g_conns[w->conn_index].pending_disconnect);
    } else {
        put("  conn_evt_count=N/A (connection gone)\n");
    }
    put("  presented_mode="); put(g_presented_mode == PRESENTED_DIRECT ? "DIRECT" : "COMPOSITED"); put("\n");
    put_kvi("  direct_window_idx=", g_direct_window_idx);
    put_kv("  g_full_redraw=", (uint64_t)g_full_redraw);
    put_kv("  g_damage_count=", (uint64_t)g_damage_count);

    uint64_t now = sys_get_ticks();
    put_kv("  last_activity_tick=", w->last_activity_tick);
    put_kv("  ticks_since_activity=", now - w->last_activity_tick);
    put("  last_msg_type_received="); put(msg_type_name(w->last_msg_type_received)); put("\n");

    for (int i = 0; i < WM_MAX_BUFFERS_PER_WINDOW; i++) {
        put_slot_kv(i, "in_use=", (uint64_t)w->buffers[i].in_use);
        put_slot_kv(i, "is_committed_slot=", (uint64_t)(i == w->committed_slot));
        put_slot_kv(i, "is_scanout_owned_slot=", (uint64_t)(i == w->scanout_owned_slot));
        put_slot_kv(i, "release_ground_truth=", (uint64_t)w->slot_release_ground_truth[i]);
    }

    output64_debug_state_t dbg;
    if (output64_debug_state(&dbg) == 0) {
        put("  display64_validity="); put(dbg.validity == 0 ? "VALID" : "RECOVERY_REQUIRED"); put("\n");
        put_kv("  display64_primary_kind=", (uint64_t)dbg.primary_kind);
        put_kv("  display64_primary_identity=", dbg.primary_identity);
    } else {
        put("  display64_debug_state query FAILED\n");
    }
    put("=== end dump ===\n");
}

// M+10A §6: DEBUG-ONLY watchdog. Diagnostic only -- never fixes, retries,
// or restarts anything; the frozen state is the evidence, so nothing
// here may disturb it. Fires once per stall (latched via
// window_t.watchdog_fired, cleared the moment real activity resumes via
// touch_window_activity()) rather than flooding the log every iteration
// this condition remains true.
#define M10A_WATCHDOG_TICKS 200 // ~2s at the kernel's fixed 100Hz tick rate (kernel/timer64.c)
static void check_liveness_watchdog(void) {
    uint64_t now = sys_get_ticks();
    for (int i = 0; i < MAX_WINDOWS; i++) {
        window_t* w = &g_windows[i];
        if (!w->in_use || !w->uses_committed_buffers || !w->ever_requested_frame || w->watchdog_fired) continue;
        if (now - w->last_activity_tick >= M10A_WATCHDOG_TICKS) {
            w->watchdog_fired = 1;
            put("*** M10A WATCHDOG: window has sent no message in >=2s while still connected ***\n");
            dump_window_liveness(w, "watchdog");
        }
    }
}

// M+5: actually revokes one attached committed-buffer slot -- unmaps +
// closes the compositor's own reference (mirrors free_window_surface()'s
// own pair of calls exactly). Only ever called at window/connection
// teardown (free_window_buffers(), below), never to retire a buffer
// between commits during normal operation -- see
// notify_buffer_released()'s own comment on why those are two genuinely
// different operations. `notify` is always 0 from that one call site
// (the client is going away or already gone -- nothing to notify, same
// precedent free_window_surface() itself already sets by never sending
// anything); kept as a parameter only so a future real teardown-time
// notification need, if one ever arises, doesn't require re-deriving
// this.
static void release_buffer(window_t* w, int slot, int notify) {
    if (!w->buffers[slot].in_use) return;
    uint64_t token = w->buffers[slot].token;
    sys_munmap(w->buffers[slot].addr, w->buffers[slot].bytes);
    sys_handle_close(w->buffers[slot].shm_handle);
    w->buffers[slot].in_use = 0;
    if (notify) {
        wm_msg_t m; for (uint64_t i = 0; i < sizeof(m); i++) ((char*)&m)[i] = 0;
        m.type = WM_MSG_BUFFER_RELEASED;
        m.version = WM_PROTO_VERSION;
        m.window_id = w->window_id;
        m.shm_token = token;
        queue_event(w->conn_index, &m);
    }
}

static void free_window_buffers(window_t* w) {
    if (!w->uses_committed_buffers) return;
    for (int i = 0; i < WM_MAX_BUFFERS_PER_WINDOW; i++) release_buffer(w, i, 0);
    w->uses_committed_buffers = 0;
    w->committed_slot = -1;
}

static void close_window(int idx) {
    // M+9B §12: MUST run before free_window_buffers() below -- that
    // call unmaps/releases every attached slot unconditionally,
    // including one the display hardware might still be directly
    // scanning out. Leaving/unbinding first guarantees no VirtIO
    // resource is ever left referencing pages this function is about
    // to free -- exit-fullscreen/minimize/close/client-death (§12) all
    // funnel through this one path already (WM_MSG_DESTROY_WINDOW and
    // disconnect_conn() on client death both call close_window()).
    if (g_presented_mode == PRESENTED_DIRECT && idx == g_direct_window_idx) direct_scanout_force_leave();

    window_t* w = &g_windows[idx];
    free_window_surface(w);
    free_window_buffers(w);
    zorder_remove(idx);
    if (g_keyboard_focus == idx) set_keyboard_focus(-1);
    if (g_active_window == idx) activate_window(-1);
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
    int32_t tx0 = w->x + win_border_off(w), ty0 = w->y + win_border_off(w);
    return px >= tx0 && py >= ty0 && px < tx0 + (int32_t)w->content_w && py < ty0 + win_title_off(w);
}
static int hit_test_content(const window_t* w, int32_t px, int32_t py) {
    int32_t cx0 = w->x + win_border_off(w), cy0 = w->y + win_border_off(w) + win_title_off(w);
    return px >= cx0 && py >= cy0 && px < cx0 + (int32_t)w->content_w && py < cy0 + (int32_t)w->content_h;
}

// ── M+12A: compositor policy boundary ───────────────────────────────
// Extracts the Desktop-shaped DECISIONS this file used to make inline at
// window-creation and pointer-press time, behind a small in-process
// interface. This is NOT the eventual Desktop/Console IPC protocol -- it
// is an in-process C function-pointer table swapped at compile time only
// (see COMPOSITOR_POLICY_TEST_ALT below), matching the M+12 architecture
// report's instruction not to build the final cross-process Environment
// protocol prematurely.
//
// Invariant (non-negotiable, per M+12A review): POLICY DECIDES, COMPOSITOR
// EXECUTES. A policy callback returns a decision value and touches NOTHING
// else -- it never calls raise_window()/set_keyboard_focus()/
// activate_window()/queue_event() or writes to g_windows[]/g_zorder[]
// itself. Every mutation stays in this file's own ordinary code, driven by
// whatever the policy returned. This is what keeps a future third-party
// policy implementation from ever needing direct compositor-mutation
// authority -- it can only ever answer questions, never act.
//
// g_policy has PROCESS lifetime: assigned once, at compile time (via the
// #ifdef below), never reassigned at runtime in M+12A. There is exactly
// one active policy for the whole life of the process -- no refcounting,
// no dynamic allocation, nothing to leak or double-free.
//
// Deliberately NOT extracted this phase (see the M+12A plan's own table):
//   - new-window auto-focus-on-creation
//   - initial z-order insertion of a newly created window
// Both stay ordinary compositor mechanism for M+12A -- there is no second
// behavior anywhere in this codebase that would exercise a hook for either
// one yet, and this file's own "no speculative API" rule means a hook with
// only one possible answer is not built. This is NOT a claim that either
// one is permanently Environment-neutral: a future Desktop Environment may
// well want to suppress focus-stealing for a background-launched app, and
// a future Environment may want different initial stacking. Revisit BOTH
// the day a real second consumer needs to answer either question
// differently -- do not treat their absence here as an architectural
// decision, only as "no consumer yet."
typedef struct {
    int raise;            // move to top of z-order if not already there
    int keyboard_focus;   // become the WM_MSG_KEY_EVENT routing target
    int activate;         // become the decoration-focused/active window
} compositor_activation_decision_t;

typedef enum {
    COMPOSITOR_TITLEBAR_ACTION_NONE = 0,
    COMPOSITOR_TITLEBAR_ACTION_BEGIN_DRAG,
    COMPOSITOR_TITLEBAR_ACTION_REQUEST_CLOSE,
} compositor_titlebar_action_t;

// "Titlebar" is Desktop-specific terminology for a Desktop-specific
// concept (decorated windows with a mouse-oriented title bar) -- this
// hook is accepted for M+12A ONLY because the compositor still physically
// owns titlebar hit-testing and rendering today, and extracting the
// DECISION it makes is strictly incremental. It is NOT the permanent
// Environment policy ABI, and it does not survive decorations moving out
// of compositor-owned Desktop behavior (a later phase, not M+12A): when
// that happens, this hook should disappear or evolve into whatever
// generic shell/decoration interaction mechanism replaces it. Do not
// treat its shape as load-bearing beyond this phase.
typedef struct {
    // A brand-new window (already in g_windows[idx], not yet in g_zorder,
    // not yet damaged) needs an initial position. Pure decision: reads
    // `w`/`idx` only, returns *x_out/*y_out. The compositor applies them
    // directly and does not clamp/validate in M+12A (both policies below
    // only ever return values already known safe).
    void (*place_new_window)(const window_t* w, int idx, int32_t* x_out, int32_t* y_out);

    // A press landed on window idx (already hit-tested by the caller).
    // Pure decision: which of raise/keyboard-focus/activate should this
    // interaction cause? The compositor performs whichever of the three
    // come back true, via the existing mechanism primitives, exactly as
    // it always has.
    compositor_activation_decision_t (*on_window_activate_request)(int idx);

    // A press specifically landed on window idx's titlebar (see this
    // struct's own header comment on why this hook is transitional).
    // Pure decision: what does this button, at this location, mean? The
    // compositor executes the returned action itself.
    compositor_titlebar_action_t (*on_titlebar_press)(int idx, uint32_t btn);
} compositor_policy_t;

static void default_place_new_window(const window_t* w, int idx, int32_t* x_out, int32_t* y_out) {
    (void)w;
    *x_out = (int32_t)(40 + 30 * (idx % 6));
    *y_out = (int32_t)(40 + 30 * (idx % 6));
}
static compositor_activation_decision_t default_on_window_activate_request(int idx) {
    (void)idx;
    compositor_activation_decision_t d;
    d.raise = 1;
    d.keyboard_focus = 1;
    d.activate = 1;
    return d;
}
static compositor_titlebar_action_t default_on_titlebar_press(int idx, uint32_t btn) {
    (void)idx;
    if (btn == INPUT64_BTN_MIDDLE) return COMPOSITOR_TITLEBAR_ACTION_REQUEST_CLOSE;
    if (btn == INPUT64_BTN_LEFT)   return COMPOSITOR_TITLEBAR_ACTION_BEGIN_DRAG;
    return COMPOSITOR_TITLEBAR_ACTION_NONE;
}
// __attribute__((unused)): genuinely unreferenced in a COMPOSITOR_POLICY_TEST_ALT
// build (g_policy points at g_alt_policy instead, below) -- kept defined
// unconditionally anyway so the two policies stay textually side by side.
__attribute__((unused))
static const compositor_policy_t g_default_policy = {
    default_place_new_window, default_on_window_activate_request, default_on_titlebar_press,
};

#ifdef COMPOSITOR_POLICY_TEST_ALT
// Deliberately different from the default in an easily observable way,
// ONLY so compositor_policy_test64 (PACKAGE_DEBUG64 build only) can prove
// the interface is really swappable end to end. Never reachable from a
// normal boot -- selected only at COMPILE TIME below, and only a build
// that explicitly defines this macro ever links these functions in.
static void alt_place_new_window(const window_t* w, int idx, int32_t* x_out, int32_t* y_out) {
    (void)w; (void)idx;
    *x_out = 100; *y_out = 100; // fixed, never cascades
}
static compositor_activation_decision_t alt_on_window_activate_request(int idx) {
    (void)idx;
    compositor_activation_decision_t d;
    d.raise = 0; d.keyboard_focus = 0; d.activate = 0; // clicking never steals anything
    return d;
}
static compositor_titlebar_action_t alt_on_titlebar_press(int idx, uint32_t btn) {
    (void)idx;
    if (btn == INPUT64_BTN_LEFT)   return COMPOSITOR_TITLEBAR_ACTION_REQUEST_CLOSE; // swapped vs default
    if (btn == INPUT64_BTN_MIDDLE) return COMPOSITOR_TITLEBAR_ACTION_BEGIN_DRAG;    // swapped vs default
    return COMPOSITOR_TITLEBAR_ACTION_NONE;
}
static const compositor_policy_t g_alt_policy = {
    alt_place_new_window, alt_on_window_activate_request, alt_on_titlebar_press,
};
static const compositor_policy_t* g_policy = &g_alt_policy;
#else
static const compositor_policy_t* g_policy = &g_default_policy;
#endif

// M12A_POLICY_TRACE: debug-only klog of a newly created window's
// policy-decided position and which policy produced it -- added because
// the wm protocol has no wire message that tells a client its own screen
// position (WM_MSG_WINDOW_CREATED carries only window_id; WM_MSG_CONFIGURE
// carries content w/h, never x/y), so compositor_policy_test64 cannot
// observe placement over the wire. Scraped from the serial log by that
// test instead of adding a protocol message solely for this -- same
// technique M6_FRAME_TRACE already uses elsewhere in this file. Never
// defined in a normal build.
#ifdef M12A_POLICY_TRACE
static void trace_window_placed(const window_t* w) {
    put("M12A_POLICY_TRACE PLACED\n");
    put_kv("  window_id=", w->window_id);
    put_kv("  x=", (uint64_t)(int64_t)w->x);
    put_kv("  y=", (uint64_t)(int64_t)w->y);
    put(g_policy == &g_default_policy ? "  policy=default\n" : "  policy=alt\n");
}
#endif

// ── Composition ──────────────────────────────────────────────────────
// M+4 size-dependent-lag investigation: `clip` bounds EVERY pixel this
// function touches -- border fill, titlebar fill, and (dominant cost
// for any real window) the content blit loop, which now iterates only
// the rows/columns inside clip instead of the window's full
// content_w x content_h every single call. Before this fix, moving the
// cursor one pixel over a 1000x700 window recomposited all ~700,000
// content pixels even though only a ~144-pixel cursor box actually
// needed repainting -- confirmed by manual testing showing dramatically
// worse lag over large windows than small ones, and by instrumented
// measurement (this milestone's own report) showing composited pixel
// counts scaling with WINDOW size, not damage size.
//
// `clip` is computed once per window per frame as
// window_rect ∩ union(every damage rect that touches this window) --
// a single bounding rect, not an exact per-rect walk -- specifically so
// this function is still called AT MOST ONCE per window per frame,
// preserving the earlier stale-line investigation's own fix (reading a
// client's live shared surface more than once per frame, with no
// synchronization against that client concurrently rendering, is a
// real tearing race -- see that investigation's own comment on
// compose_and_present_damage()). A bounding-rect clip can occasionally
// cover a little more than the exact union when damage is scattered
// across a window, but it is NEVER less -- still always a subset of
// what this frame's damage will present, satisfying the invariant that
// this function must never write a backbuffer pixel outside a region
// guaranteed to be presented this same frame.
static void compose_window(const window_t* w, int focused, rect_t clip) {
    rect_t wr = window_rect(w);
    rect_t effective = rect_intersect(wr, clip);
    if (!rect_valid(effective)) return; // nothing of this window is actually inside the clip region

    uint32_t border_color = focused ? BORDER_COLOR_FOCUSED : BORDER_COLOR_UNFOCUSED;
    rect_t border_paint = rect_intersect((rect_t){ w->x, w->y, w->x + (int32_t)win_total_w(w), w->y + (int32_t)win_total_h(w) }, effective);
    if (rect_valid(border_paint)) {
        bb_fill_rect(border_paint.x0, border_paint.y0, (uint32_t)(border_paint.x1 - border_paint.x0), (uint32_t)(border_paint.y1 - border_paint.y0), border_color);
#ifdef COMPOSITOR_PERF_STATS
        g_stat_composited_pixels += rect_area(border_paint);
#endif
    }

    rect_t titlebar_rect = { w->x + win_border_off(w), w->y + win_border_off(w), w->x + win_border_off(w) + (int32_t)w->content_w, w->y + win_border_off(w) + win_title_off(w) };
    rect_t titlebar_paint = rect_intersect(titlebar_rect, effective);
    if (rect_valid(titlebar_paint)) {
        bb_fill_rect(titlebar_paint.x0, titlebar_paint.y0, (uint32_t)(titlebar_paint.x1 - titlebar_paint.x0), (uint32_t)(titlebar_paint.y1 - titlebar_paint.y0), focused ? 0x3A5A85 : 0x3A3A3A);
        bb_draw_text(w->x + win_border_off(w) + 4, w->y + win_border_off(w) + 2, w->title, TITLE_TEXT_COLOR); // small (a few hundred px at most), not worth clipping glyph-by-glyph
#ifdef COMPOSITOR_PERF_STATS
        g_stat_composited_pixels += rect_area(titlebar_paint);
#endif
    }

    rect_t content_rect = { w->x + win_border_off(w), w->y + win_border_off(w) + win_title_off(w), w->x + win_border_off(w) + (int32_t)w->content_w, w->y + win_border_off(w) + win_title_off(w) + (int32_t)w->content_h };
    rect_t content_paint = rect_intersect(content_rect, effective);
    if (!rect_valid(content_paint)) return;

    // M+5: which surface actually holds this window's pixels -- the
    // LEGACY single always-live surface, or the committed-buffer table
    // (Revision 2 §22 items 1-2) -- resolved into four local variables
    // ONCE here so every read below (the bounds check + blit loop) is
    // identical regardless of which model this window uses. A window
    // uses exactly one model for its whole life (never mixed -- see
    // WM_MSG_ATTACH_BUFFER's own handler).
    int has_surface;
    uint64_t surface_addr = 0, surface_bytes = 0;
    uint32_t surface_stride = 0;
    if (w->uses_committed_buffers) {
        has_surface = (w->committed_slot >= 0 && w->buffers[w->committed_slot].in_use);
        if (has_surface) {
            const wm_buffer_t* buf = &w->buffers[w->committed_slot];
            surface_addr = buf->addr; surface_stride = buf->stride; surface_bytes = buf->bytes;
#ifdef M6_FRAME_TRACE
            {
                int idx = window_index_of(w);
                if (g_trace_presented_seq[idx] != g_trace_commit_seq[idx]) {
                    g_trace_presented_seq[idx] = g_trace_commit_seq[idx];
                    put("M6TRACE PRESENT\n");
                    put_kv("  win=", w->window_id);
                    put_kv("  seq=", g_trace_presented_seq[idx]);
                    put_kv("  tick=", sys_get_ticks());
                }
            }
#endif
        }
    } else {
        has_surface = w->has_surface;
        surface_addr = w->surface_addr; surface_stride = w->surface_stride; surface_bytes = w->surface_bytes;
    }

    if (!has_surface) {
        bb_fill_rect(content_paint.x0, content_paint.y0, (uint32_t)(content_paint.x1 - content_paint.x0), (uint32_t)(content_paint.y1 - content_paint.y0), 0x000000);
#ifdef COMPOSITOR_PERF_STATS
        g_stat_composited_pixels += rect_area(content_paint);
#endif
        return;
    }

    // Map content_paint (screen space) back to LOCAL surface row/col
    // coordinates, then blit ONLY that sub-rectangle -- clipped to BOTH
    // the display bounds (via content_paint, already intersected
    // against effective which is itself clipped to window_rect) and the
    // surface's ACTUAL mapped byte size (checked per row below, exactly
    // the same "malicious/buggy client cannot make the compositor read
    // past what it genuinely mapped" guarantee the old unclipped loop
    // had -- now checked against the narrower clipped span actually
    // read, which is always <= the old full-row check and therefore
    // never less safe).
    int32_t local_x0 = content_paint.x0 - (w->x + win_border_off(w));
    int32_t local_y0 = content_paint.y0 - (w->y + win_border_off(w) + win_title_off(w));
    uint32_t clip_w = (uint32_t)(content_paint.x1 - content_paint.x0);
    uint32_t clip_h = (uint32_t)(content_paint.y1 - content_paint.y0);

    for (uint32_t row = 0; row < clip_h; row++) {
        uint32_t src_row_idx = (uint32_t)local_y0 + row;
        uint64_t row_off = (uint64_t)src_row_idx * surface_stride + (uint64_t)local_x0 * 4;
        if (row_off + (uint64_t)clip_w * 4 > surface_bytes) break;
        int32_t dst_y = content_paint.y0 + (int32_t)row;
        const uint32_t* src_row = (const uint32_t*)(uintptr_t)(surface_addr + row_off);
        uint32_t* dst_row = &g_backbuffer[(uint32_t)dst_y * g_disp_w + (uint32_t)content_paint.x0];
        for (uint32_t col = 0; col < clip_w; col++) dst_row[col] = src_row[col];
    }
#ifdef COMPOSITOR_PERF_STATS
    g_stat_composited_pixels += rect_area(content_paint);
#endif
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

// M+7: hardware cursor dimensions/hotspot -- must match the driver's own
// persistent cursor resource size (include/virtio_gpu64.h's
// VIRTIO_GPU64_CURSOR_DIM); sys_cursor_set_image() itself is what
// actually rejects a mismatch, this constant just has to agree with it.
// Hotspot (0,0): the triangle below is anchored at its own top-left
// pixel, exactly like bb_draw_cursor()'s software shape above -- the
// SAME shape, reused pixel-for-pixel rather than inventing a new cursor
// appearance (this milestone's own explicit instruction).
#define HW_CURSOR_DIM 64

// Builds the 64x64 ARGB (0xAARRGGBB, alpha in the top byte) image the
// hardware cursor resource is uploaded with once, at startup --
// transparent (alpha 0) everywhere except the same 12-pixel triangle
// bb_draw_cursor() draws, at full opacity, in CURSOR_COLOR. `out` must
// point at HW_CURSOR_DIM*HW_CURSOR_DIM uint32_t's.
static void build_hw_cursor_image(uint32_t* out) {
    for (uint32_t i = 0; i < (uint32_t)HW_CURSOR_DIM * HW_CURSOR_DIM; i++) out[i] = 0x00000000u;
    uint32_t opaque = 0xFF000000u | CURSOR_COLOR; // full alpha + the same logical RRGGBB bb_draw_cursor() already uses
    for (int32_t r = 0; r < 12; r++) {
        for (int32_t c = 0; c <= r; c++) {
            out[(uint32_t)r * HW_CURSOR_DIM + (uint32_t)c] = opaque;
        }
    }
}

// COMPOSITOR_PERF_STATS is defined near g_full_redraw above (before
// add_damage_rect(), which increments one of its counters directly) --
// see that definition's own comment for the full rationale.

#ifdef COMPOSITOR_PERF_STATS
static uint64_t g_stat_full_frames = 0;
static uint64_t g_stat_damage_frames = 0;
static uint64_t g_stat_presents = 0;
static uint64_t g_stat_pixels = 0;
// M+8: bytes actually mirrored g_backbuffer -> g_frontbuffer (successful
// presents only) and bytes composed into g_backbuffer this session --
// see present_rect()'s own comment on why this must stay damage-scoped
// (approximately equal to successfully-presented damage bytes, never a
// full-screen copy hiding behind small damage).
static uint64_t g_stat_front_mirror_bytes = 0;
static uint64_t g_stat_composed_bytes = 0;
static uint64_t g_stat_present_failures = 0;
static uint64_t g_stat_last_report = 0;
// M+4 investigation: sum of g_damage_count across every damage-path
// frame -- avg rects/frame = this / g_stat_damage_frames. The
// cursor/drag breakdown counters (g_stat_cursor_damage_adds/
// g_stat_drag_damage_adds) are declared earlier, next to
// g_full_redraw, since add_damage_rect() itself increments the total.
static uint64_t g_stat_damage_rects_total = 0;
// M+4 size-dependent-lag investigation: the two numbers this whole
// investigation hinges on comparing. g_stat_damage_area_total is the
// sum of actual damage-rect AREAS (pixels) across every damage-path
// frame -- "how much of the screen genuinely changed." g_stat_composited_pixels
// is incremented by compose_window() itself (border+titlebar+content,
// all three now clip-bounded) -- "how many pixels the compositor
// actually read from a client surface / wrote into the backbuffer."
// Before the clipped-compositing fix, composited_pixels scaled with
// WINDOW size regardless of damage_area (a tiny cursor move over a
// large window still recomposited the whole window); after the fix,
// the two should track much more closely. g_stat_windows_touched counts
// how many (window, frame) pairs actually got composited, for the
// "number of windows intersecting damage" figure requested alongside.
// (g_stat_composited_pixels/g_stat_windows_touched themselves are
// declared earlier, next to g_full_redraw, since compose_window() --
// defined well above this point in the file -- increments the first
// one directly.)
static uint64_t g_stat_damage_area_total = 0;

// M+6: frame-pacing counters (g_stat_frame_requests/g_stat_frame_callbacks_sent/
// g_stat_commit_buffer/g_stat_buffer_releases) are declared earlier,
// next to g_stat_composited_pixels, since notify_buffer_released() and
// the WM_MSG_COMMIT_BUFFER/WM_MSG_REQUEST_FRAME handlers -- all defined
// well above this point in the file -- increment them directly.
// frame_requests/s and frame_callbacks_sent/s should track each other
// closely in steady state (one callback granted per request, per
// window, at most one outstanding at a time); if callbacks ever
// significantly outpaced requests that would indicate the idempotency
// guarantee broke down, and if requests significantly outpaced
// callbacks that would indicate callbacks are being starved.

static void report_perf_stats_if_due(void) {
    uint64_t now = sys_get_ticks();
    if (now - g_stat_last_report < 100) return; // ~1s at the 100Hz tick rate
    g_stat_last_report = now;
    uint64_t other = g_stat_total_damage_adds - g_stat_cursor_damage_adds - g_stat_drag_damage_adds;
    put_kv("perf: full_frames/s=", g_stat_full_frames);
    put_kv("perf: damage_frames/s=", g_stat_damage_frames);
    put_kv("perf: presents/s=", g_stat_presents);
    put_kv("perf: pixels_presented/s=", g_stat_pixels);
    put_kv("perf: front_mirror_bytes/s=", g_stat_front_mirror_bytes);
    put_kv("perf: composed_bytes/s=", g_stat_composed_bytes);
    put_kv("perf: present_failures/s=", g_stat_present_failures);
    put_kv("perf: damage_rects/s=", g_stat_damage_rects_total);
    put_kv("perf: damage_area_pixels/s=", g_stat_damage_area_total);
    put_kv("perf: composited_pixels/s=", g_stat_composited_pixels);
    put_kv("perf: windows_touched/s=", g_stat_windows_touched);
    put_kv("perf: cursor_damage_adds/s=", g_stat_cursor_damage_adds);
    put_kv("perf: drag_damage_adds/s=", g_stat_drag_damage_adds);
    put_kv("perf: frame_requests/s=", g_stat_frame_requests);
    put_kv("perf: frame_callbacks_sent/s=", g_stat_frame_callbacks_sent);
    put_kv("perf: commit_buffer/s=", g_stat_commit_buffer);
    put_kv("perf: buffer_releases/s=", g_stat_buffer_releases);
    put_kv("perf: other_damage_adds/s=", other);
    put_kv("perf: damage_overflow_count_total=", g_damage_overflow_count);
    g_stat_full_frames = 0; g_stat_damage_frames = 0; g_stat_presents = 0; g_stat_pixels = 0;
    g_stat_front_mirror_bytes = 0; g_stat_composed_bytes = 0; g_stat_present_failures = 0;
    g_stat_damage_rects_total = 0; g_stat_total_damage_adds = 0; g_stat_cursor_damage_adds = 0; g_stat_drag_damage_adds = 0;
    g_stat_damage_area_total = 0; g_stat_composited_pixels = 0; g_stat_windows_touched = 0;
    g_stat_frame_requests = 0; g_stat_frame_callbacks_sent = 0;
    g_stat_commit_buffer = 0; g_stat_buffer_releases = 0;
}
#endif

#ifdef M8_FAULT_INJECT
// M+8 deterministic fault injection -- compile-time gated, NEVER active
// production behavior (see this milestone's own report). A single
// countdown: 0 means "fail the very next present_rect() call", any
// positive N means "let N more calls through, then fail the one after",
// -1 means disabled. Set via m8_fault_inject_arm(), consumed by
// present_rect() below.
static int g_fault_inject_countdown = -1;
static void m8_fault_inject_arm(int n) { g_fault_inject_countdown = n; }
static int m8_fault_inject_should_fail(void) {
    if (g_fault_inject_countdown < 0) return 0;
    if (g_fault_inject_countdown == 0) { g_fault_inject_countdown = -1; return 1; }
    g_fault_inject_countdown--;
    return 0;
}
#endif

// M+8: the ONLY place a rect is ever declared "successfully presented."
// SUCCESS means exactly what this milestone's own spec requires: the
// requested rectangle became part of the visible output before this
// call returns -- not "a GPU command was submitted," not "fallback was
// merely enabled for next time." kernel/display64.c's own
// display64_gpu_flush_rect() now returns that same honest signal: if
// the GPU backend fails, it synchronously mirrors the SAME rect's
// already-correct pixels (blit_row already wrote them into the GPU
// backend's own CPU-side buffer before the flush was ever attempted)
// into the legacy framebuffer and only reports success if THAT
// synchronous write actually lands -- so sys_display_present()'s return
// value is already the true, final success/failure verdict for this
// exact rect, no further guessing needed here.
//
// Order matters, per this milestone's own explicit instruction: DISPLAY
// SUCCESS FIRST, FRONT MIRROR UPDATE SECOND. A failed present returns
// immediately, before g_frontbuffer is touched at all -- the previous,
// still-accurate front pixels for this rect are left completely alone.
// The mirror copy itself is deliberately scoped to exactly this rect
// (never a full-screen copy) so g_stat_front_mirror_bytes stays
// approximately equal to successfully-presented damage bytes regardless
// of how small the damage is (§14 of this milestone's own spec).
//
// Returns 1 on success, 0 on failure.
static int present_rect(rect_t r) {
    r = rect_clip_to_display(r);
    if (!rect_valid(r)) return 1; // nothing to present -- trivially not wrong
    // BUG FOUND DURING M+8'S OWN VISUAL VERIFICATION: the backbuffer is
    // ONE g_disp_w-wide buffer that a sub-rect present is just a window
    // into -- its REAL per-row stride is g_disp_w*4 regardless of how
    // wide the rect being presented is. output64_present_rect()'s own
    // `stride_pixels` parameter exists specifically so this is never
    // silently wrong again -- see that function's own comment. The
    // pointer passed must also point at the rect's own top-left pixel
    // WITHIN the backbuffer, not always the backbuffer's origin.
    const uint32_t* src_origin = g_backbuffer + (uint64_t)r.y0 * g_disp_w + (uint64_t)r.x0;
    uint32_t rw = (uint32_t)(r.x1 - r.x0), rh = (uint32_t)(r.y1 - r.y0);

#ifdef M8_FAULT_INJECT
    int64_t rc = m8_fault_inject_should_fail() ? (int64_t)-1 : output64_present_rect(src_origin, g_disp_w, r.x0, r.y0, rw, rh);
#else
    int64_t rc = output64_present_rect(src_origin, g_disp_w, r.x0, r.y0, rw, rh);
#endif
#ifdef COMPOSITOR_PERF_STATS
    g_stat_presents++;
    if (rc == 0) g_stat_pixels += (uint64_t)rw * rh;
    else g_stat_present_failures++;
#endif
    if (rc < 0) {
        // Post-M+12A audit regression guard: a NEVER-reset, always-compiled
        // counter (same discipline as g_diag_duplicate_release_count/
        // g_diag_stale_commit_generation_count above) -- unlike
        // COMPOSITOR_PERF_STATS's own g_stat_present_failures, this is never
        // zeroed periodically, so "stays exactly 0 for the whole session" is
        // a real, checkable invariant. output64_present_rect() failing at all
        // during ordinary operation (no fault injection armed) means the
        // compositor's own display handle -- or the request derived from
        // g_backbuffer -- has gone bad; the has_surface bug this counter was
        // added to catch is exactly this: it made EVERY future present fail,
        // full or partial, forever, the instant one committed-buffer window
        // was closed.
        g_diag_present_failure_count++;
#ifdef COMPOSITOR_PRESENT_FAIL_TRACE
        put("DBG_PRESENT_FAIL: output64_present_rect failed\n");
        put_kv("  g_diag_present_failure_count=", g_diag_present_failure_count);
        put_kv("  x=", (uint64_t)r.x0); put_kv("  y=", (uint64_t)r.y0);
        put_kv("  w=", (uint64_t)rw); put_kv("  h=", (uint64_t)rh);
#endif
        return 0; // FAILURE -- g_frontbuffer is not touched, per this function's own header comment
    }

    // SUCCESS -- mirror ONLY this rect, row by row, directly from
    // g_backbuffer (the pixels the output was just proven to have
    // presented) into g_frontbuffer. Both buffers share g_disp_w as
    // their real stride (same layout, same allocation size).
    for (uint32_t row = 0; row < rh; row++) {
        uint32_t* dst = &g_frontbuffer[(uint32_t)(r.y0 + (int32_t)row) * g_disp_w + (uint32_t)r.x0];
        const uint32_t* src = &g_backbuffer[(uint32_t)(r.y0 + (int32_t)row) * g_disp_w + (uint32_t)r.x0];
        for (uint32_t col = 0; col < rw; col++) dst[col] = src[col];
    }
#ifdef COMPOSITOR_PERF_STATS
    g_stat_front_mirror_bytes += (uint64_t)rw * rh * 4u;
#endif
    return 1;
}

// M+4 second stale-line investigation: composition and presentation
// split into independently-selectable steps (composite_full/
// composite_damage_scoped, present_full/present_damage) specifically so
// the diagnostic matrix below (COMPOSITOR_DIAGNOSTIC_MODE) can combine
// them in all four ways -- see that macro's own comment for the full
// rationale. Behaviorally, mode D (damage composite + damage present)
// is exactly the pre-existing default path; nothing about normal
// operation changes when the macro is left at its default (0).

// Composites the ENTIRE screen into the backbuffer: background, every
// z-order window in full, then the cursor. Never touches g_damage[] --
// this is the Milestone 30 "recompute everything" path, unconditionally
// correct by construction since nothing is skipped.
static void composite_full(void) {
    bb_fill_rect(0, 0, g_disp_w, g_disp_h, BG_COLOR);
    rect_t whole = { 0, 0, (int32_t)g_disp_w, (int32_t)g_disp_h };
    for (int i = 0; i < g_zorder_count; i++) {
        int idx = g_zorder[i];
        compose_window(&g_windows[idx], idx == g_active_window, whole); // clip = whole display -- equivalent to the old always-unclipped call
    }
    // M+7: when the hardware cursor is active, the compositor never
    // draws a cursor into g_backbuffer at all -- position is handled
    // entirely through sys_cursor_move(), outside the desktop damage/
    // composite/present path (see handle_input_event()'s own comment).
    if (!g_hw_cursor_active) bb_draw_cursor(g_cursor_x, g_cursor_y);
}

// Composites only what g_damage[] says changed -- exactly the M+4
// four-pass fix's passes 1-3 (clear damaged rects, composite each
// touching window exactly once, cursor once), pulled out into its own
// function so the diagnostic matrix can pair it with either presentation
// step. See compose_and_present_damage()'s own header comment (below,
// now just a thin wrapper) for the full race-condition rationale this
// "exactly once" design fixes.
static void composite_damage_scoped(void) {
    for (int i = 0; i < g_damage_count; i++) {
        rect_t r = g_damage[i];
        bb_fill_rect(r.x0, r.y0, (uint32_t)(r.x1 - r.x0), (uint32_t)(r.y1 - r.y0), BG_COLOR);
    }
    for (int zi = 0; zi < g_zorder_count; zi++) {
        int idx = g_zorder[zi];
        window_t* w = &g_windows[idx];
        rect_t wr = window_rect(w);
        // M+4 size-dependent-lag fix: clip = window_rect ∩ union(every
        // damage rect touching this window) -- a single bounding rect
        // computed once, so compose_window() below is still called AT
        // MOST ONCE per window per frame (preserving the earlier
        // stale-line fix's "read a client's live surface only once per
        // frame" invariant) while composing only the area that actually
        // changed, not the window's full size. See compose_window()'s
        // own header comment for the full rationale.
        rect_t combined = { 0, 0, 0, 0 };
        int touches_damage = 0;
        for (int i = 0; i < g_damage_count; i++) {
            if (rect_intersects(wr, g_damage[i])) {
                combined = touches_damage ? rect_union(combined, g_damage[i]) : g_damage[i];
                touches_damage = 1;
            }
        }
        if (touches_damage) {
            rect_t clip = rect_intersect(wr, combined);
#ifdef COMPOSITOR_PERF_STATS
            g_stat_windows_touched++;
#endif
            compose_window(w, idx == g_active_window, clip);
        }
    }
    // M+7: with hardware cursor active, no cursor damage rect is ever
    // added to g_damage[] in the first place (handle_input_event() skips
    // it entirely), so cursor_hit below would already always be false --
    // this explicit guard is defense in depth, not load-bearing on its
    // own, matching composite_full()'s own guard for the same reason.
    if (!g_hw_cursor_active) {
        rect_t cur = cursor_rect_at(g_cursor_x, g_cursor_y);
        int cursor_hit = 0;
        for (int i = 0; i < g_damage_count; i++) {
            if (rect_intersects(cur, g_damage[i])) { cursor_hit = 1; break; }
        }
        if (cursor_hit) bb_draw_cursor(g_cursor_x, g_cursor_y);
    }
}

// Returns 1 if the whole screen is now known-presented, 0 on failure --
// the caller (compose_and_present_full()) uses this to decide whether
// g_full_redraw may be cleared; per this milestone's own instruction, a
// failed full-screen present must leave g_full_redraw SET so the next
// paced frame retries the whole screen again.
static int present_full(void) {
    rect_t whole = { 0, 0, (int32_t)g_disp_w, (int32_t)g_disp_h };
    return present_rect(whole);
}

// M+8: presents every currently-pending damage rect independently (one
// rect's failure never affects another's outcome -- §4 of this
// milestone's own spec) and compacts g_damage[] in place, keeping ONLY
// the rects that failed. Successful rects are retired (removed) exactly
// as before; failed rects remain queued for the next paced attempt,
// coalescing normally with whatever new damage arrives meanwhile.
// Returns 1 if every rect succeeded (the pass is now fully presented),
// 0 if any rect is still pending.
static int present_damage(void) {
    int kept = 0;
    int all_ok = 1;
    for (int i = 0; i < g_damage_count; i++) {
        if (present_rect(g_damage[i])) continue; // succeeded -- retire (don't copy forward)
        g_damage[kept++] = g_damage[i]; // failed -- retain for retry
        all_ok = 0;
    }
    g_damage_count = kept;
    return all_ok;
}

// M+4 second stale-line investigation: define to check, every frame,
// whether the cursor's (and, while dragging, the dragged window's)
// LAST-PRESENTED rect -- what a real display is still physically
// showing from the previous frame, tracked independently of "current
// logical position" or "previous input-event position" -- is fully
// covered by THIS frame's damage before that damage gets consumed. If
// it is not, klogs the exact rects involved: this is precisely the
// frame an old cursor/window trace will survive on screen. A coarse
// but cheap check (nine sample points: four corners, four edge
// midpoints, center of the last-presented rect, each tested against
// every current damage rect) rather than exact polygon coverage -- more
// than enough resolution for a 12x12 cursor box or a window-sized rect
// against a handful of damage rects, and cheap enough to run every
// frame without perturbing timing.
// #define COMPOSITOR_DAMAGE_TRACE 1

#ifdef COMPOSITOR_DAMAGE_TRACE
static rect_t g_trace_last_cursor_rect;
static int g_trace_last_cursor_valid = 0;
static int g_trace_last_drag_idx = -1;
static rect_t g_trace_last_drag_rect;

static int trace_point_covered(int32_t px, int32_t py) {
    for (int i = 0; i < g_damage_count; i++) {
        if (px >= g_damage[i].x0 && px < g_damage[i].x1 && py >= g_damage[i].y0 && py < g_damage[i].y1) return 1;
    }
    return 0;
}

// Returns 1 if `r` looks fully covered by this frame's damage (or a
// full redraw is already happening this frame, which trivially covers
// everything).
static int trace_rect_covered(rect_t r) {
    if (g_full_redraw) return 1;
    int32_t xs[3] = { r.x0, (r.x0 + r.x1 - 1) / 2, r.x1 - 1 };
    int32_t ys[3] = { r.y0, (r.y0 + r.y1 - 1) / 2, r.y1 - 1 };
    for (int xi = 0; xi < 3; xi++)
        for (int yi = 0; yi < 3; yi++)
            if (!trace_point_covered(xs[xi], ys[yi])) return 0;
    return 1;
}

static void trace_dump_rect(const char* label, rect_t r) {
    put(label);
    char b[96]; int p = 0;
    b[p++] = '(';
    append_u64(b, &p, (uint64_t)(int64_t)r.x0); b[p++] = ',';
    append_u64(b, &p, (uint64_t)(int64_t)r.y0); b[p++] = ')'; b[p++] = '-'; b[p++] = '(';
    append_u64(b, &p, (uint64_t)(int64_t)r.x1); b[p++] = ',';
    append_u64(b, &p, (uint64_t)(int64_t)r.y1); b[p++] = ')'; b[p++] = '\n'; b[p] = 0;
    put(b);
}

static inline int rect_equal(rect_t a, rect_t b) {
    return a.x0 == b.x0 && a.y0 == b.y0 && a.x1 == b.x1 && a.y1 == b.y1;
}

static void trace_check_coverage(void) {
    // A STATIONARY cursor/window needs no coverage at all this frame --
    // whatever is on screen for it is already correct from the frame
    // that actually presented it, regardless of what ELSE is damaged
    // elsewhere on screen. Checking coverage unconditionally here was
    // itself a bug in this diagnostic: it fired on every frame where
    // unrelated damage (e.g. an animating client) simply didn't happen
    // to overlap an UNMOVED cursor, which is correct behavior, not
    // staleness. Only a rect whose on-screen position actually CHANGED
    // since the last present needs its old-position pixels to be
    // recoverable from this frame's damage.
    rect_t cur_now = cursor_rect_at(g_cursor_x, g_cursor_y);
    int cursor_moved = g_trace_last_cursor_valid && !rect_equal(cur_now, g_trace_last_cursor_rect);
    if (cursor_moved && !trace_rect_covered(g_trace_last_cursor_rect)) {
        put("TRACE: STALE CURSOR -- last-presented rect not covered by this frame's damage!\n");
        trace_dump_rect("TRACE:   last-presented cursor rect: ", g_trace_last_cursor_rect);
        trace_dump_rect("TRACE:   current cursor rect:        ", cur_now);
        put_kv("TRACE:   damage_count=", (uint64_t)g_damage_count);
        for (int i = 0; i < g_damage_count; i++) trace_dump_rect("TRACE:   damage rect: ", g_damage[i]);
    }
    if (g_trace_last_drag_idx >= 0 && g_windows[g_trace_last_drag_idx].in_use) {
        rect_t win_now = window_rect(&g_windows[g_trace_last_drag_idx]);
        int win_moved = !rect_equal(win_now, g_trace_last_drag_rect);
        if (win_moved && !trace_rect_covered(g_trace_last_drag_rect)) {
            put("TRACE: STALE DRAGGED WINDOW -- last-presented rect not covered by this frame's damage!\n");
            trace_dump_rect("TRACE:   last-presented window rect: ", g_trace_last_drag_rect);
            trace_dump_rect("TRACE:   current window rect:        ", win_now);
            put_kv("TRACE:   damage_count=", (uint64_t)g_damage_count);
            for (int i = 0; i < g_damage_count; i++) trace_dump_rect("TRACE:   damage rect: ", g_damage[i]);
        }
    }
}

static void trace_record_presented(void) {
    g_trace_last_cursor_rect = cursor_rect_at(g_cursor_x, g_cursor_y);
    g_trace_last_cursor_valid = 1;
    if (g_dragging >= 0) {
        g_trace_last_drag_idx = g_dragging;
        g_trace_last_drag_rect = window_rect(&g_windows[g_dragging]);
    } else {
        g_trace_last_drag_idx = -1;
    }
}
#endif

// The Milestone 30 path, unmodified in behavior -- the mandatory safe
// fallback for the initial draw, g_full_redraw, and any future case
// nobody has taught the damage path about yet. A thin wrapper over
// composite_full()/present_full() (split apart above so the diagnostic
// matrix can reuse them) so every existing call site is untouched.
// Returns 1 if the whole screen is now known-presented (the caller may
// clear g_full_redraw), 0 if presentation failed (g_full_redraw MUST
// stay set -- per this milestone's own explicit instruction -- so the
// next paced frame retries the whole screen).
static int compose_and_present_full(void) {
    composite_full();
    g_composed_generation++;
#ifdef COMPOSITOR_PERF_STATS
    g_stat_composed_bytes += (uint64_t)g_disp_w * g_disp_h * 4u;
#endif
    int ok = present_full();
    if (ok) {
        g_presented_generation = g_composed_generation;
        g_front_valid = 1;
        g_front_partial = 0;
    } else {
        g_front_partial = 1;
    }
#ifdef COMPOSITOR_DAMAGE_TRACE
    // A full redraw changes what's physically on screen too -- the next
    // damage-path frame's coverage check must compare against THIS
    // frame's presented state, not whatever damage-path frame came
    // before it, or a full-redraw frame would look like a silent
    // discontinuity and produce a false positive on the very next check.
    // Only record it as "presented" if it actually was.
    if (ok) trace_record_presented();
#endif
#ifdef COMPOSITOR_PERF_STATS
    g_stat_full_frames++;
#endif
    return ok;
}

// ── M+9B: direct scanout enter/update/leave ──────────────────────────
// direct_scanout_force_leave(): the ONLY sanctioned way out of
// PRESENTED_DIRECT (§11's exact ordering). Called both from a failed
// eligibility re-check on the next commit AND from every other exit
// point this milestone identified (raise_window() raising a DIFFERENT
// window, close_window() on the direct-owned window, hardware cursor
// becoming unavailable, WM_MSG_SET_FULLSCREEN's own exit branch).
// Idempotent -- a no-op if not currently PRESENTED_DIRECT.
static void direct_scanout_force_leave(void) {
    if (g_presented_mode != PRESENTED_DIRECT) return;
    if (g_direct_window_idx < 0 || !g_windows[g_direct_window_idx].in_use) {
        // Window already gone (close_window() clears in_use before this
        // could run in the normal call order, but this stays defensive
        // regardless of call order) -- state is unrecoverable via the
        // normal window-buffer path; just reset bookkeeping. The kernel
        // side's own resource is still cached; nothing further can be
        // done without a valid window/slot to unbind through, so it is
        // deliberately left bound (a small, bounded leak of ONE cache
        // slot until this window's connection teardown -- which already
        // ran free_window_buffers()/disconnect handling for the client
        // side -- rather than risk unbinding the wrong resource).
        g_presented_mode = PRESENTED_COMPOSITED;
        g_direct_window_idx = -1;
        return;
    }
    window_t* w = &g_windows[g_direct_window_idx];
    int slot = w->scanout_owned_slot;

    // §11 steps 2-5: force a full composited redraw of the CURRENT
    // scene (this window's own content included, via its normal
    // committed_slot -- composite_full()/compose_window() read that
    // exactly like any other window, direct mode never touched it) and
    // run it through the ORDINARY present path -- this must succeed
    // BEFORE scanout switches back, so the compositor's own resource is
    // already fresh the instant it becomes visible again.
    mark_full_redraw();
    int recompose_ok = compose_and_present_full();

    if (!recompose_ok) {
        // Never switch scanout away from a buffer we cannot yet prove
        // has a working replacement -- stay PRESENTED_DIRECT, old
        // buffer remains alive and visible, exactly as it was. The
        // next paced frame will retry via g_full_redraw (already set).
        return;
    }

    if (output64_direct_leave() < 0) {
#ifdef M9B_STATS
        g_stat_direct_failures++;
#endif
        // Switch-back itself failed at the VirtIO level -- the OLD
        // direct buffer is still the genuinely visible output (§11: a
        // failed leave must never be treated as having happened).
        return;
    }

    g_presented_mode = PRESENTED_COMPOSITED;
    g_direct_window_idx = -1;
#ifdef M9B_STATS
    g_stat_direct_to_composed++;
#endif

    // Only NOW, after hardware is confirmed no longer scanning out this
    // resource, may it be unbound and the client buffer actually
    // released (§7/§11 step 7-8).
    if (slot >= 0 && w->buffers[slot].in_use) {
        output64_direct_unbind(w->buffers[slot].token);
        w->scanout_owned_slot = -1;
        notify_buffer_released(w, slot); // idempotently records slot_release_ground_truth[slot] itself -- see that function's own comment
#ifdef M9B_STATS
        g_stat_completed_scanout_releases++;
#endif
    }

    // Cross-window cache-leak fix: a double-buffering fullscreen client
    // (e.g. GfxDemo alternating fs_bufs[0]/fs_bufs[1]) binds a NEW token
    // into the kernel's 2-slot direct-scanout cache on every commit that
    // supersedes the previous one (direct_scanout_try_enter_or_update()
    // above), but the SUPERSEDED token's own cache slot is only ever
    // marked BOUND, never unbound -- unbinding happens exclusively here,
    // and only for whichever ONE slot was scanout_owned_slot at the
    // moment of exit. Any OTHER token this same window bound during its
    // fullscreen session (its "other" alternation buffer, left cached
    // but inactive) was therefore never released, permanently occupying
    // a cache slot even after the window fully returns to composited
    // mode -- confirmed via live trace: cache went from
    // [BOUND id=5][BOUND id=6] to [BOUND id=5][EMPTY] after this
    // function only unbound id=6, leaking id=5 forever. Sweep every
    // buffer this window owns and defensively unbind each one's token
    // too -- display64_direct_scanout_unbind() is already a safe no-op
    // for a token that isn't cached, and safely refuses (rather than
    // corrupting state) a token that's still SLOT_ACTIVE, so this is
    // harmless for slots that were never bound or already handled above.
    for (int i = 0; i < WM_MAX_BUFFERS_PER_WINDOW; i++) {
        if (i == slot) continue; // already handled above
        if (w->buffers[i].in_use) output64_direct_unbind(w->buffers[i].token);
    }
#ifdef M9B_STATS
    m9b_dump_stats();
#endif
}

// direct_scanout_try_enter_or_update(): called from WM_MSG_COMMIT_BUFFER
// for a fullscreen window's every commit. Returns 1 if this commit was
// handled entirely through direct scanout (caller must skip its own
// normal damage_content_rect()/notify_buffer_released() for THIS
// commit -- §15: steady-state direct frames must add zero compositor
// damage), or 0 if the caller should fall through to the normal
// composited path (ineligible, or the direct attempt itself failed --
// direct_scanout_force_leave() has already been called in that case if
// this window was the previous direct owner, so the normal path picks
// up a fully composited, fully consistent scene).
static int direct_scanout_try_enter_or_update(window_t* w, int idx, int slot, int old_slot) {
    if (!direct_scanout_eligible(w, idx)) {
        if (g_presented_mode == PRESENTED_DIRECT && g_direct_window_idx == idx) direct_scanout_force_leave();
        return 0;
    }

#ifdef M9B_STATS
    g_stat_direct_attempts++;
#endif
    uint64_t token = w->buffers[slot].token;
    uint32_t bytes = w->content_w * w->content_h * 4u;

    if (output64_direct_bind(token, w->content_w, w->content_h) < 0) {
#ifdef M9B_STATS
        g_stat_direct_failures++;
#endif
        if (g_presented_mode == PRESENTED_DIRECT && g_direct_window_idx == idx) direct_scanout_force_leave();
        return 0;
    }

    int was_already_direct = (g_presented_mode == PRESENTED_DIRECT && g_direct_window_idx == idx);
    // switch_active is unconditionally 1: every commit for the direct-
    // scanned window's steady state also means "this exact buffer is
    // the one that should be visible now," whether or not the window
    // itself was already the active owner -- e.g. GfxDemo alternating
    // fs_bufs[0]/fs_bufs[1] every frame needs scanout repointed to
    // whichever one it just committed, every time.
    int64_t rc = output64_direct_present(token, 0, 0, w->content_w, w->content_h, 1);
    if (rc < 0) {
#ifdef M9B_STATS
        g_stat_direct_failures++;
#endif
        if (g_presented_mode == PRESENTED_DIRECT && g_direct_window_idx == idx) direct_scanout_force_leave();
        return 0;
    }

#ifdef M9B_STATS
    g_stat_direct_successes++;
    g_stat_direct_frames++;
    g_stat_direct_transfer_bytes += bytes;
    if (!was_already_direct) g_stat_composed_to_direct++;
#else
    (void)bytes; (void)was_already_direct;
#endif

    g_presented_mode = PRESENTED_DIRECT;
    g_direct_window_idx = idx;

    // `old_slot` is now PROVABLY superseded -- the successful present()
    // above confirms hardware has already moved on to `slot`, exactly
    // mirroring the non-direct COMMIT_BUFFER path's own "the old buffer
    // is provably never read again the instant committed_slot changed"
    // reasoning. This must fire regardless of whether old_slot was
    // ITSELF previously scanout-owned (the fs_bufs[0]<->fs_bufs[1]
    // steady-state alternation) or an ordinary composited buffer being
    // superseded by direct mode for the very FIRST time (entering
    // direct mode from composited). An earlier version of this check
    // required `w->scanout_owned_slot == old_slot`, which is false on
    // exactly that first transition (scanout_owned_slot is still -1 at
    // that point) -- since the COMMIT_BUFFER handler's own normal
    // release path is ALSO skipped whenever this function returns 1,
    // that guard permanently skipped releasing the client's last
    // composited buffer on every fullscreen entry, leaking one commit
    // buffer forever: the client's own alternation would eventually
    // wait on that exact slot's WM_MSG_BUFFER_RELEASED after returning
    // to composited mode, which would then never arrive -- a permanent
    // freeze one frame after fullscreen exit (reproduced and confirmed
    // via manual testing).
    if (old_slot >= 0 && old_slot != slot) {
        notify_buffer_released(w, old_slot); // idempotently records slot_release_ground_truth[old_slot] itself -- see that function's own comment
#ifdef M9B_STATS
        g_stat_completed_scanout_releases++;
#endif
    }
    w->scanout_owned_slot = slot;
    return 1;
}

// M+4 second stale-line investigation: selects which of the four
// composite/present combinations compose_and_present_damage() below
// actually runs, to isolate WHICH layer a visible artifact comes from
// (manual interactive testing kept reproducing stale cursor/window
// traces that no scripted QEMU-monitor input burst this session could
// re-trigger -- this matrix exists to localize the bug without needing
// a live repro):
//   0 = D (default/unchanged): damage-scoped composite + damage-only
//       present -- today's real path, both stages scoped.
//   1 = A: full composite + full present every frame -- the Milestone
//       30 path in all but name; if artifacts appear here, they are
//       not a compositor bug at all (nothing is skipped anywhere).
//   2 = B: full composite (backbuffer always 100% fresh) + damage-only
//       present -- isolates the PRESENT/TRANSFER layer specifically:
//       composition can never be stale here, so any visible artifact
//       must come from present_rect()/sys_display_present()/the GPU
//       transfer+flush path presenting the wrong pixels or the wrong
//       rect.
//   3 = C: damage-scoped composite + full present every frame --
//       isolates composition COVERAGE: presentation gets the whole
//       screen every time (so it can never be the cause), so any
//       visible artifact must mean composite_damage_scoped() left
//       stale pixels sitting in the backbuffer somewhere damage failed
//       to reach.
// Not left enabled by default; #define to 1/2/3 to select a mode.
// #define COMPOSITOR_DIAGNOSTIC_MODE 0
#ifndef COMPOSITOR_DIAGNOSTIC_MODE
// #define COMPOSITOR_DIAGNOSTIC_MODE 0 // left undefined by a normal build -- this is the real, unchanged default path
#endif

// M-next: redraws and presents ONLY the accumulated damage rects,
// instead of the whole screen. For each rect: clear its background,
// redraw (in z-order) every window whose bounds intersect it, redraw
// the cursor if it intersects, then present just that rect. Windows
// that don't intersect ANY damage rect are never touched at all.
//
// Correctness note: `compose_window()` itself is NOT clip-rect-aware --
// it always redraws a whole intersecting window's content, even the
// portion outside the current damage rect. That's deliberate, not an
// oversight: drawing a few extra pixels into the BACKBUFFER that this
// frame won't present is wasted work, never incorrect (the backbuffer
// stays fully self-consistent, and those pixels are simply correct
// early for whenever their own region is next damaged). Threading a
// clip rect through compose_window/bb_fill_rect/bb_draw_text for a
// theoretically tighter redraw is exactly the kind of complexity §17
// says to avoid when the simpler version is still correct.
//
// M+4 stale-line investigation (first pass): restructured composition
// into passes so each window is read/composited AT MOST ONCE per frame
// (composite_damage_scoped() above) -- fixed a real read-consistency
// race (compose_window() re-reads a client's live shared surface with
// no synchronization; calling it twice in one frame could observe two
// different animation frames). Confirmed via a forced-full-composite
// diagnostic that this was NOT the cause of the "staircase" pattern
// manual testing first flagged (that pattern is legitimate output of
// gfx_demo64's own XOR-based gradient, visible even at idle) -- but the
// fix is real and stays regardless.
//
// M+4 stale-line investigation (second pass): manual interactive
// testing continued to reproduce stale cursor/window traces that no
// scripted QEMU-monitor input burst this session could re-trigger.
// COMPOSITOR_DAMAGE_TRACE below adds the exact invariant check needed
// to catch this from a REAL interactive session's own log rather than a
// screenshot: before consuming this frame's damage, verify the cursor's
// (and, if dragging, the dragged window's) LAST-PRESENTED rect --
// what is actually still on screen from the previous frame -- is fully
// covered by this frame's damage. If it is not, that is the exact
// frame an old trace will survive, logged with every rect involved.
#ifdef COMPOSITOR_FRONT_CONSISTENCY_CHECK
// M+8 debug-only: an EXHAUSTIVE (not sampled, unlike COMPOSITOR_DAMAGE_TRACE's
// own 9-point check -- a single missed pixel is exactly what this is
// hunting for) comparison of every pixel OUTSIDE all currently-pending
// damage rects between g_backbuffer and g_frontbuffer. If g_frontbuffer
// is established and these ever differ outside pending damage, some
// scene mutation changed a pixel that damage tracking never recorded --
// exactly the "forgotten damage" class of bug this exists to catch.
// O(width*height) per call -- debug/test builds only, never enabled in
// a release build (see this milestone's own instruction).
static void m8_check_front_consistency(void) {
    if (!g_front_valid) return;
    for (uint32_t y = 0; y < g_disp_h; y++) {
        for (uint32_t x = 0; x < g_disp_w; x++) {
            int in_damage = 0;
            for (int i = 0; i < g_damage_count; i++) {
                if ((int32_t)x >= g_damage[i].x0 && (int32_t)x < g_damage[i].x1 &&
                    (int32_t)y >= g_damage[i].y0 && (int32_t)y < g_damage[i].y1) { in_damage = 1; break; }
            }
            if (in_damage) continue;
            uint32_t idx = y * g_disp_w + x;
            if (g_backbuffer[idx] != g_frontbuffer[idx]) {
                put("M8DBG: front/back mismatch outside pending damage -- forgotten damage bug\n");
                put_kv("  x=", (uint64_t)x);
                put_kv("  y=", (uint64_t)y);
                put_kv("  back=", (uint64_t)g_backbuffer[idx]);
                put_kv("  front=", (uint64_t)g_frontbuffer[idx]);
                return; // one report per frame is enough to flag the bug
            }
        }
    }
}
#endif

// M+8: unlike compose_and_present_full(), the caller (main loop) never
// needs this function's return value -- g_damage_count itself, updated
// in place by present_damage(), already IS the retry state (a nonzero
// count after this call means "still pending," and the main loop's own
// `g_full_redraw || g_damage_count > 0` gate already re-enters this
// path on the next paced tick with exactly the right set of rects).
static void compose_and_present_damage(void) {
#ifdef COMPOSITOR_PERF_STATS
    g_stat_damage_rects_total += (uint64_t)g_damage_count;
    for (int _i = 0; _i < g_damage_count; _i++) g_stat_damage_area_total += rect_area(g_damage[_i]);
#endif
    g_composed_generation++;
#ifdef COMPOSITOR_DEBUG_DAMAGE
    put_kv("KDBG damage_count=", (uint64_t)g_damage_count);
    put_kv("KDBG cursor_x=", (uint64_t)(int64_t)g_cursor_x);
    put_kv("KDBG cursor_y=", (uint64_t)(int64_t)g_cursor_y);
    for (int i = 0; i < g_damage_count; i++) {
        rect_t r = g_damage[i];
        put_kv("KDBG rect x0=", (uint64_t)(int64_t)r.x0);
        put_kv("KDBG rect y0=", (uint64_t)(int64_t)r.y0);
        put_kv("KDBG rect x1=", (uint64_t)(int64_t)r.x1);
        put_kv("KDBG rect y1=", (uint64_t)(int64_t)r.y1);
    }
#endif
#ifdef COMPOSITOR_DAMAGE_TRACE
    trace_check_coverage();
#endif

#ifdef COMPOSITOR_PERF_STATS
    for (int _i = 0; _i < g_damage_count; _i++) g_stat_composed_bytes += (uint64_t)rect_area(g_damage[_i]) * 4u;
#endif

    int ok = 1;
#if COMPOSITOR_DIAGNOSTIC_MODE == 1 // A: full composite + full present
    // Legacy diagnostic mode, off by default -- not wired into the M+8
    // retry model (it presents the whole screen regardless of g_damage[]
    // contents), so damage is simply consumed exactly as it always was.
    composite_full();
    ok = present_full();
    g_damage_count = 0;
#elif COMPOSITOR_DIAGNOSTIC_MODE == 2 // B: full composite + damage present
    composite_full();
    ok = present_damage(); // still compacts g_damage[] to failures-only, harmless here
#elif COMPOSITOR_DIAGNOSTIC_MODE == 3 // C: damage composite + full present
    composite_damage_scoped();
    ok = present_full();
    g_damage_count = 0;
#else // D / default: damage composite + damage present (unchanged real path)
    composite_damage_scoped();
    ok = present_damage(); // every pending rect (old retries + new) succeeded this pass?
#endif
    if (ok) {
        g_presented_generation = g_composed_generation;
        g_front_partial = 0;
    } else {
        g_front_partial = 1; // some regions still pending -- g_damage_count already reflects exactly which
    }

#ifdef COMPOSITOR_DAMAGE_TRACE
    // Only record this pass as "presented" if it actually fully was --
    // see compose_and_present_full()'s own identical reasoning.
    if (ok) trace_record_presented();
#endif
#ifdef COMPOSITOR_PERF_STATS
    g_stat_damage_frames++;
#endif
#ifdef COMPOSITOR_FRONT_CONSISTENCY_CHECK
    m8_check_front_consistency();
#endif
}

#if defined(M8_FAULT_INJECT) && defined(M8_FAULT_INJECT_SELFTEST)
// Forward-declared: real definition (input event -> cursor/window state)
// lives further down; this self-test's hardware-cursor case needs it
// earlier in the file.
static void handle_input_event(const input64_event_t* ev);

// M+8 deterministic fault-injection self-test (§12 of this milestone's
// own spec) -- proves the whole front/back/generation contract against
// the REAL running compositor (real g_backbuffer, real g_frontbuffer,
// real sys_display_present), not a synthetic stand-in. Compile-time
// gated behind BOTH flags; never active production behavior. Called
// once from _start(), after the initial full draw has already
// established a valid front image.
static int m8_st_ok = 1;
static void m8_st_check(const char* name, int cond) {
    put(cond ? "M8SELFTEST PASS: " : "M8SELFTEST FAIL: ");
    put(name); put("\n");
    if (!cond) m8_st_ok = 0;
}
static void m8_selftest_fault_injection(void) {
    put("M8SELFTEST: starting\n");

    // The initial compose_and_present_full() call (immediately before
    // this self-test runs) never clears g_full_redraw itself -- only the
    // main loop's own wrapper does, and it hasn't run its first
    // iteration yet at this point (see that call site's own comment:
    // "g_full_redraw is also already 1, kept in sync below"). Since a
    // valid full frame IS already presented (that initial call
    // succeeded), it's correct to start this test's damage tracking from
    // a clean slate -- otherwise add_damage_rect() below is a silent
    // no-op (it deliberately does nothing while g_full_redraw is set),
    // which was the actual cause of every case failing the first time
    // this self-test was run.
    g_full_redraw = 0;

    // Case 1: single rect, injected failure -- front must stay
    // unchanged, damage must retain the rect. Calls present_damage()
    // DIRECTLY, not the full compose_and_present_damage() wrapper --
    // composite_damage_scoped() (which that wrapper calls first) repaints
    // every damaged rect from CURRENT SCENE STATE (background, since no
    // window exists yet this early in _start()), which would overwrite
    // this test's own manually-painted pixels before present_rect() ever
    // saw them. present_damage()/present_rect() are exactly the units
    // this test needs to exercise in isolation -- composition itself is
    // already proven correct by this whole session's own damage-tracking
    // history and is not what's under test here.
    rect_t r1 = { 0, 0, 8, 8 };
    uint32_t before = g_frontbuffer[0];
    bb_fill_rect(r1.x0, r1.y0, 8, 8, 0x00FF00FFu); // distinct test color
    add_damage_rect(r1);
    m8_fault_inject_arm(0); // fail the very next present_rect() call
    present_damage();
    m8_st_check("failed present leaves front unchanged", g_frontbuffer[0] == before);
    m8_st_check("failed present retains damage", g_damage_count == 1);
    m8_st_check("backbuffer already has the new pixels regardless", g_backbuffer[0] == 0x00FF00FFu);

    // Retry, no injection this time -- must now succeed and retire.
    present_damage();
    m8_st_check("retry succeeds and updates front", g_frontbuffer[0] == 0x00FF00FFu);
    m8_st_check("retry retires damage", g_damage_count == 0);

    // Case 2: three rects, fail exactly the middle one.
    rect_t ra = { 20, 0, 28, 8 }, rb = { 40, 0, 48, 8 }, rc = { 60, 0, 68, 8 };
    bb_fill_rect(ra.x0, ra.y0, 8, 8, 0x00AA0000u);
    bb_fill_rect(rb.x0, rb.y0, 8, 8, 0x0000AA00u);
    bb_fill_rect(rc.x0, rc.y0, 8, 8, 0x000000AAu);
    add_damage_rect(ra); add_damage_rect(rb); add_damage_rect(rc);
    m8_fault_inject_arm(1); // let 1 through (ra), fail the next (rb)
    present_damage();
    m8_st_check("rect A succeeded", g_frontbuffer[(uint32_t)ra.y0 * g_disp_w + (uint32_t)ra.x0] == 0x00AA0000u);
    m8_st_check("rect C succeeded despite B failing", g_frontbuffer[(uint32_t)rc.y0 * g_disp_w + (uint32_t)rc.x0] == 0x000000AAu);
    m8_st_check("rect B still pending", g_damage_count == 1 && g_damage[0].x0 == rb.x0);
    present_damage();
    m8_st_check("rect B succeeds on retry", g_frontbuffer[(uint32_t)rb.y0 * g_disp_w + (uint32_t)rb.x0] == 0x0000AA00u);
    m8_st_check("all damage retired", g_damage_count == 0);

    // Case 3: full-screen redraw, injected failure, then retry -- the
    // full-redraw counterpart to cases 1/2 above, requested explicitly
    // before final acceptance since compose_and_present_full()/
    // present_full() are the OTHER half of the changed presentation
    // lifecycle (compose_and_present_damage()/present_damage() alone
    // don't exercise this path at all).
    m8_fault_inject_arm(0);
    int full_ok1 = compose_and_present_full();
    m8_st_check("failed full redraw reports failure", full_ok1 == 0);
    m8_st_check("failed full redraw leaves front_partial set", g_front_partial == 1);
    int full_ok2 = compose_and_present_full();
    m8_st_check("retried full redraw succeeds", full_ok2 == 1);
    m8_st_check("front fully valid again after retry", g_front_partial == 0 && g_front_valid == 1);

    // Case 4: rapid A -> B -> C mutation of the SAME region before it is
    // ever presented -- only the final state (C) should ever reach
    // g_frontbuffer; B must never be independently observed there.
    // add_damage_rect()'s own coalescing (same rect touched repeatedly)
    // means this also proves repeated damage to one region collapses
    // correctly instead of accumulating duplicate entries.
    rect_t rt = { 80, 0, 88, 8 };
    int damage_before_abc = g_damage_count;
    bb_fill_rect(rt.x0, rt.y0, 8, 8, 0x00111111u); add_damage_rect(rt); // A
    bb_fill_rect(rt.x0, rt.y0, 8, 8, 0x00222222u); add_damage_rect(rt); // B
    bb_fill_rect(rt.x0, rt.y0, 8, 8, 0x00333333u); add_damage_rect(rt); // C -- final
    m8_st_check("rapid A->B->C coalesces into exactly one damage rect", g_damage_count == damage_before_abc + 1);
    present_damage();
    m8_st_check("only the final state C is presented, never intermediate B",
                g_frontbuffer[(uint32_t)rt.y0 * g_disp_w + (uint32_t)rt.x0] == 0x00333333u);

    // Case 5: hardware cursor motion must add ZERO compositor damage and
    // cause ZERO backbuffer/frontbuffer modification -- it bypasses this
    // entire path via sys_cursor_move()/MOVE_CURSOR (see
    // handle_input_event()'s own POINTER_REL branch). Only meaningful
    // when a hardware cursor backend actually came up this boot.
    if (g_hw_cursor_active) {
        int damage_before_cursor = g_damage_count;
        uint32_t front_snapshot = g_frontbuffer[0];
        input64_event_t ev; for (uint64_t i = 0; i < sizeof(ev); i++) ((char*)&ev)[i] = 0;
        ev.type = (uint32_t)INPUT64_EVENT_POINTER_REL;
        ev.a = 5; ev.b = 5;
        handle_input_event(&ev);
        m8_st_check("hardware cursor motion adds zero compositor damage", g_damage_count == damage_before_cursor);
        m8_st_check("hardware cursor motion does not touch g_frontbuffer", g_frontbuffer[0] == front_snapshot);
    } else {
        put("M8SELFTEST: hardware cursor not active this boot -- case 5 skipped (not a failure)\n");
    }

    // Restore: force a full redraw so the test pattern never lingers
    // into normal operation.
    g_full_redraw = 1;

    put(m8_st_ok ? "M8SELFTEST: all cases PASS\n" : "M8SELFTEST: at least one case FAILED\n");
}
#endif

// ── Client message handling ─────────────────────────────────────────
static int find_free_window_slot(void) {
    for (int i = 0; i < MAX_WINDOWS; i++) if (!g_windows[i].in_use) return i;
    return -1;
}

// M+10A: records that `w` is still actively driving the protocol -- see
// window_t's own last_activity_tick/last_msg_type_received comment.
// Called once per successfully-dispatched message that resolved a real
// window, from inside handle_client_message() below.
static inline void touch_window_activity(window_t* w, uint32_t msg_type) {
    w->last_activity_tick = sys_get_ticks();
    w->last_msg_type_received = msg_type;
    w->watchdog_fired = 0;
}

static void handle_client_message(int conn_index, const wm_msg_t* msg) {
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
        w->committed_slot = -1; // M+5: 0 (zero-init default) would otherwise misread as "slot 0 is committed" -- explicit until a real WM_MSG_COMMIT_BUFFER sets it
        w->scanout_owned_slot = -1; // M+9B: same reasoning -- 0 would otherwise misread as "slot 0 is scanout-owned"
        w->last_activity_tick = sys_get_ticks(); // M+10A: seed to "now", not the zero-init default -- see window_t's own field comment
        for (int si = 0; si < WM_MAX_BUFFERS_PER_WINDOW; si++) w->slot_release_ground_truth[si] = 1; // M+10A: nothing committed yet -- every slot starts "free"
        int ti = 0; while (ti < WM_TITLE_MAX && msg->title[ti]) { w->title[ti] = msg->title[ti]; ti++; }
        w->title[ti] = 0;
        // M+12A: placement is now a policy DECISION (compositor still owns
        // applying it) -- see this file's own "compositor policy boundary"
        // section for why. g_zorder insertion and auto-focus/activate below
        // stay ordinary mechanism this phase (no hook -- see that section's
        // header comment on why they're deferred, not permanently mechanism).
        int32_t px, py;
        g_policy->place_new_window(w, idx, &px, &py);
        w->x = px; w->y = py;
        g_zorder[g_zorder_count++] = idx;
        set_keyboard_focus_ex(idx, 0); // don't notify -- WINDOW_CREATED reply is still pending on this same connection
        activate_window(idx);
#ifdef M12A_POLICY_TRACE
        trace_window_placed(w);
#endif

        wm_msg_t reply; for (uint64_t i = 0; i < sizeof(reply); i++) ((char*)&reply)[i] = 0;
        reply.type = WM_MSG_WINDOW_CREATED; reply.version = WM_PROTO_VERSION; reply.window_id = w->window_id;
        queue_event(conn_index, &reply);
        // activate_window() above already damaged this window (it's the
        // new active target); nothing further needed for a brand-new,
        // still-contentless window.
        break;
    }
    case WM_MSG_DESTROY_WINDOW: {
        window_t* w = find_window_owned_by(conn_index, msg->window_id);
        if (!w) { send_error(conn_index, msg->window_id, WM_ERR_BAD_WINDOW); break; }
        // Damage the CLOSED window's own rect BEFORE removing it --
        // whatever was underneath (desktop background, or another
        // window it was overlapping) gets correctly re-exposed when
        // compose_and_present_damage() redraws that rect from scratch
        // and repaints every window still in z-order that intersects
        // it, in order.
        add_damage_window(w);
        close_window(window_index_of(w));
        break;
    }
    case WM_MSG_ATTACH_SURFACE: {
        window_t* w = find_window_owned_by(conn_index, msg->window_id);
        if (!w) { send_error(conn_index, msg->window_id, WM_ERR_BAD_WINDOW); break; }
        touch_window_activity(w, msg->type);
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
        // A (re)attached surface may be a different size than before,
        // so damage the window's full current rect rather than trying
        // to reason about old-vs-new size deltas -- simple and correct.
        add_damage_window(w);
        break;
    }
    case WM_MSG_COMMIT: {
        window_t* w = find_window_owned_by(conn_index, msg->window_id);
        if (!w) { send_error(conn_index, msg->window_id, WM_ERR_BAD_WINDOW); break; }
        touch_window_activity(w, msg->type);
        damage_content_rect(w, msg->x, msg->y, msg->w, msg->h);
        break;
    }
    case WM_MSG_ATTACH_BUFFER: {
        window_t* w = find_window_owned_by(conn_index, msg->window_id);
        if (!w) { send_error(conn_index, msg->window_id, WM_ERR_BAD_WINDOW); break; }
        touch_window_activity(w, msg->type);
        // A window uses EXACTLY ONE surface model for its whole life --
        // never mixed. has_surface here means "already used the LEGACY
        // WM_MSG_ATTACH_SURFACE path", which is mutually exclusive with
        // committed buffers.
        if (w->has_surface && !w->uses_committed_buffers) {
            send_error(conn_index, msg->window_id, WM_ERR_BAD_SURFACE); break;
        }
        if (msg->w == 0 || msg->h == 0 || msg->w > 4096 || msg->h > 4096) {
            send_error(conn_index, msg->window_id, WM_ERR_BAD_SIZE); break;
        }
        if (msg->format != WM_FORMAT_XRGB8888 || msg->stride < msg->w * 4) {
            send_error(conn_index, msg->window_id, WM_ERR_BAD_SURFACE); break;
        }
        uint64_t required = (uint64_t)(msg->h - 1) * msg->stride + (uint64_t)msg->w * 4;

        int slot = -1;
        for (int i = 0; i < WM_MAX_BUFFERS_PER_WINDOW; i++) if (!w->buffers[i].in_use) { slot = i; break; }
        if (slot < 0) { send_error(conn_index, msg->window_id, WM_ERR_NO_RESOURCES); break; } // both slots already attached -- M+5 is strictly double-buffered

        int64_t shm_h = sys_shm_open_token(msg->shm_token);
        if (shm_h < 0) { send_error(conn_index, msg->window_id, WM_ERR_BAD_SURFACE); break; }
        int64_t actual = sys_shm_size((int)shm_h);
        if (actual < 0 || (uint64_t)actual < required) {
            sys_handle_close((int)shm_h);
            send_error(conn_index, msg->window_id, WM_ERR_BAD_SURFACE);
            break;
        }
        uint64_t addr = sys_shm_map((int)shm_h, 0 /* read-only -- the compositor never writes a client's buffer */);
        if (addr == (uint64_t)-1) {
            sys_handle_close((int)shm_h);
            send_error(conn_index, msg->window_id, WM_ERR_BAD_SURFACE);
            break;
        }

        w->uses_committed_buffers = 1;
        w->buffers[slot].in_use = 1;
        w->buffers[slot].token = msg->shm_token;
        w->buffers[slot].shm_handle = (int)shm_h;
        w->buffers[slot].addr = addr;
        w->buffers[slot].w = msg->w; w->buffers[slot].h = msg->h; w->buffers[slot].stride = msg->stride;
        w->buffers[slot].bytes = (uint64_t)actual;
        send_ack(conn_index, msg->window_id);
        // Deliberately NOT displayed yet -- attaching a buffer is not a
        // commit (see this message's own wmproto64.h comment); no damage
        // is added here.
        break;
    }
    case WM_MSG_COMMIT_BUFFER: {
        window_t* w = find_window_owned_by(conn_index, msg->window_id);
        if (!w) { send_error(conn_index, msg->window_id, WM_ERR_BAD_WINDOW); break; }
        touch_window_activity(w, msg->type);
        if (!w->uses_committed_buffers) { send_error(conn_index, msg->window_id, WM_ERR_BAD_SURFACE); break; }

        int slot = -1;
        for (int i = 0; i < WM_MAX_BUFFERS_PER_WINDOW; i++) {
            if (w->buffers[i].in_use && w->buffers[i].token == msg->shm_token) { slot = i; break; }
        }
        if (slot < 0) { send_error(conn_index, msg->window_id, WM_ERR_BAD_SURFACE); break; } // stale/unknown/already-retired buffer token

        // M+10A protocol redesign (real enforcement, not diagnostic-only
        // -- see the M+10A follow-up audit): COMMIT_BUFFER's own
        // `generation` is ToxenOS's implicit acknowledgement of whichever
        // CONFIGURE the content answers. Three cases, checked BEFORE any
        // state mutation at all:
        //   generation == current_configure_generation -- valid, proceed
        //     normally below (this covers the pre-CONFIGURE case too:
        //     both start at 0, so a window's very first commits, made
        //     before ever toggling fullscreen, are always "current").
        //   generation <  current_configure_generation -- stale. The
        //     client rendered for a configure that has since been
        //     superseded (e.g. it fired a second SET_FULLSCREEN before
        //     dispatching the first's own CONFIGURE reply). Must NEVER
        //     become current content, touch committed_slot, enter direct
        //     scanout, or affect frame-callback bookkeeping -- but the
        //     buffer itself must not be stranded either: the client
        //     already marked it owned_by_compositor the instant it sent
        //     this message (wmclient64.h's own wm_commit_buffer()), so
        //     it is given back immediately via the ordinary release
        //     path, exactly as if the compositor had briefly held then
        //     discarded it.
        //   generation >  current_configure_generation -- impossible
        //     under a well-behaved client (it can never have applied a
        //     CONFIGURE this compositor never sent), diagnosed loudly as
        //     a real protocol violation, and rejected the same way.
        if (msg->generation < w->configure_generation) {
            g_diag_stale_commit_generation_count++;
            if (!g_diag_stale_commit_generation_reported) {
                g_diag_stale_commit_generation_reported = 1;
                put("*** M10A: COMMIT_BUFFER answers a superseded CONFIGURE generation -- rejected ***\n");
                put_kv("  window_id=", w->window_id);
                put_kv("  commit_generation=", (uint64_t)msg->generation);
                put_kv("  current_configure_generation=", (uint64_t)w->configure_generation);
            }
            w->slot_release_ground_truth[slot] = 0; // ownership was transferred by this commit regardless of acceptance -- see notify_buffer_released()'s own comment
            notify_buffer_released(w, slot); // give it straight back -- never adopted, never stranded
            // M+10A follow-up audit: NO reply is sent here (nor on the
            // other two COMMIT_BUFFER outcomes below) -- see this file's
            // own comment at the accepted-path's send_ack() removal for
            // why a real client-observed bug makes this mandatory, not
            // just consistent with COMMIT_BUFFER's original "no reply on
            // success" design intent.
            break;
        }
        if (msg->generation > w->configure_generation) {
            g_diag_impossible_commit_generation_count++;
            if (!g_diag_impossible_commit_generation_reported) {
                g_diag_impossible_commit_generation_reported = 1;
                put("*** M10A: COMMIT_BUFFER generation EXCEEDS current CONFIGURE -- protocol violation, rejected ***\n");
                put_kv("  window_id=", w->window_id);
                put_kv("  commit_generation=", (uint64_t)msg->generation);
                put_kv("  current_configure_generation=", (uint64_t)w->configure_generation);
            }
            w->slot_release_ground_truth[slot] = 0;
            notify_buffer_released(w, slot);
            break;
        }

        // Valid: generation == current_configure_generation. The atomic
        // swap point: window_t.committed_slot changes in one assignment,
        // so any compositor frame that reads it (compose_window(), via
        // w->committed_slot) sees either the fully-old or fully-new
        // buffer, never a mix -- see wmproto64.h's own comment on why
        // this is enough without a bigger transaction mechanism.
        int old_slot = w->committed_slot;
        w->committed_slot = slot;
        w->slot_release_ground_truth[slot] = 0; // M+10A: this slot is owned again as of this commit -- see notify_buffer_released()'s own comment
        w->content_w = w->buffers[slot].w; w->content_h = w->buffers[slot].h; // geometry always comes from the committed buffer itself, never inferred
        // BUG FIX (post-M+12A audit): this used to also set `w->has_surface = 1`
        // here, "generically" marking the window as having displayable content.
        // It must NOT -- see window_t's own has_surface field comment for the
        // invariant. A committed-buffer window is represented by
        // uses_committed_buffers/committed_slot alone; compose_window() never
        // even reads has_surface for one (it branches on uses_committed_buffers
        // first). The only real effect of that stray write was on close: it made
        // free_window_surface() treat this window as if a real WM_MSG_ATTACH_SURFACE
        // had populated w->shm_handle -- which for a buffer-model window is still
        // its zero-initialized default (0), the same number as the compositor's
        // OWN display handle (opened first, at startup, before any client
        // exists). Closing ANY window that had ever committed a buffer therefore
        // closed the compositor's own display handle out from under it, and
        // every present -- for every window, for the rest of the session --
        // failed silently from that point on (see present_rect()'s own
        // g_diag_present_failure_count for how this is now caught if it ever
        // regresses).

        // M+10A follow-up audit: send_ack() REMOVED here. COMMIT_BUFFER
        // was always documented as "no reply on success, client never
        // reads it" -- but sending one anyway used to be harmless
        // ONLY because ACK carries no correlation ID and every consumer
        // discarded it unconditionally. The M+10A redesign changed that:
        // wm_attach_buffer()'s own wmc_wait_reply(ACK) treats ANY ACK as
        // proof ITS OWN request succeeded, with no way to tell it apart
        // from an unrelated, still-undrained COMMIT_BUFFER ACK sitting
        // ahead of it on the wire (wm_commit_buffer() never waits for
        // its own reply). Live stress testing proved this reachable: a
        // stale COMMIT_BUFFER's ACK satisfied a LATER wm_attach_buffer()
        // call, which then reported success while the compositor had
        // never actually processed that ATTACH_BUFFER -- the client's
        // own buffer bookkeeping (locally computed shm token/mapping)
        // stayed self-consistent, but the compositor's matching
        // window_t.buffers[] slot was simply never populated, so a
        // later commit to it could be released back but never adopted,
        // permanently confusing the client's own alternation index.
        // Removing this reply is the correct fix at the root: nothing
        // has ever consumed it, and its mere presence on the wire was
        // the entire hazard.
#ifdef COMPOSITOR_PERF_STATS
        g_stat_commit_buffer++;
#endif
        int win_idx = window_index_of(w);
#ifdef M6_FRAME_TRACE
        {
            g_trace_commit_seq[win_idx]++;
            put("M6TRACE COMMIT\n");
            put_kv("  win=", w->window_id);
            put_kv("  seq=", g_trace_commit_seq[win_idx]);
            put_kv("  tick=", sys_get_ticks());
        }
#endif

        // M+9B: give the eligible fullscreen window's own commit a
        // chance to bypass composition entirely (§3-§8) BEFORE any
        // normal damage/release bookkeeping runs -- a window that isn't
        // fullscreen (the overwhelmingly common case) fails eligibility
        // instantly and this is a cheap no-op, falling through to the
        // unmodified M+5 path below exactly as before.
        if (w->is_fullscreen && direct_scanout_try_enter_or_update(w, win_idx, slot, old_slot)) {
            break; // handled entirely through direct scanout -- §15: zero compositor damage this frame
        }

        damage_content_rect(w, msg->x, msg->y, msg->w, msg->h);

        // Notify the buffer this commit just retired -- see
        // WM_MSG_BUFFER_RELEASED's own header comment on why this is
        // correct to do IMMEDIATELY (no async GPU fence tracking needed
        // yet): compose_window() only ever reads w->committed_slot's
        // CURRENT value, so the old buffer is provably never read again
        // the instant the assignment above happened. Notify only --
        // notify_buffer_released() leaves the slot attached (in_use,
        // still mapped) since the client will commit this exact buffer
        // again on a future frame without ever re-attaching it.
        // M+10A protocol redesign: only ONE case needs checking here now
        // (still actively scanout-owned -- direct_scanout_try_enter_or_update()/
        // force_leave() are the only two places that release such a slot,
        // always after confirming hardware no longer references it --
        // defer to them). The second case the OLD scalar-based logic
        // needed here (a slot already released moments ago, out of band,
        // by one of those same two functions) no longer needs a check AT
        // ALL: notify_buffer_released() is now itself idempotent via
        // slot_release_ground_truth[] (see its own comment), so simply
        // calling it unconditionally for a slot that's ALREADY been
        // released is a correct, silent no-op rather than a duplicate
        // wire message -- this is exactly what closes the M+10A audit's
        // proven bug (a single scalar cannot represent two independent
        // out-of-band releases in flight for two different slots at
        // once; per-slot ground truth structurally can).
        if (old_slot >= 0 && old_slot != slot) {
            if (old_slot == w->scanout_owned_slot) {
#ifdef M9B_STATS
                g_stat_deferred_scanout_releases++;
#endif
            } else {
                notify_buffer_released(w, old_slot);
            }
        }
        break;
    }
    // M+6: see wmproto64.h's own WM_MSG_REQUEST_FRAME comment for the
    // full protocol contract. No reply on success -- the eventual
    // WM_MSG_FRAME (sent from dispatch_frame_callbacks(), main loop) IS
    // the reply.
    case WM_MSG_REQUEST_FRAME: {
        window_t* w = find_window_owned_by(conn_index, msg->window_id);
        if (!w) { send_error(conn_index, msg->window_id, WM_ERR_BAD_WINDOW); break; }
        touch_window_activity(w, msg->type);
        if (!w->uses_committed_buffers) { send_error(conn_index, msg->window_id, WM_ERR_BAD_SURFACE); break; }
        w->frame_callback_pending = 1; // idempotent -- at most one outstanding grant per window, regardless of repeat requests
        w->ever_requested_frame = 1; // M+10A: see window_t's own field comment -- gates watchdog eligibility
#ifdef COMPOSITOR_PERF_STATS
        g_stat_frame_requests++;
#endif
#ifdef M6_FRAME_TRACE
        put("M6TRACE REQUEST_FRAME received\n");
        put_kv("  win=", w->window_id);
        put_kv("  tick=", sys_get_ticks());
#endif
        break;
    }
    // M+9A: real generic compositor fullscreen -- see wmproto64.h's own
    // WM_MSG_SET_FULLSCREEN/WM_MSG_CONFIGURE comments for the protocol
    // contract. Entirely composited: no VirtIO/scanout concept appears
    // anywhere here or in window_t -- that split is M+9B's own concern,
    // strictly gated behind this milestone passing first.
    case WM_MSG_SET_FULLSCREEN: {
        window_t* w = find_window_owned_by(conn_index, msg->window_id);
        if (!w) { send_error(conn_index, msg->window_id, WM_ERR_BAD_WINDOW); break; }
        touch_window_activity(w, msg->type);

        int want_fullscreen = msg->pressed ? 1 : 0;
        if (want_fullscreen && !w->is_fullscreen) {
            // Entering: save the exact pre-fullscreen geometry (the ONLY
            // copy of it that survives -- nothing else on window_t
            // remembers the old decorated position/size once x/y/
            // content_w/content_h are overwritten below), then take over
            // the entire output. Raised to the top of z-order and
            // focused so nothing else appears above it -- already
            // correct, expected behavior for plain composited fullscreen
            // on its own, and exactly the property M+9B will later add a
            // stricter, VirtIO-aware eligibility check on top of.
            w->saved_x = w->x; w->saved_y = w->y;
            w->saved_content_w = w->content_w; w->saved_content_h = w->content_h;
            add_damage_window(w); // old (decorated) rect, before geometry changes
            w->is_fullscreen = 1;
            w->x = 0; w->y = 0;
            w->content_w = g_disp_w; w->content_h = g_disp_h;
            int idx = window_index_of(w);
            if (idx >= 0) { raise_window(idx); set_keyboard_focus(idx); activate_window(idx); }
            add_damage_window(w); // new (fullscreen) rect
        } else if (!want_fullscreen && w->is_fullscreen) {
            // M+9B §11/§12: an explicit fullscreen-exit request ends
            // direct scanout immediately rather than waiting for this
            // window's next commit to fail its own eligibility recheck
            // (which would also catch it, one frame interval later --
            // this is promptness, not a correctness requirement).
            {
                int exiting_idx = window_index_of(w);
                if (g_presented_mode == PRESENTED_DIRECT && g_direct_window_idx == exiting_idx) direct_scanout_force_leave();
            }
            // Exiting: restore exactly the geometry captured on entry --
            // never re-derived or guessed.
            add_damage_window(w); // old (fullscreen) rect
            w->is_fullscreen = 0;
            w->x = w->saved_x; w->y = w->saved_y;
            w->content_w = w->saved_content_w; w->content_h = w->saved_content_h;
            add_damage_window(w); // new (restored) rect
        }
        // else: already in the requested state -- idempotent no-op,
        // still configured below so a client can't get stuck waiting for
        // a reply it will never otherwise see.

        // M+10A protocol redesign: monotonic per-window generation,
        // never reused, incremented on every CONFIGURE this window is
        // EVER sent (including a no-op re-request of the current state
        // -- simpler than special-casing that, and still safe: "multiple
        // CONFIGURE messages must be safe" per wmproto64.h's own
        // comment).
        w->configure_generation++;
        wm_msg_t cfg; for (uint64_t i = 0; i < sizeof(cfg); i++) ((char*)&cfg)[i] = 0;
        cfg.type = WM_MSG_CONFIGURE; cfg.version = WM_PROTO_VERSION; cfg.window_id = w->window_id;
        cfg.w = w->content_w; cfg.h = w->content_h; cfg.pressed = (uint32_t)w->is_fullscreen;
        cfg.generation = w->configure_generation;
        queue_event(conn_index, &cfg);
        // M+10A follow-up audit: NO trailing WM_MSG_ACK here anymore --
        // see the COMMIT_BUFFER handler's own comment on why. wm_set_fullscreen()
        // is fire-and-forget (never waits for any reply, including this
        // one), so this ACK was ALSO purely floating, undrained wire
        // noise that a later wm_attach_buffer() call's own
        // wmc_wait_reply(ACK) could wrongly consume as proof of ITS OWN
        // success -- exactly the same hazard COMMIT_BUFFER's removed ACK
        // posed, and just as reachable here: GfxDemo calls
        // wm_attach_buffer() for its fullscreen pair immediately after a
        // SET_FULLSCREEN transition confirms fullscreen. CONFIGURE
        // (above) is the only reply anything ever actually consumes.
        break;
    }
    case WM_MSG_SET_TITLE: {
        window_t* w = find_window_owned_by(conn_index, msg->window_id);
        if (!w) { send_error(conn_index, msg->window_id, WM_ERR_BAD_WINDOW); break; }
        int ti = 0; while (ti < WM_TITLE_MAX && msg->title[ti]) { w->title[ti] = msg->title[ti]; ti++; }
        w->title[ti] = 0;
        // The titlebar is a small part of the window, but damaging the
        // whole window rect keeps this simple and correct rather than
        // computing the titlebar's own sub-rect specially -- title
        // changes are rare and not performance-sensitive.
        add_damage_window(w);
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
static void handle_input_event(const input64_event_t* ev) {
    if (ev->type == (uint32_t)INPUT64_EVENT_KEY) {
        // The one authoritative routing rule (this file's own seat64
        // audit): KEY_EVENT goes to g_keyboard_focus and nothing else --
        // not pointer focus, not the last hit-tested window, not
        // g_dragging, not g_active_window.
        if (g_keyboard_focus < 0) return;
        window_t* w = &g_windows[g_keyboard_focus];
        wm_msg_t m; for (uint64_t i = 0; i < sizeof(m); i++) ((char*)&m)[i] = 0;
        m.type = WM_MSG_KEY_EVENT; m.version = WM_PROTO_VERSION; m.window_id = w->window_id;
        m.key_code = (uint32_t)ev->a; m.pressed = ev->pressed; m.modifiers = ev->modifiers; m.ascii = ev->ascii;
        queue_event(w->conn_index, &m);
        return;
    }

    if (ev->type == (uint32_t)INPUT64_EVENT_POINTER_REL || ev->type == (uint32_t)INPUT64_EVENT_POINTER_ABS) {
        int32_t nx, ny;
        if (ev->type == (uint32_t)INPUT64_EVENT_POINTER_ABS) {
            // M+7A: normalize the device's own raw range to THIS boot's
            // actual logical display geometry -- never hardcoded against
            // any particular resolution. g_abs_range is fetched once at
            // startup (see _start()); a zero-width device range (should
            // never happen for a real VirtIO tablet, defended anyway)
            // falls back to display center rather than dividing by zero.
            int32_t rx = g_abs_range.max_x - g_abs_range.min_x;
            int32_t ry = g_abs_range.max_y - g_abs_range.min_y;
            nx = rx > 0 ? (int32_t)(((int64_t)(ev->a - g_abs_range.min_x) * ((int64_t)g_disp_w - 1)) / rx) : (int32_t)(g_disp_w / 2);
            ny = ry > 0 ? (int32_t)(((int64_t)(ev->b - g_abs_range.min_y) * ((int64_t)g_disp_h - 1)) / ry) : (int32_t)(g_disp_h / 2);
        } else {
            nx = g_cursor_x + ev->a;
            ny = g_cursor_y + ev->b;
        }
        int32_t old_x = g_cursor_x, old_y = g_cursor_y;
        if (nx < 0) nx = 0;
        if (ny < 0) ny = 0;
        if (nx >= (int32_t)g_disp_w) nx = (int32_t)g_disp_w - 1;
        if (ny >= (int32_t)g_disp_h) ny = (int32_t)g_disp_h - 1;
        g_cursor_x = nx; g_cursor_y = ny;

        if (nx != old_x || ny != old_y) {
            // M+7: with the hardware cursor active, position updates
            // bypass the desktop damage/composite/present path ENTIRELY
            // -- no cursor damage rect, no window repaint underneath, no
            // copy into g_backbuffer, no ordinary present call caused by
            // cursor motion alone. Just the one cheap MOVE_CURSOR
            // command through the generic display64 cursor abstraction.
            if (g_hw_cursor_active) {
                if (output64_cursor_move(nx, ny) < 0) {
                    // Runtime failure after having worked at startup --
                    // fall back to software cursor cleanly (this
                    // milestone's own item 8): disable the hardware
                    // cursor plane (best-effort; its own failure doesn't
                    // matter, we're abandoning it either way), then
                    // damage the cursor's current rect ONCE so the next
                    // composite pass draws it in software starting from
                    // here -- no ghost, no double cursor, no stale
                    // baked-in image, since bb_draw_cursor() was never
                    // called anywhere while hardware cursor was active.
                    g_hw_cursor_active = 0;
                    output64_cursor_set_visible(0);
                    add_damage_rect(cursor_rect_at(nx, ny));
                    // M+9B §13: software cursor is incompatible with
                    // direct scanout (nothing composites a cursor into a
                    // client's own buffer) -- a hardware-cursor runtime
                    // failure must end direct mode exactly like any
                    // other eligibility loss.
                    direct_scanout_force_leave();
                }
            } else {
                // M-next: §17.3 -- damage just the cursor's own old and
                // new rects (they usually overlap for ordinary mouse
                // speeds and coalesce into one small rect in
                // add_damage_rect) instead of the whole screen. If
                // nothing else is damaged this pacing tick,
                // compose_and_present_damage() touches only this small
                // area -- no window or background redraw anywhere else
                // at all.
#ifdef COMPOSITOR_PERF_STATS
                g_stat_cursor_damage_adds += 2;
#endif
                add_damage_rect(cursor_rect_at(old_x, old_y));
                add_damage_rect(cursor_rect_at(nx, ny));
            }
        }

        if (g_dragging >= 0) {
#ifdef COMPOSITOR_PERF_STATS
            g_stat_drag_damage_adds += 2;
#endif
            // The whole dragged window moves with the cursor -- damage
            // its old AND new rect so the background/whatever it swept
            // over gets redrawn (this is what actually prevents the
            // "stale trail" bug a naive cursor-only fix would have).
            window_t* dw = &g_windows[g_dragging];
            rect_t old_win = window_rect(dw);
            dw->x = g_cursor_x - g_drag_off_x;
            dw->y = g_cursor_y - g_drag_off_y;
            add_damage_rect(old_win);
            add_damage_window(dw);
            return;
        }
        int idx = hit_test_any(g_cursor_x, g_cursor_y);
        if (idx >= 0 && hit_test_content(&g_windows[idx], g_cursor_x, g_cursor_y)) {
            window_t* w = &g_windows[idx];
            wm_msg_t m; for (uint64_t i = 0; i < sizeof(m); i++) ((char*)&m)[i] = 0;
            m.type = WM_MSG_POINTER_MOTION; m.version = WM_PROTO_VERSION; m.window_id = w->window_id;
            m.x = g_cursor_x - (w->x + win_border_off(w));
            m.y = g_cursor_y - (w->y + win_border_off(w) + win_title_off(w));
            queue_event(w->conn_index, &m);
        }
        return;
    }

    if (ev->type == (uint32_t)INPUT64_EVENT_POINTER_BUTTON) {
        uint32_t btn = (uint32_t)ev->a;
        if (ev->pressed) {
            if (g_dragging < 0) {
                int idx = hit_test_any(g_cursor_x, g_cursor_y);
#ifdef M12A_POLICY_TRACE
                { put("M12A_POLICY_TRACE PRESS\n"); put_kv("  cursor_x=", (uint64_t)(int64_t)g_cursor_x); put_kv("  cursor_y=", (uint64_t)(int64_t)g_cursor_y); put_kvi("  hit_idx=", idx); put_kv("  btn=", (uint64_t)btn); }
#endif
                if (idx >= 0) {
                    // M+12A: POLICY DECIDES, COMPOSITOR EXECUTES -- see this
                    // file's own "compositor policy boundary" section. The
                    // decision itself (matches tinywl's own
                    // server_cursor_button: activate + focus on button PRESS
                    // only -- see this file's own seat64 audit) is unchanged
                    // from before this milestone; only WHERE it is made moved.
                    // Pointer motion alone, handled entirely separately above,
                    // never reaches this code and never touches
                    // g_keyboard_focus or g_active_window -- hovering another
                    // window cannot steal keyboard focus.
                    compositor_activation_decision_t act = g_policy->on_window_activate_request(idx);
                    // M-next: raising only actually changes what's on screen
                    // if this window wasn't ALREADY topmost -- the
                    // overwhelmingly common case is clicking inside the
                    // window you're already using, which should cost nothing
                    // to redraw. activate_window() below is already a no-op
                    // (and adds no damage) when idx is already active, via
                    // its own `g_active_window == idx` check -- this mirrors
                    // that for the z-order side.
                    if (act.raise && (g_zorder_count == 0 || g_zorder[g_zorder_count - 1] != idx)) {
                        add_damage_window(&g_windows[idx]);
                        raise_window(idx);
                    }
                    if (act.keyboard_focus) set_keyboard_focus(idx);
                    if (act.activate) activate_window(idx);
                    window_t* w = &g_windows[idx];
                    if (hit_test_titlebar(w, g_cursor_x, g_cursor_y)) {
                        compositor_titlebar_action_t tact = g_policy->on_titlebar_press(idx, btn);
                        if (tact == COMPOSITOR_TITLEBAR_ACTION_REQUEST_CLOSE) {
                            wm_msg_t m; for (uint64_t i = 0; i < sizeof(m); i++) ((char*)&m)[i] = 0;
                            m.type = WM_MSG_CLOSE_REQUEST; m.version = WM_PROTO_VERSION; m.window_id = w->window_id;
                            queue_event(w->conn_index, &m);
                        } else if (tact == COMPOSITOR_TITLEBAR_ACTION_BEGIN_DRAG) {
                            g_dragging = idx;
                            g_drag_off_x = g_cursor_x - w->x;
                            g_drag_off_y = g_cursor_y - w->y;
                        }
                    } else if (hit_test_content(w, g_cursor_x, g_cursor_y)) {
                        wm_msg_t m; for (uint64_t i = 0; i < sizeof(m); i++) ((char*)&m)[i] = 0;
                        m.type = WM_MSG_POINTER_BUTTON; m.version = WM_PROTO_VERSION; m.window_id = w->window_id;
                        m.button = btn; m.pressed = 1;
                        m.x = g_cursor_x - (w->x + win_border_off(w));
                        m.y = g_cursor_y - (w->y + win_border_off(w) + win_title_off(w));
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
                    m.x = g_cursor_x - (w->x + win_border_off(w));
                    m.y = g_cursor_y - (w->y + win_border_off(w) + win_title_off(w));
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
static void disconnect_stalled_conns(void) {
    for (int ci = 0; ci < MAX_CLIENTS; ci++) {
        if (g_conns[ci].in_use && g_conns[ci].pending_disconnect) {
            // M-next: a stalled client can own several windows scattered
            // anywhere on screen -- reasoning about each one's exact
            // exposed-background damage precisely isn't worth it for a
            // disconnect cleanup path, so this is exactly the
            // conservative case §17 says to just fall back on.
            mark_full_redraw();
            disconnect_conn(ci);
        }
    }
}

// M+12B: builds THIS iteration's FINAL WAIT_ANY interest set -- must be
// called only from the main loop's own final tail, AFTER both
// disconnect_stalled_conns() sweeps and the final flush_conn_queue()
// pass, never before, so g_conns[]'s evt_count/pending_disconnect
// fields are already this iteration's last word (see the main loop's
// own comment). `out` must have room for at least 2 + 2*MAX_CLIENTS
// entries; the actual count written never exceeds SYS64_WAIT_ANY_MAX
// (defensive -- see include/process64.h's own comment on why that
// bound is never actually reachable here: this compositor's own
// handle table, PROCESS64_MAX_HANDLES slots, cannot hold more handles
// than that regardless of MAX_CLIENTS's nominal array size).
// __attribute__((unused)): unused under a COMPOSITOR_LEGACY_POLL_MODE
// build, which never reaches the M+12B tail at all -- same precedent as
// M+12A's own g_default_policy under COMPOSITOR_POLICY_TEST_ALT.
static int __attribute__((unused)) build_wait_any_set(int listen_h, int* out) {
    int n = 0;
    out[n++] = g_input_h;
    if (n < SYS64_WAIT_ANY_MAX) out[n++] = listen_h;
    for (int ci = 0; ci < MAX_CLIENTS; ci++) {
        if (!g_conns[ci].in_use) continue;
        if (n < SYS64_WAIT_ANY_MAX) out[n++] = g_conns[ci].req_r;
        // Milestone invariant: evt_w joins the wait set ONLY when the
        // final flush this same iteration left real data queued for a
        // connection that isn't already a lost cause -- see
        // flush_conn_queue()'s own WOULDBLOCK-leaves-evt_count-nonzero
        // behavior. An always-writable pipe with an empty queue must
        // NEVER be waited on: that would make WAIT_ANY return
        // immediately every time and silently recreate busy polling.
        if (g_conns[ci].evt_count > 0 && !g_conns[ci].pending_disconnect && n < SYS64_WAIT_ANY_MAX) {
            out[n++] = g_conns[ci].evt_w;
        }
    }
    return n;
}

// M+12B: "what is the earliest reason I need to run again, given the
// state exactly as I am about to sleep" -- computed from FINAL,
// post-composition/post-frame-callback-dispatch/post-watchdog state
// (the caller runs this only after all of that, per the main loop's own
// tail ordering), so it never sleeps past a deadline this SAME
// iteration's own work just created. Returns SYS64_WAIT_FOREVER if
// nothing local is pending at all. The one deliberate exception to
// "zero periodic wakes": a window with an armed-but-not-yet-fired
// liveness watchdog has no external event source of its own, so its
// own deadline caps the wait -- see check_liveness_watchdog()'s own
// comment for why that check alone can never produce a wake by itself.
static uint64_t __attribute__((unused)) compute_wait_any_timeout(uint64_t now, uint64_t last_frame_tick, uint64_t last_callback_tick) {
    uint64_t deadline = SYS64_WAIT_FOREVER;

    if (g_presented_mode != PRESENTED_DIRECT && (g_full_redraw || g_damage_count > 0)) {
        uint64_t due = last_frame_tick + COMPOSITOR_PACING_TICKS;
        uint64_t rem = (due > now) ? (due - now) : 0;
        if (deadline == SYS64_WAIT_FOREVER || rem < deadline) deadline = rem;
    }

    int any_callback_pending = 0;
    for (int i = 0; i < MAX_WINDOWS; i++) {
        if (g_windows[i].in_use && g_windows[i].uses_committed_buffers && g_windows[i].frame_callback_pending) {
            any_callback_pending = 1;
            break;
        }
    }
    if (any_callback_pending) {
        uint64_t due = last_callback_tick + COMPOSITOR_PACING_TICKS;
        uint64_t rem = (due > now) ? (due - now) : 0;
        if (deadline == SYS64_WAIT_FOREVER || rem < deadline) deadline = rem;
    }

    for (int i = 0; i < MAX_WINDOWS; i++) {
        window_t* w = &g_windows[i];
        if (!w->in_use || !w->uses_committed_buffers || !w->ever_requested_frame || w->watchdog_fired) continue;
        uint64_t due = w->last_activity_tick + M10A_WATCHDOG_TICKS;
        uint64_t rem = (due > now) ? (due - now) : 0;
        if (deadline == SYS64_WAIT_FOREVER || rem < deadline) deadline = rem;
    }

    return deadline;
}

void _start(void) {
    put("compositor64: starting\n");

    if (!output64_init(&g_disp_w, &g_disp_h)) { put("compositor64: sys_display_open FAILED -- no display available\n"); sys_exit(1); }
    put_kv("compositor64: display width=", g_disp_w);
    put_kv("compositor64: display height=", g_disp_h);

    int64_t ih = sys_input_open();
    if (ih < 0) { put("compositor64: sys_input_open FAILED\n"); sys_exit(1); }
    g_input_h = (int)ih;

    // M+7A: fetch the active absolute pointer device's own range ONCE,
    // exactly like the display geometry above -- see
    // handle_input_event()'s own comment on why this is never hardcoded
    // against any particular resolution. Absence (-1) just means no
    // absolute pointer device is active this boot (plain PS/2-only) --
    // not an error; POINTER_ABS events simply never arrive in that case.
    g_have_abs_range = (sys_input_get_abs_range(g_input_h, &g_abs_range) == 0);
    put(g_have_abs_range ? "compositor64: absolute pointer device active\n"
                          : "compositor64: no absolute pointer device -- PS/2 relative only\n");

    uint64_t bb_size = (uint64_t)g_disp_w * g_disp_h * 4;
    uint64_t bb_addr = sys_mmap(bb_size);
    if (bb_addr == (uint64_t)-1) { put("compositor64: backbuffer sys_mmap FAILED\n"); sys_exit(1); }
    g_backbuffer = (uint32_t*)(uintptr_t)bb_addr;

    // M+8: same size/format as g_backbuffer, own separate allocation --
    // never rendered into, only ever mirrored into rect-by-rect after a
    // successful present (see present_rect()). Zero-initialized by
    // sys_mmap like every other fresh mapping; g_front_valid stays 0
    // until the first full successful present actually establishes a
    // complete, meaningful front image.
    uint64_t fb_front_addr = sys_mmap(bb_size);
    if (fb_front_addr == (uint64_t)-1) { put("compositor64: frontbuffer sys_mmap FAILED\n"); sys_exit(1); }
    g_frontbuffer = (uint32_t*)(uintptr_t)fb_front_addr;

    g_cursor_x = (int32_t)(g_disp_w / 2);
    g_cursor_y = (int32_t)(g_disp_h / 2);

    // M+7: decide the cursor path ONCE, here, before compose_and_present_full()'s
    // very first call below ever runs -- so the very first frame this
    // compositor ever presents already reflects the right mode, and
    // bb_draw_cursor() is never called even a single time when hardware
    // cursor ends up active (no stale software cursor to invalidate
    // later, no ghost, no transition logic needed on this forward path --
    // see this milestone's own report on why "decide once at boot"
    // avoids the harder live-toggle case entirely). Falls back to the
    // complete, unmodified software cursor path on ANY failure --
    // missing device, no cursor virtqueue, or the image upload itself
    // failing -- never fatal to the compositor either way.
    // M+10A cursor-visibility investigation: A/B/C isolation switches.
    // Off by default -- only ever defined for one specific diagnostic
    // build at a time, never both, never left on. Neither touches the
    // cursor RENDERING path itself (bb_draw_cursor()/build_hw_cursor_image()/
    // the kernel's own cursor backend are all untouched); each only
    // forces this boot-time DECISION to a known value so the same real
    // hardware/kernel path can be exercised with the other variable held
    // fixed.
    //   A: -DCOMPOSITOR_FORCE_SOFTWARE_CURSOR    -- hw cursor negotiation skipped entirely, forces the software fallback
    //   B: -DCOMPOSITOR_FORCE_NO_DIRECT_SCANOUT  -- hw cursor negotiated normally, direct scanout forced unavailable
    //   C: neither defined                        -- current default (both enabled, whatever the real hardware supports)
#ifndef COMPOSITOR_FORCE_SOFTWARE_CURSOR
    if (output64_cursor_available()) {
        uint64_t hw_cursor_size = (uint64_t)HW_CURSOR_DIM * HW_CURSOR_DIM * 4;
        uint64_t hw_cursor_addr = sys_mmap(hw_cursor_size);
        if (hw_cursor_addr != (uint64_t)-1) {
            uint32_t* hw_cursor_px = (uint32_t*)(uintptr_t)hw_cursor_addr;
            build_hw_cursor_image(hw_cursor_px);
#ifdef COMPOSITOR_CURSOR_IMAGE_SELFTEST
            // M+10A cursor investigation: verify the 64x64 backing
            // actually contains non-transparent pixels BEFORE the
            // upload syscall -- see build_hw_cursor_image()'s own
            // comment for the expected shape (a 78-pixel opaque
            // triangle, alpha 0 everywhere else).
            {
                uint32_t opaque_count = 0;
                for (uint32_t i = 0; i < (uint32_t)HW_CURSOR_DIM * HW_CURSOR_DIM; i++) {
                    if ((hw_cursor_px[i] >> 24) != 0) opaque_count++;
                }
                put_kv("compositor64: cursor image opaque_pixel_count=", (uint64_t)opaque_count);
            }
#endif
            if (output64_cursor_set_image(HW_CURSOR_DIM, HW_CURSOR_DIM, hw_cursor_px, 0, 0) == 0 &&
                output64_cursor_move(g_cursor_x, g_cursor_y) == 0) {
                g_hw_cursor_active = 1;
                put("compositor64: hardware cursor active\n");
            }
            sys_munmap(hw_cursor_addr, hw_cursor_size);
        }
    }
#endif
    if (!g_hw_cursor_active) put("compositor64: hardware cursor unavailable -- using software cursor\n");

    // M+9B: capability check, once -- see direct_scanout_eligible()'s
    // own comment on why this is cached rather than re-queried per
    // commit. A framebuffer-only boot (SYS64_DISPLAY_DIRECT_QUERY
    // returns 0) makes every later eligibility check fail instantly and
    // permanently -- fullscreen continues through normal composition
    // unconditionally, exactly like this milestone's own §2 requires.
#ifdef COMPOSITOR_FORCE_NO_DIRECT_SCANOUT
    g_direct_supported = 0;
    put("compositor64: direct scanout FORCED OFF (M+10A cursor investigation build)\n");
#else
    g_direct_supported = (int)output64_direct_supported();
    put(g_direct_supported ? "compositor64: direct scanout backend available\n"
                            : "compositor64: direct scanout unavailable -- fullscreen stays fully composited\n");
#endif

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
    compose_and_present_full(); // initial draw -- g_full_redraw is also already 1, kept in sync below
#if defined(M8_FAULT_INJECT) && defined(M8_FAULT_INJECT_SELFTEST)
    m8_selftest_fault_injection();
#endif

    uint64_t last_frame_tick = sys_get_ticks();
    // M+6: frame-callback dispatch runs on its OWN pacing clock,
    // independent of last_frame_tick above -- see
    // dispatch_frame_callbacks()'s own comment on why reusing the
    // presentation-pacing clock would either delay callbacks behind
    // unrelated damage or change existing presentation latency.
    uint64_t last_callback_tick = sys_get_ticks();

#ifdef COMPOSITOR_LEGACY_POLL_MODE
    // M+12B: byte-for-byte the pre-M+12B busy-poll main loop, kept
    // available as a temporary bring-up/fallback path until WAIT_ANY has
    // been physically verified -- see the #else branch below for the
    // normal, event-driven default. Not the long-term shape of this
    // function; remove this whole #ifdef branch once M+12B itself is
    // accepted (mirrors this milestone's own has_surface-bug-fix
    // precedent of documenting history rather than silently deleting
    // it, just in the other direction: here the OLD behavior is what's
    // preserved for a while, not the fix).
    for (;;) {
        input64_event_t ev;
        while (sys_handle_try_read(g_input_h, (char*)&ev, sizeof(ev)) == (int64_t)sizeof(ev)) {
            handle_input_event(&ev);
        }

        while (accept_one_client((int)listen_h) >= 0) { /* drain any/all pending connects this iteration */ }

        for (int ci = 0; ci < MAX_CLIENTS; ci++) {
            if (!g_conns[ci].in_use) continue;
            for (;;) {
                wm_msg_t msg;
                int64_t n = sys_handle_try_read(g_conns[ci].req_r, (char*)&msg, sizeof(msg));
                if (n == (int64_t)sizeof(msg)) {
                    handle_client_message(ci, &msg);
                    continue;
                }
                if (n == 0) { // EOF -- client exited or crashed
                    mark_full_redraw(); // see disconnect_stalled_conns()'s own comment -- same reasoning
                    disconnect_conn(ci);
                }
                break; // would-block, error, or just handled EOF -- move to next connection
            }
        }

        // Milestone 32.1: tear down anything queue_event()/
        // flush_conn_queue() flagged above BEFORE flushing queues below
        // -- a connection already marked pending_disconnect has nothing
        // further written to or read from its (about to be closed)
        // handles.
        disconnect_stalled_conns();

        // Milestone 32.1: deliver whatever is queued, via non-blocking
        // writes only -- this is what guarantees the loop above (input
        // draining, accepting, request processing) can NEVER be blocked
        // by one client's full or stalled event pipe. See queue_event()
        // for the full delivery/coalescing/backpressure design.
        for (int ci = 0; ci < MAX_CLIENTS; ci++) {
            if (g_conns[ci].in_use) flush_conn_queue(ci);
        }

        // M-next: render+present at most once per pacing interval,
        // gated on there actually being something to draw. Damage
        // keeps accumulating across loop iterations below the pacing
        // rate (nothing is ever silently dropped -- see add_damage_rect
        // itself for the only case that discards detail, which falls
        // back to a full redraw rather than losing damage); a burst of
        // input/commit-driven damage arriving faster than the pacing
        // rate collapses into a single composite+present, which is
        // exactly what turns "one full-screen recompose per PS/2
        // packet" into "one recompose per frame interval" (§17.2).
        // M+9B §15/§18: while a direct-scanned window owns the output,
        // the normal composite/present pipeline has nothing useful to
        // do -- whatever it would draw is not the visible output, and
        // running it anyway would burn real compositor_bytes/
        // front_mirror_bytes this milestone's own steady-state
        // expectation requires to stay zero. Any damage/full-redraw
        // request that accumulates during direct mode is NOT lost --
        // it stays pending and is picked up by direct_scanout_force_leave()'s
        // own mark_full_redraw()+compose_and_present_full() call the
        // instant direct mode actually ends.
        if (g_presented_mode != PRESENTED_DIRECT && (g_full_redraw || g_damage_count > 0)) {
            uint64_t now = sys_get_ticks();
            if (now - last_frame_tick >= COMPOSITOR_PACING_TICKS) {
                // M+8: g_full_redraw/g_damage_count are no longer cleared
                // unconditionally here -- each present function now owns
                // clearing exactly what it actually got onto the display
                // (compose_and_present_full() clears g_full_redraw only
                // on success; compose_and_present_damage() compacts
                // g_damage[] down to whatever failed, via present_damage()).
                // A failed present therefore correctly re-enters this same
                // `if` on the very next paced tick with the same pending
                // work, instead of silently discarding it.
#ifdef COMPOSITOR_FORCE_FULL_REDRAW_DIAGNOSTIC
                compose_and_present_full();
                g_full_redraw = 0; // legacy diagnostic mode -- not wired into the M+8 retry model, see its own definition
#else
                if (g_full_redraw) {
                    if (compose_and_present_full()) g_full_redraw = 0;
                } else {
                    compose_and_present_damage();
                }
#endif
                last_frame_tick = now;
            }
        }

        // M+6: grant any outstanding frame-callback requests, rate-limited
        // to the same COMPOSITOR_PACING_TICKS cadence as presentation but
        // on its own independent clock -- see last_callback_tick's own
        // comment above for why. Runs every iteration this gate is open,
        // regardless of whether there was damage to present this tick.
        {
            uint64_t now_cb = sys_get_ticks();
            if (now_cb - last_callback_tick >= COMPOSITOR_PACING_TICKS) {
                dispatch_frame_callbacks();
                last_callback_tick = now_cb;
            }
        }
#ifdef COMPOSITOR_PERF_STATS
        report_perf_stats_if_due();
#endif
        check_liveness_watchdog();
    }
#else
    // M+12B: event-driven main loop -- the normal/default path. Same
    // input/accept/request drain and the same composition/frame-
    // callback/watchdog work as the legacy loop above, in the exact
    // dependency order the M+12B design audit established (see that
    // audit for the full producer-by-producer trace): every function
    // that can queue_event() runs BEFORE the final flush below, so
    // nothing can enqueue a NEW outbound event between the final flush
    // and the eventual sys_handle_wait_any call.
    for (;;) {
        input64_event_t ev;
        while (sys_handle_try_read(g_input_h, (char*)&ev, sizeof(ev)) == (int64_t)sizeof(ev)) {
            handle_input_event(&ev);
        }

        while (accept_one_client((int)listen_h) >= 0) { /* drain any/all pending connects this iteration */ }

        for (int ci = 0; ci < MAX_CLIENTS; ci++) {
            if (!g_conns[ci].in_use) continue;
            for (;;) {
                wm_msg_t msg;
                int64_t n = sys_handle_try_read(g_conns[ci].req_r, (char*)&msg, sizeof(msg));
                if (n == (int64_t)sizeof(msg)) {
                    handle_client_message(ci, &msg);
                    continue;
                }
                if (n == 0) { // EOF -- client exited or crashed
                    mark_full_redraw();
                    disconnect_conn(ci);
                }
                break;
            }
        }

        // Composition/presentation -- confirmed by the M+12B producer
        // audit to never itself queue_event() (compose_and_present_full/
        // damage, present_rect/present_damage, composite_full/
        // composite_damage_scoped never call queue_event,
        // notify_buffer_released, or direct_scanout_force_leave/
        // direct_scanout_try_enter_or_update -- those are only ever
        // reached from input/request handling and close_window()).
        // Positioned before the final flush anyway, so its own damage/
        // full-redraw-clearing effects are already reflected in the
        // timeout calculation below.
        if (g_presented_mode != PRESENTED_DIRECT && (g_full_redraw || g_damage_count > 0)) {
            uint64_t now = sys_get_ticks();
            if (now - last_frame_tick >= COMPOSITOR_PACING_TICKS) {
#ifdef COMPOSITOR_FORCE_FULL_REDRAW_DIAGNOSTIC
                compose_and_present_full();
                g_full_redraw = 0;
#else
                if (g_full_redraw) {
                    if (compose_and_present_full()) g_full_redraw = 0;
                } else {
                    compose_and_present_damage();
                }
#endif
                last_frame_tick = now;
            }
        }

        // Frame-callback dispatch IS a real queue_event() producer
        // (grant_frame_callback -> queue_event(WM_MSG_FRAME)) -- moved
        // here, before the final flush/disconnect tail, specifically so
        // a callback granted this iteration is flushed THIS iteration
        // and its evt_w correctly joins the interest set below if the
        // pipe happens to be full, instead of the old loop's ordering
        // (which ran this AFTER its own flush, leaving no wake source
        // registered for it at all under WAIT_ANY).
        {
            uint64_t now_cb = sys_get_ticks();
            if (now_cb - last_callback_tick >= COMPOSITOR_PACING_TICKS) {
                dispatch_frame_callbacks();
                last_callback_tick = now_cb;
            }
        }
#ifdef COMPOSITOR_PERF_STATS
        report_perf_stats_if_due();
#endif
        check_liveness_watchdog(); // diagnostic only -- sets watchdog_fired, read by the timeout calc below; never itself queues an event

        // ── M+12B final quiescence tail ──────────────────────────────
        // Two disconnect sweeps bracket the FINAL flush. Sweep #1
        // catches anything flagged pending_disconnect by input/request/
        // composition/frame-callback work above. Sweep #2 catches a
        // hard event-pipe write failure the final flush itself
        // discovers (flush_conn_queue's own broken-pipe branch). Proven
        // (see the M+12B design audit, not just assumed): sweep #2's own
        // close_window() teardown can never flag a THIRD, different
        // connection -- every event it can produce
        // (set_keyboard_focus_ex's focus-lost message,
        // notify_buffer_released, direct_scanout_force_leave) targets
        // the SAME connection already being torn down (close_window's
        // own g_keyboard_focus==idx precondition, and
        // direct_scanout_force_leave/notify_buffer_released both
        // operating on that same window), never a different surviving
        // one. From here to the eventual sys_handle_wait_any call below,
        // NO code may enqueue a new outbound event -- disconnect_conn()
        // only closes handles and resets state, it never calls
        // queue_event() for anyone else.
        disconnect_stalled_conns();

        for (int ci = 0; ci < MAX_CLIENTS; ci++) {
            if (g_conns[ci].in_use) flush_conn_queue(ci);
        }

        disconnect_stalled_conns();

        // Pre-WAIT invariant: no in-use connection may still be
        // pending_disconnect here -- proven structurally impossible by
        // the two-sweep argument above, so finding one means some path
        // this audit missed violated it. Never panics (bring-up, not a
        // fatal condition): skip blocking this pass and let the next
        // iteration's own two sweeps try again.
        int m12b_wait_ok = 1;
        for (int ci = 0; ci < MAX_CLIENTS; ci++) {
            if (g_conns[ci].in_use && g_conns[ci].pending_disconnect) {
                m12b_wait_ok = 0;
                g_diag_pending_disconnect_at_wait_count++;
                if (!g_diag_pending_disconnect_at_wait_reported) {
                    g_diag_pending_disconnect_at_wait_reported = 1;
                    put("*** M12B: pending_disconnect still set immediately before WAIT_ANY -- skipping block, retrying ***\n");
                }
                break;
            }
        }

        if (m12b_wait_ok) {
            uint64_t wa_now = sys_get_ticks();
            uint64_t timeout = compute_wait_any_timeout(wa_now, last_frame_tick, last_callback_tick);
            // "If compositor-local work is already due now, continue
            // the main loop -- do not call WAIT_ANY with timeout=0
            // merely to rediscover that fact." timeout==0 means
            // compute_wait_any_timeout already found something due
            // right now, so the syscall would tell us nothing we don't
            // already know; skip it entirely.
            if (timeout != 0) {
                int wa_handles[2 + 2 * MAX_CLIENTS];
                int wa_count = build_wait_any_set((int)listen_h, wa_handles);
#ifdef COMPOSITOR_WAIT_ANY_TRACE
                put_kv("M12B WAIT_ANY: handle_count=", (uint64_t)wa_count);
                put_kv("M12B WAIT_ANY: timeout_ticks=", timeout);
#endif
                int64_t wrc = sys_handle_wait_any(wa_handles, (uint32_t)wa_count, timeout);
#ifdef COMPOSITOR_WAIT_ANY_TRACE
                put_kv("M12B WAIT_ANY: rc=", (uint64_t)wrc);
#else
                (void)wrc;
#endif
            }
        }
    }
#endif // COMPOSITOR_LEGACY_POLL_MODE
}
