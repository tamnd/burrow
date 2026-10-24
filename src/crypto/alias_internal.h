/* Go's crypto/internal/fips140/alias, from alias.c.
 *
 * Copyright 2026 The burrow Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style licence that can be found
 * in the LICENSE file. */

#ifndef BURROW_SRC_CRYPTO_ALIAS_INTERNAL_H
#define BURROW_SRC_CRYPTO_ALIAS_INTERNAL_H

#include "burrow/crypto/fips140.h"

#include "burrow/core.h"
#include "burrow/slice.h"

#include <stdbool.h>

/* Package alias implements memory aliasing tests.
 * This code also exists as golang.org/x/crypto/internal/alias. */

/* AnyOverlap reports whether x and y share memory at any (not necessarily
 * corresponding) index. The memory beyond the slice length is ignored. */
bool burrow__alias_any_overlap(Slice x, Slice y);

/* InexactOverlap reports whether x and y share memory at any non-corresponding
 * index. The memory beyond the slice length is ignored. Note that x and y can
 * have different lengths and still not have any inexact overlap.
 *
 * InexactOverlap can be used to implement the requirements of the crypto/cipher
 * AEAD, Block, BlockMode and Stream interfaces. */
bool burrow__alias_inexact_overlap(Slice x, Slice y);

#endif
