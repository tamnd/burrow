/* Derived from Go's src/crypto/internal/fips140/sha512/sha512.go and
 * sha512block.go.
 * Go source: go1.27.1.
 *
 * Copyright 2009 The Go Authors. All rights reserved.
 * Copyright 2026 The burrow Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style licence that can be found
 * in the LICENSE file. */

#include "burrow/crypto/sha512.h"

#include "burrow/core.h"
#include "burrow/hash.h"
#include "burrow/mem.h"
#include "burrow/panic.h"
#include "burrow/slice.h"
#include "burrow/type.h"

#include <string.h>

typedef struct Sha512Digest {
    uint64_t h[8];
    Byte x[SHA512_BLOCK_SIZE];
    Int nx;
    uint64_t len;
    Int size;
} Sha512Digest;

static const uint64_t sha512_k[80] = {
    0x428A2F98D728AE22ULL, 0x7137449123EF65CDULL, 0xB5C0FBCFEC4D3B2FULL,
    0xE9B5DBA58189DBBCULL, 0x3956C25BF348B538ULL, 0x59F111F1B605D019ULL,
    0x923F82A4AF194F9BULL, 0xAB1C5ED5DA6D8118ULL, 0xD807AA98A3030242ULL,
    0x12835B0145706FBEULL, 0x243185BE4EE4B28CULL, 0x550C7DC3D5FFB4E2ULL,
    0x72BE5D74F27B896FULL, 0x80DEB1FE3B1696B1ULL, 0x9BDC06A725C71235ULL,
    0xC19BF174CF692694ULL, 0xE49B69C19EF14AD2ULL, 0xEFBE4786384F25E3ULL,
    0x0FC19DC68B8CD5B5ULL, 0x240CA1CC77AC9C65ULL, 0x2DE92C6F592B0275ULL,
    0x4A7484AA6EA6E483ULL, 0x5CB0A9DCBD41FBD4ULL, 0x76F988DA831153B5ULL,
    0x983E5152EE66DFABULL, 0xA831C66D2DB43210ULL, 0xB00327C898FB213FULL,
    0xBF597FC7BEEF0EE4ULL, 0xC6E00BF33DA88FC2ULL, 0xD5A79147930AA725ULL,
    0x06CA6351E003826FULL, 0x142929670A0E6E70ULL, 0x27B70A8546D22FFCULL,
    0x2E1B21385C26C926ULL, 0x4D2C6DFC5AC42AEDULL, 0x53380D139D95B3DFULL,
    0x650A73548BAF63DEULL, 0x766A0ABB3C77B2A8ULL, 0x81C2C92E47EDAEE6ULL,
    0x92722C851482353BULL, 0xA2BFE8A14CF10364ULL, 0xA81A664BBC423001ULL,
    0xC24B8B70D0F89791ULL, 0xC76C51A30654BE30ULL, 0xD192E819D6EF5218ULL,
    0xD69906245565A910ULL, 0xF40E35855771202AULL, 0x106AA07032BBD1B8ULL,
    0x19A4C116B8D2D0C8ULL, 0x1E376C085141AB53ULL, 0x2748774CDF8EEB99ULL,
    0x34B0BCB5E19B48A8ULL, 0x391C0CB3C5C95A63ULL, 0x4ED8AA4AE3418ACBULL,
    0x5B9CCA4F7763E373ULL, 0x682E6FF3D6B2B8A3ULL, 0x748F82EE5DEFB2FCULL,
    0x78A5636F43172F60ULL, 0x84C87814A1F0AB72ULL, 0x8CC702081A6439ECULL,
    0x90BEFFFA23631E28ULL, 0xA4506CEBDE82BDE9ULL, 0xBEF9A3F7B2C67915ULL,
    0xC67178F2E372532BULL, 0xCA273ECEEA26619CULL, 0xD186B8C721C0C207ULL,
    0xEADA7DD6CDE0EB1EULL, 0xF57D4F7FEE6ED178ULL, 0x06F067AA72176FBAULL,
    0x0A637DC5A2C898A6ULL, 0x113F9804BEF90DAEULL, 0x1B710B35131C471BULL,
    0x28DB77F523047D84ULL, 0x32CAAB7B40C72493ULL, 0x3C9EBE0A15C9BEBCULL,
    0x431D67C49C100D4CULL, 0x4CC5D4BECB3E42B6ULL, 0x597F299CFC657E2AULL,
    0x5FCB6FAB3AD6FAECULL, 0x6C44198C4A475817ULL,
};

static inline uint64_t sha512_rotr(uint64_t x, int k) {
    return (x >> k) | (x << (64 - k));
}

static inline uint64_t sha512_be64(const Byte *p) {
    return (uint64_t)p[0] << 56 | (uint64_t)p[1] << 48 | (uint64_t)p[2] << 40 |
           (uint64_t)p[3] << 32 | (uint64_t)p[4] << 24 | (uint64_t)p[5] << 16 |
           (uint64_t)p[6] << 8 | (uint64_t)p[7];
}

static inline void sha512_put_be64(Byte *b, uint64_t v) {
    for (int i = 0; i < 8; i++)
        b[i] = (Byte)(v >> (56 - 8 * i));
}

/* One round. The caller rotates the roles of the eight variables instead of
 * moving their values: d takes e's new value and h takes a's. */
#define SHA512_ROUND(a, b, c, d, e, f, g, h, i)                                        \
    do {                                                                               \
        uint64_t t1_ =                                                                 \
            (h) + (sha512_rotr(e, 14) ^ sha512_rotr(e, 18) ^ sha512_rotr(e, 41)) +     \
            (((e) & (f)) ^ (~(e) & (g))) + sha512_k[i] + w[i];                         \
        uint64_t t2_ =                                                                 \
            (sha512_rotr(a, 28) ^ sha512_rotr(a, 34) ^ sha512_rotr(a, 39)) +           \
            (((a) & (b)) ^ ((a) & (c)) ^ ((b) & (c)));                                 \
        (d) += t1_;                                                                    \
        (h) = t1_ + t2_;                                                               \
    } while (0)

/* Hashes n bytes of p, which is a whole number of blocks. */
static void sha512_block(Sha512Digest *dig, const Byte *p, Int n) {
    uint64_t w[80];
    uint64_t h0 = dig->h[0], h1 = dig->h[1], h2 = dig->h[2], h3 = dig->h[3];
    uint64_t h4 = dig->h[4], h5 = dig->h[5], h6 = dig->h[6], h7 = dig->h[7];
    for (; n >= SHA512_BLOCK_SIZE; p += SHA512_BLOCK_SIZE, n -= SHA512_BLOCK_SIZE) {
        for (Int i = 0; i < 16; i++)
            w[i] = sha512_be64(p + 8 * i);
        for (int i = 16; i < 80; i++) {
            uint64_t v1 = w[i - 2];
            uint64_t t1 = sha512_rotr(v1, 19) ^ sha512_rotr(v1, 61) ^ (v1 >> 6);
            uint64_t v2 = w[i - 15];
            uint64_t t2 = sha512_rotr(v2, 1) ^ sha512_rotr(v2, 8) ^ (v2 >> 7);
            w[i] = t1 + w[i - 7] + t2 + w[i - 16];
        }
        uint64_t a = h0, b = h1, c = h2, d = h3, e = h4, f = h5, g = h6, h = h7;
        /* Go's loop shifts the eight working variables down one place per
         * round. Unrolling by eight and renaming instead leaves the compiler
         * nothing to move. */
        for (int i = 0; i < 80; i += 8) {
            SHA512_ROUND(a, b, c, d, e, f, g, h, i);
            SHA512_ROUND(h, a, b, c, d, e, f, g, i + 1);
            SHA512_ROUND(g, h, a, b, c, d, e, f, i + 2);
            SHA512_ROUND(f, g, h, a, b, c, d, e, i + 3);
            SHA512_ROUND(e, f, g, h, a, b, c, d, i + 4);
            SHA512_ROUND(d, e, f, g, h, a, b, c, i + 5);
            SHA512_ROUND(c, d, e, f, g, h, a, b, i + 6);
            SHA512_ROUND(b, c, d, e, f, g, h, a, i + 7);
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

static void sha512_reset(void *self) {
    Sha512Digest *d = self;
    static const uint64_t init512[8] = {
        0x6A09E667F3BCC908ULL, 0xBB67AE8584CAA73BULL, 0x3C6EF372FE94F82BULL,
        0xA54FF53A5F1D36F1ULL, 0x510E527FADE682D1ULL, 0x9B05688C2B3E6C1FULL,
        0x1F83D9ABFB41BD6BULL, 0x5BE0CD19137E2179ULL,
    };
    static const uint64_t init224[8] = {
        0x8C3D37C819544DA2ULL, 0x73E1996689DCD4D6ULL, 0x1DFAB7AE32FF9C82ULL,
        0x679DD514582F9FCFULL, 0x0F6D2B697BD44DA8ULL, 0x77E36F7304C48942ULL,
        0x3F9D85A86A1D36C8ULL, 0x1112E6AD91D692A1ULL,
    };
    static const uint64_t init256[8] = {
        0x22312194FC2BF72CULL, 0x9F555FA3C84C64C2ULL, 0x2393B86B6F53B151ULL,
        0x963877195940EABDULL, 0x96283EE2A88EFFE3ULL, 0xBE5E1E2553863992ULL,
        0x2B0199FC2C85B8AAULL, 0x0EB72DDC81C52CA2ULL,
    };
    static const uint64_t init384[8] = {
        0xCBBB9D5DC1059ED8ULL, 0x629A292A367CD507ULL, 0x9159015A3070DD17ULL,
        0x152FECD8F70E5939ULL, 0x67332667FFC00B31ULL, 0x8EB44A8768581511ULL,
        0xDB0C2E0D64F98FA7ULL, 0x47B5481DBEFA4FA4ULL,
    };
    const uint64_t *init;
    switch (d->size) {
    case SHA512_SIZE384:
        init = init384;
        break;
    case SHA512_SIZE224:
        init = init224;
        break;
    case SHA512_SIZE256:
        init = init256;
        break;
    default:
        init = init512;
        break;
    }
    memcpy(d->h, init, sizeof d->h);
    d->nx = 0;
    d->len = 0;
}

static void sha512_update(Sha512Digest *d, const Byte *p, Int n);

/* Always writes all 64 bytes. The shorter sums keep a prefix. */
static void sha512_check_sum(Sha512Digest *d, Byte out[SHA512_SIZE]) {
    /* Padding: add 1 bit and 0 bits until 112 bytes mod 128, then the length
     * in bits as a big endian 128 bit number, of which the top half is always
     * zero here. */
    uint64_t len = d->len;
    Byte tmp[128 + 16] = {0x80};
    uint64_t t = len % 128 < 112 ? 112 - len % 128 : 128 + 112 - len % 128;
    sha512_put_be64(tmp + t + 8, len << 3);
    sha512_update(d, tmp, (Int)(t + 16));
    if (d->nx != 0)
        panic_str(BURROW_S("d.nx != 0"));
    for (Int i = 0; i < 8; i++)
        sha512_put_be64(out + 8 * i, d->h[i]);
}

static void sha512_update(Sha512Digest *d, const Byte *p, Int n) {
    d->len += (uint64_t)n;
    if (d->nx > 0) {
        Int k = SHA512_BLOCK_SIZE - d->nx < n ? SHA512_BLOCK_SIZE - d->nx : n;
        memcpy(d->x + d->nx, p, (size_t)k);
        d->nx += k;
        if (d->nx == SHA512_BLOCK_SIZE) {
            sha512_block(d, d->x, SHA512_BLOCK_SIZE);
            d->nx = 0;
        }
        p += k;
        n -= k;
    }
    if (n >= SHA512_BLOCK_SIZE) {
        Int k = n & ~(Int)(SHA512_BLOCK_SIZE - 1);
        sha512_block(d, p, k);
        p += k;
        n -= k;
    }
    if (n > 0) {
        memcpy(d->x, p, (size_t)n);
        d->nx = n;
    }
}

static Int sha512_write(void *self, Slice p, Error *err) {
    sha512_update(self, (const Byte *)p.p, p.len);
    BURROW_OUT(err, BURROW_NO_ERROR);
    return p.len;
}

static Slice sha512_sum_append(void *self, Alloc *a, Slice in) {
    /* Work on a copy so the caller can keep writing. */
    Sha512Digest d0 = *(Sha512Digest *)self;
    Byte h[SHA512_SIZE];
    sha512_check_sum(&d0, h);
    if (in.elem == NULL)
        in = slice_nil(TYPE_BYTE);
    return slice_append(a, in, h, ((Sha512Digest *)self)->size);
}

static Int sha512_block_size(void *self) {
    (void)self;
    return SHA512_BLOCK_SIZE;
}

static Int sha512_size(void *self) {
    return ((Sha512Digest *)self)->size;
}

static const HashVT sha512_vt = {
    {NULL, sha512_write}, sha512_sum_append, sha512_reset,
    sha512_size,          sha512_block_size,
};

static Hash sha512_make(Alloc *a, Int size) {
    Sha512Digest *d = BURROW_NEW(a, Sha512Digest);
    if (d == NULL)
        return (Hash){NULL, NULL};
    d->size = size;
    sha512_reset(d);
    Hash h = {&sha512_vt, d};
    return h;
}

Hash sha512_new(Alloc *a) {
    return sha512_make(a, SHA512_SIZE);
}

Hash sha512_new384(Alloc *a) {
    return sha512_make(a, SHA512_SIZE384);
}

Hash sha512_new512224(Alloc *a) {
    return sha512_make(a, SHA512_SIZE224);
}

Hash sha512_new512256(Alloc *a) {
    return sha512_make(a, SHA512_SIZE256);
}

static void sha512_one_shot(Int size, Slice data, Byte out[SHA512_SIZE]) {
    Sha512Digest d;
    d.size = size;
    sha512_reset(&d);
    sha512_update(&d, (const Byte *)data.p, data.len);
    sha512_check_sum(&d, out);
}

Sha512Sum512Ret sha512_sum512(Slice data) {
    Sha512Sum512Ret r;
    sha512_one_shot(SHA512_SIZE, data, r.a);
    return r;
}

Sha512Sum384Ret sha512_sum384(Slice data) {
    Sha512Sum384Ret r;
    Byte h[SHA512_SIZE];
    sha512_one_shot(SHA512_SIZE384, data, h);
    memcpy(r.a, h, sizeof r.a);
    return r;
}

Sha512Sum512224Ret sha512_sum512224(Slice data) {
    Sha512Sum512224Ret r;
    Byte h[SHA512_SIZE];
    sha512_one_shot(SHA512_SIZE224, data, h);
    memcpy(r.a, h, sizeof r.a);
    return r;
}

Sha512Sum512256Ret sha512_sum512256(Slice data) {
    Sha512Sum512256Ret r;
    Byte h[SHA512_SIZE];
    sha512_one_shot(SHA512_SIZE256, data, h);
    memcpy(r.a, h, sizeof r.a);
    return r;
}
