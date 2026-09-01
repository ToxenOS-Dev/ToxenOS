// kernel/pixfmt64.c — Milestone 29: framebuffer pixel-format handling.
// See include/pixfmt64.h for the design rationale (logical 0x00RRGGBB
// color space for every client, converted to the real hardware layout
// only here).
#include "../include/pixfmt64.h"
#include "../include/klog.h"

int pixfmt64_from_mb2(uint8_t bpp,
                       uint8_t red_pos,   uint8_t red_size,
                       uint8_t green_pos, uint8_t green_size,
                       uint8_t blue_pos,  uint8_t blue_size,
                       pixfmt64_t* out)
{
    if (bpp != 16 && bpp != 24 && bpp != 32) return -1;
    if (red_size == 0 || green_size == 0 || blue_size == 0) return -1;
    if (red_size > 8 || green_size > 8 || blue_size > 8) return -1; // no >8-bit channels this milestone
    if ((uint32_t)red_pos   + red_size   > bpp) return -1;
    if ((uint32_t)green_pos + green_size > bpp) return -1;
    if ((uint32_t)blue_pos  + blue_size  > bpp) return -1;

    // Overlap check: build each channel's bitmask and confirm their
    // union has exactly the sum of their individual population counts
    // (i.e. no bit claimed by more than one channel).
    uint32_t rmask = ((uint32_t)1 << red_size)   - 1;
    uint32_t gmask = ((uint32_t)1 << green_size) - 1;
    uint32_t bmask = ((uint32_t)1 << blue_size)  - 1;
    uint32_t rbits = rmask << red_pos;
    uint32_t gbits = gmask << green_pos;
    uint32_t bbits = bmask << blue_pos;
    if ((rbits & gbits) || (rbits & bbits) || (gbits & bbits)) return -1;

    out->bpp             = bpp;
    out->bytes_per_pixel = (uint8_t)(bpp / 8);
    out->red_pos    = red_pos;   out->red_size   = red_size;
    out->green_pos  = green_pos; out->green_size = green_size;
    out->blue_pos   = blue_pos;  out->blue_size  = blue_size;
    return 0;
}

uint32_t pixfmt64_pack(const pixfmt64_t* fmt, uint32_t rgb)
{
    uint32_t r = (rgb >> 16) & 0xFF;
    uint32_t g = (rgb >> 8)  & 0xFF;
    uint32_t b =  rgb        & 0xFF;

    // Scale each 8-bit channel down to the hardware field's width by
    // dropping the low bits (e.g. 8-bit -> 5-bit keeps the top 5 bits).
    uint32_t rv = r >> (8 - fmt->red_size);
    uint32_t gv = g >> (8 - fmt->green_size);
    uint32_t bv = b >> (8 - fmt->blue_size);

    return (rv << fmt->red_pos) | (gv << fmt->green_pos) | (bv << fmt->blue_pos);
}

static int check(int* pass, int* fail, const char* name, uint32_t got, uint32_t want) {
    if (got == want) { (*pass)++; return 1; }
    (*fail)++;
    klog("pixfmt64_selftest: FAIL "); klog(name); klog("\n");
    return 0;
}

int pixfmt64_selftest(void)
{
    int pass = 0, fail = 0;
    pixfmt64_t fmt;

    // XRGB8888 -- what QEMU/Bochs VBE actually reports (red_pos=16).
    pixfmt64_from_mb2(32, 16,8, 8,8, 0,8, &fmt);
    check(&pass, &fail, "xrgb8888 red",   pixfmt64_pack(&fmt, 0xFF0000), 0x00FF0000);
    check(&pass, &fail, "xrgb8888 green", pixfmt64_pack(&fmt, 0x00FF00), 0x0000FF00);
    check(&pass, &fail, "xrgb8888 blue",  pixfmt64_pack(&fmt, 0x0000FF), 0x000000FF);
    check(&pass, &fail, "xrgb8888 white", pixfmt64_pack(&fmt, 0xFFFFFF), 0x00FFFFFF);
    klog(fail == 0 ? "pixfmt64_selftest: XRGB8888 (red_pos=16) PASS\n" : "");

    // Synthetic "BGRX8888" -- swapped field positions (red_pos=0,
    // blue_pos=16) -- not producible via QEMU's VBE, exercised directly
    // to prove pack() is genuinely position-driven, not hardcoded.
    int before = fail;
    pixfmt64_from_mb2(32, 0,8, 8,8, 16,8, &fmt);
    check(&pass, &fail, "bgrx8888 red",   pixfmt64_pack(&fmt, 0xFF0000), 0x000000FF); // red -> pos0
    check(&pass, &fail, "bgrx8888 green", pixfmt64_pack(&fmt, 0x00FF00), 0x0000FF00);
    check(&pass, &fail, "bgrx8888 blue",  pixfmt64_pack(&fmt, 0x0000FF), 0x00FF0000);
    klog(fail == before ? "pixfmt64_selftest: synthetic BGRX8888 (swapped field positions) PASS\n" : "");

    // RGB565 (16bpp): red_size=5, green_size=6, blue_size=5.
    before = fail;
    pixfmt64_from_mb2(16, 11,5, 5,6, 0,5, &fmt);
    check(&pass, &fail, "rgb565 red",   pixfmt64_pack(&fmt, 0xFF0000), 0xF800);
    check(&pass, &fail, "rgb565 green", pixfmt64_pack(&fmt, 0x00FF00), 0x07E0);
    check(&pass, &fail, "rgb565 blue",  pixfmt64_pack(&fmt, 0x0000FF), 0x001F);
    check(&pass, &fail, "rgb565 white", pixfmt64_pack(&fmt, 0xFFFFFF), 0xFFFF);
    klog(fail == before ? "pixfmt64_selftest: RGB565 (16bpp, 5/6/5) PASS\n" : "");

    // 24bpp, RGB-in-memory order (red_pos=0 -- deliberately the OPPOSITE
    // of XRGB8888's red_pos=16, so this genuinely exercises a different
    // byte order, not just a narrower bpp of the same layout).
    before = fail;
    pixfmt64_from_mb2(24, 0,8, 8,8, 16,8, &fmt);
    check(&pass, &fail, "rgb24 red",   pixfmt64_pack(&fmt, 0xFF0000), 0x0000FF);
    check(&pass, &fail, "rgb24 green", pixfmt64_pack(&fmt, 0x00FF00), 0x00FF00);
    check(&pass, &fail, "rgb24 blue",  pixfmt64_pack(&fmt, 0x0000FF), 0xFF0000);
    klog(fail == before ? "pixfmt64_selftest: 24bpp RGB-in-memory order PASS\n" : "");

    // Validation: bad bpp, overlapping fields, out-of-range field must
    // all be rejected.
    before = fail;
    int r1 = pixfmt64_from_mb2(8, 5,3, 2,3, 0,2, &fmt);   // unsupported bpp
    int r2 = pixfmt64_from_mb2(32, 0,16, 8,8, 16,8, &fmt); // red overlaps green/blue
    int r3 = pixfmt64_from_mb2(16, 12,5, 5,6, 0,5, &fmt);  // red_pos+size=17 > bpp=16
    if (r1 == 0) { fail++; klog("pixfmt64_selftest: FAIL accepted bpp=8\n"); } else pass++;
    if (r2 == 0) { fail++; klog("pixfmt64_selftest: FAIL accepted overlapping fields\n"); } else pass++;
    if (r3 == 0) { fail++; klog("pixfmt64_selftest: FAIL accepted out-of-range field\n"); } else pass++;
    klog(fail == before ? "pixfmt64_selftest: rejects unsupported/invalid formats PASS\n" : "");

    klog("pixfmt64_selftest: pass="); klog_hex("", (uint32_t)pass);
    klog("pixfmt64_selftest: fail="); klog_hex("", (uint32_t)fail);
    return fail == 0;
}
