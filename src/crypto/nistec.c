/* Derived from Go's src/crypto/internal/fips140/subtle/constant_time.go.
 * Go source: go1.27.1.
 *
 * The two byte comparisons the four nistec_pNNN.c files share. crypto/subtle
 * has the equality one for slices, but not the ordering one, which Go keeps in
 * its FIPS module for the element decoders.
 *
 * Copyright 2022 The Go Authors. All rights reserved.
 * Copyright 2026 The burrow Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style licence that can be found
 * in the LICENSE file. */

#include "nistec.h"

#include "burrow/core.h"

#include <stdint.h>

int burrow__nistec_less_or_eq_bytes(const uint8_t *x, const uint8_t *y, Int n) {
    /* A constant time subtraction chain y - x, a byte at a time from the least
     * significant end. If there is no borrow at the end, x <= y. Go does this
     * 64 bits at a time, which gives the same borrow. */
    uint32_t b = 0;
    for (Int i = n - 1; i >= 0; i--) {
        uint32_t d = (uint32_t)y[i] - (uint32_t)x[i] - b;
        b = (d >> 8) & 1;
    }
    return (int)(b ^ 1);
}

int burrow__nistec_equal_bytes(const uint8_t *x, const uint8_t *y, Int n) {
    uint32_t v = 0;
    for (Int i = 0; i < n; i++)
        v |= (uint32_t)(x[i] ^ y[i]);
    return (int)((v - 1) >> 31);
}
