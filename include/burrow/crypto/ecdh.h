/* crypto/ecdh, Elliptic Curve Diffie-Hellman over the NIST curves and over
 * Curve25519.
 *
 *     EcdhPrivateKey *alice = ecdh_curve_generate_key(ecdh_x25519(), a,
 *                                                     (IoReader){0}, &err);
 *     EcdhPrivateKey *bob = ecdh_curve_generate_key(ecdh_x25519(), a,
 *                                                   (IoReader){0}, &err);
 *     Slice s1 = ecdh_private_key_ecdh(alice, a, ecdh_private_key_public_key(bob),
 *                                      &err);
 *     Slice s2 = ecdh_private_key_ecdh(bob, a, ecdh_private_key_public_key(alice),
 *                                      &err);
 *
 * s1 and s2 are now the same 32 bytes, which neither side sent. A shared secret
 * is not uniformly random, so it wants a key derivation function, like HKDF,
 * before it is a key.
 *
 * A curve is one of the four values the functions below return, and nothing
 * else: Go's Curve is an interface with an unexported method, so no package
 * outside crypto/ecdh can make one either. Comparing two of them with == is
 * how to tell whether keys are on the same curve.
 *
 * Keys are allocated from the Alloc they are made with, in one block, and freed
 * with ecdh_private_key_free and ecdh_public_key_free. The public key of a
 * private key is part of the same block and is not freed on its own.
 *
 * Go's NIST curves go through BoringCrypto in a boring build and through the
 * FIPS 140 module otherwise, which also checks every key it generates. burrow has
 * neither, and does what the module does without the checks: the arithmetic is
 * the same fiat-crypto code, and P-256 has the same precomputed tables.
 *
 * Copyright 2022 The Go Authors. All rights reserved.
 * Copyright 2026 The burrow Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style licence that can be found
 * in the LICENSE file. */

/* burrow:package crypto/ecdh */

#ifndef BURROW_CRYPTO_ECDH_H
#define BURROW_CRYPTO_ECDH_H

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

/* ecdh.Curve: one of P-256, P-384, P-521 and X25519. */
typedef struct EcdhCurve EcdhCurve;

/* ecdh.PublicKey: a point on a curve, as its encoding. */
typedef struct EcdhPublicKey EcdhPublicKey;

extern const Type burrow_type_EcdhPublicKey;
#define TYPE_ECDH_PUBLIC_KEY TYPE_OF(EcdhPublicKey)

/* ecdh.PrivateKey: a scalar, with the public key it makes. */
typedef struct EcdhPrivateKey EcdhPrivateKey;

extern const Type burrow_type_EcdhPrivateKey;
#define TYPE_ECDH_PRIVATE_KEY TYPE_OF(EcdhPrivateKey)

/* ecdh.P256, P384 and P521: the NIST curves of FIPS 186-3, also called
 * secp256r1, secp384r1 and secp521r1, and prime256v1 for the first. Each call
 * gives the same curve. */
BURROW_STATIC(ret) const EcdhCurve *ecdh_p256(void);
BURROW_STATIC(ret) const EcdhCurve *ecdh_p384(void);
BURROW_STATIC(ret) const EcdhCurve *ecdh_p521(void);

/* ecdh.X25519: X25519 from RFC 7748, the function over Curve25519. */
BURROW_STATIC(ret) const EcdhCurve *ecdh_x25519(void);

/* What %v prints for a curve in Go: "P-256", "P-384", "P-521" or "X25519". */
BURROW_STATIC(ret) Str ecdh_curve_string(const EcdhCurve *c);

/* Curve.GenerateKey: a new private key on c, from a, with its scalar made from
 * the bytes of rand.
 *
 * A nil rand means the system's generator. So does any other rand, as it does
 * in Go from 1.26, unless GODEBUG has cryptocustomrand=1, when rand is read
 * and a nil one means crypto_rand_reader. An error reading it is returned as it
 * is. */
BURROW_OWNS(ret) EcdhPrivateKey *ecdh_curve_generate_key(const EcdhCurve *c, Alloc *a,
                                                         IoReader rand, Error *err);

/* Curve.NewPrivateKey: the private key with the scalar in key, from a.
 *
 * For the NIST curves key is big endian and as long as the curve's order, so
 * 32, 48 or 66 bytes, and it has to be more than zero and less than the order.
 * Anything else is "crypto/ecdh: invalid private key". For X25519 any 32 bytes
 * will do, and any other length is "crypto/ecdh: invalid private key size". */
BURROW_OWNS(ret) EcdhPrivateKey *
ecdh_curve_new_private_key(const EcdhCurve *c, Alloc *a, Slice key, Error *err);

/* Curve.NewPublicKey: the public key with the encoding in key, from a.
 *
 * For the NIST curves key is the uncompressed encoding of a point on the
 * curve, from section 4.3.6 of ANSI X9.62: a 4, then x and y. Compressed points
 * and the point at infinity are errors. For X25519 key is any 32 bytes, as RFC
 * 7748 has it, and a bad one is not found until ecdh_private_key_ecdh. */
BURROW_OWNS(ret) EcdhPublicKey *ecdh_curve_new_public_key(const EcdhCurve *c, Alloc *a,
                                                          Slice key, Error *err);

/* Frees a key from ecdh_curve_generate_key, ecdh_curve_new_private_key or
 * ecdh_curve_new_public_key. NULL is fine. */
void ecdh_private_key_free(EcdhPrivateKey *k);
void ecdh_public_key_free(EcdhPublicKey *k);

/* PrivateKey.ECDH: the shared secret of k and remote, from a. For the NIST
 * curves it is the x coordinate of the shared point, as long as an element of
 * the field, from section 3.3.1 of SEC 1. For X25519 it is the 32 bytes RFC 7748
 * gives, and an error when they are all zero, which is what a remote key of
 * small order gives.
 *
 * remote has to be on the same curve as k. A nil Slice on an error. */
BURROW_OWNS(ret) Slice ecdh_private_key_ecdh(const EcdhPrivateKey *k, Alloc *a,
                                             const EcdhPublicKey *remote, Error *err);

/* PrivateKey.Bytes: a copy of the scalar, from a, in the encoding
 * ecdh_curve_new_private_key takes. */
BURROW_OWNS(ret) Slice ecdh_private_key_bytes(const EcdhPrivateKey *k, Alloc *a);

/* PrivateKey.Curve. */
BURROW_STATIC(ret) const EcdhCurve *ecdh_private_key_curve(const EcdhPrivateKey *k);

/* PrivateKey.Equal: whether x holds an EcdhPrivateKey on the same curve with the
 * same scalar, compared in constant time. Make x with
 * BURROW_ANY(TYPE_ECDH_PRIVATE_KEY, other). */
bool ecdh_private_key_equal(const EcdhPrivateKey *k, CryptoPrivateKey x);

/* PrivateKey.PublicKey: the public key of k, which is part of k. */
BURROW_BORROWS(ret, k) const EcdhPublicKey *
ecdh_private_key_public_key(const EcdhPrivateKey *k);

/* PrivateKey.Public: the same, as a CryptoPublicKey that holds an
 * EcdhPublicKey. */
BURROW_BORROWS(ret, k) CryptoPublicKey ecdh_private_key_public(const EcdhPrivateKey *k);

/* PublicKey.Bytes: a copy of the encoding, from a, in the form
 * ecdh_curve_new_public_key takes. */
BURROW_OWNS(ret) Slice ecdh_public_key_bytes(const EcdhPublicKey *k, Alloc *a);

/* PublicKey.Curve. */
BURROW_STATIC(ret) const EcdhCurve *ecdh_public_key_curve(const EcdhPublicKey *k);

/* PublicKey.Equal: whether x holds an EcdhPublicKey on the same curve with the
 * same encoding. */
bool ecdh_public_key_equal(const EcdhPublicKey *k, CryptoPublicKey x);

/* ecdh.KeyExchanger: something that does the private half of a key exchange,
 * possibly with a key that never leaves a hardware module. An EcdhPrivateKey is
 * one, through ecdh_private_key_key_exchanger. */
typedef struct EcdhKeyExchangerVT {
    const Type *self_type;
    const EcdhPublicKey *(*public_key)(void *self);
    const EcdhCurve *(*curve)(void *self);
    Slice (*ecdh)(void *self, Alloc *a, const EcdhPublicKey *remote, Error *err);
} EcdhKeyExchangerVT;

typedef struct EcdhKeyExchanger {
    const EcdhKeyExchangerVT *vt;
    void *data;
} EcdhKeyExchanger;

BURROW_BORROWS(ret, x) static inline const EcdhPublicKey *
ecdh_key_exchanger_public_key(EcdhKeyExchanger x) {
    return x.vt->public_key(x.data);
}

BURROW_STATIC(ret) static inline const EcdhCurve *
ecdh_key_exchanger_curve(EcdhKeyExchanger x) {
    return x.vt->curve(x.data);
}

BURROW_OWNS(ret) static inline Slice
ecdh_key_exchanger_ecdh(EcdhKeyExchanger x, Alloc *a, const EcdhPublicKey *remote,
                        Error *err) {
    return x.vt->ecdh(x.data, a, remote, err);
}

/* k as an EcdhKeyExchanger, which borrows it. */
BURROW_BORROWS(ret, k) EcdhKeyExchanger
ecdh_private_key_key_exchanger(const EcdhPrivateKey *k);

#ifdef __cplusplus
}
#endif

#endif /* BURROW_CRYPTO_ECDH_H */
