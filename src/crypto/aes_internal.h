/* What crypto/cipher needs from inside crypto/aes: the expanded key, and ways
 * to run it over many blocks at once, which is where the speed of CTR, CBC
 * decryption and GCM comes from. Go's crypto/cipher reaches into
 * crypto/internal/fips140/aes for the same reason.
 *
 * Copyright 2026 The burrow Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style licence that can be found
 * in the LICENSE file. */

#ifndef BURROW_CRYPTO_AES_INTERNAL_H
#define BURROW_CRYPTO_AES_INTERNAL_H

#include "burrow/crypto/aes.h"

#include "burrow/core.h"
#include "burrow/crypto/cipher.h"
#include "burrow/error.h"

#include <stddef.h>
#include <stdint.h>

/* Which code a block runs on, picked when it is made. */
enum {
    AES_IMPL_CT,    /* the portable bitsliced code */
    AES_IMPL_X86,   /* AES-NI */
    AES_IMPL_ARM64, /* the Armv8 AES instructions */
};

typedef struct AesBlock {
    int rounds;
    int impl;
    /* The round keys as FIPS 197 lays them out, (rounds + 1) * 16 bytes. */
    Byte enc[240];
    /* The decryption keys for the equivalent inverse cipher, which only the
     * instructions use. */
    Byte dec[240];
    /* The round keys spread over bit planes for the portable code, eight words
     * a round. */
    uint64_t sk[120];
} AesBlock;

/* The AesBlock behind b, or NULL when b is not AES. */
AesBlock *burrow__aes_block_of(CipherBlock b);

/* Encrypts or decrypts n consecutive blocks of src into dst, which may be src
 * exactly. No checks: the callers have done them. */
void burrow__aes_encrypt_blocks(const AesBlock *b, Byte *dst, const Byte *src,
                                size_t n);
void burrow__aes_decrypt_blocks(const AesBlock *b, Byte *dst, const Byte *src,
                                size_t n);

/* Go's tests run every implementation through cryptotest.TestAllImplementations.
 * This is how burrow's do it: with on true, blocks made afterwards use the
 * portable code even where the processor has AES instructions. */
void burrow__aes_set_portable(bool on);

/* The name Go registers its AES instructions under on this architecture,
 * "AES-NI" or "Armv8.0", with *available saying whether this processor has
 * them. NULL when burrow was built without any. */
const char *burrow__aes_hardware(bool *available);

/* aes.CTR's XORKeyStreamAt, which Go's tests reach through the FIPS module:
 * the key stream from offset on, whatever has been read from s so far, without
 * moving s. False, with nothing written, when s is not CTR over an AES block. */
bool burrow__aes_ctr_xor_key_stream_at(CipherStream s, Slice dst, Slice src,
                                       uint64_t offset);

/* CMAC, from crypto/internal/fips140/aes/gcm, in cmac.c. */
typedef struct Cmac {
    AesBlock b;
    Byte k1[AES_BLOCK_SIZE];
    Byte k2[AES_BLOCK_SIZE];
} Cmac;

/* NewCMAC: c over a copy of b, with the subkeys worked out. */
void burrow__cmac_init(Cmac *c, const AesBlock *b);

/* CMAC.MAC: the tag of m into out. */
void burrow__cmac_mac(Cmac *c, Byte out[AES_BLOCK_SIZE], Slice m);

/* CounterKDF, from the same package, in cmac.c: a KDF in counter mode over
 * CMAC-AES, from NIST SP 800-108 Revision 1 Update 1, Section 4.1, with a
 * 256-bit output, an 8-bit label and a 96-bit context. */
typedef struct CounterKdf {
    Cmac mac;
} CounterKdf;

/* NewCounterKDF: kdf over a copy of b. */
void burrow__counter_kdf_init(CounterKdf *kdf, const AesBlock *b);

/* CounterKDF.DeriveKey: the key for label and context into out. */
void burrow__counter_kdf_derive_key(CounterKdf *kdf, Byte out[32], Byte label,
                                    const Byte context[12]);

/* GCM's errOpen, "cipher: message authentication failed", which Go does not
 * export either. */
extern const Error burrow__gcm_err_open;

#endif /* BURROW_CRYPTO_AES_INTERNAL_H */
