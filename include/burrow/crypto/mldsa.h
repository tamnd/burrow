/* crypto/mldsa, the quantum-resistant signature scheme ML-DSA of NIST FIPS 204,
 * once called Dilithium.
 *
 *     MldsaPrivateKey *sk = mldsa_generate_key(a, mldsa_mldsa44(), &err);
 *     Slice sig = mldsa_private_key_sign_deterministic(sk, a, msg,
 *                                                      (CryptoSignerOpts){0}, &err);
 *
 *     MldsaPublicKey *pk = mldsa_new_public_key(a, mldsa_mldsa44(), pk_bytes, &err);
 *     Error verr = mldsa_verify(pk, msg, sig, NULL);
 *
 * Most programs want ML-DSA-44. ML-DSA-65 and ML-DSA-87 are the same with
 * bigger keys and signatures and a higher security level.
 *
 * A private key is its 32 byte seed, and is expanded when it is made into the
 * values signing needs, which take about 90 KB. A public key is kept as its
 * encoding, and Verify expands it each time, as Go does. Keys are allocated from
 * the Alloc they are made with and freed with the _free function of their type.
 * The public key of a private key is part of the private key and is not freed
 * on its own.
 *
 * Every operation on secret data is constant time. Go does the FIPS 140 self
 * test and the pairwise consistency test of a new key only in FIPS mode, which
 * burrow does not have, so it does neither.
 *
 * Copyright 2025 The Go Authors. All rights reserved.
 * Copyright 2026 The burrow Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style licence that can be found
 * in the LICENSE file. */

/* burrow:package crypto/mldsa */

#ifndef BURROW_CRYPTO_MLDSA_H
#define BURROW_CRYPTO_MLDSA_H

#include "burrow/core.h"
#include "burrow/crypto.h"
#include "burrow/error.h"
#include "burrow/io.h"
#include "burrow/mem.h"
#include "burrow/slice.h"
#include "burrow/type.h"

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* PrivateKeySize: the size of the seed a private key is made from. */
#define MLDSA_PRIVATE_KEY_SIZE 32

/* MLDSA44PublicKeySize and the rest: the sizes of public keys and signatures
 * for each parameter set. */
#define MLDSA_MLDSA44_PUBLIC_KEY_SIZE 1312
#define MLDSA_MLDSA65_PUBLIC_KEY_SIZE 1952
#define MLDSA_MLDSA87_PUBLIC_KEY_SIZE 2592
#define MLDSA_MLDSA44_SIGNATURE_SIZE 2420
#define MLDSA_MLDSA65_SIGNATURE_SIZE 3309
#define MLDSA_MLDSA87_SIGNATURE_SIZE 4627

/* Parameters: one of the parameter sets of FIPS 204. Go's is a small value
 * that compares equal to itself. Here each set is one static object and
 * mldsa_mldsa44 and the others always give the same pointer, which can be
 * compared with ==. NULL, Go's zero Parameters, is not a set, and neither is
 * any other pointer. */
typedef struct MldsaParameters MldsaParameters;

/* MLDSA44, MLDSA65 and MLDSA87: the three parameter sets. */
BURROW_STATIC(ret) const MldsaParameters *mldsa_mldsa44(void);
BURROW_STATIC(ret) const MldsaParameters *mldsa_mldsa65(void);
BURROW_STATIC(ret) const MldsaParameters *mldsa_mldsa87(void);

/* Parameters.String: the name of params, as "ML-DSA-44". */
BURROW_STATIC(ret) Str mldsa_parameters_string(const MldsaParameters *params);

/* Parameters.PublicKeySize and SignatureSize: the sizes for params, in bytes. */
Int mldsa_parameters_public_key_size(const MldsaParameters *params);
Int mldsa_parameters_signature_size(const MldsaParameters *params);

/* PrivateKey: the secret key, with the values worked out from its seed. It
 * signs, and as a CryptoSigner it is what crypto.Signer wants. */
typedef struct MldsaPrivateKey MldsaPrivateKey;

extern const Type burrow_type_MldsaPrivateKey;
#define TYPE_MLDSA_PRIVATE_KEY TYPE_OF(MldsaPrivateKey)

/* PublicKey: the key that verifies the signatures of its private key. */
typedef struct MldsaPublicKey MldsaPublicKey;

extern const Type burrow_type_MldsaPublicKey;
#define TYPE_MLDSA_PUBLIC_KEY TYPE_OF(MldsaPublicKey)

/* Options: what signing and verifying can take besides the message. context,
 * at most 255 bytes, keeps apart signatures made for different purposes, and
 * the same one has to be given to sign and to verify. It is empty by default. */
typedef struct MldsaOptions {
    Str context;
} MldsaOptions;

extern const Type burrow_type_MldsaOptions;
#define TYPE_MLDSA_OPTIONS TYPE_OF(MldsaOptions)

/* Options.HashFunc: zero, which is what makes opts a crypto.SignerOpts. NULL is
 * fine, as a nil *Options is in Go. */
CryptoHash mldsa_options_hash_func(const MldsaOptions *opts);

/* opts as a CryptoSignerOpts for mldsa_private_key_sign, which points at opts,
 * so opts has to outlive it. NULL is fine and is the same as empty options. */
BURROW_BORROWS(ret, opts) CryptoSignerOpts
mldsa_options_as_signer_opts(const MldsaOptions *opts);

/* ------------------------------------------------------------ private keys */

/* GenerateKey: a new private key for params from the system's generator, from
 * a. A params that is not one of the three sets is "mldsa: invalid
 * parameters". Keep the key secret. */
BURROW_OWNS(ret) MldsaPrivateKey *
mldsa_generate_key(Alloc *a, const MldsaParameters *params, Error *err);

/* NewPrivateKey: the key for params with the given seed, from a. A seed that
 * is not MLDSA_PRIVATE_KEY_SIZE bytes is "mldsa: invalid seed length". */
BURROW_OWNS(ret) MldsaPrivateKey *
mldsa_new_private_key(Alloc *a, const MldsaParameters *params, Slice seed, Error *err);

/* Frees a key from mldsa_generate_key or mldsa_new_private_key, clearing it
 * first. NULL is fine. */
void mldsa_private_key_free(MldsaPrivateKey *sk);

/* PrivateKey.Bytes: the seed of sk, from a. Keep it secret. */
BURROW_OWNS(ret) Slice mldsa_private_key_bytes(const MldsaPrivateKey *sk, Alloc *a);

/* PrivateKey.Equal: whether x is an MldsaPrivateKey made from the same seed
 * with the same parameters. Anything else, a nil one among them, is false. */
bool mldsa_private_key_equal(const MldsaPrivateKey *sk, CryptoPrivateKey x);

/* PrivateKey.PublicKey: the public key of sk. Go returns a copy, so that the
 * public key does not keep the private one alive for its collector. Here it is
 * the one inside sk, which lives as long as sk does. */
BURROW_BORROWS(ret, sk) const MldsaPublicKey *
mldsa_private_key_public_key(const MldsaPrivateKey *sk);

/* PrivateKey.Public: the same, as a CryptoPublicKey. */
BURROW_BORROWS(ret, sk) CryptoPublicKey
mldsa_private_key_public(const MldsaPrivateKey *sk);

/* PrivateKey.Sign: a signature of message by sk, from a. rand is ignored, as
 * the randomness comes from the system's generator, and can be nil.
 *
 * When opts is nil or says zero the message is signed as it is, with the
 * context of opts if it is an mldsa_options_as_signer_opts. When opts says
 * CRYPTO_MLDSA_MU, message is the 64 byte μ that the caller has worked out
 * itself, as RFC 9881 calls external μ. Any other hash is "mldsa: invalid
 * SignerOpts". A NULL sk is "mldsa: zero private key", as Go's zero PrivateKey
 * is. */
BURROW_OWNS(ret) Slice mldsa_private_key_sign(const MldsaPrivateKey *sk, Alloc *a,
                                              IoReader rand, Slice message,
                                              CryptoSignerOpts opts, Error *err);

/* PrivateKey.SignDeterministic: the same, with no randomness, so that the same
 * message gives the same signature. */
BURROW_OWNS(ret) Slice mldsa_private_key_sign_deterministic(const MldsaPrivateKey *sk,
                                                            Alloc *a, Slice message,
                                                            CryptoSignerOpts opts,
                                                            Error *err);

/* sk as a CryptoSigner, which borrows it. */
BURROW_BORROWS(ret, sk) CryptoSigner
mldsa_private_key_signer(const MldsaPrivateKey *sk);

/* ------------------------------------------------------------- public keys */

/* NewPublicKey: the key for params in its encoded form, from a. A params that
 * is not one of the sets is "mldsa: invalid parameters", and an encoding of the
 * wrong length is "mldsa: invalid public key length". */
BURROW_OWNS(ret) MldsaPublicKey *mldsa_new_public_key(Alloc *a,
                                                      const MldsaParameters *params,
                                                      Slice encoding, Error *err);

/* Frees a key from mldsa_new_public_key. NULL is fine, and so is the public
 * key of a private key, which is left alone. */
void mldsa_public_key_free(MldsaPublicKey *pk);

/* PublicKey.Bytes: the encoded form of pk, from a. */
BURROW_OWNS(ret) Slice mldsa_public_key_bytes(const MldsaPublicKey *pk, Alloc *a);

/* PublicKey.Equal: whether x is an MldsaPublicKey with the same encoding and
 * parameters. Anything else, a nil one among them, is false. */
bool mldsa_public_key_equal(const MldsaPublicKey *pk, CryptoPublicKey x);

/* PublicKey.Parameters: the parameter set of pk. */
BURROW_STATIC(ret) const MldsaParameters *
mldsa_public_key_parameters(const MldsaPublicKey *pk);

/* Verify: no error when signature is a valid signature of message by pk, with
 * the context of opts, which can be NULL for none. A NULL pk is "mldsa: nil
 * public key". A signature of the wrong length is "mldsa: invalid signature
 * length", a hint that is not encoded right is "mldsa: invalid signature
 * encoding", and any other bad signature is "mldsa: invalid signature". */
BURROW_STATIC(ret) Error mldsa_verify(const MldsaPublicKey *pk, Slice message,
                                      Slice signature, const MldsaOptions *opts);

#ifdef __cplusplus
}
#endif

#endif /* BURROW_CRYPTO_MLDSA_H */
