/* Go's crypto/internal/fips140only, what the packages outside the FIPS 140-3
 * module ask before they do what GODEBUG=fips140=only does not allow.
 *
 * Copyright 2026 The burrow Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style licence that can be found
 * in the LICENSE file. */

#ifndef BURROW_SRC_CRYPTO_FIPS140ONLY_INTERNAL_H
#define BURROW_SRC_CRYPTO_FIPS140ONLY_INTERNAL_H

#include "burrow/crypto/fips140.h"

#include "burrow/core.h"
#include "burrow/error.h"
#include "burrow/hash.h"
#include "burrow/io.h"
#include "burrow/mem/arena.h"

#include "fips140_module.h"
#include "rand_internal.h"

/* Enforced reports whether FIPS 140-only mode is enabled and enforced, in which
 * non-approved cryptography returns an error or panics. */
static inline bool burrow__fips140only_enforced(void) {
    return fips140_enforced();
}

static inline bool burrow__fips140only_approved_hash(Hash h) {
    return burrow__fips140_approved_hash(h);
}

/* Not in Go: ApprovedHash(h()), with the hash h makes freed after. */
static inline bool burrow__fips140only_approved_hash_func(HashNewFunc h) {
    Arena ar;
    arena_init(&ar, NULL, 0);
    bool ok = burrow__fips140_approved_hash(h(arena_allocator(&ar)));
    arena_free(&ar);
    return ok;
}

static inline bool burrow__fips140only_approved_random_reader(IoReader r) {
    return burrow__crypto_rand_is_default_reader(r);
}

/* Not in Go: a sentinel error private to the file, which is what Go's
 * errors.New in a function comes to when nothing compares an error with it. */
#define BURROW_FIPS140ONLY_ERROR(name, text)                                           \
    static const Str name##__text = {(const Byte *)("" text),                          \
                                     (Int)(sizeof(text) - 1)};                         \
    static const Error name = {&burrow_sentinel_error_vt, &name##__text}

#endif /* BURROW_SRC_CRYPTO_FIPS140ONLY_INTERNAL_H */
