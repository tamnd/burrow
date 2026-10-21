/* Derived from Go's src/crypto/hmac/hmac.go and
 * src/crypto/internal/fips140/hmac/hmac.go.
 * Go source: go1.27.1.
 *
 * Copyright 2009 The Go Authors. All rights reserved.
 * Copyright 2026 The burrow Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style licence that can be found
 * in the LICENSE file. */

#include "burrow/crypto/hmac.h"

#include "burrow/core.h"
#include "burrow/crypto/sha256.h"
#include "burrow/crypto/subtle.h"
#include "burrow/hash.h"
#include "burrow/mem.h"
#include "burrow/mem/arena.h"
#include "burrow/panic.h"
#include "burrow/slice.h"
#include "burrow/type.h"

#include "fips140_module.h"

#include <string.h>

/* The inner hash has the key xor ipad written into it already and takes the
 * message. Sum finishes it and feeds the result to the outer hash after the key
 * xor opad. */
typedef struct Hmac {
    Hash inner;
    Hash outer;
    Byte *ipad;
    Byte *opad;
    Int block_size;
} Hmac;

CRYPTO_FIPS_TEXT static Int hmac_write(void *self, Slice p, Error *err) {
    return hash_write(((Hmac *)self)->inner, p, err);
}

CRYPTO_FIPS_TEXT static Slice hmac_sum(void *self, Alloc *a, Slice in) {
    Hmac *h = self;
    if (in.elem == NULL)
        in = slice_nil(TYPE_BYTE);
    Int orig = in.len;
    in = hash_sum(a, h->inner, in);
    hash_reset(h->outer);
    hash_write(h->outer, slice_from(h->opad, h->block_size, h->block_size, TYPE_BYTE),
               NULL);
    hash_write(h->outer, slice_sub(in, orig, in.len), NULL);
    /* The outer sum goes where the inner one was, which it has finished
     * reading. */
    return hash_sum(a, h->outer, slice_sub(in, 0, orig));
}

CRYPTO_FIPS_TEXT static void hmac_reset(void *self) {
    Hmac *h = self;
    hash_reset(h->inner);
    hash_write(h->inner, slice_from(h->ipad, h->block_size, h->block_size, TYPE_BYTE),
               NULL);
}

CRYPTO_FIPS_TEXT static Int hmac_size(void *self) {
    return hash_size(((Hmac *)self)->outer);
}

CRYPTO_FIPS_TEXT static Int hmac_block_size(void *self) {
    return hash_block_size(((Hmac *)self)->inner);
}

static const HashVT hmac_vt = {
    {NULL, hmac_write}, hmac_sum, hmac_reset, hmac_size, hmac_block_size,
};

CRYPTO_FIPS_TEXT Hash hmac_new(Alloc *a, HashNewFunc h, Slice key) {
    Hash nil = {NULL, NULL};
    Hmac *hm = BURROW_NEW(a, Hmac);
    if (hm == NULL)
        return nil;
    hm->outer = h(a);
    hm->inner = h(a);
    if (hm->outer.vt == NULL || hm->inner.vt == NULL)
        return nil;
    if (hm->outer.vt == hm->inner.vt && hm->outer.data == hm->inner.data)
        panic_str(
            BURROW_S("crypto/hmac: hash generation function does not produce unique "
                     "values"));
    Int bs = hash_block_size(hm->inner);
    hm->block_size = bs;
    hm->ipad = BURROW_NEW_N(a, Byte, (size_t)bs);
    hm->opad = BURROW_NEW_N(a, Byte, (size_t)bs);
    if (hm->ipad == NULL || hm->opad == NULL)
        return nil;
    if (key.len > bs) {
        hash_write(hm->outer, key, NULL);
        key = hash_sum(a, hm->outer, slice_nil(TYPE_BYTE));
        if (key.len == 0)
            return nil;
    }
    if (key.len > 0)
        memcpy(hm->ipad, key.p, (size_t)key.len);
    memcpy(hm->opad, hm->ipad, (size_t)bs);
    for (Int i = 0; i < bs; i++) {
        hm->ipad[i] ^= 0x36;
        hm->opad[i] ^= 0x5c;
    }
    hash_write(hm->inner, slice_from(hm->ipad, bs, bs, TYPE_BYTE), NULL);
    Hash r = {&hmac_vt, hm};
    return r;
}

CRYPTO_FIPS_TEXT bool hmac_equal(Slice mac1, Slice mac2) {
    return subtle_constant_time_compare(mac1, mac2) == 1;
}

/* ------------------------------------------------------------------- CAST */

/* From Go's src/crypto/internal/fips140/hmac/cast.go. */

CRYPTO_FIPS_RODATA static const Byte hmac_cast_input[16] = {
    0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08,
    0x09, 0x0a, 0x0b, 0x0c, 0x0d, 0x0e, 0x0f, 0x10,
};

CRYPTO_FIPS_RODATA static const Byte hmac_cast_want[32] = {
    0xf0, 0x8d, 0x82, 0x8d, 0x4c, 0x9e, 0xad, 0x3d, 0xdc, 0x12, 0x9c,
    0x4e, 0x70, 0xc4, 0x19, 0x2a, 0x4f, 0x12, 0x73, 0x23, 0x73, 0x77,
    0x66, 0x05, 0x10, 0xee, 0x57, 0x6b, 0x3a, 0xc7, 0x14, 0x41,
};

CRYPTO_FIPS_TEXT Error burrow__hmac_cast(void *arg) {
    (void)arg;
    Int n = (Int)sizeof hmac_cast_input;
    Slice input = slice_from((void *)(uintptr_t)hmac_cast_input, n, n, TYPE_BYTE);
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    Hash h = hmac_new(a, sha256_new, input);
    if (h.vt == NULL) {
        arena_free(&ar);
        return burrow_err_out_of_memory;
    }
    hash_write(h, input, NULL);
    hash_write(h, input, NULL);
    Slice got = hash_sum(a, h, (Slice){0});
    bool ok = got.len == (Int)sizeof hmac_cast_want &&
              memcmp(got.p, hmac_cast_want, sizeof hmac_cast_want) == 0;
    arena_free(&ar);
    return ok ? BURROW_NO_ERROR
              : errors_new(error_allocator(), BURROW_S("unexpected result"));
}
