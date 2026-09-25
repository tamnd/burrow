/* crypto/sha3, the SHA-3 hashes and the SHAKE and cSHAKE extendable output
 * functions from FIPS 202 and SP 800-185.
 *
 *     Sha3Sum256Ret s = sha3_sum256(data);  // s.a is the 32 byte digest
 *
 *     Sha3 h = {0};                         // the zero value is SHA3-256
 *     sha3_write(&h, part1, NULL);
 *     sha3_write(&h, part2, NULL);
 *     Slice sum = sha3_sum(&h, a, slice_nil(TYPE_BYTE));
 *
 *     Sha3SHAKE x = {0};                    // the zero value is SHAKE256
 *     sha3_shake_write(&x, data, NULL);
 *     sha3_shake_read(&x, out, NULL);       // as much output as out holds
 *
 * Both types are plain structs, so they can live on the stack or inside
 * another struct with no allocation, and a zero one works as Go's does. The
 * constructors return one allocated from a, for code that wants a pointer, and
 * sha3_as_hash and sha3_shake_as_xof turn either kind into the hash.Hash and
 * hash.XOF interfaces.
 *
 * Writing after reading from a SHAKE panics with "sha3: Write after Read", as
 * it does in Go. A cSHAKE keeps its function name and customisation string in
 * memory from the allocator it was made with, so that Reset can absorb them
 * again.
 *
 * Copyright 2014 The Go Authors. All rights reserved.
 * Copyright 2026 The burrow Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style licence that can be found
 * in the LICENSE file. */

/* burrow:package crypto/sha3 */

#ifndef BURROW_CRYPTO_SHA3_H
#define BURROW_CRYPTO_SHA3_H

#include "burrow/core.h"
#include "burrow/hash.h"
#include "burrow/mem.h"
#include "burrow/slice.h"

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* A SHA-3 hash. Leave the fields alone: a zero Sha3 becomes SHA3-256 the
 * first time it is used, and the constructors fill in the others. */
typedef struct Sha3 {
    uint64_t a[25]; /* the Keccak state, lane by lane */
    Int n;          /* where the next byte goes in the state */
    Int rate;       /* the block size in bytes, 0 before first use */
    Int output_len; /* the digest size in bytes */
    Byte dsbyte;    /* the domain separation and first padding bits */
    bool squeezing; /* output has been read, so input is over */
} Sha3;

/* A SHAKE or cSHAKE extendable output function. A zero Sha3SHAKE becomes
 * SHAKE256 the first time it is used. */
typedef struct Sha3SHAKE {
    Sha3 d;
    Slice init_block; /* cSHAKE's encoded N and S, empty for SHAKE */
} Sha3SHAKE;

typedef struct Sha3Sum224Ret {
    Byte a[28];
} Sha3Sum224Ret;

typedef struct Sha3Sum256Ret {
    Byte a[32];
} Sha3Sum256Ret;

typedef struct Sha3Sum384Ret {
    Byte a[48];
} Sha3Sum384Ret;

typedef struct Sha3Sum512Ret {
    Byte a[64];
} Sha3Sum512Ret;

/* ------------------------------------------------------------ one shot sums */

Sha3Sum224Ret sha3_sum224(Slice data);
Sha3Sum256Ret sha3_sum256(Slice data);
Sha3Sum384Ret sha3_sum384(Slice data);
Sha3Sum512Ret sha3_sum512(Slice data);

/* length bytes of SHAKE128 or SHAKE256 output for data, in a new Slice from
 * a. A nil Slice if a is out of memory. */
BURROW_OWNS(ret) Slice sha3_sum_shake128(Alloc *a, Slice data, Int length);
BURROW_OWNS(ret) Slice sha3_sum_shake256(Alloc *a, Slice data, Int length);

/* ------------------------------------------------------------------- SHA-3 */

/* A new SHA3-224, SHA3-256, SHA3-384 or SHA3-512 hash allocated from a, or
 * NULL if a is out of memory. */
BURROW_OWNS(ret) Sha3 *sha3_new224(Alloc *a);
BURROW_OWNS(ret) Sha3 *sha3_new256(Alloc *a);
BURROW_OWNS(ret) Sha3 *sha3_new384(Alloc *a);
BURROW_OWNS(ret) Sha3 *sha3_new512(Alloc *a);

/* Absorbs p. It never fails, and sets *err, which may be NULL, to no error. */
Int sha3_write(Sha3 *d, Slice p, Error *err);

/* Appends the digest of what has been written so far to b and returns the
 * result, growing b from a if it has to. The state is not changed, so writing
 * can carry on. */
BURROW_OWNS(ret) BURROW_BORROWS(ret, b) Slice sha3_sum(Sha3 *d, Alloc *a, Slice b);

void sha3_reset(Sha3 *d);
Int sha3_size(Sha3 *d);
Int sha3_block_size(Sha3 *d);

/* The state as bytes that sha3_unmarshal_binary takes back, which are the
 * bytes Go's MarshalBinary gives. */
BURROW_OWNS(ret) Slice sha3_marshal_binary(Sha3 *d, Alloc *a, Error *err);
BURROW_OWNS(ret) BURROW_BORROWS(ret, b) Slice sha3_append_binary(Sha3 *d, Alloc *a,
                                                                 Slice b, Error *err);

/* Restores a state from sha3_marshal_binary. It has to be from the same
 * function: a SHA3-256 state does not go into a SHA3-512. */
BURROW_STATIC(ret) Error sha3_unmarshal_binary(Sha3 *d, Slice b);

/* A copy of d allocated from a, which carries on independently. */
BURROW_OWNS(ret) HashCloner sha3_clone(Sha3 *d, Alloc *a, Error *err);

/* d as a Hash, which borrows d. */
BURROW_BORROWS(ret, d) Hash sha3_as_hash(Sha3 *d);

/* d as a hash.Cloner, which borrows d. */
BURROW_BORROWS(ret, d) HashCloner sha3_as_cloner(Sha3 *d);

/* ------------------------------------------------------------ SHAKE, cSHAKE */

/* A new SHAKE128 or SHAKE256 allocated from a, or NULL if a is out of
 * memory. */
BURROW_OWNS(ret) Sha3SHAKE *sha3_new_shake128(Alloc *a);
BURROW_OWNS(ret) Sha3SHAKE *sha3_new_shake256(Alloc *a);

/* A new cSHAKE128 or cSHAKE256 with function name n and customisation string
 * s, both Slices of Byte. With both empty it is plain SHAKE. NULL if a is out
 * of memory. */
BURROW_OWNS(ret) Sha3SHAKE *sha3_new_cshake128(Alloc *a, Slice n, Slice s);
BURROW_OWNS(ret) Sha3SHAKE *sha3_new_cshake256(Alloc *a, Slice n, Slice s);

/* Absorbs p. Panics if output has already been read. */
Int sha3_shake_write(Sha3SHAKE *s, Slice p, Error *err);

/* Fills p with the next len(p) bytes of output. It never fails. */
Int sha3_shake_read(Sha3SHAKE *s, Slice p, Error *err);

/* Goes back to the state after construction, with cSHAKE's N and S absorbed
 * again. */
void sha3_shake_reset(Sha3SHAKE *s);
Int sha3_shake_block_size(Sha3SHAKE *s);

BURROW_OWNS(ret) Slice sha3_shake_marshal_binary(Sha3SHAKE *s, Alloc *a, Error *err);
BURROW_OWNS(ret) BURROW_BORROWS(ret, b) Slice sha3_shake_append_binary(Sha3SHAKE *s,
                                                                       Alloc *a,
                                                                       Slice b,
                                                                       Error *err);

/* Restores a state from sha3_shake_marshal_binary. A cSHAKE state carries its
 * N and S, which are copied into memory from a. */
BURROW_STATIC(ret) Error sha3_shake_unmarshal_binary(Sha3SHAKE *s, Alloc *a, Slice b);

/* s as a hash.XOF, which borrows s. */
BURROW_BORROWS(ret, s) HashXOF sha3_shake_as_xof(Sha3SHAKE *s);

#ifdef __cplusplus
}
#endif

#endif /* BURROW_CRYPTO_SHA3_H */
