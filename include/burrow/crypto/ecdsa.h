/* crypto/ecdsa, the Elliptic Curve Digital Signature Algorithm of FIPS 186-5.
 *
 *     Arena ar;
 *     arena_init(&ar, NULL, 0);
 *     Alloc *a = arena_allocator(&ar);
 *     Error err = BURROW_NO_ERROR;
 *     EcdsaPrivateKey *k = ecdsa_generate_key(a, elliptic_p256(), (IoReader){0}, &err);
 *     Slice sig = ecdsa_sign_asn1(a, (IoReader){0}, k, digest, &err);
 *     bool ok = ecdsa_verify_asn1(&k->public_key, digest, sig);
 *     arena_free(&ar);
 *
 * digest is the output of a hash of the message, which the caller works out.
 * ECDSA signs a number as long as the curve's order, so a longer digest is cut
 * down to that and a shorter one is used as it is.
 *
 * Signing on P-224, P-256, P-384 and P-521 uses the same code as Go's FIPS 140
 * module: constant time arithmetic modulo the order, and a nonce that comes out
 * of an HMAC_DRBG over SHA-512, seeded from random bytes, the private key and
 * the digest, so that a broken random source does not give the key away. The
 * signatures are not deterministic unless the random source is nil and the
 * caller asks for RFC 6979 through ecdsa_private_key_sign. Any other curve, an
 * EllipticCurveParams of your own, goes through a generic implementation built
 * on math/big, which is not constant time.
 *
 * The keys are structs with their fields in the open, as Go's are, so a key can
 * be put together from integers by hand. The ones this package makes come from
 * the Alloc given, in one block with the integers they point at, and are freed
 * with ecdsa_private_key_free and ecdsa_public_key_free. A key built by hand is
 * freed however it was built. An arena makes all of that someone else's job.
 *
 * Go's ECDSA goes through BoringCrypto in a boring build and through the FIPS
 * 140 module otherwise, where FIPS mode adds self tests and a pairwise check of
 * every key generated. burrow has neither mode, so neither check runs, as they
 * do not in a Go program that is not in FIPS mode.
 *
 * Copyright 2011 The Go Authors. All rights reserved.
 * Copyright 2026 The burrow Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style licence that can be found
 * in the LICENSE file. */

/* burrow:package crypto/ecdsa */

#ifndef BURROW_CRYPTO_ECDSA_H
#define BURROW_CRYPTO_ECDSA_H

#include "burrow/core.h"
#include "burrow/crypto.h"
#include "burrow/crypto/ecdh.h"
#include "burrow/crypto/elliptic.h"
#include "burrow/error.h"
#include "burrow/io.h"
#include "burrow/math/big.h"
#include "burrow/mem.h"
#include "burrow/slice.h"
#include "burrow/type.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ecdsa.PublicKey: a point on a curve. Go deprecates reading and writing X and
 * Y directly, in favour of ecdsa_parse_uncompressed_public_key and
 * ecdsa_public_key_bytes, but they are still how a key is put together by
 * hand. */
typedef struct EcdsaPublicKey {
    EllipticCurve curve;
    BigInt *x, *y;
} EcdsaPublicKey;

extern const Type burrow_type_EcdsaPublicKey;
#define TYPE_ECDSA_PUBLIC_KEY TYPE_OF(EcdsaPublicKey)

/* ecdsa.PrivateKey: a scalar, with the public key it makes. D is deprecated in
 * the same way, for ecdsa_parse_raw_private_key and ecdsa_private_key_bytes. */
typedef struct EcdsaPrivateKey {
    EcdsaPublicKey public_key;
    BigInt *d;
} EcdsaPrivateKey;

extern const Type burrow_type_EcdsaPrivateKey;
#define TYPE_ECDSA_PRIVATE_KEY TYPE_OF(EcdsaPrivateKey)

/* GenerateKey: a new private key on c, from a.
 *
 * For the four NIST curves the scalar comes from the system's generator,
 * whatever rand is, as it does in Go from 1.26, unless GODEBUG has
 * cryptocustomrand=1, when it comes from rand and a nil rand means
 * crypto_rand_reader. Any other curve reads rand, read with io_read_full. An
 * error reading it is returned as it is. */
BURROW_OWNS(ret) EcdsaPrivateKey *ecdsa_generate_key(Alloc *a, EllipticCurve c,
                                                     IoReader rand, Error *err);

/* ParseUncompressedPublicKey: the public key whose point is data, in the
 * uncompressed form of section 4.3.6 of ANSI X9.62, a 4 and then x and y, from
 * a. curve has to be one of the four NIST curves, and the point has to be on
 * it. */
BURROW_OWNS(ret) EcdsaPublicKey *
ecdsa_parse_uncompressed_public_key(Alloc *a, EllipticCurve curve, Slice data,
                                    Error *err);

/* ParseRawPrivateKey: the private key whose scalar is data, big endian and as
 * long as the curve's order, from a. The scalar has to be more than zero and
 * less than the order, and curve one of the four NIST curves. */
BURROW_OWNS(ret) EcdsaPrivateKey *
ecdsa_parse_raw_private_key(Alloc *a, EllipticCurve curve, Slice data, Error *err);

/* Frees a key from ecdsa_generate_key, ecdsa_parse_raw_private_key or
 * ecdsa_parse_uncompressed_public_key, and the integers that came with it.
 * Not for a key built by hand, or for the public key inside a private one.
 * NULL is fine. */
void ecdsa_private_key_free(EcdsaPrivateKey *k);
void ecdsa_public_key_free(EcdsaPublicKey *k);

/* SignASN1: the signature of hash by priv, DER encoded as the SEQUENCE of two
 * INTEGERs that X.509 and TLS use, from a. rand is treated the way
 * ecdsa_generate_key treats it, and a nil rand means the system's generator.
 * An empty hash is "ecdsa: hash cannot be empty". */
BURROW_OWNS(ret) Slice ecdsa_sign_asn1(Alloc *a, IoReader rand,
                                       const EcdsaPrivateKey *priv, Slice hash,
                                       Error *err);

/* VerifyASN1: whether sig is a valid signature of hash by pub, in the encoding
 * ecdsa_sign_asn1 writes. Anything else, an empty hash included, is false. */
bool ecdsa_verify_asn1(const EcdsaPublicKey *pub, Slice hash, Slice sig);

/* Sign: ecdsa_sign_asn1 with the signature as its two integers, r returned and
 * s in *s, both new from a. Go deprecates it for ecdsa_sign_asn1. NULL, with
 * NULL in *s, on an error. */
BURROW_OWNS(ret) BigInt *ecdsa_sign(Alloc *a, IoReader rand,
                                    const EcdsaPrivateKey *priv, Slice hash, BigInt **s,
                                    Error *err);

/* Verify: whether r and s are a valid signature of hash by pub. Go deprecates
 * it for ecdsa_verify_asn1. */
bool ecdsa_verify(const EcdsaPublicKey *pub, Slice hash, const BigInt *r,
                  const BigInt *s);

/* PublicKey.Bytes: the uncompressed encoding of pub's point, from a, which
 * ecdsa_parse_uncompressed_public_key reads. An error for a curve that is not
 * one of the four, a point not on its curve, and infinity. */
BURROW_OWNS(ret) Slice ecdsa_public_key_bytes(const EcdsaPublicKey *pub, Alloc *a,
                                              Error *err);

/* PublicKey.ECDH: pub as a crypto/ecdh key, from a. P-224 is not a curve
 * crypto/ecdh has, so is "ecdsa: unsupported curve by crypto/ecdh", as is any
 * curve of your own. */
BURROW_OWNS(ret) EcdhPublicKey *ecdsa_public_key_ecdh(const EcdsaPublicKey *pub,
                                                      Alloc *a, Error *err);

/* PublicKey.Equal: whether x holds an EcdsaPublicKey with the same curve and
 * the same point. Two curves are the same when their vtables and data are, so
 * a copy of P-256's params is not P-256. Make x with
 * BURROW_ANY(TYPE_ECDSA_PUBLIC_KEY, other). */
bool ecdsa_public_key_equal(const EcdsaPublicKey *pub, CryptoPublicKey x);

/* PrivateKey.Bytes: the scalar as ecdsa_parse_raw_private_key reads it, from
 * a, with the same errors as ecdsa_public_key_bytes and one for a scalar that
 * is zero or too large. */
BURROW_OWNS(ret) Slice ecdsa_private_key_bytes(const EcdsaPrivateKey *priv, Alloc *a,
                                               Error *err);

/* PrivateKey.ECDH: priv as a crypto/ecdh key, from a, on the same curves as
 * ecdsa_public_key_ecdh. */
BURROW_OWNS(ret) EcdhPrivateKey *ecdsa_private_key_ecdh(const EcdsaPrivateKey *priv,
                                                        Alloc *a, Error *err);

/* PrivateKey.Equal: whether x holds an EcdsaPrivateKey with the same public key
 * and scalar, the scalar compared in constant time. */
bool ecdsa_private_key_equal(const EcdsaPrivateKey *priv, CryptoPrivateKey x);

/* PrivateKey.Public: the public key inside priv, as a CryptoPublicKey that
 * holds an EcdsaPublicKey. */
BURROW_BORROWS(ret, priv) CryptoPublicKey
ecdsa_private_key_public(const EcdsaPrivateKey *priv);

/* PrivateKey.Sign: signs digest, from a, in the encoding ecdsa_sign_asn1
 * writes. This is the crypto.Signer method.
 *
 * opts may be nil, a zero CryptoSignerOpts. When it is not, it has to name a
 * hash, and digest has to be as long as that hash's output. With a rand, the
 * signature is ecdsa_sign_asn1's. With a nil rand the signature is the
 * deterministic one of RFC 6979, which needs opts to name the hash, as RFC 6979
 * uses it for the nonce, and needs one of the four NIST curves. */
BURROW_OWNS(ret) Slice ecdsa_private_key_sign(const EcdsaPrivateKey *priv, Alloc *a,
                                              IoReader rand, Slice digest,
                                              CryptoSignerOpts opts, Error *err);

/* priv as a CryptoSigner, which borrows it. */
BURROW_BORROWS(ret, priv) CryptoSigner
ecdsa_private_key_signer(const EcdsaPrivateKey *priv);

/* The methods a PublicKey and a PrivateKey have in Go because they embed an
 * elliptic.Curve, which are the curve's. */
BURROW_BORROWS(ret, k) static inline EllipticCurveParams *
ecdsa_public_key_params(EcdsaPublicKey k) {
    return elliptic_curve_params(k.curve);
}

static inline bool ecdsa_public_key_is_on_curve(EcdsaPublicKey k, const BigInt *x,
                                                const BigInt *y) {
    return elliptic_curve_is_on_curve(k.curve, x, y);
}

BURROW_OWNS(ret) static inline BigInt *
ecdsa_public_key_add(EcdsaPublicKey k, Alloc *a, const BigInt *x1, const BigInt *y1,
                     const BigInt *x2, const BigInt *y2, BigInt **y) {
    return elliptic_curve_add(k.curve, a, x1, y1, x2, y2, y);
}

BURROW_OWNS(ret) static inline BigInt *
ecdsa_public_key_double(EcdsaPublicKey k, Alloc *a, const BigInt *x1, const BigInt *y1,
                        BigInt **y) {
    return elliptic_curve_double(k.curve, a, x1, y1, y);
}

BURROW_OWNS(ret) static inline BigInt *
ecdsa_public_key_scalar_mult(EcdsaPublicKey k, Alloc *a, const BigInt *x1,
                             const BigInt *y1, Slice scalar, BigInt **y) {
    return elliptic_curve_scalar_mult(k.curve, a, x1, y1, scalar, y);
}

BURROW_OWNS(ret) static inline BigInt *
ecdsa_public_key_scalar_base_mult(EcdsaPublicKey k, Alloc *a, Slice scalar,
                                  BigInt **y) {
    return elliptic_curve_scalar_base_mult(k.curve, a, scalar, y);
}

BURROW_BORROWS(ret, k) static inline EllipticCurveParams *
ecdsa_private_key_params(EcdsaPrivateKey k) {
    return elliptic_curve_params(k.public_key.curve);
}

static inline bool ecdsa_private_key_is_on_curve(EcdsaPrivateKey k, const BigInt *x,
                                                 const BigInt *y) {
    return elliptic_curve_is_on_curve(k.public_key.curve, x, y);
}

BURROW_OWNS(ret) static inline BigInt *
ecdsa_private_key_add(EcdsaPrivateKey k, Alloc *a, const BigInt *x1, const BigInt *y1,
                      const BigInt *x2, const BigInt *y2, BigInt **y) {
    return elliptic_curve_add(k.public_key.curve, a, x1, y1, x2, y2, y);
}

BURROW_OWNS(ret) static inline BigInt *
ecdsa_private_key_double(EcdsaPrivateKey k, Alloc *a, const BigInt *x1,
                         const BigInt *y1, BigInt **y) {
    return elliptic_curve_double(k.public_key.curve, a, x1, y1, y);
}

BURROW_OWNS(ret) static inline BigInt *
ecdsa_private_key_scalar_mult(EcdsaPrivateKey k, Alloc *a, const BigInt *x1,
                              const BigInt *y1, Slice scalar, BigInt **y) {
    return elliptic_curve_scalar_mult(k.public_key.curve, a, x1, y1, scalar, y);
}

BURROW_OWNS(ret) static inline BigInt *
ecdsa_private_key_scalar_base_mult(EcdsaPrivateKey k, Alloc *a, Slice scalar,
                                   BigInt **y) {
    return elliptic_curve_scalar_base_mult(k.public_key.curve, a, scalar, y);
}

#ifdef __cplusplus
}
#endif

#endif /* BURROW_CRYPTO_ECDSA_H */
