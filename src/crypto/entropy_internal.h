/* The CPU jitter-based SP 800-90B entropy source, Go's
 * crypto/internal/entropy/v1.0.0, for drbg.c and the tests.
 *
 * Copyright 2026 The burrow Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style licence that can be found
 * in the LICENSE file. */

#ifndef BURROW_SRC_CRYPTO_ENTROPY_INTERNAL_H
#define BURROW_SRC_CRYPTO_ENTROPY_INTERNAL_H

#include "burrow/core.h"
#include "burrow/error.h"
#include "burrow/slice.h"

#include <stdint.h>

/* Version returns the version of the entropy source.
 *
 * This is independent of the FIPS 140-3 module version, in order to reuse the
 * ESV certificate across module versions. */
const char *burrow__entropy_version(void);

/* ScratchBuffer is a large buffer that will be written to using atomics, to
 * generate noise from memory access timings. Its contents do not matter.
 *
 * Not in Go: it is words rather than bytes, so that the atomics are aligned. */
typedef struct EntropyScratchBuffer {
    uint32_t words[(1 << 25) / 4];
} EntropyScratchBuffer;

/* Seed returns a 384-bit seed with full entropy.
 *
 * memory is passed in to allow changing the allocation strategy without
 * modifying the frozen and certified entropy source in this package.
 *
 * Seed returns an error if the entropy source startup health tests fail, which
 * has a non-negligible chance of happening. */
Error burrow__entropy_seed(Byte seed[48], EntropyScratchBuffer *memory);

/* Samples starts a new entropy source, collects the requested number of
 * samples, conducts startup health tests, and returns the samples or an error
 * if the health tests fail.
 *
 * The health tests have a non-negligible chance of failing. */
Error burrow__entropy_samples(Slice samples, EntropyScratchBuffer *memory);

/* RepetitionCountTest implements the repetition count test from SP 800-90B
 * Section 4.4.1. It returns an error if any symbol is repeated C = 41 or more
 * times in a row. */
Error burrow__entropy_repetition_count_test(Slice samples);

/* AdaptiveProportionTest implements the adaptive proportion test from SP
 * 800-90B Section 4.4.2. It returns an error if any symbol appears C = 410 or
 * more times in the last W = 512 samples. */
Error burrow__entropy_adaptive_proportion_test(Slice samples);

/* SHA-384 of the 1024 samples, from the entropy source's own copy of SHA-384,
 * which keeps it apart from any change to the module's. */
void burrow__entropy_sha384(Byte digest[48], const Byte p[1024]);

/* The same for any length, for tests. */
void burrow__entropy_testing_only_sha384(Byte digest[48], Slice p);

#endif /* BURROW_SRC_CRYPTO_ENTROPY_INTERNAL_H */
