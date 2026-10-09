/* SHA-3 and SHAKE on the stack, for code in the library that hashes and reads
 * without an allocator, crypto/mlkem first. A Sha3 started by one of these is
 * written with burrow__sha3_write and read once with burrow__sha3_read: for
 * the fixed size hashes a read of the digest size is the sum, and a SHAKE can
 * be read as many times as wanted.
 *
 * Copyright 2026 The burrow Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style licence that can be found
 * in the LICENSE file. */

#ifndef BURROW_SRC_CRYPTO_SHA3_INTERNAL_H
#define BURROW_SRC_CRYPTO_SHA3_INTERNAL_H

#include "burrow/crypto/sha3.h"

#include "burrow/core.h"

/* Start d as SHA3-256, SHA3-512, SHAKE128 or SHAKE256 with nothing written. */
void burrow__sha3_init256(Sha3 *d);
void burrow__sha3_init512(Sha3 *d);
void burrow__shake128_init(Sha3 *d);
void burrow__shake256_init(Sha3 *d);

/* Absorbs the n bytes at p, which can be NULL when n is 0. It panics after a
 * read, as sha3_write does. */
void burrow__sha3_write(Sha3 *d, const void *p, Int n);

/* Squeezes the next n bytes into out. */
void burrow__sha3_read(Sha3 *d, void *out, Int n);

#endif /* BURROW_SRC_CRYPTO_SHA3_INTERNAL_H */
