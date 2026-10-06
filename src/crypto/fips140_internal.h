/* What fips140.c has inside, for tests/fips140_test.c.
 *
 * Copyright 2026 The burrow Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style licence that can be found
 * in the LICENSE file. */

#ifndef BURROW_SRC_CRYPTO_FIPS140_INTERNAL_H
#define BURROW_SRC_CRYPTO_FIPS140_INTERNAL_H

#include "burrow/crypto/fips140.h"

/* Looks at the fips140 setting in godebug, a GODEBUG value such as
 * "fips140=on,x=1" or NULL for none, and panics the way fips140_enabled does
 * for one it does not accept. Returns for none, empty and off. For tests,
 * which cannot change the environment of a process that has already read it. */
void burrow__fips140_check(const char *godebug);

#endif /* BURROW_SRC_CRYPTO_FIPS140_INTERNAL_H */
