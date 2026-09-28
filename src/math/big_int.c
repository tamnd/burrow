/* math/big: signed integers, int.go, intconv.go, intmarsh.go and prime.go.
 *
 * The bi_ functions are Go's Int methods as written, over the nat layer and
 * its scratch memory. The big_int_ functions around them are the public API:
 * each one enters, reserves room in the receiver where that saves a copy, runs
 * the bi_ function, commits every receiver it changed and leaves.
 *
 * Copyright 2009 The Go Authors. All rights reserved.
 * Copyright 2026 The burrow Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style licence that can be found
 * in the LICENSE file. */

#include "big_internal.h"

#include "burrow/declare.h"
#include "burrow/encoding.h"
#include "burrow/io.h"
#include "burrow/math.h"
#include "burrow/mem/heap.h"
#include "burrow/panic.h"

#include <math.h>

#define INT_ONE (&(const BigInt){NULL, false, NAT_ONE})

/* A zero Int in scratch memory, Go's new(Int). */
#define BI_ZERO ((BigInt){NULL, false, {NULL, 0, 0}})

/* A base from the API as the int the conversions take. One that does not fit is
 * as invalid as it is, and -1 says so. */
static int big_base(Int base) {
    return base < 0 || base > 64 ? -1 : (int)base;
}

static inline Int big_max_int(Int a, Int b) {
    return a > b ? a : b;
}

/* --------------------------------------------------------------- int.go */

void bi_set_int64(BigInt *z, int64_t x) {
    bool neg = false;
    uint64_t u = (uint64_t)x;
    if (x < 0) {
        neg = true;
        u = 0 - u;
    }
    z->abs = nat_set_uint64(z->abs, u);
    z->neg = neg;
}

void bi_set_uint64(BigInt *z, uint64_t x) {
    z->abs = nat_set_uint64(z->abs, x);
    z->neg = false;
}

void bi_set(BigInt *z, const BigInt *x) {
    if (z != x) {
        z->abs = nat_set(z->abs, x->abs);
        z->neg = x->neg;
    }
}

void bi_abs(BigInt *z, const BigInt *x) {
    bi_set(z, x);
    z->neg = false;
}

void bi_neg(BigInt *z, const BigInt *x) {
    bi_set(z, x);
    z->neg = z->abs.len > 0 && !z->neg; /* 0 has no sign */
}

void bi_add(BigInt *z, const BigInt *x, const BigInt *y) {
    bool neg = x->neg;
    if (x->neg == y->neg) {
        /* x + y == x + y
         * (-x) + (-y) == -(x + y) */
        z->abs = nat_add(z->abs, x->abs, y->abs);
    } else {
        /* x + (-y) == x - y == -(y - x)
         * (-x) + y == y - x == -(x - y) */
        if (nat_cmp(x->abs, y->abs) >= 0) {
            z->abs = nat_sub(z->abs, x->abs, y->abs);
        } else {
            neg = !neg;
            z->abs = nat_sub(z->abs, y->abs, x->abs);
        }
    }
    z->neg = z->abs.len > 0 && neg; /* 0 has no sign */
}

void bi_sub(BigInt *z, const BigInt *x, const BigInt *y) {
    bool neg = x->neg;
    if (x->neg != y->neg) {
        /* x - (-y) == x + y
         * (-x) - y == -(x + y) */
        z->abs = nat_add(z->abs, x->abs, y->abs);
    } else {
        /* x - y == x - y == -(y - x)
         * (-x) - (-y) == y - x == -(x - y) */
        if (nat_cmp(x->abs, y->abs) >= 0) {
            z->abs = nat_sub(z->abs, x->abs, y->abs);
        } else {
            neg = !neg;
            z->abs = nat_sub(z->abs, y->abs, x->abs);
        }
    }
    z->neg = z->abs.len > 0 && neg; /* 0 has no sign */
}

void bi_mul(BigInt *z, const BigInt *x, const BigInt *y) {
    /* x * y == x * y
     * x * (-y) == -(x * y)
     * (-x) * y == -(x * y)
     * (-x) * (-y) == x * y */
    if (x == y) {
        z->abs = nat_sqr(z->abs, x->abs);
        z->neg = false;
        return;
    }
    bool neg = x->neg != y->neg;
    z->abs = nat_mul(z->abs, x->abs, y->abs);
    z->neg = z->abs.len > 0 && neg; /* 0 has no sign */
}

static void bi_mul_range(BigInt *z, int64_t a, int64_t b) {
    if (a > b) {
        bi_set_int64(z, 1); /* empty range */
        return;
    }
    if (a <= 0 && b >= 0) {
        bi_set_int64(z, 0); /* range includes 0 */
        return;
    }
    /* a <= b && (b < 0 || a > 0) */
    bool neg = false;
    uint64_t ua = (uint64_t)a, ub = (uint64_t)b;
    if (a < 0) {
        neg = (ub - ua) % 2 == 0;
        uint64_t t = ua;
        ua = 0 - ub; /* negated as unsigned, so -2⁶³ comes out as 2⁶³ */
        ub = 0 - t;
    }
    z->abs = nat_mul_range(z->abs, ua, ub);
    z->neg = neg;
}

void bi_quo(BigInt *z, const BigInt *x, const BigInt *y) {
    bool neg = x->neg != y->neg;
    Nat r;
    z->abs = nat_div(z->abs, NAT_NIL, x->abs, y->abs, &r);
    z->neg = z->abs.len > 0 && neg; /* 0 has no sign */
}

void bi_rem(BigInt *z, const BigInt *x, const BigInt *y) {
    bool neg = x->neg;
    Nat r;
    nat_div(NAT_NIL, z->abs, x->abs, y->abs, &r);
    z->abs = r;
    z->neg = z->abs.len > 0 && neg; /* 0 has no sign */
}

void bi_quo_rem(BigInt *z, const BigInt *x, const BigInt *y, BigInt *r) {
    bool zneg = x->neg != y->neg;
    bool rneg = x->neg;
    Nat rr;
    z->abs = nat_div(z->abs, r->abs, x->abs, y->abs, &rr);
    r->abs = rr;
    z->neg = z->abs.len > 0 && zneg; /* 0 has no sign */
    r->neg = r->abs.len > 0 && rneg;
}

void bi_div(BigInt *z, const BigInt *x, const BigInt *y) {
    bool y_neg = y->neg; /* z may be an alias for y */
    BigInt r = BI_ZERO;
    bi_quo_rem(z, x, y, &r);
    if (r.neg) {
        if (y_neg)
            bi_add(z, z, INT_ONE);
        else
            bi_sub(z, z, INT_ONE);
    }
}

void bi_mod(BigInt *z, const BigInt *x, const BigInt *y) {
    const BigInt *y0 = y; /* save y */
    BigInt y0c = BI_ZERO;
    if (z == y || nat_alias(z->abs, y->abs)) {
        bi_set(&y0c, y);
        y0 = &y0c;
    }
    BigInt q = BI_ZERO;
    bi_quo_rem(&q, x, y, z);
    if (z->neg) {
        if (y0->neg)
            bi_sub(z, z, y0);
        else
            bi_add(z, z, y0);
    }
}

static void bi_div_mod(BigInt *z, const BigInt *x, const BigInt *y, BigInt *m) {
    const BigInt *y0 = y; /* save y */
    BigInt y0c = BI_ZERO;
    if (z == y || nat_alias(z->abs, y->abs)) {
        bi_set(&y0c, y);
        y0 = &y0c;
    }
    bi_quo_rem(z, x, y, m);
    if (m->neg) {
        if (y0->neg) {
            bi_add(z, z, INT_ONE);
            bi_sub(m, m, y0);
        } else {
            bi_sub(z, z, INT_ONE);
            bi_add(m, m, y0);
        }
    }
}

static void bi_divide(BigInt *z, const BigInt *x, const BigInt *y, BigInt *r,
                      BigRoundingMode mode) {
    Nat z_abs = NAT_NIL;
    if (z != NULL)
        z_abs = z->abs;
    bool r_neg = false;
    Nat r_abs = NAT_NIL;
    if (r != NULL)
        r_abs = r->abs;
    Nat y_abs = y->abs; /* save y */
    if (z == y || r == y || nat_alias(z_abs, y->abs) || nat_alias(r_abs, y->abs))
        y_abs = nat_set(NAT_NIL, y->abs);
    bool x_neg = x->neg, y_neg = y->neg;
    bool neg = x_neg != y_neg;
    Nat rr;
    z_abs = nat_div(z_abs, r_abs, x->abs, y->abs, &rr);
    r_abs = rr;
    if (r_abs.len > 0) {
        switch (mode) {
        case BIG_TRUNC:
            r_neg = x_neg;
            break;
        case BIG_FLOOR:
            r_neg = y_neg;
            if (neg) {
                z_abs = nat_add(z_abs, z_abs, NAT_ONE);
                r_abs = nat_sub(r_abs, y_abs, r_abs);
            }
            break;
        case BIG_CEIL:
            r_neg = !y_neg;
            if (!neg) {
                z_abs = nat_add(z_abs, z_abs, NAT_ONE);
                r_abs = nat_sub(r_abs, y_abs, r_abs);
            }
            break;
        case BIG_ROUND: {
            int c = nat_cmp(nat_mul(NAT_NIL, r_abs, NAT_TWO), y_abs);
            if (c == 0) {
                bool even = z_abs.len == 0 || (z_abs.p[0] & 1) == 0;
                c = even ? -1 : 1;
            }
            if (c < 0) {
                r_neg = x_neg;
            } else {
                r_neg = !x_neg;
                z_abs = nat_add(z_abs, z_abs, NAT_ONE);
                r_abs = nat_sub(r_abs, y_abs, r_abs);
            }
            break;
        }
        default:
            panic_str(BURROW_S("unsupported rounding mode"));
        }
    }
    if (z != NULL) {
        z->abs = z_abs;
        z->neg = neg && z_abs.len > 0; /* 0 has no sign */
    }
    if (r != NULL) {
        r->abs = r_abs;
        r->neg = r_neg;
    }
}

int bi_cmp(const BigInt *x, const BigInt *y) {
    /* x cmp y == x cmp y
     * x cmp (-y) == x
     * (-x) cmp y == y
     * (-x) cmp (-y) == -(x cmp y) */
    if (x == y)
        return 0;
    if (x->neg == y->neg) {
        int r = nat_cmp(x->abs, y->abs);
        return x->neg ? -r : r;
    }
    return x->neg ? -1 : 1;
}

/* low64: the least significant 64 bits of x. */
static uint64_t big_low64(Nat x) {
    if (x.len == 0)
        return 0;
    uint64_t v = (uint64_t)x.p[0];
#if BITS_UINT_SIZE == 32
    if (x.len > 1)
        return (uint64_t)x.p[1] << 32 | v;
#endif
    return v;
}

double bi_float64(const BigInt *x, BigAccuracy *acc) {
    Int n = nat_bit_len(x->abs);
    if (n == 0) {
        *acc = BIG_EXACT;
        return 0.0;
    }
    /* Fast path: no more than 53 significant bits. */
    if (n <= 53 || (n < 64 && n - (Int)nat_trailing_zero_bits(x->abs) <= 53)) {
        double f = (double)big_low64(x->abs);
        if (x->neg)
            f = -f;
        *acc = BIG_EXACT;
        return f;
    }
    /* The top 54 bits, the 54th for rounding, and whether anything below
     * them is set. Rounded to 53 bits, ties to even, as Float64 does. */
    Uint shift = (Uint)(n - 54);
    uint64_t m = big_low64(nat_rsh(NAT_NIL, x->abs, shift));
    Uint sticky = nat_sticky(x->abs, shift);
    uint64_t half = m & 1;
    m >>= 1;
    int e = (int)shift + 1;
    bool up = half != 0 && (sticky != 0 || (m & 1) != 0);
    bool exact = half == 0 && sticky == 0;
    if (up) {
        m++;
        if (m == (uint64_t)1 << 53) {
            m >>= 1;
            e++;
        }
    }
    if (e + 53 > 1024) {
        /* overflow */
        *acc = x->neg ? BIG_BELOW : BIG_ABOVE;
        return x->neg ? -HUGE_VAL : HUGE_VAL;
    }
    double f = ldexp((double)m, e);
    if (exact)
        *acc = BIG_EXACT;
    else
        *acc = up != x->neg ? BIG_ABOVE : BIG_BELOW;
    return x->neg ? -f : f;
}

bool bi_exp(BigInt *z, const BigInt *x, const BigInt *y, const BigInt *m, bool slow) {
    /* See Knuth, volume 2, section 4.6.3. */
    Nat x_words = x->abs;
    BigInt inverse = BI_ZERO;
    if (y->neg) {
        if (m == NULL || m->abs.len == 0) {
            bi_set_int64(z, 1);
            return true;
        }
        /* for y < 0: x**y mod |m| == (x**(-1))**|y| mod |m| */
        if (!bi_mod_inverse(&inverse, x, m))
            return false;
        x_words = inverse.abs;
    }
    Nat y_words = y->abs;

    Nat m_words = NAT_NIL;
    BigInt mc = BI_ZERO;
    if (m != NULL) {
        if (z == m || nat_alias(z->abs, m->abs)) {
            bi_set(&mc, m);
            m = &mc;
        }
        m_words = m->abs; /* m.abs may be nil for m == 0 */
    }

    bool x_neg = x->neg;
    z->abs = nat_exp_nn(z->abs, x_words, y_words, m_words, slow);
    z->neg = z->abs.len > 0 && x_neg && y_words.len > 0 && (y_words.p[0] & 1) == 1;
    if (z->neg && m_words.len > 0) {
        /* make modulus result positive, z == x**y mod |m| && 0 <= z < |m| */
        z->abs = nat_sub(z->abs, m_words, z->abs);
        z->neg = false;
    }
    return true;
}

/* lehmerSimulate: Lehmer's simulation of the Euclidean division on the
 * leading words of A and B, the cosequences and whether the last one is even. */
static void big_lehmer_simulate(const BigInt *A, const BigInt *B, BigWord *pu0,
                                BigWord *pu1, BigWord *pv0, BigWord *pv1, bool *peven) {
    BigWord a1, a2, u0, u1, u2, v0, v1, v2;

    Int m = B->abs.len; /* m >= 2 */
    Int n = A->abs.len; /* n >= m >= 2 */

    /* extract the top Word of bits from A and B */
    Uint h = big_nlz(A->abs.p[n - 1]);
    a1 = A->abs.p[n - 1] << h | (h == 0 ? 0 : A->abs.p[n - 2] >> (BIG_W - h));
    /* B may have implicit zero words in the high bits if the lengths differ */
    if (n == m)
        a2 = B->abs.p[n - 1] << h | (h == 0 ? 0 : B->abs.p[n - 2] >> (BIG_W - h));
    else if (n == m + 1)
        a2 = h == 0 ? 0 : B->abs.p[n - 2] >> (BIG_W - h);
    else
        a2 = 0;

    /* Since we are calculating with full words to avoid overflow, we use
     * 'even' to track the sign of the cosequences. For even iterations: u0,
     * v1 >= 0 && u1, v0 <= 0. For odd iterations: u0, v1 <= 0 && u1, v0 >= 0.
     * The first iteration starts with k=1 (odd). */
    bool even = false;
    /* variables to track the cosequences */
    u0 = 0, u1 = 1, u2 = 0;
    v0 = 0, v1 = 0, v2 = 1;

    /* Calculate the quotient and cosequences using Collins' stopping
     * condition. Note that overflow of a Word is not possible when computing
     * the remainder sequence and cosequences since the cosequence size is
     * bounded by the input size. See section 4.2 of Jebelean for details. */
    while (a2 >= v2 && a1 - a2 >= v1 + v2) {
        BigWord q = a1 / a2, r = a1 % a2;
        a1 = a2;
        a2 = r;
        BigWord t = u1 + q * u2;
        u0 = u1;
        u1 = u2;
        u2 = t;
        t = v1 + q * v2;
        v0 = v1;
        v1 = v2;
        v2 = t;
        even = !even;
    }
    *pu0 = u0;
    *pu1 = u1;
    *pv0 = v0;
    *pv1 = v1;
    *peven = even;
}

/* mulW: z = x * (-1 if neg) * w. */
static void big_mul_w(BigInt *z, const BigInt *x, bool neg, BigWord w) {
    bool xneg = x->neg;
    z->abs = nat_mul_add_ww(z->abs, x->abs, w, 0);
    z->neg = xneg != neg;
}

/* lehmerUpdate: A, B = u0*A + v0*B, u1*A + v1*B, with q and r as scratch. */
static void big_lehmer_update(BigInt *A, BigInt *B, BigInt *q, BigInt *r, BigWord u0,
                              BigWord u1, BigWord v0, BigWord v1, bool even) {
    big_mul_w(q, B, even, v0);
    big_mul_w(r, A, even, u1);
    big_mul_w(A, A, !even, u0);
    big_mul_w(B, B, !even, v1);
    bi_add(A, A, q);
    bi_add(B, B, r);
}

/* euclidUpdate: one step of the Euclidean algorithm and of the cosequence
 * when extended, rotating the pointers the way Go's multiple return does. */
static void big_euclid_update(BigInt **A, BigInt **B, BigInt **Ua, BigInt **Ub,
                              BigInt *q, BigInt **r, bool extended) {
    bi_quo_rem(q, *A, *B, *r);

    if (extended) {
        /* Ua, Ub = Ub, Ua - q*Ub */
        bi_mul(q, q, *Ub);
        BigInt *t = *Ua;
        *Ua = *Ub;
        *Ub = t;
        bi_sub(*Ub, *Ub, q);
    }

    /* A, B, r = B, r, A */
    BigInt *a = *A;
    *A = *B;
    *B = *r;
    *r = a;
}

/* lehmerGCD: the gcd by Lehmer's algorithm, with the cosequences when x or y
 * is wanted. Jebelean, "Improving the multiprecision Euclidean algorithm". */
static void big_lehmer_gcd(BigInt *z, BigInt *x, BigInt *y, const BigInt *a,
                           const BigInt *b) {
    BigInt sA = BI_ZERO, sB = BI_ZERO, sUa = BI_ZERO, sUb = BI_ZERO, sq = BI_ZERO,
           sr = BI_ZERO;
    BigInt *A = &sA, *B = &sB, *Ua = NULL, *Ub = NULL;

    /* ensure A = |a| and B = |b| so we don't have to worry about signs */
    bi_abs(A, a);
    bi_abs(B, b);

    bool extended = x != NULL || y != NULL;

    if (extended) {
        /* Ua (Ub) tracks how many times input a has been accumulated into
         * A (B). */
        Ua = &sUa;
        bi_set_int64(Ua, 1);
        Ub = &sUb;
    }

    /* temp variables for multiprecision update */
    BigInt *q = &sq;
    BigInt *r = &sr;

    /* ensure A >= B */
    if (nat_cmp(A->abs, B->abs) < 0) {
        BigInt *t = A;
        A = B;
        B = t;
        t = Ub;
        Ub = Ua;
        Ua = t;
    }

    /* loop invariant A >= B */
    while (B->abs.len > 1) {
        /* Attempt to calculate in single-precision using leading words of A
         * and B. */
        BigWord u0, u1, v0, v1;
        bool even;
        big_lehmer_simulate(A, B, &u0, &u1, &v0, &v1, &even);

        /* multiprecision Step */
        if (v0 != 0) {
            /* Simulate the effect of the single-precision steps using the
             * cosequences. A = u0*A + v0*B, B = u1*A + v1*B */
            big_lehmer_update(A, B, q, r, u0, u1, v0, v1, even);

            if (extended) {
                /* Ua = u0*Ua + v0*Ub, Ub = u1*Ua + v1*Ub */
                big_lehmer_update(Ua, Ub, q, r, u0, u1, v0, v1, even);
            }
        } else {
            /* Single-digit calculations failed to simulate any quotients. Do
             * a standard Euclidean step. */
            big_euclid_update(&A, &B, &Ua, &Ub, q, &r, extended);
        }
    }

    if (B->abs.len > 0) {
        /* extended Euclidean algorithm base case if B is a single Word */
        if (A->abs.len > 1) {
            /* A is longer than a single Word, so one update is needed. */
            big_euclid_update(&A, &B, &Ua, &Ub, q, &r, extended);
        }
        if (B->abs.len > 0) {
            /* A and B are both a single Word. */
            BigWord a_word = A->abs.p[0], b_word = B->abs.p[0];
            if (extended) {
                BigWord ua = 1, ub = 0, va = 0, vb = 1;
                bool even = true;
                while (b_word != 0) {
                    BigWord qw = a_word / b_word, rw = a_word % b_word;
                    a_word = b_word;
                    b_word = rw;
                    BigWord t = ua + qw * ub;
                    ua = ub;
                    ub = t;
                    t = va + qw * vb;
                    va = vb;
                    vb = t;
                    even = !even;
                }

                big_mul_w(Ua, Ua, !even, ua);
                big_mul_w(Ub, Ub, even, va);
                bi_add(Ua, Ua, Ub);
            } else {
                while (b_word != 0) {
                    BigWord t = a_word % b_word;
                    a_word = b_word;
                    b_word = t;
                }
            }
            A->abs.p[0] = a_word;
        }
    }
    bool neg_a = a->neg;
    if (y != NULL) {
        /* avoid aliasing b needed in the division below */
        const BigInt *Bd;
        if (y == b) {
            bi_set(B, b);
            Bd = B;
        } else {
            Bd = b;
        }
        /* y = (z - a*x)/b */
        bi_mul(y, a, Ua); /* y can safely alias a */
        if (neg_a)
            y->neg = !y->neg;
        bi_sub(y, A, y);
        bi_div(y, y, Bd);
    }

    if (x != NULL) {
        bi_set(x, Ua);
        if (neg_a)
            x->neg = !x->neg;
    }

    bi_set(z, A);
}

void bi_gcd(BigInt *z, BigInt *x, BigInt *y, const BigInt *a, const BigInt *b) {
    if (a->abs.len == 0 || b->abs.len == 0) {
        Int len_a = a->abs.len, len_b = b->abs.len;
        bool neg_a = a->neg, neg_b = b->neg;
        if (len_a == 0)
            bi_set(z, b);
        else
            bi_set(z, a);
        z->neg = false;
        if (x != NULL) {
            if (len_a == 0) {
                bi_set_uint64(x, 0);
            } else {
                bi_set_uint64(x, 1);
                x->neg = neg_a;
            }
        }
        if (y != NULL) {
            if (len_b == 0) {
                bi_set_uint64(y, 0);
            } else {
                bi_set_uint64(y, 1);
                y->neg = neg_b;
            }
        }
        return;
    }
    big_lehmer_gcd(z, x, y, a, b);
}

bool bi_mod_inverse(BigInt *z, const BigInt *g, const BigInt *n) {
    /* GCD expects parameters a and b to be > 0. */
    BigInt n2 = BI_ZERO, g2 = BI_ZERO;
    if (n->neg) {
        bi_neg(&n2, n);
        n = &n2;
    }
    if (g->neg) {
        bi_mod(&g2, g, n);
        g = &g2;
    }
    BigInt d = BI_ZERO, x = BI_ZERO;
    bi_gcd(&d, &x, NULL, g, n);

    /* if and only if d==1, g and n are relatively prime */
    if (bi_cmp(&d, INT_ONE) != 0)
        return false;
    /* x and y are such that g*x + n*y = 1, therefore x is the inverse
     * element, but it may be negative, so convert to the range 0 <= z < |n| */
    if (x.neg)
        bi_add(z, &x, n);
    else
        bi_set(z, &x);
    return true;
}

int bi_jacobi(const BigInt *x, const BigInt *y) {
    if (y->abs.len == 0 || (y->abs.p[0] & 1) == 0) {
        Str ys = nat_itoa(y->abs, y->neg, 10);
        panic_str(
            fmt_sprintf_v(error_allocator(),
                          "big: invalid 2nd argument to Int.Jacobi: need odd integer "
                          "but got %s",
                          ys));
    }

    /* We use the formulation described in chapter 2, section 2.4, "The
     * Yacas Book of Algorithms":
     * http://yacas.sourceforge.net/Algo.book.pdf */
    BigInt a = BI_ZERO, b = BI_ZERO, c = BI_ZERO;
    bi_set(&a, x);
    bi_set(&b, y);
    int j = 1;

    if (b.neg) {
        if (a.neg)
            j = -1;
        b.neg = false;
    }

    for (;;) {
        if (bi_cmp(&b, INT_ONE) == 0)
            return j;
        if (a.abs.len == 0)
            return 0;
        bi_mod(&a, &a, &b);
        if (a.abs.len == 0)
            return 0;
        /* a > 0 */

        /* handle factors of 2 in 'a' */
        Uint s = nat_trailing_zero_bits(a.abs);
        if ((s & 1) != 0) {
            BigWord bmod8 = b.abs.p[0] & 7;
            if (bmod8 == 3 || bmod8 == 5)
                j = -j;
        }
        bi_rsh(&c, &a, s); /* a = 2^s*c */

        /* swap numerator and denominator */
        if ((b.abs.p[0] & 3) == 3 && (c.abs.p[0] & 3) == 3)
            j = -j;
        bi_set(&a, &b);
        bi_set(&b, &c);
    }
}

/* modSqrt3Mod4Prime: z = sqrt(x) mod p for p = 3 mod 4, x^((p+1)/4). */
static void big_mod_sqrt_3mod4(BigInt *z, const BigInt *x, const BigInt *p) {
    BigInt e = BI_ZERO;
    bi_add(&e, p, INT_ONE); /* e = p + 1 */
    bi_rsh(&e, &e, 2);      /* e = (p + 1) / 4 */
    bi_exp(z, x, &e, p, false);
}

/* modSqrt5Mod8Prime: Atkin's algorithm for p = 5 mod 8. */
static void big_mod_sqrt_5mod8(BigInt *z, const BigInt *x, const BigInt *p) {
    /* p == 5 mod 8 implies p = e*8 + 5; e is the quotient and 5 the
     * remainder on division by 8. */
    BigInt e = BI_ZERO, tx = BI_ZERO, alpha = BI_ZERO, beta = BI_ZERO;
    bi_rsh(&e, p, 3);  /* e = (p - 5) / 8 */
    bi_lsh(&tx, x, 1); /* tx = 2*x */
    bi_exp(&alpha, &tx, &e, p, false);
    bi_mul(&beta, &alpha, &alpha);
    bi_mod(&beta, &beta, p);
    bi_mul(&beta, &beta, &tx);
    bi_mod(&beta, &beta, p);
    bi_sub(&beta, &beta, INT_ONE);
    bi_mul(&beta, &beta, x);
    bi_mod(&beta, &beta, p);
    bi_mul(&beta, &beta, &alpha);
    bi_mod(z, &beta, p);
}

/* modSqrtTonelliShanks: Tonelli-Shanks for any odd prime p. */
static void big_mod_sqrt_tonelli_shanks(BigInt *z, const BigInt *x, const BigInt *p) {
    /* Break p-1 into s*2^e such that s is odd. */
    BigInt s = BI_ZERO;
    bi_sub(&s, p, INT_ONE);
    Uint e = nat_trailing_zero_bits(s.abs);
    bi_rsh(&s, &s, e);

    /* find some non-square n */
    BigInt n = BI_ZERO;
    bi_set_int64(&n, 2);
    while (bi_jacobi(&n, p) != -1)
        bi_add(&n, &n, INT_ONE);

    /* Core of the Tonelli-Shanks algorithm. Follows the description in
     * section 6 of "Square roots from 1; 24, 51, 10 to Dan Shanks" by Ezra
     * Brown:
     * https://www.maa.org/sites/default/files/pdf/upload_library/22/Polya/07468342.di020786.02p0470a.pdf */
    BigInt y = BI_ZERO, b = BI_ZERO, g = BI_ZERO, t = BI_ZERO;
    bi_add(&y, &s, INT_ONE);
    bi_rsh(&y, &y, 1);
    bi_exp(&y, x, &y, p, false);  /* y = x^((s+1)/2) */
    bi_exp(&b, x, &s, p, false);  /* b = x^s */
    bi_exp(&g, &n, &s, p, false); /* g = n^s */
    Uint r = e;
    for (;;) {
        /* find the least m such that ord_p(b) = 2^m */
        Uint m = 0;
        bi_set(&t, &b);
        while (bi_cmp(&t, INT_ONE) != 0) {
            bi_mul(&t, &t, &t);
            bi_mod(&t, &t, p);
            m++;
        }

        if (m == 0) {
            bi_set(z, &y);
            return;
        }

        bi_set_int64(&t, 0);
        bi_set_bit(&t, &t, (Int)(r - m - 1), 1);
        bi_exp(&t, &g, &t, p, false);
        /* t = g^(2^(r-m-1)) mod p */
        bi_mul(&g, &t, &t);
        bi_mod(&g, &g, p); /* g = g^(2^(r-m)) mod p */
        bi_mul(&y, &y, &t);
        bi_mod(&y, &y, p);
        bi_mul(&b, &b, &g);
        bi_mod(&b, &b, p);
        r = m;
    }
}

static bool bi_mod_sqrt(BigInt *z, const BigInt *x, const BigInt *p) {
    switch (bi_jacobi(x, p)) {
    case -1:
        return false; /* x is not a square mod p */
    case 0:
        bi_set_int64(z, 0); /* sqrt(0) mod p = 0 */
        return true;
    default:
        break;
    }
    BigInt xm = BI_ZERO;
    if (x->neg || bi_cmp(x, p) >= 0) { /* ensure 0 <= x < p */
        bi_mod(&xm, x, p);
        x = &xm;
    }

    if (p->abs.p[0] % 4 == 3)
        /* Check whether p is 3 mod 4, and if so, use the faster algorithm. */
        big_mod_sqrt_3mod4(z, x, p);
    else if (p->abs.p[0] % 8 == 5)
        /* Check whether p is 5 mod 8, use Atkin's algorithm. */
        big_mod_sqrt_5mod8(z, x, p);
    else
        /* Otherwise, use Tonelli-Shanks. */
        big_mod_sqrt_tonelli_shanks(z, x, p);
    return true;
}

void bi_lsh(BigInt *z, const BigInt *x, Uint n) {
    bool neg = x->neg;
    z->abs = nat_lsh(z->abs, x->abs, n);
    z->neg = neg;
}

void bi_rsh(BigInt *z, const BigInt *x, Uint n) {
    if (x->neg) {
        /* (-x) >> s == ^(x-1) >> s == ^((x-1) >> s) == -(((x-1) >> s) + 1) */
        Nat t = nat_sub(z->abs, x->abs, NAT_ONE); /* no underflow because |x| > 0 */
        t = nat_rsh(t, t, n);
        z->abs = nat_add(t, t, NAT_ONE);
        z->neg = true; /* z cannot be zero if x is negative */
        return;
    }
    z->abs = nat_rsh(z->abs, x->abs, n);
    z->neg = false;
}

static Uint bi_bit(const BigInt *x, Int i) {
    if (i == 0) {
        /* optimization for common case: odd/even test of x */
        if (x->abs.len > 0)
            return (Uint)(x->abs.p[0] & 1); /* bit 0 is same for -x */
        return 0;
    }
    if (i < 0)
        panic_str(BURROW_S("negative bit index"));
    if (x->neg) {
        Nat t = nat_sub(NAT_NIL, x->abs, NAT_ONE);
        return nat_bit(t, (Uint)i) ^ 1;
    }
    return nat_bit(x->abs, (Uint)i);
}

void bi_set_bit(BigInt *z, const BigInt *x, Int i, Uint b) {
    if (i < 0)
        panic_str(BURROW_S("negative bit index"));
    if (x->neg) {
        Nat t = nat_sub(z->abs, x->abs, NAT_ONE);
        t = nat_set_bit(t, t, (Uint)i, b ^ 1);
        z->abs = nat_add(t, t, NAT_ONE);
        z->neg = z->abs.len > 0;
        return;
    }
    z->abs = nat_set_bit(z->abs, x->abs, (Uint)i, b);
    z->neg = false;
}

static void bi_and(BigInt *z, const BigInt *x, const BigInt *y) {
    if (x->neg == y->neg) {
        if (x->neg) {
            /* (-x) & (-y) == ^(x-1) & ^(y-1) == ^((x-1) | (y-1)) == -(((x-1) | (y-1)) + 1) */
            Nat x1 = nat_sub(NAT_NIL, x->abs, NAT_ONE);
            Nat y1 = nat_sub(NAT_NIL, y->abs, NAT_ONE);
            z->abs = nat_or(z->abs, x1, y1);
            z->abs = nat_add(z->abs, z->abs, NAT_ONE);
            z->neg = true; /* z cannot be zero if x and y are negative */
            return;
        }
        /* x & y == x & y */
        z->abs = nat_and(z->abs, x->abs, y->abs);
        z->neg = false;
        return;
    }

    /* x->neg != y->neg */
    if (x->neg) {
        const BigInt *t = x; /* & is symmetric */
        x = y;
        y = t;
    }

    /* x & (-y) == x & ^(y-1) == x &^ (y-1) */
    Nat y1 = nat_sub(NAT_NIL, y->abs, NAT_ONE);
    z->abs = nat_and_not(z->abs, x->abs, y1);
    z->neg = false;
}

static void bi_and_not(BigInt *z, const BigInt *x, const BigInt *y) {
    if (x->neg == y->neg) {
        if (x->neg) {
            /* (-x) &^ (-y) == ^(x-1) &^ ^(y-1) == ^(x-1) & (y-1) == (y-1) &^ (x-1) */
            Nat x1 = nat_sub(NAT_NIL, x->abs, NAT_ONE);
            Nat y1 = nat_sub(NAT_NIL, y->abs, NAT_ONE);
            z->abs = nat_and_not(z->abs, y1, x1);
            z->neg = false;
            return;
        }
        /* x &^ y == x &^ y */
        z->abs = nat_and_not(z->abs, x->abs, y->abs);
        z->neg = false;
        return;
    }

    if (x->neg) {
        /* (-x) &^ y == ^(x-1) &^ y == ^(x-1) & ^y == ^((x-1) | y) == -(((x-1) | y) + 1) */
        Nat x1 = nat_sub(NAT_NIL, x->abs, NAT_ONE);
        z->abs = nat_or(z->abs, x1, y->abs);
        z->abs = nat_add(z->abs, z->abs, NAT_ONE);
        z->neg = true; /* z cannot be zero if x is negative and y is positive */
        return;
    }

    /* x &^ (-y) == x &^ ^(y-1) == x & (y-1) */
    Nat y1 = nat_sub(NAT_NIL, y->abs, NAT_ONE);
    z->abs = nat_and(z->abs, x->abs, y1);
    z->neg = false;
}

static void bi_or(BigInt *z, const BigInt *x, const BigInt *y) {
    if (x->neg == y->neg) {
        if (x->neg) {
            /* (-x) | (-y) == ^(x-1) | ^(y-1) == ^((x-1) & (y-1)) == -(((x-1) & (y-1)) + 1) */
            Nat x1 = nat_sub(NAT_NIL, x->abs, NAT_ONE);
            Nat y1 = nat_sub(NAT_NIL, y->abs, NAT_ONE);
            z->abs = nat_and(z->abs, x1, y1);
            z->abs = nat_add(z->abs, z->abs, NAT_ONE);
            z->neg = true; /* z cannot be zero if x and y are negative */
            return;
        }
        /* x | y == x | y */
        z->abs = nat_or(z->abs, x->abs, y->abs);
        z->neg = false;
        return;
    }

    /* x->neg != y->neg */
    if (x->neg) {
        const BigInt *t = x; /* | is symmetric */
        x = y;
        y = t;
    }

    /* x | (-y) == x | ^(y-1) == ^((y-1) &^ x) == -(^((y-1) &^ x) + 1) */
    Nat y1 = nat_sub(NAT_NIL, y->abs, NAT_ONE);
    z->abs = nat_and_not(z->abs, y1, x->abs);
    z->abs = nat_add(z->abs, z->abs, NAT_ONE);
    z->neg = true; /* z cannot be zero if one of x or y is negative */
}

static void bi_xor(BigInt *z, const BigInt *x, const BigInt *y) {
    if (x->neg == y->neg) {
        if (x->neg) {
            /* (-x) ^ (-y) == ^(x-1) ^ ^(y-1) == (x-1) ^ (y-1) */
            Nat x1 = nat_sub(NAT_NIL, x->abs, NAT_ONE);
            Nat y1 = nat_sub(NAT_NIL, y->abs, NAT_ONE);
            z->abs = nat_xor(z->abs, x1, y1);
            z->neg = false;
            return;
        }
        /* x ^ y == x ^ y */
        z->abs = nat_xor(z->abs, x->abs, y->abs);
        z->neg = false;
        return;
    }

    /* x->neg != y->neg */
    if (x->neg) {
        const BigInt *t = x; /* ^ is symmetric */
        x = y;
        y = t;
    }

    /* x ^ (-y) == x ^ ^(y-1) == ^(x ^ (y-1)) == -((x ^ (y-1)) + 1) */
    Nat y1 = nat_sub(NAT_NIL, y->abs, NAT_ONE);
    z->abs = nat_xor(z->abs, x->abs, y1);
    z->abs = nat_add(z->abs, z->abs, NAT_ONE);
    z->neg = true; /* z cannot be zero if only one of x or y is negative */
}

static void bi_not(BigInt *z, const BigInt *x) {
    if (x->neg) {
        /* ^(-x) == ^(^(x-1)) == x-1 */
        z->abs = nat_sub(z->abs, x->abs, NAT_ONE);
        z->neg = false;
        return;
    }
    /* ^x == -x-1 == -(x+1) */
    z->abs = nat_add(z->abs, x->abs, NAT_ONE);
    z->neg = true; /* z cannot be zero if x is positive */
}

/* ------------------------------------------------------------- prime.go */

/* probablyPrimeMillerRabin: reps rounds of Miller-Rabin with pseudo random
 * bases, the last of them 2 when force2. n is odd and above 3. */
static bool big_probably_prime_miller_rabin(Nat n, Int reps, bool force2) {
    Nat nm1 = nat_sub(NAT_NIL, n, NAT_ONE);
    /* determine q, k such that nm1 = q << k */
    Uint k = nat_trailing_zero_bits(nm1);
    Nat q = nat_rsh(NAT_NIL, nm1, k);

    Nat nm3 = nat_sub(NAT_NIL, nm1, NAT_TWO);
    Alloc *sa = big_scratch_alloc();
    MathRandRand *rnd = math_rand_new(sa, math_rand_new_source(sa, (int64_t)n.p[0]));
    if (rnd == NULL)
        big_oom();

    Nat x = NAT_NIL, y = NAT_NIL, quotient = NAT_NIL;
    Int nm3_len = nat_bit_len(nm3);

    for (Int i = 0; i < reps; i++) {
        if (i == reps - 1 && force2) {
            x = nat_set(x, NAT_TWO);
        } else {
            x = nat_random(x, rnd, nm3, nm3_len);
            x = nat_add(x, x, NAT_TWO);
        }
        y = nat_exp_nn(y, x, q, n, false);
        if (nat_cmp(y, NAT_ONE) == 0 || nat_cmp(y, nm1) == 0)
            continue;
        bool next = false;
        for (Uint j = 1; j < k; j++) {
            y = nat_sqr(y, y);
            Nat r;
            quotient = nat_div(quotient, y, y, n, &r);
            y = r;
            if (nat_cmp(y, nm1) == 0) {
                next = true;
                break;
            }
            if (nat_cmp(y, NAT_ONE) == 0)
                return false;
        }
        if (!next)
            return false;
    }
    return true;
}

/* probablyPrimeLucas: the strong Lucas probable prime test with Baillie's
 * parameters, "almost extra strong", as Go does it. */
static bool big_probably_prime_lucas(Nat n) {
    /* Discard 0, 1. */
    if (n.len == 0 || nat_cmp(n, NAT_ONE) == 0)
        return false;
    /* Two is the only even prime. Already checked by caller, but here to
     * allow direct testing. */
    if ((n.p[0] & 1) == 0)
        return nat_cmp(n, NAT_TWO) == 0;

    /* Baillie-OEIS "method C" for choosing D, P, Q, as in
     * https://oeis.org/A217719/a217719.txt: try increasing P >= 3 such that
     * D = P^2 - 4 (so Q = 1) until Jacobi(D, n) = -1. The search is expected
     * to succeed for non-square n after about 1.8 iterations. */
    BigWord p = 3;
    BigWord dw[1] = {1};
    Nat d = {dw, 1, 1};
    Nat t1 = NAT_NIL; /* temp */
    BigInt int_d = {NULL, false, d};
    BigInt int_n = {NULL, false, n};
    for (;; p++) {
        if (p > 10000) {
            /* This is widely believed to be impossible. If we get a report,
             * we'll want the number. */
            Str ns = nat_itoa(n, false, 10);
            panic_str(fmt_sprintf_v(
                error_allocator(),
                "math/big: internal error: cannot find (D/n) = -1 for %s", ns));
        }
        dw[0] = p * p - 4;
        int j = bi_jacobi(&int_d, &int_n);
        if (j == -1)
            break;
        if (j == 0) {
            /* d = p²-4 = (p-2)(p+2). If (d/n) == 0 then d shares a prime
             * factor with n. Since the loop proceeds in increasing p and
             * starts with p-2==1, the shared prime factor must be p+2. If p+2
             * == n, then n is prime; otherwise p+2 is a proper factor of n. */
            return n.len == 1 && n.p[0] == p + 2;
        }
        if (p == 40) {
            /* We'll never find (d/n) = -1 if n is a square. If n is a
             * non-square we expect to find a d in just a few attempts on
             * average. After 40 attempts, take a moment to check if n is
             * indeed a square. */
            t1 = nat_sqrt(t1, n);
            t1 = nat_sqr(t1, t1);
            if (nat_cmp(t1, n) == 0)
                return false;
        }
    }

    /* Grantham definition of "extra strong Lucas pseudoprime", after
     * Thm 2.3 on p. 876 (D, P, Q above have become Δ, b, 1):
     *
     * Let U_n = U_n(b, 1), V_n = V_n(b, 1), and Δ = b²-4. An extra strong
     * Lucas pseudoprime to base b is a composite n = 2^r s + Jacobi(Δ, n),
     * where s is odd and gcd(n, 2*Δ) = 1, such that either (i) U_s ≡ 0 mod n
     * and V_s ≡ ±2 mod n, or (ii) V_{2^t s} ≡ 0 mod n for some 0 ≤ t < r-1.
     *
     * We know gcd(n, Δ) = 1 or else we'd have found Jacobi(d, n) == 0 above.
     * We know gcd(n, 2) = 1 because n is odd.
     *
     * Arrange s = (n - Jacobi(Δ, n)) / 2^r = (n+1) / 2^r. */
    Nat s = nat_add(NAT_NIL, n, NAT_ONE);
    Int r = (Int)nat_trailing_zero_bits(s);
    s = nat_rsh(s, s, (Uint)r);
    Nat nm2 = nat_sub(NAT_NIL, n, NAT_TWO); /* n-2 */

    /* We apply the "almost extra strong" test, which checks the above
     * conditions except for U_s ≡ 0 mod n, which allows us to avoid computing
     * any U_k values. Jacobsen points out that maybe we should just do the
     * full extra strong test: "It is also possible to recover U_n using
     * Crandall and Pomerance equation 3.13: U_n = D^-1 (2V_{n+1} - PV_n)
     * allowing us to run the full extra-strong test at the cost of a single
     * modular inversion. This computation is easy and fast in GMP, so we can
     * get the full extra-strong test at essentially the same performance as
     * the almost extra strong test."
     *
     * Compute Lucas sequence V_s(b, 1), where:
     *
     *	V(0) = 2
     *	V(1) = P
     *	V(k) = P V(k-1) - Q V(k-2).
     *
     * (Remember that due to method C above, P = b, Q = 1.)
     *
     * In general V(k) = α^k + β^k, where α and β are roots of x² - Px + Q.
     * Crandall and Pomerance (p.147) observe that for 0 ≤ j ≤ k,
     *
     *	V(j+k) = V(j)V(k) - V(k-j).
     *
     * So in particular, to quickly double the subscript:
     *
     *	V(2k) = V(k)² - 2
     *	V(2k+1) = V(k) V(k+1) - P
     *
     * We can therefore start with k=0 and build up to k=s in log₂(s) steps. */
    Nat nat_p = nat_set_word(NAT_NIL, p);
    Nat vk = nat_set_word(NAT_NIL, 2);
    Nat vk1 = nat_set_word(NAT_NIL, p);
    Nat t2 = NAT_NIL; /* temp */
    Nat rr;
    for (Int i = nat_bit_len(s); i >= 0; i--) {
        if (nat_bit(s, (Uint)i) != 0) {
            /* k' = 2k+1
             * V(k') = V(2k+1) = V(k) V(k+1) - P. */
            t1 = nat_mul(t1, vk, vk1);
            t1 = nat_add(t1, t1, n);
            t1 = nat_sub(t1, t1, nat_p);
            t2 = nat_div(t2, vk, t1, n, &rr);
            vk = rr;
            /* V(k'+1) = V(2k+2) = V(k+1)² - 2. */
            t1 = nat_sqr(t1, vk1);
            t1 = nat_add(t1, t1, nm2);
            t2 = nat_div(t2, vk1, t1, n, &rr);
            vk1 = rr;
        } else {
            /* k' = 2k
             * V(k'+1) = V(2k+1) = V(k) V(k+1) - P. */
            t1 = nat_mul(t1, vk, vk1);
            t1 = nat_add(t1, t1, n);
            t1 = nat_sub(t1, t1, nat_p);
            t2 = nat_div(t2, vk1, t1, n, &rr);
            vk1 = rr;
            /* V(k') = V(2k) = V(k)² - 2 */
            t1 = nat_sqr(t1, vk);
            t1 = nat_add(t1, t1, nm2);
            t2 = nat_div(t2, vk, t1, n, &rr);
            vk = rr;
        }
    }

    /* Now k=s, so vk = V(s). Check V(s) ≡ ±2 (mod n). */
    if (nat_cmp(vk, NAT_TWO) == 0 || nat_cmp(vk, nm2) == 0) {
        /* Check U(s) ≡ 0. As suggested by Jacobsen, apply Crandall and
         * Pomerance equation 3.13:
         *
         *	U(k) = D⁻¹ (2 V(k+1) - P V(k))
         *
         * Since we are checking for U(k) == 0 it suffices to check 2 V(k+1)
         * == P V(k) mod n, or P V(k) - 2 V(k+1) == 0 mod n. */
        Nat u1 = nat_mul(t1, vk, nat_p);
        Nat u2 = nat_lsh(t2, vk1, 1);
        if (nat_cmp(u1, u2) < 0) {
            Nat t = u1;
            u1 = u2;
            u2 = t;
        }
        u1 = nat_sub(u1, u1, u2);
        Nat t3 = vk1; /* steal vk1, no longer needed below */
        vk1 = NAT_NIL;
        u2 = nat_div(u2, t3, u1, n, &t3);
        if (t3.len == 0)
            return true;
    }

    /* Check V(2^t s) ≡ 0 mod n for some 0 ≤ t < r-1. */
    for (Int t = 0; t < r - 1; t++) {
        if (vk.len == 0) /* vk == 0 */
            return true;
        /* Optimization: V(k) = 2 is a fixed point for V(k') = V(k)² - 2, so
         * if V(k) = 2, we can stop: we will never find a future V(k) == 0. */
        if (vk.len == 1 && vk.p[0] == 2) /* vk == 2 */
            return false;
        /* k' = 2k
         * V(k') = V(2k) = V(k)² - 2 */
        t1 = nat_sqr(t1, vk);
        t1 = nat_sub(t1, t1, NAT_TWO);
        t2 = nat_div(t2, vk, t1, n, &rr);
        vk = rr;
    }
    return false;
}

static bool bi_probably_prime(const BigInt *x, Int n) {
    /* Note regarding the doc comment above: It would be more precise to say
     * that the Baillie-PSW test uses the extra strong Lucas test as its
     * Lucas test, but since no one knows how to tell any of the Lucas tests
     * apart inside the 2⁶⁴ range, the doc comment is fine as is. */
    if (n < 0)
        panic_str(BURROW_S("negative n for ProbablyPrime"));
    if (x->neg || x->abs.len == 0)
        return false;

    /* primeBitMask records the primes < 64. */
    const uint64_t prime_bit_mask =
        (uint64_t)1 << 2 | (uint64_t)1 << 3 | (uint64_t)1 << 5 | (uint64_t)1 << 7 |
        (uint64_t)1 << 11 | (uint64_t)1 << 13 | (uint64_t)1 << 17 | (uint64_t)1 << 19 |
        (uint64_t)1 << 23 | (uint64_t)1 << 29 | (uint64_t)1 << 31 | (uint64_t)1 << 37 |
        (uint64_t)1 << 41 | (uint64_t)1 << 43 | (uint64_t)1 << 47 | (uint64_t)1 << 53 |
        (uint64_t)1 << 59 | (uint64_t)1 << 61;

    BigWord w = x->abs.p[0];
    if (x->abs.len == 1 && w < 64)
        return (prime_bit_mask & ((uint64_t)1 << w)) != 0;

    if ((w & 1) == 0)
        return false; /* x is even */

    const uint64_t primes_a = (uint64_t)3 * 5 * 7 * 11 * 13 * 17 * 19 * 23 * 37;
    const uint64_t primes_b = (uint64_t)29 * 31 * 41 * 43 * 47 * 53;

    uint32_t ra, rb;
#if BITS_UINT_SIZE == 32
    ra = (uint32_t)nat_mod_w(x->abs, (BigWord)primes_a);
    rb = (uint32_t)nat_mod_w(x->abs, (BigWord)primes_b);
#else
    BigWord rm = nat_mod_w(x->abs, (BigWord)(primes_a * primes_b));
    ra = (uint32_t)(rm % primes_a);
    rb = (uint32_t)(rm % primes_b);
#endif

    /* Do more trial divisions to rule out small factors. */
    if (ra % 3 == 0 || ra % 5 == 0 || ra % 7 == 0 || ra % 11 == 0 || ra % 13 == 0 ||
        ra % 17 == 0 || ra % 19 == 0 || ra % 23 == 0 || ra % 37 == 0 || rb % 29 == 0 ||
        rb % 31 == 0 || rb % 41 == 0 || rb % 43 == 0 || rb % 47 == 0 || rb % 53 == 0)
        return false;

    return big_probably_prime_miller_rabin(x->abs, n + 1, true) &&
           big_probably_prime_lucas(x->abs);
}

/* ----------------------------------------------------------- intconv.go */

Error bi_scan_sign(BigScanner *r, bool *neg) {
    *neg = false;
    Byte ch;
    Error err = big_scanner_read(r, &ch);
    if (BURROW_FAILED(err))
        return err;
    switch (ch) {
    case '-':
        *neg = true;
        break;
    case '+':
        /* nothing to do */
        break;
    default:
        big_scanner_unread(r);
    }
    return BURROW_NO_ERROR;
}

void bi_scan(BigInt *z, BigScanner *r, int base, int *b, Error *err) {
    /* determine sign */
    bool neg;
    Error e = bi_scan_sign(r, &neg);
    if (BURROW_FAILED(e)) {
        *b = 0;
        *err = e;
        return;
    }

    /* determine mantissa */
    Int count;
    z->abs = nat_scan(z->abs, r, base, false, b, &count, &e);
    if (BURROW_FAILED(e)) {
        *err = e;
        return;
    }
    z->neg = z->abs.len > 0 && neg; /* 0 has no sign */
    *err = BURROW_NO_ERROR;
}

/* setFromScanner: z from all of r, false when r holds anything more than a
 * number. */
static bool bi_set_from_scanner(BigInt *z, BigScanner *r, int base) {
    int b;
    Error err;
    bi_scan(z, r, base, &b, &err);
    if (BURROW_FAILED(err))
        return false;
    /* entire content must have been consumed */
    Byte ch;
    return errors_is(big_scanner_read(r, &ch), io_eof);
}

/* ---------------------------------------------------------- public: Int */

/* The last step of every public function that sets z: z's words back in its
 * own memory, the scratch memory empty again. */
static BigInt *big_done(BigInt *z, Nat orig) {
    big_commit(&z->abs, z->a, orig);
    big_leave();
    return z;
}

/* A public function with one receiver z, whose words should have room for n
 * before bi runs. */
#define BIG_UNARY(z, n, ...)                                                           \
    do {                                                                               \
        big_enter();                                                                   \
        big_reserve(&(z)->abs, (z)->a, (n));                                           \
        Nat orig_ = (z)->abs;                                                          \
        __VA_ARGS__;                                                                   \
        return big_done((z), orig_);                                                   \
    } while (0)

BigInt *big_new_int(Alloc *a, int64_t x) {
    BigInt *z =
        mem_alloc(a != NULL ? a : heap_allocator(), sizeof(BigInt), _Alignof(BigInt));
    if (z == NULL)
        big_oom();
    *z = BIG_INT(a);
    return big_int_set_int64(z, x);
}

Int big_int_sign(const BigInt *x) {
    if (x->abs.len == 0)
        return 0;
    return x->neg ? -1 : 1;
}

BigInt *big_int_set_int64(BigInt *z, int64_t x) {
    BIG_UNARY(z, 64 / BITS_UINT_SIZE, bi_set_int64(z, x));
}

BigInt *big_int_set_uint64(BigInt *z, uint64_t x) {
    BIG_UNARY(z, 64 / BITS_UINT_SIZE, bi_set_uint64(z, x));
}

BigInt *big_int_set(BigInt *z, const BigInt *x) {
    BIG_UNARY(z, x->abs.len, bi_set(z, x));
}

Slice big_int_bits(const BigInt *x) {
    return slice_from(x->abs.p, x->abs.len, x->abs.len, TYPE_UINT);
}

BigInt *big_int_set_bits(BigInt *z, Slice abs) {
    BIG_UNARY(z, abs.len, {
        z->abs = nat_norm(nat_set(z->abs, nat_view(abs.p, abs.len)));
        z->neg = false;
    });
}

BigInt *big_int_abs(BigInt *z, const BigInt *x) {
    BIG_UNARY(z, x->abs.len, bi_abs(z, x));
}

BigInt *big_int_neg(BigInt *z, const BigInt *x) {
    BIG_UNARY(z, x->abs.len, bi_neg(z, x));
}

BigInt *big_int_add(BigInt *z, const BigInt *x, const BigInt *y) {
    BIG_UNARY(z, big_max_int(x->abs.len, y->abs.len) + 1, bi_add(z, x, y));
}

BigInt *big_int_sub(BigInt *z, const BigInt *x, const BigInt *y) {
    BIG_UNARY(z, big_max_int(x->abs.len, y->abs.len), bi_sub(z, x, y));
}

BigInt *big_int_mul(BigInt *z, const BigInt *x, const BigInt *y) {
    BIG_UNARY(z, 0, bi_mul(z, x, y));
}

BigInt *big_int_mul_range(BigInt *z, int64_t a, int64_t b) {
    BIG_UNARY(z, 0, bi_mul_range(z, a, b));
}

BigInt *big_int_binomial(BigInt *z, int64_t n, int64_t k) {
    BIG_UNARY(z, 0, {
        if (k > n || k < 0) {
            bi_set_int64(z, 0);
        } else {
            /* reduce the number of multiplications by reducing k */
            if (k > n - k)
                k = n - k; /* C(n, k) == C(n, n-k) */
            /* C(n, k) = n(n-1)(n-2)...(n-k+1) / k!
             *         = n(n-1)(n-2)...(n-k+1) / 1*2*3...*k
             *
             * Using the multiplicative formula produces smaller values at
             * each step, requiring fewer allocations and computations:
             *
             * z = 1
             * for i := 0; i < k; i = i+1 {
             *     z *= n-i
             *     z /= i+1
             * }
             *
             * finally to avoid computing i+1 twice per loop:
             *
             * z = 1
             * i := 0
             * for i < k {
             *     z *= n-i
             *     i++
             *     z /= i
             * } */
            BigInt N = BI_ZERO, K = BI_ZERO, i = BI_ZERO, t = BI_ZERO;
            bi_set_int64(&N, n);
            bi_set_int64(&K, k);
            bi_set(z, INT_ONE);
            while (bi_cmp(&i, &K) < 0) {
                bi_sub(&t, &N, &i);
                bi_mul(z, z, &t);
                bi_add(&i, &i, INT_ONE);
                bi_quo(z, z, &i);
            }
        }
    });
}

BigInt *big_int_quo(BigInt *z, const BigInt *x, const BigInt *y) {
    BIG_UNARY(z, 0, bi_quo(z, x, y));
}

BigInt *big_int_rem(BigInt *z, const BigInt *x, const BigInt *y) {
    BIG_UNARY(z, 0, bi_rem(z, x, y));
}

BigInt *big_int_quo_rem(BigInt *z, const BigInt *x, const BigInt *y, BigInt *r) {
    big_enter();
    Nat zo = z->abs, ro = r->abs;
    bi_quo_rem(z, x, y, r);
    big_commit(&r->abs, r->a, ro);
    return big_done(z, zo);
}

BigInt *big_int_div(BigInt *z, const BigInt *x, const BigInt *y) {
    BIG_UNARY(z, 0, bi_div(z, x, y));
}

BigInt *big_int_mod(BigInt *z, const BigInt *x, const BigInt *y) {
    BIG_UNARY(z, 0, bi_mod(z, x, y));
}

BigInt *big_int_div_mod(BigInt *z, const BigInt *x, const BigInt *y, BigInt *m) {
    big_enter();
    Nat zo = z->abs, mo = m->abs;
    bi_div_mod(z, x, y, m);
    big_commit(&m->abs, m->a, mo);
    return big_done(z, zo);
}

BigInt *big_int_divide(BigInt *z, const BigInt *x, const BigInt *y, BigInt *r,
                       BigRoundingMode mode) {
    big_enter();
    Nat zo = z != NULL ? z->abs : NAT_NIL;
    Nat ro = r != NULL ? r->abs : NAT_NIL;
    bi_divide(z, x, y, r, mode);
    if (r != NULL)
        big_commit(&r->abs, r->a, ro);
    if (z != NULL)
        big_commit(&z->abs, z->a, zo);
    big_leave();
    return z;
}

Int big_int_cmp(const BigInt *x, const BigInt *y) {
    return bi_cmp(x, y);
}

Int big_int_cmp_abs(const BigInt *x, const BigInt *y) {
    return nat_cmp(x->abs, y->abs);
}

int64_t big_int_int64(const BigInt *x) {
    uint64_t v = big_low64(x->abs);
    if (x->neg)
        v = 0 - v;
    return (int64_t)v;
}

uint64_t big_int_uint64(const BigInt *x) {
    return big_low64(x->abs);
}

bool big_int_is_int64(const BigInt *x) {
    if (x->abs.len <= 64 / (Int)BIG_W) {
        uint64_t w = big_low64(x->abs);
        return w >> 63 == 0 || (x->neg && w == (uint64_t)1 << 63);
    }
    return false;
}

bool big_int_is_uint64(const BigInt *x) {
    return !x->neg && x->abs.len <= 64 / (Int)BIG_W;
}

double big_int_float64(const BigInt *x, BigAccuracy *acc) {
    BigAccuracy a;
    big_enter();
    double f = bi_float64(x, &a);
    big_leave();
    if (acc != NULL)
        *acc = a;
    return f;
}

BigInt *big_int_set_string(BigInt *z, Str s, Int base, bool *ok) {
    big_enter();
    Nat orig = z->abs;
    BigScanner r;
    big_scanner_init(&r, s);
    bool good = bi_set_from_scanner(z, &r, big_base(base));
    big_done(z, orig);
    if (ok != NULL)
        *ok = good;
    return good ? z : NULL;
}

BigInt *big_int_set_bytes(BigInt *z, Slice buf) {
    BIG_UNARY(z, (buf.len + BIG_S - 1) / BIG_S, {
        z->abs = nat_set_bytes(z->abs, buf);
        z->neg = false;
    });
}

Slice big_int_bytes(const BigInt *x, Alloc *a) {
    Int n = x->abs.len * BIG_S;
    Slice buf = slice_make(a, TYPE_BYTE, n, n);
    if (buf.p == NULL && n > 0)
        big_oom();
    Int i = nat_bytes(x->abs, buf);
    /* buf[i:], moved to the front so that the slice starts where its memory
     * does. */
    if (i > 0)
        memmove(buf.p, (Byte *)buf.p + i, (size_t)(n - i));
    buf.len = n - i;
    return buf;
}

Slice big_int_fill_bytes(const BigInt *x, Slice buf) {
    /* Clear whole buffer. */
    if (buf.len > 0)
        memset(buf.p, 0, (size_t)buf.len);
    nat_bytes(x->abs, buf);
    return buf;
}

Int big_int_bit_len(const BigInt *x) {
    return nat_bit_len(x->abs);
}

Uint big_int_trailing_zero_bits(const BigInt *x) {
    return nat_trailing_zero_bits(x->abs);
}

BigInt *big_int_exp(BigInt *z, const BigInt *x, const BigInt *y, const BigInt *m) {
    big_enter();
    Nat orig = z->abs;
    bool ok = bi_exp(z, x, y, m, false);
    big_done(z, orig);
    return ok ? z : NULL;
}

BigInt *big_int_gcd(BigInt *z, BigInt *x, BigInt *y, const BigInt *a, const BigInt *b) {
    big_enter();
    Nat zo = z->abs;
    Nat xo = x != NULL ? x->abs : NAT_NIL;
    Nat yo = y != NULL ? y->abs : NAT_NIL;
    bi_gcd(z, x, y, a, b);
    if (x != NULL)
        big_commit(&x->abs, x->a, xo);
    if (y != NULL)
        big_commit(&y->abs, y->a, yo);
    return big_done(z, zo);
}

BigInt *big_int_rand(BigInt *z, MathRandRand *rnd, const BigInt *n) {
    BIG_UNARY(z, n->abs.len, {
        /* z.neg is not modified before the if check, because z and n might
         * alias. */
        if (n->neg || n->abs.len == 0) {
            z->neg = false;
            z->abs = nat_to(z->abs, 0);
        } else {
            z->neg = false;
            z->abs = nat_random(z->abs, rnd, n->abs, nat_bit_len(n->abs));
        }
    });
}

BigInt *big_int_mod_inverse(BigInt *z, const BigInt *g, const BigInt *n) {
    big_enter();
    Nat orig = z->abs;
    bool ok = bi_mod_inverse(z, g, n);
    big_done(z, orig);
    return ok ? z : NULL;
}

Int big_jacobi(const BigInt *x, const BigInt *y) {
    big_enter();
    int j = bi_jacobi(x, y);
    big_leave();
    return j;
}

BigInt *big_int_mod_sqrt(BigInt *z, const BigInt *x, const BigInt *p) {
    big_enter();
    Nat orig = z->abs;
    bool ok = bi_mod_sqrt(z, x, p);
    big_done(z, orig);
    return ok ? z : NULL;
}

BigInt *big_int_lsh(BigInt *z, const BigInt *x, Uint n) {
    BIG_UNARY(z, x->abs.len == 0 ? 0 : x->abs.len + (Int)(n / BIG_W) + 1,
              bi_lsh(z, x, n));
}

BigInt *big_int_rsh(BigInt *z, const BigInt *x, Uint n) {
    BIG_UNARY(z, 0, bi_rsh(z, x, n));
}

Uint big_int_bit(const BigInt *x, Int i) {
    big_enter();
    Uint b = bi_bit(x, i);
    big_leave();
    return b;
}

BigInt *big_int_set_bit(BigInt *z, const BigInt *x, Int i, Uint b) {
    BIG_UNARY(z, 0, bi_set_bit(z, x, i, b));
}

BigInt *big_int_and(BigInt *z, const BigInt *x, const BigInt *y) {
    BIG_UNARY(z, 0, bi_and(z, x, y));
}

BigInt *big_int_and_not(BigInt *z, const BigInt *x, const BigInt *y) {
    BIG_UNARY(z, 0, bi_and_not(z, x, y));
}

BigInt *big_int_or(BigInt *z, const BigInt *x, const BigInt *y) {
    BIG_UNARY(z, 0, bi_or(z, x, y));
}

BigInt *big_int_xor(BigInt *z, const BigInt *x, const BigInt *y) {
    BIG_UNARY(z, 0, bi_xor(z, x, y));
}

BigInt *big_int_not(BigInt *z, const BigInt *x) {
    BIG_UNARY(z, 0, bi_not(z, x));
}

BigInt *big_int_sqrt(BigInt *z, const BigInt *x) {
    BIG_UNARY(z, 0, {
        if (x->neg)
            panic_str(BURROW_S("square root of negative number"));
        z->neg = false;
        z->abs = nat_sqrt(z->abs, x->abs);
    });
}

bool big_int_probably_prime(const BigInt *x, Int n) {
    big_enter();
    bool p = bi_probably_prime(x, n);
    big_leave();
    return p;
}

/* ------------------------------------------------- public: conversions */

Str big_accuracy_string(BigAccuracy i, Alloc *a) {
    switch (i) {
    case BIG_BELOW:
        return str_clone(a, BURROW_S("Below"));
    case BIG_EXACT:
        return str_clone(a, BURROW_S("Exact"));
    case BIG_ABOVE:
        return str_clone(a, BURROW_S("Above"));
    default:
        return fmt_sprintf_v(a, "Accuracy(%d)", (int)i);
    }
}

Str big_rounding_mode_string(BigRoundingMode i, Alloc *a) {
    static const char *const names[] = {"ToNearestEven", "ToNearestAway",
                                        "ToZero",        "AwayFromZero",
                                        "ToNegativeInf", "ToPositiveInf"};
    if (i < sizeof names / sizeof names[0])
        return str_clone(a, str_from_cstr(names[i]));
    return fmt_sprintf_v(a, "RoundingMode(%d)", (int)i);
}

Str big_int_text(const BigInt *x, Alloc *a, Int base) {
    if (x == NULL)
        return str_clone(a, BURROW_S("<nil>"));
    big_enter();
    Str s = str_clone(a, nat_itoa(x->abs, x->neg, big_base(base)));
    big_leave();
    return s;
}

Str big_int_string(const BigInt *x, Alloc *a) {
    return big_int_text(x, a, 10);
}

/* append(buf, s...) for a byte slice that may still be the zero Slice. */
static Slice big_append_str(Alloc *a, Slice buf, Str s) {
    if (buf.elem == NULL)
        buf.elem = TYPE_BYTE;
    Slice r = slice_append(a, buf, s.p, s.len);
    if (r.p == NULL && s.len > 0)
        big_oom();
    return r;
}

Slice big_int_append(const BigInt *x, Alloc *a, Slice buf, Int base) {
    if (x == NULL)
        return big_append_str(a, buf, BURROW_S("<nil>"));
    big_enter();
    Slice r = big_append_str(a, buf, nat_itoa(x->abs, x->neg, big_base(base)));
    big_leave();
    return r;
}

/* writeMultiple: text count times, into s. */
static void big_write_multiple(FmtState s, Str text, Int count) {
    if (text.len <= 0 || count <= 0)
        return;
    Slice b = slice_from((void *)(uintptr_t)text.p, text.len, text.len, TYPE_BYTE);
    for (Int i = 0; i < count; i++)
        s.vt->write(s.data, b, NULL);
}

void big_int_format(const BigInt *x, FmtState s, Rune ch) {
    /* determine base */
    int base;
    switch (ch) {
    case 'b':
        base = 2;
        break;
    case 'o':
    case 'O':
        base = 8;
        break;
    case 'd':
    case 's':
    case 'v':
        base = 10;
        break;
    case 'x':
    case 'X':
        base = 16;
        break;
    default: {
        /* unknown format */
        IoWriter w = fmt_state_writer(&s);
        big_enter();
        Str xs = x == NULL ? BURROW_S("<nil>") : nat_itoa(x->abs, x->neg, 10);
        fmt_fprintf_v(w, "%%!%c(big.Int=%s)", ch, xs);
        big_leave();
        return;
    }
    }

    if (x == NULL) {
        big_write_multiple(s, BURROW_S("<nil>"), 1);
        return;
    }

    /* determine sign character */
    Str sign = BURROW_STR_EMPTY;
    if (x->neg)
        sign = BURROW_S("-");
    else if (s.vt->flag(s.data, '+')) /* supersedes ' ' when both specified */
        sign = BURROW_S("+");
    else if (s.vt->flag(s.data, ' '))
        sign = BURROW_S(" ");

    /* determine prefix characters for indicating output base */
    Str prefix = BURROW_STR_EMPTY;
    if (s.vt->flag(s.data, '#')) {
        switch (ch) {
        case 'b': /* binary */
            prefix = BURROW_S("0b");
            break;
        case 'o': /* octal */
            prefix = BURROW_S("0");
            break;
        case 'x': /* hexadecimal */
            prefix = BURROW_S("0x");
            break;
        case 'X':
            prefix = BURROW_S("0X");
            break;
        default:
            break;
        }
    }
    if (ch == 'O')
        prefix = BURROW_S("0o");

    big_enter();
    Str digits = nat_utoa(x->abs, base);
    if (ch == 'X') {
        /* nat_utoa's buffer is scratch memory of our own */
        Byte *d = (Byte *)(uintptr_t)digits.p;
        for (Int i = 0; i < digits.len; i++)
            if ('a' <= d[i] && d[i] <= 'z')
                d[i] = (Byte)('A' + (d[i] - 'a'));
    }

    /* number of characters for the three classes of number padding */
    Int left =
        0; /* space characters to left of digits for right justification ("%8d") */
    Int zeros = 0; /* zero characters (actually cs[0]) as left-most digits ("%.8d") */
    Int right =
        0; /* space characters to right of digits for left justification ("%-8d") */

    /* determine number padding from precision: the least number of digits to
     * output */
    bool precision_set;
    Int precision = s.vt->precision(s.data, &precision_set);
    if (precision_set) {
        if (digits.len < precision) {
            zeros = precision - digits.len; /* count of zero padding */
        } else if (digits.len == 1 && digits.p[0] == '0' && precision == 0) {
            big_leave();
            return; /* print nothing if zero value (x == 0) and zero precision ("." or ".0") */
        }
    }

    /* determine field pad from width: the least number of characters to
     * output */
    Int length = sign.len + prefix.len + zeros + digits.len;
    bool width_set;
    Int width = s.vt->width(s.data, &width_set);
    if (width_set && length < width) { /* pad as specified */
        Int d = width - length;
        if (s.vt->flag(s.data, '-'))
            /* pad on the right with spaces; supersedes '0' when both specified */
            right = d;
        else if (s.vt->flag(s.data, '0') && !precision_set)
            /* pad with zeros unless precision also specified */
            zeros = d;
        else
            /* pad on the left with spaces */
            left = d;
    }

    /* print number as [left pad][sign][prefix][zero pad][digits][right pad] */
    big_write_multiple(s, BURROW_S(" "), left);
    big_write_multiple(s, sign, 1);
    big_write_multiple(s, prefix, 1);
    big_write_multiple(s, BURROW_S("0"), zeros);
    big_write_multiple(s, digits, 1);
    big_write_multiple(s, BURROW_S(" "), right);
    big_leave();
}

Error big_int_scan(BigInt *z, FmtScanState s, Rune ch) {
    s.vt->skip_space(s.data); /* skip leading space characters */
    int base = 0;
    switch (ch) {
    case 'b':
        base = 2;
        break;
    case 'o':
        base = 8;
        break;
    case 'd':
        base = 10;
        break;
    case 'x':
    case 'X':
        base = 16;
        break;
    case 's':
    case 'v':
        /* let scan determine the base */
        break;
    default:
        return errors_new(error_allocator(), BURROW_S("Int.Scan: invalid verb"));
    }
    big_enter();
    Nat orig = z->abs;
    BigScanner r;
    big_scanner_init_state(&r, &s);
    int b;
    Error err;
    bi_scan(z, &r, base, &b, &err);
    big_done(z, orig);
    return err;
}

/* ---------------------------------------------------------- intmarsh.go */

/* Gob codec version. Permits backward-compatible changes to the encoding. */
#define BIG_INT_GOB_VERSION 1

Slice big_int_gob_encode(const BigInt *x, Alloc *a, Error *err) {
    BURROW_OUT(err, BURROW_NO_ERROR);
    if (x == NULL)
        return slice_nil(TYPE_BYTE);
    Int n = 1 + x->abs.len * BIG_S; /* extra byte for version and sign bit */
    Slice buf = slice_make(a, TYPE_BYTE, n, n);
    if (buf.p == NULL)
        big_oom();
    Int i = nat_bytes(x->abs, buf) - 1; /* i >= 0 */
    Byte b = BIG_INT_GOB_VERSION << 1;  /* make space for sign bit */
    if (x->neg)
        b |= 1;
    ((Byte *)buf.p)[i] = b;
    if (i > 0)
        memmove(buf.p, (Byte *)buf.p + i, (size_t)(n - i));
    buf.len = n - i;
    return buf;
}

Error big_int_gob_decode(BigInt *z, Slice buf) {
    if (buf.len == 0) {
        /* Other side sent a nil or default value. */
        z->neg = false;
        z->abs.len = 0;
        return BURROW_NO_ERROR;
    }
    Byte b = ((const Byte *)buf.p)[0];
    if (b >> 1 != BIG_INT_GOB_VERSION)
        return fmt_errorf_v("Int.GobDecode: encoding version %d not supported",
                            (int)(b >> 1));
    big_enter();
    Nat orig = z->abs;
    z->neg = (b & 1) != 0;
    z->abs = nat_set_bytes(z->abs, slice_sub(buf, 1, buf.len));
    big_done(z, orig);
    return BURROW_NO_ERROR;
}

Slice big_int_append_text(const BigInt *x, Alloc *a, Slice b, Error *err) {
    BURROW_OUT(err, BURROW_NO_ERROR);
    return big_int_append(x, a, b, 10);
}

Slice big_int_marshal_text(const BigInt *x, Alloc *a, Error *err) {
    return big_int_append_text(x, a, slice_nil(TYPE_BYTE), err);
}

Error big_int_unmarshal_text(BigInt *z, Slice text) {
    big_enter();
    Nat orig = z->abs;
    BigScanner r;
    Str s = {text.p, text.len};
    big_scanner_init(&r, s);
    bool ok = bi_set_from_scanner(z, &r, 0);
    big_done(z, orig);
    if (!ok)
        return fmt_errorf_v("math/big: cannot unmarshal %q into a *big.Int", s);
    return BURROW_NO_ERROR;
}

Slice big_int_marshal_json(const BigInt *x, Alloc *a, Error *err) {
    BURROW_OUT(err, BURROW_NO_ERROR);
    if (x == NULL)
        return big_append_str(a, slice_nil(TYPE_BYTE), BURROW_S("null"));
    return big_int_append(x, a, slice_nil(TYPE_BYTE), 10);
}

Error big_int_unmarshal_json(BigInt *z, Slice text) {
    /* Ignore null, like in the main JSON package. */
    if (text.len == 4 && memcmp(text.p, "null", 4) == 0)
        return BURROW_NO_ERROR;
    return big_int_unmarshal_text(z, text);
}

/* ------------------------------------------------------------- the type */

static Slice big_int_m_append_text(BigInt *self, Alloc *a, Slice b, Error *err) {
    return big_int_append_text(self, a, b, err);
}

static void big_int_m_format(BigInt *self, FmtState s, Rune ch) {
    big_int_format(self, s, ch);
}

static Error big_int_m_gob_decode(BigInt *self, Alloc *a, Slice buf) {
    (void)a;
    return big_int_gob_decode(self, buf);
}

static Slice big_int_m_gob_encode(BigInt *self, Alloc *a, Error *err) {
    return big_int_gob_encode(self, a, err);
}

static Slice big_int_m_marshal_json(BigInt *self, Alloc *a, Error *err) {
    return big_int_marshal_json(self, a, err);
}

static Slice big_int_m_marshal_text(BigInt *self, Alloc *a, Error *err) {
    return big_int_marshal_text(self, a, err);
}

static Error big_int_m_scan(BigInt *self, FmtScanState s, Rune ch) {
    return big_int_scan(self, s, ch);
}

static Str big_int_m_string(BigInt *self) {
    return big_int_string(self, error_allocator());
}

static Error big_int_m_unmarshal_json(BigInt *self, Alloc *a, Slice text) {
    (void)a;
    return big_int_unmarshal_json(self, text);
}

static Error big_int_m_unmarshal_text(BigInt *self, Alloc *a, Slice text) {
    (void)a;
    return big_int_unmarshal_text(self, text);
}

#define BIG_SIG_FORMAT(IN, OUT) IN(0, FmtState) IN(1, Rune)
#define BIG_SIG_SCAN(IN, OUT) IN(0, FmtScanState) IN(1, Rune) OUT(Error)
#define BIG_SIG_STRING(IN, OUT) OUT(Str)

#define BIG_INT_METHODS(M, T)                                                          \
    M(T, AppendText, big_int_m_append_text, ENCODING_SIG_APPEND_TEXT)                  \
    M(T, Format, big_int_m_format, BIG_SIG_FORMAT)                                     \
    M(T, GobDecode, big_int_m_gob_decode, ENCODING_SIG_UNMARSHAL_BINARY)               \
    M(T, GobEncode, big_int_m_gob_encode, ENCODING_SIG_MARSHAL_BINARY)                 \
    M(T, MarshalJSON, big_int_m_marshal_json, ENCODING_SIG_MARSHAL_TEXT)               \
    M(T, MarshalText, big_int_m_marshal_text, ENCODING_SIG_MARSHAL_TEXT)               \
    M(T, Scan, big_int_m_scan, BIG_SIG_SCAN)                                           \
    M(T, String, big_int_m_string, BIG_SIG_STRING)                                     \
    M(T, UnmarshalJSON, big_int_m_unmarshal_json, ENCODING_SIG_UNMARSHAL_TEXT)         \
    M(T, UnmarshalText, big_int_m_unmarshal_text, ENCODING_SIG_UNMARSHAL_TEXT)

BURROW_METHODS_DEFINE(BigInt, BIG_INT_METHODS);

const Type burrow_type_BigInt = {
    {(const Byte *)"Int", 3},
    {(const Byte *)"math/big", 8},
    KIND_STRUCT,
    (uint32_t)sizeof(BigInt),
    (uint16_t)_Alignof(BigInt),
    0,
    (uint16_t)(sizeof burrow__methods_BigInt / sizeof burrow__methods_BigInt[0]),
    NULL,
    burrow__methods_BigInt,
    NULL,
    NULL,
    0,
    0x62696769U, /* "bigi" */
    NULL,
};

const Type *const TYPE_BIG_INT = &burrow_type_BigInt;
