/* ChaCha20, Poly1305 and the ChaCha20-Poly1305 AEAD of RFC 8439, with the
 * XChaCha20-Poly1305 variant and its 24 byte nonces. Go vendors these from
 * golang.org/x/crypto/chacha20, internal/poly1305 and chacha20poly1305 into
 * the standard library, where nothing outside it can import them, so here they
 * are a private header. crypto/hpke and crypto/tls reach the AEAD through
 * cipher.AEAD, the same as Go's do.
 *
 * Chacha20Cipher and Poly1305Mac are plain structs that can live anywhere, and
 * the functions that make them from an Alloc are there for the places Go's
 * tests call New. A Poly1305 key is for one message only: two messages under
 * the same key let anyone who sees both forge a third.
 *
 * Go refuses to make either AEAD in FIPS 140-only mode. burrow has no such mode
 * yet, so here they are always available.
 *
 * Copyright 2016 The Go Authors. All rights reserved.
 * Copyright 2026 The burrow Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style licence that can be found
 * in the LICENSE file. */

#ifndef BURROW_CRYPTO_CHACHA20POLY1305_H
#define BURROW_CRYPTO_CHACHA20POLY1305_H

/* crypto.h comes first so that the amalgamation files this header with
 * package crypto, which the packages that use it all import. */
#include "burrow/crypto.h"

#include "burrow/core.h"
#include "burrow/crypto/cipher.h"
#include "burrow/error.h"
#include "burrow/mem.h"
#include "burrow/slice.h"

#include <stdbool.h>
#include <stdint.h>

/* ---------------------------------------------------------------- chacha20 */

enum {
    /* The size of a ChaCha20 key, in bytes. */
    CHACHA20_KEY_SIZE = 32,
    /* The size of the nonce of ChaCha20 as RFC 8439 has it, in bytes. */
    CHACHA20_NONCE_SIZE = 12,
    /* The size of the nonce of XChaCha20, in bytes. */
    CHACHA20_NONCE_SIZE_X = 24,
    /* The size of one block of key stream. */
    CHACHA20_BLOCK_SIZE = 64,
};

/* chacha20.Cipher: a stateful instance of ChaCha20 or XChaCha20 with one key
 * and nonce. The fields are private. */
typedef struct Chacha20Cipher {
    uint32_t key[8];
    uint32_t counter;
    uint32_t nonce[3];
    /* The key stream of the last block, of which the last len bytes have not
     * been used yet. */
    Byte buf[CHACHA20_BLOCK_SIZE];
    Int len;
    /* Set once the counter has gone past its last value. */
    bool overflow;
    /* The quarter rounds of the first column round that do not depend on the
     * counter, worked out once. */
    bool precomp_done;
    uint32_t p1, p5, p9, p13;
    uint32_t p2, p6, p10, p14;
    uint32_t p3, p7, p11, p15;
} Chacha20Cipher;

/* NewUnauthenticatedCipher: ChaCha20 with a 32 byte key, or XChaCha20 when the
 * nonce is 24 bytes, starting at block 0. Any other key is the error
 * "chacha20: wrong key size", and any other nonce "chacha20: wrong nonce size".
 *
 * There is no authentication here. The nonce must never be used twice with the
 * same key, and only an XChaCha20 nonce is long enough to pick at random. */
BURROW_OWNS(ret) Chacha20Cipher *
chacha20_new_unauthenticated_cipher(Alloc *a, Slice key, Slice nonce, Error *err);

/* Cipher.SetCounter: moves s to block counter, where it would be after
 * counter * 64 bytes. It panics with "chacha20: SetCounter attempted to rollback
 * counter" when counter is before a block it has already handed out. */
void chacha20_cipher_set_counter(Chacha20Cipher *s, uint32_t counter);

/* Cipher.XORKeyStream: sets dst to src XORed with the key stream. dst is at
 * least as long as src, and the two overlap entirely or not at all, or it
 * panics. Going past the 2^32 blocks one nonce has panics with "chacha20:
 * counter overflow". */
void chacha20_cipher_xor_key_stream(Chacha20Cipher *s, Slice dst, Slice src);

/* s as a cipher.Stream. It points at s, so s has to outlive it. */
CipherStream chacha20_cipher_as_cipher_stream(Chacha20Cipher *s);

/* HChaCha20: the 32 bytes derived from a 32 byte key and a 16 byte nonce, the
 * first step of XChaCha20, from a. The errors are "chacha20: wrong HChaCha20 key
 * size" and "chacha20: wrong HChaCha20 nonce size". */
BURROW_OWNS(ret) Slice chacha20_hchacha20(Alloc *a, Slice key, Slice nonce, Error *err);

/* ---------------------------------------------------------------- poly1305 */

enum {
    /* The size of a Poly1305 authenticator, in bytes. */
    POLY1305_TAG_SIZE = 16,
};

/* poly1305.MAC: the authenticator of everything written to it so far. The
 * fields are private, except that h is where a test can start it from a state
 * other than zero. */
typedef struct Poly1305Mac {
    uint64_t h[3];
    uint64_t r[2];
    uint64_t s[2];
    Byte buffer[POLY1305_TAG_SIZE];
    Int offset;
    bool finalized;
} Poly1305Mac;

/* Sum: the authenticator of m under the one-time key into out. */
void poly1305_sum(Byte out[POLY1305_TAG_SIZE], Slice m,
                  const Byte key[CHACHA20_KEY_SIZE]);

/* Verify: whether mac is the authenticator of m under key, in constant time. */
bool poly1305_verify(const Byte mac[POLY1305_TAG_SIZE], Slice m,
                     const Byte key[CHACHA20_KEY_SIZE]);

/* New, into a Poly1305Mac of the caller's. */
void poly1305_mac_init(Poly1305Mac *h, const Byte key[CHACHA20_KEY_SIZE]);

/* New: a MAC for the one-time key, from a. NULL when a is out of memory. */
BURROW_OWNS(ret) Poly1305Mac *poly1305_new(Alloc *a, const Byte key[CHACHA20_KEY_SIZE]);

/* MAC.Size: always POLY1305_TAG_SIZE. */
Int poly1305_mac_size(const Poly1305Mac *h);

/* MAC.Write: adds p to the message and returns its length. It panics with
 * "poly1305: write to MAC after Sum or Verify" once either has been called. */
Int poly1305_mac_write(Poly1305Mac *h, Slice p);

/* MAC.Sum: b with the authenticator of everything written appended, the way
 * Go's append does, growing from a when b has no room. */
BURROW_OWNS(ret) BURROW_BORROWS(ret, b) Slice poly1305_mac_sum(Poly1305Mac *h, Alloc *a,
                                                               Slice b);

/* MAC.Verify: whether expected is the authenticator of everything written, in
 * constant time. */
bool poly1305_mac_verify(Poly1305Mac *h, Slice expected);

/* -------------------------------------------------------- chacha20poly1305 */

enum {
    /* The size of the key of either AEAD, in bytes. */
    CHACHA20POLY1305_KEY_SIZE = 32,
    /* The nonce of ChaCha20-Poly1305. Too short to pick at random if one key
     * seals more than 2^32 messages. */
    CHACHA20POLY1305_NONCE_SIZE = 12,
    /* The nonce of XChaCha20-Poly1305, long enough to pick at random. */
    CHACHA20POLY1305_NONCE_SIZE_X = 24,
    /* The size of the tag, and how much longer a ciphertext is than its
     * plaintext. */
    CHACHA20POLY1305_OVERHEAD = 16,
};

/* New: ChaCha20-Poly1305 with a 32 byte key, as a cipher.AEAD allocated from
 * a. Any other key is the error "chacha20poly1305: bad key length".
 *
 * Seal panics with "chacha20poly1305: bad nonce length passed to Seal" when
 * the nonce is not 12 bytes, and Open with the same for Open. A failed Open is
 * "chacha20poly1305: message authentication failed". */
BURROW_OWNS(ret) CipherAEAD chacha20poly1305_new(Alloc *a, Slice key, Error *err);

/* NewX: XChaCha20-Poly1305, the same with 24 byte nonces, which are safe to
 * pick at random. */
BURROW_OWNS(ret) CipherAEAD chacha20poly1305_new_x(Alloc *a, Slice key, Error *err);

/* errOpen, which Go does not export either. */
extern const Error burrow__chacha20poly1305_err_open;

#endif /* BURROW_CRYPTO_CHACHA20POLY1305_H */
