/* crypto/pbkdf2, the password based key derivation function PBKDF2 from
 * RFC 8018.
 *
 *     Error err;
 *     Slice key = pbkdf2_key(a, sha256_new, password, salt, 4096, 32, &err);
 *
 * A password is short and people pick guessable ones, so a key made from it
 * has to be slow to make, and PBKDF2 does that by running HMAC over and over.
 * The more iterations the slower each guess is for an attacker and for you.
 * The salt should be at least 8 random bytes, which is what the RFC
 * recommends, different for every password and stored next to whatever the
 * key protects.
 *
 * The key is a Slice of Byte allocated from a. On an error it is nil and *err,
 * which may be NULL, says why. Go's FIPS 140-only mode is not here, so the
 * errors it adds never come back.
 *
 * Copyright 2012 The Go Authors. All rights reserved.
 * Copyright 2026 The burrow Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style licence that can be found
 * in the LICENSE file. */

/* burrow:package crypto/pbkdf2 */

#ifndef BURROW_CRYPTO_PBKDF2_H
#define BURROW_CRYPTO_PBKDF2_H

#include "burrow/core.h"
#include "burrow/error.h"
#include "burrow/hash.h"
#include "burrow/mem.h"
#include "burrow/slice.h"

#ifdef __cplusplus
extern "C" {
#endif

/* key_length bytes of key from password and salt, a Slice of Byte, with iter
 * rounds of HMAC using the hash h makes. An iter under 1 counts as 1. Fails if
 * key_length is not positive or is more blocks than the RFC allows. */
BURROW_OWNS(ret) Slice pbkdf2_key(Alloc *a, HashNewFunc h, Str password, Slice salt,
                                  Int iter, Int key_length, Error *err);

#ifdef __cplusplus
}
#endif

#endif /* BURROW_CRYPTO_PBKDF2_H */
