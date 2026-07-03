#ifndef VGATERM64_H
#define VGATERM64_H

#include <stdint.h>

// Milestone 13: a real scrolling VGA text-mode console for ToxenOS64,
// separate from kernel/kernel64.c's own one-shot boot-diagnostic
// vga_puts()/out_line() (which never scrolls and is only ever used
// during the boot banner, before this module takes over). This is what
// makes userland sys_write output (shell64, shw, etc. -- see
// kernel/syscall64.c's sys64_write) visible in the QEMU graphical
// window, not just the serial/klog log.
//
// Tracks a cursor (row, col) across calls, interprets '\n'/'\r'/'\b'/
// '\t' the way a real terminal would (in particular '\b': move back one
// cell and blank it, matching user64/tox64.h's tox_readline's "\b \b"
// erase sequence), wraps at the right edge, and scrolls the whole
// screen up by one row once the cursor would run off the bottom. Also
// updates the real VGA hardware cursor (CRTC ports 0x3D4/0x3D5) so the
// blinking caret tracks where output/input actually is.
void vgaterm64_write(const char* buf, uint64_t len);

// Blanks the whole screen and resets the cursor to (0,0) -- used once,
// right before the normal (non-debug) boot path hands off to userland,
// so the user lands on a clean shell prompt instead of the boot
// diagnostics that were on screen up to that point.
void vgaterm64_clear(void);

// Milestone 15: sets the attribute byte (fg in low nibble, bg in high
// nibble -- the classic 4-bit VGA palette) used by every subsequent
// vgaterm64_write() call, mirroring 32-bit ToxenOS's stateful
// set_color()-then-print() shape rather than threading a color
// parameter through every write call site. Does not affect already-
// written cells, and is independent of vgaterm64_clear()'s own fixed
// blank-cell attribute.
void vgaterm64_set_color(uint8_t attr);

#endif // VGATERM64_H
