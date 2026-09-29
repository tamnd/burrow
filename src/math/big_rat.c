/* math/big: rational numbers, rat.go, ratconv.go and ratmarsh.go.
 *
 * The br_ functions are Go's Rat methods as written, over the Int and nat
 * layers and their scratch memory. The big_rat_ functions around them are the
 * public API, and they work the way the Int ones do, with two receivers to
 * commit instead of one.
 *
 * Copyright 2010 The Go Authors. All rights reserved.
 * Copyright 2026 The burrow Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style licence that can be found
 * in the LICENSE file. */

#include "big_internal.h"

#include "burrow/declare.h"
#include "burrow/encoding.h"
#include "burrow/io.h"
#include "burrow/mem/heap.h"
#include "burrow/panic.h"
#include "burrow/strconv.h"

#include <math.h>

/* A zero Int in scratch memory, Go's new(Int). */
#define BI_ZERO ((BigInt){NULL, false, {NULL, 0, 0}})

static void br_division_by_zero(void) {
    panic_str(BURROW_S("division by zero"));
}

/* --------------------------------------------------------------- rat.go */

/* norm: z in lowest terms, with a denominator of 1 for an integer. */
void br_norm(BigRat *z) {
    /* Go only ever reads the denominator's magnitude; keeping its sign clear
       here means the C side, which prints and compares the whole Int, agrees. */
    z->b.neg = false;
    if (z->a.abs.len == 0) {
        /* z == 0; normalize sign and denominator */
        z->a.neg = false;
        z->b.abs = nat_set_word(z->b.abs, 1);
        return;
    }
    if (z->b.abs.len == 0) {
        /* z is integer; normalize denominator */
        z->b.abs = nat_set_word(z->b.abs, 1);
        return;
    }
    /* z is fraction; normalize numerator and denominator */
    bool neg = z->a.neg;
    z->a.neg = false;
    BigInt f = BI_ZERO;
    bi_gcd(&f, NULL, NULL, &z->a, &z->b);
    if (nat_cmp(f.abs, NAT_ONE) != 0) {
        Nat r;
        z->a.abs = nat_div(z->a.abs, NAT_NIL, z->a.abs, f.abs, &r);
        z->b.abs = nat_div(z->b.abs, NAT_NIL, z->b.abs, f.abs, &r);
    }
    z->a.neg = neg;
}

/* mulDenom: z = x*y, where a denominator with no words is 1. */
static Nat br_mul_denom(Nat z, Nat x, Nat y) {
    if (x.len == 0 && y.len == 0)
        return nat_set_word(z, 1);
    if (x.len == 0)
        return nat_set(z, y);
    if (y.len == 0)
        return nat_set(z, x);
    return nat_mul(z, x, y);
}

/* scaleDenom: z = x*f, where an f with no words is 1. */
static void br_scale_denom(BigInt *z, const BigInt *x, Nat f) {
    if (f.len == 0) {
        bi_set(z, x);
        return;
    }
    z->abs = nat_mul(z->abs, x->abs, f);
    z->neg = x->neg;
}

static bool br_set_float64(BigRat *z, double f) {
    const int exp_mask = (1 << 11) - 1;
    uint64_t bits;
    memcpy(&bits, &f, sizeof bits);
    uint64_t mantissa = bits & (((uint64_t)1 << 52) - 1);
    int exp = (int)((bits >> 52) & (uint64_t)exp_mask);
    if (exp == exp_mask) /* non-finite */
        return false;
    if (exp == 0) { /* denormal */
        exp -= 1022;
    } else { /* normal */
        mantissa |= (uint64_t)1 << 52;
        exp -= 1023;
    }

    int shift = 52 - exp;

    /* Optimization (?): partially pre-normalise. */
    while ((mantissa & 1) == 0 && shift > 0) {
        mantissa >>= 1;
        shift--;
    }

    bi_set_uint64(&z->a, mantissa);
    z->a.neg = f < 0;
    bi_set(&z->b, &(const BigInt){NULL, false, NAT_ONE});
    if (shift > 0)
        bi_lsh(&z->b, &z->b, (Uint)shift);
    else
        bi_lsh(&z->a, &z->a, (Uint)-shift);
    br_norm(z);
    return true;
}

static uint64_t br_low64(Nat x) {
    if (x.len == 0)
        return 0;
    uint64_t v = x.p[0];
#if BITS_UINT_SIZE == 32
    if (x.len > 1)
        v |= (uint64_t)x.p[1] << 32;
#endif
    return v;
}

/* quotToFloat32 and quotToFloat64 in one: the nearest float with an msize
 * bit mantissa and an ebias exponent bias to a/b, rounded half to even, as a
 * double, which holds a float32 result exactly. */
static double br_quot_to_float(Nat a, Nat b, int msize, int ebias, bool *exact) {
    const int msize1 = msize + 1; /* incl. implicit 1 */
    const int msize2 = msize1 + 1;
    const Int emin = 1 - ebias;

    Int alen = nat_bit_len(a);
    if (alen == 0) {
        *exact = true;
        return 0;
    }
    Int blen = nat_bit_len(b);
    if (blen == 0)
        br_division_by_zero();

    /* 1. Left-shift A or B such that quotient A/B is in [1<<Msize1, 1<<(Msize2+1)
     * (Msize2 bits if A < B when they are left-aligned, Msize2+1 bits if A >= B).
     * This is 2 or 3 more than the float mantissa field width of Msize:
     * - the optional extra bit is shifted away in step 3 below.
     * - the high-order 1 is omitted in "normal" representation;
     * - the low-order 1 will be used during rounding then discarded. */
    Int exp = alen - blen;
    Nat a2 = nat_set(NAT_NIL, a);
    Nat b2 = nat_set(NAT_NIL, b);
    Int shift = msize2 - exp;
    if (shift > 0)
        a2 = nat_lsh(a2, a2, (Uint)shift);
    else if (shift < 0)
        b2 = nat_lsh(b2, b2, (Uint)-shift);

    /* 2. Compute quotient and remainder (q, r).  NB: due to the
     * extra shift, the low-order bit of q is logically the
     * high-order bit of r. */
    Nat r;
    Nat q = nat_div(NAT_NIL, a2, a2, b2, &r); /* (recycle a2) */
    uint64_t mantissa = br_low64(q);
    bool have_rem = r.len > 0; /* mantissa&1 && !haveRem => remainder is exactly half */

    /* 3. If quotient didn't fit in Msize2 bits, redo division by b2<<1
     * (in effect---we accomplish this incrementally). */
    if (mantissa >> msize2 == 1) {
        if ((mantissa & 1) == 1)
            have_rem = true;
        mantissa >>= 1;
        exp++;
    }
    if (mantissa >> msize1 != 1)
        panic_str(fmt_sprintf_v(error_allocator(), "expected exactly %d bits of result",
                                msize2));

    /* 4. Rounding. */
    if (emin - msize <= exp && exp <= emin) {
        /* Denormal case; lose 'shift' bits of precision. */
        Uint sh = (Uint)(emin - (exp - 1)); /* [1..Esize1) */
        uint64_t lostbits = mantissa & (((uint64_t)1 << sh) - 1);
        have_rem = have_rem || lostbits != 0;
        mantissa >>= sh;
        exp = 2 - ebias; /* == exp + shift */
    }
    /* Round q using round-half-to-even. */
    *exact = !have_rem;
    if ((mantissa & 1) != 0) {
        *exact = false;
        if (have_rem || (mantissa & 2) != 0) {
            if (++mantissa >= (uint64_t)1 << msize2) {
                /* Complete rollover 11...1 => 100...0, so shift is safe */
                mantissa >>= 1;
                exp++;
            }
        }
    }
    mantissa >>= 1; /* discard rounding bit.  Mantissa now scaled by 1<<Msize1. */

    /* ldexp takes an int, and anything past a few thousand is an infinity or
     * zero either way. */
    Int e = exp - msize1;
    if (e > 10000)
        e = 10000;
    else if (e < -10000)
        e = -10000;
    return ldexp((double)mantissa, (int)e);
}

static void br_set_frac(BigRat *z, const BigInt *a, const BigInt *b) {
    z->a.neg = a->neg != b->neg;
    Nat babs = b->abs;
    if (babs.len == 0)
        br_division_by_zero();
    if (&z->a == b || nat_alias(z->a.abs, babs))
        babs = nat_set(NAT_NIL, babs); /* make a copy */
    z->a.abs = nat_set(z->a.abs, a->abs);
    z->b.abs = nat_set(z->b.abs, babs);
    br_norm(z);
}

static void br_set_frac64(BigRat *z, int64_t a, int64_t b) {
    if (b == 0)
        br_division_by_zero();
    bi_set_int64(&z->a, a);
    uint64_t ub = (uint64_t)b;
    if (b < 0) {
        ub = 0 - ub;
        z->a.neg = !z->a.neg;
    }
    z->b.abs = nat_set_uint64(z->b.abs, ub);
    br_norm(z);
}

static void br_set(BigRat *z, const BigRat *x) {
    if (z != x) {
        bi_set(&z->a, &x->a);
        bi_set(&z->b, &x->b);
    }
    if (z->b.abs.len == 0)
        z->b.abs = nat_set_word(z->b.abs, 1);
}

static bool br_is_int(const BigRat *x) {
    return x->b.abs.len == 0 || nat_cmp(x->b.abs, NAT_ONE) == 0;
}

static void br_add(BigRat *z, const BigRat *x, const BigRat *y, bool sub) {
    BigInt a1 = BI_ZERO, a2 = BI_ZERO;
    br_scale_denom(&a1, &x->a, y->b.abs);
    br_scale_denom(&a2, &y->a, x->b.abs);
    if (sub)
        bi_sub(&z->a, &a1, &a2);
    else
        bi_add(&z->a, &a1, &a2);
    z->b.abs = br_mul_denom(z->b.abs, x->b.abs, y->b.abs);
    br_norm(z);
}

static void br_mul(BigRat *z, const BigRat *x, const BigRat *y) {
    if (x == y) {
        /* a squared Rat is positive and can't be reduced (no need to call norm()) */
        z->a.neg = false;
        z->a.abs = nat_sqr(z->a.abs, x->a.abs);
        if (x->b.abs.len == 0)
            z->b.abs = nat_set_word(z->b.abs, 1);
        else
            z->b.abs = nat_sqr(z->b.abs, x->b.abs);
        return;
    }
    bi_mul(&z->a, &x->a, &y->a);
    z->b.abs = br_mul_denom(z->b.abs, x->b.abs, y->b.abs);
    br_norm(z);
}

static void br_quo(BigRat *z, const BigRat *x, const BigRat *y) {
    if (y->a.abs.len == 0)
        br_division_by_zero();
    BigInt a = BI_ZERO, b = BI_ZERO;
    br_scale_denom(&a, &x->a, y->b.abs);
    br_scale_denom(&b, &y->a, x->b.abs);
    z->a.abs = a.abs;
    z->b.abs = b.abs;
    z->a.neg = a.neg != b.neg;
    br_norm(z);
}

/* ----------------------------------------------------------- ratconv.go */

Error big_scan_exponent(BigScanner *r, bool base2ok, bool sep_ok, int64_t *exp,
                        int *base) {
    *exp = 0;
    *base = 10;
    /* one char look-ahead */
    Byte ch;
    Error err = big_scanner_read(r, &ch);
    if (BURROW_FAILED(err)) {
        if (errors_is(err, io_eof))
            err = BURROW_NO_ERROR;
        return err;
    }

    /* exponent char */
    switch (ch) {
    case 'e':
    case 'E':
        *base = 10;
        break;
    case 'p':
    case 'P':
        if (base2ok) {
            *base = 2;
            break; /* ok */
        }
        /* binary exponent not permitted */
        big_scanner_unread(r);
        return BURROW_NO_ERROR;
    default:
        big_scanner_unread(r); /* ch does not belong to exponent anymore */
        return BURROW_NO_ERROR;
    }

    /* sign */
    Slice digits = slice_nil(TYPE_BYTE);
    Alloc *sa = big_scratch_alloc();
    err = big_scanner_read(r, &ch);
    if (BURROW_OK(err) && (ch == '+' || ch == '-')) {
        if (ch == '-')
            digits = big_append_str(sa, digits, BURROW_S("-"));
        err = big_scanner_read(r, &ch);
    }

    /* prev encodes the previously seen char: it is one
     * of '_', '0' (a digit), or '.' (anything else). A
     * valid separator '_' may only occur after a digit. */
    int prev = '.';
    bool inval_sep = false;

    /* exponent value */
    bool has_digits = false;
    while (BURROW_OK(err)) {
        if ('0' <= ch && ch <= '9') {
            digits = big_append_str(sa, digits, (Str){&ch, 1});
            prev = '0';
            has_digits = true;
        } else if (ch == '_' && sep_ok) {
            if (prev != '0')
                inval_sep = true;
            prev = '_';
        } else {
            big_scanner_unread(r); /* ch does not belong to number anymore */
            break;
        }
        err = big_scanner_read(r, &ch);
    }

    if (errors_is(err, io_eof))
        err = BURROW_NO_ERROR;
    if (BURROW_OK(err) && !has_digits)
        err = big_err_no_digits;
    if (BURROW_OK(err))
        *exp = strconv_parse_int((Str){digits.p, digits.len}, 10, 64, &err);
    /* other errors take precedence over invalid separators */
    if (BURROW_OK(err) && (inval_sep || prev == '_'))
        err = big_err_inval_sep;
    return err;
}

static bool br_rat_tok(void *env, Rune ch) {
    (void)env;
    switch (ch) {
    case '+':
    case '-':
    case '/':
    case '.':
    case 'e':
    case 'E':
        return true;
    default:
        return '0' <= ch && ch <= '9';
    }
}

static bool br_at_eof(BigScanner *r) {
    Byte ch;
    return errors_is(big_scanner_read(r, &ch), io_eof);
}

static bool br_set_string(BigRat *z, Str s) {
    if (s.len == 0)
        return false;

    /* parse fraction a/b, if any */
    Int sep = -1;
    for (Int i = 0; i < s.len; i++) {
        if (s.p[i] == '/') {
            sep = i;
            break;
        }
    }
    BigScanner r;
    if (sep >= 0) {
        big_scanner_init(&r, (Str){s.p, sep});
        if (!bi_set_from_scanner(&z->a, &r, 0))
            return false;
        big_scanner_init(&r, (Str){s.p + sep + 1, s.len - sep - 1});
        int b;
        Int count;
        Error err;
        z->b.abs = nat_scan(z->b.abs, &r, 0, false, &b, &count, &err);
        if (BURROW_FAILED(err))
            return false;
        /* entire string must have been consumed */
        if (!br_at_eof(&r))
            return false;
        if (z->b.abs.len == 0)
            return false;
        br_norm(z);
        return true;
    }

    /* parse floating-point number */
    big_scanner_init(&r, s);

    /* sign */
    bool neg;
    if (BURROW_FAILED(bi_scan_sign(&r, &neg)))
        return false;

    /* mantissa */
    int base;
    Int fcount; /* fractional digit count; valid if <= 0 */
    Error err;
    z->a.abs = nat_scan(z->a.abs, &r, 0, true, &base, &fcount, &err);
    if (BURROW_FAILED(err))
        return false;

    /* exponent */
    int64_t exp;
    int ebase;
    if (BURROW_FAILED(big_scan_exponent(&r, true, true, &exp, &ebase)))
        return false;

    /* there should be no unread characters left */
    if (!br_at_eof(&r))
        return false;

    /* special-case 0 (see also issue #16176) */
    if (z->a.abs.len == 0) {
        br_norm(z);
        return true;
    }
    /* len(z.a.abs) > 0 */

    /* The mantissa may have a radix point (fcount <= 0) and there
     * may be a nonzero exponent exp. The radix point amounts to a
     * division by base**(-fcount), which equals a multiplication by
     * base**fcount. An exponent means multiplication by ebase**exp.
     * Multiplications are commutative, so we can apply them in any
     * order. We only have powers of 2 and 10, and we split powers
     * of 10 into the product of the same powers of 2 and 5. This
     * may reduce the size of shift/multiplication factors or
     * divisors required to create the final fraction, depending
     * on the actual floating-point value. */

    /* determine binary or decimal exponent contribution of radix point */
    int64_t exp2 = 0, exp5 = 0;
    if (fcount < 0) {
        /* The mantissa has a radix point ddd.dddd; and
         * -fcount is the number of digits to the right
         * of '.'. Adjust relevant exponent accordingly. */
        int64_t d = (int64_t)fcount;
        switch (base) {
        case 10:
            exp5 = d;
            exp2 = d; /* 10**e == 5**e * 2**e */
            break;
        case 2:
            exp2 = d;
            break;
        case 8:
            exp2 = d * 3; /* octal digits are 3 bits each */
            break;
        case 16:
            exp2 = d * 4; /* hexadecimal digits are 4 bits each */
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
        exp2 += exp; /* see the case 10 above */
        break;
    case 2:
        exp2 += exp;
        break;
    default:
        panic_str(BURROW_S("unexpected exponent base"));
    }
    /* exp consumed - not needed anymore */

    /* apply exp5 contributions
     * (start with exp5 so the numbers to multiply are smaller) */
    if (exp5 != 0) {
        int64_t n = exp5;
        if (n < 0) {
            if (n == INT64_MIN)
                return false; /* some sort of overflow */
            n = -n;
        }
        if (n > 1000000)
            return false; /* avoid excessively large exponents */
        Nat pow5 = nat_exp_nn(z->b.abs, NAT_FIVE, nat_set_word(NAT_NIL, (BigWord)n),
                              NAT_NIL, false); /* use underlying array of z.b.abs */
        if (exp5 > 0) {
            z->a.abs = nat_mul(z->a.abs, z->a.abs, pow5);
            z->b.abs = nat_set_word(z->b.abs, 1);
        } else {
            z->b.abs = pow5;
        }
    } else {
        z->b.abs = nat_set_word(z->b.abs, 1);
    }

    /* apply exp2 contributions */
    if (exp2 < -10000000 || exp2 > 10000000)
        return false; /* avoid excessively large exponents */
    if (exp2 > 0)
        z->a.abs = nat_lsh(z->a.abs, z->a.abs, (Uint)exp2);
    else if (exp2 < 0)
        z->b.abs = nat_lsh(z->b.abs, z->b.abs, (Uint)-exp2);

    z->a.neg = neg && z->a.abs.len > 0; /* 0 has no sign */

    br_norm(z);
    return true;
}

/* marshal: "a/b" on the end of buf. */
static Slice br_marshal(const BigRat *x, Alloc *a, Slice buf) {
    buf = big_append_str(a, buf, nat_itoa(x->a.abs, x->a.neg, 10));
    buf = big_append_str(a, buf, BURROW_S("/"));
    if (x->b.abs.len != 0)
        return big_append_str(a, buf, nat_itoa(x->b.abs, x->b.neg, 10));
    return big_append_str(a, buf, BURROW_S("1"));
}

static Str br_str(Slice s) {
    return (Str){s.p, s.len};
}

/* The bytes in scratch memory as a string from a. */
static Str br_clone(Alloc *a, Slice s) {
    return str_clone(a, br_str(s));
}

static Str br_float_string(const BigRat *x, Int prec) {
    Alloc *sa = big_scratch_alloc();
    Slice buf = slice_nil(TYPE_BYTE);

    if (br_is_int(x)) {
        buf = big_append_str(sa, buf, nat_itoa(x->a.abs, x->a.neg, 10));
        if (prec > 0) {
            buf = big_append_str(sa, buf, BURROW_S("."));
            for (Int i = prec; i > 0; i--)
                buf = big_append_str(sa, buf, BURROW_S("0"));
        }
        return br_str(buf);
    }
    /* x.b.abs != 0 */

    Nat r;
    Nat q = nat_div(NAT_NIL, NAT_NIL, x->a.abs, x->b.abs, &r);

    Nat p = NAT_ONE;
    if (prec > 0)
        p = nat_exp_nn(NAT_NIL, NAT_TEN, nat_set_uint64(NAT_NIL, (uint64_t)prec),
                       NAT_NIL, false);

    r = nat_mul(r, r, p);
    Nat r2;
    r = nat_div(r, NAT_NIL, r, x->b.abs, &r2);

    /* see if we need to round up */
    r2 = nat_add(r2, r2, r2);
    if (nat_cmp(x->b.abs, r2) <= 0) {
        r = nat_add(r, r, NAT_ONE);
        if (nat_cmp(r, p) >= 0) {
            q = nat_add(NAT_NIL, q, NAT_ONE);
            r = nat_sub(NAT_NIL, r, p);
        }
    }

    if (x->a.neg)
        buf = big_append_str(sa, buf, BURROW_S("-"));
    buf = big_append_str(sa, buf, nat_utoa(q, 10)); /* itoa ignores sign if q == 0 */

    if (prec > 0) {
        buf = big_append_str(sa, buf, BURROW_S("."));
        Str rs = nat_utoa(r, 10);
        for (Int i = prec - rs.len; i > 0; i--)
            buf = big_append_str(sa, buf, BURROW_S("0"));
        buf = big_append_str(sa, buf, rs);
    }

    return br_str(buf);
}

static Int br_float_prec(const BigRat *x, bool *exact) {
    /* Go's Denom, without the Int around it. */
    Nat d = x->b.abs.len == 0 ? NAT_ONE : x->b.abs; /* d >= 1 */

    /* Determine p2 by counting factors of 2.
     * p2 corresponds to the trailing zero bits in d.
     * Do this first to reduce q as much as possible. */
    Nat q = NAT_NIL;
    Uint p2 = nat_trailing_zero_bits(d);
    q = nat_rsh(q, d, p2);

    /* Determine p5 by counting factors of 5.
     * Build a table starting with an initial power of 5,
     * and use repeated squaring until the factor doesn't
     * divide q anymore. Then use the table to determine
     * the power of 5 in q. */
    enum { fp = 13 }; /* f == 5^fp */
    Nat tab[64];      /* tab[i] == (5^fp)^(2^i) == 5^(fp·2^i) */
    int ntab = 0;     /* tab entries */
    Nat f =
        nat_set_word(NAT_NIL, 1220703125); /* == 5^fp (must fit into a uint32 Word) */
    Nat t = NAT_NIL, r = NAT_NIL;          /* temporaries */
    for (;;) {
        (void)nat_div(t, r, q, f, &r);
        if (r.len != 0)
            break; /* f doesn't divide q evenly */
        tab[ntab++] = f;
        f = nat_sqr(NAT_NIL, f); /* nat(nil) to ensure a new f for each table entry */
    }

    /* Factor q using the table entries, if any.
     * We start with the largest factor f = tab[len(tab)-1]
     * that evenly divides q. It does so at most once because
     * otherwise f·f would also divide q. That can't be true
     * because f·f is the next higher table entry, contradicting
     * how f was chosen in the first place.
     * The same reasoning applies to the subsequent factors. */
    Uint p5 = 0;
    for (int i = ntab - 1; i >= 0; i--) {
        t = nat_div(t, r, q, tab[i], &r);
        if (r.len == 0) {
            p5 += (Uint)fp * ((Uint)1 << i); /* tab[i] == 5^(fp·2^i) */
            q = nat_set(q, t);
        }
    }

    /* If fp != 1, we may still have multiples of 5 left. */
    for (;;) {
        t = nat_div(t, r, q, NAT_FIVE, &r);
        if (r.len != 0)
            break;
        p5++;
        q = nat_set(q, t);
    }

    *exact = nat_cmp(q, NAT_ONE) == 0;
    return (Int)(p2 > p5 ? p2 : p5);
}

/* ---------------------------------------------------------- public: Rat */

static bool br_inside(Nat x, Nat buf) {
    uintptr_t p = (uintptr_t)x.p, b = (uintptr_t)buf.p;
    return x.p != NULL && buf.p != NULL && p >= b &&
           p < b + (uintptr_t)buf.cap * sizeof(BigWord);
}

static Nat br_scratch_copy(Nat x) {
    Nat c = {big_alloc(x.len), x.len, x.len};
    nat_copy(c, x);
    return c;
}

/* Both halves of z back in their own memory. Inv swaps the halves' words,
 * and a half that ended up in the other's buffer moves out to scratch memory
 * first, so that neither commit writes over what the other still has to
 * copy. */
static BigRat *br_done(BigRat *z, Nat oa, Nat ob) {
    if (br_inside(z->a.abs, ob))
        z->a.abs = br_scratch_copy(z->a.abs);
    if (br_inside(z->b.abs, oa))
        z->b.abs = br_scratch_copy(z->b.abs);
    big_commit(&z->a.abs, z->a.a, oa);
    big_commit(&z->b.abs, z->b.a, ob);
    big_leave();
    return z;
}

/* A public function with receiver z. */
#define BR_OP(z, ...)                                                                  \
    do {                                                                               \
        big_enter();                                                                   \
        Nat oa_ = (z)->a.abs, ob_ = (z)->b.abs;                                        \
        __VA_ARGS__;                                                                   \
        return br_done((z), oa_, ob_);                                                 \
    } while (0)

void big_rat_free(BigRat *x) {
    if (x == NULL)
        return;
    big_int_free(&x->a);
    big_int_free(&x->b);
}

BigRat *big_new_rat(Alloc *a, int64_t num, int64_t den) {
    BigRat *z =
        mem_alloc(a != NULL ? a : heap_allocator(), sizeof(BigRat), _Alignof(BigRat));
    if (z == NULL)
        big_oom();
    *z = BIG_RAT(a);
    return big_rat_set_frac64(z, num, den);
}

BigRat *big_rat_set_float64(BigRat *z, double f) {
    big_enter();
    Nat oa = z->a.abs, ob = z->b.abs;
    bool ok = br_set_float64(z, f);
    br_done(z, oa, ob);
    return ok ? z : NULL;
}

float big_rat_float32(const BigRat *x, bool *exact) {
    Nat b = x->b.abs.len == 0 ? NAT_ONE : x->b.abs;
    bool e;
    big_enter();
    float f = (float)br_quot_to_float(x->a.abs, b, 23, 127, &e);
    big_leave();
    if (f == INFINITY) /* f is the magnitude, so only +Inf can come out */
        e = false;
    if (x->a.neg)
        f = -f;
    if (exact != NULL)
        *exact = e;
    return f;
}

double big_rat_float64(const BigRat *x, bool *exact) {
    Nat b = x->b.abs.len == 0 ? NAT_ONE : x->b.abs;
    bool e;
    big_enter();
    double f = br_quot_to_float(x->a.abs, b, 52, 1023, &e);
    big_leave();
    if (f == (double)INFINITY)
        e = false;
    if (x->a.neg)
        f = -f;
    if (exact != NULL)
        *exact = e;
    return f;
}

BigRat *big_rat_set_frac(BigRat *z, const BigInt *a, const BigInt *b) {
    BR_OP(z, br_set_frac(z, a, b));
}

BigRat *big_rat_set_frac64(BigRat *z, int64_t a, int64_t b) {
    BR_OP(z, br_set_frac64(z, a, b));
}

BigRat *big_rat_set_int(BigRat *z, const BigInt *x) {
    BR_OP(z, {
        bi_set(&z->a, x);
        z->b.abs = nat_set_word(z->b.abs, 1);
    });
}

BigRat *big_rat_set_int64(BigRat *z, int64_t x) {
    BR_OP(z, {
        bi_set_int64(&z->a, x);
        z->b.abs = nat_set_word(z->b.abs, 1);
    });
}

BigRat *big_rat_set_uint64(BigRat *z, uint64_t x) {
    BR_OP(z, {
        bi_set_uint64(&z->a, x);
        z->b.abs = nat_set_word(z->b.abs, 1);
    });
}

BigRat *big_rat_set(BigRat *z, const BigRat *x) {
    BR_OP(z, br_set(z, x));
}

BigRat *big_rat_abs(BigRat *z, const BigRat *x) {
    BR_OP(z, {
        br_set(z, x);
        z->a.neg = false;
    });
}

BigRat *big_rat_neg(BigRat *z, const BigRat *x) {
    BR_OP(z, {
        br_set(z, x);
        z->a.neg = z->a.abs.len > 0 && !z->a.neg; /* 0 has no sign */
    });
}

BigRat *big_rat_inv(BigRat *z, const BigRat *x) {
    if (x->a.abs.len == 0)
        br_division_by_zero();
    BR_OP(z, {
        br_set(z, x);
        Nat t = z->a.abs;
        z->a.abs = z->b.abs;
        z->b.abs = t;
    });
}

Int big_rat_sign(const BigRat *x) {
    return big_int_sign(&x->a);
}

bool big_rat_is_int(const BigRat *x) {
    return br_is_int(x);
}

BigInt *big_rat_num(BigRat *x) {
    return &x->a;
}

BigInt *big_rat_denom(BigRat *x) {
    /* Note that x.b.neg is guaranteed false. */
    if (x->b.abs.len == 0)
        big_int_set_int64(&x->b, 1);
    return &x->b;
}

Int big_rat_cmp(const BigRat *x, const BigRat *y) {
    big_enter();
    BigInt a = BI_ZERO, b = BI_ZERO;
    br_scale_denom(&a, &x->a, y->b.abs);
    br_scale_denom(&b, &y->a, x->b.abs);
    int r = bi_cmp(&a, &b);
    big_leave();
    return r;
}

BigRat *big_rat_add(BigRat *z, const BigRat *x, const BigRat *y) {
    BR_OP(z, br_add(z, x, y, false));
}

BigRat *big_rat_sub(BigRat *z, const BigRat *x, const BigRat *y) {
    BR_OP(z, br_add(z, x, y, true));
}

BigRat *big_rat_mul(BigRat *z, const BigRat *x, const BigRat *y) {
    BR_OP(z, br_mul(z, x, y));
}

BigRat *big_rat_quo(BigRat *z, const BigRat *x, const BigRat *y) {
    BR_OP(z, br_quo(z, x, y));
}

BigRat *big_rat_set_string(BigRat *z, Str s, bool *ok) {
    big_enter();
    Nat oa = z->a.abs, ob = z->b.abs;
    bool good = br_set_string(z, s);
    br_done(z, oa, ob);
    if (ok != NULL)
        *ok = good;
    return good ? z : NULL;
}

Str big_rat_string(const BigRat *x, Alloc *a) {
    big_enter();
    Str s = br_clone(a, br_marshal(x, big_scratch_alloc(), slice_nil(TYPE_BYTE)));
    big_leave();
    return s;
}

Str big_rat_rat_string(const BigRat *x, Alloc *a) {
    if (br_is_int(x))
        return big_int_string(&x->a, a);
    return big_rat_string(x, a);
}

Str big_rat_float_string(const BigRat *x, Alloc *a, Int prec) {
    big_enter();
    Str s = str_clone(a, br_float_string(x, prec));
    big_leave();
    return s;
}

Int big_rat_float_prec(const BigRat *x, bool *exact) {
    bool e;
    big_enter();
    Int n = br_float_prec(x, &e);
    big_leave();
    if (exact != NULL)
        *exact = e;
    return n;
}

Error big_rat_scan(BigRat *z, FmtScanState s, Rune ch) {
    Error err = BURROW_NO_ERROR;
    Slice tok = s.vt->token(s.data, true, (RuneFunc){br_rat_tok, NULL}, &err);
    if (BURROW_FAILED(err))
        return err;
    switch (ch) {
    case 'e':
    case 'f':
    case 'g':
    case 'E':
    case 'F':
    case 'G':
    case 'v':
        break;
    default:
        return errors_new(error_allocator(), BURROW_S("Rat.Scan: invalid verb"));
    }
    big_enter();
    Nat oa = z->a.abs, ob = z->b.abs;
    bool ok = br_set_string(z, br_str(tok));
    br_done(z, oa, ob);
    if (!ok)
        return errors_new(error_allocator(), BURROW_S("Rat.Scan: invalid syntax"));
    return BURROW_NO_ERROR;
}

/* ---------------------------------------------------------- ratmarsh.go */

/* Gob codec version. Permits backward-compatible changes to the encoding. */
#define BIG_RAT_GOB_VERSION 1

Slice big_rat_gob_encode(const BigRat *x, Alloc *a, Error *err) {
    BURROW_OUT(err, BURROW_NO_ERROR);
    if (x == NULL)
        return slice_nil(TYPE_BYTE);
    /* extra bytes for version and sign bit (1), and numerator length (4) */
    Int n = 1 + 4 + (x->a.abs.len + x->b.abs.len) * BIG_S;
    Slice buf = slice_make(a, TYPE_BYTE, n, n);
    if (buf.p == NULL)
        big_oom();
    Byte *p = buf.p;
    Int i = nat_bytes(x->b.abs, buf);
    Int j = nat_bytes(x->a.abs, slice_sub(buf, 0, i));
    Int num = i - j;
    if ((Int)(uint32_t)num != num) {
        mem_free(a, buf.p, (size_t)n, 1);
        BURROW_OUT(err, errors_new(error_allocator(),
                                   BURROW_S("Rat.GobEncode: numerator too large")));
        return slice_nil(TYPE_BYTE);
    }
    p[j - 4] = (Byte)((uint32_t)num >> 24);
    p[j - 3] = (Byte)((uint32_t)num >> 16);
    p[j - 2] = (Byte)((uint32_t)num >> 8);
    p[j - 1] = (Byte)num;
    j -= 1 + 4;
    Byte b = BIG_RAT_GOB_VERSION << 1; /* make space for sign bit */
    if (x->a.neg)
        b |= 1;
    p[j] = b;
    if (j > 0)
        memmove(p, p + j, (size_t)(n - j));
    buf.len = n - j;
    return buf;
}

Error big_rat_gob_decode(BigRat *z, Slice buf) {
    if (buf.len == 0) {
        /* Other side sent a nil or default value. */
        z->a.neg = false;
        z->a.abs.len = 0;
        z->b.neg = false;
        z->b.abs.len = 0;
        return BURROW_NO_ERROR;
    }
    if (buf.len < 5)
        return errors_new(error_allocator(),
                          BURROW_S("Rat.GobDecode: buffer too small"));
    const Byte *p = buf.p;
    Byte b = p[0];
    if (b >> 1 != BIG_RAT_GOB_VERSION)
        return fmt_errorf_v("Rat.GobDecode: encoding version %d not supported",
                            (int)(b >> 1));
    const Int j = 1 + 4;
    uint32_t ln = (uint32_t)p[1] << 24 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 8 |
                  (uint32_t)p[4];
    if ((uint64_t)ln > (uint64_t)(BURROW_INT_MAX - j))
        return errors_new(error_allocator(), BURROW_S("Rat.GobDecode: invalid length"));
    Int i = j + (Int)ln;
    if (buf.len < i)
        return errors_new(error_allocator(),
                          BURROW_S("Rat.GobDecode: buffer too small"));
    big_enter();
    Nat oa = z->a.abs, ob = z->b.abs;
    z->a.neg = (b & 1) != 0;
    z->a.abs = nat_set_bytes(z->a.abs, slice_sub(buf, j, i));
    z->b.abs = nat_set_bytes(z->b.abs, slice_sub(buf, i, buf.len));
    br_done(z, oa, ob);
    return BURROW_NO_ERROR;
}

Slice big_rat_append_text(const BigRat *x, Alloc *a, Slice b, Error *err) {
    BURROW_OUT(err, BURROW_NO_ERROR);
    if (br_is_int(x))
        return big_int_append(&x->a, a, b, 10);
    big_enter();
    Slice r = br_marshal(x, a, b);
    big_leave();
    return r;
}

Slice big_rat_marshal_text(const BigRat *x, Alloc *a, Error *err) {
    return big_rat_append_text(x, a, slice_nil(TYPE_BYTE), err);
}

Error big_rat_unmarshal_text(BigRat *z, Slice text) {
    Str s = {text.p, text.len};
    bool ok;
    big_rat_set_string(z, s, &ok);
    if (!ok)
        return fmt_errorf_v("math/big: cannot unmarshal %q into a *big.Rat", s);
    return BURROW_NO_ERROR;
}

/* ------------------------------------------------------------- the type */

static Slice big_rat_m_append_text(BigRat *self, Alloc *a, Slice b, Error *err) {
    return big_rat_append_text(self, a, b, err);
}

static Error big_rat_m_gob_decode(BigRat *self, Alloc *a, Slice buf) {
    (void)a;
    return big_rat_gob_decode(self, buf);
}

static Slice big_rat_m_gob_encode(BigRat *self, Alloc *a, Error *err) {
    return big_rat_gob_encode(self, a, err);
}

static Slice big_rat_m_marshal_text(BigRat *self, Alloc *a, Error *err) {
    return big_rat_marshal_text(self, a, err);
}

static Error big_rat_m_scan(BigRat *self, FmtScanState s, Rune ch) {
    return big_rat_scan(self, s, ch);
}

static Str big_rat_m_string(BigRat *self) {
    return big_rat_string(self, error_allocator());
}

static Error big_rat_m_unmarshal_text(BigRat *self, Alloc *a, Slice text) {
    (void)a;
    return big_rat_unmarshal_text(self, text);
}

#define BIG_RAT_SIG_SCAN(IN, OUT) IN(0, FmtScanState) IN(1, Rune) OUT(Error)
#define BIG_RAT_SIG_STRING(IN, OUT) OUT(Str)

#define BIG_RAT_METHODS(M, T)                                                          \
    M(T, AppendText, big_rat_m_append_text, ENCODING_SIG_APPEND_TEXT)                  \
    M(T, GobDecode, big_rat_m_gob_decode, ENCODING_SIG_UNMARSHAL_BINARY)               \
    M(T, GobEncode, big_rat_m_gob_encode, ENCODING_SIG_MARSHAL_BINARY)                 \
    M(T, MarshalText, big_rat_m_marshal_text, ENCODING_SIG_MARSHAL_TEXT)               \
    M(T, Scan, big_rat_m_scan, BIG_RAT_SIG_SCAN)                                       \
    M(T, String, big_rat_m_string, BIG_RAT_SIG_STRING)                                 \
    M(T, UnmarshalText, big_rat_m_unmarshal_text, ENCODING_SIG_UNMARSHAL_TEXT)

BURROW_METHODS_DEFINE(BigRat, BIG_RAT_METHODS);

const Type burrow_type_BigRat = {
    {(const Byte *)"Rat", 3},
    {(const Byte *)"math/big", 8},
    KIND_STRUCT,
    (uint32_t)sizeof(BigRat),
    (uint16_t)_Alignof(BigRat),
    0,
    (uint16_t)(sizeof burrow__methods_BigRat / sizeof burrow__methods_BigRat[0]),
    NULL,
    burrow__methods_BigRat,
    NULL,
    NULL,
    0,
    0x62696772U, /* "bigr" */
    NULL,
};

const Type *const TYPE_BIG_RAT = &burrow_type_BigRat;
