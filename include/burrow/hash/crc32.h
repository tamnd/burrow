/* hash/crc32, the 32 bit cyclic redundancy check.
 *
 * CRC-32 is the checksum in gzip, zip, PNG and Ethernet, which all use the IEEE
 * polynomial, and in iSCSI, ext4 and most storage formats since, which use
 * Castagnoli. It catches accidental corruption well and deliberate tampering
 * not at all, so it belongs next to data on a disk or a wire and never in front
 * of anything an attacker can write.
 *
 * The one call most people want:
 *
 *     uint32_t sum = crc32_checksum_ieee(data);
 *
 * A table picks the polynomial. crc32_make_table builds one, and asking for
 * CRC32_IEEE or CRC32_CASTAGNOLI gives back a table the package already has,
 * which is also how Update knows to take the fast path for those two. The bits
 * are reversed, least significant bit first, as they are in Go and in every
 * format above.
 *
 * Copyright 2026 The burrow Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style licence that can be found
 * in the LICENSE file. */

/* burrow:package hash/crc32 */

#ifndef BURROW_HASH_CRC32_H
#define BURROW_HASH_CRC32_H

#include "burrow/core.h"
#include "burrow/hash.h"
#include "burrow/mem.h"
#include "burrow/slice.h"

#ifdef __cplusplus
extern "C" {
#endif

/* The size of a CRC-32 checksum in bytes. */
#define CRC32_SIZE 4

/* The polynomials, in reversed notation. */
#define CRC32_IEEE 0xedb88320u       /* by far the most common */
#define CRC32_CASTAGNOLI 0x82f63b78u /* better error detection, used by iSCSI */
#define CRC32_KOOPMAN 0xeb31d82eu    /* also better, and rarely seen */

/* A 256 word table for one polynomial. It must not be modified once made. */
typedef struct Crc32Table {
    uint32_t v[256];
} Crc32Table;

/* The table for CRC32_IEEE. It is const and there from the start, so it needs
 * no crc32_make_table call before use. */
extern const Crc32Table *const crc32_ieee_table;

/* Returns the table for poly. For CRC32_IEEE and CRC32_CASTAGNOLI that is the
 * package's own static table and a nil allocator is fine. Any other polynomial
 * gets a new table from a, which the caller owns. */
BURROW_OWNS(ret) const Crc32Table *crc32_make_table(Alloc *a, uint32_t poly);

/* A new Hash32 computing the CRC-32 with tab, allocated from a. Sum appends the
 * checksum big endian, the way the network and every file format write it. The
 * hash keeps a pointer to tab, which has to outlive it. */
BURROW_OWNS(ret) BURROW_BORROWS(ret, tab) HashHash32 crc32_new(Alloc *a,
                                                               const Crc32Table *tab);

/* crc32_new with the IEEE table. */
BURROW_OWNS(ret) HashHash32 crc32_new_ieee(Alloc *a);

/* Returns crc with the bytes of p added, which is how you checksum a stream in
 * pieces without a Hash: start at zero and feed each result back in. */
uint32_t crc32_update(uint32_t crc, const Crc32Table *tab, Slice p);

/* The CRC-32 of data with tab. */
uint32_t crc32_checksum(Slice data, const Crc32Table *tab);

/* The CRC-32 of data with the IEEE polynomial. */
uint32_t crc32_checksum_ieee(Slice data);

#ifdef __cplusplus
}
#endif

#endif /* BURROW_HASH_CRC32_H */
