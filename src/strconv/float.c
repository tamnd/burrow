/* Derived from Go's src/internal/strconv/atof.go, ftoa.go, uscale.go,
 * decimal.go, atoc.go and ctoa.go.
 * Go source: go1.27.1.
 *
 * Copyright 2009 The Go Authors. All rights reserved.
 * Copyright 2026 The burrow Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style licence that can be found
 * in the LICENSE file. */

#include "burrow/strconv.h"

#include "burrow/core.h"
#include "burrow/math/bits.h"
#include "burrow/mem.h"
#include "burrow/panic.h"
#include "burrow/slice.h"
#include "burrow/type.h"

#include <float.h>
#include <stdint.h>
#include <string.h>

/* Go 1.27 converts between binary and decimal floating point by unrounded
 * scaling, from Russ Cox's "Floating-Point Printing and Parsing Can Be Simple
 * And Fast" (https://research.swtch.com/fp). It replaced Ryu and Eisel-Lemire.
 * Every conversion multiplies a 64 bit mantissa by a 128 bit power of ten and
 * keeps two extra bits and a sticky bit, which is enough to round correctly in
 * all but the cases the old multiprecision decimal code still handles: parsing
 * input with more than 19 significant digits that the fast path cannot decide,
 * and printing more than 18 digits.
 *
 * All of it is integer arithmetic except the exact fast paths in parsing,
 * which multiply two doubles that are both exact. Those need the product to be
 * rounded once, to double, and a compiler that evaluates in x87 extended
 * precision rounds it twice. There the fast paths are skipped, and the integer
 * path, which is always right, gives the same answer a little slower. */

#if defined(FLT_EVAL_METHOD) && FLT_EVAL_METHOD == 0
#define FLOAT_EXACT_PATHS 1
#else
#define FLOAT_EXACT_PATHS 0
#endif

enum {
    F32_MANT_BITS = 23,
    F32_EXP_BITS = 8,
    F32_BIAS = -127,
    F32_MIN_EXP = -189,

    F64_MANT_BITS = 52,
    F64_EXP_BITS = 11,
    F64_BIAS = -1023,
    F64_MIN_EXP = -1085,
};

typedef struct FloatInfo {
    int mant_bits;
    int exp_bits;
    int bias;
} FloatInfo;

static const FloatInfo float32_info = {F32_MANT_BITS, F32_EXP_BITS, F32_BIAS};
static const FloatInfo float64_info = {F64_MANT_BITS, F64_EXP_BITS, F64_BIAS};

/* How a parse went, which is internal/strconv's error. */
typedef enum FloatCode {
    FLOAT_OK,
    FLOAT_RANGE,
    FLOAT_SYNTAX,
} FloatCode;

static double f64_from_bits(uint64_t b) {
    double f;
    memcpy(&f, &b, sizeof f);
    return f;
}

static float f32_from_bits(uint32_t b) {
    float f;
    memcpy(&f, &b, sizeof f);
    return f;
}

static uint64_t f64_bits(double f) {
    uint64_t b;
    memcpy(&b, &f, sizeof b);
    return b;
}

static uint32_t f32_bits(float f) {
    uint32_t b;
    memcpy(&b, &f, sizeof b);
    return b;
}

static Byte float_lower(Byte c) {
    return (Byte)(c | ('x' - 'X'));
}

/* x >> s rounding toward minus infinity, which is what Go's >> does to a
 * negative int. C leaves that shift to the implementation. */
static int floor_shift(int64_t x, int s) {
    return (int)(x >= 0 ? x >> s : -((-x - 1) >> s) - 1);
}

/* ------------------------------------------------------ unrounded scaling */

/* floor(log10(2^x)) and floor(log2(10^x)), close enough over the range used. */
static int log10_pow2(int x) {
    return floor_shift((int64_t)x * 78913, 18);
}

static int log2_pow10(int x) {
    return floor_shift((int64_t)x * 108853, 15);
}

/* floor(log10(3/4 * 2^e)), the footprint of a power of two whose lower
 * neighbour is only half as far away. */
static int skewed(int e) {
    return floor_shift((int64_t)e * 631305 - 261663, 21);
}

static const uint64_t pow10_u64[20] = {
    1ULL,
    10ULL,
    100ULL,
    1000ULL,
    10000ULL,
    100000ULL,
    1000000ULL,
    10000000ULL,
    100000000ULL,
    1000000000ULL,
    10000000000ULL,
    100000000000ULL,
    1000000000000ULL,
    10000000000000ULL,
    100000000000000ULL,
    1000000000000000ULL,
    10000000000000000ULL,
    100000000000000000ULL,
    1000000000000000000ULL,
    10000000000000000000ULL,
};

/* A value with two extra bits below the point, a half bit and a sticky bit
 * that says whether anything nonzero was lost below that. */
typedef uint64_t Unrounded;

static uint64_t un_floor(Unrounded u) {
    return u >> 2;
}

static uint64_t un_round(Unrounded u) {
    return (u + 1 + ((u >> 2) & 1)) >> 2;
}

static uint64_t un_ceil(Unrounded u) {
    return (u + 3) >> 2;
}

static Unrounded un_div(Unrounded u, uint64_t d) {
    return (u / d) | (u & 1) | (Unrounded)(u % d != 0);
}

/* The smallest unrounded value that rounds to x. */
static Unrounded un_min(uint64_t x) {
    return (x << 2) - 2;
}

/* The constants for scaling by 2^e * 10^p. */
typedef struct Scaler {
    uint64_t hi;
    uint64_t lo;
    int s;
} Scaler;

static void prescale(Scaler *pre, int e, int p, int lp) {
    pre->hi = burrow__strconv_pow10[p - BURROW__STRCONV_POW10_MIN][0];
    pre->lo = burrow__strconv_pow10[p - BURROW__STRCONV_POW10_MIN][1];
    pre->s = -(e + lp + 3);
}

/* x * 2^e * 10^p, unrounded, for x with its top bit set. The low half of the
 * power of ten is only needed when the high half leaves the answer in doubt. */
static Unrounded uscale(uint64_t x, const Scaler *c) {
    uint64_t mid;
    uint64_t hi = bits_mul64(x, c->hi, &mid);
    unsigned s = (unsigned)c->s & 63;
    if (hi >> s << s != hi)
        return (hi >> s) | 1;
    uint64_t mid2 = bits_mul64(x, c->lo, NULL);
    hi -= (uint64_t)(mid < mid2);
    return (hi >> s) | (uint64_t)(mid - mid2 > 1);
}

static Int float_num_digits(uint64_t d) {
    Int nd = log10_pow2((int)bits_len64(d));
    return nd + (d >= pow10_u64[nd]);
}

/* The n digit decimal form of m * 2^e, as d * 10^-p. n is at most 18. For 'f',
 * n is an upper bound and digits past prec are dropped. */
static uint64_t fixed_width_float(uint64_t m, int e, int n, int64_t prec, Byte fmt,
                                  int *pout) {
    int p = n - 1 - log10_pow2(e + 63);
    Scaler pre;
    prescale(&pre, e, p, log2_pow10(p));
    Unrounded u = uscale(m, &pre);
    if (u >= un_min(pow10_u64[n])) {
        u = un_div(u, 10);
        p--;
    }
    if (fmt == 'f') {
        while (p > prec) {
            u = un_div(u, 10);
            p--;
        }
    }
    *pout = -p;
    return un_round(u);
}

/* The shortest d * 10^p that reads back as m * 2^e. */
static uint64_t short_float(uint64_t m, int e, int mant_bits, int min_exp, int *pout) {
    uint64_t lo, hi;
    int p;
    int z = 63 - mant_bits;
    if (m == 1ULL << 63 && e > min_exp) {
        p = -skewed(e + z);
        lo = m - (1ULL << (z - 2));
        hi = m + (1ULL << (z - 1));
    } else if (e >= min_exp) {
        p = -log10_pow2(e + z);
        lo = m - (1ULL << (z - 1));
        hi = m + (1ULL << (z - 1));
    } else {
        z = z + (min_exp - e);
        p = -log10_pow2(e + z);
        lo = m - (1ULL << (z - 1));
        hi = m + (1ULL << (z - 1));
    }
    uint64_t odd = (m >> z) & 1;

    Scaler pre;
    prescale(&pre, e, p, log2_pow10(p));
    uint64_t dmin = un_ceil(uscale(lo, &pre) + odd);
    uint64_t dmax = un_floor(uscale(hi, &pre) - odd);

    uint64_t d = dmax / 10;
    if (d * 10 >= dmin) {
        *pout = -(p - 1);
        return d;
    }
    d = dmin;
    if (d < dmax)
        d = un_round(uscale(m, &pre));
    *pout = -p;
    return d;
}

/* The nd digits of d * 10^p into s. Returns the digits left once trailing
 * zeros are dropped, and the decimal point through dp. */
static Int set_digits(Byte *s, uint64_t d, int p, Int nd, Int *dp) {
    burrow__strconv_format_base10(s, nd, d);
    *dp = nd + p;
    while (nd > 0 && s[nd - 1] == '0')
        nd--;
    return nd;
}

static double pack64(uint64_t m, int e, bool *range) {
    if ((m & (1ULL << 52)) == 0)
        return f64_from_bits(m);
    if (e >= 0x7FF - 1075) {
        *range = true;
        return f64_from_bits((m & (1ULL << 63)) | (0x7FFULL << 52));
    }
    return f64_from_bits((m & ~(1ULL << 52)) | ((uint64_t)(1075 + e) << 52));
}

static float pack32(uint32_t m, int e, bool *range) {
    if ((m & (1U << 23)) == 0)
        return f32_from_bits(m);
    if (e >= 0xFF - 150) {
        *range = true;
        return f32_from_bits((m & (1U << 31)) | (0xFFU << 23));
    }
    return f32_from_bits((m & ~(1U << 23)) | ((uint32_t)(150 + e) << 23));
}

/* d * 10^p rounded to the nearest double, for d of at most 19 digits. */
static double parse_float64(uint64_t d, int p, uint64_t sign, bool *range) {
    int b = (int)bits_len64(d);
    int lp = log2_pow10(p);
    int e = 53 - b - lp;
    if (e > 1074)
        e = 1074;
    Scaler pre;
    prescale(&pre, e - (64 - b), p, lp);
    if (pre.s >= 64)
        return f64_from_bits(sign);
    Unrounded u = uscale(d << (64 - b), &pre);

    /* Branch free for: if it rounds to 2^53, shift one more place. */
    int s = u >= un_min(1ULL << 53);
    u = (u >> s) | (u & 1);
    e = e - s;

    return pack64(sign | un_round(u), -e, range);
}

static float parse_float32(uint64_t d, int p, uint32_t sign, bool *range) {
    int b = (int)bits_len64(d);
    int lp = log2_pow10(p);
    int e = 24 - b - lp;
    if (e > 149)
        e = 149;
    Scaler pre;
    prescale(&pre, e - (64 - b), p, lp);
    if (pre.s >= 64)
        return f32_from_bits(sign);
    Unrounded u = uscale(d << (64 - b), &pre);

    int s = u >= un_min(1ULL << 24);
    u = (u >> s) | (u & 1);
    e = e - s;

    return pack32(sign | (uint32_t)un_round(u), -e, range);
}

/* ------------------------------------------------ multiprecision decimal */

/* The slow path, for the few conversions scaling cannot settle. It holds up to
 * 800 digits, which is enough for any double exactly. */
typedef struct Decimal {
    Byte d[800];
    Int nd;
    Int dp;
    bool neg;
    bool trunc;
} Decimal;

#define DECIMAL_CAP ((Int)sizeof(((Decimal *)0)->d))

static void dec_trim(Decimal *a) {
    while (a->nd > 0 && a->d[a->nd - 1] == '0')
        a->nd--;
    if (a->nd == 0)
        a->dp = 0;
}

static void dec_assign(Decimal *a, uint64_t v) {
    Byte buf[24];
    Int n = 0;
    while (v > 0) {
        uint64_t v1 = v / 10;
        v -= 10 * v1;
        buf[n++] = (Byte)(v + '0');
        v = v1;
    }
    a->nd = 0;
    for (n--; n >= 0; n--)
        a->d[a->nd++] = buf[n];
    a->dp = a->nd;
    dec_trim(a);
}

/* The most a shift can move in one pass without overflowing a 64 bit word,
 * which has to hold 9 << k. */
#define DECIMAL_MAX_SHIFT 60

static void dec_right_shift(Decimal *a, unsigned k) {
    Int r = 0;
    Int w = 0;

    uint64_t n = 0;
    for (; n >> k == 0; r++) {
        if (r >= a->nd) {
            if (n == 0) {
                a->nd = 0;
                return;
            }
            while (n >> k == 0) {
                n = n * 10;
                r++;
            }
            break;
        }
        uint64_t c = a->d[r];
        n = n * 10 + c - '0';
    }
    a->dp -= r - 1;

    uint64_t mask = (1ULL << k) - 1;

    for (; r < a->nd; r++) {
        uint64_t c = a->d[r];
        uint64_t dig = n >> k;
        n &= mask;
        a->d[w++] = (Byte)(dig + '0');
        n = n * 10 + c - '0';
    }

    while (n > 0) {
        uint64_t dig = n >> k;
        n &= mask;
        if (w < DECIMAL_CAP)
            a->d[w++] = (Byte)(dig + '0');
        else if (dig > 0)
            a->trunc = true;
        n = n * 10;
    }

    a->nd = w;
    dec_trim(a);
}

/* For a left shift by k, the digits it adds, one fewer when the number starts
 * below the cutoff, which is the leading digits of 5^k. */
typedef struct LeftCheat {
    Int delta;
    const char *cutoff;
} LeftCheat;

static const LeftCheat left_cheats[] = {
    {0, ""},
    {1, "5"},
    {1, "25"},
    {1, "125"},
    {2, "625"},
    {2, "3125"},
    {2, "15625"},
    {3, "78125"},
    {3, "390625"},
    {3, "1953125"},
    {4, "9765625"},
    {4, "48828125"},
    {4, "244140625"},
    {4, "1220703125"},
    {5, "6103515625"},
    {5, "30517578125"},
    {5, "152587890625"},
    {6, "762939453125"},
    {6, "3814697265625"},
    {6, "19073486328125"},
    {7, "95367431640625"},
    {7, "476837158203125"},
    {7, "2384185791015625"},
    {7, "11920928955078125"},
    {8, "59604644775390625"},
    {8, "298023223876953125"},
    {8, "1490116119384765625"},
    {9, "7450580596923828125"},
    {9, "37252902984619140625"},
    {9, "186264514923095703125"},
    {10, "931322574615478515625"},
    {10, "4656612873077392578125"},
    {10, "23283064365386962890625"},
    {10, "116415321826934814453125"},
    {11, "582076609134674072265625"},
    {11, "2910383045673370361328125"},
    {11, "14551915228366851806640625"},
    {12, "72759576141834259033203125"},
    {12, "363797880709171295166015625"},
    {12, "1818989403545856475830078125"},
    {13, "9094947017729282379150390625"},
    {13, "45474735088646411895751953125"},
    {13, "227373675443232059478759765625"},
    {13, "1136868377216160297393798828125"},
    {14, "5684341886080801486968994140625"},
    {14, "28421709430404007434844970703125"},
    {14, "142108547152020037174224853515625"},
    {15, "710542735760100185871124267578125"},
    {15, "3552713678800500929355621337890625"},
    {15, "17763568394002504646778106689453125"},
    {16, "88817841970012523233890533447265625"},
    {16, "444089209850062616169452667236328125"},
    {16, "2220446049250313080847263336181640625"},
    {16, "11102230246251565404236316680908203125"},
    {17, "55511151231257827021181583404541015625"},
    {17, "277555756156289135105907917022705078125"},
    {17, "1387778780781445675529539585113525390625"},
    {18, "6938893903907228377647697925567626953125"},
    {18, "34694469519536141888238489627838134765625"},
    {18, "173472347597680709441192448139190673828125"},
    {19, "867361737988403547205962240695953369140625"},
};

static bool prefix_is_less_than(const Byte *b, Int n, const char *s) {
    for (Int i = 0; s[i] != 0; i++) {
        if (i >= n)
            return true;
        if (b[i] != (Byte)s[i])
            return b[i] < (Byte)s[i];
    }
    return false;
}

static void dec_left_shift(Decimal *a, unsigned k) {
    Int delta = left_cheats[k].delta;
    if (prefix_is_less_than(a->d, a->nd, left_cheats[k].cutoff))
        delta--;

    Int r = a->nd;
    Int w = a->nd + delta;

    uint64_t n = 0;
    for (r--; r >= 0; r--) {
        n += ((uint64_t)a->d[r] - '0') << k;
        uint64_t quo = n / 10;
        uint64_t rem = n - 10 * quo;
        w--;
        if (w < DECIMAL_CAP)
            a->d[w] = (Byte)(rem + '0');
        else if (rem != 0)
            a->trunc = true;
        n = quo;
    }

    while (n > 0) {
        uint64_t quo = n / 10;
        uint64_t rem = n - 10 * quo;
        w--;
        if (w < DECIMAL_CAP)
            a->d[w] = (Byte)(rem + '0');
        else if (rem != 0)
            a->trunc = true;
        n = quo;
    }

    a->nd += delta;
    if (a->nd >= DECIMAL_CAP)
        a->nd = DECIMAL_CAP;
    a->dp += delta;
    dec_trim(a);
}

/* Multiply by 2^k, or divide for a negative k. */
static void dec_shift(Decimal *a, Int k) {
    if (a->nd == 0)
        return;
    if (k > 0) {
        while (k > DECIMAL_MAX_SHIFT) {
            dec_left_shift(a, DECIMAL_MAX_SHIFT);
            k -= DECIMAL_MAX_SHIFT;
        }
        dec_left_shift(a, (unsigned)k);
    } else if (k < 0) {
        while (k < -DECIMAL_MAX_SHIFT) {
            dec_right_shift(a, DECIMAL_MAX_SHIFT);
            k += DECIMAL_MAX_SHIFT;
        }
        dec_right_shift(a, (unsigned)-k);
    }
}

static bool dec_should_round_up(const Decimal *a, int64_t nd) {
    if (nd < 0 || nd >= a->nd)
        return false;
    if (a->d[nd] == '5' && nd + 1 == a->nd) {
        /* Exactly halfway, so to even, unless digits were dropped, in which
         * case it is more than halfway. */
        if (a->trunc)
            return true;
        return nd > 0 && (a->d[nd - 1] - '0') % 2 != 0;
    }
    return a->d[nd] >= '5';
}

static void dec_round(Decimal *a, int64_t nd) {
    if (nd < 0 || nd >= a->nd)
        return;
    if (dec_should_round_up(a, nd)) {
        for (int64_t i = nd - 1; i >= 0; i--) {
            if (a->d[i] < '9') {
                a->d[i]++;
                a->nd = (Int)(i + 1);
                return;
            }
        }
        /* All nines, so it is a one with the point moved. */
        a->d[0] = '1';
        a->nd = 1;
        a->dp++;
    } else {
        a->nd = (Int)nd;
        dec_trim(a);
    }
}

static uint64_t dec_rounded_integer(const Decimal *a) {
    if (a->dp > 20)
        return 0xFFFFFFFFFFFFFFFFULL;
    Int i;
    uint64_t n = 0;
    for (i = 0; i < a->dp && i < a->nd; i++)
        n = n * 10 + (uint64_t)(a->d[i] - '0');
    for (; i < a->dp; i++)
        n *= 10;
    if (dec_should_round_up(a, a->dp))
        n++;
    return n;
}

/* Reads s, which read_float has already accepted, into b. */
static bool dec_set(Decimal *b, Str s) {
    Int i = 0;
    b->nd = 0;
    b->dp = 0;
    b->neg = false;
    b->trunc = false;

    if (i >= s.len)
        return false;
    if (s.p[i] == '+') {
        i++;
    } else if (s.p[i] == '-') {
        i++;
        b->neg = true;
    }

    bool sawdot = false;
    bool sawdigits = false;
    for (; i < s.len; i++) {
        Byte c = s.p[i];
        if (c == '_')
            continue;
        if (c == '.') {
            if (sawdot)
                return false;
            sawdot = true;
            b->dp = b->nd;
            continue;
        }
        if ('0' <= c && c <= '9') {
            sawdigits = true;
            if (c == '0' && b->nd == 0) {
                b->dp--;
                continue;
            }
            if (b->nd < DECIMAL_CAP)
                b->d[b->nd++] = c;
            else if (c != '0')
                b->trunc = true;
            continue;
        }
        break;
    }
    if (!sawdigits)
        return false;
    if (!sawdot)
        b->dp = b->nd;

    if (i < s.len && float_lower(s.p[i]) == 'e') {
        i++;
        if (i >= s.len)
            return false;
        Int esign = 1;
        if (s.p[i] == '+') {
            i++;
        } else if (s.p[i] == '-') {
            i++;
            esign = -1;
        }
        if (i >= s.len || s.p[i] < '0' || s.p[i] > '9')
            return false;
        Int e = 0;
        for (; i < s.len && (('0' <= s.p[i] && s.p[i] <= '9') || s.p[i] == '_'); i++) {
            if (s.p[i] == '_')
                continue;
            if (e < 10000)
                e = e * 10 + s.p[i] - '0';
        }
        b->dp += e * esign;
    }

    return i == s.len;
}

/* Decimal power of ten to binary power of two, for the shifts in float_bits. */
static const Int powtab[] = {1, 3, 6, 9, 13, 16, 19, 23, 26};

#define POWTAB_LEN ((Int)(sizeof powtab / sizeof powtab[0]))

static uint64_t dec_float_bits(Decimal *d, const FloatInfo *flt, bool *overflow) {
    Int exp;
    uint64_t mant;

    if (d->nd == 0) {
        mant = 0;
        exp = flt->bias;
        goto out;
    }

    /* Obvious overflow and underflow, with bounds for 64 bit floats. */
    if (d->dp > 310)
        goto overflow;
    if (d->dp < -330) {
        mant = 0;
        exp = flt->bias;
        goto out;
    }

    /* Scale by powers of two into [0.5, 1). */
    exp = 0;
    while (d->dp > 0) {
        Int n = d->dp >= POWTAB_LEN ? 27 : powtab[d->dp];
        dec_shift(d, -n);
        exp += n;
    }
    while (d->dp < 0 || (d->dp == 0 && d->d[0] < '5')) {
        Int n = -d->dp >= POWTAB_LEN ? 27 : powtab[-d->dp];
        dec_shift(d, n);
        exp -= n;
    }

    /* The range is [0.5, 1) and floating point's is [1, 2). */
    exp--;

    /* Below the smallest exponent, denormalise. */
    if (exp < flt->bias + 1) {
        Int n = flt->bias + 1 - exp;
        dec_shift(d, -n);
        exp += n;
    }

    if (exp - flt->bias >= (1 << flt->exp_bits) - 1)
        goto overflow;

    dec_shift(d, 1 + flt->mant_bits);
    mant = dec_rounded_integer(d);

    /* Rounding may have carried into a new bit. */
    if (mant == 2ULL << flt->mant_bits) {
        mant >>= 1;
        exp++;
        if (exp - flt->bias >= (1 << flt->exp_bits) - 1)
            goto overflow;
    }

    if ((mant & (1ULL << flt->mant_bits)) == 0)
        exp = flt->bias;
    goto out;

overflow:
    mant = 0;
    exp = (1 << flt->exp_bits) - 1 + flt->bias;
    *overflow = true;

out:;
    uint64_t bits = mant & ((1ULL << flt->mant_bits) - 1);
    bits |= (uint64_t)((exp - flt->bias) & ((1 << flt->exp_bits) - 1)) << flt->mant_bits;
    if (d->neg)
        bits |= 1ULL << flt->mant_bits << flt->exp_bits;
    return bits;
}

/* ----------------------------------------------------------------- parsing */

static Int common_prefix_len_ignore_case(Str s, const char *prefix) {
    Int n = (Int)strlen(prefix);
    if (s.len < n)
        n = s.len;
    for (Int i = 0; i < n; i++) {
        Byte c = s.p[i];
        if ('A' <= c && c <= 'Z')
            c = (Byte)(c + 'a' - 'A');
        if (c != (Byte)prefix[i])
            return i;
    }
    return n;
}

/* inf, infinity and nan in any case, the first two signed or not, at the start
 * of s. */
static bool special(Str s, double *f, Int *n) {
    if (s.len == 0)
        return false;
    bool neg = false;
    Int nsign = 0;
    switch (s.p[0]) {
    case '+':
    case '-':
        neg = s.p[0] == '-';
        nsign = 1;
        s.p++;
        s.len--;
        /* fall through */
    case 'i':
    case 'I': {
        Int k = common_prefix_len_ignore_case(s, "infinity");
        /* Anything longer than "inf" is fine, but without all of "infinity"
         * only the "inf" is taken. */
        if (3 < k && k < 8)
            k = 3;
        if (k == 3 || k == 8) {
            *f = f64_from_bits(neg ? 0xFFF0000000000000ULL : 0x7FF0000000000000ULL);
            *n = nsign + k;
            return true;
        }
        break;
    }
    case 'n':
    case 'N':
        if (common_prefix_len_ignore_case(s, "nan") == 3) {
            *f = f64_from_bits(0x7FF8000000000001ULL);
            *n = 3;
            return true;
        }
        break;
    default:
        break;
    }
    return false;
}

typedef struct FloatRead {
    uint64_t mantissa;
    Int exp;
    bool neg;
    bool trunc;
    bool hex;
    Int n;
} FloatRead;

/* The mantissa and exponent of the decimal or hexadecimal number at the start
 * of s. r->n is how much of s it took, which it sets even when s is not a
 * number, since that is what ParseComplex goes on from. */
static bool read_float(Str s, FloatRead *r) {
    Int i = 0;
    bool underscores = false;
    memset(r, 0, sizeof *r);

    if (i >= s.len)
        goto fail;
    if (s.p[i] == '+') {
        i++;
    } else if (s.p[i] == '-') {
        i++;
        r->neg = true;
    }

    uint64_t base = 10;
    Int max_mant_digits = 19; /* 10^19 fits in 64 bits */
    Byte exp_char = 'e';
    if (i + 2 < s.len && s.p[i] == '0' && float_lower(s.p[i + 1]) == 'x') {
        base = 16;
        max_mant_digits = 16; /* 16^16 does too */
        i += 2;
        exp_char = 'p';
        r->hex = true;
    }

    bool sawdot = false;
    bool sawdigits = false;
    Int nd = 0;
    Int nd_mant = 0;
    Int dp = 0;
    for (; i < s.len; i++) {
        Byte c = s.p[i];
        if (c == '_') {
            underscores = true;
            continue;
        }
        if (c == '.') {
            if (sawdot)
                break;
            sawdot = true;
            dp = nd;
            continue;
        }
        if ('0' <= c && c <= '9') {
            sawdigits = true;
            if (c == '0' && nd == 0) {
                dp--;
                continue;
            }
            nd++;
            if (nd_mant < max_mant_digits) {
                r->mantissa = r->mantissa * base + (uint64_t)(c - '0');
                nd_mant++;
            } else if (c != '0') {
                r->trunc = true;
            }
            continue;
        }
        if (base == 16 && 'a' <= float_lower(c) && float_lower(c) <= 'f') {
            sawdigits = true;
            nd++;
            if (nd_mant < max_mant_digits) {
                r->mantissa = r->mantissa * 16 + (uint64_t)(float_lower(c) - 'a' + 10);
                nd_mant++;
            } else {
                r->trunc = true;
            }
            continue;
        }
        break;
    }
    if (!sawdigits)
        goto fail;
    if (!sawdot)
        dp = nd;

    if (base == 16) {
        dp *= 4;
        nd_mant *= 4;
    }

    if (i < s.len && float_lower(s.p[i]) == exp_char) {
        i++;
        if (i >= s.len)
            goto fail;
        Int esign = 1;
        if (s.p[i] == '+') {
            i++;
        } else if (s.p[i] == '-') {
            i++;
            esign = -1;
        }
        if (i >= s.len || s.p[i] < '0' || s.p[i] > '9')
            goto fail;
        Int e = 0;
        for (; i < s.len && (('0' <= s.p[i] && s.p[i] <= '9') || s.p[i] == '_'); i++) {
            if (s.p[i] == '_') {
                underscores = true;
                continue;
            }
            if (e < 10000)
                e = e * 10 + s.p[i] - '0';
        }
        dp += e * esign;
    } else if (base == 16) {
        /* A hex float has to have its p exponent. */
        goto fail;
    }

    if (r->mantissa != 0)
        r->exp = dp - nd_mant;

    if (underscores && !burrow__strconv_underscore_ok(str_from_bytes(s.p, i)))
        goto fail;

    r->n = i;
    return true;

fail:
    r->n = i;
    return false;
}

#if FLOAT_EXACT_PATHS
static const double float64_pow10[] = {
    1e0,  1e1,  1e2,  1e3,  1e4,  1e5,  1e6,  1e7,  1e8,  1e9,  1e10, 1e11,
    1e12, 1e13, 1e14, 1e15, 1e16, 1e17, 1e18, 1e19, 1e20, 1e21, 1e22,
};

static const float float32_pow10[] = {1e0f, 1e1f, 1e2f, 1e3f, 1e4f, 1e5f,
                                      1e6f, 1e7f, 1e8f, 1e9f, 1e10f};

/* mantissa * 10^exp in one correctly rounded operation, when both factors are
 * exact: an integer, an integer times an exact power of ten, or one divided by
 * an exact power of ten. */
static bool atof64_exact(uint64_t mantissa, Int exp, bool neg, double *out) {
    if (mantissa >> F64_MANT_BITS != 0)
        return false;
    double f = (double)mantissa;
    if (neg)
        f = -f;
    if (exp == 0) {
        *out = f;
        return true;
    }
    if (exp > 0 && exp <= 15 + 22) {
        /* A big exponent on a short number can move some zeros into it. */
        if (exp > 22) {
            f *= float64_pow10[exp - 22];
            exp = 22;
        }
        if (f > 1e15 || f < -1e15)
            return false;
        *out = f * float64_pow10[exp];
        return true;
    }
    if (exp < 0 && exp >= -22) {
        *out = f / float64_pow10[-exp];
        return true;
    }
    return false;
}

static bool atof32_exact(uint64_t mantissa, Int exp, bool neg, float *out) {
    if (mantissa >> F32_MANT_BITS != 0)
        return false;
    float f = (float)mantissa;
    if (neg)
        f = -f;
    if (exp == 0) {
        *out = f;
        return true;
    }
    if (exp > 0 && exp <= 7 + 10) {
        if (exp > 10) {
            f *= float32_pow10[exp - 10];
            exp = 10;
        }
        if (f > 1e7f || f < -1e7f)
            return false;
        *out = f * float32_pow10[exp];
        return true;
    }
    if (exp < 0 && exp >= -10) {
        *out = f / float32_pow10[-exp];
        return true;
    }
    return false;
}
#endif

/* A hex float, already read into mantissa and exp, rounded to flt. Only a
 * mantissa with more bits than fit rounds at all. */
static double atof_hex(const FloatInfo *flt, uint64_t mantissa, Int exp, bool neg, bool trunc,
                       bool *range) {
    Int max_exp = (1 << flt->exp_bits) + flt->bias - 2;
    Int min_exp = flt->bias + 1;
    exp += flt->mant_bits;

    /* A leading one and mant_bits more, then two rounding bits, the lower of
     * them sticky. */
    while (mantissa != 0 && mantissa >> (flt->mant_bits + 2) == 0) {
        mantissa <<= 1;
        exp--;
    }
    if (trunc)
        mantissa |= 1;
    while (mantissa >> (1 + flt->mant_bits + 2) != 0) {
        mantissa = (mantissa >> 1) | (mantissa & 1);
        exp++;
    }

    /* Too small, so denormalise and hope. */
    while (mantissa > 1 && exp < min_exp - 2) {
        mantissa = (mantissa >> 1) | (mantissa & 1);
        exp++;
    }

    /* Round on the two low bits, to even. */
    uint64_t round = mantissa & 3;
    mantissa >>= 2;
    round |= mantissa & 1;
    exp += 2;
    if (round == 3) {
        mantissa++;
        if (mantissa == 1ULL << (1 + flt->mant_bits)) {
            mantissa >>= 1;
            exp++;
        }
    }

    if (mantissa >> flt->mant_bits == 0)
        exp = flt->bias;
    if (exp > max_exp) {
        mantissa = 1ULL << flt->mant_bits;
        exp = max_exp + 1;
        *range = true;
    }

    uint64_t bits = mantissa & ((1ULL << flt->mant_bits) - 1);
    bits |= (uint64_t)((exp - flt->bias) & ((1 << flt->exp_bits) - 1)) << flt->mant_bits;
    if (neg)
        bits |= 1ULL << flt->mant_bits << flt->exp_bits;
    if (flt == &float32_info)
        return (double)f32_from_bits((uint32_t)bits);
    return f64_from_bits(bits);
}

static double atof32(Str s, Int *n, FloatCode *code) {
    double special_value;
    *code = FLOAT_OK;
    if (special(s, &special_value, n))
        return (double)(float)special_value;

    FloatRead r;
    bool ok = read_float(s, &r);
    *n = r.n;
    if (!ok) {
        *code = FLOAT_SYNTAX;
        return 0;
    }

    bool range = false;
    if (r.hex) {
        double f = atof_hex(&float32_info, r.mantissa, r.exp, r.neg, r.trunc, &range);
        *code = range ? FLOAT_RANGE : FLOAT_OK;
        return f;
    }

    uint32_t sign = (uint32_t)r.neg << 31;
    if (r.mantissa == 0)
        return (double)f32_from_bits(sign);
    if (r.exp > 40) {
        *code = FLOAT_RANGE;
        return (double)f32_from_bits(sign | (0xFFU << 23));
    }
    if (r.exp < -70)
        return (double)f32_from_bits(sign);
#if FLOAT_EXACT_PATHS
    float exact;
    if (!r.trunc && atof32_exact(r.mantissa, r.exp, r.neg, &exact))
        return (double)exact;
#endif
    float f = parse_float32(r.mantissa, (int)r.exp, sign, &range);
    if (!r.trunc || f32_bits(f) == f32_bits(parse_float32(r.mantissa + 1, (int)r.exp, sign,
                                                          &(bool){false}))) {
        *code = range ? FLOAT_RANGE : FLOAT_OK;
        return (double)f;
    }

    Decimal dec;
    if (!dec_set(&dec, str_from_bytes(s.p, *n))) {
        *code = FLOAT_SYNTAX;
        return 0;
    }
    bool overflow = false;
    uint64_t b = dec_float_bits(&dec, &float32_info, &overflow);
    *code = overflow ? FLOAT_RANGE : FLOAT_OK;
    return (double)f32_from_bits((uint32_t)b);
}

static double atof64(Str s, Int *n, FloatCode *code) {
    double special_value;
    *code = FLOAT_OK;
    if (special(s, &special_value, n))
        return special_value;

    FloatRead r;
    bool ok = read_float(s, &r);
    *n = r.n;
    if (!ok) {
        *code = FLOAT_SYNTAX;
        return 0;
    }

    bool range = false;
    if (r.hex) {
        double f = atof_hex(&float64_info, r.mantissa, r.exp, r.neg, r.trunc, &range);
        *code = range ? FLOAT_RANGE : FLOAT_OK;
        return f;
    }

    uint64_t sign = (uint64_t)r.neg << 63;
    if (r.mantissa == 0)
        return f64_from_bits(sign);
    if (r.exp > 310) {
        *code = FLOAT_RANGE;
        return f64_from_bits(sign | (0x7FFULL << 52));
    }
    if (r.exp < -345)
        return f64_from_bits(sign);
#if FLOAT_EXACT_PATHS
    double exact;
    if (!r.trunc && atof64_exact(r.mantissa, r.exp, r.neg, &exact))
        return exact;
#endif
    double f = parse_float64(r.mantissa, (int)r.exp, sign, &range);
    /* With digits dropped from the mantissa, the answer stands if one more
     * in the last kept place gives the same float. */
    if (!r.trunc || f64_bits(f) == f64_bits(parse_float64(r.mantissa + 1, (int)r.exp, sign,
                                                          &(bool){false}))) {
        *code = range ? FLOAT_RANGE : FLOAT_OK;
        return f;
    }

    Decimal dec;
    if (!dec_set(&dec, str_from_bytes(s.p, *n))) {
        *code = FLOAT_SYNTAX;
        return 0;
    }
    bool overflow = false;
    uint64_t b = dec_float_bits(&dec, &float64_info, &overflow);
    *code = overflow ? FLOAT_RANGE : FLOAT_OK;
    return f64_from_bits(b);
}

static double parse_float_prefix(Str s, Int bit_size, Int *n, FloatCode *code) {
    if (bit_size == 32)
        return atof32(s, n, code);
    return atof64(s, n, code);
}

static void float_report(Error *err, const char *func, Str s, FloatCode code) {
    if (err == NULL)
        return;
    switch (code) {
    case FLOAT_RANGE:
        *err = burrow__strconv_num_error(func, s, strconv_err_range);
        break;
    case FLOAT_SYNTAX:
        *err = burrow__strconv_num_error(func, s, strconv_err_syntax);
        break;
    case FLOAT_OK:
    default:
        *err = BURROW_NO_ERROR;
        break;
    }
}

double strconv_parse_float(Str s, Int bit_size, Error *err) {
    Int n;
    FloatCode code;
    double f = parse_float_prefix(s, bit_size, &n, &code);
    if (n != s.len) {
        f = 0;
        code = FLOAT_SYNTAX;
    }
    float_report(err, "ParseFloat", s, code);
    return f;
}

static Complex128 complex_syntax(Error *err, Str s) {
    float_report(err, "ParseComplex", s, FLOAT_SYNTAX);
    return (Complex128){0, 0};
}

static Complex128 complex_done(Error *err, Str s, FloatCode pending, double re, double im) {
    float_report(err, "ParseComplex", s, pending);
    return (Complex128){re, im};
}

Complex128 strconv_parse_complex(Str s, Int bit_size, Error *err) {
    Str in = s;
    Int size = bit_size == 64 ? 32 : 64; /* complex64 is two float32s */

    if (s.len >= 2 && s.p[0] == '(' && s.p[s.len - 1] == ')')
        s = str_from_bytes(s.p + 1, s.len - 2);

    FloatCode pending = FLOAT_OK;
    Int n;
    FloatCode code;

    /* The real part, or the imaginary one if an i follows it. */
    double re = parse_float_prefix(s, size, &n, &code);
    if (code != FLOAT_OK) {
        if (code != FLOAT_RANGE)
            return complex_syntax(err, in);
        pending = code;
    }
    s = str_from_bytes(s.p + n, s.len - n);

    if (s.len == 0)
        return complex_done(err, in, pending, re, 0);

    switch (s.p[0]) {
    case '+':
        /* Take the + so that +NaNi works, but not from ++, which is an error
         * the next parse should see. */
        if (s.len > 1 && s.p[1] != '+')
            s = str_from_bytes(s.p + 1, s.len - 1);
        break;
    case '-':
        break;
    case 'i':
        if (s.len == 1)
            return complex_done(err, in, pending, 0, re);
        return complex_syntax(err, in);
    default:
        return complex_syntax(err, in);
    }

    double im = parse_float_prefix(s, size, &n, &code);
    if (code != FLOAT_OK) {
        if (code != FLOAT_RANGE)
            return complex_syntax(err, in);
        pending = code;
    }
    s = str_from_bytes(s.p + n, s.len - n);
    if (s.len != 1 || s.p[0] != 'i')
        return complex_syntax(err, in);
    return complex_done(err, in, pending, re, im);
}

/* -------------------------------------------------------------- formatting */

/* Where output goes. p NULL means count only. first is the first byte, which
 * FormatComplex needs to know before it writes anything. */
typedef struct FloatOut {
    Byte *p;
    int64_t n;
    Byte first;
} FloatOut;

static void fo_bytes(FloatOut *o, const void *b, int64_t k) {
    if (k <= 0)
        return;
    if (o->n == 0)
        o->first = *(const Byte *)b;
    if (o->p != NULL)
        memcpy(o->p + o->n, b, (size_t)k);
    o->n += k;
}

static void fo_byte(FloatOut *o, Byte c) {
    fo_bytes(o, &c, 1);
}

/* k copies of c, and in the counting run only an addition, so that a huge
 * precision costs nothing to measure. */
static void fo_fill(FloatOut *o, Byte c, int64_t k) {
    if (k <= 0)
        return;
    if (o->n == 0)
        o->first = c;
    if (o->p != NULL)
        memset(o->p + o->n, c, (size_t)k);
    o->n += k;
}

static void fo_uint(FloatOut *o, uint64_t u) {
    Byte buf[20];
    Int nd = float_num_digits(u | 1);
    burrow__strconv_format_base10(buf, nd, u);
    fo_bytes(o, buf, nd);
}

typedef enum FloatForm {
    FORM_TEXT,
    FORM_B,
    FORM_X,
    FORM_EFG,
} FloatForm;

/* Everything the writing needs, worked out once for both runs. The digits
 * point into buf or dec, so a job is not copied once it is prepared. */
typedef struct FloatJob {
    FloatForm form;
    bool neg;
    Byte fmt;
    int64_t prec;
    const char *text;
    uint64_t mant;
    Int exp;
    int mant_bits;
    const Byte *digits;
    Int dp;
    Int nd;
    bool shortest;
    Byte buf[32];
    Decimal dec;
} FloatJob;

static void ftoa_prepare(FloatJob *j, uint64_t bits, const FloatInfo *flt, int min_exp,
                         Byte fmt, int64_t prec) {
    int exp_bits = flt->exp_bits;
    int mant_bits = flt->mant_bits;

    j->fmt = fmt;
    j->prec = prec;
    j->neg = bits >> (exp_bits + mant_bits) != 0;
    int exp = (int)(bits >> mant_bits) & ((1 << exp_bits) - 1);
    uint64_t mant = bits & ((1ULL << mant_bits) - 1);

    if (exp == (1 << exp_bits) - 1) {
        j->form = FORM_TEXT;
        j->text = mant != 0 ? "NaN" : j->neg ? "-Inf" : "+Inf";
        return;
    }
    if (exp == 0)
        exp++;
    else
        mant |= 1ULL << mant_bits;
    exp += flt->bias;

    /* The binary and hex formats are easy. */
    if (fmt == 'b') {
        j->form = FORM_B;
        j->mant = mant;
        j->exp = exp - mant_bits;
        return;
    }
    if (fmt == 'x' || fmt == 'X') {
        j->form = FORM_X;
        j->mant = mant;
        j->exp = exp;
        j->mant_bits = mant_bits;
        return;
    }

    j->form = FORM_EFG;
    j->digits = j->buf;
    j->dp = 0;
    j->nd = 0;
    j->shortest = false;

    if (mant == 0) {
        j->shortest = prec < 0;
        return;
    }

    /* A negative precision means as few digits as read back exactly. */
    if (prec < 0) {
        int s = 64 - (int)bits_len64(mant);
        int p;
        uint64_t d = short_float(mant << s, exp - s - mant_bits, mant_bits, min_exp, &p);
        j->nd = set_digits(j->buf, d, p, float_num_digits(d), &j->dp);
        switch (fmt) {
        case 'e':
        case 'E':
            j->prec = j->nd - 1 > 0 ? j->nd - 1 : 0;
            break;
        case 'f':
            j->prec = j->nd - j->dp > 0 ? j->nd - j->dp : 0;
            break;
        case 'g':
        case 'G':
            j->prec = j->nd;
            break;
        default:
            break;
        }
        j->shortest = true;
        return;
    }

    /* A fixed number of digits, by scaling when there are at most 18. */
    int64_t digits = prec;
    switch (fmt) {
    case 'f':
        /* prec counts digits after the point, so this is an upper bound on
         * the total, and fixed_width_float drops the extra. */
        if (exp >= 0)
            digits = 1 + log10_pow2(1 + exp) + prec;
        else
            digits = 1 + prec - log10_pow2(-exp);
        break;
    case 'e':
    case 'E':
        digits++;
        break;
    case 'g':
    case 'G':
        if (prec == 0)
            prec = 1;
        digits = prec;
        break;
    default:
        digits = 1;
        break;
    }
    j->prec = prec;
    if (digits <= 18) {
        /* digits <= 0 happens for %f of a very small number, which is all
         * zeros. */
        if (digits > 0) {
            int s = 64 - (int)bits_len64(mant);
            int p;
            uint64_t d = fixed_width_float(mant << s, exp - s - mant_bits, (int)digits, prec,
                                           fmt, &p);
            if (d != 0)
                j->nd = set_digits(j->buf, d, p, float_num_digits(d), &j->dp);
        }
        return;
    }

    /* More than 18 digits takes the multiprecision decimal. */
    Decimal *d = &j->dec;
    d->neg = false;
    d->trunc = false;
    dec_assign(d, mant);
    dec_shift(d, exp - mant_bits);
    switch (fmt) {
    case 'e':
    case 'E':
        dec_round(d, prec + 1);
        break;
    case 'f':
        dec_round(d, d->dp + prec);
        break;
    case 'g':
    case 'G':
        dec_round(d, prec);
        break;
    default:
        break;
    }
    j->digits = d->d;
    j->dp = d->dp;
    j->nd = d->nd;
}

static int64_t min64(int64_t a, int64_t b) {
    return a < b ? a : b;
}

static int64_t max64(int64_t a, int64_t b) {
    return a > b ? a : b;
}

/* %e, %f and %g, from the digits. */
static void emit_efg(FloatOut *o, const FloatJob *j) {
    Byte fmt = j->fmt;
    int64_t prec = j->prec;
    int64_t dp = j->dp;
    int64_t nd = j->nd;
    const Byte *s = j->digits;

    if (fmt == 'g' || fmt == 'G') {
        /* Trailing zeros in the fraction of the e form are trimmed. */
        int64_t eprec = prec;
        if (eprec > nd && nd >= dp)
            eprec = nd;
        /* %e when the exponent is below -4 or at least the precision, taking
         * the precision as 6 for the shortest form. */
        if (j->shortest)
            eprec = 6;
        int64_t exp = dp - 1;
        if (exp < -4 || exp >= eprec) {
            if (prec > nd)
                prec = nd;
            prec--;
            fmt = (Byte)(fmt + 'e' - 'g');
        } else {
            if (prec > dp)
                prec = nd;
            prec = max64(prec - dp, 0);
            fmt = 'f';
        }
    }

    switch (fmt) {
    case 'e':
    case 'E': {
        if (j->neg)
            fo_byte(o, '-');
        fo_byte(o, nd != 0 ? s[0] : '0');
        if (prec > 0) {
            fo_byte(o, '.');
            int64_t i = 1;
            int64_t m = min64(nd, prec + 1);
            if (i < m) {
                fo_bytes(o, s + i, m - i);
                i = m;
            }
            fo_fill(o, '0', prec + 1 - i);
        }

        fo_byte(o, fmt);
        int64_t exp = nd == 0 ? 0 : dp - 1; /* zero has exponent 0 */
        Byte ch = '+';
        if (exp < 0) {
            ch = '-';
            exp = -exp;
        }
        fo_byte(o, ch);
        if (exp < 10) {
            fo_byte(o, '0');
            fo_byte(o, (Byte)('0' + exp));
        } else if (exp < 100) {
            fo_byte(o, (Byte)('0' + exp / 10));
            fo_byte(o, (Byte)('0' + exp % 10));
        } else {
            fo_byte(o, (Byte)('0' + exp / 100));
            fo_byte(o, (Byte)('0' + exp / 10 % 10));
            fo_byte(o, (Byte)('0' + exp % 10));
        }
        return;
    }
    case 'f':
        if (j->neg)
            fo_byte(o, '-');

        /* The integer part, padded with zeros. */
        if (dp > 0) {
            int64_t m = min64(nd, dp);
            fo_bytes(o, s, m);
            fo_fill(o, '0', dp - m);
        } else {
            fo_byte(o, '0');
        }

        if (prec > 0) {
            fo_byte(o, '.');
            int64_t lz = min64(prec, max64(0, -dp));        /* leading zeros */
            int64_t off = dp + lz;
            int64_t m = min64(prec - lz, max64(0, nd - off)); /* digits */
            int64_t tz = max64(0, prec - lz - m);           /* trailing zeros */
            fo_fill(o, '0', lz);
            fo_bytes(o, s + off, m);
            fo_fill(o, '0', tz);
        }
        return;
    default:
        break;
    }

    /* A format Go does not know. */
    fo_byte(o, '%');
    fo_byte(o, fmt);
}

/* %b: -ddddp+ddd, the mantissa and the binary exponent in decimal. */
static void emit_b(FloatOut *o, const FloatJob *j) {
    if (j->neg)
        fo_byte(o, '-');
    fo_uint(o, j->mant);
    fo_byte(o, 'p');
    if (j->exp >= 0) {
        fo_byte(o, '+');
        fo_uint(o, (uint64_t)j->exp);
    } else {
        fo_byte(o, '-');
        fo_uint(o, (uint64_t)-j->exp);
    }
}

/* %x: -0x1.yyyyyp+ddd, or -0x0p+00 for zero. */
static void emit_x(FloatOut *o, const FloatJob *j) {
    static const char lower_digits[] = "0123456789abcdef";
    static const char upper_digits[] = "0123456789ABCDEF";
    uint64_t mant = j->mant;
    Int exp = j->exp;
    int64_t prec = j->prec;
    Byte fmt = j->fmt;

    if (mant == 0)
        exp = 0;

    /* The leading one, if there is one, at bit 60. */
    mant <<= 60 - j->mant_bits;
    while (mant != 0 && (mant & (1ULL << 60)) == 0) {
        mant <<= 1;
        exp--;
    }

    if (prec >= 0 && prec < 15) {
        unsigned shift = (unsigned)(prec * 4);
        uint64_t extra = (mant << shift) & ((1ULL << 60) - 1);
        mant >>= 60 - shift;
        if ((extra | (mant & 1)) > 1ULL << 59)
            mant++;
        mant <<= 60 - shift;
        if ((mant & (1ULL << 61)) != 0) {
            /* It carried out of the leading digit. */
            mant >>= 1;
            exp++;
        }
    }

    const char *hex = fmt == 'X' ? upper_digits : lower_digits;

    if (j->neg)
        fo_byte(o, '-');
    fo_byte(o, '0');
    fo_byte(o, fmt);
    fo_byte(o, (Byte)('0' + ((mant >> 60) & 1)));

    mant <<= 4; /* past the leading digit */
    if (prec < 0 && mant != 0) {
        fo_byte(o, '.');
        while (mant != 0) {
            fo_byte(o, (Byte)hex[(mant >> 60) & 15]);
            mant <<= 4;
        }
    } else if (prec > 0) {
        /* Sixteen digits empty the mantissa, and the rest are zeros. */
        fo_byte(o, '.');
        int64_t shown = min64(prec, 16);
        for (int64_t i = 0; i < shown; i++) {
            fo_byte(o, (Byte)hex[(mant >> 60) & 15]);
            mant <<= 4;
        }
        fo_fill(o, '0', prec - shown);
    }

    fo_byte(o, fmt == float_lower(fmt) ? 'p' : 'P');
    Byte ch = '+';
    if (exp < 0) {
        ch = '-';
        exp = -exp;
    }
    fo_byte(o, ch);
    if (exp < 100) {
        fo_byte(o, (Byte)('0' + exp / 10));
        fo_byte(o, (Byte)('0' + exp % 10));
    } else if (exp < 1000) {
        fo_byte(o, (Byte)('0' + exp / 100));
        fo_byte(o, (Byte)('0' + exp / 10 % 10));
        fo_byte(o, (Byte)('0' + exp % 10));
    } else {
        fo_byte(o, (Byte)('0' + exp / 1000));
        fo_byte(o, (Byte)('0' + exp / 100 % 10));
        fo_byte(o, (Byte)('0' + exp / 10 % 10));
        fo_byte(o, (Byte)('0' + exp % 10));
    }
}

static void emit_float(FloatOut *o, const FloatJob *j) {
    switch (j->form) {
    case FORM_TEXT:
        fo_bytes(o, j->text, (int64_t)strlen(j->text));
        break;
    case FORM_B:
        emit_b(o, j);
        break;
    case FORM_X:
        emit_x(o, j);
        break;
    case FORM_EFG:
    default:
        emit_efg(o, j);
        break;
    }
}

/* Past this the length would not fit in an Int, and Go could not allocate the
 * result either. */
#define FLOAT_PREC_MAX (INT64_MAX / 4)

/* Prepares j, or says the output could not be allocated when prec is past
 * FLOAT_PREC_MAX. A bad bit size is a panic first, as it is in Go. */
static bool float_job(FloatJob *j, double f, Byte fmt, int64_t prec, Int bit_size,
                      const char *panic_message) {
    if (bit_size != 32 && bit_size != 64)
        panic_str(str_from_cstr(panic_message));
    if (prec > FLOAT_PREC_MAX)
        return false;
    if (bit_size == 32)
        ftoa_prepare(j, f32_bits((float)f), &float32_info, F32_MIN_EXP, fmt, prec);
    else
        ftoa_prepare(j, f64_bits(f), &float64_info, F64_MIN_EXP, fmt, prec);
    return true;
}

Str strconv_format_float(Alloc *a, double f, Byte fmt, Int prec, Int bit_size) {
    FloatJob j;
    if (!float_job(&j, f, fmt, prec, bit_size, "strconv: illegal FormatFloat bitSize"))
        return BURROW_STR_EMPTY;

    FloatOut count = {NULL, 0, 0};
    emit_float(&count, &j);
    if (count.n > BURROW_INT_MAX)
        return BURROW_STR_EMPTY;

    Byte *p = (Byte *)mem_alloc_nozero(a, (size_t)count.n, 1);
    if (p == NULL)
        return BURROW_STR_EMPTY;
    FloatOut o = {p, 0, 0};
    emit_float(&o, &j);
    return str_from_bytes(p, (Int)o.n);
}

/* Grows dst by n and returns where the new bytes start, or NULL when it could
 * not. */
static Byte *float_grow(Alloc *a, Slice *dst, int64_t n) {
    /* A zero Slice means Go's nil []byte. */
    if (dst->elem == NULL)
        *dst = slice_nil(TYPE_BYTE);
    if (n > BURROW_INT_MAX - dst->len) {
        *dst = slice_nil(TYPE_BYTE);
        return NULL;
    }
    Int old = dst->len;
    *dst = slice_append(a, *dst, NULL, (Int)n);
    return dst->p == NULL ? NULL : (Byte *)dst->p + old;
}

Slice strconv_append_float(Alloc *a, Slice dst, double f, Byte fmt, Int prec, Int bit_size) {
    FloatJob j;
    if (!float_job(&j, f, fmt, prec, bit_size, "strconv: illegal AppendFloat bitSize"))
        return slice_nil(TYPE_BYTE);

    FloatOut count = {NULL, 0, 0};
    emit_float(&count, &j);
    Byte *p = float_grow(a, &dst, count.n);
    if (p == NULL)
        return dst;
    FloatOut o = {p, 0, 0};
    emit_float(&o, &j);
    return dst;
}

/* (re+imi), with the + only when the imaginary part has no sign of its own. */
static void emit_complex(FloatOut *o, const FloatJob *re, const FloatJob *im, bool im_signed) {
    fo_byte(o, '(');
    emit_float(o, re);
    if (!im_signed)
        fo_byte(o, '+');
    emit_float(o, im);
    fo_bytes(o, "i)", 2);
}

Str strconv_format_complex(Alloc *a, Complex128 c, Byte fmt, Int prec, Int bit_size) {
    if (bit_size != 64 && bit_size != 128)
        panic_str(BURROW_S("invalid bitSize"));
    bit_size >>= 1; /* complex64 is two float32s */

    FloatJob re, im;
    if (!float_job(&re, c.re, fmt, prec, bit_size, "strconv: illegal AppendFloat bitSize") ||
        !float_job(&im, c.im, fmt, prec, bit_size, "strconv: illegal AppendFloat bitSize"))
        return BURROW_STR_EMPTY;

    FloatOut first = {NULL, 0, 0};
    emit_float(&first, &im);
    bool im_signed = first.first == '+' || first.first == '-';

    FloatOut count = {NULL, 0, 0};
    emit_complex(&count, &re, &im, im_signed);
    if (count.n > BURROW_INT_MAX)
        return BURROW_STR_EMPTY;

    Byte *p = (Byte *)mem_alloc_nozero(a, (size_t)count.n, 1);
    if (p == NULL)
        return BURROW_STR_EMPTY;
    FloatOut o = {p, 0, 0};
    emit_complex(&o, &re, &im, im_signed);
    return str_from_bytes(p, (Int)o.n);
}
