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
#include "burrow/crypto/subtle.h"
#include "burrow/hash.h"
#include "burrow/mem.h"
#include "burrow/panic.h"
#include "burrow/slice.h"
#include "burrow/type.h"

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

static Int hmac_write(void *self, Slice p, Error *err) {
    return hash_write(((Hmac *)self)->inner, p, err);
}

static Slice hmac_sum(void *self, Alloc *a, Slice in) {
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

static void hmac_reset(void *self) {
    Hmac *h = self;
    hash_reset(h->inner);
    hash_write(h->inner, slice_from(h->ipad, h->block_size, h->block_size, TYPE_BYTE),
               NULL);
}

static Int hmac_size(void *self) {
    return hash_size(((Hmac *)self)->outer);
}

static Int hmac_block_size(void *self) {
    return hash_block_size(((Hmac *)self)->inner);
}

static const HashVT hmac_vt = {
    {NULL, hmac_write}, hmac_sum, hmac_reset, hmac_size, hmac_block_size,
};

Hash hmac_new(Alloc *a, HashNewFunc h, Slice key) {
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

bool hmac_equal(Slice mac1, Slice mac2) {
    return subtle_constant_time_compare(mac1, mac2) == 1;
}
