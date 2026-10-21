/* Derived from Go's src/crypto/internal/fips140/aes/gcm/cmac.go.
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
#include "burrow/crypto/subtle.h"
#include "burrow/slice.h"
#include "burrow/type.h"

#include "aes_internal.h"

#include <string.h>

static Slice cmac_bytes(Byte *p, Int n) {
    return slice_from(p, n, n, TYPE_BYTE);
}

/* shiftLeft sets x to x << 1, and returns MSB₁(x). */
static Byte cmac_shift_left(Byte x[AES_BLOCK_SIZE]) {
    Byte msb = 0;
    for (int i = AES_BLOCK_SIZE - 1; i >= 0; i--) {
        Byte next = (Byte)(x[i] >> 7);
        x[i] = (Byte)(x[i] << 1 | msb);
        msb = next;
    }
    return msb;
}

static void cmac_derive_subkeys(Cmac *c) {
    burrow__aes_encrypt_blocks(&c->b, c->k1, c->k1, 1);
    Byte msb = cmac_shift_left(c->k1);
    c->k1[AES_BLOCK_SIZE - 1] ^= (Byte)(msb * 0x87);

    memcpy(c->k2, c->k1, AES_BLOCK_SIZE);
    msb = cmac_shift_left(c->k2);
    c->k2[AES_BLOCK_SIZE - 1] ^= (Byte)(msb * 0x87);
}

void burrow__cmac_init(Cmac *c, const AesBlock *b) {
    memset(c, 0, sizeof *c);
    c->b = *b;
    cmac_derive_subkeys(c);
}

void burrow__cmac_mac(Cmac *c, Byte out[AES_BLOCK_SIZE], Slice m) {
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
