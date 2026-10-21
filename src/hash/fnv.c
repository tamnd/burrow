/* Derived from Go's src/hash/fnv/fnv.go.
 * Go source: go1.27.1.
 *
 * Copyright 2011 The Go Authors. All rights reserved.
 * Copyright 2026 The burrow Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style licence that can be found
 * in the LICENSE file. */

#include "burrow/hash/fnv.h"
#include "burrow/core.h"
#include "burrow/declare.h"
#include "burrow/encoding.h"
#include "burrow/error.h"
#include "burrow/hash.h"
#include "burrow/io.h"
#include "burrow/math/bits.h"
#include "burrow/mem.h"
#include "burrow/slice.h"
#include "burrow/type.h"

#include <string.h>

/* One state struct serves all six. The 32 and 64 bit hashes keep their state
 * in s[1], and the 128 bit ones use both words, s[0] the high half. */
typedef struct FnvSum {
    uint64_t s[2];
} FnvSum;

#define FNV_OFFSET32 2166136261u
#define FNV_OFFSET64 14695981039346656037ULL
#define FNV_OFFSET128_LOWER 0x62b821756295c58dULL
#define FNV_OFFSET128_HIGHER 0x6c62272e07bb0142ULL
#define FNV_PRIME32 16777619u
#define FNV_PRIME64 1099511628211ULL
#define FNV_PRIME128_LOWER 0x13bULL
#define FNV_PRIME128_SHIFT 24

static Slice fnv_append_be(Alloc *a, Slice in, const uint64_t *w, int nw, int size) {
    Byte b[16];
    int n = 0;
    for (int i = 0; i < nw; i++)
        for (int sh = size * 8 / nw - 8; sh >= 0; sh -= 8)
            b[n++] = (Byte)(w[i] >> sh);
    if (in.elem == NULL)
        in = slice_nil(TYPE_BYTE);
    return slice_append(a, in, b, n);
}

static Int fnv_block_size(void *self) {
    (void)self;
    return 1;
}

/* ------------------------------------------------------------------ 32 bit */

static Int fnv_write32(void *self, Slice data, Error *err) {
    FnvSum *s = self;
    uint32_t hash = (uint32_t)s->s[1];
    const Byte *p = (const Byte *)data.p;
    for (Int i = 0; i < data.len; i++) {
        hash *= FNV_PRIME32;
        hash ^= p[i];
    }
    s->s[1] = hash;
    BURROW_OUT(err, BURROW_NO_ERROR);
    return data.len;
}

static Int fnv_write32a(void *self, Slice data, Error *err) {
    FnvSum *s = self;
    uint32_t hash = (uint32_t)s->s[1];
    const Byte *p = (const Byte *)data.p;
    for (Int i = 0; i < data.len; i++) {
        hash ^= p[i];
        hash *= FNV_PRIME32;
    }
    s->s[1] = hash;
    BURROW_OUT(err, BURROW_NO_ERROR);
    return data.len;
}

static Slice fnv_sum_32(void *self, Alloc *a, Slice in) {
    return fnv_append_be(a, in, &((FnvSum *)self)->s[1], 1, 4);
}

static void fnv_reset32(void *self) {
    ((FnvSum *)self)->s[1] = FNV_OFFSET32;
}

static Int fnv_size32(void *self) {
    (void)self;
    return 4;
}

static uint32_t fnv_sum32(void *self) {
    return (uint32_t)((FnvSum *)self)->s[1];
}

/* ------------------------------------------------------------------ 64 bit */

static Int fnv_write64(void *self, Slice data, Error *err) {
    FnvSum *s = self;
    uint64_t hash = s->s[1];
    const Byte *p = (const Byte *)data.p;
    for (Int i = 0; i < data.len; i++) {
        hash *= FNV_PRIME64;
        hash ^= p[i];
    }
    s->s[1] = hash;
    BURROW_OUT(err, BURROW_NO_ERROR);
    return data.len;
}

static Int fnv_write64a(void *self, Slice data, Error *err) {
    FnvSum *s = self;
    uint64_t hash = s->s[1];
    const Byte *p = (const Byte *)data.p;
    for (Int i = 0; i < data.len; i++) {
        hash ^= p[i];
        hash *= FNV_PRIME64;
    }
    s->s[1] = hash;
    BURROW_OUT(err, BURROW_NO_ERROR);
    return data.len;
}

static Slice fnv_sum_64(void *self, Alloc *a, Slice in) {
    return fnv_append_be(a, in, &((FnvSum *)self)->s[1], 1, 8);
}

static void fnv_reset64(void *self) {
    ((FnvSum *)self)->s[1] = FNV_OFFSET64;
}

static Int fnv_size64(void *self) {
    (void)self;
    return 8;
}

static uint64_t fnv_sum64(void *self) {
    return ((FnvSum *)self)->s[1];
}

/* ----------------------------------------------------------------- 128 bit */

/* Multiplies the 128 bit state by the 128 bit prime, which is
 * 2^88 + 0x13b, modulo 2^128. */
static void fnv_mul128(FnvSum *s) {
    uint64_t lo;
    uint64_t hi = bits_mul64(FNV_PRIME128_LOWER, s->s[1], &lo);
    hi += (s->s[1] << FNV_PRIME128_SHIFT) + FNV_PRIME128_LOWER * s->s[0];
    s->s[0] = hi;
    s->s[1] = lo;
}

static Int fnv_write128(void *self, Slice data, Error *err) {
    FnvSum *s = self;
    const Byte *p = (const Byte *)data.p;
    for (Int i = 0; i < data.len; i++) {
        fnv_mul128(s);
        s->s[1] ^= p[i];
    }
    BURROW_OUT(err, BURROW_NO_ERROR);
    return data.len;
}

static Int fnv_write128a(void *self, Slice data, Error *err) {
    FnvSum *s = self;
    const Byte *p = (const Byte *)data.p;
    for (Int i = 0; i < data.len; i++) {
        s->s[1] ^= p[i];
        fnv_mul128(s);
    }
    BURROW_OUT(err, BURROW_NO_ERROR);
    return data.len;
}

static Slice fnv_sum_128(void *self, Alloc *a, Slice in) {
    return fnv_append_be(a, in, ((FnvSum *)self)->s, 2, 16);
}

static void fnv_reset128(void *self) {
    FnvSum *s = self;
    s->s[0] = FNV_OFFSET128_HIGHER;
    s->s[1] = FNV_OFFSET128_LOWER;
}

static Int fnv_size128(void *self) {
    (void)self;
    return 16;
}

/* -------------------------------------------------------------- marshaling */

BURROW_SENTINEL_ERROR(fnv_err_identifier, "hash/fnv: invalid hash state identifier");
BURROW_SENTINEL_ERROR(fnv_err_size, "hash/fnv: invalid hash state size");

/* The state of a 32 or 64 bit hash is s[1], and of a 128 bit one both words,
 * high first, as fnv_append_be lays them out. */
static Slice fnv_append_state(const FnvSum *s, Alloc *a, Slice b, Error *err,
                              Byte magic, int size) {
    Byte st[4 + 16] = {'f', 'n', 'v', magic};
    int nw = size == 16 ? 2 : 1;
    const uint64_t *w = size == 16 ? s->s : &s->s[1];
    int n = 4;
    for (int i = 0; i < nw; i++)
        for (int sh = size * 8 / nw - 8; sh >= 0; sh -= 8)
            st[n++] = (Byte)(w[i] >> sh);
    if (b.elem == NULL)
        b = slice_nil(TYPE_BYTE);
    Int len = b.len;
    Slice r = slice_append(a, b, st, n);
    BURROW_OUT(err, r.len == len + n ? BURROW_NO_ERROR : burrow_err_out_of_memory);
    return r;
}

static Slice fnv_marshal_state(const FnvSum *s, Alloc *a, Error *err, Byte magic,
                               int size) {
    Slice b = slice_make(a, TYPE_BYTE, 0, 4 + size);
    if (b.p == NULL) {
        BURROW_OUT(err, burrow_err_out_of_memory);
        return slice_nil(TYPE_BYTE);
    }
    return fnv_append_state(s, a, b, err, magic, size);
}

static Error fnv_unmarshal_state(FnvSum *s, Slice b, Byte magic, int size) {
    const Byte *p = b.p;
    if (b.len < 4 || memcmp(p, "fnv", 3) != 0 || p[3] != magic)
        return fnv_err_identifier;
    if (b.len != 4 + size)
        return fnv_err_size;
    int nw = size == 16 ? 2 : 1;
    uint64_t w[2] = {0, 0};
    for (int i = 0; i < nw; i++)
        for (int j = 0; j < size / nw; j++)
            w[i] = w[i] << 8 | p[4 + i * (size / nw) + j];
    if (nw == 2)
        s->s[0] = w[0];
    s->s[1] = w[nw - 1];
    return BURROW_NO_ERROR;
}

/* ------------------------------------------------------------- method sets
 *
 * Go has a type for each of the six, sum32 to sum128a, each with its own
 * methods. Here they share FnvSum, so each gets a name for the thunks to cast
 * to, its own method list and descriptor, and a vtable as the HashCloner that
 * Clone returns as well as the one New returns. */

/* The marshaling methods and Clone of the hash called v, whose magic ends in
 * the byte magic and whose state is size bytes. */
#define FNV_VARIANT(v, magic, size)                                                    \
    typedef FnvSum FnvSum##v;                                                          \
    static Slice fnv_append_binary##v(FnvSum *s, Alloc *a, Slice b, Error *err) {      \
        return fnv_append_state(s, a, b, err, magic, size);                            \
    }                                                                                  \
    static Slice fnv_marshal_binary##v(FnvSum *s, Alloc *a, Error *err) {              \
        return fnv_marshal_state(s, a, err, magic, size);                              \
    }                                                                                  \
    static Error fnv_unmarshal_binary##v(FnvSum *s, Alloc *a, Slice b) {               \
        (void)a;                                                                       \
        return fnv_unmarshal_state(s, b, magic, size);                                 \
    }                                                                                  \
    static HashCloner fnv_clone##v(void *self, Alloc *a, Error *err);

FNV_VARIANT(32, 1, 4)
FNV_VARIANT(32a, 2, 4)
FNV_VARIANT(64, 3, 8)
FNV_VARIANT(64a, 4, 8)
FNV_VARIANT(128, 5, 16)
FNV_VARIANT(128a, 6, 16)

#define FNV_METHODS32(M, T, v)                                                         \
    M(T, AppendBinary, fnv_append_binary##v, ENCODING_SIG_APPEND_BINARY)               \
    M(T, BlockSize, fnv_block_size, HASH_SIG_BLOCK_SIZE)                               \
    M(T, Clone, fnv_clone##v, HASH_SIG_CLONE)                                          \
    M(T, MarshalBinary, fnv_marshal_binary##v, ENCODING_SIG_MARSHAL_BINARY)            \
    M(T, Reset, fnv_reset32, HASH_SIG_RESET)                                           \
    M(T, Size, fnv_size32, HASH_SIG_SIZE)                                              \
    M(T, Sum, fnv_sum_32, HASH_SIG_SUM)                                                \
    M(T, Sum32, fnv_sum32, HASH_SIG_SUM32)                                             \
    M(T, UnmarshalBinary, fnv_unmarshal_binary##v, ENCODING_SIG_UNMARSHAL_BINARY)      \
    M(T, Write, fnv_write##v, IO_SIG_WRITE)
#define FNV_METHODS64(M, T, v)                                                         \
    M(T, AppendBinary, fnv_append_binary##v, ENCODING_SIG_APPEND_BINARY)               \
    M(T, BlockSize, fnv_block_size, HASH_SIG_BLOCK_SIZE)                               \
    M(T, Clone, fnv_clone##v, HASH_SIG_CLONE)                                          \
    M(T, MarshalBinary, fnv_marshal_binary##v, ENCODING_SIG_MARSHAL_BINARY)            \
    M(T, Reset, fnv_reset64, HASH_SIG_RESET)                                           \
    M(T, Size, fnv_size64, HASH_SIG_SIZE)                                              \
    M(T, Sum, fnv_sum_64, HASH_SIG_SUM)                                                \
    M(T, Sum64, fnv_sum64, HASH_SIG_SUM64)                                             \
    M(T, UnmarshalBinary, fnv_unmarshal_binary##v, ENCODING_SIG_UNMARSHAL_BINARY)      \
    M(T, Write, fnv_write##v, IO_SIG_WRITE)
#define FNV_METHODS128(M, T, v)                                                        \
    M(T, AppendBinary, fnv_append_binary##v, ENCODING_SIG_APPEND_BINARY)               \
    M(T, BlockSize, fnv_block_size, HASH_SIG_BLOCK_SIZE)                               \
    M(T, Clone, fnv_clone##v, HASH_SIG_CLONE)                                          \
    M(T, MarshalBinary, fnv_marshal_binary##v, ENCODING_SIG_MARSHAL_BINARY)            \
    M(T, Reset, fnv_reset128, HASH_SIG_RESET)                                          \
    M(T, Size, fnv_size128, HASH_SIG_SIZE)                                             \
    M(T, Sum, fnv_sum_128, HASH_SIG_SUM)                                               \
    M(T, UnmarshalBinary, fnv_unmarshal_binary##v, ENCODING_SIG_UNMARSHAL_BINARY)      \
    M(T, Write, fnv_write##v, IO_SIG_WRITE)

#define FNV_M32(M, T) FNV_METHODS32(M, T, 32)
#define FNV_M32A(M, T) FNV_METHODS32(M, T, 32a)
#define FNV_M64(M, T) FNV_METHODS64(M, T, 64)
#define FNV_M64A(M, T) FNV_METHODS64(M, T, 64a)
#define FNV_M128(M, T) FNV_METHODS128(M, T, 128)
#define FNV_M128A(M, T) FNV_METHODS128(M, T, 128a)

BURROW_METHODS_DEFINE(FnvSum32, FNV_M32);
BURROW_METHODS_DEFINE(FnvSum32a, FNV_M32A);
BURROW_METHODS_DEFINE(FnvSum64, FNV_M64);
BURROW_METHODS_DEFINE(FnvSum64a, FNV_M64A);
BURROW_METHODS_DEFINE(FnvSum128, FNV_M128);
BURROW_METHODS_DEFINE(FnvSum128a, FNV_M128A);

/* Not in Go: Go's sum32 is a uint32 and its sum128 a [2]uint64, but all six
 * here are the one FnvSum struct, so that is what the descriptors say. */
#define FNV_TYPE(v, gonm)                                                              \
    static const Type fnv_sum##v##_type = {                                            \
        {(const Byte *)(gonm), (Int)(sizeof(gonm) - 1)},                               \
        {(const Byte *)"hash/fnv", 8},                                                 \
        KIND_STRUCT,                                                                   \
        (uint32_t)sizeof(FnvSum),                                                      \
        (uint16_t)_Alignof(FnvSum),                                                    \
        0,                                                                             \
        (uint16_t)(sizeof burrow__methods_FnvSum##v /                                  \
                   sizeof burrow__methods_FnvSum##v[0]),                               \
        NULL,                                                                          \
        burrow__methods_FnvSum##v,                                                     \
        NULL,                                                                          \
        NULL,                                                                          \
        0,                                                                             \
        0,                                                                             \
        NULL,                                                                          \
    }

FNV_TYPE(32, "sum32");
FNV_TYPE(32a, "sum32a");
FNV_TYPE(64, "sum64");
FNV_TYPE(64a, "sum64a");
FNV_TYPE(128, "sum128");
FNV_TYPE(128a, "sum128a");

#define FNV_HASH_VT(v, n)                                                              \
    {{&fnv_sum##v##_type, fnv_write##v},                                               \
     fnv_sum_##n,                                                                      \
     fnv_reset##n,                                                                     \
     fnv_size##n,                                                                      \
     fnv_block_size}

static const HashHash32VT fnv32_vt = {FNV_HASH_VT(32, 32), fnv_sum32};
static const HashHash32VT fnv32a_vt = {FNV_HASH_VT(32a, 32), fnv_sum32};
static const HashHash64VT fnv64_vt = {FNV_HASH_VT(64, 64), fnv_sum64};
static const HashHash64VT fnv64a_vt = {FNV_HASH_VT(64a, 64), fnv_sum64};
static const HashVT fnv128_vt = FNV_HASH_VT(128, 128);
static const HashVT fnv128a_vt = FNV_HASH_VT(128a, 128);

static const HashClonerVT fnv32_cloner_vt = {FNV_HASH_VT(32, 32), fnv_clone32};
static const HashClonerVT fnv32a_cloner_vt = {FNV_HASH_VT(32a, 32), fnv_clone32a};
static const HashClonerVT fnv64_cloner_vt = {FNV_HASH_VT(64, 64), fnv_clone64};
static const HashClonerVT fnv64a_cloner_vt = {FNV_HASH_VT(64a, 64), fnv_clone64a};
static const HashClonerVT fnv128_cloner_vt = {FNV_HASH_VT(128, 128), fnv_clone128};
static const HashClonerVT fnv128a_cloner_vt = {FNV_HASH_VT(128a, 128), fnv_clone128a};

static HashCloner fnv_clone(const FnvSum *s, Alloc *a, Error *err,
                            const HashClonerVT *vt) {
    FnvSum *r = BURROW_NEW(a, FnvSum);
    if (r == NULL) {
        BURROW_OUT(err, burrow_err_out_of_memory);
        return (HashCloner){NULL, NULL};
    }
    *r = *s;
    BURROW_OUT(err, BURROW_NO_ERROR);
    HashCloner h = {vt, r};
    return h;
}

#define FNV_CLONE(v)                                                                   \
    static HashCloner fnv_clone##v(void *self, Alloc *a, Error *err) {                 \
        return fnv_clone(self, a, err, &fnv##v##_cloner_vt);                           \
    }

FNV_CLONE(32)
FNV_CLONE(32a)
FNV_CLONE(64)
FNV_CLONE(64a)
FNV_CLONE(128)
FNV_CLONE(128a)

/* ------------------------------------------------------------ constructors */

HashHash32 fnv_new32(Alloc *a) {
    FnvSum *s = BURROW_NEW(a, FnvSum);
    if (s == NULL)
        return (HashHash32){NULL, NULL};
    s->s[1] = FNV_OFFSET32;
    HashHash32 h = {&fnv32_vt, s};
    return h;
}

HashHash32 fnv_new32a(Alloc *a) {
    FnvSum *s = BURROW_NEW(a, FnvSum);
    if (s == NULL)
        return (HashHash32){NULL, NULL};
    s->s[1] = FNV_OFFSET32;
    HashHash32 h = {&fnv32a_vt, s};
    return h;
}

HashHash64 fnv_new64(Alloc *a) {
    FnvSum *s = BURROW_NEW(a, FnvSum);
    if (s == NULL)
        return (HashHash64){NULL, NULL};
    s->s[1] = FNV_OFFSET64;
    HashHash64 h = {&fnv64_vt, s};
    return h;
}

HashHash64 fnv_new64a(Alloc *a) {
    FnvSum *s = BURROW_NEW(a, FnvSum);
    if (s == NULL)
        return (HashHash64){NULL, NULL};
    s->s[1] = FNV_OFFSET64;
    HashHash64 h = {&fnv64a_vt, s};
    return h;
}

Hash fnv_new128(Alloc *a) {
    FnvSum *s = BURROW_NEW(a, FnvSum);
    if (s == NULL)
        return (Hash){NULL, NULL};
    fnv_reset128(s);
    Hash h = {&fnv128_vt, s};
    return h;
}

Hash fnv_new128a(Alloc *a) {
    FnvSum *s = BURROW_NEW(a, FnvSum);
    if (s == NULL)
        return (Hash){NULL, NULL};
    fnv_reset128(s);
    Hash h = {&fnv128a_vt, s};
    return h;
}
