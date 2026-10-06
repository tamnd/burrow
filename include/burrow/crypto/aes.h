/* crypto/aes, the Advanced Encryption Standard from FIPS 197.
 *
 *     Error err;
 *     CipherBlock block = aes_new_cipher(a, key, &err);
 *     if (BURROW_FAILED(err))
 *         return err;
 *     CipherAEAD gcm = cipher_new_gcm(a, block, &err);
 *
 * What comes back is a cipher.Block, which encrypts one 16 byte block and is
 * not much use alone. Put it in one of the modes in burrow/crypto/cipher.h,
 * which for almost every program should be GCM.
 *
 * On x86 processors with AES-NI and arm64 ones with the Armv8 AES instructions
 * the work is done by those instructions, which take the same time whatever
 * the key and data are. Everywhere else, and wherever GODEBUG has cpu.aes=off,
 * it is portable C that is also constant time: it never looks up a table with
 * a secret, which is where the usual table driven AES leaks its key through
 * the cache. Go's portable AES does use tables. The answers are the same
 * either way.
 *
 * A Block is safe to use from any number of threads at once, since nothing in
 * it changes after aes_new_cipher. Go's BoringCrypto and FIPS 140 modes are not
 * here.
 *
 * Copyright 2009 The Go Authors. All rights reserved.
 * Copyright 2026 The burrow Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style licence that can be found
 * in the LICENSE file. */

/* burrow:package crypto/aes */

#ifndef BURROW_CRYPTO_AES_H
#define BURROW_CRYPTO_AES_H

#include "burrow/core.h"
#include "burrow/crypto/cipher.h"
#include "burrow/error.h"
#include "burrow/mem.h"
#include "burrow/slice.h"
#include "burrow/type.h"

#ifdef __cplusplus
extern "C" {
#endif

/* The AES block size in bytes. */
#define AES_BLOCK_SIZE 16

/* aes.KeySizeError: the length of a key that was not 16, 24 or 32 bytes. Its
 * message is Go's, "crypto/aes: invalid key size 7". */
typedef Int AesKeySizeError;

extern const Type *const TYPE_AES_KEY_SIZE_ERROR;

/* Go's Error method, built in a. */
BURROW_OWNS(ret) Str aes_key_size_error_error(AesKeySizeError k, Alloc *a);

/* An Error for k with its message, built in a. Out of memory gives
 * burrow_err_out_of_memory. */
BURROW_OWNS(ret) Error aes_key_size_error_as_error(AesKeySizeError k, Alloc *a);

/* aes.NewCipher: AES-128, AES-192 or AES-256 with key, a Slice of Byte 16, 24
 * or 32 bytes long, allocated from a. The key is expanded and not kept, so it
 * can change or go away after this returns. Any other length gives a nil Block
 * and an AesKeySizeError, in the calling goroutine's error arena. A nil Block
 * and burrow_err_out_of_memory if a is out of memory. */
BURROW_OWNS(ret) CipherBlock aes_new_cipher(Alloc *a, Slice key, Error *err);

#ifdef __cplusplus
}
#endif

#endif /* BURROW_CRYPTO_AES_H */
