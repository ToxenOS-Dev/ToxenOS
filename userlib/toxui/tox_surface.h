// ToxenOS/userlib/toxui/tox_surface.h — Milestone 33: the application-
// surface abstraction every ToxUI drawing primitive operates on.
//
// Deliberately NOT a new framebuffer/display abstraction -- ToxenOS
// already has one of those (kernel/display64.c + the compositor's
// shared-memory client surfaces, Milestone 29/30). tox_surface_t just
// gives ToxUI code (and application code calling it) one small struct
// to pass around instead of a raw pixel pointer + three separate
// dimension arguments at every call site, and wraps EXACTLY the pixel
// format the existing graphics path already uses end to end:
//
//   user64/wmproto64.h's WM_FORMAT_XRGB8888 -- confirmed, not assumed,
//   by reading user64/gfx_demo64.c's own render_frame(): each pixel is
//   a uint32_t packed (R<<16)|(G<<8)|B (top byte unused/0), row-major,
//   tightly packed by default (stride == width*4). This is the SAME
//   format kernel/display64.c's DISPLAY64_FORMAT_LOGICAL_XRGB8888
//   ultimately presents to hardware and the compositor blits verbatim
//   from a client's attached surface -- so a tox_surface_t built
//   directly over a wmclient64 wm_attach_surface() mapping needs no
//   conversion at all to reach the screen.
//
// tox_surface_t does NOT own its pixel memory -- `pixels` typically
// points at a wm_attach_surface() shared-memory mapping (owned/
// unmapped by the application itself) or a plain heap/mmap buffer the
// caller manages. Contrast with tox_image_t (tox_image.h), which DOES
// own its decoded pixel data.
#ifndef TOX_SURFACE_H
#define TOX_SURFACE_H
#include <stdint.h>

typedef enum {
    TOX_SURFACE_XRGB8888 = 1, // 4 bytes/pixel, packed uint32_t 0x00RRGGBB, native endianness -- see header comment
} tox_surface_fmt_t;

typedef struct {
    uint32_t* pixels;       // row-major, NOT owned by this struct
    uint32_t width, height; // in pixels
    uint32_t stride;        // BYTES per row (>= width*4; may exceed it for a sub-rect view -- see tox_surface_view)
    tox_surface_fmt_t format;
    // Milestone 33: the current clip rectangle (tox_draw.h's
    // tox_surface_set_clip/tox_surface_clip_reset) -- every tox_draw.h
    // primitive intersects its own target rect against this AND the
    // surface's own (width,height) bounds before touching any pixel.
    // Defaults to the whole surface (set below).
    int32_t clip_x, clip_y;
    uint32_t clip_w, clip_h;
} tox_surface_t;

static inline void tox_surface_init(tox_surface_t* s, void* pixels, uint32_t width, uint32_t height, uint32_t stride) {
    s->pixels = (uint32_t*)pixels;
    s->width = width;
    s->height = height;
    s->stride = stride ? stride : width * 4u;
    s->format = TOX_SURFACE_XRGB8888;
    s->clip_x = 0; s->clip_y = 0; s->clip_w = width; s->clip_h = height;
}

// A read/write VIEW into a sub-rectangle of an existing surface,
// sharing its pixel memory (no copy) -- e.g. so a widget can draw into
// "its own" region of a window's surface using the exact same
// primitives, with clipping automatically bounded to that region. The
// caller must ensure (x,y,w,h) lies within `src`; out-of-range input
// produces a degenerate (0-sized) view rather than reading/writing out
// of bounds.
static inline void tox_surface_view(const tox_surface_t* src, int32_t x, int32_t y, uint32_t w, uint32_t h, tox_surface_t* out) {
    if (x < 0 || y < 0 || (uint32_t)x >= src->width || (uint32_t)y >= src->height) {
        out->pixels = 0; out->width = 0; out->height = 0; out->stride = 0; out->format = src->format;
        out->clip_x = 0; out->clip_y = 0; out->clip_w = 0; out->clip_h = 0;
        return;
    }
    uint32_t max_w = src->width - (uint32_t)x;
    uint32_t max_h = src->height - (uint32_t)y;
    out->width = w < max_w ? w : max_w;
    out->height = h < max_h ? h : max_h;
    out->stride = src->stride;
    out->format = src->format;
    out->pixels = (uint32_t*)((uint8_t*)src->pixels + (uint64_t)y * src->stride + (uint64_t)x * 4u);
    out->clip_x = 0; out->clip_y = 0; out->clip_w = out->width; out->clip_h = out->height;
}

#endif // TOX_SURFACE_H
