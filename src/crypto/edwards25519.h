/* The edwards25519 group and the two fields under it, which crypto/ed25519 is
 * built on. Go keeps these in crypto/internal/fips140/edwards25519 and its field
 * subpackage, where nothing outside the standard library can import them, so
 * here they are a private header that the files of crypto/ed25519 and the tests
 * include.
 *
 * Three types, as in Go. Edwards25519Element is a number modulo 2^255-19, kept
 * as five 51 bit limbs, and its functions start burrow__fe_. Edwards25519Scalar
 * is a number modulo the group order l = 2^252 +
 * 27742317777372353535851937790883648493, kept in the Montgomery form of the
 * fiat-crypto code it runs on, and its functions start burrow__sc_.
 * Edwards25519Point is a point on the curve in extended coordinates, and its
 * functions start burrow__ge_. Every function takes the result first, like Go's
 * receiver, returns it, and allows any of its arguments to be the same object.
 *
 * Where Go returns a slice of 32 bytes, the function here writes them to an
 * array the caller has, which is what Go's own outlined bytes methods do. Where
 * Go returns an error for input of the wrong length, the function takes a Slice
 * and an Error * and returns NULL, leaving the result as it was.
 *
 * A zero Edwards25519Element and a zero Edwards25519Scalar are both zero. A zero
 * Edwards25519Point is not a point, and every function that reads one panics
 * with "edwards25519: use of uninitialized Point", as Go's does.
 *
 * Copyright 2026 The burrow Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style licence that can be found
 * in the LICENSE file. */

#ifndef BURROW_CRYPTO_EDWARDS25519_H
#define BURROW_CRYPTO_EDWARDS25519_H

/* ed25519.h comes first so that the amalgamation files this header with
 * crypto/ed25519, the package these are part of. */
#include "burrow/crypto/ed25519.h"

#include "burrow/core.h"
#include "burrow/error.h"
#include "burrow/slice.h"

#include <stdint.h>

/* ----------------------------------------------------------------- field */

/* t.l0 + t.l1*2^51 + t.l2*2^102 + t.l3*2^153 + t.l4*2^204. Between operations
 * every limb is below 2^52. */
typedef struct Edwards25519Element {
    uint64_t l0, l1, l2, l3, l4;
} Edwards25519Element;

Edwards25519Element *burrow__fe_zero(Edwards25519Element *v);
Edwards25519Element *burrow__fe_one(Edwards25519Element *v);
Edwards25519Element *burrow__fe_set(Edwards25519Element *v,
                                    const Edwards25519Element *a);
Edwards25519Element *burrow__fe_add(Edwards25519Element *v,
                                    const Edwards25519Element *a,
                                    const Edwards25519Element *b);
Edwards25519Element *burrow__fe_subtract(Edwards25519Element *v,
                                         const Edwards25519Element *a,
                                         const Edwards25519Element *b);
Edwards25519Element *burrow__fe_negate(Edwards25519Element *v,
                                       const Edwards25519Element *a);
Edwards25519Element *burrow__fe_invert(Edwards25519Element *v,
                                       const Edwards25519Element *z);
Edwards25519Element *burrow__fe_multiply(Edwards25519Element *v,
                                         const Edwards25519Element *x,
                                         const Edwards25519Element *y);
Edwards25519Element *burrow__fe_square(Edwards25519Element *v,
                                       const Edwards25519Element *x);
/* x^(2^n). n has to be positive. */
Edwards25519Element *burrow__fe_square_n(Edwards25519Element *v,
                                         const Edwards25519Element *x, int n);
Edwards25519Element *burrow__fe_mult32(Edwards25519Element *v,
                                       const Edwards25519Element *x, uint32_t y);
/* x^((p-5)/8). */
Edwards25519Element *burrow__fe_pow22523(Edwards25519Element *v,
                                         const Edwards25519Element *x);
/* The non-negative square root of u/v into r. *was_square is 1 if u/v is a
 * square, and 0 if not, when r is what section 4.3 of
 * draft-irtf-cfrg-ristretto255-decaf448-00 says. */
Edwards25519Element *burrow__fe_sqrt_ratio(Edwards25519Element *r,
                                           const Edwards25519Element *u,
                                           const Edwards25519Element *v,
                                           int *was_square);
/* a if cond is 1 and b if it is 0. */
Edwards25519Element *burrow__fe_select(Edwards25519Element *v,
                                       const Edwards25519Element *a,
                                       const Edwards25519Element *b, int cond);
/* Swaps v and u if cond is 1. */
void burrow__fe_swap(Edwards25519Element *v, Edwards25519Element *u, int cond);
Edwards25519Element *burrow__fe_absolute(Edwards25519Element *v,
                                         const Edwards25519Element *u);
int burrow__fe_is_negative(const Edwards25519Element *v);
int burrow__fe_equal(const Edwards25519Element *v, const Edwards25519Element *u);
/* Reads 32 little endian bytes. The top bit is ignored and values from p up are
 * taken, as RFC 7748 says. Anything but 32 bytes is an error. */
Edwards25519Element *burrow__fe_set_bytes(Edwards25519Element *v, Slice x, Error *err);
/* The canonical 32 byte little endian encoding. */
void burrow__fe_bytes(const Edwards25519Element *v, uint8_t out[32]);

/* What Go's tests reach inside the field package for. */
Edwards25519Element *burrow__fe_reduce(Edwards25519Element *v);
Edwards25519Element *burrow__fe_carry_propagate(Edwards25519Element *v);
void burrow__fe_mul_generic(Edwards25519Element *v, const Edwards25519Element *a,
                            const Edwards25519Element *b);
void burrow__fe_square_generic(Edwards25519Element *v, const Edwards25519Element *a);

/* Go's uint128 in fe_generic.go, and its mul and addMul: a * b, and v + a * b. */
typedef struct Edwards25519Uint128 {
    uint64_t lo, hi;
} Edwards25519Uint128;

Edwards25519Uint128 burrow__fe_mul64(uint64_t a, uint64_t b);
Edwards25519Uint128 burrow__fe_add_mul64(Edwards25519Uint128 v, uint64_t a, uint64_t b);

/* sqrt(-1), 2^((p-1)/4). */
extern const Edwards25519Element burrow__fe_sqrt_m1;

/* ---------------------------------------------------------------- scalar */

/* The scalar in the Montgomery domain, with R = 2^256, as four little endian 64
 * bit words. */
typedef struct Edwards25519Scalar {
    uint64_t s[4];
} Edwards25519Scalar;

Edwards25519Scalar *burrow__sc_set(Edwards25519Scalar *s, const Edwards25519Scalar *x);
Edwards25519Scalar *burrow__sc_add(Edwards25519Scalar *s, const Edwards25519Scalar *x,
                                   const Edwards25519Scalar *y);
Edwards25519Scalar *burrow__sc_subtract(Edwards25519Scalar *s,
                                        const Edwards25519Scalar *x,
                                        const Edwards25519Scalar *y);
Edwards25519Scalar *burrow__sc_negate(Edwards25519Scalar *s,
                                      const Edwards25519Scalar *x);
Edwards25519Scalar *burrow__sc_multiply(Edwards25519Scalar *s,
                                        const Edwards25519Scalar *x,
                                        const Edwards25519Scalar *y);
/* x * y + z. */
Edwards25519Scalar *burrow__sc_multiply_add(Edwards25519Scalar *s,
                                            const Edwards25519Scalar *x,
                                            const Edwards25519Scalar *y,
                                            const Edwards25519Scalar *z);
/* x mod l, for 64 little endian bytes of x. */
Edwards25519Scalar *burrow__sc_set_uniform_bytes(Edwards25519Scalar *s, Slice x,
                                                 Error *err);
/* 32 little endian bytes that have to be below l already. */
Edwards25519Scalar *burrow__sc_set_canonical_bytes(Edwards25519Scalar *s, Slice x,
                                                   Error *err);
/* 32 bytes clamped as RFC 8032 section 5.1.5 says, then reduced. */
Edwards25519Scalar *burrow__sc_set_bytes_with_clamping(Edwards25519Scalar *s, Slice x,
                                                       Error *err);
void burrow__sc_bytes(const Edwards25519Scalar *s, uint8_t out[32]);
int burrow__sc_equal(const Edwards25519Scalar *s, const Edwards25519Scalar *t);

/* What Go's tests reach inside the package for. */
bool burrow__sc_is_reduced(Slice s);
void burrow__sc_non_adjacent_form(const Edwards25519Scalar *s, unsigned w,
                                  int8_t naf[256]);
void burrow__sc_signed_radix16(const Edwards25519Scalar *s, int8_t digits[64]);

/* ----------------------------------------------------------------- point */

/* x = X/Z, y = Y/Z and xy = T/Z, from https://eprint.iacr.org/2008/522. */
typedef struct Edwards25519Point {
    Edwards25519Element x, y, z, t;
} Edwards25519Point;

typedef struct Edwards25519ProjP1xP1 {
    Edwards25519Element X, Y, Z, T;
} Edwards25519ProjP1xP1;

typedef struct Edwards25519ProjP2 {
    Edwards25519Element X, Y, Z;
} Edwards25519ProjP2;

typedef struct Edwards25519ProjCached {
    Edwards25519Element YplusX, YminusX, Z, T2d;
} Edwards25519ProjCached;

typedef struct Edwards25519AffineCached {
    Edwards25519Element YplusX, YminusX, T2d;
} Edwards25519AffineCached;

/* d, the curve constant in -x^2 + y^2 = 1 + d x^2 y^2, and 2d. */
extern const Edwards25519Element burrow__ge_d;
extern const Edwards25519Element burrow__ge_d2;

Edwards25519Point *burrow__ge_identity(Edwards25519Point *v);
Edwards25519Point *burrow__ge_generator(Edwards25519Point *v);
Edwards25519Point *burrow__ge_set(Edwards25519Point *v, const Edwards25519Point *u);
/* The RFC 8032 section 5.1.2 encoding. */
void burrow__ge_bytes(const Edwards25519Point *v, uint8_t out[32]);
/* Decodes 32 bytes, taking the non-canonical encodings most implementations
 * take: a y that is not reduced, and an x of zero with the sign bit set. */
Edwards25519Point *burrow__ge_set_bytes(Edwards25519Point *v, Slice x, Error *err);
Edwards25519Point *burrow__ge_add(Edwards25519Point *v, const Edwards25519Point *p,
                                  const Edwards25519Point *q);
Edwards25519Point *burrow__ge_subtract(Edwards25519Point *v, const Edwards25519Point *p,
                                       const Edwards25519Point *q);
Edwards25519Point *burrow__ge_negate(Edwards25519Point *v, const Edwards25519Point *p);
int burrow__ge_equal(const Edwards25519Point *v, const Edwards25519Point *u);
/* x * B for the generator B, in constant time. */
Edwards25519Point *burrow__ge_scalar_base_mult(Edwards25519Point *v,
                                               const Edwards25519Scalar *x);
/* x * q, in constant time. */
Edwards25519Point *burrow__ge_scalar_mult(Edwards25519Point *v,
                                          const Edwards25519Scalar *x,
                                          const Edwards25519Point *q);
/* a * A + b * B for the generator B, in time that depends on the inputs. */
Edwards25519Point *burrow__ge_var_time_double_scalar_base_mult(
    Edwards25519Point *v, const Edwards25519Scalar *a, const Edwards25519Point *A,
    const Edwards25519Scalar *b);

/* The coordinate systems the arithmetic moves between, for the tests. */
Edwards25519ProjP2 *burrow__ge_p2_zero(Edwards25519ProjP2 *v);
Edwards25519ProjP2 *burrow__ge_p2_from_p1xp1(Edwards25519ProjP2 *v,
                                             const Edwards25519ProjP1xP1 *p);
Edwards25519ProjP2 *burrow__ge_p2_from_p3(Edwards25519ProjP2 *v,
                                          const Edwards25519Point *p);
Edwards25519Point *burrow__ge_from_p1xp1(Edwards25519Point *v,
                                         const Edwards25519ProjP1xP1 *p);
Edwards25519Point *burrow__ge_from_p2(Edwards25519Point *v,
                                      const Edwards25519ProjP2 *p);
Edwards25519ProjCached *burrow__ge_cached_zero(Edwards25519ProjCached *v);
Edwards25519ProjCached *burrow__ge_cached_from_p3(Edwards25519ProjCached *v,
                                                  const Edwards25519Point *p);
Edwards25519AffineCached *burrow__ge_affine_zero(Edwards25519AffineCached *v);
Edwards25519AffineCached *burrow__ge_affine_from_p3(Edwards25519AffineCached *v,
                                                    const Edwards25519Point *p);
Edwards25519ProjP1xP1 *burrow__ge_p1xp1_add(Edwards25519ProjP1xP1 *v,
                                            const Edwards25519Point *p,
                                            const Edwards25519ProjCached *q);
Edwards25519ProjP1xP1 *burrow__ge_p1xp1_sub(Edwards25519ProjP1xP1 *v,
                                            const Edwards25519Point *p,
                                            const Edwards25519ProjCached *q);
Edwards25519ProjP1xP1 *burrow__ge_p1xp1_add_affine(Edwards25519ProjP1xP1 *v,
                                                   const Edwards25519Point *p,
                                                   const Edwards25519AffineCached *q);
Edwards25519ProjP1xP1 *burrow__ge_p1xp1_sub_affine(Edwards25519ProjP1xP1 *v,
                                                   const Edwards25519Point *p,
                                                   const Edwards25519AffineCached *q);
Edwards25519ProjP1xP1 *burrow__ge_p1xp1_double(Edwards25519ProjP1xP1 *v,
                                               const Edwards25519ProjP2 *p);

/* ---------------------------------------------------------------- tables */

/* Q, 2Q up to 8Q, for a constant time multiple of a point that changes. */
typedef struct Edwards25519ProjLookupTable {
    Edwards25519ProjCached points[8];
} Edwards25519ProjLookupTable;

/* The same for a point that does not change, kept affine. */
typedef struct Edwards25519AffineLookupTable {
    Edwards25519AffineCached points[8];
} Edwards25519AffineLookupTable;

/* Q, 3Q, 5Q up to 15Q, for a variable time multiple of a point that changes. */
typedef struct Edwards25519NafLookupTable5 {
    Edwards25519ProjCached points[8];
} Edwards25519NafLookupTable5;

/* Q, 3Q up to 127Q, for a variable time multiple of a point that does not. */
typedef struct Edwards25519NafLookupTable8 {
    Edwards25519AffineCached points[64];
} Edwards25519NafLookupTable8;

void burrow__ge_proj_table_from_p3(Edwards25519ProjLookupTable *v,
                                   const Edwards25519Point *q);
void burrow__ge_affine_table_from_p3(Edwards25519AffineLookupTable *v,
                                     const Edwards25519Point *q);
void burrow__ge_naf5_table_from_p3(Edwards25519NafLookupTable5 *v,
                                   const Edwards25519Point *q);
void burrow__ge_naf8_table_from_p3(Edwards25519NafLookupTable8 *v,
                                   const Edwards25519Point *q);
/* x * Q for -8 <= x <= 8, in constant time. */
void burrow__ge_proj_table_select(const Edwards25519ProjLookupTable *v,
                                  Edwards25519ProjCached *dest, int8_t x);
void burrow__ge_affine_table_select(const Edwards25519AffineLookupTable *v,
                                    Edwards25519AffineCached *dest, int8_t x);
/* x * Q for odd x, 0 < x < 16 and 0 < x < 128, in variable time. */
void burrow__ge_naf5_table_select(const Edwards25519NafLookupTable5 *v,
                                  Edwards25519ProjCached *dest, int8_t x);
void burrow__ge_naf8_table_select(const Edwards25519NafLookupTable8 *v,
                                  Edwards25519AffineCached *dest, int8_t x);

/* The tables for the generator, 256^i * B for i up to 31 and the NAF table of B,
 * which Go builds the first time they are used and which are constants here,
 * printed from Go's by tools/gen-ed25519-tables.sh. */
extern const Edwards25519AffineLookupTable burrow__ge_basepoint_table[32];
extern const Edwards25519NafLookupTable8 burrow__ge_basepoint_naf_table;

#endif /* BURROW_CRYPTO_EDWARDS25519_H */
