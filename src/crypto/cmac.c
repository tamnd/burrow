/* Derived from Go's src/crypto/internal/fips140/aes/gcm/cmac.go, ctrkdf.go and
 * cast.go.
 * Go source: go1.27.1.
 *
 * CMAC, from NIST SP 800-38B. Go keeps it inside the FIPS module, for its
 * counter mode KDF and its tests, and does not export it. Here it is internal
 * too, and what reaches it is tests/cmac_test.c.
 *
 * Copyright 2024 The Go Authors. All rights reserved.
 * Copyright 2026 The burrow Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style licence that can be found
 * in the LICENSE file. */

#include "burrow/crypto/cipher.h"

#include "burrow/core.h"
#include "burrow/crypto/aes.h"
#include "burrow/crypto/subtle.h"
#include "burrow/error.h"
#include "burrow/mem/arena.h"
#include "burrow/slice.h"
#include "burrow/type.h"

#include "aes_internal.h"
#include "fips140_module.h"

#include <string.h>

CRYPTO_FIPS_TEXT static Slice cmac_bytes(Byte *p, Int n) {
    return slice_from(p, n, n, TYPE_BYTE);
}

/* shiftLeft sets x to x << 1, and returns MSB₁(x). */
CRYPTO_FIPS_TEXT static Byte cmac_shift_left(Byte x[AES_BLOCK_SIZE]) {
    Byte msb = 0;
    for (int i = AES_BLOCK_SIZE - 1; i >= 0; i--) {
        Byte next = (Byte)(x[i] >> 7);
        x[i] = (Byte)(x[i] << 1 | msb);
        msb = next;
    }
    return msb;
}

CRYPTO_FIPS_TEXT static void cmac_derive_subkeys(Cmac *c) {
    burrow__aes_encrypt_blocks(&c->b, c->k1, c->k1, 1);
    Byte msb = cmac_shift_left(c->k1);
    c->k1[AES_BLOCK_SIZE - 1] ^= (Byte)(msb * 0x87);

    memcpy(c->k2, c->k1, AES_BLOCK_SIZE);
    msb = cmac_shift_left(c->k2);
    c->k2[AES_BLOCK_SIZE - 1] ^= (Byte)(msb * 0x87);
}

CRYPTO_FIPS_TEXT void burrow__cmac_init(Cmac *c, const AesBlock *b) {
    memset(c, 0, sizeof *c);
    c->b = *b;
    cmac_derive_subkeys(c);
}

CRYPTO_FIPS_TEXT void burrow__cmac_mac(Cmac *c, Byte out[AES_BLOCK_SIZE], Slice m) {
    Byte x[AES_BLOCK_SIZE] = {0};
    Slice xs = cmac_bytes(x, AES_BLOCK_SIZE);
    if (m.len == 0) {
        /* Special-cased as a single empty partial final block. */
        memcpy(x, c->k2, AES_BLOCK_SIZE);
        x[m.len] ^= 0x80;
        burrow__aes_encrypt_blocks(&c->b, x, x, 1);
        memcpy(out, x, AES_BLOCK_SIZE);
        return;
    }
    while (m.len >= AES_BLOCK_SIZE) {
        subtle_xor_bytes(xs, slice_sub(m, 0, AES_BLOCK_SIZE), xs);
        if (m.len == AES_BLOCK_SIZE) {
            /* Final complete block. */
            subtle_xor_bytes(xs, cmac_bytes(c->k1, AES_BLOCK_SIZE), xs);
        }
        burrow__aes_encrypt_blocks(&c->b, x, x, 1);
        m = slice_sub(m, AES_BLOCK_SIZE, m.len);
    }
    if (m.len > 0) {
        /* Final incomplete block. */
        subtle_xor_bytes(xs, m, xs);
        subtle_xor_bytes(xs, cmac_bytes(c->k2, AES_BLOCK_SIZE), xs);
        x[m.len] ^= 0x80;
        burrow__aes_encrypt_blocks(&c->b, x, x, 1);
    }
    memcpy(out, x, AES_BLOCK_SIZE);
}

/* ------------------------------------------------------------- CounterKDF */

/* From Go's src/crypto/internal/fips140/aes/gcm/ctrkdf.go. */

CRYPTO_FIPS_TEXT void burrow__counter_kdf_init(CounterKdf *kdf, const AesBlock *b) {
    burrow__cmac_init(&kdf->mac, b);
}

CRYPTO_FIPS_TEXT void burrow__counter_kdf_derive_key(CounterKdf *kdf, Byte out[32],
                                                     Byte label,
                                                     const Byte context[12]) {
    Byte input[AES_BLOCK_SIZE] = {0};
    input[2] = label;
    memcpy(input + 4, context, 12);

    input[1] = 0x01; /* i = 1 */
    burrow__cmac_mac(&kdf->mac, out, cmac_bytes(input, AES_BLOCK_SIZE));

    input[1] = 0x02; /* i = 2 */
    burrow__cmac_mac(&kdf->mac, out + AES_BLOCK_SIZE,
                     cmac_bytes(input, AES_BLOCK_SIZE));
}

/* ------------------------------------------------------------------- CAST */

/* From Go's src/crypto/internal/fips140/aes/gcm/cast.go. Counter KDF covers
 * CMAC per IG 10.3.B, and CMAC covers GCM per IG 10.3.A Resolution 1.d(i). AES
 * decryption is covered by the CBC CAST in package crypto/internal/fips140/aes.
 */

CRYPTO_FIPS_RODATA static const Byte cmac_cast_key[16] = {
    0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08,
    0x09, 0x0a, 0x0b, 0x0c, 0x0d, 0x0e, 0x0f, 0x10,
};

CRYPTO_FIPS_RODATA static const Byte cmac_cast_context[12] = {
    0x21, 0x22, 0x23, 0x24, 0x25, 0x26, 0x27, 0x28, 0x29, 0x2a, 0x2b, 0x2c,
};

CRYPTO_FIPS_RODATA static const Byte cmac_cast_want[32] = {
    0xe6, 0x86, 0x96, 0x97, 0x08, 0xfc, 0x90, 0x30, 0x36, 0x1c, 0x65,
    0x94, 0xb2, 0x62, 0xa5, 0xf7, 0xcb, 0x9d, 0x93, 0x94, 0xda, 0xf1,
    0x94, 0x09, 0x6a, 0x27, 0x5e, 0x85, 0x22, 0x5e, 0x7a, 0xee,
};

CRYPTO_FIPS_TEXT Error burrow__ctrkdf_cast(void *arg) {
    Arena ar;
    arena_init(&ar, NULL, 0);
    Error err = BURROW_NO_ERROR;
    Slice key = slice_from((void *)(uintptr_t)cmac_cast_key, 16, 16, TYPE_BYTE);
    CipherBlock b = aes_new_cipher(arena_allocator(&ar), key, &err);
    if (BURROW_FAILED(err)) {
        arena_free(&ar);
        return err;
    }
    CounterKdf kdf;
    burrow__counter_kdf_init(&kdf, burrow__aes_block_of(b));
    arena_free(&ar);
    Byte got[32];
    burrow__counter_kdf_derive_key(&kdf, got, 0xFF, cmac_cast_context);
    if (memcmp(got, cmac_cast_want, sizeof got) != 0)
        return errors_new(error_allocator(), BURROW_S("unexpected result"));
    return BURROW_NO_ERROR;
}
