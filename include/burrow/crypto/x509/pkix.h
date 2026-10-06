/* crypto/x509/pkix, the ASN.1 structures X.509 certificates, CRLs and OCSP
 * share.
 *
 * Every struct here has a type descriptor with Go's field names and asn1
 * struct tags, so encoding/asn1 reads and writes them directly:
 *
 *     PkixRDNSequence rdns = slice_nil(TYPE_PKIX_RDN_SEQUENCE);
 *     asn1_unmarshal(a, der, BURROW_ANY(TYPE_PKIX_RDN_SEQUENCE, &rdns), &err);
 *
 *     PkixName name = {0};
 *     pkix_name_fill_from_rdn_sequence(&name, a, &rdns);
 *     Str s = pkix_name_string(name, a);    // "CN=example.com,O=Example"
 *
 * Copyright 2011 The Go Authors. All rights reserved.
 * Copyright 2026 The burrow Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style licence that can be found
 * in the LICENSE file. */

/* burrow:package crypto/x509/pkix */

#ifndef BURROW_CRYPTO_X509_PKIX_H
#define BURROW_CRYPTO_X509_PKIX_H

#include "burrow/core.h"
#include "burrow/encoding/asn1.h"
#include "burrow/iface.h"
#include "burrow/math/big.h"
#include "burrow/mem.h"
#include "burrow/own.h"
#include "burrow/slice.h"
#include "burrow/time.h"
#include "burrow/type.h"

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* AlgorithmIdentifier, RFC 5280 section 4.1.1.2. parameters is optional, and
 * the zero Asn1RawValue leaves it out. */
typedef struct PkixAlgorithmIdentifier {
    Asn1ObjectIdentifier algorithm;
    Asn1RawValue parameters;
} PkixAlgorithmIdentifier;

extern const Type burrow_type_PkixAlgorithmIdentifier;
#define TYPE_PKIX_ALGORITHM_IDENTIFIER TYPE_OF(PkixAlgorithmIdentifier)

/* AttributeTypeAndValue, RFC 5280 section 4.1.2.4. Parsed from a name in a
 * certificate, value holds a Str for the string types, an int64_t for an
 * INTEGER, an Asn1BitString, a Bytes for an OCTET STRING, an
 * Asn1ObjectIdentifier, a Time, a bool, nothing for NULL, and an Asn1RawValue
 * for anything else. */
typedef struct PkixAttributeTypeAndValue {
    Asn1ObjectIdentifier type;
    Any value;
} PkixAttributeTypeAndValue;

extern const Type burrow_type_PkixAttributeTypeAndValue;
#define TYPE_PKIX_ATTRIBUTE_TYPE_AND_VALUE TYPE_OF(PkixAttributeTypeAndValue)

/* RelativeDistinguishedNameSET: a slice of PkixAttributeTypeAndValue, written
 * as a SET OF. */
typedef Slice PkixRelativeDistinguishedNameSET;

extern const Type burrow_type_PkixRelativeDistinguishedNameSET;
#define TYPE_PKIX_RELATIVE_DISTINGUISHED_NAME_SET                                      \
    TYPE_OF(PkixRelativeDistinguishedNameSET)

/* RDNSequence: a slice of PkixRelativeDistinguishedNameSET, which is what the
 * subject and issuer of a certificate are. */
typedef Slice PkixRDNSequence;

extern const Type burrow_type_PkixRDNSequence;
#define TYPE_PKIX_RDN_SEQUENCE TYPE_OF(PkixRDNSequence)

/* RDNSequence.String: r roughly in RFC 2253 form, the last RDN first, such as
 * "CN=J. Smith,O=Widget Inc.,C=US", from a. A value whose attribute type is
 * not one of the common ones and which is not a string is written as # and its
 * DER in hex. */
BURROW_OWNS(ret) Str pkix_rdn_sequence_string(PkixRDNSequence r, Alloc *a);

/* AttributeTypeAndValueSET, from RFC 2986 (PKCS #10). value is a slice of
 * slices of PkixAttributeTypeAndValue, written as a SET of SEQUENCEs. */
typedef struct PkixAttributeTypeAndValueSET {
    Asn1ObjectIdentifier type;
    Slice value;
} PkixAttributeTypeAndValueSET;

extern const Type burrow_type_PkixAttributeTypeAndValueSET;
#define TYPE_PKIX_ATTRIBUTE_TYPE_AND_VALUE_SET TYPE_OF(PkixAttributeTypeAndValueSET)

/* Extension, RFC 5280 section 4.2. value is the extension's DER. */
typedef struct PkixExtension {
    Asn1ObjectIdentifier id;
    bool critical;
    Slice value;
} PkixExtension;

extern const Type burrow_type_PkixExtension;
#define TYPE_PKIX_EXTENSION TYPE_OF(PkixExtension)

/* Name: an X.509 distinguished name, with the common attributes in fields of
 * their own. The slice fields are slices of Str, and names and extra_names
 * slices of PkixAttributeTypeAndValue.
 *
 * names is every attribute a parsed name had, filled by
 * pkix_name_fill_from_rdn_sequence. extra_names is the other way: attributes
 * to put into a name that is being written, which win over the fields with the
 * same type. A nil extra_names and an empty one differ, the same as in Go: the
 * string of a name with a nil one includes the uncommon attributes in names,
 * and with an empty one it does not. */
typedef struct PkixName {
    Slice country, organization, organizational_unit;
    Slice locality, province;
    Slice street_address, postal_code;
    Str serial_number, common_name;
    Slice names;
    Slice extra_names;
} PkixName;

/* Name.FillFromRDNSequence: every attribute in rdns appended to n->names, and
 * the string ones of the common types to their fields as well, from a. The
 * grouping into RDNs is lost. The values are shared with rdns. False when a
 * runs out of memory, with whatever was appended by then. */
bool pkix_name_fill_from_rdn_sequence(PkixName *n, Alloc *a,
                                      const PkixRDNSequence *rdns);

/* Name.ToRDNSequence: n as an RDNSequence from a, one RDN for each field with
 * all its values in it, in the order country, province, locality, street
 * address, postal code, organization, organizational unit, common name and
 * serial number, and then one RDN for each of extra_names. A field whose type
 * is in extra_names is left out. The nil slice when there is nothing. The
 * attributes are copied into a, and their strings point at n's. */
BURROW_OWNS(ret) PkixRDNSequence pkix_name_to_rdn_sequence(PkixName n, Alloc *a);

/* Name.String: n roughly in RFC 2253 form, from a. See
 * pkix_rdn_sequence_string. */
BURROW_OWNS(ret) Str pkix_name_string(PkixName n, Alloc *a);

/* RevokedCertificate, RFC 5280 section 5.1. extensions is a slice of
 * PkixExtension. */
typedef struct PkixRevokedCertificate {
    BigInt *serial_number;
    Time revocation_time;
    Slice extensions;
} PkixRevokedCertificate;

extern const Type burrow_type_PkixRevokedCertificate;
#define TYPE_PKIX_REVOKED_CERTIFICATE TYPE_OF(PkixRevokedCertificate)

/* TBSCertificateList, RFC 5280 section 5.1. raw is the DER it was parsed
 * from, revoked_certificates a slice of PkixRevokedCertificate and extensions
 * a slice of PkixExtension. Deprecated in Go in favour of
 * x509.RevocationList. */
typedef struct PkixTBSCertificateList {
    Asn1RawContent raw;
    Int version;
    PkixAlgorithmIdentifier signature;
    PkixRDNSequence issuer;
    Time this_update;
    Time next_update;
    Slice revoked_certificates;
    Slice extensions;
} PkixTBSCertificateList;

extern const Type burrow_type_PkixTBSCertificateList;
#define TYPE_PKIX_TBS_CERTIFICATE_LIST TYPE_OF(PkixTBSCertificateList)

/* CertificateList, RFC 5280 section 5.1. Deprecated in Go in favour of
 * x509.RevocationList. */
typedef struct PkixCertificateList {
    PkixTBSCertificateList tbs_cert_list;
    PkixAlgorithmIdentifier signature_algorithm;
    Asn1BitString signature_value;
} PkixCertificateList;

extern const Type burrow_type_PkixCertificateList;
#define TYPE_PKIX_CERTIFICATE_LIST TYPE_OF(PkixCertificateList)

/* CertificateList.HasExpired: whether now is at or after the list's next
 * update. A list with no next update has always expired. */
bool pkix_certificate_list_has_expired(const PkixCertificateList *cert_list, Time now);

#ifdef __cplusplus
}
#endif

#endif
