/* crypto/ed25519, the Ed25519 signature scheme from RFC 8032.
 *
 *     Ed25519PrivateKey priv;
 *     Ed25519PublicKey pub = ed25519_generate_key(a, (IoReader){0}, &priv, &err);
 *     Slice sig = ed25519_sign(a, priv, message);
 *     if (!ed25519_verify(pub, message, sig))
 *         return errors_new(a, BURROW_S("bad signature"));
 *
 * Keys are Slices of Byte, as Go's are slices of byte. A private key is the 32
 * byte seed RFC 8032 calls the private key, followed by the 32 byte public key.
 * A seed alone makes a key with ed25519_new_key_from_seed, and
 * ed25519_private_key_seed gives it back.
 *
 * ed25519_verify and ed25519_verify_with_options follow Go's rules for what is
 * a valid signature, which are those of most other implementations and not
 * quite the stricter ones in RFC 8032: see edwards25519.h for the two encodings
 * of a point that are let through.
 *
 * Signing does not use entropy, and the same key and message always give the
 * same signature. The rand argument of ed25519_private_key_sign is there
 * because a CryptoSigner has one, and is ignored.
 *
 * Go keeps the key it unpacked from the last private key it saw, so that signing
 * many messages with one key does not hash the seed every time, and it runs the
 * self tests FIPS 140 asks for. Neither is here: every call hashes the seed,
 * which costs one SHA-512 of 32 bytes, and there is no FIPS mode to test for.
 *
 * Copyright 2016 The Go Authors. All rights reserved.
 * Copyright 2026 The burrow Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style licence that can be found
 * in the LICENSE file. */

/* burrow:package crypto/ed25519 */

#ifndef BURROW_CRYPTO_ED25519_H
#define BURROW_CRYPTO_ED25519_H

#include "burrow/core.h"
#include "burrow/crypto.h"
#include "burrow/error.h"
#include "burrow/io.h"
#include "burrow/mem.h"
#include "burrow/slice.h"
#include "burrow/type.h"

#ifdef __cplusplus
extern "C" {
#endif

/* The sizes, in bytes, of a public key, a private key, a signature and a seed. */
#define ED25519_PUBLIC_KEY_SIZE 32
#define ED25519_PRIVATE_KEY_SIZE 64
#define ED25519_SIGNATURE_SIZE 64
#define ED25519_SEED_SIZE 32

/* ed25519.PublicKey, 32 bytes. */
typedef Slice Ed25519PublicKey;

extern const Type burrow_type_Ed25519PublicKey;
#define TYPE_ED25519_PUBLIC_KEY TYPE_OF(Ed25519PublicKey)

/* ed25519.PrivateKey, 64 bytes: the seed, then the public key. */
typedef Slice Ed25519PrivateKey;

extern const Type burrow_type_Ed25519PrivateKey;
#define TYPE_ED25519_PRIVATE_KEY TYPE_OF(Ed25519PrivateKey)

/* PublicKey.Equal: whether x holds an Ed25519PublicKey with the same bytes as
 * pub. */
bool ed25519_public_key_equal(Ed25519PublicKey pub, CryptoPublicKey x);

/* PrivateKey.Public: a copy of the public half of priv, from a. A nil Slice if
 * a is out of memory. */
BURROW_OWNS(ret) Ed25519PublicKey ed25519_private_key_public(Ed25519PrivateKey priv,
                                                             Alloc *a);

/* PrivateKey.Equal: whether x holds an Ed25519PrivateKey with the same bytes as
 * priv, compared in constant time. */
bool ed25519_private_key_equal(Ed25519PrivateKey priv, CryptoPrivateKey x);

/* PrivateKey.Seed: a copy of the seed, the first 32 bytes of priv, from a. This
 * is what RFC 8032 calls the private key. */
BURROW_OWNS(ret) Slice ed25519_private_key_seed(Ed25519PrivateKey priv, Alloc *a);

/* ed25519.Options: which variant to sign or verify with. hash is zero for
 * Ed25519 and CRYPTO_SHA512 for Ed25519ph, whose message is a SHA-512 digest.
 * context, at most 255 bytes, selects Ed25519ctx when it is not empty and hash
 * is zero, and is the context string of Ed25519ph. */
typedef struct Ed25519Options {
    CryptoHash hash;
    Str context;
} Ed25519Options;

extern const Type burrow_type_Ed25519Options;
#define TYPE_ED25519_OPTIONS TYPE_OF(Ed25519Options)

/* Options.HashFunc: o's hash, zero for Ed25519 and Ed25519ctx. */
CryptoHash ed25519_options_hash_func(const Ed25519Options *o);

/* The same, as a CryptoSignerOpts for ed25519_private_key_sign. It
 * points at o, so o has to outlive it. */
BURROW_BORROWS(ret, o) CryptoSignerOpts
ed25519_options_as_signer_opts(const Ed25519Options *o);

/* PrivateKey.Sign: signs message with priv and returns the signature from a.
 * rand is ignored and can be nil.
 *
 * When opts says CRYPTO_SHA512 this is Ed25519ph and message has to be a
 * SHA-512 digest. Otherwise opts has to say zero and message is signed as it
 * is, since Ed25519 reads a message twice and cannot sign a digest of it. opts
 * can be a crypto_hash_as_signer_opts of zero or CRYPTO_SHA512, or an
 * ed25519_options_as_signer_opts, which is the one way to give a context. */
BURROW_OWNS(ret) Slice ed25519_private_key_sign(Ed25519PrivateKey priv, Alloc *a,
                                                IoReader rand, Slice message,
                                                CryptoSignerOpts opts, Error *err);

/* priv as a CryptoSigner, which public_key and sign of the CryptoSigner work
 * through. public_key has to return a CryptoPublicKey that points at an
 * Ed25519PublicKey, and this is where it is kept: ed25519_private_key_signer
 * fills s in with priv and the public half of it and returns a CryptoSigner
 * that borrows s, which has to outlive it. Nothing is copied, so priv's bytes
 * have to as well. */
typedef struct Ed25519Signer {
    Ed25519PrivateKey priv;
    Ed25519PublicKey pub;
} Ed25519Signer;

BURROW_BORROWS(ret, s) CryptoSigner ed25519_private_key_signer(Ed25519PrivateKey priv,
                                                               Ed25519Signer *s);

/* ed25519.GenerateKey: a key from 32 bytes of random, which is what
 * ed25519_new_key_from_seed would make of them. Returns the public key from a
 * and sets *priv to the private key from a.
 *
 * A nil random means the system's generator, as it does in Go from 1.26. With
 * GODEBUG=cryptocustomrand=1 it means crypto_rand_reader instead, which is
 * what it meant before. An error reading random is returned as it is. */
BURROW_OWNS(ret) Ed25519PublicKey ed25519_generate_key(Alloc *a, IoReader random,
                                                       Ed25519PrivateKey *priv,
                                                       Error *err);

/* ed25519.NewKeyFromSeed: the private key for a 32 byte seed, from a. Panics with
 * "ed25519: bad seed length: " and the length for any other size. */
BURROW_OWNS(ret) Ed25519PrivateKey ed25519_new_key_from_seed(Alloc *a, Slice seed);

/* ed25519.Sign: the signature of message by priv, from a. Panics if priv is not
 * 64 bytes. */
BURROW_OWNS(ret) Slice ed25519_sign(Alloc *a, Ed25519PrivateKey priv, Slice message);

/* ed25519.Verify: whether sig is a valid Ed25519 signature of message by pub.
 * Panics if pub is not 32 bytes. None of the inputs are taken to be secret, and
 * the time it takes depends on them. */
bool ed25519_verify(Ed25519PublicKey pub, Slice message, Slice sig);

/* ed25519.VerifyWithOptions: no error when sig is a valid signature of message
 * by pub, for the variant opts names: see Ed25519Options. Panics if pub is not
 * 32 bytes. Like ed25519_verify, it takes nothing to be secret. */
BURROW_STATIC(ret) Error ed25519_verify_with_options(Ed25519PublicKey pub,
                                                     Slice message, Slice sig,
                                                     const Ed25519Options *opts);

#ifdef __cplusplus
}
#endif

#endif /* BURROW_CRYPTO_ED25519_H */
