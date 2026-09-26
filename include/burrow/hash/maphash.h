/* hash/maphash, the hash the map uses, for hash tables you build yourself.
 *
 * The hash is seeded, fast, and different on every run of the program, which
 * is what makes it safe for a table that holds keys from outside: nobody can
 * pick keys that collide without knowing the seed. It is not a cryptographic
 * hash, and its values are not stable across runs, machines or versions, so
 * never store one or send one anywhere.
 *
 *     MaphashSeed seed = maphash_make_seed();
 *     uint64_t h = maphash_string(seed, BURROW_S("hello"));
 *
 * MaphashHash is the streaming form, and like Go's its zero value is ready to
 * use. It picks a random seed the first time it needs one:
 *
 *     MaphashHash h = {0};
 *     maphash_hash_write_string(&h, BURROW_S("hello, "));
 *     maphash_hash_write_string(&h, BURROW_S("world"));
 *     uint64_t sum = maphash_hash_sum64(&h);
 *
 * Writing the same bytes with the same seed gives the same sum however the
 * bytes are split between the writes, and maphash_bytes and maphash_string
 * give that sum too.
 *
 * Copyright 2019 The Go Authors. All rights reserved.
 * Copyright 2026 The burrow Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style licence that can be found
 * in the LICENSE file. */

/* burrow:package hash/maphash */

#ifndef BURROW_HASH_MAPHASH_H
#define BURROW_HASH_MAPHASH_H

#include "burrow/core.h"
#include "burrow/error.h"
#include "burrow/hash.h"
#include "burrow/mem.h"
#include "burrow/slice.h"
#include "burrow/type.h"

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* A random seed for the hash. The zero value is not a seed, and hashing with
 * it panics, so make one with maphash_make_seed. Two hashes agree only when
 * they use the same seed. */
typedef struct MaphashSeed {
    uint64_t s;
} MaphashSeed;

/* How many bytes the streaming hash buffers before it hashes them. */
#define MAPHASH_BUF_SIZE 128

/* The streaming hash. The fields are here so that you can declare one on the
 * stack and not so that you can read them. */
typedef struct MaphashHash {
    MaphashSeed seed;  /* the seed the hash started from */
    MaphashSeed state; /* the hash of every byte flushed so far */
    Byte buf[MAPHASH_BUF_SIZE];
    Int n; /* how many bytes of buf are waiting */
} MaphashHash;

/* A new random seed. */
MaphashSeed maphash_make_seed(void);

/* The hash of b, a []byte, with seed. The same as writing b to a MaphashHash
 * with that seed and asking for Sum64. */
uint64_t maphash_bytes(MaphashSeed seed, Slice b);

/* The hash of s with seed. It equals maphash_bytes of the same bytes. */
uint64_t maphash_string(MaphashSeed seed, Str s);

/* The hash of the value of type t at v, with seed. Values that are equal by
 * type_equal hash the same, so a Str hashes by its bytes, and 0.0 and -0.0
 * hash the same. A NaN hashes differently each time, as it does in a map.
 * Panics if t is not comparable, the way Go panics on an interface holding a
 * slice. */
uint64_t maphash_comparable(MaphashSeed seed, const Type *t, const void *v);

/* Adds the value of type t at v to h, the way maphash_comparable hashes it.
 * The result depends on everything written before, so writing a value and then
 * a string is not the same as the other way round. */
void maphash_write_comparable(MaphashHash *h, const Type *t, const void *v);

/* Adds b to h. Never fails, and returns b.len, as Go's does. err may be NULL. */
Int maphash_hash_write(MaphashHash *h, Slice b, Error *err);

/* Adds the bytes of s to h. */
Int maphash_hash_write_string(MaphashHash *h, Str s, Error *err);

/* Adds one byte to h. Never fails. */
BURROW_STATIC(ret) Error maphash_hash_write_byte(MaphashHash *h, Byte c);

/* The seed h uses, picking a random one first if it has none yet. */
MaphashSeed maphash_hash_seed(MaphashHash *h);

/* Starts h over with seed and nothing written. Panics on a zero seed. */
void maphash_hash_set_seed(MaphashHash *h, MaphashSeed seed);

/* Starts h over with the seed it already has, and nothing written. */
void maphash_hash_reset(MaphashHash *h);

/* The hash of everything written to h so far. Does not change h, so you can
 * keep writing after asking. */
uint64_t maphash_hash_sum64(MaphashHash *h);

/* Appends the Sum64 of h to b, little endian, as Go's Sum does. */
BURROW_OWNS(ret) BURROW_BORROWS(ret, b) Slice maphash_hash_sum(MaphashHash *h, Alloc *a,
                                                               Slice b);

/* 8, the size of the sum, and MAPHASH_BUF_SIZE, the block size. */
Int maphash_hash_size(const MaphashHash *h);
Int maphash_hash_block_size(const MaphashHash *h);

/* A copy of h made with a, which carries on from the same point, as a
 * HashCloner. When the allocator says no, the result is nil and err is
 * burrow_err_out_of_memory. */
BURROW_OWNS(ret) HashCloner maphash_hash_clone(MaphashHash *h, Alloc *a, Error *err);

/* h as the hash interfaces, so it can go where a Hash, a HashHash64 or a
 * HashCloner is wanted. Each points at h, so h has to outlive it. */
BURROW_BORROWS(ret, h) Hash maphash_hash_as_hash(MaphashHash *h);
BURROW_BORROWS(ret, h) HashHash64 maphash_hash_as_hash64(MaphashHash *h);
BURROW_BORROWS(ret, h) HashCloner maphash_hash_as_cloner(MaphashHash *h);

/* ----------------------------------------------------------------- Hasher
 *
 * Go's Hasher[T]: how a hash based container hashes and compares its elements,
 * for elements that cannot be map keys or that want a different idea of
 * equal, such as strings compared without case. Hash writes v into h, and
 * Equal says whether x and y are the same. Two values that are Equal have to
 * write the same thing, and neither method may depend on anything but its
 * arguments. v, x and y point at values of the element type. */
typedef struct MaphashHasherVT {
    void (*hash)(void *self, MaphashHash *h, const void *v);
    bool (*equal)(void *self, const void *x, const void *y);
} MaphashHasherVT;

typedef struct MaphashHasher {
    const MaphashHasherVT *vt;
    void *data;
} MaphashHasher;

static inline void maphash_hasher_hash(MaphashHasher hr, MaphashHash *h,
                                       const void *v) {
    hr.vt->hash(hr.data, h, v);
}

static inline bool maphash_hasher_equal(MaphashHasher hr, const void *x,
                                        const void *y) {
    return hr.vt->equal(hr.data, x, y);
}

/* Go's ComparableHasher[T], the Hasher whose Equal is ==. Go names T in the
 * type; here it is the descriptor, and the rules are Go's: T has to be a type
 * that can be compared, and the values handed in have to be comparable, or
 * Hash panics the way maphash_comparable does. */
typedef struct MaphashComparableHasher {
    const Type *t;
} MaphashComparableHasher;

/* maphash_write_comparable(h, ch.t, v). */
void maphash_comparable_hasher_hash(MaphashComparableHasher ch, MaphashHash *h,
                                    const void *v);

/* type_equal(ch.t, x, y), which is Go's x == y. */
bool maphash_comparable_hasher_equal(MaphashComparableHasher ch, const void *x,
                                     const void *y);

/* ch as a MaphashHasher. It points at ch, so ch has to outlive it. */
BURROW_BORROWS(ret, ch) MaphashHasher
maphash_comparable_hasher_as_hasher(MaphashComparableHasher *ch);

#ifdef __cplusplus
}
#endif

#endif /* BURROW_HASH_MAPHASH_H */
