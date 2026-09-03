// ToxenOS/userlib/toxui/tox_draw.c — see tox_draw.h's header comment.
//
// Image scaling: tox_draw_image uses ONE bilinear-resampling code path
// for every case (native size, upscale, downscale, crop) rather than a
// separate "fast exact copy" special case -- when src and dst rects
// are the same size, the bilinear sample point for each destination
// pixel lands exactly on the corresponding source pixel's center, so
// the interpolation weights degenerate to (1,0) and the result is
// byte-identical to a plain copy anyway. This keeps the primitive's
// logic in one place at a small, accepted cost in the native-size case
// (a few redundant floating-point multiplies per pixel).
//
// Alpha: straight (non-premultiplied) bilinear interpolation of all
// four RGBA channels together, including alpha itself. This can very
// slightly fringe colors right at a hard transparent/opaque edge under
// heavy scaling (a well-known, generally accepted tradeoff of this
// simpler technique vs. premultiplied-alpha resampling) -- judged an
// acceptable simplification for this milestone's "reusable image
// drawing primitives" scope, not a giant image-processing framework.
#include <stdint.h>
#include "tox_draw.h"
#include "tox_math.h"

void tox_surface_set_clip(tox_surface_t* s, int32_t x, int32_t y, int32_t w, int32_t h) {
    int64_t x0 = x, y0 = y;
    int64_t x1 = (int64_t)x + (w > 0 ? w : 0);
    int64_t y1 = (int64_t)y + (h > 0 ? h : 0);
    if (x0 < 0) x0 = 0;
    if (y0 < 0) y0 = 0;
    if (x1 > (int64_t)s->width) x1 = s->width;
    if (y1 > (int64_t)s->height) y1 = s->height;
    if (x1 < x0) x1 = x0;
    if (y1 < y0) y1 = y0;
    s->clip_x = (int32_t)x0;
    s->clip_y = (int32_t)y0;
    s->clip_w = (uint32_t)(x1 - x0);
    s->clip_h = (uint32_t)(y1 - y0);
}

void tox_surface_clip_reset(tox_surface_t* s) {
    s->clip_x = 0; s->clip_y = 0; s->clip_w = s->width; s->clip_h = s->height;
}

// Shared bounds+clip intersection, using int64 intermediates throughout
// specifically so a caller-supplied (x + w) can never wrap around in
// 32-bit arithmetic before the clamp gets a chance to catch it (see
// this file's own overflow-safety requirement). Returns 0 (nothing to
// draw) for a zero/negative-size input or a rect that clips away to
// nothing; never partially fills *out_* on that path.
static int clip_rect(const tox_surface_t* s, int32_t x, int32_t y, int32_t w, int32_t h,
                      int32_t* out_x0, int32_t* out_y0, int32_t* out_x1, int32_t* out_y1) {
    if (w <= 0 || h <= 0) return 0;
    int64_t x0 = x, y0 = y;
    int64_t x1 = (int64_t)x + w;
    int64_t y1 = (int64_t)y + h;
    int64_t cx0 = s->clip_x, cy0 = s->clip_y;
    int64_t cx1 = (int64_t)s->clip_x + (int64_t)s->clip_w;
    int64_t cy1 = (int64_t)s->clip_y + (int64_t)s->clip_h;
    if (x0 < cx0) x0 = cx0;
    if (y0 < cy0) y0 = cy0;
    if (x1 > cx1) x1 = cx1;
    if (y1 > cy1) y1 = cy1;
    if (x0 < 0) x0 = 0;
    if (y0 < 0) y0 = 0;
    if (x1 > (int64_t)s->width) x1 = s->width;
    if (y1 > (int64_t)s->height) y1 = s->height;
    if (x0 >= x1 || y0 >= y1) return 0;
    *out_x0 = (int32_t)x0; *out_y0 = (int32_t)y0; *out_x1 = (int32_t)x1; *out_y1 = (int32_t)y1;
    return 1;
}

void tox_fill_rect(tox_surface_t* dst, int32_t x, int32_t y, int32_t w, int32_t h, tox_color_t color) {
    int32_t x0, y0, x1, y1;
    if (!dst->pixels || !clip_rect(dst, x, y, w, h, &x0, &y0, &x1, &y1)) return;
    for (int32_t yy = y0; yy < y1; yy++) {
        uint32_t* row = (uint32_t*)((uint8_t*)dst->pixels + (uint64_t)yy * dst->stride);
        for (int32_t xx = x0; xx < x1; xx++) row[xx] = color;
    }
}

void tox_draw_rect(tox_surface_t* dst, int32_t x, int32_t y, int32_t w, int32_t h, tox_color_t color) {
    if (w <= 0 || h <= 0) return;
    tox_fill_rect(dst, x, y, w, 1, color);
    if (h > 1) tox_fill_rect(dst, x, y + h - 1, w, 1, color);
    tox_fill_rect(dst, x, y, 1, h, color);
    if (w > 1) tox_fill_rect(dst, x + w - 1, y, 1, h, color);
}

void tox_blend_pixel(tox_surface_t* dst, int32_t x, int32_t y, uint8_t r, uint8_t g, uint8_t b, uint8_t a) {
    if (!dst->pixels) return;
    if (x < dst->clip_x || y < dst->clip_y) return;
    if (x >= dst->clip_x + (int32_t)dst->clip_w || y >= dst->clip_y + (int32_t)dst->clip_h) return;
    if ((uint32_t)x >= dst->width || (uint32_t)y >= dst->height) return; // clip is already bounded to this, but stay defensive
    if (a == 0) return;

    uint32_t* p = (uint32_t*)((uint8_t*)dst->pixels + (uint64_t)y * dst->stride) + x;
    if (a == 255) { *p = TOX_RGB(r, g, b); return; }

    uint32_t old = *p;
    uint32_t or_ = (old >> 16) & 0xFF, og = (old >> 8) & 0xFF, ob = old & 0xFF;
    uint32_t inv = 255u - a;
    uint32_t nr = ((uint32_t)r * a + or_ * inv) / 255u;
    uint32_t ng = ((uint32_t)g * a + og * inv) / 255u;
    uint32_t nb = ((uint32_t)b * a + ob * inv) / 255u;
    *p = TOX_RGB(nr, ng, nb);
}

void tox_draw_line(tox_surface_t* dst, int32_t x0, int32_t y0, int32_t x1, int32_t y1, tox_color_t color) {
    uint8_t r = (uint8_t)((color >> 16) & 0xFF), g = (uint8_t)((color >> 8) & 0xFF), b = (uint8_t)(color & 0xFF);
    int32_t dx = x1 > x0 ? x1 - x0 : x0 - x1;
    int32_t dy = y1 > y0 ? y1 - y0 : y0 - y1;
    int32_t sx = x0 < x1 ? 1 : -1;
    int32_t sy = y0 < y1 ? 1 : -1;
    int32_t err = dx - dy;
    for (;;) {
        tox_blend_pixel(dst, x0, y0, r, g, b, 255);
        if (x0 == x1 && y0 == y1) break;
        int32_t e2 = 2 * err;
        if (e2 > -dy) { err -= dy; x0 += sx; }
        if (e2 < dx)  { err += dx; y0 += sy; }
    }
}

void tox_fill_circle(tox_surface_t* dst, int32_t cx, int32_t cy, int32_t r, tox_color_t color) {
    if (r <= 0) return;
    int64_t r2 = (int64_t)r * r;
    for (int32_t yy = -r; yy <= r; yy++) {
        int64_t dx2 = r2 - (int64_t)yy * yy;
        if (dx2 < 0) continue;
        int32_t dx = (int32_t)tox_sqrt((double)dx2);
        tox_fill_rect(dst, cx - dx, cy + yy, 2 * dx + 1, 1, color);
    }
}

void tox_draw_circle(tox_surface_t* dst, int32_t cx, int32_t cy, int32_t r, tox_color_t color) {
    if (r <= 0) return;
    uint8_t rr = (uint8_t)((color >> 16) & 0xFF), gg = (uint8_t)((color >> 8) & 0xFF), bb = (uint8_t)(color & 0xFF);
    int32_t x = r, y = 0, err = 0;
    while (x >= y) {
        tox_blend_pixel(dst, cx + x, cy + y, rr, gg, bb, 255);
        tox_blend_pixel(dst, cx + y, cy + x, rr, gg, bb, 255);
        tox_blend_pixel(dst, cx - y, cy + x, rr, gg, bb, 255);
        tox_blend_pixel(dst, cx - x, cy + y, rr, gg, bb, 255);
        tox_blend_pixel(dst, cx - x, cy - y, rr, gg, bb, 255);
        tox_blend_pixel(dst, cx - y, cy - x, rr, gg, bb, 255);
        tox_blend_pixel(dst, cx + y, cy - x, rr, gg, bb, 255);
        tox_blend_pixel(dst, cx + x, cy - y, rr, gg, bb, 255);
        y++;
        if (err <= 0) { err += 2 * y + 1; }
        if (err > 0)  { x--; err -= 2 * x + 1; }
    }
}

void tox_fill_rounded_rect(tox_surface_t* dst, int32_t x, int32_t y, int32_t w, int32_t h, int32_t radius, tox_color_t color) {
    if (w <= 0 || h <= 0) return;
    if (radius <= 0) { tox_fill_rect(dst, x, y, w, h, color); return; }
    int32_t maxr = (w < h ? w : h) / 2;
    if (radius > maxr) radius = maxr;
    if (radius <= 0) { tox_fill_rect(dst, x, y, w, h, color); return; }

    tox_fill_rect(dst, x + radius, y, w - 2 * radius, h, color);
    tox_fill_rect(dst, x, y + radius, radius, h - 2 * radius, color);
    tox_fill_rect(dst, x + w - radius, y + radius, radius, h - 2 * radius, color);

    for (int32_t yy = 0; yy < radius; yy++) {
        int64_t dy = radius - 1 - yy;
        int64_t dx2 = (int64_t)radius * radius - dy * dy;
        if (dx2 < 0) dx2 = 0;
        int32_t span = (int32_t)tox_sqrt((double)dx2);
        if (span <= 0) continue;
        tox_fill_rect(dst, x + radius - span, y + yy, span, 1, color);                 // top-left
        tox_fill_rect(dst, x + w - radius, y + yy, span, 1, color);                    // top-right
        tox_fill_rect(dst, x + radius - span, y + h - 1 - yy, span, 1, color);         // bottom-left
        tox_fill_rect(dst, x + w - radius, y + h - 1 - yy, span, 1, color);            // bottom-right
    }
}

// Bilinear-samples `img` at floating-point source coordinates
// (sx,sy), clamped to the (src_x,src_y,src_w,src_h) crop rectangle
// intersected with the image's own real bounds.
static void sample_bilinear(const tox_image_t* img, double sx, double sy,
                             int32_t src_x, int32_t src_y, int32_t src_w, int32_t src_h,
                             uint8_t* out_r, uint8_t* out_g, uint8_t* out_b, uint8_t* out_a) {
    double min_x = src_x, max_x = (double)src_x + src_w - 1;
    double min_y = src_y, max_y = (double)src_y + src_h - 1;
    if (min_x < 0) min_x = 0;
    if (min_y < 0) min_y = 0;
    if (max_x > (double)img->width - 1)  max_x = (double)img->width - 1;
    if (max_y > (double)img->height - 1) max_y = (double)img->height - 1;
    if (max_x < min_x) max_x = min_x;
    if (max_y < min_y) max_y = min_y;

    if (sx < min_x) sx = min_x; if (sx > max_x) sx = max_x;
    if (sy < min_y) sy = min_y; if (sy > max_y) sy = max_y;

    int32_t x0 = (int32_t)tox_floor(sx);
    int32_t y0 = (int32_t)tox_floor(sy);
    int32_t x1 = x0 + 1; if ((double)x1 > max_x) x1 = x0;
    int32_t y1 = y0 + 1; if ((double)y1 > max_y) y1 = y0;
    double fx = sx - (double)x0;
    double fy = sy - (double)y0;

    const uint8_t* p00 = img->pixels + (uint64_t)y0 * img->stride + (uint64_t)x0 * 4;
    const uint8_t* p10 = img->pixels + (uint64_t)y0 * img->stride + (uint64_t)x1 * 4;
    const uint8_t* p01 = img->pixels + (uint64_t)y1 * img->stride + (uint64_t)x0 * 4;
    const uint8_t* p11 = img->pixels + (uint64_t)y1 * img->stride + (uint64_t)x1 * 4;

    double w00 = (1 - fx) * (1 - fy), w10 = fx * (1 - fy), w01 = (1 - fx) * fy, w11 = fx * fy;
    double r = p00[0] * w00 + p10[0] * w10 + p01[0] * w01 + p11[0] * w11;
    double g = p00[1] * w00 + p10[1] * w10 + p01[1] * w01 + p11[1] * w11;
    double b = p00[2] * w00 + p10[2] * w10 + p01[2] * w01 + p11[2] * w11;
    double a = p00[3] * w00 + p10[3] * w10 + p01[3] * w01 + p11[3] * w11;
    *out_r = (uint8_t)(r + 0.5);
    *out_g = (uint8_t)(g + 0.5);
    *out_b = (uint8_t)(b + 0.5);
    *out_a = (uint8_t)(a + 0.5);
}

void tox_draw_image(tox_surface_t* dst, const tox_image_t* img,
                     int32_t dst_x, int32_t dst_y, int32_t dst_w, int32_t dst_h,
                     int32_t src_x, int32_t src_y, int32_t src_w, int32_t src_h) {
    if (!dst->pixels || !img->pixels) return;
    if (dst_w <= 0 || dst_h <= 0 || src_w <= 0 || src_h <= 0) return;

    int32_t cx0, cy0, cx1, cy1;
    if (!clip_rect(dst, dst_x, dst_y, dst_w, dst_h, &cx0, &cy0, &cx1, &cy1)) return;

    double scale_x = (double)src_w / (double)dst_w;
    double scale_y = (double)src_h / (double)dst_h;

    for (int32_t dy = cy0; dy < cy1; dy++) {
        double sy = (double)src_y + ((double)(dy - dst_y) + 0.5) * scale_y - 0.5;
        for (int32_t dx = cx0; dx < cx1; dx++) {
            double sx = (double)src_x + ((double)(dx - dst_x) + 0.5) * scale_x - 0.5;
            uint8_t r, g, b, a;
            sample_bilinear(img, sx, sy, src_x, src_y, src_w, src_h, &r, &g, &b, &a);
            tox_blend_pixel(dst, dx, dy, r, g, b, a);
        }
    }
}

void tox_draw_image_native(tox_surface_t* dst, const tox_image_t* img, int32_t x, int32_t y) {
    tox_draw_image(dst, img, x, y, (int32_t)img->width, (int32_t)img->height,
                   0, 0, (int32_t)img->width, (int32_t)img->height);
}

void tox_draw_image_fit(tox_surface_t* dst, const tox_image_t* img,
                         int32_t dst_x, int32_t dst_y, int32_t dst_w, int32_t dst_h,
                         tox_fit_mode_t fit) {
    if (dst_w <= 0 || dst_h <= 0 || img->width == 0 || img->height == 0) return;

    switch (fit) {
    case TOX_FIT_STRETCH:
        tox_draw_image(dst, img, dst_x, dst_y, dst_w, dst_h, 0, 0, (int32_t)img->width, (int32_t)img->height);
        return;
    case TOX_FIT_NATIVE: {
        int32_t x = dst_x + (dst_w - (int32_t)img->width) / 2;
        int32_t y = dst_y + (dst_h - (int32_t)img->height) / 2;
        tox_draw_image_native(dst, img, x, y);
        return;
    }
    case TOX_FIT_CONTAIN:
    case TOX_FIT_COVER: {
        double scale_x = (double)dst_w / (double)img->width;
        double scale_y = (double)dst_h / (double)img->height;
        double scale = (fit == TOX_FIT_CONTAIN) ? (scale_x < scale_y ? scale_x : scale_y)
                                                 : (scale_x > scale_y ? scale_x : scale_y);
        int32_t w = (int32_t)((double)img->width * scale + 0.5);
        int32_t h = (int32_t)((double)img->height * scale + 0.5);
        if (w < 1) w = 1;
        if (h < 1) h = 1;
        int32_t x = dst_x + (dst_w - w) / 2;
        int32_t y = dst_y + (dst_h - h) / 2;

        if (fit == TOX_FIT_COVER) {
            // The scaled image can exceed (dst_w,dst_h) here -- force a
            // temporary clip to exactly the intended destination
            // rectangle so the overflow is cropped rather than leaking
            // past it (a caller-set narrower clip is intentionally not
            // preserved through this -- see this function's own scope:
            // one clip rectangle, no stack).
            int32_t old_cx = dst->clip_x, old_cy = dst->clip_y;
            uint32_t old_cw = dst->clip_w, old_ch = dst->clip_h;
            tox_surface_set_clip(dst, dst_x, dst_y, dst_w, dst_h);
            tox_draw_image(dst, img, x, y, w, h, 0, 0, (int32_t)img->width, (int32_t)img->height);
            dst->clip_x = old_cx; dst->clip_y = old_cy; dst->clip_w = old_cw; dst->clip_h = old_ch;
        } else {
            tox_draw_image(dst, img, x, y, w, h, 0, 0, (int32_t)img->width, (int32_t)img->height);
        }
        return;
    }
    }
}
