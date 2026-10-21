/* Derived from Go's src/crypto/internal/fips140/drbg/ctrdrbg.go and cast.go.
 * Go source: go1.27.1.
 *
 * Only the CTR_DRBG itself and its self-test are here so far. Nothing reads
 * from it yet: Go's drbg.Read, which uses it in FIPS 140-3 mode, comes with
 * that mode.
 *
 * Copyright 2024 The Go Authors. All rights reserved.
 * Copyright 2026 The burrow Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style licence that can be found
 * in the LICENSE file. */

/* First, for the amalgamation, which puts a file in the package of the first
 * burrow header it includes. The DRBG is built on AES, as in Go. */
#include "burrow/crypto/aes.h"

#include "burrow/core.h"
#include "burrow/error.h"
#include "burrow/panic.h"
#include "burrow/slice.h"
#include "burrow/type.h"

#include "aes_internal.h"
#include "drbg_internal.h"
#include "fips140_module.h"

#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#define DRBG_KEY_SIZE (256 / 8)
#define DRBG_RESEED_INTERVAL ((uint64_t)1 << 48)
#define DRBG_MAX_REQUEST_SIZE ((1 << 19) / 8)

CRYPTO_FIPS_TEXT static void drbg_increment(Byte v[AES_BLOCK_SIZE]) {
    unsigned carry = 1;
    for (int i = AES_BLOCK_SIZE - 1; i >= 0; i--) {
        unsigned x = v[i] + carry;
        v[i] = (Byte)x;
        carry = x >> 8;
    }
}

/* Not in Go: aes.CTR's XORKeyStream followed by RoundToBlock, which is all the
 * DRBG asks of it. A partial last block uses up the whole block. */
CRYPTO_FIPS_TEXT static void drbg_xor_key_stream(DrbgCounter *c, Byte *dst,
                                                 const Byte *src, size_t n) {
    Byte ks[AES_BLOCK_SIZE];
    while (n > 0) {
        burrow__aes_encrypt_blocks(&c->b, ks, c->v, 1);
        drbg_increment(c->v);
        size_t m = n < AES_BLOCK_SIZE ? n : AES_BLOCK_SIZE;
        for (size_t i = 0; i < m; i++)
            dst[i] = (Byte)(src[i] ^ ks[i]);
        dst += m;
        src += m;
        n -= m;
    }
    memset(ks, 0, sizeof ks);
}

/* Not in Go: aes.NewCTR(aes.New(key), iv), into c. */
CRYPTO_FIPS_TEXT static void drbg_set_ctr(DrbgCounter *c, const Byte key[DRBG_KEY_SIZE],
                                          const Byte iv[AES_BLOCK_SIZE]) {
    burrow__aes_block_init(&c->b, key, DRBG_KEY_SIZE);
    memcpy(c->v, iv, AES_BLOCK_SIZE);
}

CRYPTO_FIPS_TEXT static void drbg_update(DrbgCounter *c,
                                         const Byte seed[DRBG_SEED_SIZE]) {
    /* CTR_DRBG_Update, per Section 10.2.1.2. */

    Byte temp[DRBG_SEED_SIZE];
    drbg_xor_key_stream(c, temp, seed, DRBG_SEED_SIZE);
    Byte *k = temp;
    Byte *v = temp + DRBG_KEY_SIZE;

    /* Again, we pre-increment V, like in NewCounter. */
    drbg_increment(v);

    drbg_set_ctr(c, k, v);
    memset(temp, 0, sizeof temp);
}

CRYPTO_FIPS_TEXT void burrow__drbg_new_counter(DrbgCounter *c,
                                               const Byte entropy[DRBG_SEED_SIZE]) {
    /* CTR_DRBG_Instantiate_algorithm, per Section 10.2.1.3.1. */

    Byte k[DRBG_KEY_SIZE] = {0};
    Byte v[AES_BLOCK_SIZE] = {0};

    /* V starts at 0, but is incremented in CTR_DRBG_Update before each use,
     * unlike AES-CTR where it is incremented after each use. */
    v[AES_BLOCK_SIZE - 1] = 1;

    memset(c, 0, sizeof *c);
    drbg_set_ctr(c, k, v);
    drbg_update(c, entropy);
    c->reseed_counter = 1;
}

CRYPTO_FIPS_TEXT void
burrow__drbg_counter_reseed(DrbgCounter *c, const Byte entropy[DRBG_SEED_SIZE],
                            const Byte additional_input[DRBG_SEED_SIZE]) {
    /* CTR_DRBG_Reseed_algorithm, per Section 10.2.1.4.1. */

    Byte seed[DRBG_SEED_SIZE];
    for (size_t i = 0; i < DRBG_SEED_SIZE; i++)
        seed[i] = (Byte)(entropy[i] ^ additional_input[i]);
    drbg_update(c, seed);
    c->reseed_counter = 1;
    memset(seed, 0, sizeof seed);
}

CRYPTO_FIPS_TEXT bool burrow__drbg_counter_generate(DrbgCounter *c, Slice out,
                                                    const Byte *additional_input) {
    /* CTR_DRBG_Generate_algorithm, per Section 10.2.1.5.1. */

    if (out.len > DRBG_MAX_REQUEST_SIZE)
        panic_str(
            BURROW_S("crypto/drbg: internal error: request size exceeds maximum"));

    /* Step 1. */
    if (c->reseed_counter > DRBG_RESEED_INTERVAL)
        return true;

    /* Step 2. */
    Byte zero[DRBG_SEED_SIZE] = {0};
    if (additional_input != NULL) {
        drbg_update(c, additional_input);
    } else {
        /* If the additional input is null, the first CTR_DRBG_Update is
         * skipped, but the additional input is replaced with an all-zero
         * string for the second CTR_DRBG_Update. */
        additional_input = zero;
    }

    /* Steps 3-5. */
    Byte *o = out.p;
    if (out.len > 0) {
        memset(o, 0, (size_t)out.len);
        drbg_xor_key_stream(c, o, o, (size_t)out.len);
    }

    /* Step 6. */
    drbg_update(c, additional_input);

    /* Step 7. */
    c->reseed_counter++;

    /* Step 8. */
    return false;
}

/* ------------------------------------------------------------------- CAST */

/* From Go's src/crypto/internal/fips140/drbg/cast.go. Per IG 10.3.A,
 * Resolution 7: "A KAT of a DRBG may be performed by: Instantiate with known
 * data, Reseed with other known data, Generate and then compare the result to a
 * pre-computed value." */

CRYPTO_FIPS_RODATA static const Byte drbg_cast_entropy[DRBG_SEED_SIZE] = {
    0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08, 0x09, 0x0a, 0x0b, 0x0c,
    0x0d, 0x0e, 0x0f, 0x10, 0x11, 0x12, 0x13, 0x14, 0x15, 0x16, 0x17, 0x18,
    0x19, 0x1a, 0x1b, 0x1c, 0x1d, 0x1e, 0x1f, 0x20, 0x21, 0x22, 0x23, 0x24,
    0x25, 0x26, 0x27, 0x28, 0x29, 0x2a, 0x2b, 0x2c, 0x2d, 0x2e, 0x2f, 0x30,
};

CRYPTO_FIPS_RODATA static const Byte drbg_cast_reseed_entropy[DRBG_SEED_SIZE] = {
    0x31, 0x32, 0x33, 0x34, 0x35, 0x36, 0x37, 0x38, 0x39, 0x3a, 0x3b, 0x3c,
    0x3d, 0x3e, 0x3f, 0x40, 0x41, 0x42, 0x43, 0x44, 0x45, 0x46, 0x47, 0x48,
    0x49, 0x4a, 0x4b, 0x4c, 0x4d, 0x4e, 0x4f, 0x50, 0x51, 0x52, 0x53, 0x54,
    0x55, 0x56, 0x57, 0x58, 0x59, 0x5a, 0x5b, 0x5c, 0x5d, 0x5e, 0x5f, 0x60,
};

CRYPTO_FIPS_RODATA static const Byte drbg_cast_additional_input[DRBG_SEED_SIZE] = {
    0x61, 0x62, 0x63, 0x64, 0x65, 0x66, 0x67, 0x68, 0x69, 0x6a, 0x6b, 0x6c,
    0x6d, 0x6e, 0x6f, 0x70, 0x71, 0x72, 0x73, 0x74, 0x75, 0x76, 0x77, 0x78,
    0x79, 0x7a, 0x7b, 0x7c, 0x7d, 0x7e, 0x7f, 0x80, 0x81, 0x82, 0x83, 0x84,
    0x85, 0x86, 0x87, 0x88, 0x89, 0x8a, 0x8b, 0x8c, 0x8d, 0x8e, 0x8f, 0x90,
};

CRYPTO_FIPS_RODATA static const Byte drbg_cast_want[32] = {
    0x6e, 0x6e, 0x47, 0x9d, 0x24, 0xf8, 0x6a, 0x3b, 0x77, 0x87, 0xa8,
    0xf8, 0x18, 0x6d, 0x98, 0x5a, 0x53, 0xbe, 0xbe, 0xed, 0xde, 0xab,
    0x92, 0x28, 0xf0, 0xf4, 0xac, 0x6e, 0x10, 0xbf, 0x01, 0x93,
};

CRYPTO_FIPS_TEXT Error burrow__drbg_cast(void *arg) {
    DrbgCounter c;
    burrow__drbg_new_counter(&c, drbg_cast_entropy);
    burrow__drbg_counter_reseed(&c, drbg_cast_reseed_entropy,
                                drbg_cast_additional_input);
    Byte got[sizeof drbg_cast_want];
    Int n = (Int)sizeof got;
    (void)burrow__drbg_counter_generate(&c, slice_from(got, n, n, TYPE_BYTE),
                                        drbg_cast_additional_input);
    if (memcmp(got, drbg_cast_want, sizeof got) != 0)
        return errors_new(error_allocator(), BURROW_S("unexpected result"));
    return BURROW_NO_ERROR;
}
