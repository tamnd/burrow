/* crypto/x509/pkix tests. pkix has no tests of its own in Go. These are the
 * ones in x509_test.go that test it: TestPKIXNameString, TestRDNSequenceString,
 * TestParseDERCRL and TestCRLWithoutExpiry, with ParseDERCRL being what it is
 * in Go, asn1.Unmarshal into a CertificateList. The certificate subject in
 * TestPKIXNameString comes from a small struct for the start of a certificate
 * rather than from x509.ParseCertificates.
 *
 * Copyright 2009 The Go Authors. All rights reserved.
 * Copyright 2026 The burrow Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style licence that can be found
 * in the LICENSE file. */

#include "burrow/crypto/x509/pkix.h"

#include "burrow/declare.h"
#include "burrow/encoding/asn1.h"
#include "burrow/error.h"
#include "burrow/mem/arena.h"
#include "burrow/testing.h"

#include "check.h"
#include "pkix_test_data.h"

#include <string.h>

static const char *cz(Alloc *a, Str s) {
    char *p = mem_alloc_nozero(a, (size_t)s.len + 1, 1);
    if (s.len > 0)
        memcpy(p, s.p, (size_t)s.len);
    p[s.len] = 0;
    return p;
}

static Slice bytes_of(const Byte *p, Int n) {
    return (Slice){(void *)(uintptr_t)p, n, n, TYPE_BYTE};
}

/* A Str in a, for an Any to point at. */
static Any str_any(Alloc *a, const char *s) {
    Str *p = mem_alloc(a, sizeof(Str), _Alignof(Str));
    *p = str_from_cstr(s);
    return BURROW_ANY(TYPE_STRING, p);
}

static PkixAttributeTypeAndValue atv(Asn1ObjectIdentifier t, Any v) {
    return (PkixAttributeTypeAndValue){t, v};
}

static Slice atvs(Alloc *a, const PkixAttributeTypeAndValue *v, Int n) {
    return slice_append(a, slice_nil(TYPE_PKIX_ATTRIBUTE_TYPE_AND_VALUE), v, n);
}

#define ATVS(a, ...)                                                                   \
    atvs((a), (const PkixAttributeTypeAndValue[]){__VA_ARGS__},                        \
         (Int)(sizeof((const PkixAttributeTypeAndValue[]){__VA_ARGS__}) /              \
               sizeof(PkixAttributeTypeAndValue)))

static Slice strs(Alloc *a, const char *s) {
    Str v = str_from_cstr(s);
    return slice_append(a, slice_nil(TYPE_STRING), &v, 1);
}

/* The start of a certificate, which is as far as the subject. Go allows
 * elements after the last field of a struct, and so does encoding/asn1 here,
 * so the rest of the certificate is skipped. */
#define TBS_FIELDS(F, T)                                                               \
    F(T, Asn1RawContent, raw, Raw, "")                                                 \
    F(T, Int, version, Version, "asn1:\"optional,explicit,default:0,tag:0\"")          \
    F(T, Asn1RawValue, serial_number, SerialNumber, "")                                \
    F(T, PkixAlgorithmIdentifier, signature_algorithm, SignatureAlgorithm, "")         \
    F(T, PkixRDNSequence, issuer, Issuer, "")                                          \
    F(T, Asn1RawValue, validity, Validity, "")                                         \
    F(T, PkixRDNSequence, subject, Subject, "")
BURROW_STRUCT_AS(TbsCertificate, TBS_FIELDS);

#define CERTIFICATE_FIELDS(F, T) F(T, TbsCertificate, tbs, TBSCertificate, "")
BURROW_STRUCT_AS(Certificate, CERTIFICATE_FIELDS);

static void TestPKIXNameString(TestingT *t) {
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);

    Certificate cert = {0};
    Error err = BURROW_NO_ERROR;
    asn1_unmarshal(a, bytes_of(cert_bytes, (Int)sizeof cert_bytes),
                   BURROW_ANY(TYPE_OF(Certificate), &cert), &err);
    if (BURROW_FAILED(err))
        testing_t_fatalf_v(t, "%v", err);
    PkixName subject = {0};
    CHECK(pkix_name_fill_from_rdn_sequence(&subject, a, &cert.tbs.subject));

    /* Check that parsed non-standard attributes are printed. */
    PkixName with_extra = {0};
    with_extra.locality = strs(a, "Gophertown");
    with_extra.extra_names =
        ATVS(a, atv(ASN1_OID(1, 2, 3, 4, 5), str_any(a, "golang.org")));
    PkixRDNSequence rdns = pkix_name_to_rdn_sequence(with_extra, a);
    PkixName nn = {0};
    CHECK(pkix_name_fill_from_rdn_sequence(&nn, a, &rdns));

    /* Check that zero-length non-nil ExtraNames hide Names. */
    Slice extra = ATVS(a, atv(ASN1_OID(1, 2, 3, 4, 5), str_any(a, "backing array")));
    PkixName extra_not_nil = {0};
    extra_not_nil.locality = strs(a, "Gophertown");
    extra_not_nil.extra_names = slice_sub(extra, 0, 0);
    extra_not_nil.names =
        ATVS(a, atv(ASN1_OID(1, 2, 3, 4, 5), str_any(a, "golang.org")));

    PkixName kille = {0};
    kille.common_name = BURROW_S("Steve Kille");
    kille.organization = strs(a, "Isode Limited");
    kille.organizational_unit = strs(a, "RFCs");
    kille.locality = strs(a, "Richmond");
    kille.province = strs(a, "Surrey");
    kille.street_address = strs(a, "The Square");
    kille.postal_code = strs(a, "TW9 1DT");
    kille.serial_number = BURROW_S("RFC 2253");
    kille.country = strs(a, "GB");

    PkixName escapes = {0};
    escapes.organization = strs(a, "#Google, Inc. \n-> 'Alphabet\" ");
    escapes.country = strs(a, "US");

    PkixName override = {0};
    override.common_name = BURROW_S("foo.com");
    override.organization = strs(a, "Gopher Industries");
    override.extra_names = ATVS(a, atv(ASN1_OID(2, 5, 4, 3), str_any(a, "bar.com")));

    PkixName extra_first = {0};
    extra_first.locality = strs(a, "Gophertown");
    extra_first.extra_names =
        ATVS(a, atv(ASN1_OID(1, 2, 3, 4, 5), str_any(a, "golang.org")));

    /* If there are no ExtraNames, the Names are printed instead. */
    PkixName names_only = {0};
    names_only.locality = strs(a, "Gophertown");
    names_only.names = ATVS(a, atv(ASN1_OID(1, 2, 3, 4, 5), str_any(a, "golang.org")));

    /* If there are both, print only the ExtraNames. */
    PkixName both = {0};
    both.locality = strs(a, "Gophertown");
    both.extra_names = ATVS(a, atv(ASN1_OID(1, 2, 3, 4, 5), str_any(a, "golang.org")));
    both.names = ATVS(a, atv(ASN1_OID(1, 2, 3, 4, 6), str_any(a, "example.com")));

    /* Non-string value falls back to hex-encoded DER (issue 33093). */
    Int forty_two = 42;
    PkixName non_string = {0};
    non_string.common_name = BURROW_S("foobar");
    non_string.extra_names =
        ATVS(a, atv(ASN1_OID(1, 2, 3, 4), BURROW_ANY(TYPE_INT, &forty_two)));

    /* String containing non-PrintableString chars (here, UTF-8) is still
     * rendered as a string per RFC 2253 section 2.4, not hex-encoded. */
    PkixName utf8 = {0};
    utf8.common_name = BURROW_S("foobar");
    utf8.extra_names =
        ATVS(a, atv(ASN1_OID(2, 3, 4, 5), str_any(a, "Lu\xc4\x8di\xc4\x87")));

    /* String beginning with '#' has the '#' escaped (RFC 2253 section 2.4). */
    PkixName hash = {0};
    hash.common_name = BURROW_S("foobar");
    hash.extra_names = ATVS(a, atv(ASN1_OID(2, 3, 4, 5), str_any(a, "#abcdef")));

    /* Printable string with an embedded RFC 2253 escapable character. */
    PkixName comma = {0};
    comma.common_name = BURROW_S("foobar");
    comma.extra_names = ATVS(a, atv(ASN1_OID(2, 3, 4, 5), str_any(a, "abcdef,GHI")));

    /* Printable string with leading and trailing space gets escaped. */
    PkixName spaces = {0};
    spaces.common_name = BURROW_S("foobar");
    spaces.extra_names = ATVS(a, atv(ASN1_OID(2, 3, 4, 5), str_any(a, "   abcdef ")));

    struct {
        PkixName dn;
        const char *want;
    } tests[] = {
        {nn, "L=Gophertown,1.2.3.4.5=golang.org"},
        {extra_not_nil, "L=Gophertown"},
        {kille, "SERIALNUMBER=RFC 2253,CN=Steve Kille,OU=RFCs,O=Isode Limited,"
                "POSTALCODE=TW9 1DT,STREET=The Square,L=Richmond,ST=Surrey,C=GB"},
        {subject, "CN=mail.google.com,O=Google LLC,L=Mountain View,ST=California,C=US"},
        {escapes, "O=\\#Google\\, Inc. \n-\\> 'Alphabet\\\"\\ ,C=US"},
        {override, "CN=bar.com,O=Gopher Industries"},
        {extra_first, "1.2.3.4.5=golang.org,L=Gophertown"},
        {names_only, "L=Gophertown,1.2.3.4.5=golang.org"},
        {both, "1.2.3.4.5=golang.org,L=Gophertown"},
        {non_string, "1.2.3.4=#02012a,CN=foobar"},
        {utf8, "2.3.4.5=Lu\xc4\x8di\xc4\x87,CN=foobar"},
        {hash, "2.3.4.5=\\#abcdef,CN=foobar"},
        {comma, "2.3.4.5=abcdef\\,GHI,CN=foobar"},
        {spaces, "2.3.4.5=\\   abcdef\\ ,CN=foobar"},
    };
    for (size_t i = 0; i < sizeof tests / sizeof tests[0]; i++) {
        const char *got = cz(a, pkix_name_string(tests[i].dn, a));
        if (strcmp(got, tests[i].want) != 0)
            testing_t_errorf_v(t, "#%d: String() = \n%s\n, want \n%s", (int)i, got,
                               tests[i].want);
    }

    const PkixAttributeTypeAndValue *e = extra.p;
    if (strcmp(cz(a, *(const Str *)e[0].value.data), "backing array") != 0)
        testing_t_errorf_v(t,
                           "the backing array of an empty ExtraNames got modified by "
                           "String");
    arena_free(&ar);
}

static void TestRDNSequenceString(TestingT *t) {
    /* Test some extra cases that get lost in pkix.Name conversions such as
     * multi-valued attributes. */
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    Asn1ObjectIdentifier oid_country = ASN1_OID(2, 5, 4, 6);
    Asn1ObjectIdentifier oid_organization = ASN1_OID(2, 5, 4, 10);
    Asn1ObjectIdentifier oid_organizational_unit = ASN1_OID(2, 5, 4, 11);
    Asn1ObjectIdentifier oid_common_name = ASN1_OID(2, 5, 4, 3);

    PkixRelativeDistinguishedNameSET sets[3] = {
        ATVS(a, atv(oid_country, str_any(a, "US"))),
        ATVS(a, atv(oid_organization, str_any(a, "Widget Inc."))),
        ATVS(a, atv(oid_organizational_unit, str_any(a, "Sales")),
             atv(oid_common_name, str_any(a, "J. Smith"))),
    };
    PkixRDNSequence seq = {sets, 3, 3, TYPE_PKIX_RELATIVE_DISTINGUISHED_NAME_SET};
    const char *got = cz(a, pkix_rdn_sequence_string(seq, a));
    const char *want = "OU=Sales+CN=J. Smith,O=Widget Inc.,C=US";
    if (strcmp(got, want) != 0)
        testing_t_errorf_v(t, "#0: String() = \n%s\n, want \n%s", got, want);
    arena_free(&ar);
}

/* x509.ParseDERCRL */
static bool parse_der_crl(Alloc *a, Slice der, PkixCertificateList *out, Error *err) {
    *out = (PkixCertificateList){0};
    Slice rest =
        asn1_unmarshal(a, der, BURROW_ANY(TYPE_PKIX_CERTIFICATE_LIST, out), err);
    if (BURROW_FAILED(*err))
        return false;
    if (rest.len != 0) {
        *err = errors_new(a, BURROW_S("x509: trailing data after CRL"));
        return false;
    }
    return true;
}

static void TestParseDERCRL(TestingT *t) {
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    PkixCertificateList cert_list;
    Error err = BURROW_NO_ERROR;
    if (!parse_der_crl(a, bytes_of(der_crl, (Int)sizeof der_crl), &cert_list, &err)) {
        testing_t_errorf_v(t, "error parsing: %v", err);
        arena_free(&ar);
        return;
    }
    Int num_certs = cert_list.tbs_cert_list.revoked_certificates.len;
    Int expected = 88;
    if (num_certs != expected)
        testing_t_errorf_v(t, "bad number of revoked certificates. got: %d want: %d",
                           (int)num_certs, (int)expected);

    if (pkix_certificate_list_has_expired(&cert_list, time_from_unix(1302517272, 0)))
        testing_t_errorf_v(t, "CRL has expired (but shouldn't have)");

    /* Not in Go: the parts this package names. */
    CHECK_STR_EQ(cz(a, asn1_object_identifier_string(
                           cert_list.signature_algorithm.algorithm, a)),
                 "1.2.840.113549.1.1.5");
    CHECK_STR_EQ(cz(a, pkix_rdn_sequence_string(cert_list.tbs_cert_list.issuer, a)),
                 "C=IT,OU=FINMECCANICA,O=FINMECCANICA,CN=PKI FINMECCANICA");
    CHECK_INT_EQ(cert_list.tbs_cert_list.version, 1);
    /* The TBS list is the SEQUENCE at offset 4, 0x0c93 bytes and a 4 byte
     * header. */
    CHECK(cert_list.tbs_cert_list.raw.p == der_crl + 4);
    CHECK_INT_EQ(cert_list.tbs_cert_list.raw.len, 0x0c93 + 4);
    CHECK(pkix_certificate_list_has_expired(&cert_list, time_from_unix(1304542662, 0)));
    CHECK(
        !pkix_certificate_list_has_expired(&cert_list, time_from_unix(1304542661, 0)));

    /* The raw TBS is written back as it was, so the whole list comes out the
     * same. */
    Slice again =
        asn1_marshal(a, BURROW_ANY(TYPE_PKIX_CERTIFICATE_LIST, &cert_list), &err);
    CHECK(BURROW_OK(err));
    CHECK(again.len == (Int)sizeof der_crl &&
          memcmp(again.p, der_crl, sizeof der_crl) == 0);

    Slice trailing =
        slice_append(a, slice_nil(TYPE_BYTE), der_crl, (Int)sizeof der_crl);
    Byte zero = 0;
    trailing = slice_append(a, trailing, &zero, 1);
    CHECK(!parse_der_crl(a, trailing, &cert_list, &err));
    CHECK_STR_EQ(cz(a, error_text(err)), "x509: trailing data after CRL");
    arena_free(&ar);
}

static void TestCRLWithoutExpiry(TestingT *t) {
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    PkixCertificateList cert_list;
    Error err = BURROW_NO_ERROR;
    if (!parse_der_crl(a, bytes_of(crl_without_expiry, (Int)sizeof crl_without_expiry),
                       &cert_list, &err))
        testing_t_fatalf_v(t, "%v", err);
    if (!time_is_zero(cert_list.tbs_cert_list.next_update))
        testing_t_errorf_v(t, "NextUpdate is not the zero value");
    arena_free(&ar);
}

/* Not in Go's tests: a name written with encoding/asn1 and read back. */
static void TestNameRoundTrip(TestingT *t) {
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    PkixName n = {0};
    n.common_name = BURROW_S("example.com");
    n.organization = strs(a, "Example");
    Str two = BURROW_S("Two");
    n.organization = slice_append(a, n.organization, &two, 1);
    n.country = strs(a, "NZ");
    PkixRDNSequence rdns = pkix_name_to_rdn_sequence(n, a);
    CHECK_INT_EQ(rdns.len, 3);

    Error err = BURROW_NO_ERROR;
    Slice der = asn1_marshal(a, BURROW_ANY(TYPE_PKIX_RDN_SEQUENCE, &rdns), &err);
    if (BURROW_FAILED(err))
        testing_t_fatalf_v(t, "%v", err);
    PkixRDNSequence back = slice_nil(TYPE_PKIX_RELATIVE_DISTINGUISHED_NAME_SET);
    asn1_unmarshal(a, der, BURROW_ANY(TYPE_PKIX_RDN_SEQUENCE, &back), &err);
    if (BURROW_FAILED(err))
        testing_t_fatalf_v(t, "%v", err);
    PkixName m = {0};
    CHECK(pkix_name_fill_from_rdn_sequence(&m, a, &back));
    /* DER sorts a SET OF by encoding, so the two O values come back swapped,
     * the same as in Go. */
    CHECK_STR_EQ(cz(a, pkix_name_string(m, a)), "CN=example.com,O=Two+O=Example,C=NZ");
    CHECK_STR_EQ(cz(a, pkix_name_string(n, a)), "CN=example.com,O=Example+O=Two,C=NZ");
    CHECK_INT_EQ(m.names.len, 4);
    CHECK_INT_EQ(m.organization.len, 2);
    CHECK_STR_EQ(cz(a, m.common_name), "example.com");
    CHECK_STR_EQ(cz(a, *(const Str *)slice_at(m.organization, 0)), "Two");

    /* An empty name has no RDNs at all. */
    PkixName empty = {0};
    PkixRDNSequence none = pkix_name_to_rdn_sequence(empty, a);
    CHECK(none.p == NULL && none.len == 0);
    CHECK_STR_EQ(cz(a, pkix_name_string(empty, a)), "");

    /* An extension with critical false leaves it out. */
    PkixExtension ext = {ASN1_OID(2, 5, 29, 19), false,
                         slice_append(a, slice_nil(TYPE_BYTE), "\x30\x00", 2)};
    Slice ed = asn1_marshal(a, BURROW_ANY(TYPE_PKIX_EXTENSION, &ext), &err);
    CHECK(BURROW_OK(err));
    CHECK(ed.len == 11 &&
          memcmp(ed.p, "\x30\x09\x06\x03\x55\x1d\x13\x04\x02\x30\x00", 11) == 0);
    arena_free(&ar);
}

#define TESTS(X)                                                                       \
    X(TestPKIXNameString)                                                              \
    X(TestRDNSequenceString)                                                           \
    X(TestParseDERCRL)                                                                 \
    X(TestCRLWithoutExpiry)                                                            \
    X(TestNameRoundTrip)

TESTING_MAIN(TESTS)
