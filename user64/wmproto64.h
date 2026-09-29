// ToxenOS/user64/wmproto64.h — Milestone 30: the compositor/client wire
// protocol. Deliberately small and fixed-width: every message is the
// SAME struct (wm_msg_t), tagged by `type` and `version`, so the ABI
// never depends on C structure accidents (padding, per-message-type
// struct layouts, etc.) -- a message is always exactly sizeof(wm_msg_t)
// bytes, both directions.
//
// Transport: one pipe PAIR per client connection --
//   request pipe:  client writes wm_msg_t, compositor reads it (control)
//   event pipe:    compositor writes wm_msg_t, client reads it (events)
// Never a single bidirectional pipe (keeps read/write ends unambiguous,
// matches every other pipe use in this codebase). Pixel data never
// goes through either pipe -- only a shared-memory TOKEN (see
// include/shm64.h's shm64_t::token) identifying the surface object;
// the actual pixels are read directly by the compositor from its own
// mapping of that object.
//
// Bootstrap (Milestone 32): the compositor publishes the well-known
// service name WM_SERVICE_NAME (include/service64.h's generic named-
// service registry) and a client connects to it via sys_service_connect
// -- see user64/wmclient64.h's wm_connect(). Neither side needs to know
// the other's pid or pipe handle numbers, and a client no longer needs
// to be the compositor's own child (or a child at all) to connect --
// this replaced Milestone 30's original bootstrap, which encoded the
// client's two spawn-inherited handle numbers into its own args string.
// That mechanism is gone entirely now; see the Milestone 32 summary for
// the full rationale. Everything below this point (the message
// protocol itself) is UNCHANGED from Milestone 30.
#ifndef WMPROTO64_H
#define WMPROTO64_H
#include <stdint.h>
#include "tox64.h"

#define WM_PROTO_VERSION 1
#define WM_TITLE_MAX 16

// Milestone 32: the well-known service name the compositor publishes
// (user64/compositor64.c's sys_service_listen) and every client
// connects to (user64/wmclient64.h's wm_connect). A plain string
// constant, not a kernel primitive -- the kernel has no idea this name
// means "window manager," it is just bytes to include/service64.h.
#define WM_SERVICE_NAME "tox.wm"

// Logical client-surface pixel format -- 32-bit packed 0x00RRGGBB,
// independent of the physical framebuffer's actual layout (Milestone
// 29's pixfmt64 handles that conversion at present time). Clients never
// need to know or care what the real display hardware looks like.
#define WM_FORMAT_XRGB8888 1

typedef enum {
    WM_MSG_HELLO           = 1,  // client -> compositor: registration (must be the first message)
    WM_MSG_WELCOME         = 2,  // compositor -> client: client_id assigned
    WM_MSG_CREATE_WINDOW   = 3,  // client -> compositor: w,h,title -- requests a new window
    WM_MSG_WINDOW_CREATED  = 4,  // compositor -> client: window_id assigned
    WM_MSG_DESTROY_WINDOW  = 5,  // client -> compositor: window_id
    WM_MSG_ATTACH_SURFACE  = 6,  // client -> compositor: window_id, shm_token, w,h,stride,format
    WM_MSG_COMMIT          = 7,  // client -> compositor: window_id, damage rect (x,y,w,h)
    WM_MSG_SET_TITLE       = 8,  // client -> compositor: window_id, title
    WM_MSG_KEY_EVENT       = 9,  // compositor -> client: window_id, key_code,pressed,modifiers,ascii
    WM_MSG_POINTER_MOTION  = 10, // compositor -> client: window_id, x,y (CLIENT-CONTENT-RELATIVE)
    WM_MSG_POINTER_BUTTON  = 11, // compositor -> client: window_id, button,pressed,x,y
    WM_MSG_POINTER_WHEEL   = 12, // compositor -> client: window_id, x = wheel delta
    WM_MSG_FOCUS           = 13, // compositor -> client: window_id, pressed (1=gained,0=lost)
    WM_MSG_CLOSE_REQUEST   = 14, // compositor -> client: window_id (e.g. middle-click on title bar)
    WM_MSG_ACK             = 15, // compositor -> client: window_id = the id being acknowledged
    WM_MSG_ERROR           = 16, // compositor -> client: x = error code (negative, see WM_ERR_*)
    // Milestone 32.1: client -> compositor, recognized ONLY by a
    // compositor64 binary built with COMPOSITOR64_TEST_MODE defined
    // (see user64/compositor64.c and user64/compositor_stall_test64.c)
    // -- lets that self-test's driver cleanly end a disposable test
    // compositor instance (releasing its exclusive display/input
    // ownership via ordinary process exit) instead of leaking it as a
    // permanently-running orphan process. The real compositor64.nex64
    // built for normal graphical boot does NOT define
    // COMPOSITOR64_TEST_MODE, so it just replies WM_ERR_BAD_MESSAGE to
    // this like any other unrecognized type -- completely inert there.
    WM_MSG_TEST_SHUTDOWN   = 17,

    // M+5: committed, double-buffered client surfaces (Revision 2 §22
    // items 1-2) -- coexists with WM_MSG_ATTACH_SURFACE/WM_MSG_COMMIT
    // above, never mixed on the same window (a window uses exactly one
    // model, decided by whichever message type its client sends first).
    // Reuses every existing wm_msg_t field with no new struct layout at
    // all -- see each message's own comment for which fields carry
    // meaning.
    //
    // WM_MSG_ATTACH_BUFFER: client -> compositor: window_id, shm_token,
    // w,h,stride,format -- registers a NEW buffer for double-buffered
    // commit. Unlike WM_MSG_ATTACH_SURFACE, this does NOT display
    // anything yet -- the buffer is just made known to the compositor,
    // exactly like Wayland's wl_surface.attach not yet being a commit.
    // The buffer's identity for every later message is its OWN
    // shm_token -- no separate buffer-id space is introduced. At most
    // WM_MAX_BUFFERS_PER_WINDOW (compositor64.c) buffers may be attached
    // to one window at a time (M+5 is deliberately double-buffered, not
    // an arbitrary-depth swapchain). Replies WM_MSG_ACK, or WM_MSG_ERROR
    // (WM_ERR_BAD_SURFACE if this window already uses the legacy
    // single-surface model, or WM_ERR_NO_RESOURCES if both buffer slots
    // are already attached).
    WM_MSG_ATTACH_BUFFER   = 18,

    // WM_MSG_COMMIT_BUFFER: client -> compositor: window_id, shm_token
    // (which ALREADY-ATTACHED buffer becomes this window's new committed
    // frame), x,y,w,h = damage rect (identical content-local-coordinate
    // semantics to WM_MSG_COMMIT). This is the atomic swap point: the
    // compositor's own window_t.committed_slot changes to point at the
    // named buffer in one assignment, so any compositor frame that reads
    // it sees either the fully-old or fully-new buffer, never a mix.
    // The PREVIOUSLY committed buffer (if any, and if different) is
    // released immediately afterward -- see WM_MSG_BUFFER_RELEASED's own
    // comment on why "immediately" is correct for ToxenOS specifically.
    // Replies WM_MSG_ACK, or WM_MSG_ERROR (WM_ERR_BAD_SURFACE if
    // shm_token names a buffer never attached to this window, or one
    // already retired).
    //
    // M+10A protocol redesign: `generation` names which WM_MSG_CONFIGURE
    // this commit's content answers -- the client's own last-applied
    // configure generation at the moment it rendered this content (see
    // wm_window_t's own comment in wmclient64.h). The compositor does
    // not reject a stale generation this milestone (ToxenOS's fullscreen
    // transitions are synchronous enough server-side that a genuinely
    // wrong-generation commit isn't reachable from either current
    // client), but it is diagnostically counted -- see compositor64.c's
    // own g_diag_stale_commit_generation_count -- as a real protocol
    // assertion for future clients that pipeline commits ahead of
    // configure acknowledgement.
    WM_MSG_COMMIT_BUFFER   = 19,

    // WM_MSG_BUFFER_RELEASED: compositor -> client: window_id, shm_token
    // identifying which buffer the client may now safely draw into
    // again. Sent the instant a buffer stops being window_t.committed_slot
    // (see WM_MSG_COMMIT_BUFFER above) -- correct without any GPU-fence
    // or "wait for the frame to finish presenting" bookkeeping because
    // compositor64's entire composite+present pipeline is synchronous
    // end to end today (CPU compositing, and M+2/M+3's VirtIO-GPU
    // transfer+flush, are both polling/synchronous, never async): by the
    // time ANY message handler returns, nothing is still reading the
    // buffer it just retired. This is a real simplification specific to
    // today's architecture, not a general rule -- revisit this the day
    // real GPU fences/async submission exist (Revision 2 §06/§07, still
    // explicitly deferred).
    WM_MSG_BUFFER_RELEASED = 20,

    // M+6: compositor-driven frame pacing for committed-buffer clients
    // (Revision 3 §22 item 5) -- opt-in per window: a committed-buffer
    // window that never sends WM_MSG_REQUEST_FRAME is simply never sent
    // WM_MSG_FRAME, and behaves exactly as M+5 left it. A legacy
    // single-surface window (WM_MSG_ATTACH_SURFACE) can never use this
    // at all -- see WM_MSG_REQUEST_FRAME's own error case below.
    //
    // Deliberately a SEPARATE message/concept from WM_MSG_BUFFER_RELEASED,
    // not merged into one message or one state bit: a released buffer
    // means "this SPECIFIC buffer is safe to write again"; a frame
    // callback means "the compositor is ready for this WINDOW to produce
    // another frame." A well-behaved animated client needs BOTH before
    // rendering its next frame, and the two can legally arrive in either
    // order (nothing ties their delivery together on the wire) -- see
    // user64/wmclient64.h's own wm_wait_frame() for how a client
    // reconciles that without deadlocking either way.
    //
    // WM_MSG_REQUEST_FRAME: client -> compositor: window_id -- asks for
    // exactly one future WM_MSG_FRAME callback for this window. At most
    // ONE callback is ever outstanding per window (compositor64's own
    // window_t.frame_callback_pending flag) -- a repeat request before
    // the previous one's callback fires is an idempotent no-op, never
    // queuing a second callback. Replies WM_MSG_ERROR (WM_ERR_BAD_SURFACE)
    // if the window doesn't use the M+5 committed-buffer model -- same
    // error WM_MSG_COMMIT_BUFFER itself already uses for that case.
    // Otherwise no reply -- the reply IS the eventual WM_MSG_FRAME.
    WM_MSG_REQUEST_FRAME   = 21,

    // WM_MSG_FRAME: compositor -> client: window_id -- "you may render
    // and commit your next frame now." Sent at a compositor frame-pacing
    // boundary (compositor64's own COMPOSITOR_PACING_TICKS interval,
    // the same one M-next's damage/present pacing already uses),
    // deliberately never immediately upon receiving WM_MSG_REQUEST_FRAME
    // -- this is what keeps REQUEST_FRAME -> FRAME -> REQUEST_FRAME from
    // ever becoming a new busy loop. This is compositor SCHEDULING,
    // explicitly NOT VSync: there is no real vblank signal anywhere in
    // this stack (basic VirtIO-GPU 2D mode has none), and
    // RESOURCE_FLUSH completion is command completion, not a scanout
    // signal -- never call this VSync in code or comments (Revision 3
    // §09's own finding).
    //
    // M+10A protocol redesign: `generation` is stamped with the
    // compositor's own w->configure_generation at the INSTANT this
    // callback is granted (not whatever the client believed when it
    // sent REQUEST_FRAME) -- this is what lets a client recognize a
    // callback granted before a since-superseded fullscreen/configure
    // transition as stale (generation < the client's own current known
    // configure generation) rather than ambiguously accepting it as
    // permission for its NEW mode. See wmclient64.h's wm_window_t and
    // its dispatcher's own WM_MSG_FRAME handling for the client-side
    // half of this contract: a stale grant is recognized and silently
    // re-requested, never treated as satisfying the current wait.
    WM_MSG_FRAME           = 22,

    // M+9A: real generic compositor fullscreen -- reuses the existing
    // wm_msg_t layout with zero new struct fields, same discipline
    // WM_MSG_ATTACH_BUFFER/WM_MSG_REQUEST_FRAME's own comments already
    // establish. Belongs to compositor64 as generic policy; a client
    // (e.g. GfxDemo's F11 handler) just sends the request -- fullscreen
    // itself is never hardcoded into any one client.
    //
    // WM_MSG_SET_FULLSCREEN: client -> compositor: window_id, `pressed`
    // reused as the fullscreen boolean (1 = enter, 0 = exit) -- same
    // "reuse `pressed` for a semantic bool" precedent WM_MSG_FOCUS
    // already sets (1=gained/0=lost). Entering while already fullscreen,
    // or exiting while not, is an idempotent no-op (still replies
    // WM_MSG_ACK). Replies WM_MSG_ERROR (WM_ERR_BAD_WINDOW) for an
    // unknown window. On success, the compositor always follows with
    // exactly one WM_MSG_CONFIGURE (below) before the WM_MSG_ACK.
    //
    // M+10A protocol redesign: this is now explicitly a FIRE-AND-FORGET
    // request, never a request/reply the client blocks on -- see
    // wmclient64.h's own wm_set_fullscreen() comment. The transition is
    // asynchronous: the client learns the result (and the geometry) only
    // via the eventual WM_MSG_CONFIGURE, exactly like any other unsolicited
    // state update, so it can keep dispatching/rendering the whole time
    // instead of stopping the world for one specific reply.
    WM_MSG_SET_FULLSCREEN  = 23,

    // WM_MSG_CONFIGURE: compositor -> client: window_id, w,h = the
    // content dimensions the client should render at from now on,
    // `pressed` reused as the CURRENT fullscreen state (1=fullscreen,
    // 0=normal) so a client always knows its present state without
    // tracking it independently. Sent exactly once per
    // WM_MSG_SET_FULLSCREEN transition (both entering and exiting) --
    // never a general-purpose live-resize protocol; this milestone's own
    // scope is fullscreen enter/exit only, not arbitrary client resizing.
    // A client that ignores this (or one still using WM_MSG_ATTACH_SURFACE/
    // WM_MSG_COMMIT) simply keeps rendering at its old size -- compositor64
    // does not force-stretch or reject an old buffer, it just composites
    // whatever the client actually attaches, at whatever size that is.
    //
    // M+10A protocol redesign: `generation` is a per-window counter the
    // compositor increments on every CONFIGURE it ever sends for that
    // window (window_t.configure_generation), starting from 1 for the
    // first one. Monotonic and never reused. A client applies a received
    // CONFIGURE only if its generation is greater than the highest one
    // already applied -- this is what makes a burst of CONFIGURE 40, 41,
    // 42 safe to receive in any read granularity: 40 and 41 are
    // correctly superseded, and only 42's geometry/fullscreen state ever
    // becomes current. See wmclient64.h's wm_window_t.
    WM_MSG_CONFIGURE       = 24,
} wm_msg_type_t;

#define WM_ERR_BAD_VERSION   -1
#define WM_ERR_BAD_WINDOW    -2
#define WM_ERR_BAD_SIZE      -3
#define WM_ERR_BAD_SURFACE   -4
#define WM_ERR_NO_RESOURCES  -5
#define WM_ERR_BAD_MESSAGE   -6

// Fixed-width, fixed-layout envelope -- every field always present
// (unused ones always 0), no unions, so the wire size never depends on
// which message type is active. All integer fields are plain
// uint32_t/int32_t/uint64_t (no bitfields, no implicit padding
// surprises beyond normal struct alignment, which is identical on both
// ends since compositor and clients are built with the same compiler).
typedef struct {
    uint32_t type;        // wm_msg_type_t
    uint32_t version;     // WM_PROTO_VERSION -- receiver must reject a mismatch
    uint32_t client_id;   // assigned at WM_MSG_WELCOME; echoed back by well-behaved clients (advisory -- the pipe itself is what actually scopes a connection)
    uint32_t window_id;   // subject window, 0 if not applicable
    int32_t  x, y;        // position / pointer coords / damage rect origin / (WM_MSG_ERROR: x = error code)
    uint32_t w, h;        // size / damage rect size
    uint64_t shm_token;   // WM_MSG_ATTACH_SURFACE: the surface's shm64 token
    uint32_t format;      // WM_MSG_ATTACH_SURFACE: WM_FORMAT_*
    uint32_t stride;      // WM_MSG_ATTACH_SURFACE: bytes per row
    uint32_t key_code;    // WM_MSG_KEY_EVENT: logical keycode (include/input64.h space)
    uint32_t pressed;     // WM_MSG_KEY_EVENT / WM_MSG_POINTER_BUTTON / WM_MSG_FOCUS: 1=down/gained, 0=up/lost
    uint32_t modifiers;   // WM_MSG_KEY_EVENT: modifier bitmask (include/input64.h's INPUT64_MOD_*)
    uint32_t ascii;       // WM_MSG_KEY_EVENT: translated character (0 if none)
    uint32_t button;      // WM_MSG_POINTER_BUTTON: which button (INPUT64_BTN_* bit value)
    // M+10A protocol redesign: WM_MSG_CONFIGURE (the compositor's own
    // per-window monotonic counter, see that message's own comment),
    // WM_MSG_FRAME (the configure generation live when this callback was
    // granted), WM_MSG_COMMIT_BUFFER (the generation this content
    // answers, per the client's own last-applied CONFIGURE) -- 0 for
    // every other message type.
    uint32_t generation;
    char     title[WM_TITLE_MAX]; // WM_MSG_CREATE_WINDOW / WM_MSG_SET_TITLE -- NUL-padded, not guaranteed NUL-terminated if exactly WM_TITLE_MAX long
} wm_msg_t;

#endif // WMPROTO64_H
