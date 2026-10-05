/* The parts of crypto/mldsa that its tests look at, from Go's
 * crypto/internal/fips140/mldsa: the field arithmetic of field.go, signing and
 * verifying with a μ and randomness given, the semi-expanded private keys that
 * only the ACVP and Wycheproof tests use, and the self test.
 *
 * Go's tests see which rejection a signature went through by setting the
 * package variable testingOnlyRejectionReason. burrow has no mutable globals
 * for that, so the functions that sign take the same callback as an argument.
 *
 * Copyright 2025 The Go Authors. All rights reserved.
 * Copyright 2026 The burrow Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style licence that can be found
 * in the LICENSE file. */

#ifndef BURROW_SRC_CRYPTO_MLDSA_INTERNAL_H
#define BURROW_SRC_CRYPTO_MLDSA_INTERNAL_H

#include "burrow/crypto/mldsa.h"

#include "burrow/core.h"
#include "burrow/error.h"
#include "burrow/mem.h"
#include "burrow/slice.h"

#include <stdbool.h>
#include <stdint.h>

enum {
    MLDSA_N = 256,
    MLDSA_Q = 8380417,         /* 2²³ - 2¹³ + 1 */
    MLDSA_RR = 2365951,        /* R² mod q, aka R in the Montgomery domain */
    MLDSA_ONE = 4193792,       /* R mod q, aka 1 in the Montgomery domain */
    MLDSA_MINUS_ONE = 4186625, /* (q - 1) * R mod q, aka -1 in the Montgomery domain */
};

/* R = 2³², and -q⁻¹ mod R, so that q * qNegInv ≡ -1 mod R. */
#define MLDSA_R UINT64_C(4294967296)
#define MLDSA_Q_NEG_INV UINT32_C(4236238847)

/* fieldElement: an element n of ℤ_q in the Montgomery domain, an integer x in
 * [0, q) with x ≡ n * R (mod q). */
typedef uint32_t MldsaFieldElement;

/* zetas: ζ^BitRev₈(k) mod q for each k, in the Montgomery domain. */
extern const MldsaFieldElement burrow__mldsa_zetas[256];

/* fieldToMontgomery: false for an a that is not below q, which is Go's
 * "mldsa: unreduced field element", and a in the Montgomery domain in *out
 * otherwise. */
bool burrow__mldsa_field_to_montgomery(uint32_t a, MldsaFieldElement *out);

MldsaFieldElement burrow__mldsa_field_sub_to_montgomery(uint32_t a, uint32_t b);
uint32_t burrow__mldsa_field_from_montgomery(MldsaFieldElement a);
int32_t burrow__mldsa_field_centered_mod(MldsaFieldElement r);
uint32_t burrow__mldsa_field_infinity_norm(MldsaFieldElement r);
MldsaFieldElement burrow__mldsa_field_reduce_once(uint32_t a);
MldsaFieldElement burrow__mldsa_field_add(MldsaFieldElement a, MldsaFieldElement b);
MldsaFieldElement burrow__mldsa_field_sub(MldsaFieldElement a, MldsaFieldElement b);
MldsaFieldElement burrow__mldsa_field_montgomery_mul(MldsaFieldElement a,
                                                     MldsaFieldElement b);
MldsaFieldElement burrow__mldsa_field_montgomery_mul_sub(MldsaFieldElement a,
                                                         MldsaFieldElement b,
                                                         MldsaFieldElement c);
MldsaFieldElement burrow__mldsa_field_montgomery_add_mul(MldsaFieldElement a,
                                                         MldsaFieldElement b,
                                                         MldsaFieldElement c,
                                                         MldsaFieldElement d);

/* power2Round, with the high part in *hi and the low part in *lo. */
void burrow__mldsa_power2_round(MldsaFieldElement r, uint16_t *hi,
                                MldsaFieldElement *lo);

/* highBits, decompose and useHint for γ₂ = (q - 1) / 32 and (q - 1) / 88. */
uint8_t burrow__mldsa_high_bits32(uint32_t x);
uint8_t burrow__mldsa_high_bits88(uint32_t x);
uint8_t burrow__mldsa_decompose32(MldsaFieldElement r, int32_t *r0);
uint8_t burrow__mldsa_decompose88(MldsaFieldElement r, int32_t *r0);
uint8_t burrow__mldsa_use_hint32(MldsaFieldElement r, uint8_t hint);
uint8_t burrow__mldsa_use_hint88(MldsaFieldElement r, uint8_t hint);

/* constantTimeAbs. */
uint32_t burrow__mldsa_constant_time_abs(int32_t x);

/* pubKeySize and sigSize, worked out from the parameters as Go does, for the
 * test that they agree with the constants. */
Int burrow__mldsa_pub_key_size(const MldsaParameters *params);
Int burrow__mldsa_sig_size(const MldsaParameters *params);

/* testingOnlyRejectionReason: fn is called with "z", "r0", "ct0" or "h" each
 * time a signature is rejected and tried again. */
typedef struct MldsaRejectionHook {
    void (*fn)(void *env, Str reason);
    void *env;
} MldsaRejectionHook;

/* SignExternalMuDeterministic, with the rejections going to hook, which can be
 * NULL. A μ that is not 64 bytes is "mldsa: invalid message hash length". */
BURROW_OWNS(ret) Slice burrow__mldsa_sign_external_mu_deterministic(
    const MldsaPrivateKey *sk, Alloc *a, Slice mu, const MldsaRejectionHook *hook,
    Error *err);

/* TestingOnlySignWithRandom and TestingOnlySignExternalMuWithRandom: signing
 * with the 32 bytes of random in place of the system's. Any other length is
 * "mldsa: invalid random length". */
BURROW_OWNS(ret) Slice burrow__mldsa_testing_only_sign_with_random(
    const MldsaPrivateKey *sk, Alloc *a, Slice msg, Str context, Slice random,
    Error *err);
BURROW_OWNS(ret) Slice burrow__mldsa_testing_only_sign_external_mu_with_random(
    const MldsaPrivateKey *sk, Alloc *a, Slice mu, Slice random, Error *err);

/* VerifyExternalMu. */
BURROW_STATIC(ret) Error burrow__mldsa_verify_external_mu(const MldsaPublicKey *pk,
                                                          Slice mu, Slice sig);

/* TestingOnlyNewPrivateKeyFromSemiExpanded: a key from the semi-expanded
 * encoding of FIPS 204, from a, after checking that all of it agrees. Its seed
 * is random, so mldsa_private_key_bytes of it is not the seed of anything. */
BURROW_OWNS(ret) MldsaPrivateKey *
burrow__mldsa_testing_only_new_private_key_from_semi_expanded(Alloc *a, Slice sk,
                                                              Error *err);

/* TestingOnlyPrivateKeySemiExpandedBytes: sk in that encoding, from a. A nil
 * Slice when a is out of memory. */
BURROW_OWNS(ret) Slice burrow__mldsa_testing_only_private_key_semi_expanded_bytes(
    const MldsaPrivateKey *sk, Alloc *a);

/* fips140CAST: the known answer test Go runs before its first use of ML-DSA in
 * FIPS mode, with the rejections going to hook, which can be NULL. */
BURROW_STATIC(ret) Error burrow__mldsa_fips140_cast(const MldsaRejectionHook *hook);

/* Go's zero PrivateKey and PublicKey, which a program cannot get at here, for
 * the tests of what the functions do with them. NULL when a is out of memory.
 * Free them with mldsa_private_key_free and mldsa_public_key_free. */
BURROW_OWNS(ret) MldsaPrivateKey *burrow__mldsa_new_zero_private_key(Alloc *a);
BURROW_OWNS(ret) MldsaPublicKey *burrow__mldsa_new_zero_public_key(Alloc *a);

/* Whether a and b are the same in every field, Go's *a == *b. */
bool burrow__mldsa_public_key_identical(const MldsaPublicKey *a,
                                        const MldsaPublicKey *b);

#endif /* BURROW_SRC_CRYPTO_MLDSA_INTERNAL_H */
