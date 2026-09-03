// ToxenOS/userlib/toxui/tox_font.h — Milestone 33: scalable (TrueType)
// font loading, glyph rasterization, and metrics -- the replacement
// for the graphical UI's old fixed 8x16 bitmap font (kernel/fbterm64.c
// and user64/compositor64.c's OWN duplicated copy of that same font
// data both remain completely untouched and independent of this file;
// see this header's own "keep the kernel/debug console independent"
// note below).
//
// TrueType parsing/rasterization itself is
// userlib/toxui/third_party/stb_truetype.h (vendored, see
// tox_stb_impl.c's header comment for provenance/license) -- this file
// is the ToxenOS-side glue: reading a .ttf off the VFS, owning its
// bytes for as long as the font is used (stb_truetype keeps pointers
// INTO the original font data rather than copying it), and a small
// bounded glyph cache so drawing the same text every frame does not
// re-rasterize every glyph every time (see tox_font.c's header
// comment for the cache design).
//
// Ownership: tox_font_load allocates (via tox_heap.h) the raw file
// bytes AND the tox_font_t's own bookkeeping; tox_font_free releases
// everything it owns, INCLUDING every currently-cached glyph bitmap --
// no separate "clear the cache" step is needed or exposed. A failed
// load leaves `out` untouched and leaks nothing.
#ifndef TOX_FONT_H
#define TOX_FONT_H
#include <stdint.h>

// Bounded, reclaimable glyph cache -- see tox_font.c's header comment.
// Not "an advanced font atlas manager" (explicitly out of scope) --
// each cached glyph is its own small tox_alloc'd grayscale bitmap.
#define TOX_GLYPH_CACHE_SIZE 128

typedef struct {
    int used;
    uint32_t codepoint;
    int pixel_size;
    int advance_x;             // pixels, rounded to the nearest integer at this pixel size
    int bearing_x, bearing_y;  // offset from the pen position to the bitmap's top-left corner
    int width, height;         // bitmap dimensions in pixels (0x0 for a glyph with no visible ink, e.g. space)
    uint8_t* bitmap;           // tox_alloc'd, 1 byte/pixel anti-aliased coverage (0=transparent, 255=fully covered); NULL iff width*height==0
} tox_glyph_t;

typedef struct {
    uint8_t* file_data;   // tox_alloc'd raw .ttf bytes -- kept alive for stb_truetype's internal pointers-into-the-original-buffer
    uint64_t file_size;
    void* info;            // stbtt_fontinfo*, tox_alloc'd (opaque here so callers never need third_party/stb_truetype.h -- see tox_font.c)
    int units_per_em_ascent, units_per_em_descent, units_per_em_line_gap; // raw font-design-unit vmetrics, scaled per call in tox_font_get_metrics
    tox_glyph_t cache[TOX_GLYPH_CACHE_SIZE];
    int next_evict; // round-robin eviction index once the cache is full
} tox_font_t;

// Loads a TrueType (.ttf) font file from the ToxenOS filesystem.
// Returns 0 with `out` filled on success, or -1 (file not found/
// unreadable, or the data isn't a font stb_truetype recognizes) with
// `out` left completely untouched.
int tox_font_load(const char* path, tox_font_t* out);

// Releases every resource `font` owns (raw file bytes, the parsed
// stbtt_fontinfo, and every currently-cached glyph bitmap) and zeroes
// the struct. Safe on an already-zeroed/never-loaded tox_font_t.
void tox_font_free(tox_font_t* font);

typedef struct { int ascent, descent, line_gap; } tox_font_metrics_t;

// Vertical metrics (baseline-to-top, baseline-to-bottom, recommended
// extra inter-line spacing) scaled to `pixel_size` -- ascent is
// positive, descent is negative (matches stb_truetype's own
// convention, i.e. font-design-space "up" stays positive after
// scaling), exactly what a caller needs to position successive text
// baselines `ascent - descent + line_gap` pixels apart.
void tox_font_get_metrics(const tox_font_t* font, int pixel_size, tox_font_metrics_t* out);

// Returns a POINTER to a cached, rasterized glyph for `codepoint` at
// `pixel_size` (a UTF-32 codepoint -- see tox_text.h for UTF-8
// decoding into this), rasterizing and inserting it into the font's
// own bounded cache on a miss. The returned pointer is owned by
// `font`'s cache -- valid until that cache slot is evicted or
// tox_font_free runs; never free it yourself. Returns NULL only on
// allocation failure (a codepoint with no glyph in the font, e.g. an
// unmapped character, is NOT a failure -- it returns a valid,
// legitimately empty glyph, matching a normal font's ".notdef"/missing-
// glyph behavior).
const tox_glyph_t* tox_font_get_glyph(tox_font_t* font, uint32_t codepoint, int pixel_size);

// Kerning adjustment (pixels, may be negative) to apply BETWEEN two
// consecutive codepoints already drawn/measured via tox_font_get_glyph
// -- 0 if the font has no kerning table or the pair has no adjustment
// (both are completely normal, not errors).
int tox_font_get_kerning(const tox_font_t* font, uint32_t cp1, uint32_t cp2, int pixel_size);

#endif // TOX_FONT_H
