/* crypto/sha512, the SHA-384, SHA-512, SHA-512/224 and SHA-512/256 hashes
 * from FIPS 180-4.
 *
 * All four run the same 64 bit compression function and differ only in where
 * they start and how much of the result they keep. On a 64 bit machine
 * SHA-512/256 is often faster than SHA-256 for long input.
 *
 *     Sha512Sum512Ret s = sha512_sum512(data);  // s.a is the 64 byte digest
 *     Hash h = sha512_new384(a);                // to feed SHA-384 in pieces
 *
 * Go returns the one shot sums as arrays, which are values. C arrays do not
 * assign, so they come back wrapped in structs that do.
 *
 * Copyright 2009 The Go Authors. All rights reserved.
 * Copyright 2026 The burrow Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style licence that can be found
 * in the LICENSE file. */

/* burrow:package crypto/sha512 */

#ifndef BURROW_CRYPTO_SHA512_H
#define BURROW_CRYPTO_SHA512_H

#include "burrow/core.h"
#include "burrow/hash.h"
#include "burrow/mem.h"
#include "burrow/slice.h"

#ifdef __cplusplus
extern "C" {
#endif

/* The size of each checksum in bytes, and the block size of all of them in
 * bytes. */
#define SHA512_SIZE 64
#define SHA512_SIZE224 28
#define SHA512_SIZE256 32
#define SHA512_SIZE384 48
#define SHA512_BLOCK_SIZE 128

typedef struct Sha512Sum512Ret {
    Byte a[SHA512_SIZE];
} Sha512Sum512Ret;

typedef struct Sha512Sum384Ret {
    Byte a[SHA512_SIZE384];
} Sha512Sum384Ret;

typedef struct Sha512Sum512224Ret {
    Byte a[SHA512_SIZE224];
} Sha512Sum512224Ret;

typedef struct Sha512Sum512256Ret {
    Byte a[SHA512_SIZE256];
} Sha512Sum512256Ret;

/* A new hash of each kind, allocated from a. A nil Hash if a is out of
 * memory. */
BURROW_OWNS(ret) Hash sha512_new(Alloc *a);
BURROW_OWNS(ret) Hash sha512_new384(Alloc *a);
BURROW_OWNS(ret) Hash sha512_new512224(Alloc *a);
BURROW_OWNS(ret) Hash sha512_new512256(Alloc *a);

/* The checksums of data, a Slice of Byte. */
Sha512Sum512Ret sha512_sum512(Slice data);
Sha512Sum384Ret sha512_sum384(Slice data);
Sha512Sum512224Ret sha512_sum512224(Slice data);
Sha512Sum512256Ret sha512_sum512256(Slice data);

#ifdef __cplusplus
}
#endif

#endif /* BURROW_CRYPTO_SHA512_H */
