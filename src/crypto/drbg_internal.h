/* crypto/internal/fips140/drbg's CTR_DRBG, from drbg.c.
 *
 * Copyright 2026 The burrow Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style licence that can be found
 * in the LICENSE file. */

#ifndef BURROW_SRC_CRYPTO_DRBG_INTERNAL_H
#define BURROW_SRC_CRYPTO_DRBG_INTERNAL_H

#include "burrow/crypto/aes.h"

#include "burrow/core.h"
#include "burrow/io.h"

#include "aes_internal.h"

#include <stdbool.h>
#include <stdint.h>

/* SeedSize: the key and one block. */
#define DRBG_SEED_SIZE (32 + AES_BLOCK_SIZE)

/* Counter is an SP 800-90A Rev. 1 CTR_DRBG instantiated with AES-256.
 *
 * Per Table 3, it has a security strength of 256 bits, a seed size of 384 bits,
 * a counter length of 128 bits, a reseed interval of 2^48 requests, and a
 * maximum request size of 2^19 bits (2^16 bytes, 64 KiB).
 *
 * We support a narrow range of parameters that fit the needs of our RNG:
 * AES-256, no derivation function, no personalization string, no prediction
 * resistance, and 384-bit additional input.
 *
 * WARNING: this type provides tightly scoped support for the DRBG
 * functionality we need for FIPS 140-3 _only_. This type _should not_ be used
 * outside of the FIPS 140-3 module for any other use.
 *
 * In particular, as documented, Counter does not support the derivation
 * function, or personalization strings which are necessary for safely using
 * this DRBG for generic purposes without leaking sensitive values. */
typedef struct DrbgCounter {
    /* Not in Go: Go keeps an aes.CTR. This is its key, and in v the counter
     * of the next block of key stream. */
    AesBlock b;
    Byte v[AES_BLOCK_SIZE];
    uint64_t reseed_counter;
} DrbgCounter;

/* NewCounter, into c. */
void burrow__drbg_new_counter(DrbgCounter *c, const Byte entropy[DRBG_SEED_SIZE]);

/* Counter.Reseed. */
void burrow__drbg_counter_reseed(DrbgCounter *c, const Byte entropy[DRBG_SEED_SIZE],
                                 const Byte additional_input[DRBG_SEED_SIZE]);

/* Generate produces at most maxRequestSize bytes of random data in out.
 * additional_input may be NULL, Go's nil. True when a reseed is required. */
bool burrow__drbg_counter_generate(DrbgCounter *c, Slice out,
                                   const Byte *additional_input);

/* Read fills b with cryptographically secure random bytes. In FIPS mode, it
 * uses an SP 800-90A Rev. 1 Deterministic Random Bit Generator (DRBG).
 * Otherwise, it uses the operating system's random number generator. */
void burrow__drbg_read(Slice b);

/* SetTestingReader sets a global, deterministic cryptographic randomness
 * source for testing purposes. Its Read method must never return an error, it
 * must never return short, and it must be safe for concurrent use.
 *
 * This is only intended to be used by the testing/cryptotest package.
 *
 * Not in Go: a zero r unsets it. */
void burrow__drbg_set_testing_reader(IoReader r);

#endif /* BURROW_SRC_CRYPTO_DRBG_INTERNAL_H */
