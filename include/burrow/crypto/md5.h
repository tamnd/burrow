/* crypto/md5, the MD5 hash from RFC 1321.
 *
 * MD5 is broken as a cryptographic hash: collisions can be made on a laptop.
 * It is here because file formats and protocols still name it, and it is fine
 * as a checksum against accidents. For anything an attacker can influence, use
 * crypto/sha256.
 *
 *     Md5SumRet s = md5_sum(data);          // s.a is the 16 byte digest
 *
 *     Hash h = md5_new(a);
 *     hash_write(h, part1, NULL);
 *     hash_write(h, part2, NULL);
 *     Slice sum = hash_sum(a, h, slice_nil(TYPE_BYTE));
 *
 * Go returns the one shot sum as a [16]byte, which is a value. C arrays do not
 * assign, so it comes back wrapped in a struct that does.
 *
 * Copyright 2009 The Go Authors. All rights reserved.
 * Copyright 2026 The burrow Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style licence that can be found
 * in the LICENSE file. */

/* burrow:package crypto/md5 */

#ifndef BURROW_CRYPTO_MD5_H
#define BURROW_CRYPTO_MD5_H

#include "burrow/core.h"
#include "burrow/hash.h"
#include "burrow/mem.h"
#include "burrow/slice.h"

#ifdef __cplusplus
extern "C" {
#endif

/* The size of an MD5 checksum in bytes, and the block size in bytes. */
#define MD5_SIZE 16
#define MD5_BLOCK_SIZE 64

typedef struct Md5SumRet {
    Byte a[MD5_SIZE];
} Md5SumRet;

/* A new MD5 hash, allocated from a. A nil Hash if a is out of memory. */
BURROW_OWNS(ret) Hash md5_new(Alloc *a);

/* The MD5 checksum of data, a Slice of Byte. */
Md5SumRet md5_sum(Slice data);

#ifdef __cplusplus
}
#endif

#endif /* BURROW_CRYPTO_MD5_H */
