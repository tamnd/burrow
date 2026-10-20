/* Derived from Go's src/crypto/tls/tls_test.go.
 * Go source: go1.27.1.
 *
 * Only TestX509KeyPair, TestCipherSuites, TestVersionName and
 * TestX509KeyPairPopulateCertificate for now. The rest of the file needs
 * connections and handshakes, which come in later. The PEMs TestX509KeyPair
 * declares are in tls_test_gen.h, and the ML-DSA ones are always tried, as
 * there is no FIPS 140-3 module here to be v1.0.0. Go ranges over the
 * disabledCipherSuites map; here that map is a predicate, so the test asks it
 * about every suite there is.
 *
 * Copyright 2012 The Go Authors. All rights reserved.
 * Copyright 2026 The burrow Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style licence that can be found
 * in the LICENSE file. */

#include "check.h"

#include "../src/crypto/tls_internal.h"
#include "burrow/burrow.h"
#include "burrow/bytes.h"
#include "burrow/crypto/ecdsa.h"
#include "burrow/crypto/elliptic.h"
#include "burrow/crypto/tls.h"
#include "burrow/crypto/x509.h"
#include "burrow/encoding/pem.h"
#include "burrow/math/big.h"
#include "burrow/panic.h"
#include "burrow/strings.h"
#include "tls_test_gen.h"

#include <stdint.h>

/* testingKey, from a. */
static Slice testing_key(Alloc *a, Slice s) {
    static Byte testing[] = "TESTING KEY";
    static Byte private_[] = "PRIVATE KEY";
    return bytes_replace_all(a, s, slice_from(testing, 11, 11, TYPE_BYTE),
                             slice_from(private_, 11, 11, TYPE_BYTE));
}

/* Not in Go: the PEM of one test joined to another, the + of two Go strings. */
static Slice pem_cat(Alloc *a, Slice x, Slice y) {
    Slice b = slice_make(a, TYPE_BYTE, 0, x.len + y.len);
    b = slice_append(a, b, x.p, x.len);
    return slice_append(a, b, y.p, y.len);
}

typedef struct KeyPairTest {
    const char *algo;
    Slice cert;
    Slice key;
} KeyPairTest;

static void TestX509KeyPair(TestingT *t) {
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    Slice rsa_cert_pem = tls_test_pem(a, x509_key_pair_rsa_cert_pem);
    Slice rsa_key_pem = testing_key(a, tls_test_pem(a, x509_key_pair_rsa_key_pem));
    /* keyPEM is the same as rsaKeyPEM, but declares itself as just
     * "PRIVATE KEY", not "RSA PRIVATE KEY".  https://golang.org/issue/4477 */
    Slice key_pem = testing_key(a, tls_test_pem(a, x509_key_pair_key_pem));
    Slice ecdsa_cert_pem = tls_test_pem(a, x509_key_pair_ecdsa_cert_pem);
    Slice ecdsa_key_pem = testing_key(a, tls_test_pem(a, x509_key_pair_ecdsa_key_pem));

    KeyPairTest key_pair_tests[] = {
        {"ECDSA", ecdsa_cert_pem, ecdsa_key_pem},
        {"RSA", rsa_cert_pem, rsa_key_pem},
        {"RSA-untyped", rsa_cert_pem, key_pem}, /* golang.org/issue/4477 */
        {"ML-DSA-44", tls_test_pem(a, test_mldsa44_cert_pem),
         testing_key(a, tls_test_pem(a, test_mldsa44_key_pem))},
        {"ML-DSA-65", tls_test_pem(a, test_mldsa65_cert_pem),
         testing_key(a, tls_test_pem(a, test_mldsa65_key_pem))},
        {"ML-DSA-87", tls_test_pem(a, test_mldsa87_cert_pem),
         testing_key(a, tls_test_pem(a, test_mldsa87_key_pem))},
    };

    Error err = BURROW_NO_ERROR;
    for (size_t i = 0; i < sizeof key_pair_tests / sizeof key_pair_tests[0]; i++) {
        const KeyPairTest *test = &key_pair_tests[i];
        Slice pem = pem_cat(a, test->cert, test->key);
        (void)tls_x509_key_pair(a, pem, pem, &err);
        if (BURROW_FAILED(err))
            testing_t_errorf_v(t, "Failed to load %s cert followed by %s key: %s",
                               test->algo, test->algo, err);
        pem = pem_cat(a, test->key, test->cert);
        (void)tls_x509_key_pair(a, pem, pem, &err);
        if (BURROW_FAILED(err))
            testing_t_errorf_v(t, "Failed to load %s key followed by %s cert: %s",
                               test->algo, test->algo, err);
    }

    (void)tls_x509_key_pair(a, rsa_key_pem, rsa_cert_pem, &err);
    if (!BURROW_FAILED(err))
        testing_t_fatalf_v(t, "X509KeyPair didn't return an error when arguments were "
                              "switched");
    Str sub_str = BURROW_S("been switched");
    if (!strings_contains(error_text(err), sub_str))
        testing_t_fatalf_v(t,
                           "Expected %q in the error when switching arguments to "
                           "X509KeyPair, but the error was %q",
                           sub_str, err);

    (void)tls_x509_key_pair(a, rsa_cert_pem, rsa_cert_pem, &err);
    if (!BURROW_FAILED(err))
        testing_t_fatalf_v(t, "X509KeyPair didn't return an error when both arguments "
                              "were certificates");
    sub_str = BURROW_S("certificate");
    if (!strings_contains(error_text(err), sub_str))
        testing_t_fatalf_v(
            t,
            "Expected %q in the error when both arguments to X509KeyPair "
            "were certificates, but the error was %q",
            sub_str, err);

    Slice nonsense_pem = tls_test_pem(a, x509_key_pair_nonsense_pem);

    (void)tls_x509_key_pair(a, nonsense_pem, nonsense_pem, &err);
    if (!BURROW_FAILED(err))
        testing_t_fatalf_v(t, "X509KeyPair didn't return an error when both arguments "
                              "were nonsense");
    sub_str = BURROW_S("NONSENSE");
    if (!strings_contains(error_text(err), sub_str))
        testing_t_fatalf_v(
            t,
            "Expected %q in the error when both arguments to X509KeyPair "
            "were nonsense, but the error was %q",
            sub_str, err);

    (void)tls_x509_key_pair(a, rsa_cert_pem, ecdsa_key_pem, &err);
    if (!BURROW_FAILED(err))
        testing_t_error_v(t,
                          "Load of RSA certificate succeeded with ECDSA private key");
    (void)tls_x509_key_pair(a, ecdsa_cert_pem, rsa_key_pem, &err);
    if (!BURROW_FAILED(err))
        testing_t_error_v(t,
                          "Load of ECDSA certificate succeeded with RSA private key");
    arena_free(&ar);
}

static const TlsCipherSuite *cipher_suite_in(Slice list, uint16_t id) {
    TlsCipherSuitePtr *p = (TlsCipherSuitePtr *)list.p;
    for (Int i = 0; i < list.len; i++) {
        if (p[i]->id == id)
            return p[i];
    }
    return NULL;
}

/* CipherSuiteByID, the closure in TestCipherSuites. */
static const TlsCipherSuite *cipher_suite_by_id(Alloc *a, uint16_t id) {
    const TlsCipherSuite *c = cipher_suite_in(tls_cipher_suites(a), id);
    if (c == NULL)
        c = cipher_suite_in(tls_insecure_cipher_suites(a), id);
    return c;
}

static bool contains_id(Slice ids, uint16_t id) {
    const uint16_t *p = (const uint16_t *)ids.p;
    for (Int i = 0; i < ids.len; i++) {
        if (p[i] == id)
            return true;
    }
    return false;
}

/* http2isBadCipher is copied from net/http.
 * TODO: if it ends up exposed somewhere, use that instead. */
static bool http2_is_bad_cipher(uint16_t cipher) {
    switch (cipher) {
    case TLS_TLS_RSA_WITH_RC4_128_SHA:
    case TLS_TLS_RSA_WITH_3DES_EDE_CBC_SHA:
    case TLS_TLS_RSA_WITH_AES_128_CBC_SHA:
    case TLS_TLS_RSA_WITH_AES_256_CBC_SHA:
    case TLS_TLS_RSA_WITH_AES_128_CBC_SHA256:
    case TLS_TLS_RSA_WITH_AES_128_GCM_SHA256:
    case TLS_TLS_RSA_WITH_AES_256_GCM_SHA384:
    case TLS_TLS_ECDHE_ECDSA_WITH_RC4_128_SHA:
    case TLS_TLS_ECDHE_ECDSA_WITH_AES_128_CBC_SHA:
    case TLS_TLS_ECDHE_ECDSA_WITH_AES_256_CBC_SHA:
    case TLS_TLS_ECDHE_RSA_WITH_RC4_128_SHA:
    case TLS_TLS_ECDHE_RSA_WITH_3DES_EDE_CBC_SHA:
    case TLS_TLS_ECDHE_RSA_WITH_AES_128_CBC_SHA:
    case TLS_TLS_ECDHE_RSA_WITH_AES_256_CBC_SHA:
    case TLS_TLS_ECDHE_ECDSA_WITH_AES_128_CBC_SHA256:
    case TLS_TLS_ECDHE_RSA_WITH_AES_128_CBC_SHA256:
        return true;
    default:
        return false;
    }
}

static bool name_has(Str name, const char *part) {
    return strings_contains(name, str_from_cstr(part));
}

/* isBetter, the comparison in TestCipherSuites, for preference list i. */
static int is_better(TestingT *t, Alloc *al, int i, uint16_t a, uint16_t b) {
    const TlsSuite *a_suite = burrow__tls_cipher_suite_by_id(a);
    const TlsSuite *b_suite = burrow__tls_cipher_suite_by_id(b);
    Str a_name = tls_cipher_suite_name(al, a);
    Str b_name = tls_cipher_suite_name(al, b);
    /* * < RC4 */
    if (!name_has(a_name, "RC4") && name_has(b_name, "RC4"))
        return -1;
    else if (name_has(a_name, "RC4") && !name_has(b_name, "RC4"))
        return +1;
    /* * < CBC_SHA256 */
    if (!name_has(a_name, "CBC_SHA256") && name_has(b_name, "CBC_SHA256"))
        return -1;
    else if (name_has(a_name, "CBC_SHA256") && !name_has(b_name, "CBC_SHA256"))
        return +1;
    /* * < 3DES */
    if (!name_has(a_name, "3DES") && name_has(b_name, "3DES"))
        return -1;
    else if (name_has(a_name, "3DES") && !name_has(b_name, "3DES"))
        return +1;
    /* ECDHE < * */
    if ((a_suite->flags & TLS_SUITE_ECDHE) != 0 &&
        (b_suite->flags & TLS_SUITE_ECDHE) == 0)
        return -1;
    else if ((a_suite->flags & TLS_SUITE_ECDHE) == 0 &&
             (b_suite->flags & TLS_SUITE_ECDHE) != 0)
        return +1;
    /* AEAD < CBC */
    if (a_suite->aead != NULL && b_suite->aead == NULL)
        return -1;
    else if (a_suite->aead == NULL && b_suite->aead != NULL)
        return +1;
    /* AES < ChaCha20 */
    if (name_has(a_name, "AES") && name_has(b_name, "CHACHA20")) {
        /* negative for cipherSuitesPreferenceOrder */
        if (i == 0)
            return -1;
        else
            return +1;
    } else if (name_has(a_name, "CHACHA20") && name_has(b_name, "AES")) {
        /* negative for cipherSuitesPreferenceOrderNoAES */
        if (i != 0)
            return -1;
        else
            return +1;
    }
    /* AES-128 < AES-256 */
    if (name_has(a_name, "AES_128") && name_has(b_name, "AES_256"))
        return -1;
    else if (name_has(a_name, "AES_256") && name_has(b_name, "AES_128"))
        return +1;
    /* ECDSA < RSA */
    if ((a_suite->flags & TLS_SUITE_EC_SIGN) != 0 &&
        (b_suite->flags & TLS_SUITE_EC_SIGN) == 0)
        return -1;
    else if ((a_suite->flags & TLS_SUITE_EC_SIGN) == 0 &&
             (b_suite->flags & TLS_SUITE_EC_SIGN) != 0)
        return +1;
    testing_t_fatalf_v(t, "two ciphersuites are equal by all criteria: %v and %v",
                       a_name, b_name);
    panic_str(BURROW_S("unreachable"));
}

static void TestCipherSuites(TestingT *t) {
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);

    uint16_t last_id = 0;
    Slice list = tls_cipher_suites(a);
    for (Int i = 0; i < list.len; i++) {
        const TlsCipherSuite *c = ((TlsCipherSuitePtr *)list.p)[i];
        if (last_id > c->id)
            testing_t_errorf_v(
                t, "CipherSuites are not ordered by ID: got %#04x after %#04x", c->id,
                last_id);
        else
            last_id = c->id;

        if (c->insecure)
            testing_t_errorf_v(
                t, "%#04x: Insecure CipherSuite returned by CipherSuites()", c->id);
    }
    last_id = 0;
    list = tls_insecure_cipher_suites(a);
    for (Int i = 0; i < list.len; i++) {
        const TlsCipherSuite *c = ((TlsCipherSuitePtr *)list.p)[i];
        if (last_id > c->id)
            testing_t_errorf_v(
                t, "InsecureCipherSuites are not ordered by ID: got %#04x after %#04x",
                c->id, last_id);
        else
            last_id = c->id;

        if (!c->insecure)
            testing_t_errorf_v(
                t, "%#04x: not Insecure CipherSuite returned by InsecureCipherSuites()",
                c->id);
    }

    Slice defaults = burrow__tls_default_cipher_suites(a, false);
    for (Int i = 0; i < burrow__tls_ncipher_suites; i++) {
        const TlsSuite *c = &burrow__tls_cipher_suites[i];
        const TlsCipherSuite *cc = cipher_suite_by_id(a, c->id);
        if (cc == NULL) {
            testing_t_errorf_v(t, "%#04x: no CipherSuite entry", c->id);
            continue;
        }

        bool tls12_only = (c->flags & TLS_SUITE_TLS12) != 0;
        if (tls12_only && cc->supported_versions.len != 1)
            testing_t_errorf_v(
                t, "%#04x: suite is TLS 1.2 only, but SupportedVersions is %v", c->id,
                cc->supported_versions);
        else if (!tls12_only && cc->supported_versions.len != 3)
            testing_t_errorf_v(t,
                               "%#04x: suite TLS 1.0-1.2, but SupportedVersions is %v",
                               c->id, cc->supported_versions);

        if (cc->insecure) {
            if (contains_id(defaults, c->id))
                testing_t_errorf_v(t, "%#04x: insecure suite in default list", c->id);
        } else {
            if (!contains_id(defaults, c->id))
                testing_t_errorf_v(t, "%#04x: secure suite not in default list", c->id);
        }

        Str got = tls_cipher_suite_name(a, c->id);
        if (!str_eq(got, cc->name))
            testing_t_errorf_v(t,
                               "%#04x: unexpected CipherSuiteName: got %q, expected %q",
                               c->id, got, cc->name);
    }
    for (Int i = 0; i < burrow__tls_ncipher_suites_tls13; i++) {
        const TlsSuiteTLS13 *c = &burrow__tls_cipher_suites_tls13[i];
        const TlsCipherSuite *cc = cipher_suite_by_id(a, c->id);
        if (cc == NULL) {
            testing_t_errorf_v(t, "%#04x: no CipherSuite entry", c->id);
            continue;
        }

        if (cc->insecure)
            testing_t_errorf_v(t, "%#04x: Insecure %v, expected false", c->id,
                               cc->insecure);
        if (cc->supported_versions.len != 1 ||
            ((const uint16_t *)cc->supported_versions.p)[0] != TLS_VERSION_TLS13)
            testing_t_errorf_v(
                t, "%#04x: suite is TLS 1.3 only, but SupportedVersions is %v", c->id,
                cc->supported_versions);

        Str got = tls_cipher_suite_name(a, c->id);
        if (!str_eq(got, cc->name))
            testing_t_errorf_v(t,
                               "%#04x: unexpected CipherSuiteName: got %q, expected %q",
                               c->id, got, cc->name);
    }

    Str got = tls_cipher_suite_name(a, 0xabc);
    if (!str_eq(got, BURROW_S("0x0ABC")))
        testing_t_errorf_v(
            t, "unexpected fallback CipherSuiteName: got %q, expected 0x0ABC", got);

    if (burrow__tls_npreference_order != burrow__tls_ncipher_suites)
        testing_t_errorf_v(
            t, "cipherSuitesPreferenceOrder is not the same size as cipherSuites");
    if (burrow__tls_npreference_order_no_aes != burrow__tls_npreference_order)
        testing_t_errorf_v(t,
                           "cipherSuitesPreferenceOrderNoAES is not the same size as "
                           "cipherSuitesPreferenceOrder");

    /* Check that disabled suites are marked insecure. */
    for (Int i = 0; i < burrow__tls_ncipher_suites; i++) {
        uint16_t id = burrow__tls_cipher_suites[i].id;
        if (!burrow__tls_disabled_cipher_suite(id))
            continue;
        const TlsCipherSuite *c = cipher_suite_by_id(a, id);
        if (c == NULL) {
            testing_t_errorf_v(t, "%#04x: no CipherSuite entry", id);
            continue;
        }
        if (!c->insecure)
            testing_t_errorf_v(t, "%#04x: disabled by default but not marked insecure",
                               id);
    }

    const uint16_t *orders[2] = {burrow__tls_cipher_suites_preference_order,
                                 burrow__tls_cipher_suites_preference_order_no_aes};
    for (int i = 0; i < 2; i++) {
        const uint16_t *pref_order = orders[i];
        Int n = burrow__tls_npreference_order;
        /* Check that insecure and HTTP/2 bad cipher suites are at the end of
         * the preference lists. */
        bool saw_insecure = false, saw_bad = false;
        for (Int j = 0; j < n; j++) {
            uint16_t id = pref_order[j];
            const TlsCipherSuite *c = cipher_suite_by_id(a, id);
            if (c == NULL) {
                testing_t_errorf_v(t, "%#04x: no CipherSuite entry", id);
                continue;
            }

            if (c->insecure)
                saw_insecure = true;
            else if (saw_insecure)
                testing_t_errorf_v(t, "%#04x: secure suite after insecure one(s)", id);

            if (http2_is_bad_cipher(id))
                saw_bad = true;
            else if (saw_bad)
                testing_t_errorf_v(t, "%#04x: non-bad suite after bad HTTP/2 one(s)",
                                   id);
        }

        /* Check that the list is sorted according to the documented criteria. */
        bool sorted = true;
        for (Int j = n - 1; j > 0; j--) {
            if (is_better(t, a, i, pref_order[j], pref_order[j - 1]) < 0) {
                sorted = false;
                break;
            }
        }
        if (!sorted)
            testing_t_error_v(t,
                              "preference order is not sorted according to the rules");
    }
    arena_free(&ar);
}

static void TestVersionName(TestingT *t) {
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    Str got = tls_version_name(a, TLS_VERSION_TLS13);
    Str exp = BURROW_S("TLS 1.3");
    if (!str_eq(got, exp))
        testing_t_errorf_v(t, "unexpected VersionName: got %q, expected %q", got, exp);
    got = tls_version_name(a, 0x12a);
    exp = BURROW_S("0x012A");
    if (!str_eq(got, exp))
        testing_t_errorf_v(t, "unexpected fallback VersionName: got %q, expected %q",
                           got, exp);
    arena_free(&ar);
}

static void TestX509KeyPairPopulateCertificate(TestingT *t) {
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    Error err = BURROW_NO_ERROR;
    EcdsaPrivateKey *key = ecdsa_generate_key(a, elliptic_p256(), (IoReader){0}, &err);
    if (BURROW_FAILED(err))
        testing_t_fatal_v(t, err);
    Slice key_der = x509_marshal_pkcs8_private_key(
        a, BURROW_ANY(TYPE_ECDSA_PRIVATE_KEY, key), &err);
    if (BURROW_FAILED(err))
        testing_t_fatal_v(t, err);
    PemBlock key_block = {BURROW_S("PRIVATE KEY"), NULL, key_der, 0};
    Slice key_pem = pem_encode_to_memory(a, &key_block);
    X509Certificate tmpl = {0};
    tmpl.serial_number = big_new_int(a, 1);
    tmpl.subject.common_name = BURROW_S("test");
    Slice cert_der = x509_create_certificate(a, (IoReader){0}, &tmpl, &tmpl,
                                             ecdsa_private_key_public(key),
                                             ecdsa_private_key_signer(key), &err);
    if (BURROW_FAILED(err))
        testing_t_fatal_v(t, err);
    PemBlock cert_block = {BURROW_S("CERTIFICATE"), NULL, cert_der, 0};
    Slice cert_pem = pem_encode_to_memory(a, &cert_block);

    TlsCertificate cert = tls_x509_key_pair(a, cert_pem, key_pem, &err);
    if (BURROW_FAILED(err))
        testing_t_fatal_v(t, err);
    if (cert.leaf == NULL)
        testing_t_fatal_v(t, "Leaf should be populated");
    arena_free(&ar);
}

#define TESTS(X)                                                                       \
    X(TestX509KeyPair)                                                                 \
    X(TestCipherSuites)                                                                \
    X(TestVersionName)                                                                 \
    X(TestX509KeyPairPopulateCertificate)

TESTING_MAIN(TESTS)
