/* crypto/x509, X.509 certificates and the key formats that go with them.
 *
 * This part has the key formats: PKCS #1 and SEC 1 private keys, PKCS #8 for
 * any of them, PKIX public keys, the old encrypted PEM blocks of RFC 1423, and
 * OIDs. It also parses certificates, certificate requests and revocation
 * lists, checks the signatures on them, and makes new ones of each. Reading a
 * key the way most programs meet one:
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
#include "burrow/crypto/x509/pkix.h"
#include "burrow/declare.h"
#include "burrow/encoding/asn1.h"
#include "burrow/encoding/pem.h"
#include "burrow/error.h"
#include "burrow/func.h"
#include "burrow/iface.h"
#include "burrow/io.h"
#include "burrow/math/big.h"
#include "burrow/mem.h"
#include "burrow/net.h"
#include "burrow/net/url.h"
#include "burrow/slice.h"
#include "burrow/time.h"
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

/* ----------------------------------------------------------- certificates */

/* PolicyMapping: one entry of a policyMappings extension, a policy of the
 * issuer and the policy of the subject it counts as. */
typedef struct X509PolicyMapping {
    X509OID issuer_domain_policy;
    X509OID subject_domain_policy;
} X509PolicyMapping;

extern const Type burrow_type_X509PolicyMapping;
#define TYPE_X509_POLICY_MAPPING TYPE_OF(X509PolicyMapping)

/* The element types of the slices of *url.URL and *net.IPNet in a
 * certificate. */
BURROW_PTR_TYPE_DECL(X509URLPtr, Url);
BURROW_PTR_TYPE_DECL(X509IPNetPtr, NetIPNet);
#define TYPE_X509_URL_PTR TYPE_OF(X509URLPtr)
#define TYPE_X509_IP_NET_PTR TYPE_OF(X509IPNetPtr)

/* Certificate: an X.509 v1, v2 or v3 certificate, field for field the same as
 * Go's. The raw fields point into the DER it was parsed from, so keep that
 * around for as long as the certificate.
 *
 * The slices hold, in order: extensions and extra_extensions PkixExtension;
 * unhandled_critical_extensions, unknown_ext_key_usage and policy_identifiers
 * Asn1ObjectIdentifier; ext_key_usage X509ExtKeyUsage; ip_addresses NetIP;
 * uris Url pointers; permitted_ip_ranges and excluded_ip_ranges NetIPNet
 * pointers; policies X509OID; policy_mappings X509PolicyMapping; and the rest,
 * which are names, Str.
 *
 * public_key holds what x509_parse_pkix_public_key would give, and is empty
 * when public_key_algorithm is X509_UNKNOWN_PUBLIC_KEY_ALGORITHM.
 *
 * max_path_len, inhibit_any_policy, inhibit_policy_mapping and
 * require_explicit_policy are -1 or 0 when they are not set, and their _zero
 * fields tell a real zero from an absent one, as in Go. */
typedef struct X509Certificate {
    Slice raw;                         /* the whole certificate */
    Slice raw_tbs_certificate;         /* the part that is signed */
    Slice raw_subject_public_key_info; /* the SubjectPublicKeyInfo */
    Slice raw_subject;                 /* the subject's DER */
    Slice raw_issuer;                  /* the issuer's DER */
    Slice raw_signature_algorithm;     /* the AlgorithmIdentifier */

    Slice signature;
    X509SignatureAlgorithm signature_algorithm;

    X509PublicKeyAlgorithm public_key_algorithm;
    Any public_key;

    Int version;
    BigInt *serial_number;
    PkixName issuer;
    PkixName subject;
    Time not_before, not_after;
    X509KeyUsage key_usage;

    Slice extensions;
    Slice extra_extensions;
    Slice unhandled_critical_extensions;

    Slice ext_key_usage;
    Slice unknown_ext_key_usage;

    bool basic_constraints_valid;
    bool is_ca;
    Int max_path_len;
    bool max_path_len_zero;

    Slice subject_key_id;
    Slice authority_key_id;

    Slice ocsp_server;
    Slice issuing_certificate_url;

    Slice dns_names;
    Slice email_addresses;
    Slice ip_addresses;
    Slice uris;

    bool permitted_dns_domains_critical;
    Slice permitted_dns_domains;
    Slice excluded_dns_domains;
    Slice permitted_ip_ranges;
    Slice excluded_ip_ranges;
    Slice permitted_email_addresses;
    Slice excluded_email_addresses;
    Slice permitted_uri_domains;
    Slice excluded_uri_domains;

    Slice crl_distribution_points;

    Slice policy_identifiers;
    Slice policies;

    Int inhibit_any_policy;
    bool inhibit_any_policy_zero;
    Int inhibit_policy_mapping;
    bool inhibit_policy_mapping_zero;
    Int require_explicit_policy;
    bool require_explicit_policy_zero;

    Slice policy_mappings;
} X509Certificate;

extern const Type burrow_type_X509Certificate;
#define TYPE_X509_CERTIFICATE TYPE_OF(X509Certificate)

BURROW_PTR_TYPE_DECL(X509CertificatePtr, X509Certificate);
#define TYPE_X509_CERTIFICATE_PTR TYPE_OF(X509CertificatePtr)

/* ParseCertificate: the certificate in der, from a. Anything after it is
 * "x509: trailing data". A negative serial number is an error unless GODEBUG
 * has x509negativeserial=1. */
BURROW_OWNS(ret) X509Certificate *x509_parse_certificate(Alloc *a, Slice der,
                                                         Error *err);

/* ParseCertificates: the certificates in der, one after another with nothing
 * between them, as a slice of X509Certificate pointers from a. */
BURROW_OWNS(ret) Slice x509_parse_certificates(Alloc *a, Slice der, Error *err);

/* Certificate.Equal: whether c and other have the same raw bytes. Two NULLs
 * are equal and a NULL is not equal to anything else. */
bool x509_certificate_equal(const X509Certificate *c, const X509Certificate *other);

/* Certificate.CheckSignatureFrom: no error when the signature on c is a valid
 * one from parent. Only the basic constraints and key usage of parent are
 * looked at, and parent has to be allowed to sign certificates.
 * x509_constraint_violation_error when it is not. SHA-1 signatures are not
 * accepted. */
BURROW_STATIC(ret) Error x509_certificate_check_signature_from(
    const X509Certificate *c, const X509Certificate *parent);

/* Certificate.CheckSignature: no error when signature is a valid signature of
 * signed by c's public key with algo. MD5 is an X509InsecureAlgorithmError and
 * SHA-1 is accepted. */
BURROW_STATIC(ret) Error x509_certificate_check_signature(const X509Certificate *c,
                                                          X509SignatureAlgorithm algo,
                                                          Slice signed_data,
                                                          Slice signature);

/* Certificate.CheckCRLSignature: no error when crl is signed by c. Deprecated
 * in Go in favour of x509_revocation_list_check_signature_from. */
BURROW_STATIC(ret) Error x509_certificate_check_crl_signature(
    const X509Certificate *c, const PkixCertificateList *crl);

/* ------------------------------------------------------------------ errors */

/* ErrUnsupportedAlgorithm: the signature uses an algorithm this cannot check,
 * "x509: cannot verify signature: algorithm unimplemented". */
extern const Error x509_err_unsupported_algorithm;

/* InsecureAlgorithmError: a signature with an algorithm that is not safe to
 * trust, such as MD5. errors_as with TYPE_X509_INSECURE_ALGORITHM_ERROR gives a
 * pointer to the algorithm. */
typedef X509SignatureAlgorithm X509InsecureAlgorithmError;

extern const Type *const TYPE_X509_INSECURE_ALGORITHM_ERROR;

/* InsecureAlgorithmError.Error: "x509: cannot verify signature: insecure
 * algorithm SHA1-RSA" and so on, from a. */
BURROW_OWNS(ret) Str x509_insecure_algorithm_error_error(X509InsecureAlgorithmError e,
                                                         Alloc *a);

/* e as an Error, from a. */
BURROW_OWNS(ret) Error
x509_insecure_algorithm_error_as_error(X509InsecureAlgorithmError e, Alloc *a);

/* ConstraintViolationError: the parent of a certificate or list is not allowed
 * to sign it. Go's is an empty struct, and C's cannot be, so the byte in it
 * means nothing. x509_constraint_violation_error is the value as an Error, and
 * errors_as finds it with TYPE_X509_CONSTRAINT_VIOLATION_ERROR. */
typedef struct X509ConstraintViolationError {
    Byte unused_;
} X509ConstraintViolationError;

extern const Type *const TYPE_X509_CONSTRAINT_VIOLATION_ERROR;
extern const Error x509_constraint_violation_error;

/* ConstraintViolationError.Error: "x509: invalid signature: parent certificate
 * cannot sign this kind of certificate". */
BURROW_STATIC(ret) Str
x509_constraint_violation_error_error(X509ConstraintViolationError e);

/* UnhandledCriticalExtension: a certificate has a critical extension that
 * this does not understand. The same arrangement as the one above. */
typedef struct X509UnhandledCriticalExtension {
    Byte unused_;
} X509UnhandledCriticalExtension;

extern const Type *const TYPE_X509_UNHANDLED_CRITICAL_EXTENSION;
extern const Error x509_unhandled_critical_extension;

/* UnhandledCriticalExtension.Error: "x509: unhandled critical extension". */
BURROW_STATIC(ret) Str
x509_unhandled_critical_extension_error(X509UnhandledCriticalExtension e);

/* ------------------------------------------------------- certificate requests */

/* CertificateRequest: a PKCS #10 certificate signing request. The raw fields
 * point into the DER it was parsed from. attributes is a slice of
 * PkixAttributeTypeAndValueSET and deprecated in Go; extensions and
 * extra_extensions are PkixExtension, and the names are as in a
 * certificate. */
typedef struct X509CertificateRequest {
    Slice raw;                         /* the whole request */
    Slice raw_tbs_certificate_request; /* the part that is signed */
    Slice raw_subject_public_key_info; /* the SubjectPublicKeyInfo */
    Slice raw_subject;                 /* the subject's DER */
    Slice raw_signature_algorithm;     /* the AlgorithmIdentifier */

    Int version;
    Slice signature;
    X509SignatureAlgorithm signature_algorithm;

    X509PublicKeyAlgorithm public_key_algorithm;
    Any public_key;

    PkixName subject;

    Slice attributes;

    Slice extensions;
    Slice extra_extensions;

    Slice dns_names;
    Slice email_addresses;
    Slice ip_addresses;
    Slice uris;
} X509CertificateRequest;

extern const Type burrow_type_X509CertificateRequest;
#define TYPE_X509_CERTIFICATE_REQUEST TYPE_OF(X509CertificateRequest)

/* ParseCertificateRequest: the request in der, from a. */
BURROW_OWNS(ret) X509CertificateRequest *
x509_parse_certificate_request(Alloc *a, Slice der, Error *err);

/* CertificateRequest.CheckSignature: no error when the signature on c is
 * valid. */
BURROW_STATIC(ret) Error
x509_certificate_request_check_signature(const X509CertificateRequest *c);

/* ------------------------------------------------------- revocation lists */

/* RevocationListEntry: one entry of the revokedCertificates of a CRL. raw
 * points into the DER the list was parsed from, and extensions and
 * extra_extensions are slices of PkixExtension. reason_code is the reasonCode
 * extension of RFC 5280 section 5.3.1, and zero both when it is absent and
 * when it says unspecified. */
typedef struct X509RevocationListEntry {
    Slice raw;
    BigInt *serial_number;
    Time revocation_time;
    Int reason_code;
    Slice extensions;
    Slice extra_extensions;
} X509RevocationListEntry;

extern const Type burrow_type_X509RevocationListEntry;
#define TYPE_X509_REVOCATION_LIST_ENTRY TYPE_OF(X509RevocationListEntry)

/* RevocationList: a certificate revocation list of RFC 5280. The raw fields
 * point into the DER it was parsed from. revoked_certificate_entries is a
 * slice of X509RevocationListEntry, revoked_certificates the same entries as
 * PkixRevokedCertificate, which Go has deprecated, and extensions and
 * extra_extensions slices of PkixExtension. */
typedef struct X509RevocationList {
    Slice raw;                     /* the whole list */
    Slice raw_tbs_revocation_list; /* the part that is signed */
    Slice raw_issuer;              /* the issuer's DER */
    Slice raw_signature_algorithm; /* the AlgorithmIdentifier */

    PkixName issuer;
    Slice authority_key_id;

    Slice signature;
    X509SignatureAlgorithm signature_algorithm;

    Slice revoked_certificate_entries;
    Slice revoked_certificates;

    BigInt *number;

    Time this_update;
    Time next_update;

    Slice extensions;
    Slice extra_extensions;
} X509RevocationList;

extern const Type burrow_type_X509RevocationList;
#define TYPE_X509_REVOCATION_LIST TYPE_OF(X509RevocationList)

/* ParseRevocationList: the X.509 v2 CRL in der, from a. Anything after the
 * list is ignored, as in Go. */
BURROW_OWNS(ret) X509RevocationList *x509_parse_revocation_list(Alloc *a, Slice der,
                                                                Error *err);

/* RevocationList.CheckSignatureFrom: no error when rl is signed by parent,
 * which has to be allowed to sign CRLs. */
BURROW_STATIC(ret) Error x509_revocation_list_check_signature_from(
    const X509RevocationList *rl, const X509Certificate *parent);

/* ParseCRL: the CRL in crl_bytes, which can be DER or a PEM block of type
 * "X509 CRL" with nothing before it, from a. Deprecated in Go in favour of
 * x509_parse_revocation_list. */
BURROW_OWNS(ret) PkixCertificateList *x509_parse_crl(Alloc *a, Slice crl_bytes,
                                                     Error *err);

/* ParseDERCRL: the CRL in der, from a. Deprecated in Go, as above. */
BURROW_OWNS(ret) PkixCertificateList *x509_parse_dercrl(Alloc *a, Slice der,
                                                        Error *err);

/* ---------------------------------------------------------------- creating */

/* CreateCertificate: a new certificate in DER, from a, made from template and
 * signed by priv as parent. The certificate is self-signed when parent is
 * template. pub is the public key of the new certificate, in an Any the way
 * x509_marshal_pkix_public_key takes one, and priv is the signer of parent,
 * whose public key has to be parent's when parent has one.
 *
 * Of template, these are used: authority_key_id, basic_constraints_valid,
 * crl_distribution_points, dns_names, email_addresses, excluded_dns_domains,
 * excluded_email_addresses, excluded_ip_ranges, excluded_uri_domains,
 * ext_key_usage, extra_extensions, ip_addresses, is_ca,
 * issuing_certificate_url, key_usage, max_path_len, max_path_len_zero,
 * not_after, not_before, ocsp_server, permitted_dns_domains,
 * permitted_dns_domains_critical, permitted_email_addresses,
 * permitted_ip_ranges, permitted_uri_domains, policies, policy_identifiers,
 * raw_subject, serial_number, signature_algorithm, subject, subject_key_id,
 * unknown_ext_key_usage and uris. policy_identifiers is only used instead of
 * policies when GODEBUG has x509usepolicies=0.
 *
 * A NULL serial_number gets 20 random bytes from rand, with the top bit
 * cleared. A CA without a subject_key_id gets the first 20 bytes of the
 * SHA-256 of its public key, or the SHA-1 of it with GODEBUG x509sha256skid=0.
 * A zero rand is crypto_rand_reader, and a zero priv is an error, as a key
 * that is not a crypto.Signer is in Go. */
BURROW_OWNS(ret) Slice x509_create_certificate(Alloc *a, IoReader rand,
                                               const X509Certificate *template_,
                                               const X509Certificate *parent, Any pub,
                                               CryptoSigner priv, Error *err);

/* CreateCertificateRequest: a new PKCS #10 certificate request in DER, from
 * a, made from template and signed by priv. Of template, these are used:
 * attributes, dns_names, email_addresses, extra_extensions, ip_addresses,
 * uris, raw_subject, signature_algorithm and subject. The public key is
 * priv's. */
BURROW_OWNS(ret) Slice x509_create_certificate_request(
    Alloc *a, IoReader rand, const X509CertificateRequest *template_, CryptoSigner priv,
    Error *err);

/* CreateRevocationList: a new X.509 v2 CRL in DER, from a, made from template
 * and signed by priv as issuer. issuer has to have the CRL signing key usage
 * and a subject key ID. Of template, these are used:
 * revoked_certificate_entries, or revoked_certificates when there are none,
 * number, this_update, next_update, extra_extensions and
 * signature_algorithm. */
BURROW_OWNS(ret) Slice x509_create_revocation_list(Alloc *a, IoReader rand,
                                                   const X509RevocationList *template_,
                                                   const X509Certificate *issuer,
                                                   CryptoSigner priv, Error *err);

/* Certificate.CreateCRL: a CRL in DER, from a, signed by priv as c, listing
 * revoked_certs, a slice of PkixRevokedCertificate. Deprecated in Go in
 * favour of x509_create_revocation_list, since it writes a v1 CRL. */
BURROW_OWNS(ret) Slice x509_certificate_create_crl(const X509Certificate *c, Alloc *a,
                                                   IoReader rand, CryptoSigner priv,
                                                   Slice revoked_certs, Time now,
                                                   Time expiry, Error *err);

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

/* ------------------------------------------------------------- CertPool */

/* CertPool: a set of certificates, looked up by subject when a chain is
 * built. The pool keeps its own memory, from the Alloc it was made with, and
 * x509_cert_pool_free gives it back. A certificate added with
 * x509_cert_pool_add_cert is borrowed, so it has to live as long as the pool
 * does. One read from PEM is parsed into the pool's memory. Go parses those
 * again each time they are needed to keep the system pool small, and this
 * keeps the parsed certificate instead. */
typedef struct X509CertPool X509CertPool;

/* A constraint AddCertWithConstraint puts on the chains a root may finish.
 * chain is a slice of X509Certificate pointers, and an error rejects it. */
BURROW_FUNC(X509CertConstraint, Error, Slice chain);

/* NewCertPool: an empty pool, from a. */
BURROW_OWNS(ret) X509CertPool *x509_new_cert_pool(Alloc *a);

/* Gives back the memory of s and the certificates it parsed. s may be NULL. */
void x509_cert_pool_free(X509CertPool *s);

/* CertPool.Clone: a copy of s, from a. The certificates are shared with s, as
 * they are in Go, so the copy must not outlive s. */
BURROW_OWNS(ret) X509CertPool *x509_cert_pool_clone(const X509CertPool *s, Alloc *a);

/* CertPool.AddCert: adds cert to s, unless s has it already. Panics when cert
 * is NULL. */
void x509_cert_pool_add_cert(X509CertPool *s, const X509Certificate *cert);

/* CertPool.AddCertWithConstraint: AddCert, with constraint called on every
 * chain that cert ends once the chain is built. A zero constraint is none. */
void x509_cert_pool_add_cert_with_constraint(X509CertPool *s,
                                             const X509Certificate *cert,
                                             X509CertConstraint constraint);

/* CertPool.AppendCertsFromPEM: adds each CERTIFICATE block in pem_certs that
 * has no headers and parses, and says whether one was added. */
bool x509_cert_pool_append_certs_from_pem(X509CertPool *s, Slice pem_certs);

/* CertPool.Subjects: the raw subject of each certificate in s, in the order
 * they were added. The slice is from a, and the subjects in it are borrowed
 * from s. Deprecated in Go, since it leaves out the system roots. */
BURROW_OWNS(ret) Slice x509_cert_pool_subjects(const X509CertPool *s, Alloc *a);

/* CertPool.Equal: whether s and other hold the same certificates. */
bool x509_cert_pool_equal(const X509CertPool *s, const X509CertPool *other);

#ifdef __cplusplus
}
#endif

#endif
