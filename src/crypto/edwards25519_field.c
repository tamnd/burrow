/* Derived from Go's src/crypto/internal/fips140/edwards25519/field/fe.go and
 * fe_generic.go.
 * Go source: go1.27.1.
 *
 * Go has an assembly multiply and square for amd64. These are the generic ones
 * it falls back to elsewhere, on bits_mul64, which is one instruction wherever
 * the compiler has a 128 bit product.
 *
 * Copyright 2017 The Go Authors. All rights reserved.
 * Copyright 2026 The burrow Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style licence that can be found
 * in the LICENSE file. */

#include "edwards25519.h"

#include "burrow/crypto/subtle.h"
#include "burrow/error.h"
#include "burrow/math/bits.h"
#include "burrow/slice.h"
#include "burrow/type.h"

#include <stdint.h>

#define FE_MASK_LOW_51_BITS ((UINT64_C(1) << 51) - 1)

static const Edwards25519Element fe_zero_value = {0, 0, 0, 0, 0};
static const Edwards25519Element fe_one_value = {1, 0, 0, 0, 0};

/* sqrt(-1), 2^((p-1)/4). */
const Edwards25519Element burrow__fe_sqrt_m1 = {1718705420411056, 234908883556509,
                                                2233514472574048, 2117202627021982,
                                                765476049583133};

static uint64_t fe_le64(const uint8_t *p) {
    return (uint64_t)p[0] | (uint64_t)p[1] << 8 | (uint64_t)p[2] << 16 |
           (uint64_t)p[3] << 24 | (uint64_t)p[4] << 32 | (uint64_t)p[5] << 40 |
           (uint64_t)p[6] << 48 | (uint64_t)p[7] << 56;
}

static void fe_put_le64(uint8_t *p, uint64_t v) {
    for (int i = 0; i < 8; i++)
        p[i] = (uint8_t)(v >> (8 * i));
}

/* --------------------------------------------------------------- generic */

typedef Edwards25519Uint128 FeUint128;

static FeUint128 fe_mul(uint64_t a, uint64_t b) {
    FeUint128 r;
    r.hi = bits_mul64(a, b, &r.lo);
    return r;
}

static FeUint128 fe_add_mul(FeUint128 v, uint64_t a, uint64_t b) {
    uint64_t lo, c;
    uint64_t hi = bits_mul64(a, b, &lo);
    lo = bits_add64(lo, v.lo, 0, &c);
    hi = bits_add64(hi, v.hi, c, &c);
    return (FeUint128){lo, hi};
}

Edwards25519Uint128 burrow__fe_mul64(uint64_t a, uint64_t b) {
    return fe_mul(a, b);
}

Edwards25519Uint128 burrow__fe_add_mul64(Edwards25519Uint128 v, uint64_t a,
                                         uint64_t b) {
    return fe_add_mul(v, a, b);
}

/* v * 19, as v + (v + v<<3)<<1, which is what Go writes so that it is a shift
 * and two adds on machines with a slow multiply. */
static uint64_t fe_mul19(uint64_t v) {
    return v + ((v + (v << 3)) << 1);
}

static FeUint128 fe_add_mul19(FeUint128 v, uint64_t a, uint64_t b) {
    return fe_add_mul(v, fe_mul19(a), b);
}

static FeUint128 fe_add_mul38(FeUint128 v, uint64_t a, uint64_t b) {
    return fe_add_mul(v, fe_mul19(a), b * 2);
}

static uint64_t fe_shift_right_by_51(FeUint128 a) {
    return (a.hi << (64 - 51)) | (a.lo >> 51);
}

void burrow__fe_mul_generic(Edwards25519Element *v, const Edwards25519Element *a,
                            const Edwards25519Element *b) {
    uint64_t a0 = a->l0, a1 = a->l1, a2 = a->l2, a3 = a->l3, a4 = a->l4;
    uint64_t b0 = b->l0, b1 = b->l1, b2 = b->l2, b3 = b->l3, b4 = b->l4;

    /* Limb multiplication works like pen-and-paper columnar multiplication,
     * but with 51-bit limbs instead of digits. The terms that land above 2^255
     * come back down multiplied by 19, by the reduction identity
     * a * 2^255 + b = a * 19 + b mod 2^255 - 19. */
    FeUint128 r0 = fe_mul(a0, b0);
    r0 = fe_add_mul19(r0, a1, b4);
    r0 = fe_add_mul19(r0, a2, b3);
    r0 = fe_add_mul19(r0, a3, b2);
    r0 = fe_add_mul19(r0, a4, b1);

    FeUint128 r1 = fe_mul(a0, b1);
    r1 = fe_add_mul(r1, a1, b0);
    r1 = fe_add_mul19(r1, a2, b4);
    r1 = fe_add_mul19(r1, a3, b3);
    r1 = fe_add_mul19(r1, a4, b2);

    FeUint128 r2 = fe_mul(a0, b2);
    r2 = fe_add_mul(r2, a1, b1);
    r2 = fe_add_mul(r2, a2, b0);
    r2 = fe_add_mul19(r2, a3, b4);
    r2 = fe_add_mul19(r2, a4, b3);

    FeUint128 r3 = fe_mul(a0, b3);
    r3 = fe_add_mul(r3, a1, b2);
    r3 = fe_add_mul(r3, a2, b1);
    r3 = fe_add_mul(r3, a3, b0);
    r3 = fe_add_mul19(r3, a4, b4);

    FeUint128 r4 = fe_mul(a0, b4);
    r4 = fe_add_mul(r4, a1, b3);
    r4 = fe_add_mul(r4, a2, b2);
    r4 = fe_add_mul(r4, a3, b1);
    r4 = fe_add_mul(r4, a4, b0);

    /* Then each coefficient carries into the one above, as in carry_propagate
     * but wider. r0 is below (1 + 19*4) * 2^52 * 2^52 < 2^111, so no carry is
     * more than 60 bits, and r4 is below 5 * 2^104 < 2^107, so 19 * c4 fits in
     * 61. One more carry chain then leaves every limb just above 2^51. */
    uint64_t c0 = fe_shift_right_by_51(r0);
    uint64_t c1 = fe_shift_right_by_51(r1);
    uint64_t c2 = fe_shift_right_by_51(r2);
    uint64_t c3 = fe_shift_right_by_51(r3);
    uint64_t c4 = fe_shift_right_by_51(r4);

    uint64_t rr0 = (r0.lo & FE_MASK_LOW_51_BITS) + fe_mul19(c4);
    uint64_t rr1 = (r1.lo & FE_MASK_LOW_51_BITS) + c0;
    uint64_t rr2 = (r2.lo & FE_MASK_LOW_51_BITS) + c1;
    uint64_t rr3 = (r3.lo & FE_MASK_LOW_51_BITS) + c2;
    uint64_t rr4 = (r4.lo & FE_MASK_LOW_51_BITS) + c3;

    v->l0 = (rr0 & FE_MASK_LOW_51_BITS) + fe_mul19(rr4 >> 51);
    v->l1 = (rr1 & FE_MASK_LOW_51_BITS) + (rr0 >> 51);
    v->l2 = (rr2 & FE_MASK_LOW_51_BITS) + (rr1 >> 51);
    v->l3 = (rr3 & FE_MASK_LOW_51_BITS) + (rr2 >> 51);
    v->l4 = (rr4 & FE_MASK_LOW_51_BITS) + (rr3 >> 51);
}

/* One squaring of the five limbs in l, in place. Squaring needs only half the
 * products a multiply does, since l_i * l_j and l_j * l_i are the same, so
 * those are doubled instead, which is where the 38s come from. */
static void fe_square_limbs(uint64_t l[5]) {
    uint64_t l0 = l[0], l1 = l[1], l2 = l[2], l3 = l[3], l4 = l[4];

    FeUint128 r0 = fe_mul(l0, l0);
    r0 = fe_add_mul38(r0, l1, l4);
    r0 = fe_add_mul38(r0, l2, l3);

    FeUint128 r1 = fe_mul(l0 * 2, l1);
    r1 = fe_add_mul38(r1, l2, l4);
    r1 = fe_add_mul19(r1, l3, l3);

    FeUint128 r2 = fe_mul(l0 * 2, l2);
    r2 = fe_add_mul(r2, l1, l1);
    r2 = fe_add_mul38(r2, l3, l4);

    FeUint128 r3 = fe_mul(l0 * 2, l3);
    r3 = fe_add_mul(r3, l1 * 2, l2);
    r3 = fe_add_mul19(r3, l4, l4);

    FeUint128 r4 = fe_mul(l0 * 2, l4);
    r4 = fe_add_mul(r4, l1 * 2, l3);
    r4 = fe_add_mul(r4, l2, l2);

    uint64_t c0 = fe_shift_right_by_51(r0);
    uint64_t c1 = fe_shift_right_by_51(r1);
    uint64_t c2 = fe_shift_right_by_51(r2);
    uint64_t c3 = fe_shift_right_by_51(r3);
    uint64_t c4 = fe_shift_right_by_51(r4);

    uint64_t rr0 = (r0.lo & FE_MASK_LOW_51_BITS) + fe_mul19(c4);
    uint64_t rr1 = (r1.lo & FE_MASK_LOW_51_BITS) + c0;
    uint64_t rr2 = (r2.lo & FE_MASK_LOW_51_BITS) + c1;
    uint64_t rr3 = (r3.lo & FE_MASK_LOW_51_BITS) + c2;
    uint64_t rr4 = (r4.lo & FE_MASK_LOW_51_BITS) + c3;

    l[0] = (rr0 & FE_MASK_LOW_51_BITS) + fe_mul19(rr4 >> 51);
    l[1] = (rr1 & FE_MASK_LOW_51_BITS) + (rr0 >> 51);
    l[2] = (rr2 & FE_MASK_LOW_51_BITS) + (rr1 >> 51);
    l[3] = (rr3 & FE_MASK_LOW_51_BITS) + (rr2 >> 51);
    l[4] = (rr4 & FE_MASK_LOW_51_BITS) + (rr3 >> 51);
}

void burrow__fe_square_generic(Edwards25519Element *v, const Edwards25519Element *a) {
    uint64_t l[5] = {a->l0, a->l1, a->l2, a->l3, a->l4};
    fe_square_limbs(l);
    v->l0 = l[0];
    v->l1 = l[1];
    v->l2 = l[2];
    v->l3 = l[3];
    v->l4 = l[4];
}

/* Brings the limbs back below 2^52 by carrying each one's top bits into the
 * next, and the top limb's into the bottom times 19. */
Edwards25519Element *burrow__fe_carry_propagate(Edwards25519Element *v) {
    uint64_t l0 = v->l0;
    v->l0 = (v->l0 & FE_MASK_LOW_51_BITS) + fe_mul19(v->l4 >> 51);
    v->l4 = (v->l4 & FE_MASK_LOW_51_BITS) + (v->l3 >> 51);
    v->l3 = (v->l3 & FE_MASK_LOW_51_BITS) + (v->l2 >> 51);
    v->l2 = (v->l2 & FE_MASK_LOW_51_BITS) + (v->l1 >> 51);
    v->l1 = (v->l1 & FE_MASK_LOW_51_BITS) + (l0 >> 51);
    return v;
}

/* ------------------------------------------------------------------- fe */

Edwards25519Element *burrow__fe_zero(Edwards25519Element *v) {
    *v = fe_zero_value;
    return v;
}

Edwards25519Element *burrow__fe_one(Edwards25519Element *v) {
    *v = fe_one_value;
    return v;
}

Edwards25519Element *burrow__fe_reduce(Edwards25519Element *v) {
    burrow__fe_carry_propagate(v);

    /* After the light reduction v < 2^255 + 2^13 * 19, but it has to be below
     * 2^255 - 19. If v >= 2^255 - 19, then v + 19 >= 2^255, which carries out
     * of the top limb, so c is 0 if v < 2^255 - 19 and 1 otherwise. */
    uint64_t c = (v->l0 + 19) >> 51;
    c = (v->l1 + c) >> 51;
    c = (v->l2 + c) >> 51;
    c = (v->l3 + c) >> 51;
    c = (v->l4 + c) >> 51;

    /* If v < 2^255 - 19 and c = 0, this does nothing. Otherwise it applies the
     * reduction identity to the carry. */
    v->l0 += 19 * c;

    v->l1 += v->l0 >> 51;
    v->l0 = v->l0 & FE_MASK_LOW_51_BITS;
    v->l2 += v->l1 >> 51;
    v->l1 = v->l1 & FE_MASK_LOW_51_BITS;
    v->l3 += v->l2 >> 51;
    v->l2 = v->l2 & FE_MASK_LOW_51_BITS;
    v->l4 += v->l3 >> 51;
    v->l3 = v->l3 & FE_MASK_LOW_51_BITS;
    /* no additional carry */
    v->l4 = v->l4 & FE_MASK_LOW_51_BITS;

    return v;
}

Edwards25519Element *burrow__fe_add(Edwards25519Element *v,
                                    const Edwards25519Element *a,
                                    const Edwards25519Element *b) {
    v->l0 = a->l0 + b->l0;
    v->l1 = a->l1 + b->l1;
    v->l2 = a->l2 + b->l2;
    v->l3 = a->l3 + b->l3;
    v->l4 = a->l4 + b->l4;
    return burrow__fe_carry_propagate(v);
}

Edwards25519Element *burrow__fe_subtract(Edwards25519Element *v,
                                         const Edwards25519Element *a,
                                         const Edwards25519Element *b) {
    /* 2 * p first, so the subtraction cannot go below zero, and then b, which
     * can be up to 2^255 + 2^13 * 19. */
    v->l0 = (a->l0 + UINT64_C(0xFFFFFFFFFFFDA)) - b->l0;
    v->l1 = (a->l1 + UINT64_C(0xFFFFFFFFFFFFE)) - b->l1;
    v->l2 = (a->l2 + UINT64_C(0xFFFFFFFFFFFFE)) - b->l2;
    v->l3 = (a->l3 + UINT64_C(0xFFFFFFFFFFFFE)) - b->l3;
    v->l4 = (a->l4 + UINT64_C(0xFFFFFFFFFFFFE)) - b->l4;
    return burrow__fe_carry_propagate(v);
}

Edwards25519Element *burrow__fe_negate(Edwards25519Element *v,
                                       const Edwards25519Element *a) {
    return burrow__fe_subtract(v, &fe_zero_value, a);
}

Edwards25519Element *burrow__fe_multiply(Edwards25519Element *v,
                                         const Edwards25519Element *x,
                                         const Edwards25519Element *y) {
    burrow__fe_mul_generic(v, x, y);
    return v;
}

Edwards25519Element *burrow__fe_square(Edwards25519Element *v,
                                       const Edwards25519Element *x) {
    burrow__fe_square_generic(v, x);
    return v;
}

Edwards25519Element *burrow__fe_square_n(Edwards25519Element *v,
                                         const Edwards25519Element *x, int n) {
    uint64_t l[5] = {x->l0, x->l1, x->l2, x->l3, x->l4};
    for (int i = 0; i < n; i++)
        fe_square_limbs(l);
    v->l0 = l[0];
    v->l1 = l[1];
    v->l2 = l[2];
    v->l3 = l[3];
    v->l4 = l[4];
    return v;
}

Edwards25519Element *burrow__fe_invert(Edwards25519Element *v,
                                       const Edwards25519Element *z) {
    /* Exponentiation by p - 2, in 254 squarings and 11 multiplications, the
     * squarings grouped for square_n. Zero comes back as zero. */
    Edwards25519Element z11, t0, t1, t2, t;

    burrow__fe_square(&t1, z);           /* 2 */
    burrow__fe_square(&t, &t1);          /* 4 */
    burrow__fe_square(&t, &t);           /* 8 */
    burrow__fe_multiply(&t2, &t, z);     /* 9 */
    burrow__fe_multiply(&z11, &t2, &t1); /* 11 */
    burrow__fe_square(&t, &z11);         /* 22 */
    burrow__fe_multiply(&t0, &t, &t2);   /* 31 = 2^5 - 2^0 */

    burrow__fe_square_n(&t, &t0, 5);   /* 2^10 - 2^5 */
    burrow__fe_multiply(&t2, &t, &t0); /* 2^10 - 1 */

    burrow__fe_square_n(&t, &t2, 5);   /* 2^15 - 2^5 */
    burrow__fe_multiply(&t0, &t, &t0); /* 2^15 - 1 */

    burrow__fe_square_n(&t, &t0, 15);  /* 2^30 - 2^15 */
    burrow__fe_multiply(&t1, &t, &t0); /* 2^30 - 1 */

    burrow__fe_square_n(&t, &t1, 30);  /* 2^60 - 2^30 */
    burrow__fe_multiply(&t0, &t, &t1); /* 2^60 - 1 */

    burrow__fe_square_n(&t, &t0, 60);  /* 2^120 - 2^60 */
    burrow__fe_multiply(&t1, &t, &t0); /* 2^120 - 1 */

    burrow__fe_square_n(&t, &t1, 120); /* 2^240 - 2^120 */
    burrow__fe_multiply(&t, &t, &t1);  /* 2^240 - 1 */

    burrow__fe_square_n(&t, &t, 10);  /* 2^250 - 2^10 */
    burrow__fe_multiply(&t, &t, &t2); /* 2^250 - 1 */

    burrow__fe_square_n(&t, &t, 5); /* 2^255 - 2^5 */

    return burrow__fe_multiply(v, &t, &z11); /* 2^255 - 21 */
}

Edwards25519Element *burrow__fe_set(Edwards25519Element *v,
                                    const Edwards25519Element *a) {
    *v = *a;
    return v;
}

Edwards25519Element *burrow__fe_set_bytes(Edwards25519Element *v, Slice x, Error *err) {
    if (x.len != 32) {
        BURROW_OUT(err, errors_new(error_allocator(),
                                   BURROW_S("edwards25519: invalid field element input "
                                            "size")));
        return NULL;
    }
    const uint8_t *p = x.p;

    /* Bits 0:51 (bytes 0:8, bits 0:64, shift 0, mask 51). */
    v->l0 = fe_le64(p) & FE_MASK_LOW_51_BITS;
    /* Bits 51:102 (bytes 6:14, bits 48:112, shift 3, mask 51). */
    v->l1 = (fe_le64(p + 6) >> 3) & FE_MASK_LOW_51_BITS;
    /* Bits 102:153 (bytes 12:20, bits 96:160, shift 6, mask 51). */
    v->l2 = (fe_le64(p + 12) >> 6) & FE_MASK_LOW_51_BITS;
    /* Bits 153:204 (bytes 19:27, bits 152:216, shift 1, mask 51). */
    v->l3 = (fe_le64(p + 19) >> 1) & FE_MASK_LOW_51_BITS;
    /* Bits 204:255 (bytes 24:32, bits 192:256, shift 12, mask 51). Not bytes
     * 25:33 with shift 4, which would read past the end. */
    v->l4 = (fe_le64(p + 24) >> 12) & FE_MASK_LOW_51_BITS;

    BURROW_OUT(err, (Error){0});
    return v;
}

void burrow__fe_bytes(const Edwards25519Element *v, uint8_t out[32]) {
    Edwards25519Element t = *v;
    burrow__fe_reduce(&t);

    /* Five 51 bit limbs into four 64 bit words:
     *
     *  255    204    153    102     51      0
     *    |--l4--|--l3--|--l2--|--l1--|--l0--|
     *   |---u3---|---u2---|---u1---|---u0---|
     * 256      192      128       64        0 */
    uint64_t u0 = t.l1 << 51 | t.l0;
    uint64_t u1 = t.l2 << (102 - 64) | t.l1 >> (64 - 51);
    uint64_t u2 = t.l3 << (153 - 128) | t.l2 >> (128 - 102);
    uint64_t u3 = t.l4 << (204 - 192) | t.l3 >> (192 - 153);

    fe_put_le64(out, u0);
    fe_put_le64(out + 8, u1);
    fe_put_le64(out + 16, u2);
    fe_put_le64(out + 24, u3);
}

int burrow__fe_equal(const Edwards25519Element *v, const Edwards25519Element *u) {
    uint8_t sa[32], sv[32];
    burrow__fe_bytes(u, sa);
    burrow__fe_bytes(v, sv);
    return (int)subtle_constant_time_compare(slice_from(sa, 32, 32, TYPE_BYTE),
                                             slice_from(sv, 32, 32, TYPE_BYTE));
}

/* All ones if cond is 1, and 0 if it is 0. */
static uint64_t fe_mask64_bits(int cond) {
    return ~((uint64_t)cond - 1);
}

Edwards25519Element *burrow__fe_select(Edwards25519Element *v,
                                       const Edwards25519Element *a,
                                       const Edwards25519Element *b, int cond) {
    uint64_t m = fe_mask64_bits(cond);
    v->l0 = (m & a->l0) | (~m & b->l0);
    v->l1 = (m & a->l1) | (~m & b->l1);
    v->l2 = (m & a->l2) | (~m & b->l2);
    v->l3 = (m & a->l3) | (~m & b->l3);
    v->l4 = (m & a->l4) | (~m & b->l4);
    return v;
}

void burrow__fe_swap(Edwards25519Element *v, Edwards25519Element *u, int cond) {
    uint64_t m = fe_mask64_bits(cond);
    uint64_t t = m & (v->l0 ^ u->l0);
    v->l0 ^= t;
    u->l0 ^= t;
    t = m & (v->l1 ^ u->l1);
    v->l1 ^= t;
    u->l1 ^= t;
    t = m & (v->l2 ^ u->l2);
    v->l2 ^= t;
    u->l2 ^= t;
    t = m & (v->l3 ^ u->l3);
    v->l3 ^= t;
    u->l3 ^= t;
    t = m & (v->l4 ^ u->l4);
    v->l4 ^= t;
    u->l4 ^= t;
}

int burrow__fe_is_negative(const Edwards25519Element *v) {
    uint8_t b[32];
    burrow__fe_bytes(v, b);
    return b[0] & 1;
}

Edwards25519Element *burrow__fe_absolute(Edwards25519Element *v,
                                         const Edwards25519Element *u) {
    Edwards25519Element neg;
    burrow__fe_negate(&neg, u);
    return burrow__fe_select(v, &neg, u, burrow__fe_is_negative(u));
}

/* lo + hi * 2^51 = a * b. */
static uint64_t fe_mul51(uint64_t a, uint32_t b, uint64_t *hi) {
    uint64_t ml;
    uint64_t mh = bits_mul64(a, b, &ml);
    *hi = (mh << 13) | (ml >> 51);
    return ml & FE_MASK_LOW_51_BITS;
}

Edwards25519Element *burrow__fe_mult32(Edwards25519Element *v,
                                       const Edwards25519Element *x, uint32_t y) {
    uint64_t x0hi, x1hi, x2hi, x3hi, x4hi;
    uint64_t x0lo = fe_mul51(x->l0, y, &x0hi);
    uint64_t x1lo = fe_mul51(x->l1, y, &x1hi);
    uint64_t x2lo = fe_mul51(x->l2, y, &x2hi);
    uint64_t x3lo = fe_mul51(x->l3, y, &x3hi);
    uint64_t x4lo = fe_mul51(x->l4, y, &x4hi);
    v->l0 = x0lo + 19 * x4hi; /* carried over per the reduction identity */
    v->l1 = x1lo + x0hi;
    v->l2 = x2lo + x1hi;
    v->l3 = x3lo + x2hi;
    v->l4 = x4lo + x3hi;
    /* The hi parts are only 32 bits, plus any earlier excess, so the carry
     * propagation can be skipped. */
    return v;
}

Edwards25519Element *burrow__fe_pow22523(Edwards25519Element *v,
                                         const Edwards25519Element *x) {
    Edwards25519Element t0, t1, t2;

    burrow__fe_square(&t0, x);          /* x^2 */
    burrow__fe_multiply(&t1, x, &t0);   /* x^3 */
    burrow__fe_square(&t0, &t1);        /* x^6 */
    burrow__fe_square(&t0, &t0);        /* x^12 */
    burrow__fe_multiply(&t0, &t1, &t0); /* x^15 */
    burrow__fe_square(&t0, &t0);        /* x^30 */
    burrow__fe_multiply(&t0, x, &t0);   /* x^31 = 2^5 - 1 */

    burrow__fe_square_n(&t1, &t0, 5);   /* 2^10 - 2^5 */
    burrow__fe_multiply(&t1, &t1, &t0); /* 2^10 - 1 */

    burrow__fe_square_n(&t2, &t1, 5);   /* 2^15 - 2^5 */
    burrow__fe_multiply(&t0, &t2, &t0); /* 2^15 - 1 */

    burrow__fe_square_n(&t2, &t0, 15);  /* 2^30 - 2^15 */
    burrow__fe_multiply(&t2, &t2, &t0); /* 2^30 - 1 */

    burrow__fe_square_n(&t0, &t2, 30);  /* 2^60 - 2^30 */
    burrow__fe_multiply(&t0, &t0, &t2); /* 2^60 - 1 */

    burrow__fe_square_n(&t2, &t0, 60);  /* 2^120 - 2^60 */
    burrow__fe_multiply(&t2, &t2, &t0); /* 2^120 - 1 */

    burrow__fe_square_n(&t0, &t2, 120); /* 2^240 - 2^120 */
    burrow__fe_multiply(&t0, &t0, &t2); /* 2^240 - 1 */

    burrow__fe_square_n(&t0, &t0, 10);  /* 2^250 - 2^10 */
    burrow__fe_multiply(&t0, &t0, &t1); /* 2^250 - 1 */

    burrow__fe_square_n(&t0, &t0, 2);      /* 2^252 - 4 */
    return burrow__fe_multiply(v, &t0, x); /* 2^252 - 3 */
}

Edwards25519Element *burrow__fe_sqrt_ratio(Edwards25519Element *r,
                                           const Edwards25519Element *u,
                                           const Edwards25519Element *v,
                                           int *was_square) {
    Edwards25519Element t0, v2, uv3, uv7, rr, check, u_neg, r_prime;

    /* r = (u * v3) * (u * v7)^((p-5)/8) */
    burrow__fe_square(&v2, v);
    burrow__fe_multiply(&uv3, u, burrow__fe_multiply(&t0, &v2, v));
    burrow__fe_multiply(&uv7, &uv3, burrow__fe_square(&t0, &v2));
    burrow__fe_multiply(&rr, &uv3, burrow__fe_pow22523(&t0, &uv7));

    burrow__fe_multiply(&check, v, burrow__fe_square(&t0, &rr)); /* check = v * r^2 */

    burrow__fe_negate(&u_neg, u);
    int correct_sign_sqrt = burrow__fe_equal(&check, u);
    int flipped_sign_sqrt = burrow__fe_equal(&check, &u_neg);
    int flipped_sign_sqrt_i =
        burrow__fe_equal(&check, burrow__fe_multiply(&t0, &u_neg, &burrow__fe_sqrt_m1));

    burrow__fe_multiply(&r_prime, &rr, &burrow__fe_sqrt_m1); /* r_prime = SQRT_M1 * r */
    /* r = CT_SELECT(r_prime IF flipped_sign_sqrt | flipped_sign_sqrt_i ELSE r) */
    burrow__fe_select(&rr, &r_prime, &rr, flipped_sign_sqrt | flipped_sign_sqrt_i);

    burrow__fe_absolute(r, &rr); /* the non-negative square root */
    *was_square = correct_sign_sqrt | flipped_sign_sqrt;
    return r;
}
