// kernel/display64.c — Milestone 29: low-level framebuffer/display layer.
// See include/display64.h for the design rationale and layering.
#include <stdint.h>
#include "../include/display64.h"
#include "../include/physmem64.h"
#include "../include/process64.h"
#include "../include/klog.h"

static uint64_t    fb_base  = 0;   // mapped virtual address, 0 = unavailable
static uint32_t    fb_width = 0, fb_height = 0, fb_pitch = 0;
static pixfmt64_t  fb_fmt;
static int         fb_avail = 0;
static uint32_t    owner_pid = 0;  // 0 = unowned (real pids start at 1)

static inline uint64_t lock(void) {
    uint64_t flags;
    __asm__ volatile ("pushfq\n\tpop %0\n\tcli" : "=r"(flags) :: "memory");
    return flags;
}
static inline void unlock(uint64_t flags) {
    if (flags & (1ULL << 9)) __asm__ volatile ("sti" ::: "memory");
}

int display64_acquire(void) {
    uint64_t flags = lock();
    if (owner_pid != 0) { unlock(flags); return -1; }
    int pid = process64_current_pid();
    if (pid <= 0) { unlock(flags); return -1; }
    owner_pid = (uint32_t)pid;
    unlock(flags);
    return 0;
}

void display64_release(void) {
    uint64_t flags = lock();
    owner_pid = 0;
    unlock(flags);
}

uint32_t display64_owner_pid(void) {
    uint64_t flags = lock();
    uint32_t p = owner_pid;
    unlock(flags);
    return p;
}

int display64_init(uint64_t fb_addr, uint32_t width, uint32_t height,
                    uint32_t pitch, const pixfmt64_t* fmt)
{
    fb_avail = 0;
    if (!fb_addr || !fmt || width < 320 || height < 200) return -1;

    uint64_t size = (uint64_t)height * (uint64_t)pitch;
    void* vaddr = physmem64_map_mmio(fb_addr, size);
    if (!vaddr) return -1;

    fb_base   = (uint64_t)(uintptr_t)vaddr;
    fb_width  = width;
    fb_height = height;
    fb_pitch  = pitch;
    fb_fmt    = *fmt;
    fb_avail  = 1;
    return 0;
}

int display64_available(void) { return fb_avail; }

void display64_get_info(display64_info_t* out)
{
    if (!out) return;
    out->width  = fb_width;
    out->height = fb_height;
    out->pitch  = fb_pitch;
    out->format = fb_fmt;
}

// Writes `native` (only the low bytes_per_pixel bytes are meaningful)
// at byte offset `off` into the framebuffer, in little-endian order --
// i.e. exactly how a real 16/24/32-bit little-endian store would lay
// the bytes out, so this works uniformly across all three supported
// depths without the compiler ever performing a misaligned wide store
// for the 24bpp case.
static inline void write_native(uint64_t off, uint32_t native)
{
    uint8_t* p = (uint8_t*)(uintptr_t)fb_base + off;
    switch (fb_fmt.bytes_per_pixel) {
    case 2:
        p[0] = (uint8_t)(native & 0xFF);
        p[1] = (uint8_t)((native >> 8) & 0xFF);
        break;
    case 3:
        p[0] = (uint8_t)(native & 0xFF);
        p[1] = (uint8_t)((native >> 8) & 0xFF);
        p[2] = (uint8_t)((native >> 16) & 0xFF);
        break;
    default: // 4
        p[0] = (uint8_t)(native & 0xFF);
        p[1] = (uint8_t)((native >> 8) & 0xFF);
        p[2] = (uint8_t)((native >> 16) & 0xFF);
        p[3] = (uint8_t)((native >> 24) & 0xFF);
        break;
    }
}

void display64_put_pixel(uint32_t x, uint32_t y, uint32_t rgb)
{
    if (!fb_avail || x >= fb_width || y >= fb_height) return;
    uint64_t off = (uint64_t)y * fb_pitch + (uint64_t)x * fb_fmt.bytes_per_pixel;
    write_native(off, pixfmt64_pack(&fb_fmt, rgb));
}

void display64_fill_rect(uint32_t x, uint32_t y, uint32_t w, uint32_t h, uint32_t rgb)
{
    if (!fb_avail) return;
    if (x >= fb_width || y >= fb_height) return;
    if (x + w > fb_width)  w = fb_width  - x;
    if (y + h > fb_height) h = fb_height - y;

    uint32_t native = pixfmt64_pack(&fb_fmt, rgb);
    for (uint32_t row = 0; row < h; row++) {
        uint64_t base = (uint64_t)(y + row) * fb_pitch + (uint64_t)x * fb_fmt.bytes_per_pixel;
        for (uint32_t col = 0; col < w; col++)
            write_native(base + (uint64_t)col * fb_fmt.bytes_per_pixel, native);
    }
}

void display64_copy_rows(uint32_t dst_y, uint32_t src_y, uint32_t num_rows)
{
    if (!fb_avail) return;
    if (src_y >= fb_height || dst_y >= fb_height) return;
    if (src_y + num_rows > fb_height) num_rows = fb_height - src_y;
    if (dst_y + num_rows > fb_height) num_rows = fb_height - dst_y;
    if (num_rows == 0) return;

    uint8_t* base = (uint8_t*)(uintptr_t)fb_base;
    uint64_t bytes_per_row = (uint64_t)fb_pitch;
    uint64_t total = bytes_per_row * num_rows;

    if (dst_y < src_y) {
        uint8_t* dst = base + (uint64_t)dst_y * bytes_per_row;
        uint8_t* src = base + (uint64_t)src_y * bytes_per_row;
        for (uint64_t i = 0; i < total; i++) dst[i] = src[i];
    } else if (dst_y > src_y) {
        // Copy backward so an overlapping downward move doesn't clobber
        // source rows before they're read (not currently exercised by
        // fbterm64's upward-only scroll, but keeps this primitive
        // correct for any future caller).
        for (uint64_t i = total; i > 0; i--) {
            uint8_t* dst = base + (uint64_t)dst_y * bytes_per_row;
            uint8_t* src = base + (uint64_t)src_y * bytes_per_row;
            dst[i - 1] = src[i - 1];
        }
    }
}

void display64_blit_row(uint32_t x, uint32_t y, uint32_t w, const uint32_t* logical_pixels)
{
    if (!fb_avail || y >= fb_height || x >= fb_width || !logical_pixels) return;
    if (x + w > fb_width) w = fb_width - x;

    uint64_t base = (uint64_t)y * fb_pitch + (uint64_t)x * fb_fmt.bytes_per_pixel;
    for (uint32_t col = 0; col < w; col++) {
        uint32_t native = pixfmt64_pack(&fb_fmt, logical_pixels[col]);
        write_native(base + (uint64_t)col * fb_fmt.bytes_per_pixel, native);
    }
}

static inline uint32_t read_native(uint64_t off)
{
    const uint8_t* p = (const uint8_t*)(uintptr_t)fb_base + off;
    switch (fb_fmt.bytes_per_pixel) {
    case 2: return (uint32_t)p[0] | ((uint32_t)p[1] << 8);
    case 3: return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16);
    default: return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
    }
}

uint32_t display64_read_pixel_raw(uint32_t x, uint32_t y)
{
    if (!fb_avail || x >= fb_width || y >= fb_height) return 0;
    return read_native((uint64_t)y * fb_pitch + (uint64_t)x * fb_fmt.bytes_per_pixel);
}

static int st_check(int* pass, int* fail, const char* name, int ok) {
    if (ok) { (*pass)++; return 1; }
    (*fail)++;
    klog("display64_selftest: FAIL "); klog(name); klog("\n");
    return 0;
}

int display64_selftest(void)
{
    int pass = 0, fail = 0;
    if (!fb_avail) {
        klog("display64_selftest: no display available -- SKIPPED\n");
        return 1;
    }

    uint32_t w = fb_width, h = fb_height;

    // Full-bounds coverage, including the true bottom-right corner --
    // exactly what fbterm64_clear relies on to avoid leaving a stale
    // strip when height isn't a multiple of the terminal cell height.
    uint32_t magic1 = pixfmt64_pack(&fb_fmt, 0x123456);
    display64_fill_rect(0, 0, w, h, 0x123456);
    int ok = display64_read_pixel_raw(0, 0) == magic1
           && display64_read_pixel_raw(w - 1, 0) == magic1
           && display64_read_pixel_raw(0, h - 1) == magic1
           && display64_read_pixel_raw(w - 1, h - 1) == magic1;
    st_check(&pass, &fail, "full-bounds fill (incl. true bottom-right corner)", ok);

    // Deliberately non-CHAR_H-aligned partial rectangle at the bottom
    // edge -- the exact shape scroll/clear use for the "remainder"
    // strip below the last whole character row.
    display64_fill_rect(0, 0, w, h, 0);
    uint32_t y0 = (h > 50) ? h - 37 : 0; // 37 is not a multiple of 32
    uint32_t hh = h - y0;
    uint32_t magic2 = pixfmt64_pack(&fb_fmt, 0xABCDEF);
    display64_fill_rect(0, y0, w, hh, 0xABCDEF);
    ok = display64_read_pixel_raw(0, y0) == magic2
      && display64_read_pixel_raw(w - 1, h - 1) == magic2
      && (y0 == 0 || display64_read_pixel_raw(0, y0 - 1) != magic2);
    st_check(&pass, &fail, "non-CHAR_H-aligned partial rect reaches true bottom edge", ok);

    // copy_rows: two distinct bands, copy one over the other.
    display64_fill_rect(0, 0, w, h, 0);
    display64_fill_rect(0, 0, w, 10, 0x111111);
    display64_fill_rect(0, 10, w, 10, 0x222222);
    display64_copy_rows(0, 10, 10);
    uint32_t magic3 = pixfmt64_pack(&fb_fmt, 0x222222);
    ok = display64_read_pixel_raw(0, 0) == magic3 && display64_read_pixel_raw(w - 1, 9) == magic3;
    st_check(&pass, &fail, "copy_rows", ok);

    // Clipping: a rect that overshoots the edge must clip, not crash,
    // and still correctly paint the in-bounds portion.
    display64_fill_rect(0, 0, w, h, 0);
    uint32_t magic4 = pixfmt64_pack(&fb_fmt, 0x333333);
    display64_fill_rect(w - 5, h - 5, 100, 100, 0x333333);
    ok = display64_read_pixel_raw(w - 1, h - 1) == magic4;
    st_check(&pass, &fail, "fill_rect clips an out-of-bounds rect safely", ok);

    display64_fill_rect(0, 0, w, h, 0); // leave a clean screen behind

    klog("display64_selftest: pass="); klog_hex("", (uint32_t)pass);
    klog("display64_selftest: fail="); klog_hex("", (uint32_t)fail);
    return fail == 0;
}

void display64_dump(void)
{
    klog("display64: dump ---\n");
    if (!fb_avail) { klog("  unavailable\n"); klog("display64: dump end ---\n"); return; }
    klog_hex("  width:  ", fb_width);
    klog_hex("  height: ", fb_height);
    klog_hex("  pitch:  ", fb_pitch);
    klog_hex("  bpp:    ", fb_fmt.bpp);
    klog_hex("  red_pos/size:   ", ((uint32_t)fb_fmt.red_pos << 8)   | fb_fmt.red_size);
    klog_hex("  green_pos/size: ", ((uint32_t)fb_fmt.green_pos << 8) | fb_fmt.green_size);
    klog_hex("  blue_pos/size:  ", ((uint32_t)fb_fmt.blue_pos << 8)  | fb_fmt.blue_size);
    klog_hex("  owner pid:      ", owner_pid);
    klog("display64: dump end ---\n");
}
