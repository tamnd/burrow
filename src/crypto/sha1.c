/* Derived from Go's src/crypto/sha1/sha1.go and sha1block.go.
 * Go source: go1.27.1.
 *
 * Copyright 2009 The Go Authors. All rights reserved.
 * Copyright 2026 The burrow Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style licence that can be found
 * in the LICENSE file. */

#include "burrow/crypto/sha1.h"

#include "burrow/core.h"
#include "burrow/hash.h"
#include "burrow/mem.h"
#include "burrow/panic.h"
#include "burrow/slice.h"
#include "burrow/type.h"

#include <string.h>

typedef struct Sha1Digest {
    uint32_t h[5];
    Byte x[SHA1_BLOCK_SIZE];
    Int nx;
    uint64_t len;
} Sha1Digest;

#define SHA1_K0 0x5A827999U
#define SHA1_K1 0x6ED9EBA1U
#define SHA1_K2 0x8F1BBCDCU
#define SHA1_K3 0xCA62C1D6U

static inline uint32_t sha1_rotl(uint32_t x, int k) {
    return (x << k) | (x >> (32 - k));
}

static inline uint32_t sha1_be32(const Byte *p) {
    return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 |
           (uint32_t)p[3];
}

static inline void sha1_put_be32(Byte *b, uint32_t v) {
    b[0] = (Byte)(v >> 24);
    b[1] = (Byte)(v >> 16);
    b[2] = (Byte)(v >> 8);
    b[3] = (Byte)v;
}

static inline void sha1_put_be64(Byte *b, uint64_t v) {
    for (int i = 0; i < 8; i++)
        b[i] = (Byte)(v >> (56 - 8 * i));
}

/* Hashes n bytes of p, which is a whole number of blocks. */
static void sha1_block(Sha1Digest *dig, const Byte *p, Int n) {
    uint32_t w[16];
    uint32_t h0 = dig->h[0], h1 = dig->h[1], h2 = dig->h[2], h3 = dig->h[3],
             h4 = dig->h[4];
    for (; n >= SHA1_BLOCK_SIZE; p += SHA1_BLOCK_SIZE, n -= SHA1_BLOCK_SIZE) {
        for (Int i = 0; i < 16; i++)
            w[i] = sha1_be32(p + 4 * i);
        uint32_t a = h0, b = h1, c = h2, d = h3, e = h4;
        uint32_t f, t, tmp;
        int i = 0;
        /* Each of the four rounds of 20 steps has its own mixing function and
         * constant. Past the first 16 steps the schedule is computed in place
         * in a ring of 16 words. */
        for (; i < 16; i++) {
            f = (b & c) | (~b & d);
            t = sha1_rotl(a, 5) + f + e + w[i & 0xf] + SHA1_K0;
            e = d;
            d = c;
            c = sha1_rotl(b, 30);
            b = a;
            a = t;
        }
        for (; i < 20; i++) {
            tmp = w[(i - 3) & 0xf] ^ w[(i - 8) & 0xf] ^ w[(i - 14) & 0xf] ^ w[i & 0xf];
            w[i & 0xf] = sha1_rotl(tmp, 1);
            f = (b & c) | (~b & d);
            t = sha1_rotl(a, 5) + f + e + w[i & 0xf] + SHA1_K0;
            e = d;
            d = c;
            c = sha1_rotl(b, 30);
            b = a;
            a = t;
        }
        for (; i < 40; i++) {
            tmp = w[(i - 3) & 0xf] ^ w[(i - 8) & 0xf] ^ w[(i - 14) & 0xf] ^ w[i & 0xf];
            w[i & 0xf] = sha1_rotl(tmp, 1);
            f = b ^ c ^ d;
            t = sha1_rotl(a, 5) + f + e + w[i & 0xf] + SHA1_K1;
            e = d;
            d = c;
            c = sha1_rotl(b, 30);
            b = a;
            a = t;
        }
        for (; i < 60; i++) {
            tmp = w[(i - 3) & 0xf] ^ w[(i - 8) & 0xf] ^ w[(i - 14) & 0xf] ^ w[i & 0xf];
            w[i & 0xf] = sha1_rotl(tmp, 1);
            f = ((b | c) & d) | (b & c);
            t = sha1_rotl(a, 5) + f + e + w[i & 0xf] + SHA1_K2;
            e = d;
            d = c;
            c = sha1_rotl(b, 30);
            b = a;
            a = t;
        }
        for (; i < 80; i++) {
            tmp = w[(i - 3) & 0xf] ^ w[(i - 8) & 0xf] ^ w[(i - 14) & 0xf] ^ w[i & 0xf];
            w[i & 0xf] = sha1_rotl(tmp, 1);
            f = b ^ c ^ d;
            t = sha1_rotl(a, 5) + f + e + w[i & 0xf] + SHA1_K3;
            e = d;
            d = c;
            c = sha1_rotl(b, 30);
            b = a;
            a = t;
        }
        h0 += a;
        h1 += b;
        h2 += c;
        h3 += d;
        h4 += e;
    }
    dig->h[0] = h0;
    dig->h[1] = h1;
    dig->h[2] = h2;
    dig->h[3] = h3;
    dig->h[4] = h4;
}

static void sha1_reset(void *self) {
    Sha1Digest *d = self;
    d->h[0] = 0x67452301U;
    d->h[1] = 0xEFCDAB89U;
    d->h[2] = 0x98BADCFEU;
    d->h[3] = 0x10325476U;
    d->h[4] = 0xC3D2E1F0U;
    d->nx = 0;
    d->len = 0;
}

static void sha1_update(Sha1Digest *d, const Byte *p, Int n);

static void sha1_check_sum(Sha1Digest *d, Byte out[SHA1_SIZE]) {
    /* Padding: add 1 bit and 0 bits until 56 bytes mod 64, then the length in
     * bits as a big endian 64 bit number. */
    uint64_t len = d->len;
    Byte tmp[64 + 8] = {0x80};
    uint64_t t = len % 64 < 56 ? 56 - len % 64 : 64 + 56 - len % 64;
    sha1_put_be64(tmp + t, len << 3);
    sha1_update(d, tmp, (Int)(t + 8));
    if (d->nx != 0)
        panic_str(BURROW_S("d.nx != 0"));
    for (Int i = 0; i < 5; i++)
        sha1_put_be32(out + 4 * i, d->h[i]);
}

static void sha1_update(Sha1Digest *d, const Byte *p, Int n) {
    d->len += (uint64_t)n;
    if (d->nx > 0) {
        Int k = SHA1_BLOCK_SIZE - d->nx < n ? SHA1_BLOCK_SIZE - d->nx : n;
        memcpy(d->x + d->nx, p, (size_t)k);
        d->nx += k;
        if (d->nx == SHA1_BLOCK_SIZE) {
            sha1_block(d, d->x, SHA1_BLOCK_SIZE);
            d->nx = 0;
        }
        p += k;
        n -= k;
    }
    if (n >= SHA1_BLOCK_SIZE) {
        Int k = n & ~(Int)(SHA1_BLOCK_SIZE - 1);
        sha1_block(d, p, k);
        p += k;
        n -= k;
    }
    if (n > 0) {
        memcpy(d->x, p, (size_t)n);
        d->nx = n;
    }
}

static Int sha1_write(void *self, Slice p, Error *err) {
    sha1_update(self, (const Byte *)p.p, p.len);
    BURROW_OUT(err, BURROW_NO_ERROR);
    return p.len;
}

static Slice sha1_sum_append(void *self, Alloc *a, Slice in) {
    /* Work on a copy so the caller can keep writing. */
    Sha1Digest d0 = *(Sha1Digest *)self;
    Byte h[SHA1_SIZE];
    sha1_check_sum(&d0, h);
    if (in.elem == NULL)
        in = slice_nil(TYPE_BYTE);
    return slice_append(a, in, h, SHA1_SIZE);
}

static Int sha1_block_size(void *self) {
    (void)self;
    return SHA1_BLOCK_SIZE;
}

static Int sha1_size(void *self) {
    (void)self;
    return SHA1_SIZE;
}

static const HashVT sha1_vt = {
    {NULL, sha1_write}, sha1_sum_append, sha1_reset, sha1_size, sha1_block_size,
};

Hash sha1_new(Alloc *a) {
    Sha1Digest *d = BURROW_NEW(a, Sha1Digest);
    if (d == NULL)
        return (Hash){NULL, NULL};
    sha1_reset(d);
    Hash h = {&sha1_vt, d};
    return h;
}

Sha1SumRet sha1_sum(Slice data) {
    Sha1Digest d;
    Sha1SumRet r;
    sha1_reset(&d);
    sha1_update(&d, (const Byte *)data.p, data.len);
    sha1_check_sum(&d, r.a);
    return r;
}
