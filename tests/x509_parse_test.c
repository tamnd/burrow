/* crypto/x509 tests for parsing certificates, CSRs and CRLs: Go's
 * parser_test.go and the tests of x509_test.go that read fixed certificates,
 * CSRs and CRLs.
 *
 * The tests that make a certificate first with CreateCertificate come with
 * certificate creation. TestDomainNameValid leaves out the comparison with
 * domainToReverseLabels, which comes with verification, and TestCertificateParse
 * the VerifyHostname call for the same reason. TestParsePolicies reads the two
 * certificates it uses from testdata through the generated header.
 *
 * Copyright 2009 The Go Authors. All rights reserved.
 * Copyright 2026 The burrow Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style licence that can be found
 * in the LICENSE file. */

#include "burrow/crypto/x509.h"

#include "burrow/bytes.h"
#include "burrow/crypto/dsa.h"
#include "burrow/crypto/ed25519.h"
#include "burrow/crypto/rsa.h"
#include "burrow/crypto/x509/pkix.h"
#include "burrow/encoding/asn1.h"
#include "burrow/encoding/base64.h"
#include "burrow/encoding/hex.h"
#include "burrow/encoding/pem.h"
#include "burrow/error.h"
#include "burrow/math/big.h"
#include "burrow/mem/arena.h"
#include "burrow/strings.h"
#include "burrow/testing.h"
#include "burrow/time.h"

#include "../src/crypto/cryptobyte.h"
#include "../src/crypto/x509_internal.h"
#include "check.h"
#include "x509_parse_test_gen.h"

#include <stdint.h>
#include <string.h>

static Slice cbytes(const char *s) {
    Int n = (Int)strlen(s);
    return (Slice){(void *)(uintptr_t)s, n, n, TYPE_BYTE};
}

static Slice hexb(Alloc *a, const char *s) {
    Error err = BURROW_NO_ERROR;
    Slice b = hex_decode_string(a, str_from_cstr(s), &err);
    if (BURROW_FAILED(err))
        panic_str(BURROW_S("x509_parse_test: bad hex"));
    return b;
}

/* fromBase64 */
static Slice from_base64(Alloc *a, const char *s) {
    Error err = BURROW_NO_ERROR;
    Slice b =
        base64_encoding_decode_string(base64_std_encoding, a, str_from_cstr(s), &err);
    if (BURROW_FAILED(err))
        panic_str(BURROW_S("x509_parse_test: bad base64"));
    return b;
}

/* The pieces of a long generated string joined up. */
static const char *joined(Alloc *a, const char *const *parts) {
    size_t n = 0;
    for (const char *const *p = parts; *p != NULL; p++)
        n += strlen(*p);
    char *s = mem_alloc_nozero(a, n + 1, 1);
    n = 0;
    for (const char *const *p = parts; *p != NULL; p++) {
        size_t l = strlen(*p);
        memcpy(s + n, *p, l);
        n += l;
    }
    s[n] = 0;
    return s;
}

/* The bytes of the first PEM block in s. */
static Slice pem_bytes(Alloc *a, const char *s) {
    Slice rest;
    PemBlock *b = pem_decode(a, cbytes(s), &rest);
    if (b == NULL)
        panic_str(BURROW_S("x509_parse_test: no PEM block"));
    return b->bytes;
}

static BigInt *from_hex(Alloc *a, const char *s) {
    bool ok = false;
    BigInt *z = big_int_set_string(big_new_int(a, 0), str_from_cstr(s), 16, &ok);
    if (!ok)
        panic_str(BURROW_S("x509_parse_test: bad hex number"));
    return z;
}

static Str cat(Alloc *a, Str x, Str y) {
    Byte *p = mem_alloc_nozero(a, (size_t)(x.len + y.len), 1);
    memcpy(p, x.p, (size_t)x.len);
    memcpy(p + x.len, y.p, (size_t)y.len);
    return (Str){p, x.len + y.len};
}

static bool contains(Error err, const char *want) {
    return BURROW_FAILED(err) && strings_contains(error_text(err), str_from_cstr(want));
}

static bool text_is(Error err, const char *want) {
    return BURROW_FAILED(err) && str_eq(error_text(err), str_from_cstr(want));
}

/* ParseCertificate of the first PEM block in s. */
static X509Certificate *parse_pem(Alloc *a, const char *s, Error *err) {
    return x509_parse_certificate(a, pem_bytes(a, s), err);
}

#define ARENA(a)                                                                       \
    Arena ar;                                                                          \
    arena_init(&ar, NULL, 0);                                                          \
    Alloc *a = arena_allocator(&ar)

/* ---------------------------------------------------------- parser_test.go */

static void TestParseASN1String(TestingT *t) {
    static const struct {
        const char *name;
        uint8_t tag;
        const char *value;
        Int value_len;
        const char *expected;
        const char *expected_err;
    } tests[] = {
        {"T61String", CRYPTOBYTE_ASN1_T61_STRING, "\xbf\x61\x3f", 3,
         "\xc2\xbf"
         "a?",
         NULL},
        {"PrintableString", CRYPTOBYTE_ASN1_PRINTABLE_STRING, "PQR", 3, "PQR", NULL},
        {"PrintableString (invalid)", CRYPTOBYTE_ASN1_PRINTABLE_STRING, "\x01\x02\x03",
         3, "", "invalid PrintableString"},
        {"UTF8String", CRYPTOBYTE_ASN1_UTF8_STRING, "PQR", 3, "PQR", NULL},
        {"UTF8String (invalid)", CRYPTOBYTE_ASN1_UTF8_STRING, "\xff", 1, "",
         "invalid UTF-8 string"},
        {"BMPString", 30, "PQ", 2, "\xe5\x81\x91", NULL},
        {"BMPString (invalid length)", 30, "\xff", 1, "", "invalid BMPString"},
        {"BMPString (invalid surrogate)", 30, "PQ\xd8\x01", 4, "", "invalid BMPString"},
        {"BMPString (invalid noncharacter 0xfdd1)", 30, "PQ\xfd\xd1", 4, "",
         "invalid BMPString"},
        {"BMPString (invalid noncharacter 0xffff)", 30, "PQ\xff\xff", 4, "",
         "invalid BMPString"},
        {"BMPString (invalid noncharacter 0xfffe)", 30, "PQ\xff\xfe", 4, "",
         "invalid BMPString"},
        {"IA5String", CRYPTOBYTE_ASN1_IA5_STRING, "PQ", 2, "PQ", NULL},
        {"IA5String (invalid)", CRYPTOBYTE_ASN1_IA5_STRING, "\xff", 1, "",
         "invalid IA5String"},
        {"NumericString", 18, "12", 2, "12", NULL},
        {"NumericString (invalid)", 18, "P", 1, "", "invalid NumericString"},
    };
    ARENA(a);
    for (size_t i = 0; i < sizeof tests / sizeof tests[0]; i++) {
        Slice value = {(void *)(uintptr_t)tests[i].value, tests[i].value_len,
                       tests[i].value_len, TYPE_BYTE};
        Str out = BURROW_STR_EMPTY;
        Error err = burrow__x509_parse_asn1_string(a, tests[i].tag, value, &out);
        if (BURROW_FAILED(err) &&
            (tests[i].expected_err == NULL || !text_is(err, tests[i].expected_err)))
            testing_t_errorf_v(t, "%s: parseASN1String returned unexpected error: %v",
                               tests[i].name, err);
        else if (!BURROW_FAILED(err) && tests[i].expected_err != NULL)
            testing_t_errorf_v(t, "%s: parseASN1String didn't fail, expected: %s",
                               tests[i].name, tests[i].expected_err);
        if (!str_eq(out, str_from_cstr(tests[i].expected)))
            testing_t_errorf_v(t,
                               "%s: parseASN1String returned unexpected value: got %q",
                               tests[i].name, out);
    }
    arena_free(&ar);
}

static void TestPolicyParse(TestingT *t) {
    ARENA(a);
    Error err = BURROW_NO_ERROR;
    X509Certificate *c = parse_pem(a, policy_pem, &err);
    if (BURROW_FAILED(err)) {
        testing_t_errorf_v(t, "%v", err);
        arena_free(&ar);
        return;
    }
    if (c->policies.len != 9)
        testing_t_errorf_v(t, "unexpected number of policies: got %d, want %d",
                           c->policies.len, 9);
    if (c->policy_mappings.len != 9)
        testing_t_errorf_v(t, "unexpected number of policy mappings: got %d, want %d",
                           c->policy_mappings.len, 9);
    if (!c->require_explicit_policy_zero)
        testing_t_errorf_v(t, "expected RequireExplicitPolicyZero to be set");
    if (!c->inhibit_policy_mapping_zero)
        testing_t_errorf_v(t, "expected InhibitPolicyMappingZero to be set");
    if (!c->inhibit_any_policy_zero)
        testing_t_errorf_v(t, "expected InhibitAnyPolicyZero to be set");
    arena_free(&ar);
}

static void TestParsePolicies(TestingT *t) {
    const char *const certs[] = {policy_leaf_duplicate_pem, policy_leaf_invalid_pem};
    ARENA(a);
    for (size_t i = 0; i < 2; i++) {
        Error err = BURROW_NO_ERROR;
        if (parse_pem(a, certs[i], &err) != NULL || !BURROW_FAILED(err))
            testing_t_errorf_v(t, "#%d: parsing should've failed", (Int)i);
    }
    arena_free(&ar);
}

static void TestParseCertificateNegativeMaxPathLength(TestingT *t) {
    const char *const certs[] = {negative_max_path_len_0, negative_max_path_len_1};
    ARENA(a);
    for (size_t i = 0; i < 2; i++) {
        Error err = BURROW_NO_ERROR;
        (void)parse_pem(a, certs[i], &err);
        if (!text_is(err, "x509: invalid basic constraints"))
            testing_t_errorf_v(
                t,
                "ParseCertificate() = %v; want = \"x509: invalid basic constraints\"",
                err);
    }
    arena_free(&ar);
}

static void TestDomainNameValid(TestingT *t) {
    ARENA(a);
    /* strings.Repeat("a.a", 84) + "aaa" and the rest. */
    Str rep84 = strings_repeat(a, BURROW_S("a.a"), 84);
    Str a63 = strings_repeat(a, BURROW_S("a"), 63);
    Str a64 = strings_repeat(a, BURROW_S("a"), 64);
    struct {
        const char *name;
        Str dns_name;
        bool constraint;
        bool valid;
    } tests[] = {
        {"254 char label, name", cat(a, rep84, BURROW_S("aaa")), false, true},
        {"254 char label, constraint", cat(a, rep84, BURROW_S("aaa")), true, true},
        {"253 char label, name", cat(a, rep84, BURROW_S("aa")), false, true},
        {"253 char label, constraint", cat(a, rep84, BURROW_S("aa")), true, true},
        {"64 char single label, name", a64, false, true},
        {"64 char single label, constraint", a64, true, true},
        {"64 char label, name", cat(a, BURROW_S("a."), a64), false, true},
        {"64 char label, constraint", cat(a, BURROW_S("a."), a64), true, true},
        {"empty name, constraint", BURROW_S(""), true, true},
        {"empty label, name", BURROW_S("a..a"), false, false},
        {"empty label, constraint", BURROW_S("a..a"), true, false},
        {"period, name", BURROW_S("."), false, false},
        {"period, constraint", BURROW_S("."), true, false},
        {"valid, name", BURROW_S("a.b.c"), false, true},
        {"valid, constraint", BURROW_S("a.b.c"), true, true},
        {"leading period, name", BURROW_S(".a.b.c"), false, false},
        {"leading period, constraint", BURROW_S(".a.b.c"), true, true},
        {"trailing period, name", BURROW_S("a."), false, false},
        {"trailing period, constraint", BURROW_S("a."), true, false},
        {"bare label, name", BURROW_S("a"), false, true},
        {"bare label, constraint", BURROW_S("a"), true, true},
        {"63 char single label, name", a63, false, true},
        {"63 char single label, constraint", a63, true, true},
        {"63 char label, name", cat(a, BURROW_S("a."), a63), false, true},
        {"63 char label, constraint", cat(a, BURROW_S("a."), a63), true, true},
    };
    for (size_t i = 0; i < sizeof tests / sizeof tests[0]; i++) {
        bool valid =
            burrow__x509_domain_name_valid(tests[i].dns_name, tests[i].constraint);
        if (valid != tests[i].valid)
            testing_t_errorf_v(t, "%s: domainNameValid(%q, %t) = %t; want %t",
                               tests[i].name, tests[i].dns_name, tests[i].constraint,
                               valid, tests[i].valid);
    }
    arena_free(&ar);
}

static bool bytes_are(Slice b, const char *want, Int n) {
    return b.len == n && memcmp(b.p, want, (size_t)n) == 0;
}

static void TestParseNameTypes(TestingT *t) {
    ARENA(a);
    Error err = BURROW_NO_ERROR;
    X509Certificate *cert = parse_pem(a, name_types_pem, &err);
    if (BURROW_FAILED(err)) {
        testing_t_errorf_v(t, "ParseCertificate failed: %v", err);
        arena_free(&ar);
        return;
    }
    PkixAttributeTypeAndValue *names = cert->subject.names.p;
    for (Int i = 0; i < cert->subject.names.len; i++) {
        Str oid = asn1_object_identifier_string(names[i].type, a);
        Any v = names[i].value;
        const Int *arcs = names[i].type.p;
        Int last = names[i].type.len == 9 ? arcs[8] : 0;
        bool ok = false;
        if (!strings_has_prefix(oid, BURROW_S("1.2.840.113554.4.1.72585.2.")))
            last = 0;
        switch (last) {
        case 1:
            ok = v.t == TYPE_STRING &&
                 str_eq(*(Str *)v.data, BURROW_S("utf8-string\xf0\x9f\xa6\x8a"));
            break;
        case 2:
            ok = v.t == TYPE_BYTES && bytes_are(*(Slice *)v.data, "octet-string", 12);
            break;
        case 3: {
            const Asn1RawValue *rv = v.data;
            ok = v.t == TYPE_ASN1_RAW_VALUE && rv->cls == 0 && rv->tag == 13 &&
                 !rv->is_compound && bytes_are(rv->bytes, "\x01\x02\x03\x04\x05", 5) &&
                 bytes_are(rv->full_bytes, "\x0d\x05\x01\x02\x03\x04\x05", 7);
            break;
        }
        case 4:
            ok = v.t == NULL;
            break;
        case 5: {
            const Asn1RawValue *rv = v.data;
            ok = v.t == TYPE_ASN1_RAW_VALUE && rv->cls == 0 && rv->tag == 16 &&
                 rv->is_compound && bytes_are(rv->bytes, "\x01\x02", 2) &&
                 bytes_are(rv->full_bytes, "\x30\x02\x01\x02", 4);
            break;
        }
        case 6:
            ok = v.t == TYPE_INT64 && *(int64_t *)v.data == 42;
            break;
        case 7:
            ok = v.t == TYPE_ASN1_OBJECT_IDENTIFIER &&
                 asn1_object_identifier_equal(*(Asn1ObjectIdentifier *)v.data,
                                              names[i].type);
            break;
        case 8: {
            const Asn1BitString *bs = v.data;
            ok = v.t == TYPE_ASN1_BIT_STRING && bs->bit_length == 32 &&
                 bytes_are(bs->bytes, "\xaa\xbb\xcc\xdd", 4);
            break;
        }
        case 9:
            ok = v.t == TYPE_TIME &&
                 time_equal(*(Time *)v.data, time_date(2025, TIME_SEPTEMBER, 2, 18, 43,
                                                       17, 0, time_utc_loc));
            break;
        case 10:
            ok = v.t == TYPE_BOOL && *(bool *)v.data;
            break;
        default:
            testing_t_errorf_v(t, "unexpected attribute type: %s", oid);
            continue;
        }
        if (!ok)
            testing_t_errorf_v(t, "unexpected value for %s", oid);
    }

    /* asn1.Marshal does not encode NULL. */
    PkixName extra;
    memset(&extra, 0, sizeof extra);
    extra.extra_names = cert->subject.names;
    for (Int i = 0; i < extra.extra_names.len; i++)
        if (names[i].value.t == NULL)
            names[i].value = BURROW_ANY(TYPE_ASN1_RAW_VALUE,
                                        (void *)(uintptr_t)&asn1_null_raw_value);
    PkixRDNSequence rdns = pkix_name_to_rdn_sequence(extra, a);
    Slice got = asn1_marshal(a, BURROW_ANY(TYPE_PKIX_RDN_SEQUENCE, &rdns), &err);
    if (BURROW_FAILED(err))
        testing_t_errorf_v(t, "asn1.Marshal failed: %v", err);
    else if (!bytes_equal(got, cert->raw_subject))
        testing_t_errorf_v(t, "unexpected marshaled RDNSequence: got %x, want %x", got,
                           cert->raw_subject);
    arena_free(&ar);
}

/* ------------------------------------------------------------ x509_test.go */

static void TestCertificateParse(TestingT *t) {
    ARENA(a);
    Error err = BURROW_NO_ERROR;
    Slice certs = x509_parse_certificates(a, from_base64(a, cert_bytes), &err);
    if (BURROW_FAILED(err))
        testing_t_errorf_v(t, "%v", err);
    if (certs.len != 2) {
        testing_t_errorf_v(t, "Wrong number of certs: got %d want 2", certs.len);
        arena_free(&ar);
        return;
    }
    X509Certificate *const *c = certs.p;
    err = x509_certificate_check_signature_from(c[0], c[1]);
    if (BURROW_FAILED(err))
        testing_t_errorf_v(t, "%v", err);
    if (c[0]->extensions.len != 10)
        testing_t_errorf_v(t, "want %d extensions, got %d", (Int)10,
                           c[0]->extensions.len);
    arena_free(&ar);
}

static void TestErrorStrings(TestingT *t) {
    X509ConstraintViolationError cv = {0};
    X509UnhandledCriticalExtension uc = {0};
    if (!str_eq(x509_constraint_violation_error_error(cv),
                error_text(x509_constraint_violation_error)))
        testing_t_errorf_v(t, "ConstraintViolationError.Error() = %q",
                           x509_constraint_violation_error_error(cv));
    if (!str_eq(x509_unhandled_critical_extension_error(uc),
                BURROW_S("x509: unhandled critical extension")))
        testing_t_errorf_v(t, "UnhandledCriticalExtension.Error() = %q",
                           x509_unhandled_critical_extension_error(uc));
    if (errors_as(x509_constraint_violation_error,
                  TYPE_X509_CONSTRAINT_VIOLATION_ERROR) == NULL)
        testing_t_errorf_v(t, "errors.As did not find ConstraintViolationError");
}

static void TestCertificateEqualOnNil(TestingT *t) {
    X509Certificate non_nil;
    memset(&non_nil, 0, sizeof non_nil);
    if (!x509_certificate_equal(NULL, NULL))
        testing_t_errorf_v(t, "Nil certificates: cNil1 is not equal to cNil2");
    if (x509_certificate_equal(NULL, &non_nil))
        testing_t_errorf_v(t, "Unexpectedly cNil1 is equal to cNonNil");
    if (x509_certificate_equal(&non_nil, NULL))
        testing_t_errorf_v(t, "Unexpectedly cNonNil is equal to cNil1");
}

static void TestMismatchedSignatureAlgorithm(TestingT *t) {
    ARENA(a);
    Error err = BURROW_NO_ERROR;
    X509Certificate *cert = parse_pem(a, rsa_pss_self_signed_pem, &err);
    if (BURROW_FAILED(err)) {
        testing_t_errorf_v(t, "%v", err);
        arena_free(&ar);
        return;
    }
    err = x509_certificate_check_signature(cert, X509_ECDSA_WITH_SHA256,
                                           slice_nil(TYPE_BYTE), slice_nil(TYPE_BYTE));
    if (!BURROW_FAILED(err))
        testing_t_errorf_v(t, "CheckSignature unexpectedly return no error");
    else if (!contains(err, " but have public key of type "))
        testing_t_errorf_v(
            t,
            "Expected error containing \" but have public key of type \", "
            "but got %q",
            error_text(err));
    arena_free(&ar);
}

static void TestECDSA(TestingT *t) {
    const char *const certs[] = {ecdsa_sha256_p256_cert_pem, ecdsa_sha256_p384_cert_pem,
                                 ecdsa_sha384_p521_cert_pem};
    ARENA(a);
    for (Int i = 0; i < 3; i++) {
        Error err = BURROW_NO_ERROR;
        X509Certificate *cert = parse_pem(a, certs[i], &err);
        if (BURROW_FAILED(err)) {
            testing_t_errorf_v(t, "%d: failed to parse certificate: %v", i, err);
            continue;
        }
        if (cert->signature_algorithm != ecdsa_test_sig_algo[i])
            testing_t_errorf_v(t, "%d: signature algorithm is %d, want %d", i,
                               cert->signature_algorithm, ecdsa_test_sig_algo[i]);
        if (cert->public_key.t != TYPE_ECDSA_PUBLIC_KEY)
            testing_t_errorf_v(t, "%d: wanted an ECDSA public key", i);
        if (cert->public_key_algorithm != X509_ECDSA)
            testing_t_errorf_v(t, "%d: public key algorithm is %d, want ECDSA", i,
                               cert->public_key_algorithm);
        err = x509_certificate_check_signature_from(cert, cert);
        if (BURROW_FAILED(err))
            testing_t_errorf_v(t, "%d: certificate verification failed: %v", i, err);
    }
    arena_free(&ar);
}

static void TestParseCertificateWithDsaPublicKey(TestingT *t) {
    ARENA(a);
    DsaPublicKey want = {
        {from_hex(a,
                  "00BC84B52743B169158BB85974E3E832AF5EFCFC42B264349095313A4A013EEE069"
                  "A1B937D92E51ACF297A1C77880DF25C8607D8204B4DC45651305EF4A63B40C7D8C4"
                  "2D91EDA397D8F51CBC9D0A531FE2C6F1E55E9357D205C39D395358968CBEDAC1132"
                  "0C607BE16CB9DB492B6E78163305A34DD99CE43C64927D13A0040EB97"),
         from_hex(a, "009A67067F66A323F5D4EC7902C73FE5D9E36FA74F"),
         from_hex(a,
                  "009147778295BF5893542BC41BA806898A29E43261DBC85441C37D92E97ED80D323"
                  "D44825FDDE8374D0FF15877798812682599B216BBCC31B9DCCAD527465FEAFFD7FC"
                  "2A193612E575E34E7A98AF4D10339FE47390A518CB9975B3160B1D0285D1418D097"
                  "7C52994F43C29A053E3D685834104C9FAFDC221E38BE9F3989D7A8E42")},
        from_hex(a,
                 "59A27C269FCDE45AA2160A5C980C19211A820095091AB9C5DC8309AB7EC1B3A48C2E2"
                 "67C6D35FEE9B71BCBB92F16AC8E559129347FB5C00BEEDD10BA8915C90698755CA965"
                 "735A32DC7575BED806E1E38F768FFBC24E41123DC73F1C6E9E4D0C9E692128853AFE2"
                 "9DC665FA993DCA9C903B7BF00B6442B9A76A5DADC6186317A"),
    };
    Error err = BURROW_NO_ERROR;
    X509Certificate *cert = parse_pem(a, dsa_cert_pem, &err);
    if (BURROW_FAILED(err)) {
        testing_t_errorf_v(t, "Failed to parse certificate: %v", err);
        arena_free(&ar);
        return;
    }
    if (cert->public_key_algorithm != X509_DSA)
        testing_t_errorf_v(t, "Parsed key algorithm was not DSA");
    if (cert->public_key.t != TYPE_DSA_PUBLIC_KEY) {
        testing_t_errorf_v(t, "Parsed key was not a DSA key");
        arena_free(&ar);
        return;
    }
    const DsaPublicKey *got = cert->public_key.data;
    if (big_int_cmp(want.y, got->y) != 0 ||
        big_int_cmp(want.parameters.p, got->parameters.p) != 0 ||
        big_int_cmp(want.parameters.q, got->parameters.q) != 0 ||
        big_int_cmp(want.parameters.g, got->parameters.g) != 0)
        testing_t_errorf_v(t, "Parsed key differs from expected key");
    arena_free(&ar);
}

static void TestParseCertificateWithDSASignatureAlgorithm(TestingT *t) {
    ARENA(a);
    Error err = BURROW_NO_ERROR;
    X509Certificate *cert = parse_pem(a, dsa_cert_pem, &err);
    if (BURROW_FAILED(err))
        testing_t_errorf_v(t, "Failed to parse certificate: %v", err);
    else if (cert->signature_algorithm != X509_DSA_WITH_SHA1)
        testing_t_errorf_v(t, "Parsed signature algorithm was not DSAWithSHA1");
    arena_free(&ar);
}

static void TestVerifyCertificateWithDSASignature(TestingT *t) {
    ARENA(a);
    Error err = BURROW_NO_ERROR;
    X509Certificate *cert = parse_pem(a, dsa_cert_pem, &err);
    if (BURROW_FAILED(err))
        testing_t_errorf_v(t, "Failed to parse certificate: %v", err);
    /* The test certificate is self-signed. */
    else if (!BURROW_FAILED(x509_certificate_check_signature_from(cert, cert)))
        testing_t_errorf_v(t, "Expected error verifying DSA certificate");
    arena_free(&ar);
}

static void TestRSAPSSSelfSigned(TestingT *t) {
    const char *const certs[] = {rsa_pss_self_signed_pem,
                                 rsa_pss_self_signed_openssl110_pem};
    ARENA(a);
    for (Int i = 0; i < 2; i++) {
        Error err = BURROW_NO_ERROR;
        X509Certificate *cert = parse_pem(a, certs[i], &err);
        if (BURROW_FAILED(err)) {
            testing_t_errorf_v(t, "#%d: failed to parse: %v", i, err);
            continue;
        }
        err = x509_certificate_check_signature_from(cert, cert);
        if (BURROW_FAILED(err))
            testing_t_errorf_v(t, "#%d: signature check failed: %v", i, err);
    }
    arena_free(&ar);
}

static void TestEd25519SelfSigned(TestingT *t) {
    ARENA(a);
    Error err = BURROW_NO_ERROR;
    X509Certificate *cert = parse_pem(a, ed25519_certificate, &err);
    if (BURROW_FAILED(err))
        testing_t_errorf_v(t, "Failed to parse: %v", err);
    else if (cert->public_key_algorithm != X509_ED25519)
        testing_t_errorf_v(t, "Parsed key algorithm was not Ed25519");
    else if (cert->public_key.t != TYPE_ED25519_PUBLIC_KEY)
        testing_t_errorf_v(t, "Parsed key was not an Ed25519 key");
    else if (((const Slice *)cert->public_key.data)->len != ED25519_PUBLIC_KEY_SIZE)
        testing_t_errorf_v(t, "Invalid Ed25519 key");
    else if (BURROW_FAILED(err = x509_certificate_check_signature_from(cert, cert)))
        testing_t_errorf_v(t, "Signature check failed: %v", err);
    arena_free(&ar);
}

static void TestParseDERCRL(TestingT *t) {
    ARENA(a);
    Error err = BURROW_NO_ERROR;
    PkixCertificateList *list =
        x509_parse_dercrl(a, from_base64(a, joined(a, der_crl_base64)), &err);
    if (BURROW_FAILED(err)) {
        testing_t_errorf_v(t, "error parsing: %v", err);
    } else {
        if (list->tbs_cert_list.revoked_certificates.len != 88)
            testing_t_errorf_v(t,
                               "bad number of revoked certificates. got: %d want: %d",
                               list->tbs_cert_list.revoked_certificates.len, (Int)88);
        if (pkix_certificate_list_has_expired(list, time_from_unix(1302517272, 0)))
            testing_t_errorf_v(t, "CRL has expired (but shouldn't have)");
    }
    arena_free(&ar);
}

static void TestCRLWithoutExpiry(TestingT *t) {
    ARENA(a);
    Error err = BURROW_NO_ERROR;
    PkixCertificateList *list =
        x509_parse_dercrl(a, from_base64(a, crl_without_expiry_base64), &err);
    if (BURROW_FAILED(err))
        testing_t_errorf_v(t, "%v", err);
    else if (!time_is_zero(list->tbs_cert_list.next_update))
        testing_t_errorf_v(t, "NextUpdate is not the zero value");
    arena_free(&ar);
}

static void TestParsePEMCRL(TestingT *t) {
    ARENA(a);
    Error err = BURROW_NO_ERROR;
    PkixCertificateList *list = x509_parse_crl(a, from_base64(a, pem_crl_base64), &err);
    if (BURROW_FAILED(err)) {
        testing_t_errorf_v(t, "error parsing: %v", err);
    } else {
        if (list->tbs_cert_list.revoked_certificates.len != 2)
            testing_t_errorf_v(t,
                               "bad number of revoked certificates. got: %d want: %d",
                               list->tbs_cert_list.revoked_certificates.len, (Int)2);
        if (pkix_certificate_list_has_expired(list, time_from_unix(1302517272, 0)))
            testing_t_errorf_v(t, "CRL has expired (but shouldn't have)");
    }
    arena_free(&ar);
}

static bool has_extension(Slice exts, Asn1ObjectIdentifier id) {
    const PkixExtension *e = exts.p;
    for (Int i = 0; i < exts.len; i++)
        if (asn1_object_identifier_equal(e[i].id, id))
            return true;
    return false;
}

static const Int oid_basic_constraints[] = {2, 5, 29, 19};
static const Int oid_key_usage[] = {2, 5, 29, 15};

#define OID(arcs)                                                                      \
    ((Asn1ObjectIdentifier){(void *)(uintptr_t)(arcs),                                 \
                            (Int)(sizeof(arcs) / sizeof((arcs)[0])),                   \
                            (Int)(sizeof(arcs) / sizeof((arcs)[0])), TYPE_INT})

static void TestParseCertificateRequest(TestingT *t) {
    const char *const csrs[] = {csr_base64_0, csr_base64_1};
    ARENA(a);
    for (Int i = 0; i < 2; i++) {
        Error err = BURROW_NO_ERROR;
        X509CertificateRequest *csr =
            x509_parse_certificate_request(a, from_base64(a, csrs[i]), &err);
        if (BURROW_FAILED(err)) {
            testing_t_errorf_v(t, "failed to parse CSR: %v", err);
            continue;
        }
        const Str *email = csr->email_addresses.p;
        if (csr->email_addresses.len != 1 ||
            !str_eq(email[0], BURROW_S("gopher@golang.org")))
            testing_t_errorf_v(t, "incorrect email addresses found");
        const Str *dns = csr->dns_names.p;
        if (csr->dns_names.len != 1 || !str_eq(dns[0], BURROW_S("test.example.com")))
            testing_t_errorf_v(t, "incorrect DNS names found");
        const Str *country = csr->subject.country.p;
        if (csr->subject.country.len != 1 || !str_eq(country[0], BURROW_S("AU")))
            testing_t_errorf_v(t, "incorrect Subject name");
        if (!has_extension(csr->extensions, OID(oid_basic_constraints)))
            testing_t_errorf_v(t, "basic constraints extension not found in CSR");
        if (BURROW_FAILED(err = x509_certificate_request_check_signature(csr)))
            testing_t_errorf_v(t, "CheckSignature: %v", err);
    }
    arena_free(&ar);
}

static void TestCriticalFlagInCSRRequestedExtensions(TestingT *t) {
    ARENA(a);
    Error err = BURROW_NO_ERROR;
    X509CertificateRequest *csr =
        x509_parse_certificate_request(a, from_base64(a, critical_csr_base64), &err);
    if (BURROW_FAILED(err)) {
        testing_t_errorf_v(t, "failed to parse CSR: %v", err);
        arena_free(&ar);
        return;
    }
    struct {
        Asn1ObjectIdentifier id;
        Slice value;
    } expected[] = {
        {OID(oid_basic_constraints), from_base64(a, "MAYBAf8CAQA=")},
        {OID(oid_key_usage), from_base64(a, "AwIChA==")},
    };
    if (csr->extensions.len != 2) {
        testing_t_errorf_v(t, "expected to find %d extensions but found %d", (Int)2,
                           csr->extensions.len);
        arena_free(&ar);
        return;
    }
    const PkixExtension *ext = csr->extensions.p;
    for (Int i = 0; i < 2; i++) {
        if (!asn1_object_identifier_equal(ext[i].id, expected[i].id))
            testing_t_errorf_v(t, "extension #%d has unexpected type %s", i,
                               asn1_object_identifier_string(ext[i].id, a));
        if (!bytes_equal(ext[i].value, expected[i].value))
            testing_t_errorf_v(t,
                               "extension #%d has unexpected contents %x (expected %x)",
                               i, ext[i].value, expected[i].value);
    }
    arena_free(&ar);
}

static void TestInsecureAlgorithmErrorString(TestingT *t) {
    static const struct {
        X509SignatureAlgorithm sa;
        const char *want;
    } tests[] = {
        {X509_MD5_WITH_RSA,
         "x509: cannot verify signature: insecure algorithm MD5-RSA"},
        {X509_SHA1_WITH_RSA,
         "x509: cannot verify signature: insecure algorithm SHA1-RSA"},
        {X509_ECDSA_WITH_SHA1,
         "x509: cannot verify signature: insecure algorithm ECDSA-SHA1"},
        {X509_MD2_WITH_RSA, "x509: cannot verify signature: insecure algorithm 1"},
        {-1, "x509: cannot verify signature: insecure algorithm -1"},
        {0, "x509: cannot verify signature: insecure algorithm 0"},
        {9999, "x509: cannot verify signature: insecure algorithm 9999"},
    };
    ARENA(a);
    for (Int i = 0; i < (Int)(sizeof tests / sizeof tests[0]); i++) {
        Str got = x509_insecure_algorithm_error_error(tests[i].sa, a);
        if (!str_eq(got, str_from_cstr(tests[i].want)))
            testing_t_errorf_v(t, "%d. mismatch.\n got: %s\nwant: %s\n", i, got,
                               tests[i].want);
        Error err = x509_insecure_algorithm_error_as_error(tests[i].sa, a);
        if (!str_eq(error_text(err), got))
            testing_t_errorf_v(t, "%d. Error() = %s, want %s", i, error_text(err), got);
    }
    arena_free(&ar);
}

/* TestMD5 and TestSHA1: a self-signed certificate whose signature uses a hash
 * that is no longer trusted. */
static void insecure_self_signed(TestingT *t, const char *pem,
                                 X509SignatureAlgorithm want) {
    ARENA(a);
    Error err = BURROW_NO_ERROR;
    X509Certificate *cert = parse_pem(a, pem, &err);
    if (BURROW_FAILED(err)) {
        testing_t_errorf_v(t, "failed to parse certificate: %v", err);
        arena_free(&ar);
        return;
    }
    if (cert->signature_algorithm != want)
        testing_t_errorf_v(t, "signature algorithm is %d, want %d",
                           cert->signature_algorithm, want);
    err = x509_certificate_check_signature_from(cert, cert);
    if (!BURROW_FAILED(err))
        testing_t_errorf_v(t, "certificate verification succeeded incorrectly");
    else if (errors_as(err, TYPE_X509_INSECURE_ALGORITHM_ERROR) == NULL)
        testing_t_errorf_v(t,
                           "certificate verification returned %v, wanted "
                           "InsecureAlgorithmError",
                           err);
    arena_free(&ar);
}

static void TestMD5(TestingT *t) {
    insecure_self_signed(t, md5_cert, X509_MD5_WITH_RSA);
}

static void TestSHA1(TestingT *t) {
    insecure_self_signed(t, ecdsa_sha1_cert_pem, X509_ECDSA_WITH_SHA1);
}

static void TestRSAMissingNULLParameters(TestingT *t) {
    ARENA(a);
    Error err = BURROW_NO_ERROR;
    if (parse_pem(a, cert_missing_rsa_null, &err) != NULL || !BURROW_FAILED(err))
        testing_t_errorf_v(t,
                           "unexpected success when parsing certificate with missing "
                           "RSA NULL parameter");
    else if (!contains(err, "missing NULL"))
        testing_t_errorf_v(
            t,
            "unrecognised error when parsing certificate with missing RSA "
            "NULL parameter: %v",
            err);
    arena_free(&ar);
}

static void TestISOOIDInCertificate(TestingT *t) {
    ARENA(a);
    Error err = BURROW_NO_ERROR;
    X509Certificate *cert = parse_pem(a, cert_iso_oid, &err);
    if (BURROW_FAILED(err))
        testing_t_errorf_v(t, "certificate with ISO OID failed to parse: %v", err);
    else if (cert->signature_algorithm == X509_UNKNOWN_SIGNATURE_ALGORITHM)
        testing_t_errorf_v(t, "ISO OID not recognised in certificate");
    arena_free(&ar);
}

static void TestMultipleRDN(TestingT *t) {
    ARENA(a);
    Error err = BURROW_NO_ERROR;
    X509Certificate *cert = parse_pem(a, cert_multiple_rdn, &err);
    if (BURROW_FAILED(err)) {
        testing_t_errorf_v(
            t, "certificate with two elements in an RDN failed to parse: %v", err);
    } else {
        if (!str_eq(cert->subject.common_name, BURROW_S("eportal.mss.edus.si")))
            testing_t_errorf_v(t, "got common name of %q", cert->subject.common_name);
        if (!str_eq(cert->subject.serial_number, BURROW_S("1236484010010")))
            testing_t_errorf_v(t, "got serial number of %q",
                               cert->subject.serial_number);
    }
    arena_free(&ar);
}

/* A certificate that must not parse, with want in the error. */
static void parse_fails(TestingT *t, const char *pem, const char *want) {
    ARENA(a);
    Error err = BURROW_NO_ERROR;
    if (parse_pem(a, pem, &err) != NULL || !BURROW_FAILED(err))
        testing_t_errorf_v(t, "unexpected success");
    else if (!contains(err, want))
        testing_t_errorf_v(t, "expected %s in error but got %v", want, err);
    arena_free(&ar);
}

/* A certificate that must parse. */
static X509Certificate *parse_ok(TestingT *t, Alloc *a, const char *pem) {
    Error err = BURROW_NO_ERROR;
    X509Certificate *cert = parse_pem(a, pem, &err);
    if (BURROW_FAILED(err))
        testing_t_errorf_v(t, "failed to parse certificate: %v", err);
    return cert;
}

static void TestEmptyNameConstraints(TestingT *t) {
    parse_fails(t, empty_name_constraints_pem, "empty name constraints");
}

static void TestCriticalNameConstraintWithUnknownType(TestingT *t) {
    ARENA(a);
    X509Certificate *cert =
        parse_ok(t, a, critical_name_constraint_with_unknown_type_pem);
    if (cert != NULL && cert->unhandled_critical_extensions.len != 1)
        testing_t_errorf_v(t, "expected one unhandled critical extension, but found %d",
                           cert->unhandled_critical_extensions.len);
    arena_free(&ar);
}

static void TestBadIPMask(TestingT *t) {
    parse_fails(t, bad_ip_mask_pem, "contained invalid mask");
}

static void TestAdditionFieldsInGeneralSubtree(TestingT *t) {
    ARENA(a);
    (void)parse_ok(t, a, additional_general_subtree_pem);
    arena_free(&ar);
}

static void TestMultipleURLsInCRLDP(TestingT *t) {
    ARENA(a);
    X509Certificate *cert = parse_ok(t, a, multiple_urls_in_crldp_pem);
    if (cert != NULL) {
        const Str *got = cert->crl_distribution_points.p;
        if (cert->crl_distribution_points.len != 2 ||
            !str_eq(got[0], BURROW_S("http://epscd.catcert.net/crl/ec-acc.crl")) ||
            !str_eq(got[1], BURROW_S("http://epscd2.catcert.net/crl/ec-acc.crl")))
            testing_t_errorf_v(t,
                               "CRL distribution points = %d of them, not the two URLs",
                               cert->crl_distribution_points.len);
    }
    arena_free(&ar);
}

static const Int oid_signature_sha256_with_rsa[] = {1, 2, 840, 113549, 1, 1, 11};

static void TestRawSignatureAlgorithm(TestingT *t) {
    ARENA(a);
    X509Certificate *cert = parse_ok(t, a, pem_certificate);
    if (cert != NULL) {
        Slice raw = cert->raw_signature_algorithm;
        PkixAlgorithmIdentifier ai;
        memset(&ai, 0, sizeof ai);
        Error err = BURROW_NO_ERROR;
        Slice rest =
            raw.len == 0
                ? raw
                : asn1_unmarshal(a, raw,
                                 BURROW_ANY(TYPE_PKIX_ALGORITHM_IDENTIFIER, &ai), &err);
        if (raw.len == 0)
            testing_t_errorf_v(t, "RawSignatureAlgorithm is empty");
        else if (BURROW_FAILED(err))
            testing_t_errorf_v(t, "failed to unmarshal RawSignatureAlgorithm: %v", err);
        else if (rest.len != 0)
            testing_t_errorf_v(t, "trailing data after RawSignatureAlgorithm: %x",
                               rest);
        else if (!asn1_object_identifier_equal(ai.algorithm,
                                               OID(oid_signature_sha256_with_rsa)))
            testing_t_errorf_v(t, "unexpected OID: got %s",
                               asn1_object_identifier_string(ai.algorithm, a));
    }
    arena_free(&ar);
}

static void TestParseCertificateRawEquals(TestingT *t) {
    ARENA(a);
    Slice der = pem_bytes(a, pem_certificate);
    Error err = BURROW_NO_ERROR;
    X509Certificate *cert = x509_parse_certificate(a, der, &err);
    if (BURROW_FAILED(err))
        testing_t_errorf_v(t, "failed to parse certificate: %v", err);
    else if (!bytes_equal(der, cert->raw))
        testing_t_errorf_v(t, "unexpected Certificate.Raw\ngot: %x\nwant: %x\n",
                           cert->raw, der);
    arena_free(&ar);
}

static void TestSigAlgMismatch(TestingT *t) {
    const char *const certs[] = {mismatching_sig_alg_id_pem,
                                 mismatching_sig_alg_param_pem};
    ARENA(a);
    for (size_t i = 0; i < 2; i++) {
        Error err = BURROW_NO_ERROR;
        (void)parse_pem(a, certs[i], &err);
        if (!BURROW_FAILED(err))
            testing_t_errorf_v(t, "expected ParseCertificate to fail");
        else if (!text_is(err, "x509: inner and outer signature algorithm identifiers "
                               "don't match"))
            testing_t_errorf_v(t, "unexpected error from ParseCertificate: got %v",
                               err);
    }
    arena_free(&ar);
}

static void TestAuthKeyIdOptional(TestingT *t) {
    ARENA(a);
    (void)parse_ok(t, a, optional_auth_key_id_pem);
    arena_free(&ar);
}

static void TestLargeOID(TestingT *t) {
    ARENA(a);
    (void)parse_ok(t, a, large_oid_pem);
    arena_free(&ar);
}

static void TestParseUniqueID(TestingT *t) {
    ARENA(a);
    X509Certificate *cert = parse_ok(t, a, unique_id_pem);
    if (cert != NULL && cert->extensions.len != 7)
        testing_t_errorf_v(
            t,
            "unexpected number of extensions (probably because the extension "
            "section was not parsed): got %d, want 7",
            cert->extensions.len);
    arena_free(&ar);
}

static void TestParseRevocationList(TestingT *t) {
    ARENA(a);
    Error err = BURROW_NO_ERROR;
    X509RevocationList *list =
        x509_parse_revocation_list(a, from_base64(a, joined(a, der_crl_base64)), &err);
    if (BURROW_FAILED(err))
        testing_t_errorf_v(t, "error parsing: %v", err);
    else if (list->revoked_certificate_entries.len != 88 ||
             list->revoked_certificates.len != 88)
        testing_t_errorf_v(t, "bad number of revoked certificates. got: %d want: %d",
                           list->revoked_certificate_entries.len, (Int)88);
    arena_free(&ar);
}

static void TestParseNegativeSerial(TestingT *t) {
    ARENA(a);
    Error err = BURROW_NO_ERROR;
    if (parse_pem(a, negative_serial_cert, &err) != NULL || !BURROW_FAILED(err))
        testing_t_errorf_v(t, "parsed certificate with negative serial");
    /* x509negativeserial=1 lets it through. */
    burrow__x509_godebug_set("x509negativeserial=1");
    err = BURROW_NO_ERROR;
    X509Certificate *cert = parse_pem(a, negative_serial_cert, &err);
    if (BURROW_FAILED(err))
        testing_t_errorf_v(t, "with x509negativeserial=1: %v", err);
    else if (big_int_sign(cert->serial_number) != -1)
        testing_t_errorf_v(t, "with x509negativeserial=1 the serial is not negative");
    burrow__x509_godebug_set(NULL);
    arena_free(&ar);
}

static void TestDuplicateExtensionsCert(TestingT *t) {
    ARENA(a);
    Error err = BURROW_NO_ERROR;
    if (!BURROW_FAILED((parse_pem(a, dup_ext_cert, &err), err)))
        testing_t_errorf_v(t,
                           "ParseCertificate should fail when parsing certificate with "
                           "duplicate extensions");
    arena_free(&ar);
}

static void TestDuplicateExtensionsCSR(TestingT *t) {
    ARENA(a);
    Error err = BURROW_NO_ERROR;
    (void)x509_parse_certificate_request(a, pem_bytes(a, dup_ext_csr), &err);
    if (!BURROW_FAILED(err))
        testing_t_errorf_v(t,
                           "ParseCertificateRequest should fail when parsing CSR with "
                           "duplicate extensions");
    arena_free(&ar);
}

static void TestDuplicateAttributesCSR(TestingT *t) {
    ARENA(a);
    Error err = BURROW_NO_ERROR;
    (void)x509_parse_certificate_request(a, pem_bytes(a, dup_att_csr), &err);
    if (BURROW_FAILED(err))
        testing_t_errorf_v(
            t,
            "ParseCertificateRequest should succeed when parsing CSR with "
            "duplicate attributes: %v",
            err);
    arena_free(&ar);
}

static void TestIPv4MappedIPsParse(TestingT *t) {
    const char *const ders[] = {ipv4_mapped_san_cert, ipv4_mapped_constraint_cert};
    const char *const names[] = {"SAN", "constraint"};
    ARENA(a);
    for (size_t i = 0; i < 2; i++) {
        Error err = BURROW_NO_ERROR;
        (void)x509_parse_certificate(a, hexb(a, ders[i]), &err);
        if (!BURROW_FAILED(err))
            testing_t_errorf_v(t,
                               "%s: expected error when parsing certificate containing "
                               "IPv4-mapped IPv6 address",
                               names[i]);
        else if (!contains(err, "IPv4-mapped IPv6 address"))
            testing_t_errorf_v(t, "%s: unexpected error: %v", names[i], err);
    }
    arena_free(&ar);
}

#define TESTS(X)                                                                       \
    X(TestParseASN1String)                                                             \
    X(TestPolicyParse)                                                                 \
    X(TestParsePolicies)                                                               \
    X(TestParseCertificateNegativeMaxPathLength)                                       \
    X(TestDomainNameValid)                                                             \
    X(TestParseNameTypes)                                                              \
    X(TestCertificateParse)                                                            \
    X(TestErrorStrings)                                                                \
    X(TestCertificateEqualOnNil)                                                       \
    X(TestMismatchedSignatureAlgorithm)                                                \
    X(TestECDSA)                                                                       \
    X(TestParseCertificateWithDsaPublicKey)                                            \
    X(TestParseCertificateWithDSASignatureAlgorithm)                                   \
    X(TestVerifyCertificateWithDSASignature)                                           \
    X(TestRSAPSSSelfSigned)                                                            \
    X(TestEd25519SelfSigned)                                                           \
    X(TestParseDERCRL)                                                                 \
    X(TestCRLWithoutExpiry)                                                            \
    X(TestParsePEMCRL)                                                                 \
    X(TestParseCertificateRequest)                                                     \
    X(TestCriticalFlagInCSRRequestedExtensions)                                        \
    X(TestInsecureAlgorithmErrorString)                                                \
    X(TestMD5)                                                                         \
    X(TestSHA1)                                                                        \
    X(TestRSAMissingNULLParameters)                                                    \
    X(TestISOOIDInCertificate)                                                         \
    X(TestMultipleRDN)                                                                 \
    X(TestEmptyNameConstraints)                                                        \
    X(TestCriticalNameConstraintWithUnknownType)                                       \
    X(TestBadIPMask)                                                                   \
    X(TestAdditionFieldsInGeneralSubtree)                                              \
    X(TestMultipleURLsInCRLDP)                                                         \
    X(TestRawSignatureAlgorithm)                                                       \
    X(TestParseCertificateRawEquals)                                                   \
    X(TestSigAlgMismatch)                                                              \
    X(TestAuthKeyIdOptional)                                                           \
    X(TestLargeOID)                                                                    \
    X(TestParseUniqueID)                                                               \
    X(TestParseRevocationList)                                                         \
    X(TestParseNegativeSerial)                                                         \
    X(TestDuplicateExtensionsCert)                                                     \
    X(TestDuplicateExtensionsCSR)                                                      \
    X(TestDuplicateAttributesCSR)                                                      \
    X(TestIPv4MappedIPsParse)

TESTING_MAIN(TESTS)
