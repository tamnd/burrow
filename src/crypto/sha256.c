/* Derived from Go's src/crypto/internal/fips140/sha256/sha256.go and
 * sha256block.go.
 * Go source: go1.27.1.
 *
 * Copyright 2009 The Go Authors. All rights reserved.
 * Copyright 2026 The burrow Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style licence that can be found
 * in the LICENSE file. */

#include "burrow/crypto/sha256.h"

#include "burrow/core.h"
#include "burrow/hash.h"
#include "burrow/mem.h"
#include "burrow/panic.h"
#include "burrow/slice.h"
#include "burrow/type.h"

#include <string.h>

typedef struct Sha256Digest {
    uint32_t h[8];
    Byte x[SHA256_BLOCK_SIZE];
    Int nx;
    uint64_t len;
    bool is224;
} Sha256Digest;

static const uint32_t sha256_k[64] = {
    0x428A2F98U, 0x71374491U, 0xB5C0FBCFU, 0xE9B5DBA5U, 0x3956C25BU, 0x59F111F1U,
    0x923F82A4U, 0xAB1C5ED5U, 0xD807AA98U, 0x12835B01U, 0x243185BEU, 0x550C7DC3U,
    0x72BE5D74U, 0x80DEB1FEU, 0x9BDC06A7U, 0xC19BF174U, 0xE49B69C1U, 0xEFBE4786U,
    0x0FC19DC6U, 0x240CA1CCU, 0x2DE92C6FU, 0x4A7484AAU, 0x5CB0A9DCU, 0x76F988DAU,
    0x983E5152U, 0xA831C66DU, 0xB00327C8U, 0xBF597FC7U, 0xC6E00BF3U, 0xD5A79147U,
    0x06CA6351U, 0x14292967U, 0x27B70A85U, 0x2E1B2138U, 0x4D2C6DFCU, 0x53380D13U,
    0x650A7354U, 0x766A0ABBU, 0x81C2C92EU, 0x92722C85U, 0xA2BFE8A1U, 0xA81A664BU,
    0xC24B8B70U, 0xC76C51A3U, 0xD192E819U, 0xD6990624U, 0xF40E3585U, 0x106AA070U,
    0x19A4C116U, 0x1E376C08U, 0x2748774CU, 0x34B0BCB5U, 0x391C0CB3U, 0x4ED8AA4AU,
    0x5B9CCA4FU, 0x682E6FF3U, 0x748F82EEU, 0x78A5636FU, 0x84C87814U, 0x8CC70208U,
    0x90BEFFFAU, 0xA4506CEBU, 0xBEF9A3F7U, 0xC67178F2U,
};

static inline uint32_t sha256_rotr(uint32_t x, int k) {
    return (x >> k) | (x << (32 - k));
}

static inline uint32_t sha256_be32(const Byte *p) {
    return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 |
           (uint32_t)p[3];
}

static inline void sha256_put_be32(Byte *b, uint32_t v) {
    b[0] = (Byte)(v >> 24);
    b[1] = (Byte)(v >> 16);
    b[2] = (Byte)(v >> 8);
    b[3] = (Byte)v;
}

static inline void sha256_put_be64(Byte *b, uint64_t v) {
    for (int i = 0; i < 8; i++)
        b[i] = (Byte)(v >> (56 - 8 * i));
}

/* One round. The caller rotates the roles of the eight variables instead of
 * moving their values: d takes e's new value and h takes a's. */
#define SHA256_ROUND(a, b, c, d, e, f, g, h, i)                                        \
    do {                                                                               \
        uint32_t t1_ = (h) +                                                           \
                       (sha256_rotr(e, 6) ^ sha256_rotr(e, 11) ^ sha256_rotr(e, 25)) + \
                       (((e) & (f)) ^ (~(e) & (g))) + sha256_k[i] + w[i];              \
        uint32_t t2_ = (sha256_rotr(a, 2) ^ sha256_rotr(a, 13) ^ sha256_rotr(a, 22)) + \
                       (((a) & (b)) ^ ((a) & (c)) ^ ((b) & (c)));                      \
        (d) += t1_;                                                                    \
        (h) = t1_ + t2_;                                                               \
    } while (0)

/* Hashes n bytes of p, which is a whole number of blocks. */
static void sha256_block(Sha256Digest *dig, const Byte *p, Int n) {
    uint32_t w[64];
    uint32_t h0 = dig->h[0], h1 = dig->h[1], h2 = dig->h[2], h3 = dig->h[3];
    uint32_t h4 = dig->h[4], h5 = dig->h[5], h6 = dig->h[6], h7 = dig->h[7];
    for (; n >= SHA256_BLOCK_SIZE; p += SHA256_BLOCK_SIZE, n -= SHA256_BLOCK_SIZE) {
        for (Int i = 0; i < 16; i++)
            w[i] = sha256_be32(p + 4 * i);
        for (int i = 16; i < 64; i++) {
            uint32_t v1 = w[i - 2];
            uint32_t t1 = sha256_rotr(v1, 17) ^ sha256_rotr(v1, 19) ^ (v1 >> 10);
            uint32_t v2 = w[i - 15];
            uint32_t t2 = sha256_rotr(v2, 7) ^ sha256_rotr(v2, 18) ^ (v2 >> 3);
            w[i] = t1 + w[i - 7] + t2 + w[i - 16];
        }
        uint32_t a = h0, b = h1, c = h2, d = h3, e = h4, f = h5, g = h6, h = h7;
        /* Go's loop shifts the eight working variables down one place per
         * round. Unrolling by eight and renaming instead leaves the compiler
         * nothing to move. */
        for (int i = 0; i < 64; i += 8) {
            SHA256_ROUND(a, b, c, d, e, f, g, h, i);
            SHA256_ROUND(h, a, b, c, d, e, f, g, i + 1);
            SHA256_ROUND(g, h, a, b, c, d, e, f, i + 2);
            SHA256_ROUND(f, g, h, a, b, c, d, e, i + 3);
            SHA256_ROUND(e, f, g, h, a, b, c, d, i + 4);
            SHA256_ROUND(d, e, f, g, h, a, b, c, i + 5);
            SHA256_ROUND(c, d, e, f, g, h, a, b, i + 6);
            SHA256_ROUND(b, c, d, e, f, g, h, a, i + 7);
        }
        h0 += a;
        h1 += b;
        h2 += c;
        h3 += d;
        h4 += e;
        h5 += f;
        h6 += g;
        h7 += h;
    }
    dig->h[0] = h0;
    dig->h[1] = h1;
    dig->h[2] = h2;
    dig->h[3] = h3;
    dig->h[4] = h4;
    dig->h[5] = h5;
    dig->h[6] = h6;
    dig->h[7] = h7;
}

static void sha256_reset(void *self) {
    Sha256Digest *d = self;
    static const uint32_t init256[8] = {
        0x6A09E667U, 0xBB67AE85U, 0x3C6EF372U, 0xA54FF53AU,
        0x510E527FU, 0x9B05688CU, 0x1F83D9ABU, 0x5BE0CD19U,
    };
    static const uint32_t init224[8] = {
        0xC1059ED8U, 0x367CD507U, 0x3070DD17U, 0xF70E5939U,
        0xFFC00B31U, 0x68581511U, 0x64F98FA7U, 0xBEFA4FA4U,
    };
    memcpy(d->h, d->is224 ? init224 : init256, sizeof d->h);
    d->nx = 0;
    d->len = 0;
}

static void sha256_update(Sha256Digest *d, const Byte *p, Int n);

/* Always writes all 32 bytes. SHA-224 keeps the first 28. */
static void sha256_check_sum(Sha256Digest *d, Byte out[SHA256_SIZE]) {
    /* Padding: add 1 bit and 0 bits until 56 bytes mod 64, then the length in
     * bits as a big endian 64 bit number. */
    uint64_t len = d->len;
    Byte tmp[64 + 8] = {0x80};
    uint64_t t = len % 64 < 56 ? 56 - len % 64 : 64 + 56 - len % 64;
    sha256_put_be64(tmp + t, len << 3);
    sha256_update(d, tmp, (Int)(t + 8));
    if (d->nx != 0)
        panic_str(BURROW_S("d.nx != 0"));
    for (Int i = 0; i < 8; i++)
        sha256_put_be32(out + 4 * i, d->h[i]);
}

static void sha256_update(Sha256Digest *d, const Byte *p, Int n) {
    d->len += (uint64_t)n;
    if (d->nx > 0) {
        Int k = SHA256_BLOCK_SIZE - d->nx < n ? SHA256_BLOCK_SIZE - d->nx : n;
        memcpy(d->x + d->nx, p, (size_t)k);
        d->nx += k;
        if (d->nx == SHA256_BLOCK_SIZE) {
            sha256_block(d, d->x, SHA256_BLOCK_SIZE);
            d->nx = 0;
        }
        p += k;
        n -= k;
    }
    if (n >= SHA256_BLOCK_SIZE) {
        Int k = n & ~(Int)(SHA256_BLOCK_SIZE - 1);
        sha256_block(d, p, k);
        p += k;
        n -= k;
    }
    if (n > 0) {
        memcpy(d->x, p, (size_t)n);
        d->nx = n;
    }
}

static Int sha256_write(void *self, Slice p, Error *err) {
    sha256_update(self, (const Byte *)p.p, p.len);
    BURROW_OUT(err, BURROW_NO_ERROR);
    return p.len;
}

static Slice sha256_sum_append(void *self, Alloc *a, Slice in) {
    /* Work on a copy so the caller can keep writing. */
    Sha256Digest d0 = *(Sha256Digest *)self;
    Byte h[SHA256_SIZE];
    sha256_check_sum(&d0, h);
    if (in.elem == NULL)
        in = slice_nil(TYPE_BYTE);
    return slice_append(a, in, h,
                        ((Sha256Digest *)self)->is224 ? SHA256_SIZE224 : SHA256_SIZE);
}

static Int sha256_block_size(void *self) {
    (void)self;
    return SHA256_BLOCK_SIZE;
}

static Int sha256_size(void *self) {
    return ((Sha256Digest *)self)->is224 ? SHA256_SIZE224 : SHA256_SIZE;
}

static const HashVT sha256_vt = {
    {NULL, sha256_write}, sha256_sum_append, sha256_reset,
    sha256_size,          sha256_block_size,
};

static Hash sha256_make(Alloc *a, bool is224) {
    Sha256Digest *d = BURROW_NEW(a, Sha256Digest);
    if (d == NULL)
        return (Hash){NULL, NULL};
    d->is224 = is224;
    sha256_reset(d);
    Hash h = {&sha256_vt, d};
    return h;
}

Hash sha256_new(Alloc *a) {
    return sha256_make(a, false);
}

Hash sha256_new224(Alloc *a) {
    return sha256_make(a, true);
}

Sha256Sum256Ret sha256_sum256(Slice data) {
    Sha256Digest d;
    Sha256Sum256Ret r;
    d.is224 = false;
    sha256_reset(&d);
    sha256_update(&d, (const Byte *)data.p, data.len);
    sha256_check_sum(&d, r.a);
    return r;
}

Sha256Sum224Ret sha256_sum224(Slice data) {
    Sha256Digest d;
    Sha256Sum224Ret r;
    Byte h[SHA256_SIZE];
    d.is224 = true;
    sha256_reset(&d);
    sha256_update(&d, (const Byte *)data.p, data.len);
    sha256_check_sum(&d, h);
    memcpy(r.a, h, sizeof r.a);
    return r;
}
