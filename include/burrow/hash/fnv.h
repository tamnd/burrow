/* hash/fnv, the Fowler-Noll-Vo hashes.
 *
 * FNV is a family of small, fast, non-cryptographic hashes, good for hash
 * tables and for spreading keys across buckets. It comes in 32, 64 and 128 bit
 * sizes, and each size in two variants. FNV-1 multiplies and then mixes in the
 * byte. FNV-1a mixes in the byte and then multiplies, which spreads short keys
 * better and is the one to use unless something asks for plain FNV-1.
 *
 *     HashHash64 h = fnv_new64a(a);
 *     hash_write(hash_hash64_as_hash(h), key, NULL);
 *     uint64_t bucket = hash_hash64_sum64(h) % nbuckets;
 *
 * Every hash is allocated from a and belongs to the caller.
 *
 * Copyright 2026 The burrow Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style licence that can be found
 * in the LICENSE file. */

/* burrow:package hash/fnv */

#ifndef BURROW_HASH_FNV_H
#define BURROW_HASH_FNV_H

#include "burrow/core.h"
#include "burrow/hash.h"
#include "burrow/mem.h"

#ifdef __cplusplus
extern "C" {
#endif

/* 32 bit FNV-1 and FNV-1a. */
BURROW_OWNS(ret) HashHash32 fnv_new32(Alloc *a);
BURROW_OWNS(ret) HashHash32 fnv_new32a(Alloc *a);

/* 64 bit FNV-1 and FNV-1a. */
BURROW_OWNS(ret) HashHash64 fnv_new64(Alloc *a);
BURROW_OWNS(ret) HashHash64 fnv_new64a(Alloc *a);

/* 128 bit FNV-1 and FNV-1a. There is no 128 bit integer to return, so these
 * are a plain Hash and the sum comes from Sum, big endian, high half first. */
BURROW_OWNS(ret) Hash fnv_new128(Alloc *a);
BURROW_OWNS(ret) Hash fnv_new128a(Alloc *a);

#ifdef __cplusplus
}
#endif

#endif /* BURROW_HASH_FNV_H */
