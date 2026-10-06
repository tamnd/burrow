/* crypto/des, the Data Encryption Standard and Triple DES from FIPS 46-3.
 *
 *     Error err;
 *     CipherBlock block = des_new_triple_des_cipher(a, key, &err);
 *     if (BURROW_FAILED(err))
 *         return err;
 *     CipherBlockMode mode = cipher_new_cbc_decrypter(a, block, iv);
 *
 * DES is broken, and Triple DES is slow and has a block small enough to run
 * out of. They are here for the old protocols and files that still need them.
 * Anything new should use crypto/aes.
 *
 * What comes back is a cipher.Block that does one 8 byte block at a time, for
 * the modes in burrow/crypto/cipher.h. It is safe to use from any number of
 * threads at once, since nothing in it changes after it is made.
 *
 * Like Go's, it looks up a table with bits of the key and the data in every
 * round, so it is not constant time and its key can leak through the cache to
 * anyone who can time it.
 *
 * Copyright 2010 The Go Authors. All rights reserved.
 * Copyright 2026 The burrow Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style licence that can be found
 * in the LICENSE file. */

/* burrow:package crypto/des */

#ifndef BURROW_CRYPTO_DES_H
#define BURROW_CRYPTO_DES_H

#include "burrow/core.h"
#include "burrow/crypto/cipher.h"
#include "burrow/error.h"
#include "burrow/mem.h"
#include "burrow/slice.h"
#include "burrow/type.h"

#ifdef __cplusplus
extern "C" {
#endif

/* The DES block size in bytes. */
#define DES_BLOCK_SIZE 8

/* des.KeySizeError: the length of a key that was not 8 bytes, or 24 for
 * Triple DES. Its message is "crypto/des: invalid key size " and the number. */
typedef Int DesKeySizeError;

extern const Type *const TYPE_DES_KEY_SIZE_ERROR;

/* Go's Error method, built in a. */
BURROW_OWNS(ret) Str des_key_size_error_error(DesKeySizeError k, Alloc *a);

/* An Error for k with its message, built in a. Out of memory gives
 * burrow_err_out_of_memory. */
BURROW_OWNS(ret) Error des_key_size_error_as_error(DesKeySizeError k, Alloc *a);

/* des.NewCipher: DES with key, a Slice of 8 Byte. Another length sets *err to
 * a DesKeySizeError and gives a Block with a NULL table. The Block is allocated
 * from a. */
BURROW_OWNS(ret) CipherBlock des_new_cipher(Alloc *a, Slice key, Error *err);

/* des.NewTripleDESCipher: Triple DES, encrypt with the first 8 bytes of key,
 * decrypt with the next 8 and encrypt with the last 8. key is a Slice of 24
 * Byte, and anything else is a DesKeySizeError. For two key Triple DES, repeat
 * the first 8 bytes at the end. */
BURROW_OWNS(ret) CipherBlock des_new_triple_des_cipher(Alloc *a, Slice key, Error *err);

#ifdef __cplusplus
}
#endif

#endif /* BURROW_CRYPTO_DES_H */
