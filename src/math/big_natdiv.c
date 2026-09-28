/* math/big: division of natural numbers, natdiv.go.
 *
 * Long division of word digits, with the quotient digit guessed from the top
 * two words of the remainder and the top word of the divisor, refined with one
 * more of each, and checked at full width. Past divRecursiveThreshold words it
 * is the same long division over digits of half the divisor's width, with each
 * digit found by a recursive call (Burnikel and Ziegler). Go's natdiv.go has
 * the long explanation and the proofs.
 *
 * Copyright 2009 The Go Authors. All rights reserved.
 * Copyright 2026 The burrow Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style licence that can be found
 * in the LICENSE file. */

#include "big_internal.h"

#include "burrow/panic.h"

#define BIG_DIV_RECURSIVE_THRESHOLD 40

static void nat_div_basic(Nat q, Nat u, Nat v);
static void nat_div_recursive(Nat z, Nat u, Nat v);
static Nat nat_div_large(Nat z, Nat u, Nat u_in, Nat v_in, Nat *r);

/* rem: u mod v, in z. */
Nat nat_rem(Nat z, Nat u, Nat v) {
    if (nat_alias(z, u))
        z = NAT_NIL;
    ArenaMark mark = big_stk_save();
    Int ql = u.len - (v.len - 1);
    Nat q = big_stk_nat(ql > 1 ? ql : 1);
    Nat r;
    nat_div(q, z, u, v, &r);
    big_stk_restore(mark);
    return r;
}

Nat nat_div(Nat z, Nat z2, Nat u, Nat v, Nat *r) {
    if (v.len == 0)
        panic_str(BURROW_S("division by zero"));

    if (v.len == 1) {
        /* A single word divisor. The remainder could go in z2 as a nat, but
         * that would allocate for nothing when the caller only wants q. */
        BigWord r2;
        Nat q = nat_div_w(z, u, v.p[0], &r2);
        *r = nat_set_word(z2, r2);
        return q;
    }

    if (nat_cmp(u, v) < 0) {
        *r = nat_set(z2, u);
        return nat_to(z, 0);
    }

    return nat_div_large(z, z2, u, v, r);
}

/* divWVW: z = (xn<<_W + x) / y, returning the remainder. */
static BigWord nat_div_wvw(Nat z, BigWord xn, Nat x, BigWord y) {
    BigWord r = xn;
    if (x.len == 1) {
        Uint rr;
        z.p[0] = bits_div(r, x.p[0], y, &rr);
        return rr;
    }
    BigWord rec = big_reciprocal_word(y);
    for (Int i = z.len - 1; i >= 0; i--)
        z.p[i] = big_div_ww(r, x.p[i], y, rec, &r);
    return r;
}

Nat nat_div_w(Nat z, Nat x, BigWord y, BigWord *r) {
    Int m = x.len;
    *r = 0;
    if (y == 0)
        panic_str(BURROW_S("division by zero"));
    if (y == 1)
        return nat_set(z, x); /* result is x */
    if (m == 0)
        return nat_to(z, 0); /* result is 0 */
    /* m > 0 */
    z = nat_make(z, m);
    *r = nat_div_wvw(z, 0, x, y);
    return nat_norm(z);
}

BigWord nat_mod_w(Nat x, BigWord d) {
    /* Go computes the quotient too and throws it away. */
    Nat q = nat_make(NAT_NIL, x.len);
    return nat_div_wvw(q, 0, x, d);
}

/* divLarge: u_in / v_in for len(u_in) >= len(v_in) > 1, with the remainder
 * built in u, whose memory may be reused. */
static Nat nat_div_large(Nat z, Nat u, Nat u_in, Nat v_in, Nat *r) {
    Int n = v_in.len;
    Int m = u_in.len - n;

    /* Scale the inputs so that the divisor's top bit is set: the good guess
     * guarantee needs it. The scaled u may grow by a word. */
    ArenaMark mark = big_stk_save();
    Uint shift = big_nlz(v_in.p[n - 1]);
    Nat v = big_stk_nat(n);
    u = nat_make(u, u_in.len + 1);
    if (shift == 0) {
        nat_copy(v, v_in);
        nat_copy(nat_to(u, u_in.len), u_in);
        u.p[u_in.len] = 0;
    } else {
        big_lsh_vu(v, v_in, shift);
        u.p[u_in.len] = big_lsh_vu(nat_to(u, u_in.len), u_in, shift);
    }

    /* The quotient has at most m+1 words, and must not share memory with u. */
    if (nat_alias(z, u))
        z = NAT_NIL;
    Nat q = nat_make(z, m + 1);

    if (n < BIG_DIV_RECURSIVE_THRESHOLD)
        nat_div_basic(q, u, v);
    else
        nat_div_recursive(q, u, v);
    big_stk_restore(mark);

    q = nat_norm(q);

    /* Undo the scaling of the remainder. */
    if (shift != 0)
        big_rsh_vu(u, u, shift);
    *r = nat_norm(u);
    return q;
}

/* x1<<_W + x2 > y1<<_W + y2 */
static inline bool big_greater_than(BigWord x1, BigWord x2, BigWord y1, BigWord y2) {
    return x1 > y1 || (x1 == y1 && x2 > y2);
}

/* divBasic: long division, q = u / v with the remainder left in u. q is
 * len(u) - len(v) + 1 words or, when the top word would be zero, one less. */
static void nat_div_basic(Nat q, Nat u, Nat v) {
    Int n = v.len;
    Int m = u.len - n;

    ArenaMark mark = big_stk_save();
    Nat qhatv = big_stk_nat(n + 1);

    /* Everything about the top word of v, set up once. */
    BigWord vn1 = v.p[n - 1];
    BigWord rec = big_reciprocal_word(vn1);

    /* The top word of the current remainder, which the first step takes to
     * be zero. */
    BigWord ujn = 0;

    for (Int j = m; j >= 0; j--) {
        /* The 2 by 1 guess, and its refinement to 3 by 2. */
        BigWord qhat = BIG_M;
        if (ujn != vn1) {
            BigWord rhat;
            qhat = big_div_ww(ujn, u.p[j + n - 1], vn1, rec, &rhat);

            /* x1 | x2 = q̂v_{n-2} */
            BigWord vn2 = v.p[n - 2];
            BigWord x2;
            BigWord x1 = big_mul_ww(qhat, vn2, &x2);
            BigWord ujn2 = u.p[j + n - 2];
            while (big_greater_than(x1, x2, rhat, ujn2)) {
                qhat--;
                BigWord prev = rhat;
                rhat += vn1;
                /* r̂ overflowed, so the test would be false from here. */
                if (rhat < prev)
                    break;
                if (vn2 > x2)
                    x1--;
                x2 -= vn2;
            }
        }

        /* Subtract q̂·v from the current section of u. When j+n+1 runs past
         * the end of u, which happens on the first step, the top word of q̂·v
         * has to be zero and is left out. */
        qhatv.p[n] = big_mul_add_vww(nat_to(qhatv, n), v, qhat, 0);
        Int qhl = qhatv.len;
        if (j + qhl > u.len && qhatv.p[n] == 0)
            qhl--;
        Nat us = nat_slice(u, j, j + qhl);
        BigWord c = big_sub_vv(us, us, nat_to(qhatv, qhl));
        if (c != 0) {
            Nat un = nat_slice(u, j, j + n);
            c = big_add_vv(un, un, v);
            /* If n == qhl the carry from the subtraction lands in the ignored
             * top word and the two cancel. */
            if (n < qhl)
                u.p[j + n] += c;
            qhat--;
        }

        ujn = u.p[j + n - 1];

        /* Save the quotient digit. A zero top digit, when q was made one
         * word short for it, has nowhere to go and needs none. */
        if (j == m && m == q.len && qhat == 0)
            continue;
        q.p[j] = qhat;
    }

    big_stk_restore(mark);
}

static void nat_div_recursive_step(Nat z, Nat u, Nat v, int depth);

/* divRecursive: z = u / v with the remainder left in u, for len(v) of at
 * least BIG_DIV_RECURSIVE_THRESHOLD. z starts as len(u) - len(v) + 1 words. */
static void nat_div_recursive(Nat z, Nat u, Nat v) {
    nat_clear(z);
    nat_div_recursive_step(z, u, v, 0);
}

/* divRecursiveStep: z += u / v, with the remainder left in u. z must be
 * long enough for the quotient, and depth is only for tracing. */
static void nat_div_recursive_step(Nat z, Nat u, Nat v, int depth) {
    u = nat_norm(u);
    v = nat_norm(v);
    if (u.len == 0) {
        nat_clear(z);
        return;
    }

    /* Short divisors go to long division. */
    Int n = v.len;
    if (n < BIG_DIV_RECURSIVE_THRESHOLD) {
        nat_div_basic(z, u, v);
        return;
    }

    /* u < v: the quotient is zero and u is the remainder already. */
    Int m = u.len - n;
    if (m < 0)
        return;

    /* Long division over wide digits of B words. Each step divides the top
     * B+n words of the current section of u, taking a quotient of B words
     * that the refinement below fixes up by at most one or two. */
    Int b = n / 2;

    ArenaMark mark0 = big_stk_save();
    Nat qhat0 = big_stk_nat(b + 1);

    Int j = m;
    while (j > b) {
        /* Divide u[j-B:j+n] by v, the section of u the next wide digit comes
         * from. Get the 2 by 1 wide digit guess from the top words: uu[s:B+n]
         * over v[s:], which is 3 by 2 in half digits. */
        Int s = b - 1;
        Nat uu = nat_from(u, j - b);

        Nat qhat = qhat0;
        nat_clear(qhat);
        nat_div_recursive_step(qhat, nat_slice(uu, s, b + n), nat_from(v, s),
                               depth + 1);
        qhat = nat_norm(qhat);

        /* The recursive call left the top part of the remainder in
         * uu[s:B+n]. Subtract q̂ times the bottom words of v from uu to
         * finish the remainder, adding v back while it goes negative. That
         * is the refinement and the check in one. */
        ArenaMark mark = big_stk_save();
        Nat qhatv = big_stk_nat(3 * n);
        nat_clear(qhatv);
        qhatv = nat_mul(qhatv, qhat, nat_to(v, s));
        for (int i = 0; i < 2; i++) {
            int e = nat_cmp(qhatv, nat_norm(uu));
            if (e <= 0)
                break;
            big_sub_vw(qhat, qhat, 1);
            BigWord c = big_sub_vv(nat_to(qhatv, s), nat_to(qhatv, s), nat_to(v, s));
            if (qhatv.len > s)
                big_sub_vw(nat_from(qhatv, s), nat_from(qhatv, s), c);
            nat_add_to(nat_from(uu, s), nat_from(v, s));
        }
        if (nat_cmp(qhatv, nat_norm(uu)) > 0)
            panic_str(BURROW_S("impossible"));
        BigWord c = big_sub_vv(nat_to(uu, qhatv.len), nat_to(uu, qhatv.len), qhatv);
        if (c > 0)
            big_sub_vw(nat_from(uu, qhatv.len), nat_from(uu, qhatv.len), c);
        nat_add_to(nat_from(z, j - b), qhat);
        j -= b;
        big_stk_restore(mark);
    }

    /* The last step: what is left of u, j+n words with j <= B, divided the
     * same way. */
    Int s = b - 1;
    Nat qhat = qhat0;
    nat_clear(qhat);
    nat_div_recursive_step(qhat, nat_norm(nat_from(u, s)), nat_from(v, s), depth + 1);
    qhat = nat_norm(qhat);
    Nat qhatv = big_stk_nat(3 * n);
    nat_clear(qhatv);
    qhatv = nat_mul(qhatv, qhat, nat_to(v, s));
    /* Go runs both rounds here without stopping early. */
    for (int i = 0; i < 2; i++) {
        int e = nat_cmp(qhatv, nat_norm(u));
        if (e > 0) {
            big_sub_vw(qhat, qhat, 1);
            BigWord c = big_sub_vv(nat_to(qhatv, s), nat_to(qhatv, s), nat_to(v, s));
            if (qhatv.len > s)
                big_sub_vw(nat_from(qhatv, s), nat_from(qhatv, s), c);
            nat_add_to(nat_from(u, s), nat_from(v, s));
        }
    }
    if (nat_cmp(qhatv, nat_norm(u)) > 0)
        panic_str(BURROW_S("impossible"));
    BigWord c = big_sub_vv(nat_to(u, qhatv.len), nat_to(u, qhatv.len), qhatv);
    if (c > 0)
        c = big_sub_vw(nat_from(u, qhatv.len), nat_from(u, qhatv.len), c);
    if (c > 0)
        panic_str(BURROW_S("impossible"));

    /* Done. */
    nat_add_to(z, nat_norm(qhat));
    big_stk_restore(mark0);
}
