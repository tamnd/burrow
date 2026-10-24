/* The parts of crypto/rsa that its tests look at, from Go's
 * crypto/internal/fips140/rsa: the Miller-Rabin test and the totient that key
 * generation uses, the PSS encoding, the DER prefixes of the hashes, and
 * nonZeroRandomBytes, which Go exports to its tests in rsa_export_test.go.
 * Then the module's own keys and the functions on them, which the ACVP tests
 * call the way Go's acvp_test.go does.
 *
 * Copyright 2024 The Go Authors. All rights reserved.
 * Copyright 2026 The burrow Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style licence that can be found
 * in the LICENSE file. */

#ifndef BURROW_SRC_CRYPTO_RSA_INTERNAL_H
#define BURROW_SRC_CRYPTO_RSA_INTERNAL_H

/* rsa.h comes first so that the amalgamation files this header with package
 * crypto/rsa. */
#include "burrow/crypto/rsa.h"

#include "burrow/core.h"
#include "burrow/error.h"
#include "burrow/hash.h"
#include "burrow/io.h"
#include "burrow/mem.h"
#include "burrow/slice.h"

#include <stdbool.h>
#include <stddef.h>

#include "bigmod.h"

/* millerRabin: a candidate w, with the m that w - 1 = 2 * m. */
typedef struct RsaMillerRabin RsaMillerRabin;

/* millerRabinSetup: w set up for the test, from a. A w that is not 3 mod 4 is
 * "candidate is not 3 mod 4". */
BURROW_OWNS(ret) RsaMillerRabin *burrow__rsa_miller_rabin_setup(Alloc *a, Slice w,
                                                                Error *err);

/* millerRabinIteration: one round with the base b, which has to be as long as
 * w. true in *possibly_prime is millerRabinPOSSIBLYPRIME. */
BURROW_STATIC(ret) Error burrow__rsa_miller_rabin_iteration(const RsaMillerRabin *mr,
                                                            Slice b,
                                                            bool *possibly_prime);

/* errDivisorTooLarge: what totient gives for a gcd(p-1, q-1) of more than 32
 * bits, after which GenerateKey tries other primes. */
extern const Error burrow__rsa_err_divisor_too_large;

/* totient: λ(N) = lcm(p-1, q-1) for the primes p and q, big endian, as long as
 * a modulus of that value is, from a. */
BURROW_OWNS(ret) Slice burrow__rsa_totient(Alloc *a, Slice p, Slice q, Error *err);

/* emsaPSSEncode: the PSS encoding of m_hash with salt, em_bits long, from a. */
BURROW_OWNS(ret) Slice burrow__rsa_emsa_pss_encode(Alloc *a, Slice m_hash, Int em_bits,
                                                   Slice salt, Hash hash, Error *err);

/* emsaPSSVerify: no error when em is the PSS encoding of m_hash with a salt
 * of s_len bytes, or of any length for a s_len of -1. em is not changed. */
BURROW_STATIC(ret) Error burrow__rsa_emsa_pss_verify(Slice m_hash, Slice em,
                                                     Int em_bits, Int s_len, Hash hash);

/* hashPrefixes: the DER prefix of a PKCS #1 v1.5 signature with the hash
 * named name, as crypto_hash_string names it, and false for a name with
 * none. */
bool burrow__rsa_hash_prefix(Str name, Slice *prefix);

/* nonZeroRandomBytes: fills s with random bytes from random, none of them
 * zero. */
BURROW_STATIC(ret) Error burrow__rsa_non_zero_random_bytes(Slice s, IoReader random);

/* Not in Go: hands each secret part of priv's precomputed key to mark, the
 * private exponent, the primes and the values worked out from them, so that
 * tools/ctgrind.c can mark them undefined. false when priv has no precomputed
 * key, which rsa_generate_key and rsa_private_key_precompute make. */
bool burrow__rsa_mark_secrets(const RsaPrivateKey *priv,
                              void (*mark)(void *p, size_t n));

/* Reads the GODEBUG settings from value as if it were the environment's, or
 * from the environment again when value is NULL. */
void burrow__rsa_godebug_set(const char *value);

/* What PrivateKey.Export gives, from the Alloc it is asked to use. */
typedef struct RsaFipsExport {
    Slice n;
    Int e;
    Slice d, p, q, dp, dq, qinv;
    bool crt;
} RsaFipsExport;

/* GenerateKey generates a new RSA key pair of the given bit size. bits must
 * be at least 32. */
BURROW_OWNS(ret) RsaFipsKey *burrow__rsa_fips_generate_key(Alloc *a, IoReader rand,
                                                           Int bits, Error *err);

/* NewPrivateKey: the key with modulus N, public exponent e, private exponent
 * d and primes P and Q, from a, once it has been checked. tmp is for the
 * check. */
BURROW_OWNS(ret) RsaFipsKey *burrow__rsa_fips_new_private_key(Alloc *a, Alloc *tmp,
                                                              Slice N, Int e, Slice d,
                                                              Slice P, Slice Q,
                                                              Error *err);

/* Frees k, which may be NULL, wiping its secrets first. */
void burrow__rsa_fips_key_free(RsaFipsKey *k);

/* PrivateKey.Export, from tmp. */
RsaFipsExport burrow__rsa_fips_export(const RsaFipsKey *k, Alloc *tmp);

/* SignPKCS1v15 and VerifyPKCS1v15. hash is the name of the hash, as
 * crypto_hash_string gives it, or empty when the message is signed directly.
 * The signature is from tmp, and the public key is N and e. */
bool burrow__rsa_fips_sign_pkcs1_v15(const RsaFipsKey *k, Str hash, Slice hashed,
                                     Alloc *tmp, Slice *out, Error *err);
BURROW_STATIC(ret) Error burrow__rsa_fips_verify_pkcs1_v15(const BigmodModulus *N,
                                                           Int e, Str hash,
                                                           Slice hashed, Slice sig,
                                                           Alloc *tmp);

/* SignPSS, with a salt of salt_length bytes, and VerifyPSS, which works out
 * the length of the salt. The signature is from tmp. */
bool burrow__rsa_fips_sign_pss(IoReader rand, const RsaFipsKey *k, Hash hash,
                               Slice hashed, Int salt_length, Alloc *tmp, Slice *out,
                               Error *err);
BURROW_STATIC(ret) Error burrow__rsa_fips_verify_pss(const BigmodModulus *N, Int e,
                                                     Hash hash, Slice digest, Slice sig,
                                                     Alloc *tmp);

/* EncryptOAEP and DecryptOAEP, with no label for an empty one. What they give
 * is from tmp. */
bool burrow__rsa_fips_encrypt_oaep(Hash hash, Hash mgf_hash, IoReader random,
                                   const BigmodModulus *N, Int e, Slice msg,
                                   Slice label, Alloc *tmp, Slice *out, Error *err);
bool burrow__rsa_fips_decrypt_oaep(Hash hash, Hash mgf_hash, const RsaFipsKey *priv,
                                   Slice ciphertext, Slice label, Alloc *tmp,
                                   Slice *out, Error *err);

/* NewPrivateKeyWithPrecomputation: as burrow__rsa_fips_new_private_key, with
 * the CRT values dP, dQ and qInv given rather than worked out. */
BURROW_OWNS(ret) RsaFipsKey *burrow__rsa_fips_new_private_key_with_precomputation(
    Alloc *a, Alloc *tmp, Slice N, Int e, Slice d, Slice P, Slice Q, Slice dP, Slice dQ,
    Slice qInv, Error *err);

/* TestingOnlyLargeExponentPublicKey is a variant of [PublicKey] that supports
 * large public exponents. It is only meant for supporting the full ACVP test
 * suite, which unfortunately forces us to choose between a fixed exponent and
 * the full (2¹⁶, 2²⁵⁶) range. This type must not be used in production code,
 * nor for e values < 2³¹, which instead must use [PublicKey]. */
typedef struct RsaFipsLargeExponentPublicKey {
    const BigmodModulus *n;
    /* E is the public exponent, represented as a big-endian byte slice. */
    Slice e;
} RsaFipsLargeExponentPublicKey;

/* TestingOnlyLargeExponentPrivateKey is a variant of [PrivateKey] that
 * supports large public exponents. It is only meant for supporting the full
 * ACVP test suite. This type must not be used in production code. */
typedef struct RsaFipsLargeExponentKey RsaFipsLargeExponentKey;

/* TestingOnlyLargeExponentVerifyPKCS1v15 and TestingOnlyLargeExponentVerifyPSS,
 * which use tmp. */
BURROW_STATIC(ret) Error burrow__rsa_fips_testing_only_large_exponent_verify_pkcs1_v15(
    const RsaFipsLargeExponentPublicKey *pub, Str hash, Slice hashed, Slice sig,
    Alloc *tmp);
BURROW_STATIC(ret) Error burrow__rsa_fips_testing_only_large_exponent_verify_pss(
    const RsaFipsLargeExponentPublicKey *pub, Hash hash, Slice digest, Slice sig,
    Alloc *tmp);

/* TestingOnlyNewLargeExponentPrivateKeyWithPrecomputation creates a new RSA
 * private key with a large public exponent from the given parameters. It is
 * only meant for ACVP testing. The key is from a, and tmp is for the check. */
BURROW_OWNS(ret) RsaFipsLargeExponentKey *
burrow__rsa_fips_testing_only_new_large_exponent_private_key_with_precomputation(
    Alloc *a, Alloc *tmp, Slice N, Slice e, Slice d, Slice P, Slice Q, Slice dP,
    Slice dQ, Slice qInv, Error *err);

/* Frees k, which may be NULL, wiping its secrets first. */
void burrow__rsa_fips_large_exponent_key_free(RsaFipsLargeExponentKey *k);

/* TestingOnlyLargeExponentDecryptOAEP and TestingOnlyLargeExponentEncryptOAEP.
 * What they give is from tmp. */
bool burrow__rsa_fips_testing_only_large_exponent_decrypt_oaep(
    Hash hash, Hash mgf_hash, const RsaFipsLargeExponentKey *priv, Slice ciphertext,
    Slice label, Alloc *tmp, Slice *out, Error *err);
bool burrow__rsa_fips_testing_only_large_exponent_encrypt_oaep(
    Hash hash, Hash mgf_hash, IoReader random, const RsaFipsLargeExponentPublicKey *pub,
    Slice msg, Slice label, Alloc *tmp, Slice *out, Error *err);

#endif /* BURROW_SRC_CRYPTO_RSA_INTERNAL_H */
