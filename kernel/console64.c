// kernel/console64.c — Milestone 21: console abstraction layer.
// Dispatches write/clear/set_color to either the framebuffer terminal
// (fbterm64) or the VGA text-mode terminal (vgaterm64) depending on
// which backend initialized successfully.  All kernel output paths
// (kernel_main64 diagnostics + syscall64's sys64_write/clear/set_color)
// go through this layer, making the display backend a single-point choice.
//
// M-next follow-up: this dispatch used to call fbterm64_* unconditionally
// whenever fbterm64_available() -- true for the whole life of any
// framebuffer boot, graphical or not. Milestone 30/32 keeps shell64
// running as a live sibling process even in graphical mode (see
// user64/init64.c's own comment on why), and shell64's sys_write() calls
// (its prompt, "command not found", etc.) come straight through here --
// so every one of them was drawn directly onto the SAME physical
// framebuffer the compositor presents into, with nothing checking who
// currently owns it. This was invisible before M-next only because the
// old compositor unconditionally redrew the ENTIRE screen on almost
// every event (any pointer move included), so stray fbterm64 text got
// painted over within one frame; M-next's damage-tracking only redraws
// what's actually damaged, so text drawn outside any tracked damage
// rect now persists indefinitely -- exposing, not causing, this bug.
// Fixed at the single point every kernel-side console write already
// funnels through: once display64 has an owner (a compositor has
// sys_display_open()'d it), the framebuffer terminal stops rendering
// entirely. fbterm64_available() is left untouched (it still reflects
// whether the hardware/format supports a framebuffer terminal at all),
// so the instant display64_release() happens (compositor exit/crash),
// this same dispatch resumes drawing exactly as before -- a fallback
// console with no separate suspend/resume state machine to maintain.
#include <stdint.h>
#include "../include/console64.h"
#include "../include/fbterm64.h"
#include "../include/vgaterm64.h"
#include "../include/display64.h"

static inline int fbterm64_owned_by_compositor(void) {
    return display64_owner_pid() != 0;
}

void console64_init(uint64_t fb_addr, uint32_t width, uint32_t height,
                    uint32_t pitch, const pixfmt64_t* fmt)
{
    // Try framebuffer first; fbterm64_init is a no-op (leaves fb_avail=0)
    // if the format is unsupported, dimensions are tiny, or the physical
    // mapping fails.
    fbterm64_init(fb_addr, width, height, pitch, fmt);
    // VGA text mode requires no explicit init — hardware default is valid.
}

void console64_write(const char* buf, uint64_t len)
{
    if (fbterm64_owned_by_compositor()) return; // see header comment
    if (fbterm64_available()) fbterm64_write(buf, len);
    else                      vgaterm64_write(buf, len);
}

void console64_clear(void)
{
    if (fbterm64_owned_by_compositor()) return; // see header comment
    if (fbterm64_available()) fbterm64_clear();
    else                      vgaterm64_clear();
}

void console64_set_color(uint8_t attr)
{
    if (fbterm64_owned_by_compositor()) return; // see header comment
    if (fbterm64_available()) fbterm64_set_color(attr);
    else                      vgaterm64_set_color(attr);
}
