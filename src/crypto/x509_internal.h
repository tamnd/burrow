/* What x509.c has inside, for the crypto/x509 tests.
 *
 * Copyright 2026 The burrow Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style licence that can be found
 * in the LICENSE file. */

#ifndef BURROW_SRC_CRYPTO_X509_INTERNAL_H
#define BURROW_SRC_CRYPTO_X509_INTERNAL_H

#include "burrow/crypto/x509.h"

#include "burrow/error.h"

#include <stdbool.h>
#include <stdint.h>

/* errInvalidOID, "invalid oid", which the OID functions give for anything
 * that is not an OID. */
extern const Error burrow__x509_err_invalid_oid;

/* Reads the GODEBUG settings from value as if it were the environment's, or
 * forgets them when value is NULL so that the next use reads the environment
 * again. For tests, which cannot change the environment of a process that has
 * already looked at it. */
void burrow__x509_godebug_set(const char *value);

/* parseASN1String, which turns the ASN.1 string of tag in value into *out. */
Error burrow__x509_parse_asn1_string(Alloc *a, uint8_t tag, Slice value, Str *out);

/* domainNameValid, whether s is a domain name, or a constraint on one when
 * constraint is set. */
bool burrow__x509_domain_name_valid(Str s, bool constraint);

/* parseRFC2821Mailbox, with the unquoted local part made in a. */
bool burrow__x509_parse_rfc2821_mailbox(Alloc *a, Str in, Str *local, Str *domain);

/* asn1BitLength, the number of bits in bit_string up to the last one set. */
Int burrow__x509_asn1_bit_length(Slice bit_string);

/* marshalSANs, the subjectAltName extension value for the names given. */
Slice burrow__x509_marshal_sans(Alloc *a, Slice dns_names, Slice email_addresses,
                                Slice ip_addresses, Slice uris, Error *err);

/* Entry i of signatureAlgorithmDetails, or false when i is past the end.
 * params is the DER of the parameters a PSS algorithm writes and nil for the
 * others. */
bool burrow__x509_signature_details(Int i, CryptoHash *hash, bool *is_rsa_pss,
                                    Slice *params);

/* The parts of a CertPool that chain building reads, from x509_verify.c. */
Int burrow__x509_cert_pool_len(const X509CertPool *s);

/* The certificate at index n of s, with its constraint in *constraint when
 * that is not NULL. */
const X509Certificate *burrow__x509_cert_pool_cert(const X509CertPool *s, Int n,
                                                   X509CertConstraint *constraint);

/* Whether s has a certificate with the same DER as cert. */
bool burrow__x509_cert_pool_contains(const X509CertPool *s,
                                     const X509Certificate *cert);

/* potentialParent */
typedef struct X509PotentialParent {
    const X509Certificate *cert;
    X509CertConstraint constraint;
} X509PotentialParent;

/* findPotentialParents: the certificates in s whose subject is cert's
 * issuer, best key id match first, as an array from a with *n entries. */
X509PotentialParent *burrow__x509_cert_pool_find_potential_parents(
    const X509CertPool *s, const X509Certificate *cert, Alloc *a, Int *n);

/* Marks s as the system pool, which Equal tells apart from other pools. */
void burrow__x509_cert_pool_set_system(X509CertPool *s);

/* GODEBUG x509usefallbackroots=1 and x509sslcertoverrideplatform=0. */
bool burrow__x509_use_fallback_roots(void);
bool burrow__x509_no_cert_override(void);

/* systemRootsPool: the shared system pool, or NULL when there is none. */
X509CertPool *burrow__x509_system_roots_pool(void);

#endif /* BURROW_SRC_CRYPTO_X509_INTERNAL_H */
