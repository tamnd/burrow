/* math, the floating point functions and constants.
 *
 * This is Go's math package and not a wrapper around the C library's. Go
 * carries its own implementations, most of them from FDLIBM, and the answers
 * they give differ in the last bit from glibc's, musl's, Apple's and the
 * Microsoft runtime's, which also differ from each other. So every function
 * here is Go's code, ported, and gives the same bits Go does on every
 * platform. Nothing links against libm.
 *
 *     double h = math_hypot(3, 4);           // 5
 *     double r = math_round(-2.5);           // -3
 *     double y = math_pow(2, 10);            // 1024
 *
 * Where Go returns two results the first comes back and the second goes to an
 * out parameter at the end, which may be NULL:
 *
 *     Int exp;
 *     double frac = math_frexp(8, &exp);     // 0.5 and 4
 *     double c;
 *     double s = math_sincos(x, &c);
 *
 * Go's int is Int here, in math_ldexp, math_ilogb, math_jn and the others.
 *
 * The results are exact to Go only if the compiler does not fuse a multiply
 * and an add into one instruction, which rounds once where Go rounds twice.
 * The source turns that off for itself with the pragmas GCC, Clang and MSVC
 * understand, so this is only a concern with a compiler that has none of
 * them, or on 32 bit x86 without SSE2, where the x87 unit keeps more
 * precision than a double has.
 *
 * Copyright 2009 The Go Authors. All rights reserved.
 * Copyright 2026 The burrow Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style licence that can be found
 * in the LICENSE file. */

/* burrow:package math */

#ifndef BURROW_MATH_H
#define BURROW_MATH_H

#include "burrow/core.h"

#include <stdint.h>
#include <string.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ---------------------------------------------------------------- constants
 *
 * Go's are exact until they are used, and these are the double each one
 * becomes when it is. */

#define MATH_E 2.7182818284590451
#define MATH_PI 3.1415926535897931
#define MATH_PHI 1.6180339887498949
#define MATH_SQRT2 1.4142135623730951
#define MATH_SQRT_E 1.6487212707001282
#define MATH_SQRT_PI 1.7724538509055161
#define MATH_SQRT_PHI 1.272019649514069
#define MATH_LN2 0.69314718055994529
#define MATH_LOG2_E 1.4426950408889634
#define MATH_LN10 2.3025850929940459
#define MATH_LOG10_E 0.43429448190325182

/* The largest finite value of each float type, and the smallest above zero,
 * which is a subnormal. */
#define MATH_MAX_FLOAT32 3.40282346638528859811704183484516925440e+38
#define MATH_SMALLEST_NONZERO_FLOAT32 1.401298464324817070923729583289916131280e-45
#define MATH_MAX_FLOAT64 1.79769313486231570814527423731704356798070e+308
#define MATH_SMALLEST_NONZERO_FLOAT64 4.9406564584124654417656879286822137236505980e-324

/* The integer limits. MATH_MAX_INT and the others without a size follow Int
 * and Uint, so they are the 32 bit ones on a 32 bit machine, as in Go. */
#define MATH_MAX_INT8 INT8_MAX
#define MATH_MIN_INT8 INT8_MIN
#define MATH_MAX_INT16 INT16_MAX
#define MATH_MIN_INT16 INT16_MIN
#define MATH_MAX_INT32 INT32_MAX
#define MATH_MIN_INT32 INT32_MIN
#define MATH_MAX_INT64 INT64_MAX
#define MATH_MIN_INT64 INT64_MIN
#define MATH_MAX_UINT8 UINT8_MAX
#define MATH_MAX_UINT16 UINT16_MAX
#define MATH_MAX_UINT32 UINT32_MAX
#define MATH_MAX_UINT64 UINT64_MAX
#define MATH_MAX_INT INTPTR_MAX
#define MATH_MIN_INT INTPTR_MIN
#define MATH_MAX_UINT UINTPTR_MAX

/* --------------------------------------------------------------------- bits
 *
 * The IEEE 754 bits of a float and back. Go's unsafe casts, done with memcpy,
 * which every compiler turns into a register move. */

static inline uint64_t math_float64bits(double f) {
    uint64_t b;
    memcpy(&b, &f, sizeof b);
    return b;
}

static inline double math_float64frombits(uint64_t b) {
    double f;
    memcpy(&f, &b, sizeof f);
    return f;
}

static inline uint32_t math_float32bits(float f) {
    uint32_t b;
    memcpy(&b, &f, sizeof b);
    return b;
}

static inline float math_float32frombits(uint32_t b) {
    float f;
    memcpy(&f, &b, sizeof f);
    return f;
}

/* Positive infinity if sign >= 0, negative infinity if sign < 0. */
static inline double math_inf(Int sign) {
    return math_float64frombits(sign >= 0 ? 0x7FF0000000000000U : 0xFFF0000000000000U);
}

/* An IEEE 754 not-a-number, the same bits as Go's. */
static inline double math_nan(void) {
    return math_float64frombits(0x7FF8000000000001U);
}

static inline bool math_is_nan(double f) {
    return f != f;
}

/* Whether f is an infinity, of either sign when sign is 0, positive only when
 * sign > 0 and negative only when sign < 0. */
static inline bool math_is_inf(double f, Int sign) {
    return (sign >= 0 && f > MATH_MAX_FLOAT64) || (sign <= 0 && f < -MATH_MAX_FLOAT64);
}

/* Whether the sign bit is set, which is true for -0 and false for +0. */
static inline bool math_signbit(double x) {
    return (int64_t)math_float64bits(x) < 0;
}

static inline double math_abs(double x) {
    return math_float64frombits(math_float64bits(x) & ~(1ULL << 63));
}

/* f with the sign of sign. */
static inline double math_copysign(double f, double sign) {
    return math_float64frombits((math_float64bits(f) & ~(1ULL << 63)) |
                                (math_float64bits(sign) & (1ULL << 63)));
}

/* ------------------------------------------------------------------ rounding */

double math_floor(double x);
double math_ceil(double x);
double math_trunc(double x);

/* The nearest integer, half away from zero. */
double math_round(double x);

/* The nearest integer, half to even. */
double math_round_to_even(double x);

/* The integer part, with the fraction in *frac. Both have the sign of f. */
double math_modf(double f, double *frac);

/* ---------------------------------------------------------------- arithmetic */

/* The maximum of x-y and 0. */
double math_dim(double x, double y);

/* The larger and the smaller of two values. An infinity wins over NaN, as in
 * Go, and +0 is larger than -0. */
double math_max(double x, double y);
double math_min(double x, double y);

/* The remainder of x/y with the sign of x. */
double math_mod(double x, double y);

/* The IEEE 754 remainder, x - n*y with n the nearest integer to x/y. */
double math_remainder(double x, double y);

/* x*y + z with one rounding. */
double math_fma(double x, double y, double z);

double math_sqrt(double x);
double math_cbrt(double x);

/* sqrt(p*p + q*q), without the overflow or underflow the formula has. */
double math_hypot(double p, double q);

/* x to the y, with every special case Go lists. */
double math_pow(double x, double y);

/* 10 to the n. Zero below -323 and infinity above 308. */
double math_pow10(Int n);

/* --------------------------------------------------------- the float layout */

/* frac and exp with f == frac * 2^exp and frac in [0.5, 1). Zero, infinities
 * and NaN come back as they are with *exp set to 0. */
double math_frexp(double f, Int *exp);

/* frac * 2^exp, the inverse of math_frexp. */
double math_ldexp(double frac, Int exp);

/* The binary exponent of x, as a double. */
double math_logb(double x);

/* The binary exponent of x, as an Int. MATH_MIN_INT32 for 0 and
 * MATH_MAX_INT32 for an infinity or NaN. */
Int math_ilogb(double x);

/* The next double after x in the direction of y. */
double math_nextafter(double x, double y);
float math_nextafter32(float x, float y);

/* -------------------------------------------------------- exponents and logs */

double math_exp(double x);
double math_exp2(double x);

/* e^x - 1, accurate when x is near zero. */
double math_expm1(double x);

double math_log(double x);
double math_log10(double x);
double math_log2(double x);

/* log(1 + x), accurate when x is near zero. */
double math_log1p(double x);

/* ------------------------------------------------------------- trigonometry
 *
 * Radians. Arguments of 2^29 and above are reduced with Payne and Hanek's
 * method, so the answer is still right for very large x. */

double math_sin(double x);
double math_cos(double x);
double math_tan(double x);

/* The sine, with the cosine in *cos. */
double math_sincos(double x, double *cos);

double math_asin(double x);
double math_acos(double x);
double math_atan(double x);

/* The arc tangent of y/x, using the signs of both to find the quadrant. */
double math_atan2(double y, double x);

double math_sinh(double x);
double math_cosh(double x);
double math_tanh(double x);
double math_asinh(double x);
double math_acosh(double x);
double math_atanh(double x);

/* ------------------------------------------------------ special functions */

double math_erf(double x);
double math_erfc(double x);
double math_erfinv(double x);
double math_erfcinv(double x);

double math_gamma(double x);

/* The natural log of |Gamma(x)|, with the sign of Gamma(x), 1 or -1, in
 * *sign. */
double math_lgamma(double x, Int *sign);

/* Bessel functions of the first kind, of order 0, 1 and n. */
double math_j0(double x);
double math_j1(double x);
double math_jn(Int n, double x);

/* Bessel functions of the second kind, of order 0, 1 and n. */
double math_y0(double x);
double math_y1(double x);
double math_yn(Int n, double x);

#ifdef __cplusplus
}
#endif

#endif /* BURROW_MATH_H */
