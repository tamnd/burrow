/* crypto/hkdf, the HMAC based key derivation function from RFC 5869.
 *
 *     Error err;
 *     Slice key = hkdf_key(a, sha256_new, secret, salt, BURROW_S("my app v1"), 32, &err);
 *
 * HKDF turns a secret that has enough entropy in it but not in a usable shape,
 * such as the output of a key exchange, into as many keys as you need. It
 * runs in two steps. Extract mixes the secret with an optional salt into a
 * pseudorandom key the size of the hash, and Expand stretches that into keys of
 * whatever length, with the info string telling one key from another. hkdf_key
 * does both.
 *
 * HKDF is not for passwords: a password does not have the entropy it assumes,
 * and crypto/pbkdf2 is the one to use for those.
 *
 * The results are Slices of Byte allocated from a. On an error they are nil
 * and *err, which may be NULL, says why. Go's FIPS 140-only mode is not here,
 * so the errors it adds never come back.
 *
 * Copyright 2014 The Go Authors. All rights reserved.
 * Copyright 2026 The burrow Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style licence that can be found
 * in the LICENSE file. */

/* burrow:package crypto/hkdf */

#ifndef BURROW_CRYPTO_HKDF_H
#define BURROW_CRYPTO_HKDF_H

#include "burrow/core.h"
#include "burrow/error.h"
#include "burrow/hash.h"
#include "burrow/mem.h"
#include "burrow/slice.h"

#ifdef __cplusplus
extern "C" {
#endif

/* The pseudorandom key from secret and salt, both Slices of Byte, which is as
 * long as h's sum. An empty salt is a salt of zeros as long as the sum, which
 * is what RFC 5869 says. It never fails. */
BURROW_OWNS(ret) Slice hkdf_extract(Alloc *a, HashNewFunc h, Slice secret, Slice salt,
                                    Error *err);

/* key_length bytes of key from pseudorandom_key, which should come from
 * hkdf_extract or already be uniformly random, and info. Fails if key_length
 * is more than 255 times the size of h's sum, which is as far as the RFC goes. */
BURROW_OWNS(ret) Slice hkdf_expand(Alloc *a, HashNewFunc h, Slice pseudorandom_key,
                                   Str info, Int key_length, Error *err);

/* hkdf_extract and then hkdf_expand. */
BURROW_OWNS(ret) Slice hkdf_key(Alloc *a, HashNewFunc h, Slice secret, Slice salt,
                                Str info, Int key_length, Error *err);

#ifdef __cplusplus
}
#endif

#endif /* BURROW_CRYPTO_HKDF_H */
