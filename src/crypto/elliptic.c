/* crypto/elliptic: the four NIST curves through crypto/internal/fips140/nistec,
 * and the generic curve arithmetic of params.go on math/big.
 *
 * Copyright 2010 The Go Authors. All rights reserved.
 * Copyright 2026 The burrow Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style licence that can be found
 * in the LICENSE file. */

#include "burrow/crypto/elliptic.h"

#include "burrow/core.h"
#include "burrow/crypto/rand.h"
#include "burrow/error.h"
#include "burrow/io.h"
#include "burrow/math/big.h"
#include "burrow/mem.h"
#include "burrow/mem/heap.h"
#include "burrow/panic.h"
#include "burrow/slice.h"
#include "burrow/sync.h"
#include "burrow/type.h"

#include "nistec.h"

#include <stdint.h>
#include <string.h>

const Type burrow_type_EllipticCurveParams = {
    BURROW_S_INIT("CurveParams"),
    BURROW_S_INIT("crypto/elliptic"),
    KIND_STRUCT,
    (uint32_t)sizeof(EllipticCurveParams),
    (uint16_t)_Alignof(EllipticCurveParams),
    0,
    0,
    NULL,
    NULL,
    NULL,
    NULL,
    0,
    0,
    NULL,
};

/* ---------------------------------------------------------------- helpers */

static Alloc *ell_alloc(Alloc *a) {
    return a != NULL ? a : heap_allocator();
}

/* new(big.Int).Set(x), from a. */
static BigInt *ell_new(Alloc *a, const BigInt *x) {
    BigInt *z = big_new_int(a, 0);
    return x != NULL ? big_int_set(z, x) : z;
}

/* Hands back x and y as the two results: x returned, y through out. */
static BigInt *ell_result(Alloc *a, const BigInt *x, const BigInt *y, BigInt **out) {
    BigInt *rx = ell_new(a, x);
    if (out != NULL)
        *out = ell_new(a, y);
    return rx;
}

static BigInt *ell_nil(BigInt **y) {
    if (y != NULL)
        *y = NULL;
    return NULL;
}

/* Hands v out through out, or frees it when the caller passed no out. */
static void ell_out(Alloc *a, BigInt **out, BigInt *v) {
    if (out != NULL) {
        *out = v;
    } else if (v != NULL) {
        big_int_free(v);
        mem_free(ell_alloc(a), v, sizeof(BigInt), _Alignof(BigInt));
    }
}

static Slice ell_bytes(Alloc *a, Int n) {
    a = ell_alloc(a);
    uint8_t *p = mem_alloc(a, (size_t)n, 1);
    if (p == NULL)
        panic_str(BURROW_S("crypto/elliptic: out of memory"));
    memset(p, 0, (size_t)n);
    return slice_from(p, n, n, TYPE_BYTE);
}

static Slice ell_sub(uint8_t *p, Int from, Int to) {
    return slice_from(p + from, to - from, to - from, TYPE_BYTE);
}

static Int ell_byte_len(const EllipticCurveParams *params) {
    return (params->bit_size + 7) / 8;
}

/* -------------------------------------------------------------- nistCurve */

typedef struct EllipticNist {
    const NistecCurve *ec;
    EllipticCurveParams params;
    BigInt p, n, b, gx, gy;
} EllipticNist;

static EllipticNist elliptic_nist[4] = {
    {&burrow__nistec_p224, {0}, {0}, {0}, {0}, {0}, {0}},
    {&burrow__nistec_p256, {0}, {0}, {0}, {0}, {0}, {0}},
    {&burrow__nistec_p384, {0}, {0}, {0}, {0}, {0}, {0}},
    {&burrow__nistec_p521, {0}, {0}, {0}, {0}, {0}, {0}},
};
static SyncOnce elliptic_once;

/* P and N in decimal and the rest in hex, as nistec.go has them, with P from
 * SP 800-186 and the rest from FIPS 186-4. */
static const struct {
    const char *name;
    Int bit_size;
    const char *p, *n, *b, *gx, *gy;
} elliptic_consts[4] = {
    {
        "P-224",
        224,
        "26959946667150639794667015087019630673557916260026308143510066298881",
        "26959946667150639794667015087019625940457807714424391721682722368061",
        "b4050a850c04b3abf54132565044b0b7d7bfd8ba270b39432355ffb4",
        "b70e0cbd6bb4bf7f321390b94a03c1d356c21122343280d6115c1d21",
        "bd376388b5f723fb4c22dfe6cd4375a05a07476444d5819985007e34",
    },
    {
        "P-256",
        256,
        "11579208921035624876269744694940757353008614341529031419553363130886"
        "7097853951",
        "11579208921035624876269744694940757352999695522413576034242225906106"
        "8512044369",
        "5ac635d8aa3a93e7b3ebbd55769886bc651d06b0cc53b0f63bce3c3e27d2604b",
        "6b17d1f2e12c4247f8bce6e563a440f277037d812deb33a0f4a13945d898c296",
        "4fe342e2fe1a7f9b8ee7eb4a7c0f9e162bce33576b315ececbb6406837bf51f5",
    },
    {
        "P-384",
        384,
        "39402006196394479212279040100143613805079739270465446667948293404245"
        "721771496870329047266088258938001861606973112319",
        "39402006196394479212279040100143613805079739270465446667946905279627"
        "659399113263569398956308152294913554433653942643",
        "b3312fa7e23ee7e4988e056be3f82d19181d9c6efe8141120314088f5013875ac656"
        "398d8a2ed19d2a85c8edd3ec2aef",
        "aa87ca22be8b05378eb1c71ef320ad746e1d3b628ba79b9859f741e082542a385502"
        "f25dbf55296c3a545e3872760ab7",
        "3617de4a96262c6f5d9e98bf9292dc29f8f41dbd289a147ce9da3113b5f0b8c00a60"
        "b1ce1d7e819d7a431d7c90ea0e5f",
    },
    {
        "P-521",
        521,
        "68647976601306097149819007990813932172694353001433054093944634591855"
        "43183397656052122559640661454554977296311391480858037121987999716643"
        "812574028291115057151",
        "68647976601306097149819007990813932172694353001433054093944634591855"
        "43183397655394245057746333217197532963996371363321113864768612440380"
        "340372808892707005449",
        "0051953eb9618e1c9a1f929a21a0b68540eea2da725b99b315f3b8b489918ef109e1"
        "56193951ec7e937b1652c0bd3bb1bf073573df883d2c34f1ef451fd46b503f00",
        "00c6858e06b70404e9cd9e3ecb662395b4429c648139053fb521f828af606b4d3dba"
        "a14b5e77efe75928fe1dc127a2ffa8de3348b3c1856a429bf97e7e31c2e5bd66",
        "011839296a789a3bc0045c8a5fb42c7d1bd998f54449579b446817afbd17273e662c"
        "97ee72995ef42640c550b9013fad0761353c7086a272c24088be94769fd16650",
    },
};

static void ell_set(BigInt *z, const char *s, Int base) {
    bool ok = false;
    big_int_set_string(z, str_from_cstr(s), base, &ok);
    if (!ok)
        panic_str(BURROW_S("crypto/elliptic: internal error: invalid encoding"));
}

static void elliptic_init(void *env) {
    (void)env;
    for (int i = 0; i < 4; i++) {
        EllipticNist *c = &elliptic_nist[i];
        ell_set(&c->p, elliptic_consts[i].p, 10);
        ell_set(&c->n, elliptic_consts[i].n, 10);
        ell_set(&c->b, elliptic_consts[i].b, 16);
        ell_set(&c->gx, elliptic_consts[i].gx, 16);
        ell_set(&c->gy, elliptic_consts[i].gy, 16);
        c->params.p = &c->p;
        c->params.n = &c->n;
        c->params.b = &c->b;
        c->params.gx = &c->gx;
        c->params.gy = &c->gy;
        c->params.bit_size = elliptic_consts[i].bit_size;
        c->params.name = str_from_cstr(elliptic_consts[i].name);
    }
}

static EllipticCurveParams *nist_params(void *self) {
    return &((EllipticNist *)self)->params;
}

/* pointFromAffine: false where Go has an error. The error nistec makes goes
 * back to the error arena, since nobody sees it. */
static bool nist_from_affine(const EllipticNist *c, NistecPoint *p, const BigInt *x,
                             const BigInt *y) {
    /* (0, 0) is by convention the point at infinity, which can't be
     * represented in affine coordinates. See Issue 37294. */
    if (big_int_sign(x) == 0 && big_int_sign(y) == 0) {
        c->ec->point_new(p);
        return true;
    }
    /* Reject values that would not get correctly encoded. */
    if (big_int_sign(x) < 0 || big_int_sign(y) < 0)
        return false;
    if (big_int_bit_len(x) > c->params.bit_size ||
        big_int_bit_len(y) > c->params.bit_size)
        return false;
    /* Encode the coordinates and let SetBytes reject invalid points. */
    Int n = ell_byte_len(&c->params);
    uint8_t buf[NISTEC_MAX_POINT_BYTES];
    buf[0] = 4; /* uncompressed point */
    big_int_fill_bytes(x, ell_sub(buf, 1, 1 + n));
    big_int_fill_bytes(y, ell_sub(buf, 1 + n, 1 + 2 * n));
    ArenaMark m = error_mark();
    Error e = BURROW_NO_ERROR;
    bool ok =
        c->ec->set_bytes(c->ec->point_new(p), ell_sub(buf, 0, 1 + 2 * n), &e) != NULL;
    error_release(m);
    return ok;
}

/* pointToAffine. */
static BigInt *nist_to_affine(const EllipticNist *c, Alloc *a, const NistecPoint *p,
                              BigInt **y) {
    uint8_t out[NISTEC_MAX_POINT_BYTES];
    Int len = c->ec->bytes(p, out);
    if (len == 1 && out[0] == 0) {
        /* This is the encoding of the point at infinity, which the Curve API
         * represents as (0, 0) by convention. */
        return ell_result(a, NULL, NULL, y);
    }
    Int n = ell_byte_len(&c->params);
    BigInt *x = big_int_set_bytes(big_new_int(a, 0), ell_sub(out, 1, 1 + n));
    if (y != NULL)
        *y = big_int_set_bytes(big_new_int(a, 0), ell_sub(out, 1 + n, len));
    return x;
}

static bool nist_is_on_curve(void *self, const BigInt *x, const BigInt *y) {
    const EllipticNist *c = self;
    /* IsOnCurve is documented to reject (0, 0), the conventional point at
     * infinity, which however is accepted by pointFromAffine. */
    if (big_int_sign(x) == 0 && big_int_sign(y) == 0)
        return false;
    NistecPoint p;
    return nist_from_affine(c, &p, x, y);
}

static BigInt *nist_add(void *self, Alloc *a, const BigInt *x1, const BigInt *y1,
                        const BigInt *x2, const BigInt *y2, BigInt **y) {
    const EllipticNist *c = self;
    NistecPoint p1, p2;
    if (!nist_from_affine(c, &p1, x1, y1))
        panic_str(BURROW_S("crypto/elliptic: Add was called on an invalid point"));
    if (!nist_from_affine(c, &p2, x2, y2))
        panic_str(BURROW_S("crypto/elliptic: Add was called on an invalid point"));
    return nist_to_affine(c, a, c->ec->add(&p1, &p1, &p2), y);
}

static BigInt *nist_double(void *self, Alloc *a, const BigInt *x1, const BigInt *y1,
                           BigInt **y) {
    const EllipticNist *c = self;
    NistecPoint p;
    if (!nist_from_affine(c, &p, x1, y1))
        panic_str(BURROW_S("crypto/elliptic: Double was called on an invalid point"));
    return nist_to_affine(c, a, c->ec->double_(&p, &p), y);
}

/* normalizeScalar brings the scalar within the byte size of the order of the
 * curve, as expected by the nistec scalar multiplication functions, into out,
 * which has room for NISTEC_MAX_ELEMENT_BYTES. */
static Slice nist_normalize_scalar(const EllipticNist *c, Slice scalar, uint8_t *out) {
    Int size = (big_int_bit_len(c->params.n) + 7) / 8;
    if (scalar.len == size)
        return scalar;
    BigInt s = BIG_INT(NULL);
    big_int_set_bytes(&s, scalar);
    if (scalar.len > size)
        big_int_mod(&s, &s, c->params.n);
    Slice r = big_int_fill_bytes(&s, ell_sub(out, 0, size));
    big_int_free(&s);
    return r;
}

static BigInt *nist_scalar_mult(void *self, Alloc *a, const BigInt *bx,
                                const BigInt *by, Slice scalar, BigInt **y) {
    const EllipticNist *c = self;
    NistecPoint p;
    if (!nist_from_affine(c, &p, bx, by))
        panic_str(
            BURROW_S("crypto/elliptic: ScalarMult was called on an invalid point"));
    uint8_t buf[NISTEC_MAX_ELEMENT_BYTES];
    scalar = nist_normalize_scalar(c, scalar, buf);
    Error e = BURROW_NO_ERROR;
    if (c->ec->scalar_mult(&p, &p, scalar, &e) == NULL)
        panic_str(BURROW_S("crypto/elliptic: nistec rejected normalized scalar"));
    return nist_to_affine(c, a, &p, y);
}

static BigInt *nist_scalar_base_mult(void *self, Alloc *a, Slice scalar, BigInt **y) {
    const EllipticNist *c = self;
    NistecPoint p;
    uint8_t buf[NISTEC_MAX_ELEMENT_BYTES];
    scalar = nist_normalize_scalar(c, scalar, buf);
    Error e = BURROW_NO_ERROR;
    if (c->ec->scalar_base_mult(c->ec->point_new(&p), scalar, &e) == NULL)
        panic_str(BURROW_S("crypto/elliptic: nistec rejected normalized scalar"));
    return nist_to_affine(c, a, &p, y);
}

/* nistCurve.Unmarshal: Unmarshal for these curves, which lets nistec check the
 * point. */
static BigInt *nist_unmarshal(const EllipticNist *c, Alloc *a, Slice data, BigInt **y) {
    const uint8_t *d = data.p;
    if (data.len == 0 || d[0] != 4)
        return ell_nil(y);
    /* Use SetBytes to check that data encodes a valid point. */
    NistecPoint p;
    ArenaMark m = error_mark();
    Error e = BURROW_NO_ERROR;
    bool ok = c->ec->set_bytes(c->ec->point_new(&p), data, &e) != NULL;
    error_release(m);
    if (!ok)
        return ell_nil(y);
    /* We don't use pointToAffine because it involves an expensive field
     * inversion to convert from Jacobian to affine coordinates, which we
     * already have. */
    Int n = ell_byte_len(&c->params);
    uint8_t *q = data.p;
    BigInt *x = big_int_set_bytes(big_new_int(a, 0), ell_sub(q, 1, 1 + n));
    if (y != NULL)
        *y = big_int_set_bytes(big_new_int(a, 0), ell_sub(q, 1 + n, data.len));
    return x;
}

/* nistCurve.UnmarshalCompressed. */
static BigInt *nist_unmarshal_compressed(const EllipticNist *c, Alloc *a, Slice data,
                                         BigInt **y) {
    const uint8_t *d = data.p;
    if (data.len == 0 || (d[0] != 2 && d[0] != 3))
        return ell_nil(y);
    NistecPoint p;
    ArenaMark m = error_mark();
    Error e = BURROW_NO_ERROR;
    bool ok = c->ec->set_bytes(c->ec->point_new(&p), data, &e) != NULL;
    error_release(m);
    if (!ok)
        return ell_nil(y);
    return nist_to_affine(c, a, &p, y);
}

/* The type is unexported in Go, so a type assertion can't name it. */
static const EllipticCurveVT elliptic_nist_vt = {
    NULL,        nist_params,      nist_is_on_curve,      nist_add,
    nist_double, nist_scalar_mult, nist_scalar_base_mult,
};

static EllipticCurve elliptic_nist_curve(int i) {
    sync_once_do(&elliptic_once, BURROW_FN(Func, elliptic_init, NULL));
    EllipticCurve c = {&elliptic_nist_vt, &elliptic_nist[i]};
    return c;
}

EllipticCurve elliptic_p224(void) {
    return elliptic_nist_curve(0);
}

EllipticCurve elliptic_p256(void) {
    return elliptic_nist_curve(1);
}

EllipticCurve elliptic_p384(void) {
    return elliptic_nist_curve(2);
}

EllipticCurve elliptic_p521(void) {
    return elliptic_nist_curve(3);
}

/* matchesSpecificCurve: the NIST curve whose params these are, or NULL. Only
 * the same pointer counts, so a copy of the params is generic. */
static EllipticNist *ell_specific(const EllipticCurveParams *params) {
    for (int i = 0; i < 4; i++)
        if (params == &elliptic_nist[i].params)
            return &elliptic_nist[i];
    return NULL;
}

/* ------------------------------------------------------------ CurveParams */

EllipticCurveParams *elliptic_curve_params_params(EllipticCurveParams *curve) {
    return curve;
}

/* polynomial: x^3 - 3x + b mod p, into x3, which must not be x. */
static void ell_polynomial(const EllipticCurveParams *curve, BigInt *x3,
                           const BigInt *x) {
    BigInt three_x = BIG_INT(NULL);
    big_int_mul(x3, x, x);
    big_int_mul(x3, x3, x);
    big_int_lsh(&three_x, x, 1);
    big_int_add(&three_x, &three_x, x);
    big_int_sub(x3, x3, &three_x);
    big_int_add(x3, x3, curve->b);
    big_int_mod(x3, x3, curve->p);
    big_int_free(&three_x);
}

static bool ell_generic_is_on_curve(const EllipticCurveParams *curve, const BigInt *x,
                                    const BigInt *y) {
    if (big_int_sign(x) < 0 || big_int_cmp(x, curve->p) >= 0 || big_int_sign(y) < 0 ||
        big_int_cmp(y, curve->p) >= 0)
        return false;
    /* y² = x³ - 3x + b */
    BigInt y2 = BIG_INT(NULL), poly = BIG_INT(NULL);
    big_int_mul(&y2, y, y);
    big_int_mod(&y2, &y2, curve->p);
    ell_polynomial(curve, &poly, x);
    bool on = big_int_cmp(&poly, &y2) == 0;
    big_int_free(&y2);
    big_int_free(&poly);
    return on;
}

bool elliptic_curve_params_is_on_curve(EllipticCurveParams *curve, const BigInt *x,
                                       const BigInt *y) {
    /* If there is a dedicated constant-time implementation for this curve
     * operation, use that instead of the generic one. */
    EllipticNist *specific = ell_specific(curve);
    if (specific != NULL)
        return nist_is_on_curve(specific, x, y);
    return ell_generic_is_on_curve(curve, x, y);
}

/* panicIfNotOnCurve, through the interface as Go has it. */
static void ell_panic_if_not_on_curve(EllipticCurve curve, const BigInt *x,
                                      const BigInt *y) {
    /* (0, 0) is the point at infinity by convention. It's ok to operate on it,
     * although IsOnCurve is documented to return false for it. See Issue
     * #37294. */
    if (big_int_sign(x) == 0 && big_int_sign(y) == 0)
        return;
    if (!elliptic_curve_is_on_curve(curve, x, y))
        panic_str(BURROW_S("crypto/elliptic: attempted operation on invalid point"));
}

/* A point in Jacobian coordinates, for the generic arithmetic. */
typedef struct EllJac {
    BigInt x, y, z;
} EllJac;

static void ell_jac_init(EllJac *j) {
    j->x = BIG_INT(NULL);
    j->y = BIG_INT(NULL);
    j->z = BIG_INT(NULL);
}

static void ell_jac_free(EllJac *j) {
    big_int_free(&j->x);
    big_int_free(&j->y);
    big_int_free(&j->z);
}

/* zForAffine: the Jacobian Z value for the affine point (x, y). If x and y are
 * zero, it assumes that they represent the point at infinity because (0, 0) is
 * not on any of the curves handled here. */
static void ell_z_for_affine(BigInt *z, const BigInt *x, const BigInt *y) {
    big_int_set_int64(z, 0);
    if (big_int_sign(x) != 0 || big_int_sign(y) != 0)
        big_int_set_int64(z, 1);
}

/* affineFromJacobian reverses the Jacobian transform, with the results from
 * a. */
static BigInt *ell_affine_from_jacobian(const EllipticCurveParams *curve, Alloc *a,
                                        const EllJac *j, BigInt **y) {
    if (big_int_sign(&j->z) == 0)
        return ell_result(a, NULL, NULL, y);
    BigInt zinv = BIG_INT(NULL), zinvsq = BIG_INT(NULL);
    big_int_mod_inverse(&zinv, &j->z, curve->p);
    big_int_mul(&zinvsq, &zinv, &zinv);
    BigInt *x = big_new_int(a, 0);
    big_int_mul(x, &j->x, &zinvsq);
    big_int_mod(x, x, curve->p);
    big_int_mul(&zinvsq, &zinvsq, &zinv);
    if (y != NULL) {
        *y = big_new_int(a, 0);
        big_int_mul(*y, &j->y, &zinvsq);
        big_int_mod(*y, *y, curve->p);
    }
    big_int_free(&zinv);
    big_int_free(&zinvsq);
    return x;
}

static void ell_double_jacobian(const EllipticCurveParams *curve, EllJac *out,
                                const BigInt *x, const BigInt *y, const BigInt *z);

/* addJacobian: (x1, y1, z1) + (x2, y2, z2) into out, which may be either of
 * them, by add-2007-bl from
 * https://hyperelliptic.org/EFD/g1p/auto-shortw-jacobian-3.html. */
static void ell_add_jacobian(const EllipticCurveParams *curve, EllJac *out,
                             const BigInt *x1, const BigInt *y1, const BigInt *z1,
                             const BigInt *x2, const BigInt *y2, const BigInt *z2) {
    const BigInt *p = curve->p;
    if (big_int_sign(z1) == 0) {
        big_int_set(&out->x, x2);
        big_int_set(&out->y, y2);
        big_int_set(&out->z, z2);
        return;
    }
    if (big_int_sign(z2) == 0) {
        big_int_set(&out->x, x1);
        big_int_set(&out->y, y1);
        big_int_set(&out->z, z1);
        return;
    }
    BigInt x3 = BIG_INT(NULL), y3 = BIG_INT(NULL), z3 = BIG_INT(NULL);
    BigInt z1z1 = BIG_INT(NULL), z2z2 = BIG_INT(NULL), u1 = BIG_INT(NULL),
           u2 = BIG_INT(NULL), h = BIG_INT(NULL), i = BIG_INT(NULL), j = BIG_INT(NULL),
           s1 = BIG_INT(NULL), s2 = BIG_INT(NULL), r = BIG_INT(NULL), v = BIG_INT(NULL);

    big_int_mul(&z1z1, z1, z1);
    big_int_mod(&z1z1, &z1z1, p);
    big_int_mul(&z2z2, z2, z2);
    big_int_mod(&z2z2, &z2z2, p);

    big_int_mul(&u1, x1, &z2z2);
    big_int_mod(&u1, &u1, p);
    big_int_mul(&u2, x2, &z1z1);
    big_int_mod(&u2, &u2, p);
    big_int_sub(&h, &u2, &u1);
    bool x_equal = big_int_sign(&h) == 0;
    if (big_int_sign(&h) == -1)
        big_int_add(&h, &h, p);
    big_int_lsh(&i, &h, 1);
    big_int_mul(&i, &i, &i);
    big_int_mul(&j, &h, &i);

    big_int_mul(&s1, y1, z2);
    big_int_mul(&s1, &s1, &z2z2);
    big_int_mod(&s1, &s1, p);
    big_int_mul(&s2, y2, z1);
    big_int_mul(&s2, &s2, &z1z1);
    big_int_mod(&s2, &s2, p);
    big_int_sub(&r, &s2, &s1);
    if (big_int_sign(&r) == -1)
        big_int_add(&r, &r, p);
    bool y_equal = big_int_sign(&r) == 0;
    if (x_equal && y_equal) {
        ell_double_jacobian(curve, out, x1, y1, z1);
    } else {
        big_int_lsh(&r, &r, 1);
        big_int_mul(&v, &u1, &i);

        big_int_set(&x3, &r);
        big_int_mul(&x3, &x3, &x3);
        big_int_sub(&x3, &x3, &j);
        big_int_sub(&x3, &x3, &v);
        big_int_sub(&x3, &x3, &v);
        big_int_mod(&x3, &x3, p);

        big_int_set(&y3, &r);
        big_int_sub(&v, &v, &x3);
        big_int_mul(&y3, &y3, &v);
        big_int_mul(&s1, &s1, &j);
        big_int_lsh(&s1, &s1, 1);
        big_int_sub(&y3, &y3, &s1);
        big_int_mod(&y3, &y3, p);

        big_int_add(&z3, z1, z2);
        big_int_mul(&z3, &z3, &z3);
        big_int_sub(&z3, &z3, &z1z1);
        big_int_sub(&z3, &z3, &z2z2);
        big_int_mul(&z3, &z3, &h);
        big_int_mod(&z3, &z3, p);

        big_int_set(&out->x, &x3);
        big_int_set(&out->y, &y3);
        big_int_set(&out->z, &z3);
    }
    BigInt *tmp[] = {&x3, &y3, &z3, &z1z1, &z2z2, &u1, &u2,
                     &h,  &i,  &j,  &s1,   &s2,   &r,  &v};
    for (size_t k = 0; k < sizeof tmp / sizeof tmp[0]; k++)
        big_int_free(tmp[k]);
}

/* doubleJacobian: 2 * (x, y, z) into out, which may be the same point, by
 * dbl-2001-b from
 * https://hyperelliptic.org/EFD/g1p/auto-shortw-jacobian-3.html. */
static void ell_double_jacobian(const EllipticCurveParams *curve, EllJac *out,
                                const BigInt *x, const BigInt *y, const BigInt *z) {
    const BigInt *p = curve->p;
    BigInt delta = BIG_INT(NULL), gamma = BIG_INT(NULL), alpha = BIG_INT(NULL),
           alpha2 = BIG_INT(NULL), x3 = BIG_INT(NULL), beta8 = BIG_INT(NULL),
           z3 = BIG_INT(NULL);

    big_int_mul(&delta, z, z);
    big_int_mod(&delta, &delta, p);
    big_int_mul(&gamma, y, y);
    big_int_mod(&gamma, &gamma, p);
    big_int_sub(&alpha, x, &delta);
    if (big_int_sign(&alpha) == -1)
        big_int_add(&alpha, &alpha, p);
    big_int_add(&alpha2, x, &delta);
    big_int_mul(&alpha, &alpha, &alpha2);
    big_int_set(&alpha2, &alpha);
    big_int_lsh(&alpha, &alpha, 1);
    big_int_add(&alpha, &alpha, &alpha2);

    /* Go's beta is alpha2 under another name. */
    BigInt *beta = big_int_mul(&alpha2, x, &gamma);

    big_int_mul(&x3, &alpha, &alpha);
    big_int_lsh(&beta8, beta, 3);
    big_int_mod(&beta8, &beta8, p);
    big_int_sub(&x3, &x3, &beta8);
    if (big_int_sign(&x3) == -1)
        big_int_add(&x3, &x3, p);
    big_int_mod(&x3, &x3, p);

    big_int_add(&z3, y, z);
    big_int_mul(&z3, &z3, &z3);
    big_int_sub(&z3, &z3, &gamma);
    if (big_int_sign(&z3) == -1)
        big_int_add(&z3, &z3, p);
    big_int_sub(&z3, &z3, &delta);
    if (big_int_sign(&z3) == -1)
        big_int_add(&z3, &z3, p);
    big_int_mod(&z3, &z3, p);

    big_int_lsh(beta, beta, 2);
    big_int_sub(beta, beta, &x3);
    if (big_int_sign(beta) == -1)
        big_int_add(beta, beta, p);
    /* And y3 is alpha. */
    BigInt *y3 = big_int_mul(&alpha, &alpha, beta);

    big_int_mul(&gamma, &gamma, &gamma);
    big_int_lsh(&gamma, &gamma, 3);
    big_int_mod(&gamma, &gamma, p);

    big_int_sub(y3, y3, &gamma);
    if (big_int_sign(y3) == -1)
        big_int_add(y3, y3, p);
    big_int_mod(y3, y3, p);

    big_int_set(&out->x, &x3);
    big_int_set(&out->y, y3);
    big_int_set(&out->z, &z3);
    BigInt *tmp[] = {&delta, &gamma, &alpha, &alpha2, &x3, &beta8, &z3};
    for (size_t k = 0; k < sizeof tmp / sizeof tmp[0]; k++)
        big_int_free(tmp[k]);
}

BigInt *elliptic_curve_params_add(EllipticCurveParams *curve, Alloc *a,
                                  const BigInt *x1, const BigInt *y1, const BigInt *x2,
                                  const BigInt *y2, BigInt **y) {
    EllipticNist *specific = ell_specific(curve);
    if (specific != NULL)
        return nist_add(specific, a, x1, y1, x2, y2, y);
    EllipticCurve c = elliptic_curve_params_as_elliptic_curve(curve);
    ell_panic_if_not_on_curve(c, x1, y1);
    ell_panic_if_not_on_curve(c, x2, y2);

    BigInt z1 = BIG_INT(NULL), z2 = BIG_INT(NULL);
    ell_z_for_affine(&z1, x1, y1);
    ell_z_for_affine(&z2, x2, y2);
    EllJac j;
    ell_jac_init(&j);
    ell_add_jacobian(curve, &j, x1, y1, &z1, x2, y2, &z2);
    BigInt *x = ell_affine_from_jacobian(curve, a, &j, y);
    ell_jac_free(&j);
    big_int_free(&z1);
    big_int_free(&z2);
    return x;
}

BigInt *elliptic_curve_params_double(EllipticCurveParams *curve, Alloc *a,
                                     const BigInt *x1, const BigInt *y1, BigInt **y) {
    EllipticNist *specific = ell_specific(curve);
    if (specific != NULL)
        return nist_double(specific, a, x1, y1, y);
    ell_panic_if_not_on_curve(elliptic_curve_params_as_elliptic_curve(curve), x1, y1);

    BigInt z1 = BIG_INT(NULL);
    ell_z_for_affine(&z1, x1, y1);
    EllJac j;
    ell_jac_init(&j);
    ell_double_jacobian(curve, &j, x1, y1, &z1);
    BigInt *x = ell_affine_from_jacobian(curve, a, &j, y);
    ell_jac_free(&j);
    big_int_free(&z1);
    return x;
}

BigInt *elliptic_curve_params_scalar_mult(EllipticCurveParams *curve, Alloc *a,
                                          const BigInt *bx, const BigInt *by, Slice k,
                                          BigInt **y) {
    EllipticNist *specific = ell_specific(curve);
    if (specific != NULL)
        return nist_scalar_mult(specific, a, bx, by, k, y);
    ell_panic_if_not_on_curve(elliptic_curve_params_as_elliptic_curve(curve), bx, by);

    BigInt bz = BIG_INT(NULL);
    big_int_set_int64(&bz, 1);
    EllJac j;
    ell_jac_init(&j);
    const uint8_t *kb = k.p;
    for (Int i = 0; i < k.len; i++) {
        uint8_t b = kb[i];
        for (int bit = 0; bit < 8; bit++) {
            ell_double_jacobian(curve, &j, &j.x, &j.y, &j.z);
            if ((b & 0x80) == 0x80)
                ell_add_jacobian(curve, &j, bx, by, &bz, &j.x, &j.y, &j.z);
            b = (uint8_t)(b << 1);
        }
    }
    BigInt *x = ell_affine_from_jacobian(curve, a, &j, y);
    ell_jac_free(&j);
    big_int_free(&bz);
    return x;
}

BigInt *elliptic_curve_params_scalar_base_mult(EllipticCurveParams *curve, Alloc *a,
                                               Slice k, BigInt **y) {
    EllipticNist *specific = ell_specific(curve);
    if (specific != NULL)
        return nist_scalar_base_mult(specific, a, k, y);
    return elliptic_curve_params_scalar_mult(curve, a, curve->gx, curve->gy, k, y);
}

static EllipticCurveParams *ell_params_params(void *self) {
    return self;
}

static bool ell_params_is_on_curve(void *self, const BigInt *x, const BigInt *y) {
    return elliptic_curve_params_is_on_curve(self, x, y);
}

static BigInt *ell_params_add(void *self, Alloc *a, const BigInt *x1, const BigInt *y1,
                              const BigInt *x2, const BigInt *y2, BigInt **y) {
    return elliptic_curve_params_add(self, a, x1, y1, x2, y2, y);
}

static BigInt *ell_params_double(void *self, Alloc *a, const BigInt *x1,
                                 const BigInt *y1, BigInt **y) {
    return elliptic_curve_params_double(self, a, x1, y1, y);
}

static BigInt *ell_params_scalar_mult(void *self, Alloc *a, const BigInt *x1,
                                      const BigInt *y1, Slice k, BigInt **y) {
    return elliptic_curve_params_scalar_mult(self, a, x1, y1, k, y);
}

static BigInt *ell_params_scalar_base_mult(void *self, Alloc *a, Slice k, BigInt **y) {
    return elliptic_curve_params_scalar_base_mult(self, a, k, y);
}

static const EllipticCurveVT elliptic_params_vt = {
    &burrow_type_EllipticCurveParams,
    ell_params_params,
    ell_params_is_on_curve,
    ell_params_add,
    ell_params_double,
    ell_params_scalar_mult,
    ell_params_scalar_base_mult,
};

EllipticCurve elliptic_curve_params_as_elliptic_curve(EllipticCurveParams *curve) {
    EllipticCurve c = {&elliptic_params_vt, curve};
    return c;
}

/* ---------------------------------------------------------------- package */

static const uint8_t ell_mask[] = {0xff, 0x1, 0x3, 0x7, 0xf, 0x1f, 0x3f, 0x7f};

Slice elliptic_generate_key(Alloc *a, EllipticCurve curve, IoReader rand, BigInt **x,
                            BigInt **y, Error *err) {
    const BigInt *n = elliptic_curve_params(curve)->n;
    Int bit_size = big_int_bit_len(n);
    Int byte_len = (bit_size + 7) / 8;
    if (rand.vt == NULL)
        rand = crypto_rand_reader;
    Slice priv = ell_bytes(a, byte_len);
    uint8_t *p = priv.p;
    BigInt k = BIG_INT(NULL);
    BigInt *rx = NULL, *ry = NULL;
    while (rx == NULL) {
        Error e = BURROW_NO_ERROR;
        (void)io_read_full(rand, priv, &e);
        if (BURROW_FAILED(e)) {
            big_int_free(&k);
            mem_free(ell_alloc(a), p, (size_t)byte_len, 1);
            BURROW_OUT(err, e);
            if (x != NULL)
                *x = NULL;
            if (y != NULL)
                *y = NULL;
            return (Slice){0};
        }
        /* We have to mask off any excess bits in the case that the size of the
         * underlying field is not a whole number of bytes. */
        p[0] &= ell_mask[bit_size % 8];
        /* This is because, in tests, rand will return all zeros and we don't
         * want to get the point at infinity and loop forever. */
        p[1] ^= 0x42;

        /* If the scalar is out of range, sample another random number. */
        if (big_int_cmp(big_int_set_bytes(&k, priv), n) >= 0)
            continue;

        rx = elliptic_curve_scalar_base_mult(curve, a, priv, &ry);
    }
    big_int_free(&k);
    ell_out(a, x, rx);
    ell_out(a, y, ry);
    BURROW_OUT(err, BURROW_NO_ERROR);
    return priv;
}

Slice elliptic_marshal(Alloc *a, EllipticCurve curve, const BigInt *x,
                       const BigInt *y) {
    ell_panic_if_not_on_curve(curve, x, y);

    Int n = ell_byte_len(elliptic_curve_params(curve));
    Slice ret = ell_bytes(a, 1 + 2 * n);
    uint8_t *p = ret.p;
    p[0] = 4; /* uncompressed point */
    big_int_fill_bytes(x, ell_sub(p, 1, 1 + n));
    big_int_fill_bytes(y, ell_sub(p, 1 + n, 1 + 2 * n));
    return ret;
}

Slice elliptic_marshal_compressed(Alloc *a, EllipticCurve curve, const BigInt *x,
                                  const BigInt *y) {
    ell_panic_if_not_on_curve(curve, x, y);
    Int n = ell_byte_len(elliptic_curve_params(curve));
    Slice compressed = ell_bytes(a, 1 + n);
    uint8_t *p = compressed.p;
    p[0] = (uint8_t)(big_int_bit(y, 0) | 2);
    big_int_fill_bytes(x, ell_sub(p, 1, 1 + n));
    return compressed;
}

BigInt *elliptic_unmarshal(Alloc *a, EllipticCurve curve, Slice data, BigInt **y) {
    if (curve.vt == &elliptic_nist_vt)
        return nist_unmarshal(curve.data, a, data, y);

    const EllipticCurveParams *params = elliptic_curve_params(curve);
    Int n = ell_byte_len(params);
    if (data.len != 1 + 2 * n)
        return ell_nil(y);
    uint8_t *d = data.p;
    if (d[0] != 4) /* uncompressed form */
        return ell_nil(y);
    BigInt *x = big_int_set_bytes(big_new_int(a, 0), ell_sub(d, 1, 1 + n));
    BigInt *ry = big_int_set_bytes(big_new_int(a, 0), ell_sub(d, 1 + n, data.len));
    if (big_int_cmp(x, params->p) >= 0 || big_int_cmp(ry, params->p) >= 0 ||
        !elliptic_curve_is_on_curve(curve, x, ry)) {
        big_int_free(x);
        big_int_free(ry);
        mem_free(ell_alloc(a), x, sizeof(BigInt), _Alignof(BigInt));
        mem_free(ell_alloc(a), ry, sizeof(BigInt), _Alignof(BigInt));
        return ell_nil(y);
    }
    ell_out(a, y, ry);
    return x;
}

BigInt *elliptic_unmarshal_compressed(Alloc *a, EllipticCurve curve, Slice data,
                                      BigInt **y) {
    if (curve.vt == &elliptic_nist_vt)
        return nist_unmarshal_compressed(curve.data, a, data, y);

    const EllipticCurveParams *params = elliptic_curve_params(curve);
    Int n = ell_byte_len(params);
    if (data.len != 1 + n)
        return ell_nil(y);
    uint8_t *d = data.p;
    if (d[0] != 2 && d[0] != 3) /* compressed form */
        return ell_nil(y);
    const BigInt *p = params->p;
    BigInt x = BIG_INT(NULL), ry = BIG_INT(NULL);
    big_int_set_bytes(&x, ell_sub(d, 1, data.len));
    BigInt *got = NULL;
    if (big_int_cmp(&x, p) < 0) {
        /* y² = x³ - 3x + b */
        ell_polynomial(params, &ry, &x);
        if (big_int_mod_sqrt(&ry, &ry, p) != NULL) {
            if ((uint8_t)big_int_bit(&ry, 0) != (d[0] & 1)) {
                big_int_neg(&ry, &ry);
                big_int_mod(&ry, &ry, p);
            }
            if (elliptic_curve_is_on_curve(curve, &x, &ry))
                got = ell_result(a, &x, &ry, y);
        }
    }
    big_int_free(&x);
    big_int_free(&ry);
    return got != NULL ? got : ell_nil(y);
}
