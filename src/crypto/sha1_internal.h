/* What sha1.c has for crypto/tls and nothing else.
 *
 * Copyright 2026 The burrow Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style licence that can be found
 * in the LICENSE file. */

#ifndef BURROW_SRC_CRYPTO_SHA1_INTERNAL_H
#define BURROW_SRC_CRYPTO_SHA1_INTERNAL_H

#include "burrow/hash.h"
#include "burrow/mem.h"

/* A SHA-1 whose sum takes the same time however full its last block is, the
 * one Go's sha1.digest has as ConstantTimeSum. crypto/tls puts it behind the
 * HMAC of its CBC cipher suites. A nil Hash when a is out of memory. */
Hash burrow__sha1_new_constant_time(Alloc *a);

#endif /* BURROW_SRC_CRYPTO_SHA1_INTERNAL_H */
