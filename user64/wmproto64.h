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
    char     title[WM_TITLE_MAX]; // WM_MSG_CREATE_WINDOW / WM_MSG_SET_TITLE -- NUL-padded, not guaranteed NUL-terminated if exactly WM_TITLE_MAX long
} wm_msg_t;

#endif // WMPROTO64_H
