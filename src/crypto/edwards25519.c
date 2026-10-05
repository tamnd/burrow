/* Derived from Go's src/crypto/internal/fips140/edwards25519/edwards25519.go,
 * scalarmult.go and tables.go. Go source: go1.27.1.
 *
 * Go builds the generator's tables the first time they are used. Here they are
 * constants, in edwards25519_table.c.
 *
 * Copyright 2017 The Go Authors. All rights reserved.
 * Copyright 2026 The burrow Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style licence that can be found
 * in the LICENSE file. */

#include "edwards25519.h"

#include "burrow/crypto/subtle.h"
#include "burrow/error.h"
#include "burrow/panic.h"
#include "burrow/slice.h"
#include "burrow/type.h"

#include <stdint.h>

/* d, the curve constant, and 2d. */
const Edwards25519Element burrow__ge_d = {929955233495203, 466365720129213,
                                          1662059464998953, 2033849074728123,
                                          1442794654840575};
const Edwards25519Element burrow__ge_d2 = {1859910466990425, 932731440258426,
                                           1072319116312658, 1815898335770999,
                                           633789495995903};

static const uint8_t ge_generator_bytes[32] = {
    0x58, 0x66, 0x66, 0x66, 0x66, 0x66, 0x66, 0x66, 0x66, 0x66, 0x66,
    0x66, 0x66, 0x66, 0x66, 0x66, 0x66, 0x66, 0x66, 0x66, 0x66, 0x66,
    0x66, 0x66, 0x66, 0x66, 0x66, 0x66, 0x66, 0x66, 0x66, 0x66};

static bool ge_fe_is_zero_value(const Edwards25519Element *v) {
    return (v->l0 | v->l1 | v->l2 | v->l3 | v->l4) == 0;
}

/* A zero Point has x and y both zero, and no point on the curve does, since
 * that is not a solution of the curve equation. */
static void ge_check_initialized(const Edwards25519Point *p) {
    if (ge_fe_is_zero_value(&p->x) && ge_fe_is_zero_value(&p->y))
        panic_str(BURROW_S("edwards25519: use of uninitialized Point"));
}

/* ------------------------------------------------------------ constructors */

static Edwards25519Point *ge_from_const_bytes(Edwards25519Point *v,
                                              const uint8_t b[32]) {
    Error err = {0};
    burrow__ge_set_bytes(v, slice_from((void *)(uintptr_t)b, 32, 32, TYPE_BYTE), &err);
    return v;
}

/* What decoding the bytes 1, 0, 0 ... gives, set directly since every scalar
 * multiplication starts from it. */
Edwards25519Point *burrow__ge_identity(Edwards25519Point *v) {
    burrow__fe_zero(&v->x);
    burrow__fe_one(&v->y);
    burrow__fe_one(&v->z);
    burrow__fe_zero(&v->t);
    return v;
}

Edwards25519Point *burrow__ge_generator(Edwards25519Point *v) {
    return ge_from_const_bytes(v, ge_generator_bytes);
}

Edwards25519ProjP2 *burrow__ge_p2_zero(Edwards25519ProjP2 *v) {
    burrow__fe_zero(&v->X);
    burrow__fe_one(&v->Y);
    burrow__fe_one(&v->Z);
    return v;
}

Edwards25519ProjCached *burrow__ge_cached_zero(Edwards25519ProjCached *v) {
    burrow__fe_one(&v->YplusX);
    burrow__fe_one(&v->YminusX);
    burrow__fe_one(&v->Z);
    burrow__fe_zero(&v->T2d);
    return v;
}

Edwards25519AffineCached *burrow__ge_affine_zero(Edwards25519AffineCached *v) {
    burrow__fe_one(&v->YplusX);
    burrow__fe_one(&v->YminusX);
    burrow__fe_zero(&v->T2d);
    return v;
}

/* -------------------------------------------------------------- assignments */

Edwards25519Point *burrow__ge_set(Edwards25519Point *v, const Edwards25519Point *u) {
    *v = *u;
    return v;
}

/* ----------------------------------------------------------------- encoding */

void burrow__ge_bytes(const Edwards25519Point *v, uint8_t out[32]) {
    ge_check_initialized(v);

    Edwards25519Element z_inv, x, y;
    burrow__fe_invert(&z_inv, &v->z);       /* z_inv = 1 / Z */
    burrow__fe_multiply(&x, &v->x, &z_inv); /* x = X / Z */
    burrow__fe_multiply(&y, &v->y, &z_inv); /* y = Y / Z */

    burrow__fe_bytes(&y, out);
    out[31] |= (uint8_t)(burrow__fe_is_negative(&x) << 7);
}

Edwards25519Point *burrow__ge_set_bytes(Edwards25519Point *v, Slice x, Error *err) {
    /* Section 5.1.3 of RFC 8032 says a y that is not reduced is invalid, and
     * that is the one check it adds over Section 3.1. Go leaves it out to
     * match most implementations, ref10 among them, which is what consensus
     * protocols depend on. Section 5.1.3 also says x = 0 with the sign bit set
     * is invalid, and that is left out for the same reason. */
    Edwards25519Element y;
    Error ferr;
    if (burrow__fe_set_bytes(&y, x, &ferr) == NULL) {
        BURROW_OUT(err,
                   errors_new(error_allocator(),
                              BURROW_S("edwards25519: invalid point encoding length")));
        return NULL;
    }

    /* -x^2 + y^2 = 1 + d * x^2 * y^2
     * x^2 + d * x^2 * y^2 = y^2 - 1
     * x^2 = (y^2 - 1) / (d * y^2 + 1) */

    /* u = y^2 - 1 */
    Edwards25519Element y2, u, vv, one;
    burrow__fe_one(&one);
    burrow__fe_square(&y2, &y);
    burrow__fe_subtract(&u, &y2, &one);

    /* vv = d * y^2 + 1 */
    burrow__fe_multiply(&vv, &y2, &burrow__ge_d);
    burrow__fe_add(&vv, &vv, &one);

    /* x = +√(u/v) */
    Edwards25519Element xx, xx_neg;
    int was_square;
    burrow__fe_sqrt_ratio(&xx, &u, &vv, &was_square);
    if (was_square == 0) {
        BURROW_OUT(err, errors_new(error_allocator(),
                                   BURROW_S("edwards25519: invalid point encoding")));
        return NULL;
    }

    /* Select the negative square root if the sign bit is set. */
    burrow__fe_negate(&xx_neg, &xx);
    burrow__fe_select(&xx, &xx_neg, &xx, ((const uint8_t *)x.p)[31] >> 7);

    v->x = xx;
    v->y = y;
    burrow__fe_one(&v->z);
    burrow__fe_multiply(&v->t, &xx, &y); /* xy = T / Z */

    BURROW_OUT(err, (Error){0});
    return v;
}

/* --------------------------------------------------- conversions to and from */

Edwards25519ProjP2 *burrow__ge_p2_from_p1xp1(Edwards25519ProjP2 *v,
                                             const Edwards25519ProjP1xP1 *p) {
    burrow__fe_multiply(&v->X, &p->X, &p->T);
    burrow__fe_multiply(&v->Y, &p->Y, &p->Z);
    burrow__fe_multiply(&v->Z, &p->Z, &p->T);
    return v;
}

Edwards25519ProjP2 *burrow__ge_p2_from_p3(Edwards25519ProjP2 *v,
                                          const Edwards25519Point *p) {
    v->X = p->x;
    v->Y = p->y;
    v->Z = p->z;
    return v;
}

Edwards25519Point *burrow__ge_from_p1xp1(Edwards25519Point *v,
                                         const Edwards25519ProjP1xP1 *p) {
    burrow__fe_multiply(&v->x, &p->X, &p->T);
    burrow__fe_multiply(&v->y, &p->Y, &p->Z);
    burrow__fe_multiply(&v->z, &p->Z, &p->T);
    burrow__fe_multiply(&v->t, &p->X, &p->Y);
    return v;
}

Edwards25519Point *burrow__ge_from_p2(Edwards25519Point *v,
                                      const Edwards25519ProjP2 *p) {
    burrow__fe_multiply(&v->x, &p->X, &p->Z);
    burrow__fe_multiply(&v->y, &p->Y, &p->Z);
    burrow__fe_square(&v->z, &p->Z);
    burrow__fe_multiply(&v->t, &p->X, &p->Y);
    return v;
}

Edwards25519ProjCached *burrow__ge_cached_from_p3(Edwards25519ProjCached *v,
                                                  const Edwards25519Point *p) {
    burrow__fe_add(&v->YplusX, &p->y, &p->x);
    burrow__fe_subtract(&v->YminusX, &p->y, &p->x);
    v->Z = p->z;
    burrow__fe_multiply(&v->T2d, &p->t, &burrow__ge_d2);
    return v;
}

Edwards25519AffineCached *burrow__ge_affine_from_p3(Edwards25519AffineCached *v,
                                                    const Edwards25519Point *p) {
    burrow__fe_add(&v->YplusX, &p->y, &p->x);
    burrow__fe_subtract(&v->YminusX, &p->y, &p->x);
    burrow__fe_multiply(&v->T2d, &p->t, &burrow__ge_d2);

    Edwards25519Element inv_z;
    burrow__fe_invert(&inv_z, &p->z);
    burrow__fe_multiply(&v->YplusX, &v->YplusX, &inv_z);
    burrow__fe_multiply(&v->YminusX, &v->YminusX, &inv_z);
    burrow__fe_multiply(&v->T2d, &v->T2d, &inv_z);
    return v;
}

/* --------------------------------------------------------- (re)addition */

Edwards25519ProjP1xP1 *burrow__ge_p1xp1_add(Edwards25519ProjP1xP1 *v,
                                            const Edwards25519Point *p,
                                            const Edwards25519ProjCached *q) {
    Edwards25519Element YplusX, YminusX, PP, MM, TT2d, ZZ2;

    burrow__fe_add(&YplusX, &p->y, &p->x);
    burrow__fe_subtract(&YminusX, &p->y, &p->x);

    burrow__fe_multiply(&PP, &YplusX, &q->YplusX);
    burrow__fe_multiply(&MM, &YminusX, &q->YminusX);
    burrow__fe_multiply(&TT2d, &p->t, &q->T2d);
    burrow__fe_multiply(&ZZ2, &p->z, &q->Z);

    burrow__fe_add(&ZZ2, &ZZ2, &ZZ2);

    burrow__fe_subtract(&v->X, &PP, &MM);
    burrow__fe_add(&v->Y, &PP, &MM);
    burrow__fe_add(&v->Z, &ZZ2, &TT2d);
    burrow__fe_subtract(&v->T, &ZZ2, &TT2d);
    return v;
}

Edwards25519ProjP1xP1 *burrow__ge_p1xp1_sub(Edwards25519ProjP1xP1 *v,
                                            const Edwards25519Point *p,
                                            const Edwards25519ProjCached *q) {
    Edwards25519Element YplusX, YminusX, PP, MM, TT2d, ZZ2;

    burrow__fe_add(&YplusX, &p->y, &p->x);
    burrow__fe_subtract(&YminusX, &p->y, &p->x);

    burrow__fe_multiply(&PP, &YplusX, &q->YminusX); /* flipped sign */
    burrow__fe_multiply(&MM, &YminusX, &q->YplusX); /* flipped sign */
    burrow__fe_multiply(&TT2d, &p->t, &q->T2d);
    burrow__fe_multiply(&ZZ2, &p->z, &q->Z);

    burrow__fe_add(&ZZ2, &ZZ2, &ZZ2);

    burrow__fe_subtract(&v->X, &PP, &MM);
    burrow__fe_add(&v->Y, &PP, &MM);
    burrow__fe_subtract(&v->Z, &ZZ2, &TT2d); /* flipped sign */
    burrow__fe_add(&v->T, &ZZ2, &TT2d);      /* flipped sign */
    return v;
}

Edwards25519ProjP1xP1 *burrow__ge_p1xp1_add_affine(Edwards25519ProjP1xP1 *v,
                                                   const Edwards25519Point *p,
                                                   const Edwards25519AffineCached *q) {
    Edwards25519Element YplusX, YminusX, PP, MM, TT2d, Z2;

    burrow__fe_add(&YplusX, &p->y, &p->x);
    burrow__fe_subtract(&YminusX, &p->y, &p->x);

    burrow__fe_multiply(&PP, &YplusX, &q->YplusX);
    burrow__fe_multiply(&MM, &YminusX, &q->YminusX);
    burrow__fe_multiply(&TT2d, &p->t, &q->T2d);

    burrow__fe_add(&Z2, &p->z, &p->z);

    burrow__fe_subtract(&v->X, &PP, &MM);
    burrow__fe_add(&v->Y, &PP, &MM);
    burrow__fe_add(&v->Z, &Z2, &TT2d);
    burrow__fe_subtract(&v->T, &Z2, &TT2d);
    return v;
}

Edwards25519ProjP1xP1 *burrow__ge_p1xp1_sub_affine(Edwards25519ProjP1xP1 *v,
                                                   const Edwards25519Point *p,
                                                   const Edwards25519AffineCached *q) {
    Edwards25519Element YplusX, YminusX, PP, MM, TT2d, Z2;

    burrow__fe_add(&YplusX, &p->y, &p->x);
    burrow__fe_subtract(&YminusX, &p->y, &p->x);

    burrow__fe_multiply(&PP, &YplusX, &q->YminusX); /* flipped sign */
    burrow__fe_multiply(&MM, &YminusX, &q->YplusX); /* flipped sign */
    burrow__fe_multiply(&TT2d, &p->t, &q->T2d);

    burrow__fe_add(&Z2, &p->z, &p->z);

    burrow__fe_subtract(&v->X, &PP, &MM);
    burrow__fe_add(&v->Y, &PP, &MM);
    burrow__fe_subtract(&v->Z, &Z2, &TT2d); /* flipped sign */
    burrow__fe_add(&v->T, &Z2, &TT2d);      /* flipped sign */
    return v;
}

Edwards25519Point *burrow__ge_add(Edwards25519Point *v, const Edwards25519Point *p,
                                  const Edwards25519Point *q) {
    ge_check_initialized(p);
    ge_check_initialized(q);
    Edwards25519ProjCached q_cached;
    Edwards25519ProjP1xP1 result;
    burrow__ge_cached_from_p3(&q_cached, q);
    burrow__ge_p1xp1_add(&result, p, &q_cached);
    return burrow__ge_from_p1xp1(v, &result);
}

Edwards25519Point *burrow__ge_subtract(Edwards25519Point *v, const Edwards25519Point *p,
                                       const Edwards25519Point *q) {
    ge_check_initialized(p);
    ge_check_initialized(q);
    Edwards25519ProjCached q_cached;
    Edwards25519ProjP1xP1 result;
    burrow__ge_cached_from_p3(&q_cached, q);
    burrow__ge_p1xp1_sub(&result, p, &q_cached);
    return burrow__ge_from_p1xp1(v, &result);
}

/* ---------------------------------------------------------------- doubling */

Edwards25519ProjP1xP1 *burrow__ge_p1xp1_double(Edwards25519ProjP1xP1 *v,
                                               const Edwards25519ProjP2 *p) {
    Edwards25519Element XX, YY, ZZ2, XplusYsq;

    burrow__fe_square(&XX, &p->X);
    burrow__fe_square(&YY, &p->Y);
    burrow__fe_square(&ZZ2, &p->Z);
    burrow__fe_add(&ZZ2, &ZZ2, &ZZ2);
    burrow__fe_add(&XplusYsq, &p->X, &p->Y);
    burrow__fe_square(&XplusYsq, &XplusYsq);

    burrow__fe_add(&v->Y, &YY, &XX);
    burrow__fe_subtract(&v->Z, &YY, &XX);

    burrow__fe_subtract(&v->X, &XplusYsq, &v->Y);
    burrow__fe_subtract(&v->T, &ZZ2, &v->Z);
    return v;
}

/* ---------------------------------------------------------- negation, equal */

Edwards25519Point *burrow__ge_negate(Edwards25519Point *v, const Edwards25519Point *p) {
    ge_check_initialized(p);
    burrow__fe_negate(&v->x, &p->x);
    v->y = p->y;
    v->z = p->z;
    burrow__fe_negate(&v->t, &p->t);
    return v;
}

int burrow__ge_equal(const Edwards25519Point *v, const Edwards25519Point *u) {
    ge_check_initialized(v);
    ge_check_initialized(u);

    Edwards25519Element t1, t2, t3, t4;
    burrow__fe_multiply(&t1, &v->x, &u->z);
    burrow__fe_multiply(&t2, &u->x, &v->z);
    burrow__fe_multiply(&t3, &v->y, &u->z);
    burrow__fe_multiply(&t4, &u->y, &v->z);

    return burrow__fe_equal(&t1, &t2) & burrow__fe_equal(&t3, &t4);
}

/* ------------------------------------------------- constant time operations */

/* a if cond is 1 and b if cond is 0. */
static void ge_cached_select(Edwards25519ProjCached *v, const Edwards25519ProjCached *a,
                             const Edwards25519ProjCached *b, int cond) {
    burrow__fe_select(&v->YplusX, &a->YplusX, &b->YplusX, cond);
    burrow__fe_select(&v->YminusX, &a->YminusX, &b->YminusX, cond);
    burrow__fe_select(&v->Z, &a->Z, &b->Z, cond);
    burrow__fe_select(&v->T2d, &a->T2d, &b->T2d, cond);
}

static void ge_affine_select(Edwards25519AffineCached *v,
                             const Edwards25519AffineCached *a,
                             const Edwards25519AffineCached *b, int cond) {
    burrow__fe_select(&v->YplusX, &a->YplusX, &b->YplusX, cond);
    burrow__fe_select(&v->YminusX, &a->YminusX, &b->YminusX, cond);
    burrow__fe_select(&v->T2d, &a->T2d, &b->T2d, cond);
}

/* -v if cond is 1, and v as it is if cond is 0. */
static void ge_cached_cond_neg(Edwards25519ProjCached *v, int cond) {
    Edwards25519Element neg;
    burrow__fe_swap(&v->YplusX, &v->YminusX, cond);
    burrow__fe_negate(&neg, &v->T2d);
    burrow__fe_select(&v->T2d, &neg, &v->T2d, cond);
}

static void ge_affine_cond_neg(Edwards25519AffineCached *v, int cond) {
    Edwards25519Element neg;
    burrow__fe_swap(&v->YplusX, &v->YminusX, cond);
    burrow__fe_negate(&neg, &v->T2d);
    burrow__fe_select(&v->T2d, &neg, &v->T2d, cond);
}

/* ------------------------------------------------------------------- tables */

void burrow__ge_proj_table_from_p3(Edwards25519ProjLookupTable *v,
                                   const Edwards25519Point *q) {
    /* Goes from q to 8q, each the one before plus q. */
    burrow__ge_cached_from_p3(&v->points[0], q);
    Edwards25519Point tmp_p3;
    Edwards25519ProjP1xP1 tmp_p1xp1;
    for (int i = 0; i < 7; i++) {
        burrow__ge_p1xp1_add(&tmp_p1xp1, q, &v->points[i]);
        burrow__ge_cached_from_p3(&v->points[i + 1],
                                  burrow__ge_from_p1xp1(&tmp_p3, &tmp_p1xp1));
    }
}

void burrow__ge_affine_table_from_p3(Edwards25519AffineLookupTable *v,
                                     const Edwards25519Point *q) {
    burrow__ge_affine_from_p3(&v->points[0], q);
    Edwards25519Point tmp_p3;
    Edwards25519ProjP1xP1 tmp_p1xp1;
    for (int i = 0; i < 7; i++) {
        burrow__ge_p1xp1_add_affine(&tmp_p1xp1, q, &v->points[i]);
        burrow__ge_affine_from_p3(&v->points[i + 1],
                                  burrow__ge_from_p1xp1(&tmp_p3, &tmp_p1xp1));
    }
}

void burrow__ge_naf5_table_from_p3(Edwards25519NafLookupTable5 *v,
                                   const Edwards25519Point *q) {
    /* q, 3q, 5q up to 15q, each the one before plus 2q. */
    burrow__ge_cached_from_p3(&v->points[0], q);
    Edwards25519Point q2;
    burrow__ge_add(&q2, q, q);
    Edwards25519Point tmp_p3;
    Edwards25519ProjP1xP1 tmp_p1xp1;
    for (int i = 0; i < 7; i++) {
        burrow__ge_p1xp1_add(&tmp_p1xp1, &q2, &v->points[i]);
        burrow__ge_cached_from_p3(&v->points[i + 1],
                                  burrow__ge_from_p1xp1(&tmp_p3, &tmp_p1xp1));
    }
}

void burrow__ge_naf8_table_from_p3(Edwards25519NafLookupTable8 *v,
                                   const Edwards25519Point *q) {
    burrow__ge_affine_from_p3(&v->points[0], q);
    Edwards25519Point q2;
    burrow__ge_add(&q2, q, q);
    Edwards25519Point tmp_p3;
    Edwards25519ProjP1xP1 tmp_p1xp1;
    for (int i = 0; i < 63; i++) {
        burrow__ge_p1xp1_add_affine(&tmp_p1xp1, &q2, &v->points[i]);
        burrow__ge_affine_from_p3(&v->points[i + 1],
                                  burrow__ge_from_p1xp1(&tmp_p3, &tmp_p1xp1));
    }
}

void burrow__ge_proj_table_select(const Edwards25519ProjLookupTable *v,
                                  Edwards25519ProjCached *dest, int8_t x) {
    /* |x| in constant time. */
    int8_t xmask = (int8_t)(x >> 7);
    uint8_t xabs = (uint8_t)((x + xmask) ^ xmask);

    burrow__ge_cached_zero(dest);
    for (int j = 1; j <= 8; j++) {
        /* dest = v.points[j-1] if j == |x|. */
        int cond = (int)subtle_constant_time_byte_eq(xabs, (uint8_t)j);
        ge_cached_select(dest, &v->points[j - 1], dest, cond);
    }
    /* -dest if x was negative. */
    ge_cached_cond_neg(dest, xmask & 1);
}

void burrow__ge_affine_table_select(const Edwards25519AffineLookupTable *v,
                                    Edwards25519AffineCached *dest, int8_t x) {
    int8_t xmask = (int8_t)(x >> 7);
    uint8_t xabs = (uint8_t)((x + xmask) ^ xmask);

    burrow__ge_affine_zero(dest);
    for (int j = 1; j <= 8; j++) {
        int cond = (int)subtle_constant_time_byte_eq(xabs, (uint8_t)j);
        ge_affine_select(dest, &v->points[j - 1], dest, cond);
    }
    ge_affine_cond_neg(dest, xmask & 1);
}

void burrow__ge_naf5_table_select(const Edwards25519NafLookupTable5 *v,
                                  Edwards25519ProjCached *dest, int8_t x) {
    *dest = v->points[x / 2];
}

void burrow__ge_naf8_table_select(const Edwards25519NafLookupTable8 *v,
                                  Edwards25519AffineCached *dest, int8_t x) {
    *dest = v->points[x / 2];
}

/* ------------------------------------------------------ scalar multiplication */

/* Four doublings of *tmp1 into *v, with *tmp2 as scratch: v = 16 * tmp1. */
static void ge_times16(Edwards25519Point *v, Edwards25519ProjP1xP1 *tmp1,
                       Edwards25519ProjP2 *tmp2) {
    for (int k = 0; k < 4; k++) {
        burrow__ge_p2_from_p1xp1(tmp2, tmp1);
        burrow__ge_p1xp1_double(tmp1, tmp2);
    }
    burrow__ge_from_p1xp1(v, tmp1);
}

Edwards25519Point *burrow__ge_scalar_base_mult(Edwards25519Point *v,
                                               const Edwards25519Scalar *x) {
    /* x = sum(x_i * 16^i), so x * B = sum(B * x_i * 16^i), as the Ed25519 paper
     * has it. Grouping the even and the odd coefficients,
     *
     *     x * B = x_0 * 16^0 * B + x_2 * 16^2 * B + ... + x_62 * 16^62 * B
     *           + 16 * (x_1 * 16^0 * B + x_3 * 16^2 * B + ... + x_63 * 16^62 * B)
     *
     * and table i gives x_i * 16^(2i) * B, with four doublings for the 16. */
    int8_t digits[64];
    burrow__sc_signed_radix16(x, digits);

    Edwards25519AffineCached multiple;
    Edwards25519ProjP1xP1 tmp1;
    Edwards25519ProjP2 tmp2;

    /* The odd coefficients first. */
    burrow__ge_identity(v);
    for (int i = 1; i < 64; i += 2) {
        burrow__ge_affine_table_select(&burrow__ge_basepoint_table[i / 2], &multiple,
                                       digits[i]);
        burrow__ge_p1xp1_add_affine(&tmp1, v, &multiple);
        burrow__ge_from_p1xp1(v, &tmp1);
    }

    /* Times 16. v in P2 coordinates, doubled into tmp1, and three more times. */
    burrow__ge_p2_from_p3(&tmp2, v);
    burrow__ge_p1xp1_double(&tmp1, &tmp2);
    for (int k = 0; k < 3; k++) {
        burrow__ge_p2_from_p1xp1(&tmp2, &tmp1);
        burrow__ge_p1xp1_double(&tmp1, &tmp2);
    }
    burrow__ge_from_p1xp1(v, &tmp1);

    /* Then the even ones. */
    for (int i = 0; i < 64; i += 2) {
        burrow__ge_affine_table_select(&burrow__ge_basepoint_table[i / 2], &multiple,
                                       digits[i]);
        burrow__ge_p1xp1_add_affine(&tmp1, v, &multiple);
        burrow__ge_from_p1xp1(v, &tmp1);
    }

    return v;
}

Edwards25519Point *burrow__ge_scalar_mult(Edwards25519Point *v,
                                          const Edwards25519Scalar *x,
                                          const Edwards25519Point *q) {
    ge_check_initialized(q);

    Edwards25519ProjLookupTable table;
    burrow__ge_proj_table_from_p3(&table, q);

    /* x = sum(x_i * 16^i), so
     *
     *     x * Q = sum(Q * x_i * 16^i)
     *           = Q * x_0 + 16 * (Q * x_1 + 16 * ( ... + Q * x_63) ... )
     *
     * worked out from the inside, with the table for the x_i * Q and four
     * doublings for each 16. */
    int8_t digits[64];
    burrow__sc_signed_radix16(x, digits);

    /* The first round is outside the loop, to save working out 16 * identity. */
    Edwards25519ProjCached multiple;
    Edwards25519ProjP1xP1 tmp1;
    Edwards25519ProjP2 tmp2;
    burrow__ge_proj_table_select(&table, &multiple, digits[63]);

    burrow__ge_identity(v);
    burrow__ge_p1xp1_add(&tmp1, v, &multiple); /* tmp1 = x_63 * Q */
    for (int i = 62; i >= 0; i--) {
        ge_times16(v, &tmp1, &tmp2); /* v = 16 * (what came before) */
        burrow__ge_proj_table_select(&table, &multiple, digits[i]);
        burrow__ge_p1xp1_add(&tmp1, v, &multiple); /* tmp1 = x_i * Q + v */
    }
    burrow__ge_from_p1xp1(v, &tmp1);
    return v;
}

Edwards25519Point *burrow__ge_var_time_double_scalar_base_mult(
    Edwards25519Point *v, const Edwards25519Scalar *a, const Edwards25519Point *A,
    const Edwards25519Scalar *b) {
    ge_check_initialized(A);

    /* As with one variable base, the scalars become digits that index a table.
     * Variable time is allowed here, so the lookups and the digits need not be
     * constant time, and a non-adjacent form of width w takes the place of
     * radix 16. That is one digit for each binary place, but a digit can be as
     * big as 2^(w-1) in magnitude, so the nonzero digits are as few as they
     * can be, and so are the additions. */
    Edwards25519NafLookupTable5 a_table;
    burrow__ge_naf5_table_from_p3(&a_table, A);
    /* The basepoint does not change, so its NAF is wider and its table bigger. */
    int8_t a_naf[256], b_naf[256];
    burrow__sc_non_adjacent_form(a, 5, a_naf);
    burrow__sc_non_adjacent_form(b, 8, b_naf);

    /* The first nonzero coefficient. */
    int i = 255;
    while (i >= 0) {
        if (a_naf[i] != 0 || b_naf[i] != 0)
            break;
        i--;
    }

    Edwards25519ProjCached mult_a;
    Edwards25519AffineCached mult_b;
    Edwards25519ProjP1xP1 tmp1;
    Edwards25519ProjP2 tmp2;
    burrow__ge_p2_zero(&tmp2);

    /* From the high bits to the low, doubling each time and adding in a
     * multiple from a table wherever a coefficient is nonzero. */
    for (; i >= 0; i--) {
        burrow__ge_p1xp1_double(&tmp1, &tmp2);

        if (a_naf[i] > 0) {
            burrow__ge_from_p1xp1(v, &tmp1);
            burrow__ge_naf5_table_select(&a_table, &mult_a, a_naf[i]);
            burrow__ge_p1xp1_add(&tmp1, v, &mult_a);
        } else if (a_naf[i] < 0) {
            burrow__ge_from_p1xp1(v, &tmp1);
            burrow__ge_naf5_table_select(&a_table, &mult_a, (int8_t)-a_naf[i]);
            burrow__ge_p1xp1_sub(&tmp1, v, &mult_a);
        }

        if (b_naf[i] > 0) {
            burrow__ge_from_p1xp1(v, &tmp1);
            burrow__ge_naf8_table_select(&burrow__ge_basepoint_naf_table, &mult_b,
                                         b_naf[i]);
            burrow__ge_p1xp1_add_affine(&tmp1, v, &mult_b);
        } else if (b_naf[i] < 0) {
            burrow__ge_from_p1xp1(v, &tmp1);
            burrow__ge_naf8_table_select(&burrow__ge_basepoint_naf_table, &mult_b,
                                         (int8_t)-b_naf[i]);
            burrow__ge_p1xp1_sub_affine(&tmp1, v, &mult_b);
        }

        burrow__ge_p2_from_p1xp1(&tmp2, &tmp1);
    }

    return burrow__ge_from_p2(v, &tmp2);
}
