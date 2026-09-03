// ToxenOS/userlib/toxui/tox_math.c — see tox_math.h's header comment.
#include <stdint.h>
#include "tox_math.h"

// Direct inline asm rather than __builtin_floor/ceil/sqrt/fabs: this
// whole codebase builds at the default -O0 (see the Makefile -- no
// -O flag anywhere), and at -O0 GCC lowers those builtins to a real
// CALL to an external floor/ceil/sqrt/fabs symbol instead of the
// hardware instruction (verified empirically -- it only inlines them
// starting at -O1/-O2). Writing the instruction directly makes these
// four correct regardless of what optimization level ever compiles
// this file, rather than depending on a codegen heuristic.
double tox_floor(double x) { double r; __asm__ volatile ("roundsd $0x9, %1, %0" : "=x" (r) : "x" (x)); return r; }
double tox_ceil(double x)  { double r; __asm__ volatile ("roundsd $0xA, %1, %0" : "=x" (r) : "x" (x)); return r; }
double tox_sqrt(double x)  { double r; __asm__ volatile ("sqrtsd %1, %0" : "=x" (r) : "x" (x)); return r; }
double tox_fabs(double x)  { uint64_t mask = 0x7FFFFFFFFFFFFFFFULL; union { double d; uint64_t u; } c; c.d = x; c.u &= mask; return c.d; }

// x87 FCOS: available with no CR4/OSFXSR enablement (only CR0.EM=0,
// already true -- see kernel/process64.c's fpu64_init header comment).
// "t"/"u" are GCC's x87-stack-register constraints (st(0)/st(1)).
double tox_cos(double x) {
    double result;
    __asm__ volatile ("fcos" : "=t" (result) : "0" (x));
    return result;
}

// acos(x) = atan2(sqrt(1-x^2), x) via x87 FPATAN (computes
// atan(st(1)/st(0)) with full quadrant awareness, matching atan2's
// semantics despite the historical instruction name).
double tox_acos(double x) {
    double s = tox_sqrt(1.0 - x * x);
    double result;
    __asm__ volatile ("fpatan" : "=t" (result) : "0" (x), "u" (s) : "st(1)");
    return result;
}

// Plain-C exp/ln pair (range reduction + Taylor series) backing
// tox_pow -- deliberately NOT x87 FYL2X/F2XM1/FSCALE stack juggling,
// which is easy to get subtly wrong via inline asm constraints for a
// path this header comment already establishes is dead code for
// ToxUI's own purposes (only stb_truetype's unused SDF path reaches
// it). "Real" math, just not hand-optimized.
static double tox_exp(double x) {
    if (x == 0.0) return 1.0;
    const double ln2 = 0.6931471805599453;
    int n = (int)(x / ln2 + (x >= 0.0 ? 0.5 : -0.5));
    double r = x - (double)n * ln2;
    double term = 1.0, sum = 1.0;
    for (int i = 1; i <= 15; i++) { term *= r / (double)i; sum += term; }
    // Scale by 2^n via direct IEEE-754 exponent-field manipulation --
    // valid as long as the result stays within the normal double range,
    // true for every value stb_truetype's (unused) SDF path could ever
    // actually produce.
    union { double d; uint64_t u; } conv;
    conv.d = sum;
    conv.u = (uint64_t)((int64_t)conv.u + ((int64_t)n << 52));
    return conv.d;
}

static double tox_ln(double x) {
    if (x <= 0.0) return 0.0; // domain error -- unreachable in practice, see header comment
    union { double d; uint64_t u; } conv;
    conv.d = x;
    int e = (int)((conv.u >> 52) & 0x7FF) - 1023;
    conv.u = (conv.u & 0x000FFFFFFFFFFFFFULL) | 0x3FF0000000000000ULL; // mantissa -> [1,2)
    double m = conv.d;
    double t = (m - 1.0) / (m + 1.0);
    double t2 = t * t;
    double term = t, sum = t;
    for (int i = 1; i <= 12; i++) { term *= t2; sum += term / (double)(2 * i + 1); }
    return 2.0 * sum + (double)e * 0.6931471805599453;
}

double tox_pow(double x, double y) {
    if (x == 0.0) return (y == 0.0) ? 1.0 : 0.0;
    int neg = x < 0.0;
    double ax = neg ? -x : x;
    double result = tox_exp(y * tox_ln(ax));
    // Only meaningful for the odd-root-like calls stb_truetype's
    // (unused) SDF cubic solver actually makes (e.g. pow(-x, 1/3)) --
    // a fully general real pow() of a negative base to an arbitrary
    // real exponent is undefined anyway.
    return neg ? -result : result;
}

double tox_fmod(double x, double y) {
    if (y == 0.0) return 0.0;
    double q = x / y;
    double qi = (q < 0.0) ? tox_ceil(q) : tox_floor(q);
    return x - qi * y;
}
