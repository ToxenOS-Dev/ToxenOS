#ifndef DISPLAY64_H
#define DISPLAY64_H

#include <stdint.h>
#include "pixfmt64.h"

// Milestone 29: the low-level display/framebuffer layer beneath
// kernel/fbterm64.c. Owns the framebuffer's physical mapping, pitch, and
// pixel format; fbterm64 is now just ONE client of this layer (a text
// renderer), not the owner of framebuffer geometry/pixel-format logic --
// a future Milestone 30 compositor is another client, going through the
// present operation (kernel/syscall64.c's SYS64_DISPLAY_PRESENT) instead
// of these direct kernel-side calls.
//
// MMIO mapping: reuses kernel/physmem64.c's physmem64_map_mmio() (the
// same cache-disabled physical direct-map extension Milestone 28's
// AHCI/NVMe/VirtIO drivers use for their controller registers) instead
// of kernel/fbterm64.c's old ad hoc low-identity-map extension -- one
// physical-mapping mechanism for the whole kernel instead of two. Like
// every other physmem64_map_mmio caller, display64_init() MUST run
// before the first process64_spawn() (see physmem64.h's ordering
// comment); it already does, since console64_init() runs during early
// boot, long before any process exists.
//
// All coordinates/rectangles are clamped to the actual display_width/
// display_height internally -- callers get silently-clipped, not
// undefined, behavior for an out-of-range rectangle, which is what lets
// fbterm64_clear()/scrolling correctly cover the true framebuffer height
// even when it is not an exact multiple of the terminal's character
// cell height (the Milestone 21-28 "bottom strip" bug).

typedef struct {
    uint32_t width, height, pitch;
    pixfmt64_t format;
} display64_info_t;

// Validates `fmt` is already-checked (see pixfmt64_from_mb2) and maps
// the framebuffer physical range via physmem64_map_mmio(). Returns 0 on
// success, -1 on failure (bad geometry, or the MMIO mapping failed) --
// on failure display64_available() reports 0 and every other call here
// is a safe no-op, mirroring the old fbterm64_available() gating so
// console64 can fall back to vgaterm64 exactly as before.
int display64_init(uint64_t fb_addr, uint32_t width, uint32_t height,
                    uint32_t pitch, const pixfmt64_t* fmt);
int display64_available(void);
void display64_get_info(display64_info_t* out);

// Writes one pixel, converting the logical 0x00RRGGBB color to the
// real hardware layout via pixfmt64_pack(). No-op if (x,y) is out of
// bounds or the display isn't available.
void display64_put_pixel(uint32_t x, uint32_t y, uint32_t rgb);

// Fills an axis-aligned rectangle with one logical color, clipped to the
// actual display bounds -- the primitive fbterm64_clear()/scrolling use
// to correctly cover the full framebuffer height.
void display64_fill_rect(uint32_t x, uint32_t y, uint32_t w, uint32_t h, uint32_t rgb);

// Moves `num_rows` full scanlines from src_y to dst_y via a raw byte
// copy (no per-pixel pack/unpack -- the bytes are already in the
// correct hardware format, only their row position changes). Used by
// fbterm64's scroll. Overlapping ranges are handled correctly (copies
// in the safe direction for an upward scroll, dst_y < src_y).
void display64_copy_rows(uint32_t dst_y, uint32_t src_y, uint32_t num_rows);

// Writes one scanline of `w` logical 0x00RRGGBB pixels (packed here,
// hardware format written out), clipped to display bounds. Used by
// SYS64_DISPLAY_PRESENT to stream a validated userspace row directly
// into the framebuffer without an intermediate full-frame kernel copy.
void display64_blit_row(uint32_t x, uint32_t y, uint32_t w, const uint32_t* logical_pixels);

// Logs width/height/pitch/bpp and the parsed RGB field positions/sizes
// via klog(). Debug use only.
void display64_dump(void);

// Reads back the raw native (hardware-format) pixel value at (x,y), or
// 0 if out of bounds/unavailable -- the read-side counterpart to
// display64_put_pixel/blit_row's writes, used by display64_selftest()
// to verify what was actually written rather than merely that nothing
// crashed. Also useful in general as a display diagnostic.
uint32_t display64_read_pixel_raw(uint32_t x, uint32_t y);

// Milestone 29: exercises fill_rect/copy_rows/clipping against the
// REAL active display via direct pixel read-back, including a
// deliberately non-CHAR_H-aligned partial rectangle (the exact shape
// kernel/fbterm64.c's scroll/clear use to cover a framebuffer whose
// height isn't an exact multiple of the terminal's 32px cell height --
// see that file's header comment on the "bottom strip" bug this fixed).
// QEMU's fixed VBE mode table only ever offered this environment a
// 1024x768x32 framebuffer (an exact multiple of 32) no matter what
// GRUB gfxmode/gfxpayload was requested, so this test proves the
// PRIMITIVE handles an arbitrary, non-aligned rectangle correctly
// directly, rather than relying on booting a genuinely different
// resolution to exercise it end-to-end. Leaves the screen cleared to
// black when done. Returns 1 if every case passed (or if no display is
// available -- vacuously, there is nothing to test).
int display64_selftest(void);

// ── Milestone 29: single-owner present privilege ────────────────────
// The physical framebuffer is a privileged global resource -- no
// process maps its MMIO directly (see kernel/syscall64.c's
// SYS64_DISPLAY_PRESENT, which copies through a validated userspace
// pointer entirely inside the kernel). This is the access-control half
// of that: only one process may hold an open HANDLE64_DISPLAY handle
// at a time, exactly mirroring kernel/input64.c's single-consumer input
// policy and for the same reason -- ToxenOS has no credential system
// yet, so "first (and only) owner wins" stands in for real access
// control, while still shaping the ABI around the intended future
// model where one compositor process owns presentation.
int display64_acquire(void);
void display64_release(void);
uint32_t display64_owner_pid(void); // 0 if unowned

#endif // DISPLAY64_H
