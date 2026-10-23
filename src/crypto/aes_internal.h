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

/* Sets b up for key, which is len bytes, 16, 24 or 32. What aes_new_cipher
 * does after its checks, for the code inside the module that keeps its blocks
 * in its own structs. */
void burrow__aes_block_init(AesBlock *b, const Byte *key, Int len);

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

/* Go's crypto/internal/fips140/aes/gcm constructors from gcm_nonces.go, which
 * crypto/tls and crypto/hpke use rather than cipher.NewGCM, which fips140=only
 * does not allow. Not in Go: b is a CipherBlock rather than an *aes.Block, and
 * they panic when it is not AES. */

/* NewGCMWithCounterNonce returns a new AEAD that works like GCM, but enforces
 * the construction of deterministic nonces. The nonce must be 96 bits, the
 * first 32 bits must be an encoding of the module name, and the last 64 bits
 * must be a counter. The starting value of the counter is set on the first call
 * to Seal, and each subsequent call must increment it as a big-endian uint64.
 * If the counter reaches the starting value minus one, Seal will panic.
 *
 * This complies with FIPS 140-3 IG C.H Scenario 3. */
CipherAEAD burrow__gcm_new_with_counter_nonce(Alloc *a, CipherBlock b, Error *err);

/* NewGCMForTLS12 returns a new AEAD that works like GCM, but enforces the
 * construction of nonces as specified in RFC 5288, Section 3 and RFC 9325,
 * Section 7.2.1.
 *
 * This complies with FIPS 140-3 IG C.H Scenario 1.a. */
CipherAEAD burrow__gcm_new_for_tls12(Alloc *a, CipherBlock b, Error *err);

/* NewGCMForSSH returns a new AEAD that works like GCM, but enforces the
 * construction of nonces as specified in RFC 5647.
 *
 * This complies with FIPS 140-3 IG C.H Scenario 1.d. */
CipherAEAD burrow__gcm_new_for_ssh(Alloc *a, CipherBlock b, Error *err);

/* NewGCMWithXORCounterNonce returns a new AEAD that works like GCM, but
 * enforces the construction of deterministic nonces. The nonce must be 96 bits,
 * the first 32 bits must be an encoding of the module name, and the last 64
 * bits must be a counter XOR'd with a fixed value. The module name and XOR mask
 * can be set with [GCMWithCounterNonce.SetNoncePrefixAndMask], or they are set
 * on the first call to Seal, assuming the counter starts at zero. Each
 * subsequent call must increment the counter as a big-endian uint64. If the
 * counter reaches 2⁶⁴ minus one, Seal will panic.
 *
 * This complies with FIPS 140-3 IG C.H Scenario 3. */
CipherAEAD burrow__gcm_new_with_xor_counter_nonce(Alloc *a, CipherBlock b, Error *err);

/* NewGCMForTLS13 returns a new AEAD that works like GCM, but enforces the
 * construction of nonces as specified in RFC 8446, Section 5.3.
 *
 * This complies with FIPS 140-3 IG C.H Scenario 1.a. */
CipherAEAD burrow__gcm_new_for_tls13(Alloc *a, CipherBlock b, Error *err);

/* NewGCMForHPKE returns a new AEAD that works like GCM, but enforces the
 * construction of nonces as specified in RFC 9180, Section 5.2.
 *
 * This complies with FIPS 140-3 IG C.H Scenario 5. */
CipherAEAD burrow__gcm_new_for_hpke(Alloc *a, CipherBlock b, Error *err);

/* NewGCMForQUIC returns a new AEAD that works like GCM, but enforces the
 * construction of nonces as specified in RFC 9001, Section 5.3.
 *
 * Unlike in TLS 1.3, the QUIC nonce counter does not always start at zero, as
 * the packet number does not reset on key updates, so the XOR mask must be
 * provided explicitly instead of being learned on the first Seal call. Note
 * that the nonce passed to Seal must already be XOR'd with the IV, the IV is
 * provided here only to allow Seal to enforce that the counter is strictly
 * increasing.
 *
 * This complies with FIPS 140-3 IG C.H Scenario 5. */
CipherAEAD burrow__gcm_new_for_quic(Alloc *a, CipherBlock b, Slice iv, Error *err);

/* SetNoncePrefixAndMask sets the fixed prefix and XOR mask for the nonces used
 * in Seal. It must be called before the first call to Seal.
 *
 * The first 32 bits of nonce are used as the fixed prefix, and the last 64 bits
 * are used as the XOR mask.
 *
 * Note that Seal expects the nonce to be already XOR'd with the mask. The mask
 * is provided here only to allow Seal to enforce that the counter is strictly
 * increasing.
 *
 * Not in Go: aead must be from one of the XOR counter constructors above, and
 * anything else panics. */
Error burrow__gcm_set_nonce_prefix_and_mask(CipherAEAD aead, Slice nonce);

#endif /* BURROW_CRYPTO_AES_INTERNAL_H */
