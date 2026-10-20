/* hash: the interfaces every hash function speaks.
 *
 * A hash here is an io.Writer you feed bytes to and then ask for a sum. The
 * checksums in hash/crc32, hash/crc64, hash/adler32 and hash/fnv implement
 * these, and so will the cryptographic hashes under crypto when they arrive,
 * which is the point: code that takes a Hash does not care which one it got.
 *
 *     Arena ar;
 *     arena_init(&ar, heap_allocator(), 0);
 *     Alloc *a = arena_allocator(&ar);
 *
 *     char msg[] = "hello";
 *     HashHash32 h = crc32_new_ieee(a);
 *     hash_write(hash_hash32_as_hash(h), slice_from(msg, 5, 5, TYPE_BYTE), NULL);
 *     uint32_t sum = hash_hash32_sum32(h);
 *
 * Write never fails for any hash in the standard library, the same as in Go, so
 * passing NULL for the error is fine when you know which hash you have.
 *
 * Sum appends the current hash to b and returns the result, and does not change
 * the state, so you can keep writing after asking. It takes an allocator
 * because appending may have to grow b.
 *
 * A constructor such as crc32_new returns a nil hash, one whose vt is NULL, when
 * the allocator runs out of memory, and a table maker returns NULL.
 *
 * Copyright 2026 The burrow Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style licence that can be found
 * in the LICENSE file. */

/* burrow:package hash */

#ifndef BURROW_HASH_H
#define BURROW_HASH_H

#include "burrow/core.h"
#include "burrow/error.h"
#include "burrow/io.h"
#include "burrow/mem.h"
#include "burrow/slice.h"
#include "burrow/type.h"

#ifdef __cplusplus
extern "C" {
#endif

/* hash.Hash.
 *
 * The io.Writer comes first, as a member, so a Hash converts to an IoWriter by
 * taking its address. The rules in burrow/iface.h hold here as everywhere. */
typedef struct HashVT {
    IoWriterVT writer;
    /* Appends the current hash to b and returns the result. */
    Slice (*sum)(void *self, Alloc *a, Slice b);
    /* Puts the hash back where New left it. */
    void (*reset)(void *self);
    /* How many bytes Sum appends. */
    Int (*size)(void *self);
    /* The size of the block the hash works on underneath. Write takes any
     * length, but a multiple of this is faster for some hashes. */
    Int (*block_size)(void *self);
} HashVT;

typedef struct Hash {
    const HashVT *vt;
    void *data;
} Hash;

/* A function that makes a new Hash, which is what Go's func() hash.Hash is.
 * crypto/hmac, crypto/hkdf and crypto/pbkdf2 take one so they can make as many
 * hashes of the same kind as they need, and sha256_new is one as it stands. It
 * returns a nil Hash when a runs out of memory. */
typedef Hash (*HashNewFunc)(Alloc *a);

/* hash.Hash32, a Hash whose sum is also a 32 bit number. */
typedef struct HashHash32VT {
    HashVT hash;
    uint32_t (*sum32)(void *self);
} HashHash32VT;

typedef struct HashHash32 {
    const HashHash32VT *vt;
    void *data;
} HashHash32;

/* hash.Hash64, a Hash whose sum is also a 64 bit number. */
typedef struct HashHash64VT {
    HashVT hash;
    uint64_t (*sum64)(void *self);
} HashHash64VT;

typedef struct HashHash64 {
    const HashHash64VT *vt;
    void *data;
} HashHash64;

/* hash.Cloner, a Hash that can copy its own state. Clone returns an
 * independent hash that carries on from the same point. */
typedef struct HashCloner HashCloner;

typedef struct HashClonerVT {
    HashVT hash;
    HashCloner (*clone)(void *self, Alloc *a, Error *err);
} HashClonerVT;

struct HashCloner {
    const HashClonerVT *vt;
    void *data;
};

/* hash.XOF, an extendable output function: write the input, then read as much
 * output as you want. It has no Sum and no Size, because the output has no
 * fixed length. */
typedef struct HashXOFVT {
    IoWriterVT writer;
    IoReaderVT reader;
    void (*reset)(void *self);
    Int (*block_size)(void *self);
} HashXOFVT;

typedef struct HashXOF {
    const HashXOFVT *vt;
    void *data;
} HashXOF;

/* ------------------------------------------------------------- method calls
 *
 * Calling through the vtable by hand means spelling out the embedding, so
 * these do it for you. Each one is a load and an indirect call. */

static inline Int hash_write(Hash h, Slice p, Error *err) {
    return h.vt->writer.write(h.data, p, err);
}

BURROW_OWNS(ret) BURROW_BORROWS(ret, b) static inline Slice hash_sum(Alloc *a, Hash h,
                                                                     Slice b) {
    return h.vt->sum(h.data, a, b);
}

static inline void hash_reset(Hash h) {
    h.vt->reset(h.data);
}

static inline Int hash_size(Hash h) {
    return h.vt->size(h.data);
}

static inline Int hash_block_size(Hash h) {
    return h.vt->block_size(h.data);
}

static inline uint32_t hash_hash32_sum32(HashHash32 h) {
    return h.vt->sum32(h.data);
}

static inline uint64_t hash_hash64_sum64(HashHash64 h) {
    return h.vt->sum64(h.data);
}

static inline HashCloner hash_cloner_clone(Alloc *a, HashCloner h, Error *err) {
    return h.vt->clone(h.data, a, err);
}

static inline Int hash_xof_read(HashXOF h, Slice p, Error *err) {
    return h.vt->reader.read(h.data, p, err);
}

/* ------------------------------------------------------------- conversions
 *
 * Go's implicit conversion from a wider interface to a narrower one, written
 * out. A nil value in gives a nil value out. */

static inline IoWriter hash_as_io_writer(Hash h) {
    IoWriter w = {NULL, NULL};
    if (h.vt != NULL) {
        w.vt = &h.vt->writer;
        w.data = h.data;
    }
    return w;
}

static inline Hash hash_hash32_as_hash(HashHash32 h) {
    Hash r = {NULL, NULL};
    if (h.vt != NULL) {
        r.vt = &h.vt->hash;
        r.data = h.data;
    }
    return r;
}

static inline Hash hash_hash64_as_hash(HashHash64 h) {
    Hash r = {NULL, NULL};
    if (h.vt != NULL) {
        r.vt = &h.vt->hash;
        r.data = h.data;
    }
    return r;
}

static inline Hash hash_cloner_as_hash(HashCloner h) {
    Hash r = {NULL, NULL};
    if (h.vt != NULL) {
        r.vt = &h.vt->hash;
        r.data = h.data;
    }
    return r;
}

static inline IoWriter hash_xof_as_io_writer(HashXOF h) {
    IoWriter w = {NULL, NULL};
    if (h.vt != NULL) {
        w.vt = &h.vt->writer;
        w.data = h.data;
    }
    return w;
}

static inline IoReader hash_xof_as_io_reader(HashXOF h) {
    IoReader r = {NULL, NULL};
    if (h.vt != NULL) {
        r.vt = &h.vt->reader;
        r.data = h.data;
    }
    return r;
}

/* ------------------------------------------------------------ method sets
 *
 * Go's hashes are more than a Hash. Every one in the standard library is also
 * a Cloner and an encoding.BinaryMarshaler, BinaryAppender and
 * BinaryUnmarshaler, and code that holds a Hash asks for those with a type
 * assertion, as crypto/tls does with h.(hash.Cloner). A vtable cannot be asked
 * that, so a hash lists its methods in the descriptor its vtable names as
 * self_type, the same way io_copy finds WriterTo:
 *
 *     #define MY_HASH_METHODS(M, T)                                            \
 *         M(T, AppendBinary, my_append, ENCODING_SIG_APPEND_BINARY)            \
 *         M(T, BlockSize, my_block_size, HASH_SIG_BLOCK_SIZE)                  \
 *         M(T, Clone, my_clone, HASH_SIG_CLONE)                                \
 *         ...
 *
 * The C functions take the receiver as a pointer and then:
 *
 *     Int write(T *self, Slice p, Error *err);
 *     Slice sum(T *self, Alloc *a, Slice b);
 *     void reset(T *self);
 *     Int size(T *self);
 *     Int block_size(T *self);
 *     HashCloner clone(T *self, Alloc *a, Error *err);
 *     uint32_t sum32(T *self);
 *     uint64_t sum64(T *self);
 *
 * Write's shape is io's IO_SIG_WRITE. The marshaling methods have the shapes
 * in burrow/encoding.h, and an Any holding the Hash finds them:
 *
 *     Slice state = encoding_marshal_binary(a, BURROW_ANY(TYPE_OF(Hash), &h), &err);
 *
 * The descriptors of Hash and Cloner list Go's methods for them, so reflect
 * can tell which methods a hash has beyond the interface's. */
extern const Type burrow_type_Hash;
extern const Type burrow_type_HashCloner;

/* Alloc * under one word, since a signature list names its types by a single
 * token. */
typedef Alloc *HashAllocArg;
extern const Type burrow_type_HashAllocArg;

#define HASH_SIG_SUM(IN, OUT) IN(0, HashAllocArg) IN(1, Bytes) OUT(Bytes)
#define HASH_SIG_RESET(IN, OUT)
#define HASH_SIG_SIZE(IN, OUT) OUT(Int)
#define HASH_SIG_BLOCK_SIZE(IN, OUT) OUT(Int)
#define HASH_SIG_CLONE(IN, OUT) IN(0, HashAllocArg) OUT(HashCloner) OUT(ErrorResult)
#define HASH_SIG_SUM32(IN, OUT) OUT(uint32_t)
#define HASH_SIG_SUM64(IN, OUT) OUT(uint64_t)

/* Go's h.(hash.Cloner), the comma ok form: whether the type behind h lists a
 * Clone method of HASH_SIG_CLONE's shape. A nil h is not a Cloner. */
bool hash_is_cloner(Hash h);

/* h.(hash.Cloner).Clone(): an independent hash that carries on from where h
 * is, allocated from a. When h is not a Cloner this returns a nil HashCloner
 * and Go's type assertion error rather than panicking, which is the rule
 * burrow/encoding.h follows too. */
BURROW_OWNS(ret) HashCloner hash_clone(Alloc *a, Hash h, Error *err);

#ifdef __cplusplus
}
#endif

#endif /* BURROW_HASH_H */
