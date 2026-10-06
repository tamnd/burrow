/* The NIST P-224, P-256, P-384 and P-521 groups, which crypto/ecdh is built on
 * and crypto/ecdsa and crypto/elliptic will be. Go keeps these in
 * crypto/internal/fips140/nistec, where nothing outside the standard library
 * can import them, so here they are a private header.
 *
 * A point is in projective coordinates (X:Y:Z), with x = X/Z and y = Y/Z, and
 * infinity is (0:1:0). Each coordinate is a field element from fiat-crypto,
 * which is a Montgomery form number in four or six 64 bit words, or seven,
 * eight or twelve 32 bit words, and for P-521 an unsaturated Solinas number in
 * nine or nineteen words. The structs here only need the size of one, which is
 * what the counts below are. Each nistec_pNNN.c checks its count against the
 * fiat header it includes.
 *
 * Go has a type per curve with the same methods on each, and the packages that
 * use them are generic over that. Here a curve is a NistecCurve, a table of the
 * same methods over a NistecPoint, which is big enough for a point on any of
 * the four. Every method takes the result first, like Go's receiver, and
 * allows any of its points to be the same object. Where Go returns an error,
 * the method takes an Error * and returns NULL, leaving the result as it was.
 * Where Go returns a slice of bytes, the method writes them to an array the
 * caller has, of NISTEC_MAX_POINT_BYTES, and returns how many it wrote.
 *
 * Copyright 2026 The burrow Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style licence that can be found
 * in the LICENSE file. */

#ifndef BURROW_CRYPTO_NISTEC_H
#define BURROW_CRYPTO_NISTEC_H

/* ecdh.h comes first so that the amalgamation files this header with
 * crypto/ecdh, the package these are part of. */
#include "burrow/crypto/ecdh.h"

#include "burrow/core.h"
#include "burrow/error.h"
#include "burrow/slice.h"

#include <stdint.h>

#if defined(__SIZEOF_INT128__)
typedef uint64_t NistecLimb;
#define NISTEC_P224_LIMBS 4
#define NISTEC_P256_LIMBS 4
#define NISTEC_P384_LIMBS 6
#define NISTEC_P521_LIMBS 9
#else
typedef uint32_t NistecLimb;
#define NISTEC_P224_LIMBS 7
#define NISTEC_P256_LIMBS 8
#define NISTEC_P384_LIMBS 12
#define NISTEC_P521_LIMBS 19
#endif

typedef struct NistecP224Element {
    NistecLimb l[NISTEC_P224_LIMBS];
} NistecP224Element;
typedef struct NistecP256Element {
    NistecLimb l[NISTEC_P256_LIMBS];
} NistecP256Element;
typedef struct NistecP384Element {
    NistecLimb l[NISTEC_P384_LIMBS];
} NistecP384Element;
typedef struct NistecP521Element {
    NistecLimb l[NISTEC_P521_LIMBS];
} NistecP521Element;

typedef struct NistecP224Point {
    NistecP224Element x, y, z;
} NistecP224Point;
typedef struct NistecP256Point {
    NistecP256Element x, y, z;
} NistecP256Point;
typedef struct NistecP384Point {
    NistecP384Element x, y, z;
} NistecP384Point;
typedef struct NistecP521Point {
    NistecP521Element x, y, z;
} NistecP521Point;

typedef union NistecPoint {
    NistecP224Point p224;
    NistecP256Point p256;
    NistecP384Point p384;
    NistecP521Point p521;
} NistecPoint;

/* The longest element is P-521's, and the longest encoding of a point is the
 * uncompressed one, a 4 and then x and y. */
#define NISTEC_MAX_ELEMENT_BYTES 66
#define NISTEC_MAX_POINT_BYTES (1 + 2 * NISTEC_MAX_ELEMENT_BYTES)

typedef struct NistecCurve {
    /* "P-256" and so on. */
    const char *name;
    /* The length of an element of the base field or the scalar field, which is
     * the same for each of the four. */
    Int element_bytes;

    /* NewPNNNPoint: sets p to the point at infinity. */
    NistecPoint *(*point_new)(NistecPoint *p);
    NistecPoint *(*set_generator)(NistecPoint *p);
    NistecPoint *(*set)(NistecPoint *p, const NistecPoint *q);
    /* Decodes the compressed, uncompressed or infinity encoding in b, from
     * Section 2.3.4 of SEC 1, version 2.0. */
    NistecPoint *(*set_bytes)(NistecPoint *p, Slice b, Error *err);
    /* The uncompressed or infinity encoding. Infinity is one zero byte. */
    Int (*bytes)(const NistecPoint *p, uint8_t *out);
    /* x on its own, which is an error for infinity. Writes element_bytes. */
    uint8_t *(*bytes_x)(const NistecPoint *p, uint8_t *out, Error *err);
    /* The compressed or infinity encoding. */
    Int (*bytes_compressed)(const NistecPoint *p, uint8_t *out);
    NistecPoint *(*add)(NistecPoint *q, const NistecPoint *p1, const NistecPoint *p2);
    NistecPoint *(*double_)(NistecPoint *q, const NistecPoint *p);
    /* scalar * q, for a big endian scalar. P-256 wants it to be element_bytes
     * long and the others take any length, as in Go. */
    NistecPoint *(*scalar_mult)(NistecPoint *p, const NistecPoint *q, Slice scalar,
                                Error *err);
    /* scalar * the generator, for a scalar of element_bytes. */
    NistecPoint *(*scalar_base_mult)(NistecPoint *p, Slice scalar, Error *err);
} NistecCurve;

extern const NistecCurve burrow__nistec_p224;
extern const NistecCurve burrow__nistec_p256;
extern const NistecCurve burrow__nistec_p384;
extern const NistecCurve burrow__nistec_p521;

/* crypto/internal/fips140/subtle's ConstantTimeLessOrEqBytes on two arrays of
 * n bytes: 1 if x <= y as big endian numbers and 0 if not, in time that does
 * not depend on either. */
int burrow__nistec_less_or_eq_bytes(const uint8_t *x, const uint8_t *y, Int n);

/* 1 if the n bytes at x and y are the same and 0 if not, in time that does not
 * depend on them. */
int burrow__nistec_equal_bytes(const uint8_t *x, const uint8_t *y, Int n);

#endif
