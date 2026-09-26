/* hash/maphash, derived from Go's src/hash/maphash/maphash.go and
 * maphash_runtime.go (go1.27.1).
 *
 * Copyright 2019 The Go Authors. All rights reserved.
 * Copyright 2026 The burrow Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style licence that can be found
 * in the LICENSE file. */

#include "burrow/hash/maphash.h"

#include "burrow/core.h"
#include "burrow/hash.h"
#include "burrow/mem.h"
#include "burrow/panic.h"
#include "burrow/runtime.h"
#include "burrow/slice.h"
#include "burrow/type.h"

#include <string.h>

static void maphash_uninitialized(void) {
    panic_str(BURROW_S("maphash: use of uninitialized Seed"));
}

/* Go's rthash: an empty buffer leaves the state as it was. */
static uint64_t rthash(const Byte *p, Int n, uint64_t seed) {
    if (n == 0)
        return seed;
    return runtime_memhash(p, (size_t)n, seed);
}

/* Bytes and String hash in blocks of MAPHASH_BUF_SIZE, so that they agree with
 * the streaming hash, which flushes its buffer in blocks of that size. */
static uint64_t maphash_raw(MaphashSeed seed, const Byte *p, Int n) {
    uint64_t state = seed.s;
    if (state == 0)
        maphash_uninitialized();
    while (n > MAPHASH_BUF_SIZE) {
        state = rthash(p, MAPHASH_BUF_SIZE, state);
        p += MAPHASH_BUF_SIZE;
        n -= MAPHASH_BUF_SIZE;
    }
    return rthash(p, n, state);
}

uint64_t maphash_bytes(MaphashSeed seed, Slice b) {
    return maphash_raw(seed, (const Byte *)b.p, b.len);
}

uint64_t maphash_string(MaphashSeed seed, Str s) {
    return maphash_raw(seed, s.p, s.len);
}

MaphashSeed maphash_make_seed(void) {
    uint64_t s;
    do
        s = runtime_rand64();
    while (s == 0);
    return (MaphashSeed){s};
}

/* The zero MaphashHash picks its seed the first time it needs one. */
static void init_seed(MaphashHash *h) {
    if (h->seed.s == 0) {
        MaphashSeed seed = maphash_make_seed();
        h->seed = seed;
        h->state = seed;
    }
}

static void flush(MaphashHash *h) {
    if (h->n != MAPHASH_BUF_SIZE)
        panic_str(BURROW_S("maphash: flush of partially full buffer"));
    init_seed(h);
    h->state.s = rthash(h->buf, h->n, h->state.s);
    h->n = 0;
}

static void write_raw(MaphashHash *h, const Byte *p, Int n) {
    if (h->n > 0 && h->n <= MAPHASH_BUF_SIZE) {
        Int k = MAPHASH_BUF_SIZE - h->n < n ? MAPHASH_BUF_SIZE - h->n : n;
        memcpy(h->buf + h->n, p, (size_t)k);
        h->n += k;
        if (h->n < MAPHASH_BUF_SIZE)
            return;
        p += k;
        n -= k;
        flush(h);
    }
    if (n > MAPHASH_BUF_SIZE) {
        init_seed(h);
        while (n > MAPHASH_BUF_SIZE) {
            h->state.s = rthash(p, MAPHASH_BUF_SIZE, h->state.s);
            p += MAPHASH_BUF_SIZE;
            n -= MAPHASH_BUF_SIZE;
        }
    }
    if (n > 0)
        memcpy(h->buf, p, (size_t)n);
    h->n = n;
}

Int maphash_hash_write(MaphashHash *h, Slice b, Error *err) {
    write_raw(h, (const Byte *)b.p, b.len);
    BURROW_OUT(err, BURROW_NO_ERROR);
    return b.len;
}

Int maphash_hash_write_string(MaphashHash *h, Str s, Error *err) {
    write_raw(h, s.p, s.len);
    BURROW_OUT(err, BURROW_NO_ERROR);
    return s.len;
}

Error maphash_hash_write_byte(MaphashHash *h, Byte c) {
    if (h->n == MAPHASH_BUF_SIZE)
        flush(h);
    h->buf[h->n++] = c;
    return BURROW_NO_ERROR;
}

MaphashSeed maphash_hash_seed(MaphashHash *h) {
    init_seed(h);
    return h->seed;
}

void maphash_hash_set_seed(MaphashHash *h, MaphashSeed seed) {
    if (seed.s == 0)
        maphash_uninitialized();
    h->seed = seed;
    h->state = seed;
    h->n = 0;
}

void maphash_hash_reset(MaphashHash *h) {
    init_seed(h);
    h->state = h->seed;
    h->n = 0;
}

uint64_t maphash_hash_sum64(MaphashHash *h) {
    init_seed(h);
    return rthash(h->buf, h->n, h->state.s);
}

Slice maphash_hash_sum(MaphashHash *h, Alloc *a, Slice b) {
    uint64_t x = maphash_hash_sum64(h);
    Byte le[8];
    for (int i = 0; i < 8; i++)
        le[i] = (Byte)(x >> (8 * i));
    if (b.elem == NULL)
        b = slice_nil(TYPE_BYTE);
    return slice_append(a, b, le, 8);
}

Int maphash_hash_size(const MaphashHash *h) {
    (void)h;
    return 8;
}

Int maphash_hash_block_size(const MaphashHash *h) {
    (void)h;
    return MAPHASH_BUF_SIZE;
}

/* ---------------------------------------------------------- comparable */

/* Go's comparableHash. Go hashes through the map's hasher for T, and the
 * map's hasher here is type_hash, so this is the same thing. */
static uint64_t comparable_hash(const Type *t, const void *v, uint64_t seed) {
    if (!type_is_comparable(t))
        runtime_panic_unhashable(t);
    return type_hash(t, v, seed);
}

uint64_t maphash_comparable(MaphashSeed seed, const Type *t, const void *v) {
    return comparable_hash(t, v, seed.s);
}

/* Go folds the count of buffered bytes into the state first, so that a value
 * written after some bytes does not hash the same as one written before them,
 * and leaves the bytes where they are. */
void maphash_write_comparable(MaphashHash *h, const Type *t, const void *v) {
    if (h->n != 0) {
        Int n = h->n;
        h->state.s = comparable_hash(TYPE_INT, &n, h->state.s);
    }
    h->state.s = comparable_hash(t, v, h->state.s);
}

/* ---------------------------------------------------------- interfaces */

static Int vt_write(void *self, Slice p, Error *err) {
    return maphash_hash_write(self, p, err);
}

static Slice vt_sum(void *self, Alloc *a, Slice b) {
    return maphash_hash_sum(self, a, b);
}

static void vt_reset(void *self) {
    maphash_hash_reset(self);
}

static Int vt_size(void *self) {
    return maphash_hash_size(self);
}

static Int vt_block_size(void *self) {
    return maphash_hash_block_size(self);
}

static uint64_t vt_sum64(void *self) {
    return maphash_hash_sum64(self);
}

#define MAPHASH_HASH_VT {{NULL, vt_write}, vt_sum, vt_reset, vt_size, vt_block_size}

static const HashHash64VT hash64_vt = {MAPHASH_HASH_VT, vt_sum64};

static HashCloner vt_clone(void *self, Alloc *a, Error *err);

static const HashClonerVT cloner_vt = {MAPHASH_HASH_VT, vt_clone};

static HashCloner vt_clone(void *self, Alloc *a, Error *err) {
    return maphash_hash_clone(self, a, err);
}

HashCloner maphash_hash_clone(MaphashHash *h, Alloc *a, Error *err) {
    MaphashHash *r = BURROW_NEW(a, MaphashHash);
    if (r == NULL) {
        BURROW_OUT(err, burrow_err_out_of_memory);
        return (HashCloner){NULL, NULL};
    }
    init_seed(h);
    *r = *h;
    BURROW_OUT(err, BURROW_NO_ERROR);
    return (HashCloner){&cloner_vt, r};
}

Hash maphash_hash_as_hash(MaphashHash *h) {
    return (Hash){&hash64_vt.hash, h};
}

HashHash64 maphash_hash_as_hash64(MaphashHash *h) {
    return (HashHash64){&hash64_vt, h};
}

HashCloner maphash_hash_as_cloner(MaphashHash *h) {
    return (HashCloner){&cloner_vt, h};
}

/* ---------------------------------------------------------- hashers */

void maphash_comparable_hasher_hash(MaphashComparableHasher ch, MaphashHash *h,
                                    const void *v) {
    maphash_write_comparable(h, ch.t, v);
}

bool maphash_comparable_hasher_equal(MaphashComparableHasher ch, const void *x,
                                     const void *y) {
    return type_equal(ch.t, x, y);
}

static void ch_hash(void *self, MaphashHash *h, const void *v) {
    maphash_comparable_hasher_hash(*(const MaphashComparableHasher *)self, h, v);
}

static bool ch_equal(void *self, const void *x, const void *y) {
    return maphash_comparable_hasher_equal(*(const MaphashComparableHasher *)self, x,
                                           y);
}

static const MaphashHasherVT comparable_hasher_vt = {ch_hash, ch_equal};

MaphashHasher maphash_comparable_hasher_as_hasher(MaphashComparableHasher *ch) {
    return (MaphashHasher){&comparable_hasher_vt, ch};
}
