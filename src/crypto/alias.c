/* Derived from Go's src/crypto/internal/fips140/alias/alias.go.
 * Go source: go1.27.1.
 *
 * Copyright 2018 The Go Authors. All rights reserved.
 * Copyright 2026 The burrow Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style licence that can be found
 * in the LICENSE file. */

#include "alias_internal.h"

#include "burrow/core.h"
#include "burrow/slice.h"

#include "fips140_module.h"

#include <stdint.h>

/* Not in Go: the addresses are compared as integers, as Go compares them as
 * uintptrs, since comparing pointers into two different arrays is undefined in
 * C. */
CRYPTO_FIPS_TEXT bool burrow__alias_any_overlap(Slice x, Slice y) {
    if (x.len <= 0 || y.len <= 0)
        return false;
    uintptr_t xp = (uintptr_t)x.p;
    uintptr_t yp = (uintptr_t)y.p;
    return xp <= yp + (uintptr_t)(y.len - 1) && yp <= xp + (uintptr_t)(x.len - 1);
}

CRYPTO_FIPS_TEXT bool burrow__alias_inexact_overlap(Slice x, Slice y) {
    if (x.len == 0 || y.len == 0 || x.p == y.p)
        return false;
    return burrow__alias_any_overlap(x, y);
}
