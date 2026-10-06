/* What rand.c has inside, for tests/crypto_rand_test.c and for GCM's random
 * nonces.
 *
 * Copyright 2026 The burrow Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style licence that can be found
 * in the LICENSE file. */

#ifndef BURROW_SRC_CRYPTO_RAND_INTERNAL_H
#define BURROW_SRC_CRYPTO_RAND_INTERNAL_H

#include "burrow/crypto/rand.h"

#include "burrow/core.h"
#include "burrow/io.h"

/* crypto/internal/rand.IsDefaultReader: whether r is the system generator
 * crypto_rand_reader starts out as. */
bool burrow__crypto_rand_is_default_reader(IoReader r);

/* The system generator, never crypto_rand_reader, which is where Go's
 * crypto/internal/fips140/drbg.Read gets its bytes outside FIPS mode. It fills
 * p or the process ends. */
void burrow__crypto_rand_system(Slice p);

/* Reads the GODEBUG settings from value as if it were the environment's, or
 * forgets them when value is NULL so that the next use reads the environment
 * again. For tests, which cannot change the environment of a process that has
 * already looked at it. */
void burrow__crypto_rand_godebug_set(const char *value);

#endif /* BURROW_SRC_CRYPTO_RAND_INTERNAL_H */
