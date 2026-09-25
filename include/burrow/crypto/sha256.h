/* crypto/sha256, the SHA-224 and SHA-256 hashes from FIPS 180-4.
 *
 *     Sha256Sum256Ret s = sha256_sum256(data);  // s.a is the 32 byte digest
 *
 *     Hash h = sha256_new(a);
 *     hash_write(h, part1, NULL);
 *     hash_write(h, part2, NULL);
 *     Slice sum = hash_sum(a, h, slice_nil(TYPE_BYTE));
 *
 * Go returns the one shot sums as arrays, which are values. C arrays do not
 * assign, so they come back wrapped in structs that do.
 *
 * Copyright 2009 The Go Authors. All rights reserved.
 * Copyright 2026 The burrow Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style licence that can be found
 * in the LICENSE file. */

/* burrow:package crypto/sha256 */

#ifndef BURROW_CRYPTO_SHA256_H
#define BURROW_CRYPTO_SHA256_H

#include "burrow/core.h"
#include "burrow/hash.h"
#include "burrow/mem.h"
#include "burrow/slice.h"

#ifdef __cplusplus
extern "C" {
#endif

/* The sizes of a SHA-256 and a SHA-224 checksum in bytes, and the block size
 * of both in bytes. */
#define SHA256_SIZE 32
#define SHA256_SIZE224 28
#define SHA256_BLOCK_SIZE 64

typedef struct Sha256Sum256Ret {
    Byte a[SHA256_SIZE];
} Sha256Sum256Ret;

typedef struct Sha256Sum224Ret {
    Byte a[SHA256_SIZE224];
} Sha256Sum224Ret;

/* A new SHA-256 or SHA-224 hash, allocated from a. A nil Hash if a is out of
 * memory. */
BURROW_OWNS(ret) Hash sha256_new(Alloc *a);
BURROW_OWNS(ret) Hash sha256_new224(Alloc *a);

/* The SHA-256 and SHA-224 checksums of data, a Slice of Byte. */
Sha256Sum256Ret sha256_sum256(Slice data);
Sha256Sum224Ret sha256_sum224(Slice data);

#ifdef __cplusplus
}
#endif

#endif /* BURROW_CRYPTO_SHA256_H */
