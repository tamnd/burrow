/* Derived from Go's src/crypto/des/block.go, cipher.go and const.go.
 * Go source: go1.27.1.
 *
 * Copyright 2011 The Go Authors. All rights reserved.
 * Copyright 2026 The burrow Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style licence that can be found
 * in the LICENSE file. */

#include "burrow/crypto/des.h"

#include "burrow/core.h"
#include "burrow/crypto/cipher.h"
#include "burrow/error.h"
#include "burrow/mem.h"
#include "burrow/panic.h"
#include "burrow/slice.h"
#include "burrow/type.h"

#include "des_internal.h"

#include <stdint.h>
#include <string.h>

/* ---------------------------------------------------------- KeySizeError */

/* The length first, so the data pointer of the Error is a pointer to a
 * DesKeySizeError and errors_as hands it straight back. */
typedef struct DesKeySizeBox {
    DesKeySizeError k;
    Str message;
} DesKeySizeBox;

static const Type des_key_size_desc = {
    {(const Byte *)"KeySizeError", 12},
    {(const Byte *)"crypto/des", 10},
    KIND_INT,
    (uint32_t)sizeof(DesKeySizeError),
    (uint16_t)_Alignof(DesKeySizeError),
    0,
    0,
    NULL,
    NULL,
    NULL,
    NULL,
    0,
    0x64656b73U, /* "deks" */
    NULL,
};

const Type *const TYPE_DES_KEY_SIZE_ERROR = &des_key_size_desc;

static const char des_key_size_prefix[] = "crypto/des: invalid key size ";

#define DES_PREFIX_LEN ((Int)sizeof(des_key_size_prefix) - 1)

/* Writes the message for k to p, or only counts it when p is NULL, and returns
 * its length. */
static Int des_key_size_message(Byte *p, DesKeySizeError k) {
    Byte digits[20];
    Int n = 0;
    uint64_t u = k < 0 ? (uint64_t)0 - (uint64_t)k : (uint64_t)k;
    do {
        digits[n++] = (Byte)('0' + (int)(u % 10));
        u /= 10;
    } while (u != 0);
    Int len = DES_PREFIX_LEN + (k < 0) + n;
    if (p != NULL) {
        memcpy(p, des_key_size_prefix, (size_t)DES_PREFIX_LEN);
        p += DES_PREFIX_LEN;
        if (k < 0)
            *p++ = '-';
        while (n > 0)
            *p++ = digits[--n];
    }
    return len;
}

static Str des_key_size_text(const void *self) {
    return ((const DesKeySizeBox *)self)->message;
}

static Error des_key_size_clone(const void *self, Alloc *a) {
    return des_key_size_error_as_error(((const DesKeySizeBox *)self)->k, a);
}

static const ErrorVT des_key_size_vt = {
    .self_type = &des_key_size_desc,
    .message = des_key_size_text,
    .clone = des_key_size_clone,
};

Error des_key_size_error_as_error(DesKeySizeError k, Alloc *a) {
    Int mlen = des_key_size_message(NULL, k);
    DesKeySizeBox *b = (DesKeySizeBox *)mem_alloc_nozero(
        a, sizeof(DesKeySizeBox) + (size_t)mlen, _Alignof(DesKeySizeBox));
    if (b == NULL)
        return burrow_err_out_of_memory;
    Byte *p = (Byte *)(b + 1);
    des_key_size_message(p, k);
    b->k = k;
    b->message = str_from_bytes(p, mlen);
    return (Error){&des_key_size_vt, b};
}

Str des_key_size_error_error(DesKeySizeError k, Alloc *a) {
    Int mlen = des_key_size_message(NULL, k);
    Byte *p = (Byte *)mem_alloc_nozero(a, (size_t)mlen, 1);
    if (p == NULL)
        return BURROW_STR_EMPTY;
    des_key_size_message(p, k);
    return str_from_bytes(p, mlen);
}

/* ---------------------------------------------------------------- tables */

/* Used in the key schedule to select 56 bits from a 64-bit input. */
static const uint8_t des_permuted_choice1[56] = {
    7,  15, 23, 31, 39, 47, 55, 63, 6,  14, 22, 30, 38, 46, 54, 62, 5,  13, 21,
    29, 37, 45, 53, 61, 4,  12, 20, 28, 1,  9,  17, 25, 33, 41, 49, 57, 2,  10,
    18, 26, 34, 42, 50, 58, 3,  11, 19, 27, 35, 43, 51, 59, 36, 44, 52, 60,
};

/* Used in the key schedule to produce each subkey by selecting 48 bits from
 * the 56-bit input. */
static const uint8_t des_permuted_choice2[48] = {
    42, 39, 45, 32, 55, 51, 53, 28, 41, 50, 35, 46, 33, 37, 44, 52,
    30, 48, 40, 49, 29, 36, 43, 54, 15, 4,  25, 19, 9,  1,  26, 16,
    5,  11, 23, 8,  12, 7,  17, 0,  22, 3,  10, 14, 6,  20, 27, 24,
};

/* Size of left rotation per round in each half of the key schedule. */
static const uint8_t des_ks_rotations[16] = {1, 1, 2, 2, 2, 2, 2, 2,
                                             1, 2, 2, 2, 2, 2, 2, 1};

/* feistelBox[s][16*i+j] holds the output of the permutation function for
 * sBoxes[s][i][j] << 4*(7-s), rotated left by one bit, which Go factors out
 * of the rounds. Go works it out from the S-boxes the first time a key is set
 * up. Here it is the same table written out, and tests/des_test.c works it out
 * again from the S-boxes to check it. */
const uint32_t burrow__des_feistel_box[8][64] = {
    {
        0x01010400U, 0x00000000U, 0x00010000U, 0x01010404U, 0x01010004U, 0x00010404U,
        0x00000004U, 0x00010000U, 0x00000400U, 0x01010400U, 0x01010404U, 0x00000400U,
        0x01000404U, 0x01010004U, 0x01000000U, 0x00000004U, 0x00000404U, 0x01000400U,
        0x01000400U, 0x00010400U, 0x00010400U, 0x01010000U, 0x01010000U, 0x01000404U,
        0x00010004U, 0x01000004U, 0x01000004U, 0x00010004U, 0x00000000U, 0x00000404U,
        0x00010404U, 0x01000000U, 0x00010000U, 0x01010404U, 0x00000004U, 0x01010000U,
        0x01010400U, 0x01000000U, 0x01000000U, 0x00000400U, 0x01010004U, 0x00010000U,
        0x00010400U, 0x01000004U, 0x00000400U, 0x00000004U, 0x01000404U, 0x00010404U,
        0x01010404U, 0x00010004U, 0x01010000U, 0x01000404U, 0x01000004U, 0x00000404U,
        0x00010404U, 0x01010400U, 0x00000404U, 0x01000400U, 0x01000400U, 0x00000000U,
        0x00010004U, 0x00010400U, 0x00000000U, 0x01010004U,
    },
    {
        0x80108020U, 0x80008000U, 0x00008000U, 0x00108020U, 0x00100000U, 0x00000020U,
        0x80100020U, 0x80008020U, 0x80000020U, 0x80108020U, 0x80108000U, 0x80000000U,
        0x80008000U, 0x00100000U, 0x00000020U, 0x80100020U, 0x00108000U, 0x00100020U,
        0x80008020U, 0x00000000U, 0x80000000U, 0x00008000U, 0x00108020U, 0x80100000U,
        0x00100020U, 0x80000020U, 0x00000000U, 0x00108000U, 0x00008020U, 0x80108000U,
        0x80100000U, 0x00008020U, 0x00000000U, 0x00108020U, 0x80100020U, 0x00100000U,
        0x80008020U, 0x80100000U, 0x80108000U, 0x00008000U, 0x80100000U, 0x80008000U,
        0x00000020U, 0x80108020U, 0x00108020U, 0x00000020U, 0x00008000U, 0x80000000U,
        0x00008020U, 0x80108000U, 0x00100000U, 0x80000020U, 0x00100020U, 0x80008020U,
        0x80000020U, 0x00100020U, 0x00108000U, 0x00000000U, 0x80008000U, 0x00008020U,
        0x80000000U, 0x80100020U, 0x80108020U, 0x00108000U,
    },
    {
        0x00000208U, 0x08020200U, 0x00000000U, 0x08020008U, 0x08000200U, 0x00000000U,
        0x00020208U, 0x08000200U, 0x00020008U, 0x08000008U, 0x08000008U, 0x00020000U,
        0x08020208U, 0x00020008U, 0x08020000U, 0x00000208U, 0x08000000U, 0x00000008U,
        0x08020200U, 0x00000200U, 0x00020200U, 0x08020000U, 0x08020008U, 0x00020208U,
        0x08000208U, 0x00020200U, 0x00020000U, 0x08000208U, 0x00000008U, 0x08020208U,
        0x00000200U, 0x08000000U, 0x08020200U, 0x08000000U, 0x00020008U, 0x00000208U,
        0x00020000U, 0x08020200U, 0x08000200U, 0x00000000U, 0x00000200U, 0x00020008U,
        0x08020208U, 0x08000200U, 0x08000008U, 0x00000200U, 0x00000000U, 0x08020008U,
        0x08000208U, 0x00020000U, 0x08000000U, 0x08020208U, 0x00000008U, 0x00020208U,
        0x00020200U, 0x08000008U, 0x08020000U, 0x08000208U, 0x00000208U, 0x08020000U,
        0x00020208U, 0x00000008U, 0x08020008U, 0x00020200U,
    },
    {
        0x00802001U, 0x00002081U, 0x00002081U, 0x00000080U, 0x00802080U, 0x00800081U,
        0x00800001U, 0x00002001U, 0x00000000U, 0x00802000U, 0x00802000U, 0x00802081U,
        0x00000081U, 0x00000000U, 0x00800080U, 0x00800001U, 0x00000001U, 0x00002000U,
        0x00800000U, 0x00802001U, 0x00000080U, 0x00800000U, 0x00002001U, 0x00002080U,
        0x00800081U, 0x00000001U, 0x00002080U, 0x00800080U, 0x00002000U, 0x00802080U,
        0x00802081U, 0x00000081U, 0x00800080U, 0x00800001U, 0x00802000U, 0x00802081U,
        0x00000081U, 0x00000000U, 0x00000000U, 0x00802000U, 0x00002080U, 0x00800080U,
        0x00800081U, 0x00000001U, 0x00802001U, 0x00002081U, 0x00002081U, 0x00000080U,
        0x00802081U, 0x00000081U, 0x00000001U, 0x00002000U, 0x00800001U, 0x00002001U,
        0x00802080U, 0x00800081U, 0x00002001U, 0x00002080U, 0x00800000U, 0x00802001U,
        0x00000080U, 0x00800000U, 0x00002000U, 0x00802080U,
    },
    {
        0x00000100U, 0x02080100U, 0x02080000U, 0x42000100U, 0x00080000U, 0x00000100U,
        0x40000000U, 0x02080000U, 0x40080100U, 0x00080000U, 0x02000100U, 0x40080100U,
        0x42000100U, 0x42080000U, 0x00080100U, 0x40000000U, 0x02000000U, 0x40080000U,
        0x40080000U, 0x00000000U, 0x40000100U, 0x42080100U, 0x42080100U, 0x02000100U,
        0x42080000U, 0x40000100U, 0x00000000U, 0x42000000U, 0x02080100U, 0x02000000U,
        0x42000000U, 0x00080100U, 0x00080000U, 0x42000100U, 0x00000100U, 0x02000000U,
        0x40000000U, 0x02080000U, 0x42000100U, 0x40080100U, 0x02000100U, 0x40000000U,
        0x42080000U, 0x02080100U, 0x40080100U, 0x00000100U, 0x02000000U, 0x42080000U,
        0x42080100U, 0x00080100U, 0x42000000U, 0x42080100U, 0x02080000U, 0x00000000U,
        0x40080000U, 0x42000000U, 0x00080100U, 0x02000100U, 0x40000100U, 0x00080000U,
        0x00000000U, 0x40080000U, 0x02080100U, 0x40000100U,
    },
    {
        0x20000010U, 0x20400000U, 0x00004000U, 0x20404010U, 0x20400000U, 0x00000010U,
        0x20404010U, 0x00400000U, 0x20004000U, 0x00404010U, 0x00400000U, 0x20000010U,
        0x00400010U, 0x20004000U, 0x20000000U, 0x00004010U, 0x00000000U, 0x00400010U,
        0x20004010U, 0x00004000U, 0x00404000U, 0x20004010U, 0x00000010U, 0x20400010U,
        0x20400010U, 0x00000000U, 0x00404010U, 0x20404000U, 0x00004010U, 0x00404000U,
        0x20404000U, 0x20000000U, 0x20004000U, 0x00000010U, 0x20400010U, 0x00404000U,
        0x20404010U, 0x00400000U, 0x00004010U, 0x20000010U, 0x00400000U, 0x20004000U,
        0x20000000U, 0x00004010U, 0x20000010U, 0x20404010U, 0x00404000U, 0x20400000U,
        0x00404010U, 0x20404000U, 0x00000000U, 0x20400010U, 0x00000010U, 0x00004000U,
        0x20400000U, 0x00404010U, 0x00004000U, 0x00400010U, 0x20004010U, 0x00000000U,
        0x20404000U, 0x20000000U, 0x00400010U, 0x20004010U,
    },
    {
        0x00200000U, 0x04200002U, 0x04000802U, 0x00000000U, 0x00000800U, 0x04000802U,
        0x00200802U, 0x04200800U, 0x04200802U, 0x00200000U, 0x00000000U, 0x04000002U,
        0x00000002U, 0x04000000U, 0x04200002U, 0x00000802U, 0x04000800U, 0x00200802U,
        0x00200002U, 0x04000800U, 0x04000002U, 0x04200000U, 0x04200800U, 0x00200002U,
        0x04200000U, 0x00000800U, 0x00000802U, 0x04200802U, 0x00200800U, 0x00000002U,
        0x04000000U, 0x00200800U, 0x04000000U, 0x00200800U, 0x00200000U, 0x04000802U,
        0x04000802U, 0x04200002U, 0x04200002U, 0x00000002U, 0x00200002U, 0x04000000U,
        0x04000800U, 0x00200000U, 0x04200800U, 0x00000802U, 0x00200802U, 0x04200800U,
        0x00000802U, 0x04000002U, 0x04200802U, 0x04200000U, 0x00200800U, 0x00000000U,
        0x00000002U, 0x04200802U, 0x00000000U, 0x00200802U, 0x04200000U, 0x00000800U,
        0x04000002U, 0x04000800U, 0x00000800U, 0x00200002U,
    },
    {
        0x10001040U, 0x00001000U, 0x00040000U, 0x10041040U, 0x10000000U, 0x10001040U,
        0x00000040U, 0x10000000U, 0x00040040U, 0x10040000U, 0x10041040U, 0x00041000U,
        0x10041000U, 0x00041040U, 0x00001000U, 0x00000040U, 0x10040000U, 0x10000040U,
        0x10001000U, 0x00001040U, 0x00041000U, 0x00040040U, 0x10040040U, 0x10041000U,
        0x00001040U, 0x00000000U, 0x00000000U, 0x10040040U, 0x10000040U, 0x10001000U,
        0x00041040U, 0x00040000U, 0x00041040U, 0x00040000U, 0x10041000U, 0x00001000U,
        0x00000040U, 0x10040040U, 0x00001000U, 0x00041040U, 0x10001000U, 0x00000040U,
        0x10000040U, 0x10040000U, 0x10040040U, 0x10000000U, 0x00040000U, 0x10001040U,
        0x00000000U, 0x10041040U, 0x00040040U, 0x10000040U, 0x10040000U, 0x10001000U,
        0x10001040U, 0x00000000U, 0x10041040U, 0x00041000U, 0x00041000U, 0x00001040U,
        0x00001040U, 0x00040040U, 0x10000000U, 0x10041000U,
    },
};

/* ----------------------------------------------------------------- block */

static inline uint64_t des_be64(const Byte *p) {
    return (uint64_t)p[0] << 56 | (uint64_t)p[1] << 48 | (uint64_t)p[2] << 40 |
           (uint64_t)p[3] << 32 | (uint64_t)p[4] << 24 | (uint64_t)p[5] << 16 |
           (uint64_t)p[6] << 8 | (uint64_t)p[7];
}

static inline void des_put_be64(Byte *p, uint64_t v) {
    for (int i = 7; i >= 0; i--) {
        p[i] = (Byte)v;
        v >>= 8;
    }
}

#define DES_BOX burrow__des_feistel_box

/* DES Feistel function: two rounds, with the halves passed in and out. */
static inline void des_feistel(uint32_t *lp, uint32_t *rp, uint64_t k0, uint64_t k1) {
    uint32_t l = *lp, r = *rp, t;

    t = r ^ (uint32_t)(k0 >> 32);
    l ^= DES_BOX[7][t & 0x3f] ^ DES_BOX[5][(t >> 8) & 0x3f] ^
         DES_BOX[3][(t >> 16) & 0x3f] ^ DES_BOX[1][(t >> 24) & 0x3f];

    t = ((r << 28) | (r >> 4)) ^ (uint32_t)k0;
    l ^= DES_BOX[6][t & 0x3f] ^ DES_BOX[4][(t >> 8) & 0x3f] ^
         DES_BOX[2][(t >> 16) & 0x3f] ^ DES_BOX[0][(t >> 24) & 0x3f];

    t = l ^ (uint32_t)(k1 >> 32);
    r ^= DES_BOX[7][t & 0x3f] ^ DES_BOX[5][(t >> 8) & 0x3f] ^
         DES_BOX[3][(t >> 16) & 0x3f] ^ DES_BOX[1][(t >> 24) & 0x3f];

    t = ((l << 28) | (l >> 4)) ^ (uint32_t)k1;
    r ^= DES_BOX[6][t & 0x3f] ^ DES_BOX[4][(t >> 8) & 0x3f] ^
         DES_BOX[2][(t >> 16) & 0x3f] ^ DES_BOX[0][(t >> 24) & 0x3f];

    *lp = l;
    *rp = r;
}

/* General purpose function to perform DES block permutations. */
static uint64_t des_permute_block(uint64_t src, const uint8_t *permutation, int n) {
    uint64_t block = 0;
    for (int position = 0; position < n; position++) {
        uint64_t bit = (src >> permutation[position]) & 1;
        block |= bit << (unsigned)((n - 1) - position);
    }
    return block;
}

uint64_t burrow__des_permute_initial_block(uint64_t block) {
    /* block = b7 b6 b5 b4 b3 b2 b1 b0 (8 bytes) */
    uint64_t b1 = block >> 48;
    uint64_t b2 = block << 48;
    block ^= b1 ^ b2 ^ b1 << 48 ^ b2 >> 48;

    /* block = b1 b0 b5 b4 b3 b2 b7 b6 */
    b1 = block >> 32 & 0xff00ff;
    b2 = (block & 0xff00ff00);
    block ^= b1 << 32 ^ b2 ^ b1 << 8 ^ b2 << 24; /* exchange b0 b4 with b3 b7 */

    /* block is now b1 b3 b5 b7 b0 b2 b4 b6, the permutation:
     *                  ...  8
     *                  ... 24
     *                  ... 40
     *                  ... 56
     *  7  6  5  4  3  2  1  0
     * 23 22 21 20 19 18 17 16
     *                  ... 32
     *                  ... 48 */

    /* exchange 4,5,6,7 with 32,33,34,35 etc. */
    b1 = block & 0x0f0f00000f0f0000;
    b2 = block & 0x0000f0f00000f0f0;
    block ^= b1 ^ b2 ^ b1 >> 12 ^ b2 << 12;

    /* block is the permutation:
     *
     *   [+8]         [+40]
     *
     *  7  6  5  4
     * 23 22 21 20
     *  3  2  1  0
     * 19 18 17 16    [+32] */

    /* exchange 0,1,4,5 with 18,19,22,23 */
    b1 = block & 0x3300330033003300;
    b2 = block & 0x00cc00cc00cc00cc;
    block ^= b1 ^ b2 ^ b1 >> 6 ^ b2 << 6;

    /* block is the permutation:
     * 15 14
     * 13 12
     * 11 10
     *  9  8
     *  7  6
     *  5  4
     *  3  2
     *  1  0 [+16] [+32] [+64] */

    /* exchange 0,2,4,6 with 9,11,13,15: */
    b1 = block & 0xaaaaaaaa55555555;
    block ^= b1 ^ b1 >> 33 ^ b1 << 33;

    /* block is the permutation:
     * 6 14 22 30 38 46 54 62
     * 4 12 20 28 36 44 52 60
     * 2 10 18 26 34 42 50 58
     * 0  8 16 24 32 40 48 56
     * 7 15 23 31 39 47 55 63
     * 5 13 21 29 37 45 53 61
     * 3 11 19 27 35 43 51 59
     * 1  9 17 25 33 41 49 57 */
    return block;
}

uint64_t burrow__des_permute_final_block(uint64_t block) {
    /* Perform the same bit exchanges as permuteInitialBlock but in reverse
     * order. */
    uint64_t b1 = block & 0xaaaaaaaa55555555;
    block ^= b1 ^ b1 >> 33 ^ b1 << 33;

    b1 = block & 0x3300330033003300;
    uint64_t b2 = block & 0x00cc00cc00cc00cc;
    block ^= b1 ^ b2 ^ b1 >> 6 ^ b2 << 6;

    b1 = block & 0x0f0f00000f0f0000;
    b2 = block & 0x0000f0f00000f0f0;
    block ^= b1 ^ b2 ^ b1 >> 12 ^ b2 << 12;

    b1 = block >> 32 & 0xff00ff;
    b2 = (block & 0xff00ff00);
    block ^= b1 << 32 ^ b2 ^ b1 << 8 ^ b2 << 24;

    b1 = block >> 48;
    b2 = block << 48;
    block ^= b1 ^ b2 ^ b1 << 48 ^ b2 >> 48;
    return block;
}

/* The initial permutation and the one bit rotation of both halves that the
 * feistel box has factored in. */
static inline void des_begin(const Byte *src, uint32_t *left, uint32_t *right) {
    uint64_t b = burrow__des_permute_initial_block(des_be64(src));
    uint32_t l = (uint32_t)(b >> 32), r = (uint32_t)b;
    *left = (l << 1) | (l >> 31);
    *right = (r << 1) | (r >> 31);
}

/* Undoes the rotation, switches left and right and does the final
 * permutation. */
static inline void des_end(Byte *dst, uint32_t left, uint32_t right) {
    left = (left << 31) | (left >> 1);
    right = (right << 31) | (right >> 1);
    uint64_t pre_output = ((uint64_t)right << 32) | (uint64_t)left;
    des_put_be64(dst, burrow__des_permute_final_block(pre_output));
}

static void des_crypt_block(const uint64_t *subkeys, Byte *dst, const Byte *src,
                            bool decrypt) {
    uint32_t left, right;
    des_begin(src, &left, &right);
    if (decrypt) {
        for (Int i = 0; i < 8; i++)
            des_feistel(&left, &right, subkeys[15 - 2 * i], subkeys[15 - (2 * i + 1)]);
    } else {
        for (Int i = 0; i < 8; i++)
            des_feistel(&left, &right, subkeys[2 * i], subkeys[2 * i + 1]);
    }
    des_end(dst, left, right);
}

/* Creates 16 28-bit blocks rotated according to the rotation schedule. */
static void des_ks_rotate(uint32_t in, uint32_t out[16]) {
    uint32_t last = in;
    for (int i = 0; i < 16; i++) {
        /* 28-bit circular left shift */
        uint32_t left = (last << (4 + des_ks_rotations[i])) >> 4;
        uint32_t right = (last << 4) >> (32 - des_ks_rotations[i]);
        out[i] = left | right;
        last = out[i];
    }
}

/* Expand 48-bit input to 64-bit, with each 6-bit block padded by extra two bits
 * at the top. By doing so, we can have the input blocks (four bits each), and
 * the key blocks (six bits each) well-aligned without extra shifts/rotations
 * for alignments. */
static uint64_t des_unpack(uint64_t x) {
    return ((x >> (6 * 1)) & 0xff) << (8 * 0) | ((x >> (6 * 3)) & 0xff) << (8 * 1) |
           ((x >> (6 * 5)) & 0xff) << (8 * 2) | ((x >> (6 * 7)) & 0xff) << (8 * 3) |
           ((x >> (6 * 0)) & 0xff) << (8 * 4) | ((x >> (6 * 2)) & 0xff) << (8 * 5) |
           ((x >> (6 * 4)) & 0xff) << (8 * 6) | ((x >> (6 * 6)) & 0xff) << (8 * 7);
}

/* Creates 16 56-bit subkeys from the original key. */
static void des_generate_subkeys(uint64_t subkeys[16], const Byte *key_bytes) {
    /* apply PC1 permutation to key */
    uint64_t key = des_be64(key_bytes);
    uint64_t permuted_key = des_permute_block(key, des_permuted_choice1, 56);

    /* rotate halves of permuted key according to the rotation schedule */
    uint32_t left_rotations[16], right_rotations[16];
    des_ks_rotate((uint32_t)(permuted_key >> 28), left_rotations);
    des_ks_rotate((uint32_t)(permuted_key << 4) >> 4, right_rotations);

    /* generate subkeys */
    for (int i = 0; i < 16; i++) {
        /* combine halves to form 56-bit input to PC2 */
        uint64_t pc2_input =
            (uint64_t)left_rotations[i] << 28 | (uint64_t)right_rotations[i];
        /* apply PC2 permutation to 7 byte input */
        subkeys[i] = des_unpack(des_permute_block(pc2_input, des_permuted_choice2, 48));
    }
}

/* ---------------------------------------------------------------- Blocks */

typedef struct DesCipher {
    uint64_t subkeys[16];
} DesCipher;

typedef struct TripleDesCipher {
    DesCipher cipher1, cipher2, cipher3;
} TripleDesCipher;

/* Go's alias.InexactOverlap on two runs of n bytes. */
static bool des_inexact_overlap(const void *x, const void *y, Int n) {
    uintptr_t a = (uintptr_t)x;
    uintptr_t b = (uintptr_t)y;
    if (a == b)
        return false;
    return a <= b + (uintptr_t)(n - 1) && b <= a + (uintptr_t)(n - 1);
}

static void des_check(Slice dst, Slice src) {
    if (src.len < DES_BLOCK_SIZE)
        panic_str(BURROW_S("crypto/des: input not full block"));
    if (dst.len < DES_BLOCK_SIZE)
        panic_str(BURROW_S("crypto/des: output not full block"));
    if (des_inexact_overlap(dst.p, src.p, DES_BLOCK_SIZE))
        panic_str(BURROW_S("crypto/des: invalid buffer overlap"));
}

static Int des_block_size(void *self) {
    (void)self;
    return DES_BLOCK_SIZE;
}

static void des_encrypt(void *self, Slice dst, Slice src) {
    des_check(dst, src);
    des_crypt_block(((const DesCipher *)self)->subkeys, (Byte *)dst.p,
                    (const Byte *)src.p, false);
}

static void des_decrypt(void *self, Slice dst, Slice src) {
    des_check(dst, src);
    des_crypt_block(((const DesCipher *)self)->subkeys, (Byte *)dst.p,
                    (const Byte *)src.p, true);
}

static const CipherBlockVT des_block_vt = {NULL, des_block_size, des_encrypt,
                                           des_decrypt};

CipherBlock des_new_cipher(Alloc *a, Slice key, Error *err) {
    if (key.len != 8) {
        BURROW_OUT(err, des_key_size_error_as_error(key.len, error_allocator()));
        return (CipherBlock){NULL, NULL};
    }
    DesCipher *c = BURROW_NEW(a, DesCipher);
    if (c == NULL) {
        BURROW_OUT(err, burrow_err_out_of_memory);
        return (CipherBlock){NULL, NULL};
    }
    des_generate_subkeys(c->subkeys, (const Byte *)key.p);
    BURROW_OUT(err, BURROW_NO_ERROR);
    return (CipherBlock){&des_block_vt, c};
}

static void triple_des_encrypt(void *self, Slice dst, Slice src) {
    const TripleDesCipher *c = self;
    des_check(dst, src);

    uint32_t left, right;
    des_begin((const Byte *)src.p, &left, &right);
    for (Int i = 0; i < 8; i++)
        des_feistel(&left, &right, c->cipher1.subkeys[2 * i],
                    c->cipher1.subkeys[2 * i + 1]);
    for (Int i = 0; i < 8; i++)
        des_feistel(&right, &left, c->cipher2.subkeys[15 - 2 * i],
                    c->cipher2.subkeys[15 - (2 * i + 1)]);
    for (Int i = 0; i < 8; i++)
        des_feistel(&left, &right, c->cipher3.subkeys[2 * i],
                    c->cipher3.subkeys[2 * i + 1]);
    des_end((Byte *)dst.p, left, right);
}

static void triple_des_decrypt(void *self, Slice dst, Slice src) {
    const TripleDesCipher *c = self;
    des_check(dst, src);

    uint32_t left, right;
    des_begin((const Byte *)src.p, &left, &right);
    for (Int i = 0; i < 8; i++)
        des_feistel(&left, &right, c->cipher3.subkeys[15 - 2 * i],
                    c->cipher3.subkeys[15 - (2 * i + 1)]);
    for (Int i = 0; i < 8; i++)
        des_feistel(&right, &left, c->cipher2.subkeys[2 * i],
                    c->cipher2.subkeys[2 * i + 1]);
    for (int i = 0; i < 8; i++)
        des_feistel(&left, &right, c->cipher1.subkeys[15 - 2 * i],
                    c->cipher1.subkeys[15 - (2 * i + 1)]);
    des_end((Byte *)dst.p, left, right);
}

static const CipherBlockVT triple_des_block_vt = {
    NULL, des_block_size, triple_des_encrypt, triple_des_decrypt};

CipherBlock des_new_triple_des_cipher(Alloc *a, Slice key, Error *err) {
    if (key.len != 24) {
        BURROW_OUT(err, des_key_size_error_as_error(key.len, error_allocator()));
        return (CipherBlock){NULL, NULL};
    }
    TripleDesCipher *c = BURROW_NEW(a, TripleDesCipher);
    if (c == NULL) {
        BURROW_OUT(err, burrow_err_out_of_memory);
        return (CipherBlock){NULL, NULL};
    }
    const Byte *k = (const Byte *)key.p;
    des_generate_subkeys(c->cipher1.subkeys, k);
    des_generate_subkeys(c->cipher2.subkeys, k + 8);
    des_generate_subkeys(c->cipher3.subkeys, k + 16);
    BURROW_OUT(err, BURROW_NO_ERROR);
    return (CipherBlock){&triple_des_block_vt, c};
}
