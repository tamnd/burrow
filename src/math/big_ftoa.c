/* math/big: Float to text, ftoa.go and decimal.go.
 *
 * The digits are built in scratch memory and only the finished text is
 * appended to the caller's buffer, so nothing here commits anything.
 *
 * Copyright 2015 The Go Authors. All rights reserved.
 * Copyright 2026 The burrow Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style licence that can be found
 * in the LICENSE file. */

#include "big_float.h"

#include "burrow/fmt.h"
#include "burrow/io.h"
#include "burrow/panic.h"

/* ------------------------------------------------------------ decimal.go */

/* A decimal represents an unsigned floating-point number in decimal
 * representation. The value of a non-zero decimal d is d.mant * 10**d.exp
 * with 0.1 <= d.mant < 1, with the most-significant mantissa digit at index
 * 0. For the zero decimal, the mantissa length and exponent are 0. The zero
 * value for decimal represents a ready-to-use 0.0. */
typedef struct BfDecimal {
    Byte *mant; /* mantissa ASCII digits, big-endian */
    Int len, cap;
    Int exp; /* exponent */
} BfDecimal;

static void bf_dec_reserve(BfDecimal *d, Int n) {
    if (n <= d->cap)
        return;
    Int c = d->cap * 2;
    if (c < n)
        c = n;
    if (c < 16)
        c = 16;
    Byte *p = mem_alloc_nozero(big_scratch_alloc(), (size_t)c, 1);
    if (p == NULL)
        big_oom();
    if (d->len > 0)
        memcpy(p, d->mant, (size_t)d->len);
    d->mant = p;
    d->cap = c;
}

static void bf_dec_push(BfDecimal *d, Byte c) {
    bf_dec_reserve(d, d->len + 1);
    d->mant[d->len++] = c;
}

/* at returns the i'th mantissa digit, starting with the most significant
 * digit at 0. */
static Byte bf_dec_at(const BfDecimal *d, Int i) {
    if (0 <= i && i < d->len)
        return d->mant[i];
    return '0';
}

/* Maximum shift amount that can be done in one pass without overflow.
 * A Word has _W bits and (1<<maxShift - 1)*10 + 9 must fit into Word. */
#define DEC_MAX_SHIFT (BIG_W - 4)

static void bf_dec_trim(BfDecimal *x);

/* rsh implements x >> s, for s <= maxShift. */
static void bf_dec_rsh(BfDecimal *x, Uint s) {
    /* Division by 1<<s using shift-and-subtract algorithm. */

    /* pick up enough leading digits to cover first shift */
    Int r = 0; /* read index */
    BigWord n = 0;
    while (n >> s == 0 && r < x->len) {
        BigWord ch = x->mant[r];
        r++;
        n = n * 10 + ch - '0';
    }

    /* x < 1<<s */
    if (n == 0) {
        /* x == 0; shouldn't get here, but handle anyway */
        x->len = 0;
        return;
    }
    while (n >> s == 0) {
        r++;
        n *= 10;
    }
    x->exp += 1 - r;

    /* read a digit, write a digit */
    Int w = 0; /* write index */
    BigWord mask = ((BigWord)1 << s) - 1;
    while (r < x->len) {
        BigWord ch = x->mant[r];
        r++;
        BigWord d = n >> s;
        n &= mask; /* n -= d << s */
        x->mant[w] = (Byte)(d + '0');
        w++;
        n = n * 10 + ch - '0';
    }

    /* write extra digits that still fit */
    while (n > 0 && w < x->len) {
        BigWord d = n >> s;
        n &= mask;
        x->mant[w] = (Byte)(d + '0');
        w++;
        n = n * 10;
    }
    x->len = w; /* the number may be shorter (e.g. 1024 >> 10) */

    /* append additional digits that didn't fit */
    while (n > 0) {
        BigWord d = n >> s;
        n &= mask;
        bf_dec_push(x, (Byte)(d + '0'));
        n = n * 10;
    }

    bf_dec_trim(x);
}

/* init initializes x to the decimal representation of m << shift (for
 * shift >= 0), or m >> -shift (for shift < 0). */
static void bf_dec_init(BfDecimal *x, Nat m, Int shift) {
    /* special case 0 */
    if (m.len == 0) {
        x->len = 0;
        x->exp = 0;
        return;
    }

    /* Optimization: If we need to shift right, first remove any trailing
     * zero bits from m to reduce shift amount that needs to be done in
     * decimal format (since that is likely slower). */
    if (shift < 0) {
        Uint ntz = nat_trailing_zero_bits(m);
        Uint s = (Uint)-shift;
        if (s >= ntz)
            s = ntz; /* shift at most ntz bits */
        m = nat_rsh(NAT_NIL, m, s);
        shift += (Int)s;
    }

    /* Do any shift left in binary representation. */
    if (shift > 0) {
        m = nat_lsh(NAT_NIL, m, (Uint)shift);
        shift = 0;
    }

    /* Convert mantissa into decimal representation. */
    Str s = nat_utoa(m, 10);
    Int n = s.len;
    x->exp = n;
    /* Trim trailing zeros; instead the exponent is tracking
     * the decimal point independent of the number of digits. */
    while (n > 0 && s.p[n - 1] == '0')
        n--;
    x->len = 0;
    bf_dec_reserve(x, n);
    if (n > 0)
        memcpy(x->mant, s.p, (size_t)n);
    x->len = n;

    /* Do any (remaining) shift right in decimal representation. */
    if (shift < 0) {
        while (shift < -(Int)DEC_MAX_SHIFT) {
            bf_dec_rsh(x, DEC_MAX_SHIFT);
            shift += (Int)DEC_MAX_SHIFT;
        }
        bf_dec_rsh(x, (Uint)-shift);
    }
}

/* shouldRoundUp reports if x should be rounded up if shortened to n digits.
 * n must be a valid index for x.mant. */
static bool bf_dec_should_round_up(const BfDecimal *x, Int n) {
    if (x->mant == NULL)
        return false;
    if (x->mant[n] == '5' && n + 1 == x->len) {
        /* exactly halfway - round to even */
        return n > 0 && ((x->mant[n - 1] - '0') & 1) != 0;
    }
    /* not halfway - digit tells all (x.mant has no trailing zeros) */
    return x->mant[n] >= '5';
}

static void bf_dec_round_up(BfDecimal *x, Int n);
static void bf_dec_round_down(BfDecimal *x, Int n);

/* round sets x to (at most) n mantissa digits by rounding it to the nearest
 * even value with n (or fever) mantissa digits. If n < 0, x remains
 * unchanged. */
static void bf_dec_round(BfDecimal *x, Int n) {
    if (n < 0 || n >= x->len)
        return; /* nothing to do */

    if (bf_dec_should_round_up(x, n))
        bf_dec_round_up(x, n);
    else
        bf_dec_round_down(x, n);
}

static void bf_dec_round_up(BfDecimal *x, Int n) {
    if (n < 0 || n >= x->len)
        return; /* nothing to do */
    /* 0 <= n < len(x.mant) */

    /* find first digit < '9' */
    while (n > 0 && x->mant[n - 1] >= '9')
        n--;

    if (n == 0) {
        /* all digits are '9's => round up to '1' and update exponent */
        x->mant[0] = '1'; /* ok since len(x.mant) > n */
        x->len = 1;
        x->exp++;
        return;
    }

    /* n > 0 && x.mant[n-1] < '9' */
    x->mant[n - 1]++;
    x->len = n;
    /* x already trimmed */
}

static void bf_dec_round_down(BfDecimal *x, Int n) {
    if (n < 0 || n >= x->len)
        return; /* nothing to do */
    x->len = n;
    bf_dec_trim(x);
}

/* trim cuts off any trailing zeros from x's mantissa; they are meaningless
 * for the value of x. */
static void bf_dec_trim(BfDecimal *x) {
    Int i = x->len;
    while (i > 0 && x->mant[i - 1] == '0')
        i--;
    x->len = i;
    if (i == 0)
        x->exp = 0;
}

/* --------------------------------------------------------------- ftoa.go */

typedef struct BfBuf {
    Alloc *a;
    Slice s;
} BfBuf;

static void bf_buf_str(BfBuf *b, Str s) {
    b->s = big_append_str(b->a, b->s, s);
}

static void bf_buf_bytes(BfBuf *b, const Byte *p, Int n) {
    Str s = {p, n};
    bf_buf_str(b, s);
}

static void bf_buf_byte(BfBuf *b, Byte c) {
    bf_buf_bytes(b, &c, 1);
}

/* strconv.AppendInt(buf, i, 10) */
static void bf_buf_int(BfBuf *b, int64_t i) {
    Byte d[24];
    Int n = (Int)sizeof d;
    uint64_t u = (uint64_t)i;
    if (i < 0)
        u = 0 - u;
    do {
        d[--n] = (Byte)('0' + u % 10);
        u /= 10;
    } while (u != 0);
    if (i < 0)
        d[--n] = '-';
    bf_buf_bytes(b, d + n, (Int)sizeof d - n);
}

/* %e: d.ddddde±dd */
static void bf_fmt_e(BfBuf *buf, Byte fmt, Int prec, const BfDecimal *d) {
    /* first digit */
    Byte ch = '0';
    if (d->len > 0)
        ch = d->mant[0];
    bf_buf_byte(buf, ch);

    /* .moredigits */
    if (prec > 0) {
        bf_buf_byte(buf, '.');
        Int i = 1;
        Int m = d->len < prec + 1 ? d->len : prec + 1;
        if (i < m) {
            bf_buf_bytes(buf, d->mant + i, m - i);
            i = m;
        }
        for (; i <= prec; i++)
            bf_buf_byte(buf, '0');
    }

    /* e± */
    bf_buf_byte(buf, fmt);
    int64_t exp = 0;
    if (d->len > 0)
        exp = (int64_t)d->exp - 1; /* -1 because first digit was printed before '.' */
    if (exp < 0) {
        ch = '-';
        exp = -exp;
    } else {
        ch = '+';
    }
    bf_buf_byte(buf, ch);

    /* dd...d */
    if (exp < 10)
        bf_buf_byte(buf, '0'); /* at least 2 exponent digits */
    bf_buf_int(buf, exp);
}

/* %f: ddddddd.ddddd */
static void bf_fmt_f(BfBuf *buf, Int prec, const BfDecimal *d) {
    /* integer, padded with zeros as needed */
    if (d->exp > 0) {
        Int m = d->len < d->exp ? d->len : d->exp;
        bf_buf_bytes(buf, d->mant, m);
        for (; m < d->exp; m++)
            bf_buf_byte(buf, '0');
    } else {
        bf_buf_byte(buf, '0');
    }

    /* fraction */
    if (prec > 0) {
        bf_buf_byte(buf, '.');
        for (Int i = 0; i < prec; i++)
            bf_buf_byte(buf, bf_dec_at(d, d->exp + i));
    }
}

static void bf_round_shortest(BfDecimal *d, const BigFloat *x) {
    /* if the mantissa is zero, the number is zero - stop now */
    if (d->len == 0)
        return;

    /* Approach: All numbers in the interval [x - 1/2ulp, x + 1/2ulp]
     * (possibly exclusive) round to x for the given precision of x.
     * Compute the lower and upper bound in decimal form and find the
     * shortest decimal number d such that lower <= d <= upper. */

    /* TODO(gri) strconv/ftoa.do describes a shortcut in some cases.
     * See if we can use it (in adjusted form) here as well. */

    /* 1) Compute normalized mantissa mant and exponent exp for x such
     * that the lsb of mant corresponds to 1/2 ulp for the precision of
     * x (i.e., for mant we want x.prec + 1 bits). */
    Nat mant = nat_set(NAT_NIL, x->mant);
    Int exp = (Int)x->exp - nat_bit_len(mant);
    Int s = nat_bit_len(mant) - (Int)((Uint)x->prec + 1);
    if (s < 0)
        mant = nat_lsh(mant, mant, (Uint)-s);
    else if (s > 0)
        mant = nat_rsh(mant, mant, (Uint) + s);
    exp += s;
    /* x = mant * 2**exp with lsb(mant) == 1/2 ulp of x.prec */

    /* 2) Compute lower bound by subtracting 1/2 ulp. */
    BfDecimal lower = {0};
    bf_dec_init(&lower, nat_sub(NAT_NIL, mant, NAT_ONE), exp);

    /* 3) Compute upper bound by adding 1/2 ulp. */
    BfDecimal upper = {0};
    bf_dec_init(&upper, nat_add(NAT_NIL, mant, NAT_ONE), exp);

    /* The upper and lower bounds are possible outputs only if
     * the original mantissa is even, so that ToNearestEven rounding
     * would round to the original mantissa and not the neighbors. */
    bool inclusive =
        (mant.p[0] & 2) == 0; /* test bit 1 since original mantissa was shifted by 1 */

    /* Now we can figure out the minimum number of digits required.
     * Walk along until d has distinguished itself from upper and lower. */
    for (Int i = 0; i < d->len; i++) {
        Byte m = d->mant[i];
        Byte l = bf_dec_at(&lower, i);
        Byte u = bf_dec_at(&upper, i);

        /* Okay to round down (truncate) if lower has a different digit
         * or if lower is inclusive and is exactly the result of rounding
         * down (i.e., and we have reached the final digit of lower). */
        bool okdown = l != m || (inclusive && i + 1 == lower.len);

        /* Okay to round up if upper has a different digit and either upper
         * is inclusive or upper is bigger than the result of rounding up. */
        bool okup = m != u && (inclusive || m + 1 < u || i + 1 < upper.len ||
                               (i >= upper.len && m < '9'));

        /* If it's okay to do either, then round to the nearest one.
         * If it's okay to do only one, do it. */
        if (okdown && okup) {
            bf_dec_round(d, i + 1);
            return;
        }
        if (okdown) {
            bf_dec_round_down(d, i + 1);
            return;
        }
        if (okup) {
            bf_dec_round_up(d, i + 1);
            return;
        }
    }
}

/* fmtB appends the string of x in the format mantissa "p" exponent with a
 * decimal mantissa and a binary exponent, or "0" if x is zero, and returns
 * the extended buffer. The mantissa is normalized such that is uses x.Prec()
 * bits in binary representation. The sign of x is ignored, and x must not be
 * an Inf. (The caller handles Inf before invoking fmtB.) */
static void bf_fmt_b(BfBuf *buf, const BigFloat *x) {
    if (x->form == BF_ZERO) {
        bf_buf_byte(buf, '0');
        return;
    }

    /* adjust mantissa to use exactly x.prec bits */
    Nat m = x->mant;
    uint32_t w = (uint32_t)x->mant.len * (uint32_t)BIG_W;
    if (w < x->prec)
        m = nat_lsh(NAT_NIL, m, (Uint)(x->prec - w));
    else if (w > x->prec)
        m = nat_rsh(NAT_NIL, m, (Uint)(w - x->prec));

    bf_buf_str(buf, nat_utoa(m, 10));
    bf_buf_byte(buf, 'p');
    int64_t e = (int64_t)x->exp - (int64_t)x->prec;
    if (e >= 0)
        bf_buf_byte(buf, '+');
    bf_buf_int(buf, e);
}

/* fmtX appends the string of x in the format "0x1." mantissa "p" exponent
 * with a hexadecimal mantissa and a binary exponent, or "0x0p0" if x is zero,
 * and returns the extended buffer. A non-zero mantissa is normalized such
 * that 1.0 <= mantissa < 2.0. The sign of x is ignored, and x must not be an
 * Inf. (The caller handles Inf before invoking fmtX.) */
static void bf_fmt_x(BfBuf *buf, const BigFloat *x0, Int prec) {
    if (x0->form == BF_ZERO) {
        bf_buf_str(buf, BURROW_S("0x0"));
        if (prec > 0) {
            bf_buf_byte(buf, '.');
            for (Int i = 0; i < prec; i++)
                bf_buf_byte(buf, '0');
        }
        bf_buf_str(buf, BURROW_S("p+00"));
        return;
    }

    /* round mantissa to n bits */
    Uint n;
    if (prec < 0)
        n = 1 + (bf_min_prec(x0) - 1 + 3) / 4 * 4; /* round MinPrec up to 1 mod 4 */
    else
        n = 1 + 4 * (Uint)prec;
    /* n%4 == 1 */
    BigFloat xr = BIG_FLOAT(NULL);
    bf_set_prec(&xr, n);
    xr.mode = x0->mode;
    xr.acc = BIG_EXACT;
    bf_set(&xr, x0);
    const BigFloat *x = &xr;

    /* adjust mantissa to use exactly n bits */
    Nat m = x->mant;
    Uint w = (Uint)x->mant.len * BIG_W;
    if (w < n)
        m = nat_lsh(NAT_NIL, m, n - w);
    else if (w > n)
        m = nat_rsh(NAT_NIL, m, w - n);
    int64_t exp64 = (int64_t)x->exp - 1; /* avoid wrap-around */

    Str hm = nat_utoa(m, 16);
    bf_buf_str(buf, BURROW_S("0x1"));
    if (hm.len > 1) {
        bf_buf_byte(buf, '.');
        bf_buf_bytes(buf, hm.p + 1, hm.len - 1);
    }

    bf_buf_byte(buf, 'p');
    if (exp64 >= 0) {
        bf_buf_byte(buf, '+');
    } else {
        exp64 = -exp64;
        bf_buf_byte(buf, '-');
    }
    /* Force at least two exponent digits, to match fmt. */
    if (exp64 < 10)
        bf_buf_byte(buf, '0');
    bf_buf_int(buf, exp64);
}

/* fmtP appends the string of x in the format "0x." mantissa "p" exponent
 * with a hexadecimal mantissa and a binary exponent, or "0" if x is zero,
 * and returns the extended buffer. The mantissa is normalized such that
 * 0.5 <= 0.mantissa < 1.0. The sign of x is ignored, and x must not be an
 * Inf. (The caller handles Inf before invoking fmtP.) */
static void bf_fmt_p(BfBuf *buf, const BigFloat *x) {
    if (x->form == BF_ZERO) {
        bf_buf_byte(buf, '0');
        return;
    }

    /* remove trailing 0 words early
     * (no need to convert to hex 0's and trim later) */
    Nat m = x->mant;
    Int i = 0;
    while (i < m.len && m.p[i] == 0)
        i++;
    m = nat_from(m, i);

    Str h = nat_utoa(m, 16);
    Int hn = h.len;
    while (hn > 0 && h.p[hn - 1] == '0')
        hn--;
    bf_buf_str(buf, BURROW_S("0x."));
    bf_buf_bytes(buf, h.p, hn);
    bf_buf_byte(buf, 'p');
    if (x->exp >= 0)
        bf_buf_byte(buf, '+');
    bf_buf_int(buf, x->exp);
}

static void bf_append_buf(BfBuf *buf, const BigFloat *x, Byte fmt, Int prec) {
    /* sign */
    Int start = buf->s.len;
    if (x->neg)
        bf_buf_byte(buf, '-');

    /* Inf */
    if (x->form == BF_INF) {
        if (!x->neg)
            bf_buf_byte(buf, '+');
        bf_buf_str(buf, BURROW_S("Inf"));
        return;
    }

    /* pick off easy formats */
    switch (fmt) {
    case 'b':
        bf_fmt_b(buf, x);
        return;
    case 'p':
        bf_fmt_p(buf, x);
        return;
    case 'x':
        bf_fmt_x(buf, x, prec);
        return;
    default:
        break;
    }

    /* Algorithm:
     *   1) convert Float to multiprecision decimal
     *   2) round to desired precision
     *   3) read digits out and format */

    /* 1) convert Float to multiprecision decimal */
    BfDecimal d = {0}; /* == 0.0 */
    if (x->form == BF_FINITE) {
        /* x != 0 */
        bf_dec_init(&d, x->mant, (Int)x->exp - nat_bit_len(x->mant));
    }

    /* 2) round to desired precision */
    bool shortest = false;
    if (prec < 0) {
        shortest = true;
        bf_round_shortest(&d, x);
        /* Precision for shortest representation mode. */
        switch (fmt) {
        case 'e':
        case 'E':
            prec = d.len - 1;
            break;
        case 'f':
            prec = d.len - d.exp > 0 ? d.len - d.exp : 0;
            break;
        case 'g':
        case 'G':
            prec = d.len;
            break;
        default:
            break;
        }
    } else {
        /* round appropriately */
        switch (fmt) {
        case 'e':
        case 'E':
            /* one digit before and number of digits after decimal point */
            bf_dec_round(&d, 1 + prec);
            break;
        case 'f':
            /* number of digits before and after decimal point */
            bf_dec_round(&d, d.exp + prec);
            break;
        case 'g':
        case 'G':
            if (prec == 0)
                prec = 1;
            bf_dec_round(&d, prec);
            break;
        default:
            break;
        }
    }

    /* 3) read digits out and format */
    switch (fmt) {
    case 'e':
    case 'E':
        bf_fmt_e(buf, fmt, prec, &d);
        return;
    case 'f':
        bf_fmt_f(buf, prec, &d);
        return;
    case 'g':
    case 'G': {
        /* trim trailing fractional zeros in %e format */
        Int eprec = prec;
        if (eprec > d.len && d.len >= d.exp)
            eprec = d.len;
        /* %e is used if the exponent from the conversion
         * is less than -4 or greater than or equal to the precision.
         * If precision was the shortest possible, use eprec = 6 for
         * this decision. */
        if (shortest)
            eprec = 6;
        Int exp = d.exp - 1;
        if (exp < -4 || exp >= eprec) {
            if (prec > d.len)
                prec = d.len;
            bf_fmt_e(buf, (Byte)(fmt + 'e' - 'g'), prec - 1, &d);
            return;
        }
        if (prec > d.exp)
            prec = d.len;
        bf_fmt_f(buf, prec - d.exp > 0 ? prec - d.exp : 0, &d);
        return;
    }
    default:
        break;
    }

    /* unknown format */
    if (x->neg)
        buf->s.len = start; /* sign was added prematurely - remove it again */
    Byte pct[2] = {'%', fmt};
    bf_buf_bytes(buf, pct, 2);
}

Slice bf_append(const BigFloat *x, Alloc *a, Slice buf, Byte fmt, Int prec) {
    BfBuf b = {a, buf};
    bf_append_buf(&b, x, fmt, prec);
    if (b.s.elem == NULL)
        b.s.elem = TYPE_BYTE;
    return b.s;
}

Str bf_text(const BigFloat *x, Alloc *a, Byte format, Int prec) {
    Int cap = 10; /* TODO(gri) determine a good/better value here */
    if (prec > 0)
        cap += prec;
    Slice buf = slice_make(a, TYPE_BYTE, 0, cap);
    if (buf.p == NULL)
        big_oom();
    Slice r = bf_append(x, a, buf, format, prec);
    return (Str){r.p, r.len};
}

/* ---------------------------------------------------------------- public */

Str big_float_text(const BigFloat *x, Alloc *a, Byte format, Int prec) {
    big_enter();
    Str s = bf_text(x, a, format, prec);
    big_leave();
    return s;
}

Str big_float_string(const BigFloat *x, Alloc *a) {
    return big_float_text(x, a, 'g', 10);
}

Slice big_float_append(const BigFloat *x, Alloc *a, Slice buf, Byte format, Int prec) {
    big_enter();
    Slice r = bf_append(x, a, buf, format, prec);
    big_leave();
    return r;
}

/* Format implements fmt.Formatter. It accepts all the regular formats for
 * floating-point numbers ('b', 'e', 'E', 'f', 'F', 'g', 'G', 'x') as well as
 * 'p' and 'v'. See (*Float).Text for the interpretation of 'p'. The 'v'
 * format is handled like 'g'. Format also supports the minimum precision in
 * digits, the minimum field width, and format flags for left or right
 * justification, and zero padding. */
void big_float_format(const BigFloat *x, FmtState s, Rune format) {
    bool has_prec;
    Int prec = s.vt->precision(s.data, &has_prec);
    if (!has_prec)
        prec = 6; /* default precision for 'e', 'f' */

    switch (format) {
    case 'e':
    case 'E':
    case 'f':
    case 'b':
    case 'p':
    case 'x':
        /* nothing to do */
        break;
    case 'F':
        /* (*Float).Text doesn't support 'F'; handle like 'f' */
        format = 'f';
        break;
    case 'v':
        /* handle like 'g' */
        format = 'g';
        if (!has_prec)
            prec = -1; /* default precision for 'g', 'G' */
        break;
    case 'g':
    case 'G':
        if (!has_prec)
            prec = -1; /* default precision for 'g', 'G' */
        break;
    default: {
        IoWriter w = fmt_state_writer(&s);
        big_enter();
        Str xs = bf_text(x, big_scratch_alloc(), 'g', 10);
        fmt_fprintf_v(w, "%%!%c(*big.Float=%s)", format, xs);
        big_leave();
        return;
    }
    }

    big_enter();
    Slice b =
        bf_append(x, big_scratch_alloc(), slice_nil(TYPE_BYTE), (Byte)format, prec);
    Str buf = {b.p, b.len};
    if (buf.len == 0)
        buf = BURROW_S("?"); /* should never happen, but don't crash */
    /* len(buf) > 0 */

    Str sign = BURROW_STR_EMPTY;
    if (buf.p[0] == '-') {
        sign = BURROW_S("-");
        buf.p++;
        buf.len--;
    } else if (buf.p[0] == '+') {
        /* +Inf */
        sign = BURROW_S("+");
        if (s.vt->flag(s.data, ' '))
            sign = BURROW_S(" ");
        buf.p++;
        buf.len--;
    } else if (s.vt->flag(s.data, '+')) {
        sign = BURROW_S("+");
    } else if (s.vt->flag(s.data, ' ')) {
        sign = BURROW_S(" ");
    }

    Int padding = 0;
    bool has_width;
    Int width = s.vt->width(s.data, &has_width);
    if (has_width && width > sign.len + buf.len)
        padding = width - sign.len - buf.len;

    if (s.vt->flag(s.data, '0') && x->form != BF_INF) {
        /* 0-padding on left */
        big_write_multiple(s, sign, 1);
        big_write_multiple(s, BURROW_S("0"), padding);
        big_write_multiple(s, buf, 1);
    } else if (s.vt->flag(s.data, '-')) {
        /* padding on right */
        big_write_multiple(s, sign, 1);
        big_write_multiple(s, buf, 1);
        big_write_multiple(s, BURROW_S(" "), padding);
    } else {
        /* padding on left */
        big_write_multiple(s, BURROW_S(" "), padding);
        big_write_multiple(s, sign, 1);
        big_write_multiple(s, buf, 1);
    }
    big_leave();
}
