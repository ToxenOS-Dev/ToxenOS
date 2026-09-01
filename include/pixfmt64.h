#ifndef PIXFMT64_H
#define PIXFMT64_H

#include <stdint.h>

// Milestone 29: describes the REAL hardware pixel layout reported by the
// Multiboot2 framebuffer tag's direct-RGB color info (red/green/blue
// field position + mask size), instead of the Milestone 21-28 assumption
// that every framebuffer is packed 0x00RRGGBB 32bpp merely because QEMU's
// default Bochs VBE mode happens to look that way.
//
// Every framebuffer CLIENT (kernel/fbterm64.c, kernel/display64.c's own
// callers, the userspace present syscall) works exclusively in a fixed
// LOGICAL color space -- 0x00RRGGBB, 8 bits per channel, same as the
// existing vga_rgb palette -- and never touches raw hardware pixel bytes
// directly. pixfmt64_pack() is the one place that logical color is
// converted into whatever the ACTUAL panel/emulated adapter expects,
// scaling each 8-bit channel down if the hardware field is narrower
// (e.g. RGB565's 5/6/5-bit channels).
typedef struct {
    uint8_t bpp;              // bits per pixel: 16, 24, or 32 (others rejected)
    uint8_t bytes_per_pixel;  // bpp/8, precomputed: 2, 3, or 4
    uint8_t red_pos,   red_size;
    uint8_t green_pos, green_size;
    uint8_t blue_pos,  blue_size;
} pixfmt64_t;

// Validates and builds a pixfmt64_t from Multiboot2's direct-RGB color
// info fields. Rejects (returns -1, *out left unmodified):
//   - bpp not in {16, 24, 32} (this milestone's supported depth set --
//     other legacy depths, e.g. 8bpp palettized, are out of scope and
//     must be rejected cleanly rather than mis-rendered),
//   - any channel's field_pos+field_size exceeding bpp,
//   - any two channels' bit ranges overlapping.
// Returns 0 with *out filled on success.
int pixfmt64_from_mb2(uint8_t bpp,
                       uint8_t red_pos,   uint8_t red_size,
                       uint8_t green_pos, uint8_t green_size,
                       uint8_t blue_pos,  uint8_t blue_size,
                       pixfmt64_t* out);

// Packs a logical 0x00RRGGBB color into `fmt`'s native pixel encoding,
// right-justified in the low `fmt->bpp` bits of the returned uint32_t
// (the caller writes only the low bytes_per_pixel bytes of it, e.g. for
// a 24bpp format only the low 3 bytes are meaningful).
uint32_t pixfmt64_pack(const pixfmt64_t* fmt, uint32_t rgb);

// Milestone 29: QEMU's Bochs VBE only ever exposes one fixed 32bpp
// channel layout (red_pos=16/green_pos=8/blue_pos=0 -- "XRGB8888"), so
// no amount of GRUB gfxmode tweaking can produce a genuinely different
// RGB/BGR ordering or a 24bpp mode to boot-test against real hardware.
// This exercises pixfmt64_from_mb2/pixfmt64_pack directly against
// synthetic formats instead: XRGB8888, a swapped-order "BGRX8888", a
// 16bpp RGB565, and a 24bpp RGB-in-memory layout, plus
// pixfmt64_from_mb2's validation (rejects bad bpp, overlapping/
// out-of-range fields). Logs each case and a final tally via klog().
// Returns 1 if every case passed.
int pixfmt64_selftest(void);

#endif // PIXFMT64_H
