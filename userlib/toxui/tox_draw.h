// ToxenOS/userlib/toxui/tox_draw.h — Milestone 33: core software 2D
// drawing primitives ToxUI (and, later, real widgets) build on. Shapes
// only -- no buttons/sliders/panels/etc, see this file's own
// implementation header comment for the full non-goal list.
//
// Every primitive here respects `dst`'s width/height/stride AND its
// current clip rectangle (tox_surface_set_clip/tox_surface_clip_reset
// below) -- drawing partially or entirely outside either clips safely
// (never reads/writes outside the surface's actual pixel buffer, never
// produces unsigned-wraparound addressing from a negative coordinate).
// All coordinates are signed (int32_t) for exactly this reason: a
// widget positioned or scrolled partly off-surface is a normal case,
// not an error.
#ifndef TOX_DRAW_H
#define TOX_DRAW_H
#include <stdint.h>
#include "tox_surface.h"
#include "tox_image.h"

// 0x00RRGGBB, matching tox_surface_t's own pixel format exactly (see
// tox_surface.h) -- a color used for a SHAPE fill (rect/line/circle)
// has no separate alpha; those primitives are always fully opaque
// (there is no "translucent rectangle" primitive this milestone -- see
// tox_blend_pixel below if a future caller genuinely needs one, it's a
// two-line composition of that + tox_fill_rect's bounds logic).
typedef uint32_t tox_color_t;
#define TOX_RGB(r,g,b) (((uint32_t)(r) << 16) | ((uint32_t)(g) << 8) | (uint32_t)(b))

// ── Clipping ─────────────────────────────────────────────────────────
// One current clip rectangle per surface (no stack -- see this file's
// implementation header comment for why that's the right amount of
// complexity here). Defaults to the whole surface (set by
// tox_surface_init); every drawing primitive intersects its own target
// rect against BOTH this and the surface's own bounds before touching
// any pixel.
void tox_surface_set_clip(tox_surface_t* s, int32_t x, int32_t y, int32_t w, int32_t h);
void tox_surface_clip_reset(tox_surface_t* s); // clip = the whole surface again

// ── Shape primitives ─────────────────────────────────────────────────
void tox_fill_rect(tox_surface_t* dst, int32_t x, int32_t y, int32_t w, int32_t h, tox_color_t color);
void tox_draw_rect(tox_surface_t* dst, int32_t x, int32_t y, int32_t w, int32_t h, tox_color_t color); // 1px outline
void tox_fill_rounded_rect(tox_surface_t* dst, int32_t x, int32_t y, int32_t w, int32_t h, int32_t radius, tox_color_t color);
void tox_draw_line(tox_surface_t* dst, int32_t x0, int32_t y0, int32_t x1, int32_t y1, tox_color_t color);
void tox_fill_circle(tox_surface_t* dst, int32_t cx, int32_t cy, int32_t r, tox_color_t color);
void tox_draw_circle(tox_surface_t* dst, int32_t cx, int32_t cy, int32_t r, tox_color_t color); // 1px outline

// Blends a single RGBA source pixel (0-255 per channel) onto one
// destination pixel using standard "source over" alpha compositing --
// the primitive every other blending operation in this file (image
// drawing, anti-aliased glyph drawing in tox_text.c) is built from.
// alpha==255 is the common opaque fast path (a plain overwrite);
// alpha==0 is a no-op. Does its own bounds+clip check, so it is safe to
// call with an out-of-range (x,y).
void tox_blend_pixel(tox_surface_t* dst, int32_t x, int32_t y, uint8_t r, uint8_t g, uint8_t b, uint8_t a);

// ── Image drawing ────────────────────────────────────────────────────
// Draws the (src_x,src_y,src_w,src_h) rectangle of `img` (pass
// 0,0,img->width,img->height for the whole image) into the
// (dst_x,dst_y,dst_w,dst_h) rectangle of `dst`, resampling with
// bilinear interpolation when the source and destination rectangles
// differ in size (degenerates to an exact per-pixel copy when they
// match, including the common "native size, no scaling" case -- see
// tox_draw.c's header comment for why one code path correctly and
// efficiently covers both). Alpha-blended against `dst`'s existing
// content (source-over) -- an image with real transparency composites
// correctly, never producing a black box around transparent pixels.
// Clipped to `dst`'s current clip rectangle and bounds; a source
// rectangle extending outside `img`'s own bounds is clamped rather
// than reading out of range.
void tox_draw_image(tox_surface_t* dst, const tox_image_t* img,
                     int32_t dst_x, int32_t dst_y, int32_t dst_w, int32_t dst_h,
                     int32_t src_x, int32_t src_y, int32_t src_w, int32_t src_h);

// Convenience: the whole image, at its own native size, top-left at (x,y).
void tox_draw_image_native(tox_surface_t* dst, const tox_image_t* img, int32_t x, int32_t y);

// ── Fit modes ────────────────────────────────────────────────────────
// Generic scaling POLICY primitives -- deliberately kept here rather
// than inventing an application/desktop-shell-level "wallpaper
// manager": the caller (a future desktop shell, or Milestone 33's own
// wallpaper demo) decides which fit mode it wants for a given image
// and target rectangle; this library has no opinion of its own about
// which one a wallpaper, an icon, or anything else "should" use.
typedef enum {
    TOX_FIT_STRETCH = 0, // fills (dst_w,dst_h) exactly, ignoring the image's own aspect ratio
    TOX_FIT_CONTAIN,     // scales to fit entirely within (dst_w,dst_h), preserving aspect ratio, centered (may letterbox)
    TOX_FIT_COVER,       // scales to fully cover (dst_w,dst_h), preserving aspect ratio, centered (may crop)
    TOX_FIT_NATIVE,      // no scaling at all, centered within (dst_w,dst_h)
} tox_fit_mode_t;

void tox_draw_image_fit(tox_surface_t* dst, const tox_image_t* img,
                         int32_t dst_x, int32_t dst_y, int32_t dst_w, int32_t dst_h,
                         tox_fit_mode_t fit);

#endif // TOX_DRAW_H
