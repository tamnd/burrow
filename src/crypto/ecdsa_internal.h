/* What ecdsa.c has inside, for tests/ecdsa_test.c: the parts of Go's
 * crypto/internal/fips140/ecdsa that its own tests and crypto/ecdsa's tests
 * reach, which Go can because they are in the same module.
 *
 * Copyright 2024 The Go Authors. All rights reserved.
 * Copyright 2026 The burrow Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style licence that can be found
 * in the LICENSE file. */

#ifndef BURROW_SRC_CRYPTO_ECDSA_INTERNAL_H
#define BURROW_SRC_CRYPTO_ECDSA_INTERNAL_H

/* ecdsa.h comes first so that the amalgamation files this header with
 * crypto/ecdsa. */
#include "burrow/crypto/ecdsa.h"

#include "burrow/core.h"
#include "burrow/error.h"
#include "burrow/func.h"
#include "burrow/hash.h"
#include "burrow/mem.h"
#include "burrow/slice.h"

#include <stdbool.h>
#include <stdint.h>

/* ecdsa.Curve: one of the four NIST curves, with its order as a modulus. */
typedef struct EcdsaFipsCurve EcdsaFipsCurve;

/* ecdsa.P224, P256, P384 and P521. */
const EcdsaFipsCurve *burrow__ecdsa_fips_p224(void);
const EcdsaFipsCurve *burrow__ecdsa_fips_p256(void);
const EcdsaFipsCurve *burrow__ecdsa_fips_p384(void);
const EcdsaFipsCurve *burrow__ecdsa_fips_p521(void);

/* The length of the curve's order in bytes, N.Size() in Go. */
Int burrow__ecdsa_fips_size(const EcdsaFipsCurve *c);

/* What randomPoint takes to fill b, returning false and setting *err when it
 * cannot. */
BURROW_FUNC(EcdsaFill, bool, Slice b, Error *err);

/* randomPoint: a scalar k from what gen gives, rejecting zero and anything not
 * less than the order, and k times the generator. k goes to k_out, as long as
 * the order, and the point's uncompressed encoding to p_out, which has room
 * for NISTEC_MAX_POINT_BYTES. *loops, when loops is not NULL, goes up by one
 * for each rejection, which is Go's testingOnlyRejectionSamplingLooped. */
bool burrow__ecdsa_random_point(const EcdsaFipsCurve *c, EcdsaFill gen, uint8_t *k_out,
                                uint8_t *p_out, Int *loops, Error *err);

/* bits2octets: hash cut down to the order and reduced modulo it, as long as
 * the order, in out. hashToNat is the same without the bytes at the end. */
void burrow__ecdsa_bits2octets(const EcdsaFipsCurve *c, Slice hash, uint8_t *out);

/* hmacDRBG: the HMAC_DRBG of SP 800-90A Rev. 1 that the nonces come from. */
typedef struct EcdsaDrbg EcdsaDrbg;

/* TestingOnlyNewDRBG: a DRBG over the hash h makes, seeded with entropy,
 * nonce and a plain personalization string, all from a, which has to last as
 * long as the DRBG. An arena is the thing to give it. */
BURROW_OWNS(ret) EcdsaDrbg *
burrow__ecdsa_new_drbg(Alloc *a, HashNewFunc h, Slice entropy, Slice nonce, Slice pers);

/* hmacDRBG.Generate: fills out, which can be at most 65536 bytes. */
void burrow__ecdsa_drbg_generate(EcdsaDrbg *d, Slice out);

/* sign: the signature of hash by the scalar d, as long as the order, with the
 * nonce from drbg. r and s each get as many bytes as the order has. */
bool burrow__ecdsa_sign_with_drbg(const EcdsaFipsCurve *c, Slice d, EcdsaDrbg *drbg,
                                  Slice hash, uint8_t *r, uint8_t *s, Error *err);

#endif /* BURROW_SRC_CRYPTO_ECDSA_INTERNAL_H */
