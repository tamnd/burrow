/* Derived from Go's src/crypto/sha3/sha3.go and
 * src/crypto/internal/fips140/sha3/sha3.go, shake.go, hashes.go and
 * keccakf.go.
 * Go source: go1.27.1.
 *
 * Go keeps the state as 200 bytes and reads them as 25 lanes on a little
 * endian machine. This keeps the 25 lanes and moves bytes in and out of them
 * in little endian order, which is the same state on every machine without a
 * byte swap in the permutation.
 *
 * Copyright 2014 The Go Authors. All rights reserved.
 * Copyright 2026 The burrow Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style licence that can be found
 * in the LICENSE file. */

#include "burrow/crypto/sha3.h"

#include "burrow/core.h"
#include "burrow/error.h"
#include "burrow/hash.h"
#include "burrow/mem.h"
#include "burrow/panic.h"
#include "burrow/slice.h"
#include "burrow/type.h"

#include <string.h>

/* The domain separation byte of each function, with the first bit of the
 * padding. */
enum {
    SHA3_DS_SHA3 = 0x06,
    SHA3_DS_SHAKE = 0x1f,
    SHA3_DS_CSHAKE = 0x04,
    SHA3_DS_KECCAK = 0x01,
};

/* The rates, (1600 - 2 * the security strength) / 8. */
enum {
    SHA3_RATE_K256 = (1600 - 256) / 8,
    SHA3_RATE_K448 = (1600 - 448) / 8,
    SHA3_RATE_K512 = (1600 - 512) / 8,
    SHA3_RATE_K768 = (1600 - 768) / 8,
    SHA3_RATE_K1024 = (1600 - 1024) / 8,
};

/* The magic, the rate, the state, n and the direction. */
enum { SHA3_MARSHALED_SIZE = 4 + 1 + 200 + 1 + 1 };

static const Str sha3_invalid_state_text = {(const Byte *)"sha3: invalid hash state",
                                            24};
static const Error sha3_err_invalid_state = {&burrow_sentinel_error_vt,
                                             &sha3_invalid_state_text};
static const Str sha3_invalid_identifier_text = {
    (const Byte *)"sha3: invalid hash state identifier", 35};
static const Error sha3_err_invalid_identifier = {&burrow_sentinel_error_vt,
                                                  &sha3_invalid_identifier_text};
static const Str sha3_invalid_function_text = {
    (const Byte *)"sha3: invalid hash state function", 33};
static const Error sha3_err_invalid_function = {&burrow_sentinel_error_vt,
                                                &sha3_invalid_function_text};

/* ------------------------------------------------------------ the permutation */

static const uint64_t sha3_rc[24] = {
    0x0000000000000001U, 0x0000000000008082U, 0x800000000000808AU, 0x8000000080008000U,
    0x000000000000808BU, 0x0000000080000001U, 0x8000000080008081U, 0x8000000000008009U,
    0x000000000000008AU, 0x0000000000000088U, 0x0000000080008009U, 0x000000008000000AU,
    0x000000008000808BU, 0x800000000000008BU, 0x8000000000008089U, 0x8000000000008003U,
    0x8000000000008002U, 0x8000000000000080U, 0x000000000000800AU, 0x800000008000000AU,
    0x8000000080008081U, 0x8000000000008080U, 0x0000000080000001U, 0x8000000080008008U,
};

static inline uint64_t sha3_rotl(uint64_t x, int k) {
    return (x << k) | (x >> (64 - k));
}

/* Keccak-f[1600], Go's keccakF1600Generic, which is the in place version from
 * the Keccak reference code. */
static void sha3_keccakf(uint64_t a[25]) {
    uint64_t t, bc0, bc1, bc2, bc3, bc4, d0, d1, d2, d3, d4;
    const uint64_t *rc = sha3_rc;

    for (int i = 0; i < 24; i += 4) {
        /* Combines the 5 steps in each round into 2 steps. */
        /* Unrolls 4 rounds per loop and spreads some steps across rounds. */

        /* Round 1 */
        bc0 = a[0] ^ a[5] ^ a[10] ^ a[15] ^ a[20];
        bc1 = a[1] ^ a[6] ^ a[11] ^ a[16] ^ a[21];
        bc2 = a[2] ^ a[7] ^ a[12] ^ a[17] ^ a[22];
        bc3 = a[3] ^ a[8] ^ a[13] ^ a[18] ^ a[23];
        bc4 = a[4] ^ a[9] ^ a[14] ^ a[19] ^ a[24];
        d0 = bc4 ^ sha3_rotl(bc1, 1);
        d1 = bc0 ^ sha3_rotl(bc2, 1);
        d2 = bc1 ^ sha3_rotl(bc3, 1);
        d3 = bc2 ^ sha3_rotl(bc4, 1);
        d4 = bc3 ^ sha3_rotl(bc0, 1);

        bc0 = a[0] ^ d0;
        t = a[6] ^ d1;
        bc1 = sha3_rotl(t, 44);
        t = a[12] ^ d2;
        bc2 = sha3_rotl(t, 43);
        t = a[18] ^ d3;
        bc3 = sha3_rotl(t, 21);
        t = a[24] ^ d4;
        bc4 = sha3_rotl(t, 14);
        a[0] = bc0 ^ (bc2 & ~bc1) ^ rc[i];
        a[6] = bc1 ^ (bc3 & ~bc2);
        a[12] = bc2 ^ (bc4 & ~bc3);
        a[18] = bc3 ^ (bc0 & ~bc4);
        a[24] = bc4 ^ (bc1 & ~bc0);

        t = a[10] ^ d0;
        bc2 = sha3_rotl(t, 3);
        t = a[16] ^ d1;
        bc3 = sha3_rotl(t, 45);
        t = a[22] ^ d2;
        bc4 = sha3_rotl(t, 61);
        t = a[3] ^ d3;
        bc0 = sha3_rotl(t, 28);
        t = a[9] ^ d4;
        bc1 = sha3_rotl(t, 20);
        a[10] = bc0 ^ (bc2 & ~bc1);
        a[16] = bc1 ^ (bc3 & ~bc2);
        a[22] = bc2 ^ (bc4 & ~bc3);
        a[3] = bc3 ^ (bc0 & ~bc4);
        a[9] = bc4 ^ (bc1 & ~bc0);

        t = a[20] ^ d0;
        bc4 = sha3_rotl(t, 18);
        t = a[1] ^ d1;
        bc0 = sha3_rotl(t, 1);
        t = a[7] ^ d2;
        bc1 = sha3_rotl(t, 6);
        t = a[13] ^ d3;
        bc2 = sha3_rotl(t, 25);
        t = a[19] ^ d4;
        bc3 = sha3_rotl(t, 8);
        a[20] = bc0 ^ (bc2 & ~bc1);
        a[1] = bc1 ^ (bc3 & ~bc2);
        a[7] = bc2 ^ (bc4 & ~bc3);
        a[13] = bc3 ^ (bc0 & ~bc4);
        a[19] = bc4 ^ (bc1 & ~bc0);

        t = a[5] ^ d0;
        bc1 = sha3_rotl(t, 36);
        t = a[11] ^ d1;
        bc2 = sha3_rotl(t, 10);
        t = a[17] ^ d2;
        bc3 = sha3_rotl(t, 15);
        t = a[23] ^ d3;
        bc4 = sha3_rotl(t, 56);
        t = a[4] ^ d4;
        bc0 = sha3_rotl(t, 27);
        a[5] = bc0 ^ (bc2 & ~bc1);
        a[11] = bc1 ^ (bc3 & ~bc2);
        a[17] = bc2 ^ (bc4 & ~bc3);
        a[23] = bc3 ^ (bc0 & ~bc4);
        a[4] = bc4 ^ (bc1 & ~bc0);

        t = a[15] ^ d0;
        bc3 = sha3_rotl(t, 41);
        t = a[21] ^ d1;
        bc4 = sha3_rotl(t, 2);
        t = a[2] ^ d2;
        bc0 = sha3_rotl(t, 62);
        t = a[8] ^ d3;
        bc1 = sha3_rotl(t, 55);
        t = a[14] ^ d4;
        bc2 = sha3_rotl(t, 39);
        a[15] = bc0 ^ (bc2 & ~bc1);
        a[21] = bc1 ^ (bc3 & ~bc2);
        a[2] = bc2 ^ (bc4 & ~bc3);
        a[8] = bc3 ^ (bc0 & ~bc4);
        a[14] = bc4 ^ (bc1 & ~bc0);

        /* Round 2 */
        bc0 = a[0] ^ a[5] ^ a[10] ^ a[15] ^ a[20];
        bc1 = a[1] ^ a[6] ^ a[11] ^ a[16] ^ a[21];
        bc2 = a[2] ^ a[7] ^ a[12] ^ a[17] ^ a[22];
        bc3 = a[3] ^ a[8] ^ a[13] ^ a[18] ^ a[23];
        bc4 = a[4] ^ a[9] ^ a[14] ^ a[19] ^ a[24];
        d0 = bc4 ^ sha3_rotl(bc1, 1);
        d1 = bc0 ^ sha3_rotl(bc2, 1);
        d2 = bc1 ^ sha3_rotl(bc3, 1);
        d3 = bc2 ^ sha3_rotl(bc4, 1);
        d4 = bc3 ^ sha3_rotl(bc0, 1);

        bc0 = a[0] ^ d0;
        t = a[16] ^ d1;
        bc1 = sha3_rotl(t, 44);
        t = a[7] ^ d2;
        bc2 = sha3_rotl(t, 43);
        t = a[23] ^ d3;
        bc3 = sha3_rotl(t, 21);
        t = a[14] ^ d4;
        bc4 = sha3_rotl(t, 14);
        a[0] = bc0 ^ (bc2 & ~bc1) ^ rc[i + 1];
        a[16] = bc1 ^ (bc3 & ~bc2);
        a[7] = bc2 ^ (bc4 & ~bc3);
        a[23] = bc3 ^ (bc0 & ~bc4);
        a[14] = bc4 ^ (bc1 & ~bc0);

        t = a[20] ^ d0;
        bc2 = sha3_rotl(t, 3);
        t = a[11] ^ d1;
        bc3 = sha3_rotl(t, 45);
        t = a[2] ^ d2;
        bc4 = sha3_rotl(t, 61);
        t = a[18] ^ d3;
        bc0 = sha3_rotl(t, 28);
        t = a[9] ^ d4;
        bc1 = sha3_rotl(t, 20);
        a[20] = bc0 ^ (bc2 & ~bc1);
        a[11] = bc1 ^ (bc3 & ~bc2);
        a[2] = bc2 ^ (bc4 & ~bc3);
        a[18] = bc3 ^ (bc0 & ~bc4);
        a[9] = bc4 ^ (bc1 & ~bc0);

        t = a[15] ^ d0;
        bc4 = sha3_rotl(t, 18);
        t = a[6] ^ d1;
        bc0 = sha3_rotl(t, 1);
        t = a[22] ^ d2;
        bc1 = sha3_rotl(t, 6);
        t = a[13] ^ d3;
        bc2 = sha3_rotl(t, 25);
        t = a[4] ^ d4;
        bc3 = sha3_rotl(t, 8);
        a[15] = bc0 ^ (bc2 & ~bc1);
        a[6] = bc1 ^ (bc3 & ~bc2);
        a[22] = bc2 ^ (bc4 & ~bc3);
        a[13] = bc3 ^ (bc0 & ~bc4);
        a[4] = bc4 ^ (bc1 & ~bc0);

        t = a[10] ^ d0;
        bc1 = sha3_rotl(t, 36);
        t = a[1] ^ d1;
        bc2 = sha3_rotl(t, 10);
        t = a[17] ^ d2;
        bc3 = sha3_rotl(t, 15);
        t = a[8] ^ d3;
        bc4 = sha3_rotl(t, 56);
        t = a[24] ^ d4;
        bc0 = sha3_rotl(t, 27);
        a[10] = bc0 ^ (bc2 & ~bc1);
        a[1] = bc1 ^ (bc3 & ~bc2);
        a[17] = bc2 ^ (bc4 & ~bc3);
        a[8] = bc3 ^ (bc0 & ~bc4);
        a[24] = bc4 ^ (bc1 & ~bc0);

        t = a[5] ^ d0;
        bc3 = sha3_rotl(t, 41);
        t = a[21] ^ d1;
        bc4 = sha3_rotl(t, 2);
        t = a[12] ^ d2;
        bc0 = sha3_rotl(t, 62);
        t = a[3] ^ d3;
        bc1 = sha3_rotl(t, 55);
        t = a[19] ^ d4;
        bc2 = sha3_rotl(t, 39);
        a[5] = bc0 ^ (bc2 & ~bc1);
        a[21] = bc1 ^ (bc3 & ~bc2);
        a[12] = bc2 ^ (bc4 & ~bc3);
        a[3] = bc3 ^ (bc0 & ~bc4);
        a[19] = bc4 ^ (bc1 & ~bc0);

        /* Round 3 */
        bc0 = a[0] ^ a[5] ^ a[10] ^ a[15] ^ a[20];
        bc1 = a[1] ^ a[6] ^ a[11] ^ a[16] ^ a[21];
        bc2 = a[2] ^ a[7] ^ a[12] ^ a[17] ^ a[22];
        bc3 = a[3] ^ a[8] ^ a[13] ^ a[18] ^ a[23];
        bc4 = a[4] ^ a[9] ^ a[14] ^ a[19] ^ a[24];
        d0 = bc4 ^ sha3_rotl(bc1, 1);
        d1 = bc0 ^ sha3_rotl(bc2, 1);
        d2 = bc1 ^ sha3_rotl(bc3, 1);
        d3 = bc2 ^ sha3_rotl(bc4, 1);
        d4 = bc3 ^ sha3_rotl(bc0, 1);

        bc0 = a[0] ^ d0;
        t = a[11] ^ d1;
        bc1 = sha3_rotl(t, 44);
        t = a[22] ^ d2;
        bc2 = sha3_rotl(t, 43);
        t = a[8] ^ d3;
        bc3 = sha3_rotl(t, 21);
        t = a[19] ^ d4;
        bc4 = sha3_rotl(t, 14);
        a[0] = bc0 ^ (bc2 & ~bc1) ^ rc[i + 2];
        a[11] = bc1 ^ (bc3 & ~bc2);
        a[22] = bc2 ^ (bc4 & ~bc3);
        a[8] = bc3 ^ (bc0 & ~bc4);
        a[19] = bc4 ^ (bc1 & ~bc0);

        t = a[15] ^ d0;
        bc2 = sha3_rotl(t, 3);
        t = a[1] ^ d1;
        bc3 = sha3_rotl(t, 45);
        t = a[12] ^ d2;
        bc4 = sha3_rotl(t, 61);
        t = a[23] ^ d3;
        bc0 = sha3_rotl(t, 28);
        t = a[9] ^ d4;
        bc1 = sha3_rotl(t, 20);
        a[15] = bc0 ^ (bc2 & ~bc1);
        a[1] = bc1 ^ (bc3 & ~bc2);
        a[12] = bc2 ^ (bc4 & ~bc3);
        a[23] = bc3 ^ (bc0 & ~bc4);
        a[9] = bc4 ^ (bc1 & ~bc0);

        t = a[5] ^ d0;
        bc4 = sha3_rotl(t, 18);
        t = a[16] ^ d1;
        bc0 = sha3_rotl(t, 1);
        t = a[2] ^ d2;
        bc1 = sha3_rotl(t, 6);
        t = a[13] ^ d3;
        bc2 = sha3_rotl(t, 25);
        t = a[24] ^ d4;
        bc3 = sha3_rotl(t, 8);
        a[5] = bc0 ^ (bc2 & ~bc1);
        a[16] = bc1 ^ (bc3 & ~bc2);
        a[2] = bc2 ^ (bc4 & ~bc3);
        a[13] = bc3 ^ (bc0 & ~bc4);
        a[24] = bc4 ^ (bc1 & ~bc0);

        t = a[20] ^ d0;
        bc1 = sha3_rotl(t, 36);
        t = a[6] ^ d1;
        bc2 = sha3_rotl(t, 10);
        t = a[17] ^ d2;
        bc3 = sha3_rotl(t, 15);
        t = a[3] ^ d3;
        bc4 = sha3_rotl(t, 56);
        t = a[14] ^ d4;
        bc0 = sha3_rotl(t, 27);
        a[20] = bc0 ^ (bc2 & ~bc1);
        a[6] = bc1 ^ (bc3 & ~bc2);
        a[17] = bc2 ^ (bc4 & ~bc3);
        a[3] = bc3 ^ (bc0 & ~bc4);
        a[14] = bc4 ^ (bc1 & ~bc0);

        t = a[10] ^ d0;
        bc3 = sha3_rotl(t, 41);
        t = a[21] ^ d1;
        bc4 = sha3_rotl(t, 2);
        t = a[7] ^ d2;
        bc0 = sha3_rotl(t, 62);
        t = a[18] ^ d3;
        bc1 = sha3_rotl(t, 55);
        t = a[4] ^ d4;
        bc2 = sha3_rotl(t, 39);
        a[10] = bc0 ^ (bc2 & ~bc1);
        a[21] = bc1 ^ (bc3 & ~bc2);
        a[7] = bc2 ^ (bc4 & ~bc3);
        a[18] = bc3 ^ (bc0 & ~bc4);
        a[4] = bc4 ^ (bc1 & ~bc0);

        /* Round 4 */
        bc0 = a[0] ^ a[5] ^ a[10] ^ a[15] ^ a[20];
        bc1 = a[1] ^ a[6] ^ a[11] ^ a[16] ^ a[21];
        bc2 = a[2] ^ a[7] ^ a[12] ^ a[17] ^ a[22];
        bc3 = a[3] ^ a[8] ^ a[13] ^ a[18] ^ a[23];
        bc4 = a[4] ^ a[9] ^ a[14] ^ a[19] ^ a[24];
        d0 = bc4 ^ sha3_rotl(bc1, 1);
        d1 = bc0 ^ sha3_rotl(bc2, 1);
        d2 = bc1 ^ sha3_rotl(bc3, 1);
        d3 = bc2 ^ sha3_rotl(bc4, 1);
        d4 = bc3 ^ sha3_rotl(bc0, 1);

        bc0 = a[0] ^ d0;
        t = a[1] ^ d1;
        bc1 = sha3_rotl(t, 44);
        t = a[2] ^ d2;
        bc2 = sha3_rotl(t, 43);
        t = a[3] ^ d3;
        bc3 = sha3_rotl(t, 21);
        t = a[4] ^ d4;
        bc4 = sha3_rotl(t, 14);
        a[0] = bc0 ^ (bc2 & ~bc1) ^ rc[i + 3];
        a[1] = bc1 ^ (bc3 & ~bc2);
        a[2] = bc2 ^ (bc4 & ~bc3);
        a[3] = bc3 ^ (bc0 & ~bc4);
        a[4] = bc4 ^ (bc1 & ~bc0);

        t = a[5] ^ d0;
        bc2 = sha3_rotl(t, 3);
        t = a[6] ^ d1;
        bc3 = sha3_rotl(t, 45);
        t = a[7] ^ d2;
        bc4 = sha3_rotl(t, 61);
        t = a[8] ^ d3;
        bc0 = sha3_rotl(t, 28);
        t = a[9] ^ d4;
        bc1 = sha3_rotl(t, 20);
        a[5] = bc0 ^ (bc2 & ~bc1);
        a[6] = bc1 ^ (bc3 & ~bc2);
        a[7] = bc2 ^ (bc4 & ~bc3);
        a[8] = bc3 ^ (bc0 & ~bc4);
        a[9] = bc4 ^ (bc1 & ~bc0);

        t = a[10] ^ d0;
        bc4 = sha3_rotl(t, 18);
        t = a[11] ^ d1;
        bc0 = sha3_rotl(t, 1);
        t = a[12] ^ d2;
        bc1 = sha3_rotl(t, 6);
        t = a[13] ^ d3;
        bc2 = sha3_rotl(t, 25);
        t = a[14] ^ d4;
        bc3 = sha3_rotl(t, 8);
        a[10] = bc0 ^ (bc2 & ~bc1);
        a[11] = bc1 ^ (bc3 & ~bc2);
        a[12] = bc2 ^ (bc4 & ~bc3);
        a[13] = bc3 ^ (bc0 & ~bc4);
        a[14] = bc4 ^ (bc1 & ~bc0);

        t = a[15] ^ d0;
        bc1 = sha3_rotl(t, 36);
        t = a[16] ^ d1;
        bc2 = sha3_rotl(t, 10);
        t = a[17] ^ d2;
        bc3 = sha3_rotl(t, 15);
        t = a[18] ^ d3;
        bc4 = sha3_rotl(t, 56);
        t = a[19] ^ d4;
        bc0 = sha3_rotl(t, 27);
        a[15] = bc0 ^ (bc2 & ~bc1);
        a[16] = bc1 ^ (bc3 & ~bc2);
        a[17] = bc2 ^ (bc4 & ~bc3);
        a[18] = bc3 ^ (bc0 & ~bc4);
        a[19] = bc4 ^ (bc1 & ~bc0);

        t = a[20] ^ d0;
        bc3 = sha3_rotl(t, 41);
        t = a[21] ^ d1;
        bc4 = sha3_rotl(t, 2);
        t = a[22] ^ d2;
        bc0 = sha3_rotl(t, 62);
        t = a[23] ^ d3;
        bc1 = sha3_rotl(t, 55);
        t = a[24] ^ d4;
        bc2 = sha3_rotl(t, 39);
        a[20] = bc0 ^ (bc2 & ~bc1);
        a[21] = bc1 ^ (bc3 & ~bc2);
        a[22] = bc2 ^ (bc4 & ~bc3);
        a[23] = bc3 ^ (bc0 & ~bc4);
        a[24] = bc4 ^ (bc1 & ~bc0);
    }
}

static inline uint64_t sha3_le64(const Byte *p) {
    return (uint64_t)p[0] | (uint64_t)p[1] << 8 | (uint64_t)p[2] << 16 |
           (uint64_t)p[3] << 24 | (uint64_t)p[4] << 32 | (uint64_t)p[5] << 40 |
           (uint64_t)p[6] << 48 | (uint64_t)p[7] << 56;
}

static inline void sha3_put_le64(Byte *b, uint64_t v) {
    for (int i = 0; i < 8; i++)
        b[i] = (Byte)(v >> (8 * i));
}

/* -------------------------------------------------------------- the sponge */

static void sha3_setup(Sha3 *d, Int rate, Int output_len, Byte dsbyte) {
    memset(d, 0, sizeof *d);
    d->rate = rate;
    d->output_len = output_len;
    d->dsbyte = dsbyte;
}

/* The zero value is SHA3-256. */
static inline void sha3_init(Sha3 *d) {
    if (d->output_len == 0)
        sha3_setup(d, SHA3_RATE_K512, 32, SHA3_DS_SHA3);
}

static void sha3_permute(Sha3 *d) {
    sha3_keccakf(d->a);
    d->n = 0;
}

static void sha3_pad_and_permute(Sha3 *d) {
    d->a[d->n >> 3] ^= (uint64_t)d->dsbyte << (8 * (d->n & 7));
    d->a[(d->rate - 1) >> 3] ^= (uint64_t)0x80 << (8 * ((d->rate - 1) & 7));
    sha3_permute(d);
    d->squeezing = true;
}

static void sha3_absorb(Sha3 *d, const Byte *p, Int len) {
    if (d->squeezing)
        panic_str(BURROW_S("sha3: Write after Read"));
    Int rate = d->rate;
    while (len > 0) {
        if (d->n == 0 && len >= rate) {
            /* A whole block, a lane at a time. Every rate is a whole number
             * of lanes. */
            for (Int i = 0; i < rate / 8; i++)
                d->a[i] ^= sha3_le64(p + 8 * i);
            sha3_keccakf(d->a);
            p += rate;
            len -= rate;
            continue;
        }
        Int k = rate - d->n < len ? rate - d->n : len;
        len -= k;
        /* Bytes up to a lane boundary, whole lanes, then the bytes left. */
        for (; k > 0 && (d->n & 7) != 0; k--, d->n++)
            d->a[d->n >> 3] ^= (uint64_t)*p++ << (8 * (d->n & 7));
        for (; k >= 8; k -= 8, d->n += 8, p += 8)
            d->a[d->n >> 3] ^= sha3_le64(p);
        for (; k > 0; k--, d->n++)
            d->a[d->n >> 3] ^= (uint64_t)*p++ << (8 * (d->n & 7));
        if (d->n == rate)
            sha3_permute(d);
    }
}

static void sha3_squeeze(Sha3 *d, Byte *out, Int len) {
    if (!d->squeezing)
        sha3_pad_and_permute(d);
    while (len > 0) {
        if (d->n == d->rate)
            sha3_permute(d);
        if ((d->n & 7) == 0 && len >= 8 && d->rate - d->n >= 8) {
            sha3_put_le64(out, d->a[d->n >> 3]);
            out += 8;
            len -= 8;
            d->n += 8;
            continue;
        }
        *out++ = (Byte)(d->a[d->n >> 3] >> (8 * (d->n & 7)));
        len--;
        d->n++;
    }
}

static const Byte *sha3_magic(Byte dsbyte) {
    switch (dsbyte) {
    case SHA3_DS_SHA3:
        return (const Byte *)"sha\x08";
    case SHA3_DS_SHAKE:
        return (const Byte *)"sha\x09";
    case SHA3_DS_CSHAKE:
        return (const Byte *)"sha\x0a";
    case SHA3_DS_KECCAK:
        return (const Byte *)"sha\x0b";
    default:
        panic_str(BURROW_S("unknown dsbyte"));
    }
}

static Slice sha3_digest_append_binary(Sha3 *d, Alloc *a, Slice b) {
    Byte buf[SHA3_MARSHALED_SIZE];
    memcpy(buf, sha3_magic(d->dsbyte), 4);
    buf[4] = (Byte)d->rate;
    for (Int i = 0; i < 25; i++)
        sha3_put_le64(buf + 5 + 8 * i, d->a[i]);
    buf[205] = (Byte)d->n;
    buf[206] = (Byte)d->squeezing;
    if (b.elem == NULL)
        b = slice_nil(TYPE_BYTE);
    return slice_append(a, b, buf, SHA3_MARSHALED_SIZE);
}

static Error sha3_digest_unmarshal_binary(Sha3 *d, Slice data) {
    if (data.len != SHA3_MARSHALED_SIZE || data.p == NULL)
        return sha3_err_invalid_state;
    const Byte *b = (const Byte *)data.p;
    if (memcmp(b, sha3_magic(d->dsbyte), 4) != 0)
        return sha3_err_invalid_identifier;
    if ((Int)b[4] != d->rate)
        return sha3_err_invalid_function;
    /* Go copies the state in before it checks n and the direction, so a
     * state that fails those checks still changes the lanes. */
    for (Int i = 0; i < 25; i++)
        d->a[i] = sha3_le64(b + 5 + 8 * i);
    Int n = b[205];
    Byte state = b[206];
    if (n > d->rate)
        return sha3_err_invalid_state;
    d->n = n;
    if (state > 1)
        return sha3_err_invalid_state;
    d->squeezing = state == 1;
    return BURROW_NO_ERROR;
}

/* ------------------------------------------------------------------- SHA-3 */

static Sha3 *sha3_make(Alloc *a, Int rate, Int output_len) {
    Sha3 *d = BURROW_NEW(a, Sha3);
    if (d != NULL)
        sha3_setup(d, rate, output_len, SHA3_DS_SHA3);
    return d;
}

Sha3 *sha3_new224(Alloc *a) {
    return sha3_make(a, SHA3_RATE_K448, 28);
}

Sha3 *sha3_new256(Alloc *a) {
    return sha3_make(a, SHA3_RATE_K512, 32);
}

Sha3 *sha3_new384(Alloc *a) {
    return sha3_make(a, SHA3_RATE_K768, 48);
}

Sha3 *sha3_new512(Alloc *a) {
    return sha3_make(a, SHA3_RATE_K1024, 64);
}

Int sha3_write(Sha3 *d, Slice p, Error *err) {
    sha3_init(d);
    sha3_absorb(d, (const Byte *)p.p, p.len);
    BURROW_OUT(err, BURROW_NO_ERROR);
    return p.len;
}

/* The digest into out, which has room for 64 bytes, from a copy of d. */
static void sha3_digest_sum(const Sha3 *d, Byte *out) {
    if (d->squeezing)
        panic_str(BURROW_S("sha3: Sum after Read"));
    Sha3 dup = *d;
    sha3_squeeze(&dup, out, dup.output_len);
}

Slice sha3_sum(Sha3 *d, Alloc *a, Slice b) {
    sha3_init(d);
    Byte h[64];
    sha3_digest_sum(d, h);
    if (b.elem == NULL)
        b = slice_nil(TYPE_BYTE);
    return slice_append(a, b, h, d->output_len);
}

void sha3_reset(Sha3 *d) {
    sha3_init(d);
    memset(d->a, 0, sizeof d->a);
    d->squeezing = false;
    d->n = 0;
}

Int sha3_size(Sha3 *d) {
    sha3_init(d);
    return d->output_len;
}

Int sha3_block_size(Sha3 *d) {
    sha3_init(d);
    return d->rate;
}

Slice sha3_marshal_binary(Sha3 *d, Alloc *a, Error *err) {
    return sha3_append_binary(d, a, slice_make(a, TYPE_BYTE, 0, SHA3_MARSHALED_SIZE),
                              err);
}

Slice sha3_append_binary(Sha3 *d, Alloc *a, Slice b, Error *err) {
    sha3_init(d);
    Int len = b.len;
    Slice r = sha3_digest_append_binary(d, a, b);
    BURROW_OUT(err, r.len == len + SHA3_MARSHALED_SIZE ? BURROW_NO_ERROR
                                                       : burrow_err_out_of_memory);
    return r;
}

Error sha3_unmarshal_binary(Sha3 *d, Slice b) {
    sha3_init(d);
    return sha3_digest_unmarshal_binary(d, b);
}

static Int sha3_vt_write(void *self, Slice p, Error *err) {
    return sha3_write(self, p, err);
}

static Slice sha3_vt_sum(void *self, Alloc *a, Slice b) {
    return sha3_sum(self, a, b);
}

static void sha3_vt_reset(void *self) {
    sha3_reset(self);
}

static Int sha3_vt_size(void *self) {
    return sha3_size(self);
}

static Int sha3_vt_block_size(void *self) {
    return sha3_block_size(self);
}

static HashCloner sha3_vt_clone(void *self, Alloc *a, Error *err) {
    return sha3_clone(self, a, err);
}

static const HashClonerVT sha3_vt = {
    {{NULL, sha3_vt_write},
     sha3_vt_sum,
     sha3_vt_reset,
     sha3_vt_size,
     sha3_vt_block_size},
    sha3_vt_clone,
};

HashCloner sha3_clone(Sha3 *d, Alloc *a, Error *err) {
    Sha3 *r = BURROW_NEW(a, Sha3);
    if (r == NULL) {
        BURROW_OUT(err, burrow_err_out_of_memory);
        return (HashCloner){NULL, NULL};
    }
    *r = *d;
    BURROW_OUT(err, BURROW_NO_ERROR);
    return sha3_as_cloner(r);
}

HashCloner sha3_as_cloner(Sha3 *d) {
    HashCloner h = {&sha3_vt, d};
    return h;
}

Hash sha3_as_hash(Sha3 *d) {
    Hash h = {&sha3_vt.hash, d};
    return h;
}

/* The one shot sums, on a Sha3 on the stack. */
static void sha3_sum_into(Int rate, Int output_len, Slice data, Byte *out) {
    Sha3 d;
    sha3_setup(&d, rate, output_len, SHA3_DS_SHA3);
    sha3_absorb(&d, (const Byte *)data.p, data.len);
    sha3_squeeze(&d, out, output_len);
}

Sha3Sum224Ret sha3_sum224(Slice data) {
    Sha3Sum224Ret r;
    sha3_sum_into(SHA3_RATE_K448, 28, data, r.a);
    return r;
}

Sha3Sum256Ret sha3_sum256(Slice data) {
    Sha3Sum256Ret r;
    sha3_sum_into(SHA3_RATE_K512, 32, data, r.a);
    return r;
}

Sha3Sum384Ret sha3_sum384(Slice data) {
    Sha3Sum384Ret r;
    sha3_sum_into(SHA3_RATE_K768, 48, data, r.a);
    return r;
}

Sha3Sum512Ret sha3_sum512(Slice data) {
    Sha3Sum512Ret r;
    sha3_sum_into(SHA3_RATE_K1024, 64, data, r.a);
    return r;
}

/* ------------------------------------------------------------ SHAKE, cSHAKE */

static void sha3_shake_setup(Sha3SHAKE *s, Int rate, Int output_len) {
    sha3_setup(&s->d, rate, output_len, SHA3_DS_SHAKE);
    s->init_block = slice_nil(TYPE_BYTE);
}

/* The zero value is SHAKE256. */
static inline void sha3_shake_init(Sha3SHAKE *s) {
    if (s->d.output_len == 0)
        sha3_shake_setup(s, SHA3_RATE_K512, 64);
}

/* left_encode from SP 800-185: x in as few big endian bytes as it takes, at
 * least one, after a byte that says how many. */
static Int sha3_left_encode(uint64_t x, Byte out[9]) {
    Int n = 1;
    while (n < 8 && (x >> (8 * n)) != 0)
        n++;
    out[0] = (Byte)n;
    for (Int i = 0; i < n; i++)
        out[1 + i] = (Byte)(x >> (8 * (n - 1 - i)));
    return n + 1;
}

/* bytepad(data, rate), absorbed: the encoded rate, the data, and zeros to the
 * end of the block. */
static void sha3_bytepad_write(Sha3SHAKE *s, Slice data, Int rate) {
    Byte enc[9];
    Int n = sha3_left_encode((uint64_t)rate, enc);
    sha3_absorb(&s->d, enc, n);
    sha3_absorb(&s->d, (const Byte *)data.p, data.len);
    Int padlen = rate - (n + data.len) % rate;
    if (padlen < rate) {
        static const Byte zeros[SHA3_RATE_K256] = {0};
        sha3_absorb(&s->d, zeros, padlen);
    }
}

static Sha3SHAKE *sha3_shake_make(Alloc *a, Int rate, Int output_len) {
    Sha3SHAKE *s = BURROW_NEW(a, Sha3SHAKE);
    if (s != NULL)
        sha3_shake_setup(s, rate, output_len);
    return s;
}

Sha3SHAKE *sha3_new_shake128(Alloc *a) {
    return sha3_shake_make(a, SHA3_RATE_K256, 32);
}

Sha3SHAKE *sha3_new_shake256(Alloc *a) {
    return sha3_shake_make(a, SHA3_RATE_K512, 64);
}

static Sha3SHAKE *sha3_new_cshake(Alloc *a, Slice n, Slice s, Int rate,
                                  Int output_len) {
    Sha3SHAKE *c = sha3_shake_make(a, rate, output_len);
    if (c == NULL || (n.len == 0 && s.len == 0))
        return c;
    c->d.dsbyte = SHA3_DS_CSHAKE;
    Byte enc[9];
    Slice b = slice_make(a, TYPE_BYTE, 0, 9 + n.len + 9 + s.len);
    if (b.p == NULL) {
        mem_free(a, c, sizeof *c, _Alignof(Sha3SHAKE));
        return NULL;
    }
    b = slice_append(a, b, enc, sha3_left_encode((uint64_t)n.len * 8, enc));
    b = slice_append(a, b, n.p, n.len);
    b = slice_append(a, b, enc, sha3_left_encode((uint64_t)s.len * 8, enc));
    b = slice_append(a, b, s.p, s.len);
    c->init_block = b;
    sha3_bytepad_write(c, b, rate);
    return c;
}

Sha3SHAKE *sha3_new_cshake128(Alloc *a, Slice n, Slice s) {
    return sha3_new_cshake(a, n, s, SHA3_RATE_K256, 32);
}

Sha3SHAKE *sha3_new_cshake256(Alloc *a, Slice n, Slice s) {
    return sha3_new_cshake(a, n, s, SHA3_RATE_K512, 64);
}

Int sha3_shake_write(Sha3SHAKE *s, Slice p, Error *err) {
    sha3_shake_init(s);
    sha3_absorb(&s->d, (const Byte *)p.p, p.len);
    BURROW_OUT(err, BURROW_NO_ERROR);
    return p.len;
}

Int sha3_shake_read(Sha3SHAKE *s, Slice p, Error *err) {
    sha3_shake_init(s);
    sha3_squeeze(&s->d, (Byte *)p.p, p.len);
    BURROW_OUT(err, BURROW_NO_ERROR);
    return p.len;
}

void sha3_shake_reset(Sha3SHAKE *s) {
    sha3_shake_init(s);
    memset(s->d.a, 0, sizeof s->d.a);
    s->d.squeezing = false;
    s->d.n = 0;
    if (s->init_block.len != 0)
        sha3_bytepad_write(s, s->init_block, s->d.rate);
}

Int sha3_shake_block_size(Sha3SHAKE *s) {
    sha3_shake_init(s);
    return s->d.rate;
}

Slice sha3_shake_marshal_binary(Sha3SHAKE *s, Alloc *a, Error *err) {
    sha3_shake_init(s);
    Slice b = slice_make(a, TYPE_BYTE, 0, SHA3_MARSHALED_SIZE + s->init_block.len);
    return sha3_shake_append_binary(s, a, b, err);
}

Slice sha3_shake_append_binary(Sha3SHAKE *s, Alloc *a, Slice b, Error *err) {
    sha3_shake_init(s);
    Int want = b.len + SHA3_MARSHALED_SIZE + s->init_block.len;
    b = sha3_digest_append_binary(&s->d, a, b);
    b = slice_append(a, b, s->init_block.p, s->init_block.len);
    BURROW_OUT(err, b.len == want ? BURROW_NO_ERROR : burrow_err_out_of_memory);
    return b;
}

Error sha3_shake_unmarshal_binary(Sha3SHAKE *s, Alloc *a, Slice b) {
    sha3_shake_init(s);
    if (b.len < SHA3_MARSHALED_SIZE)
        return sha3_err_invalid_state;
    Error err =
        sha3_digest_unmarshal_binary(&s->d, slice_sub(b, 0, SHA3_MARSHALED_SIZE));
    if (err.vt != NULL)
        return err;
    Slice rest = slice_sub(b, SHA3_MARSHALED_SIZE, b.len);
    Slice init = slice_nil(TYPE_BYTE);
    if (rest.len > 0) {
        init = slice_append(a, init, rest.p, rest.len);
        if (init.len != rest.len)
            return burrow_err_out_of_memory;
    }
    s->init_block = init;
    return BURROW_NO_ERROR;
}

static Int sha3_shake_vt_write(void *self, Slice p, Error *err) {
    return sha3_shake_write(self, p, err);
}

static Int sha3_shake_vt_read(void *self, Slice p, Error *err) {
    return sha3_shake_read(self, p, err);
}

static void sha3_shake_vt_reset(void *self) {
    sha3_shake_reset(self);
}

static Int sha3_shake_vt_block_size(void *self) {
    return sha3_shake_block_size(self);
}

static const HashXOFVT sha3_shake_vt = {
    {NULL, sha3_shake_vt_write},
    {NULL, sha3_shake_vt_read},
    sha3_shake_vt_reset,
    sha3_shake_vt_block_size,
};

HashXOF sha3_shake_as_xof(Sha3SHAKE *s) {
    HashXOF h = {&sha3_shake_vt, s};
    return h;
}

static Slice sha3_sum_shake(Alloc *a, Slice data, Int length, Int rate,
                            Int output_len) {
    Slice out = slice_make(a, TYPE_BYTE, length, length);
    if (out.p == NULL && length > 0)
        return slice_nil(TYPE_BYTE);
    Sha3 d;
    sha3_setup(&d, rate, output_len, SHA3_DS_SHAKE);
    sha3_absorb(&d, (const Byte *)data.p, data.len);
    sha3_squeeze(&d, (Byte *)out.p, length);
    return out;
}

Slice sha3_sum_shake128(Alloc *a, Slice data, Int length) {
    return sha3_sum_shake(a, data, length, SHA3_RATE_K256, 32);
}

Slice sha3_sum_shake256(Alloc *a, Slice data, Int length) {
    return sha3_sum_shake(a, data, length, SHA3_RATE_K512, 64);
}
