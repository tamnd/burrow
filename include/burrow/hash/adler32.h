/* hash/adler32, the Adler-32 checksum from RFC 1950.
 *
 * Adler-32 is the checksum at the end of a zlib stream. It is two 16 bit sums,
 * one of the bytes and one of the running totals, which makes it faster than
 * CRC-32 and weaker on short inputs. Use it where a format asks for it.
 *
 *     uint32_t sum = adler32_checksum(data);
 *
 * Copyright 2026 The burrow Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style licence that can be found
 * in the LICENSE file. */

/* burrow:package hash/adler32 */

#ifndef BURROW_HASH_ADLER32_H
#define BURROW_HASH_ADLER32_H

#include "burrow/core.h"
#include "burrow/hash.h"
#include "burrow/mem.h"
#include "burrow/slice.h"

#ifdef __cplusplus
extern "C" {
#endif

/* The size of an Adler-32 checksum in bytes. */
#define ADLER32_SIZE 4

/* A new Hash32 computing the Adler-32 checksum, allocated from a. Sum appends
 * the checksum big endian. */
BURROW_OWNS(ret) HashHash32 adler32_new(Alloc *a);

/* The Adler-32 checksum of data. */
uint32_t adler32_checksum(Slice data);

#ifdef __cplusplus
}
#endif

#endif /* BURROW_HASH_ADLER32_H */
