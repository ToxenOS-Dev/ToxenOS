#ifndef FBTERM64_H
#define FBTERM64_H

#include <stdint.h>
#include "pixfmt64.h"

// Milestone 21: framebuffer terminal backend for ToxenOS64.
// Renders text via a scaled 8x16 bitmap font (16x32 pixels per
// character at 2x scale) into the display64 layer beneath it. VGA
// 4-bit color attributes map to standard 32-bit RGB logical values.
//
// Milestone 29: fbterm64 is now just ONE CLIENT of kernel/display64.c
// (the general framebuffer layer) -- it owns no framebuffer geometry or
// pixel-format logic of its own anymore (that's display64_init/
// display64_get_info), only the text-grid/cursor/glyph-rendering state
// a terminal needs. This also fixed several previously-known bugs:
//   - the "bottom strip" bug (clear/scroll only ever covered whole
//     character rows, leaving height % CHAR_H pixels permanently
//     uncleared when the framebuffer height wasn't an exact multiple of
//     the 32px cell height) -- clear/scroll now size their fill against
//     the display's REAL height (display64_get_info), not fb_rows*CHAR_H;
//   - a visible, non-destructive software cursor (see fbterm64.c's
//     screen_buf[] -- the actual character last drawn at the cursor
//     cell is tracked so moving the cursor away redraws the true glyph
//     instead of leaving a blank or stale highlighted cell behind);
//   - backspace at column 0 now moves up to the end of the previous
//     line instead of being a no-op there;
//   - tab now actually blanks the cells it skips over instead of just
//     advancing the column index over stale pixels.
//
// Call fbterm64_available() to check whether init succeeded before
// routing any output through this backend — console64.c does this
// automatically and falls back to vgaterm64 if unavailable.

void fbterm64_init(uint64_t fb_addr, uint32_t width, uint32_t height,
                   uint32_t pitch, const pixfmt64_t* fmt);

void fbterm64_write(const char* buf, uint64_t len);
void fbterm64_clear(void);
void fbterm64_set_color(uint8_t attr);
int  fbterm64_available(void);

#endif // FBTERM64_H
