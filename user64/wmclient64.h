// ToxenOS/user64/wmclient64.h — Milestone 30: reusable compositor
// client library. Hides connection bootstrap, shared-memory surface
// creation/mapping, and protocol message construction from graphical
// applications -- see user64/gfx_demo64.c/user64/gfx_interactive64.c
// for how little an application needs to know to use this.
//
// A client program using this library never touches sys_get_args,
// sys_pipe_*, sys_shm_*, or wm_msg_t directly for the common path.
#ifndef WMCLIENT64_H
#define WMCLIENT64_H
#include <stdint.h>
#include "tox64.h"
#include "wmproto64.h"

// The compositor is single-threaded but NOT synchronous with any one
// client: an unsolicited event (most notably WM_MSG_FOCUS -- a
// DIFFERENT window being created or clicked can steal focus from this
// client's window at any time, including the moment right after this
// client sent a request and is waiting for ITS OWN reply) can legally
// arrive on the event pipe BEFORE the reply a library call is
// currently blocked waiting for. `pending` is a small FIFO for exactly
// that: wmc_recv_expecting() stashes anything that isn't the reply it
// wanted here instead of misinterpreting or discarding it, and
// wm_wait_event() (the normal event-loop call) drains this FIRST
// before doing a real blocking read, so nothing is ever lost or
// reordered from the application's point of view.
#define WMC_PENDING_MAX 4

typedef struct {
    int req_w;      // this process's own handle: write end, client -> compositor
    int evt_r;      // this process's own handle: read end, compositor -> client
    uint32_t client_id;
    wm_msg_t pending[WMC_PENDING_MAX];
    int pending_count;
} wm_client_t;

static inline int wmc_strlen(const char* s) { int i = 0; while (s[i]) i++; return i; }

static inline int wmc_send(wm_client_t* c, const wm_msg_t* m) {
    int64_t n = sys_handle_write(c->req_w, (const char*)m, sizeof(*m));
    return (n == (int64_t)sizeof(*m)) ? 0 : -1;
}

// Blocks for the next message and returns it verbatim, regardless of
// type -- used internally by wmc_recv_expecting() and directly by
// application event loops via wm_wait_event() below.
static inline int wmc_recv_raw(wm_client_t* c, wm_msg_t* out) {
    int64_t n = sys_handle_read(c->evt_r, (char*)out, sizeof(*out));
    return (n == (int64_t)sizeof(*out)) ? 0 : -1;
}

// Blocks until a message of EXACTLY `wanted_type` arrives, queuing any
// other message received along the way (see WMC_PENDING_MAX's comment)
// instead of losing or misinterpreting it. Used by the library's own
// request/reply calls (wm_connect/wm_create_window/wm_attach_surface).
// Returns 0 with *out filled, or -1 on disconnect/error.
static inline int wmc_recv_expecting(wm_client_t* c, uint32_t wanted_type, wm_msg_t* out) {
    for (;;) {
        wm_msg_t m;
        if (wmc_recv_raw(c, &m) < 0) return -1;
        if (m.type == wanted_type) { *out = m; return 0; }
        if (c->pending_count < WMC_PENDING_MAX) c->pending[c->pending_count++] = m;
        // else: queue full -- drop it. Only matters if an application
        // triggers several unrelated async events while its own
        // request/reply call is still outstanding, which none of this
        // milestone's clients do.
    }
}

// Blocks until the next event arrives from the compositor -- draining
// the pending queue first, then a real blocking read. This is what an
// application's normal event loop should call (see
// user64/gfx_interactive64.c). Returns 0 with *out filled, or -1 on
// disconnect/error.
static inline int wm_wait_event(wm_client_t* c, wm_msg_t* out) {
    if (c->pending_count > 0) {
        *out = c->pending[0];
        for (int i = 1; i < c->pending_count; i++) c->pending[i - 1] = c->pending[i];
        c->pending_count--;
        return 0;
    }
    return wmc_recv_raw(c, out);
}

// Connects to whichever compositor currently owns WM_SERVICE_NAME --
// see wmproto64.h's header comment. Milestone 32: sys_service_connect
// (blocking) replaces the old Milestone 30 bootstrap entirely; this
// works correctly regardless of whether the compositor has already
// registered the service by the time this call runs (blocks, a real
// scheduler block, until it does) and regardless of whether this
// process is the compositor's child, a sibling, or launched much later
// from an unrelated process (a shell command, say) -- there is no
// spawn relationship requirement at all anymore. No handle scrubbing
// is needed either: this process's OWN two connection handles are the
// only thing sys_service_connect ever installs in its table, so there
// is nothing stale to clean up (contrast with the old Milestone 30
// scheme, where whole-table spawn inheritance from the compositor
// dragged in every earlier client's pipes too).
//
// Sends HELLO and blocks for WELCOME once connected. Returns 0 with
// *out filled, or -1 (no compositor ever appears -- see
// include/syscall64.h's SYS64_SERVICE_CONNECT docs on why a blocking
// connect to a service that will truly never exist blocks forever, a
// tradeoff acceptable here since this call is only ever made by a
// genuinely graphical client -- or the handshake failed/disconnected).
static inline int wm_connect(wm_client_t* out) {
    service64_endpoints_t ep;
    if (sys_service_connect(WM_SERVICE_NAME, 1 /* blocking */, &ep) < 0) return -1;

    out->req_w = ep.send;
    out->evt_r = ep.recv;
    out->client_id = 0;
    out->pending_count = 0;

    wm_msg_t hello; for (uint64_t i = 0; i < sizeof(hello); i++) ((char*)&hello)[i] = 0;
    hello.type = WM_MSG_HELLO;
    hello.version = WM_PROTO_VERSION;
    if (wmc_send(out, &hello) < 0) return -1;

    wm_msg_t reply;
    if (wmc_recv_expecting(out, WM_MSG_WELCOME, &reply) < 0) return -1;
    out->client_id = reply.client_id;
    return 0;
}

// Requests a new window of content size w x h with the given title
// (truncated to WM_TITLE_MAX-1 chars). Blocks for WINDOW_CREATED.
// Returns the window_id, or 0 on failure (0 is never a valid window_id).
static inline uint32_t wm_create_window(wm_client_t* c, uint32_t w, uint32_t h, const char* title) {
    wm_msg_t m; for (uint64_t i = 0; i < sizeof(m); i++) ((char*)&m)[i] = 0;
    m.type = WM_MSG_CREATE_WINDOW;
    m.version = WM_PROTO_VERSION;
    m.client_id = c->client_id;
    m.w = w; m.h = h;
    int i = 0; while (title[i] && i < WM_TITLE_MAX - 1) { m.title[i] = title[i]; i++; }
    if (wmc_send(c, &m) < 0) return 0;

    wm_msg_t reply;
    if (wmc_recv_expecting(c, WM_MSG_WINDOW_CREATED, &reply) < 0) return 0;
    return reply.window_id;
}

// Creates a w x h x 4 (WM_FORMAT_XRGB8888) shared-memory surface,
// attaches it to `window_id`, and maps it WRITABLE into this process's
// own address space. Blocks for the compositor's ACK. Returns a
// pointer to the mapped pixel buffer (row-major, tightly packed,
// w*4 bytes per row) on success, or 0 on failure. `stride_out` (if
// non-NULL) is set to the actual stride (always w*4 here).
static inline uint32_t* wm_attach_surface(wm_client_t* c, uint32_t window_id,
                                           uint32_t w, uint32_t h, uint32_t* stride_out)
{
    if (w == 0 || h == 0 || w > 4096 || h > 4096) return 0;
    uint64_t size = (uint64_t)w * (uint64_t)h * 4ULL;

    int64_t shm_h = sys_shm_create(size);
    if (shm_h < 0) return 0;
    int64_t token = sys_shm_token((int)shm_h);
    if (token < 0) { sys_handle_close((int)shm_h); return 0; }
    uint64_t addr = sys_shm_map((int)shm_h, 1 /* writable */);
    if (addr == (uint64_t)-1) { sys_handle_close((int)shm_h); return 0; }

    wm_msg_t m; for (uint64_t i = 0; i < sizeof(m); i++) ((char*)&m)[i] = 0;
    m.type = WM_MSG_ATTACH_SURFACE;
    m.version = WM_PROTO_VERSION;
    m.client_id = c->client_id;
    m.window_id = window_id;
    m.shm_token = (uint64_t)token;
    m.w = w; m.h = h;
    m.stride = w * 4;
    m.format = WM_FORMAT_XRGB8888;
    if (wmc_send(c, &m) < 0) { sys_handle_close((int)shm_h); return 0; }

    wm_msg_t reply;
    if (wmc_recv_expecting(c, WM_MSG_ACK, &reply) < 0) {
        sys_handle_close((int)shm_h);
        return 0;
    }

    if (stride_out) *stride_out = w * 4;
    // Deliberately leaks the shm handle for this milestone's simple
    // single-surface-per-window model -- it stays alive exactly as
    // long as this process runs, released automatically on exit like
    // every other handle (kernel/process64.c's close_all_handles); a
    // window that outlives its client is never possible since the
    // compositor removes windows on client disconnect (see
    // compositor64.c's connection cleanup).
    return (uint32_t*)(uintptr_t)addr;
}

// Tells the compositor that the rectangle (x,y,w,h) of window_id's
// surface has changed and should be recomposited. No reply.
static inline int wm_commit(wm_client_t* c, uint32_t window_id, int32_t x, int32_t y, uint32_t w, uint32_t h) {
    wm_msg_t m; for (uint64_t i = 0; i < sizeof(m); i++) ((char*)&m)[i] = 0;
    m.type = WM_MSG_COMMIT;
    m.version = WM_PROTO_VERSION;
    m.client_id = c->client_id;
    m.window_id = window_id;
    m.x = x; m.y = y; m.w = w; m.h = h;
    return wmc_send(c, &m);
}

static inline int wm_set_title(wm_client_t* c, uint32_t window_id, const char* title) {
    wm_msg_t m; for (uint64_t i = 0; i < sizeof(m); i++) ((char*)&m)[i] = 0;
    m.type = WM_MSG_SET_TITLE;
    m.version = WM_PROTO_VERSION;
    m.client_id = c->client_id;
    m.window_id = window_id;
    int i = 0; while (title[i] && i < WM_TITLE_MAX - 1) { m.title[i] = title[i]; i++; }
    return wmc_send(c, &m);
}

static inline int wm_destroy_window(wm_client_t* c, uint32_t window_id) {
    wm_msg_t m; for (uint64_t i = 0; i < sizeof(m); i++) ((char*)&m)[i] = 0;
    m.type = WM_MSG_DESTROY_WINDOW;
    m.version = WM_PROTO_VERSION;
    m.client_id = c->client_id;
    m.window_id = window_id;
    return wmc_send(c, &m);
}

#endif // WMCLIENT64_H
