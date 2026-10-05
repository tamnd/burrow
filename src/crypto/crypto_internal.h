/* What crypto.c has inside, for the packages that take a CryptoHash and need
 * the function that makes it rather than one Hash.
 *
 * Copyright 2026 The burrow Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style licence that can be found
 * in the LICENSE file. */

#ifndef BURROW_SRC_CRYPTO_CRYPTO_INTERNAL_H
#define BURROW_SRC_CRYPTO_CRYPTO_INTERNAL_H

#include "burrow/crypto.h"

#include "burrow/hash.h"

/* What crypto_hash_new calls for h: the function registered for it, or
 * burrow's own, or NULL when h is not available. Go's crypto/ecdsa gets the
 * same thing from Hash.New through crypto/internal/fips140hash.UnwrapNew. */
HashNewFunc burrow__crypto_hash_new_func(CryptoHash h);

#endif /* BURROW_SRC_CRYPTO_CRYPTO_INTERNAL_H */
