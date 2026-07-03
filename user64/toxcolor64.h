// ToxenOS/user64/toxcolor64.h — Milestone 15: named constants for the
// 16-color VGA palette (4-bit foreground, set via sys_set_color --
// see user64/tox64.h). Numeric values match 32-bit ToxenOS's palette
// exactly, but as a header of named constants rather than raw hex
// literals scattered through call sites -- 32-bit itself has no such
// header (it inlines 0x0C/0x09/etc. everywhere), so this is a small,
// deliberate readability improvement specific to the 64-bit side, not
// a port of an existing pattern.
//
// A color byte passed to sys_set_color is fg | (bg << 4); every
// constant below is a foreground-only value (background 0 = black),
// which is all this codebase uses today.
#ifndef TOXCOLOR64_H
#define TOXCOLOR64_H

#define TC64_BLACK         0x00
#define TC64_BLUE          0x01
#define TC64_GREEN         0x02
#define TC64_CYAN          0x03
#define TC64_RED           0x04
#define TC64_MAGENTA       0x05
#define TC64_ORANGE        0x06
#define TC64_LIGHT_GRAY    0x07
#define TC64_DARK_GRAY     0x08
#define TC64_BLUE_BRIGHT   0x09
#define TC64_GREEN_BRIGHT  0x0A
#define TC64_CYAN_BRIGHT   0x0B
#define TC64_RED_BRIGHT    0x0C
#define TC64_MAGENTA_BRIGHT 0x0D
#define TC64_YELLOW        0x0E
#define TC64_WHITE         0x0F

// The console's baseline color (kernel/vgaterm64.c's own default,
// kept in sync here so userland can always reset back to it by name).
#define TC64_DEFAULT TC64_LIGHT_GRAY

#endif // TOXCOLOR64_H
