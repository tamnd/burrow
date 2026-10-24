/* What ecdh.c has inside, for tests/fips140_acvp_test.c: the parts of Go's
 * crypto/internal/fips140/ecdh that crypto/internal/fips140test reaches,
 * which Go can because they are in the same module.
 *
 * Copyright 2024 The Go Authors. All rights reserved.
 * Copyright 2026 The burrow Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style licence that can be found
 * in the LICENSE file. */

#ifndef BURROW_SRC_CRYPTO_ECDH_INTERNAL_H
#define BURROW_SRC_CRYPTO_ECDH_INTERNAL_H

/* ecdh.h comes first so that the amalgamation files this header with
 * crypto/ecdh. */
#include "burrow/crypto/ecdh.h"

/* ecdh.P224. crypto/ecdh has no P-224, but the module's ECDH does, and the
 * ACVP tests use it. */
const EcdhCurve *burrow__ecdh_p224(void);

#endif
