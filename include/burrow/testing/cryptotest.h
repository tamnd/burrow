/* testing/cryptotest, a deterministic source of cryptographic randomness for
 * tests.
 *
 *     static void TestSign(TestingT *t) {
 *         cryptotest_set_global_random(t, 0);
 *         EcdsaPrivateKey *k = ecdsa_generate_key(a, elliptic_p256(), (IoReader){0},
 *                                                 &err);
 *         ...
 *     }
 *
 * The key comes out the same on every run, until the test ends and
 * crypto/rand goes back to the system's randomness.
 *
 * Copyright 2025 The Go Authors. All rights reserved.
 * Copyright 2026 The burrow Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style licence that can be found
 * in the LICENSE file. */

/* burrow:package testing/cryptotest */

#ifndef BURROW_TESTING_CRYPTOTEST_H
#define BURROW_TESTING_CRYPTOTEST_H

#include "burrow/testing.h"

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* cryptotest.SetGlobalRandom sets a global, deterministic cryptographic
 * randomness source for the duration of test t. It affects crypto/rand, and all
 * implicit sources of cryptographic randomness in the crypto/... packages.
 *
 * SetGlobalRandom may be called multiple times in the same test to reset the
 * random stream or change the seed.
 *
 * Because SetGlobalRandom affects the whole process, it cannot be used in
 * parallel tests or tests with parallel ancestors.
 *
 * Note that the way cryptographic algorithms use randomness is generally not
 * specified and may change over time. Thus, if a test expects a specific output
 * from a cryptographic function, it may fail in the future even if it uses
 * SetGlobalRandom. */
void cryptotest_set_global_random(TestingT *t, uint64_t seed);

#ifdef __cplusplus
}
#endif

#endif /* BURROW_TESTING_CRYPTOTEST_H */
