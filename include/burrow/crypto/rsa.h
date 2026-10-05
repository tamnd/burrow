/* crypto/rsa, RSA encryption and signatures as PKCS #1 and RFC 8017 define
 * them.
 *
 *     Arena ar;
 *     arena_init(&ar, NULL, 0);
 *     Alloc *a = arena_allocator(&ar);
 *     Error err = BURROW_NO_ERROR;
 *     RsaPrivateKey *k = rsa_generate_key(a, (IoReader){0}, 2048, &err);
 *     Slice sig = rsa_sign_pss(a, (IoReader){0}, k, CRYPTO_SHA256, digest, NULL, &err);
 *     Error verr = rsa_verify_pss(&k->public_key, CRYPTO_SHA256, digest, sig, NULL);
 *     arena_free(&ar);
 *
 * There are two schemes of each kind. For encryption, OAEP is the one to use in
 * new protocols and PKCS #1 v1.5 is there for the old ones. For signatures,
 * PSS is the newer one and PKCS #1 v1.5 is what most of the world still uses.
 * Encrypting with RSA is rarely the right thing to do. A key exchange like
 * crypto/ecdh or crypto/mlkem is better in nearly every case.
 *
 * Keys of fewer than 1024 bits are insecure, and every function that takes a
 * key gives an error for one, as Go 1.24 and later do. GODEBUG=rsa1024min=0
 * turns that off, which only tests should do. GenerateKey makes keys of any
 * size from 32 bits up when that is set.
 *
 * The keys are structs of BigInt pointers, as Go's are, so a key can be put
 * together by hand. A private key built that way should go through
 * rsa_private_key_precompute before it is used, which checks it and works out
 * the values that make signing and decrypting faster, or every operation does
 * that work again. Every integer this package makes is a new BigInt from the
 * Alloc given, the struct and its words, and an arena is the easy way to look
 * after them. rsa_private_key_free gives back a key rsa_generate_key made.
 *
 * Everything that uses the private key is constant time, as is PKCS #1 v1.5
 * and OAEP decryption. Go's FIPS 140 self test and the pairwise check of a new
 * key only run in FIPS mode, which burrow does not have, so neither runs here.
 *
 * Copyright 2009 The Go Authors. All rights reserved.
 * Copyright 2026 The burrow Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style licence that can be found
 * in the LICENSE file. */

/* burrow:package crypto/rsa */

#ifndef BURROW_CRYPTO_RSA_H
#define BURROW_CRYPTO_RSA_H

#include "burrow/core.h"
#include "burrow/crypto.h"
#include "burrow/error.h"
#include "burrow/hash.h"
#include "burrow/io.h"
#include "burrow/math/big.h"
#include "burrow/mem.h"
#include "burrow/slice.h"
#include "burrow/type.h"

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* PSSSaltLengthAuto: sign with the longest salt that fits, and work out the
 * length from the signature when verifying. */
#define RSA_PSS_SALT_LENGTH_AUTO 0

/* PSSSaltLengthEqualsHash: a salt as long as the hash. */
#define RSA_PSS_SALT_LENGTH_EQUALS_HASH (-1)

/* ErrMessageTooLong: a message too long for the size of the key. */
extern const Error rsa_err_message_too_long;

/* ErrDecryption: a message that would not decrypt. It says nothing more on
 * purpose, so that it cannot be used to learn about the key. */
extern const Error rsa_err_decryption;

/* ErrVerification: a signature that does not verify. */
extern const Error rsa_err_verification;

/* PublicKey: an RSA public key, the modulus n and the public exponent e. */
typedef struct RsaPublicKey {
    BigInt *n;
    Int e;
} RsaPublicKey;

extern const Type burrow_type_RsaPublicKey;
#define TYPE_RSA_PUBLIC_KEY TYPE_OF(RsaPublicKey)

/* CRTValue: what the Chinese remainder theorem needs for each prime after the
 * first two of a multi-prime key. Deprecated in Go, where nothing uses them. */
typedef struct RsaCRTValue {
    BigInt *exp;   /* D mod (prime-1) */
    BigInt *coeff; /* R·Coeff ≡ 1 mod Prime */
    BigInt *r;     /* the product of the primes before this one, p and q too */
} RsaCRTValue;

/* The values a private key keeps that make it fast, in the form the code
 * underneath wants. Only rsa_private_key_precompute and rsa_generate_key make
 * one. */
typedef struct RsaFipsKey RsaFipsKey;

/* PrecomputedValues: values worked out from the key that make operations
 * faster. dp is D mod (P-1), dq is D mod (Q-1) and qinv is Q⁻¹ mod P.
 * crt_values is only for keys of more than two primes and is deprecated. */
typedef struct RsaPrecomputedValues {
    BigInt *dp, *dq, *qinv;
    RsaCRTValue *crt_values;
    Int crt_values_len;
    RsaFipsKey *fips;
} RsaPrecomputedValues;

/* PrivateKey: an RSA private key. d is the private exponent and primes the
 * prime factors of n, at least two of them. Keys of more than two primes are
 * deprecated, and are slower because the values in crt_values are not used. */
typedef struct RsaPrivateKey {
    RsaPublicKey public_key;
    BigInt *d;
    BigInt **primes;
    Int primes_len;
    RsaPrecomputedValues precomputed;
} RsaPrivateKey;

extern const Type burrow_type_RsaPrivateKey;
#define TYPE_RSA_PRIVATE_KEY TYPE_OF(RsaPrivateKey)

/* PSSOptions: how to make or check a PSS signature. salt_length is a number
 * of bytes or RSA_PSS_SALT_LENGTH_AUTO or RSA_PSS_SALT_LENGTH_EQUALS_HASH.
 * hash, when it is not zero, is used instead of the hash given to the
 * function. */
typedef struct RsaPSSOptions {
    Int salt_length;
    CryptoHash hash;
} RsaPSSOptions;

extern const Type burrow_type_RsaPSSOptions;
#define TYPE_RSA_PSS_OPTIONS TYPE_OF(RsaPSSOptions)

/* PSSOptions.HashFunc: opts->hash. */
CryptoHash rsa_pss_options_hash_func(const RsaPSSOptions *opts);

/* opts as a CryptoSignerOpts, which makes rsa_private_key_sign sign with PSS.
 * It points at opts, so opts has to outlive it. */
BURROW_BORROWS(ret, opts) CryptoSignerOpts
rsa_pss_options_as_signer_opts(const RsaPSSOptions *opts);

/* OAEPOptions: what rsa_private_key_decrypt takes to decrypt with OAEP. hash
 * is the hash of the label and of the mask, unless mgf_hash is not zero, when
 * the mask uses that instead. label has to be the one the message was
 * encrypted with. */
typedef struct RsaOAEPOptions {
    CryptoHash hash;
    CryptoHash mgf_hash;
    Slice label;
} RsaOAEPOptions;

extern const Type burrow_type_RsaOAEPOptions;
#define TYPE_RSA_OAEP_OPTIONS TYPE_OF(RsaOAEPOptions)

/* opts as a CryptoDecrypterOpts. It points at opts, so opts has to outlive
 * it. */
BURROW_BORROWS(ret, opts) CryptoDecrypterOpts
rsa_oaep_options_as_decrypter_opts(const RsaOAEPOptions *opts);

/* PKCS1v15DecryptOptions: what rsa_private_key_decrypt takes to decrypt with
 * PKCS #1 v1.5. A session_key_len above zero makes it decrypt the way
 * rsa_decrypt_pkcs1_v15_session_key does, giving random bytes of that length
 * when the message does not decrypt, rather than an error. */
typedef struct RsaPKCS1v15DecryptOptions {
    Int session_key_len;
} RsaPKCS1v15DecryptOptions;

extern const Type burrow_type_RsaPKCS1v15DecryptOptions;
#define TYPE_RSA_PKCS1V15_DECRYPT_OPTIONS TYPE_OF(RsaPKCS1v15DecryptOptions)

/* opts as a CryptoDecrypterOpts. It points at opts, so opts has to outlive
 * it. */
BURROW_BORROWS(ret, opts) CryptoDecrypterOpts
rsa_pkcs1_v15_decrypt_options_as_decrypter_opts(const RsaPKCS1v15DecryptOptions *opts);

/* ------------------------------------------------------------- public keys */

/* PublicKey.Size: the size of the modulus in bytes, which is also the size of
 * every signature and ciphertext. */
Int rsa_public_key_size(const RsaPublicKey *pub);

/* PublicKey.Equal: whether x is an RsaPublicKey with the same n and e. */
bool rsa_public_key_equal(const RsaPublicKey *pub, CryptoPublicKey x);

/* ------------------------------------------------------------ private keys */

/* GenerateKey: a new key of bits bits, from a, with primes from random, or
 * from the system's generator when it is nil. Since Go 1.26 random is ignored
 * and the system's generator is always used, unless GODEBUG has
 * cryptocustomrand=1. Fewer than 1024 bits is an error unless GODEBUG has
 * rsa1024min=0. */
BURROW_OWNS(ret) RsaPrivateKey *rsa_generate_key(Alloc *a, IoReader random, Int bits,
                                                 Error *err);

/* GenerateMultiPrimeKey: a key of nprimes primes. Deprecated in Go: such keys
 * are slower and less secure than they look. nprimes of 2 is GenerateKey. */
BURROW_OWNS(ret) RsaPrivateKey *rsa_generate_multi_prime_key(Alloc *a, IoReader random,
                                                             Int nprimes, Int bits,
                                                             Error *err);

/* Gives back a key from rsa_generate_key or rsa_generate_multi_prime_key, and
 * every integer it points at, all of which must have come from a, clearing the
 * secret ones first. NULL is fine. */
void rsa_private_key_free(RsaPrivateKey *priv, Alloc *a);

/* PrivateKey.Size: the size of priv's modulus in bytes. */
Int rsa_private_key_size(const RsaPrivateKey *priv);

/* PrivateKey.Public: the public half of priv, which lives inside it. */
BURROW_BORROWS(ret, priv) CryptoPublicKey
rsa_private_key_public(const RsaPrivateKey *priv);

/* PrivateKey.Equal: whether x is an RsaPrivateKey with the same public key,
 * private exponent and primes. The precomputed values are not compared. */
bool rsa_private_key_equal(const RsaPrivateKey *priv, CryptoPrivateKey x);

/* PrivateKey.Validate: no error when priv looks sane, a key whose numbers
 * agree with each other. It does not test that the primes are prime. */
BURROW_STATIC(ret) Error rsa_private_key_validate(const RsaPrivateKey *priv);

/* PrivateKey.Precompute: checks priv and works out its precomputed values,
 * from a, if they are not there yet. A key that fails the checks is left
 * without them, and the functions that use it give the error. Values it
 * replaces are not freed, since a copy of the key may still point at them. */
void rsa_private_key_precompute(RsaPrivateKey *priv, Alloc *a);

/* PrivateKey.Sign: digest signed with priv, from a. With PSS when opts is an
 * rsa_pss_options_as_signer_opts and with PKCS #1 v1.5 otherwise, with the
 * hash opts names. A nil opts panics, as it does in Go. */
BURROW_OWNS(ret) Slice rsa_private_key_sign(const RsaPrivateKey *priv, Alloc *a,
                                            IoReader rand, Slice digest,
                                            CryptoSignerOpts opts, Error *err);

/* PrivateKey.Decrypt: ciphertext decrypted with priv, from a. A nil opts, an
 * Any with no type, is PKCS #1 v1.5, and so is an
 * rsa_pkcs1_v15_decrypt_options_as_decrypter_opts. An
 * rsa_oaep_options_as_decrypter_opts is OAEP. Anything else is "crypto/rsa:
 * invalid options for Decrypt". */
BURROW_OWNS(ret) Slice rsa_private_key_decrypt(const RsaPrivateKey *priv, Alloc *a,
                                               IoReader rand, Slice ciphertext,
                                               CryptoDecrypterOpts opts, Error *err);

/* priv as a CryptoSigner and a CryptoDecrypter, which borrow it. */
BURROW_BORROWS(ret, priv) CryptoSigner
rsa_private_key_signer(const RsaPrivateKey *priv);
BURROW_BORROWS(ret, priv) CryptoDecrypter
rsa_private_key_decrypter(const RsaPrivateKey *priv);

/* --------------------------------------------------------------------- PSS */

/* SignPSS: a PSS signature of digest, the output of hash, from a, with the
 * salt read from rand. opts can be NULL, the same as a salt length of
 * RSA_PSS_SALT_LENGTH_AUTO. */
BURROW_OWNS(ret) Slice rsa_sign_pss(Alloc *a, IoReader rand, const RsaPrivateKey *priv,
                                    CryptoHash hash, Slice digest,
                                    const RsaPSSOptions *opts, Error *err);

/* VerifyPSS: no error when sig is a PSS signature of digest by pub. opts can
 * be NULL. The hash in opts is not used here. */
BURROW_STATIC(ret) Error rsa_verify_pss(const RsaPublicKey *pub, CryptoHash hash,
                                        Slice digest, Slice sig,
                                        const RsaPSSOptions *opts);

/* -------------------------------------------------------------------- OAEP */

/* EncryptOAEP: msg encrypted with OAEP for pub, from a. hash is used for the
 * label and the mask, and random gives the seed. label can be empty, and has
 * to be given again to decrypt. msg can be at most rsa_public_key_size(pub) -
 * 2*hash size - 2 bytes. */
BURROW_OWNS(ret) Slice rsa_encrypt_oaep(Alloc *a, Hash hash, IoReader random,
                                        const RsaPublicKey *pub, Slice msg, Slice label,
                                        Error *err);

/* EncryptOAEPWithOptions: the same with the hashes and label in opts, so that
 * the mask can use a different hash. */
BURROW_OWNS(ret) Slice rsa_encrypt_oaep_with_options(Alloc *a, IoReader random,
                                                     const RsaPublicKey *pub, Slice msg,
                                                     const RsaOAEPOptions *opts,
                                                     Error *err);

/* DecryptOAEP: ciphertext decrypted with OAEP, from a. random is not used. */
BURROW_OWNS(ret) Slice rsa_decrypt_oaep(Alloc *a, Hash hash, IoReader random,
                                        const RsaPrivateKey *priv, Slice ciphertext,
                                        Slice label, Error *err);

/* --------------------------------------------------------- PKCS #1 v1.5 */

/* SignPKCS1v15: a PKCS #1 v1.5 signature of hashed, the output of hash, from
 * a. A hash of zero signs hashed as it is, which is only for TLS 1.1 and
 * earlier. random is not used. */
BURROW_OWNS(ret) Slice rsa_sign_pkcs1_v15(Alloc *a, IoReader random,
                                          const RsaPrivateKey *priv, CryptoHash hash,
                                          Slice hashed, Error *err);

/* VerifyPKCS1v15: no error when sig is a PKCS #1 v1.5 signature of hashed by
 * pub. */
BURROW_STATIC(ret) Error rsa_verify_pkcs1_v15(const RsaPublicKey *pub, CryptoHash hash,
                                              Slice hashed, Slice sig);

/* EncryptPKCS1v15: msg encrypted with PKCS #1 v1.5 for pub, from a, with the
 * padding read from random. msg can be at most rsa_public_key_size(pub) - 11
 * bytes. Deprecated in Go, for OAEP. */
BURROW_OWNS(ret) Slice rsa_encrypt_pkcs1_v15(Alloc *a, IoReader random,
                                             const RsaPublicKey *pub, Slice msg,
                                             Error *err);

/* DecryptPKCS1v15: ciphertext decrypted with PKCS #1 v1.5, from a. random is
 * not used. Whether it fails can tell an attacker about the key, which is
 * Bleichenbacher's attack, so a protocol should use
 * rsa_decrypt_pkcs1_v15_session_key instead. Deprecated in Go. */
BURROW_OWNS(ret) Slice rsa_decrypt_pkcs1_v15(Alloc *a, IoReader random,
                                             const RsaPrivateKey *priv,
                                             Slice ciphertext, Error *err);

/* DecryptPKCS1v15SessionKey: decrypts a session key into key, in constant
 * time, leaving key as it was when the message does not decrypt or is not as
 * long as key. Fill key with random bytes first, so that the protocol goes on
 * with a key nobody knows and fails later. It is an error only when the key is
 * too small for a session key that long. Deprecated in Go. */
BURROW_STATIC(ret) Error rsa_decrypt_pkcs1_v15_session_key(IoReader random,
                                                           const RsaPrivateKey *priv,
                                                           Slice ciphertext, Slice key);

#ifdef __cplusplus
}
#endif

#endif /* BURROW_CRYPTO_RSA_H */
