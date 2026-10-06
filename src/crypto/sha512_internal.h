/* SHA-512 on the stack, for code in the library that hashes several pieces into
 * one sum and has no allocator to make a Hash from, crypto/ed25519 first.
 *
 * Copyright 2026 The burrow Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style licence that can be found
 * in the LICENSE file. */

#ifndef BURROW_SRC_CRYPTO_SHA512_INTERNAL_H
#define BURROW_SRC_CRYPTO_SHA512_INTERNAL_H

#include "burrow/crypto/sha512.h"

#include "burrow/core.h"

#include <stdint.h>

typedef struct Sha512Digest {
    uint64_t h[8];
    Byte x[SHA512_BLOCK_SIZE];
    Int nx;
    uint64_t len;
    Int size;
} Sha512Digest;

/* Starts d as a SHA-512 hash with nothing written. */
void burrow__sha512_init(Sha512Digest *d);

/* Hashes the n bytes at p. p can be NULL when n is 0. */
void burrow__sha512_write(Sha512Digest *d, const void *p, Int n);

/* The sum of everything written. It finishes d, which has to be started again
 * before anything else is written to it. */
void burrow__sha512_sum(Sha512Digest *d, Byte out[SHA512_SIZE]);

#endif /* BURROW_SRC_CRYPTO_SHA512_INTERNAL_H */
