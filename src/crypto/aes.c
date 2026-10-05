/* Derived from Go's src/crypto/aes/aes.go and
 * src/crypto/internal/fips140/aes/aes.go, aes_asm.go and aes_generic.go.
 * Go source: go1.27.1.
 *
 * Go's portable AES looks up tables indexed by the state, which leaks the key
 * through the cache to anyone who can time it. The portable code here is
 * bitsliced instead, and does the same work whatever the key and the data are.
 * Four blocks go through at once, spread over eight 64 bit words so that word i
 * holds bit i of all 64 bytes. SubBytes is then a circuit of ANDs and XORs on
 * whole words, the one Boyar and Peralta published in "A depth-16 circuit for
 * the AES S-box" (2011), and ShiftRows and MixColumns are shifts and rotations
 * of every word, because of where each byte's bit goes:
 *
 *     bit 16*row + 4*column + block
 *
 * A row is 16 bits, so moving a row is a rotation by 16, and a column within a
 * row is 4 bits, so ShiftRows rotates each row's 16 bits by 4 times its
 * number. The inverse S-box is the forward one between two copies of the
 * affine map that undoes its affine part. BearSSL's aes_ct64 works the same
 * way and was the model for this one.
 *
 * Where the processor has AES instructions they are used instead, and they
 * are constant time too.
 *
 * Copyright 2009 The Go Authors. All rights reserved.
 * Copyright 2026 The burrow Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style licence that can be found
 * in the LICENSE file. */

#include "burrow/crypto/aes.h"

#include "burrow/atomic.h"
#include "burrow/core.h"
#include "burrow/crypto/cipher.h"
#include "burrow/error.h"
#include "burrow/mem.h"
#include "burrow/pal.h"
#include "burrow/panic.h"
#include "burrow/slice.h"
#include "burrow/type.h"

#include "aes_internal.h"
#include "internal.h"

#include <stdint.h>
#include <string.h>

/* ---------------------------------------------------------- KeySizeError */

/* The length first, so the data pointer of the Error is a pointer to an
 * AesKeySizeError and errors_as hands it straight back. */
typedef struct AesKeySizeBox {
    AesKeySizeError k;
    Str message;
} AesKeySizeBox;

static const Type aes_key_size_desc = {
    {(const Byte *)"KeySizeError", 12},
    {(const Byte *)"crypto/aes", 10},
    KIND_INT,
    (uint32_t)sizeof(AesKeySizeError),
    (uint16_t)_Alignof(AesKeySizeError),
    0,
    0,
    NULL,
    NULL,
    NULL,
    NULL,
    0,
    0x61656b73U, /* "aeks" */
    NULL,
};

const Type *const TYPE_AES_KEY_SIZE_ERROR = &aes_key_size_desc;

static const char aes_key_size_prefix[] = "crypto/aes: invalid key size ";

#define AES_PREFIX_LEN ((Int)sizeof(aes_key_size_prefix) - 1)

/* Writes the message for k to p, or only counts it when p is NULL, and returns
 * its length. */
static Int aes_key_size_message(Byte *p, AesKeySizeError k) {
    Byte digits[20];
    Int n = 0;
    uint64_t u = k < 0 ? (uint64_t)0 - (uint64_t)k : (uint64_t)k;
    do {
        digits[n++] = (Byte)('0' + (int)(u % 10));
        u /= 10;
    } while (u != 0);
    Int len = AES_PREFIX_LEN + (k < 0) + n;
    if (p != NULL) {
        memcpy(p, aes_key_size_prefix, (size_t)AES_PREFIX_LEN);
        p += AES_PREFIX_LEN;
        if (k < 0)
            *p++ = '-';
        while (n > 0)
            *p++ = digits[--n];
    }
    return len;
}

static Str aes_key_size_text(const void *self) {
    return ((const AesKeySizeBox *)self)->message;
}

static Error aes_key_size_clone(const void *self, Alloc *a) {
    return aes_key_size_error_as_error(((const AesKeySizeBox *)self)->k, a);
}

static const ErrorVT aes_key_size_vt = {
    .self_type = &aes_key_size_desc,
    .message = aes_key_size_text,
    .clone = aes_key_size_clone,
};

Error aes_key_size_error_as_error(AesKeySizeError k, Alloc *a) {
    Int mlen = aes_key_size_message(NULL, k);
    AesKeySizeBox *b = (AesKeySizeBox *)mem_alloc_nozero(
        a, sizeof(AesKeySizeBox) + (size_t)mlen, _Alignof(AesKeySizeBox));
    if (b == NULL)
        return burrow_err_out_of_memory;
    Byte *p = (Byte *)(b + 1);
    aes_key_size_message(p, k);
    b->k = k;
    b->message = str_from_bytes(p, mlen);
    return (Error){&aes_key_size_vt, b};
}

Str aes_key_size_error_error(AesKeySizeError k, Alloc *a) {
    Int mlen = aes_key_size_message(NULL, k);
    Byte *p = (Byte *)mem_alloc_nozero(a, (size_t)mlen, 1);
    if (p == NULL)
        return BURROW_STR_EMPTY;
    aes_key_size_message(p, k);
    return str_from_bytes(p, mlen);
}

/* ------------------------------------------------------------- bitsliced */

/* Swaps the bits of a picked out by m << n with the bits of b picked out by m.
 * Three rounds of it, on pairs of words chosen by one bit of their index each,
 * swap the index of a word with the index of a bit within each byte, which
 * turns eight words of bytes into eight words of bit planes and back. */
#define AES_SWAP(a, b, m, n)                                                           \
    do {                                                                               \
        uint64_t t_ = (((a) >> (n)) ^ (b)) & (m);                                      \
        (b) ^= t_;                                                                     \
        (a) ^= t_ << (n);                                                              \
    } while (0)

static void aes_ct_ortho(uint64_t q[8]) {
    const uint64_t m1 = UINT64_C(0x5555555555555555);
    const uint64_t m2 = UINT64_C(0x3333333333333333);
    const uint64_t m4 = UINT64_C(0x0F0F0F0F0F0F0F0F);
    AES_SWAP(q[0], q[1], m1, 1);
    AES_SWAP(q[2], q[3], m1, 1);
    AES_SWAP(q[4], q[5], m1, 1);
    AES_SWAP(q[6], q[7], m1, 1);
    AES_SWAP(q[0], q[2], m2, 2);
    AES_SWAP(q[1], q[3], m2, 2);
    AES_SWAP(q[4], q[6], m2, 2);
    AES_SWAP(q[5], q[7], m2, 2);
    AES_SWAP(q[0], q[4], m4, 4);
    AES_SWAP(q[1], q[5], m4, 4);
    AES_SWAP(q[2], q[6], m4, 4);
    AES_SWAP(q[3], q[7], m4, 4);
}

/* For each bit position p, the byte of the four blocks that lands there:
 * block p & 3, column (p >> 2) & 3 and row p >> 4, which is byte row + 4 *
 * column of the block. After aes_ct_ortho, byte k of word j ends up at bit
 * 8k + j of every plane. */
static const uint8_t aes_ct_index[64] = {
    0, 16, 32, 48, 4, 20, 36, 52, 8,  24, 40, 56, 12, 28, 44, 60,
    1, 17, 33, 49, 5, 21, 37, 53, 9,  25, 41, 57, 13, 29, 45, 61,
    2, 18, 34, 50, 6, 22, 38, 54, 10, 26, 42, 58, 14, 30, 46, 62,
    3, 19, 35, 51, 7, 23, 39, 55, 11, 27, 43, 59, 15, 31, 47, 63,
};

/* n blocks of in, at most four, into bit planes. The missing blocks are zero. */
static void aes_ct_load(uint64_t q[8], const Byte *in, size_t n) {
    memset(q, 0, 8 * sizeof q[0]);
    size_t len = n * 16;
    for (unsigned p = 0; p < 64; p++) {
        unsigned i = aes_ct_index[p];
        if (i < len)
            q[p & 7] |= (uint64_t)in[i] << (8 * (p >> 3));
    }
    aes_ct_ortho(q);
}

/* The first n blocks back out of bit planes. q is used up. */
static void aes_ct_store(Byte *out, uint64_t q[8], size_t n) {
    aes_ct_ortho(q);
    size_t len = n * 16;
    for (unsigned p = 0; p < 64; p++) {
        unsigned i = aes_ct_index[p];
        if (i < len)
            out[i] = (Byte)(q[p & 7] >> (8 * (p >> 3)));
    }
}

/* SubBytes on all 64 bytes: the Boyar-Peralta circuit, with q[0] the low bit.
 * The top linear layer, the shared nonlinear middle and the bottom linear
 * layer, with the S-box's constant 0x63 folded in as the four NOTs. */
static void aes_ct_sbox(uint64_t q[8]) {
    uint64_t x0 = q[7], x1 = q[6], x2 = q[5], x3 = q[4];
    uint64_t x4 = q[3], x5 = q[2], x6 = q[1], x7 = q[0];

    uint64_t y14 = x3 ^ x5;
    uint64_t y13 = x0 ^ x6;
    uint64_t y9 = x0 ^ x3;
    uint64_t y8 = x0 ^ x5;
    uint64_t t0 = x1 ^ x2;
    uint64_t y1 = t0 ^ x7;
    uint64_t y4 = y1 ^ x3;
    uint64_t y12 = y13 ^ y14;
    uint64_t y2 = y1 ^ x0;
    uint64_t y5 = y1 ^ x6;
    uint64_t y3 = y5 ^ y8;
    uint64_t t1 = x4 ^ y12;
    uint64_t y15 = t1 ^ x5;
    uint64_t y20 = t1 ^ x1;
    uint64_t y6 = y15 ^ x7;
    uint64_t y10 = y15 ^ t0;
    uint64_t y11 = y20 ^ y9;
    uint64_t y7 = x7 ^ y11;
    uint64_t y17 = y10 ^ y11;
    uint64_t y19 = y10 ^ y8;
    uint64_t y16 = t0 ^ y11;
    uint64_t y21 = y13 ^ y16;
    uint64_t y18 = x0 ^ y16;

    uint64_t t2 = y12 & y15;
    uint64_t t3 = y3 & y6;
    uint64_t t4 = t3 ^ t2;
    uint64_t t5 = y4 & x7;
    uint64_t t6 = t5 ^ t2;
    uint64_t t7 = y13 & y16;
    uint64_t t8 = y5 & y1;
    uint64_t t9 = t8 ^ t7;
    uint64_t t10 = y2 & y7;
    uint64_t t11 = t10 ^ t7;
    uint64_t t12 = y9 & y11;
    uint64_t t13 = y14 & y17;
    uint64_t t14 = t13 ^ t12;
    uint64_t t15 = y8 & y10;
    uint64_t t16 = t15 ^ t12;
    uint64_t t17 = t4 ^ t14;
    uint64_t t18 = t6 ^ t16;
    uint64_t t19 = t9 ^ t14;
    uint64_t t20 = t11 ^ t16;
    uint64_t t21 = t17 ^ y20;
    uint64_t t22 = t18 ^ y19;
    uint64_t t23 = t19 ^ y21;
    uint64_t t24 = t20 ^ y18;

    uint64_t t25 = t21 ^ t22;
    uint64_t t26 = t21 & t23;
    uint64_t t27 = t24 ^ t26;
    uint64_t t28 = t25 & t27;
    uint64_t t29 = t28 ^ t22;
    uint64_t t30 = t23 ^ t24;
    uint64_t t31 = t22 ^ t26;
    uint64_t t32 = t31 & t30;
    uint64_t t33 = t32 ^ t24;
    uint64_t t34 = t23 ^ t33;
    uint64_t t35 = t27 ^ t33;
    uint64_t t36 = t24 & t35;
    uint64_t t37 = t36 ^ t34;
    uint64_t t38 = t27 ^ t36;
    uint64_t t39 = t29 & t38;
    uint64_t t40 = t25 ^ t39;

    uint64_t t41 = t40 ^ t37;
    uint64_t t42 = t29 ^ t33;
    uint64_t t43 = t29 ^ t40;
    uint64_t t44 = t33 ^ t37;
    uint64_t t45 = t42 ^ t41;
    uint64_t z0 = t44 & y15;
    uint64_t z1 = t37 & y6;
    uint64_t z2 = t33 & x7;
    uint64_t z3 = t43 & y16;
    uint64_t z4 = t40 & y1;
    uint64_t z5 = t29 & y7;
    uint64_t z6 = t42 & y11;
    uint64_t z7 = t45 & y17;
    uint64_t z8 = t41 & y10;
    uint64_t z9 = t44 & y12;
    uint64_t z10 = t37 & y3;
    uint64_t z11 = t33 & y4;
    uint64_t z12 = t43 & y13;
    uint64_t z13 = t40 & y5;
    uint64_t z14 = t29 & y2;
    uint64_t z15 = t42 & y9;
    uint64_t z16 = t45 & y14;
    uint64_t z17 = t41 & y8;

    uint64_t t46 = z15 ^ z16;
    uint64_t t47 = z10 ^ z11;
    uint64_t t48 = z5 ^ z13;
    uint64_t t49 = z9 ^ z10;
    uint64_t t50 = z2 ^ z12;
    uint64_t t51 = z2 ^ z5;
    uint64_t t52 = z7 ^ z8;
    uint64_t t53 = z0 ^ z3;
    uint64_t t54 = z6 ^ z7;
    uint64_t t55 = z16 ^ z17;
    uint64_t t56 = z12 ^ t48;
    uint64_t t57 = t50 ^ t53;
    uint64_t t58 = z4 ^ t46;
    uint64_t t59 = z3 ^ t54;
    uint64_t t60 = t46 ^ t57;
    uint64_t t61 = z14 ^ t57;
    uint64_t t62 = t52 ^ t58;
    uint64_t t63 = t49 ^ t58;
    uint64_t t64 = z4 ^ t59;
    uint64_t t65 = t61 ^ t62;
    uint64_t t66 = z1 ^ t63;
    uint64_t s0 = t59 ^ t63;
    uint64_t s6 = t56 ^ ~t62;
    uint64_t s7 = t48 ^ ~t60;
    uint64_t t67 = t64 ^ t65;
    uint64_t s3 = t53 ^ t66;
    uint64_t s4 = t51 ^ t66;
    uint64_t s5 = t47 ^ t65;
    uint64_t s1 = t64 ^ ~s3;
    uint64_t s2 = t55 ^ ~t67;

    q[7] = s0;
    q[6] = s1;
    q[5] = s2;
    q[4] = s3;
    q[3] = s4;
    q[2] = s5;
    q[1] = s6;
    q[0] = s7;
}

/* The affine map that undoes the S-box's own, which is its own inverse up to
 * the constant: NOT of bits 0, 1, 5 and 6, then bit i is the XOR of bits
 * i + 2, i + 5 and i + 7. */
static void aes_ct_inv_affine(uint64_t q[8]) {
    uint64_t q0 = ~q[0], q1 = ~q[1], q2 = q[2], q3 = q[3];
    uint64_t q4 = q[4], q5 = ~q[5], q6 = ~q[6], q7 = q[7];
    q[0] = q2 ^ q5 ^ q7;
    q[1] = q3 ^ q6 ^ q0;
    q[2] = q4 ^ q7 ^ q1;
    q[3] = q5 ^ q0 ^ q2;
    q[4] = q6 ^ q1 ^ q3;
    q[5] = q7 ^ q2 ^ q4;
    q[6] = q0 ^ q3 ^ q5;
    q[7] = q1 ^ q4 ^ q6;
}

static void aes_ct_inv_sbox(uint64_t q[8]) {
    aes_ct_inv_affine(q);
    aes_ct_sbox(q);
    aes_ct_inv_affine(q);
}

/* Row r is bits 16r to 16r + 15, and ShiftRows moves column c + r to column c,
 * which is a rotation of the row right by 4r bits. */
static void aes_ct_shift_rows(uint64_t q[8]) {
    for (int i = 0; i < 8; i++) {
        uint64_t x = q[i];
        q[i] = (x & UINT64_C(0x000000000000FFFF)) |
               ((x >> 4) & UINT64_C(0x000000000FFF0000)) |
               ((x << 12) & UINT64_C(0x00000000F0000000)) |
               ((x >> 8) & UINT64_C(0x000000FF00000000)) |
               ((x << 8) & UINT64_C(0x0000FF0000000000)) |
               ((x >> 12) & UINT64_C(0x000F000000000000)) |
               ((x << 4) & UINT64_C(0xFFF0000000000000));
    }
}

static void aes_ct_inv_shift_rows(uint64_t q[8]) {
    for (int i = 0; i < 8; i++) {
        uint64_t x = q[i];
        q[i] = (x & UINT64_C(0x000000000000FFFF)) |
               ((x << 4) & UINT64_C(0x00000000FFF00000)) |
               ((x >> 12) & UINT64_C(0x00000000000F0000)) |
               ((x << 8) & UINT64_C(0x0000FF0000000000)) |
               ((x >> 8) & UINT64_C(0x000000FF00000000)) |
               ((x << 12) & UINT64_C(0xF000000000000000)) |
               ((x >> 4) & UINT64_C(0x0FFF000000000000));
    }
}

static inline uint64_t aes_ct_rotr(uint64_t x, unsigned n) {
    return (x >> n) | (x << (64 - n));
}

/* Multiplies every byte by x, modulo x^8 + x^4 + x^3 + x + 1. */
static void aes_ct_xtime(uint64_t q[8]) {
    uint64_t hi = q[7];
    q[7] = q[6];
    q[6] = q[5];
    q[5] = q[4];
    q[4] = q[3] ^ hi;
    q[3] = q[2] ^ hi;
    q[2] = q[1];
    q[1] = q[0] ^ hi;
    q[0] = hi;
}

/* Each byte becomes 2 * (s[r] ^ s[r + 1]) ^ s[r + 1] ^ s[r + 2] ^ s[r + 3] in
 * its column, and row r + k is the word rotated right by 16k. */
static void aes_ct_mix_columns(uint64_t q[8]) {
    uint64_t r1[8], rest[8];
    for (int i = 0; i < 8; i++) {
        r1[i] = aes_ct_rotr(q[i], 16);
        rest[i] = r1[i] ^ aes_ct_rotr(q[i], 32) ^ aes_ct_rotr(q[i], 48);
        q[i] ^= r1[i];
    }
    aes_ct_xtime(q);
    for (int i = 0; i < 8; i++)
        q[i] ^= rest[i];
}

/* InvMixColumns is MixColumns after each byte has had 4 * (s[r] ^ s[r + 2])
 * added to it, which is how the inverse matrix factors. */
static void aes_ct_inv_mix_columns(uint64_t q[8]) {
    uint64_t t[8];
    for (int i = 0; i < 8; i++)
        t[i] = q[i] ^ aes_ct_rotr(q[i], 32);
    aes_ct_xtime(t);
    aes_ct_xtime(t);
    for (int i = 0; i < 8; i++)
        q[i] ^= t[i];
    aes_ct_mix_columns(q);
}

static void aes_ct_add_key(uint64_t q[8], const uint64_t *sk) {
    for (int i = 0; i < 8; i++)
        q[i] ^= sk[i];
}

static void aes_ct_encrypt(const AesBlock *b, uint64_t q[8]) {
    const uint64_t *sk = b->sk;
    aes_ct_add_key(q, sk);
    for (int r = 1; r < b->rounds; r++) {
        aes_ct_sbox(q);
        aes_ct_shift_rows(q);
        aes_ct_mix_columns(q);
        aes_ct_add_key(q, sk + 8 * r);
    }
    aes_ct_sbox(q);
    aes_ct_shift_rows(q);
    aes_ct_add_key(q, sk + 8 * b->rounds);
}

static void aes_ct_decrypt(const AesBlock *b, uint64_t q[8]) {
    const uint64_t *sk = b->sk;
    aes_ct_add_key(q, sk + 8 * b->rounds);
    for (int r = b->rounds - 1; r > 0; r--) {
        aes_ct_inv_shift_rows(q);
        aes_ct_inv_sbox(q);
        aes_ct_add_key(q, sk + 8 * r);
        aes_ct_inv_mix_columns(q);
    }
    aes_ct_inv_shift_rows(q);
    aes_ct_inv_sbox(q);
    aes_ct_add_key(q, sk);
}

/* SubWord from the key schedule, through the same circuit, so that expanding
 * a key does not look up a table with it either. */
static void aes_ct_sub_word(Byte w[4]) {
    uint64_t q[8] = {0};
    for (int j = 0; j < 4; j++)
        for (int i = 0; i < 8; i++)
            q[i] |= (uint64_t)((w[j] >> i) & 1) << j;
    aes_ct_sbox(q);
    for (int j = 0; j < 4; j++) {
        unsigned v = 0;
        for (int i = 0; i < 8; i++)
            v |= (unsigned)((q[i] >> j) & 1) << i;
        w[j] = (Byte)v;
    }
}

/* FIPS 197 section 5.2, KeyExpansion, into b->enc. */
static void aes_expand_key(AesBlock *b, const Byte *key, int nk) {
    int words = 4 * (b->rounds + 1);
    Byte *w = b->enc;
    memcpy(w, key, (size_t)nk * 4);
    Byte rcon = 1;
    for (int i = nk; i < words; i++) {
        Byte t[4];
        memcpy(t, w + 4 * (i - 1), 4);
        if (i % nk == 0) {
            Byte t0 = t[0];
            t[0] = t[1];
            t[1] = t[2];
            t[2] = t[3];
            t[3] = t0;
            aes_ct_sub_word(t);
            t[0] ^= rcon;
            rcon = (Byte)((rcon << 1) ^ (0x1b & -(rcon >> 7)));
        } else if (nk > 6 && i % nk == 4) {
            aes_ct_sub_word(t);
        }
        for (int j = 0; j < 4; j++)
            w[4 * i + j] = (Byte)(w[4 * (i - nk) + j] ^ t[j]);
    }
}

/* The round keys in bit planes, each one copied into all four blocks. */
static void aes_ct_schedule(AesBlock *b) {
    for (int r = 0; r <= b->rounds; r++) {
        Byte four[64];
        for (int k = 0; k < 4; k++)
            memcpy(four + 16 * k, b->enc + 16 * r, 16);
        aes_ct_load(b->sk + 8 * r, four, 4);
    }
}

static void aes_ct_encrypt_blocks(const AesBlock *b, Byte *dst, const Byte *src,
                                  size_t n) {
    while (n > 0) {
        size_t k = n < 4 ? n : 4;
        uint64_t q[8];
        aes_ct_load(q, src, k);
        aes_ct_encrypt(b, q);
        aes_ct_store(dst, q, k);
        src += 16 * k;
        dst += 16 * k;
        n -= k;
    }
}

static void aes_ct_decrypt_blocks(const AesBlock *b, Byte *dst, const Byte *src,
                                  size_t n) {
    while (n > 0) {
        size_t k = n < 4 ? n : 4;
        uint64_t q[8];
        aes_ct_load(q, src, k);
        aes_ct_decrypt(b, q);
        aes_ct_store(dst, q, k);
        src += 16 * k;
        dst += 16 * k;
        n -= k;
    }
}

/* ----------------------------------------------------------------- AES-NI */

#if defined(CRYPTO_X86_AES)

CRYPTO_TARGET_X86_AES static inline __m128i aes_x86_load(const void *p) {
    __m128i v;
    memcpy(&v, p, sizeof v);
    return v;
}

CRYPTO_TARGET_X86_AES static inline void aes_x86_store(void *p, __m128i v) {
    memcpy(p, &v, sizeof v);
}

/* The equivalent inverse cipher's keys: the encryption keys backwards, with
 * InvMixColumns applied to all but the first and last. */
CRYPTO_TARGET_X86_AES static void aes_x86_dec_keys(AesBlock *b) {
    int nr = b->rounds;
    aes_x86_store(b->dec, aes_x86_load(b->enc + 16 * nr));
    for (int i = 1; i < nr; i++)
        aes_x86_store(b->dec + 16 * i,
                      _mm_aesimc_si128(aes_x86_load(b->enc + 16 * (nr - i))));
    aes_x86_store(b->dec + 16 * nr, aes_x86_load(b->enc));
}

/* Four blocks at a time, so that the four chains of aesenc overlap in the
 * pipeline, then the rest one by one. */
CRYPTO_TARGET_X86_AES static void aes_x86_encrypt_blocks(const AesBlock *b, Byte *dst,
                                                         const Byte *src, size_t n) {
    int nr = b->rounds;
    __m128i k[15];
    for (int i = 0; i <= nr; i++)
        k[i] = aes_x86_load(b->enc + 16 * i);
    for (; n >= 4; n -= 4, src += 64, dst += 64) {
        __m128i s0 = _mm_xor_si128(aes_x86_load(src), k[0]);
        __m128i s1 = _mm_xor_si128(aes_x86_load(src + 16), k[0]);
        __m128i s2 = _mm_xor_si128(aes_x86_load(src + 32), k[0]);
        __m128i s3 = _mm_xor_si128(aes_x86_load(src + 48), k[0]);
        for (int i = 1; i < nr; i++) {
            s0 = _mm_aesenc_si128(s0, k[i]);
            s1 = _mm_aesenc_si128(s1, k[i]);
            s2 = _mm_aesenc_si128(s2, k[i]);
            s3 = _mm_aesenc_si128(s3, k[i]);
        }
        aes_x86_store(dst, _mm_aesenclast_si128(s0, k[nr]));
        aes_x86_store(dst + 16, _mm_aesenclast_si128(s1, k[nr]));
        aes_x86_store(dst + 32, _mm_aesenclast_si128(s2, k[nr]));
        aes_x86_store(dst + 48, _mm_aesenclast_si128(s3, k[nr]));
    }
    for (; n > 0; n--, src += 16, dst += 16) {
        __m128i s = _mm_xor_si128(aes_x86_load(src), k[0]);
        for (int i = 1; i < nr; i++)
            s = _mm_aesenc_si128(s, k[i]);
        aes_x86_store(dst, _mm_aesenclast_si128(s, k[nr]));
    }
}

CRYPTO_TARGET_X86_AES static void aes_x86_decrypt_blocks(const AesBlock *b, Byte *dst,
                                                         const Byte *src, size_t n) {
    int nr = b->rounds;
    __m128i k[15];
    for (int i = 0; i <= nr; i++)
        k[i] = aes_x86_load(b->dec + 16 * i);
    for (; n >= 4; n -= 4, src += 64, dst += 64) {
        __m128i s0 = _mm_xor_si128(aes_x86_load(src), k[0]);
        __m128i s1 = _mm_xor_si128(aes_x86_load(src + 16), k[0]);
        __m128i s2 = _mm_xor_si128(aes_x86_load(src + 32), k[0]);
        __m128i s3 = _mm_xor_si128(aes_x86_load(src + 48), k[0]);
        for (int i = 1; i < nr; i++) {
            s0 = _mm_aesdec_si128(s0, k[i]);
            s1 = _mm_aesdec_si128(s1, k[i]);
            s2 = _mm_aesdec_si128(s2, k[i]);
            s3 = _mm_aesdec_si128(s3, k[i]);
        }
        aes_x86_store(dst, _mm_aesdeclast_si128(s0, k[nr]));
        aes_x86_store(dst + 16, _mm_aesdeclast_si128(s1, k[nr]));
        aes_x86_store(dst + 32, _mm_aesdeclast_si128(s2, k[nr]));
        aes_x86_store(dst + 48, _mm_aesdeclast_si128(s3, k[nr]));
    }
    for (; n > 0; n--, src += 16, dst += 16) {
        __m128i s = _mm_xor_si128(aes_x86_load(src), k[0]);
        for (int i = 1; i < nr; i++)
            s = _mm_aesdec_si128(s, k[i]);
        aes_x86_store(dst, _mm_aesdeclast_si128(s, k[nr]));
    }
}

#endif /* CRYPTO_X86_AES */

/* ------------------------------------------------------------ Armv8 AES */

#if defined(CRYPTO_ARM64_AES)

/* AESE adds the key before SubBytes and ShiftRows, where AES adds it after, so
 * the keys go in one place earlier and the last one is a plain XOR. */
CRYPTO_TARGET_ARM64_AES static void aes_arm64_dec_keys(AesBlock *b) {
    int nr = b->rounds;
    vst1q_u8(b->dec, vld1q_u8(b->enc + 16 * nr));
    for (int i = 1; i < nr; i++)
        vst1q_u8(b->dec + 16 * i, vaesimcq_u8(vld1q_u8(b->enc + 16 * (nr - i))));
    vst1q_u8(b->dec + 16 * nr, vld1q_u8(b->enc));
}

CRYPTO_TARGET_ARM64_AES static void
aes_arm64_encrypt_blocks(const AesBlock *b, Byte *dst, const Byte *src, size_t n) {
    int nr = b->rounds;
    uint8x16_t k[15];
    for (int i = 0; i <= nr; i++)
        k[i] = vld1q_u8(b->enc + 16 * i);
    for (; n >= 4; n -= 4, src += 64, dst += 64) {
        uint8x16_t s0 = vld1q_u8(src);
        uint8x16_t s1 = vld1q_u8(src + 16);
        uint8x16_t s2 = vld1q_u8(src + 32);
        uint8x16_t s3 = vld1q_u8(src + 48);
        for (int i = 0; i < nr - 1; i++) {
            s0 = vaesmcq_u8(vaeseq_u8(s0, k[i]));
            s1 = vaesmcq_u8(vaeseq_u8(s1, k[i]));
            s2 = vaesmcq_u8(vaeseq_u8(s2, k[i]));
            s3 = vaesmcq_u8(vaeseq_u8(s3, k[i]));
        }
        vst1q_u8(dst, veorq_u8(vaeseq_u8(s0, k[nr - 1]), k[nr]));
        vst1q_u8(dst + 16, veorq_u8(vaeseq_u8(s1, k[nr - 1]), k[nr]));
        vst1q_u8(dst + 32, veorq_u8(vaeseq_u8(s2, k[nr - 1]), k[nr]));
        vst1q_u8(dst + 48, veorq_u8(vaeseq_u8(s3, k[nr - 1]), k[nr]));
    }
    for (; n > 0; n--, src += 16, dst += 16) {
        uint8x16_t s = vld1q_u8(src);
        for (int i = 0; i < nr - 1; i++)
            s = vaesmcq_u8(vaeseq_u8(s, k[i]));
        vst1q_u8(dst, veorq_u8(vaeseq_u8(s, k[nr - 1]), k[nr]));
    }
}

CRYPTO_TARGET_ARM64_AES static void
aes_arm64_decrypt_blocks(const AesBlock *b, Byte *dst, const Byte *src, size_t n) {
    int nr = b->rounds;
    uint8x16_t k[15];
    for (int i = 0; i <= nr; i++)
        k[i] = vld1q_u8(b->dec + 16 * i);
    for (; n >= 4; n -= 4, src += 64, dst += 64) {
        uint8x16_t s0 = vld1q_u8(src);
        uint8x16_t s1 = vld1q_u8(src + 16);
        uint8x16_t s2 = vld1q_u8(src + 32);
        uint8x16_t s3 = vld1q_u8(src + 48);
        for (int i = 0; i < nr - 1; i++) {
            s0 = vaesimcq_u8(vaesdq_u8(s0, k[i]));
            s1 = vaesimcq_u8(vaesdq_u8(s1, k[i]));
            s2 = vaesimcq_u8(vaesdq_u8(s2, k[i]));
            s3 = vaesimcq_u8(vaesdq_u8(s3, k[i]));
        }
        vst1q_u8(dst, veorq_u8(vaesdq_u8(s0, k[nr - 1]), k[nr]));
        vst1q_u8(dst + 16, veorq_u8(vaesdq_u8(s1, k[nr - 1]), k[nr]));
        vst1q_u8(dst + 32, veorq_u8(vaesdq_u8(s2, k[nr - 1]), k[nr]));
        vst1q_u8(dst + 48, veorq_u8(vaesdq_u8(s3, k[nr - 1]), k[nr]));
    }
    for (; n > 0; n--, src += 16, dst += 16) {
        uint8x16_t s = vld1q_u8(src);
        for (int i = 0; i < nr - 1; i++)
            s = vaesimcq_u8(vaesdq_u8(s, k[i]));
        vst1q_u8(dst, veorq_u8(vaesdq_u8(s, k[nr - 1]), k[nr]));
    }
}

#endif /* CRYPTO_ARM64_AES */

/* --------------------------------------------------------------- dispatch */

static uint32_t aes_portable_only;

void burrow__aes_set_portable(bool on) {
    burrow__atomic_store_relaxed_u32(&aes_portable_only, on ? 1u : 0u);
}

const char *burrow__aes_hardware(bool *available) {
#if defined(CRYPTO_X86_AES)
    *available = (pal_cpu_features() & PAL_CPU_X86_AES) != 0;
    return "AES-NI";
#elif defined(CRYPTO_ARM64_AES)
    *available = (pal_cpu_features() & PAL_CPU_ARM64_AES) != 0;
    return "Armv8.0";
#else
    *available = false;
    return NULL;
#endif
}

static int aes_pick_impl(void) {
    if (burrow__atomic_load_relaxed_u32(&aes_portable_only) != 0)
        return AES_IMPL_CT;
#if defined(CRYPTO_X86_AES)
    if ((pal_cpu_features() & PAL_CPU_X86_AES) != 0)
        return AES_IMPL_X86;
#endif
#if defined(CRYPTO_ARM64_AES)
    if ((pal_cpu_features() & PAL_CPU_ARM64_AES) != 0)
        return AES_IMPL_ARM64;
#endif
    return AES_IMPL_CT;
}

void burrow__aes_encrypt_blocks(const AesBlock *b, Byte *dst, const Byte *src,
                                size_t n) {
#if defined(CRYPTO_X86_AES)
    if (b->impl == AES_IMPL_X86) {
        aes_x86_encrypt_blocks(b, dst, src, n);
        return;
    }
#endif
#if defined(CRYPTO_ARM64_AES)
    if (b->impl == AES_IMPL_ARM64) {
        aes_arm64_encrypt_blocks(b, dst, src, n);
        return;
    }
#endif
    aes_ct_encrypt_blocks(b, dst, src, n);
}

void burrow__aes_decrypt_blocks(const AesBlock *b, Byte *dst, const Byte *src,
                                size_t n) {
#if defined(CRYPTO_X86_AES)
    if (b->impl == AES_IMPL_X86) {
        aes_x86_decrypt_blocks(b, dst, src, n);
        return;
    }
#endif
#if defined(CRYPTO_ARM64_AES)
    if (b->impl == AES_IMPL_ARM64) {
        aes_arm64_decrypt_blocks(b, dst, src, n);
        return;
    }
#endif
    aes_ct_decrypt_blocks(b, dst, src, n);
}

/* ------------------------------------------------------------------ Block */

/* Go's alias.InexactOverlap on two runs of n bytes. */
static bool aes_inexact_overlap(const void *x, const void *y, Int n) {
    uintptr_t a = (uintptr_t)x;
    uintptr_t b = (uintptr_t)y;
    if (a == b)
        return false;
    return a <= b + (uintptr_t)(n - 1) && b <= a + (uintptr_t)(n - 1);
}

static void aes_check(Slice dst, Slice src) {
    if (src.len < AES_BLOCK_SIZE)
        panic_str(BURROW_S("crypto/aes: input not full block"));
    if (dst.len < AES_BLOCK_SIZE)
        panic_str(BURROW_S("crypto/aes: output not full block"));
    if (aes_inexact_overlap(dst.p, src.p, AES_BLOCK_SIZE))
        panic_str(BURROW_S("crypto/aes: invalid buffer overlap"));
}

static Int aes_block_size(void *self) {
    (void)self;
    return AES_BLOCK_SIZE;
}

static void aes_encrypt(void *self, Slice dst, Slice src) {
    aes_check(dst, src);
    burrow__aes_encrypt_blocks((const AesBlock *)self, (Byte *)dst.p,
                               (const Byte *)src.p, 1);
}

static void aes_decrypt(void *self, Slice dst, Slice src) {
    aes_check(dst, src);
    burrow__aes_decrypt_blocks((const AesBlock *)self, (Byte *)dst.p,
                               (const Byte *)src.p, 1);
}

static const CipherBlockVT aes_block_vt = {NULL, aes_block_size, aes_encrypt,
                                           aes_decrypt};

AesBlock *burrow__aes_block_of(CipherBlock b) {
    return b.vt == &aes_block_vt ? (AesBlock *)b.data : NULL;
}

CipherBlock aes_new_cipher(Alloc *a, Slice key, Error *err) {
    int nk;
    switch (key.len) {
    case 16:
    case 24:
    case 32:
        nk = (int)(key.len / 4);
        break;
    default:
        BURROW_OUT(err, aes_key_size_error_as_error(key.len, error_allocator()));
        return (CipherBlock){NULL, NULL};
    }
    AesBlock *b = BURROW_NEW(a, AesBlock);
    if (b == NULL) {
        BURROW_OUT(err, burrow_err_out_of_memory);
        return (CipherBlock){NULL, NULL};
    }
    b->rounds = nk + 6;
    b->impl = aes_pick_impl();
    aes_expand_key(b, (const Byte *)key.p, nk);
#if defined(CRYPTO_X86_AES)
    if (b->impl == AES_IMPL_X86)
        aes_x86_dec_keys(b);
#endif
#if defined(CRYPTO_ARM64_AES)
    if (b->impl == AES_IMPL_ARM64)
        aes_arm64_dec_keys(b);
#endif
    if (b->impl == AES_IMPL_CT)
        aes_ct_schedule(b);
    BURROW_OUT(err, BURROW_NO_ERROR);
    return (CipherBlock){&aes_block_vt, b};
}
