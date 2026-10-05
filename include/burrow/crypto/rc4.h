/* crypto/rc4, the RC4 stream cipher from Bruce Schneier's Applied
 * Cryptography.
 *
 *     Error err;
 *     Rc4Cipher *c = rc4_new_cipher(a, key, &err);
 *     if (BURROW_FAILED(err))
 *         return err;
 *     rc4_cipher_xor_key_stream(c, dst, src);
 *
 * RC4 is broken and should not be used for anything new. It is here for the
 * old protocols and files that still need it.
 *
 * A Cipher carries its place in the key stream, so it is for one thread at a
 * time. It also looks up its state with bytes of the key stream, so it is not
 * constant time either, as Go's is not.
 *
 * Copyright 2009 The Go Authors. All rights reserved.
 * Copyright 2026 The burrow Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style licence that can be found
 * in the LICENSE file. */

/* burrow:package crypto/rc4 */

#ifndef BURROW_CRYPTO_RC4_H
#define BURROW_CRYPTO_RC4_H

#include "burrow/core.h"
#include "burrow/crypto/cipher.h"
#include "burrow/error.h"
#include "burrow/mem.h"
#include "burrow/slice.h"
#include "burrow/type.h"

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* rc4.Cipher: an instance of RC4 using a particular key. */
typedef struct Rc4Cipher {
    uint32_t s[256];
    uint8_t i, j;
} Rc4Cipher;

/* rc4.KeySizeError: the length of a key that was not 1 to 256 bytes. Its
 * message is "crypto/rc4: invalid key size " and the number. */
typedef Int Rc4KeySizeError;

extern const Type *const TYPE_RC4_KEY_SIZE_ERROR;

/* Go's Error method, built in a. */
BURROW_OWNS(ret) Str rc4_key_size_error_error(Rc4KeySizeError k, Alloc *a);

/* An Error for k with its message, built in a. Out of memory gives
 * burrow_err_out_of_memory. */
BURROW_OWNS(ret) Error rc4_key_size_error_as_error(Rc4KeySizeError k, Alloc *a);

/* rc4.NewCipher: a Cipher allocated from a for key, a Slice of 1 to 256 Byte.
 * Another length sets *err to an Rc4KeySizeError and gives NULL. */
BURROW_OWNS(ret) Rc4Cipher *rc4_new_cipher(Alloc *a, Slice key, Error *err);

/* rc4.Cipher.Reset: zeroes the key data and makes c unusable.
 *
 * Deprecated: Reset can't guarantee that the key will be entirely removed from
 * the process's memory. */
void rc4_cipher_reset(Rc4Cipher *c);

/* rc4.Cipher.XORKeyStream: sets dst to src XORed with the key stream. dst is
 * at least as long as src, and the two overlap entirely or not at all, or it
 * panics. */
void rc4_cipher_xor_key_stream(Rc4Cipher *c, Slice dst, Slice src);

/* c as a cipher.Stream, which is what a CipherStreamReader or
 * CipherStreamWriter takes. It points at c, so c has to outlive it. */
CipherStream rc4_cipher_as_cipher_stream(Rc4Cipher *c);

#ifdef __cplusplus
}
#endif

#endif /* BURROW_CRYPTO_RC4_H */
