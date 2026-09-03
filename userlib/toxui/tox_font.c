// ToxenOS/userlib/toxui/tox_font.c — see tox_font.h's header comment.
//
// Glyph cache design: a fixed-size (TOX_GLYPH_CACHE_SIZE) array,
// linear-searched by (codepoint, pixel_size), round-robin-evicted when
// full (the oldest-inserted slot is reused next, freeing its bitmap
// first). Deliberately not a hash table or a font atlas/texture-packing
// manager -- normal UI text (a handful of distinct sizes, a few dozen
// to a couple hundred distinct glyphs on screen at once) fits
// comfortably within one bounded array with O(n) linear search, and
// "bounded, reclaimable, and repeated glyph draws don't re-rasterize"
// is the actual requirement this milestone set, not maximum
// theoretical throughput.
#include <stdint.h>
#include "tox_font.h"
#include "tox_heap.h"
#include "tox64.h"
#include "tox_stb_config.h"
#include "third_party/stb_truetype.h"

static int read_whole_file(const char* path, uint8_t** out_data, uint64_t* out_len) {
    uint64_t size = 0;
    if (sys_stat(path, &size, 0) < 0) return -1;
    if (size == 0 || size > 0x10000000ULL) return -1;

    int64_t fd = sys_open(path);
    if (fd < 0) return -1;

    uint8_t* buf = (uint8_t*)tox_alloc(size);
    if (!buf) { sys_close((int)fd); return -1; }

    uint64_t got = 0;
    while (got < size) {
        int64_t n = sys_read((int)fd, (char*)(buf + got), size - got);
        if (n <= 0) { sys_close((int)fd); tox_free(buf); return -1; }
        got += (uint64_t)n;
    }
    sys_close((int)fd);

    *out_data = buf;
    *out_len = size;
    return 0;
}

int tox_font_load(const char* path, tox_font_t* out) {
    uint8_t* data = 0;
    uint64_t len = 0;
    if (read_whole_file(path, &data, &len) < 0) return -1;
    if (len > 0x7FFFFFFFULL) { tox_free(data); return -1; } // stbtt_InitFont's own `int` offset arithmetic bounds this

    stbtt_fontinfo* info = (stbtt_fontinfo*)tox_alloc(sizeof(stbtt_fontinfo));
    if (!info) { tox_free(data); return -1; }

    if (!stbtt_InitFont(info, data, 0)) {
        // Malformed/unrecognized font data -- stb_truetype validated
        // the header/table directory internally and refused it. Unwind
        // everything allocated so far; nothing is left behind.
        tox_free(info);
        tox_free(data);
        return -1;
    }

    for (int i = 0; i < TOX_GLYPH_CACHE_SIZE; i++) {
        out->cache[i].used = 0;
        out->cache[i].bitmap = 0;
    }
    out->next_evict = 0;
    out->file_data = data;
    out->file_size = len;
    out->info = info;
    stbtt_GetFontVMetrics(info, &out->units_per_em_ascent, &out->units_per_em_descent, &out->units_per_em_line_gap);
    return 0;
}

void tox_font_free(tox_font_t* font) {
    if (!font->info) return;
    for (int i = 0; i < TOX_GLYPH_CACHE_SIZE; i++) {
        if (font->cache[i].bitmap) tox_free(font->cache[i].bitmap);
        font->cache[i].used = 0;
        font->cache[i].bitmap = 0;
    }
    tox_free(font->info);
    tox_free(font->file_data);
    font->info = 0;
    font->file_data = 0;
    font->file_size = 0;
}

void tox_font_get_metrics(const tox_font_t* font, int pixel_size, tox_font_metrics_t* out) {
    float scale = stbtt_ScaleForPixelHeight((const stbtt_fontinfo*)font->info, (float)pixel_size);
    out->ascent   = (int)((float)font->units_per_em_ascent * scale + 0.5f);
    out->descent  = (int)((float)font->units_per_em_descent * scale - 0.5f);
    out->line_gap = (int)((float)font->units_per_em_line_gap * scale + 0.5f);
}

const tox_glyph_t* tox_font_get_glyph(tox_font_t* font, uint32_t codepoint, int pixel_size) {
    for (int i = 0; i < TOX_GLYPH_CACHE_SIZE; i++) {
        if (font->cache[i].used && font->cache[i].codepoint == codepoint && font->cache[i].pixel_size == pixel_size)
            return &font->cache[i];
    }

    const stbtt_fontinfo* info = (const stbtt_fontinfo*)font->info;
    float scale = stbtt_ScaleForPixelHeight(info, (float)pixel_size);
    int glyph_index = stbtt_FindGlyphIndex(info, (int)codepoint);

    int advance = 0, lsb = 0;
    stbtt_GetGlyphHMetrics(info, glyph_index, &advance, &lsb);

    int x0 = 0, y0 = 0, x1 = 0, y1 = 0;
    stbtt_GetGlyphBitmapBox(info, glyph_index, scale, scale, &x0, &y0, &x1, &y1);
    int w = x1 - x0, h = y1 - y0;

    uint8_t* bitmap = 0;
    if (w > 0 && h > 0) {
        bitmap = (uint8_t*)tox_alloc((uint64_t)w * (uint64_t)h);
        if (!bitmap) return 0; // allocation failure -- genuinely can't proceed
        stbtt_MakeGlyphBitmap(info, bitmap, w, h, w, scale, scale, glyph_index);
    } else {
        w = 0; h = 0; // e.g. space -- a real, legitimately empty glyph, not an error
    }

    int slot = -1;
    for (int i = 0; i < TOX_GLYPH_CACHE_SIZE; i++) {
        if (!font->cache[i].used) { slot = i; break; }
    }
    if (slot < 0) {
        slot = font->next_evict;
        font->next_evict = (font->next_evict + 1) % TOX_GLYPH_CACHE_SIZE;
        if (font->cache[slot].bitmap) tox_free(font->cache[slot].bitmap);
    }

    tox_glyph_t* g = &font->cache[slot];
    g->used = 1;
    g->codepoint = codepoint;
    g->pixel_size = pixel_size;
    g->advance_x = (int)((float)advance * scale + 0.5f);
    g->bearing_x = x0;
    g->bearing_y = y0;
    g->width = w;
    g->height = h;
    g->bitmap = bitmap;
    return g;
}

int tox_font_get_kerning(const tox_font_t* font, uint32_t cp1, uint32_t cp2, int pixel_size) {
    const stbtt_fontinfo* info = (const stbtt_fontinfo*)font->info;
    float scale = stbtt_ScaleForPixelHeight(info, (float)pixel_size);
    int raw = stbtt_GetCodepointKernAdvance(info, (int)cp1, (int)cp2);
    return (int)((float)raw * scale + (raw >= 0 ? 0.5f : -0.5f));
}
