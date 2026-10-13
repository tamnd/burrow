/* crypto/dsa, the Digital Signature Algorithm of FIPS 186-3.
 *
 *     Arena ar;
 *     arena_init(&ar, NULL, 0);
 *     Alloc *a = arena_allocator(&ar);
 *     DsaPrivateKey k = {0};
 *     Error err = dsa_generate_parameters(&k.public_key.parameters, a,
 *                                         (IoReader){0}, DSA_L2048N256);
 *     if (BURROW_OK(err))
 *         err = dsa_generate_key(&k, a, (IoReader){0});
 *     BigInt *s;
 *     BigInt *r = dsa_sign(a, (IoReader){0}, &k, digest, &s, &err);
 *     bool ok = dsa_verify(&k.public_key, digest, r, s);
 *     arena_free(&ar);
 *
 * Go deprecates DSA. It is a legacy algorithm, and Ed25519 in crypto/ed25519
 * is what to use instead. Keys with 1024 bit moduli (DSA_L1024N160) are weak,
 * bigger keys are not widely supported, and FIPS 186-5 no longer approves DSA
 * for making signatures. It is here for programs that have to check old
 * signatures, and none of it is constant time.
 *
 * The parameters and keys are structs of BigInt pointers, as Go's are, so a key
 * can be put together by hand. Every integer this package makes is a new
 * BigInt from the Alloc given, the struct and its words. An arena is the easy
 * way to look after them. Otherwise each goes back with big_int_free and then
 * mem_free of the struct.
 *
 * Copyright 2011 The Go Authors. All rights reserved.
 * Copyright 2026 The burrow Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style licence that can be found
 * in the LICENSE file. */

/* burrow:package crypto/dsa */

#ifndef BURROW_CRYPTO_DSA_H
#define BURROW_CRYPTO_DSA_H

#include "burrow/core.h"
#include "burrow/error.h"
#include "burrow/io.h"
#include "burrow/math/big.h"
#include "burrow/mem.h"
#include "burrow/slice.h"
#include "burrow/type.h"

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Parameters: the domain parameters for a key, which many keys can share. The
 * bit length of q has to be a multiple of 8. */
typedef struct DsaParameters {
    BigInt *p, *q, *g;
} DsaParameters;

/* PublicKey: a DSA public key. */
typedef struct DsaPublicKey {
    DsaParameters parameters;
    BigInt *y;
} DsaPublicKey;

extern const Type burrow_type_DsaPublicKey;
#define TYPE_DSA_PUBLIC_KEY TYPE_OF(DsaPublicKey)

/* PrivateKey: a DSA private key. */
typedef struct DsaPrivateKey {
    DsaPublicKey public_key;
    BigInt *x;
} DsaPrivateKey;

extern const Type burrow_type_DsaPrivateKey;
#define TYPE_DSA_PRIVATE_KEY TYPE_OF(DsaPrivateKey)

/* ErrInvalidPublicKey: what dsa_sign gives for a key it cannot use. FIPS is
 * strict about the form of DSA keys and other code may not be, so a key made
 * somewhere else can get this, and it has to be handled. */
extern const Error dsa_err_invalid_public_key;

/* ParameterSizes: the bit lengths of the primes in a set of parameters that
 * FIPS 186-3, section 4.2, allows. */
typedef Int DsaParameterSizes;

#define DSA_L1024N160 ((DsaParameterSizes)0)
#define DSA_L2048N224 ((DsaParameterSizes)1)
#define DSA_L2048N256 ((DsaParameterSizes)2)
#define DSA_L3072N256 ((DsaParameterSizes)3)

/* GenerateParameters: a random, valid set of parameters of the sizes given, in
 * params, each integer new from a. This can take many seconds, even on a fast
 * machine. The primes come from rand, read with io_read_full, and a nil rand
 * means crypto_rand_reader. Unlike FIPS 186-3 there is no verification seed,
 * as in Go. Sizes that are not one of the four give an error, as does a failed
 * read, and params is left alone then. */
BURROW_STATIC(ret) Error dsa_generate_parameters(DsaParameters *params, Alloc *a,
                                                 IoReader rand,
                                                 DsaParameterSizes sizes);

/* GenerateKey: a key for the parameters already in priv, which have to be
 * valid, with x and y new from a. x comes from rand, read with io_read_full,
 * and a nil rand means crypto_rand_reader. */
BURROW_STATIC(ret) Error dsa_generate_key(DsaPrivateKey *priv, Alloc *a, IoReader rand);

/* Sign: the signature of hash, which should be the hash of a longer message,
 * by priv, as the pair r and s. r is returned and s put in *s, both new from
 * a, or NULL on an error. FIPS 186-3, section 4.6, says to cut the hash down to
 * the length of q in bytes first, and this does not do that for you.
 *
 * The random bytes come from the system's generator whatever rand is, as they
 * do in Go from 1.26, unless GODEBUG has cryptocustomrand=1, when they come
 * from rand and a nil rand means crypto_rand_reader. A key that cannot be used
 * gives dsa_err_invalid_public_key. A private key from someone else can make
 * this take any amount of time. */
BURROW_OWNS(ret) BigInt *dsa_sign(Alloc *a, IoReader rand, const DsaPrivateKey *priv,
                                  Slice hash, BigInt **s, Error *err);

/* Verify: whether r and s are a valid signature of hash by pub. As with
 * dsa_sign, the hash is not cut down to the length of q. */
bool dsa_verify(const DsaPublicKey *pub, Slice hash, const BigInt *r, const BigInt *s);

#ifdef __cplusplus
}
#endif

#endif /* BURROW_CRYPTO_DSA_H */
