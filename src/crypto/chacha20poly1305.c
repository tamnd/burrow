/* Derived from golang.org/x/crypto/chacha20/chacha_generic.go and xor.go,
 * internal/poly1305/poly1305.go and sum_generic.go, and
 * chacha20poly1305/chacha20poly1305.go, chacha20poly1305_generic.go and
 * xchacha20poly1305.go, as Go vendors them.
 * Go source: go1.27.1, golang.org/x/crypto v0.52.1-0.20260526024921-9beb694f9766.
 *
 * Copyright 2016 The Go Authors. All rights reserved.
 * Copyright 2026 The burrow Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style licence that can be found
 * in the LICENSE file. */

#include "chacha20poly1305.h"

#include "burrow/core.h"
#include "burrow/crypto/cipher.h"
#include "burrow/crypto/subtle.h"
#include "burrow/error.h"
#include "burrow/math/bits.h"
#include "burrow/mem.h"
#include "burrow/panic.h"
#include "burrow/slice.h"
#include "burrow/type.h"

#include <stdbool.h>
#include <stdint.h>
#include <string.h>

/* ----------------------------------------------------------------- helpers */

static uint32_t chacha20_le32(const Byte *p) {
    return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 |
           (uint32_t)p[3] << 24;
}

static void chacha20_put_le32(Byte *p, uint32_t v) {
    p[0] = (Byte)v;
    p[1] = (Byte)(v >> 8);
    p[2] = (Byte)(v >> 16);
    p[3] = (Byte)(v >> 24);
}

static uint64_t poly1305_le64(const Byte *p) {
    return (uint64_t)chacha20_le32(p) | (uint64_t)chacha20_le32(p + 4) << 32;
}

static void poly1305_put_le64(Byte *p, uint64_t v) {
    chacha20_put_le32(p, (uint32_t)v);
    chacha20_put_le32(p + 4, (uint32_t)(v >> 32));
}

/* Go's alias.AnyOverlap and alias.InexactOverlap on runs of bytes. */
static bool chacha20_any_overlap(const void *x, Int xn, const void *y, Int yn) {
    if (xn <= 0 || yn <= 0)
        return false;
    uintptr_t a = (uintptr_t)x;
    uintptr_t b = (uintptr_t)y;
    return a <= b + (uintptr_t)(yn - 1) && b <= a + (uintptr_t)(xn - 1);
}

static bool chacha20_inexact_overlap(const void *x, Int xn, const void *y, Int yn) {
    if (xn <= 0 || yn <= 0 || x == y)
        return false;
    return chacha20_any_overlap(x, xn, y, yn);
}

static Error chacha20_error(const char *msg) {
    return errors_new(error_allocator(), str_from_cstr(msg));
}

/* ---------------------------------------------------------------- chacha20 */

/* "expand 32-byte k", the constant first row. */
enum {
    CHACHA20_J0 = 0x61707865,
    CHACHA20_J1 = 0x3320646e,
    CHACHA20_J2 = 0x79622d32,
    CHACHA20_J3 = 0x6b206574,
};

#define CHACHA20_QR(a, b, c, d)                                                        \
    do {                                                                               \
        a += b;                                                                        \
        d ^= a;                                                                        \
        d = bits_rotate_left32(d, 16);                                                 \
        c += d;                                                                        \
        b ^= c;                                                                        \
        b = bits_rotate_left32(b, 12);                                                 \
        a += b;                                                                        \
        d ^= a;                                                                        \
        d = bits_rotate_left32(d, 8);                                                  \
        c += d;                                                                        \
        b ^= c;                                                                        \
        b = bits_rotate_left32(b, 7);                                                  \
    } while (0)

/* The core of HChaCha20: the first and last rows after twenty rounds, with no
 * feed forward. key is 32 bytes and nonce 16. */
static void chacha20_hchacha20_core(Byte out[32], const Byte *key, const Byte *nonce) {
    uint32_t x0 = CHACHA20_J0, x1 = CHACHA20_J1, x2 = CHACHA20_J2, x3 = CHACHA20_J3;
    uint32_t x4 = chacha20_le32(key + 0), x5 = chacha20_le32(key + 4);
    uint32_t x6 = chacha20_le32(key + 8), x7 = chacha20_le32(key + 12);
    uint32_t x8 = chacha20_le32(key + 16), x9 = chacha20_le32(key + 20);
    uint32_t x10 = chacha20_le32(key + 24), x11 = chacha20_le32(key + 28);
    uint32_t x12 = chacha20_le32(nonce + 0), x13 = chacha20_le32(nonce + 4);
    uint32_t x14 = chacha20_le32(nonce + 8), x15 = chacha20_le32(nonce + 12);

    for (int i = 0; i < 10; i++) {
        /* Diagonal round. */
        CHACHA20_QR(x0, x4, x8, x12);
        CHACHA20_QR(x1, x5, x9, x13);
        CHACHA20_QR(x2, x6, x10, x14);
        CHACHA20_QR(x3, x7, x11, x15);

        /* Column round. */
        CHACHA20_QR(x0, x5, x10, x15);
        CHACHA20_QR(x1, x6, x11, x12);
        CHACHA20_QR(x2, x7, x8, x13);
        CHACHA20_QR(x3, x4, x9, x14);
    }

    chacha20_put_le32(out + 0, x0);
    chacha20_put_le32(out + 4, x1);
    chacha20_put_le32(out + 8, x2);
    chacha20_put_le32(out + 12, x3);
    chacha20_put_le32(out + 16, x12);
    chacha20_put_le32(out + 20, x13);
    chacha20_put_le32(out + 24, x14);
    chacha20_put_le32(out + 28, x15);
}

/* newUnauthenticatedCipher, into c, for a 32 byte key and a 12 byte nonce. */
static void chacha20_setup(Chacha20Cipher *c, const Byte *key, const Byte *nonce) {
    memset(c, 0, sizeof *c);
    for (int i = 0; i < 8; i++)
        c->key[i] = chacha20_le32(key + 4 * i);
    for (int i = 0; i < 3; i++)
        c->nonce[i] = chacha20_le32(nonce + 4 * i);
}

Chacha20Cipher *chacha20_new_unauthenticated_cipher(Alloc *a, Slice key, Slice nonce,
                                                    Error *err) {
    if (key.len != CHACHA20_KEY_SIZE) {
        BURROW_OUT(err, chacha20_error("chacha20: wrong key size"));
        return NULL;
    }
    Byte subkey[CHACHA20_KEY_SIZE];
    Byte cnonce[CHACHA20_NONCE_SIZE];
    const Byte *k = (const Byte *)key.p;
    const Byte *n = (const Byte *)nonce.p;
    if (nonce.len == CHACHA20_NONCE_SIZE_X) {
        /* XChaCha20 uses the ChaCha20 core to mix 16 bytes of the nonce into a
         * new key, and the rest of the nonce goes where an RFC 8439 nonce
         * would. */
        chacha20_hchacha20_core(subkey, k, n);
        memset(cnonce, 0, 4);
        memcpy(cnonce + 4, n + 16, 8);
        k = subkey;
        n = cnonce;
    } else if (nonce.len != CHACHA20_NONCE_SIZE) {
        BURROW_OUT(err, chacha20_error("chacha20: wrong nonce size"));
        return NULL;
    }
    Chacha20Cipher *c = BURROW_NEW(a, Chacha20Cipher);
    if (c == NULL) {
        BURROW_OUT(err, burrow_err_out_of_memory);
        return NULL;
    }
    chacha20_setup(c, k, n);
    BURROW_OUT(err, BURROW_NO_ERROR);
    return c;
}

void chacha20_cipher_set_counter(Chacha20Cipher *s, uint32_t counter) {
    /* Internally s may hold buffered key stream for blocks before the
     * counter, so this is where the output really is. */
    uint32_t output_counter = s->counter - (uint32_t)s->len / CHACHA20_BLOCK_SIZE;
    if (s->overflow || counter < output_counter)
        panic_str(BURROW_S("chacha20: SetCounter attempted to rollback counter"));

    /* Within the buffered key stream the counter moves the buffer, and past it
     * the buffer goes. */
    if (counter < s->counter) {
        s->len = (Int)(s->counter - counter) * CHACHA20_BLOCK_SIZE;
    } else {
        s->counter = counter;
        s->len = 0;
    }
}

/* xorKeyStreamBlocksGeneric: n bytes, a whole number of blocks, from src to
 * dst, which may be the same. */
static void chacha20_xor_blocks(Chacha20Cipher *s, Byte *dst, const Byte *src, Int n) {
    if (n % CHACHA20_BLOCK_SIZE != 0)
        panic_str(BURROW_S("chacha20: internal error: wrong dst and/or src length"));

    /* The state as it is for every block but for the counter, c12. */
    const uint32_t c0 = CHACHA20_J0, c1 = CHACHA20_J1, c2 = CHACHA20_J2,
                   c3 = CHACHA20_J3;
    const uint32_t c4 = s->key[0], c5 = s->key[1], c6 = s->key[2], c7 = s->key[3];
    const uint32_t c8 = s->key[4], c9 = s->key[5], c10 = s->key[6], c11 = s->key[7];
    const uint32_t c13 = s->nonce[0], c14 = s->nonce[1], c15 = s->nonce[2];

    /* Three of the four quarter rounds of the first column round leave the
     * counter alone, so they come out the same for every block. */
    if (!s->precomp_done) {
        uint32_t a = c1, b = c5, c = c9, d = c13;
        CHACHA20_QR(a, b, c, d);
        s->p1 = a, s->p5 = b, s->p9 = c, s->p13 = d;
        a = c2, b = c6, c = c10, d = c14;
        CHACHA20_QR(a, b, c, d);
        s->p2 = a, s->p6 = b, s->p10 = c, s->p14 = d;
        a = c3, b = c7, c = c11, d = c15;
        CHACHA20_QR(a, b, c, d);
        s->p3 = a, s->p7 = b, s->p11 = c, s->p15 = d;
        s->precomp_done = true;
    }

    for (; n >= CHACHA20_BLOCK_SIZE; n -= CHACHA20_BLOCK_SIZE,
                                     src += CHACHA20_BLOCK_SIZE,
                                     dst += CHACHA20_BLOCK_SIZE) {
        /* The remainder of the first column round. */
        uint32_t fcr0 = c0, fcr4 = c4, fcr8 = c8, fcr12 = s->counter;
        CHACHA20_QR(fcr0, fcr4, fcr8, fcr12);

        /* The second diagonal round. */
        uint32_t x0 = fcr0, x5 = s->p5, x10 = s->p10, x15 = s->p15;
        CHACHA20_QR(x0, x5, x10, x15);
        uint32_t x1 = s->p1, x6 = s->p6, x11 = s->p11, x12 = fcr12;
        CHACHA20_QR(x1, x6, x11, x12);
        uint32_t x2 = s->p2, x7 = s->p7, x8 = fcr8, x13 = s->p13;
        CHACHA20_QR(x2, x7, x8, x13);
        uint32_t x3 = s->p3, x4 = fcr4, x9 = s->p9, x14 = s->p14;
        CHACHA20_QR(x3, x4, x9, x14);

        /* The remaining 18 rounds. */
        for (int i = 0; i < 9; i++) {
            /* Column round. */
            CHACHA20_QR(x0, x4, x8, x12);
            CHACHA20_QR(x1, x5, x9, x13);
            CHACHA20_QR(x2, x6, x10, x14);
            CHACHA20_QR(x3, x7, x11, x15);

            /* Diagonal round. */
            CHACHA20_QR(x0, x5, x10, x15);
            CHACHA20_QR(x1, x6, x11, x12);
            CHACHA20_QR(x2, x7, x8, x13);
            CHACHA20_QR(x3, x4, x9, x14);
        }

        /* Add back the initial state to generate the key stream, then XOR the
         * key stream with the source and write out the result. */
        const uint32_t x[16] = {x0, x1, x2,  x3,  x4,  x5,  x6,  x7,
                                x8, x9, x10, x11, x12, x13, x14, x15};
        const uint32_t c[16] = {c0, c1, c2,  c3,  c4,         c5,  c6,  c7,
                                c8, c9, c10, c11, s->counter, c13, c14, c15};
        for (int i = 0; i < 16; i++)
            chacha20_put_le32(dst + 4 * i, chacha20_le32(src + 4 * i) ^ (x[i] + c[i]));

        s->counter += 1;
    }
}

void chacha20_cipher_xor_key_stream(Chacha20Cipher *s, Slice dst, Slice src) {
    if (src.len == 0)
        return;
    if (dst.len < src.len)
        panic_str(BURROW_S("chacha20: output smaller than input"));
    Byte *d = (Byte *)dst.p;
    const Byte *in = (const Byte *)src.p;
    Int n = src.len;
    if (chacha20_inexact_overlap(d, n, in, n))
        panic_str(BURROW_S("chacha20: invalid buffer overlap"));

    /* First, drain any remaining key stream from a previous XORKeyStream. */
    if (s->len != 0) {
        const Byte *key_stream = s->buf + CHACHA20_BLOCK_SIZE - s->len;
        Int k = s->len < n ? s->len : n;
        for (Int i = 0; i < k; i++)
            d[i] = in[i] ^ key_stream[i];
        s->len -= k;
        d += k;
        in += k;
        n -= k;
    }
    if (n == 0)
        return;

    /* If we'd need to let the counter overflow and keep generating output,
     * panic immediately. If instead we'd only reach the last block, remember
     * not to generate any more output after the buffer is drained. */
    uint64_t num_blocks = ((uint64_t)n + CHACHA20_BLOCK_SIZE - 1) / CHACHA20_BLOCK_SIZE;
    if (s->overflow || (uint64_t)s->counter + num_blocks > (uint64_t)1 << 32)
        panic_str(BURROW_S("chacha20: counter overflow"));
    else if ((uint64_t)s->counter + num_blocks == (uint64_t)1 << 32)
        s->overflow = true;

    /* xorKeyStreamBlocks implementations expect input lengths that are a
     * multiple of the buffer size, a block here. */
    Int full = n - n % CHACHA20_BLOCK_SIZE;
    if (full > 0)
        chacha20_xor_blocks(s, d, in, full);
    d += full;
    in += full;
    n -= full;

    /* Go handles a buffer of several blocks that would cross the end of the
     * counter here. This buffer is one block, which never does. */
    if (n > 0) {
        memset(s->buf, 0, sizeof s->buf);
        memcpy(s->buf, in, (size_t)n);
        chacha20_xor_blocks(s, s->buf, s->buf, CHACHA20_BLOCK_SIZE);
        memcpy(d, s->buf, (size_t)n);
        s->len = CHACHA20_BLOCK_SIZE - n;
    }
}

static void chacha20_stream_xor(void *self, Slice dst, Slice src) {
    chacha20_cipher_xor_key_stream((Chacha20Cipher *)self, dst, src);
}

static const CipherStreamVT chacha20_stream_vt = {NULL, chacha20_stream_xor};

CipherStream chacha20_cipher_as_cipher_stream(Chacha20Cipher *s) {
    return (CipherStream){&chacha20_stream_vt, s};
}

Slice chacha20_hchacha20(Alloc *a, Slice key, Slice nonce, Error *err) {
    if (key.len != CHACHA20_KEY_SIZE) {
        BURROW_OUT(err, chacha20_error("chacha20: wrong HChaCha20 key size"));
        return slice_nil(TYPE_BYTE);
    }
    if (nonce.len != 16) {
        BURROW_OUT(err, chacha20_error("chacha20: wrong HChaCha20 nonce size"));
        return slice_nil(TYPE_BYTE);
    }
    Slice out = slice_make(a, TYPE_BYTE, 32, 32);
    if (out.p == NULL) {
        BURROW_OUT(err, burrow_err_out_of_memory);
        return out;
    }
    chacha20_hchacha20_core((Byte *)out.p, (const Byte *)key.p, (const Byte *)nonce.p);
    BURROW_OUT(err, BURROW_NO_ERROR);
    return out;
}

/* ---------------------------------------------------------------- poly1305 */

/* Poly1305 [RFC 7539] is a relatively simple algorithm: the authentication tag
 * for a 64 bytes message is approximately
 *
 *     s + m[0:16] * r⁴ + m[16:32] * r³ + m[32:48] * r² + m[48:64] * r  mod 2¹³⁰ - 5
 *
 * for some secret r and s. It can be computed sequentially like
 *
 *     for len(msg) > 0:
 *         h += read(msg, 16)
 *         h *= r
 *         h %= 2¹³⁰ - 5
 *     return h + s
 *
 * All the complexity is about doing performant constant-time math on numbers
 * larger than any available numeric type. h is kept in three 64 bit limbs, of
 * which the third holds only a few bits. */

/* r is clamped so that its top four bits of every 32 bit word and the bottom
 * two bits of the last three are clear, which is what keeps the products
 * below from overflowing. */
#define POLY1305_R_MASK0 UINT64_C(0x0FFFFFFC0FFFFFFF)
#define POLY1305_R_MASK1 UINT64_C(0x0FFFFFFC0FFFFFFC)

void poly1305_mac_init(Poly1305Mac *h, const Byte key[CHACHA20_KEY_SIZE]) {
    memset(h, 0, sizeof *h);
    h->r[0] = poly1305_le64(key + 0) & POLY1305_R_MASK0;
    h->r[1] = poly1305_le64(key + 8) & POLY1305_R_MASK1;
    h->s[0] = poly1305_le64(key + 16);
    h->s[1] = poly1305_le64(key + 24);
}

typedef struct Poly1305U128 {
    uint64_t lo, hi;
} Poly1305U128;

static Poly1305U128 poly1305_mul64(uint64_t a, uint64_t b) {
    Poly1305U128 r;
    r.hi = bits_mul64(a, b, &r.lo);
    return r;
}

static Poly1305U128 poly1305_add128(Poly1305U128 a, Poly1305U128 b) {
    uint64_t c;
    Poly1305U128 r;
    r.lo = bits_add64(a.lo, b.lo, 0, &c);
    r.hi = bits_add64(a.hi, b.hi, c, &c);
    if (c != 0)
        panic_str(BURROW_S("poly1305: unexpected overflow"));
    return r;
}

#define POLY1305_MASK_LOW2 UINT64_C(0x0000000000000003)
#define POLY1305_MASK_NOT_LOW2 (~POLY1305_MASK_LOW2)

/* updateGeneric: absorbs msg into h, 16 bytes at a time, with a final short
 * block padded with a 1 byte and zeros. */
static void poly1305_update(uint64_t h[3], const uint64_t r[2], const Byte *msg,
                            Int n) {
    uint64_t h0 = h[0], h1 = h[1], h2 = h[2];
    uint64_t r0 = r[0], r1 = r[1];

    while (n > 0) {
        /* For the first step, h + m, we use a chain of bits_add64 intrinsics.
         * The resulting value of h might exceed 2¹³⁰ - 5, but will be partially
         * reduced at the end of the multiplication below.
         *
         * The spec requires us to set a bit just above the message size, not
         * to hide leading zeroes. For full chunks, that's 1 << 128, so we can
         * just add 1 to the most significant (2¹²⁸) limb, h2. */
        uint64_t c;
        if (n >= POLY1305_TAG_SIZE) {
            h0 = bits_add64(h0, poly1305_le64(msg), 0, &c);
            h1 = bits_add64(h1, poly1305_le64(msg + 8), c, &c);
            h2 += c + 1;
            msg += POLY1305_TAG_SIZE;
            n -= POLY1305_TAG_SIZE;
        } else {
            Byte buf[POLY1305_TAG_SIZE] = {0};
            memcpy(buf, msg, (size_t)n);
            buf[n] = 1;
            h0 = bits_add64(h0, poly1305_le64(buf), 0, &c);
            h1 = bits_add64(h1, poly1305_le64(buf + 8), c, &c);
            h2 += c;
            n = 0;
        }

        /* Multiplication of big number limbs is similar to elementary school
         * columnar multiplication. Instead of digits, there are 64 bit limbs.
         *
         * We are multiplying a 3 limbs number, h, by a 2 limbs number, r.
         *
         *                        h2    h1    h0  x
         *                              r1    r0  =
         *                       ----------------
         *                      h2r0  h1r0  h0r0     <-- individual 128 bit products
         *            +   h2r1  h1r1  h0r1
         *               ------------------------
         *                 m3    m2    m1    m0      <-- result in 128 bit overlapping limbs
         *               ------------------------
         *         m3.hi m2.hi m1.hi m0.hi           <-- carry propagation
         *     +         m3.lo m2.lo m1.lo m0.lo
         *        -------------------------------
         *           t4    t3    t2    t1    t0      <-- final result in 64 bit limbs
         *
         * The main difference from pen-and-paper multiplication is that we do
         * carry propagation in a separate step, as if we wrote two digit sums
         * at first (the 128 bit limbs), and then carried the tens all at once. */
        Poly1305U128 h0r0 = poly1305_mul64(h0, r0);
        Poly1305U128 h1r0 = poly1305_mul64(h1, r0);
        Poly1305U128 h2r0 = poly1305_mul64(h2, r0);
        Poly1305U128 h0r1 = poly1305_mul64(h0, r1);
        Poly1305U128 h1r1 = poly1305_mul64(h1, r1);
        Poly1305U128 h2r1 = poly1305_mul64(h2, r1);

        /* Since h2 is known to be at most 7 (5 + 1 + 1), and r0 and r1 have
         * their top 4 bits cleared by the mask, we know that their product is
         * not going to overflow 64 bits, so we can ignore the high part of the
         * products.
         *
         * This also means that the product doesn't have a fifth limb (t4). */
        if (h2r0.hi != 0)
            panic_str(BURROW_S("poly1305: unexpected overflow"));
        if (h2r1.hi != 0)
            panic_str(BURROW_S("poly1305: unexpected overflow"));

        Poly1305U128 m0 = h0r0;
        /* These two additions don't overflow thanks again to the 4 masked bits
         * at the top of r0 and r1. */
        Poly1305U128 m1 = poly1305_add128(h1r0, h0r1);
        Poly1305U128 m2 = poly1305_add128(h2r0, h1r1);
        Poly1305U128 m3 = h2r1;

        uint64_t t0 = m0.lo;
        uint64_t t1 = bits_add64(m1.lo, m0.hi, 0, &c);
        uint64_t t2 = bits_add64(m2.lo, m1.hi, c, &c);
        uint64_t t3 = bits_add64(m3.lo, m2.hi, c, NULL);

        /* Now we have the result as 4 64 bit limbs, and we need to reduce it
         * modulo 2¹³⁰ - 5. The special shape of this Crandall prime lets us do
         * a cheap partial reduction according to the reduction identity
         *
         *     c * 2¹³⁰ + n  =  c * 5 + n  mod  2¹³⁰ - 5
         *
         * because 2¹³⁰ = 5 mod 2¹³⁰ - 5. Partial reduction since the result is
         * likely to be larger than 2¹³⁰ - 5, but still small enough to fit the
         * assumptions we make about h in the rest of the code.
         *
         * We split the final result at the 2¹³⁰ mark into h and cc, the carry.
         * Note that the carry bits are effectively shifted left by 2, in other
         * words, cc = c * 4 for the c in the reduction identity. */
        h0 = t0;
        h1 = t1;
        h2 = t2 & POLY1305_MASK_LOW2;
        Poly1305U128 cc = {t2 & POLY1305_MASK_NOT_LOW2, t3};

        /* To add c * 5 to h, we first add cc = c * 4, and then add (cc >> 2) =
         * c. */
        h0 = bits_add64(h0, cc.lo, 0, &c);
        h1 = bits_add64(h1, cc.hi, c, &c);
        h2 += c;

        cc.lo = cc.lo >> 2 | (cc.hi & 3) << 62;
        cc.hi = cc.hi >> 2;

        h0 = bits_add64(h0, cc.lo, 0, &c);
        h1 = bits_add64(h1, cc.hi, c, &c);
        h2 += c;

        /* h2 is at most 3 + 1 + 1 = 5, making the whole of h at most
         *
         *     5 * 2¹²⁸ + (2¹²⁸ - 1) = 6 * 2¹²⁸ - 1 */
    }

    h[0] = h0;
    h[1] = h1;
    h[2] = h2;
}

/* finalize: the tag, h reduced all the way modulo 2¹³⁰ - 5, plus s, mod
 * 2¹²⁸. */
static void poly1305_finalize(Byte out[POLY1305_TAG_SIZE], const uint64_t h[3],
                              const uint64_t s[2]) {
    uint64_t h0 = h[0], h1 = h[1], h2 = h[2];

    /* After the partial reduction in update, h might be more than 2¹³⁰ - 5,
     * but will be less than 2 * (2¹³⁰ - 5). To complete the reduction in
     * constant time, we compute t = h - (2¹³⁰ - 5), and select h as the
     * result if the subtraction underflows, and t otherwise. */
    uint64_t b;
    uint64_t h_minus_p0 = bits_sub64(h0, UINT64_C(0xFFFFFFFFFFFFFFFB), 0, &b);
    uint64_t h_minus_p1 = bits_sub64(h1, UINT64_C(0xFFFFFFFFFFFFFFFF), b, &b);
    (void)bits_sub64(h2, UINT64_C(0x0000000000000003), b, &b);

    /* h = h if h < p else h - p, as Go's select64. */
    h0 = (~(b - 1) & h0) | ((b - 1) & h_minus_p0);
    h1 = (~(b - 1) & h1) | ((b - 1) & h_minus_p1);

    /* Finally, we compute the last Poly1305 step
     *
     *     tag = h + s  mod  2¹²⁸
     *
     * by just doing a wide addition with the 128 low bits of h and discarding
     * the overflow. */
    uint64_t c;
    h0 = bits_add64(h0, s[0], 0, &c);
    h1 = bits_add64(h1, s[1], c, NULL);

    poly1305_put_le64(out + 0, h0);
    poly1305_put_le64(out + 8, h1);
}

/* macGeneric.Write, without the check for a finished MAC. */
static void poly1305_absorb(Poly1305Mac *h, const Byte *p, Int n) {
    if (h->offset > 0) {
        Int k = POLY1305_TAG_SIZE - h->offset;
        if (k > n)
            k = n;
        memcpy(h->buffer + h->offset, p, (size_t)k);
        if (h->offset + k < POLY1305_TAG_SIZE) {
            h->offset += k;
            return;
        }
        p += k;
        n -= k;
        h->offset = 0;
        poly1305_update(h->h, h->r, h->buffer, POLY1305_TAG_SIZE);
    }
    Int full = n - n % POLY1305_TAG_SIZE;
    if (full > 0) {
        poly1305_update(h->h, h->r, p, full);
        p += full;
        n -= full;
    }
    if (n > 0) {
        memcpy(h->buffer + h->offset, p, (size_t)n);
        h->offset += n;
    }
}

/* macGeneric.Sum: the tag so far, leaving h as it was so that more can be
 * written, which only the generic code allows. */
static void poly1305_tag(const Poly1305Mac *h, Byte out[POLY1305_TAG_SIZE]) {
    uint64_t state[3] = {h->h[0], h->h[1], h->h[2]};
    if (h->offset > 0)
        poly1305_update(state, h->r, h->buffer, h->offset);
    poly1305_finalize(out, state, h->s);
}

void poly1305_sum(Byte out[POLY1305_TAG_SIZE], Slice m,
                  const Byte key[CHACHA20_KEY_SIZE]) {
    Poly1305Mac h;
    poly1305_mac_init(&h, key);
    poly1305_absorb(&h, (const Byte *)m.p, m.len);
    poly1305_tag(&h, out);
}

bool poly1305_verify(const Byte mac[POLY1305_TAG_SIZE], Slice m,
                     const Byte key[CHACHA20_KEY_SIZE]) {
    Byte tmp[POLY1305_TAG_SIZE];
    poly1305_sum(tmp, m, key);
    return subtle_constant_time_compare(
               slice_from(tmp, POLY1305_TAG_SIZE, POLY1305_TAG_SIZE, TYPE_BYTE),
               slice_from((void *)(uintptr_t)mac, POLY1305_TAG_SIZE, POLY1305_TAG_SIZE,
                          TYPE_BYTE)) == 1;
}

Poly1305Mac *poly1305_new(Alloc *a, const Byte key[CHACHA20_KEY_SIZE]) {
    Poly1305Mac *h = BURROW_NEW(a, Poly1305Mac);
    if (h != NULL)
        poly1305_mac_init(h, key);
    return h;
}

Int poly1305_mac_size(const Poly1305Mac *h) {
    (void)h;
    return POLY1305_TAG_SIZE;
}

Int poly1305_mac_write(Poly1305Mac *h, Slice p) {
    if (h->finalized)
        panic_str(BURROW_S("poly1305: write to MAC after Sum or Verify"));
    poly1305_absorb(h, (const Byte *)p.p, p.len);
    return p.len;
}

Slice poly1305_mac_sum(Poly1305Mac *h, Alloc *a, Slice b) {
    Byte mac[POLY1305_TAG_SIZE];
    poly1305_tag(h, mac);
    h->finalized = true;
    return slice_append(a, b, mac, POLY1305_TAG_SIZE);
}

bool poly1305_mac_verify(Poly1305Mac *h, Slice expected) {
    Byte mac[POLY1305_TAG_SIZE];
    poly1305_tag(h, mac);
    h->finalized = true;
    return subtle_constant_time_compare(
               expected,
               slice_from(mac, POLY1305_TAG_SIZE, POLY1305_TAG_SIZE, TYPE_BYTE)) == 1;
}

/* -------------------------------------------------------- chacha20poly1305 */

BURROW_SENTINEL_ERROR(burrow__chacha20poly1305_err_open,
                      "chacha20poly1305: message authentication failed");

typedef struct Chacha20poly1305 {
    Byte key[CHACHA20POLY1305_KEY_SIZE];
} Chacha20poly1305;

/* Go's sliceForAppend: dst with n more bytes on the end, in place when its
 * capacity allows and copied to a new array from a when it does not. *tail is
 * where the n bytes start, NULL when a is out of memory. */
static Slice chacha20poly1305_slice_for_append(Alloc *a, Slice dst, Int n,
                                               Byte **tail) {
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
        if (dst.len > 0)
            memcpy(head.p, dst.p, (size_t)dst.len);
    }
    *tail = (Byte *)head.p + dst.len;
    return head;
}

/* writeWithPadding: b, then zeros up to the next multiple of 16. */
static void chacha20poly1305_write_padded(Poly1305Mac *p, const Byte *b, Int n) {
    poly1305_absorb(p, b, n);
    Int rem = n % 16;
    if (rem != 0) {
        static const Byte zeros[16] = {0};
        poly1305_absorb(p, zeros, 16 - rem);
    }
}

/* writeUint64. */
static void chacha20poly1305_write_u64(Poly1305Mac *p, Int n) {
    Byte buf[8];
    poly1305_put_le64(buf, (uint64_t)n);
    poly1305_absorb(p, buf, 8);
}

/* The one-time Poly1305 key, the first 32 bytes of key stream, with s moved on
 * to block 1, where the message starts. */
static void chacha20poly1305_start(Chacha20Cipher *s, Poly1305Mac *p, const Byte *key,
                                   const Byte *nonce) {
    Byte poly_key[32] = {0};
    chacha20_setup(s, key, nonce);
    chacha20_cipher_xor_key_stream(s, slice_from(poly_key, 32, 32, TYPE_BYTE),
                                   slice_from(poly_key, 32, 32, TYPE_BYTE));
    /* Set the counter to 1, skipping 32 bytes. */
    chacha20_cipher_set_counter(s, 1);
    poly1305_mac_init(p, poly_key);
    memset(poly_key, 0, sizeof poly_key);
}

/* sealGeneric, after Seal's checks. */
static Slice chacha20poly1305_seal_with(const Byte *key, Alloc *a, Slice dst,
                                        const Byte *nonce, Slice plaintext,
                                        Slice additional_data) {
    Byte *out;
    Int n = plaintext.len + POLY1305_TAG_SIZE;
    Slice ret = chacha20poly1305_slice_for_append(a, dst, n, &out);
    if (out == NULL)
        return ret;
    if (chacha20_inexact_overlap(out, n, plaintext.p, plaintext.len))
        panic_str(
            BURROW_S("chacha20poly1305: invalid buffer overlap of output and input"));
    if (chacha20_any_overlap(out, n, additional_data.p, additional_data.len))
        panic_str(BURROW_S(
            "chacha20poly1305: invalid buffer overlap of output and additional data"));
    Byte *ciphertext = out;
    Byte *tag = out + plaintext.len;

    Chacha20Cipher s;
    Poly1305Mac p;
    chacha20poly1305_start(&s, &p, key, nonce);
    chacha20_cipher_xor_key_stream(
        &s, slice_from(ciphertext, plaintext.len, plaintext.len, TYPE_BYTE), plaintext);

    chacha20poly1305_write_padded(&p, (const Byte *)additional_data.p,
                                  additional_data.len);
    chacha20poly1305_write_padded(&p, ciphertext, plaintext.len);
    chacha20poly1305_write_u64(&p, additional_data.len);
    chacha20poly1305_write_u64(&p, plaintext.len);
    poly1305_tag(&p, tag);
    return ret;
}

/* openGeneric, after Open's checks. */
static Slice chacha20poly1305_open_with(const Byte *key, Alloc *a, Slice dst,
                                        const Byte *nonce, Slice ciphertext,
                                        Slice additional_data, Error *err) {
    const Byte *tag = (const Byte *)ciphertext.p + ciphertext.len - 16;
    Int n = ciphertext.len - 16;
    const Byte *ct = (const Byte *)ciphertext.p;

    Chacha20Cipher s;
    Poly1305Mac p;
    chacha20poly1305_start(&s, &p, key, nonce);

    chacha20poly1305_write_padded(&p, (const Byte *)additional_data.p,
                                  additional_data.len);
    chacha20poly1305_write_padded(&p, ct, n);
    chacha20poly1305_write_u64(&p, additional_data.len);
    chacha20poly1305_write_u64(&p, n);

    Byte *out;
    Slice ret = chacha20poly1305_slice_for_append(a, dst, n, &out);
    if (out == NULL) {
        BURROW_OUT(err, burrow_err_out_of_memory);
        return ret;
    }
    if (chacha20_inexact_overlap(out, n, ct, n))
        panic_str(
            BURROW_S("chacha20poly1305: invalid buffer overlap of output and input"));
    if (chacha20_any_overlap(out, n, additional_data.p, additional_data.len))
        panic_str(BURROW_S(
            "chacha20poly1305: invalid buffer overlap of output and additional data"));
    Byte want[POLY1305_TAG_SIZE];
    poly1305_tag(&p, want);
    if (subtle_constant_time_compare(
            slice_from((void *)(uintptr_t)tag, 16, 16, TYPE_BYTE),
            slice_from(want, POLY1305_TAG_SIZE, POLY1305_TAG_SIZE, TYPE_BYTE)) != 1) {
        if (n > 0)
            memset(out, 0, (size_t)n);
        BURROW_OUT(err, burrow__chacha20poly1305_err_open);
        return slice_nil(TYPE_BYTE);
    }

    chacha20_cipher_xor_key_stream(&s, slice_from(out, n, n, TYPE_BYTE),
                                   slice_from((void *)(uintptr_t)ct, n, n, TYPE_BYTE));
    BURROW_OUT(err, BURROW_NO_ERROR);
    return ret;
}

/* Seal and Open's checks on the sizes, which both variants share. */
static void chacha20poly1305_check_seal(Slice nonce, Int nonce_size, Slice plaintext) {
    if (nonce.len != nonce_size)
        panic_str(BURROW_S("chacha20poly1305: bad nonce length passed to Seal"));
    /* XChaCha20-Poly1305 technically supports a 64 bit counter, so there is no
     * size limit. However, since it reuses the ChaCha20-Poly1305
     * implementation, the second half of the counter is not available. This is
     * unlikely to be an issue because the AEAD API requires the entire message
     * to be in memory, and the counter overflows at 256 GB. */
    if ((uint64_t)plaintext.len > ((uint64_t)1 << 38) - 64)
        panic_str(BURROW_S("chacha20poly1305: plaintext too large"));
}

static bool chacha20poly1305_check_open(Slice nonce, Int nonce_size, Slice ciphertext,
                                        Error *err) {
    if (nonce.len != nonce_size)
        panic_str(BURROW_S("chacha20poly1305: bad nonce length passed to Open"));
    if (ciphertext.len < 16) {
        BURROW_OUT(err, burrow__chacha20poly1305_err_open);
        return false;
    }
    if ((uint64_t)ciphertext.len > ((uint64_t)1 << 38) - 48)
        panic_str(BURROW_S("chacha20poly1305: ciphertext too large"));
    return true;
}

static Int chacha20poly1305_nonce_size(void *self) {
    (void)self;
    return CHACHA20POLY1305_NONCE_SIZE;
}

static Int chacha20poly1305_overhead(void *self) {
    (void)self;
    return CHACHA20POLY1305_OVERHEAD;
}

static Slice chacha20poly1305_seal(void *self, Alloc *a, Slice dst, Slice nonce,
                                   Slice plaintext, Slice additional_data) {
    const Chacha20poly1305 *c = (const Chacha20poly1305 *)self;
    chacha20poly1305_check_seal(nonce, CHACHA20POLY1305_NONCE_SIZE, plaintext);
    return chacha20poly1305_seal_with(c->key, a, dst, (const Byte *)nonce.p, plaintext,
                                      additional_data);
}

static Slice chacha20poly1305_open(void *self, Alloc *a, Slice dst, Slice nonce,
                                   Slice ciphertext, Slice additional_data,
                                   Error *err) {
    const Chacha20poly1305 *c = (const Chacha20poly1305 *)self;
    if (!chacha20poly1305_check_open(nonce, CHACHA20POLY1305_NONCE_SIZE, ciphertext,
                                     err))
        return slice_nil(TYPE_BYTE);
    return chacha20poly1305_open_with(c->key, a, dst, (const Byte *)nonce.p, ciphertext,
                                      additional_data, err);
}

static const CipherAEADVT chacha20poly1305_vt = {
    NULL, chacha20poly1305_nonce_size, chacha20poly1305_overhead, chacha20poly1305_seal,
    chacha20poly1305_open};

/* The key and nonce of the ChaCha20-Poly1305 that an XChaCha20-Poly1305 nonce
 * stands for. The first 4 bytes of the final nonce are unused counter
 * space. */
static void chacha20poly1305_derive_x(const Byte *key, const Byte *nonce, Byte hkey[32],
                                      Byte cnonce[12]) {
    chacha20_hchacha20_core(hkey, key, nonce);
    memset(cnonce, 0, 4);
    memcpy(cnonce + 4, nonce + 16, 8);
}

static Int xchacha20poly1305_nonce_size(void *self) {
    (void)self;
    return CHACHA20POLY1305_NONCE_SIZE_X;
}

static Slice xchacha20poly1305_seal(void *self, Alloc *a, Slice dst, Slice nonce,
                                    Slice plaintext, Slice additional_data) {
    const Chacha20poly1305 *x = (const Chacha20poly1305 *)self;
    chacha20poly1305_check_seal(nonce, CHACHA20POLY1305_NONCE_SIZE_X, plaintext);
    Byte hkey[32], cnonce[12];
    chacha20poly1305_derive_x(x->key, (const Byte *)nonce.p, hkey, cnonce);
    Slice ret =
        chacha20poly1305_seal_with(hkey, a, dst, cnonce, plaintext, additional_data);
    memset(hkey, 0, sizeof hkey);
    return ret;
}

static Slice xchacha20poly1305_open(void *self, Alloc *a, Slice dst, Slice nonce,
                                    Slice ciphertext, Slice additional_data,
                                    Error *err) {
    const Chacha20poly1305 *x = (const Chacha20poly1305 *)self;
    if (!chacha20poly1305_check_open(nonce, CHACHA20POLY1305_NONCE_SIZE_X, ciphertext,
                                     err))
        return slice_nil(TYPE_BYTE);
    Byte hkey[32], cnonce[12];
    chacha20poly1305_derive_x(x->key, (const Byte *)nonce.p, hkey, cnonce);
    Slice ret = chacha20poly1305_open_with(hkey, a, dst, cnonce, ciphertext,
                                           additional_data, err);
    memset(hkey, 0, sizeof hkey);
    return ret;
}

static const CipherAEADVT xchacha20poly1305_vt = {
    NULL, xchacha20poly1305_nonce_size, chacha20poly1305_overhead,
    xchacha20poly1305_seal, xchacha20poly1305_open};

static CipherAEAD chacha20poly1305_make(Alloc *a, Slice key, const CipherAEADVT *vt,
                                        Error *err) {
    if (key.len != CHACHA20POLY1305_KEY_SIZE) {
        BURROW_OUT(err, chacha20_error("chacha20poly1305: bad key length"));
        return (CipherAEAD){NULL, NULL};
    }
    Chacha20poly1305 *c = BURROW_NEW(a, Chacha20poly1305);
    if (c == NULL) {
        BURROW_OUT(err, burrow_err_out_of_memory);
        return (CipherAEAD){NULL, NULL};
    }
    memcpy(c->key, key.p, CHACHA20POLY1305_KEY_SIZE);
    BURROW_OUT(err, BURROW_NO_ERROR);
    return (CipherAEAD){vt, c};
}

CipherAEAD chacha20poly1305_new(Alloc *a, Slice key, Error *err) {
    return chacha20poly1305_make(a, key, &chacha20poly1305_vt, err);
}

CipherAEAD chacha20poly1305_new_x(Alloc *a, Slice key, Error *err) {
    return chacha20poly1305_make(a, key, &xchacha20poly1305_vt, err);
}
