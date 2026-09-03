// ToxenOS/userlib/toxui/tox_text.h — Milestone 33: UTF-8 decoding and
// anti-aliased text drawing/measurement on top of tox_font.h's glyph
// cache + tox_draw.h's tox_blend_pixel.
//
// UTF-8 support level: ASCII always works correctly (it's a strict
// subset of UTF-8). Multi-byte sequences are decoded into real Unicode
// codepoints (see tox_utf8_decode) and handed to stb_truetype's
// codepoint-based glyph lookup as-is -- whether a given codepoint
// actually RENDERS as something meaningful depends entirely on
// whether the loaded .ttf contains a glyph for it (an unmapped
// codepoint draws as a legitimate empty glyph, not an error -- see
// tox_font_get_glyph's own contract). Explicitly NOT implemented:
// full Unicode text shaping, bidirectional text, combining-character
// composition, or complex-script (Arabic/Indic/etc.) shaping -- each
// codepoint is drawn as one independent glyph, left-to-right, advance-
// then-kern. Documented here as later work, per the Milestone 33 scope.
#ifndef TOX_TEXT_H
#define TOX_TEXT_H
#include <stdint.h>
#include "tox_surface.h"
#include "tox_font.h"
#include "tox_draw.h"

// Decodes ONE UTF-8 codepoint starting at s[0] (which must not be the
// string's terminating NUL -- callers loop `while (*s) { ... }`).
// Malformed/truncated input (an invalid leading byte, a continuation
// byte that isn't 0x80-0xBF, or a sequence truncated by the string's
// own NUL terminator) decodes as U+FFFD (the Unicode replacement
// character) and consumes exactly 1 byte -- a defined, safe policy:
// this function never reads past `s`'s NUL terminator and never
// infinite-loops on garbage input, but does not attempt to recover
// UTF-8 sync after invalid input the way some decoders do.
// *bytes_consumed is set to how many bytes of `s` were consumed (1-4).
uint32_t tox_utf8_decode(const char* s, int* bytes_consumed);

// Draws a UTF-8 string with its FIRST line's baseline-left at (x,y)
// (y is the baseline row, matching standard text-rendering convention
// -- ascenders/most glyphs extend above y, descenders like 'g'/'y'
// extend below it). '\n' starts a new line (pen x resets to the
// original x, pen y advances by the font's own ascent-descent+
// line_gap at `pixel_size`). Consecutive glyph pairs are kerned via
// tox_font_get_kerning. Respects `dst`'s current clip rectangle and
// bounds exactly like every other tox_draw.h primitive (a glyph
// drawn partially or entirely off-surface clips safely). Returns the
// final pen x position after the last glyph on the last line (useful
// for measuring/chaining further drawing).
int tox_draw_text(tox_surface_t* dst, tox_font_t* font, int pixel_size,
                   int32_t x, int32_t y, const char* utf8, tox_color_t color);

typedef struct { int width, height; } tox_text_extent_t;

// Measures a SINGLE line of UTF-8 text (stops at the first '\n', if
// any -- a caller wanting multi-line measurement loops this per line
// itself, keeping this primitive simple) at `pixel_size` without
// drawing anything. width is the total kerned advance; height is
// ascent-descent (the font's own single-line text height at this
// pixel size, not a measurement of which pixels actually got ink).
void tox_measure_text(tox_font_t* font, int pixel_size, const char* utf8, tox_text_extent_t* out);

#endif // TOX_TEXT_H
