/* crypto/x509 tests for creating certificates, CSRs and CRLs: the tests of
 * Go's x509_test.go that make something with CreateCertificate,
 * CreateCertificateRequest, CreateRevocationList or Certificate.CreateCRL
 * and read it back.
 *
 * Go's table driven tests are loops here rather than subtests, and
 * reflect.DeepEqual is a comparison of the fields that matter. Go has two
 * variables for the same RSA key, testPrivateKey and rsaPrivateKey, and both
 * are test_key here. The ML-DSA cases always run, since the check for the
 * FIPS 140 module version that guards them in Go has nothing to check here.
 * TestRawSignatureAlgorithm is split: the certificate it parses is in
 * x509_parse_test.c, and the CSR and CRL it makes are
 * TestRawSignatureAlgorithmCreated here. TestMLDSA is split the same way,
 * with the keys in x509_test.c and the certificates in
 * TestMLDSACertificates here.
 *
 * Copyright 2009 The Go Authors. All rights reserved.
 * Copyright 2026 The burrow Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style licence that can be found
 * in the LICENSE file. */

#include "burrow/crypto/x509.h"

#include "burrow/bytes.h"
#include "burrow/crypto.h"
#include "burrow/crypto/ecdsa.h"
#include "burrow/crypto/ed25519.h"
#include "burrow/crypto/elliptic.h"
#include "burrow/crypto/mldsa.h"
#include "burrow/crypto/rsa.h"
#include "burrow/crypto/x509/pkix.h"
#include "burrow/declare.h"
#include "burrow/encoding/asn1.h"
#include "burrow/encoding/base64.h"
#include "burrow/encoding/hex.h"
#include "burrow/encoding/pem.h"
#include "burrow/error.h"
#include "burrow/hash.h"
#include "burrow/math/big.h"
#include "burrow/mem/arena.h"
#include "burrow/net.h"
#include "burrow/net/url.h"
#include "burrow/strings.h"
#include "burrow/testing.h"
#include "burrow/time.h"

#include "../src/crypto/x509_internal.h"
#include "check.h"
#include "x509_parse_test_gen.h"
#include "x509_test_gen.h"

#include <stdint.h>
#include <string.h>

/* ----------------------------------------------------------------- helpers */

static Slice cbytes(const char *s) {
    Int n = (Int)strlen(s);
    return (Slice){(void *)(uintptr_t)s, n, n, TYPE_BYTE};
}

static Slice bytes_of(const Byte *p, Int n) {
    return (Slice){(void *)(uintptr_t)p, n, n, TYPE_BYTE};
}

#define BYTES(...)                                                                     \
    bytes_of((const Byte[]){__VA_ARGS__}, (Int)sizeof((const Byte[]){__VA_ARGS__}))

static Slice hexb(Alloc *a, const char *s) {
    Error err = BURROW_NO_ERROR;
    Slice b = hex_decode_string(a, str_from_cstr(s), &err);
    if (BURROW_FAILED(err))
        panic_str(BURROW_S("x509_create_test: bad hex"));
    return b;
}

static Slice from_base64(Alloc *a, const char *s) {
    Error err = BURROW_NO_ERROR;
    Slice b =
        base64_encoding_decode_string(base64_std_encoding, a, str_from_cstr(s), &err);
    if (BURROW_FAILED(err))
        panic_str(BURROW_S("x509_create_test: bad base64"));
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
        panic_str(BURROW_S("x509_create_test: no PEM block"));
    return b->bytes;
}

static bool contains(Error err, const char *want) {
    return BURROW_FAILED(err) && strings_contains(error_text(err), str_from_cstr(want));
}

static bool text_is(Error err, const char *want) {
    return BURROW_FAILED(err) && str_eq(error_text(err), str_from_cstr(want));
}

static Slice strs_of(Alloc *a, const char *const *v, Int n) {
    Slice s = slice_nil(TYPE_STRING);
    for (Int i = 0; i < n; i++) {
        Str x = str_from_cstr(v[i]);
        s = slice_append(a, s, &x, 1);
    }
    return s;
}

#define STRS(a, ...)                                                                   \
    strs_of((a), (const char *const[]){__VA_ARGS__},                                   \
            (Int)(sizeof((const char *const[]){__VA_ARGS__}) / sizeof(const char *)))

static bool strs_equal(Slice got, Slice want) {
    if (got.len != want.len)
        return false;
    const Str *g = got.p;
    const Str *w = want.p;
    for (Int i = 0; i < got.len; i++)
        if (!str_eq(g[i], w[i]))
            return false;
    return true;
}

/* Whether s is just the one string want. */
static bool strs_are_one(Slice s, const char *want) {
    return s.len == 1 && str_eq(((const Str *)s.p)[0], str_from_cstr(want));
}

/* A Str in a, for an Any to point at. */
static Any str_any(Alloc *a, const char *s) {
    Str *p = mem_alloc(a, sizeof(Str), _Alignof(Str));
    *p = str_from_cstr(s);
    return BURROW_ANY(TYPE_STRING, p);
}

/* Bytes in a, for an Any to point at. */
static Any bytes_any(Alloc *a, Slice b) {
    Slice *p = mem_alloc(a, sizeof(Slice), _Alignof(Slice));
    *p = b;
    return BURROW_ANY(TYPE_BYTES, p);
}

static Slice one(Alloc *a, const Type *t, const void *v) {
    return slice_append(a, slice_nil(t), v, 1);
}

static NetIPNet *parse_cidr(Alloc *a, const char *s) {
    NetIPNet *n = NULL;
    Error err = BURROW_NO_ERROR;
    net_parse_cidr(a, str_from_cstr(s), &n, &err);
    if (BURROW_FAILED(err))
        panic_str(BURROW_S("x509_create_test: bad CIDR"));
    return n;
}

static Url *parse_uri(Alloc *a, const char *s) {
    Error err = BURROW_NO_ERROR;
    Url *u = url_parse(a, str_from_cstr(s), &err);
    if (BURROW_FAILED(err))
        panic_str(BURROW_S("x509_create_test: bad URL"));
    return u;
}

static X509OID must_oid(Alloc *a, const uint64_t *v, Int n) {
    Error err = BURROW_NO_ERROR;
    X509OID oid =
        x509_oid_from_ints(a, (Slice){(void *)(uintptr_t)v, n, n, TYPE_UINT64}, &err);
    if (BURROW_FAILED(err))
        panic_str(BURROW_S("x509_create_test: bad OID"));
    return oid;
}

#define MUST_OID(a, ...)                                                               \
    must_oid((a), (const uint64_t[]){__VA_ARGS__},                                     \
             (Int)(sizeof((const uint64_t[]){__VA_ARGS__}) / sizeof(uint64_t)))

static bool oids_equal(Slice got, Slice want) {
    if (got.len != want.len)
        return false;
    const X509OID *g = got.p;
    const X509OID *w = want.p;
    for (Int i = 0; i < got.len; i++)
        if (!x509_oid_equal(g[i], w[i]))
            return false;
    return true;
}

#define ARENA(a)                                                                       \
    Arena ar;                                                                          \
    arena_init(&ar, NULL, 0);                                                          \
    Alloc *a = arena_allocator(&ar)

static const Int oid_extension_subject_key_id[] = {2, 5, 29, 14};
static const Int oid_extension_subject_alt_name[] = {2, 5, 29, 17};
static const Int oid_extension_crl_number[] = {2, 5, 29, 20};
static const Int oid_extension_authority_key_id[] = {2, 5, 29, 35};
static const Int oid_extension_authority_info_access[] = {1, 3, 6, 1, 5, 5, 7, 1, 1};
static const Int oid_extension_request[] = {1, 2, 840, 113549, 1, 9, 14};
static const Int oid_signature_ecdsa_with_sha256[] = {1, 2, 840, 10045, 4, 3, 2};

#define OID(arcs)                                                                      \
    ((Asn1ObjectIdentifier){(void *)(uintptr_t)(arcs),                                 \
                            (Int)(sizeof(arcs) / sizeof((arcs)[0])),                   \
                            (Int)(sizeof(arcs) / sizeof((arcs)[0])), TYPE_INT})

/* The extension with id in exts, or NULL. */
static const PkixExtension *find_extension(Slice exts, Asn1ObjectIdentifier id) {
    const PkixExtension *e = exts.p;
    for (Int i = 0; i < exts.len; i++)
        if (asn1_object_identifier_equal(e[i].id, id))
            return &e[i];
    return NULL;
}

static bool extension_equal(const PkixExtension *x, const PkixExtension *y) {
    return asn1_object_identifier_equal(x->id, y->id) && x->critical == y->critical &&
           bytes_equal(x->value, y->value);
}

/* ------------------------------------------------------------------- keys */

/* testPrivateKey, which is also rsaPrivateKey. */
static RsaPrivateKey *test_key(Alloc *a) {
    Error err = BURROW_NO_ERROR;
    RsaPrivateKey *k =
        x509_parse_pkcs1_private_key(a, pem_bytes(a, pem_private_key), &err);
    if (BURROW_FAILED(err))
        panic_str(BURROW_S("x509_create_test: bad test key"));
    return k;
}

static EcdsaPrivateKey *ec_key(Alloc *a, EllipticCurve c) {
    Error err = BURROW_NO_ERROR;
    EcdsaPrivateKey *k = ecdsa_generate_key(a, c, (IoReader){0}, &err);
    if (BURROW_FAILED(err))
        panic_str(BURROW_S("x509_create_test: ECDSA key generation failed"));
    return k;
}

static MldsaPrivateKey *mldsa_key(Alloc *a, const MldsaParameters *params) {
    Error err = BURROW_NO_ERROR;
    MldsaPrivateKey *k = mldsa_generate_key(params, a, &err);
    if (BURROW_FAILED(err))
        panic_str(BURROW_S("x509_create_test: ML-DSA key generation failed"));
    return k;
}

/* A new Ed25519 key as a signer kept in *es. */
static CryptoSigner ed25519_key(Alloc *a, Ed25519Signer *es) {
    Error err = BURROW_NO_ERROR;
    Ed25519PrivateKey priv;
    ed25519_generate_key(a, (IoReader){0}, &priv, &err);
    if (BURROW_FAILED(err))
        panic_str(BURROW_S("x509_create_test: Ed25519 key generation failed"));
    return ed25519_private_key_signer(priv, es);
}

/* A key from x509_parse_pkcs8_private_key as a signer. */
static CryptoSigner any_signer(Any k, Ed25519Signer *es) {
    if (k.t == TYPE_RSA_PRIVATE_KEY)
        return rsa_private_key_signer(k.data);
    if (k.t == TYPE_ECDSA_PRIVATE_KEY)
        return ecdsa_private_key_signer(k.data);
    if (k.t == TYPE_ED25519_PRIVATE_KEY)
        return ed25519_private_key_signer(*(const Ed25519PrivateKey *)k.data, es);
    if (k.t == TYPE_MLDSA_PRIVATE_KEY)
        return mldsa_private_key_signer(k.data);
    panic_str(BURROW_S("x509_create_test: not a signer"));
}

/* serialiseAndParse */
static X509Certificate *serialise_and_parse(TestingT *t, Alloc *a,
                                            const X509Certificate *template_) {
    RsaPrivateKey *k = test_key(a);
    Error err = BURROW_NO_ERROR;
    Slice der = x509_create_certificate(a, (IoReader){0}, template_, template_,
                                        rsa_private_key_public(k),
                                        rsa_private_key_signer(k), &err);
    if (BURROW_FAILED(err))
        testing_t_fatalf_v(t, "failed to create certificate: %v", err);
    X509Certificate *cert = x509_parse_certificate(a, der, &err);
    if (BURROW_FAILED(err))
        testing_t_fatalf_v(t, "failed to parse certificate: %v", err);
    return cert;
}

/* CreateCertificate of a self-signed template with the test key. */
static Error create_with_test_key(Alloc *a, const X509Certificate *template_,
                                  Slice *der) {
    RsaPrivateKey *k = test_key(a);
    Error err = BURROW_NO_ERROR;
    Slice out = x509_create_certificate(a, (IoReader){0}, template_, template_,
                                        rsa_private_key_public(k),
                                        rsa_private_key_signer(k), &err);
    if (der != NULL)
        *der = out;
    return err;
}

/* marshalAndParseCSR */
static X509CertificateRequest *
marshal_and_parse_csr(TestingT *t, Alloc *a, const X509CertificateRequest *tmpl) {
    Error err = BURROW_NO_ERROR;
    Slice der = x509_create_certificate_request(
        a, (IoReader){0}, tmpl, rsa_private_key_signer(test_key(a)), &err);
    if (BURROW_FAILED(err))
        testing_t_fatalf_v(t, "%v", err);
    X509CertificateRequest *csr = x509_parse_certificate_request(a, der, &err);
    if (BURROW_FAILED(err))
        testing_t_fatalf_v(t, "%v", err);
    return csr;
}

/* A template with only a serial number of 1, a common name and the validity
 * most of Go's tests use. */
static X509Certificate simple_template(Alloc *a, const char *common_name) {
    X509Certificate c = {0};
    c.serial_number = big_new_int(a, 1);
    c.subject.common_name = str_from_cstr(common_name);
    c.not_before = time_from_unix(1000, 0);
    c.not_after = time_from_unix(100000, 0);
    return c;
}

/* ----------------------------------------------------- certificate creation */

typedef struct SelfSignedCase {
    const char *name;
    Any pub;
    CryptoSigner priv;
    bool check_sig;
    X509SignatureAlgorithm sig_algo;
} SelfSignedCase;

static void TestCreateSelfSignedCertificate(TestingT *t) {
    ARENA(a);
    RsaPrivateKey *rsa_priv = test_key(a);
    Any rsa_pub = rsa_private_key_public(rsa_priv);
    CryptoSigner rsa_signer = rsa_private_key_signer(rsa_priv);
    EcdsaPrivateKey *ecdsa_priv = ec_key(a, elliptic_p256());
    Any ecdsa_pub = ecdsa_private_key_public(ecdsa_priv);
    CryptoSigner ecdsa_signer = ecdsa_private_key_signer(ecdsa_priv);
    Ed25519Signer es;
    CryptoSigner ed25519_signer = ed25519_key(a, &es);
    MldsaPrivateKey *mldsa_priv = mldsa_key(a, mldsa_mldsa44());

    const SelfSignedCase tests[] = {
        {"RSA/RSA", rsa_pub, rsa_signer, true, X509_SHA384_WITH_RSA},
        {"RSA/ECDSA", rsa_pub, ecdsa_signer, false, X509_ECDSA_WITH_SHA384},
        {"ECDSA/RSA", ecdsa_pub, rsa_signer, false, X509_SHA256_WITH_RSA},
        {"ECDSA/ECDSA", ecdsa_pub, ecdsa_signer, true, X509_ECDSA_WITH_SHA256},
        {"RSAPSS/RSAPSS", rsa_pub, rsa_signer, true, X509_SHA256_WITH_RSAPSS},
        {"ECDSA/RSAPSS", ecdsa_pub, rsa_signer, false, X509_SHA256_WITH_RSAPSS},
        {"RSAPSS/ECDSA", rsa_pub, ecdsa_signer, false, X509_ECDSA_WITH_SHA384},
        {"Ed25519", crypto_signer_public(ed25519_signer), ed25519_signer, true,
         X509_PURE_ED25519},
        {"ML-DSA-44", mldsa_private_key_public(mldsa_priv),
         mldsa_private_key_signer(mldsa_priv), true, X509_MLDSA44},
    };

    const X509ExtKeyUsage ext_key_usage[] = {X509_EXT_KEY_USAGE_CLIENT_AUTH,
                                             X509_EXT_KEY_USAGE_SERVER_AUTH};
    Slice test_ext_key_usage = slice_append(a, slice_nil(TYPE_INT), ext_key_usage, 2);
    const Asn1ObjectIdentifier unknown[] = {ASN1_OID(1, 2, 3), ASN1_OID(2, 59, 1)};
    Slice test_unknown_ext_key_usage =
        slice_append(a, slice_nil(TYPE_ASN1_OBJECT_IDENTIFIER), unknown, 2);
    Slice extra_extension_data = cbytes("extra extension");

    for (Int i = 0; i < (Int)(sizeof tests / sizeof tests[0]); i++) {
        const SelfSignedCase *test = &tests[i];
        const char *common_name = "test.example.com";
        X509Certificate tmpl = {0};
        tmpl.serial_number = big_new_int(a, 1);
        tmpl.subject.common_name = str_from_cstr(common_name);
        tmpl.subject.organization = STRS(a, "\xce\xa3 Acme Co");
        tmpl.subject.country = STRS(a, "US");
        const PkixAttributeTypeAndValue extra_names[] = {
            {ASN1_OID(2, 5, 4, 42), str_any(a, "Gopher")},
            /* This should override the Country, above. */
            {ASN1_OID(2, 5, 4, 6), str_any(a, "NL")},
        };
        tmpl.subject.extra_names = slice_append(
            a, slice_nil(TYPE_PKIX_ATTRIBUTE_TYPE_AND_VALUE), extra_names, 2);
        tmpl.not_before = time_from_unix(1000, 0);
        tmpl.not_after = time_from_unix(100000, 0);
        tmpl.signature_algorithm = test->sig_algo;
        tmpl.subject_key_id = BYTES(1, 2, 3, 4);
        tmpl.key_usage = X509_KEY_USAGE_CERT_SIGN;
        tmpl.ext_key_usage = test_ext_key_usage;
        tmpl.unknown_ext_key_usage = test_unknown_ext_key_usage;
        tmpl.basic_constraints_valid = true;
        tmpl.is_ca = true;
        tmpl.ocsp_server = STRS(a, "http://ocsp.example.com");
        tmpl.issuing_certificate_url = STRS(a, "http://crt.example.com/ca1.crt");
        tmpl.dns_names = STRS(a, "test.example.com");
        tmpl.email_addresses = STRS(a, "gopher@golang.org");
        const NetIP ips[] = {net_ip_to4(net_ipv4(a, 127, 0, 0, 1)),
                             net_parse_ip(a, BURROW_S("2001:4860:0:2001::68"))};
        tmpl.ip_addresses = slice_append(a, slice_nil(TYPE_NET_IP), ips, 2);
        Url *uri = parse_uri(a, "https://foo.com/wibble#foo");
        tmpl.uris = one(a, TYPE_X509_URL_PTR, &uri);
        X509OID policy = MUST_OID(a, 1, 2, 3, UINT32_MAX, UINT64_MAX);
        tmpl.policies = one(a, TYPE_X509_OID, &policy);
        tmpl.permitted_dns_domains = STRS(a, ".example.com", "example.com");
        tmpl.excluded_dns_domains = STRS(a, "bar.example.com");
        NetIPNet *permitted[] = {parse_cidr(a, "192.168.1.1/16"),
                                 parse_cidr(a, "1.2.3.4/8")};
        tmpl.permitted_ip_ranges =
            slice_append(a, slice_nil(TYPE_X509_IP_NET_PTR), permitted, 2);
        NetIPNet *excluded = parse_cidr(a, "2001:db8::/48");
        tmpl.excluded_ip_ranges = one(a, TYPE_X509_IP_NET_PTR, &excluded);
        tmpl.permitted_email_addresses = STRS(a, "foo@example.com");
        tmpl.excluded_email_addresses = STRS(a, ".example.com", "example.com");
        tmpl.permitted_uri_domains = STRS(a, ".bar.com", "bar.com");
        tmpl.excluded_uri_domains = STRS(a, ".bar2.com", "bar2.com");
        tmpl.crl_distribution_points = STRS(a, "http://crl1.example.com/ca1.crl",
                                            "http://crl2.example.com/ca1.crl");
        const PkixExtension extra[] = {
            {ASN1_OID(1, 2, 3, 4), false, extra_extension_data},
            /* This extension should override the SubjectKeyId, above. */
            {OID(oid_extension_subject_key_id), false, BYTES(0x04, 0x04, 4, 3, 2, 1)},
        };
        tmpl.extra_extensions =
            slice_append(a, slice_nil(TYPE_PKIX_EXTENSION), extra, 2);

        Error err = BURROW_NO_ERROR;
        Slice der = x509_create_certificate(a, (IoReader){0}, &tmpl, &tmpl, test->pub,
                                            test->priv, &err);
        if (BURROW_FAILED(err)) {
            testing_t_errorf_v(t, "%s: failed to create certificate: %v", test->name,
                               err);
            continue;
        }
        X509Certificate *cert = x509_parse_certificate(a, der, &err);
        if (BURROW_FAILED(err)) {
            testing_t_errorf_v(t, "%s: failed to parse certificate: %v", test->name,
                               err);
            continue;
        }

        if (!oids_equal(cert->policies, tmpl.policies))
            testing_t_errorf_v(t, "%s: failed to parse policy identifiers", test->name);
        if (!strs_equal(cert->permitted_dns_domains, tmpl.permitted_dns_domains))
            testing_t_errorf_v(t, "%s: failed to parse name constraints", test->name);
        if (!strs_are_one(cert->excluded_dns_domains, "bar.example.com"))
            testing_t_errorf_v(t, "%s: failed to parse name constraint exclusions",
                               test->name);
        NetIPNet *const *pr = cert->permitted_ip_ranges.p;
        if (cert->permitted_ip_ranges.len != 2 ||
            !str_eq(net_ip_net_string(pr[0], a), BURROW_S("192.168.0.0/16")) ||
            !str_eq(net_ip_net_string(pr[1], a), BURROW_S("1.0.0.0/8")))
            testing_t_errorf_v(t, "%s: failed to parse IP constraints", test->name);
        NetIPNet *const *er = cert->excluded_ip_ranges.p;
        if (cert->excluded_ip_ranges.len != 1 ||
            !str_eq(net_ip_net_string(er[0], a), BURROW_S("2001:db8::/48")))
            testing_t_errorf_v(t, "%s: failed to parse IP constraint exclusions",
                               test->name);
        if (!strs_are_one(cert->permitted_email_addresses, "foo@example.com"))
            testing_t_errorf_v(t, "%s: failed to parse permitted email addresses",
                               test->name);
        if (!strs_equal(cert->excluded_email_addresses, tmpl.excluded_email_addresses))
            testing_t_errorf_v(t, "%s: failed to parse excluded email addresses",
                               test->name);
        if (!strs_equal(cert->permitted_uri_domains, tmpl.permitted_uri_domains))
            testing_t_errorf_v(t, "%s: failed to parse permitted URIs", test->name);
        if (!strs_equal(cert->excluded_uri_domains, tmpl.excluded_uri_domains))
            testing_t_errorf_v(t, "%s: failed to parse excluded URIs", test->name);

        if (!str_eq(cert->subject.common_name, str_from_cstr(common_name)))
            testing_t_errorf_v(t,
                               "%s: subject wasn't correctly copied from the template. "
                               "Got %s, want %s",
                               test->name, cert->subject.common_name, common_name);
        if (!strs_are_one(cert->subject.country, "NL"))
            testing_t_errorf_v(t, "%s: ExtraNames didn't override Country", test->name);

        const PkixExtension *san =
            find_extension(cert->extensions, OID(oid_extension_subject_alt_name));
        if (san != NULL && san->critical)
            testing_t_fatalf_v(t, "SAN extension is marked critical");

        bool found = false;
        const PkixAttributeTypeAndValue *names = cert->subject.names.p;
        for (Int j = 0; j < cert->subject.names.len; j++)
            if (asn1_object_identifier_equal(names[j].type, ASN1_OID(2, 5, 4, 42)))
                found = true;
        if (!found)
            testing_t_errorf_v(
                t, "%s: Names didn't contain oid 2.5.4.42 from ExtraNames", test->name);

        if (!str_eq(cert->issuer.common_name, str_from_cstr(common_name)))
            testing_t_errorf_v(t,
                               "%s: issuer wasn't correctly copied from the template. "
                               "Got %s, want %s",
                               test->name, cert->issuer.common_name, common_name);
        if (cert->signature_algorithm != test->sig_algo)
            testing_t_errorf_v(
                t,
                "%s: SignatureAlgorithm wasn't copied from template. Got "
                "%d, want %d",
                test->name, cert->signature_algorithm, test->sig_algo);

        bool eku_ok = cert->ext_key_usage.len == 2;
        for (Int j = 0; eku_ok && j < 2; j++)
            eku_ok =
                ((const X509ExtKeyUsage *)cert->ext_key_usage.p)[j] == ext_key_usage[j];
        if (!eku_ok)
            testing_t_errorf_v(
                t, "%s: extkeyusage wasn't correctly copied from the template",
                test->name);
        bool unknown_ok = cert->unknown_ext_key_usage.len == 2;
        for (Int j = 0; unknown_ok && j < 2; j++)
            unknown_ok = asn1_object_identifier_equal(
                ((const Asn1ObjectIdentifier *)cert->unknown_ext_key_usage.p)[j],
                unknown[j]);
        if (!unknown_ok)
            testing_t_errorf_v(
                t,
                "%s: unknown extkeyusage wasn't correctly copied from the "
                "template",
                test->name);
        if (!strs_equal(cert->ocsp_server, tmpl.ocsp_server))
            testing_t_errorf_v(t, "%s: OCSP servers differ from template", test->name);
        if (!strs_equal(cert->issuing_certificate_url, tmpl.issuing_certificate_url))
            testing_t_errorf_v(t, "%s: Issuing certificate URLs differ from template",
                               test->name);
        if (!strs_equal(cert->dns_names, tmpl.dns_names))
            testing_t_errorf_v(t, "%s: SAN DNS names differ from template", test->name);
        if (!strs_equal(cert->email_addresses, tmpl.email_addresses))
            testing_t_errorf_v(t, "%s: SAN emails differ from template", test->name);
        if (cert->uris.len != 1 ||
            !str_eq(url_string(((Url *const *)cert->uris.p)[0], a),
                    BURROW_S("https://foo.com/wibble#foo")))
            testing_t_errorf_v(t, "%s: URIs differ from template", test->name);
        bool ips_ok = cert->ip_addresses.len == 2;
        for (Int j = 0; ips_ok && j < 2; j++)
            ips_ok = net_ip_equal(((const NetIP *)cert->ip_addresses.p)[j], ips[j]);
        if (!ips_ok)
            testing_t_errorf_v(t, "%s: SAN IPs differ from template", test->name);
        if (!strs_equal(cert->crl_distribution_points, tmpl.crl_distribution_points))
            testing_t_errorf_v(t, "%s: CRL distribution points differ from template",
                               test->name);
        if (!bytes_equal(cert->subject_key_id, BYTES(4, 3, 2, 1)))
            testing_t_errorf_v(t, "%s: ExtraExtensions didn't override SubjectKeyId",
                               test->name);
        if (!bytes_contains(der, extra_extension_data))
            testing_t_errorf_v(t, "%s: didn't find extra extension in DER output",
                               test->name);

        if (test->check_sig) {
            err = x509_certificate_check_signature_from(cert, cert);
            if (BURROW_FAILED(err))
                testing_t_errorf_v(t, "%s: signature verification failed: %v",
                                   test->name, err);
        }
    }
    arena_free(&ar);
}

static void TestMaxPathLenNotCA(TestingT *t) {
    ARENA(a);
    X509Certificate tmpl = simple_template(a, "\xce\xa3 Acme Co");
    tmpl.subject.common_name = BURROW_S("\xce\xa3 Acme Co");
    tmpl.basic_constraints_valid = true;
    tmpl.is_ca = false;
    Int m = serialise_and_parse(t, a, &tmpl)->max_path_len;
    if (m != -1)
        testing_t_errorf_v(t, "MaxPathLen should be -1 when IsCa is false, got %d", m);

    tmpl.max_path_len = -1;
    m = serialise_and_parse(t, a, &tmpl)->max_path_len;
    if (m != -1)
        testing_t_errorf_v(t,
                           "MaxPathLen should be -1 when IsCa is false and MaxPathLen "
                           "set to -1, got %d",
                           m);

    tmpl.max_path_len = 5;
    if (!BURROW_FAILED(create_with_test_key(a, &tmpl, NULL)))
        testing_t_errorf_v(t, "specifying a MaxPathLen when IsCA is false should fail");

    tmpl.max_path_len = 0;
    tmpl.max_path_len_zero = true;
    if (!BURROW_FAILED(create_with_test_key(a, &tmpl, NULL)))
        testing_t_errorf_v(t, "setting MaxPathLenZero when IsCA is false should fail");

    tmpl.basic_constraints_valid = false;
    m = serialise_and_parse(t, a, &tmpl)->max_path_len;
    if (m != 0)
        testing_t_errorf_v(
            t,
            "Bad MaxPathLen should be ignored if BasicConstraintsValid is "
            "false, got %d",
            m);
    arena_free(&ar);
}

static void TestMaxPathLen(TestingT *t) {
    ARENA(a);
    X509Certificate tmpl = simple_template(a, "\xce\xa3 Acme Co");
    tmpl.basic_constraints_valid = true;
    tmpl.is_ca = true;

    X509Certificate *cert1 = serialise_and_parse(t, a, &tmpl);
    if (cert1->max_path_len != -1)
        testing_t_errorf_v(t, "Omitting MaxPathLen didn't turn into -1, got %d",
                           cert1->max_path_len);
    if (cert1->max_path_len_zero)
        testing_t_errorf_v(t, "Omitting MaxPathLen resulted in MaxPathLenZero");

    tmpl.max_path_len = 1;
    X509Certificate *cert2 = serialise_and_parse(t, a, &tmpl);
    if (cert2->max_path_len != 1)
        testing_t_errorf_v(t, "Setting MaxPathLen didn't work. Got %d but set 1",
                           cert2->max_path_len);
    if (cert2->max_path_len_zero)
        testing_t_errorf_v(t, "Setting MaxPathLen resulted in MaxPathLenZero");

    tmpl.max_path_len = 0;
    tmpl.max_path_len_zero = true;
    X509Certificate *cert3 = serialise_and_parse(t, a, &tmpl);
    if (cert3->max_path_len != 0)
        testing_t_errorf_v(t, "Setting MaxPathLenZero didn't work, got %d",
                           cert3->max_path_len);
    if (!cert3->max_path_len_zero)
        testing_t_errorf_v(
            t, "Setting MaxPathLen to zero didn't result in MaxPathLenZero");
    arena_free(&ar);
}

static void TestNoAuthorityKeyIdInSelfSignedCert(TestingT *t) {
    ARENA(a);
    X509Certificate tmpl = simple_template(a, "\xce\xa3 Acme Co");
    tmpl.basic_constraints_valid = true;
    tmpl.is_ca = true;
    tmpl.subject_key_id = BYTES(1, 2, 3, 4);

    if (serialise_and_parse(t, a, &tmpl)->authority_key_id.len != 0)
        testing_t_fatalf_v(
            t, "self-signed certificate contained default authority key id");

    tmpl.authority_key_id = BYTES(1, 2, 3, 4);
    if (serialise_and_parse(t, a, &tmpl)->authority_key_id.len == 0)
        testing_t_fatalf_v(t,
                           "self-signed certificate erased explicit authority key id");
    arena_free(&ar);
}

static void TestNoSubjectKeyIdInCert(TestingT *t) {
    ARENA(a);
    X509Certificate tmpl = simple_template(a, "\xce\xa3 Acme Co");
    tmpl.basic_constraints_valid = true;
    tmpl.is_ca = true;
    if (serialise_and_parse(t, a, &tmpl)->subject_key_id.len == 0)
        testing_t_fatalf_v(
            t, "self-signed certificate did not generate subject key id using "
               "the public key");

    tmpl.is_ca = false;
    if (serialise_and_parse(t, a, &tmpl)->subject_key_id.len != 0)
        testing_t_fatalf_v(
            t, "self-signed certificate generated subject key id when it isn't "
               "a CA");

    tmpl.subject_key_id = BYTES(1, 2, 3, 4);
    if (serialise_and_parse(t, a, &tmpl)->subject_key_id.len == 0)
        testing_t_fatalf_v(t, "self-signed certificate erased explicit subject key id");
    arena_free(&ar);
}

static void TestASN1BitLength(TestingT *t) {
    static const struct {
        Byte bytes[2];
        Int len;
        Int bit_len;
    } tests[] = {
        {{0}, 0, 0},    {{0x00}, 1, 0}, {{0x00, 0x00}, 2, 0}, {{0xf0}, 1, 4},
        {{0x88}, 1, 5}, {{0xff}, 1, 8}, {{0xff, 0x80}, 2, 9}, {{0xff, 0x81}, 2, 16},
    };
    for (Int i = 0; i < (Int)(sizeof tests / sizeof tests[0]); i++) {
        Slice b = tests[i].len == 0 ? slice_nil(TYPE_BYTE)
                                    : bytes_of(tests[i].bytes, tests[i].len);
        Int got = burrow__x509_asn1_bit_length(b);
        if (got != tests[i].bit_len)
            testing_t_errorf_v(t, "#%d: calculated bit-length of %d for %x, wanted %d",
                               i, got, b, tests[i].bit_len);
    }
}

BURROW_PTR_TYPE(TestBigIntPtr, BigInt);

static void TestEmptySerialNumber(TestingT *t) {
    ARENA(a);
    X509Certificate tmpl = {0};
    tmpl.dns_names = STRS(a, "example.com");
    for (Int i = 0; i < 100; i++) {
        Slice der;
        Error err = create_with_test_key(a, &tmpl, &der);
        if (BURROW_FAILED(err))
            testing_t_fatalf_v(t, "failed to create certificate: %v", err);
        X509Certificate *cert = x509_parse_certificate(a, der, &err);
        if (BURROW_FAILED(err))
            testing_t_fatalf_v(t, "failed to parse certificate: %v", err);
        Int sign = big_int_sign(cert->serial_number);
        if (sign != 1)
            testing_t_fatalf_v(t, "generated a non positive serial, sign: %d", sign);
        TestBigIntPtr serial = cert->serial_number;
        Slice b = asn1_marshal(a, BURROW_ANY(TYPE_OF(TestBigIntPtr), &serial), &err);
        if (BURROW_FAILED(err))
            testing_t_fatalf_v(t, "failed to marshal generated serial number: %v", err);
        /* subtract 2 for tag and length */
        if (b.len - 2 > 20)
            testing_t_fatalf_v(t,
                               "generated serial number larger than 20 octets when "
                               "encoded: %d",
                               b.len - 2);
    }
    arena_free(&ar);
}

static void TestEmptySubject(TestingT *t) {
    ARENA(a);
    X509Certificate tmpl = {0};
    tmpl.serial_number = big_new_int(a, 1);
    tmpl.dns_names = STRS(a, "example.com");
    X509Certificate *cert = serialise_and_parse(t, a, &tmpl);
    const PkixExtension *san =
        find_extension(cert->extensions, OID(oid_extension_subject_alt_name));
    if (san == NULL)
        testing_t_fatalf_v(t, "SAN extension is missing");
    if (!san->critical)
        testing_t_fatalf_v(t, "SAN extension is not critical");
    arena_free(&ar);
}

static void TestUnknownExtKey(TestingT *t) {
    ARENA(a);
    X509Certificate tmpl = {0};
    tmpl.serial_number = big_new_int(a, 10);
    tmpl.dns_names = STRS(a, "foo");
    X509ExtKeyUsage bad = -1;
    tmpl.ext_key_usage = one(a, TYPE_INT, &bad);
    Error err = create_with_test_key(a, &tmpl, NULL);
    if (!contains(err, "unknown extended key usage"))
        testing_t_errorf_v(t, "expected error containing %q, got %v",
                           "unknown extended key usage", err);
    arena_free(&ar);
}

static void TestIA5SANEnforcement(TestingT *t) {
    ARENA(a);
    EcdsaPrivateKey *k = ec_key(a, elliptic_p256());
    Url *test_url = parse_uri(a, "https://example.com/");
    test_url->raw_query = BURROW_S("\xe2\x88\x9e");

    X509Certificate dns = {0};
    dns.serial_number = big_new_int(a, 0);
    dns.dns_names = STRS(a, "\xe2\x88\x9e");
    X509Certificate email = {0};
    email.serial_number = big_new_int(a, 0);
    email.email_addresses = STRS(a, "\xe2\x88\x9e");
    X509Certificate uri = {0};
    uri.serial_number = big_new_int(a, 0);
    uri.uris = one(a, TYPE_X509_URL_PTR, &test_url);
    const struct {
        const char *name;
        const X509Certificate *template_;
        const char *expected_error;
    } marshal_tests[] = {
        {"marshal: unicode dNSName", &dns,
         "x509: \"\xe2\x88\x9e\" cannot be encoded as an IA5String"},
        {"marshal: unicode rfc822Name", &email,
         "x509: \"\xe2\x88\x9e\" cannot be encoded as an IA5String"},
        {"marshal: unicode uniformResourceIdentifier", &uri,
         "x509: \"https://example.com/?\xe2\x88\x9e\" cannot be encoded as an "
         "IA5String"},
    };
    for (Int i = 0; i < (Int)(sizeof marshal_tests / sizeof marshal_tests[0]); i++) {
        Error err = BURROW_NO_ERROR;
        x509_create_certificate(a, (IoReader){0}, marshal_tests[i].template_,
                                marshal_tests[i].template_, ecdsa_private_key_public(k),
                                ecdsa_private_key_signer(k), &err);
        if (!BURROW_FAILED(err))
            testing_t_errorf_v(t, "%s: expected CreateCertificate to fail",
                               marshal_tests[i].name);
        else if (!text_is(err, marshal_tests[i].expected_error))
            testing_t_errorf_v(t, "%s: unexpected error: got %q, want %q",
                               marshal_tests[i].name, error_text(err),
                               marshal_tests[i].expected_error);
    }

    const struct {
        const char *name;
        const char *cert;
        const char *expected_error;
    } unmarshal_tests[] = {
        {"unmarshal: unicode dNSName", ia5_unmarshal_cert_0,
         "x509: SAN dNSName is malformed"},
        {"unmarshal: unicode rfc822Name", ia5_unmarshal_cert_1,
         "x509: SAN rfc822Name is malformed"},
        {"unmarshal: unicode uniformResourceIdentifier", ia5_unmarshal_cert_2,
         "x509: SAN uniformResourceIdentifier is malformed"},
    };
    for (Int i = 0; i < (Int)(sizeof unmarshal_tests / sizeof unmarshal_tests[0]);
         i++) {
        Error err = BURROW_NO_ERROR;
        x509_parse_certificate(a, hexb(a, unmarshal_tests[i].cert), &err);
        if (!BURROW_FAILED(err))
            testing_t_errorf_v(t, "%s: expected CreateCertificate to fail",
                               unmarshal_tests[i].name);
        else if (!text_is(err, unmarshal_tests[i].expected_error))
            testing_t_errorf_v(t, "%s: unexpected error: got %q, want %q",
                               unmarshal_tests[i].name, error_text(err),
                               unmarshal_tests[i].expected_error);
    }
    arena_free(&ar);
}

/* brokenSigner: a signer whose signatures are always {1, 2, 3}. */
typedef struct BrokenSigner {
    CryptoPublicKey pub;
} BrokenSigner;

static CryptoPublicKey broken_signer_public(void *self) {
    return ((BrokenSigner *)self)->pub;
}

static Slice broken_signer_sign(void *self, Alloc *a, IoReader rand, Slice digest,
                                CryptoSignerOpts opts, Error *err) {
    (void)self;
    (void)rand;
    (void)digest;
    (void)opts;
    BURROW_OUT(err, BURROW_NO_ERROR);
    return bytes_clone(a, BYTES(1, 2, 3));
}

static const CryptoSignerVT broken_signer_vt = {NULL, broken_signer_public,
                                                broken_signer_sign};

static void TestCreateCertificateBrokenSigner(TestingT *t) {
    ARENA(a);
    X509Certificate tmpl = {0};
    tmpl.serial_number = big_new_int(a, 10);
    tmpl.dns_names = STRS(a, "example.com");
    RsaPrivateKey *k = test_key(a);
    BrokenSigner bs = {rsa_private_key_public(k)};
    Error err = BURROW_NO_ERROR;
    x509_create_certificate(a, (IoReader){0}, &tmpl, &tmpl, bs.pub,
                            (CryptoSigner){&broken_signer_vt, &bs}, &err);
    if (!BURROW_FAILED(err))
        testing_t_fatalf_v(t,
                           "expected CreateCertificate to fail with a broken signer");
    else if (!contains(err, "signature returned by signer is invalid"))
        testing_t_fatalf_v(
            t, "CreateCertificate returned an unexpected error: got %q, want %q",
            error_text(err), "signature returned by signer is invalid");
    arena_free(&ar);
}

static void TestCreateCertificateLegacy(TestingT *t) {
    ARENA(a);
    X509Certificate tmpl = {0};
    tmpl.serial_number = big_new_int(a, 10);
    tmpl.dns_names = STRS(a, "example.com");
    tmpl.signature_algorithm = X509_MD5_WITH_RSA;
    RsaPrivateKey *k = test_key(a);
    BrokenSigner bs = {rsa_private_key_public(k)};
    Error err = BURROW_NO_ERROR;
    x509_create_certificate(a, (IoReader){0}, &tmpl, &tmpl, bs.pub,
                            (CryptoSigner){&broken_signer_vt, &bs}, &err);
    if (!BURROW_FAILED(err))
        testing_t_fatalf_v(t, "CreateCertificate didn't fail when SignatureAlgorithm = "
                              "MD5WithRSA");
    arena_free(&ar);
}

static void TestOmitEmptyExtensions(TestingT *t) {
    ARENA(a);
    EcdsaPrivateKey *k = ec_key(a, elliptic_p256());
    X509Certificate tmpl = {0};
    tmpl.serial_number = big_new_int(a, 1);
    tmpl.subject.common_name = BURROW_S(":)");
    tmpl.not_after = time_add(time_now(), TIME_HOUR);
    tmpl.not_before = time_add(time_now(), -TIME_HOUR);
    Error err = BURROW_NO_ERROR;
    Slice der = x509_create_certificate(a, (IoReader){0}, &tmpl, &tmpl,
                                        ecdsa_private_key_public(k),
                                        ecdsa_private_key_signer(k), &err);
    if (BURROW_FAILED(err))
        testing_t_fatalf_v(t, "%v", err);
    if (bytes_contains(der, BYTES(0xA3, 0x02, 0x30, 0x00)))
        testing_t_errorf_v(t, "DER encoding contains the an empty extensions SEQUENCE");
    arena_free(&ar);
}

static void TestCreateNegativeSerial(TestingT *t) {
    ARENA(a);
    EcdsaPrivateKey *k = ec_key(a, elliptic_p256());
    X509Certificate tmpl = {0};
    tmpl.serial_number = big_new_int(a, -1);
    tmpl.subject.common_name = BURROW_S(":)");
    tmpl.not_after = time_add(time_now(), TIME_HOUR);
    tmpl.not_before = time_add(time_now(), -TIME_HOUR);
    Error err = BURROW_NO_ERROR;
    x509_create_certificate(a, (IoReader){0}, &tmpl, &tmpl, ecdsa_private_key_public(k),
                            ecdsa_private_key_signer(k), &err);
    if (!text_is(err, "x509: serial number must be positive"))
        testing_t_errorf_v(
            t, "CreateCertificate returned unexpected error: want %q, got %v",
            "x509: serial number must be positive", err);
    arena_free(&ar);
}

static void TestCertificateOIDPoliciesGODEBUG(TestingT *t) {
    ARENA(a);
    burrow__x509_godebug_set("x509usepolicies=0");
    X509Certificate tmpl = simple_template(a, "Cert");
    Asn1ObjectIdentifier id = ASN1_OID(1, 2, 3);
    tmpl.policy_identifiers = one(a, TYPE_ASN1_OBJECT_IDENTIFIER, &id);
    X509OID expect = MUST_OID(a, 1, 2, 3);
    X509Certificate *cert = serialise_and_parse(t, a, &tmpl);
    if (cert->policy_identifiers.len != 1 ||
        !asn1_object_identifier_equal(
            ((const Asn1ObjectIdentifier *)cert->policy_identifiers.p)[0], id))
        testing_t_errorf_v(t, "cert.PolicyIdentifiers = %v, want: [1.2.3]",
                           cert->policy_identifiers.len);
    if (!oids_equal(cert->policies, one(a, TYPE_X509_OID, &expect)))
        testing_t_errorf_v(t, "cert.Policies is not [1.2.3]");
    burrow__x509_godebug_set(NULL);
    arena_free(&ar);
}

static void TestCertificatePolicies(TestingT *t) {
    ARENA(a);
    X509Certificate tmpl = simple_template(a, "Cert");
    Asn1ObjectIdentifier id = ASN1_OID(1, 2, 3);
    tmpl.policy_identifiers = one(a, TYPE_ASN1_OBJECT_IDENTIFIER, &id);
    X509OID policy = MUST_OID(a, 1, 2, (uint64_t)UINT32_MAX + 1);
    tmpl.policies = one(a, TYPE_X509_OID, &policy);
    Slice expect_policies = one(a, TYPE_X509_OID, &policy);

    X509Certificate *cert = serialise_and_parse(t, a, &tmpl);
    if (!oids_equal(cert->policies, expect_policies))
        testing_t_errorf_v(t, "cert.Policies is not [1.2.4294967296]");

    burrow__x509_godebug_set("x509usepolicies=1");
    cert = serialise_and_parse(t, a, &tmpl);
    if (!oids_equal(cert->policies, expect_policies))
        testing_t_errorf_v(t, "with x509usepolicies=1, cert.Policies is not "
                              "[1.2.4294967296]");
    burrow__x509_godebug_set(NULL);
    arena_free(&ar);
}

/* TestRejectCriticalAKI, TestRejectCriticalAIA and TestRejectCriticalSKI:
 * the parser refuses a certificate with the extension id marked critical. */
static void reject_critical(TestingT *t, Asn1ObjectIdentifier id,
                            const char *expected_err) {
    ARENA(a);
    X509Certificate tmpl = simple_template(a, "Cert");
    PkixExtension ext = {id, true, BYTES(1, 2, 3)};
    tmpl.extra_extensions = one(a, TYPE_PKIX_EXTENSION, &ext);
    Slice der;
    Error err = create_with_test_key(a, &tmpl, &der);
    if (BURROW_FAILED(err))
        testing_t_fatalf_v(t, "CreateCertificate() unexpected error: %v", err);
    x509_parse_certificate(a, der, &err);
    if (!text_is(err, expected_err))
        testing_t_fatalf_v(t, "ParseCertificate() unexpected error: %v, want: %s", err,
                           expected_err);
    arena_free(&ar);
}

static void TestRejectCriticalAKI(TestingT *t) {
    reject_critical(t, OID(oid_extension_authority_key_id),
                    "x509: authority key identifier incorrectly marked critical");
}

static void TestRejectCriticalAIA(TestingT *t) {
    reject_critical(t, OID(oid_extension_authority_info_access),
                    "x509: authority info access incorrectly marked critical");
}

static void TestRejectCriticalSKI(TestingT *t) {
    reject_critical(t, OID(oid_extension_subject_key_id),
                    "x509: subject key identifier incorrectly marked critical");
}

/* messageSigner: a MessageSigner whose plain Sign is not implemented. */
BURROW_PTR_TYPE(TestRsaKeyPtr, RsaPrivateKey);

#define MESSAGE_SIGNER_FIELDS(F, T) F(T, TestRsaKeyPtr, Key, "")

BURROW_STRUCT_DECL(MessageSigner, MESSAGE_SIGNER_FIELDS);

static Slice message_signer_sign_message(MessageSigner *s, CryptoAllocArg a,
                                         IoReader rand, Bytes msg,
                                         CryptoSignerOpts opts, CryptoErrorArg err) {
    if (opts.vt->self_type == TYPE_RSA_PSS_OPTIONS) {
        BURROW_OUT(err, errors_new(error_allocator(),
                                   BURROW_S("PSSOptions passed instead of hash")));
        return slice_nil(TYPE_BYTE);
    }
    CryptoHash hash = opts.vt->hash_func(opts.data);
    Hash h = crypto_hash_new(hash, a);
    hash_write(h, msg, NULL);
    Slice tbs = hash_sum(a, h, slice_nil(TYPE_BYTE));
    return rsa_private_key_sign(s->Key, a, rand, tbs, opts, err);
}

#define MESSAGE_SIGNER_METHODS(M, T)                                                   \
    M(T, SignMessage, message_signer_sign_message, CRYPTO_SIG_SIGN_MESSAGE)

BURROW_STRUCT_DEFINE_METHODS(MessageSigner, MESSAGE_SIGNER_FIELDS,
                             MESSAGE_SIGNER_METHODS);

static CryptoPublicKey message_signer_public(void *self) {
    return rsa_private_key_public(((MessageSigner *)self)->Key);
}

static Slice message_signer_sign(void *self, Alloc *a, IoReader rand, Slice digest,
                                 CryptoSignerOpts opts, Error *err) {
    (void)self;
    (void)a;
    (void)rand;
    (void)digest;
    (void)opts;
    BURROW_OUT(err, errors_new(error_allocator(), BURROW_S("unimplemented")));
    return slice_nil(TYPE_BYTE);
}

static const CryptoSignerVT message_signer_vt = {
    TYPE_OF(MessageSigner), message_signer_public, message_signer_sign};

static void TestMessageSigner(TestingT *t) {
    ARENA(a);
    X509Certificate tmpl = simple_template(a, "Cert");
    tmpl.signature_algorithm = X509_SHA256_WITH_RSA;
    tmpl.basic_constraints_valid = true;
    tmpl.is_ca = true;
    MessageSigner ms = {test_key(a)};
    Error err = BURROW_NO_ERROR;
    Slice der = x509_create_certificate(a, (IoReader){0}, &tmpl, &tmpl,
                                        rsa_private_key_public(ms.Key),
                                        (CryptoSigner){&message_signer_vt, &ms}, &err);
    if (BURROW_FAILED(err))
        testing_t_fatalf_v(t, "CreateCertificate failed: %v", err);
    X509Certificate *cert = x509_parse_certificate(a, der, &err);
    if (BURROW_FAILED(err))
        testing_t_fatalf_v(t, "ParseCertificate failed: %v", err);
    err = x509_certificate_check_signature_from(cert, cert);
    if (BURROW_FAILED(err))
        testing_t_fatalf_v(t, "CheckSignatureFrom failed: %v", err);
    arena_free(&ar);
}

static void TestCreateCertificateNegativeMaxPathLength(TestingT *t) {
    ARENA(a);
    X509Certificate tmpl = simple_template(a, "TEST");
    tmpl.basic_constraints_valid = true;
    tmpl.is_ca = true;
    /* CreateCertificate treats -1 in the same way as: MaxPathLen == 0 &&
     * MaxPathLenZero == false. */
    tmpl.max_path_len = -1;
    Error err = create_with_test_key(a, &tmpl, NULL);
    if (BURROW_FAILED(err))
        testing_t_fatalf_v(t, "CreateCertificate() unexpected error: %v", err);

    tmpl.max_path_len = -2;
    err = create_with_test_key(a, &tmpl, NULL);
    if (!text_is(err, "x509: invalid MaxPathLen, must be greater or equal to -1"))
        testing_t_fatalf_v(
            t,
            "CreateCertificate() = %v; want = \"x509: invalid MaxPathLen, "
            "must be greater or equal to -1\"",
            err);
    arena_free(&ar);
}

static void TestIPv4MappedIPsSANCreate(TestingT *t) {
    ARENA(a);
    X509Certificate tmpl = {0};
    tmpl.serial_number = big_new_int(a, 1);
    tmpl.not_before = time_add(time_now(), -24 * TIME_HOUR);
    tmpl.not_after = time_add(time_now(), 24 * TIME_HOUR);
    tmpl.dns_names = STRS(a, "localhost");
    NetIP ip = net_parse_ip(a, BURROW_S("::ffff:192.0.2.1"));
    tmpl.ip_addresses = one(a, TYPE_NET_IP, &ip);
    X509Certificate *cert = serialise_and_parse(t, a, &tmpl);
    if (cert->ip_addresses.len != 1)
        testing_t_fatalf_v(
            t,
            "unexpected number of IP addresses in parsed certificate, got "
            "%d, want 1",
            cert->ip_addresses.len);
    Int n = ((const NetIP *)cert->ip_addresses.p)[0].len;
    if (n != 4)
        testing_t_fatalf_v(
            t,
            "unexpected IP address length in parsed certificate, got %d, "
            "want 4",
            n);
    arena_free(&ar);
}

static void TestIPv4MappedIPsConstraintCreate(TestingT *t) {
    ARENA(a);
    X509Certificate tmpl = {0};
    tmpl.serial_number = big_new_int(a, 1);
    tmpl.not_before = time_add(time_now(), -24 * TIME_HOUR);
    tmpl.not_after = time_add(time_now(), 24 * TIME_HOUR);
    tmpl.dns_names = STRS(a, "localhost");
    NetIPNet *mapped = parse_cidr(a, "::ffff:192.0.2.1/128");
    tmpl.permitted_ip_ranges = one(a, TYPE_X509_IP_NET_PTR, &mapped);
    if (!BURROW_FAILED(create_with_test_key(a, &tmpl, NULL)))
        testing_t_fatalf_v(t,
                           "unexpected success creating certificate with IPv4-mapped "
                           "IPv6 address constraint");
    arena_free(&ar);
}

/* The creation half of TestMLDSA, for one parameter set. */
static void mldsa_certificate(TestingT *t, Alloc *a, const char *private_pem,
                              const char *const *cert_pem) {
    Error err = BURROW_NO_ERROR;
    Any priv = x509_parse_pkcs8_private_key(a, pem_bytes(a, private_pem), &err);
    if (BURROW_FAILED(err) || priv.t != TYPE_MLDSA_PRIVATE_KEY)
        testing_t_fatalf_v(t, "ParsePKCS8PrivateKey failed: %v", err);
    const MldsaPrivateKey *sk = priv.data;
    const MldsaPublicKey *pk = mldsa_private_key_public_key(sk);

    X509Certificate *cert =
        x509_parse_certificate(a, pem_bytes(a, joined(a, cert_pem)), &err);
    if (BURROW_FAILED(err))
        testing_t_fatalf_v(t, "ParseCertificate failed: %v", err);
    if (!mldsa_public_key_equal(pk, cert->public_key))
        testing_t_fatalf_v(t,
                           "ParseCertificate returned certificate with public key that "
                           "does not match private key");
    if (cert->public_key_algorithm != X509_MLDSA)
        testing_t_fatalf_v(
            t,
            "ParseCertificate returned certificate with wrong public key "
            "algorithm: got %d, want MLDSA",
            cert->public_key_algorithm);
    const struct {
        X509SignatureAlgorithm algo;
        const MldsaParameters *params;
        const char *name;
    } sets[] = {
        {X509_MLDSA44, mldsa_mldsa44(), "ML-DSA-44"},
        {X509_MLDSA65, mldsa_mldsa65(), "ML-DSA-65"},
        {X509_MLDSA87, mldsa_mldsa87(), "ML-DSA-87"},
    };
    const MldsaParameters *cert_params =
        mldsa_public_key_parameters((const MldsaPublicKey *)cert->public_key.data);
    bool known = false;
    for (Int i = 0; i < 3; i++) {
        if (sets[i].params != cert_params)
            continue;
        known = true;
        if (cert->signature_algorithm != sets[i].algo)
            testing_t_fatalf_v(
                t,
                "ParseCertificate returned certificate with wrong signature "
                "algorithm: got %d, want %s",
                cert->signature_algorithm, sets[i].name);
    }
    if (!known)
        testing_t_fatalf_v(t,
                           "ParseCertificate returned certificate with unknown MLDSA "
                           "parameters");
    err = x509_certificate_check_signature_from(cert, cert);
    if (BURROW_FAILED(err))
        testing_t_fatalf_v(t, "CheckSignatureFrom failed: %v", err);

    Slice got = x509_create_certificate(a, (IoReader){0}, cert, cert,
                                        mldsa_private_key_public(sk),
                                        mldsa_private_key_signer(sk), &err);
    if (BURROW_FAILED(err))
        testing_t_fatalf_v(t, "CreateCertificate failed: %v", err);
    X509Certificate *cert2 = x509_parse_certificate(a, got, &err);
    if (BURROW_FAILED(err))
        testing_t_fatalf_v(t, "ParseCertificate failed: %v", err);
    if (!mldsa_public_key_equal(pk, cert2->public_key))
        testing_t_fatalf_v(t,
                           "ParseCertificate returned certificate with public key that "
                           "does not match private key");
    if (cert2->signature_algorithm != cert->signature_algorithm)
        testing_t_fatalf_v(t,
                           "ParseCertificate returned certificate with wrong signature "
                           "algorithm: got %d, want %d",
                           cert2->signature_algorithm, cert->signature_algorithm);
    err = x509_certificate_check_signature_from(cert2, cert2);
    if (BURROW_FAILED(err))
        testing_t_fatalf_v(t, "CheckSignatureFrom failed: %v", err);

    Slice msg = cbytes("test message");
    CryptoHash none = 0;
    Slice sig = mldsa_private_key_sign(sk, a, (IoReader){0}, msg,
                                       crypto_hash_as_signer_opts(&none), &err);
    if (BURROW_FAILED(err))
        testing_t_fatalf_v(t, "Sign failed: %v", err);
    for (Int i = 0; i < 3; i++) {
        err = x509_certificate_check_signature(cert, sets[i].algo, msg, sig);
        if (sets[i].params == cert_params) {
            if (BURROW_FAILED(err))
                testing_t_errorf_v(t, "CheckSignature(%s): got %v, want nil",
                                   sets[i].name, err);
        } else if (!contains(err, sets[i].name)) {
            testing_t_errorf_v(
                t,
                "CheckSignature(%s): got %v, want parameter mismatch error "
                "mentioning %s",
                sets[i].name, err, sets[i].name);
        }
    }
}

static void TestMLDSACertificates(TestingT *t) {
    ARENA(a);
    mldsa_certificate(t, a, mldsa44_private_key_pem, mldsa44_certificate_pem);
    mldsa_certificate(t, a, mldsa65_private_key_pem, mldsa65_certificate_pem);
    mldsa_certificate(t, a, mldsa87_private_key_pem, mldsa87_certificate_pem);
    arena_free(&ar);
}

/* ----------------------------------------------------------- CSR creation */

typedef struct CsrCase {
    const char *name;
    CryptoSigner priv;
    X509SignatureAlgorithm sig_algo;
} CsrCase;

static void TestCreateCertificateRequest(TestingT *t) {
    ARENA(a);
    RsaPrivateKey *rsa_priv = test_key(a);
    Ed25519Signer es;
    CryptoSigner ed25519_signer = ed25519_key(a, &es);
    MldsaPrivateKey *mldsa_priv = mldsa_key(a, mldsa_mldsa44());
    const CsrCase tests[] = {
        {"RSA", rsa_private_key_signer(rsa_priv), X509_SHA256_WITH_RSA},
        {"RSA-PSS-SHA256", rsa_private_key_signer(rsa_priv), X509_SHA256_WITH_RSAPSS},
        {"ECDSA-256", ecdsa_private_key_signer(ec_key(a, elliptic_p256())),
         X509_ECDSA_WITH_SHA256},
        {"ECDSA-384", ecdsa_private_key_signer(ec_key(a, elliptic_p384())),
         X509_ECDSA_WITH_SHA256},
        {"ECDSA-521", ecdsa_private_key_signer(ec_key(a, elliptic_p521())),
         X509_ECDSA_WITH_SHA256},
        {"Ed25519", ed25519_signer, X509_PURE_ED25519},
        {"ML-DSA-44", mldsa_private_key_signer(mldsa_priv), X509_MLDSA44},
    };
    for (Int i = 0; i < (Int)(sizeof tests / sizeof tests[0]); i++) {
        const CsrCase *test = &tests[i];
        X509CertificateRequest tmpl = {0};
        tmpl.subject.common_name = BURROW_S("test.example.com");
        tmpl.subject.organization = STRS(a, "\xce\xa3 Acme Co");
        tmpl.signature_algorithm = test->sig_algo;
        tmpl.dns_names = STRS(a, "test.example.com");
        tmpl.email_addresses = STRS(a, "gopher@golang.org");
        const NetIP ips[] = {net_ip_to4(net_ipv4(a, 127, 0, 0, 1)),
                             net_parse_ip(a, BURROW_S("2001:4860:0:2001::68"))};
        tmpl.ip_addresses = slice_append(a, slice_nil(TYPE_NET_IP), ips, 2);

        Error err = BURROW_NO_ERROR;
        Slice der =
            x509_create_certificate_request(a, (IoReader){0}, &tmpl, test->priv, &err);
        if (BURROW_FAILED(err)) {
            testing_t_errorf_v(t, "%s: failed to create certificate request: %v",
                               test->name, err);
            continue;
        }
        X509CertificateRequest *out = x509_parse_certificate_request(a, der, &err);
        if (BURROW_FAILED(err)) {
            testing_t_errorf_v(t, "%s: failed to create certificate request: %v",
                               test->name, err);
            continue;
        }
        err = x509_certificate_request_check_signature(out);
        if (BURROW_FAILED(err)) {
            testing_t_errorf_v(t,
                               "%s: failed to check certificate request signature: %v",
                               test->name, err);
            continue;
        }
        if (!str_eq(out->subject.common_name, tmpl.subject.common_name))
            testing_t_errorf_v(t,
                               "%s: output subject common name and template subject "
                               "common name don't match",
                               test->name);
        else if (out->subject.organization.len != tmpl.subject.organization.len)
            testing_t_errorf_v(t,
                               "%s: output subject organisation and template subject "
                               "organisation don't match",
                               test->name);
        else if (out->dns_names.len != tmpl.dns_names.len)
            testing_t_errorf_v(
                t, "%s: output DNS names and template DNS names don't match",
                test->name);
        else if (out->email_addresses.len != tmpl.email_addresses.len)
            testing_t_errorf_v(
                t,
                "%s: output email addresses and template email addresses "
                "don't match",
                test->name);
        else if (out->ip_addresses.len != tmpl.ip_addresses.len)
            testing_t_errorf_v(
                t,
                "%s: output IP addresses and template IP addresses names "
                "don't match",
                test->name);
    }
    arena_free(&ar);
}

static Slice marshal_sans(Alloc *a, Slice dns_names) {
    Error err = BURROW_NO_ERROR;
    Slice b = burrow__x509_marshal_sans(a, dns_names, slice_nil(TYPE_STRING),
                                        slice_nil(TYPE_NET_IP),
                                        slice_nil(TYPE_X509_URL_PTR), &err);
    if (BURROW_FAILED(err))
        panic_str(BURROW_S("x509_create_test: marshalSANs failed"));
    return b;
}

static void TestCertificateRequestOverrides(TestingT *t) {
    ARENA(a);
    X509CertificateRequest tmpl = {0};
    tmpl.subject.common_name = BURROW_S("test.example.com");
    tmpl.subject.organization = STRS(a, "\xce\xa3 Acme Co");
    tmpl.dns_names = STRS(a, "test.example.com");
    /* An explicit extension should override the DNSNames from the
     * template. */
    PkixExtension san = {OID(oid_extension_subject_alt_name), true,
                         marshal_sans(a, STRS(a, "foo.example.com"))};
    tmpl.extra_extensions = one(a, TYPE_PKIX_EXTENSION, &san);

    X509CertificateRequest *csr = marshal_and_parse_csr(t, a, &tmpl);
    if (!strs_are_one(csr->dns_names, "foo.example.com"))
        testing_t_errorf_v(t, "Extension did not override template. Got %d names",
                           csr->dns_names.len);
    const PkixExtension *ext = csr->extensions.p;
    if (csr->extensions.len != 1 ||
        !asn1_object_identifier_equal(ext[0].id, OID(oid_extension_subject_alt_name)) ||
        !ext[0].critical)
        testing_t_errorf_v(t, "SAN extension was not faithfully copied");

    /* If there is already an attribute with X.509 extensions then the extra
     * extensions should be added to it rather than creating a CSR with two
     * extension attributes. */
    PkixAttributeTypeAndValue aia = {OID(oid_extension_authority_info_access),
                                     bytes_any(a, cbytes("foo"))};
    Slice values = one(a, TYPE_PKIX_ATTRIBUTE_TYPE_AND_VALUE, &aia);
    PkixAttributeTypeAndValueSET set = {
        OID(oid_extension_request),
        one(a, TYPE_PKIX_RELATIVE_DISTINGUISHED_NAME_SET, &values)};
    tmpl.attributes = one(a, TYPE_PKIX_ATTRIBUTE_TYPE_AND_VALUE_SET, &set);

    csr = marshal_and_parse_csr(t, a, &tmpl);
    if (csr->attributes.len != 1)
        testing_t_errorf_v(t, "incorrect number of attributes: %d\n",
                           csr->attributes.len);
    const PkixAttributeTypeAndValueSET *attr = csr->attributes.p;
    if (csr->attributes.len < 1 ||
        !asn1_object_identifier_equal(attr[0].type, OID(oid_extension_request)) ||
        attr[0].value.len != 1 || ((const Slice *)attr[0].value.p)[0].len != 2)
        testing_t_errorf_v(t, "bad attributes");

    /* Extensions in Attributes should override those in ExtraExtensions. */
    PkixAttributeTypeAndValue san2 = {
        OID(oid_extension_subject_alt_name),
        bytes_any(a, marshal_sans(a, STRS(a, "foo2.example.com")))};
    ((Slice *)set.value.p)[0] = slice_append(a, values, &san2, 1);
    csr = marshal_and_parse_csr(t, a, &tmpl);
    if (!strs_are_one(csr->dns_names, "foo2.example.com"))
        testing_t_errorf_v(t,
                           "Attributes did not override ExtraExtensions. Got %d names",
                           csr->dns_names.len);
    arena_free(&ar);
}

static void TestCertificateRequestRoundtripFields(TestingT *t) {
    ARENA(a);
    Url *const urls[] = {parse_uri(a, "https://example.com/_"),
                         parse_uri(a, "https://example.org/_")};
    const NetIP ips[] = {net_ipv4(a, 192, 0, 2, 0), net_ipv6_loopback};
    X509CertificateRequest in = {0};
    in.dns_names = STRS(a, "example.com", "example.org");
    in.email_addresses = STRS(a, "a@example.com", "b@example.com");
    in.ip_addresses = slice_append(a, slice_nil(TYPE_NET_IP), ips, 2);
    in.uris = slice_append(a, slice_nil(TYPE_X509_URL_PTR), urls, 2);
    X509CertificateRequest *out = marshal_and_parse_csr(t, a, &in);

    if (!strs_equal(in.dns_names, out->dns_names))
        testing_t_fatalf_v(t, "Unexpected DNSNames");
    if (!strs_equal(in.email_addresses, out->email_addresses))
        testing_t_fatalf_v(t, "Unexpected EmailAddresses");
    const NetIP *got_ips = out->ip_addresses.p;
    if (out->ip_addresses.len != 2 || !net_ip_equal(ips[0], got_ips[0]) ||
        !net_ip_equal(ips[1], got_ips[1]))
        testing_t_fatalf_v(t, "Unexpected IPAddresses");
    Url *const *got_urls = out->uris.p;
    if (out->uris.len != 2 ||
        !str_eq(url_string(got_urls[0], a), url_string(urls[0], a)) ||
        !str_eq(url_string(got_urls[1], a), url_string(urls[1], a)))
        testing_t_fatalf_v(t, "Unexpected URIs");
    arena_free(&ar);
}

/* ------------------------------------------------------------ CRL creation */

static void TestCRLCreation(TestingT *t) {
    ARENA(a);
    Error err = BURROW_NO_ERROR;
    RsaPrivateKey *priv_rsa = test_key(a);
    X509Certificate *cert_rsa =
        x509_parse_certificate(a, pem_bytes(a, pem_certificate), &err);
    Any priv_ed25519 =
        x509_parse_pkcs8_private_key(a, pem_bytes(a, ed25519_crl_key), &err);
    X509Certificate *cert_ed25519 =
        x509_parse_certificate(a, pem_bytes(a, ed25519_crl_certificate), &err);
    if (BURROW_FAILED(err))
        testing_t_fatalf_v(t, "%v", err);
    Ed25519Signer es;
    const struct {
        const char *name;
        CryptoSigner priv;
        const X509Certificate *cert;
    } tests[] = {
        {"RSA CA", rsa_private_key_signer(priv_rsa), cert_rsa},
        {"Ed25519 CA", any_signer(priv_ed25519, &es), cert_ed25519},
    };

    TimeLocation *loc = time_fixed_zone(a, BURROW_S("Oz/Atlantis"), 2 * 60 * 60);
    Time now = time_in(time_from_unix(1000, 0), loc);
    Time now_utc = time_utc(now);
    Time expiry = time_from_unix(10000, 0);
    const PkixRevokedCertificate revoked[] = {
        {big_new_int(a, 1), now_utc, slice_nil(TYPE_PKIX_EXTENSION)},
        /* RevocationTime should be converted to UTC before marshaling. */
        {big_new_int(a, 42), now, slice_nil(TYPE_PKIX_EXTENSION)},
    };
    Slice revoked_certs =
        slice_append(a, slice_nil(TYPE_PKIX_REVOKED_CERTIFICATE), revoked, 2);

    for (Int i = 0; i < (Int)(sizeof tests / sizeof tests[0]); i++) {
        err = BURROW_NO_ERROR;
        Slice crl_bytes =
            x509_certificate_create_crl(tests[i].cert, a, (IoReader){0}, tests[i].priv,
                                        revoked_certs, now, expiry, &err);
        if (BURROW_FAILED(err)) {
            testing_t_errorf_v(t, "%s: error creating CRL: %v", tests[i].name, err);
            continue;
        }
        PkixCertificateList *parsed = x509_parse_dercrl(a, crl_bytes, &err);
        if (BURROW_FAILED(err)) {
            testing_t_errorf_v(t, "%s: error reparsing CRL: %v", tests[i].name, err);
            continue;
        }
        Slice got = parsed->tbs_cert_list.revoked_certificates;
        const PkixRevokedCertificate *rc = got.p;
        bool ok = got.len == 2;
        for (Int j = 0; ok && j < 2; j++)
            ok = big_int_cmp(rc[j].serial_number, revoked[j].serial_number) == 0 &&
                 time_equal(rc[j].revocation_time, now_utc) &&
                 time_location(rc[j].revocation_time) == time_utc_loc &&
                 rc[j].extensions.len == 0;
        if (!ok)
            testing_t_errorf_v(t, "%s: RevokedCertificates mismatch", tests[i].name);
    }
    arena_free(&ar);
}

/* An issuer as Go's TestCreateRevocationList makes them by hand. */
static X509Certificate *crl_issuer(Alloc *a, X509KeyUsage usage, bool subject) {
    X509Certificate *c = mem_alloc(a, sizeof *c, _Alignof(X509Certificate));
    c->key_usage = usage;
    if (subject) {
        c->subject.common_name = BURROW_S("testing");
        c->subject_key_id = BYTES(1, 2, 3);
    }
    return c;
}

static X509RevocationList *crl_template(Alloc *a, Int number) {
    X509RevocationList *rl = mem_alloc(a, sizeof *rl, _Alignof(X509RevocationList));
    if (number >= 0)
        rl->number = big_new_int(a, number);
    rl->this_update = time_add((Time){0}, 24 * TIME_HOUR);
    rl->next_update = time_add((Time){0}, 48 * TIME_HOUR);
    return rl;
}

static Slice one_entry(Alloc *a, Int reason_code, Slice extra_extensions) {
    X509RevocationListEntry e = {0};
    e.serial_number = big_new_int(a, 2);
    e.revocation_time = time_add((Time){0}, TIME_HOUR);
    e.reason_code = reason_code;
    e.extra_extensions = extra_extensions;
    return one(a, TYPE_X509_REVOCATION_LIST_ENTRY, &e);
}

typedef struct CreateCRLCase {
    const char *name;
    CryptoSigner key;
    const X509Certificate *issuer;
    const X509RevocationList *template_;
    const char *expected_error;
} CreateCRLCase;

static void check_created_crl(TestingT *t, Alloc *a, const CreateCRLCase *tc,
                              Slice crl) {
    Error err = BURROW_NO_ERROR;
    X509RevocationList *parsed = x509_parse_revocation_list(a, crl, &err);
    if (BURROW_FAILED(err)) {
        testing_t_errorf_v(t, "%s: Failed to parse generated CRL: %v", tc->name, err);
        return;
    }
    const X509RevocationList *tmpl = tc->template_;
    if (tmpl->signature_algorithm != X509_UNKNOWN_SIGNATURE_ALGORITHM &&
        parsed->signature_algorithm != tmpl->signature_algorithm) {
        testing_t_errorf_v(t, "%s: SignatureAlgorithm mismatch: got %d; want %d.",
                           tc->name, parsed->signature_algorithm,
                           tmpl->signature_algorithm);
        return;
    }

    if (tmpl->revoked_certificates.len > 0) {
        const PkixRevokedCertificate *got = parsed->revoked_certificates.p;
        const PkixRevokedCertificate *want = tmpl->revoked_certificates.p;
        bool ok = parsed->revoked_certificates.len == tmpl->revoked_certificates.len;
        for (Int i = 0; ok && i < tmpl->revoked_certificates.len; i++) {
            ok = big_int_cmp(got[i].serial_number, want[i].serial_number) == 0 &&
                 time_equal(got[i].revocation_time, want[i].revocation_time) &&
                 got[i].extensions.len == want[i].extensions.len;
            for (Int j = 0; ok && j < want[i].extensions.len; j++)
                ok = extension_equal(&((const PkixExtension *)got[i].extensions.p)[j],
                                     &((const PkixExtension *)want[i].extensions.p)[j]);
        }
        if (!ok) {
            testing_t_errorf_v(t, "%s: RevokedCertificates mismatch", tc->name);
            return;
        }
    } else {
        if (parsed->revoked_certificate_entries.len !=
            tmpl->revoked_certificate_entries.len) {
            testing_t_errorf_v(t,
                               "%s: RevokedCertificateEntries length mismatch: got %d; "
                               "want %d.",
                               tc->name, parsed->revoked_certificate_entries.len,
                               tmpl->revoked_certificate_entries.len);
            return;
        }
        const X509RevocationListEntry *got = parsed->revoked_certificate_entries.p;
        const X509RevocationListEntry *want = tmpl->revoked_certificate_entries.p;
        for (Int i = 0; i < parsed->revoked_certificate_entries.len; i++) {
            if (big_int_cmp(got[i].serial_number, want[i].serial_number) != 0 ||
                !time_equal(got[i].revocation_time, want[i].revocation_time) ||
                got[i].reason_code != want[i].reason_code) {
                testing_t_errorf_v(t, "%s: RevocationListEntry %d mismatch", tc->name,
                                   i);
                return;
            }
        }
    }

    Int n_extra = tmpl->extra_extensions.len;
    if (parsed->extensions.len != 2 + n_extra) {
        testing_t_errorf_v(
            t,
            "%s: Generated CRL has wrong number of extensions, wanted: %d, "
            "got: %d",
            tc->name, 2 + n_extra, parsed->extensions.len);
        return;
    }
    const PkixExtension *exts = parsed->extensions.p;

    /* authKeyId{Id: SubjectKeyId}: a SEQUENCE of the [0] key identifier. */
    Slice ski = tc->issuer->subject_key_id;
    Slice expected_aki = slice_nil(TYPE_BYTE);
    const Byte aki_head[] = {0x30, (Byte)(ski.len + 2), 0x80, (Byte)ski.len};
    expected_aki = slice_append(a, expected_aki, aki_head, 4);
    expected_aki = slice_append(a, expected_aki, ski.p, ski.len);
    PkixExtension aki_ext = {OID(oid_extension_authority_key_id), false, expected_aki};
    if (!extension_equal(&exts[0], &aki_ext)) {
        testing_t_errorf_v(t, "%s: Unexpected first extension: got %x", tc->name,
                           exts[0].value);
        return;
    }
    TestBigIntPtr number = tmpl->number;
    Slice expected_num =
        asn1_marshal(a, BURROW_ANY(TYPE_OF(TestBigIntPtr), &number), &err);
    if (BURROW_FAILED(err))
        testing_t_fatalf_v(t, "asn1.Marshal failed: %v", err);
    PkixExtension crl_ext = {OID(oid_extension_crl_number), false, expected_num};
    if (!extension_equal(&exts[1], &crl_ext)) {
        testing_t_errorf_v(t, "%s: Unexpected second extension: got %x", tc->name,
                           exts[1].value);
        return;
    }

    /* Go compares RawSubject to RawIssuer when the issuer has one, and the
     * issuer name through an RDNSequence round trip when it was made by hand
     * without one. */
    if (tc->issuer->raw_subject.len > 0) {
        if (!bytes_equal(tc->issuer->raw_subject, parsed->raw_issuer)) {
            testing_t_errorf_v(t, "%s: Unexpected issuer subject; wanted: %x, got: %x",
                               tc->name, tc->issuer->raw_subject, parsed->raw_issuer);
            return;
        }
    } else {
        PkixRDNSequence want = pkix_name_to_rdn_sequence(tc->issuer->subject, a);
        PkixRDNSequence got = pkix_name_to_rdn_sequence(parsed->issuer, a);
        if (!str_eq(pkix_rdn_sequence_string(want, a),
                    pkix_rdn_sequence_string(got, a))) {
            testing_t_errorf_v(
                t,
                "%s: Expected issuer.Subject, parsedCRL.Issuer to be the "
                "same; wanted: %s, got: %s",
                tc->name, pkix_name_string(tc->issuer->subject, a),
                pkix_name_string(parsed->issuer, a));
            return;
        }
    }

    for (Int i = 0; i < n_extra; i++) {
        if (!extension_equal(&exts[2 + i],
                             &((const PkixExtension *)tmpl->extra_extensions.p)[i])) {
            testing_t_errorf_v(t, "%s: Extensions mismatch at %d", tc->name, i);
            return;
        }
    }
    if (tmpl->number != NULL && parsed->number == NULL) {
        testing_t_errorf_v(t, "%s: Generated CRL missing Number", tc->name);
        return;
    }
    if (tmpl->number != NULL && big_int_cmp(tmpl->number, parsed->number) != 0) {
        testing_t_errorf_v(t, "%s: Generated CRL has wrong Number", tc->name);
        return;
    }
    if (!bytes_equal(parsed->authority_key_id, tc->issuer->subject_key_id))
        testing_t_errorf_v(
            t, "%s: Generated CRL has wrong AuthorityKeyId: got %x, want %x", tc->name,
            parsed->authority_key_id, tc->issuer->subject_key_id);
}

static void TestCreateRevocationList(TestingT *t) {
    ARENA(a);
    Error err = BURROW_NO_ERROR;
    EcdsaPrivateKey *ec256_priv = ec_key(a, elliptic_p256());
    CryptoSigner ec256 = ecdsa_private_key_signer(ec256_priv);
    Ed25519Signer es;
    CryptoSigner ed25519_priv = ed25519_key(a, &es);
    MldsaPrivateKey *mldsa_priv = mldsa_key(a, mldsa_mldsa44());

    X509Certificate *utf8_ca =
        x509_parse_certificate(a, from_base64(a, utf8_ca_base64), &err);
    Any utf8_key_raw =
        x509_parse_pkcs8_private_key(a, from_base64(a, utf8_key_base64), &err);
    if (BURROW_FAILED(err))
        testing_t_fatalf_v(t, "%v", err);
    Ed25519Signer unused;
    CryptoSigner utf8_key = any_signer(utf8_key_raw, &unused);

    X509Certificate *good = crl_issuer(a, X509_KEY_USAGE_CRL_SIGN, true);
    X509RevocationList *empty =
        mem_alloc(a, sizeof *empty, _Alignof(X509RevocationList));

    X509RevocationList *before = crl_template(a, -1);
    before->this_update = time_add((Time){0}, TIME_HOUR);
    before->next_update = (Time){0};

    X509RevocationList *nil_number = crl_template(a, -1);

    Byte long_bytes[21] = {1};
    X509RevocationList *long_number = crl_template(a, -1);
    long_number->number =
        big_int_set_bytes(big_new_int(a, 0), bytes_of(long_bytes, 21));
    Byte msb_bytes[20] = {255};
    X509RevocationList *msb_number = crl_template(a, -1);
    msb_number->number = big_int_set_bytes(big_new_int(a, 0), bytes_of(msb_bytes, 20));

    PkixRevokedCertificate old_entry = {big_new_int(a, 2),
                                        time_add((Time){0}, TIME_HOUR),
                                        slice_nil(TYPE_PKIX_EXTENSION)};
    X509RevocationList *bad_algo = crl_template(a, 5);
    bad_algo->signature_algorithm = X509_SHA256_WITH_RSA;
    bad_algo->revoked_certificates = one(a, TYPE_PKIX_REVOKED_CERTIFICATE, &old_entry);

    X509RevocationList *valid = crl_template(a, 5);
    valid->revoked_certificate_entries =
        one_entry(a, 0, slice_nil(TYPE_PKIX_EXTENSION));

    X509RevocationList *reason = crl_template(a, 5);
    reason->revoked_certificate_entries =
        one_entry(a, 1, slice_nil(TYPE_PKIX_EXTENSION));

    PkixExtension extra_ext = {ASN1_OID(2, 5, 29, 99), false, BYTES(5, 0)};
    X509RevocationList *entry_ext = crl_template(a, 5);
    entry_ext->revoked_certificate_entries =
        one_entry(a, 0, one(a, TYPE_PKIX_EXTENSION, &extra_ext));

    X509RevocationList *sha512 = crl_template(a, 5);
    sha512->signature_algorithm = X509_ECDSA_WITH_SHA512;
    sha512->revoked_certificate_entries = valid->revoked_certificate_entries;

    X509RevocationList *extra = crl_template(a, 5);
    extra->revoked_certificate_entries = valid->revoked_certificate_entries;
    extra->extra_extensions = one(a, TYPE_PKIX_EXTENSION, &extra_ext);

    PkixRevokedCertificate old_with_ext = old_entry;
    old_with_ext.extensions = one(a, TYPE_PKIX_EXTENSION, &extra_ext);
    X509RevocationList *deprecated = crl_template(a, 5);
    deprecated->revoked_certificates =
        one(a, TYPE_PKIX_REVOKED_CERTIFICATE, &old_with_ext);

    X509RevocationList *empty_list = crl_template(a, 5);

    const CreateCRLCase tests[] = {
        {"nil template", ec256, NULL, NULL, "x509: template can not be nil"},
        {"nil issuer", ec256, NULL, empty, "x509: issuer can not be nil"},
        {"issuer doesn't have crlSign key usage bit set", ec256,
         crl_issuer(a, X509_KEY_USAGE_CERT_SIGN, false), empty,
         "x509: issuer must have the crlSign key usage bit set"},
        {"issuer missing SubjectKeyId", ec256,
         crl_issuer(a, X509_KEY_USAGE_CRL_SIGN, false), empty,
         "x509: issuer certificate doesn't contain a subject key identifier"},
        {"nextUpdate before thisUpdate", ec256, good, before,
         "x509: template.ThisUpdate is after template.NextUpdate"},
        {"nil Number", ec256, good, nil_number,
         "x509: template contains nil Number field"},
        {"long Number", ec256, good, long_number, "x509: CRL number exceeds 20 octets"},
        {"long Number (20 bytes, MSB set)", ec256, good, msb_number,
         "x509: CRL number exceeds 20 octets"},
        {"invalid signature algorithm", ec256, good, bad_algo,
         "x509: requested SignatureAlgorithm does not match private key type"},
        {"valid", ec256, good, valid, NULL},
        {"valid, reason code", ec256, good, reason, NULL},
        {"valid, extra entry extension", ec256, good, entry_ext, NULL},
        {"valid, Ed25519 key", ed25519_priv, good, valid, NULL},
        {"valid, non-default signature algorithm", ec256, good, sha512, NULL},
        {"valid, extra extension", ec256, good, extra, NULL},
        {"valid, deprecated entries with extension", ec256, good, deprecated, NULL},
        {"valid, empty list", ec256, good, empty_list, NULL},
        {"valid CA with utf8 Subject fields including Email, empty list", utf8_key,
         utf8_ca, empty_list, NULL},
        {"valid, ML-DSA-44 key", mldsa_private_key_signer(mldsa_priv), good, valid,
         NULL},
    };
    for (Int i = 0; i < (Int)(sizeof tests / sizeof tests[0]); i++) {
        const CreateCRLCase *tc = &tests[i];
        err = BURROW_NO_ERROR;
        Slice crl = x509_create_revocation_list(a, (IoReader){0}, tc->template_,
                                                tc->issuer, tc->key, &err);
        if (BURROW_FAILED(err) && tc->expected_error == NULL) {
            testing_t_errorf_v(t, "%s: CreateRevocationList failed unexpectedly: %v",
                               tc->name, err);
            continue;
        }
        if (BURROW_FAILED(err) && !text_is(err, tc->expected_error)) {
            testing_t_errorf_v(
                t,
                "%s: CreateRevocationList failed unexpectedly, wanted: %s, "
                "got: %v",
                tc->name, tc->expected_error, err);
            continue;
        }
        if (!BURROW_FAILED(err) && tc->expected_error != NULL) {
            testing_t_errorf_v(t, "%s: CreateRevocationList didn't fail, expected: %s",
                               tc->name, tc->expected_error);
            continue;
        }
        if (tc->expected_error == NULL)
            check_created_crl(t, a, tc, crl);
    }
    arena_free(&ar);
}

/* pssParameters, as x509.c has it. */
#define TEST_PSS_PARAMETERS_FIELDS(F, T)                                               \
    F(T, PkixAlgorithmIdentifier, hash, Hash, "asn1:\"explicit,tag:0\"")               \
    F(T, PkixAlgorithmIdentifier, mgf, MGF, "asn1:\"explicit,tag:1\"")                 \
    F(T, Int, salt_length, SaltLength, "asn1:\"explicit,tag:2\"")                      \
    F(T, Int, trailer_field, TrailerField, "asn1:\"optional,explicit,tag:3,default:1\"")
BURROW_STRUCT_AS(TestPssParameters, TEST_PSS_PARAMETERS_FIELDS);

static void TestRSAPSAParameters(TestingT *t) {
    static const Int oid_sha256[] = {2, 16, 840, 1, 101, 3, 4, 2, 1};
    static const Int oid_sha384[] = {2, 16, 840, 1, 101, 3, 4, 2, 2};
    static const Int oid_sha512[] = {2, 16, 840, 1, 101, 3, 4, 2, 3};
    static const Int oid_mgf1[] = {1, 2, 840, 113549, 1, 1, 8};
    ARENA(a);
    CryptoHash hash;
    bool is_rsa_pss;
    Slice params;
    for (Int i = 0; burrow__x509_signature_details(i, &hash, &is_rsa_pss, &params);
         i++) {
        if (!is_rsa_pss)
            continue;
        Asn1ObjectIdentifier hash_oid = slice_nil(TYPE_INT);
        if (hash == CRYPTO_SHA256)
            hash_oid = OID(oid_sha256);
        else if (hash == CRYPTO_SHA384)
            hash_oid = OID(oid_sha384);
        else if (hash == CRYPTO_SHA512)
            hash_oid = OID(oid_sha512);

        Error err = BURROW_NO_ERROR;
        PkixAlgorithmIdentifier mgf1_params = {hash_oid, asn1_null_raw_value};
        TestPssParameters p = {0};
        p.hash = (PkixAlgorithmIdentifier){hash_oid, asn1_null_raw_value};
        p.mgf.algorithm = OID(oid_mgf1);
        p.mgf.parameters.full_bytes = asn1_marshal(
            a, BURROW_ANY(TYPE_PKIX_ALGORITHM_IDENTIFIER, &mgf1_params), &err);
        if (BURROW_FAILED(err))
            testing_t_fatalf_v(t, "failed to marshal MGF parameters: %v", err);
        p.salt_length = crypto_hash_size(hash);
        p.trailer_field = 1;
        Slice generated =
            asn1_marshal(a, BURROW_ANY(TYPE_OF(TestPssParameters), &p), &err);
        if (BURROW_FAILED(err))
            testing_t_fatalf_v(t, "failed to marshal parameters: %v", err);
        if (!bytes_equal(params, generated))
            testing_t_errorf_v(
                t,
                "hardcoded parameters for hash %d didn't match generated "
                "parameters: got (generated) %x, wanted (hardcoded) %x",
                (Int)hash, generated, params);
    }
    arena_free(&ar);
}

typedef struct MismatchCase {
    const char *name;
    CryptoSigner key;
    X509SignatureAlgorithm sig_algo;
    const char *want_err;
    X509SignatureAlgorithm want_algo;
} MismatchCase;

static void mismatch_check(TestingT *t, const char *name, const char *op, Error err,
                           const char *want_err) {
    if (want_err == NULL) {
        if (BURROW_FAILED(err))
            testing_t_errorf_v(t, "%s: %s: unexpected error: %v", name, op, err);
        return;
    }
    if (!BURROW_FAILED(err))
        testing_t_errorf_v(t, "%s: %s: expected error containing %q, got nil", name, op,
                           want_err);
    else if (!contains(err, want_err))
        testing_t_errorf_v(t, "%s: %s: error = %q, want substring %q", name, op,
                           error_text(err), want_err);
}

static void TestMismatchedTemplateSignatureAlgorithm(TestingT *t) {
    ARENA(a);
    CryptoSigner rsa_priv = rsa_private_key_signer(test_key(a));
    CryptoSigner ecdsa_priv = ecdsa_private_key_signer(ec_key(a, elliptic_p256()));
    Ed25519Signer es;
    CryptoSigner ed25519_priv = ed25519_key(a, &es);
    CryptoSigner mldsa44_priv = mldsa_private_key_signer(mldsa_key(a, mldsa_mldsa44()));
    CryptoSigner mldsa87_priv = mldsa_private_key_signer(mldsa_key(a, mldsa_mldsa87()));

    const char *mismatch_err =
        "x509: requested SignatureAlgorithm does not match private key type";
    const char *mldsa_params_err =
        "x509: requested SignatureAlgorithm does not match ML-DSA parameters";
    const MismatchCase tests[] = {
        /* Cross-key types: the requested SignatureAlgorithm's public key type
         * doesn't match the signer's key. */
        {"RSA-key/ECDSA-algo", rsa_priv, X509_ECDSA_WITH_SHA256, mismatch_err, 0},
        {"RSA-key/Ed25519-algo", rsa_priv, X509_PURE_ED25519, mismatch_err, 0},
        {"ECDSA-key/RSA-algo", ecdsa_priv, X509_SHA256_WITH_RSA, mismatch_err, 0},
        {"ECDSA-key/RSAPSS-algo", ecdsa_priv, X509_SHA256_WITH_RSAPSS, mismatch_err, 0},
        {"ECDSA-key/Ed25519-algo", ecdsa_priv, X509_PURE_ED25519, mismatch_err, 0},
        {"Ed25519-key/ECDSA-algo", ed25519_priv, X509_ECDSA_WITH_SHA256, mismatch_err,
         0},
        {"Ed25519-key/RSA-algo", ed25519_priv, X509_SHA256_WITH_RSA, mismatch_err, 0},
        /* PKCS#1 v1.5 vs PSS: both are valid for an RSA signer, so either
         * choice succeeds and the requested algorithm is honored. */
        {"RSA-key/PKCS1v15-algo", rsa_priv, X509_SHA256_WITH_RSA, NULL,
         X509_SHA256_WITH_RSA},
        {"RSA-key/PSS-algo", rsa_priv, X509_SHA256_WITH_RSAPSS, NULL,
         X509_SHA256_WITH_RSAPSS},
        {"MLDSA-key/ECDSA-algo", mldsa44_priv, X509_ECDSA_WITH_SHA256, mismatch_err, 0},
        {"ECDSA-key/MLDSA-algo", ecdsa_priv, X509_MLDSA44, mismatch_err, 0},
        /* ML-DSA-44 vs ML-DSA-87: same PublicKeyAlgorithm, but
         * signingParamsForKey rejects the parameter mismatch. */
        {"MLDSA44-key/MLDSA87-algo", mldsa44_priv, X509_MLDSA87, mldsa_params_err, 0},
        {"MLDSA87-key/MLDSA44-algo", mldsa87_priv, X509_MLDSA44, mldsa_params_err, 0},
        {"MLDSA44-key/MLDSA44-algo", mldsa44_priv, X509_MLDSA44, NULL, X509_MLDSA44},
        {"MLDSA87-key/MLDSA87-algo", mldsa87_priv, X509_MLDSA87, NULL, X509_MLDSA87},
    };
    for (Int i = 0; i < (Int)(sizeof tests / sizeof tests[0]); i++) {
        const MismatchCase *tc = &tests[i];
        Error err = BURROW_NO_ERROR;

        X509Certificate cert_tmpl = {0};
        cert_tmpl.serial_number = big_new_int(a, 1);
        cert_tmpl.subject.common_name = BURROW_S("test");
        cert_tmpl.signature_algorithm = tc->sig_algo;
        Slice cert_der =
            x509_create_certificate(a, (IoReader){0}, &cert_tmpl, &cert_tmpl,
                                    crypto_signer_public(tc->key), tc->key, &err);
        mismatch_check(t, tc->name, "CreateCertificate", err, tc->want_err);
        if (tc->want_err == NULL && !BURROW_FAILED(err)) {
            X509Certificate *cert = x509_parse_certificate(a, cert_der, &err);
            if (BURROW_FAILED(err))
                testing_t_fatalf_v(t, "%s: ParseCertificate: %v", tc->name, err);
            if (cert->signature_algorithm != tc->want_algo)
                testing_t_errorf_v(t,
                                   "%s: Certificate.SignatureAlgorithm = %d, want %d",
                                   tc->name, cert->signature_algorithm, tc->want_algo);
        }

        err = BURROW_NO_ERROR;
        X509CertificateRequest csr_tmpl = {0};
        csr_tmpl.subject.common_name = BURROW_S("test");
        csr_tmpl.signature_algorithm = tc->sig_algo;
        Slice csr_der =
            x509_create_certificate_request(a, (IoReader){0}, &csr_tmpl, tc->key, &err);
        mismatch_check(t, tc->name, "CreateCertificateRequest", err, tc->want_err);
        if (tc->want_err == NULL && !BURROW_FAILED(err)) {
            X509CertificateRequest *csr =
                x509_parse_certificate_request(a, csr_der, &err);
            if (BURROW_FAILED(err))
                testing_t_fatalf_v(t, "%s: ParseCertificateRequest: %v", tc->name, err);
            if (csr->signature_algorithm != tc->want_algo)
                testing_t_errorf_v(
                    t, "%s: CertificateRequest.SignatureAlgorithm = %d, want %d",
                    tc->name, csr->signature_algorithm, tc->want_algo);
        }

        err = BURROW_NO_ERROR;
        X509Certificate *crl_issuer_cert = crl_issuer(a, X509_KEY_USAGE_CRL_SIGN, true);
        crl_issuer_cert->subject.common_name = BURROW_S("test");
        X509RevocationList *crl_tmpl = crl_template(a, 1);
        crl_tmpl->signature_algorithm = tc->sig_algo;
        Slice crl_der = x509_create_revocation_list(a, (IoReader){0}, crl_tmpl,
                                                    crl_issuer_cert, tc->key, &err);
        mismatch_check(t, tc->name, "CreateRevocationList", err, tc->want_err);
        if (tc->want_err == NULL && !BURROW_FAILED(err)) {
            X509RevocationList *crl = x509_parse_revocation_list(a, crl_der, &err);
            if (BURROW_FAILED(err))
                testing_t_fatalf_v(t, "%s: ParseRevocationList: %v", tc->name, err);
            if (crl->signature_algorithm != tc->want_algo)
                testing_t_errorf_v(
                    t, "%s: RevocationList.SignatureAlgorithm = %d, want %d", tc->name,
                    crl->signature_algorithm, tc->want_algo);
        }
    }
    arena_free(&ar);
}

/* checkAI of TestRawSignatureAlgorithm. */
static void check_ai(TestingT *t, Alloc *a, const char *what, Slice raw,
                     Asn1ObjectIdentifier want_oid) {
    if (raw.len == 0)
        testing_t_fatalf_v(t, "%s: RawSignatureAlgorithm is empty", what);
    PkixAlgorithmIdentifier ai;
    memset(&ai, 0, sizeof ai);
    Error err = BURROW_NO_ERROR;
    Slice rest =
        asn1_unmarshal(a, raw, BURROW_ANY(TYPE_PKIX_ALGORITHM_IDENTIFIER, &ai), &err);
    if (BURROW_FAILED(err))
        testing_t_fatalf_v(t, "%s: failed to unmarshal RawSignatureAlgorithm: %v", what,
                           err);
    if (rest.len != 0)
        testing_t_fatalf_v(t, "%s: trailing data after RawSignatureAlgorithm: %x", what,
                           rest);
    if (!asn1_object_identifier_equal(ai.algorithm, want_oid))
        testing_t_fatalf_v(t, "%s: unexpected OID: got %s", what,
                           asn1_object_identifier_string(ai.algorithm, a));
}

static void TestRawSignatureAlgorithmCreated(TestingT *t) {
    ARENA(a);
    EcdsaPrivateKey *priv = ec_key(a, elliptic_p256());
    CryptoSigner signer = ecdsa_private_key_signer(priv);
    Error err = BURROW_NO_ERROR;

    X509CertificateRequest csr_tmpl = {0};
    Slice csr_der =
        x509_create_certificate_request(a, (IoReader){0}, &csr_tmpl, signer, &err);
    if (BURROW_FAILED(err))
        testing_t_fatalf_v(t, "failed to create CSR: %v", err);
    X509CertificateRequest *csr = x509_parse_certificate_request(a, csr_der, &err);
    if (BURROW_FAILED(err))
        testing_t_fatalf_v(t, "failed to parse CSR: %v", err);
    check_ai(t, a, "CertificateRequest", csr->raw_signature_algorithm,
             OID(oid_signature_ecdsa_with_sha256));

    X509Certificate issuer_tmpl = {0};
    issuer_tmpl.serial_number = big_new_int(a, 1);
    issuer_tmpl.subject.common_name = BURROW_S("issuer");
    issuer_tmpl.not_before = time_now();
    issuer_tmpl.not_after = time_add(time_now(), TIME_HOUR);
    issuer_tmpl.key_usage = X509_KEY_USAGE_CRL_SIGN;
    issuer_tmpl.is_ca = true;
    issuer_tmpl.basic_constraints_valid = true;
    Slice issuer_der =
        x509_create_certificate(a, (IoReader){0}, &issuer_tmpl, &issuer_tmpl,
                                ecdsa_private_key_public(priv), signer, &err);
    if (BURROW_FAILED(err))
        testing_t_fatalf_v(t, "failed to create issuer: %v", err);
    X509Certificate *issuer = x509_parse_certificate(a, issuer_der, &err);
    if (BURROW_FAILED(err))
        testing_t_fatalf_v(t, "failed to parse issuer: %v", err);
    X509RevocationList rl = {0};
    rl.number = big_new_int(a, 1);
    rl.this_update = time_now();
    rl.next_update = time_add(time_now(), TIME_HOUR);
    Slice crl_der =
        x509_create_revocation_list(a, (IoReader){0}, &rl, issuer, signer, &err);
    if (BURROW_FAILED(err))
        testing_t_fatalf_v(t, "failed to create CRL: %v", err);
    X509RevocationList *crl = x509_parse_revocation_list(a, crl_der, &err);
    if (BURROW_FAILED(err))
        testing_t_fatalf_v(t, "failed to parse CRL: %v", err);
    check_ai(t, a, "RevocationList", crl->raw_signature_algorithm,
             OID(oid_signature_ecdsa_with_sha256));
    arena_free(&ar);
}

static void TestDisableSHA1ForCertOnly(TestingT *t) {
    ARENA(a);
    burrow__x509_godebug_set("");
    RsaPrivateKey *k = test_key(a);
    X509Certificate tmpl = {0};
    tmpl.serial_number = big_new_int(a, 1);
    tmpl.not_before = time_add(time_now(), -TIME_HOUR);
    tmpl.not_after = time_add(time_now(), TIME_HOUR);
    tmpl.signature_algorithm = X509_SHA1_WITH_RSA;
    tmpl.basic_constraints_valid = true;
    tmpl.is_ca = true;
    tmpl.key_usage = X509_KEY_USAGE_CERT_SIGN | X509_KEY_USAGE_CRL_SIGN;
    Slice cert_der;
    Error err = create_with_test_key(a, &tmpl, &cert_der);
    if (BURROW_FAILED(err))
        testing_t_fatalf_v(t, "failed to generate test cert: %v", err);
    X509Certificate *cert = x509_parse_certificate(a, cert_der, &err);
    if (BURROW_FAILED(err))
        testing_t_fatalf_v(t, "failed to parse test cert: %v", err);

    err = x509_certificate_check_signature_from(cert, cert);
    if (!BURROW_FAILED(err))
        testing_t_errorf_v(t, "expected CheckSignatureFrom to fail");
    else if (errors_as(err, TYPE_X509_INSECURE_ALGORITHM_ERROR) == NULL)
        testing_t_errorf_v(t, "expected InsecureAlgorithmError error, got %v", err);

    X509RevocationList rl = {0};
    rl.signature_algorithm = X509_SHA1_WITH_RSA;
    rl.number = big_new_int(a, 1);
    rl.this_update = time_add(time_now(), -TIME_HOUR);
    rl.next_update = time_add(time_now(), TIME_HOUR);
    err = BURROW_NO_ERROR;
    Slice crl_der = x509_create_revocation_list(a, (IoReader){0}, &rl, cert,
                                                rsa_private_key_signer(k), &err);
    if (BURROW_FAILED(err))
        testing_t_fatalf_v(t, "failed to generate test CRL: %v", err);
    X509RevocationList *crl = x509_parse_revocation_list(a, crl_der, &err);
    if (BURROW_FAILED(err))
        testing_t_fatalf_v(t, "failed to parse test CRL: %v", err);
    err = x509_revocation_list_check_signature_from(crl, cert);
    if (BURROW_FAILED(err))
        testing_t_errorf_v(t, "unexpected error: %v", err);

    /* This is an unrelated OCSP response, which will fail signature
     * verification but shouldn't return an InsecureAlgorithmError, since SHA1
     * should be allowed for OCSP. */
    err = x509_certificate_check_signature(cert, X509_SHA1_WITH_RSA,
                                           hexb(a, ocsp_tbs_hex), slice_nil(TYPE_BYTE));
    if (!errors_is(err, rsa_err_verification))
        testing_t_errorf_v(t, "unexpected error: %v", err);
    burrow__x509_godebug_set(NULL);
    arena_free(&ar);
}

static void TestRevocationListCheckSignatureFrom(TestingT *t) {
    ARENA(a);
    EcdsaPrivateKey *good_key = ec_key(a, elliptic_p224());
    EcdsaPrivateKey *bad_key = ec_key(a, elliptic_p224());
    const struct {
        const char *name;
        X509KeyUsage key_usage;
        bool ca;
        X509PublicKeyAlgorithm algo;
        const EcdsaPrivateKey *key;
        const char *err;
    } tests[] = {
        {"valid", 0, true, X509_ECDSA, good_key, NULL},
        {"valid, key usage set", X509_KEY_USAGE_CRL_SIGN, true, X509_ECDSA, good_key,
         NULL},
        {"invalid issuer, wrong key usage", X509_KEY_USAGE_CERT_SIGN, true, X509_ECDSA,
         good_key,
         "x509: invalid signature: parent certificate cannot sign this kind of "
         "certificate"},
        {"invalid issuer, no basic constraints/ca", 0, false, X509_ECDSA, good_key,
         "x509: invalid signature: parent certificate cannot sign this kind of "
         "certificate"},
        {"invalid issuer, unsupported public key type", 0, true,
         X509_UNKNOWN_PUBLIC_KEY_ALGORITHM, good_key,
         "x509: cannot verify signature: algorithm unimplemented"},
        {"wrong key", 0, true, X509_ECDSA, bad_key, "x509: ECDSA verification failure"},
    };
    X509Certificate *crl_issuer_cert = crl_issuer(a, X509_KEY_USAGE_CRL_SIGN, false);
    crl_issuer_cert->basic_constraints_valid = true;
    crl_issuer_cert->is_ca = true;
    crl_issuer_cert->public_key_algorithm = X509_ECDSA;
    crl_issuer_cert->public_key = ecdsa_private_key_public(good_key);
    crl_issuer_cert->subject_key_id = BYTES(1, 2, 3);

    for (Int i = 0; i < (Int)(sizeof tests / sizeof tests[0]); i++) {
        X509Certificate issuer = {0};
        issuer.version = 3;
        issuer.basic_constraints_valid = tests[i].ca;
        issuer.is_ca = tests[i].ca;
        issuer.public_key_algorithm = tests[i].algo;
        issuer.public_key = ecdsa_private_key_public(tests[i].key);
        issuer.key_usage = tests[i].key_usage;

        X509RevocationList rl = {0};
        rl.number = big_new_int(a, 1);
        Error err = BURROW_NO_ERROR;
        Slice crl_der =
            x509_create_revocation_list(a, (IoReader){0}, &rl, crl_issuer_cert,
                                        ecdsa_private_key_signer(good_key), &err);
        if (BURROW_FAILED(err))
            testing_t_fatalf_v(t, "%s: failed to generate CRL: %v", tests[i].name, err);
        X509RevocationList *crl = x509_parse_revocation_list(a, crl_der, &err);
        if (BURROW_FAILED(err))
            testing_t_fatalf_v(t, "%s: failed to parse test CRL: %v", tests[i].name,
                               err);
        err = x509_revocation_list_check_signature_from(crl, &issuer);
        if (BURROW_FAILED(err) && (tests[i].err == NULL || !text_is(err, tests[i].err)))
            testing_t_errorf_v(t, "%s: unexpected error: got %v, want %s",
                               tests[i].name, err,
                               tests[i].err == NULL ? "nil" : tests[i].err);
        else if (!BURROW_FAILED(err) && tests[i].err != NULL)
            testing_t_errorf_v(t, "%s: CheckSignatureFrom did not fail: want %s",
                               tests[i].name, tests[i].err);
    }
    arena_free(&ar);
}

#define TESTS(X)                                                                       \
    X(TestCreateSelfSignedCertificate)                                                 \
    X(TestCRLCreation)                                                                 \
    X(TestCreateCertificateRequest)                                                    \
    X(TestCertificateRequestOverrides)                                                 \
    X(TestMaxPathLenNotCA)                                                             \
    X(TestMaxPathLen)                                                                  \
    X(TestNoAuthorityKeyIdInSelfSignedCert)                                            \
    X(TestNoSubjectKeyIdInCert)                                                        \
    X(TestASN1BitLength)                                                               \
    X(TestEmptySerialNumber)                                                           \
    X(TestEmptySubject)                                                                \
    X(TestCreateRevocationList)                                                        \
    X(TestRSAPSAParameters)                                                            \
    X(TestUnknownExtKey)                                                               \
    X(TestIA5SANEnforcement)                                                           \
    X(TestCreateCertificateBrokenSigner)                                               \
    X(TestMismatchedTemplateSignatureAlgorithm)                                        \
    X(TestCreateCertificateLegacy)                                                     \
    X(TestCertificateRequestRoundtripFields)                                           \
    X(TestRawSignatureAlgorithmCreated)                                                \
    X(TestDisableSHA1ForCertOnly)                                                      \
    X(TestRevocationListCheckSignatureFrom)                                            \
    X(TestOmitEmptyExtensions)                                                         \
    X(TestCreateNegativeSerial)                                                        \
    X(TestCertificateOIDPoliciesGODEBUG)                                               \
    X(TestCertificatePolicies)                                                         \
    X(TestRejectCriticalAKI)                                                           \
    X(TestRejectCriticalAIA)                                                           \
    X(TestRejectCriticalSKI)                                                           \
    X(TestMessageSigner)                                                               \
    X(TestCreateCertificateNegativeMaxPathLength)                                      \
    X(TestMLDSACertificates)                                                           \
    X(TestIPv4MappedIPsSANCreate)                                                      \
    X(TestIPv4MappedIPsConstraintCreate)

TESTING_MAIN(TESTS)
