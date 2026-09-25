/* math/cmplx, ported from Go's.
 *
 * Every line follows the Go code, over the struct rather than the built in
 * type. Where Go writes x*x or x+y on complex128 values this calls cmplx_mul
 * or cmplx_add, which do what Go's compiler does, and complex(a, b) is
 * cmplx_make.
 *
 * Copyright 2010 The Go Authors. All rights reserved.
 * Copyright 2026 The burrow Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style licence that can be found
 * in the LICENSE file. */

#include "burrow/math/cmplx.h"

#include "burrow/math.h"
#include "burrow/math/bits.h"
#include "burrow/panic.h"

/* No fused multiply and add, which would round once where Go rounds twice. */
#if defined(__clang__)
#pragma STDC FP_CONTRACT OFF
#elif defined(__GNUC__)
#pragma GCC push_options
#pragma GCC optimize("fp-contract=off")
#elif defined(_MSC_VER)
#pragma fp_contract(off)
#endif

static Complex128 cmplx_make(double re, double im) {
    Complex128 r = {re, im};
    return r;
}

/* ---------------------------------------------------------------- operators */

Complex128 cmplx_mul(Complex128 x, Complex128 y) {
    return cmplx_make(x.re * y.re - x.im * y.im, x.re * y.im + x.im * y.re);
}

/* Go's runtime.inf2one: 1 with the sign of f if f is an infinity, else 0 with
 * the sign of f. */
static double cmplx_inf2one(double f) {
    double g = 0.0;
    if (math_is_inf(f, 0))
        g = 1.0;
    return math_copysign(g, f);
}

static bool cmplx_finite(double f) {
    return !math_is_nan(f) && !math_is_inf(f, 0);
}

/* Go's runtime.complex128div. */
Complex128 cmplx_div(Complex128 n, Complex128 m) {
    double e, f; /* complex(e, f) = n/m */

    /* Algorithm for robust complex division as described in Robert L. Smith:
     * Algorithm 116: Complex division. Commun. ACM 5(8): 435 (1962). */
    if (math_abs(m.re) >= math_abs(m.im)) {
        double ratio = m.im / m.re;
        double denom = m.re + ratio * m.im;
        e = (n.re + n.im * ratio) / denom;
        f = (n.im - n.re * ratio) / denom;
    } else {
        double ratio = m.re / m.im;
        double denom = m.im + ratio * m.re;
        e = (n.re * ratio + n.im) / denom;
        f = (n.im * ratio - n.re) / denom;
    }

    if (math_is_nan(e) && math_is_nan(f)) {
        /* Correct final result to infinities and zeros if applicable.
         * Matches C99: ISO/IEC 9899:1999 - G.5.1 Multiplicative operators. */
        double a = n.re, b = n.im;
        double c = m.re, d = m.im;
        double inf = math_inf(1);
        if (c == 0 && d == 0 && (!math_is_nan(a) || !math_is_nan(b))) {
            e = math_copysign(inf, c) * a;
            f = math_copysign(inf, c) * b;
        } else if ((math_is_inf(a, 0) || math_is_inf(b, 0)) && cmplx_finite(c) &&
                   cmplx_finite(d)) {
            a = cmplx_inf2one(a);
            b = cmplx_inf2one(b);
            e = inf * (a * c + b * d);
            f = inf * (b * c - a * d);
        } else if ((math_is_inf(c, 0) || math_is_inf(d, 0)) && cmplx_finite(a) &&
                   cmplx_finite(b)) {
            c = cmplx_inf2one(c);
            d = cmplx_inf2one(d);
            e = 0 * (a * c + b * d);
            f = 0 * (b * c - a * d);
        }
    }
    return cmplx_make(e, f);
}

/* ----------------------------------------------------------- the small ones */

double cmplx_abs(Complex128 x) {
    return math_hypot(x.re, x.im);
}

double cmplx_phase(Complex128 x) {
    return math_atan2(x.im, x.re);
}

double cmplx_polar(Complex128 x, double *theta) {
    if (theta)
        *theta = cmplx_phase(x);
    return cmplx_abs(x);
}

Complex128 cmplx_rect(double r, double theta) {
    double c;
    double s = math_sincos(theta, &c);
    return cmplx_make(r * c, r * s);
}

Complex128 cmplx_conj(Complex128 x) {
    return cmplx_make(x.re, -x.im);
}

bool cmplx_is_inf(Complex128 x) {
    return math_is_inf(x.re, 0) || math_is_inf(x.im, 0);
}

bool cmplx_is_nan(Complex128 x) {
    if (math_is_inf(x.re, 0) || math_is_inf(x.im, 0))
        return false;
    return math_is_nan(x.re) || math_is_nan(x.im);
}

Complex128 cmplx_inf(void) {
    double inf = math_inf(1);
    return cmplx_make(inf, inf);
}

Complex128 cmplx_nan(void) {
    double nan = math_nan();
    return cmplx_make(nan, nan);
}

/* ---------------------------------------------------------------- sqrt, pow */

/* The complex square root, which Go took from the Cephes Math Library,
 * Release 2.8, June 2000, copyright 1984, 1987, 1989, 1992 and 2000 by Stephen
 * L. Moshier, whose readme says it may be used freely. The code is the
 * simplified version of Cephes' csqrt that Go carries. */
Complex128 cmplx_sqrt(Complex128 x) {
    if (x.im == 0) {
        /* Ensure that imag(r) has the same sign as imag(x) for imag(x) ==
         * signed zero. */
        if (x.re == 0)
            return cmplx_make(0, x.im);
        if (x.re < 0)
            return cmplx_make(0, math_copysign(math_sqrt(-x.re), x.im));
        return cmplx_make(math_sqrt(x.re), x.im);
    }
    if (math_is_inf(x.im, 0))
        return cmplx_make(math_inf(1), x.im);
    if (x.re == 0) {
        if (x.im < 0) {
            double r = math_sqrt(-0.5 * x.im);
            return cmplx_make(r, -r);
        }
        double r = math_sqrt(0.5 * x.im);
        return cmplx_make(r, r);
    }
    double a = x.re;
    double b = x.im;
    double scale;
    /* Rescale to avoid internal overflow or underflow. */
    if (math_abs(a) > 4 || math_abs(b) > 4) {
        a *= 0.25;
        b *= 0.25;
        scale = 2;
    } else {
        a *= 1.8014398509481984e16; /* 2**54 */
        b *= 1.8014398509481984e16;
        scale = 7.450580596923828125e-9; /* 2**-27 */
    }
    double r = math_hypot(a, b);
    double t;
    if (a > 0) {
        t = math_sqrt(0.5 * r + 0.5 * a);
        r = scale * math_abs((0.5 * b) / t);
        t *= scale;
    } else {
        r = math_sqrt(0.5 * r - 0.5 * a);
        t = scale * math_abs((0.5 * b) / r);
        r *= scale;
    }
    if (b < 0)
        return cmplx_make(t, -r);
    return cmplx_make(t, r);
}

Complex128 cmplx_pow(Complex128 x, Complex128 y) {
    if (x.re == 0 && x.im == 0) { /* Guaranteed also true for x == -0. */
        if (cmplx_is_nan(y))
            return cmplx_nan();
        double r = y.re, i = y.im;
        if (r == 0)
            return cmplx_make(1, 0);
        if (r < 0) {
            if (i == 0)
                return cmplx_make(math_inf(1), 0);
            return cmplx_inf();
        }
        if (r > 0)
            return cmplx_make(0, 0);
        /* A y with a NaN real part and an infinite imaginary one is not NaN
         * to cmplx_is_nan, and Go panics here for it too. */
        panic_str(BURROW_S("not reached"));
    }
    double modulus = cmplx_abs(x);
    if (modulus == 0)
        return cmplx_make(0, 0);
    double r = math_pow(modulus, y.re);
    double arg = cmplx_phase(x);
    double theta = y.re * arg;
    if (y.im != 0) {
        r *= math_exp(-y.im * arg);
        theta += y.im * math_log(modulus);
    }
    double c;
    double s = math_sincos(theta, &c);
    return cmplx_make(r * c, r * s);
}

/* ----------------------------------------------------------- exp and logs */

Complex128 cmplx_exp(Complex128 x) {
    double re = x.re, im = x.im;
    if (math_is_inf(re, 0)) {
        if (re > 0 && im == 0)
            return x;
        if (math_is_inf(im, 0) || math_is_nan(im)) {
            if (re < 0)
                return cmplx_make(0, math_copysign(0, im));
            return cmplx_make(math_inf(1), math_nan());
        }
    } else if (math_is_nan(re)) {
        if (im == 0)
            return cmplx_make(math_nan(), im);
    }
    double r = math_exp(x.re);
    double c;
    double s = math_sincos(x.im, &c);
    return cmplx_make(r * c, r * s);
}

Complex128 cmplx_log(Complex128 x) {
    return cmplx_make(math_log(cmplx_abs(x)), cmplx_phase(x));
}

Complex128 cmplx_log10(Complex128 x) {
    Complex128 z = cmplx_log(x);
    return cmplx_make(MATH_LOG10_E * z.re, MATH_LOG10_E * z.im);
}

/* --------------------------------------------------- the inverse functions */

Complex128 cmplx_asin(Complex128 x) {
    double re = x.re, im = x.im;
    if (im == 0 && math_abs(re) <= 1)
        return cmplx_make(math_asin(re), im);
    if (re == 0 && math_abs(im) <= 1)
        return cmplx_make(re, math_asinh(im));
    if (math_is_nan(im)) {
        if (re == 0)
            return cmplx_make(re, math_nan());
        if (math_is_inf(re, 0))
            return cmplx_make(math_nan(), re);
        return cmplx_nan();
    }
    if (math_is_inf(im, 0)) {
        if (math_is_nan(re))
            return x;
        if (math_is_inf(re, 0))
            return cmplx_make(math_copysign(MATH_PI / 4, re), im);
        return cmplx_make(math_copysign(0, re), im);
    }
    if (math_is_inf(re, 0))
        return cmplx_make(math_copysign(MATH_PI / 2, re), math_copysign(re, im));
    Complex128 ct = cmplx_make(-x.im, x.re); /* i * x */
    Complex128 xx = cmplx_mul(x, x);
    Complex128 x1 = cmplx_make(1 - xx.re, -xx.im); /* 1 - x*x */
    Complex128 x2 = cmplx_sqrt(x1);                /* x2 = sqrt(1 - x*x) */
    Complex128 w = cmplx_log(cmplx_add(ct, x2));
    return cmplx_make(w.im, -w.re); /* -i * w */
}

Complex128 cmplx_asinh(Complex128 x) {
    double re = x.re, im = x.im;
    if (im == 0 && math_abs(re) <= 1)
        return cmplx_make(math_asinh(re), im);
    if (re == 0 && math_abs(im) <= 1)
        return cmplx_make(re, math_asin(im));
    if (math_is_inf(re, 0)) {
        if (math_is_inf(im, 0))
            return cmplx_make(re, math_copysign(MATH_PI / 4, im));
        if (math_is_nan(im))
            return x;
        return cmplx_make(re, math_copysign(0.0, im));
    }
    if (math_is_nan(re)) {
        if (im == 0)
            return x;
        if (math_is_inf(im, 0))
            return cmplx_make(im, re);
        return cmplx_nan();
    }
    if (math_is_inf(im, 0))
        return cmplx_make(math_copysign(im, re), math_copysign(MATH_PI / 2, im));
    Complex128 xx = cmplx_mul(x, x);
    Complex128 x1 = cmplx_make(1 + xx.re, xx.im);   /* 1 + x*x */
    return cmplx_log(cmplx_add(x, cmplx_sqrt(x1))); /* log(x + sqrt(1 + x*x)) */
}

Complex128 cmplx_acos(Complex128 x) {
    Complex128 w = cmplx_asin(x);
    return cmplx_make(MATH_PI / 2 - w.re, -w.im);
}

Complex128 cmplx_acosh(Complex128 x) {
    if (x.re == 0 && x.im == 0)
        return cmplx_make(0, math_copysign(MATH_PI / 2, x.im));
    Complex128 w = cmplx_acos(x);
    if (w.im <= 0)
        return cmplx_make(-w.im, w.re); /* i * w */
    return cmplx_make(w.im, -w.re);     /* -i * w */
}

static double cmplx_reduce_pi(double x);

Complex128 cmplx_atan(Complex128 x) {
    double re = x.re, im = x.im;
    if (im == 0)
        return cmplx_make(math_atan(re), im);
    if (re == 0 && math_abs(im) <= 1)
        return cmplx_make(re, math_atanh(im));
    if (math_is_inf(im, 0) || math_is_inf(re, 0)) {
        if (math_is_nan(re))
            return cmplx_make(math_nan(), math_copysign(0, im));
        return cmplx_make(math_copysign(MATH_PI / 2, re), math_copysign(0, im));
    }
    if (math_is_nan(re) || math_is_nan(im))
        return cmplx_nan();
    double x2 = x.re * x.re;
    double a = 1 - x2 - x.im * x.im;
    if (a == 0)
        return cmplx_nan();
    double t = 0.5 * math_atan2(2 * x.re, a);
    double w = cmplx_reduce_pi(t);

    t = x.im - 1;
    double b = x2 + t * t;
    if (b == 0)
        return cmplx_nan();
    t = x.im + 1;
    double c = (x2 + t * t) / b;
    return cmplx_make(w, 0.25 * math_log(c));
}

Complex128 cmplx_atanh(Complex128 x) {
    Complex128 z = cmplx_make(-x.im, x.re); /* z = i * x */
    z = cmplx_atan(z);
    return cmplx_make(z.im, -z.re); /* z = -i * z */
}

/* ------------------------------------------------------ sines and cosines */

/* Both the hyperbolic sine and cosine, with one exp between them for the
 * larger arguments. */
static double cmplx_sinhcosh(double x, double *ch) {
    if (math_abs(x) <= 0.5) {
        *ch = math_cosh(x);
        return math_sinh(x);
    }
    double e = math_exp(x);
    double ei = 0.5 / e;
    e *= 0.5;
    *ch = e + ei;
    return e - ei;
}

Complex128 cmplx_sin(Complex128 x) {
    double re = x.re, im = x.im;
    if (im == 0 && (math_is_inf(re, 0) || math_is_nan(re)))
        return cmplx_make(math_nan(), im);
    if (math_is_inf(im, 0)) {
        if (re == 0)
            return x;
        if (math_is_inf(re, 0) || math_is_nan(re))
            return cmplx_make(math_nan(), im);
    } else if (re == 0 && math_is_nan(im)) {
        return x;
    }
    double c, ch;
    double s = math_sincos(x.re, &c);
    double sh = cmplx_sinhcosh(x.im, &ch);
    return cmplx_make(s * ch, c * sh);
}

Complex128 cmplx_sinh(Complex128 x) {
    double re = x.re, im = x.im;
    if (re == 0 && (math_is_inf(im, 0) || math_is_nan(im)))
        return cmplx_make(re, math_nan());
    if (math_is_inf(re, 0)) {
        if (im == 0)
            return cmplx_make(re, im);
        if (math_is_inf(im, 0) || math_is_nan(im))
            return cmplx_make(re, math_nan());
    } else if (im == 0 && math_is_nan(re)) {
        return cmplx_make(math_nan(), im);
    }
    double c, ch;
    double s = math_sincos(x.im, &c);
    double sh = cmplx_sinhcosh(x.re, &ch);
    return cmplx_make(c * sh, s * ch);
}

Complex128 cmplx_cos(Complex128 x) {
    double re = x.re, im = x.im;
    if (im == 0 && (math_is_inf(re, 0) || math_is_nan(re)))
        return cmplx_make(math_nan(), -im * math_copysign(0, re));
    if (math_is_inf(im, 0)) {
        if (re == 0)
            return cmplx_make(math_inf(1), -re * math_copysign(0, im));
        if (math_is_inf(re, 0) || math_is_nan(re))
            return cmplx_make(math_inf(1), math_nan());
    } else if (re == 0 && math_is_nan(im)) {
        return cmplx_make(math_nan(), 0);
    }
    double c, ch;
    double s = math_sincos(x.re, &c);
    double sh = cmplx_sinhcosh(x.im, &ch);
    return cmplx_make(c * ch, -s * sh);
}

Complex128 cmplx_cosh(Complex128 x) {
    double re = x.re, im = x.im;
    if (re == 0 && (math_is_inf(im, 0) || math_is_nan(im)))
        return cmplx_make(math_nan(), re * math_copysign(0, im));
    if (math_is_inf(re, 0)) {
        if (im == 0)
            return cmplx_make(math_inf(1), im * math_copysign(0, re));
        if (math_is_inf(im, 0) || math_is_nan(im))
            return cmplx_make(math_inf(1), math_nan());
    } else if (im == 0 && math_is_nan(re)) {
        return cmplx_make(math_nan(), im);
    }
    double c, ch;
    double s = math_sincos(x.im, &c);
    double sh = cmplx_sinhcosh(x.re, &ch);
    return cmplx_make(c * ch, s * sh);
}

/* ----------------------------------------------------------------- tangents */

/* x reduced to [-Pi/2, Pi/2] with Cody and Waite's method for moderate x and
 * Payne and Hanek's above 2^30. */
static double cmplx_reduce_pi(double x) {
    /* reduceThreshold is the maximum value of x where the reduction using
     * Cody-Waite reduction still gives accurate results. This threshold is
     * set by t*PIn being representable as a double without error where t is
     * given by t = floor(x/Pi) and PIn are the leading partial terms of Pi.
     * Since the leading terms, PI1 and PI2 below, have 30 and 32 trailing
     * zero bits respectively, t should have less than 30 significant bits.
     * t < 1<<30 -> floor(x/Pi)+0.5 < 1<<30 -> x < (1<<30-1) * Pi - 0.5
     * So, conservatively we can take x < 1<<30. */
    const double reduce_threshold = 1073741824.0; /* 1 << 30 */
    if (math_abs(x) < reduce_threshold) {
        /* Use Cody-Waite reduction in three parts. */
        /* PI1, PI2 and PI3 comprise an extended precision value of PI such
         * that PI ~= PI1 + PI2 + PI3. The parts are chosen so that PI1 and
         * PI2 have an approximately equal number of trailing zero bits. This
         * ensures that t*PI1 and t*PI2 are exact for large integer values of
         * t. The full precision PI3 ensures the approximation of PI is
         * accurate to 102 bits to handle cancellation during subtraction. */
        const double pi1 = 3.141592502593994;      /* 0x400921fb40000000 */
        const double pi2 = 1.5099578831723193e-07; /* 0x3e84442d00000000 */
        const double pi3 = 1.0780605716316238e-14; /* 0x3d08469898cc5170 */
        double t = x / MATH_PI;
        t += 0.5;
        t = (double)(int64_t)t; /* int64(t) = the multiple */
        return ((x - t * pi1) - t * pi2) - t * pi3;
    }
    /* Must apply Payne-Hanek range reduction. */
    enum { MASK = 0x7FF, SHIFT = 64 - 11 - 1, BIAS = 1023 };
    const uint64_t frac_mask = (1ULL << SHIFT) - 1;
    /* Extract out the integer and exponent such that, x = ix * 2 ** exp. */
    uint64_t ix = math_float64bits(x);
    int exp = (int)(ix >> SHIFT & MASK) - BIAS - SHIFT;
    ix &= frac_mask;
    ix |= 1ULL << SHIFT;

    /* mPi is the binary digits of 1/Pi as a uint64 array, that is,
     * 1/Pi = Sum mPi[i]*2^(-64*i). 19 64-bit digits give 1216 bits of
     * precision to handle the largest possible float64 exponent. */
    static const uint64_t m_pi[] = {
        0x0000000000000000, 0x517cc1b727220a94, 0xfe13abe8fa9a6ee0, 0x6db14acc9e21c820,
        0xff28b1d5ef5de2b0, 0xdb92371d2126e970, 0x0324977504e8c90e, 0x7f0ef58e5894d39f,
        0x74411afa975da242, 0x74ce38135a2fbf20, 0x9cc8eb1cc1a99cfa, 0x4e422fc5defc941d,
        0x8ffc4bffef02cc07, 0xf79788c5ad05368f, 0xb69b3f6793e584db, 0xa7a31fb34f2ff516,
        0xba93dd63f5f2f8bd, 0x9e839cfbc5294975, 0x35fdafd88fc6ae84, 0x2b0198237e3db5d5,
    };
    /* Use the exponent to extract the 3 appropriate uint64 digits from mPi,
     * B ~ (z0, z1, z2), such that the product leading digit has the
     * exponent -64. Note, exp >= 50 since x >= reduceThreshold and
     * exp < 971 for maximum float64. A shift of 64 is 0 in Go and undefined
     * in C, so bitshift 0 takes the other branch. */
    unsigned digit = (unsigned)(exp + 64) / 64, bitshift = (unsigned)(exp + 64) % 64;
    uint64_t z0 = m_pi[digit], z1 = m_pi[digit + 1], z2 = m_pi[digit + 2];
    if (bitshift != 0) {
        z0 = (z0 << bitshift) | (m_pi[digit + 1] >> (64 - bitshift));
        z1 = (z1 << bitshift) | (m_pi[digit + 2] >> (64 - bitshift));
        z2 = (z2 << bitshift) | (m_pi[digit + 3] >> (64 - bitshift));
    }
    /* Multiply mantissa by the digits and extract the upper two digits
     * (hi, lo). */
    uint64_t unused, z1lo, carry;
    uint64_t z2hi = bits_mul64(z2, ix, &unused);
    uint64_t z1hi = bits_mul64(z1, ix, &z1lo);
    uint64_t z0lo = z0 * ix;
    uint64_t lo = bits_add64(z1lo, z2hi, 0, &carry);
    uint64_t hi = bits_add64(z0lo, z1hi, carry, NULL);
    /* Find the magnitude of the fraction. */
    unsigned lz = (unsigned)bits_leading_zeros64(hi);
    uint64_t e = (uint64_t)(BIAS - (lz + 1));
    /* Clear implicit mantissa bit and shift into place. */
    hi = (lz + 1 == 64 ? 0 : hi << (lz + 1)) |
         (lz + 1 == 64 ? lo : lo >> (64 - (lz + 1)));
    hi >>= 64 - SHIFT;
    /* Include the exponent and convert to a float. */
    hi |= e << SHIFT;
    x = math_float64frombits(hi);
    /* map to (-Pi/2, Pi/2] */
    if (x > 0.5)
        x--;
    return MATH_PI * x;
}

/* Taylor series expansion for cosh(2y) - cos(2x). */
static double cmplx_tan_series(Complex128 z) {
    const double machep = 1.0 / 9007199254740992.0; /* 1 / (1 << 53) */

    double x = math_abs(2 * z.re);
    double y = math_abs(2 * z.im);
    x = cmplx_reduce_pi(x);
    x = x * x;
    y = y * y;
    double x2 = 1.0;
    double y2 = 1.0;
    double f = 1.0;
    double rn = 0.0;
    double d = 0.0;
    for (;;) {
        rn++;
        f *= rn;
        rn++;
        f *= rn;
        x2 *= x;
        y2 *= y;
        double t = y2 + x2;
        t /= f;
        d += t;

        rn++;
        f *= rn;
        rn++;
        f *= rn;
        x2 *= x;
        y2 *= y;
        t = y2 - x2;
        t /= f;
        d += t;
        if (!(math_abs(t / d) > machep)) {
            /* Caution: Use ! and > instead of <= for correct behavior if
             * t/d is NaN. See issue 17577. */
            break;
        }
    }
    return d;
}

Complex128 cmplx_tan(Complex128 x) {
    double re = x.re, im = x.im;
    if (math_is_inf(im, 0)) {
        if (math_is_inf(re, 0) || math_is_nan(re))
            return cmplx_make(math_copysign(0, re), math_copysign(1, im));
        return cmplx_make(math_copysign(0, math_sin(2 * re)), math_copysign(1, im));
    }
    if (re == 0 && math_is_nan(im))
        return x;
    double d = math_cos(2 * x.re) + math_cosh(2 * x.im);
    if (math_abs(d) < 0.25)
        d = cmplx_tan_series(x);
    if (d == 0)
        return cmplx_inf();
    return cmplx_make(math_sin(2 * x.re) / d, math_sinh(2 * x.im) / d);
}

Complex128 cmplx_tanh(Complex128 x) {
    double re = x.re, im = x.im;
    if (math_is_inf(re, 0)) {
        if (math_is_inf(im, 0) || math_is_nan(im))
            return cmplx_make(math_copysign(1, re), math_copysign(0, im));
        return cmplx_make(math_copysign(1, re), math_copysign(0, math_sin(2 * im)));
    }
    if (im == 0 && math_is_nan(re))
        return x;
    double d = math_cosh(2 * x.re) + math_cos(2 * x.im);
    if (d == 0)
        return cmplx_inf();
    return cmplx_make(math_sinh(2 * x.re) / d, math_sin(2 * x.im) / d);
}

Complex128 cmplx_cot(Complex128 x) {
    double d = math_cosh(2 * x.im) - math_cos(2 * x.re);
    if (math_abs(d) < 0.25)
        d = cmplx_tan_series(x);
    if (d == 0)
        return cmplx_inf();
    return cmplx_make(math_sin(2 * x.re) / d, -math_sinh(2 * x.im) / d);
}

#if defined(__GNUC__) && !defined(__clang__)
#pragma GCC pop_options
#endif
