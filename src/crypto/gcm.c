/* Derived from Go's src/crypto/cipher/gcm.go and
 * src/crypto/internal/fips140/aes/gcm/gcm.go, gcm_generic.go, gcm_nonces.go and
 * ghash.go.
 * Go source: go1.27.1.
 *
 * Galois/Counter Mode as NIST SP 800-38D has it. With an AES block the counter
 * blocks go to the AES code several at a time, and GHASH runs on the carryless
 * multiply, PCLMULQDQ on x86 and PMULL on arm64, when the processor has it and
 * the block is using the AES instructions. Otherwise GHASH is Go's portable
 * one, which multiplies with holes between the bits so that the carries have
 * nowhere to go, and takes the same time whatever the key and data are. A block
 * that is not AES gets Go's fallback, which works through the Block interface.
 *
 * Copyright 2013 The Go Authors. All rights reserved.
 * Copyright 2026 The burrow Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style licence that can be found
 * in the LICENSE file. */

#include "burrow/crypto/cipher.h"

#include "burrow/core.h"
#include "burrow/crypto/subtle.h"
#include "burrow/error.h"
#include "burrow/mem.h"
#include "burrow/pal.h"
#include "burrow/panic.h"
#include "burrow/slice.h"
#include "burrow/type.h"

#include "aes_internal.h"
#include "internal.h"
#include "rand_internal.h"

#include <stdint.h>
#include <string.h>

enum {
    GCM_BLOCK_SIZE = 16,
    GCM_STANDARD_NONCE_SIZE = 12,
    GCM_TAG_SIZE = 16,
    GCM_MINIMUM_TAG_SIZE = 12,
    /* How many counter blocks go to the AES code at once. */
    GCM_BATCH = 8,
};

/* The longest plaintext GCM takes, (2^32 - 2) blocks. */
#define GCM_MAX_PLAINTEXT (UINT64_C(0xfffffffe) * GCM_BLOCK_SIZE)

/* Which GHASH a GCM runs. */
enum {
    GCM_GHASH_CT,
    GCM_GHASH_X86,
    GCM_GHASH_ARM64,
};

BURROW_SENTINEL_ERROR(burrow__gcm_err_open, "cipher: message authentication failed");

typedef struct CipherGcm {
    /* The block, for the fallback, and the AES behind it when there is one. */
    CipherBlock b;
    const AesBlock *aes;
    Int nonce_size;
    Int tag_size;
    int ghash;
    /* E(0), the hash key, which only an AES GCM keeps. The fallback works it out
     * on every call, as Go's does. */
    Byte h[GCM_BLOCK_SIZE];
} CipherGcm;

static Slice gcm_bytes(Byte *p, Int n) {
    return slice_from(p, n, n, TYPE_BYTE);
}

/* Go's alias.InexactOverlap and alias.AnyOverlap on runs of bytes. */
static bool gcm_any_overlap(const void *x, Int xn, const void *y, Int yn) {
    if (xn <= 0 || yn <= 0)
        return false;
    uintptr_t a = (uintptr_t)x;
    uintptr_t b = (uintptr_t)y;
    return a <= b + (uintptr_t)(yn - 1) && b <= a + (uintptr_t)(xn - 1);
}

static bool gcm_inexact_overlap(const void *x, Int xn, const void *y, Int yn) {
    if (xn <= 0 || yn <= 0 || x == y)
        return false;
    return gcm_any_overlap(x, xn, y, yn);
}

/* Go's sliceForAppend: dst with n more bytes on the end, in place when its
 * capacity allows and copied to a new array from a when it does not. *tail is
 * where the n bytes start. A nil slice when a is out of memory. */
static Slice gcm_slice_for_append(Alloc *a, Slice dst, Int n, Byte **tail) {
    Int total = dst.len + n;
    Slice head;
    if (dst.p != NULL && dst.cap >= total) {
        head = slice_from(dst.p, total, dst.cap, TYPE_BYTE);
    } else {
        head = slice_make(a, TYPE_BYTE, total, total);
        if (head.p == NULL && total > 0) {
            *tail = NULL;
            return slice_nil(TYPE_BYTE);
        }
        if (dst.len > 0 && dst.p != NULL && head.p != NULL)
            memcpy(head.p, dst.p, (size_t)dst.len);
    }
    *tail = (Byte *)head.p + dst.len;
    return head;
}

static uint32_t gcm_be32(const Byte *p) {
    return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 |
           (uint32_t)p[3];
}

static void gcm_put_be32(Byte *p, uint32_t v) {
    p[0] = (Byte)(v >> 24);
    p[1] = (Byte)(v >> 16);
    p[2] = (Byte)(v >> 8);
    p[3] = (Byte)v;
}

static void gcm_put_be64(Byte *p, uint64_t v) {
    gcm_put_be32(p, (uint32_t)(v >> 32));
    gcm_put_be32(p + 4, (uint32_t)v);
}

/* ------------------------------------------------------------ GHASH in C */

/* Go's ghashMul: the carryless product of x and y. Each operand is cut into
 * four, every fourth bit in each, so that an ordinary multiply of two parts
 * leaves three zero bits between the ones it means, and the carries land in
 * those and are masked away. */
static uint64_t gcm_mul32(uint32_t x, uint32_t y) {
    uint32_t xm[4], ym[4];
    uint64_t z[4];
    for (int i = 0; i < 4; i++) {
        xm[i] = x & (UINT32_C(0x11111111) << i);
        ym[i] = y & (UINT32_C(0x11111111) << i);
    }
    for (int i = 0; i < 4; i++) {
        z[i] = ((uint64_t)xm[0] * ym[i]) ^ ((uint64_t)xm[1] * ym[(i + 3) % 4]) ^
               ((uint64_t)xm[2] * ym[(i + 2) % 4]) ^
               ((uint64_t)xm[3] * ym[(i + 1) % 4]);
        z[i] &= UINT64_C(0x1111111111111111) << i;
    }
    return z[0] | z[1] | z[2] | z[3];
}

/* One block of Go's ghash: y = (y ^ block) * h, with Karatsuba twice over and
 * then the reduction. */
static void gcm_ct_block(uint32_t y[4], const uint32_t h[4], const Byte *block) {
    for (int i = 0; i < 4; i++)
        y[3 - i] ^= gcm_be32(block + 4 * (size_t)i);
    uint64_t z_lo[3], z_hi[3], z_sum[3];
    z_lo[0] = gcm_mul32(y[0], h[0]);
    z_hi[0] = gcm_mul32(y[1], h[1]);
    z_sum[0] = gcm_mul32(y[0] ^ y[1], h[0] ^ h[1]);
    z_lo[1] = gcm_mul32(y[2], h[2]);
    z_hi[1] = gcm_mul32(y[3], h[3]);
    z_sum[1] = gcm_mul32(y[2] ^ y[3], h[2] ^ h[3]);
    z_lo[2] = gcm_mul32(y[0] ^ y[2], h[0] ^ h[2]);
    z_hi[2] = gcm_mul32(y[1] ^ y[3], h[1] ^ h[3]);
    z_sum[2] = gcm_mul32((y[0] ^ y[2]) ^ (y[1] ^ y[3]), (h[0] ^ h[2]) ^ (h[1] ^ h[3]));
    uint64_t result[3][2];
    for (int i = 0; i < 3; i++) {
        uint64_t mid = z_sum[i] ^ z_lo[i] ^ z_hi[i];
        result[i][0] = z_lo[i] ^ (mid << 32);
        result[i][1] = z_hi[i] ^ (mid >> 32);
    }
    result[2][0] ^= result[0][0] ^ result[1][0];
    result[2][1] ^= result[0][1] ^ result[1][1];
    result[0][1] ^= result[2][0];
    result[1][0] ^= result[2][1];
    uint64_t z[4];
    z[0] = result[0][0] << 1;
    z[1] = (result[0][1] << 1) | (result[0][0] >> 63);
    z[2] = (result[1][0] << 1) | (result[0][1] >> 63);
    z[3] = (result[1][1] << 1) | (result[1][0] >> 63);
    for (int i = 0; i < 2; i++) {
        uint64_t lw = z[i];
        z[i + 2] ^= lw ^ (lw >> 1) ^ (lw >> 2) ^ (lw >> 7);
        z[i + 1] ^= (lw << 63) ^ (lw << 62) ^ (lw << 57);
    }
    y[0] = (uint32_t)z[2];
    y[1] = (uint32_t)(z[2] >> 32);
    y[2] = (uint32_t)z[3];
    y[3] = (uint32_t)(z[3] >> 32);
}

static void gcm_ghash_ct(const Byte *hkey, Byte *ys, const Byte *p, size_t n) {
    uint32_t y[4], h[4];
    for (int i = 0; i < 4; i++) {
        h[3 - i] = gcm_be32(hkey + 4 * (size_t)i);
        y[3 - i] = gcm_be32(ys + 4 * (size_t)i);
    }
    for (; n >= GCM_BLOCK_SIZE; n -= GCM_BLOCK_SIZE, p += GCM_BLOCK_SIZE)
        gcm_ct_block(y, h, p);
    if (n > 0) {
        Byte last[GCM_BLOCK_SIZE] = {0};
        memcpy(last, p, n);
        gcm_ct_block(y, h, last);
    }
    for (int i = 0; i < 4; i++)
        gcm_put_be32(ys + 4 * (size_t)i, y[3 - i]);
}

/* ------------------------------------------------------- GHASH on x86-64 */

#if defined(CRYPTO_X86_AES)

CRYPTO_TARGET_X86_GCM static inline __m128i gcm_x86_load(const void *p) {
    __m128i v;
    memcpy(&v, p, sizeof v);
    return v;
}

CRYPTO_TARGET_X86_GCM static inline void gcm_x86_store(void *p, __m128i v) {
    memcpy(p, &v, sizeof v);
}

/* a * b in GF(2^128), both byte reversed, from Intel's paper on PCLMULQDQ and
 * GCM: the 256 bit carryless product, shifted left a bit because GCM's bits
 * run the other way, then reduced modulo x^128 + x^7 + x^2 + x + 1. */
CRYPTO_TARGET_X86_GCM static __m128i gcm_x86_mul(__m128i a, __m128i b) {
    __m128i lo = _mm_clmulepi64_si128(a, b, 0x00);
    __m128i mid = _mm_xor_si128(_mm_clmulepi64_si128(a, b, 0x10),
                                _mm_clmulepi64_si128(a, b, 0x01));
    __m128i hi = _mm_clmulepi64_si128(a, b, 0x11);
    lo = _mm_xor_si128(lo, _mm_slli_si128(mid, 8));
    hi = _mm_xor_si128(hi, _mm_srli_si128(mid, 8));

    __m128i c_lo = _mm_srli_epi32(lo, 31);
    __m128i c_hi = _mm_srli_epi32(hi, 31);
    lo = _mm_slli_epi32(lo, 1);
    hi = _mm_slli_epi32(hi, 1);
    __m128i across = _mm_srli_si128(c_lo, 12);
    c_hi = _mm_slli_si128(c_hi, 4);
    c_lo = _mm_slli_si128(c_lo, 4);
    lo = _mm_or_si128(lo, c_lo);
    hi = _mm_or_si128(_mm_or_si128(hi, c_hi), across);

    __m128i t =
        _mm_xor_si128(_mm_xor_si128(_mm_slli_epi32(lo, 31), _mm_slli_epi32(lo, 30)),
                      _mm_slli_epi32(lo, 25));
    __m128i t_hi = _mm_srli_si128(t, 4);
    lo = _mm_xor_si128(lo, _mm_slli_si128(t, 12));
    __m128i u =
        _mm_xor_si128(_mm_xor_si128(_mm_srli_epi32(lo, 1), _mm_srli_epi32(lo, 2)),
                      _mm_srli_epi32(lo, 7));
    u = _mm_xor_si128(u, t_hi);
    lo = _mm_xor_si128(lo, u);
    return _mm_xor_si128(hi, lo);
}

CRYPTO_TARGET_X86_GCM static void gcm_ghash_x86(const Byte *hkey, Byte *ys,
                                                const Byte *p, size_t n) {
    const __m128i rev =
        _mm_set_epi8(0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15);
    __m128i h = _mm_shuffle_epi8(gcm_x86_load(hkey), rev);
    __m128i y = _mm_shuffle_epi8(gcm_x86_load(ys), rev);
    for (; n >= GCM_BLOCK_SIZE; n -= GCM_BLOCK_SIZE, p += GCM_BLOCK_SIZE)
        y = gcm_x86_mul(_mm_xor_si128(y, _mm_shuffle_epi8(gcm_x86_load(p), rev)), h);
    if (n > 0) {
        Byte last[GCM_BLOCK_SIZE] = {0};
        memcpy(last, p, n);
        y = gcm_x86_mul(_mm_xor_si128(y, _mm_shuffle_epi8(gcm_x86_load(last), rev)), h);
    }
    gcm_x86_store(ys, _mm_shuffle_epi8(y, rev));
}

#endif /* CRYPTO_X86_AES */

/* -------------------------------------------------------- GHASH on arm64 */

#if defined(CRYPTO_ARM64_AES)

/* The x86 code above, an instruction for an instruction: the lanes are laid
 * out the same way on a little endian arm64. */

CRYPTO_TARGET_ARM64_AES static inline uint32x4_t gcm_arm64_clmul(uint32x4_t a, int ai,
                                                                 uint32x4_t b, int bi) {
    uint64x2_t a64 = vreinterpretq_u64_u32(a);
    uint64x2_t b64 = vreinterpretq_u64_u32(b);
    poly64_t x = (poly64_t)(ai ? vgetq_lane_u64(a64, 1) : vgetq_lane_u64(a64, 0));
    poly64_t y = (poly64_t)(bi ? vgetq_lane_u64(b64, 1) : vgetq_lane_u64(b64, 0));
    return vreinterpretq_u32_p128(vmull_p64(x, y));
}

/* _mm_slli_si128 and _mm_srli_si128: whole bytes towards the top or bottom. */
#define GCM_ARM64_BYTES_UP(v, k)                                                       \
    vreinterpretq_u32_u8(vextq_u8(vdupq_n_u8(0), vreinterpretq_u8_u32(v), 16 - (k)))
#define GCM_ARM64_BYTES_DOWN(v, k)                                                     \
    vreinterpretq_u32_u8(vextq_u8(vreinterpretq_u8_u32(v), vdupq_n_u8(0), (k)))

CRYPTO_TARGET_ARM64_AES static uint32x4_t gcm_arm64_mul(uint32x4_t a, uint32x4_t b) {
    uint32x4_t lo = gcm_arm64_clmul(a, 0, b, 0);
    uint32x4_t mid =
        veorq_u32(gcm_arm64_clmul(a, 0, b, 1), gcm_arm64_clmul(a, 1, b, 0));
    uint32x4_t hi = gcm_arm64_clmul(a, 1, b, 1);
    lo = veorq_u32(lo, GCM_ARM64_BYTES_UP(mid, 8));
    hi = veorq_u32(hi, GCM_ARM64_BYTES_DOWN(mid, 8));

    uint32x4_t c_lo = vshrq_n_u32(lo, 31);
    uint32x4_t c_hi = vshrq_n_u32(hi, 31);
    lo = vshlq_n_u32(lo, 1);
    hi = vshlq_n_u32(hi, 1);
    uint32x4_t across = GCM_ARM64_BYTES_DOWN(c_lo, 12);
    c_hi = GCM_ARM64_BYTES_UP(c_hi, 4);
    c_lo = GCM_ARM64_BYTES_UP(c_lo, 4);
    lo = vorrq_u32(lo, c_lo);
    hi = vorrq_u32(vorrq_u32(hi, c_hi), across);

    uint32x4_t t = veorq_u32(veorq_u32(vshlq_n_u32(lo, 31), vshlq_n_u32(lo, 30)),
                             vshlq_n_u32(lo, 25));
    uint32x4_t t_hi = GCM_ARM64_BYTES_DOWN(t, 4);
    lo = veorq_u32(lo, GCM_ARM64_BYTES_UP(t, 12));
    uint32x4_t u = veorq_u32(veorq_u32(vshrq_n_u32(lo, 1), vshrq_n_u32(lo, 2)),
                             vshrq_n_u32(lo, 7));
    u = veorq_u32(u, t_hi);
    lo = veorq_u32(lo, u);
    return veorq_u32(hi, lo);
}

/* Sixteen bytes from p, last first. */
CRYPTO_TARGET_ARM64_AES static inline uint32x4_t gcm_arm64_load_rev(const Byte *p) {
    uint8x16_t v = vrev64q_u8(vld1q_u8(p));
    return vreinterpretq_u32_u8(vextq_u8(v, v, 8));
}

CRYPTO_TARGET_ARM64_AES static void gcm_ghash_arm64(const Byte *hkey, Byte *ys,
                                                    const Byte *p, size_t n) {
    uint32x4_t h = gcm_arm64_load_rev(hkey);
    uint32x4_t y = gcm_arm64_load_rev(ys);
    for (; n >= GCM_BLOCK_SIZE; n -= GCM_BLOCK_SIZE, p += GCM_BLOCK_SIZE)
        y = gcm_arm64_mul(veorq_u32(y, gcm_arm64_load_rev(p)), h);
    if (n > 0) {
        Byte last[GCM_BLOCK_SIZE] = {0};
        memcpy(last, p, n);
        y = gcm_arm64_mul(veorq_u32(y, gcm_arm64_load_rev(last)), h);
    }
    uint8x16_t out = vrev64q_u8(vreinterpretq_u8_u32(y));
    vst1q_u8(ys, vextq_u8(out, out, 8));
}

#undef GCM_ARM64_BYTES_UP
#undef GCM_ARM64_BYTES_DOWN

#endif /* CRYPTO_ARM64_AES */

/* --------------------------------------------------------------- the mode */

/* Folds n bytes of p into the GHASH state y under hkey, the last block padded
 * with zeros when n is not a multiple of 16. */
static void gcm_ghash(const CipherGcm *g, const Byte *hkey, Byte *y, const Byte *p,
                      size_t n) {
    switch (g->ghash) {
#if defined(CRYPTO_X86_AES)
    case GCM_GHASH_X86:
        gcm_ghash_x86(hkey, y, p, n);
        return;
#endif
#if defined(CRYPTO_ARM64_AES)
    case GCM_GHASH_ARM64:
        gcm_ghash_arm64(hkey, y, p, n);
        return;
#endif
    default:
        gcm_ghash_ct(hkey, y, p, n);
        return;
    }
}

/* Encrypts n blocks of src into dst. */
static void gcm_encrypt_blocks(const CipherGcm *g, Byte *dst, Byte *src, size_t n) {
    if (g->aes != NULL) {
        burrow__aes_encrypt_blocks(g->aes, dst, src, n);
        return;
    }
    for (size_t i = 0; i < n; i++)
        cipher_block_encrypt(g->b, gcm_bytes(dst + i * GCM_BLOCK_SIZE, GCM_BLOCK_SIZE),
                             gcm_bytes(src + i * GCM_BLOCK_SIZE, GCM_BLOCK_SIZE));
}

static void gcm_hash_key(const CipherGcm *g, Byte *h) {
    if (g->aes != NULL) {
        memcpy(h, g->h, GCM_BLOCK_SIZE);
        return;
    }
    memset(h, 0, GCM_BLOCK_SIZE);
    gcm_encrypt_blocks(g, h, h, 1);
}

/* gcmInc32: the last four bytes of the counter block, big endian, plus one. */
static void gcm_inc32(Byte *counter) {
    gcm_put_be32(counter + 12, gcm_be32(counter + 12) + 1);
}

/* gcmCounterCryptGeneric, GCM_BATCH blocks at a time. out may be src exactly. */
static void gcm_counter_crypt(const CipherGcm *g, Byte *out, const Byte *src, size_t n,
                              Byte *counter) {
    Byte ctrs[GCM_BATCH * GCM_BLOCK_SIZE];
    Byte mask[GCM_BATCH * GCM_BLOCK_SIZE];
    while (n > 0) {
        size_t blocks = (n + GCM_BLOCK_SIZE - 1) / GCM_BLOCK_SIZE;
        if (blocks > GCM_BATCH)
            blocks = GCM_BATCH;
        for (size_t i = 0; i < blocks; i++) {
            memcpy(ctrs + i * GCM_BLOCK_SIZE, counter, GCM_BLOCK_SIZE);
            gcm_inc32(counter);
        }
        gcm_encrypt_blocks(g, mask, ctrs, blocks);
        size_t m = blocks * GCM_BLOCK_SIZE < n ? blocks * GCM_BLOCK_SIZE : n;
        for (size_t i = 0; i < m; i++)
            out[i] = (Byte)(src[i] ^ mask[i]);
        out += m;
        src += m;
        n -= m;
    }
}

/* deriveCounter: the first counter block from the nonce, which is the nonce and
 * then 1 for a 12 byte nonce and GHASH of it for any other length. */
static void gcm_derive_counter(const CipherGcm *g, const Byte *h, Byte *counter,
                               const Byte *nonce, Int nonce_len) {
    if (nonce_len == GCM_STANDARD_NONCE_SIZE) {
        memcpy(counter, nonce, GCM_STANDARD_NONCE_SIZE);
        memset(counter + GCM_STANDARD_NONCE_SIZE, 0, 3);
        counter[GCM_BLOCK_SIZE - 1] = 1;
        return;
    }
    Byte len_block[GCM_BLOCK_SIZE] = {0};
    gcm_put_be64(len_block + 8, (uint64_t)nonce_len * 8);
    memset(counter, 0, GCM_BLOCK_SIZE);
    gcm_ghash(g, h, counter, nonce, (size_t)nonce_len);
    gcm_ghash(g, h, counter, len_block, GCM_BLOCK_SIZE);
}

/* gcmAuth: GHASH of the additional data, the ciphertext and their lengths in
 * bits, masked with tag_mask, into out. */
static void gcm_auth(const CipherGcm *g, const Byte *h, Byte *out, const Byte *tag_mask,
                     const Byte *ct, Int ct_len, const Byte *ad, Int ad_len) {
    Byte len_block[GCM_BLOCK_SIZE];
    gcm_put_be64(len_block, (uint64_t)ad_len * 8);
    gcm_put_be64(len_block + 8, (uint64_t)ct_len * 8);
    Byte s[GCM_BLOCK_SIZE] = {0};
    gcm_ghash(g, h, s, ad, (size_t)ad_len);
    gcm_ghash(g, h, s, ct, (size_t)ct_len);
    gcm_ghash(g, h, s, len_block, GCM_BLOCK_SIZE);
    for (int i = 0; i < GCM_BLOCK_SIZE; i++)
        out[i] = (Byte)(s[i] ^ tag_mask[i]);
}

/* sealGeneric: pt_len bytes of ciphertext and then the tag into out. */
static void gcm_seal_into(const CipherGcm *g, Byte *out, const Byte *nonce,
                          Int nonce_len, const Byte *pt, Int pt_len, const Byte *ad,
                          Int ad_len) {
    Byte h[GCM_BLOCK_SIZE], counter[GCM_BLOCK_SIZE], tag_mask[GCM_BLOCK_SIZE] = {0};
    gcm_hash_key(g, h);
    gcm_derive_counter(g, h, counter, nonce, nonce_len);
    gcm_counter_crypt(g, tag_mask, tag_mask, GCM_BLOCK_SIZE, counter);
    gcm_counter_crypt(g, out, pt, (size_t)pt_len, counter);
    Byte tag[GCM_TAG_SIZE];
    gcm_auth(g, h, tag, tag_mask, out, pt_len, ad, ad_len);
    memcpy(out + pt_len, tag, (size_t)g->tag_size);
}

/* openGeneric: false when the tag is wrong, and then out is untouched. */
static bool gcm_open_into(const CipherGcm *g, Byte *out, const Byte *nonce,
                          Int nonce_len, Byte *ct, Int ct_len, const Byte *ad,
                          Int ad_len) {
    Byte h[GCM_BLOCK_SIZE], counter[GCM_BLOCK_SIZE], tag_mask[GCM_BLOCK_SIZE] = {0};
    gcm_hash_key(g, h);
    gcm_derive_counter(g, h, counter, nonce, nonce_len);
    gcm_counter_crypt(g, tag_mask, tag_mask, GCM_BLOCK_SIZE, counter);
    Byte *tag = ct + ct_len - g->tag_size;
    ct_len -= g->tag_size;
    Byte expected[GCM_TAG_SIZE];
    gcm_auth(g, h, expected, tag_mask, ct, ct_len, ad, ad_len);
    if (subtle_constant_time_compare(gcm_bytes(expected, g->tag_size),
                                     gcm_bytes(tag, g->tag_size)) != 1)
        return false;
    gcm_counter_crypt(g, out, ct, (size_t)ct_len, counter);
    return true;
}

static Int gcm_nonce_size(void *self) {
    return ((const CipherGcm *)self)->nonce_size;
}

static Int gcm_overhead(void *self) {
    return ((const CipherGcm *)self)->tag_size;
}

static Slice gcm_seal(void *self, Alloc *a, Slice dst, Slice nonce, Slice plaintext,
                      Slice additional_data) {
    const CipherGcm *g = (const CipherGcm *)self;
    if (nonce.len != g->nonce_size)
        panic_str(BURROW_S("crypto/cipher: incorrect nonce length given to GCM"));
    if (g->nonce_size == 0)
        panic_str(BURROW_S("crypto/cipher: incorrect GCM nonce size"));
    if ((uint64_t)plaintext.len > GCM_MAX_PLAINTEXT)
        panic_str(BURROW_S("crypto/cipher: message too large for GCM"));
    Byte *out;
    Int n = plaintext.len + g->tag_size;
    Slice ret = gcm_slice_for_append(a, dst, n, &out);
    if (out == NULL)
        return ret;
    if (gcm_inexact_overlap(out, n, plaintext.p, plaintext.len))
        panic_str(
            BURROW_S("crypto/cipher: invalid buffer overlap of output and input"));
    if (gcm_any_overlap(out, n, additional_data.p, additional_data.len))
        panic_str(BURROW_S(
            "crypto/cipher: invalid buffer overlap of output and additional data"));
    gcm_seal_into(g, out, (const Byte *)nonce.p, nonce.len, (const Byte *)plaintext.p,
                  plaintext.len, (const Byte *)additional_data.p, additional_data.len);
    return ret;
}

static Slice gcm_open(void *self, Alloc *a, Slice dst, Slice nonce, Slice ciphertext,
                      Slice additional_data, Error *err) {
    const CipherGcm *g = (const CipherGcm *)self;
    if (nonce.len != g->nonce_size)
        panic_str(BURROW_S("crypto/cipher: incorrect nonce length given to GCM"));
    if (g->tag_size < GCM_MINIMUM_TAG_SIZE)
        panic_str(BURROW_S("crypto/cipher: incorrect GCM tag size"));
    if (ciphertext.len < g->tag_size ||
        (uint64_t)ciphertext.len > GCM_MAX_PLAINTEXT + (uint64_t)g->tag_size) {
        BURROW_OUT(err, burrow__gcm_err_open);
        return slice_nil(TYPE_BYTE);
    }
    Byte *out;
    Int n = ciphertext.len - g->tag_size;
    Slice ret = gcm_slice_for_append(a, dst, n, &out);
    if (out == NULL) {
        BURROW_OUT(err, burrow_err_out_of_memory);
        return ret;
    }
    if (gcm_inexact_overlap(out, n, ciphertext.p, ciphertext.len))
        panic_str(
            BURROW_S("crypto/cipher: invalid buffer overlap of output and input"));
    if (gcm_any_overlap(out, n, additional_data.p, additional_data.len))
        panic_str(BURROW_S(
            "crypto/cipher: invalid buffer overlap of output and additional data"));
    if (!gcm_open_into(g, out, (const Byte *)nonce.p, nonce.len, (Byte *)ciphertext.p,
                       ciphertext.len, (const Byte *)additional_data.p,
                       additional_data.len)) {
        /* Go clears out here, so that no platform hands back plaintext that
         * failed to authenticate. */
        if (n > 0)
            memset(out, 0, (size_t)n);
        BURROW_OUT(err, burrow__gcm_err_open);
        return slice_nil(TYPE_BYTE);
    }
    BURROW_OUT(err, BURROW_NO_ERROR);
    return ret;
}

static const CipherAEADVT gcm_vt = {NULL, gcm_nonce_size, gcm_overhead, gcm_seal,
                                    gcm_open};

/* ------------------------------------------------------ the random nonces */

static Int gcm_random_nonce_size(void *self) {
    (void)self;
    return 0;
}

static Int gcm_random_overhead(void *self) {
    (void)self;
    return GCM_STANDARD_NONCE_SIZE + GCM_TAG_SIZE;
}

static Slice gcm_random_seal(void *self, Alloc *a, Slice dst, Slice nonce,
                             Slice plaintext, Slice additional_data) {
    const CipherGcm *g = (const CipherGcm *)self;
    if (nonce.len != 0)
        panic_str(
            BURROW_S("crypto/cipher: non-empty nonce passed to GCMWithRandomNonce"));
    Byte *out;
    Int n = GCM_STANDARD_NONCE_SIZE + plaintext.len + GCM_TAG_SIZE;
    Slice ret = gcm_slice_for_append(a, dst, n, &out);
    if (out == NULL)
        return ret;
    if (gcm_inexact_overlap(out, n, plaintext.p, plaintext.len))
        panic_str(
            BURROW_S("crypto/cipher: invalid buffer overlap of output and input"));
    if (gcm_any_overlap(out, n, additional_data.p, additional_data.len))
        panic_str(BURROW_S(
            "crypto/cipher: invalid buffer overlap of output and additional data"));
    Byte *ct = out + GCM_STANDARD_NONCE_SIZE;
    const Byte *pt = (const Byte *)plaintext.p;
    /* plaintext[:0] as dst puts the ciphertext twelve bytes after the plaintext,
     * which overlaps it without being it, so the plaintext moves first. */
    if (gcm_any_overlap(out, n, plaintext.p, plaintext.len)) {
        memmove(ct, plaintext.p, (size_t)plaintext.len);
        pt = ct;
    }
    /* gcm.SealWithRandomNonce, whose remaining checks cannot fail from here. */
    if ((uint64_t)plaintext.len > GCM_MAX_PLAINTEXT)
        panic_str(BURROW_S("crypto/cipher: message too large for GCM"));
    burrow__crypto_rand_system(gcm_bytes(out, GCM_STANDARD_NONCE_SIZE));
    gcm_seal_into(g, ct, out, GCM_STANDARD_NONCE_SIZE, pt, plaintext.len,
                  (const Byte *)additional_data.p, additional_data.len);
    return ret;
}

static Slice gcm_random_open(void *self, Alloc *a, Slice dst, Slice nonce,
                             Slice ciphertext, Slice additional_data, Error *err) {
    const CipherGcm *g = (const CipherGcm *)self;
    if (nonce.len != 0)
        panic_str(
            BURROW_S("crypto/cipher: non-empty nonce passed to GCMWithRandomNonce"));
    if (ciphertext.len < GCM_STANDARD_NONCE_SIZE + GCM_TAG_SIZE) {
        BURROW_OUT(err, burrow__gcm_err_open);
        return slice_nil(TYPE_BYTE);
    }
    Byte *out;
    Int n = ciphertext.len - GCM_STANDARD_NONCE_SIZE - GCM_TAG_SIZE;
    Slice ret = gcm_slice_for_append(a, dst, n, &out);
    if (out == NULL) {
        BURROW_OUT(err, burrow_err_out_of_memory);
        return ret;
    }
    if (gcm_inexact_overlap(out, n, ciphertext.p, ciphertext.len))
        panic_str(
            BURROW_S("crypto/cipher: invalid buffer overlap of output and input"));
    if (gcm_any_overlap(out, n, additional_data.p, additional_data.len))
        panic_str(BURROW_S(
            "crypto/cipher: invalid buffer overlap of output and additional data"));
    Byte nonce_copy[GCM_STANDARD_NONCE_SIZE];
    Byte *in = (Byte *)ciphertext.p;
    Int in_len = ciphertext.len - GCM_STANDARD_NONCE_SIZE;
    const Byte *nc = in;
    /* Any overlap here means out is ciphertext, so it has room for the whole of
     * it, and the AEAD contract lets everything up to its capacity be written.
     * The nonce is kept aside and the rest moved down to where out starts. */
    if (gcm_any_overlap(out, n, ciphertext.p, ciphertext.len)) {
        memcpy(nonce_copy, in, GCM_STANDARD_NONCE_SIZE);
        nc = nonce_copy;
        memmove(out, in + GCM_STANDARD_NONCE_SIZE, (size_t)in_len);
        in = out;
    } else {
        in += GCM_STANDARD_NONCE_SIZE;
    }
    /* g.GCM.Open into out[:0], whose own checks pass by construction. */
    if (!gcm_open_into(g, out, nc, GCM_STANDARD_NONCE_SIZE, in, in_len,
                       (const Byte *)additional_data.p, additional_data.len)) {
        if (n > 0)
            memset(out, 0, (size_t)n);
        BURROW_OUT(err, burrow__gcm_err_open);
        return slice_nil(TYPE_BYTE);
    }
    BURROW_OUT(err, BURROW_NO_ERROR);
    return ret;
}

static const CipherAEADVT gcm_random_vt = {
    NULL, gcm_random_nonce_size, gcm_random_overhead, gcm_random_seal, gcm_random_open};

/* ------------------------------------------------------------ constructors */

static int gcm_pick_ghash(const AesBlock *aes) {
    if (aes == NULL)
        return GCM_GHASH_CT;
#if defined(CRYPTO_X86_AES)
    if (aes->impl == AES_IMPL_X86 &&
        (pal_cpu_features() & CRYPTO_X86_GCM_NEEDS) == CRYPTO_X86_GCM_NEEDS)
        return GCM_GHASH_X86;
#endif
#if defined(CRYPTO_ARM64_AES)
    if (aes->impl == AES_IMPL_ARM64 && (pal_cpu_features() & PAL_CPU_ARM64_PMULL) != 0)
        return GCM_GHASH_ARM64;
#endif
    return GCM_GHASH_CT;
}

static CipherAEAD gcm_fail(Error *err, const char *msg) {
    BURROW_OUT(err, errors_new(error_allocator(), str_from_cstr(msg)));
    return (CipherAEAD){NULL, NULL};
}

/* newGCM and newGCMFallback, which check the same things in the same order. */
static CipherAEAD gcm_new(Alloc *a, CipherBlock b, Int nonce_size, Int tag_size,
                          const CipherAEADVT *vt, Error *err) {
    if (tag_size < GCM_MINIMUM_TAG_SIZE || tag_size > GCM_BLOCK_SIZE)
        return gcm_fail(err, "cipher: incorrect tag size given to GCM");
    if (nonce_size <= 0)
        return gcm_fail(err, "cipher: the nonce can't have zero length");
    if (cipher_block_block_size(b) != GCM_BLOCK_SIZE)
        return gcm_fail(err, "cipher: NewGCM requires 128-bit block cipher");
    CipherGcm *g = BURROW_NEW(a, CipherGcm);
    if (g == NULL) {
        BURROW_OUT(err, burrow_err_out_of_memory);
        return (CipherAEAD){NULL, NULL};
    }
    g->b = b;
    g->aes = burrow__aes_block_of(b);
    g->nonce_size = nonce_size;
    g->tag_size = tag_size;
    g->ghash = gcm_pick_ghash(g->aes);
    memset(g->h, 0, sizeof g->h);
    if (g->aes != NULL)
        burrow__aes_encrypt_blocks(g->aes, g->h, g->h, 1);
    BURROW_OUT(err, BURROW_NO_ERROR);
    return (CipherAEAD){vt, g};
}

CipherAEAD cipher_new_gcm(Alloc *a, CipherBlock b, Error *err) {
    return gcm_new(a, b, GCM_STANDARD_NONCE_SIZE, GCM_TAG_SIZE, &gcm_vt, err);
}

CipherAEAD cipher_new_gcm_with_nonce_size(Alloc *a, CipherBlock b, Int size,
                                          Error *err) {
    return gcm_new(a, b, size, GCM_TAG_SIZE, &gcm_vt, err);
}

CipherAEAD cipher_new_gcm_with_tag_size(Alloc *a, CipherBlock b, Int tag_size,
                                        Error *err) {
    return gcm_new(a, b, GCM_STANDARD_NONCE_SIZE, tag_size, &gcm_vt, err);
}

CipherAEAD cipher_new_gcm_with_random_nonce(Alloc *a, CipherBlock b, Error *err) {
    if (burrow__aes_block_of(b) == NULL)
        return gcm_fail(err, "cipher: NewGCMWithRandomNonce requires aes.Block");
    return gcm_new(a, b, GCM_STANDARD_NONCE_SIZE, GCM_TAG_SIZE, &gcm_random_vt, err);
}
