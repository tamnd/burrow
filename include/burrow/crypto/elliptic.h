/* crypto/elliptic, the standard NIST curves P-224, P-256, P-384 and P-521 over
 * prime fields, with points as math/big integers.
 *
 *     Arena ar;
 *     arena_init(&ar, NULL, 0);
 *     Alloc *a = arena_allocator(&ar);
 *     BigInt *y;
 *     BigInt *x = elliptic_curve_scalar_base_mult(elliptic_p256(), a, k, &y);
 *     Slice enc = elliptic_marshal(a, elliptic_p256(), x, y);
 *     arena_free(&ar);
 *
 * Go deprecates almost all of this package. The low level operations take and
 * return big integers, which is slow and is not constant time, and anything
 * that is not P-224, P-256, P-384 or P-521 gets a generic implementation that
 * Go calls insecure. For ECDH use crypto/ecdh, and for signatures crypto/ecdsa.
 * It is here for programs that already speak it, and it gives the same answers
 * Go's does.
 *
 * Every function that returns a point returns x and puts y in *y, both new
 * BigInts from a, the struct and its words. y may be NULL when only x is
 * wanted. The point at infinity is (0, 0). An arena is the easy way to look
 * after the results, since the operations make two new integers each call.
 *
 * Go panics when a point given to Add, Double, ScalarMult, Marshal or
 * MarshalCompressed is not on the curve, and so do these, with Go's message.
 *
 * Copyright 2010 The Go Authors. All rights reserved.
 * Copyright 2026 The burrow Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style licence that can be found
 * in the LICENSE file. */

/* burrow:package crypto/elliptic */

#ifndef BURROW_CRYPTO_ELLIPTIC_H
#define BURROW_CRYPTO_ELLIPTIC_H

#include "burrow/core.h"
#include "burrow/error.h"
#include "burrow/io.h"
#include "burrow/math/big.h"
#include "burrow/mem.h"
#include "burrow/slice.h"
#include "burrow/type.h"

#ifdef __cplusplus
extern "C" {
#endif

/* elliptic.CurveParams: the parameters of a short Weierstrass curve
 * y^2 = x^3 - 3x + b over the field of integers mod p, and a generic, non
 * constant time implementation of the Curve operations on it.
 *
 * The params of the four NIST curves are shared and must not be changed. A copy
 * of one, which points at the same integers, is a different curve as far as
 * these functions can tell, and gets the generic implementation, which is what
 * Go's tests use to check one against the other. */
typedef struct EllipticCurveParams {
    BigInt *p;    /* the order of the underlying field */
    BigInt *n;    /* the order of the base point */
    BigInt *b;    /* the constant of the curve equation */
    BigInt *gx;   /* x of the base point */
    BigInt *gy;   /* y of the base point */
    Int bit_size; /* the size of the underlying field */
    Str name;     /* the canonical name of the curve */
} EllipticCurveParams;

extern const Type burrow_type_EllipticCurveParams;
#define TYPE_ELLIPTIC_CURVE_PARAMS TYPE_OF(EllipticCurveParams)

/* elliptic.Curve: a short-form Weierstrass curve with a = -3. Only the four
 * curves below and an EllipticCurveParams are meant to be one, as in Go, where
 * a Curve of any other kind is deprecated. */
typedef struct EllipticCurveVT {
    const Type *self_type;
    /* Params: the curve's parameters. */
    EllipticCurveParams *(*params)(void *self);
    /* IsOnCurve: whether (x, y) is on the curve. Infinity is not. */
    bool (*is_on_curve)(void *self, const BigInt *x, const BigInt *y);
    /* Add: (x1, y1) + (x2, y2). */
    BigInt *(*add)(void *self, Alloc *a, const BigInt *x1, const BigInt *y1,
                   const BigInt *x2, const BigInt *y2, BigInt **y);
    /* Double: 2 * (x1, y1). */
    BigInt *(*double_)(void *self, Alloc *a, const BigInt *x1, const BigInt *y1,
                       BigInt **y);
    /* ScalarMult: k * (x1, y1), for k a big endian integer. */
    BigInt *(*scalar_mult)(void *self, Alloc *a, const BigInt *x1, const BigInt *y1,
                           Slice k, BigInt **y);
    /* ScalarBaseMult: k * G, for the base point G and k a big endian integer. */
    BigInt *(*scalar_base_mult)(void *self, Alloc *a, Slice k, BigInt **y);
} EllipticCurveVT;

typedef struct EllipticCurve {
    const EllipticCurveVT *vt;
    void *data;
} EllipticCurve;

BURROW_BORROWS(ret, c) static inline EllipticCurveParams *
elliptic_curve_params(EllipticCurve c) {
    return c.vt->params(c.data);
}

static inline bool elliptic_curve_is_on_curve(EllipticCurve c, const BigInt *x,
                                              const BigInt *y) {
    return c.vt->is_on_curve(c.data, x, y);
}

BURROW_OWNS(ret) static inline BigInt *
elliptic_curve_add(EllipticCurve c, Alloc *a, const BigInt *x1, const BigInt *y1,
                   const BigInt *x2, const BigInt *y2, BigInt **y) {
    return c.vt->add(c.data, a, x1, y1, x2, y2, y);
}

BURROW_OWNS(ret) static inline BigInt *elliptic_curve_double(EllipticCurve c, Alloc *a,
                                                             const BigInt *x1,
                                                             const BigInt *y1,
                                                             BigInt **y) {
    return c.vt->double_(c.data, a, x1, y1, y);
}

BURROW_OWNS(ret) static inline BigInt *
elliptic_curve_scalar_mult(EllipticCurve c, Alloc *a, const BigInt *x1,
                           const BigInt *y1, Slice k, BigInt **y) {
    return c.vt->scalar_mult(c.data, a, x1, y1, k, y);
}

BURROW_OWNS(ret) static inline BigInt *
elliptic_curve_scalar_base_mult(EllipticCurve c, Alloc *a, Slice k, BigInt **y) {
    return c.vt->scalar_base_mult(c.data, a, k, y);
}

/* elliptic.P224, P256, P384 and P521: the NIST curves of FIPS 186-3, section
 * D.2.2 to D.2.5, also known as secp224r1, secp256r1 or prime256v1, secp384r1
 * and secp521r1. Each call gives the same curve, and two curves are the same
 * when their data pointers are. The operations are constant time, apart from
 * what math/big does to the integers going in and out. */
BURROW_STATIC(ret) EllipticCurve elliptic_p224(void);
BURROW_STATIC(ret) EllipticCurve elliptic_p256(void);
BURROW_STATIC(ret) EllipticCurve elliptic_p384(void);
BURROW_STATIC(ret) EllipticCurve elliptic_p521(void);

/* elliptic.GenerateKey: a new key pair, with the private key as big endian
 * bytes from a and the public key in *x and *y. The private key comes from the
 * bytes of rand, read with io_read_full, and a nil rand means
 * crypto_rand_reader.
 *
 * Go deprecates this for ecdh_curve_generate_key, and for ECDSA keys,
 * crypto/ecdsa's GenerateKey. An error reading rand is returned as it is, with
 * a nil Slice and NULL in *x and *y. */
BURROW_OWNS(ret) Slice elliptic_generate_key(Alloc *a, EllipticCurve curve,
                                             IoReader rand, BigInt **x, BigInt **y,
                                             Error *err);

/* elliptic.Marshal: the point in the uncompressed form of section 4.3.6 of
 * ANSI X9.62, from a. Panics when the point is not on the curve. Infinity,
 * (0, 0), is let through and comes out as zeros after the 4, which
 * elliptic_unmarshal rejects. */
BURROW_OWNS(ret) Slice elliptic_marshal(Alloc *a, EllipticCurve curve, const BigInt *x,
                                        const BigInt *y);

/* elliptic.MarshalCompressed: the point in the compressed form of section 4.3.6
 * of ANSI X9.62, from a. Panics when the point is not on the curve. */
BURROW_OWNS(ret) Slice elliptic_marshal_compressed(Alloc *a, EllipticCurve curve,
                                                   const BigInt *x, const BigInt *y);

/* elliptic.Unmarshal: the point in data, in the form elliptic_marshal writes,
 * from a. NULL, with NULL in *y, when data is not a point on the curve, and
 * infinity is not one. */
BURROW_OWNS(ret) BigInt *elliptic_unmarshal(Alloc *a, EllipticCurve curve, Slice data,
                                            BigInt **y);

/* elliptic.UnmarshalCompressed: the point in data, in the form
 * elliptic_marshal_compressed writes, from a. NULL, with NULL in *y, when data
 * is not a point on the curve. */
BURROW_OWNS(ret) BigInt *elliptic_unmarshal_compressed(Alloc *a, EllipticCurve curve,
                                                       Slice data, BigInt **y);

/* CurveParams.Params: curve itself. */
BURROW_BORROWS(ret, curve) EllipticCurveParams *
elliptic_curve_params_params(EllipticCurveParams *curve);

/* The CurveParams methods: the generic implementation, unless curve is the
 * params of one of the four NIST curves, when they are that curve's own. Go
 * deprecates calling them directly. */
bool elliptic_curve_params_is_on_curve(EllipticCurveParams *curve, const BigInt *x,
                                       const BigInt *y);
BURROW_OWNS(ret) BigInt *elliptic_curve_params_add(EllipticCurveParams *curve, Alloc *a,
                                                   const BigInt *x1, const BigInt *y1,
                                                   const BigInt *x2, const BigInt *y2,
                                                   BigInt **y);
BURROW_OWNS(ret) BigInt *elliptic_curve_params_double(EllipticCurveParams *curve,
                                                      Alloc *a, const BigInt *x1,
                                                      const BigInt *y1, BigInt **y);
BURROW_OWNS(ret) BigInt *elliptic_curve_params_scalar_mult(EllipticCurveParams *curve,
                                                           Alloc *a, const BigInt *x1,
                                                           const BigInt *y1, Slice k,
                                                           BigInt **y);
BURROW_OWNS(ret) BigInt *
elliptic_curve_params_scalar_base_mult(EllipticCurveParams *curve, Alloc *a, Slice k,
                                       BigInt **y);

/* curve as an EllipticCurve, which borrows it. */
BURROW_BORROWS(ret, curve) EllipticCurve
elliptic_curve_params_as_elliptic_curve(EllipticCurveParams *curve);

#ifdef __cplusplus
}
#endif

#endif /* BURROW_CRYPTO_ELLIPTIC_H */
