/* Derived from Go's src/hash/fnv/fnv.go.
 * Go source: go1.27.1.
 *
 * Copyright 2011 The Go Authors. All rights reserved.
 * Copyright 2026 The burrow Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style licence that can be found
 * in the LICENSE file. */

#include "burrow/hash/fnv.h"
#include "burrow/core.h"
#include "burrow/hash.h"
#include "burrow/math/bits.h"
#include "burrow/mem.h"
#include "burrow/slice.h"
#include "burrow/type.h"

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

static const HashHash32VT fnv32_vt = {
    {{NULL, fnv_write32}, fnv_sum_32, fnv_reset32, fnv_size32, fnv_block_size},
    fnv_sum32,
};

static const HashHash32VT fnv32a_vt = {
    {{NULL, fnv_write32a}, fnv_sum_32, fnv_reset32, fnv_size32, fnv_block_size},
    fnv_sum32,
};

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

static const HashHash64VT fnv64_vt = {
    {{NULL, fnv_write64}, fnv_sum_64, fnv_reset64, fnv_size64, fnv_block_size},
    fnv_sum64,
};

static const HashHash64VT fnv64a_vt = {
    {{NULL, fnv_write64a}, fnv_sum_64, fnv_reset64, fnv_size64, fnv_block_size},
    fnv_sum64,
};

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

static const HashVT fnv128_vt = {
    {NULL, fnv_write128}, fnv_sum_128, fnv_reset128, fnv_size128, fnv_block_size,
};

static const HashVT fnv128a_vt = {
    {NULL, fnv_write128a}, fnv_sum_128, fnv_reset128, fnv_size128, fnv_block_size,
};

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
