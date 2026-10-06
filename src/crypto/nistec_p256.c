/* Derived from Go's src/crypto/internal/fips140/nistec/p256.go.
 * Go source: go1.27.1.
 *
 * P-256 is the curve most of the world uses, so Go gives it more than the
 * other three: a scalar multiplication with a signed window of five bits, and
 * for the generator one of six bits over 43 tables of precomputed affine
 * points, which take the doublings out of the loop altogether. This is the
 * pure Go version of that, which Go uses where it has no assembly. The field
 * arithmetic, SetBytes, Bytes, Add and Double are the same as for the other
 * curves, and tools/gen-nistec.sh writes them to nistec_p256_point.h. The
 * tables are in nistec_p256_table.h.
 *
 * Copyright 2022 The Go Authors. All rights reserved.
 * Copyright 2026 The burrow Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style licence that can be found
 * in the LICENSE file. */

#include "nistec.h"

#include "burrow/core.h"
#include "burrow/crypto/subtle.h"
#include "burrow/error.h"
#include "burrow/panic.h"
#include "burrow/slice.h"

#include "nistec_p256_point.h"
#include "nistec_p256_table.h"

#include <stdint.h>
#include <string.h>

/* ------------------------------------------------------------ affine points */

/* A point in affine coordinates (x, y), still in the Montgomery domain. It
 * cannot be the point at infinity. */
typedef struct P256AffinePoint {
    NistecP256Element x, y;
} P256AffinePoint;

/* Sets q = p1 + p2 if infinity is 0, and q = p1 if it is 1. p2 cannot be the
 * point at infinity, as affine coordinates have no way to say it, so callers
 * set p2 to any point and infinity to 1 instead.
 *
 * This is the complete mixed addition formula for a = -3 from "Complete
 * addition formulas for prime order elliptic curves",
 * https://eprint.iacr.org/2015/1060, Algorithm 5. */
static NistecP256Point *p256_point_add_affine(NistecP256Point *q,
                                              const NistecP256Point *p1,
                                              const P256AffinePoint *p2, int infinity) {
    NistecP256Element t0, t1, t2, t3, t4, x3, y3, z3;
    p256_mul(&t0, &p1->x, &p2->x);   /* t0 ← X1 · X2 */
    p256_mul(&t1, &p1->y, &p2->y);   /* t1 ← Y1 · Y2 */
    p256_add(&t3, &p2->x, &p2->y);   /* t3 ← X2 + Y2 */
    p256_add(&t4, &p1->x, &p1->y);   /* t4 ← X1 + Y1 */
    p256_mul(&t3, &t3, &t4);         /* t3 ← t3 · t4 */
    p256_add(&t4, &t0, &t1);         /* t4 ← t0 + t1 */
    p256_sub(&t3, &t3, &t4);         /* t3 ← t3 − t4 */
    p256_mul(&t4, &p2->y, &p1->z);   /* t4 ← Y2 · Z1 */
    p256_add(&t4, &t4, &p1->y);      /* t4 ← t4 + Y1 */
    p256_mul(&y3, &p2->x, &p1->z);   /* Y3 ← X2 · Z1 */
    p256_add(&y3, &y3, &p1->x);      /* Y3 ← Y3 + X1 */
    p256_mul(&z3, p256_b(), &p1->z); /* Z3 ← b · Z1 */
    p256_sub(&x3, &y3, &z3);         /* X3 ← Y3 − Z3 */
    p256_add(&z3, &x3, &x3);         /* Z3 ← X3 + X3 */
    p256_add(&x3, &x3, &z3);         /* X3 ← X3 + Z3 */
    p256_sub(&z3, &t1, &x3);         /* Z3 ← t1 − X3 */
    p256_add(&x3, &t1, &x3);         /* X3 ← t1 + X3 */
    p256_mul(&y3, p256_b(), &y3);    /* Y3 ← b · Y3 */
    p256_add(&t1, &p1->z, &p1->z);   /* t1 ← Z1 + Z1 */
    p256_add(&t2, &t1, &p1->z);      /* t2 ← t1 + Z1 */
    p256_sub(&y3, &y3, &t2);         /* Y3 ← Y3 − t2 */
    p256_sub(&y3, &y3, &t0);         /* Y3 ← Y3 − t0 */
    p256_add(&t1, &y3, &y3);         /* t1 ← Y3 + Y3 */
    p256_add(&y3, &t1, &y3);         /* Y3 ← t1 + Y3 */
    p256_add(&t1, &t0, &t0);         /* t1 ← t0 + t0 */
    p256_add(&t0, &t1, &t0);         /* t0 ← t1 + t0 */
    p256_sub(&t0, &t0, &t2);         /* t0 ← t0 − t2 */
    p256_mul(&t1, &t4, &y3);         /* t1 ← t4 · Y3 */
    p256_mul(&t2, &t0, &y3);         /* t2 ← t0 · Y3 */
    p256_mul(&y3, &x3, &z3);         /* Y3 ← X3 · Z3 */
    p256_add(&y3, &y3, &t2);         /* Y3 ← Y3 + t2 */
    p256_mul(&x3, &t3, &x3);         /* X3 ← t3 · X3 */
    p256_sub(&x3, &x3, &t1);         /* X3 ← X3 − t1 */
    p256_mul(&z3, &t4, &z3);         /* Z3 ← t4 · Z3 */
    p256_mul(&t1, &t3, &t0);         /* t1 ← t3 · t0 */
    p256_add(&z3, &z3, &t1);         /* Z3 ← Z3 + t1 */

    p256_select(&q->x, &p1->x, &x3, infinity);
    p256_select(&q->y, &p1->y, &y3, infinity);
    p256_select(&q->z, &p1->z, &z3, infinity);
    return q;
}

/* Sets y to -y if cond is 1, and leaves it if cond is 0. */
static void p256_negate_y(NistecP256Element *y, int cond) {
    NistecP256Element zero, neg_y;
    memset(&zero, 0, sizeof zero);
    p256_sub(&neg_y, &zero, y);
    p256_select(y, &neg_y, y, cond);
}

/* -------------------------------------------------------- scalar field */

/* A scalar in [0, ord(G)-1], as four 64 bit words, least significant first. */
typedef struct P256OrdElement {
    uint64_t w[4];
} P256OrdElement;

static uint64_t p256_be64(const uint8_t *b) {
    return (uint64_t)b[0] << 56 | (uint64_t)b[1] << 48 | (uint64_t)b[2] << 40 |
           (uint64_t)b[3] << 32 | (uint64_t)b[4] << 24 | (uint64_t)b[5] << 16 |
           (uint64_t)b[6] << 8 | (uint64_t)b[7];
}

/* x - y - b, setting *b to the borrow out. */
static uint64_t p256_sub64(uint64_t x, uint64_t y, uint64_t *b) {
    uint64_t d = x - y - *b;
    *b = ((~x & y) | (~(x ^ y) & d)) >> 63;
    return d;
}

/* Sets s to the big endian value of the 32 bytes at x, reduced modulo the
 * order. Since twice the order is more than 2^256, one conditional subtraction
 * of it is enough. */
static void p256_ord_set_bytes(P256OrdElement *s, const uint8_t *x) {
    s->w[0] = p256_be64(x + 24);
    s->w[1] = p256_be64(x + 16);
    s->w[2] = p256_be64(x + 8);
    s->w[3] = p256_be64(x);

    uint64_t b = 0;
    uint64_t t0 = p256_sub64(s->w[0], 0xf3b9cac2fc632551, &b);
    uint64_t t1 = p256_sub64(s->w[1], 0xbce6faada7179e84, &b);
    uint64_t t2 = p256_sub64(s->w[2], 0xffffffffffffffff, &b);
    uint64_t t3 = p256_sub64(s->w[3], 0xffffffff00000000, &b);
    uint64_t mask = b - 1; /* zero if the subtraction underflowed */
    s->w[0] ^= (t0 ^ s->w[0]) & mask;
    s->w[1] ^= (t1 ^ s->w[1]) & mask;
    s->w[2] ^= (t2 ^ s->w[2]) & mask;
    s->w[3] ^= (t3 ^ s->w[3]) & mask;
}

/* The 64 least significant bits of s >> n, for n less than 256. n leaks
 * through the time this takes, which is fine, as it is the same for every
 * scalar. Go shifts by 64 - n when n is a multiple of 64, which gives zero
 * there and is undefined in C, so that case is left out. */
static uint64_t p256_ord_rsh(const P256OrdElement *s, int n) {
    int i = n / 64;
    n = n % 64;
    uint64_t res = s->w[i] >> n;
    if (i + 1 < 4 && n != 0)
        res |= s->w[i + 1] << (64 - n);
    return res;
}

/* ------------------------------------------------------- scalar mult */

/* The first 16 multiples of a point, at an offset of -1 so that P is at 0 and
 * [16]P at 15. [0]P is the point at infinity and is not stored. */
typedef struct P256Table {
    NistecP256Point p[16];
} P256Table;

/* Sets p to the n-th multiple of the table's point, or to infinity for 0, in
 * time that does not depend on n. n is at most 16. */
static void p256_table_select(const P256Table *table, NistecP256Point *p, uint8_t n) {
    if (n > 16)
        panic_str(BURROW_S("nistec: internal error: p256Table called with "
                           "out-of-bounds value"));
    p256_point_new(p);
    for (uint8_t i = 1; i <= 16; i++) {
        int cond = (int)subtle_constant_time_byte_eq(i, n);
        p256_point_select(p, &table->p[i - 1], p, cond);
    }
}

static void p256_table_compute(P256Table *table, const NistecP256Point *q) {
    table->p[0] = *q;
    for (int i = 1; i < 16; i += 2) {
        p256_point_double(&table->p[i], &table->p[i / 2]);
        if (i + 1 < 16)
            p256_point_add(&table->p[i + 1], &table->p[i], q);
    }
}

/* The signed digit of a window of five bits and the one below it: a magnitude
 * of at most 16 and a sign. */
static uint8_t p256_booth_w5(uint64_t in, int *sign) {
    uint64_t s = ~((in >> 5) - 1);
    uint64_t d = (1 << 6) - in - 1;
    d = (d & s) | (in & ~s);
    d = (d >> 1) + (d & 1);
    *sign = (int)(s & 1);
    return (uint8_t)d;
}

/* Sets p = scalar * q, for a 32 byte big endian scalar. Any other length is an
 * error, and leaves p as it was. */
static NistecP256Point *p256_point_scalar_mult(NistecP256Point *p,
                                               const NistecP256Point *q, Slice scalar,
                                               Error *err) {
    if (scalar.len != 32) {
        BURROW_OUT(err,
                   errors_new(error_allocator(), BURROW_S("invalid scalar length")));
        return NULL;
    }
    P256OrdElement s;
    p256_ord_set_bytes(&s, scalar.p);

    /* The window starts at the most significant bits and moves five at a
     * time, finishing at -1, so it starts at -1 + 5 * 51 = 254. */
    int index = 254;

    /* The sign is always zero here, since the input is at most two bits. */
    int sign;
    uint8_t sel = p256_booth_w5(p256_ord_rsh(&s, index), &sign);

    /* Neither Select nor Add have exceptions for the point at infinity or a
     * zero selector, so there is no need to check for them here or in the
     * loop. q is read only to make the table, so it can be p. */
    P256Table table;
    p256_table_compute(&table, q);
    p256_table_select(&table, p, sel);

    NistecP256Point t;
    p256_point_new(&t);
    while (index >= 4) {
        index -= 5;

        p256_point_double(p, p);
        p256_point_double(p, p);
        p256_point_double(p, p);
        p256_point_double(p, p);
        p256_point_double(p, p);

        if (index >= 0) {
            sel = p256_booth_w5(p256_ord_rsh(&s, index) & 0x3f, &sign);
        } else {
            /* Booth encoding has a zero bit at index -1, so the least
             * significant word goes left by one. */
            uint64_t wvalue = (s.w[0] << 1) & 0x3f;
            sel = p256_booth_w5(wvalue, &sign);
        }

        p256_table_select(&table, &t, sel);
        p256_negate_y(&t.y, sign);
        p256_point_add(p, p, &t);
    }

    BURROW_OUT(err, BURROW_NO_ERROR);
    return p;
}

/* Sets p to the n-th point of the 32 in table i of p256_generator_tables, in
 * time that does not depend on n. n is at most 32, and for 0, p is left as
 * something that is not a point.
 *
 * The tables hold each coordinate as four 64 bit words. On a machine with
 * fiat-crypto's 32 bit field each word is two of its words, low half first. */
static void p256_affine_table_select(int i, P256AffinePoint *p, uint8_t n) {
    if (n > 32)
        panic_str(BURROW_S("nistec: internal error: p256AffineTable.Select called with "
                           "out-of-bounds value"));
    uint64_t x[4] = {0}, y[4] = {0};
    for (uint8_t j = 1; j <= 32; j++) {
        uint64_t mask = (uint64_t)0 - (uint64_t)subtle_constant_time_byte_eq(j, n);
        const uint64_t (*e)[4] = p256_generator_tables[i][j - 1];
        for (int k = 0; k < 4; k++) {
            x[k] |= e[0][k] & mask;
            y[k] |= e[1][k] & mask;
        }
    }
#if NISTEC_P256_LIMBS == 4
    for (int k = 0; k < 4; k++) {
        p->x.l[k] = (NistecLimb)x[k];
        p->y.l[k] = (NistecLimb)y[k];
    }
#else
    for (int k = 0; k < 4; k++) {
        p->x.l[2 * k] = (NistecLimb)(x[k] & 0xffffffff);
        p->x.l[2 * k + 1] = (NistecLimb)(x[k] >> 32);
        p->y.l[2 * k] = (NistecLimb)(y[k] & 0xffffffff);
        p->y.l[2 * k + 1] = (NistecLimb)(y[k] >> 32);
    }
#endif
}

/* The same as p256_booth_w5, for a window of six bits. */
static uint8_t p256_booth_w6(uint64_t in, int *sign) {
    uint64_t s = ~((in >> 6) - 1);
    uint64_t d = (1 << 7) - in - 1;
    d = (d & s) | (in & ~s);
    d = (d >> 1) + (d & 1);
    *sign = (int)(s & 1);
    return (uint8_t)d;
}

/* Sets p = scalar * the generator, for a 32 byte big endian scalar. Any other
 * length is an error, and leaves p as it was.
 *
 * This works like p256_point_scalar_mult, but the table is fixed and each one
 * is the one before doubled six times, so the loop moves to the next table
 * where the other doubles. */
static NistecP256Point *p256_point_scalar_base_mult(NistecP256Point *p, Slice scalar,
                                                    Error *err) {
    if (scalar.len != 32) {
        BURROW_OUT(err,
                   errors_new(error_allocator(), BURROW_S("invalid scalar length")));
        return NULL;
    }
    P256OrdElement s;
    p256_ord_set_bytes(&s, scalar.p);

    /* Six bits at a time, finishing at -1, so starting at -1 + 6 * 42 = 251. */
    int index = 251;

    /* The sign is always zero here, since the input is at most five bits. */
    int sign;
    uint8_t sel = p256_booth_w6(p256_ord_rsh(&s, index), &sign);

    P256AffinePoint t;
    p256_affine_table_select((index + 1) / 6, &t, sel);

    /* What the select gives for a zero selector is not a point, where it
     * should be infinity, which affine coordinates cannot say. So p is set to
     * infinity here if sel is zero, and in the loop p256_point_add_affine does
     * the same. */
    NistecP256Point inf, tp;
    p256_point_new(&inf);
    tp.x = t.x;
    tp.y = t.y;
    p256_one(&tp.z);
    p256_point_select(p, &inf, &tp, (int)subtle_constant_time_byte_eq(sel, 0));

    while (index >= 5) {
        index -= 6;

        if (index >= 0) {
            sel = p256_booth_w6(p256_ord_rsh(&s, index) & 0x7f, &sign);
        } else {
            /* Booth encoding has a zero bit at index -1, so the least
             * significant word goes left by one. */
            uint64_t wvalue = (s.w[0] << 1) & 0x7f;
            sel = p256_booth_w6(wvalue, &sign);
        }

        p256_affine_table_select((index + 1) / 6, &t, sel);
        p256_negate_y(&t.y, sign);
        p256_point_add_affine(p, p, &t, (int)subtle_constant_time_byte_eq(sel, 0));
    }

    BURROW_OUT(err, BURROW_NO_ERROR);
    return p;
}

/* ------------------------------------------------------------------ curve */

static NistecPoint *p256_v_point_new(NistecPoint *p) {
    p256_point_new(&p->p256);
    return p;
}

static NistecPoint *p256_v_set_generator(NistecPoint *p) {
    p256_point_set_generator(&p->p256);
    return p;
}

static NistecPoint *p256_v_set(NistecPoint *p, const NistecPoint *q) {
    p->p256 = q->p256;
    return p;
}

static NistecPoint *p256_v_set_bytes(NistecPoint *p, Slice b, Error *err) {
    return p256_point_set_bytes(&p->p256, b, err) == NULL ? NULL : p;
}

static Int p256_v_bytes(const NistecPoint *p, uint8_t *out) {
    return p256_point_bytes(&p->p256, out);
}

static uint8_t *p256_v_bytes_x(const NistecPoint *p, uint8_t *out, Error *err) {
    return p256_point_bytes_x(&p->p256, out, err);
}

static Int p256_v_bytes_compressed(const NistecPoint *p, uint8_t *out) {
    return p256_point_bytes_compressed(&p->p256, out);
}

static NistecPoint *p256_v_add(NistecPoint *q, const NistecPoint *p1,
                               const NistecPoint *p2) {
    p256_point_add(&q->p256, &p1->p256, &p2->p256);
    return q;
}

static NistecPoint *p256_v_double(NistecPoint *q, const NistecPoint *p) {
    p256_point_double(&q->p256, &p->p256);
    return q;
}

static NistecPoint *p256_v_scalar_mult(NistecPoint *p, const NistecPoint *q,
                                       Slice scalar, Error *err) {
    return p256_point_scalar_mult(&p->p256, &q->p256, scalar, err) == NULL ? NULL : p;
}

static NistecPoint *p256_v_scalar_base_mult(NistecPoint *p, Slice scalar, Error *err) {
    return p256_point_scalar_base_mult(&p->p256, scalar, err) == NULL ? NULL : p;
}

const NistecCurve burrow__nistec_p256 = {
    "P-256",
    p256_element_length,
    p256_v_point_new,
    p256_v_set_generator,
    p256_v_set,
    p256_v_set_bytes,
    p256_v_bytes,
    p256_v_bytes_x,
    p256_v_bytes_compressed,
    p256_v_add,
    p256_v_double,
    p256_v_scalar_mult,
    p256_v_scalar_base_mult,
};
