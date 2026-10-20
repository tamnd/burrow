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
#include "burrow/map.h"
#include "burrow/math/big.h"
#include "burrow/mem/heap.h"
#include "burrow/os.h"
#include "burrow/panic.h"
#include "burrow/strings.h"
#include "tls_test_gen.h"

#include <stdint.h>
#include <string.h>

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

/* The bits of TestCloneFuncFields, one for each callback of a Config. */
static void set_bit(void *env, int bit) {
    *(int *)env |= 1 << bit;
}

static Time clone_time(void *env) {
    set_bit(env, 0);
    return (Time){0};
}

static TlsCertificate *clone_get_certificate(void *env, TlsClientHelloInfo *chi,
                                             Error *err) {
    (void)chi;
    (void)err;
    set_bit(env, 1);
    return NULL;
}

static TlsCertificate *
clone_get_client_certificate(void *env, TlsCertificateRequestInfo *cri, Error *err) {
    (void)cri;
    (void)err;
    set_bit(env, 2);
    return NULL;
}

static TlsConfig *clone_get_config_for_client(void *env, TlsClientHelloInfo *chi,
                                              Error *err) {
    (void)chi;
    (void)err;
    set_bit(env, 3);
    return NULL;
}

static Error clone_verify_peer_certificate(void *env, Slice raw_certs,
                                           Slice verified_chains) {
    (void)raw_certs;
    (void)verified_chains;
    set_bit(env, 4);
    return BURROW_NO_ERROR;
}

static Error clone_verify_connection(void *env, const TlsConnectionState *cs) {
    (void)cs;
    set_bit(env, 5);
    return BURROW_NO_ERROR;
}

static struct TlsSessionState *clone_unwrap_session(void *env, Slice identity,
                                                    const TlsConnectionState *cs,
                                                    Error *err) {
    (void)identity;
    (void)cs;
    (void)err;
    set_bit(env, 6);
    return NULL;
}

static Slice clone_wrap_session(void *env, Alloc *a, const TlsConnectionState *cs,
                                struct TlsSessionState *ss, Error *err) {
    (void)a;
    (void)cs;
    (void)ss;
    (void)err;
    set_bit(env, 7);
    return slice_nil(TYPE_BYTE);
}

static Error clone_ech_rejection_verify(void *env, const TlsConnectionState *cs) {
    (void)cs;
    set_bit(env, 8);
    return BURROW_NO_ERROR;
}

static Slice clone_get_ech_keys(void *env, TlsClientHelloInfo *chi, Error *err) {
    (void)chi;
    (void)err;
    set_bit(env, 9);
    return slice_nil(TYPE_TLS_ENCRYPTED_CLIENT_HELLO_KEY);
}

static void TestCloneFuncFields(TestingT *t) {
    enum { expected_count = 10 };
    int called = 0;
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);

    TlsConfig c1 = {0};
    c1.time = (TlsTimeFunc){clone_time, &called};
    c1.get_certificate = (TlsGetCertificateFunc){clone_get_certificate, &called};
    c1.get_client_certificate =
        (TlsGetClientCertificateFunc){clone_get_client_certificate, &called};
    c1.get_config_for_client =
        (TlsGetConfigForClientFunc){clone_get_config_for_client, &called};
    c1.verify_peer_certificate =
        (TlsVerifyPeerCertificateFunc){clone_verify_peer_certificate, &called};
    c1.verify_connection = (TlsVerifyConnectionFunc){clone_verify_connection, &called};
    c1.unwrap_session = (TlsUnwrapSessionFunc){clone_unwrap_session, &called};
    c1.wrap_session = (TlsWrapSessionFunc){clone_wrap_session, &called};
    c1.encrypted_client_hello_rejection_verify =
        (TlsVerifyConnectionFunc){clone_ech_rejection_verify, &called};
    c1.get_encrypted_client_hello_keys =
        (TlsGetEncryptedClientHelloKeysFunc){clone_get_ech_keys, &called};

    TlsConfig *c2 = tls_config_clone(&c1, a);
    if (c2 == NULL)
        testing_t_fatal_v(t, "Clone: out of memory");

    TlsConnectionState cs = {0};
    Error err = BURROW_NO_ERROR;
    c2->time.f(c2->time.env);
    c2->get_certificate.f(c2->get_certificate.env, NULL, &err);
    c2->get_client_certificate.f(c2->get_client_certificate.env, NULL, &err);
    c2->get_config_for_client.f(c2->get_config_for_client.env, NULL, &err);
    c2->verify_peer_certificate.f(c2->verify_peer_certificate.env,
                                  slice_nil(TYPE_BYTES), slice_nil(TYPE_BYTES));
    c2->verify_connection.f(c2->verify_connection.env, &cs);
    c2->unwrap_session.f(c2->unwrap_session.env, slice_nil(TYPE_BYTE), &cs, &err);
    c2->wrap_session.f(c2->wrap_session.env, a, &cs, NULL, &err);
    c2->encrypted_client_hello_rejection_verify.f(
        c2->encrypted_client_hello_rejection_verify.env, &cs);
    c2->get_encrypted_client_hello_keys.f(c2->get_encrypted_client_hello_keys.env, NULL,
                                          &err);

    if (called != (1 << expected_count) - 1)
        testing_t_fatalf_v(t, "expected %d calls but saw calls %b", expected_count,
                           called);
    tls_config_release(c2);
    arena_free(&ar);
}

/* Not in Go: a list of one key, the []ticketKey{c.ticketKeyFromBytes(key)} of
 * TestCloneNonFuncFields. The configs that hold it let go of it. */
static TlsTicketKeys *one_ticket_key(TlsConfig *c) {
    Alloc *heap = heap_allocator();
    TlsTicketKeys *k = mem_alloc(heap, sizeof(TlsTicketKeys) + sizeof(TlsTicketKey),
                                 _Alignof(TlsTicketKeys));
    if (k == NULL)
        panic_str(BURROW_S("one_ticket_key: out of memory"));
    k->refs = 1;
    k->len = 1;
    k->cap = 1;
    k->a = heap;
    k->keys[0] = burrow__tls_config_ticket_key_from_bytes(c, c->session_ticket_key);
    return k;
}

/* There is no reflect to walk the fields of a Config here, so every field the
 * Go test sets is set by name, and compared by name after the Clone. A field
 * added to Config needs adding to both lists. The ticket key lists are
 * compared by pointer, as a clone shares them. The mutex is not compared. */
static void TestCloneNonFuncFields(TestingT *t) {
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    TlsConfig c1 = {0};

    c1.rand = os_file_as_io_reader(os_stdin);
    static Byte b[] = {'b'};
    TlsCertificate *cert = BURROW_NEW(a, TlsCertificate);
    cert->certificate = slice_make(a, TYPE_BYTES, 1, 1);
    BURROW_AT(Slice, cert->certificate, 0) = slice_from(b, 1, 1, TYPE_BYTE);
    c1.certificates = slice_from(cert, 1, 1, TYPE_TLS_CERTIFICATE);
    c1.name_to_certificate = map_make(a, TYPE_STRING, TYPE_TLS_CERTIFICATE_PTR, 0);
    Str name = BURROW_S("a");
    TlsCertificate *nil_cert = NULL;
    map_set(c1.name_to_certificate, &name, &nil_cert);
    c1.root_cas = x509_new_cert_pool(a);
    c1.client_cas = x509_new_cert_pool(a);
    c1.client_session_cache = tls_new_lru_client_session_cache(a, 10);
    c1.key_log_writer = os_file_as_io_writer(os_stdout);
    static Str protos[] = {BURROW_S_INIT("a"), BURROW_S_INIT("b")};
    c1.next_protos = slice_from(protos, 2, 2, TYPE_STRING);
    c1.server_name = BURROW_S("b");
    c1.client_auth = TLS_VERIFY_CLIENT_CERT_IF_GIVEN;
    c1.insecure_skip_verify = true;
    c1.session_tickets_disabled = true;
    c1.dynamic_record_sizing_disabled = true;
    c1.prefer_server_cipher_suites = true;
    c1.min_version = TLS_VERSION_TLS12;
    c1.max_version = TLS_VERSION_TLS12;
    memset(c1.session_ticket_key, 0, sizeof(c1.session_ticket_key));
    static uint16_t suites[] = {1, 2};
    c1.cipher_suites = slice_from(suites, 2, 2, TYPE_UINT16);
    static TlsCurveID curves[] = {TLS_CURVE_P256};
    c1.curve_preferences = slice_from(curves, 1, 1, TYPE_UINT16);
    c1.renegotiation = TLS_RENEGOTIATE_ONCE_AS_CLIENT;
    static Byte x[] = {'x'};
    c1.encrypted_client_hello_config_list = slice_from(x, 1, 1, TYPE_BYTE);
    static Byte one[] = {1};
    TlsEncryptedClientHelloKey *ech_key = BURROW_NEW(a, TlsEncryptedClientHelloKey);
    ech_key->config = slice_from(one, 1, 1, TYPE_BYTE);
    ech_key->private_key = slice_from(one, 1, 1, TYPE_BYTE);
    c1.encrypted_client_hello_keys =
        slice_from(ech_key, 1, 1, TYPE_TLS_ENCRYPTED_CLIENT_HELLO_KEY);
    c1.a = a;

    /* Set the unexported fields related to session ticket keys, which are
     * copied with Clone(). */
    c1.auto_session_ticket_keys = one_ticket_key(&c1);
    c1.session_ticket_keys = one_ticket_key(&c1);

    TlsConfig *c2 = tls_config_clone(&c1, a);
    if (c2 == NULL)
        testing_t_fatal_v(t, "Clone: out of memory");
#define SAME(f) (memcmp(&c1.f, &c2->f, sizeof(c1.f)) == 0)
    bool same =
        SAME(rand) && SAME(certificates) && SAME(name_to_certificate) &&
        SAME(root_cas) && SAME(next_protos) && SAME(server_name) && SAME(client_auth) &&
        SAME(client_cas) && SAME(cipher_suites) && SAME(client_session_cache) &&
        SAME(curve_preferences) && SAME(renegotiation) && SAME(key_log_writer) &&
        SAME(encrypted_client_hello_config_list) && SAME(encrypted_client_hello_keys) &&
        SAME(a) && SAME(session_ticket_key) && SAME(min_version) && SAME(max_version) &&
        SAME(insecure_skip_verify) && SAME(prefer_server_cipher_suites) &&
        SAME(session_tickets_disabled) && SAME(dynamic_record_sizing_disabled) &&
        SAME(session_ticket_keys) && SAME(auto_session_ticket_keys);
#undef SAME
    if (!same)
        testing_t_errorf_v(t, "clone failed to copy a field");
    tls_config_release(c2);
    tls_config_release(&c1);
    arena_free(&ar);
}

static void TestCloneNilConfig(TestingT *t) {
    Arena ar;
    arena_init(&ar, NULL, 0);
    TlsConfig *config = NULL;
    TlsConfig *cc = tls_config_clone(config, arena_allocator(&ar));
    if (cc != NULL)
        testing_t_fatalf_v(t, "Clone with nil should return nil, got: %p", (void *)cc);
    arena_free(&ar);
}

/* Issue 28744: Ensure that we don't modify memory that Config doesn't own such
 * as Certificates.
 *
 * testConfigServer is not here yet, so the config is a zero one. Nothing of
 * it but Certificates is looked at. */
static void TestBuildNameToCertificate_doesntModifyCertificates(TestingT *t) {
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    TlsCertificate want[2] = {
        parse_test_cert(a, test_rsa2048_cert_pem, test_rsa2048_key_pem),
        parse_test_cert(a, test_sni_cert_pem, test_sni_key_pem),
    };
    TlsCertificate certs[2];
    memcpy(certs, want, sizeof(certs));
    TlsConfig config = {0};
    config.certificates = slice_from(certs, 2, 2, TYPE_TLS_CERTIFICATE);

    Error err = tls_config_build_name_to_certificate(&config, a);
    if (BURROW_FAILED(err))
        testing_t_fatal_v(t, err);
    if (config.certificates.p != certs || config.certificates.len != 2 ||
        memcmp(certs, want, sizeof(certs)) != 0)
        testing_t_fatalf_v(t, "Certificates were mutated by BuildNameToCertificate");
    arena_free(&ar);
}

enum {
    SC_RSA2048,
    SC_ECDSA_P256,
    SC_ED25519,
    SC_MLDSA44,
    SC_MLDSA65,
    SC_MLDSA87,
    SC_PKCS1, /* pkcs1Cert */
};

/* The configs of the cases, SC_CONFIG_NONE for a nil one. */
enum {
    SC_CONFIG_NONE = -1,
    SC_CONFIG_RSA_TLS12, /* RSA_WITH_AES_128_GCM_SHA256, MaxVersion 1.2 */
    SC_CONFIG_RSA,       /* RSA_WITH_AES_128_GCM_SHA256 */
    SC_CONFIG_TLS10,     /* MinVersion 1.0 */
};

/* A case of TestClientHelloInfo_SupportsCertificate. A count of 0 is a nil
 * slice, except for curves, where -1 is the nil one and 0 the empty one. */
typedef struct SupportsCertificateTest {
    int c;
    const char *server_name;
    int ncipher_suites;
    uint16_t cipher_suites[1];
    int ncurves;
    TlsCurveID curves[1];
    int npoints;
    uint8_t points[1];
    int nschemes;
    TlsSignatureScheme schemes[2];
    int nversions;
    uint16_t versions[2];
    int config;
    const char *want_err;
} SupportsCertificateTest;

#define SC_RSA_GCM TLS_TLS_RSA_WITH_AES_128_GCM_SHA256
#define SC_ECDSA_GCM TLS_TLS_ECDHE_ECDSA_WITH_AES_128_GCM_SHA256
#define SC_UNCOMPRESSED TLS_POINT_FORMAT_UNCOMPRESSED

static const SupportsCertificateTest supports_certificate_tests[] = {
    {SC_RSA2048,
     "test.golang.example",
     0,
     {0},
     -1,
     {0},
     0,
     {0},
     1,
     {TLS_PSS_WITH_SHA256},
     1,
     {TLS_VERSION_TLS13},
     SC_CONFIG_NONE,
     ""},
    {SC_ECDSA_P256,
     NULL,
     0,
     {0},
     -1,
     {0},
     0,
     {0},
     2,
     {TLS_PSS_WITH_SHA256, TLS_ECDSA_WITH_P256_AND_SHA256},
     2,
     {TLS_VERSION_TLS13, TLS_VERSION_TLS12},
     SC_CONFIG_NONE,
     ""},
    {SC_RSA2048,
     "example.com",
     0,
     {0},
     -1,
     {0},
     0,
     {0},
     1,
     {TLS_PSS_WITH_SHA256},
     1,
     {TLS_VERSION_TLS13},
     SC_CONFIG_NONE,
     "not valid for requested server name"},
    {SC_ECDSA_P256,
     NULL,
     0,
     {0},
     -1,
     {0},
     0,
     {0},
     1,
     {TLS_ECDSA_WITH_P384_AND_SHA384},
     1,
     {TLS_VERSION_TLS13},
     SC_CONFIG_NONE,
     "signature algorithms"},
    {SC_PKCS1,
     NULL,
     0,
     {0},
     -1,
     {0},
     0,
     {0},
     2,
     {TLS_PSS_WITH_SHA256, TLS_ECDSA_WITH_P256_AND_SHA256},
     1,
     {TLS_VERSION_TLS13},
     SC_CONFIG_NONE,
     "signature algorithms"},

    {SC_RSA2048,
     NULL,
     1,
     {SC_RSA_GCM},
     -1,
     {0},
     0,
     {0},
     1,
     {TLS_PKCS1_WITH_SHA1},
     2,
     {TLS_VERSION_TLS13, TLS_VERSION_TLS12},
     SC_CONFIG_NONE,
     "signature algorithms"},
    /* Check that mutual version selection works. */
    {SC_RSA2048,
     NULL,
     1,
     {SC_RSA_GCM},
     -1,
     {0},
     0,
     {0},
     1,
     {TLS_PKCS1_WITH_SHA1},
     2,
     {TLS_VERSION_TLS13, TLS_VERSION_TLS12},
     SC_CONFIG_RSA_TLS12,
     ""},

    {SC_ECDSA_P256,
     NULL,
     1,
     {SC_ECDSA_GCM},
     1,
     {TLS_CURVE_P256},
     1,
     {SC_UNCOMPRESSED},
     1,
     {TLS_ECDSA_WITH_P256_AND_SHA256},
     1,
     {TLS_VERSION_TLS12},
     SC_CONFIG_NONE,
     ""},
    /* TLS 1.2 does not restrict curves based on the SignatureScheme. */
    {SC_ECDSA_P256,
     NULL,
     1,
     {SC_ECDSA_GCM},
     1,
     {TLS_CURVE_P256},
     1,
     {SC_UNCOMPRESSED},
     1,
     {TLS_ECDSA_WITH_P384_AND_SHA384},
     1,
     {TLS_VERSION_TLS12},
     SC_CONFIG_NONE,
     ""},
    /* TLS 1.2 comes with default signature schemes. */
    {SC_ECDSA_P256,
     NULL,
     1,
     {SC_ECDSA_GCM},
     1,
     {TLS_CURVE_P256},
     1,
     {SC_UNCOMPRESSED},
     0,
     {0},
     1,
     {TLS_VERSION_TLS12},
     SC_CONFIG_NONE,
     ""},
    {SC_ECDSA_P256,
     NULL,
     1,
     {SC_RSA_GCM},
     1,
     {TLS_CURVE_P256},
     1,
     {SC_UNCOMPRESSED},
     1,
     {TLS_ECDSA_WITH_P256_AND_SHA256},
     1,
     {TLS_VERSION_TLS12},
     SC_CONFIG_NONE,
     "cipher suite"},
    {SC_ECDSA_P256,
     NULL,
     1,
     {SC_ECDSA_GCM},
     1,
     {TLS_CURVE_P256},
     1,
     {SC_UNCOMPRESSED},
     1,
     {TLS_ECDSA_WITH_P256_AND_SHA256},
     1,
     {TLS_VERSION_TLS12},
     SC_CONFIG_RSA,
     "cipher suite"},
    {SC_ECDSA_P256,
     NULL,
     1,
     {SC_ECDSA_GCM},
     1,
     {TLS_CURVE_P384},
     1,
     {SC_UNCOMPRESSED},
     1,
     {TLS_ECDSA_WITH_P256_AND_SHA256},
     1,
     {TLS_VERSION_TLS12},
     SC_CONFIG_NONE,
     "certificate curve"},
    {SC_ECDSA_P256,
     NULL,
     1,
     {SC_ECDSA_GCM},
     1,
     {TLS_CURVE_P256},
     1,
     {1},
     1,
     {TLS_ECDSA_WITH_P256_AND_SHA256},
     1,
     {TLS_VERSION_TLS12},
     SC_CONFIG_NONE,
     "only incompatible point formats"},
    {SC_ECDSA_P256,
     NULL,
     1,
     {SC_ECDSA_GCM},
     1,
     {TLS_CURVE_P256},
     1,
     {SC_UNCOMPRESSED},
     1,
     {TLS_PSS_WITH_SHA256},
     1,
     {TLS_VERSION_TLS12},
     SC_CONFIG_NONE,
     "signature algorithms"},

    {SC_ED25519,
     NULL,
     1,
     {SC_ECDSA_GCM},
     1,
     {TLS_CURVE_P256},
     1,
     {SC_UNCOMPRESSED},
     1,
     {TLS_ED25519},
     1,
     {TLS_VERSION_TLS12},
     SC_CONFIG_NONE,
     ""},
    {SC_ED25519,
     NULL,
     1,
     {SC_ECDSA_GCM},
     1,
     {TLS_CURVE_P256},
     1,
     {SC_UNCOMPRESSED},
     1,
     {TLS_ED25519},
     1,
     {TLS_VERSION_TLS10},
     SC_CONFIG_TLS10,
     "doesn't support Ed25519"},
    {SC_ED25519,
     NULL,
     1,
     {SC_ECDSA_GCM},
     0,
     {0},
     1,
     {SC_UNCOMPRESSED},
     1,
     {TLS_ED25519},
     1,
     {TLS_VERSION_TLS12},
     SC_CONFIG_NONE,
     "doesn't support ECDHE"},

    {SC_RSA2048,
     NULL,
     1,
     {TLS_TLS_ECDHE_RSA_WITH_AES_128_CBC_SHA},
     1,
     {TLS_CURVE_P256},
     1,
     {SC_UNCOMPRESSED},
     0,
     {0},
     1,
     {TLS_VERSION_TLS10},
     SC_CONFIG_TLS10,
     ""},
    /* static RSA fallback */
    {SC_RSA2048,
     NULL,
     1,
     {SC_RSA_GCM},
     -1,
     {0},
     0,
     {0},
     0,
     {0},
     1,
     {TLS_VERSION_TLS12},
     SC_CONFIG_RSA,
     ""},

    {SC_MLDSA44,
     NULL,
     0,
     {0},
     -1,
     {0},
     0,
     {0},
     1,
     {TLS_MLDSA44},
     1,
     {TLS_VERSION_TLS13},
     SC_CONFIG_NONE,
     ""},
    {SC_MLDSA65,
     NULL,
     0,
     {0},
     -1,
     {0},
     0,
     {0},
     1,
     {TLS_MLDSA65},
     1,
     {TLS_VERSION_TLS13},
     SC_CONFIG_NONE,
     ""},
    {SC_MLDSA87,
     NULL,
     0,
     {0},
     -1,
     {0},
     0,
     {0},
     1,
     {TLS_MLDSA87},
     1,
     {TLS_VERSION_TLS13},
     SC_CONFIG_NONE,
     ""},
    {SC_MLDSA44,
     NULL,
     1,
     {SC_ECDSA_GCM},
     1,
     {TLS_CURVE_P256},
     1,
     {SC_UNCOMPRESSED},
     0,
     {0},
     1,
     {TLS_VERSION_TLS12},
     SC_CONFIG_NONE,
     "doesn't support ML-DSA"},
};

/* Not in Go: a Slice of the n elements at p, or a nil one when n is negative,
 * or 0 when zero_is_nil. */
static Slice sc_slice(const void *p, int n, bool zero_is_nil, const Type *elem) {
    if (n < 0 || (n == 0 && zero_is_nil))
        return slice_nil(elem);
    return slice_from((void *)(uintptr_t)p, n, n, elem);
}

static void TestClientHelloInfo_SupportsCertificate(TestingT *t) {
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);

    static const TlsSignatureScheme pkcs1_algs[] = {TLS_PKCS1_WITH_SHA1,
                                                    TLS_PKCS1_WITH_SHA256};
    TlsCertificate certs[7];
    certs[SC_RSA2048] = parse_test_cert(a, test_rsa2048_cert_pem, test_rsa2048_key_pem);
    certs[SC_ECDSA_P256] =
        parse_test_cert(a, test_ecdsap256_cert_pem, test_ecdsap256_key_pem);
    certs[SC_ED25519] = parse_test_cert(a, test_ed25519_cert_pem, test_ed25519_key_pem);
    certs[SC_MLDSA44] = parse_test_cert(a, test_mldsa44_cert_pem, test_mldsa44_key_pem);
    certs[SC_MLDSA65] = parse_test_cert(a, test_mldsa65_cert_pem, test_mldsa65_key_pem);
    certs[SC_MLDSA87] = parse_test_cert(a, test_mldsa87_cert_pem, test_mldsa87_key_pem);
    certs[SC_PKCS1] = certs[SC_RSA2048];
    certs[SC_PKCS1].supported_signature_algorithms =
        slice_from((void *)(uintptr_t)pkcs1_algs, 2, 2, TYPE_UINT16);

    static uint16_t rsa_suites[] = {TLS_TLS_RSA_WITH_AES_128_GCM_SHA256};
    TlsConfig configs[3];
    memset(configs, 0, sizeof(configs));
    configs[SC_CONFIG_RSA_TLS12].cipher_suites =
        slice_from(rsa_suites, 1, 1, TYPE_UINT16);
    configs[SC_CONFIG_RSA_TLS12].max_version = TLS_VERSION_TLS12;
    configs[SC_CONFIG_RSA].cipher_suites = slice_from(rsa_suites, 1, 1, TYPE_UINT16);
    configs[SC_CONFIG_TLS10].min_version = TLS_VERSION_TLS10;

    for (Int i = 0; i < (Int)(sizeof(supports_certificate_tests) /
                              sizeof(supports_certificate_tests[0]));
         i++) {
        const SupportsCertificateTest *tt = &supports_certificate_tests[i];
        TlsClientHelloInfo chi = {0};
        chi.server_name =
            tt->server_name != NULL ? str_from_cstr(tt->server_name) : BURROW_STR_EMPTY;
        chi.cipher_suites =
            sc_slice(tt->cipher_suites, tt->ncipher_suites, true, TYPE_UINT16);
        chi.supported_curves = sc_slice(tt->curves, tt->ncurves, false, TYPE_UINT16);
        chi.supported_points = sc_slice(tt->points, tt->npoints, true, TYPE_BYTE);
        chi.signature_schemes = sc_slice(tt->schemes, tt->nschemes, true, TYPE_UINT16);
        chi.supported_versions =
            sc_slice(tt->versions, tt->nversions, true, TYPE_UINT16);
        if (tt->config != SC_CONFIG_NONE)
            chi.config = &configs[tt->config];

        Error err = tls_client_hello_info_supports_certificate(&chi, &certs[tt->c]);
        bool want_err = tt->want_err[0] != '\0';
        if (!want_err && BURROW_FAILED(err))
            testing_t_errorf_v(t, "%d: unexpected error: %v", i, err);
        else if (want_err && !BURROW_FAILED(err))
            testing_t_errorf_v(t, "%d: unexpected success", i);
        else if (want_err &&
                 !strings_contains(error_text(err), str_from_cstr(tt->want_err)))
            testing_t_errorf_v(t, "%d: got error %q, expected %q", i, err,
                               str_from_cstr(tt->want_err));
    }
    arena_free(&ar);
}

static void TestKeyLogWriterErr(TestingT *t) {
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    OsFile *f = NULL; /* typed nil */
    TlsConfig config = {0};
    config.key_log_writer = os_file_as_io_writer(f);
    Error err = burrow__tls_config_write_key_log(&config, BURROW_S("CLIENT_RANDOM"),
                                                 slice_make(a, TYPE_BYTE, 32, 32),
                                                 slice_make(a, TYPE_BYTE, 48, 48));
    if (!BURROW_FAILED(err))
        testing_t_fatal_v(t, "writeKeyLog: expected error, got nil");

    if (!errors_is(err, os_err_invalid))
        testing_t_errorf_v(t, "got %v, want %v", err, os_err_invalid);
    if (!strings_contains(error_text(err), BURROW_S("KeyLogWriter")))
        testing_t_errorf_v(t, "got %q, want %q", error_text(err),
                           BURROW_S("KeyLogWriter"));
    arena_free(&ar);
}

#define TESTS(X)                                                                       \
    X(TestX509KeyPair)                                                                 \
    X(TestCipherSuites)                                                                \
    X(TestVersionName)                                                                 \
    X(TestX509KeyPairPopulateCertificate)                                              \
    X(TestCloneFuncFields)                                                             \
    X(TestCloneNonFuncFields)                                                          \
    X(TestCloneNilConfig)                                                              \
    X(TestBuildNameToCertificate_doesntModifyCertificates)                             \
    X(TestClientHelloInfo_SupportsCertificate)                                         \
    X(TestKeyLogWriterErr)

TESTING_MAIN(TESTS)
