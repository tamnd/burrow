/* math/big: the word vector loops, arith.go.
 *
 * Go has these in assembly for the machines it cares most about. Here they
 * are the pure Go versions in C, over math/bits' carry and 128 bit multiply,
 * which compilers turn into the add with carry and wide multiply instructions
 * the assembly uses.
 *
 * Copyright 2009 The Go Authors. All rights reserved.
 * Copyright 2026 The burrow Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style licence that can be found
 * in the LICENSE file. */

#include "big_internal.h"

/* The loops below copy the vectors' pointers and length into locals before
 * they start. Reading them through z each time would let a store to z.p[i]
 * look like it might change z.len, and the compiler would load the length
 * again after every word, which on its own made adding two long numbers
 * several times slower than Go's assembly.
 *
 * big_addc and big_subc are one word of an add or subtract with carry. With
 * __builtin_addcll, which clang and gcc 14 have, a run of them becomes a chain
 * of adc or adcs instructions. Without it they are math/bits' portable
 * version, which gives the same answer more slowly. */
#if defined(__has_builtin) && BURROW_INT_BITS == 64 && !defined(BURROW__BITS_PORTABLE)
#if __has_builtin(__builtin_addcll) && __has_builtin(__builtin_subcll)
#define BIG_CARRY_BUILTINS 1
#endif
#endif
#ifndef BIG_CARRY_BUILTINS
#define BIG_CARRY_BUILTINS 0
#endif

#if BURROW__BITS_INT128 && BURROW_INT_BITS == 64
#define BIG_WIDE 1
__extension__ typedef unsigned __int128 BigDWord;
#else
#define BIG_WIDE 0
#endif

static inline BigWord big_addc(BigWord x, BigWord y, BigWord c, BigWord *co) {
#if BIG_CARRY_BUILTINS
    unsigned long long o;
    unsigned long long s = __builtin_addcll(x, y, c, &o);
    *co = (BigWord)o;
    return (BigWord)s;
#else
    return bits_add(x, y, c, co);
#endif
}

static inline BigWord big_subc(BigWord x, BigWord y, BigWord c, BigWord *co) {
#if BIG_CARRY_BUILTINS
    unsigned long long o;
    unsigned long long s = __builtin_subcll(x, y, c, &o);
    *co = (BigWord)o;
    return (BigWord)s;
#else
    return bits_sub(x, y, c, co);
#endif
}

/* z1<<_W + z0 = x*y + c, with the carry folded in. */
static inline BigWord big_mul_add_www(BigWord x, BigWord y, BigWord c, BigWord *z0) {
#if BIG_WIDE
    BigDWord t = (BigDWord)x * y + c;
    *z0 = (BigWord)t;
    return (BigWord)(t >> 64);
#else
    BigWord lo;
    BigWord hi = bits_mul(x, y, &lo);
    Uint cc;
    lo = bits_add(lo, c, 0, &cc);
    *z0 = lo;
    return hi + cc;
#endif
}

/* z1<<_W + z0 = x*y + a + c. This never overflows two words, because
 * (2^W-1)^2 + 2(2^W-1) is 2^2W - 1. */
static inline BigWord big_mul_add2_www(BigWord x, BigWord y, BigWord a, BigWord c,
                                       BigWord *z0) {
#if BIG_WIDE
    BigDWord t = (BigDWord)x * y + a + c;
    *z0 = (BigWord)t;
    return (BigWord)(t >> 64);
#else
    BigWord lo;
    BigWord hi = big_mul_add_www(x, y, a, &lo);
    Uint cc;
    *z0 = bits_add(lo, c, 0, &cc);
    return hi + cc;
#endif
}

BigWord big_add_vv(Nat z, Nat x, Nat y) {
    BigWord *zp = z.p;
    const BigWord *xp = x.p, *yp = y.p;
    Int n = z.len, i = 0;
    BigWord c = 0;
    for (; i + 8 <= n; i += 8) {
        BigWord x0 = xp[i], x1 = xp[i + 1], x2 = xp[i + 2], x3 = xp[i + 3];
        BigWord x4 = xp[i + 4], x5 = xp[i + 5], x6 = xp[i + 6], x7 = xp[i + 7];
        BigWord y0 = yp[i], y1 = yp[i + 1], y2 = yp[i + 2], y3 = yp[i + 3];
        BigWord y4 = yp[i + 4], y5 = yp[i + 5], y6 = yp[i + 6], y7 = yp[i + 7];
        zp[i] = big_addc(x0, y0, c, &c);
        zp[i + 1] = big_addc(x1, y1, c, &c);
        zp[i + 2] = big_addc(x2, y2, c, &c);
        zp[i + 3] = big_addc(x3, y3, c, &c);
        zp[i + 4] = big_addc(x4, y4, c, &c);
        zp[i + 5] = big_addc(x5, y5, c, &c);
        zp[i + 6] = big_addc(x6, y6, c, &c);
        zp[i + 7] = big_addc(x7, y7, c, &c);
    }
    for (; i < n; i++)
        zp[i] = big_addc(xp[i], yp[i], c, &c);
    return c;
}

BigWord big_sub_vv(Nat z, Nat x, Nat y) {
    BigWord *zp = z.p;
    const BigWord *xp = x.p, *yp = y.p;
    Int n = z.len, i = 0;
    BigWord c = 0;
    for (; i + 8 <= n; i += 8) {
        BigWord x0 = xp[i], x1 = xp[i + 1], x2 = xp[i + 2], x3 = xp[i + 3];
        BigWord x4 = xp[i + 4], x5 = xp[i + 5], x6 = xp[i + 6], x7 = xp[i + 7];
        BigWord y0 = yp[i], y1 = yp[i + 1], y2 = yp[i + 2], y3 = yp[i + 3];
        BigWord y4 = yp[i + 4], y5 = yp[i + 5], y6 = yp[i + 6], y7 = yp[i + 7];
        zp[i] = big_subc(x0, y0, c, &c);
        zp[i + 1] = big_subc(x1, y1, c, &c);
        zp[i + 2] = big_subc(x2, y2, c, &c);
        zp[i + 3] = big_subc(x3, y3, c, &c);
        zp[i + 4] = big_subc(x4, y4, c, &c);
        zp[i + 5] = big_subc(x5, y5, c, &c);
        zp[i + 6] = big_subc(x6, y6, c, &c);
        zp[i + 7] = big_subc(x7, y7, c, &c);
    }
    for (; i < n; i++)
        zp[i] = big_subc(xp[i], yp[i], c, &c);
    return c;
}

BigWord big_add_vw(Nat z, Nat x, BigWord y) {
    if (z.len == 0)
        return y;
    Uint cc;
    z.p[0] = bits_add(x.p[0], y, 0, &cc);
    if (cc == 0) {
        if (z.p != x.p)
            memmove(z.p + 1, x.p + 1, (size_t)(z.len - 1) * sizeof(BigWord));
        return 0;
    }
    for (Int i = 1; i < z.len; i++) {
        BigWord xi = x.p[i];
        if (xi != BIG_M) {
            z.p[i] = xi + 1;
            if (z.p != x.p)
                memmove(z.p + i + 1, x.p + i + 1,
                        (size_t)(z.len - i - 1) * sizeof(BigWord));
            return 0;
        }
        z.p[i] = 0;
    }
    return 1;
}

BigWord big_sub_vw(Nat z, Nat x, BigWord y) {
    if (z.len == 0)
        return y;
    Uint cc;
    z.p[0] = bits_sub(x.p[0], y, 0, &cc);
    if (cc == 0) {
        if (z.p != x.p)
            memmove(z.p + 1, x.p + 1, (size_t)(z.len - 1) * sizeof(BigWord));
        return 0;
    }
    for (Int i = 1; i < z.len; i++) {
        BigWord xi = x.p[i];
        if (xi != 0) {
            z.p[i] = xi - 1;
            if (z.p != x.p)
                memmove(z.p + i + 1, x.p + i + 1,
                        (size_t)(z.len - i - 1) * sizeof(BigWord));
            return 0;
        }
        z.p[i] = BIG_M;
    }
    return 1;
}

/* The shifts run in the direction that lets z and x be the same vector. */
BigWord big_lsh_vu(Nat z, Nat x, Uint s) {
    if (s == 0) {
        nat_copy(z, x);
        return 0;
    }
    if (z.len == 0)
        return 0;
    s &= BIG_W - 1;
    Uint t = (BIG_W - s) & (BIG_W - 1);
    BigWord *zp = z.p;
    const BigWord *xp = x.p;
    Int n = z.len;
    BigWord c = xp[n - 1] >> t;
    for (Int i = n - 1; i > 0; i--)
        zp[i] = xp[i] << s | xp[i - 1] >> t;
    zp[0] = xp[0] << s;
    return c;
}

BigWord big_rsh_vu(Nat z, Nat x, Uint s) {
    if (s == 0) {
        nat_copy(z, x);
        return 0;
    }
    if (z.len == 0)
        return 0;
    s &= BIG_W - 1;
    Uint t = (BIG_W - s) & (BIG_W - 1);
    BigWord *zp = z.p;
    const BigWord *xp = x.p;
    Int n = z.len;
    BigWord c = xp[0] << t;
    for (Int i = 1; i < n; i++)
        zp[i - 1] = xp[i - 1] >> s | xp[i] << t;
    zp[n - 1] = xp[n - 1] >> s;
    return c;
}

/* The two loops below take four words at a time and keep two carry chains,
 * one adding the low halves of the products and one adding each high half into
 * the next word, so that a word waits for the previous word's carry only once
 * where it would otherwise wait twice. That is what Go's assembly does. The
 * carry out of a block is small enough to fit a word, because four words of
 * x*y + z + c are less than 2^(5W). */
BigWord big_mul_add_vww(Nat z, Nat x, BigWord y, BigWord r) {
    BigWord *zp = z.p;
    const BigWord *xp = x.p;
    Int n = z.len, i = 0;
    BigWord c = r;
    for (; i + 4 <= n; i += 4) {
        BigWord l0, l1, l2, l3, cb;
        BigWord h0 = big_mul_ww(xp[i], y, &l0);
        BigWord h1 = big_mul_ww(xp[i + 1], y, &l1);
        BigWord h2 = big_mul_ww(xp[i + 2], y, &l2);
        BigWord h3 = big_mul_ww(xp[i + 3], y, &l3);
        zp[i] = big_addc(l0, c, 0, &cb);
        zp[i + 1] = big_addc(l1, h0, cb, &cb);
        zp[i + 2] = big_addc(l2, h1, cb, &cb);
        zp[i + 3] = big_addc(l3, h2, cb, &cb);
        c = h3 + cb;
    }
    for (; i < n; i++)
        c = big_mul_add_www(xp[i], y, c, &zp[i]);
    return c;
}

BigWord big_add_mul_vvww(Nat z, Nat x, Nat y, BigWord m, BigWord a) {
    BigWord *zp = z.p;
    const BigWord *xp = x.p, *yp = y.p;
    Int n = z.len, i = 0;
    BigWord c = a;
    for (; i + 4 <= n; i += 4) {
        BigWord l0, l1, l2, l3, ca, cb;
        BigWord h0 = big_mul_ww(yp[i], m, &l0);
        BigWord h1 = big_mul_ww(yp[i + 1], m, &l1);
        BigWord h2 = big_mul_ww(yp[i + 2], m, &l2);
        BigWord h3 = big_mul_ww(yp[i + 3], m, &l3);
        BigWord s0 = big_addc(xp[i], l0, 0, &ca);
        BigWord s1 = big_addc(xp[i + 1], l1, ca, &ca);
        BigWord s2 = big_addc(xp[i + 2], l2, ca, &ca);
        BigWord s3 = big_addc(xp[i + 3], l3, ca, &ca);
        zp[i] = big_addc(s0, c, 0, &cb);
        zp[i + 1] = big_addc(s1, h0, cb, &cb);
        zp[i + 2] = big_addc(s2, h1, cb, &cb);
        zp[i + 3] = big_addc(s3, h2, cb, &cb);
        c = h3 + ca + cb;
    }
    for (; i < n; i++)
        c = big_mul_add2_www(yp[i], m, xp[i], c, &zp[i]);
    return c;
}

/* q = (x1<<_W + x0 - r)/y, with m the reciprocal of y from
 * big_reciprocal_word. x1 has to be less than y. Möller and Granlund,
 * "Improved division by invariant integers". */
BigWord big_div_ww(BigWord x1, BigWord x0, BigWord y, BigWord m, BigWord *r) {
    Uint s = big_nlz(y);
    if (s != 0) {
        /* y is not 0, so s is less than _W. */
        /* NOLINTNEXTLINE(clang-analyzer-core.BitwiseShift) */
        x1 = x1 << s | x0 >> (BIG_W - s);
        x0 <<= s;
        y <<= s;
    }
    Uint d = y;
    Uint t0;
    Uint t1 = bits_mul(m, x1, &t0);
    Uint c;
    bits_add(t0, x0, 0, &c);
    Uint ignore;
    t1 = bits_add(t1, x1, c, &ignore);
    Uint qq = t1;
    Uint dq0;
    Uint dq1 = bits_mul(d, qq, &dq0);
    Uint b;
    Uint r0 = bits_sub(x0, dq0, 0, &b);
    Uint r1 = bits_sub(x1, dq1, b, &ignore);
    if (r1 != 0) {
        qq++;
        r0 -= d;
    }
    if (r0 >= d) {
        qq++;
        r0 -= d;
    }
    *r = r0 >> s;
    return qq;
}

/* The reciprocal of the divisor, normalised, for big_div_ww. */
BigWord big_reciprocal_word(BigWord d1) {
    Uint u = d1 << big_nlz(d1);
    Uint x1 = ~u;
    Uint x0 = BIG_M;
    Uint rem;
    return bits_div(x1, x0, u, &rem);
}
