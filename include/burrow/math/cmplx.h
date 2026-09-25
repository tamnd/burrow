/* math/cmplx, the elementary functions over complex numbers.
 *
 * Every function takes and returns Complex128, the struct core.h defines with
 * re and im fields. It is a port of Go's package on top of burrow's math, so the
 * answers are Go's to the bit, special cases included: the table in C99's
 * annex G for infinities and NaNs is what Go's tests check, and what these do.
 *
 *     Complex128 z = {3, 4};
 *     double r = cmplx_abs(z);                      // 5
 *     Complex128 w = cmplx_sqrt((Complex128){-4, 0}); // 0+2i
 *
 * Polar returns two results in Go. Here the first comes back and the second,
 * the angle, goes to an out parameter that may be NULL.
 *
 * C has no operators for a struct, so the ones Go's language gives complex128
 * are functions here: cmplx_add, cmplx_sub, cmplx_mul, cmplx_div, cmplx_neg
 * and cmplx_eq. Multiplication is the textbook formula, as in Go, and
 * division is Go's runtime routine, which is Smith's algorithm with C99's
 * fixups for infinities and NaNs. Both give the bits Go gives.
 *
 * Copyright 2010 The Go Authors. All rights reserved.
 * Copyright 2026 The burrow Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style licence that can be found
 * in the LICENSE file. */

/* burrow:package math/cmplx */

#ifndef BURROW_MATH_CMPLX_H
#define BURROW_MATH_CMPLX_H

#include "burrow/core.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ---------------------------------------------------------------- operators */

static inline Complex128 cmplx_add(Complex128 x, Complex128 y) {
    Complex128 r = {x.re + y.re, x.im + y.im};
    return r;
}

static inline Complex128 cmplx_sub(Complex128 x, Complex128 y) {
    Complex128 r = {x.re - y.re, x.im - y.im};
    return r;
}

static inline Complex128 cmplx_neg(Complex128 x) {
    Complex128 r = {-x.re, -x.im};
    return r;
}

/* Go's ==, which is true for +0 against -0 and false for NaN against
 * anything. */
static inline bool cmplx_eq(Complex128 x, Complex128 y) {
    return x.re == y.re && x.im == y.im;
}

/* x*y, as (a*c - b*d) + (a*d + b*c)i with each product rounded on its own. */
Complex128 cmplx_mul(Complex128 x, Complex128 y);

/* n/m, which does not fail for a zero m: 1/0 is an infinity, as in Go. */
Complex128 cmplx_div(Complex128 n, Complex128 m);

/* ------------------------------------------------------------ the functions */

/* The magnitude of x, which is math_hypot of the two parts. */
double cmplx_abs(Complex128 x);

/* The argument of x, in [-Pi, Pi]. */
double cmplx_phase(Complex128 x);

/* The magnitude of x, with the phase in *theta. */
double cmplx_polar(Complex128 x, double *theta);

/* The complex number with magnitude r and phase theta. */
Complex128 cmplx_rect(double r, double theta);

/* The complex conjugate, x with the sign of its imaginary part flipped. */
Complex128 cmplx_conj(Complex128 x);

/* Whether either part is an infinity. */
bool cmplx_is_inf(Complex128 x);

/* Whether either part is NaN and neither is an infinity. */
bool cmplx_is_nan(Complex128 x);

/* A complex infinity and a complex NaN, with both parts set. */
Complex128 cmplx_inf(void);
Complex128 cmplx_nan(void);

Complex128 cmplx_sqrt(Complex128 x);

/* x to the y. cmplx_pow of 0 and anything is 1 for a zero y, 0 for a y whose
 * real part is positive and an infinity for one whose real part is
 * negative. */
Complex128 cmplx_pow(Complex128 x, Complex128 y);

Complex128 cmplx_exp(Complex128 x);

/* The natural logarithm, with the imaginary part in [-Pi, Pi]. */
Complex128 cmplx_log(Complex128 x);
Complex128 cmplx_log10(Complex128 x);

Complex128 cmplx_sin(Complex128 x);
Complex128 cmplx_cos(Complex128 x);
Complex128 cmplx_tan(Complex128 x);
Complex128 cmplx_cot(Complex128 x);

Complex128 cmplx_asin(Complex128 x);
Complex128 cmplx_acos(Complex128 x);
Complex128 cmplx_atan(Complex128 x);

Complex128 cmplx_sinh(Complex128 x);
Complex128 cmplx_cosh(Complex128 x);
Complex128 cmplx_tanh(Complex128 x);

Complex128 cmplx_asinh(Complex128 x);
Complex128 cmplx_acosh(Complex128 x);
Complex128 cmplx_atanh(Complex128 x);

#ifdef __cplusplus
}
#endif

#endif /* BURROW_MATH_CMPLX_H */
