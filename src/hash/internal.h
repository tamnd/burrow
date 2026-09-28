/* What the hash files share with the rest of the library and nobody else:
 * the Adler-32 update that compress/zlib keeps a running checksum with,
 * without allocating a Hash32 for it.
 *
 * Copyright 2026 The burrow Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style licence that can be found
 * in the LICENSE file. */

#ifndef BURROW_SRC_HASH_INTERNAL_H
#define BURROW_SRC_HASH_INTERNAL_H

#include "burrow/hash/adler32.h"

#include "burrow/core.h"

#include <stdint.h>

/* Adds p[0:n] to the running Adler-32 checksum d. A fresh checksum is 1. */
uint32_t burrow__adler32_update(uint32_t d, const Byte *p, Int n);

#endif /* BURROW_SRC_HASH_INTERNAL_H */
