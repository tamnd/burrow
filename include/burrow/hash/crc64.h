/* hash/crc64, the 64 bit cyclic redundancy check.
 *
 * Two polynomials are defined. ECMA, from ECMA-182, is the one xz uses and the
 * one to pick when you have a free choice. ISO, from ISO 3309, is what HDLC
 * uses. Like CRC-32, this catches accidents and not attackers.
 *
 *     const Crc64Table *tab = crc64_make_table(NULL, CRC64_ECMA);
 *     uint64_t sum = crc64_checksum(data, tab);
 *
 * Copyright 2026 The burrow Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style licence that can be found
 * in the LICENSE file. */

/* burrow:package hash/crc64 */

#ifndef BURROW_HASH_CRC64_H
#define BURROW_HASH_CRC64_H

#include "burrow/core.h"
#include "burrow/hash.h"
#include "burrow/mem.h"
#include "burrow/slice.h"

#ifdef __cplusplus
extern "C" {
#endif

/* The size of a CRC-64 checksum in bytes. */
#define CRC64_SIZE 8

/* The polynomials, in reversed notation. */
#define CRC64_ISO 0xD800000000000000ULL
#define CRC64_ECMA 0xC96C5795D7870F42ULL

/* A 256 word table for one polynomial. It must not be modified once made. */
typedef struct Crc64Table {
    uint64_t v[256];
} Crc64Table;

/* Returns the table for poly. For CRC64_ISO and CRC64_ECMA that is the
 * package's own static table and a nil allocator is fine. Any other polynomial
 * gets a new table from a, which the caller owns. */
BURROW_OWNS(ret) const Crc64Table *crc64_make_table(Alloc *a, uint64_t poly);

/* A new Hash64 computing the CRC-64 with tab, allocated from a. Sum appends the
 * checksum big endian. The hash keeps a pointer to tab, which has to outlive
 * it. */
BURROW_OWNS(ret) BURROW_BORROWS(ret, tab) HashHash64 crc64_new(Alloc *a,
                                                               const Crc64Table *tab);

/* Returns crc with the bytes of p added. */
uint64_t crc64_update(uint64_t crc, const Crc64Table *tab, Slice p);

/* The CRC-64 of data with tab. */
uint64_t crc64_checksum(Slice data, const Crc64Table *tab);

#ifdef __cplusplus
}
#endif

#endif /* BURROW_HASH_CRC64_H */
