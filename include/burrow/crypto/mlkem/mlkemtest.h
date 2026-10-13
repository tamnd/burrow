/* crypto/mlkem/mlkemtest, functions for testing ML-KEM.
 *
 *     Slice ciphertext;
 *     Slice key = mlkemtest_encapsulate768(ek, a, random, &ciphertext, &err);
 *
 * These are ML-KEM.Encaps_internal of FIPS 203: encapsulation with the 32
 * bytes of randomness given, so that a known answer test gets a known answer.
 * They are for that and nothing else. A shared key made with randomness that
 * is not secret and uniform is not a secret.
 *
 * Go refuses these in its FIPS 140-only mode. burrow has no such mode.
 *
 * Copyright 2025 The Go Authors. All rights reserved.
 * Copyright 2026 The burrow Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style licence that can be found
 * in the LICENSE file. */

/* burrow:package crypto/mlkem/mlkemtest */

#ifndef BURROW_CRYPTO_MLKEM_MLKEMTEST_H
#define BURROW_CRYPTO_MLKEM_MLKEMTEST_H

#include "burrow/core.h"
#include "burrow/crypto/mlkem.h"
#include "burrow/error.h"
#include "burrow/mem.h"
#include "burrow/slice.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Encapsulate768: the shared key, returned, and the ciphertext, in
 * *ciphertext, that ek makes with random as its randomness, both new from a.
 * random has to be 32 bytes, and any other length is "mlkemtest:
 * Encapsulate768: random must be 32 bytes", with nil Slices. */
BURROW_OWNS(ret) Slice mlkemtest_encapsulate768(const MlkemEncapsulationKey768 *ek,
                                                Alloc *a, Slice random,
                                                Slice *ciphertext, Error *err);

/* Encapsulate1024: the same for ML-KEM-1024. */
BURROW_OWNS(ret) Slice mlkemtest_encapsulate1024(const MlkemEncapsulationKey1024 *ek,
                                                 Alloc *a, Slice random,
                                                 Slice *ciphertext, Error *err);

#ifdef __cplusplus
}
#endif

#endif /* BURROW_CRYPTO_MLKEM_MLKEMTEST_H */
