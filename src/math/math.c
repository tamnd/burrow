/* math, ported from Go's pure Go implementations.
 *
 * Go builds some of these from assembly on some machines. The assembly gives
 * the same answers as the Go code where it matters to a caller (Floor, Sqrt
 * and FMA are exact either way), so this file has only the Go code, and the
 * answers are the ones Go gives on a machine with no assembly for them.
 *
 * Two things differ from reading the Go code straight across.
 *
 * Go's constant expressions are exact, so 3*Pi/4 is 3π/4 rounded once, where
 * C would round Pi and then round again after each operation. Every constant
 * expression that is not exact in doubles is written here as the double Go
 * ends up with.
 *
 * A shift by 64 or more is 0 in Go and undefined in C, so the shifts that can
 * be that wide go through math_shl64 and math_shr64.
 *
 * Copyright 2009 The Go Authors. All rights reserved.
 * Copyright 2026 The burrow Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style licence that can be found
 * in the LICENSE file. */

#include "burrow/math.h"

#include "burrow/math/bits.h"

#if defined(__x86_64__) || defined(_M_X64)
#include <emmintrin.h>
#endif

/* No fused multiply and add, which would round once where Go rounds twice. */
#if defined(__clang__)
#pragma STDC FP_CONTRACT OFF
#elif defined(__GNUC__)
#pragma GCC push_options
#pragma GCC optimize("fp-contract=off")
#elif defined(_MSC_VER)
#pragma fp_contract(off)
#endif

#define MATH_UVNAN 0x7FF8000000000001U
#define MATH_UVINF 0x7FF0000000000000U
#define MATH_UVONE 0x3FF0000000000000U
#define MATH_MASK 0x7FF
#define MATH_SHIFT (64 - 11 - 1)
#define MATH_BIAS 1023
#define MATH_SIGN_MASK (1ULL << 63)
#define MATH_FRAC_MASK ((1ULL << MATH_SHIFT) - 1)

/* Constant expressions from the Go source, as the doubles Go gives them. */
#define MATH_4_OVER_PI 1.2732395447351628       /* 4/Pi */
#define MATH_3PI_OVER_4 2.3561944901923448      /* 3*Pi/4 */
#define MATH_1_OVER_SQRT_PI 0.56418958354775628 /* 1/SqrtPi */
#define MATH_2_OVER_PI 0.63661977236758138      /* 2/Pi */

static inline uint64_t math_shl64(uint64_t x, unsigned n) {
    return n >= 64 ? 0 : x << n;
}
static inline uint64_t math_shr64(uint64_t x, unsigned n) {
    return n >= 64 ? 0 : x >> n;
}

static inline double math_f64(uint64_t b) {
    return math_float64frombits(b);
}
static inline uint64_t math_b64(double f) {
    return math_float64bits(f);
}

/* x scaled up into the normal range if it is subnormal, with the exponent
 * that undoes it in *exp. */
static double math_normalize(double x, Int *exp) {
    const double smallest_normal = 2.2250738585072014e-308; /* 2**-1022 */
    if (math_abs(x) < smallest_normal) {
        *exp = -52;
        return x * (double)(1ULL << 52);
    }
    *exp = 0;
    return x;
}

/* ------------------------------------------------------------------ rounding */

double math_trunc(double x) {
    if (math_abs(x) < 1)
        return math_copysign(0, x);
    uint64_t b = math_b64(x);
    unsigned e = (unsigned)((b >> MATH_SHIFT) & MATH_MASK) - MATH_BIAS;
    if (e < 64 - 12)
        b &= ~((1ULL << (64 - 12 - e)) - 1);
    return math_f64(b);
}

double math_modf(double f, double *frac) {
    double i = math_trunc(f);
    if (frac != NULL)
        *frac = math_copysign(f - i, f);
    return i;
}

double math_floor(double x) {
    if (x == 0 || math_is_nan(x) || math_is_inf(x, 0))
        return x;
    if (x < 0) {
        double fract;
        double d = math_modf(-x, &fract);
        if (fract != 0.0)
            d = d + 1;
        return -d;
    }
    return math_modf(x, NULL);
}

double math_ceil(double x) {
    return -math_floor(-x);
}

double math_round(double x) {
    uint64_t bits = math_b64(x);
    unsigned e = (unsigned)(bits >> MATH_SHIFT) & MATH_MASK;
    if (e < MATH_BIAS) {
        bits &= MATH_SIGN_MASK; /* +-0 */
        if (e == MATH_BIAS - 1)
            bits |= MATH_UVONE; /* +-1 */
    } else if (e < MATH_BIAS + MATH_SHIFT) {
        const uint64_t half = 1ULL << (MATH_SHIFT - 1);
        e -= MATH_BIAS;
        bits += half >> e;
        bits &= ~(MATH_FRAC_MASK >> e);
    }
    return math_f64(bits);
}

double math_round_to_even(double x) {
    uint64_t bits = math_b64(x);
    unsigned e = (unsigned)(bits >> MATH_SHIFT) & MATH_MASK;
    if (e >= MATH_BIAS) {
        const uint64_t half_minus_ulp = (1ULL << (MATH_SHIFT - 1)) - 1;
        e -= MATH_BIAS;
        bits += math_shr64(half_minus_ulp + (math_shr64(bits, MATH_SHIFT - e) & 1), e);
        bits &= ~math_shr64(MATH_FRAC_MASK, e);
    } else if (e == MATH_BIAS - 1 && (bits & MATH_FRAC_MASK) != 0) {
        bits = (bits & MATH_SIGN_MASK) | MATH_UVONE; /* +-1 */
    } else {
        bits &= MATH_SIGN_MASK; /* +-0 */
    }
    return math_f64(bits);
}

/* ---------------------------------------------------------------- arithmetic */

double math_dim(double x, double y) {
    double v = x - y;
    if (v <= 0)
        v = 0;
    /* The MSVC Release build gave back -0 for Dim(-0, 0) with a plain return.
     * Adding +0 turns -0 into +0 and leaves every other result alone. */
    return v + 0.0;
}

double math_max(double x, double y) {
    if (math_is_inf(x, 1) || math_is_inf(y, 1))
        return math_inf(1);
    if (math_is_nan(x) || math_is_nan(y))
        return math_nan();
    if (x == 0 && x == y) {
        if (math_signbit(x))
            return y;
        return x;
    }
    return x > y ? x : y;
}

double math_min(double x, double y) {
    if (math_is_inf(x, -1) || math_is_inf(y, -1))
        return math_inf(-1);
    if (math_is_nan(x) || math_is_nan(y))
        return math_nan();
    if (x == 0 && x == y) {
        if (math_signbit(x))
            return x;
        return y;
    }
    return x < y ? x : y;
}

double math_mod(double x, double y) {
    if (y == 0 || math_is_inf(x, 0) || math_is_nan(x) || math_is_nan(y))
        return math_nan();
    y = math_abs(y);
    Int yexp;
    double yfr = math_frexp(y, &yexp);
    double r = x;
    if (x < 0)
        r = -x;
    while (r >= y) {
        Int rexp;
        double rfr = math_frexp(r, &rexp);
        if (rfr < yfr)
            rexp = rexp - 1;
        r = r - math_ldexp(y, rexp - yexp);
    }
    if (x < 0)
        r = -r;
    return r;
}

double math_remainder(double x, double y) {
    const double tiny = 4.45014771701440276618e-308; /* 0x0020000000000000 */
    const double half_max = MATH_MAX_FLOAT64 / 2;
    if (math_is_nan(x) || math_is_nan(y) || math_is_inf(x, 0) || y == 0)
        return math_nan();
    if (math_is_inf(y, 0))
        return x;
    bool sign = false;
    if (x < 0) {
        x = -x;
        sign = true;
    }
    if (y < 0)
        y = -y;
    if (x == y) {
        if (sign)
            return -0.0;
        return 0;
    }
    if (y <= half_max)
        x = math_mod(x, y + y); /* now x < 2y */
    if (y < tiny) {
        if (x + x > y) {
            x -= y;
            if (x + x >= y)
                x -= y;
        }
    } else {
        double y_half = 0.5 * y;
        if (x > y_half) {
            x -= y;
            if (x >= y_half)
                x -= y;
        }
    }
    if (sign)
        x = -x;
    return x;
}

/* Go's software square root, correctly rounded, for machines with no
 * instruction for it. */
static double math_sqrt_soft(double x) {
    if (x == 0 || math_is_nan(x) || math_is_inf(x, 1))
        return x;
    if (x < 0)
        return math_nan();
    uint64_t ix = math_b64(x);
    Int exp = (Int)((ix >> MATH_SHIFT) & MATH_MASK);
    if (exp == 0) { /* subnormal x */
        while ((ix & (1ULL << MATH_SHIFT)) == 0) {
            ix <<= 1;
            exp--;
        }
        exp++;
    }
    exp -= MATH_BIAS; /* unbias exponent */
    ix &= ~((uint64_t)MATH_MASK << MATH_SHIFT);
    ix |= 1ULL << MATH_SHIFT;
    if ((exp & 1) == 1) /* odd exp, double x to make it even */
        ix <<= 1;
    exp >>= 1; /* exp = exp/2, exponent of square root */
    ix <<= 1;
    uint64_t q = 0, s = 0;
    uint64_t r = 1ULL << (MATH_SHIFT + 1); /* moving bit from MSB to LSB */
    while (r != 0) {
        uint64_t t = s + r;
        if (t <= ix) {
            s = t + r;
            ix -= t;
            q += r;
        }
        ix <<= 1;
        r >>= 1;
    }
    if (ix != 0)    /* remainder, result not exact */
        q += q & 1; /* round according to extra bit */
    ix = (q >> 1) + ((uint64_t)(exp - 1 + MATH_BIAS) << MATH_SHIFT);
    return math_f64(ix);
}

/* The instruction, where there is one. It is correctly rounded, which is what
 * Go's code is too, so the answer is the same and only faster. A negative x
 * goes to the Go code so that the NaN is Go's. */
double math_sqrt(double x) {
#if defined(__x86_64__) || defined(_M_X64)
    if (x >= 0)
        return _mm_cvtsd_f64(_mm_sqrt_sd(_mm_setzero_pd(), _mm_set_sd(x)));
#elif defined(__aarch64__) && (defined(__GNUC__) || defined(__clang__))
    if (x >= 0) {
        double r;
        __asm__("fsqrt %d0, %d1" : "=w"(r) : "w"(x));
        return r;
    }
#endif
    return math_sqrt_soft(x);
}

/* ---------------------------------------------------------------------- FMA */

/* Every arm64 has a fused multiply and add, and the software version is only
 * built where it is needed. */
#if defined(__aarch64__) && (defined(__GNUC__) || defined(__clang__))
#define MATH_HW_FMA 1
#else
#define MATH_HW_FMA 0
#endif

#if !MATH_HW_FMA
static inline uint64_t math_fma_zero(uint64_t x) {
    return x == 0 ? 1 : 0;
}
static inline uint64_t math_fma_nonzero(uint64_t x) {
    return x != 0 ? 1 : 0;
}

static inline void math_fma_shl(uint64_t u1, uint64_t u2, unsigned n, uint64_t *r1,
                                uint64_t *r2) {
    *r1 = math_shl64(u1, n) | math_shr64(u2, 64 - n) | math_shl64(u2, n - 64);
    *r2 = math_shl64(u2, n);
}

static inline void math_fma_shr(uint64_t u1, uint64_t u2, unsigned n, uint64_t *r1,
                                uint64_t *r2) {
    *r2 = math_shr64(u2, n) | math_shl64(u1, 64 - n) | math_shr64(u1, n - 64);
    *r1 = math_shr64(u1, n);
}

/* A right shift of the 128 bit number u1:u2 by n that keeps a sticky bit for
 * anything shifted out. */
static void math_fma_shrcompress(uint64_t u1, uint64_t u2, unsigned n, uint64_t *r1,
                                 uint64_t *r2) {
    if (n == 0) {
        *r1 = u1;
        *r2 = u2;
    } else if (n == 64) {
        *r1 = 0;
        *r2 = u1 | math_fma_nonzero(u2);
    } else if (n >= 128) {
        *r1 = 0;
        *r2 = math_fma_nonzero(u1 | u2);
    } else if (n < 64) {
        math_fma_shr(u1, u2, n, r1, r2);
        *r2 |= math_fma_nonzero(u2 & ((1ULL << n) - 1));
    } else {
        math_fma_shr(u1, u2, n, r1, r2);
        *r2 |= math_fma_nonzero((u1 & ((1ULL << (n - 64)) - 1)) | u2);
    }
}

static int32_t math_fma_lz(uint64_t u1, uint64_t u2) {
    int32_t l = (int32_t)bits_leading_zeros64(u1);
    if (l == 64)
        l += (int32_t)bits_leading_zeros64(u2);
    return l;
}

static void math_fma_split(uint64_t b, uint32_t *sign, int32_t *exp,
                           uint64_t *mantissa) {
    *sign = (uint32_t)(b >> 63);
    *exp = (int32_t)(b >> 52) & MATH_MASK;
    *mantissa = b & MATH_FRAC_MASK;
    if (*exp == 0) {
        /* Normalize the value if it is subnormal. */
        unsigned shift = (unsigned)(bits_leading_zeros64(*mantissa) - 11);
        *mantissa <<= shift;
        *exp = 1 - (int32_t)shift;
    } else {
        *mantissa |= 1ULL << 52;
    }
}

static double math_fma_soft(double x, double y, double z) {
    uint64_t bx = math_b64(x), by = math_b64(y), bz = math_b64(z);

    /* Inf or NaN or zero involved. At most one rounding will occur. */
    if (x == 0.0 || y == 0.0 || (bx & MATH_UVINF) == MATH_UVINF ||
        (by & MATH_UVINF) == MATH_UVINF)
        return x * y + z;
    /* Handle non-finite z separately. Evaluating x*y+z where x and y are
     * finite, but z is infinite, should always result in z. */
    if (z == 0.0)
        return x * y;
    if ((bz & MATH_UVINF) == MATH_UVINF)
        return z;

    /* Inputs are (sub)normal. Split x, y, z into sign, exponent, mantissa. */
    uint32_t xs, ys, zs;
    int32_t xe, ye, ze;
    uint64_t xm, ym, zm;
    math_fma_split(bx, &xs, &xe, &xm);
    math_fma_split(by, &ys, &ye, &ym);
    math_fma_split(bz, &zs, &ze, &zm);

    /* Compute product p = x*y as sign, exponent, two-word mantissa. Start with
     * exponent. "is normal" bit isn't subtracted yet. */
    int32_t pe = xe + ye - MATH_BIAS + 1;

    /* pm1:pm2 is the double-word mantissa for the product p. Shift left to
     * leave top bit in product. Effectively shifts the 106-bit product to the
     * left by 21. */
    uint64_t pm2;
    uint64_t pm1 = bits_mul64(xm << 10, ym << 11, &pm2);
    uint64_t zm1 = zm << 10, zm2 = 0;
    uint32_t ps = xs ^ ys; /* product sign */

    /* math_normalize to 62nd bit */
    unsigned is62zero = (unsigned)((~pm1 >> 62) & 1);
    math_fma_shl(pm1, pm2, is62zero, &pm1, &pm2);
    pe -= (int32_t)is62zero;

    /* Swap addition operands so |p| >= |z| */
    if (pe < ze || (pe == ze && pm1 < zm1)) {
        uint32_t ts = ps;
        int32_t te = pe;
        uint64_t t1 = pm1, t2 = pm2;
        ps = zs;
        pe = ze;
        pm1 = zm1;
        pm2 = zm2;
        zs = ts;
        ze = te;
        zm1 = t1;
        zm2 = t2;
    }

    /* Special case: if p == -z the result is always +0 since neither operand
     * is zero. */
    if (ps != zs && pe == ze && pm1 == zm1 && pm2 == zm2)
        return 0;

    /* Align significands */
    math_fma_shrcompress(zm1, zm2, (unsigned)(pe - ze), &zm1, &zm2);

    /* Compute resulting significands, normalizing if necessary. */
    uint64_t m, c;
    if (ps == zs) {
        /* Adding (pm1:pm2) + (zm1:zm2) */
        pm2 = bits_add64(pm2, zm2, 0, &c);
        pm1 = bits_add64(pm1, zm1, c, NULL);
        pe -= (int32_t)(~pm1 >> 63);
        math_fma_shrcompress(pm1, pm2, (unsigned)(64 + (pm1 >> 63)), &pm1, &m);
    } else {
        /* Subtracting (pm1:pm2) - (zm1:zm2) */
        pm2 = bits_sub64(pm2, zm2, 0, &c);
        pm1 = bits_sub64(pm1, zm1, c, NULL);
        int32_t nz = math_fma_lz(pm1, pm2);
        pe -= nz;
        math_fma_shl(pm1, pm2, (unsigned)(nz - 1), &m, &pm2);
        m |= math_fma_nonzero(pm2);
    }

    /* Round and break ties to even */
    if (pe > 1022 + MATH_BIAS ||
        (pe == 1022 + MATH_BIAS && ((m + (1ULL << 9)) >> 63) == 1)) {
        /* rounded value overflows exponent range */
        return math_f64(((uint64_t)ps << 63) | MATH_UVINF);
    }
    if (pe < 0) {
        unsigned n = (unsigned)(-pe);
        m = math_shr64(m, n) | math_fma_nonzero(m & (math_shl64(1, n) - 1));
        pe = 0;
    }
    m = ((m + (1ULL << 9)) >> 10) &
        ~math_fma_zero((m & ((1ULL << 10) - 1)) ^ (1ULL << 9));
    pe &= -(int32_t)math_fma_nonzero(m);
    return math_f64(((uint64_t)ps << 63) + ((uint64_t)(uint32_t)pe << 52) + m);
}
#endif

double math_fma(double x, double y, double z) {
#if MATH_HW_FMA
    /* Every arm64 has the instruction, and a fused multiply and add rounds
     * once, so it gives the one right answer math_fma_soft works out. */
    double r;
    __asm__("fmadd %d0, %d1, %d2, %d3" : "=w"(r) : "w"(x), "w"(y), "w"(z));
    return r;
#else
    return math_fma_soft(x, y, z);
#endif
}

/* ---------------------------------------------------------- the float layout */

double math_frexp(double f, Int *exp) {
    Int e = 0;
    if (f == 0 || math_is_inf(f, 0) || math_is_nan(f)) {
        if (exp != NULL)
            *exp = 0;
        return f; /* correctly return -0 */
    }
    f = math_normalize(f, &e);
    uint64_t x = math_b64(f);
    e += (Int)((x >> MATH_SHIFT) & MATH_MASK) - MATH_BIAS + 1;
    x &= ~((uint64_t)MATH_MASK << MATH_SHIFT);
    x |= (uint64_t)(-1 + MATH_BIAS) << MATH_SHIFT;
    if (exp != NULL)
        *exp = e;
    return math_f64(x);
}

double math_ldexp(double frac, Int exp) {
    if (frac == 0)
        return frac; /* correctly return -0 */
    if (math_is_inf(frac, 0) || math_is_nan(frac))
        return frac;
    Int e;
    frac = math_normalize(frac, &e);
    /* Go's int arithmetic wraps, and exp can be anything, so this is done in
     * unsigned and brought back. */
    exp = (Int)((Uint)exp + (Uint)e);
    uint64_t x = math_b64(frac);
    exp = (Int)((Uint)exp + (Uint)((Int)(x >> MATH_SHIFT) & MATH_MASK) - MATH_BIAS);
    if (exp < -1075)
        return math_copysign(0, frac); /* underflow */
    if (exp > 1023) {                  /* overflow */
        if (frac < 0)
            return math_inf(-1);
        return math_inf(1);
    }
    double m = 1;
    if (exp < -1022) { /* denormal */
        exp += 53;
        m = 1.0 / (double)(1ULL << 53); /* 2**-53 */
    }
    x &= ~((uint64_t)MATH_MASK << MATH_SHIFT);
    x |= (uint64_t)(exp + MATH_BIAS) << MATH_SHIFT;
    return m * math_f64(x);
}

static Int math_ilogb_finite(double x) {
    Int exp;
    x = math_normalize(x, &exp);
    return (Int)((math_b64(x) >> MATH_SHIFT) & MATH_MASK) - MATH_BIAS + exp;
}

double math_logb(double x) {
    if (x == 0)
        return math_inf(-1);
    if (math_is_inf(x, 0))
        return math_inf(1);
    if (math_is_nan(x))
        return x;
    return (double)math_ilogb_finite(x);
}

Int math_ilogb(double x) {
    if (x == 0)
        return MATH_MIN_INT32;
    if (math_is_nan(x))
        return MATH_MAX_INT32;
    if (math_is_inf(x, 0))
        return MATH_MAX_INT32;
    return math_ilogb_finite(x);
}

double math_nextafter(double x, double y) {
    if (math_is_nan(x) || math_is_nan(y))
        return math_nan();
    if (x == y)
        return x;
    if (x == 0)
        return math_copysign(math_f64(1), y);
    if ((y > x) == (x > 0))
        return math_f64(math_b64(x) + 1);
    return math_f64(math_b64(x) - 1);
}

float math_nextafter32(float x, float y) {
    if (math_is_nan((double)x) || math_is_nan((double)y))
        return (float)math_nan();
    if (x == y)
        return x;
    if (x == 0)
        return (float)math_copysign((double)math_float32frombits(1), (double)y);
    if ((y > x) == (x > 0))
        return math_float32frombits(math_float32bits(x) + 1);
    return math_float32frombits(math_float32bits(x) - 1);
}

/* -------------------------------------------------------- exponents and logs */

/* e^r with r = hi - lo, times 2^k. */
static double math_expmulti(double hi, double lo, Int k) {
    const double P1 = 1.66666666666666657415e-01;  /* 0x3FC55555; 0x55555555 */
    const double P2 = -2.77777777770155933842e-03; /* 0xBF66C16C; 0x16BEBD93 */
    const double P3 = 6.61375632143793436117e-05;  /* 0x3F11566A; 0xAF25DE2C */
    const double P4 = -1.65339022054652515390e-06; /* 0xBEBBBD41; 0xC5D26BF1 */
    const double P5 = 4.13813679705723846039e-08;  /* 0x3E663769; 0x72BEA4D0 */

    double r = hi - lo;
    double t = r * r;
    double c = r - t * (P1 + t * (P2 + t * (P3 + t * (P4 + t * P5))));
    double y = 1 - ((lo - (r * c) / (2 - c)) - hi);
    return math_ldexp(y, k);
}

double math_exp(double x) {
    const double ln2_hi = 6.93147180369123816490e-01;
    const double ln2_lo = 1.90821492927058770002e-10;
    const double log2e = 1.44269504088896338700e+00;
    const double overflow = 7.09782712893383973096e+02;
    const double underflow = -7.45133219101941108420e+02;
    const double near_zero = 1.0 / (1 << 28); /* 2**-28 */

    if (math_is_nan(x))
        return x;
    if (x > overflow) /* handles case where x is +inf */
        return math_inf(1);
    if (x < underflow) /* handles case where x is -inf */
        return 0;
    if (-near_zero < x && x < near_zero)
        return 1 + x;

    /* reduce; computed as r = hi - lo for extra precision. */
    Int k = 0;
    if (x < 0)
        k = (Int)(log2e * x - 0.5);
    else if (x > 0)
        k = (Int)(log2e * x + 0.5);
    double hi = x - (double)k * ln2_hi;
    double lo = (double)k * ln2_lo;

    /* compute */
    return math_expmulti(hi, lo, k);
}

double math_exp2(double x) {
    const double ln2_hi = 6.93147180369123816490e-01;
    const double ln2_lo = 1.90821492927058770002e-10;
    const double overflow = 1.0239999999999999e+03;
    const double underflow = -1.0740e+03;

    if (math_is_nan(x))
        return x;
    if (x > overflow) /* handles case where x is +inf */
        return math_inf(1);
    if (x < underflow) /* handles case where x is -inf */
        return 0;

    /* argument reduction; x = r*lg(e) + k with |r| <= ln(2)/2. */
    Int k = 0;
    if (x > 0)
        k = (Int)(x + 0.5);
    else if (x < 0)
        k = (Int)(x - 0.5);
    double t = x - (double)k;
    double hi = t * ln2_hi;
    double lo = -t * ln2_lo;

    /* compute */
    return math_expmulti(hi, lo, k);
}

double math_expm1(double x) {
    const double othreshold = 7.09782712893383973096e+02; /* 0x40862E42FEFA39EF */
    const double ln2x56 = 3.88162421113569373274e+01;     /* 0x4043687a9f1af2b1 */
    const double ln2half_x3 = 1.03972077083991796413e+00; /* 0x3ff0a2b23f3bab73 */
    const double ln2half = 3.46573590279972654709e-01;    /* 0x3fd62e42fefa39ef */
    const double ln2_hi = 6.93147180369123816490e-01;     /* 0x3fe62e42fee00000 */
    const double ln2_lo = 1.90821492927058770002e-10;     /* 0x3dea39ef35793c76 */
    const double inv_ln2 = 1.44269504088896338700e+00;    /* 0x3ff71547652b82fe */
    const double tiny = 1.0 / (double)(1ULL << 54);       /* 2**-54 */
    /* scaled coefficients related to expm1 */
    const double Q1 = -3.33333333333331316428e-02; /* 0xBFA11111111110F4 */
    const double Q2 = 1.58730158725481460165e-03;  /* 0x3F5A01A019FE5585 */
    const double Q3 = -7.93650757867487942473e-05; /* 0xBF14CE199EAADBB7 */
    const double Q4 = 4.00821782732936239552e-06;  /* 0x3ED0CFCA86E65239 */
    const double Q5 = -2.01099218183624371326e-07; /* 0xBE8AFDB76E09C32D */

    /* special cases */
    if (math_is_inf(x, 1) || math_is_nan(x))
        return x;
    if (math_is_inf(x, -1))
        return -1;

    double absx = x;
    bool sign = false;
    if (x < 0) {
        absx = -absx;
        sign = true;
    }

    /* filter out huge argument */
    if (absx >= ln2x56) { /* if |x| >= 56 * ln2 */
        if (sign)
            return -1; /* x < -56*ln2, return -1 */
        if (absx >= othreshold)
            return math_inf(1); /* if |x| >= 709.78... */
    }

    /* argument reduction */
    double c = 0;
    Int k = 0;
    if (absx > ln2half) { /* if  |x| > 0.5 * ln2 */
        double hi, lo;
        if (absx < ln2half_x3) { /* and |x| < 1.5 * ln2 */
            if (!sign) {
                hi = x - ln2_hi;
                lo = ln2_lo;
                k = 1;
            } else {
                hi = x + ln2_hi;
                lo = -ln2_lo;
                k = -1;
            }
        } else {
            if (!sign)
                k = (Int)(inv_ln2 * x + 0.5);
            else
                k = (Int)(inv_ln2 * x - 0.5);
            double t = (double)k;
            hi = x - t * ln2_hi; /* t * ln2_hi is exact here */
            lo = t * ln2_lo;
        }
        x = hi - lo;
        c = (hi - x) - lo;
    } else if (absx < tiny) { /* when |x| < 2**-54, return x */
        return x;
    } else {
        k = 0;
    }

    /* x is now in primary range */
    double hfx = 0.5 * x;
    double hxs = x * hfx;
    double r1 = 1 + hxs * (Q1 + hxs * (Q2 + hxs * (Q3 + hxs * (Q4 + hxs * Q5))));
    double t = 3 - r1 * hfx;
    double e = hxs * ((r1 - t) / (6.0 - x * t));
    if (k == 0)
        return x - (x * e - hxs); /* c is 0 */
    e = (x * (e - c) - c);
    e -= hxs;
    if (k == -1)
        return 0.5 * (x - e) - 0.5;
    if (k == 1) {
        if (x < -0.25)
            return -2 * (e - (x + 0.5));
        return 1 + 2 * (x - e);
    }
    if (k <= -2 || k > 56) { /* suffice to return exp(x)-1 */
        double y = 1 - (e - x);
        y = math_f64(math_b64(y) + ((uint64_t)k << 52)); /* add k to y's exponent */
        return y - 1;
    }
    if (k < 20) {
        double tk = math_f64(0x3ff0000000000000U -
                             (0x20000000000000U >> (unsigned)k)); /* 1-2**-k */
        double y = tk - (e - x);
        y = math_f64(math_b64(y) + ((uint64_t)k << 52)); /* add k to y's exponent */
        return y;
    }
    t = math_f64((uint64_t)(0x3ff - k) << 52); /* 2**-k */
    double y = x - (e + t);
    y++;
    y = math_f64(math_b64(y) + ((uint64_t)k << 52)); /* add k to y's exponent */
    return y;
}

double math_log(double x) {
    const double ln2_hi = 6.93147180369123816490e-01; /* 3fe62e42 fee00000 */
    const double ln2_lo = 1.90821492927058770002e-10; /* 3dea39ef 35793c76 */
    const double L1 = 6.666666666666735130e-01;       /* 3FE55555 55555593 */
    const double L2 = 3.999999999940941908e-01;       /* 3FD99999 9997FA04 */
    const double L3 = 2.857142874366239149e-01;       /* 3FD24924 94229359 */
    const double L4 = 2.222219843214978396e-01;       /* 3FCC71C5 1D8E78AF */
    const double L5 = 1.818357216161805012e-01;       /* 3FC74664 96CB03DE */
    const double L6 = 1.531383769920937332e-01;       /* 3FC39A09 D078C69F */
    const double L7 = 1.479819860511658591e-01;       /* 3FC2F112 DF3E5244 */

    /* special cases */
    if (math_is_nan(x) || math_is_inf(x, 1))
        return x;
    if (x < 0)
        return math_nan();
    if (x == 0)
        return math_inf(-1);

    /* reduce */
    Int ki;
    double f1 = math_frexp(x, &ki);
    if (f1 < MATH_SQRT2 / 2) {
        f1 *= 2;
        ki--;
    }
    double f = f1 - 1;
    double k = (double)ki;

    /* compute */
    double s = f / (2 + f);
    double s2 = s * s;
    double s4 = s2 * s2;
    double t1 = s2 * (L1 + s4 * (L3 + s4 * (L5 + s4 * L7)));
    double t2 = s4 * (L2 + s4 * (L4 + s4 * L6));
    double R = t1 + t2;
    double hfsq = 0.5 * f * f;
    return k * ln2_hi - ((hfsq - (s * (hfsq + R) + k * ln2_lo)) - f);
}

double math_log10(double x) {
    return math_log(x) * MATH_LOG10_E;
}

double math_log2(double x) {
    Int exp;
    double frac = math_frexp(x, &exp);
    /* Make sure exact powers of two give an exact answer. Don't depend on
     * Log(0.5)*(1/Ln2)+exp being exactly exp-1. */
    if (frac == 0.5)
        return (double)(exp - 1);
    return math_log(frac) * MATH_LOG2_E + (double)exp;
}

double math_log1p(double x) {
    const double sqrt2m1 = 4.142135623730950488017e-01;       /* Sqrt(2)-1 */
    const double sqrt2half_m1 = -2.928932188134524755992e-01; /* Sqrt(2)/2-1 */
    const double small = 1.0 / (1 << 29);                     /* 2**-29 */
    const double tiny = 1.0 / (double)(1ULL << 54);           /* 2**-54 */
    const double two53 = (double)(1ULL << 53);                /* 2**53 */
    const double ln2_hi = 6.93147180369123816490e-01;         /* 3fe62e42fee00000 */
    const double ln2_lo = 1.90821492927058770002e-10;         /* 3dea39ef35793c76 */
    const double Lp1 = 6.666666666666735130e-01;              /* 3FE5555555555593 */
    const double Lp2 = 3.999999999940941908e-01;              /* 3FD999999997FA04 */
    const double Lp3 = 2.857142874366239149e-01;              /* 3FD2492494229359 */
    const double Lp4 = 2.222219843214978396e-01;              /* 3FCC71C51D8E78AF */
    const double Lp5 = 1.818357216161805012e-01;              /* 3FC7466496CB03DE */
    const double Lp6 = 1.531383769920937332e-01;              /* 3FC39A09D078C69F */
    const double Lp7 = 1.479819860511658591e-01;              /* 3FC2F112DF3E5244 */

    /* special cases */
    if (x < -1 || math_is_nan(x)) /* includes -Inf */
        return math_nan();
    if (x == -1)
        return math_inf(-1);
    if (math_is_inf(x, 1))
        return math_inf(1);

    double absx = math_abs(x);

    double f = 0;
    uint64_t iu = 0;
    Int k = 1;
    if (absx < sqrt2m1) {    /*  |x| < Sqrt(2)-1 */
        if (absx < small) {  /* |x| < 2**-29 */
            if (absx < tiny) /* |x| < 2**-54 */
                return x;
            return x - x * x * 0.5;
        }
        if (x > sqrt2half_m1) { /* Sqrt(2)/2-1 < x */
            /* (Sqrt(2)/2-1) < x < (Sqrt(2)-1) */
            k = 0;
            f = x;
            iu = 1;
        }
    }
    double c = 0;
    if (k != 0) {
        double u;
        if (absx < two53) { /* 1<<53 */
            u = 1.0 + x;
            iu = math_b64(u);
            k = (Int)((iu >> 52) - 1023);
            /* correction term */
            if (k > 0)
                c = 1.0 - (u - x);
            else
                c = x - (u - 1.0);
            c /= u;
        } else {
            u = x;
            iu = math_b64(u);
            k = (Int)((iu >> 52) - 1023);
            c = 0;
        }
        iu &= 0x000fffffffffffffU;
        if (iu < 0x0006a09e667f3bcdU) {             /* mantissa of Sqrt(2) */
            u = math_f64(iu | 0x3ff0000000000000U); /* math_normalize u */
        } else {
            k++;
            u = math_f64(iu | 0x3fe0000000000000U); /* math_normalize u/2 */
            iu = (0x0010000000000000U - iu) >> 2;
        }
        f = u - 1.0; /* Sqrt(2)/2 < u < Sqrt(2) */
    }
    double hfsq = 0.5 * f * f;
    double s, R, z;
    if (iu == 0) { /* |f| < 2**-20 */
        if (f == 0) {
            if (k == 0)
                return 0;
            c += (double)k * ln2_lo;
            return (double)k * ln2_hi + c;
        }
        R = hfsq * (1.0 - 0.66666666666666666 * f); /* avoid division */
        if (k == 0)
            return f - R;
        return (double)k * ln2_hi - ((R - ((double)k * ln2_lo + c)) - f);
    }
    s = f / (2.0 + f);
    z = s * s;
    R = z * (Lp1 + z * (Lp2 + z * (Lp3 + z * (Lp4 + z * (Lp5 + z * (Lp6 + z * Lp7))))));
    if (k == 0)
        return f - (hfsq - s * (hfsq + R));
    return (double)k * ln2_hi -
           ((hfsq - (s * (hfsq + R) + ((double)k * ln2_lo + c))) - f);
}

/* x to the y. */
static bool math_is_odd_int(double x) {
    if (math_abs(x) >= (double)(1ULL << 53)) {
        /* 1 << 53 is the largest exact integer in the float64 format. Any
         * number outside this range will be truncated before the decimal
         * point and therefore will always be an even integer. */
        return false;
    }
    double xf;
    double xi = math_modf(x, &xf);
    return xf == 0 && ((int64_t)xi & 1) == 1;
}

double math_pow(double x, double y) {
    if (y == 0 || x == 1)
        return 1;
    if (y == 1)
        return x;
    if (math_is_nan(x) || math_is_nan(y))
        return math_nan();
    if (x == 0) {
        if (y < 0) {
            if (math_signbit(x) && math_is_odd_int(y))
                return math_inf(-1);
            return math_inf(1);
        }
        if (y > 0) {
            if (math_signbit(x) && math_is_odd_int(y))
                return x;
            return 0;
        }
    } else if (math_is_inf(y, 0)) {
        if (x == -1)
            return 1;
        if ((math_abs(x) < 1) == math_is_inf(y, 1))
            return 0;
        return math_inf(1);
    } else if (math_is_inf(x, 0)) {
        if (math_is_inf(x, -1))
            return math_pow(1 / x, -y); /* Pow(-0, -y) */
        if (y < 0)
            return 0;
        if (y > 0)
            return math_inf(1);
    } else if (y == 0.5) {
        return math_sqrt(x);
    } else if (y == -0.5) {
        return 1 / math_sqrt(x);
    }

    double yf;
    double yi = math_modf(math_abs(y), &yf);
    if (yf != 0 && x < 0)
        return math_nan();
    if (yi >= 9223372036854775808.0) { /* 1<<63 */
        /* yi is a large even int that will lead to overflow (or underflow to
         * 0) for all x except -1 (x == 1 was handled earlier) */
        if (x == -1)
            return 1;
        if ((math_abs(x) < 1) == (y > 0))
            return 0;
        return math_inf(1);
    }

    /* ans = a1 * 2**ae (= 1 for now). */
    double a1 = 1.0;
    Int ae = 0;

    /* ans *= x**yf */
    if (yf != 0) {
        if (yf > 0.5) {
            yf--;
            yi++;
        }
        a1 = math_exp(yf * math_log(x));
    }

    /* ans *= x**yi by repeated squaring and multiplying by the bits of yi. */
    Int xe;
    double x1 = math_frexp(x, &xe);
    for (int64_t i = (int64_t)yi; i != 0; i >>= 1) {
        if (xe < -(1 << 12) || (1 << 12) < xe) {
            /* catastrophic overflow - avoid it; ae is going to overflow
             * anyway, so the fact that xe << 1 might have overflowed doesn't
             * matter. */
            ae += xe;
            break;
        }
        if ((i & 1) == 1) {
            a1 *= x1;
            ae += xe;
        }
        x1 *= x1;
        xe *= 2;
        if (x1 < .5) {
            x1 += x1;
            xe--;
        }
    }

    /* ans = a1 * 2**ae; if y < 0 { ans = 1 / ans } but in the opposite order */
    if (y < 0) {
        a1 = 1 / a1;
        ae = -ae;
    }
    return math_ldexp(a1, ae);
}

static const double math_pow10tab[] = {
    1e00, 1e01, 1e02, 1e03, 1e04, 1e05, 1e06, 1e07, 1e08, 1e09, 1e10,
    1e11, 1e12, 1e13, 1e14, 1e15, 1e16, 1e17, 1e18, 1e19, 1e20, 1e21,
    1e22, 1e23, 1e24, 1e25, 1e26, 1e27, 1e28, 1e29, 1e30, 1e31,
};

static const double math_pow10postab32[] = {
    1e00, 1e32, 1e64, 1e96, 1e128, 1e160, 1e192, 1e224, 1e256, 1e288,
};

static const double math_pow10negtab32[] = {
    1e-00, 1e-32, 1e-64, 1e-96, 1e-128, 1e-160, 1e-192, 1e-224, 1e-256, 1e-288, 1e-320,
};

double math_pow10(Int n) {
    if (0 <= n && n <= 308)
        return math_pow10postab32[(Uint)n / 32] * math_pow10tab[(Uint)n % 32];
    if (-323 <= n && n < 0)
        return math_pow10negtab32[(Uint)-n / 32] / math_pow10tab[(Uint)-n % 32];
    if (n > 0)
        return math_inf(1);
    return 0;
}

double math_cbrt(double x) {
    const uint64_t B1 = 715094163;                /* (682-0.03306235651)*2**20 */
    const uint64_t B2 = 696219795;                /* (664-0.03306235651)*2**20 */
    const double C = 5.42857142857142815906e-01;  /* 19/35     = 0x3FE15F15F15F15F1 */
    const double D = -7.05306122448979611050e-01; /* -864/1225 = 0xBFE691DE2532C834 */
    const double E = 1.41428571428571436819e+00;  /* 99/70     = 0x3FF6A0EA0EA0EA0F */
    const double F = 1.60714285714285720630e+00;  /* 45/28     = 0x3FF9B6DB6DB6DB6E */
    const double G = 3.57142857142857150787e-01;  /* 5/14      = 0x3FD6DB6DB6DB6DB7 */
    const double smallest_normal = 2.22507385850720138309e-308; /* 2**-1022 */

    /* special cases */
    if (x == 0 || math_is_nan(x) || math_is_inf(x, 0))
        return x;

    bool sign = false;
    if (x < 0) {
        x = -x;
        sign = true;
    }

    /* rough cbrt to 5 bits */
    double t = math_f64(math_b64(x) / 3 + (B1 << 32));
    if (x < smallest_normal) {
        /* subnormal number */
        t = (double)(1ULL << 54); /* set t= 2**54 */
        t *= x;
        t = math_f64(math_b64(t) / 3 + (B2 << 32));
    }

    /* new cbrt to 23 bits */
    double r = t * t / x;
    double s = C + r * t;
    t *= G + F / (s + E + D / s);

    /* chop to 22 bits, make larger than cbrt(x) */
    t = math_f64((math_b64(t) & (0xFFFFFFFFCULL << 28)) + (1ULL << 30));

    /* one step newton iteration to 53 bits with error less than 0.667ulps */
    s = t * t; /* t*t is exact */
    r = x / s;
    double w = t + t;
    r = (r - t) / (w + r); /* r-s is exact */
    t = t + t * r;

    /* restore the sign bit */
    if (sign)
        t = -t;
    return t;
}

double math_hypot(double p, double q) {
    p = math_abs(p);
    q = math_abs(q);
    /* special cases */
    if (math_is_inf(p, 1) || math_is_inf(q, 1))
        return math_inf(1);
    if (math_is_nan(p) || math_is_nan(q))
        return math_nan();
    if (p < q) {
        double t = p;
        p = q;
        q = t;
    }
    if (p == 0)
        return 0;
    q = q / p;
    return p * math_sqrt(1 + q * q);
}

/* ------------------------------------------------------------- trigonometry */

/* The sine and cosine polynomials, from the Cephes library. */
static const double math_sin_coef[] = {
    1.58962301576546568060e-10, /* 0x3de5d8fd1fd19ccd */
    -2.50507477628578072866e-8, /* 0xbe5ae5e5a9291f5d */
    2.75573136213857245213e-6,  /* 0x3ec71de3567d48a1 */
    -1.98412698295895385996e-4, /* 0xbf2a01a019bfdf03 */
    8.33333333332211858878e-3,  /* 0x3f8111111110f7d0 */
    -1.66666666666666307295e-1, /* 0xbfc5555555555548 */
};

static const double math_cos_coef[] = {
    -1.13585365213876817300e-11, /* 0xbda8fa49a0861a9b */
    2.08757008419747316778e-9,   /* 0x3e21ee9d7b4e3f05 */
    -2.75573141792967388112e-7,  /* 0xbe927e4f7eac4bc6 */
    2.48015872888517045348e-5,   /* 0x3efa01a019c844f5 */
    -1.38888888888730564116e-3,  /* 0xbf56c16c16c14f91 */
    4.16666666666665929218e-2,   /* 0x3fa555555555554b */
};

/* Pi/4 split into three parts. */
#define PI4A 7.85398125648498535156e-1  /* 0x3fe921fb40000000 */
#define PI4B 3.77489470793079817668e-8  /* 0x3e64442d00000000 */
#define PI4C 2.69515142907905952645e-15 /* 0x3ce8469898cc5170 */

/* Above this, the extended precision reduction in the functions loses too
 * much, and math_trig_reduce does it with the bits of 4/Pi instead. */
#define REDUCE_THRESHOLD ((double)(1 << 29))

/* 4/Pi as a binary fraction, 1216 bits of it, for math_trig_reduce. */
static const uint64_t math_m_pi4[] = {
    0x0000000000000001U, 0x45f306dc9c882a53U, 0xf84eafa3ea69bb81U, 0xb6c52b3278872083U,
    0xfca2c757bd778ac3U, 0x6e48dc74849ba5c0U, 0x0c925dd413a32439U, 0xfc3bd63962534e7dU,
    0xd1046bea5d768909U, 0xd338e04d68befc82U, 0x7323ac7306a673e9U, 0x3908bf177bf25076U,
    0x3ff12fffbc0b301fU, 0xde5e2316b414da3eU, 0xda6cfd9e4f96136eU, 0x9e8c7ecd3cbfd45aU,
    0xea4f758fd7cbe2f6U, 0x7a0e73ef14a525d4U, 0xd7f6bf623f1aba10U, 0xac06608df8f6d757U,
};

/* Payne and Hanek's reduction of x to z in [0, Pi/4) and the octant j it was
 * in. For x of REDUCE_THRESHOLD and up. */
static double math_trig_reduce(double x, uint64_t *jp) {
    const double PI4 = MATH_PI / 4;
    if (x < PI4) {
        *jp = 0;
        return x;
    }
    /* Extract out the integer and exponent such that, x = ix * 2 ** exp. */
    uint64_t ix = math_b64(x);
    Int exp = (Int)((ix >> MATH_SHIFT) & MATH_MASK) - MATH_BIAS - MATH_SHIFT;
    ix &= ~((uint64_t)MATH_MASK << MATH_SHIFT);
    ix |= 1ULL << MATH_SHIFT;
    /* Use the exponent to extract the 3 appropriate uint64 digits from math_m_pi4,
     * B ~ (z0, z1, z2), such that the product leading digit has the
     * exponent -61. Note, exp >= -53 since x >= PI4 and exp < 971 for maximum
     * float64. */
    unsigned digit = (unsigned)(exp + 61) / 64, bitshift = (unsigned)(exp + 61) % 64;
    uint64_t z0 = math_shl64(math_m_pi4[digit], bitshift) |
                  math_shr64(math_m_pi4[digit + 1], 64 - bitshift);
    uint64_t z1 = math_shl64(math_m_pi4[digit + 1], bitshift) |
                  math_shr64(math_m_pi4[digit + 2], 64 - bitshift);
    uint64_t z2 = math_shl64(math_m_pi4[digit + 2], bitshift) |
                  math_shr64(math_m_pi4[digit + 3], 64 - bitshift);
    /* Multiply mantissa by the digits and extract the upper two digits (hi,
     * lo). */
    uint64_t z2hi = bits_mul64(z2, ix, NULL);
    uint64_t z1lo;
    uint64_t z1hi = bits_mul64(z1, ix, &z1lo);
    uint64_t z0lo = z0 * ix;
    uint64_t c;
    uint64_t lo = bits_add64(z1lo, z2hi, 0, &c);
    uint64_t hi = bits_add64(z0lo, z1hi, c, NULL);
    /* The top 3 bits of hi give j. */
    uint64_t j = hi >> 61;
    /* Extract the fraction and find its magnitude. */
    hi = hi << 3 | lo >> 61;
    unsigned lz = (unsigned)bits_leading_zeros64(hi);
    uint64_t e = (uint64_t)MATH_BIAS - (lz + 1);
    /* Clear implicit mantissa bit and shift into place. */
    hi = math_shl64(hi, lz + 1) | math_shr64(lo, 64 - (lz + 1));
    hi >>= 64 - MATH_SHIFT;
    /* Include the exponent and convert to a float. */
    hi |= e << MATH_SHIFT;
    double z = math_f64(hi);
    /* Map zeros to origin. */
    if ((j & 1) == 1) {
        j++;
        j &= 7;
        z--;
    }
    /* Multiply the fractional part by pi/4. */
    *jp = j;
    return z * PI4;
}

/* The reduction every trig function starts with, for 0 <= x. */
static double math_trig_octant(double x, uint64_t *jp) {
    if (x >= REDUCE_THRESHOLD)
        return math_trig_reduce(x, jp);
    /* integer part of x/(Pi/4), as integer for tests on the phase angle */
    uint64_t j = (uint64_t)(x * MATH_4_OVER_PI);
    double y = (double)j; /* integer part of x/(Pi/4), as float */
    /* map zeros to origin */
    if ((j & 1) == 1) {
        j++;
        y++;
    }
    *jp = j & 7; /* octant modulo 2Pi radians (360 degrees) */
    return ((x - y * PI4A) - y * PI4B) -
           y * PI4C; /* Extended precision modular arithmetic */
}

static inline double math_sin_poly(double z, double zz) {
    return z + z * zz *
                   ((((((math_sin_coef[0] * zz) + math_sin_coef[1]) * zz +
                       math_sin_coef[2]) *
                          zz +
                      math_sin_coef[3]) *
                         zz +
                     math_sin_coef[4]) *
                        zz +
                    math_sin_coef[5]);
}

static inline double math_cos_poly(double zz) {
    return 1.0 - 0.5 * zz +
           zz * zz *
               ((((((math_cos_coef[0] * zz) + math_cos_coef[1]) * zz +
                   math_cos_coef[2]) *
                      zz +
                  math_cos_coef[3]) *
                     zz +
                 math_cos_coef[4]) *
                    zz +
                math_cos_coef[5]);
}

double math_cos(double x) {
    /* special cases */
    if (math_is_nan(x) || math_is_inf(x, 0))
        return math_nan();

    /* make argument positive */
    bool sign = false;
    x = math_abs(x);

    uint64_t j;
    double z = math_trig_octant(x, &j);

    if (j > 3) {
        j -= 4;
        sign = !sign;
    }
    if (j > 1)
        sign = !sign;

    double zz = z * z;
    double y;
    if (j == 1 || j == 2)
        y = math_sin_poly(z, zz);
    else
        y = math_cos_poly(zz);
    if (sign)
        y = -y;
    return y;
}

double math_sin(double x) {
    /* special cases */
    if (x == 0 || math_is_nan(x))
        return x; /* return +-0 || NaN() */
    if (math_is_inf(x, 0))
        return math_nan();

    /* make argument positive but save the sign */
    bool sign = false;
    if (x < 0) {
        x = -x;
        sign = true;
    }

    uint64_t j;
    double z = math_trig_octant(x, &j);

    /* reflect in x axis */
    if (j > 3) {
        sign = !sign;
        j -= 4;
    }

    double zz = z * z;
    double y;
    if (j == 1 || j == 2)
        y = math_cos_poly(zz);
    else
        y = math_sin_poly(z, zz);
    if (sign)
        y = -y;
    return y;
}

double math_sincos(double x, double *cosp) {
    double sin, cos;
    /* special cases */
    if (x == 0) {
        sin = x; /* return +-0.0, 1.0 */
        cos = 1;
        goto out;
    }
    if (math_is_nan(x) || math_is_inf(x, 0)) {
        sin = math_nan();
        cos = math_nan();
        goto out;
    }

    /* make argument positive */
    bool sin_sign = false, cos_sign = false;
    if (x < 0) {
        x = -x;
        sin_sign = true;
    }

    uint64_t j;
    double z = math_trig_octant(x, &j);
    if (j > 3) { /* reflect in x axis */
        j -= 4;
        sin_sign = !sin_sign;
        cos_sign = !cos_sign;
    }
    if (j > 1)
        cos_sign = !cos_sign;

    double zz = z * z;
    cos = math_cos_poly(zz);
    sin = math_sin_poly(z, zz);
    if (j == 1 || j == 2) {
        double t = sin;
        sin = cos;
        cos = t;
    }
    if (cos_sign)
        cos = -cos;
    if (sin_sign)
        sin = -sin;
out:
    if (cosp != NULL)
        *cosp = cos;
    return sin;
}

/* The tangent polynomials, from Cephes. */
static const double math_tan_p[] = {
    -1.30936939181383777646e4, /* 0xc0c992d8d24f3f38 */
    1.15351664838587416140e6,  /* 0x413199eca5fc9ddd */
    -1.79565251976484877988e7, /* 0xc1711fead3299176 */
};

static const double math_tan_q[] = {
    1.00000000000000000000e0,  1.36812963470692954678e4, /* 0x40cab8a5eeb36572 */
    -1.32089234440210967447e6,                           /* 0xc13427bc582abc96 */
    2.50083801823357915839e7,                            /* 0x4177d98fc2ead8ef */
    -5.38695755929454629881e7,                           /* 0xc189afe03cbe5a31 */
};

double math_tan(double x) {
    /* special cases */
    if (x == 0 || math_is_nan(x))
        return x; /* return +-0 || NaN() */
    if (math_is_inf(x, 0))
        return math_nan();

    /* make argument positive but save the sign */
    bool sign = false;
    if (x < 0) {
        x = -x;
        sign = true;
    }

    uint64_t j;
    double z;
    if (x >= REDUCE_THRESHOLD) {
        z = math_trig_reduce(x, &j);
    } else {
        j = (uint64_t)(x * MATH_4_OVER_PI);
        double y = (double)j;
        /* map zeros and singularities to origin */
        if ((j & 1) == 1) {
            j++;
            y++;
        }
        z = ((x - y * PI4A) - y * PI4B) - y * PI4C;
    }
    double zz = z * z;

    double y;
    if (zz > 1e-14)
        y = z +
            z * (zz * (((math_tan_p[0] * zz) + math_tan_p[1]) * zz + math_tan_p[2]) /
                 ((((zz + math_tan_q[1]) * zz + math_tan_q[2]) * zz + math_tan_q[3]) *
                      zz +
                  math_tan_q[4]));
    else
        y = z;
    if ((j & 2) == 2)
        y = -1 / y;
    if (sign)
        y = -y;
    return y;
}

/* The arc tangent of 0 <= x <= 0.66, from Cephes. */
static double math_xatan(double x) {
    const double P0 = -8.750608600031904122785e-01;
    const double P1 = -1.615753718733365076637e+01;
    const double P2 = -7.500855792314704667340e+01;
    const double P3 = -1.228866684490136173410e+02;
    const double P4 = -6.485021904942025371773e+01;
    const double Q0 = +2.485846490142306297962e+01;
    const double Q1 = +1.650270098316988542046e+02;
    const double Q2 = +4.328810604912902668951e+02;
    const double Q3 = +4.853903996359136964868e+02;
    const double Q4 = +1.945506571482613964425e+02;
    double z = x * x;
    z = z * ((((P0 * z + P1) * z + P2) * z + P3) * z + P4) /
        (((((z + Q0) * z + Q1) * z + Q2) * z + Q3) * z + Q4);
    z = x * z + x;
    return z;
}

/* The arc tangent of a positive x, reduced into the range math_xatan takes. */
static double math_satan(double x) {
    const double morebits = 6.123233995736765886130e-17; /* pi/2 = PIO2 + morebits */
    const double tan3pio8 = 2.41421356237309504880;      /* tan(3*pi/8) */
    if (x <= 0.66)
        return math_xatan(x);
    if (x > tan3pio8)
        return MATH_PI / 2 - math_xatan(1 / x) + morebits;
    return MATH_PI / 4 + math_xatan((x - 1) / (x + 1)) + 0.5 * morebits;
}

double math_atan(double x) {
    if (x == 0)
        return x;
    if (x > 0)
        return math_satan(x);
    return -math_satan(-x);
}

double math_asin(double x) {
    if (x == 0)
        return x; /* special case */
    bool sign = false;
    if (x < 0) {
        x = -x;
        sign = true;
    }
    if (x > 1)
        return math_nan(); /* special case */

    double temp = math_sqrt(1 - x * x);
    if (x > 0.7)
        temp = MATH_PI / 2 - math_satan(temp / x);
    else
        temp = math_satan(x / temp);

    if (sign)
        temp = -temp;
    return temp;
}

double math_acos(double x) {
    return MATH_PI / 2 - math_asin(x);
}

double math_atan2(double y, double x) {
    /* special cases */
    if (math_is_nan(y) || math_is_nan(x))
        return math_nan();
    if (y == 0) {
        if (x >= 0 && !math_signbit(x))
            return math_copysign(0, y);
        return math_copysign(MATH_PI, y);
    }
    if (x == 0)
        return math_copysign(MATH_PI / 2, y);
    if (math_is_inf(x, 0)) {
        if (math_is_inf(x, 1)) {
            if (math_is_inf(y, 0))
                return math_copysign(MATH_PI / 4, y);
            return math_copysign(0, y);
        }
        if (math_is_inf(y, 0))
            return math_copysign(MATH_3PI_OVER_4, y);
        return math_copysign(MATH_PI, y);
    }
    if (math_is_inf(y, 0))
        return math_copysign(MATH_PI / 2, y);

    /* Call atan and determine the quadrant. */
    double q = math_atan(y / x);
    if (x < 0) {
        if (q <= 0)
            return q + MATH_PI;
        return q - MATH_PI;
    }
    return q;
}

/* ------------------------------------------------------------- hyperbolic */

double math_sinh(double x) {
    /* The coefficients are #2029 from Hart & Cheney. (20.36D) */
    const double P0 = -0.6307673640497716991184787251e+6;
    const double P1 = -0.8991272022039509355398013511e+5;
    const double P2 = -0.2894211355989563807284660366e+4;
    const double P3 = -0.2630563213397497062819489e+2;
    const double Q0 = -0.6307673640497716991212077277e+6;
    const double Q1 = 0.1521517378790019070696485176e+5;
    const double Q2 = -0.173678953558233699533450911e+3;

    bool sign = false;
    if (x < 0) {
        x = -x;
        sign = true;
    }

    double temp;
    if (x > 21) {
        temp = math_exp(x) * 0.5;
    } else if (x > 0.5) {
        double ex = math_exp(x);
        temp = (ex - 1 / ex) * 0.5;
    } else {
        double sq = x * x;
        temp = (((P3 * sq + P2) * sq + P1) * sq + P0) * x;
        temp = temp / (((sq + Q2) * sq + Q1) * sq + Q0);
    }

    if (sign)
        temp = -temp;
    return temp;
}

double math_cosh(double x) {
    x = math_abs(x);
    if (x > 21)
        return math_exp(x) * 0.5;
    double ex = math_exp(x);
    return (ex + 1 / ex) * 0.5;
}

static const double math_tanh_p[] = {
    -9.64399179425052238628e-1,
    -9.92877231001918586564e1,
    -1.61468768441708447952e3,
};

static const double math_tanh_q[] = {
    1.12811678491632931402e2,
    2.23548839060100448583e3,
    4.84406305325125486048e3,
};

double math_tanh(double x) {
    const double maxlog = 8.8029691931113054295988e+01; /* log(2**127) */
    double z = math_abs(x);
    if (z > 0.5 * maxlog) {
        if (x < 0)
            return -1;
        return 1;
    }
    if (z >= 0.625) {
        double s = math_exp(2 * z);
        z = 1 - 2 / (s + 1);
        if (x < 0)
            z = -z;
        return z;
    }
    if (x == 0)
        return x;
    double s = x * x;
    z = x + x * s * ((math_tanh_p[0] * s + math_tanh_p[1]) * s + math_tanh_p[2]) /
                (((s + math_tanh_q[0]) * s + math_tanh_q[1]) * s + math_tanh_q[2]);
    return z;
}

double math_asinh(double x) {
    const double ln2 = 6.93147180559945286227e-01; /* 0x3FE62E42FEFA39EF */
    const double near_zero = 1.0 / (1 << 28);      /* 2**-28 */
    const double large = 1 << 28;                  /* 2**28 */
    /* special cases */
    if (math_is_nan(x) || math_is_inf(x, 0))
        return x;
    bool sign = false;
    if (x < 0) {
        x = -x;
        sign = true;
    }
    double temp;
    if (x > large)
        temp = math_log(x) + ln2; /* |x| > 2**28 */
    else if (x > 2)
        temp = math_log(2 * x + 1 / (math_sqrt(x * x + 1) + x)); /* 2**28 > |x| > 2.0 */
    else if (x < near_zero)
        temp = x; /* |x| < 2**-28 */
    else
        temp =
            math_log1p(x + x * x / (1 + math_sqrt(1 + x * x))); /* 2.0 > |x| > 2**-28 */
    if (sign)
        temp = -temp;
    return temp;
}

double math_acosh(double x) {
    const double large = 1 << 28; /* 2**28 */
    /* first case is special case */
    if (x < 1 || math_is_nan(x))
        return math_nan();
    if (x == 1)
        return 0;
    if (x >= large)
        return math_log(x) + MATH_LN2; /* x > 2**28 */
    if (x > 2)
        return math_log(2 * x - 1 / (x + math_sqrt(x * x - 1))); /* 2**28 > x > 2 */
    double t = x - 1;
    return math_log1p(t + math_sqrt(2 * t + t * t)); /* 2 >= x > 1 */
}

double math_atanh(double x) {
    const double near_zero = 1.0 / (1 << 28); /* 2**-28 */
    /* special cases */
    if (x < -1 || x > 1 || math_is_nan(x))
        return math_nan();
    if (x == 1)
        return math_inf(1);
    if (x == -1)
        return math_inf(-1);
    bool sign = false;
    if (x < 0) {
        x = -x;
        sign = true;
    }
    double temp;
    if (x < near_zero) {
        temp = x;
    } else if (x < 0.5) {
        temp = x + x;
        temp = 0.5 * math_log1p(temp + temp * x / (1 - x));
    } else {
        temp = 0.5 * math_log1p((x + x) / (1 - x));
    }
    if (sign)
        temp = -temp;
    return temp;
}

#if defined(__GNUC__) && !defined(__clang__)
#pragma GCC pop_options
#endif
