/* crypto/hmac, the keyed message authentication code from FIPS 198-1.
 *
 *     Hash mac = hmac_new(a, sha256_new, key);
 *     hash_write(mac, message, NULL);
 *     Slice expected = hash_sum(a, mac, slice_nil(TYPE_BYTE));
 *     if (!hmac_equal(received_mac, expected))
 *         return errors_new(a, BURROW_S("bad MAC"));
 *
 * Both sides know the key. The sender works out the MAC of the message and
 * sends it along, and the receiver works it out again and compares. Compare
 * with hmac_equal, never with memcmp: memcmp stops at the first byte that
 * differs, and how long it took tells an attacker how much of a forged MAC was
 * right.
 *
 * What hmac_new returns is a Hash like any other, so it writes, sums, resets
 * and reports its sizes through the functions in burrow/hash.h.
 *
 * Go's HMAC is also a hash.Cloner when the hash under it is one. A Hash here
 * carries no way to ask whether it is a Cloner, so this one is not, and it does
 * not save the padded key states the way Go's does once hash marshaling is in.
 * Go's FIPS 140-only mode is not here either, so the short key and the
 * unapproved hash panics never happen.
 *
 * Copyright 2009 The Go Authors. All rights reserved.
 * Copyright 2026 The burrow Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style licence that can be found
 * in the LICENSE file. */

/* burrow:package crypto/hmac */

#ifndef BURROW_CRYPTO_HMAC_H
#define BURROW_CRYPTO_HMAC_H

#include "burrow/core.h"
#include "burrow/hash.h"
#include "burrow/mem.h"
#include "burrow/slice.h"

#ifdef __cplusplus
extern "C" {
#endif

/* A new HMAC using the hash h makes, keyed with key, a Slice of Byte, all
 * allocated from a. h is called twice, for the inner and the outer hash, and
 * has to give a different hash each time; one that hands back the same hash
 * panics. A key longer than the hash's block size is hashed first, which is
 * what the standard says to do. The key is copied, so it can change or go away
 * after this returns. A nil Hash if a is out of memory. */
BURROW_OWNS(ret) Hash hmac_new(Alloc *a, HashNewFunc h, Slice key);

/* Whether mac1 and mac2, Slices of Byte, hold the same bytes, in a time that
 * depends on their lengths and not on what is in them. A length is not secret
 * and comparing two of different lengths says false straight away. */
bool hmac_equal(Slice mac1, Slice mac2);

#ifdef __cplusplus
}
#endif

#endif /* BURROW_CRYPTO_HMAC_H */
