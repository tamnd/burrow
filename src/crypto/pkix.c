/* crypto/x509/pkix, from Go's pkix.go.
 *
 * Copyright 2011 The Go Authors. All rights reserved.
 * Copyright 2026 The burrow Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style licence that can be found
 * in the LICENSE file. */

#include "burrow/crypto/x509/pkix.h"

#include "burrow/declare.h"
#include "burrow/encoding/hex.h"
#include "burrow/fmt.h"
#include "burrow/strings.h"
#include "burrow/utf8.h"

#include <string.h>

/* ------------------------------------------------------------- descriptors */

/* A slice descriptor and nothing else, for slices the header declares. A name
 * matters to encoding/asn1 only when it ends in SET. */
#define PKIX_SLICE_TYPE(Name, label, T)                                                \
    const Type burrow_type_##Name = {                                                  \
        label,                                                                         \
        {NULL, 0},                                                                     \
        KIND_SLICE,                                                                    \
        (uint32_t)sizeof(Slice),                                                       \
        (uint16_t)_Alignof(Slice),                                                     \
        0,                                                                             \
        0,                                                                             \
        NULL,                                                                          \
        NULL,                                                                          \
        TYPE_OF(T),                                                                    \
        NULL,                                                                          \
        0,                                                                             \
        0,                                                                             \
        NULL,                                                                          \
    }

/* The label of a slice without a name. */
#define PKIX_UNNAMED {NULL, 0}

BURROW_PTR_TYPE(PkixBigIntPtr, BigInt);

#define PKIX_ALGORITHM_IDENTIFIER_FIELDS(F, T)                                         \
    F(T, Asn1ObjectIdentifier, algorithm, Algorithm, "")                               \
    F(T, Asn1RawValue, parameters, Parameters, "asn1:\"optional\"")
BURROW_STRUCT_AS_DEFINE(PkixAlgorithmIdentifier, PKIX_ALGORITHM_IDENTIFIER_FIELDS);

#define PKIX_ATTRIBUTE_TYPE_AND_VALUE_FIELDS(F, T)                                     \
    F(T, Asn1ObjectIdentifier, type, Type, "")                                         \
    F(T, Any, value, Value, "")
BURROW_STRUCT_AS_DEFINE(PkixAttributeTypeAndValue,
                        PKIX_ATTRIBUTE_TYPE_AND_VALUE_FIELDS);

PKIX_SLICE_TYPE(PkixRelativeDistinguishedNameSET,
                BURROW_S_INIT("PkixRelativeDistinguishedNameSET"),
                PkixAttributeTypeAndValue);
PKIX_SLICE_TYPE(PkixRDNSequence, BURROW_S_INIT("PkixRDNSequence"),
                PkixRelativeDistinguishedNameSET);

/* [][]AttributeTypeAndValue, the value of an AttributeTypeAndValueSET. */
typedef Slice PkixAttributeTypeAndValues;
static PKIX_SLICE_TYPE(PkixAttributeTypeAndValues, PKIX_UNNAMED,
                       PkixAttributeTypeAndValue);
typedef Slice PkixAttributeTypeAndValuesSlice;
static PKIX_SLICE_TYPE(PkixAttributeTypeAndValuesSlice, PKIX_UNNAMED,
                       PkixAttributeTypeAndValues);

#define PKIX_ATTRIBUTE_TYPE_AND_VALUE_SET_FIELDS(F, T)                                 \
    F(T, Asn1ObjectIdentifier, type, Type, "")                                         \
    F(T, PkixAttributeTypeAndValuesSlice, value, Value, "asn1:\"set\"")
BURROW_STRUCT_AS_DEFINE(PkixAttributeTypeAndValueSET,
                        PKIX_ATTRIBUTE_TYPE_AND_VALUE_SET_FIELDS);

#define PKIX_EXTENSION_FIELDS(F, T)                                                    \
    F(T, Asn1ObjectIdentifier, id, Id, "")                                             \
    F(T, bool, critical, Critical, "asn1:\"optional\"")                                \
    F(T, Bytes, value, Value, "")
BURROW_STRUCT_AS_DEFINE(PkixExtension, PKIX_EXTENSION_FIELDS);

typedef Slice PkixExtensions;
static PKIX_SLICE_TYPE(PkixExtensions, PKIX_UNNAMED, PkixExtension);

#define PKIX_REVOKED_CERTIFICATE_FIELDS(F, T)                                          \
    F(T, PkixBigIntPtr, serial_number, SerialNumber, "")                               \
    F(T, Time, revocation_time, RevocationTime, "")                                    \
    F(T, PkixExtensions, extensions, Extensions, "asn1:\"optional\"")
BURROW_STRUCT_AS_DEFINE(PkixRevokedCertificate, PKIX_REVOKED_CERTIFICATE_FIELDS);

typedef Slice PkixRevokedCertificates;
static PKIX_SLICE_TYPE(PkixRevokedCertificates, PKIX_UNNAMED, PkixRevokedCertificate);

#define PKIX_TBS_CERTIFICATE_LIST_FIELDS(F, T)                                         \
    F(T, Asn1RawContent, raw, Raw, "")                                                 \
    F(T, Int, version, Version, "asn1:\"optional,default:0\"")                         \
    F(T, PkixAlgorithmIdentifier, signature, Signature, "")                            \
    F(T, PkixRDNSequence, issuer, Issuer, "")                                          \
    F(T, Time, this_update, ThisUpdate, "")                                            \
    F(T, Time, next_update, NextUpdate, "asn1:\"optional\"")                           \
    F(T, PkixRevokedCertificates, revoked_certificates, RevokedCertificates,           \
      "asn1:\"optional\"")                                                             \
    F(T, PkixExtensions, extensions, Extensions, "asn1:\"tag:0,optional,explicit\"")
BURROW_STRUCT_AS_DEFINE(PkixTBSCertificateList, PKIX_TBS_CERTIFICATE_LIST_FIELDS);

#define PKIX_CERTIFICATE_LIST_FIELDS(F, T)                                             \
    F(T, PkixTBSCertificateList, tbs_cert_list, TBSCertList, "")                       \
    F(T, PkixAlgorithmIdentifier, signature_algorithm, SignatureAlgorithm, "")         \
    F(T, Asn1BitString, signature_value, SignatureValue, "")
BURROW_STRUCT_AS_DEFINE(PkixCertificateList, PKIX_CERTIFICATE_LIST_FIELDS);

/* ------------------------------------------------------------ RDNSequence */

/* The attribute types with names of their own, all 2.5.4.x. */
static const Int pkix_oid_country[] = {2, 5, 4, 6};
static const Int pkix_oid_organization[] = {2, 5, 4, 10};
static const Int pkix_oid_organizational_unit[] = {2, 5, 4, 11};
static const Int pkix_oid_common_name[] = {2, 5, 4, 3};
static const Int pkix_oid_serial_number[] = {2, 5, 4, 5};
static const Int pkix_oid_locality[] = {2, 5, 4, 7};
static const Int pkix_oid_province[] = {2, 5, 4, 8};
static const Int pkix_oid_street_address[] = {2, 5, 4, 9};
static const Int pkix_oid_postal_code[] = {2, 5, 4, 17};

static Asn1ObjectIdentifier pkix_oid(const Int *arcs) {
    return (Asn1ObjectIdentifier){(void *)(uintptr_t)arcs, 4, 4, TYPE_INT};
}

/* The last arc of a 2.5.4.x type, or -1 for any other. */
static Int pkix_attr_arc(Asn1ObjectIdentifier t) {
    const Int *v = (const Int *)t.p;
    if (t.len == 4 && v[0] == 2 && v[1] == 5 && v[2] == 4)
        return v[3];
    return -1;
}

/* attributeTypeNames */
static const char *pkix_attr_name(Asn1ObjectIdentifier t) {
    switch (pkix_attr_arc(t)) {
    case 6:
        return "C";
    case 10:
        return "O";
    case 11:
        return "OU";
    case 3:
        return "CN";
    case 5:
        return "SERIALNUMBER";
    case 7:
        return "L";
    case 8:
        return "ST";
    case 9:
        return "STREET";
    case 17:
        return "POSTALCODE";
    default:
        return NULL;
    }
}

/* The value with RFC 2253's characters escaped. Go ranges over the runes and
 * builds a new string from them, so a byte that is not UTF-8 comes out as
 * U+FFFD, and that is kept. */
static void pkix_write_escaped(StringsBuilder *b, Str v) {
    for (Int k = 0; k < v.len;) {
        Int size = 0;
        Rune c = utf8_decode_rune_in_string(str_from_bytes(v.p + k, v.len - k), &size);
        bool escape = false;
        switch (c) {
        case ',':
        case '+':
        case '"':
        case '\\':
        case '<':
        case '>':
        case ';':
            escape = true;
            break;
        case ' ':
            escape = k == 0 || k == v.len - 1;
            break;
        case '#':
            escape = k == 0;
            break;
        default:
            break;
        }
        if (escape)
            strings_builder_write_byte(b, '\\');
        strings_builder_write_rune(b, c, NULL);
        k += size;
    }
}

Str pkix_rdn_sequence_string(PkixRDNSequence r, Alloc *a) {
    StringsBuilder buf = STRINGS_BUILDER(a);
    const PkixRelativeDistinguishedNameSET *rdns = r.p;
    for (Int i = 0; i < r.len; i++) {
        PkixRelativeDistinguishedNameSET rdn = rdns[r.len - 1 - i];
        if (i > 0)
            strings_builder_write_byte(&buf, ',');
        const PkixAttributeTypeAndValue *atvs = rdn.p;
        for (Int j = 0; j < rdn.len; j++) {
            const PkixAttributeTypeAndValue *tv = &atvs[j];
            if (j > 0)
                strings_builder_write_byte(&buf, '+');

            Str oid_string = asn1_object_identifier_string(tv->type, a);
            const char *name = pkix_attr_name(tv->type);
            Str type_name = name != NULL ? str_from_cstr(name) : oid_string;
            if (name == NULL && tv->value.t != TYPE_STRING) {
                /* RFC 2253 section 2.4: a value whose ASN.1 type has a string
                 * form is written as a string, and anything else as its DER
                 * in hex. */
                Error err = BURROW_NO_ERROR;
                Slice der = asn1_marshal(a, tv->value, &err);
                if (BURROW_OK(err)) {
                    strings_builder_write_string(&buf, oid_string, NULL);
                    strings_builder_write_string(&buf, BURROW_S("=#"), NULL);
                    strings_builder_write_string(&buf, hex_encode_to_string(a, der),
                                                 NULL);
                    continue; /* No value escaping necessary. */
                }
            }

            Str value =
                fmt_sprint(a, (Slice){(void *)(uintptr_t)&tv->value, 1, 1, TYPE_ANY});
            strings_builder_write_string(&buf, type_name, NULL);
            strings_builder_write_byte(&buf, '=');
            pkix_write_escaped(&buf, value);
        }
    }
    return strings_builder_string(&buf);
}

/* ------------------------------------------------------------------- Name */

static bool pkix_push_str(Alloc *a, Slice *s, Str v) {
    if (s->elem == NULL)
        *s = slice_nil(TYPE_STRING);
    Slice grown = slice_append(a, *s, &v, 1);
    if (grown.len != s->len + 1)
        return false;
    *s = grown;
    return true;
}

bool pkix_name_fill_from_rdn_sequence(PkixName *n, Alloc *a,
                                      const PkixRDNSequence *rdns) {
    const PkixRelativeDistinguishedNameSET *rs = rdns->p;
    for (Int i = 0; i < rdns->len; i++) {
        PkixRelativeDistinguishedNameSET rdn = rs[i];
        const PkixAttributeTypeAndValue *atvs = rdn.p;
        for (Int j = 0; j < rdn.len; j++) {
            const PkixAttributeTypeAndValue *atv = &atvs[j];
            if (n->names.elem == NULL)
                n->names = slice_nil(TYPE_PKIX_ATTRIBUTE_TYPE_AND_VALUE);
            Slice grown = slice_append(a, n->names, atv, 1);
            if (grown.len != n->names.len + 1)
                return false;
            n->names = grown;
            if (atv->value.t != TYPE_STRING)
                continue;
            Str value = *(const Str *)atv->value.data;

            bool ok = true;
            switch (pkix_attr_arc(atv->type)) {
            case 3:
                n->common_name = value;
                break;
            case 5:
                n->serial_number = value;
                break;
            case 6:
                ok = pkix_push_str(a, &n->country, value);
                break;
            case 7:
                ok = pkix_push_str(a, &n->locality, value);
                break;
            case 8:
                ok = pkix_push_str(a, &n->province, value);
                break;
            case 9:
                ok = pkix_push_str(a, &n->street_address, value);
                break;
            case 10:
                ok = pkix_push_str(a, &n->organization, value);
                break;
            case 11:
                ok = pkix_push_str(a, &n->organizational_unit, value);
                break;
            case 17:
                ok = pkix_push_str(a, &n->postal_code, value);
                break;
            default:
                break;
            }
            if (!ok)
                return false;
        }
    }
    return true;
}

/* oidInAttributeTypeAndValue */
static bool pkix_oid_in(Asn1ObjectIdentifier oid, Slice atv) {
    const PkixAttributeTypeAndValue *v = atv.p;
    for (Int i = 0; i < atv.len; i++) {
        if (asn1_object_identifier_equal(v[i].type, oid))
            return true;
    }
    return false;
}

static bool pkix_push_rdn(Alloc *a, PkixRDNSequence *in,
                          PkixRelativeDistinguishedNameSET s) {
    if (in->elem == NULL)
        *in = slice_nil(TYPE_PKIX_RDN_SEQUENCE);
    Slice grown = slice_append(a, *in, &s, 1);
    if (grown.len != in->len + 1)
        return false;
    *in = grown;
    return true;
}

/* appendRDNs: one RDN with an attribute for each of values. The strings are
 * copied into a so the attributes do not point into n. */
static bool pkix_append_rdns(const PkixName *n, Alloc *a, PkixRDNSequence *in,
                             const Str *values, Int nvalues, const Int *arcs) {
    Asn1ObjectIdentifier oid = pkix_oid(arcs);
    if (nvalues == 0 || pkix_oid_in(oid, n->extra_names))
        return true;
    Str *strs = mem_alloc_nozero(a, sizeof(Str) * (size_t)nvalues, _Alignof(Str));
    Slice s = slice_make(a, TYPE_PKIX_ATTRIBUTE_TYPE_AND_VALUE, nvalues, nvalues);
    if (strs == NULL || s.len != nvalues)
        return false;
    PkixAttributeTypeAndValue *atv = s.p;
    for (Int i = 0; i < nvalues; i++) {
        strs[i] = values[i];
        atv[i].type = oid;
        atv[i].value = BURROW_ANY(TYPE_STRING, &strs[i]);
    }
    return pkix_push_rdn(a, in, s);
}

static bool pkix_append_field(const PkixName *n, Alloc *a, PkixRDNSequence *in,
                              Slice values, const Int *arcs) {
    return pkix_append_rdns(n, a, in, values.p, values.len, arcs);
}

PkixRDNSequence pkix_name_to_rdn_sequence(PkixName n, Alloc *a) {
    PkixRDNSequence ret = slice_nil(TYPE_PKIX_RDN_SEQUENCE);
    bool ok =
        pkix_append_field(&n, a, &ret, n.country, pkix_oid_country) &&
        pkix_append_field(&n, a, &ret, n.province, pkix_oid_province) &&
        pkix_append_field(&n, a, &ret, n.locality, pkix_oid_locality) &&
        pkix_append_field(&n, a, &ret, n.street_address, pkix_oid_street_address) &&
        pkix_append_field(&n, a, &ret, n.postal_code, pkix_oid_postal_code) &&
        pkix_append_field(&n, a, &ret, n.organization, pkix_oid_organization) &&
        pkix_append_field(&n, a, &ret, n.organizational_unit,
                          pkix_oid_organizational_unit);
    if (ok && n.common_name.len > 0)
        ok = pkix_append_rdns(&n, a, &ret, &n.common_name, 1, pkix_oid_common_name);
    if (ok && n.serial_number.len > 0)
        ok = pkix_append_rdns(&n, a, &ret, &n.serial_number, 1, pkix_oid_serial_number);
    const PkixAttributeTypeAndValue *extra = n.extra_names.p;
    for (Int i = 0; ok && i < n.extra_names.len; i++) {
        Slice one = slice_append(a, slice_nil(TYPE_PKIX_ATTRIBUTE_TYPE_AND_VALUE),
                                 &extra[i], 1);
        ok = one.len == 1 && pkix_push_rdn(a, &ret, one);
    }
    return ok ? ret : slice_nil(TYPE_PKIX_RDN_SEQUENCE);
}

Str pkix_name_string(PkixName n, Alloc *a) {
    PkixRDNSequence rdns = slice_nil(TYPE_PKIX_RDN_SEQUENCE);
    /* If there are no extra_names, surface the parsed value (all entries in
     * names) instead. */
    if (n.extra_names.p == NULL) {
        const PkixAttributeTypeAndValue *names = n.names.p;
        for (Int i = 0; i < n.names.len; i++) {
            switch (pkix_attr_arc(names[i].type)) {
            case 3:
            case 5:
            case 6:
            case 7:
            case 8:
            case 9:
            case 10:
            case 11:
            case 17:
                /* These attributes were already parsed into named fields. */
                continue;
            default:
                break;
            }
            /* Place non-standard parsed values at the beginning of the
             * sequence so they will be at the end of the string. See Go
             * issue 39924. */
            Slice one = slice_append(a, slice_nil(TYPE_PKIX_ATTRIBUTE_TYPE_AND_VALUE),
                                     &names[i], 1);
            if (one.len != 1 || !pkix_push_rdn(a, &rdns, one))
                return BURROW_STR_EMPTY;
        }
    }
    PkixRDNSequence more = pkix_name_to_rdn_sequence(n, a);
    if (more.len > 0) {
        Slice grown = slice_append(a, rdns, more.p, more.len);
        if (grown.len != rdns.len + more.len)
            return BURROW_STR_EMPTY;
        rdns = grown;
    }
    return pkix_rdn_sequence_string(rdns, a);
}

/* -------------------------------------------------------- CertificateList */

bool pkix_certificate_list_has_expired(const PkixCertificateList *cert_list, Time now) {
    return !time_before(now, cert_list->tbs_cert_list.next_update);
}
