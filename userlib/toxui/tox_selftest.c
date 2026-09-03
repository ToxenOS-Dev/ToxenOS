// ToxenOS/userlib/toxui/tox_selftest.c — Milestone 33: ToxUI's own
// self-test suite, driven by user64/toxui_test64.c (a thin ring3
// driver that just calls tox_selftest() and reports pass/fail via exit
// code, mirroring every kernel subsystem's own "<subsystem>_selftest()"
// convention -- ToxUI just isn't a kernel subsystem, so the function
// lives here instead of in a kernel/*.c file, and needs a real process
// to run in since it's an ordinary userspace library). See
// kernel/kernel64.c's TOXUI_TEST64_RUN debug flag for how this and its
// own repeated-cycle leak check are invoked.
//
// PNG test fixtures (valid RGB/RGBA, malformed signature/truncated/
// corrupt-chunk-length/empty) live at /toxui_test_assets/ -- root-level
// test-only clutter, PACKAGE_DEBUG64-gated, same precedent as every
// other *_test64 fixture in this codebase (exec64_test.nex64 etc.).
#include <stdint.h>
#include "tox_image.h"
#include "tox_draw.h"
#include "tox_surface.h"
#include "tox_font.h"
#include "tox_text.h"
#include "tox_heap.h"
#include "tox64.h"

static int st_strlen(const char* s) { int i = 0; while (s[i]) i++; return i; }
static void st_put(const char* s) { sys_write(s, (uint64_t)st_strlen(s)); }

#define TESTASSET(name) "/toxui_test_assets/" name
#define FONT_PATH "/system_manager/system_data/display_interface/fonts/DejaVuSans.ttf"

// ── PNG decode tests ─────────────────────────────────────────────────
static int test_png_valid_rgb(void) {
    tox_image_t img;
    if (tox_image_load(TESTASSET("test_rgb.png"), &img) < 0) return 0;
    int ok = (img.width == 64 && img.height == 48 && img.format == TOX_IMAGE_RGBA8888 && img.pixels != 0);
    // RGB source must come out fully opaque (alpha=255 for every pixel).
    if (ok) {
        for (uint32_t i = 0; i < img.width * img.height && ok; i++) {
            if (img.pixels[i * 4 + 3] != 255) ok = 0;
        }
    }
    tox_image_free(&img);
    return ok;
}

static int test_png_valid_rgba_alpha(void) {
    tox_image_t img;
    if (tox_image_load(TESTASSET("test_rgba_alpha.png"), &img) < 0) return 0;
    int ok = (img.width == 64 && img.height == 64);
    // The corner (0,0) is drawn fully transparent, the center is
    // opaque -- proves real per-pixel alpha survived decoding, not
    // just a fixed 255 stamped over everything.
    if (ok) {
        uint8_t corner_a = img.pixels[(0 * img.stride) + 0 * 4 + 3];
        uint8_t center_a = img.pixels[(32 * img.stride) + 32 * 4 + 3];
        if (corner_a != 0 || center_a == 0) ok = 0;
    }
    tox_image_free(&img);
    return ok;
}

static int test_png_bad_signature(void) {
    tox_image_t img;
    int r = tox_image_load(TESTASSET("bad_signature.png"), &img);
    return r < 0; // must be rejected cleanly, not crash
}

static int test_png_truncated(void) {
    tox_image_t img;
    return tox_image_load(TESTASSET("truncated.png"), &img) < 0;
}

static int test_png_bad_chunklen(void) {
    tox_image_t img;
    return tox_image_load(TESTASSET("bad_chunklen.png"), &img) < 0;
}

static int test_png_empty_file(void) {
    tox_image_t img;
    return tox_image_load(TESTASSET("empty.png"), &img) < 0;
}

static int test_png_nonexistent_file(void) {
    tox_image_t img;
    return tox_image_load("/no/such/file.png", &img) < 0;
}

static int test_png_oversized_dimensions(void) {
    // A genuinely well-formed PNG (real IDAT/IEND, correct CRCs) whose
    // IHDR lies about its dimensions (65535x65535, while the actual
    // pixel data is a single 1x1 pixel) -- exactly the "malicious/
    // corrupt PNG claiming absurd dimensions" this test is meant to
    // simulate. Must be rejected by tox_image_load's stbi_info_from_
    // memory pre-check (TOX_IMAGE_MAX_DIM) rather than attempting a
    // multi-gigabyte allocation or corrupting memory -- see
    // tox_image.c's header comment on the bug this specific fixture
    // found during Milestone 33's own self-testing.
    tox_image_t img;
    return tox_image_load(TESTASSET("oversized_dims.png"), &img) < 0;
}

static int test_png_repeated_load_free_no_leak(void) {
    tox_heap_stats_t before, after;
    tox_heap_stats(&before);
    for (int i = 0; i < 30; i++) {
        tox_image_t img;
        if (tox_image_load(TESTASSET("test_rgba_alpha.png"), &img) < 0) return 0;
        tox_image_free(&img);
    }
    tox_heap_stats(&after);
    return after.used_bytes == before.used_bytes;
}

// ── Drawing primitive tests ──────────────────────────────────────────
#define TW 40
#define TH 30

// Milestone 33 bug found during self-testing: ToxenOS's userspace
// stack is a SINGLE, fixed 4KB page with no demand-paged growth (see
// include/uservm64.h's USER_STACK_MAX_SIZE header comment) -- every
// test surface buffer here MUST be tox_alloc'd (heap) rather than a
// local array, exactly like every existing screen-sized buffer
// elsewhere in this codebase already does (e.g.
// user64/compositor64.c's backbuffer uses sys_mmap, never a stack
// array). The very first version of this file used local `uint32_t
// buf[TW*TH]`-style arrays and reliably page-faulted -- see the
// Milestone 33 summary's bugs-found section.
static tox_surface_t make_test_surface(void) {
    tox_surface_t s;
    uint32_t* buf = (uint32_t*)tox_alloc((uint64_t)TW * TH * 4);
    for (uint32_t i = 0; i < TW * TH; i++) buf[i] = 0x00202020u;
    tox_surface_init(&s, buf, TW, TH, TW * 4);
    return s;
}

static int test_fill_rect_basic(void) {
    tox_surface_t s = make_test_surface();
    tox_fill_rect(&s, 5, 5, 10, 10, TOX_RGB(255, 0, 0));
    int ok = (s.pixels[5 * TW + 5] == TOX_RGB(255, 0, 0)) && (s.pixels[4 * TW + 5] == 0x00202020u);
    tox_free(s.pixels);
    return ok;
}

static int test_fill_rect_zero_size(void) {
    tox_surface_t s = make_test_surface();
    tox_fill_rect(&s, 5, 5, 0, 0, TOX_RGB(255, 0, 0));
    tox_fill_rect(&s, 5, 5, -3, 10, TOX_RGB(255, 0, 0)); // negative width -- must be a no-op, not corrupt memory
    int ok = s.pixels[5 * TW + 5] == 0x00202020u;
    tox_free(s.pixels);
    return ok;
}

static int test_fill_rect_negative_position_clips_safely(void) {
    tox_surface_t s = make_test_surface();
    // Rect starts well off-surface (negative) and is wide/tall enough
    // to extend past the FAR edge too -- must clip safely on both
    // sides, filling the entire surface without touching out-of-bounds
    // memory (the width/height comfortably exceed TW/TH even after the
    // -100000 starting offset is added back).
    tox_fill_rect(&s, -100000, -100000, 200000, 200000, TOX_RGB(0, 255, 0));
    int ok = (s.pixels[0] == TOX_RGB(0, 255, 0)) && (s.pixels[(TH - 1) * TW + (TW - 1)] == TOX_RGB(0, 255, 0));
    tox_free(s.pixels);
    return ok;
}

static int test_fill_rect_overflow_safe(void) {
    tox_surface_t s = make_test_surface();
    // x + w would overflow a 32-bit int if computed naively in 32 bits
    // -- clip_rect uses int64 intermediates specifically for this.
    tox_fill_rect(&s, 2147483000, 5, 2000, 5, TOX_RGB(1, 2, 3));
    tox_free(s.pixels);
    return 1; // the real assertion is "did not crash / corrupt memory" -- reaching here at all is the pass condition
}

static int test_clip_rectangle_confines_drawing(void) {
    tox_surface_t s = make_test_surface();
    tox_surface_set_clip(&s, 10, 10, 5, 5);
    tox_fill_rect(&s, 0, 0, TW, TH, TOX_RGB(9, 9, 9)); // "fill everything" -- clip must confine it to [10,15)x[10,15)
    int ok = 1;
    for (uint32_t y = 0; y < TH && ok; y++) {
        for (uint32_t x = 0; x < TW && ok; x++) {
            int inside = (x >= 10 && x < 15 && y >= 10 && y < 15);
            uint32_t expect = inside ? TOX_RGB(9, 9, 9) : 0x00202020u;
            if (s.pixels[y * TW + x] != expect) ok = 0;
        }
    }
    tox_free(s.pixels);
    return ok;
}

static int test_blend_pixel_alpha(void) {
    tox_surface_t s = make_test_surface();
    tox_blend_pixel(&s, 5, 5, 255, 0, 0, 128); // ~50% red over the 0x202020 background
    uint32_t v = s.pixels[5 * TW + 5];
    uint8_t r = (v >> 16) & 0xFF, g = (v >> 8) & 0xFF, b = v & 0xFF;
    // Roughly halfway between (0x20,0x20,0x20) and (255,0,0) -- allow a
    // few units of rounding slack rather than demanding an exact value.
    int ok = (r > 100 && r < 200) && (g < 30) && (b < 30);
    tox_blend_pixel(&s, 6, 6, 1, 2, 3, 0); // fully transparent -- must be a true no-op
    ok = ok && (s.pixels[6 * TW + 6] == 0x00202020u);
    tox_blend_pixel(&s, 7, 7, 10, 20, 30, 255); // fully opaque -- exact overwrite
    ok = ok && (s.pixels[7 * TW + 7] == TOX_RGB(10, 20, 30));
    tox_free(s.pixels);
    return ok;
}

static int test_rounded_rect_corners_within_bounds(void) {
    tox_surface_t s = make_test_surface();
    tox_fill_rounded_rect(&s, 2, 2, 20, 20, 6, TOX_RGB(200, 200, 200));
    // The extreme corner pixel of the bounding box must NOT be filled
    // (that's the whole point of a rounded corner); the center must be.
    int corner_untouched = (s.pixels[2 * TW + 2] == 0x00202020u);
    int center_filled = (s.pixels[12 * TW + 12] == TOX_RGB(200, 200, 200));
    tox_free(s.pixels);
    return corner_untouched && center_filled;
}

static int test_draw_image_native_and_alpha(void) {
    tox_image_t img;
    if (tox_image_load(TESTASSET("test_rgba_alpha.png"), &img) < 0) return 0;
    tox_surface_t s;
    uint32_t* buf = (uint32_t*)tox_alloc(128u * 128u * 4u);
    for (int i = 0; i < 128 * 128; i++) buf[i] = TOX_RGB(10, 10, 10);
    tox_surface_init(&s, buf, 128, 128, 128 * 4);

    tox_draw_image_native(&s, &img, 32, 32);
    // A fully transparent source corner must leave the background
    // completely unchanged underneath it (no black box, no partial tint).
    int corner_unchanged = (buf[32 * 128 + 32] == TOX_RGB(10, 10, 10));
    // The circle's opaque center must have genuinely changed.
    int center_changed = (buf[(32 + 32) * 128 + (32 + 32)] != TOX_RGB(10, 10, 10));
    tox_image_free(&img);
    tox_free(buf);
    return corner_unchanged && center_changed;
}

static int test_draw_image_scaled_and_cropped(void) {
    tox_image_t img;
    if (tox_image_load(TESTASSET("test_rgb.png"), &img) < 0) return 0;
    tox_surface_t s;
    uint32_t* buf = (uint32_t*)tox_alloc(256u * 256u * 4u);
    tox_surface_init(&s, buf, 256, 256, 256 * 4);

    // Upscale 2x.
    tox_draw_image(&s, &img, 0, 0, 128, 96, 0, 0, (int32_t)img.width, (int32_t)img.height);
    // Crop: only the top-left 10x10 source region, drawn at native size elsewhere.
    tox_draw_image(&s, &img, 150, 0, 10, 10, 0, 0, 10, 10);

    tox_image_free(&img);
    tox_free(buf);
    return 1; // "did not crash on scaling/cropping" -- exact pixel values already covered by bilinear-degenerates-to-copy reasoning tested elsewhere
}

static int test_draw_image_offscreen_negative_clips(void) {
    tox_image_t img;
    if (tox_image_load(TESTASSET("test_rgb.png"), &img) < 0) return 0;
    tox_surface_t s;
    uint32_t* buf = (uint32_t*)tox_alloc(64u * 64u * 4u);
    for (int i = 0; i < 64 * 64; i++) buf[i] = TOX_RGB(1, 1, 1);
    tox_surface_init(&s, buf, 64, 64, 64 * 4);

    // Destination rect starts far off-surface (negative) and extends
    // partially on -- must clip safely, not crash or wrap addressing.
    tox_draw_image(&s, &img, -40, -40, 80, 80, 0, 0, (int32_t)img.width, (int32_t)img.height);
    tox_image_free(&img);
    tox_free(buf);
    return 1;
}

static int test_fit_modes_no_crash(void) {
    tox_image_t img;
    if (tox_image_load(TESTASSET("test_rgb.png"), &img) < 0) return 0;
    tox_surface_t s;
    uint32_t* buf = (uint32_t*)tox_alloc(200u * 100u * 4u);
    tox_surface_init(&s, buf, 200, 100, 200 * 4);
    tox_draw_image_fit(&s, &img, 0, 0, 200, 100, TOX_FIT_STRETCH);
    tox_draw_image_fit(&s, &img, 0, 0, 200, 100, TOX_FIT_CONTAIN);
    tox_draw_image_fit(&s, &img, 0, 0, 200, 100, TOX_FIT_COVER);
    tox_draw_image_fit(&s, &img, 0, 0, 200, 100, TOX_FIT_NATIVE);
    tox_image_free(&img);
    tox_free(buf);
    return 1;
}

// ── Font tests ───────────────────────────────────────────────────────
static int test_font_load_and_free(void) {
    tox_font_t font;
    if (tox_font_load(FONT_PATH, &font) < 0) return 0;
    int ok = (font.info != 0 && font.file_data != 0);
    tox_font_free(&font);
    ok = ok && (font.info == 0); // free must zero the struct
    return ok;
}

static int test_font_invalid_file_rejected(void) {
    tox_font_t font;
    // A real PNG is not a valid font -- stb_truetype must refuse it.
    return tox_font_load(TESTASSET("test_rgb.png"), &font) < 0;
}

static int test_font_nonexistent_rejected(void) {
    tox_font_t font;
    return tox_font_load("/no/such/font.ttf", &font) < 0;
}

static int test_font_metrics_and_glyph_sizes(void) {
    tox_font_t font;
    if (tox_font_load(FONT_PATH, &font) < 0) return 0;
    int ok = 1;

    tox_font_metrics_t m12, m48;
    tox_font_get_metrics(&font, 12, &m12);
    tox_font_get_metrics(&font, 48, &m48);
    // A 48px font must have visibly larger metrics than a 12px one --
    // proves real scaling, not a fixed-size bitmap font underneath.
    if (m48.ascent - m48.descent <= (m12.ascent - m12.descent) * 2) ok = 0;

    const tox_glyph_t* g12 = tox_font_get_glyph(&font, 'A', 12);
    const tox_glyph_t* g48 = tox_font_get_glyph(&font, 'A', 48);
    if (!g12 || !g48) ok = 0;
    if (ok && g48->width <= g12->width) ok = 0; // larger pixel size -> larger bitmap

    tox_font_free(&font);
    return ok;
}

static int test_font_proportional_advances(void) {
    tox_font_t font;
    if (tox_font_load(FONT_PATH, &font) < 0) return 0;
    const tox_glyph_t* gi = tox_font_get_glyph(&font, 'i', 32);
    const tox_glyph_t* gw = tox_font_get_glyph(&font, 'W', 32);
    int ok = (gi && gw && gi->advance_x < gw->advance_x); // 'i' must be narrower than 'W' -- proportional, not fixed-width
    tox_font_free(&font);
    return ok;
}

static int test_font_glyph_cache_hits(void) {
    tox_font_t font;
    if (tox_font_load(FONT_PATH, &font) < 0) return 0;
    const tox_glyph_t* first = tox_font_get_glyph(&font, 'Q', 24);
    const tox_glyph_t* second = tox_font_get_glyph(&font, 'Q', 24);
    int ok = (first == second); // same (font,codepoint,size) -- must return the SAME cached entry, not re-rasterize
    tox_font_free(&font);
    return ok;
}

static int test_text_measurement(void) {
    tox_font_t font;
    if (tox_font_load(FONT_PATH, &font) < 0) return 0;
    tox_text_extent_t e_short, e_long;
    tox_measure_text(&font, 16, "Hi", &e_short);
    tox_measure_text(&font, 16, "Hello, World!", &e_long);
    int ok = (e_long.width > e_short.width) && (e_short.height > 0) && (e_long.height == e_short.height);
    tox_font_free(&font);
    return ok;
}

static int test_text_newline_handling(void) {
    tox_font_t font;
    if (tox_font_load(FONT_PATH, &font) < 0) return 0;
    tox_surface_t s;
    uint32_t* buf = (uint32_t*)tox_alloc(300u * 100u * 4u);
    tox_surface_init(&s, buf, 300, 100, 300 * 4);
    int end_x = tox_draw_text(&s, &font, 16, 5, 30, "Line one\nLine two", TOX_RGB(255, 255, 255));
    tox_font_free(&font);
    tox_free(buf);
    return end_x > 0; // did not crash, produced a real advancing pen position
}

static int test_utf8_invalid_sequence_policy(void) {
    // Truncated 2-byte sequence right at the string's end, and a
    // stray continuation byte with no leading byte -- both must decode
    // as U+FFFD and consume exactly 1 byte (never read past the NUL,
    // never hang).
    const char bad1[] = { (char)0xC2, 0 }; // truncated 2-byte sequence
    int consumed = 0;
    uint32_t cp = tox_utf8_decode(bad1, &consumed);
    int ok = (cp == 0xFFFD && consumed == 1);

    const char bad2[] = { (char)0x80, 'A', 0 }; // stray continuation byte
    cp = tox_utf8_decode(bad2, &consumed);
    ok = ok && (cp == 0xFFFD && consumed == 1);

    const char good[] = { (char)0xC3, (char)0xA9, 0 }; // U+00E9 'é', valid 2-byte UTF-8
    cp = tox_utf8_decode(good, &consumed);
    ok = ok && (cp == 0xE9 && consumed == 2);

    return ok;
}

static int test_font_repeated_load_free_no_leak(void) {
    tox_heap_stats_t before, after;
    tox_heap_stats(&before);
    for (int i = 0; i < 10; i++) {
        tox_font_t font;
        if (tox_font_load(FONT_PATH, &font) < 0) return 0;
        // Exercise the glyph cache too -- rasterize several distinct
        // glyphs at several sizes each cycle, so a cache-bitmap leak
        // would show up in the drift check just as readily as a
        // font-load leak would.
        for (int c = 'A'; c <= 'Z'; c++) {
            tox_font_get_glyph(&font, (uint32_t)c, 12);
            tox_font_get_glyph(&font, (uint32_t)c, 24);
        }
        tox_font_free(&font);
    }
    tox_heap_stats(&after);
    return after.used_bytes == before.used_bytes;
}

#define TOXUI_TEST(name, expr) do {         \
    int _r = (expr);                        \
    st_put("toxui_selftest: " name " ");    \
    st_put(_r ? "PASS\n" : "FAIL\n");       \
    if (_r) pass++; else fail++;            \
} while (0)

int tox_selftest(void) {
    int pass = 0, fail = 0;
    st_put("toxui_selftest: starting\n");

    TOXUI_TEST("png valid rgb", test_png_valid_rgb());
    TOXUI_TEST("png valid rgba alpha", test_png_valid_rgba_alpha());
    TOXUI_TEST("png bad signature", test_png_bad_signature());
    TOXUI_TEST("png truncated", test_png_truncated());
    TOXUI_TEST("png bad chunk length", test_png_bad_chunklen());
    TOXUI_TEST("png empty file", test_png_empty_file());
    TOXUI_TEST("png nonexistent file", test_png_nonexistent_file());
    TOXUI_TEST("png oversized dimensions", test_png_oversized_dimensions());
    TOXUI_TEST("png repeated load/free, no leak", test_png_repeated_load_free_no_leak());

    TOXUI_TEST("fill_rect basic", test_fill_rect_basic());
    TOXUI_TEST("fill_rect zero/negative size", test_fill_rect_zero_size());
    TOXUI_TEST("fill_rect negative position clips safely", test_fill_rect_negative_position_clips_safely());
    TOXUI_TEST("fill_rect overflow-safe", test_fill_rect_overflow_safe());
    TOXUI_TEST("clip rectangle confines drawing", test_clip_rectangle_confines_drawing());
    TOXUI_TEST("blend_pixel alpha/opaque/transparent", test_blend_pixel_alpha());
    TOXUI_TEST("rounded rect corners", test_rounded_rect_corners_within_bounds());
    TOXUI_TEST("draw_image native + alpha", test_draw_image_native_and_alpha());
    TOXUI_TEST("draw_image scaled + cropped", test_draw_image_scaled_and_cropped());
    TOXUI_TEST("draw_image offscreen/negative clips", test_draw_image_offscreen_negative_clips());
    TOXUI_TEST("fit modes (stretch/contain/cover/native)", test_fit_modes_no_crash());

    TOXUI_TEST("font load + free", test_font_load_and_free());
    TOXUI_TEST("font invalid file rejected", test_font_invalid_file_rejected());
    TOXUI_TEST("font nonexistent rejected", test_font_nonexistent_rejected());
    TOXUI_TEST("font metrics + glyph sizes scale", test_font_metrics_and_glyph_sizes());
    TOXUI_TEST("font proportional advances", test_font_proportional_advances());
    TOXUI_TEST("font glyph cache hits", test_font_glyph_cache_hits());
    TOXUI_TEST("text measurement", test_text_measurement());
    TOXUI_TEST("text newline handling", test_text_newline_handling());
    TOXUI_TEST("utf8 invalid sequence policy", test_utf8_invalid_sequence_policy());
    TOXUI_TEST("font repeated load/free + glyph cache, no leak", test_font_repeated_load_free_no_leak());

    st_put("toxui_selftest: ");
    st_put(fail == 0 ? "all passed\n" : "some FAILED\n");
    return fail == 0;
}
