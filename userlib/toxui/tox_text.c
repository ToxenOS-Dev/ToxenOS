// ToxenOS/userlib/toxui/tox_text.c — see tox_text.h's header comment.
#include <stdint.h>
#include "tox_text.h"

uint32_t tox_utf8_decode(const char* s, int* bytes_consumed) {
    uint8_t b0 = (uint8_t)s[0];
    if (b0 < 0x80) { *bytes_consumed = 1; return b0; }

    int extra;
    uint32_t cp;
    if      ((b0 & 0xE0) == 0xC0) { extra = 1; cp = b0 & 0x1F; }
    else if ((b0 & 0xF0) == 0xE0) { extra = 2; cp = b0 & 0x0F; }
    else if ((b0 & 0xF8) == 0xF0) { extra = 3; cp = b0 & 0x07; }
    else { *bytes_consumed = 1; return 0xFFFD; } // invalid leading byte

    for (int i = 1; i <= extra; i++) {
        uint8_t bi = (uint8_t)s[i];
        // bi==0 check first -- never reads past the string's own NUL
        // terminator even for a sequence truncated right at the end.
        if (bi == 0 || (bi & 0xC0) != 0x80) { *bytes_consumed = 1; return 0xFFFD; }
        cp = (cp << 6) | (uint32_t)(bi & 0x3F);
    }
    *bytes_consumed = extra + 1;
    return cp;
}

int tox_draw_text(tox_surface_t* dst, tox_font_t* font, int pixel_size,
                   int32_t x, int32_t y, const char* utf8, tox_color_t color) {
    uint8_t cr = (uint8_t)((color >> 16) & 0xFF);
    uint8_t cg = (uint8_t)((color >> 8) & 0xFF);
    uint8_t cb = (uint8_t)(color & 0xFF);

    tox_font_metrics_t m;
    tox_font_get_metrics(font, pixel_size, &m);
    int32_t line_advance = m.ascent - m.descent + m.line_gap;

    int32_t pen_x = x, pen_y = y, start_x = x;
    uint32_t prev_cp = 0;
    int have_prev = 0;

    const char* p = utf8;
    while (*p) {
        int consumed;
        uint32_t cp = tox_utf8_decode(p, &consumed);
        p += consumed;

        if (cp == (uint32_t)'\n') {
            pen_x = start_x;
            pen_y += line_advance;
            have_prev = 0;
            continue;
        }

        if (have_prev) pen_x += tox_font_get_kerning(font, prev_cp, cp, pixel_size);

        const tox_glyph_t* g = tox_font_get_glyph(font, cp, pixel_size);
        if (g) {
            if (g->bitmap) {
                for (int gy = 0; gy < g->height; gy++) {
                    const uint8_t* row = g->bitmap + (uint64_t)gy * (uint64_t)g->width;
                    for (int gx = 0; gx < g->width; gx++) {
                        uint8_t cov = row[gx];
                        if (cov == 0) continue;
                        tox_blend_pixel(dst, pen_x + g->bearing_x + gx, pen_y + g->bearing_y + gy, cr, cg, cb, cov);
                    }
                }
            }
            pen_x += g->advance_x;
        }
        prev_cp = cp;
        have_prev = 1;
    }
    return pen_x;
}

void tox_measure_text(tox_font_t* font, int pixel_size, const char* utf8, tox_text_extent_t* out) {
    tox_font_metrics_t m;
    tox_font_get_metrics(font, pixel_size, &m);

    int32_t pen_x = 0;
    uint32_t prev_cp = 0;
    int have_prev = 0;

    const char* p = utf8;
    while (*p) {
        int consumed;
        uint32_t cp = tox_utf8_decode(p, &consumed);
        if (cp == (uint32_t)'\n') break; // single-line measurement only, see header comment
        p += consumed;

        if (have_prev) pen_x += tox_font_get_kerning(font, prev_cp, cp, pixel_size);
        const tox_glyph_t* g = tox_font_get_glyph(font, cp, pixel_size);
        if (g) pen_x += g->advance_x;
        prev_cp = cp;
        have_prev = 1;
    }

    out->width = pen_x;
    out->height = m.ascent - m.descent;
}
