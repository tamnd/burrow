/* The parts of crypto/rsa that its tests look at, from Go's
 * crypto/internal/fips140/rsa: the Miller-Rabin test and the totient that key
 * generation uses, the PSS encoding, the DER prefixes of the hashes, and
 * nonZeroRandomBytes, which Go exports to its tests in rsa_export_test.go.
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

/* Reads the GODEBUG settings from value as if it were the environment's, or
 * from the environment again when value is NULL. */
void burrow__rsa_godebug_set(const char *value);

#endif /* BURROW_SRC_CRYPTO_RSA_INTERNAL_H */
