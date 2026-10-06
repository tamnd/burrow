/* What tests/des_test.c needs from inside crypto/des, which Go's
 * internal_test.go gets by being in the package.
 *
 * Copyright 2026 The burrow Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style licence that can be found
 * in the LICENSE file. */

#ifndef BURROW_CRYPTO_DES_INTERNAL_H
#define BURROW_CRYPTO_DES_INTERNAL_H

#include "burrow/crypto/des.h"

#include <stdint.h>

/* Go's permuteInitialBlock and permuteFinalBlock. */
uint64_t burrow__des_permute_initial_block(uint64_t block);
uint64_t burrow__des_permute_final_block(uint64_t block);

/* Go's feistelBox, which Go fills in the first time a key is set up and which
 * is a constant table here. */
extern const uint32_t burrow__des_feistel_box[8][64];

#endif /* BURROW_CRYPTO_DES_INTERNAL_H */
