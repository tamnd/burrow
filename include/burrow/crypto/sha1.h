/* crypto/sha1, the SHA-1 hash from RFC 3174.
 *
 * SHA-1 is broken as a cryptographic hash: two different documents with the
 * same SHA-1 were published in 2017. It is here because git, older
 * certificates and older protocols still name it. For anything new, use
 * crypto/sha256.
 *
 *     Sha1SumRet s = sha1_sum(data);        // s.a is the 20 byte digest
 *     Hash h = sha1_new(a);                 // to feed it in pieces
 *
 * Go returns the one shot sum as a [20]byte, which is a value. C arrays do not
 * assign, so it comes back wrapped in a struct that does.
 *
 * Copyright 2009 The Go Authors. All rights reserved.
 * Copyright 2026 The burrow Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style licence that can be found
 * in the LICENSE file. */

/* burrow:package crypto/sha1 */

#ifndef BURROW_CRYPTO_SHA1_H
#define BURROW_CRYPTO_SHA1_H

#include "burrow/core.h"
#include "burrow/hash.h"
#include "burrow/mem.h"
#include "burrow/slice.h"

#ifdef __cplusplus
extern "C" {
#endif

/* The size of a SHA-1 checksum in bytes, and the block size in bytes. */
#define SHA1_SIZE 20
#define SHA1_BLOCK_SIZE 64

typedef struct Sha1SumRet {
    Byte a[SHA1_SIZE];
} Sha1SumRet;

/* A new SHA-1 hash, allocated from a. A nil Hash if a is out of memory. */
BURROW_OWNS(ret) Hash sha1_new(Alloc *a);

/* The SHA-1 checksum of data, a Slice of Byte. */
Sha1SumRet sha1_sum(Slice data);

#ifdef __cplusplus
}
#endif

#endif /* BURROW_CRYPTO_SHA1_H */
