/* math/big: binary floating point numbers, float.go, floatconv.go, sqrt.go
 * and floatmarsh.go.
 *
 * The bf_ functions are Go's Float methods as written, over the nat layer and
 * its scratch memory. The big_float_ functions around them are the public API
 * and work the way the Int ones do: the mantissa is the one field with memory
 * behind it, and it is committed back into the receiver's own buffer on the
 * way out. The text conversions are in big_ftoa.c.
 *
 * Copyright 2014 The Go Authors. All rights reserved.
 * Copyright 2026 The burrow Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style licence that can be found
 * in the LICENSE file. */

#include "big_float.h"

#include "burrow/declare.h"
#include "burrow/encoding.h"
#include "burrow/io.h"
#include "burrow/math.h"
#include "burrow/mem/heap.h"
#include "burrow/panic.h"

#include <math.h>

/* ---------------------------------------------------------------- ErrNaN */

Str big_err_nan_error(BigErrNaN err) {
    return err.msg;
}

const Type burrow_type_BigErrNaN = {
    {(const Byte *)"ErrNaN", 6},
    {(const Byte *)"math/big", 8},
    KIND_STRUCT,
    (uint32_t)sizeof(BigErrNaN),
    (uint16_t)_Alignof(BigErrNaN),
    0,
    0,
    NULL,
    NULL,
    NULL,
    NULL,
    0,
    0x6269676eU, /* "bign" */
    NULL,
};

const Type *const TYPE_BIG_ERR_NAN = &burrow_type_BigErrNaN;

static Str bf_nan_message(const void *self) {
    return ((const BigErrNaN *)self)->msg;
}

static Error bf_nan_clone(const void *self, Alloc *a);

static const ErrorVT bf_nan_vt = {
    .self_type = &burrow_type_BigErrNaN,
    .message = bf_nan_message,
    .clone = bf_nan_clone,
};

/* Every ErrNaN is one of the constants below, so a copy is the same one. */
static Error bf_nan_clone(const void *self, Alloc *a) {
    (void)a;
    return (Error){&bf_nan_vt, self};
}

void bf_nan(const BigErrNaN *e) {
    Error err = {&bf_nan_vt, e};
    panic(BURROW_ANY(TYPE_ERROR, &err));
}

#define BF_NAN(name, text) static const BigErrNaN name = {BURROW_S_INIT(text)}

BF_NAN(bf_nan_new_float, "NewFloat(NaN)");
BF_NAN(bf_nan_set_float64, "Float.SetFloat64(NaN)");
BF_NAN(bf_nan_add, "addition of infinities with opposite signs");
BF_NAN(bf_nan_sub, "subtraction of infinities with equal signs");
BF_NAN(bf_nan_mul, "multiplication of zero with infinity");
BF_NAN(bf_nan_quo, "division of zero by zero or infinity by infinity");
BF_NAN(bf_nan_sqrt, "square root of negative operand");

/* --------------------------------------------------------------- float.go */

static BigAccuracy bf_make_acc(bool above) {
    return above ? BIG_ABOVE : BIG_BELOW;
}

void bf_set_prec(BigFloat *z, Uint prec) {
    z->acc = BIG_EXACT; /* optimistically assume no rounding is needed */

    /* special case */
    if (prec == 0) {
        z->prec = 0;
        if (z->form == BF_FINITE) {
            /* truncate z to 0 */
            z->acc = bf_make_acc(z->neg);
            z->form = BF_ZERO;
        }
        return;
    }

    /* general case */
    if (prec > BIG_MAX_PREC)
        prec = BIG_MAX_PREC;
    uint32_t old = z->prec;
    z->prec = (uint32_t)prec;
    if (z->prec < old)
        bf_round(z, 0);
}

Uint bf_min_prec(const BigFloat *x) {
    if (x->form != BF_FINITE)
        return 0;
    return (Uint)x->mant.len * BIG_W - nat_trailing_zero_bits(x->mant);
}

Int bf_sign(const BigFloat *x) {
    if (x->form == BF_ZERO)
        return 0;
    if (x->neg)
        return -1;
    return 1;
}

int64_t bf_mant_exp(const BigFloat *x, BigFloat *mant) {
    int64_t exp = 0;
    if (x->form == BF_FINITE)
        exp = x->exp;
    if (mant != NULL) {
        bf_copy(mant, x);
        if (mant->form == BF_FINITE)
            mant->exp = 0;
    }
    return exp;
}

void bf_set_exp_and_round(BigFloat *z, int64_t exp, Uint sbit) {
    if (exp < BIG_MIN_EXP) {
        /* underflow */
        z->acc = bf_make_acc(z->neg);
        z->form = BF_ZERO;
        return;
    }

    if (exp > BIG_MAX_EXP) {
        /* overflow */
        z->acc = bf_make_acc(!z->neg);
        z->form = BF_INF;
        return;
    }

    z->form = BF_FINITE;
    z->exp = (int32_t)exp;
    bf_round(z, sbit);
}

void bf_set_mant_exp(BigFloat *z, const BigFloat *mant, int64_t exp) {
    bf_copy(z, mant);

    if (z->form != BF_FINITE)
        return;
    /* 0 < |mant| < +Inf */
    bf_set_exp_and_round(z, (int64_t)z->exp + exp, 0);
}

bool bf_is_int(const BigFloat *x) {
    if (x->form != BF_FINITE)
        return x->form == BF_ZERO;
    /* x.form == finite */
    if (x->exp <= 0)
        return false;
    /* x.exp > 0 */
    return x->prec <= (uint32_t)x->exp ||
           bf_min_prec(x) <= (Uint)x->exp; /* not enough bits for fractional mantissa */
}

/* validate0: what is wrong with x, or "" when nothing is. */
Str bf_validate0(const BigFloat *x) {
    if (x->form != BF_FINITE)
        return BURROW_STR_EMPTY;
    Int m = x->mant.len;
    if (m == 0)
        return BURROW_S("nonzero finite number with empty mantissa");
    const BigWord msb = (BigWord)1 << (BIG_W - 1);
    if ((x->mant.p[m - 1] & msb) == 0) {
        Alloc *sa = big_scratch_alloc();
        Str t = bf_text(x, sa, 'p', 0);
        return fmt_sprintf_v(sa, "msb not set in last word %#x of %s", x->mant.p[m - 1],
                             t);
    }
    if (x->prec == 0)
        return BURROW_S("zero precision finite number");
    return BURROW_STR_EMPTY;
}

/* round rounds z according to z.mode to z.prec bits and sets z.acc
 * accordingly. z's mantissa must be normalized (with the msb set) or empty.
 *
 * CAUTION: The rounding modes ToNegativeInf, ToPositiveInf are affected by
 * the sign of z. For correct rounding, the sign of z must be set correctly
 * before calling round. */
void bf_round(BigFloat *z, Uint sbit) {
    z->acc = BIG_EXACT;
    if (z->form != BF_FINITE) {
        /* ±0 or ±Inf => nothing left to do */
        return;
    }
    /* z.form == finite && len(z.mant) > 0
     * m > 0 implies z.prec > 0 (checked by validate) */

    uint32_t m = (uint32_t)z->mant.len;  /* present mantissa length in words */
    uint32_t bits = m * (uint32_t)BIG_W; /* present mantissa bits; bits > 0 */
    if (bits <= z->prec) {
        /* mantissa fits => nothing to do */
        return;
    }
    /* bits > z.prec */

    /* Rounding is based on two bits: the rounding bit (rbit) and the
     * sticky bit (sbit). The rbit is the bit immediately before the
     * z.prec leading mantissa bits (the "0.5"). The sbit is set if any
     * of the bits before the rbit are set (the "0.25", "0.125", etc.):
     *
     *   rbit  sbit  => "fractional part"
     *
     *   0     0        == 0
     *   0     1        >  0  , < 0.5
     *   1     0        == 0.5
     *   1     1        >  0.5, < 1.0 */

    /* bits > z.prec: mantissa too large => round */
    Uint r = (Uint)(bits - z->prec - 1); /* rounding bit position; r >= 0 */
    Uint rbit = nat_bit(z->mant, r) &
                1; /* rounding bit; be safe and ensure it's a single bit */
    /* The sticky bit is only needed for rounding ToNearestEven
     * or when the rounding bit is zero. Avoid computation otherwise. */
    if (sbit == 0 && (rbit == 0 || z->mode == BIG_TO_NEAREST_EVEN))
        sbit = nat_sticky(z->mant, r);
    sbit &= 1; /* be safe and ensure it's a single bit */

    /* cut off extra words */
    uint32_t n = (uint32_t)((z->prec + (BIG_W - 1)) /
                            BIG_W); /* mantissa length in words for desired precision */
    if (m > n) {
        nat_copy(z->mant,
                 nat_from(z->mant, (Int)(m - n))); /* move n last words to front */
        z->mant = nat_to(z->mant, (Int)n);
    }

    /* determine number of trailing zero bits (ntz) and compute lsb mask of
     * mantissa's least-significant word */
    Uint ntz = n * BIG_W - z->prec; /* 0 <= ntz < _W */
    BigWord lsb = (BigWord)1 << ntz;

    /* round if result is inexact */
    if ((rbit | sbit) != 0) {
        /* Make rounding decision: The result mantissa is truncated ("rounded
         * down") by default. Decide if we need to increment, or "round up",
         * the (unsigned) mantissa. */
        bool inc = false;
        switch (z->mode) {
        case BIG_TO_NEGATIVE_INF:
            inc = z->neg;
            break;
        case BIG_TO_ZERO:
            /* nothing to do */
            break;
        case BIG_TO_NEAREST_EVEN:
            inc = rbit != 0 && (sbit != 0 || (z->mant.p[0] & lsb) != 0);
            break;
        case BIG_TO_NEAREST_AWAY:
            inc = rbit != 0;
            break;
        case BIG_AWAY_FROM_ZERO:
            inc = true;
            break;
        case BIG_TO_POSITIVE_INF:
            inc = !z->neg;
            break;
        default:
            panic_str(BURROW_S("unreachable"));
        }

        /* A positive result (!z.neg) is Above the exact result if we
         * increment, and it's Below if we truncate (Exact results require no
         * rounding). For a negative result (z.neg) it is exactly the
         * opposite. */
        z->acc = bf_make_acc(inc != z->neg);

        if (inc) {
            /* add 1 to mantissa */
            if (big_add_vw(z->mant, z->mant, lsb) != 0) {
                /* mantissa overflow => adjust exponent */
                if (z->exp >= BIG_MAX_EXP) {
                    /* exponent overflow */
                    z->form = BF_INF;
                    return;
                }
                z->exp++;
                /* adjust mantissa: divide by 2 to compensate for exponent
                 * adjustment */
                big_rsh_vu(z->mant, z->mant, 1);
                /* set msb == carry == 1 from the mantissa overflow above */
                const BigWord msb = (BigWord)1 << (BIG_W - 1);
                z->mant.p[n - 1] |= msb;
            }
        }
    }

    /* zero out trailing bits in least-significant word */
    z->mant.p[0] &= ~(lsb - 1);
}

static void bf_set_bits64(BigFloat *z, bool neg, uint64_t x) {
    if (z->prec == 0)
        z->prec = 64;
    z->acc = BIG_EXACT;
    z->neg = neg;
    if (x == 0) {
        z->form = BF_ZERO;
        return;
    }
    /* x != 0 */
    z->form = BF_FINITE;
    Int s = bits_leading_zeros64(x);
    z->mant = nat_set_uint64(z->mant, x << s);
    z->exp = (int32_t)(64 - s); /* always fits */
    if (z->prec < 64)
        bf_round(z, 0);
}

void bf_set_uint64(BigFloat *z, uint64_t x) {
    bf_set_bits64(z, false, x);
}

void bf_set_int64(BigFloat *z, int64_t x) {
    uint64_t u = (uint64_t)x;
    if (x < 0)
        u = 0 - u;
    /* We cannot simply call z.SetUint64(uint64(u)) and change the sign
     * afterwards because the sign affects rounding. */
    bf_set_bits64(z, x < 0, u);
}

void bf_set_float64(BigFloat *z, double x) {
    if (z->prec == 0)
        z->prec = 53;
    if (math_is_nan(x))
        bf_nan(&bf_nan_set_float64);
    z->acc = BIG_EXACT;
    z->neg = math_signbit(x); /* handle -0, -Inf correctly */
    if (x == 0) {
        z->form = BF_ZERO;
        return;
    }
    if (math_is_inf(x, 0)) {
        z->form = BF_INF;
        return;
    }
    z->form = BF_FINITE;
    int exp;
    double fmant = frexp(x, &exp); /* get normalized mantissa */
    uint64_t fbits;
    memcpy(&fbits, &fmant, sizeof fbits);
    z->mant = nat_set_uint64(z->mant, (uint64_t)1 << 63 | fbits << 11);
    z->exp = (int32_t)exp; /* always fits */
    if (z->prec < 53)
        bf_round(z, 0);
}

/* fnorm normalizes mantissa m by shifting it to the left such that the msb
 * of the most-significant word (msw) is 1. It returns the shift amount. It
 * assumes that len(m) != 0. */
int64_t bf_fnorm(Nat m) {
    if (m.len == 0)
        return 0;
    Uint s = big_nlz(m.p[m.len - 1]);
    if (s > 0)
        big_lsh_vu(m, m, s);
    return (int64_t)s;
}

void bf_set_int(BigFloat *z, const BigInt *x) {
    /* TODO(gri) can be more efficient if z.prec > 0
     * but small compared to the size of x, or if there
     * are many trailing 0's. */
    uint32_t bits = (uint32_t)nat_bit_len(x->abs);
    if (z->prec == 0)
        z->prec = bits > 64 ? bits : 64;
    z->acc = BIG_EXACT;
    z->neg = x->neg;
    if (x->abs.len == 0) {
        z->form = BF_ZERO;
        return;
    }
    /* x != 0 */
    z->mant = nat_set(z->mant, x->abs);
    bf_fnorm(z->mant);
    bf_set_exp_and_round(z, bits, 0);
}

static void bf_set_rat(BigFloat *z, const BigRat *x) {
    if (x->b.abs.len == 0 || nat_cmp(x->b.abs, NAT_ONE) == 0) {
        bf_set_int(z, &x->a);
        return;
    }
    BigFloat a = BIG_FLOAT(NULL), b = BIG_FLOAT(NULL);
    bf_set_int(&a, &x->a);
    bf_set_int(&b, &x->b);
    if (z->prec == 0)
        z->prec = a.prec > b.prec ? a.prec : b.prec;
    bf_quo(z, &a, &b);
}

void bf_set_inf(BigFloat *z, bool signbit) {
    z->acc = BIG_EXACT;
    z->form = BF_INF;
    z->neg = signbit;
}

void bf_set(BigFloat *z, const BigFloat *x) {
    z->acc = BIG_EXACT;
    if (z != x) {
        z->form = x->form;
        z->neg = x->neg;
        if (x->form == BF_FINITE) {
            z->exp = x->exp;
            z->mant = nat_set(z->mant, x->mant);
        }
        if (z->prec == 0)
            z->prec = x->prec;
        else if (z->prec < x->prec)
            bf_round(z, 0);
    }
}

void bf_copy(BigFloat *z, const BigFloat *x) {
    if (z != x) {
        z->prec = x->prec;
        z->mode = x->mode;
        z->acc = x->acc;
        z->form = x->form;
        z->neg = x->neg;
        if (z->form == BF_FINITE) {
            z->mant = nat_set(z->mant, x->mant);
            z->exp = x->exp;
        }
    }
}

/* msb32 returns the 32 most significant bits of x. */
static uint32_t bf_msb32(Nat x) {
    Int i = x.len - 1;
    if (i < 0)
        return 0;
#if BITS_UINT_SIZE == 32
    return (uint32_t)x.p[i];
#else
    return (uint32_t)(x.p[i] >> 32);
#endif
}

/* msb64 returns the 64 most significant bits of x. */
static uint64_t bf_msb64(Nat x) {
    Int i = x.len - 1;
    if (i < 0)
        return 0;
#if BITS_UINT_SIZE == 32
    uint64_t v = (uint64_t)x.p[i] << 32;
    if (i > 0)
        v |= (uint64_t)x.p[i - 1];
    return v;
#else
    return (uint64_t)x.p[i];
#endif
}

static uint64_t bf_uint64(const BigFloat *x, BigAccuracy *acc) {
    switch (x->form) {
    case BF_FINITE:
        if (x->neg) {
            *acc = BIG_ABOVE;
            return 0;
        }
        /* 0 < x < +Inf */
        if (x->exp <= 0) {
            /* 0 < x < 1 */
            *acc = BIG_BELOW;
            return 0;
        }
        /* 1 <= x < Inf */
        if (x->exp <= 64) {
            /* u = trunc(x) fits into a uint64 */
            uint64_t u = bf_msb64(x->mant) >> (64 - (uint32_t)x->exp);
            *acc = bf_min_prec(x) <= 64 ? BIG_EXACT : BIG_BELOW; /* x truncated */
            return u;
        }
        /* x too large */
        *acc = BIG_BELOW;
        return UINT64_MAX;
    case BF_ZERO:
        *acc = BIG_EXACT;
        return 0;
    case BF_INF:
        if (x->neg) {
            *acc = BIG_ABOVE;
            return 0;
        }
        *acc = BIG_BELOW;
        return UINT64_MAX;
    default:
        panic_str(BURROW_S("unreachable"));
    }
}

static int64_t bf_int64(const BigFloat *x, BigAccuracy *acc) {
    switch (x->form) {
    case BF_FINITE: {
        /* 0 < |x| < +Inf */
        BigAccuracy a = bf_make_acc(x->neg);
        if (x->exp <= 0) {
            /* 0 < |x| < 1 */
            *acc = a;
            return 0;
        }
        /* x.exp > 0 */

        /* 1 <= |x| < +Inf */
        if (x->exp <= 63) {
            /* i = trunc(x) fits into an int64 (excluding math.MinInt64) */
            int64_t i = (int64_t)(bf_msb64(x->mant) >> (64 - (uint32_t)x->exp));
            if (x->neg)
                i = -i;
            *acc = a; /* x truncated */
            if (bf_min_prec(x) <= (Uint)x->exp)
                *acc = BIG_EXACT;
            return i;
        }
        if (x->neg) {
            /* check for special case x == math.MinInt64 (i.e., x == -(0.5 << 64)) */
            if (x->exp == 64 && bf_min_prec(x) == 1)
                a = BIG_EXACT;
            *acc = a;
            return INT64_MIN;
        }
        /* x too large */
        *acc = BIG_BELOW;
        return INT64_MAX;
    }
    case BF_ZERO:
        *acc = BIG_EXACT;
        return 0;
    case BF_INF:
        if (x->neg) {
            *acc = BIG_ABOVE;
            return INT64_MIN;
        }
        *acc = BIG_BELOW;
        return INT64_MAX;
    default:
        panic_str(BURROW_S("unreachable"));
    }
}

/* x - 1 in int32 arithmetic, wrapping the way Go's does: MinExp - 1 is
 * MaxExp, which makes Float32 and Float64 of the smallest exponent overflow
 * rather than underflow, as they do in Go. */
static int32_t bf_dec32(int32_t x) {
    uint32_t u = (uint32_t)x - 1;
    return u > INT32_MAX ? -(int32_t)(UINT32_MAX - u) - 1 : (int32_t)u;
}

static float bf_f32(uint32_t bits) {
    float f;
    memcpy(&f, &bits, sizeof f);
    return f;
}

static float bf_float32(const BigFloat *x, BigAccuracy *acc) {
    switch (x->form) {
    case BF_FINITE: {
        /* 0 < |x| < +Inf */

        enum {
            fbits = 32, /*        float size */
            mbits = 23, /*        mantissa size (excluding implicit msb) */
            ebits = fbits - mbits - 1,     /*     8  exponent size */
            bias = (1 << (ebits - 1)) - 1, /*   127  exponent bias */
            emin = 1 - bias, /*  -126  smallest unbiased exponent (normal) */
            emax = bias      /*   127  largest unbiased exponent (normal) */
        };

        /* Float mantissa m is 0.5 <= m < 1.0; compute exponent e for float32
         * mantissa. */
        int32_t e =
            bf_dec32(x->exp); /* exponent for normal mantissa m with 1.0 <= m < 2.0 */

        /* Compute precision p for float32 mantissa.
         * If the exponent is too small, we have a denormal number before
         * rounding and fewer than p mantissa bits of precision available
         * (the exponent remains fixed but the mantissa gets shifted right). */
        int64_t p = mbits + 1; /* precision of normal float */
        if (e < emin) {
            /* recompute precision */
            p = mbits + 1 - emin + (int64_t)e;
            /* If p == 0, the mantissa of x is shifted so much to the right
             * that its msb falls immediately to the right of the float32
             * mantissa space. In other words, if the smallest denormal is
             * considered "1.0", for p == 0, the mantissa value m is >= 0.5.
             * If m > 0.5, it is rounded up to 1.0; i.e., the smallest denormal.
             * If m == 0.5, it is rounded down to even, i.e., 0.0.
             * If p < 0, the mantissa value m is <= "0.25" which is never rounded up. */
            if (p < 0 /* m <= 0.25 */ ||
                (p == 0 && nat_sticky(x->mant, (Uint)x->mant.len * BIG_W - 1) ==
                               0) /* m == 0.5 */) {
                /* underflow to ±0 */
                if (x->neg) {
                    *acc = BIG_ABOVE;
                    return -0.0F;
                }
                *acc = BIG_BELOW;
                return 0.0F;
            }
            /* otherwise, round up
             * We handle p == 0 explicitly because it's easy and because
             * Float.round doesn't support rounding to 0 bits of precision. */
            if (p == 0) {
                if (x->neg) {
                    *acc = BIG_BELOW;
                    return -bf_f32(1);
                }
                *acc = BIG_ABOVE;
                return bf_f32(1);
            }
        }
        /* p > 0 */

        /* round */
        BigFloat r = BIG_FLOAT(NULL);
        r.prec = (uint32_t)p;
        bf_set(&r, x);
        e = bf_dec32(r.exp);

        /* Rounding may have caused r to overflow to ±Inf
         * (rounding never causes underflows to 0).
         * If the exponent is too large, also overflow to ±Inf. */
        if (r.form == BF_INF || e > emax) {
            /* overflow */
            if (x->neg) {
                *acc = BIG_BELOW;
                return -(float)INFINITY;
            }
            *acc = BIG_ABOVE;
            return (float)INFINITY;
        }
        /* e <= emax */

        /* Determine sign, biased exponent, and mantissa. */
        uint32_t sign = 0, bexp = 0, mant = 0;
        if (x->neg)
            sign = (uint32_t)1 << (fbits - 1);

        /* Rounding may have caused a denormal number to
         * become normal. Check again. */
        if (e < emin) {
            /* denormal number: recompute precision
             * Since rounding may have at best increased precision
             * and we have eliminated p <= 0 early, we know p > 0.
             * bexp == 0 for denormals */
            p = mbits + 1 - emin + (int64_t)e;
            mant = fbits - p < 32 ? bf_msb32(r.mant) >> (uint32_t)(fbits - p)
                                  : 0; /* Go's shift */
        } else {
            /* normal number: emin <= e <= emax */
            bexp = (uint32_t)(e + bias) << mbits;
            mant = bf_msb32(r.mant) >> ebits &
                   (((uint32_t)1 << mbits) - 1); /* cut off msb (implicit 1 bit) */
        }

        *acc = r.acc;
        return bf_f32(sign | bexp | mant);
    }
    case BF_ZERO:
        *acc = BIG_EXACT;
        return x->neg ? -0.0F : 0.0F;
    case BF_INF:
        *acc = BIG_EXACT;
        return x->neg ? -(float)INFINITY : (float)INFINITY;
    default:
        panic_str(BURROW_S("unreachable"));
    }
}

static double bf_f64(uint64_t bits) {
    double f;
    memcpy(&f, &bits, sizeof f);
    return f;
}

double bf_float64(const BigFloat *x, BigAccuracy *acc) {
    switch (x->form) {
    case BF_FINITE: {
        /* 0 < |x| < +Inf */

        enum {
            fbits = 64, /*        float size */
            mbits = 52, /*        mantissa size (excluding implicit msb) */
            ebits = fbits - mbits - 1,     /*    11  exponent size */
            bias = (1 << (ebits - 1)) - 1, /*  1023  exponent bias */
            emin = 1 - bias, /* -1022  smallest unbiased exponent (normal) */
            emax = bias      /*  1023  largest unbiased exponent (normal) */
        };

        /* Float mantissa m is 0.5 <= m < 1.0; compute exponent e for float64
         * mantissa. */
        int32_t e =
            bf_dec32(x->exp); /* exponent for normal mantissa m with 1.0 <= m < 2.0 */

        /* Compute precision p for float64 mantissa.
         * If the exponent is too small, we have a denormal number before
         * rounding and fewer than p mantissa bits of precision available
         * (the exponent remains fixed but the mantissa gets shifted right). */
        int64_t p = mbits + 1; /* precision of normal float */
        if (e < emin) {
            /* recompute precision */
            p = mbits + 1 - emin + (int64_t)e;
            /* If p == 0, the mantissa of x is shifted so much to the right
             * that its msb falls immediately to the right of the float64
             * mantissa space. In other words, if the smallest denormal is
             * considered "1.0", for p == 0, the mantissa value m is >= 0.5.
             * If m > 0.5, it is rounded up to 1.0; i.e., the smallest denormal.
             * If m == 0.5, it is rounded down to even, i.e., 0.0.
             * If p < 0, the mantissa value m is <= "0.25" which is never rounded up. */
            if (p < 0 /* m <= 0.25 */ ||
                (p == 0 && nat_sticky(x->mant, (Uint)x->mant.len * BIG_W - 1) ==
                               0) /* m == 0.5 */) {
                /* underflow to ±0 */
                if (x->neg) {
                    *acc = BIG_ABOVE;
                    return -0.0;
                }
                *acc = BIG_BELOW;
                return 0.0;
            }
            /* otherwise, round up
             * We handle p == 0 explicitly because it's easy and because
             * Float.round doesn't support rounding to 0 bits of precision. */
            if (p == 0) {
                if (x->neg) {
                    *acc = BIG_BELOW;
                    return -bf_f64(1);
                }
                *acc = BIG_ABOVE;
                return bf_f64(1);
            }
        }
        /* p > 0 */

        /* round */
        BigFloat r = BIG_FLOAT(NULL);
        r.prec = (uint32_t)p;
        bf_set(&r, x);
        e = bf_dec32(r.exp);

        /* Rounding may have caused r to overflow to ±Inf
         * (rounding never causes underflows to 0).
         * If the exponent is too large, also overflow to ±Inf. */
        if (r.form == BF_INF || e > emax) {
            /* overflow */
            if (x->neg) {
                *acc = BIG_BELOW;
                return -(double)INFINITY;
            }
            *acc = BIG_ABOVE;
            return (double)INFINITY;
        }
        /* e <= emax */

        /* Determine sign, biased exponent, and mantissa. */
        uint64_t sign = 0, bexp = 0, mant = 0;
        if (x->neg)
            sign = (uint64_t)1 << (fbits - 1);

        /* Rounding may have caused a denormal number to
         * become normal. Check again. */
        if (e < emin) {
            /* denormal number: recompute precision
             * Since rounding may have at best increased precision
             * and we have eliminated p <= 0 early, we know p > 0.
             * bexp == 0 for denormals */
            p = mbits + 1 - emin + (int64_t)e;
            mant = fbits - p < 64 ? bf_msb64(r.mant) >> (uint32_t)(fbits - p)
                                  : 0; /* Go's shift */
        } else {
            /* normal number: emin <= e <= emax */
            bexp = (uint64_t)(e + bias) << mbits;
            mant = bf_msb64(r.mant) >> ebits &
                   (((uint64_t)1 << mbits) - 1); /* cut off msb (implicit 1 bit) */
        }

        *acc = r.acc;
        return bf_f64(sign | bexp | mant);
    }
    case BF_ZERO:
        *acc = BIG_EXACT;
        return x->neg ? -0.0 : 0.0;
    case BF_INF:
        *acc = BIG_EXACT;
        return x->neg ? -(double)INFINITY : (double)INFINITY;
    default:
        panic_str(BURROW_S("unreachable"));
    }
}

/* Int: z = trunc(x), false for an infinity. */
static bool bf_int(const BigFloat *x, BigInt *z, BigAccuracy *acc) {
    switch (x->form) {
    case BF_FINITE: {
        /* 0 < |x| < +Inf */
        BigAccuracy a = bf_make_acc(x->neg);
        if (x->exp <= 0) {
            /* 0 < |x| < 1 */
            bi_set_int64(z, 0);
            *acc = a;
            return true;
        }
        /* x.exp > 0 */

        /* 1 <= |x| < +Inf
         * determine minimum required precision for x */
        Uint all_bits = (Uint)x->mant.len * BIG_W;
        Uint exp = (Uint)x->exp;
        if (bf_min_prec(x) <= exp)
            a = BIG_EXACT;
        /* shift mantissa as needed */
        z->neg = x->neg;
        if (exp > all_bits)
            z->abs = nat_lsh(z->abs, x->mant, exp - all_bits);
        else if (exp < all_bits)
            z->abs = nat_rsh(z->abs, x->mant, all_bits - exp);
        else
            z->abs = nat_set(z->abs, x->mant);
        *acc = a;
        return true;
    }
    case BF_ZERO:
        bi_set_int64(z, 0);
        *acc = BIG_EXACT;
        return true;
    case BF_INF:
        *acc = bf_make_acc(x->neg);
        return false;
    default:
        panic_str(BURROW_S("unreachable"));
    }
}

/* Rat: z = x, false for an infinity. */
static bool bf_rat(const BigFloat *x, BigRat *z, BigAccuracy *acc) {
    switch (x->form) {
    case BF_FINITE: {
        /* 0 < |x| < +Inf */
        int32_t all_bits = (int32_t)x->mant.len * (int32_t)BIG_W;
        /* build up numerator and denominator */
        z->a.neg = x->neg;
        if (x->exp > all_bits) {
            z->a.abs = nat_lsh(z->a.abs, x->mant, (Uint)(x->exp - all_bits));
            z->b.abs = nat_to(z->b.abs, 0); /* == 1 (see Rat) */
            /* z already in normal form */
        } else if (x->exp < all_bits) {
            z->a.abs = nat_set(z->a.abs, x->mant);
            Nat t = nat_set_uint64(z->b.abs, 1);
            z->b.abs = nat_lsh(t, t, (Uint)(all_bits - x->exp));
            br_norm(z);
        } else {
            z->a.abs = nat_set(z->a.abs, x->mant);
            z->b.abs = nat_to(z->b.abs, 0); /* == 1 (see Rat) */
            /* z already in normal form */
        }
        *acc = BIG_EXACT;
        return true;
    }
    case BF_ZERO:
        bi_set_int64(&z->a, 0);
        z->b.abs = nat_set_word(z->b.abs, 1);
        *acc = BIG_EXACT;
        return true;
    case BF_INF:
        *acc = bf_make_acc(x->neg);
        return false;
    default:
        panic_str(BURROW_S("unreachable"));
    }
}

void bf_neg(BigFloat *z, const BigFloat *x) {
    bf_set(z, x);
    z->neg = !z->neg;
}

/* z = x + y, ignoring signs of x and y for the addition
 * but using the sign of z for rounding the result.
 * x and y must have a non-empty mantissa and valid exponent. */
static void bf_uadd(BigFloat *z, const BigFloat *x, const BigFloat *y) {
    /* Note: This implementation requires 2 shifts most of the
     * time. It is also inefficient if exponents or precisions
     * differ by wide margins. The following article describes
     * an efficient (but much more complicated) implementation
     * compatible with the internal representation used here:
     *
     * Vincent Lefèvre: "The Generic Multiple-Precision Floating-
     * Point Addition With Exact Rounding (as in the MPFR Library)"
     * http://www.vinc17.net/research/papers/rnc6.pdf */

    int64_t ex = (int64_t)x->exp - (int64_t)x->mant.len * (int64_t)BIG_W;
    int64_t ey = (int64_t)y->exp - (int64_t)y->mant.len * (int64_t)BIG_W;

    bool al = nat_alias(z->mant, x->mant) || nat_alias(z->mant, y->mant);

    /* TODO(gri) having a combined add-and-shift primitive
     *           could make this code significantly faster */
    if (ex < ey) {
        if (al) {
            Nat t = nat_lsh(NAT_NIL, y->mant, (Uint)(ey - ex));
            z->mant = nat_add(z->mant, x->mant, t);
        } else {
            z->mant = nat_lsh(z->mant, y->mant, (Uint)(ey - ex));
            z->mant = nat_add(z->mant, x->mant, z->mant);
        }
    } else if (ex > ey) {
        if (al) {
            Nat t = nat_lsh(NAT_NIL, x->mant, (Uint)(ex - ey));
            z->mant = nat_add(z->mant, t, y->mant);
        } else {
            z->mant = nat_lsh(z->mant, x->mant, (Uint)(ex - ey));
            z->mant = nat_add(z->mant, z->mant, y->mant);
        }
        ex = ey;
    } else {
        z->mant = nat_add(z->mant, x->mant, y->mant);
    }
    /* len(z.mant) > 0 */

    bf_set_exp_and_round(
        z, ex + (int64_t)z->mant.len * (int64_t)BIG_W - bf_fnorm(z->mant), 0);
}

/* z = x - y for |x| > |y|, ignoring signs of x and y for the subtraction
 * but using the sign of z for rounding the result.
 * x and y must have a non-empty mantissa and valid exponent. */
static void bf_usub(BigFloat *z, const BigFloat *x, const BigFloat *y) {
    /* This code is symmetric to uadd.
     * We have not factored the common code out because
     * eventually uadd (and usub) should be optimized
     * by special-casing, and the code will diverge. */

    int64_t ex = (int64_t)x->exp - (int64_t)x->mant.len * (int64_t)BIG_W;
    int64_t ey = (int64_t)y->exp - (int64_t)y->mant.len * (int64_t)BIG_W;

    bool al = nat_alias(z->mant, x->mant) || nat_alias(z->mant, y->mant);

    if (ex < ey) {
        if (al) {
            Nat t = nat_lsh(NAT_NIL, y->mant, (Uint)(ey - ex));
            z->mant = nat_sub(t, x->mant, t);
        } else {
            z->mant = nat_lsh(z->mant, y->mant, (Uint)(ey - ex));
            z->mant = nat_sub(z->mant, x->mant, z->mant);
        }
    } else if (ex > ey) {
        if (al) {
            Nat t = nat_lsh(NAT_NIL, x->mant, (Uint)(ex - ey));
            z->mant = nat_sub(t, t, y->mant);
        } else {
            z->mant = nat_lsh(z->mant, x->mant, (Uint)(ex - ey));
            z->mant = nat_sub(z->mant, z->mant, y->mant);
        }
        ex = ey;
    } else {
        z->mant = nat_sub(z->mant, x->mant, y->mant);
    }

    /* operands may have canceled each other out */
    if (z->mant.len == 0) {
        z->acc = BIG_EXACT;
        z->form = BF_ZERO;
        z->neg = false;
        return;
    }
    /* len(z.mant) > 0 */

    bf_set_exp_and_round(
        z, ex + (int64_t)z->mant.len * (int64_t)BIG_W - bf_fnorm(z->mant), 0);
}

/* z = x * y, ignoring signs of x and y for the multiplication
 * but using the sign of z for rounding the result.
 * x and y must have a non-empty mantissa and valid exponent. */
static void bf_umul(BigFloat *z, const BigFloat *x, const BigFloat *y) {
    /* Note: This is doing too much work if the precision
     * of z is less than the sum of the precisions of x
     * and y which is often the case (e.g., if all floats
     * have the same precision).
     * TODO(gri) Optimize this for the common case. */

    int64_t e = (int64_t)x->exp + (int64_t)y->exp;
    if (x == y)
        z->mant = nat_sqr(z->mant, x->mant);
    else
        z->mant = nat_mul(z->mant, x->mant, y->mant);

    bf_set_exp_and_round(z, e - bf_fnorm(z->mant), 0);
}

/* z = x / y, ignoring signs of x and y for the division
 * but using the sign of z for rounding the result.
 * x and y must have a non-empty mantissa and valid exponent. */
static void bf_uquo(BigFloat *z, const BigFloat *x, const BigFloat *y) {
    /* mantissa length in words for desired result precision + 1
     * (at least one extra bit so we get the rounding bit after
     * the division) */
    Int n = (Int)(z->prec / BIG_W) + 1;

    /* compute adjusted x.mant such that we get enough result precision */
    Nat xadj = x->mant;
    Int d = n - x->mant.len + y->mant.len;
    if (d > 0) {
        /* d extra words needed => add d "0 digits" to x */
        Int len = x->mant.len + d;
        xadj = (Nat){big_alloc(len), len, len};
        nat_clear(nat_to(xadj, d));
        nat_copy(nat_from(xadj, d), x->mant);
    }
    /* TODO(gri): If we have too many digits (d < 0), we should be able
     * to shorten x for faster division. But we must be extra careful
     * with rounding in that case. */

    /* Compute d before division since there may be aliasing of x.mant
     * (via xadj) or y.mant with z.mant. */
    d = xadj.len - y->mant.len;

    /* divide */
    Nat r;
    z->mant = nat_div(z->mant, NAT_NIL, xadj, y->mant, &r);
    int64_t e =
        (int64_t)x->exp - (int64_t)y->exp - (int64_t)(d - z->mant.len) * (int64_t)BIG_W;

    /* The result is long enough to include (at least) the rounding bit.
     * If there's a non-zero remainder, the corresponding fractional part
     * (if it were computed), would have a non-zero sticky bit (if it were
     * zero, it couldn't have a non-zero remainder). */
    Uint sbit = 0;
    if (r.len > 0)
        sbit = 1;

    bf_set_exp_and_round(z, e - bf_fnorm(z->mant), sbit);
}

/* ucmp returns -1, 0, or +1, depending on whether
 * |x| < |y|, |x| == |y|, or |x| > |y|.
 * x and y must have a non-empty mantissa and valid exponent. */
static int bf_ucmp(const BigFloat *x, const BigFloat *y) {
    if (x->exp < y->exp)
        return -1;
    if (x->exp > y->exp)
        return +1;
    /* x.exp == y.exp */

    /* compare mantissas */
    Int i = x->mant.len;
    Int j = y->mant.len;
    while (i > 0 || j > 0) {
        BigWord xm = 0, ym = 0;
        if (i > 0) {
            i--;
            xm = x->mant.p[i];
        }
        if (j > 0) {
            j--;
            ym = y->mant.p[j];
        }
        if (xm < ym)
            return -1;
        if (xm > ym)
            return +1;
    }

    return 0;
}

static uint32_t bf_max_prec(const BigFloat *x, const BigFloat *y) {
    return x->prec > y->prec ? x->prec : y->prec;
}

void bf_add(BigFloat *z, const BigFloat *x, const BigFloat *y) {
    if (z->prec == 0)
        z->prec = bf_max_prec(x, y);

    if (x->form == BF_FINITE && y->form == BF_FINITE) {
        /* x + y (common case)

         * Below we set z.neg = x.neg, and when z aliases y this will
         * change the y operand's sign. This is fine, because if an
         * operand aliases the receiver it'll be overwritten, but we still
         * want the original x.neg and y.neg values when we evaluate
         * x.neg != y.neg, so we need to save y.neg before setting z.neg. */
        bool yneg = y->neg;

        z->neg = x->neg;
        if (x->neg == yneg) {
            /* x + y == x + y
             * (-x) + (-y) == -(x + y) */
            bf_uadd(z, x, y);
        } else {
            /* x + (-y) == x - y == -(y - x)
             * (-x) + y == y - x == -(x - y) */
            if (bf_ucmp(x, y) > 0) {
                bf_usub(z, x, y);
            } else {
                z->neg = !z->neg;
                bf_usub(z, y, x);
            }
        }
        if (z->form == BF_ZERO && z->mode == BIG_TO_NEGATIVE_INF && z->acc == BIG_EXACT)
            z->neg = true;
        return;
    }

    if (x->form == BF_INF && y->form == BF_INF && x->neg != y->neg) {
        /* +Inf + -Inf
         * -Inf + +Inf
         * value of z is undefined but make sure it's valid */
        z->acc = BIG_EXACT;
        z->form = BF_ZERO;
        z->neg = false;
        bf_nan(&bf_nan_add);
    }

    if (x->form == BF_ZERO && y->form == BF_ZERO) {
        /* ±0 + ±0 */
        z->acc = BIG_EXACT;
        z->form = BF_ZERO;
        z->neg = x->neg && y->neg; /* -0 + -0 == -0 */
        return;
    }

    if (x->form == BF_INF || y->form == BF_ZERO) {
        /* ±Inf + y
         * x + ±0 */
        bf_set(z, x);
        return;
    }

    /* ±0 + y
     * x + ±Inf */
    bf_set(z, y);
}

void bf_sub(BigFloat *z, const BigFloat *x, const BigFloat *y) {
    if (z->prec == 0)
        z->prec = bf_max_prec(x, y);

    if (x->form == BF_FINITE && y->form == BF_FINITE) {
        /* x - y (common case) */
        bool yneg = y->neg;
        z->neg = x->neg;
        if (x->neg != yneg) {
            /* x - (-y) == x + y
             * (-x) - y == -(x + y) */
            bf_uadd(z, x, y);
        } else {
            /* x - y == x - y == -(y - x)
             * (-x) - (-y) == y - x == -(x - y) */
            if (bf_ucmp(x, y) > 0) {
                bf_usub(z, x, y);
            } else {
                z->neg = !z->neg;
                bf_usub(z, y, x);
            }
        }
        if (z->form == BF_ZERO && z->mode == BIG_TO_NEGATIVE_INF && z->acc == BIG_EXACT)
            z->neg = true;
        return;
    }

    if (x->form == BF_INF && y->form == BF_INF && x->neg == y->neg) {
        /* +Inf - +Inf
         * -Inf - -Inf
         * value of z is undefined but make sure it's valid */
        z->acc = BIG_EXACT;
        z->form = BF_ZERO;
        z->neg = false;
        bf_nan(&bf_nan_sub);
    }

    if (x->form == BF_ZERO && y->form == BF_ZERO) {
        /* ±0 - ±0 */
        z->acc = BIG_EXACT;
        z->form = BF_ZERO;
        z->neg = x->neg && !y->neg; /* -0 - +0 == -0 */
        return;
    }

    if (x->form == BF_INF || y->form == BF_ZERO) {
        /* ±Inf - y
         * x - ±0 */
        bf_set(z, x);
        return;
    }

    /* ±0 - y
     * x - ±Inf */
    bf_neg(z, y);
}

void bf_mul(BigFloat *z, const BigFloat *x, const BigFloat *y) {
    if (z->prec == 0)
        z->prec = bf_max_prec(x, y);

    z->neg = x->neg != y->neg;

    if (x->form == BF_FINITE && y->form == BF_FINITE) {
        /* x * y (common case) */
        bf_umul(z, x, y);
        return;
    }

    z->acc = BIG_EXACT;
    if ((x->form == BF_ZERO && y->form == BF_INF) ||
        (x->form == BF_INF && y->form == BF_ZERO)) {
        /* ±0 * ±Inf
         * ±Inf * ±0
         * value of z is undefined but make sure it's valid */
        z->form = BF_ZERO;
        z->neg = false;
        bf_nan(&bf_nan_mul);
    }

    if (x->form == BF_INF || y->form == BF_INF) {
        /* ±Inf * y
         * x * ±Inf */
        z->form = BF_INF;
        return;
    }

    /* ±0 * y
     * x * ±0 */
    z->form = BF_ZERO;
}

void bf_quo(BigFloat *z, const BigFloat *x, const BigFloat *y) {
    if (z->prec == 0)
        z->prec = bf_max_prec(x, y);

    z->neg = x->neg != y->neg;

    if (x->form == BF_FINITE && y->form == BF_FINITE) {
        /* x / y (common case) */
        bf_uquo(z, x, y);
        return;
    }

    z->acc = BIG_EXACT;
    if ((x->form == BF_ZERO && y->form == BF_ZERO) ||
        (x->form == BF_INF && y->form == BF_INF)) {
        /* ±0 / ±0
         * ±Inf / ±Inf
         * value of z is undefined but make sure it's valid */
        z->form = BF_ZERO;
        z->neg = false;
        bf_nan(&bf_nan_quo);
    }

    if (x->form == BF_ZERO || y->form == BF_INF) {
        /* ±0 / y
         * x / ±Inf */
        z->form = BF_ZERO;
        return;
    }

    /* x / ±0
     * ±Inf / y */
    z->form = BF_INF;
}

/* ord classifies x and returns:
 *
 *	-2 if -Inf == x
 *	-1 if -Inf < x < 0
 *	 0 if x == 0 (signed or unsigned)
 *	+1 if 0 < x < +Inf
 *	+2 if x == +Inf */
static int bf_ord(const BigFloat *x) {
    int m;
    switch (x->form) {
    case BF_FINITE:
        m = 1;
        break;
    case BF_ZERO:
        return 0;
    case BF_INF:
        m = 2;
        break;
    default:
        panic_str(BURROW_S("unreachable"));
    }
    if (x->neg)
        m = -m;
    return m;
}

static int bf_cmp(const BigFloat *x, const BigFloat *y) {
    int mx = bf_ord(x);
    int my = bf_ord(y);
    if (mx < my)
        return -1;
    if (mx > my)
        return +1;
    /* mx == my */

    /* only if |mx| == 1 we have to compare the mantissae */
    switch (mx) {
    case -1:
        return bf_ucmp(y, x);
    case +1:
        return bf_ucmp(x, y);
    default:
        return 0;
    }
}

/* ---------------------------------------------------------------- sqrt.go */

/* Compute √x (to z.prec precision) by solving
 *
 *	1/t² - x = 0
 *
 * for t (using Newton's method), and then inverting. */
static void bf_sqrt_inverse(BigFloat *z, const BigFloat *x) {
    /* let
     *   f(t) = 1/t² - x
     * then
     *   g(t) = f(t)/f'(t) = -½t(1 - xt²)
     * and the next guess is given by
     *   t2 = t - g(t) = ½t(3 - xt²) */
    BigFloat u = BIG_FLOAT(NULL), v = BIG_FLOAT(NULL), three = BIG_FLOAT(NULL);
    bf_set_float64(&three, 3.0);

    BigAccuracy acc;
    double xf = bf_float64(x, &acc);
    BigFloat sqi = BIG_FLOAT(NULL);
    bf_set_float64(&sqi, 1 / math_sqrt(xf));
    for (uint32_t prec = z->prec + 32; sqi.prec < prec;) {
        sqi.prec *= 2;
        /* ng */
        u.prec = sqi.prec;
        v.prec = sqi.prec;
        bf_mul(&u, &sqi, &sqi); /* u = t² */
        bf_mul(&u, x, &u);      /*   = xt² */
        bf_sub(&v, &three, &u); /* v = 3 - xt² */
        bf_mul(&u, &sqi, &v);   /* u = t(3 - xt²) */
        u.exp--;                /*   = ½t(3 - xt²) */
        bf_set(&sqi, &u);
    }
    /* sqi = 1/√x */

    /* x/√x = √x */
    bf_mul(z, x, &sqi);
}

static void bf_sqrt(BigFloat *z, const BigFloat *x) {
    if (z->prec == 0)
        z->prec = x->prec;

    if (bf_sign(x) == -1) {
        /* following IEEE754-2008 (section 7.2) */
        bf_nan(&bf_nan_sqrt);
    }

    /* handle ±0 and +∞ */
    if (x->form != BF_FINITE) {
        z->acc = BIG_EXACT;
        z->form = x->form;
        z->neg = x->neg; /* IEEE754-2008 requires √±0 = ±0 */
        return;
    }

    /* MantExp sets the argument's precision to the receiver's, and
     * when z.prec > x.prec this will lower z.prec. Restore it after
     * the MantExp call. */
    uint32_t prec = z->prec;
    int64_t b = bf_mant_exp(x, z);
    z->prec = prec;

    /* Compute √(z·2**b) as
     *   √( z)·2**(½b)     if b is even
     *   √(2z)·2**(⌊½b⌋)   if b > 0 is odd
     *   √(½z)·2**(⌈½b⌉)   if b < 0 is odd */
    switch (b % 2) {
    case 0:
        /* nothing to do */
        break;
    case 1:
        z->exp++;
        break;
    case -1:
        z->exp--;
        break;
    default:
        break;
    }
    /* 0.25 <= z < 2.0 */

    /* Solving 1/x² - z = 0 avoids Quo calls and is faster, especially
     * for high precisions. */
    bf_sqrt_inverse(z, z);

    /* re-attach halved exponent */
    bf_set_mant_exp(z, z, b / 2);
}

/* ----------------------------------------------------------- floatconv.go */

/* These powers of 5 fit into a uint64.
 *
 *	for p, q := uint64(0), uint64(1); p < q; p, q = q, q*5 {
 *		fmt.Println(q)
 *	} */
static const uint64_t bf_pow5tab[] = {
    1,
    5,
    25,
    125,
    625,
    3125,
    15625,
    78125,
    390625,
    1953125,
    9765625,
    48828125,
    244140625,
    1220703125,
    6103515625,
    30517578125,
    152587890625,
    762939453125,
    3814697265625,
    19073486328125,
    95367431640625,
    476837158203125,
    2384185791015625,
    11920928955078125,
    59604644775390625,
    298023223876953125,
    1490116119384765625,
    7450580596923828125,
};

/* pow5 sets z to 5**n. */
static void bf_pow5(BigFloat *z, uint64_t n) {
    const uint64_t m = sizeof bf_pow5tab / sizeof bf_pow5tab[0] - 1;
    if (n <= m) {
        bf_set_uint64(z, bf_pow5tab[n]);
        return;
    }
    /* n > m */

    bf_set_uint64(z, bf_pow5tab[m]);
    n -= m;

    /* use more bits for f than for z
     * TODO(gri) what is the right number? */
    BigFloat f = BIG_FLOAT(NULL);
    bf_set_prec(&f, (Uint)z->prec + 64);
    bf_set_uint64(&f, 5);

    while (n > 0) {
        if ((n & 1) != 0)
            bf_mul(z, z, &f);
        bf_mul(&f, &f, &f);
        n >>= 1;
    }
}

/* scan is like Parse but reads the longest possible prefix representing a
 * valid floating point number from r rather than a string. It serves as the
 * implementation of Parse. It does not recognize ±Inf and does not expect EOF
 * at the end. It returns whether it set z, the way Go's f result is z or
 * nil. */
static bool bf_scan(BigFloat *z, BigScanner *r, int base, int *b, Error *err) {
    uint32_t prec = z->prec;
    if (prec == 0)
        prec = 64;
    *b = 0;
    *err = BURROW_NO_ERROR;

    /* A reasonable value in case of an error. */
    z->form = BF_ZERO;

    /* sign */
    *err = bi_scan_sign(r, &z->neg);
    if (BURROW_FAILED(*err))
        return false;

    /* mantissa */
    Int fcount; /* fractional digit count; valid if <= 0 */
    z->mant = nat_scan(z->mant, r, base, true, b, &fcount, err);
    if (BURROW_FAILED(*err))
        return false;

    /* exponent */
    int64_t exp;
    int ebase;
    *err = big_scan_exponent(r, true, base == 0, &exp, &ebase);
    if (BURROW_FAILED(*err))
        return false;

    /* special-case 0 */
    if (z->mant.len == 0) {
        z->prec = prec;
        z->acc = BIG_EXACT;
        z->form = BF_ZERO;
        return true;
    }
    /* len(z.mant) > 0 */

    /* The mantissa may have a radix point (fcount <= 0) and there
     * may be a nonzero exponent exp. The radix point amounts to a
     * division by b**(-fcount). An exponent means multiplication by
     * ebase**exp. Finally, mantissa normalization (shift left) requires
     * a correcting multiplication by 2**(-shiftcount). Multiplications
     * are commutative, so we can apply them in any order as long as there
     * is no loss of precision. We only have powers of 2 and 10, and
     * we split powers of 10 into the product of the same powers of
     * 2 and 5. This reduces the size of the multiplication factor
     * needed for base-10 exponents. */

    /* normalize mantissa and determine initial exponent contributions */
    int64_t exp2 = (int64_t)z->mant.len * (int64_t)BIG_W - bf_fnorm(z->mant);
    int64_t exp5 = 0;

    /* determine binary or decimal exponent contribution of radix point */
    if (fcount < 0) {
        /* The mantissa has a radix point ddd.dddd; and
         * -fcount is the number of digits to the right
         * of '.'. Adjust relevant exponent accordingly. */
        int64_t d = (int64_t)fcount;
        switch (*b) {
        case 10:
            exp5 = d;
            exp2 += d; /* 10**e == 5**e * 2**e */
            break;
        case 2:
            exp2 += d;
            break;
        case 8:
            exp2 += d * 3; /* octal digits are 3 bits each */
            break;
        case 16:
            exp2 += d * 4; /* hexadecimal digits are 4 bits each */
            break;
        default:
            panic_str(BURROW_S("unexpected mantissa base"));
        }
        /* fcount consumed - not needed anymore */
    }

    /* take actual exponent into account */
    switch (ebase) {
    case 10:
        exp5 += exp;
        exp2 += exp; /* see fallthrough above */
        break;
    case 2:
        exp2 += exp;
        break;
    default:
        panic_str(BURROW_S("unexpected exponent base"));
    }
    /* exp consumed - not needed anymore */

    /* apply 2**exp2 */
    if (BIG_MIN_EXP <= exp2 && exp2 <= BIG_MAX_EXP) {
        z->prec = prec;
        z->form = BF_FINITE;
        z->exp = (int32_t)exp2;
    } else {
        *err = errors_new(error_allocator(), BURROW_S("exponent overflow"));
        return false;
    }

    if (exp5 == 0) {
        /* no decimal exponent contribution */
        bf_round(z, 0);
        return true;
    }
    /* exp5 != 0 */

    /* apply 5**exp5 */
    BigFloat p = BIG_FLOAT(NULL);
    bf_set_prec(
        &p, (Uint)z->prec +
                64); /* use more bits for p -- TODO(gri) what is the right number? */
    if (exp5 < 0) {
        bf_pow5(&p, (uint64_t)-exp5);
        bf_quo(z, z, &p);
    } else {
        bf_pow5(&p, (uint64_t)exp5);
        bf_mul(z, z, &p);
    }

    return true;
}

static bool bf_parse(BigFloat *z, Str s, int base, int *b, Error *err) {
    *b = 0;
    *err = BURROW_NO_ERROR;
    /* scan doesn't handle ±Inf */
    if (s.len == 3 && (str_eq(s, BURROW_S("Inf")) || str_eq(s, BURROW_S("inf")))) {
        bf_set_inf(z, false);
        return true;
    }
    if (s.len == 4 && (s.p[0] == '+' || s.p[0] == '-')) {
        Str t = str_from_bytes(s.p + 1, 3);
        if (str_eq(t, BURROW_S("Inf")) || str_eq(t, BURROW_S("inf"))) {
            bf_set_inf(z, s.p[0] == '-');
            return true;
        }
    }

    BigScanner r;
    big_scanner_init(&r, s);
    if (!bf_scan(z, &r, base, b, err))
        return false;

    /* entire string must have been consumed */
    Byte ch;
    Error err2 = big_scanner_read(&r, &ch);
    if (BURROW_OK(err2)) {
        *err = fmt_errorf_v("expected end of string, found %q", (Rune)ch);
        return false;
    }
    if (!errors_is(err2, io_eof)) {
        *err = err2;
        return false;
    }
    return true;
}

/* ---------------------------------------------------------- public: Float */

/* A public function with receiver z. */
#define BF_OP(z, ...)                                                                  \
    do {                                                                               \
        big_enter();                                                                   \
        Nat o_ = (z)->mant;                                                            \
        __VA_ARGS__;                                                                   \
        big_commit(&(z)->mant, (z)->a, o_);                                            \
        big_leave();                                                                   \
        return (z);                                                                    \
    } while (0)

void big_float_free(BigFloat *x) {
    if (x == NULL)
        return;
    BigInt t = {x->a, false, x->mant};
    big_int_free(&t);
    x->mant = t.abs;
    x->form = BF_ZERO;
    x->neg = false;
    x->exp = 0;
    x->acc = BIG_EXACT;
}

BigFloat *big_new_float(Alloc *a, double x) {
    if (math_is_nan(x))
        bf_nan(&bf_nan_new_float);
    BigFloat *z = mem_alloc(a != NULL ? a : heap_allocator(), sizeof(BigFloat),
                            _Alignof(BigFloat));
    if (z == NULL)
        big_oom();
    *z = BIG_FLOAT(a);
    return big_float_set_float64(z, x);
}

BigFloat *big_parse_float(Alloc *a, Str s, Int base, Uint prec, BigRoundingMode mode,
                          Int *b, Error *err) {
    BigFloat *z = mem_alloc(a != NULL ? a : heap_allocator(), sizeof(BigFloat),
                            _Alignof(BigFloat));
    if (z == NULL)
        big_oom();
    *z = BIG_FLOAT(a);
    big_float_set_prec(z, prec);
    big_float_set_mode(z, mode);
    return big_float_parse(z, s, base, b, err);
}

BigFloat *big_float_set_prec(BigFloat *z, Uint prec) {
    BF_OP(z, bf_set_prec(z, prec));
}

BigFloat *big_float_set_mode(BigFloat *z, BigRoundingMode mode) {
    z->mode = mode;
    z->acc = BIG_EXACT;
    return z;
}

Uint big_float_prec(const BigFloat *x) {
    return (Uint)x->prec;
}

Uint big_float_min_prec(const BigFloat *x) {
    return bf_min_prec(x);
}

BigRoundingMode big_float_mode(const BigFloat *x) {
    return x->mode;
}

BigAccuracy big_float_acc(const BigFloat *x) {
    return x->acc;
}

Int big_float_sign(const BigFloat *x) {
    return bf_sign(x);
}

Int big_float_mant_exp(const BigFloat *x, BigFloat *mant) {
    if (mant == NULL)
        return (Int)bf_mant_exp(x, NULL);
    big_enter();
    Nat o = mant->mant;
    int64_t e = bf_mant_exp(x, mant);
    big_commit(&mant->mant, mant->a, o);
    big_leave();
    return (Int)e;
}

BigFloat *big_float_set_mant_exp(BigFloat *z, const BigFloat *mant, Int exp) {
    BF_OP(z, bf_set_mant_exp(z, mant, (int64_t)exp));
}

bool big_float_signbit(const BigFloat *x) {
    return x->neg;
}

bool big_float_is_inf(const BigFloat *x) {
    return x->form == BF_INF;
}

bool big_float_is_int(const BigFloat *x) {
    return bf_is_int(x);
}

BigFloat *big_float_set_uint64(BigFloat *z, uint64_t x) {
    BF_OP(z, bf_set_uint64(z, x));
}

BigFloat *big_float_set_int64(BigFloat *z, int64_t x) {
    BF_OP(z, bf_set_int64(z, x));
}

BigFloat *big_float_set_float64(BigFloat *z, double x) {
    if (math_is_nan(x))
        bf_nan(&bf_nan_set_float64);
    BF_OP(z, bf_set_float64(z, x));
}

BigFloat *big_float_set_int(BigFloat *z, const BigInt *x) {
    BF_OP(z, bf_set_int(z, x));
}

BigFloat *big_float_set_rat(BigFloat *z, const BigRat *x) {
    BF_OP(z, bf_set_rat(z, x));
}

BigFloat *big_float_set_inf(BigFloat *z, bool signbit) {
    bf_set_inf(z, signbit);
    return z;
}

BigFloat *big_float_set(BigFloat *z, const BigFloat *x) {
    BF_OP(z, bf_set(z, x));
}

BigFloat *big_float_copy(BigFloat *z, const BigFloat *x) {
    BF_OP(z, bf_copy(z, x));
}

uint64_t big_float_uint64(const BigFloat *x, BigAccuracy *acc) {
    BigAccuracy a;
    uint64_t u = bf_uint64(x, &a);
    if (acc != NULL)
        *acc = a;
    return u;
}

int64_t big_float_int64(const BigFloat *x, BigAccuracy *acc) {
    BigAccuracy a;
    int64_t i = bf_int64(x, &a);
    if (acc != NULL)
        *acc = a;
    return i;
}

float big_float_float32(const BigFloat *x, BigAccuracy *acc) {
    BigAccuracy a;
    big_enter();
    float f = bf_float32(x, &a);
    big_leave();
    if (acc != NULL)
        *acc = a;
    return f;
}

double big_float_float64(const BigFloat *x, BigAccuracy *acc) {
    BigAccuracy a;
    big_enter();
    double f = bf_float64(x, &a);
    big_leave();
    if (acc != NULL)
        *acc = a;
    return f;
}

BigInt *big_float_int(const BigFloat *x, BigInt *z, BigAccuracy *acc) {
    BigAccuracy a;
    big_enter();
    Nat o = z->abs;
    bool ok = bf_int(x, z, &a);
    big_commit(&z->abs, z->a, o);
    big_leave();
    if (acc != NULL)
        *acc = a;
    return ok ? z : NULL;
}

BigRat *big_float_rat(const BigFloat *x, BigRat *z, BigAccuracy *acc) {
    BigAccuracy a;
    big_enter();
    Nat oa = z->a.abs, ob = z->b.abs;
    bool ok = bf_rat(x, z, &a);
    big_commit(&z->a.abs, z->a.a, oa);
    big_commit(&z->b.abs, z->b.a, ob);
    big_leave();
    if (acc != NULL)
        *acc = a;
    return ok ? z : NULL;
}

BigFloat *big_float_abs(BigFloat *z, const BigFloat *x) {
    BF_OP(z, {
        bf_set(z, x);
        z->neg = false;
    });
}

BigFloat *big_float_neg(BigFloat *z, const BigFloat *x) {
    BF_OP(z, bf_neg(z, x));
}

BigFloat *big_float_add(BigFloat *z, const BigFloat *x, const BigFloat *y) {
    BF_OP(z, bf_add(z, x, y));
}

BigFloat *big_float_sub(BigFloat *z, const BigFloat *x, const BigFloat *y) {
    BF_OP(z, bf_sub(z, x, y));
}

BigFloat *big_float_mul(BigFloat *z, const BigFloat *x, const BigFloat *y) {
    BF_OP(z, bf_mul(z, x, y));
}

BigFloat *big_float_quo(BigFloat *z, const BigFloat *x, const BigFloat *y) {
    BF_OP(z, bf_quo(z, x, y));
}

BigFloat *big_float_sqrt(BigFloat *z, const BigFloat *x) {
    BF_OP(z, bf_sqrt(z, x));
}

Int big_float_cmp(const BigFloat *x, const BigFloat *y) {
    return bf_cmp(x, y);
}

BigFloat *big_float_parse(BigFloat *z, Str s, Int base, Int *b, Error *err) {
    int rb;
    Error e;
    big_enter();
    Nat o = z->mant;
    bool ok = bf_parse(z, s, (int)base, &rb, &e);
    big_commit(&z->mant, z->a, o);
    big_leave();
    if (b != NULL)
        *b = rb;
    BURROW_OUT(err, e);
    return ok ? z : NULL;
}

BigFloat *big_float_set_string(BigFloat *z, Str s, bool *ok) {
    Error err;
    BigFloat *f = big_float_parse(z, s, 0, NULL, &err);
    if (ok != NULL)
        *ok = f != NULL;
    return f;
}

Error big_float_scan(BigFloat *z, FmtScanState s, Rune ch) {
    (void)ch;
    s.vt->skip_space(s.data);
    big_enter();
    Nat o = z->mant;
    BigScanner r;
    big_scanner_init_state(&r, &s);
    int b;
    Error err;
    bf_scan(z, &r, 0, &b, &err);
    big_commit(&z->mant, z->a, o);
    big_leave();
    return err;
}

/* --------------------------------------------------------- floatmarsh.go */

/* Gob codec version. Permits backward-compatible changes to the encoding. */
#define BIG_FLOAT_GOB_VERSION 1

static void bf_put32(Byte *p, uint32_t v) {
    p[0] = (Byte)(v >> 24);
    p[1] = (Byte)(v >> 16);
    p[2] = (Byte)(v >> 8);
    p[3] = (Byte)v;
}

static uint32_t bf_get32(const Byte *p) {
    return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 |
           (uint32_t)p[3];
}

Slice big_float_gob_encode(const BigFloat *x, Alloc *a, Error *err) {
    BURROW_OUT(err, BURROW_NO_ERROR);
    if (x == NULL)
        return slice_nil(TYPE_BYTE);

    /* determine max. space (bytes) required for encoding */
    Int sz = 1 + 1 + 4; /* version + mode|acc|form|neg (3+2+2+1bit) + prec */
    Int n = 0;          /* number of mantissa words */
    if (x->form == BF_FINITE) {
        /* add space for mantissa and exponent */
        n = (Int)((x->prec + (BIG_W - 1)) /
                  BIG_W); /* required mantissa length in words for given precision */
        /* actual mantissa slice could be shorter (trailing 0's) or longer (unused bits):
         * - if shorter, only encode the words present
         * - if longer, cut off unused words when encoding in bytes
         *   (in practice, this should never happen since rounding
         *   takes care of it, but be safe and do it always) */
        if (x->mant.len < n)
            n = x->mant.len;
        /* len(x.mant) >= n */
        sz += 4 + n * BIG_S; /* exp + mant */
    }
    Slice buf = slice_make(a, TYPE_BYTE, sz, sz);
    if (buf.p == NULL)
        big_oom();
    Byte *p = buf.p;

    p[0] = BIG_FLOAT_GOB_VERSION;
    Byte b =
        (Byte)((x->mode & 7) << 5 | (Byte)((x->acc + 1) & 3) << 3 | (x->form & 3) << 1);
    if (x->neg)
        b |= 1;
    p[1] = b;
    bf_put32(p + 2, x->prec);

    if (x->form == BF_FINITE) {
        bf_put32(p + 6, (uint32_t)x->exp);
        nat_bytes(nat_from(x->mant, x->mant.len - n),
                  slice_sub(buf, 10, sz)); /* cut off unused trailing words */
    }

    return buf;
}

Error big_float_gob_decode(BigFloat *z, Slice buf) {
    if (buf.len == 0) {
        /* Other side sent a nil or default value. */
        z->prec = 0;
        z->mode = BIG_TO_NEAREST_EVEN;
        z->acc = BIG_EXACT;
        z->form = BF_ZERO;
        z->neg = false;
        z->exp = 0;
        z->mant.len = 0;
        return BURROW_NO_ERROR;
    }
    if (buf.len < 6)
        return errors_new(error_allocator(),
                          BURROW_S("Float.GobDecode: buffer too small"));
    const Byte *p = buf.p;

    if (p[0] != BIG_FLOAT_GOB_VERSION)
        return fmt_errorf_v("Float.GobDecode: encoding version %d not supported",
                            (int)p[0]);

    uint32_t old_prec = z->prec;
    BigRoundingMode old_mode = z->mode;

    Byte b = p[1];
    z->mode = (BigRoundingMode)((b >> 5) & 7);
    z->acc = (BigAccuracy)(((b >> 3) & 3) - 1);
    z->form = (uint8_t)((b >> 1) & 3);
    z->neg = (b & 1) != 0;
    z->prec = bf_get32(p + 2);

    big_enter();
    Nat o = z->mant;
    Error err = BURROW_NO_ERROR;
    if (z->form == BF_FINITE) {
        if (buf.len < 10) {
            err = errors_new(
                error_allocator(),
                BURROW_S("Float.GobDecode: buffer too small for finite form float"));
            goto done;
        }
        z->exp = (int32_t)bf_get32(p + 6);
        z->mant = nat_set_bytes(z->mant, slice_sub(buf, 10, buf.len));
    }

    if (old_prec != 0) {
        z->mode = old_mode;
        bf_set_prec(z, (Uint)old_prec);
    }

    Str msg = bf_validate0(z);
    if (msg.len > 0)
        err = fmt_errorf_v("Float.GobDecode: %s", msg);

done:
    big_commit(&z->mant, z->a, o);
    big_leave();
    return err;
}

Slice big_float_append_text(const BigFloat *x, Alloc *a, Slice b, Error *err) {
    BURROW_OUT(err, BURROW_NO_ERROR);
    if (x == NULL)
        return big_append_str(a, b, BURROW_S("<nil>"));
    return big_float_append(x, a, b, 'g', -1);
}

Slice big_float_marshal_text(const BigFloat *x, Alloc *a, Error *err) {
    return big_float_append_text(x, a, slice_nil(TYPE_BYTE), err);
}

Error big_float_unmarshal_text(BigFloat *z, Slice text) {
    Str s = {text.p, text.len};
    Error err;
    big_float_parse(z, s, 0, NULL, &err);
    if (BURROW_FAILED(err))
        return fmt_errorf_v("math/big: cannot unmarshal %q into a *big.Float (%s)", s,
                            error_text(err));
    return BURROW_NO_ERROR;
}

/* ------------------------------------------------------------- the type */

static Slice big_float_m_append_text(BigFloat *self, Alloc *a, Slice b, Error *err) {
    return big_float_append_text(self, a, b, err);
}

static void big_float_m_format(BigFloat *self, FmtState s, Rune ch) {
    big_float_format(self, s, ch);
}

static Error big_float_m_gob_decode(BigFloat *self, Alloc *a, Slice buf) {
    (void)a;
    return big_float_gob_decode(self, buf);
}

static Slice big_float_m_gob_encode(BigFloat *self, Alloc *a, Error *err) {
    return big_float_gob_encode(self, a, err);
}

static Slice big_float_m_marshal_text(BigFloat *self, Alloc *a, Error *err) {
    return big_float_marshal_text(self, a, err);
}

static Error big_float_m_scan(BigFloat *self, FmtScanState s, Rune ch) {
    return big_float_scan(self, s, ch);
}

static Str big_float_m_string(BigFloat *self) {
    return big_float_string(self, error_allocator());
}

static Error big_float_m_unmarshal_text(BigFloat *self, Alloc *a, Slice text) {
    (void)a;
    return big_float_unmarshal_text(self, text);
}

#define BIG_FLOAT_SIG_FORMAT(IN, OUT) IN(0, FmtState) IN(1, Rune)
#define BIG_FLOAT_SIG_SCAN(IN, OUT) IN(0, FmtScanState) IN(1, Rune) OUT(Error)
#define BIG_FLOAT_SIG_STRING(IN, OUT) OUT(Str)

#define BIG_FLOAT_METHODS(M, T)                                                        \
    M(T, AppendText, big_float_m_append_text, ENCODING_SIG_APPEND_TEXT)                \
    M(T, Format, big_float_m_format, BIG_FLOAT_SIG_FORMAT)                             \
    M(T, GobDecode, big_float_m_gob_decode, ENCODING_SIG_UNMARSHAL_BINARY)             \
    M(T, GobEncode, big_float_m_gob_encode, ENCODING_SIG_MARSHAL_BINARY)               \
    M(T, MarshalText, big_float_m_marshal_text, ENCODING_SIG_MARSHAL_TEXT)             \
    M(T, Scan, big_float_m_scan, BIG_FLOAT_SIG_SCAN)                                   \
    M(T, String, big_float_m_string, BIG_FLOAT_SIG_STRING)                             \
    M(T, UnmarshalText, big_float_m_unmarshal_text, ENCODING_SIG_UNMARSHAL_TEXT)

BURROW_METHODS_DEFINE(BigFloat, BIG_FLOAT_METHODS);

const Type burrow_type_BigFloat = {
    {(const Byte *)"Float", 5},
    {(const Byte *)"math/big", 8},
    KIND_STRUCT,
    (uint32_t)sizeof(BigFloat),
    (uint16_t)_Alignof(BigFloat),
    0,
    (uint16_t)(sizeof burrow__methods_BigFloat / sizeof burrow__methods_BigFloat[0]),
    NULL,
    burrow__methods_BigFloat,
    NULL,
    NULL,
    0,
    0x62696766U, /* "bigf" */
    NULL,
};

const Type *const TYPE_BIG_FLOAT = &burrow_type_BigFloat;
