// ToxenOS/userlib/toxui/tox_math.h — Milestone 33: the tiny freestanding
// "libm" ToxUI needs, since ToxenOS has no libc/libm at all and
// userlib/toxui/third_party/stb_truetype.h's macro overrides
// (STBTT_sqrt/STBTT_ifloor/etc., see tox_stb_impl.c) need real
// double-precision math functions to bind to.
//
// tox_floor/tox_ceil/tox_sqrt/tox_fabs are hardware SSE4.1/SSE2
// instructions via GCC builtins (verified to compile to a single
// roundsd/sqrtsd/andpd instruction each with no external call --
// see the Milestone 33 summary) -- these back stb_truetype's CORE
// rasterizer path (glyph bitmap generation) and must be genuinely
// correct.
//
// tox_cos/tox_acos (x87 FCOS/FPATAN via inline asm -- x87 needs no
// CR4/OSFXSR enablement, only CR0.EM=0, already true since Milestone 1)
// and tox_pow/tox_fmod (plain C: range-reduction + Taylor series,
// avoiding fragile hand-written x87 stack juggling for functions that
// are, per an explicit source audit of stb_truetype.h, ONLY reachable
// from stbtt_GetGlyphSDF's cubic-solver helper -- the optional signed-
// distance-field feature, which ToxUI does not use anywhere. They must
// still COMPILE correctly (stb_truetype.h's SDF code is compiled
// unconditionally, just never called), so real, reasonable
// implementations are provided rather than stubs, but their precision
// was not a design priority the way the core-path functions' was.
#ifndef TOX_MATH_H
#define TOX_MATH_H

double tox_floor(double x);
double tox_ceil(double x);
double tox_sqrt(double x);
double tox_fabs(double x);
double tox_cos(double x);
double tox_acos(double x);
double tox_pow(double x, double y);
double tox_fmod(double x, double y);

#endif // TOX_MATH_H
