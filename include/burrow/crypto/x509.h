/* crypto/x509, X.509 certificates and the key formats that go with them.
 *
 * This part has the key formats: PKCS #1 and SEC 1 private keys, PKCS #8 for
 * any of them, PKIX public keys, the old encrypted PEM blocks of RFC 1423, and
 * OIDs. Reading a key the way most programs meet one:
 *
 *     PemBlock *b = pem_decode(a, pem, &rest);
 *     Any key = x509_parse_pkcs8_private_key(a, b->bytes, &err);
 *     if (key.t == TYPE_ECDSA_PRIVATE_KEY) {
 *         EcdsaPrivateKey *k = key.data;
 *         ...
 *     }
 *
 * A key in an Any is the way Go's `any` holds one: the type is the key type,
 * such as TYPE_RSA_PRIVATE_KEY, and the data points at the key. That is the
 * same for Ed25519, whose keys are slices, so the data of an Ed25519 key is a
 * pointer to the Slice.
 *
 * Everything a parse returns is made from the Alloc it is given, in more
 * pieces than are worth freeing one at a time, so give it an arena. The same
 * goes for the errors, which live in the error arena unless they are one of
 * the sentinels.
 *
 * Copyright 2009 The Go Authors. All rights reserved.
 * Copyright 2026 The burrow Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style licence that can be found
 * in the LICENSE file. */

/* burrow:package crypto/x509 */

#ifndef BURROW_CRYPTO_X509_H
#define BURROW_CRYPTO_X509_H

#include "burrow/core.h"
#include "burrow/crypto.h"
#include "burrow/crypto/ecdsa.h"
#include "burrow/crypto/rsa.h"
#include "burrow/encoding/asn1.h"
#include "burrow/encoding/pem.h"
#include "burrow/error.h"
#include "burrow/iface.h"
#include "burrow/io.h"
#include "burrow/mem.h"
#include "burrow/slice.h"
#include "burrow/type.h"

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ------------------------------------------------------------------ enums */

/* SignatureAlgorithm: how a certificate, request or list is signed. */
typedef Int X509SignatureAlgorithm;

#define X509_UNKNOWN_SIGNATURE_ALGORITHM ((X509SignatureAlgorithm)0)
#define X509_MD2_WITH_RSA ((X509SignatureAlgorithm)1)  /* not supported */
#define X509_MD5_WITH_RSA ((X509SignatureAlgorithm)2)  /* for signing only */
#define X509_SHA1_WITH_RSA ((X509SignatureAlgorithm)3) /* see below */
#define X509_SHA256_WITH_RSA ((X509SignatureAlgorithm)4)
#define X509_SHA384_WITH_RSA ((X509SignatureAlgorithm)5)
#define X509_SHA512_WITH_RSA ((X509SignatureAlgorithm)6)
#define X509_DSA_WITH_SHA1 ((X509SignatureAlgorithm)7)   /* not supported */
#define X509_DSA_WITH_SHA256 ((X509SignatureAlgorithm)8) /* not supported */
#define X509_ECDSA_WITH_SHA1 ((X509SignatureAlgorithm)9) /* see below */
#define X509_ECDSA_WITH_SHA256 ((X509SignatureAlgorithm)10)
#define X509_ECDSA_WITH_SHA384 ((X509SignatureAlgorithm)11)
#define X509_ECDSA_WITH_SHA512 ((X509SignatureAlgorithm)12)
#define X509_SHA256_WITH_RSAPSS ((X509SignatureAlgorithm)13)
#define X509_SHA384_WITH_RSAPSS ((X509SignatureAlgorithm)14)
#define X509_SHA512_WITH_RSAPSS ((X509SignatureAlgorithm)15)
#define X509_PURE_ED25519 ((X509SignatureAlgorithm)16)
#define X509_MLDSA44 ((X509SignatureAlgorithm)17)
#define X509_MLDSA65 ((X509SignatureAlgorithm)18)
#define X509_MLDSA87 ((X509SignatureAlgorithm)19)

/* The two SHA-1 algorithms are supported for signing anything and for checking
 * the signatures on revocation lists, requests and OCSP responses, but not on
 * certificates. */

/* SignatureAlgorithm.String: "SHA256-RSA" and so on, or the number in a for
 * one that is not in the list. */
BURROW_OWNS(ret) Str x509_signature_algorithm_string(X509SignatureAlgorithm algo,
                                                     Alloc *a);

/* PublicKeyAlgorithm: the kind of key a certificate is for. DSA keys are read
 * and never written. */
typedef Int X509PublicKeyAlgorithm;

#define X509_UNKNOWN_PUBLIC_KEY_ALGORITHM ((X509PublicKeyAlgorithm)0)
#define X509_RSA ((X509PublicKeyAlgorithm)1)
#define X509_DSA ((X509PublicKeyAlgorithm)2)
#define X509_ECDSA ((X509PublicKeyAlgorithm)3)
#define X509_ED25519 ((X509PublicKeyAlgorithm)4)
#define X509_MLDSA ((X509PublicKeyAlgorithm)5)

/* PublicKeyAlgorithm.String: "RSA", "DSA", "ECDSA", "Ed25519" or "ML-DSA", or
 * the number in a for anything else. */
BURROW_OWNS(ret) Str x509_public_key_algorithm_string(X509PublicKeyAlgorithm algo,
                                                      Alloc *a);

/* KeyUsage: what a key is for, as a set of bits. */
typedef Int X509KeyUsage;

#define X509_KEY_USAGE_DIGITAL_SIGNATURE ((X509KeyUsage)1)
#define X509_KEY_USAGE_CONTENT_COMMITMENT ((X509KeyUsage)2)
#define X509_KEY_USAGE_KEY_ENCIPHERMENT ((X509KeyUsage)4)
#define X509_KEY_USAGE_DATA_ENCIPHERMENT ((X509KeyUsage)8)
#define X509_KEY_USAGE_KEY_AGREEMENT ((X509KeyUsage)16)
#define X509_KEY_USAGE_CERT_SIGN ((X509KeyUsage)32)
#define X509_KEY_USAGE_CRL_SIGN ((X509KeyUsage)64)
#define X509_KEY_USAGE_ENCIPHER_ONLY ((X509KeyUsage)128)
#define X509_KEY_USAGE_DECIPHER_ONLY ((X509KeyUsage)256)

/* KeyUsage.String: the name RFC 5280 gives a single bit, such as
 * "digitalSignature" or "cRLSign", and "KeyUsage(3)" from a for anything that
 * is not exactly one of them. */
BURROW_OWNS(ret) Str x509_key_usage_string(X509KeyUsage i, Alloc *a);

/* ExtKeyUsage: one extended purpose a key is for. */
typedef Int X509ExtKeyUsage;

#define X509_EXT_KEY_USAGE_ANY ((X509ExtKeyUsage)0)
#define X509_EXT_KEY_USAGE_SERVER_AUTH ((X509ExtKeyUsage)1)
#define X509_EXT_KEY_USAGE_CLIENT_AUTH ((X509ExtKeyUsage)2)
#define X509_EXT_KEY_USAGE_CODE_SIGNING ((X509ExtKeyUsage)3)
#define X509_EXT_KEY_USAGE_EMAIL_PROTECTION ((X509ExtKeyUsage)4)
#define X509_EXT_KEY_USAGE_IPSEC_END_SYSTEM ((X509ExtKeyUsage)5)
#define X509_EXT_KEY_USAGE_IPSEC_TUNNEL ((X509ExtKeyUsage)6)
#define X509_EXT_KEY_USAGE_IPSEC_USER ((X509ExtKeyUsage)7)
#define X509_EXT_KEY_USAGE_TIME_STAMPING ((X509ExtKeyUsage)8)
#define X509_EXT_KEY_USAGE_OCSP_SIGNING ((X509ExtKeyUsage)9)
#define X509_EXT_KEY_USAGE_MICROSOFT_SERVER_GATED_CRYPTO ((X509ExtKeyUsage)10)
#define X509_EXT_KEY_USAGE_NETSCAPE_SERVER_GATED_CRYPTO ((X509ExtKeyUsage)11)
#define X509_EXT_KEY_USAGE_MICROSOFT_COMMERCIAL_CODE_SIGNING ((X509ExtKeyUsage)12)
#define X509_EXT_KEY_USAGE_MICROSOFT_KERNEL_CODE_SIGNING ((X509ExtKeyUsage)13)

/* ExtKeyUsage.String: the short name, such as "serverAuth" or "msSGC", and
 * "ExtKeyUsage(14)" from a for a number that is not one. */
BURROW_OWNS(ret) Str x509_ext_key_usage_string(X509ExtKeyUsage i, Alloc *a);

/* PEMCipher: the cipher x509_encrypt_pem_block encrypts with. */
typedef Int X509PEMCipher;

#define X509_PEM_CIPHER_DES ((X509PEMCipher)1)
#define X509_PEM_CIPHER3_DES ((X509PEMCipher)2)
#define X509_PEM_CIPHER_AES128 ((X509PEMCipher)3)
#define X509_PEM_CIPHER_AES192 ((X509PEMCipher)4)
#define X509_PEM_CIPHER_AES256 ((X509PEMCipher)5)

/* -------------------------------------------------------------------- OID */

/* OID: an ASN.1 object identifier, kept as its DER contents, which is what
 * lets it hold arcs too big for an Asn1ObjectIdentifier. The zero OID is
 * empty, and its string is "". */
typedef struct X509OID {
    Slice der;
} X509OID;

extern const Type burrow_type_X509OID;
#define TYPE_X509_OID TYPE_OF(X509OID)

/* ParseOID: the OID written in dotted decimal, such as "1.2.840.113549", from
 * a. The arcs can be any size. Anything that is not at least two arcs of
 * digits, or whose first two arcs are not a valid start, is "invalid oid". */
BURROW_OWNS(ret) X509OID x509_parse_oid(Alloc *a, Str oid, Error *err);

/* OIDFromInts: the OID with the arcs in oid, a slice of uint64_t, from a. */
BURROW_OWNS(ret) X509OID x509_oid_from_ints(Alloc *a, Slice oid, Error *err);

/* OIDFromASN1OID: the OID with the same arcs as asn1_oid, from a. A negative
 * arc is an error. */
BURROW_OWNS(ret) X509OID x509_oid_from_asn1_oid(Alloc *a, Asn1ObjectIdentifier asn1_oid,
                                                Error *err);

/* OID.Equal: whether the two are the same OID. */
bool x509_oid_equal(X509OID oid, X509OID other);

/* OID.EqualASN1OID: whether oid has the arcs in other. An oid with an arc
 * that does not fit in an Int is never equal to one. */
bool x509_oid_equal_asn1_oid(X509OID oid, Asn1ObjectIdentifier other);

/* OID.String: oid in dotted decimal, from a. */
BURROW_OWNS(ret) Str x509_oid_string(X509OID oid, Alloc *a);

/* OID.AppendText, OID.MarshalText: the string of o, appended to b or on its
 * own, from a. Neither fails, but they keep Go's error to fit the
 * encoding.TextMarshaler shape. */
BURROW_OWNS(ret) BURROW_BORROWS(ret, b) Slice x509_oid_append_text(X509OID o, Alloc *a,
                                                                   Slice b, Error *err);
BURROW_OWNS(ret) Slice x509_oid_marshal_text(X509OID o, Alloc *a, Error *err);

/* OID.UnmarshalText: x509_parse_oid into *o. On an error *o is left as it
 * was. */
BURROW_STATIC(ret) Error x509_oid_unmarshal_text(X509OID *o, Alloc *a, Slice text);

/* OID.AppendBinary, OID.MarshalBinary: the DER contents of o, appended to b or
 * copied, from a. */
BURROW_OWNS(ret) BURROW_BORROWS(ret, b) Slice x509_oid_append_binary(X509OID o,
                                                                     Alloc *a, Slice b,
                                                                     Error *err);
BURROW_OWNS(ret) Slice x509_oid_marshal_binary(X509OID o, Alloc *a, Error *err);

/* OID.UnmarshalBinary: a copy of b in a, if b is the DER contents of an OID,
 * into *o. On an error *o is left as it was. */
BURROW_STATIC(ret) Error x509_oid_unmarshal_binary(X509OID *o, Alloc *a, Slice b);

/* ExtKeyUsage.OID: the OID of eku, from a. Panics for a value that is not one
 * of the constants above, as Go does. */
BURROW_OWNS(ret) X509OID x509_ext_key_usage_oid(X509ExtKeyUsage eku, Alloc *a);

/* ------------------------------------------------------------------- keys */

/* ParsePKCS1PrivateKey: the RSA private key in der, which is PKCS #1 ASN.1, the
 * "RSA PRIVATE KEY" of a PEM block. The key is precomputed and validated. If
 * der is a SEC 1 or PKCS #8 key the error says which function to use. With
 * GODEBUG x509rsacrt=0 a key whose CRT values do not check out is tried again
 * without them, as Go does. */
BURROW_OWNS(ret) RsaPrivateKey *x509_parse_pkcs1_private_key(Alloc *a, Slice der,
                                                             Error *err);

/* MarshalPKCS1PrivateKey: key in PKCS #1 form, from a. Precomputes key first,
 * which is why it is not const. */
BURROW_OWNS(ret) Slice x509_marshal_pkcs1_private_key(Alloc *a, RsaPrivateKey *key);

/* ParsePKCS1PublicKey: the RSA public key in der, PKCS #1 ASN.1, the "RSA
 * PUBLIC KEY" of a PEM block. */
BURROW_OWNS(ret) RsaPublicKey *x509_parse_pkcs1_public_key(Alloc *a, Slice der,
                                                           Error *err);

/* MarshalPKCS1PublicKey: key in PKCS #1 form, from a. */
BURROW_OWNS(ret) Slice x509_marshal_pkcs1_public_key(Alloc *a, const RsaPublicKey *key);

/* ParseECPrivateKey: the EC private key in der, the SEC 1 form of RFC 5915,
 * the "EC PRIVATE KEY" of a PEM block. The curve has to be one of the four
 * NIST curves. */
BURROW_OWNS(ret) EcdsaPrivateKey *x509_parse_ec_private_key(Alloc *a, Slice der,
                                                            Error *err);

/* MarshalECPrivateKey: key in SEC 1 form, from a. Only the four NIST curves
 * have an OID to write. */
BURROW_OWNS(ret) Slice x509_marshal_ec_private_key(Alloc *a, const EcdsaPrivateKey *key,
                                                   Error *err);

/* ParsePKCS8PrivateKey: the private key in der, an unencrypted PKCS #8 key,
 * the "PRIVATE KEY" of a PEM block. The Any holds an RsaPrivateKey, an
 * EcdsaPrivateKey, an Ed25519PrivateKey, an MldsaPrivateKey or an
 * EcdhPrivateKey for X25519. An ML-DSA key has to be the seed alone, which is
 * what the error for the other two forms says. */
BURROW_OWNS(ret) Any x509_parse_pkcs8_private_key(Alloc *a, Slice der, Error *err);

/* MarshalPKCS8PrivateKey: key, any of the types the parse gives back, in PKCS
 * #8 form, from a. An RSA key is precomputed and validated first. An ECDH key
 * on a NIST curve is written as an EC key with that curve. */
BURROW_OWNS(ret) Slice x509_marshal_pkcs8_private_key(Alloc *a, Any key, Error *err);

/* ParsePKIXPublicKey: the public key in der, a SubjectPublicKeyInfo, the
 * "PUBLIC KEY" of a PEM block. The Any holds an RsaPublicKey, a DsaPublicKey,
 * an EcdsaPublicKey, an Ed25519PublicKey, an MldsaPublicKey or an
 * EcdhPublicKey for X25519. */
BURROW_OWNS(ret) Any x509_parse_pkix_public_key(Alloc *a, Slice der, Error *err);

/* MarshalPKIXPublicKey: pub in SubjectPublicKeyInfo form, from a. pub can be
 * any of the types above but DSA, and an EcdhPublicKey on any of its four
 * curves. */
BURROW_OWNS(ret) Slice x509_marshal_pkix_public_key(Alloc *a, Any pub, Error *err);

/* ------------------------------------------------------- encrypted PEM */

/* IncorrectPasswordError: what x509_decrypt_pem_block says when the padding
 * comes out wrong, which is usually the password. */
extern const Error x509_incorrect_password_error;

/* IsEncryptedPEMBlock: whether b has a DEK-Info header, which is what marks it
 * as encrypted in the RFC 1423 way. Deprecated in Go, because that encryption
 * is insecure by design. */
bool x509_is_encrypted_pem_block(const PemBlock *b);

/* DecryptPEMBlock: the bytes of b decrypted with password, from a. Deprecated
 * in Go for the same reason, and because a wrong password is not always
 * caught: about one in 256 come out as garbage with no error. */
BURROW_OWNS(ret) Slice x509_decrypt_pem_block(Alloc *a, const PemBlock *b,
                                              Slice password, Error *err);

/* EncryptPEMBlock: a block of type block_type holding data encrypted with alg
 * and a key from password, with the headers that say so, all from a. The IV
 * comes from rand, and a zero IoReader means crypto_rand_reader. Deprecated in
 * Go, as above. */
BURROW_OWNS(ret) PemBlock *x509_encrypt_pem_block(Alloc *a, IoReader rand,
                                                  Str block_type, Slice data,
                                                  Slice password, X509PEMCipher alg,
                                                  Error *err);

#ifdef __cplusplus
}
#endif

#endif
