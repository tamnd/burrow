/* Derived from Go's src/crypto/tls/tls_test.go.
 * Go source: go1.27.1.
 *
 * The tests that listen and dial do it over loopback TCP, so they skip where
 * there is no readiness poll FD. TestConnectionStateMarshal, the ML-KEM and
 * ML-DSA handshakes and ECH are still to come. Where Go compares with
 * reflect.DeepEqual, the certificates here are compared by their DER. The PEMs
 * TestX509KeyPair declares are in tls_test_gen.h, and the ML-DSA ones are
 * always tried, as there is no FIPS 140-3 module here to be v1.0.0. Go ranges
 * over the disabledCipherSuites map; here that map is a predicate, so the test
 * asks it about every suite there is.
 *
 * Copyright 2012 The Go Authors. All rights reserved.
 * Copyright 2026 The burrow Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style licence that can be found
 * in the LICENSE file. */

#include "check.h"

#include "../src/crypto/tls_internal.h"
#include "burrow/burrow.h"
#include "burrow/bytes.h"
#include "burrow/chan.h"
#include "burrow/context.h"
#include "burrow/crypto/ecdsa.h"
#include "burrow/crypto/elliptic.h"
#include "burrow/crypto/tls.h"
#include "burrow/crypto/x509.h"
#include "burrow/encoding/pem.h"
#include "burrow/fmt.h"
#include "burrow/io.h"
#include "burrow/map.h"
#include "burrow/math/big.h"
#include "burrow/mem/arena.h"
#include "burrow/mem/heap.h"
#include "burrow/net.h"
#include "burrow/netpoll.h"
#include "burrow/os.h"
#include "burrow/panic.h"
#include "burrow/strings.h"
#include "burrow/sync.h"
#include "burrow/time.h"
#include "tls_handshake_test.h"
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

/* ---------------------------------------------------------------- the dials */

#if defined(BURROW_NETPOLL_READINESS) && !defined(BURROW_OS_WASI)
#define HAVE_TCP 1
#endif

/* Not in Go: the tests below listen and dial on loopback, which needs the
 * readiness poll FD. */
static void need_tcp(TestingT *t) {
#if !defined(HAVE_TCP)
    testing_t_skip_v(t, "TCP here needs the readiness poll FD");
#else
    (void)t;
#endif
}

/* Not in Go: Go's == on two errors. */
static bool same_error(Error a, Error b) {
    return a.vt == b.vt && a.data == b.data;
}

static NetListener new_local_listener(TestingT *t) {
    testing_t_helper(t);
    Error err = BURROW_NO_ERROR;
    NetListener ln =
        net_listen(heap_allocator(), BURROW_S("tcp"), BURROW_S("127.0.0.1:0"), &err);
    if (BURROW_FAILED(err)) {
        err = BURROW_NO_ERROR;
        ln = net_listen(heap_allocator(), BURROW_S("tcp6"), BURROW_S("[::1]:0"), &err);
    }
    if (BURROW_FAILED(err)) {
        testing_t_fatal_v(t, err);
        return (NetListener){NULL, NULL};
    }
    return ln;
}

/* Not in Go: ln.Addr().String(), made in a. */
static Str listener_addr(Alloc *a, NetListener ln) {
    NetAddr addr = ln.vt->addr(ln.data);
    return addr.vt->string(addr.data, a);
}

/* Not in Go: c.RemoteAddr().String(), made in a. */
static Str remote_addr(Alloc *a, NetConn c) {
    NetAddr addr = c.vt->remote_addr(c.data);
    if (addr.vt == NULL)
        return BURROW_S("<nil>");
    return addr.vt->string(addr.data, a);
}

/* Not in Go: the goroutine TestDialTimeout starts. A NetConn is two words, so
 * the Conns go over acceptc by pointer, made on the heap. */
typedef struct DialTimeoutAccept {
    NetListener listener;
    Chan *acceptc;
} DialTimeoutAccept;

static void dial_timeout_accept(void *env) {
    DialTimeoutAccept *j = (DialTimeoutAccept *)env;
    for (;;) {
        Error err = BURROW_NO_ERROR;
        NetConn conn = j->listener.vt->accept(j->listener.data, &err);
        if (BURROW_FAILED(err)) {
            chan_close(j->acceptc);
            return;
        }
        NetConn *p = BURROW_NEW(heap_allocator(), NetConn);
        if (p == NULL) {
            net_conn_free(conn);
            chan_close(j->acceptc);
            return;
        }
        *p = conn;
        chan_send(j->acceptc, &p);
    }
}

/* Not in Go: lconn.Close() on a Conn from acceptc, which also gives it back. */
static void accepted_close(NetConn *p) {
    net_conn_free(*p);
    mem_free(heap_allocator(), p, sizeof *p, _Alignof(NetConn));
}

static bool is_timeout_error(Error err) {
    return net_error_timeout(err);
}

static void TestDialTimeout(TestingT *t) {
    if (testing_short()) {
        testing_t_skip_v(t, "skipping in short mode");
        return;
    }
    need_tcp(t);

    Duration timeout = 100 * TIME_MICROSECOND;
    while (!testing_t_failed(t)) {
        Chan *acceptc = chan_make(heap_allocator(), TYPE_UNSAFE_POINTER, 0);
        if (acceptc == NULL) {
            testing_t_fatal_v(t, "out of memory");
            return;
        }
        DialTimeoutAccept job = {new_local_listener(t), acceptc};
        SyncWaitGroup wg = {0};
        sync_wait_group_go(&wg, BURROW_FN(Func, dial_timeout_accept, &job));

        Arena ar;
        arena_init(&ar, NULL, 0);
        Alloc *a = arena_allocator(&ar);
        Str addr = listener_addr(a, job.listener);
        NetDialer dialer;
        memset(&dialer, 0, sizeof dialer);
        dialer.timeout = timeout;
        Error err = BURROW_NO_ERROR;
        TlsConn *conn = tls_dial_with_dialer(heap_allocator(), &dialer, BURROW_S("tcp"),
                                             addr, NULL, &err);
        if (!BURROW_FAILED(err)) {
            (void)tls_conn_close(conn);
            tls_conn_free(conn);
            testing_t_errorf_v(t,
                               "DialWithTimeout unexpectedly completed successfully");
        } else if (!is_timeout_error(err)) {
            testing_t_errorf_v(t, "resulting error not a timeout: %v\nType %T: %#v",
                               err, err, err);
        }

        (void)job.listener.vt->closer.close(job.listener.data);

        /* We're looking for a timeout during the handshake, so check that the
         * Listener actually accepted the connection to initiate it. (If the
         * server takes too long to accept the connection, we might cancel
         * before the underlying net.Conn is ever dialed — without ever
         * attempting a handshake.) */
        NetConn *lconn = NULL;
        bool ok = chan_recv(acceptc, &lconn);
        if (ok) {
            /* The Listener accepted a connection, so assume that it was from
             * our Dial: we triggered the timeout at the point where we wanted
             * it! */
            testing_t_logf_v(t, "Listener accepted a connection from %s",
                             remote_addr(a, *lconn));
            accepted_close(lconn);
        }
        /* Close any spurious extra connections from the listener. (This is
         * possible if there are, for example, stray Dial calls from other
         * tests.) */
        NetConn *extra_conn = NULL;
        while (chan_recv(acceptc, &extra_conn)) {
            testing_t_logf_v(t, "spurious extra connection from %s",
                             remote_addr(a, *extra_conn));
            accepted_close(extra_conn);
        }
        sync_wait_group_wait(&wg);
        net_listener_free(job.listener);
        chan_free(acceptc);
        if (!ok)
            testing_t_logf_v(t,
                             "with timeout %v, DialWithDialer returned before listener "
                             "accepted any connections; retrying",
                             duration_string(timeout, a));
        arena_free(&ar);
        if (ok)
            break;
        timeout *= 2;
    }
}

/* Not in Go: the goroutine that accepts one connection from ln and shakes
 * hands on it as the server, with a clone of testConfigServer. srv is NULL
 * when that failed, and serr says why, kept in ar. */
typedef struct AcceptServer {
    NetListener ln;
    TlsConfig config;
    NetConn sconn;
    TlsConn *srv;
    bool ok;
    Error serr;
    Arena ar;
} AcceptServer;

static void accept_server_init(AcceptServer *j, NetListener ln, Alloc *a) {
    memset(j, 0, sizeof *j);
    j->ln = ln;
    j->config = test_config_server(a);
    arena_init(&j->ar, NULL, 0);
}

static void accept_server(void *env) {
    AcceptServer *j = (AcceptServer *)env;
    Error err = BURROW_NO_ERROR;
    j->sconn = j->ln.vt->accept(j->ln.data, &err);
    if (BURROW_FAILED(err)) {
        j->serr = error_retain(arena_allocator(&j->ar), err);
        return;
    }
    j->srv = tls_server(heap_allocator(), j->sconn, &j->config);
    if (j->srv == NULL) {
        j->serr = burrow_err_out_of_memory;
        return;
    }
    err = tls_conn_handshake(j->srv);
    if (BURROW_FAILED(err)) {
        j->serr =
            error_retain(arena_allocator(&j->ar), fmt_errorf_v("handshake: %v", err));
        return;
    }
    j->ok = true;
}

static void accept_server_free(AcceptServer *j) {
    tls_conn_free(j->srv);
    net_conn_free(j->sconn);
    tls_config_release(&j->config);
    arena_free(&j->ar);
}

/* Not in Go: the end of a test that started accept_server, which Go defers. */
static void accept_server_end(AcceptServer *j, SyncWaitGroup *wg, TlsConn *conn) {
    if (conn != NULL) {
        (void)tls_conn_close(conn);
        tls_conn_free(conn);
    }
    (void)j->ln.vt->closer.close(j->ln.data);
    sync_wait_group_wait(wg);
    net_listener_free(j->ln);
    accept_server_free(j);
}

static void deadline_on_write(TestingT *t, AcceptServer *job, SyncWaitGroup *wg,
                              TlsConn *conn) {
    sync_wait_group_wait(wg);
    TlsConn *srv = job->ok ? job->srv : NULL;
    if (srv == NULL) {
        testing_t_error_v(t, job->serr);
        return;
    }

    /* Make sure the client/server is setup correctly and is able to do a
     * typical Write/Read */
    Byte buf[6] = {0};
    Error err = BURROW_NO_ERROR;
    (void)tls_conn_write(srv, slice_from((Byte *)(uintptr_t)"foobar", 6, 6, TYPE_BYTE),
                         &err);
    if (BURROW_FAILED(err))
        testing_t_errorf_v(t, "Write err: %v", err);
    err = BURROW_NO_ERROR;
    Int n = tls_conn_read(conn, slice_from(buf, 6, 6, TYPE_BYTE), &err);
    Str got = {buf, 6};
    if (n != 6 || BURROW_FAILED(err) || !str_eq(got, BURROW_S("foobar")))
        testing_t_errorf_v(t, "Read = %d, %v, data %q; want 6, nil, foobar", n, err,
                           got);

    /* Set a deadline which should cause Write to timeout */
    err = tls_conn_set_deadline(srv, time_now());
    if (BURROW_FAILED(err)) {
        testing_t_errorf_v(t, "SetDeadline(time.Now()) err: %v", err);
        return;
    }
    err = BURROW_NO_ERROR;
    (void)tls_conn_write(
        srv, slice_from((Byte *)(uintptr_t)"should fail", 11, 11, TYPE_BYTE), &err);
    if (!BURROW_FAILED(err)) {
        testing_t_error_v(t, "Write should have timed out");
        return;
    }

    /* Clear deadline and make sure it still times out */
    Time zero;
    memset(&zero, 0, sizeof zero);
    err = tls_conn_set_deadline(srv, zero);
    if (BURROW_FAILED(err)) {
        testing_t_errorf_v(t, "SetDeadline(time.Time{}) err: %v", err);
        return;
    }
    static const char broken[] = "This connection is permanently broken";
    Int len = (Int)sizeof broken - 1;
    err = BURROW_NO_ERROR;
    (void)tls_conn_write(
        srv, slice_from((Byte *)(uintptr_t)broken, len, len, TYPE_BYTE), &err);
    if (!BURROW_FAILED(err)) {
        testing_t_error_v(t, "Write which previously failed should still time out");
        return;
    }

    /* Verify the error */
    if (net_error_temporary(err) != false)
        testing_t_error_v(t, "Write timed out but incorrectly classified the error as "
                             "Temporary");
    if (!is_timeout_error(err))
        testing_t_error_v(t, "Write timed out but did not classify the error as a "
                             "Timeout");
}

static void TestDeadlineOnWrite(TestingT *t) {
    if (testing_short()) {
        testing_t_skip_v(t, "skipping in short mode");
        return;
    }
    need_tcp(t);

    NetListener ln = new_local_listener(t);
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    AcceptServer job;
    accept_server_init(&job, ln, a);
    SyncWaitGroup wg = {0};
    sync_wait_group_go(&wg, BURROW_FN(Func, accept_server, &job));

    TlsConfig client_config = test_config_client(a);
    client_config.max_version = TLS_VERSION_TLS12;
    Error err = BURROW_NO_ERROR;
    TlsConn *conn = tls_dial(heap_allocator(), BURROW_S("tcp"), listener_addr(a, ln),
                             &client_config, &err);
    if (BURROW_FAILED(err))
        testing_t_error_v(t, err);
    else
        deadline_on_write(t, &job, &wg, conn);
    accept_server_end(&job, &wg, conn);
    tls_config_release(&client_config);
    arena_free(&ar);
}

/* readerFunc, a func that is an io.Reader. */
typedef struct ReaderFunc {
    Int (*f)(void *env, Slice b, Error *err);
    void *env;
} ReaderFunc;

static Int reader_func_read(void *self, Slice b, Error *err) {
    ReaderFunc *f = (ReaderFunc *)self;
    return f->f(f->env, b, err);
}

static const Type reader_func_type = {
    BURROW_S_INIT("readerFunc"),
    BURROW_S_INIT("tls_test"),
    KIND_STRUCT,
    (uint32_t)sizeof(ReaderFunc),
    (uint16_t)_Alignof(ReaderFunc),
    0,
    0,
    NULL,
    NULL,
    NULL,
    NULL,
    0,
    0x72646672U, /* "rdfr" */
    NULL,
};

static const IoReaderVT reader_func_vt = {&reader_func_type, reader_func_read};

/* Not in Go: the goroutine of TestDialer. */
typedef struct DialerServer {
    NetListener ln;
    Chan *unblock_server;
} DialerServer;

static void dialer_server(void *env) {
    DialerServer *j = (DialerServer *)env;
    Error err = BURROW_NO_ERROR;
    NetConn conn = j->ln.vt->accept(j->ln.data, &err);
    if (BURROW_FAILED(err))
        return;
    (void)chan_recv(j->unblock_server, NULL);
    net_conn_free(conn);
}

static Int dialer_rand(void *env, Slice b, Error *err) {
    (void)err;
    /* By the time crypto/tls wants randomness, that means it has a TCP
     * connection, so we're past the Dialer's dial and now blocked in a
     * handshake. Cancel our context and see if we get unstuck. (Our TCP
     * listener above never reads or writes, so the Handshake would otherwise
     * be stuck forever) */
    BURROW_CALLF0(*(ContextCancelFunc *)env);
    return b.len;
}

/* TestDialer tests that tls.Dialer.DialContext can abort in the middle of a
 * handshake. (The other cases are all handled by the existing dial tests in
 * this package, which all also flow through the same code shared code paths) */
static void TestDialer(TestingT *t) {
    need_tcp(t);
    NetListener ln = new_local_listener(t);

    Chan *unblock_server = chan_make(heap_allocator(), TYPE_BOOL, 0); /* close-only */
    if (unblock_server == NULL) {
        net_listener_free(ln);
        testing_t_fatal_v(t, "out of memory");
        return;
    }
    DialerServer job = {ln, unblock_server};
    SyncWaitGroup wg = {0};
    sync_wait_group_go(&wg, BURROW_FN(Func, dialer_server, &job));

    ContextCancelFunc cancel;
    Context ctx = context_with_cancel(heap_allocator(), context_background(), &cancel);
    if (BURROW_CONTEXT_IS_NIL(ctx)) {
        testing_t_fatal_v(t, "out of memory");
        return;
    }
    ReaderFunc rand = {dialer_rand, &cancel};
    TlsConfig config;
    memset(&config, 0, sizeof config);
    config.rand = (IoReader){&reader_func_vt, &rand};
    config.server_name = BURROW_S("foo");
    TlsDialer d = {NULL, &config};
    Arena ar;
    arena_init(&ar, NULL, 0);
    Error err = BURROW_NO_ERROR;
    NetConn c = tls_dialer_dial_context(&d, heap_allocator(), ctx, BURROW_S("tcp"),
                                        listener_addr(arena_allocator(&ar), ln), &err);
    if (!same_error(err, context_canceled))
        testing_t_errorf_v(t, "err = %v; want context.Canceled", err);

    tls_conn_free(tls_conn_from_net_conn(c));
    chan_close(unblock_server);
    (void)ln.vt->closer.close(ln.data);
    sync_wait_group_wait(&wg);
    BURROW_CALLF0(cancel);
    context_release(ctx);
    chan_free(unblock_server);
    net_listener_free(ln);
    tls_config_release(&config);
    arena_free(&ar);
}

static Error conn_read_nonzero_and_eof_on(TlsConn *conn, AcceptServer *job,
                                          SyncWaitGroup *wg, Duration delay) {
    sync_wait_group_wait(wg);
    TlsConn *srv = job->ok ? job->srv : NULL;
    if (srv == NULL)
        return job->serr;

    Byte buf[6] = {0};
    Slice b = slice_from(buf, 6, 6, TYPE_BYTE);
    Str got = {buf, 6};

    Error err = BURROW_NO_ERROR;
    (void)tls_conn_write(srv, slice_from((Byte *)(uintptr_t)"foobar", 6, 6, TYPE_BYTE),
                         &err);
    err = BURROW_NO_ERROR;
    Int n = tls_conn_read(conn, b, &err);
    if (n != 6 || BURROW_FAILED(err) || !str_eq(got, BURROW_S("foobar")))
        return fmt_errorf_v("Read = %d, %v, data %q; want 6, nil, foobar", n, err, got);

    (void)tls_conn_write(srv, slice_from((Byte *)(uintptr_t)"abcdef", 6, 6, TYPE_BYTE),
                         &err);
    (void)tls_conn_close(srv);
    time_sleep(delay);
    err = BURROW_NO_ERROR;
    n = tls_conn_read(conn, b, &err);
    if (n != 6 || !str_eq(got, BURROW_S("abcdef")))
        return fmt_errorf_v("Read = %d, buf= %q; want 6, abcdef", n, got);
    if (!same_error(err, io_eof))
        return fmt_errorf_v("Second Read error = %v; want io.EOF", err);
    return BURROW_NO_ERROR;
}

/* The error comes back made in a. */
static Error test_conn_read_nonzero_and_eof(TestingT *t, Duration delay, Alloc *a) {
    NetListener ln = new_local_listener(t);
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *ca = arena_allocator(&ar);
    AcceptServer job;
    accept_server_init(&job, ln, ca);
    SyncWaitGroup wg = {0};
    sync_wait_group_go(&wg, BURROW_FN(Func, accept_server, &job));

    TlsConfig client_config = test_config_client(ca);
    /* In TLS 1.3, alerts are encrypted and disguised as application data, so
     * the opportunistic peek won't work. */
    client_config.max_version = TLS_VERSION_TLS12;
    Error err = BURROW_NO_ERROR;
    TlsConn *conn = tls_dial(heap_allocator(), BURROW_S("tcp"), listener_addr(ca, ln),
                             &client_config, &err);
    if (BURROW_FAILED(err)) {
        testing_t_error_v(t, err);
        err = BURROW_NO_ERROR;
    } else {
        err = error_retain(a, conn_read_nonzero_and_eof_on(conn, &job, &wg, delay));
    }
    accept_server_end(&job, &wg, conn);
    tls_config_release(&client_config);
    arena_free(&ar);
    return err;
}

/* tests that Conn.Read returns (non-zero, io.EOF) instead of (non-zero, nil)
 * when a Close (alertCloseNotify) is sitting right behind the application data
 * in the buffer. */
static void TestConnReadNonzeroAndEOF(TestingT *t) {
    /* This test is racy: it assumes that after a write to a localhost TCP
     * connection, the peer TCP connection can immediately read it. Because
     * it's racy, we skip this test in short mode, and then retry it several
     * times with an increasing sleep in between our final write (via
     * srv.Close below) and the following read. */
    if (testing_short()) {
        testing_t_skip_v(t, "skipping in short mode");
        return;
    }
    need_tcp(t);
    Arena ar;
    arena_init(&ar, NULL, 0);
    Error err = BURROW_NO_ERROR;
    for (Duration delay = TIME_MILLISECOND; delay <= 64 * TIME_MILLISECOND;
         delay *= 2) {
        err = test_conn_read_nonzero_and_eof(t, delay, arena_allocator(&ar));
        if (!BURROW_FAILED(err)) {
            arena_free(&ar);
            return;
        }
    }
    testing_t_error_v(t, err);
    arena_free(&ar);
}

/* Not in Go: the goroutine of TestTLSUniqueMatches. The server Conns are kept
 * until the test is done with what they hand over. */
typedef struct TLSUniqueServer {
    TestingT *t;
    NetListener ln;
    TlsConfig *server_config;
    Chan *server_tls_uniques;
    Chan *parent_done;
    Chan *child_done;
    NetConn sconn[2];
    TlsConn *srv[2];
    Slice unique[2];
} TLSUniqueServer;

static void tls_unique_server(void *env) {
    TLSUniqueServer *j = (TLSUniqueServer *)env;
    for (int i = 0; i < 2; i++) {
        Error err = BURROW_NO_ERROR;
        j->sconn[i] = j->ln.vt->accept(j->ln.data, &err);
        if (BURROW_FAILED(err)) {
            testing_t_error_v(j->t, err);
            break;
        }
        j->srv[i] = tls_server(heap_allocator(), j->sconn[i], j->server_config);
        if (j->srv[i] == NULL) {
            testing_t_error_v(j->t, "out of memory");
            break;
        }
        err = tls_conn_handshake(j->srv[i]);
        if (BURROW_FAILED(err)) {
            testing_t_error_v(j->t, err);
            break;
        }
        j->unique[i] = tls_conn_connection_state(j->srv[i]).tls_unique;
        Slice *unique = &j->unique[i];
        SelectCase cases[] = {
            BURROW_RECV(j->parent_done, NULL),
            BURROW_SEND(j->server_tls_uniques, &unique),
        };
        if (chan_select(cases, 2) == 0)
            break;
    }
    chan_close(j->child_done);
}

static const Byte zero12[12];

/* Not in Go: the body of TestTLSUniqueMatches, up to where it returns. The
 * client Conns are left in conns for the test to give back. */
static void tls_unique_matches(TestingT *t, TLSUniqueServer *job, Str addr,
                               TlsConfig *client_config, TlsConn **conns) {
    Error err = BURROW_NO_ERROR;
    TlsConn *conn =
        tls_dial(heap_allocator(), BURROW_S("tcp"), addr, client_config, &err);
    if (BURROW_FAILED(err)) {
        testing_t_error_v(t, err);
        return;
    }
    conns[0] = conn;

    Slice *server_tls_uniques_value = NULL;
    SelectCase cases[] = {
        BURROW_RECV(job->child_done, NULL),
        BURROW_RECV(job->server_tls_uniques, &server_tls_uniques_value),
    };
    if (chan_select(cases, 2) == 0)
        return;

    Slice zero = slice_from((Byte *)(uintptr_t)zero12, 12, 12, TYPE_BYTE);
    if (!bytes_equal(tls_conn_connection_state(conn).tls_unique,
                     *server_tls_uniques_value))
        testing_t_error_v(t, "client and server channel bindings differ");
    if (server_tls_uniques_value->p == NULL ||
        bytes_equal(*server_tls_uniques_value, zero))
        testing_t_error_v(t, "tls-unique is empty or zero");
    (void)tls_conn_close(conn);

    conn = tls_dial(heap_allocator(), BURROW_S("tcp"), addr, client_config, &err);
    if (BURROW_FAILED(err)) {
        testing_t_error_v(t, err);
        return;
    }
    conns[1] = conn;
    if (!tls_conn_connection_state(conn).did_resume)
        testing_t_error_v(t, "second session did not use resumption");

    if (chan_select(cases, 2) == 0)
        return;

    if (!bytes_equal(tls_conn_connection_state(conn).tls_unique,
                     *server_tls_uniques_value))
        testing_t_error_v(t, "client and server channel bindings differ when session "
                             "resumption is used");
    if (server_tls_uniques_value->p == NULL ||
        bytes_equal(*server_tls_uniques_value, zero))
        testing_t_error_v(t, "resumption tls-unique is empty or zero");
}

static void TestTLSUniqueMatches(TestingT *t) {
    need_tcp(t);
    NetListener ln = new_local_listener(t);
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);

    TlsConfig server_config = test_config_server(a);
    server_config.max_version = TLS_VERSION_TLS12; /* TLSUnique is not defined in
                                                      TLS 1.3 */

    TLSUniqueServer job;
    memset(&job, 0, sizeof job);
    job.t = t;
    job.ln = ln;
    job.server_config = &server_config;
    job.server_tls_uniques = chan_make(heap_allocator(), TYPE_UNSAFE_POINTER, 0);
    job.parent_done = chan_make(heap_allocator(), TYPE_BOOL, 0);
    job.child_done = chan_make(heap_allocator(), TYPE_BOOL, 0);
    if (job.server_tls_uniques == NULL || job.parent_done == NULL ||
        job.child_done == NULL) {
        testing_t_fatal_v(t, "out of memory");
        return;
    }
    SyncWaitGroup wg = {0};
    sync_wait_group_go(&wg, BURROW_FN(Func, tls_unique_server, &job));

    TlsConfig client_config = test_config_client(a);
    client_config.client_session_cache =
        tls_new_lru_client_session_cache(heap_allocator(), 1);
    TlsConn *conns[2] = {NULL, NULL};
    tls_unique_matches(t, &job, listener_addr(a, ln), &client_config, conns);

    chan_close(job.parent_done);
    (void)ln.vt->closer.close(ln.data);
    sync_wait_group_wait(&wg);
    for (int i = 0; i < 2; i++) {
        if (conns[i] != NULL)
            (void)tls_conn_close(conns[i]);
        tls_conn_free(conns[i]);
        tls_conn_free(job.srv[i]);
        net_conn_free(job.sconn[i]);
    }
    chan_free(job.server_tls_uniques);
    chan_free(job.parent_done);
    chan_free(job.child_done);
    net_listener_free(ln);
    tls_lru_client_session_cache_free(client_config.client_session_cache);
    tls_config_release(&client_config);
    tls_config_release(&server_config);
    arena_free(&ar);
}

/* Not in Go: what testenv.MustHaveExternalNetwork asks here. */
static void must_have_external_network(TestingT *t) {
    if (testing_short()) {
        testing_t_skip_v(t, "skipping test: no external network in -short mode");
        return;
    }
    need_tcp(t);
}

static void TestVerifyHostname(TestingT *t) {
    must_have_external_network(t);

    Error err = BURROW_NO_ERROR;
    TlsConn *c = tls_dial(heap_allocator(), BURROW_S("tcp"),
                          BURROW_S("www.google.com:https"), NULL, &err);
    if (BURROW_FAILED(err)) {
        testing_t_fatal_v(t, err);
        return;
    }
    err = tls_conn_verify_hostname(c, BURROW_S("www.google.com"));
    if (BURROW_FAILED(err)) {
        testing_t_fatalf_v(t, "verify www.google.com: %v", err);
        return;
    }
    err = tls_conn_verify_hostname(c, BURROW_S("www.yahoo.com"));
    if (!BURROW_FAILED(err)) {
        testing_t_fatalf_v(t, "verify www.yahoo.com succeeded");
        return;
    }
    (void)tls_conn_close(c);
    tls_conn_free(c);

    TlsConfig config;
    memset(&config, 0, sizeof config);
    config.insecure_skip_verify = true;
    err = BURROW_NO_ERROR;
    c = tls_dial(heap_allocator(), BURROW_S("tcp"), BURROW_S("www.google.com:https"),
                 &config, &err);
    if (BURROW_FAILED(err)) {
        testing_t_fatal_v(t, err);
        return;
    }
    err = tls_conn_verify_hostname(c, BURROW_S("www.google.com"));
    if (!BURROW_FAILED(err)) {
        testing_t_fatalf_v(t, "verify www.google.com succeeded with "
                              "InsecureSkipVerify=true");
        return;
    }
    (void)tls_conn_close(c);
    tls_conn_free(c);
    tls_config_release(&config);
}

static void TestRealResumption(TestingT *t) {
    must_have_external_network(t);

    TlsConfig config;
    memset(&config, 0, sizeof config);
    config.server_name = BURROW_S("yahoo.com");
    config.client_session_cache = tls_new_lru_client_session_cache(heap_allocator(), 0);
    static const char get[] =
        "GET / HTTP/1.1\r\nHost: yahoo.com\r\nConnection: close\r\n\r\n";
    Int get_len = (Int)sizeof get - 1;
    Byte buf[4096];

    bool did_resume = false;
    for (int i = 0; i < 10 && !did_resume; i++) {
        Error err = BURROW_NO_ERROR;
        TlsConn *conn = tls_dial(heap_allocator(), BURROW_S("tcp"),
                                 BURROW_S("yahoo.com:443"), &config, &err);
        if (BURROW_FAILED(err)) {
            testing_t_log_v(t, "Dial error:", err);
            continue;
        }
        /* Do a read to consume the NewSessionTicket messages. */
        (void)tls_conn_write(
            conn, slice_from((Byte *)(uintptr_t)get, get_len, get_len, TYPE_BYTE),
            &err);
        (void)tls_conn_read(conn, slice_from(buf, 4096, 4096, TYPE_BYTE), &err);
        (void)tls_conn_close(conn);
        tls_conn_free(conn);

        err = BURROW_NO_ERROR;
        conn = tls_dial(heap_allocator(), BURROW_S("tcp"), BURROW_S("yahoo.com:443"),
                        &config, &err);
        if (BURROW_FAILED(err)) {
            testing_t_log_v(t, "second Dial error:", err);
            continue;
        }
        TlsConnectionState state = tls_conn_connection_state(conn);
        did_resume = state.did_resume;
        (void)tls_conn_close(conn);
        tls_conn_free(conn);
    }

    tls_lru_client_session_cache_free(config.client_session_cache);
    tls_config_release(&config);
    if (!did_resume)
        testing_t_fatal_v(t, "no connection used session resumption");
}

/* changeImplConn is a net.Conn which can change its Write and Close methods. */
typedef struct ChangeImplConn ChangeImplConn;
struct ChangeImplConn {
    NetConn conn;
    Int (*write_func)(ChangeImplConn *w, Slice p, Error *err);
    Error (*close_func)(ChangeImplConn *w);

    /* Not in Go: what the funcs close over. */
    Chan *conn_closed;
    Chan *in_write;
};

static Int change_impl_read(void *self, Slice p, Error *err) {
    ChangeImplConn *w = (ChangeImplConn *)self;
    return w->conn.vt->reader.read(w->conn.data, p, err);
}

static Int change_impl_write(void *self, Slice p, Error *err) {
    ChangeImplConn *w = (ChangeImplConn *)self;
    if (w->write_func != NULL)
        return w->write_func(w, p, err);
    return w->conn.vt->writer.write(w->conn.data, p, err);
}

static Error change_impl_close(void *self) {
    ChangeImplConn *w = (ChangeImplConn *)self;
    if (w->close_func != NULL)
        return w->close_func(w);
    return w->conn.vt->closer.close(w->conn.data);
}

static NetAddr change_impl_local_addr(void *self) {
    ChangeImplConn *w = (ChangeImplConn *)self;
    return w->conn.vt->local_addr(w->conn.data);
}

static NetAddr change_impl_remote_addr(void *self) {
    ChangeImplConn *w = (ChangeImplConn *)self;
    return w->conn.vt->remote_addr(w->conn.data);
}

static Error change_impl_set_deadline(void *self, Time t) {
    ChangeImplConn *w = (ChangeImplConn *)self;
    return w->conn.vt->set_deadline(w->conn.data, t);
}

static Error change_impl_set_read_deadline(void *self, Time t) {
    ChangeImplConn *w = (ChangeImplConn *)self;
    return w->conn.vt->set_read_deadline(w->conn.data, t);
}

static Error change_impl_set_write_deadline(void *self, Time t) {
    ChangeImplConn *w = (ChangeImplConn *)self;
    return w->conn.vt->set_write_deadline(w->conn.data, t);
}

static const Type change_impl_type = {
    BURROW_S_INIT("changeImplConn"),
    BURROW_S_INIT("tls_test"),
    KIND_STRUCT,
    (uint32_t)sizeof(ChangeImplConn),
    (uint16_t)_Alignof(ChangeImplConn),
    0,
    0,
    NULL,
    NULL,
    NULL,
    NULL,
    0,
    0x63686963U, /* "chic" */
    NULL,
};

static const NetConnVT change_impl_vt = {
    {&change_impl_type, change_impl_read},
    {&change_impl_type, change_impl_write},
    {&change_impl_type, change_impl_close},
    change_impl_local_addr,
    change_impl_remote_addr,
    change_impl_set_deadline,
    change_impl_set_read_deadline,
    change_impl_set_write_deadline,
};

BURROW_SENTINEL_ERROR(err_conn_closed, "conn closed for test");

static Error conn_closed_close(ChangeImplConn *w) {
    chan_close(w->conn_closed);
    return BURROW_NO_ERROR;
}

static Int conn_closed_write(ChangeImplConn *w, Slice p, Error *err) {
    (void)p;
    bool yes = true;
    chan_send(w->in_write, &yes);
    (void)chan_recv(w->conn_closed, NULL);
    BURROW_OUT(err, err_conn_closed);
    return 0;
}

/* Not in Go: the goroutine that closes tconn while it writes. */
typedef struct CloseInWrite {
    TlsConn *tconn;
    Chan *in_write;
    Chan *close_returned;
} CloseInWrite;

static void close_in_write(void *env) {
    CloseInWrite *j = (CloseInWrite *)env;
    (void)chan_recv(j->in_write, NULL);
    (void)tls_conn_close(j->tconn); /* test that this doesn't block forever. */
    bool yes = true;
    chan_send(j->close_returned, &yes);
}

static void conn_close_breaking_write(TestingT *t, AcceptServer *job, SyncWaitGroup *wg,
                                      ChangeImplConn *conn, TlsConn *tconn) {
    Error err = tls_conn_handshake(tconn);
    if (BURROW_FAILED(err)) {
        testing_t_error_v(t, err);
        return;
    }

    sync_wait_group_wait(wg);
    if (!job->ok) {
        testing_t_error_v(t, job->serr);
        return;
    }

    Chan *in_write = chan_make(heap_allocator(), TYPE_BOOL, 1);
    Chan *close_returned = chan_make(heap_allocator(), TYPE_BOOL, 1);
    if (conn->conn_closed == NULL || in_write == NULL || close_returned == NULL) {
        chan_free(in_write);
        chan_free(close_returned);
        testing_t_error_v(t, "out of memory");
        return;
    }
    conn->close_func = conn_closed_close;

    conn->in_write = in_write;
    conn->write_func = conn_closed_write;

    CloseInWrite closer = {tconn, in_write, close_returned};
    SyncWaitGroup cwg = {0};
    sync_wait_group_go(&cwg, BURROW_FN(Func, close_in_write, &closer));

    err = BURROW_NO_ERROR;
    (void)tls_conn_write(tconn, slice_from((Byte *)(uintptr_t)"foo", 3, 3, TYPE_BYTE),
                         &err);
    if (!same_error(err, err_conn_closed))
        testing_t_errorf_v(t, "Write error = %v; want errConnClosed", err);

    (void)chan_recv(close_returned, NULL);
    err = tls_conn_close(tconn);
    if (!same_error(err, net_err_closed))
        testing_t_errorf_v(t, "Close error = %v; want net.ErrClosed", err);
    sync_wait_group_wait(&cwg);
    chan_free(in_write);
    chan_free(close_returned);
}

static void TestConnCloseBreakingWrite(TestingT *t) {
    need_tcp(t);
    NetListener ln = new_local_listener(t);
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    AcceptServer job;
    accept_server_init(&job, ln, a);
    SyncWaitGroup wg = {0};
    sync_wait_group_go(&wg, BURROW_FN(Func, accept_server, &job));

    Error err = BURROW_NO_ERROR;
    NetConn cconn =
        net_dial(heap_allocator(), BURROW_S("tcp"), listener_addr(a, ln), &err);
    if (BURROW_FAILED(err)) {
        testing_t_error_v(t, err);
        accept_server_end(&job, &wg, NULL);
        arena_free(&ar);
        return;
    }

    ChangeImplConn conn;
    memset(&conn, 0, sizeof conn);
    conn.conn = cconn;
    conn.conn_closed = chan_make(heap_allocator(), TYPE_BOOL, 0);

    TlsConfig client_config = test_config_client(a);
    TlsConn *tconn =
        tls_client(heap_allocator(), (NetConn){&change_impl_vt, &conn}, &client_config);
    if (tconn == NULL)
        testing_t_error_v(t, "out of memory");
    else
        conn_close_breaking_write(t, &job, &wg, &conn, tconn);

    tls_conn_free(tconn);
    net_conn_free(cconn);
    accept_server_end(&job, &wg, NULL);
    chan_free(conn.conn_closed);
    tls_config_release(&client_config);
    arena_free(&ar);
}

/* Not in Go: one side of TestConnCloseWrite. err is what the func returned,
 * kept in ar, and the job goes over err_chan once it is set. */
typedef struct CloseWriteSide {
    NetListener ln;
    Chan *client_done_chan;
    Chan *err_chan;
    Error err;
    Arena ar;
} CloseWriteSide;

/* Not in Go: data as a string, for %q. */
static Str read_text(Slice data) {
    Str s = {(const Byte *)data.p, data.len};
    return s;
}

/* Not in Go: srv.Close() and what comes after it in serverCloseWrite. */
static Error server_close_write_on(CloseWriteSide *j, TlsConn *srv, Alloc *a) {
    Error err = BURROW_NO_ERROR;
    Slice data = io_read_all(a, net_conn_as_io_reader(tls_conn_as_net_conn(srv)), &err);
    if (BURROW_FAILED(err))
        return err;
    if (data.len > 0)
        return fmt_errorf_v("Read data = %q; want nothing", read_text(data));

    err = tls_conn_close_write(srv);
    if (BURROW_FAILED(err))
        return fmt_errorf_v("server CloseWrite: %v", err);

    /* Wait for clientCloseWrite to finish, so we know we tested the CloseWrite
     * before we defer the sconn.Close above, which would also cause the client
     * to unblock like CloseWrite. */
    (void)chan_recv(j->client_done_chan, NULL);
    return BURROW_NO_ERROR;
}

static void server_close_write(void *env) {
    CloseWriteSide *j = (CloseWriteSide *)env;
    Alloc *a = arena_allocator(&j->ar);
    Error err = BURROW_NO_ERROR;
    NetConn sconn = j->ln.vt->accept(j->ln.data, &err);
    if (BURROW_FAILED(err)) {
        j->err = error_retain(a, fmt_errorf_v("accept: %v", err));
        CloseWriteSide *p = j;
        chan_send(j->err_chan, &p);
        return;
    }

    TlsConfig server_config = test_config_server(a);
    TlsConn *srv = tls_server(heap_allocator(), sconn, &server_config);
    Error ret = burrow_err_out_of_memory;
    if (srv != NULL) {
        err = tls_conn_handshake(srv);
        if (BURROW_FAILED(err)) {
            ret = fmt_errorf_v("handshake: %v", err);
        } else {
            ret = server_close_write_on(j, srv, a);
            (void)tls_conn_close(srv);
        }
    }
    j->err = error_retain(a, ret);
    tls_conn_free(srv);
    net_conn_free(sconn);
    tls_config_release(&server_config);
    CloseWriteSide *p = j;
    chan_send(j->err_chan, &p);
}

/* Not in Go: what clientCloseWrite does once it has a Conn. */
static Error client_close_write_on(TlsConn *conn, Alloc *a) {
    Error err = tls_conn_handshake(conn);
    if (BURROW_FAILED(err))
        return err;

    err = tls_conn_close_write(conn);
    if (BURROW_FAILED(err))
        return fmt_errorf_v("client CloseWrite: %v", err);

    Byte zero = 0;
    err = BURROW_NO_ERROR;
    (void)tls_conn_write(conn, slice_from(&zero, 1, 1, TYPE_BYTE), &err);
    if (!same_error(err, burrow__tls_err_shutdown))
        return fmt_errorf_v("CloseWrite error = %v; want errShutdown", err);

    err = BURROW_NO_ERROR;
    Slice data =
        io_read_all(a, net_conn_as_io_reader(tls_conn_as_net_conn(conn)), &err);
    if (BURROW_FAILED(err))
        return err;
    if (data.len > 0)
        return fmt_errorf_v("Read data = %q; want nothing", read_text(data));
    return BURROW_NO_ERROR;
}

static void client_close_write(void *env) {
    CloseWriteSide *j = (CloseWriteSide *)env;
    Alloc *a = arena_allocator(&j->ar);
    TlsConfig client_config = test_config_client(a);
    Error err = BURROW_NO_ERROR;
    TlsConn *conn = tls_dial(heap_allocator(), BURROW_S("tcp"), listener_addr(a, j->ln),
                             &client_config, &err);
    if (BURROW_FAILED(err)) {
        j->err = error_retain(a, err);
    } else {
        j->err = error_retain(a, client_close_write_on(conn, a));
        (void)tls_conn_close(conn);
        tls_conn_free(conn);
    }
    tls_config_release(&client_config);
    chan_close(j->client_done_chan);
    CloseWriteSide *p = j;
    chan_send(j->err_chan, &p);
}

static void TestConnCloseWrite(TestingT *t) {
    need_tcp(t);
    NetListener ln = new_local_listener(t);

    Chan *client_done_chan = chan_make(heap_allocator(), TYPE_BOOL, 0);
    Chan *err_chan = chan_make(heap_allocator(), TYPE_UNSAFE_POINTER, 2);
    if (client_done_chan == NULL || err_chan == NULL) {
        testing_t_fatal_v(t, "out of memory");
        return;
    }
    CloseWriteSide sides[2];
    memset(sides, 0, sizeof sides);
    for (int i = 0; i < 2; i++) {
        sides[i].ln = ln;
        sides[i].client_done_chan = client_done_chan;
        sides[i].err_chan = err_chan;
        arena_init(&sides[i].ar, NULL, 0);
    }

    SyncWaitGroup wg = {0};
    sync_wait_group_go(&wg, BURROW_FN(Func, server_close_write, &sides[0]));
    sync_wait_group_go(&wg, BURROW_FN(Func, client_close_write, &sides[1]));

    bool failed = false;
    for (int i = 0; i < 2 && !failed; i++) {
        Chan *after = time_after_chan(heap_allocator(), 10 * TIME_SECOND);
        if (after == NULL) {
            testing_t_fatal_v(t, "out of memory");
            return;
        }
        CloseWriteSide *side = NULL;
        SelectCase cases[] = {
            BURROW_RECV(err_chan, &side),
            BURROW_RECV(after, NULL),
        };
        Int chosen = chan_select(cases, 2);
        chan_free(after);
        if (chosen == 1) {
            testing_t_fatal_v(t, "deadlock");
            return;
        }
        if (BURROW_FAILED(side->err)) {
            testing_t_error_v(t, side->err);
            failed = true;
        }
    }

    (void)ln.vt->closer.close(ln.data);
    sync_wait_group_wait(&wg);
    net_listener_free(ln);
    for (int i = 0; i < 2; i++)
        arena_free(&sides[i].ar);
    chan_free(client_done_chan);
    chan_free(err_chan);
    if (failed)
        return;

    /* Also test CloseWrite being called before the handshake is finished: */
    {
        NetListener ln2 = new_local_listener(t);

        Arena ar;
        arena_init(&ar, NULL, 0);
        Alloc *a = arena_allocator(&ar);
        Error err = BURROW_NO_ERROR;
        NetConn net_conn =
            net_dial(heap_allocator(), BURROW_S("tcp"), listener_addr(a, ln2), &err);
        if (BURROW_FAILED(err)) {
            net_listener_free(ln2);
            arena_free(&ar);
            testing_t_fatal_v(t, err);
            return;
        }
        TlsConfig client_config = test_config_client(a);
        TlsConn *conn = tls_client(heap_allocator(), net_conn, &client_config);
        if (conn == NULL) {
            testing_t_error_v(t, "out of memory");
        } else {
            err = tls_conn_close_write(conn);
            if (!same_error(err, burrow__tls_err_early_close_write))
                testing_t_errorf_v(t, "CloseWrite error = %v; want errEarlyCloseWrite",
                                   err);
        }
        tls_conn_free(conn);
        net_conn_free(net_conn);
        net_listener_free(ln2);
        tls_config_release(&client_config);
        arena_free(&ar);
    }
}

/* Not in Go: the server of TestWarningAlertFlood. Its error is kept in ar. */
typedef struct AlertFloodServer {
    NetListener ln;
    Error err;
    Arena ar;
} AlertFloodServer;

static Error warning_alert_flood_on(TlsConn *srv, Alloc *a) {
    Error err = tls_conn_handshake(srv);
    if (BURROW_FAILED(err))
        return fmt_errorf_v("handshake: %v", err);

    err = BURROW_NO_ERROR;
    (void)io_read_all(a, net_conn_as_io_reader(tls_conn_as_net_conn(srv)), &err);
    Error ret = BURROW_NO_ERROR;
    Str expected = BURROW_S("too many ignored");
    if (!BURROW_FAILED(err))
        ret = errors_new(a, BURROW_S("unexpected lack of error from server"));
    else if (!strings_contains(error_text(err), expected))
        ret = fmt_errorf_v("expected error containing %q, but saw: %s", expected,
                           error_text(err));
    (void)tls_conn_close(srv);
    return ret;
}

static void warning_alert_flood_server(void *env) {
    AlertFloodServer *j = (AlertFloodServer *)env;
    Alloc *a = arena_allocator(&j->ar);
    Error err = BURROW_NO_ERROR;
    NetConn sconn = j->ln.vt->accept(j->ln.data, &err);
    if (BURROW_FAILED(err)) {
        j->err = error_retain(a, fmt_errorf_v("accept: %v", err));
        return;
    }

    TlsConfig server_config = test_config_server(a);
    TlsConn *srv = tls_server(heap_allocator(), sconn, &server_config);
    if (srv == NULL)
        j->err = burrow_err_out_of_memory;
    else
        j->err = error_retain(a, warning_alert_flood_on(srv, a));
    tls_conn_free(srv);
    net_conn_free(sconn);
    tls_config_release(&server_config);
}

static void TestWarningAlertFlood(TestingT *t) {
    need_tcp(t);
    NetListener ln = new_local_listener(t);

    AlertFloodServer server;
    memset(&server, 0, sizeof server);
    server.ln = ln;
    arena_init(&server.ar, NULL, 0);
    SyncWaitGroup wg = {0};
    sync_wait_group_go(&wg, BURROW_FN(Func, warning_alert_flood_server, &server));

    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    TlsConfig client_config = test_config_client(a);
    client_config.max_version =
        TLS_VERSION_TLS12; /* there are no warning alerts in TLS 1.3 */
    Error err = BURROW_NO_ERROR;
    TlsConn *conn = tls_dial(heap_allocator(), BURROW_S("tcp"), listener_addr(a, ln),
                             &client_config, &err);
    if (BURROW_FAILED(err)) {
        testing_t_error_v(t, err);
    } else {
        err = tls_conn_handshake(conn);
        if (BURROW_FAILED(err)) {
            testing_t_error_v(t, err);
        } else {
            for (int i = 0; i < TLS_MAX_USELESS_RECORDS + 1; i++)
                (void)burrow__tls_conn_send_alert(conn, TLS_ALERT_NO_RENEGOTIATION);
        }
    }

    if (BURROW_FAILED(err))
        (void)ln.vt->closer.close(ln.data);
    sync_wait_group_wait(&wg);
    if (!BURROW_FAILED(err) && BURROW_FAILED(server.err))
        testing_t_error_v(t, server.err);
    if (conn != NULL) {
        (void)tls_conn_close(conn);
        tls_conn_free(conn);
    }
    net_listener_free(ln);
    tls_config_release(&client_config);
    arena_free(&server.ar);
    arena_free(&ar);
}

/* Not in Go: the constants of TestConnectionState. */
static const char cs_alpn_protocol[] = "golang";
static const char cs_server_name[] = "test.golang.example";
static const char cs_sct1[] = "dummy sct 1";
static const char cs_sct2[] = "dummy sct 2";
static const char cs_ocsp[] = "dummy ocsp";

static Slice cs_bytes(const char *s) {
    Int n = (Int)strlen(s);
    return slice_from((Byte *)(uintptr_t)s, n, n, TYPE_BYTE);
}

/* Not in Go: what the checkConnectionState closure gets. */
typedef struct CheckConnState {
    const TlsConnectionState *cs;
    uint16_t version;
    bool is_client;
    Slice server_leaf, server_root, client_leaf, client_root;
} CheckConnState;

static Slice cert_raw(Slice certs, Int i) {
    return ((X509Certificate *const *)certs.p)[i]->raw;
}

static void check_connection_state(void *env, TestingT *t) {
    const CheckConnState *c = (const CheckConnState *)env;
    const TlsConnectionState *cs = c->cs;
    /* On the client, the peer is the server (testRSA2048Cert under
     * testRootCert); on the server, the peer is the client
     * (testClientRSA2048Cert under testClientRootCert). */
    Slice peer_leaf = c->server_leaf, peer_root = c->server_root;
    if (!c->is_client) {
        peer_leaf = c->client_leaf;
        peer_root = c->client_root;
    }
    if (cs->version != c->version)
        testing_t_errorf_v(t, "got Version %x, expected %x", cs->version, c->version);

    if (!cs->handshake_complete)
        testing_t_errorf_v(t, "got HandshakeComplete %v, expected true",
                           cs->handshake_complete);

    if (cs->did_resume)
        testing_t_errorf_v(t, "got DidResume %v, expected false", cs->did_resume);

    if (cs->cipher_suite == 0)
        testing_t_errorf_v(t, "got zero CipherSuite");

    if (cs->curve_id == 0)
        testing_t_errorf_v(t, "got zero CurveID");

    Str alpn = str_from_cstr(cs_alpn_protocol);
    if (!str_eq(cs->negotiated_protocol, alpn))
        testing_t_errorf_v(t, "got ALPN protocol %q, expected %q",
                           cs->negotiated_protocol, alpn);

    if (!cs->negotiated_protocol_is_mutual)
        testing_t_errorf_v(t, "got NegotiatedProtocolIsMutual %v, expected true",
                           cs->negotiated_protocol_is_mutual);

    Str server_name = str_from_cstr(cs_server_name);
    if (!str_eq(cs->server_name, server_name))
        testing_t_errorf_v(t, "got ServerName %q, expected %q", cs->server_name,
                           server_name);

    if (cs->peer_certificates.len != 1)
        testing_t_errorf_v(t, "got %d PeerCertificates, expected %d",
                           cs->peer_certificates.len, 1);
    else if (!bytes_equal(cert_raw(cs->peer_certificates, 0), peer_leaf))
        testing_t_errorf_v(t, "got PeerCertificates %x, expected %x",
                           cert_raw(cs->peer_certificates, 0), peer_leaf);

    const Slice *chains = (const Slice *)cs->verified_chains.p;
    if (cs->verified_chains.len != 1)
        testing_t_errorf_v(t, "got %d long verified chain, expected %d",
                           cs->verified_chains.len, 1);
    else if (chains[0].len != 2)
        testing_t_errorf_v(t, "got %d verified chain, expected %d", chains[0].len, 2);
    else if (!bytes_equal(cert_raw(chains[0], 0), peer_leaf))
        testing_t_errorf_v(t, "got verified chain[0][0] %x, expected %x",
                           cert_raw(chains[0], 0), peer_leaf);
    else if (!bytes_equal(cert_raw(chains[0], 1), peer_root))
        testing_t_errorf_v(t, "got verified chain[0][1] %x, expected %x",
                           cert_raw(chains[0], 1), peer_root);

    /* Only TLS 1.3 supports OCSP and SCTs on client certs. */
    const Slice *scts = (const Slice *)cs->signed_certificate_timestamps.p;
    Slice sct1 = cs_bytes(cs_sct1), sct2 = cs_bytes(cs_sct2), ocsp = cs_bytes(cs_ocsp);
    if (c->is_client || c->version == TLS_VERSION_TLS13) {
        if (cs->signed_certificate_timestamps.len != 2)
            testing_t_errorf_v(t, "got %d SCTs, expected %d",
                               cs->signed_certificate_timestamps.len, 2);
        else if (!bytes_equal(scts[0], sct1))
            testing_t_errorf_v(t, "got SCTs %x, expected %x", scts[0], sct1);
        else if (!bytes_equal(scts[1], sct2))
            testing_t_errorf_v(t, "got SCTs %x, expected %x", scts[1], sct2);
        if (!bytes_equal(cs->ocsp_response, ocsp))
            testing_t_errorf_v(t, "got OCSP %x, expected %x", cs->ocsp_response, ocsp);
    } else {
        if (!slice_is_nil(cs->signed_certificate_timestamps))
            testing_t_errorf_v(t, "got %d SCTs, expected nil",
                               cs->signed_certificate_timestamps.len);
        if (!slice_is_nil(cs->ocsp_response))
            testing_t_errorf_v(t, "got OCSP %x, expected nil", cs->ocsp_response);
    }

    if (c->version == TLS_VERSION_TLS13) {
        if (!slice_is_nil(cs->tls_unique))
            testing_t_errorf_v(t, "got TLSUnique %x, expected nil", cs->tls_unique);
    } else {
        if (slice_is_nil(cs->tls_unique))
            testing_t_errorf_v(t, "got nil TLSUnique");
    }
}

/* Not in Go: reflect.DeepEqual of two []*x509.Certificate, which here compares
 * the certificates by their DER. */
static bool same_certs(Slice a, Slice b) {
    if (a.len != b.len)
        return false;
    for (Int i = 0; i < a.len; i++)
        if (!bytes_equal(cert_raw(a, i), cert_raw(b, i)))
            return false;
    return true;
}

/* Not in Go: reflect.DeepEqual of two [][]byte or [][]*x509.Certificate. */
static bool same_slices(Slice a, Slice b, bool certs) {
    if (slice_is_nil(a) != slice_is_nil(b) || a.len != b.len)
        return false;
    for (Int i = 0; i < a.len; i++) {
        Slice x = ((const Slice *)a.p)[i], y = ((const Slice *)b.p)[i];
        if (certs ? !same_certs(x, y) : !bytes_equal(x, y))
            return false;
    }
    return true;
}

/* Not in Go: what the compareConnectionStates closure gets. */
typedef struct CompareConnStates {
    const TlsConnectionState *cs1, *cs2;
} CompareConnStates;

static void compare_connection_states(void *env, TestingT *t) {
    const CompareConnStates *c = (const CompareConnStates *)env;
    const TlsConnectionState *cs1 = c->cs1, *cs2 = c->cs2;
    if (cs1->version != cs2->version)
        testing_t_errorf_v(t, "Version mismatch: %x != %x", cs1->version, cs2->version);
    if (cs1->handshake_complete != cs2->handshake_complete)
        testing_t_errorf_v(t, "HandshakeComplete mismatch: %v != %v",
                           cs1->handshake_complete, cs2->handshake_complete);
    /* DidResume is expected to be different. */
    if (cs1->cipher_suite != cs2->cipher_suite)
        testing_t_errorf_v(t, "CipherSuite mismatch: %x != %x", cs1->cipher_suite,
                           cs2->cipher_suite);
    if (cs1->curve_id != cs2->curve_id) {
        Arena ar;
        arena_init(&ar, NULL, 0);
        Alloc *a = arena_allocator(&ar);
        testing_t_errorf_v(t, "CurveID mismatch: %s != %s",
                           tls_curve_id_string(cs1->curve_id, a),
                           tls_curve_id_string(cs2->curve_id, a));
        arena_free(&ar);
    }
    if (!str_eq(cs1->negotiated_protocol, cs2->negotiated_protocol))
        testing_t_errorf_v(t, "NegotiatedProtocol mismatch: %q != %q",
                           cs1->negotiated_protocol, cs2->negotiated_protocol);
    if (cs1->negotiated_protocol_is_mutual != cs2->negotiated_protocol_is_mutual)
        testing_t_errorf_v(t, "NegotiatedProtocolIsMutual mismatch: %v != %v",
                           cs1->negotiated_protocol_is_mutual,
                           cs2->negotiated_protocol_is_mutual);
    if (!str_eq(cs1->server_name, cs2->server_name))
        testing_t_errorf_v(t, "ServerName mismatch: %q != %q", cs1->server_name,
                           cs2->server_name);
    if (slice_is_nil(cs1->peer_certificates) != slice_is_nil(cs2->peer_certificates) ||
        !same_certs(cs1->peer_certificates, cs2->peer_certificates))
        testing_t_errorf_v(t, "PeerCertificates mismatch");
    if (!same_slices(cs1->verified_chains, cs2->verified_chains, true))
        testing_t_errorf_v(t, "VerifiedChains mismatch");
    if (!same_slices(cs1->signed_certificate_timestamps,
                     cs2->signed_certificate_timestamps, false))
        testing_t_errorf_v(t, "SignedCertificateTimestamps mismatch: %x != %x",
                           cs1->signed_certificate_timestamps,
                           cs2->signed_certificate_timestamps);
    if (!bytes_equal(cs1->ocsp_response, cs2->ocsp_response))
        testing_t_errorf_v(t, "OCSPResponse mismatch: %x != %x", cs1->ocsp_response,
                           cs2->ocsp_response);
    /* TLSUnique is expected to be different. */
}

/* Not in Go: what the Resume subtest of TestConnectionState gets. */
typedef struct ConnStateResume {
    TlsConfig *client_config, *server_config;
    const TlsConnectionState *ss, *cs;
} ConnStateResume;

static void connection_state_resume(void *env, TestingT *t) {
    const ConnStateResume *r = (const ConnStateResume *)env;
    /* TODO: test changing parameters between original and resumed connection
     * when the protocol allows it. */

    Handshake h;
    test_handshake(t, r->client_config, r->server_config, &h);
    if (BURROW_FAILED(h.err))
        testing_t_fatalf_v(t, "handshake failed: %v", h.err);

    if (!h.client_state.did_resume || !h.server_state.did_resume)
        testing_t_errorf_v(t, "DidResume is false");

    CompareConnStates client = {r->cs, &h.client_state};
    CompareConnStates server = {r->ss, &h.server_state};
    testing_t_run(t, BURROW_S("Client"),
                  BURROW_FN(TestingTFunc, compare_connection_states, &client));
    testing_t_run(t, BURROW_S("Server"),
                  BURROW_FN(TestingTFunc, compare_connection_states, &server));
    handshake_free(&h);
}

static void connection_state_version(void *env, TestingT *t) {
    uint16_t v = *(const uint16_t *)env;
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    Slice scts = slice_make(a, TYPE_BYTES, 2, 2);
    if (scts.p == NULL)
        testing_t_fatal_v(t, "out of memory");
    ((Slice *)scts.p)[0] = cs_bytes(cs_sct1);
    ((Slice *)scts.p)[1] = cs_bytes(cs_sct2);
    Slice ocsp = cs_bytes(cs_ocsp);

    TlsCertificate server_cert =
        parse_test_cert(a, test_rsa2048_cert_pem, test_rsa2048_key_pem);
    server_cert.signed_certificate_timestamps = scts;
    server_cert.ocsp_staple = ocsp;
    TlsCertificate client_cert =
        parse_test_cert(a, test_client_rsa2048_cert_pem, test_client_rsa2048_key_pem);
    client_cert.signed_certificate_timestamps = scts;
    client_cert.ocsp_staple = ocsp;
    TlsCertificate root = parse_test_cert(a, test_root_cert_pem, test_root_key_pem);
    TlsCertificate client_root =
        parse_test_cert(a, test_client_root_cert_pem, test_client_root_key_pem);
    X509CertPool *root_pool = x509_new_cert_pool(a);
    X509CertPool *client_root_pool = x509_new_cert_pool(a);
    if (root_pool == NULL || client_root_pool == NULL)
        testing_t_fatal_v(t, "out of memory");
    x509_cert_pool_add_cert(root_pool, root.leaf);
    x509_cert_pool_add_cert(client_root_pool, client_root.leaf);
    Str next_protos[1] = {str_from_cstr(cs_alpn_protocol)};

    TlsConfig server_config;
    memset(&server_config, 0, sizeof server_config);
    server_config.time = (TlsTimeFunc){test_time, NULL};
    server_config.certificates = slice_from(&server_cert, 1, 1, TYPE_TLS_CERTIFICATE);
    server_config.min_version = v;
    server_config.max_version = v;
    server_config.client_cas = client_root_pool;
    server_config.client_auth = TLS_REQUIRE_AND_VERIFY_CLIENT_CERT;
    server_config.next_protos = slice_from(next_protos, 1, 1, TYPE_STRING);
    TlsConfig client_config;
    memset(&client_config, 0, sizeof client_config);
    client_config.time = (TlsTimeFunc){test_time, NULL};
    client_config.certificates = slice_from(&client_cert, 1, 1, TYPE_TLS_CERTIFICATE);
    client_config.min_version = v;
    client_config.max_version = v;
    client_config.root_cas = root_pool;
    client_config.next_protos = slice_from(next_protos, 1, 1, TYPE_STRING);
    client_config.server_name = str_from_cstr(cs_server_name);
    client_config.client_session_cache =
        tls_new_lru_client_session_cache(heap_allocator(), 1);
    if (client_config.client_session_cache.vt == NULL)
        testing_t_fatal_v(t, "out of memory");

    Handshake h;
    test_handshake(t, &client_config, &server_config, &h);
    if (BURROW_FAILED(h.err))
        testing_t_fatalf_v(t, "handshake failed: %v", h.err);

    CheckConnState check = {
        &h.client_state,
        v,
        true,
        ((Slice *)server_cert.certificate.p)[0],
        ((Slice *)root.certificate.p)[0],
        ((Slice *)client_cert.certificate.p)[0],
        ((Slice *)client_root.certificate.p)[0],
    };
    testing_t_run(t, BURROW_S("Client"),
                  BURROW_FN(TestingTFunc, check_connection_state, &check));
    CheckConnState server_check = check;
    server_check.cs = &h.server_state;
    server_check.is_client = false;
    testing_t_run(t, BURROW_S("Server"),
                  BURROW_FN(TestingTFunc, check_connection_state, &server_check));

    ConnStateResume resume = {&client_config, &server_config, &h.server_state,
                              &h.client_state};
    testing_t_run(t, BURROW_S("Resume"),
                  BURROW_FN(TestingTFunc, connection_state_resume, &resume));

    handshake_free(&h);
    tls_lru_client_session_cache_free(client_config.client_session_cache);
    tls_config_release(&server_config);
    tls_config_release(&client_config);
    arena_free(&ar);
}

static const uint16_t connection_state_versions[] = {
    TLS_VERSION_TLS10, TLS_VERSION_TLS12, TLS_VERSION_TLS13};
static const char *const connection_state_names[] = {"TLSv10", "TLSv12", "TLSv13"};

static void TestConnectionState(TestingT *t) {
    for (int i = 0; i < 3; i++)
        testing_t_run(t, str_from_cstr(connection_state_names[i]),
                      BURROW_FN(TestingTFunc, connection_state_version,
                                (void *)(uintptr_t)&connection_state_versions[i]));
}

/* Not in Go: brokenSigner, a crypto.Signer that replaces opts with
 * opts.HashFunc(), so rsa.PSSOptions are discarded. */
typedef struct BrokenSigner {
    CryptoSigner signer;
} BrokenSigner;

static CryptoPublicKey broken_signer_public(void *self) {
    return crypto_signer_public(((BrokenSigner *)self)->signer);
}

static Slice broken_signer_sign(void *self, Alloc *a, IoReader rand, Slice digest,
                                CryptoSignerOpts opts, Error *err) {
    /* Replace opts with opts.HashFunc(), so rsa.PSSOptions are discarded. */
    CryptoHash h = opts.vt->hash_func(opts.data);
    return crypto_signer_sign(((BrokenSigner *)self)->signer, a, rand, digest,
                              crypto_hash_as_signer_opts(&h), err);
}

static const CryptoSignerVT broken_signer_vt = {NULL, broken_signer_public,
                                                broken_signer_sign};

static const TlsSignatureScheme pkcs1_only_schemes[] = {TLS_PKCS1_WITH_SHA1,
                                                        TLS_PKCS1_WITH_SHA256};

/* TestPKCS1OnlyCert uses a client certificate with a broken crypto.Signer that
 * always makes PKCS #1 v1.5 signatures, so can't be used with RSA-PSS. */
static void TestPKCS1OnlyCert(TestingT *t) {
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    TlsCertificate rsa =
        parse_test_cert(a, test_client_rsa2048_cert_pem, test_client_rsa2048_key_pem);
    BrokenSigner broken = {rsa_private_key_signer(rsa.private_key.data)};
    CryptoSigner signer = {&broken_signer_vt, &broken};
    TlsCertificate cert;
    memset(&cert, 0, sizeof cert);
    cert.certificate = rsa.certificate;
    cert.private_key = BURROW_ANY(TYPE_CRYPTO_SIGNER, &signer);
    TlsConfig client_config = test_config_client(a);
    client_config.certificates = slice_from(&cert, 1, 1, TYPE_TLS_CERTIFICATE);
    TlsConfig server_config = test_config_server(a);
    server_config.max_version = TLS_VERSION_TLS12; /* TLS 1.3 doesn't support
                                                      PKCS #1 v1.5 */
    server_config.client_auth = TLS_REQUIRE_ANY_CLIENT_CERT;

    /* If RSA-PSS is selected, the handshake should fail. */
    Handshake h;
    test_handshake(t, &client_config, &server_config, &h);
    if (!BURROW_FAILED(h.err))
        testing_t_fatal_v(t, "expected broken certificate to cause connection to fail");
    handshake_free(&h);

    cert.supported_signature_algorithms = slice_from(
        (TlsSignatureScheme *)(uintptr_t)pkcs1_only_schemes, 2, 2, TYPE_UINT16);

    /* But if the certificate restricts supported algorithms, RSA-PSS should not
     * be selected, and the handshake should succeed. */
    test_handshake(t, &client_config, &server_config, &h);
    if (BURROW_FAILED(h.err))
        testing_t_error_v(t, h.err);
    handshake_free(&h);

    tls_config_release(&server_config);
    tls_config_release(&client_config);
    arena_free(&ar);
}

/* Not in Go: the goroutine of TestEarlyLargeCertMsg. */
static void early_large_cert_job(void *env) {
    NetConn *client = (NetConn *)env;
    Byte msg[9] = {TLS_RECORD_TYPE_HANDSHAKE, 3, 4,   0,  4,
                   TLS_TYPE_CERTIFICATE,      1, 255, 255};
    Error err = BURROW_NO_ERROR;
    (void)client->vt->writer.write(client->data, slice_from(msg, 9, 9, TYPE_BYTE),
                                   &err);
    (void)err;
}

static void TestEarlyLargeCertMsg(TestingT *t) {
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    NetConn client, server;
    local_pipe(a, &client, &server);

    SyncWaitGroup wg = {0};
    sync_wait_group_go(&wg, BURROW_FN(Func, early_large_cert_job, &client));

    Str expected_err = BURROW_S("tls: handshake message of length 131071 bytes exceeds "
                                "maximum of 65536 bytes");
    TlsConfig server_config = test_config_server(a);
    TlsConn *serv_conn = tls_server(heap_allocator(), server, &server_config);
    if (serv_conn == NULL)
        testing_t_fatal_v(t, "out of memory");
    Error err = tls_conn_handshake(serv_conn);
    sync_wait_group_wait(&wg);
    if (!BURROW_FAILED(err))
        testing_t_fatal_v(t, "unexpected success");
    if (!str_eq(error_text(err), expected_err))
        testing_t_fatalf_v(t, "unexpected error: got %q, want %q", error_text(err),
                           expected_err);

    tls_conn_free(serv_conn);
    local_pipe_free(client);
    tls_config_release(&server_config);
    arena_free(&ar);
}

static void TestLargeCertMsg(TestingT *t) {
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    Error err = BURROW_NO_ERROR;
    EcdsaPrivateKey *k = ecdsa_generate_key(a, elliptic_p256(), (IoReader){0}, &err);
    if (BURROW_FAILED(err))
        testing_t_fatal_v(t, err);
    Int oid[3] = {1, 2, 3};
    /* Ballast to inflate the certificate beyond the regular handshake record
     * size. */
    Slice ballast = slice_make(a, TYPE_BYTE, 65536, 65536);
    if (ballast.p == NULL)
        testing_t_fatal_v(t, "out of memory");
    PkixExtension ext = {slice_from(oid, 3, 3, TYPE_INT), false, ballast};
    X509Certificate tmpl;
    memset(&tmpl, 0, sizeof tmpl);
    tmpl.serial_number = big_new_int(a, 1);
    tmpl.subject.common_name = BURROW_S("test");
    tmpl.extra_extensions = slice_from(&ext, 1, 1, TYPE_PKIX_EXTENSION);
    Slice cert = x509_create_certificate(a, (IoReader){0}, &tmpl, &tmpl,
                                         ecdsa_private_key_public(k),
                                         ecdsa_private_key_signer(k), &err);
    if (BURROW_FAILED(err))
        testing_t_fatal_v(t, err);

    TlsConfig client_config = test_config_client(a);
    TlsConfig server_config = test_config_server(a);
    client_config.insecure_skip_verify = true;
    TlsCertificate c;
    memset(&c, 0, sizeof c);
    c.certificate = slice_from(&cert, 1, 1, TYPE_BYTES);
    c.private_key = BURROW_ANY(TYPE_ECDSA_PRIVATE_KEY, k);
    server_config.certificates = slice_from(&c, 1, 1, TYPE_TLS_CERTIFICATE);
    Handshake h;
    test_handshake(t, &client_config, &server_config, &h);
    if (BURROW_FAILED(h.err))
        testing_t_fatalf_v(t, "unexpected failure: %s", error_text(h.err));
    handshake_free(&h);

    tls_config_release(&server_config);
    tls_config_release(&client_config);
    arena_free(&ar);
}

/* Not in Go: messageOnlySigner, a crypto.Signer whose Sign fails, found to be
 * a crypto.MessageSigner through the SignMessage in its type's method set. */
#define MESSAGE_ONLY_SIGNER_FIELDS(F, T) F(T, CryptoSigner, Signer, "")

BURROW_STRUCT_DECL(MessageOnlySigner, MESSAGE_ONLY_SIGNER_FIELDS);

static Slice message_only_signer_sign_message(MessageOnlySigner *s, CryptoAllocArg a,
                                              IoReader rand, Bytes msg,
                                              CryptoSignerOpts opts,
                                              CryptoErrorArg err) {
    Hash h = crypto_hash_new(opts.vt->hash_func(opts.data), a);
    if (h.vt == NULL) {
        BURROW_OUT(err, burrow_err_out_of_memory);
        return slice_nil(TYPE_BYTE);
    }
    (void)hash_write(h, msg, NULL);
    Slice digest = hash_sum(a, h, slice_nil(TYPE_BYTE));
    return crypto_signer_sign(s->Signer, a, rand, digest, opts, err);
}

#define MESSAGE_ONLY_SIGNER_METHODS(M, T)                                              \
    M(T, SignMessage, message_only_signer_sign_message, CRYPTO_SIG_SIGN_MESSAGE)

BURROW_STRUCT_DEFINE_METHODS(MessageOnlySigner, MESSAGE_ONLY_SIGNER_FIELDS,
                             MESSAGE_ONLY_SIGNER_METHODS);

static CryptoPublicKey message_only_signer_public(void *self) {
    return crypto_signer_public(((MessageOnlySigner *)self)->Signer);
}

static Slice message_only_signer_sign(void *self, Alloc *a, IoReader rand, Slice msg,
                                      CryptoSignerOpts opts, Error *err) {
    (void)self;
    (void)a;
    (void)rand;
    (void)msg;
    (void)opts;
    BURROW_OUT(
        err, errors_new(error_allocator(), BURROW_S("messageOnlySigner: Sign called")));
    return slice_nil(TYPE_BYTE);
}

static const CryptoSignerVT message_only_signer_vt = {
    TYPE_OF(MessageOnlySigner), message_only_signer_public, message_only_signer_sign};

/* Not in Go: a Certificate with the chain of c and messageOnlySigner{key}, the
 * signer s, which has to outlive it. */
static TlsCertificate message_only_cert(const TlsCertificate *c, MessageOnlySigner *s,
                                        CryptoSigner *signer, CryptoSigner key) {
    s->Signer = key;
    signer->vt = &message_only_signer_vt;
    signer->data = s;
    TlsCertificate out;
    memset(&out, 0, sizeof out);
    out.certificate = c->certificate;
    out.private_key = BURROW_ANY(TYPE_CRYPTO_SIGNER, signer);
    return out;
}

static void test_message_signer(TestingT *t, uint16_t version) {
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    TlsConfig client_config = test_config_client(a);
    TlsConfig server_config = test_config_server(a);
    server_config.client_auth = TLS_REQUIRE_ANY_CLIENT_CERT;
    client_config.min_version = version;
    client_config.max_version = version;
    server_config.min_version = version;
    server_config.max_version = version;
    TlsCertificate client_rsa =
        parse_test_cert(a, test_client_rsa2048_cert_pem, test_client_rsa2048_key_pem);
    TlsCertificate server_rsa =
        parse_test_cert(a, test_rsa2048_cert_pem, test_rsa2048_key_pem);
    MessageOnlySigner client_signer, server_signer;
    CryptoSigner client_key, server_key;
    TlsCertificate client_cert =
        message_only_cert(&client_rsa, &client_signer, &client_key,
                          rsa_private_key_signer(client_rsa.private_key.data));
    TlsCertificate server_cert =
        message_only_cert(&server_rsa, &server_signer, &server_key,
                          rsa_private_key_signer(server_rsa.private_key.data));
    client_config.certificates = slice_from(&client_cert, 1, 1, TYPE_TLS_CERTIFICATE);
    server_config.certificates = slice_from(&server_cert, 1, 1, TYPE_TLS_CERTIFICATE);

    Handshake h;
    test_handshake(t, &client_config, &server_config, &h);
    if (version < TLS_VERSION_TLS12) {
        if (!BURROW_FAILED(h.err))
            testing_t_fatal_v(t, "expected failure for TLS 1.0/1.1");
    } else {
        if (BURROW_FAILED(h.err))
            testing_t_fatalf_v(t, "unexpected failure: %s", error_text(h.err));
    }
    handshake_free(&h);

    TlsCertificate client_ecdsa = parse_test_cert(a, test_client_ecdsap256_cert_pem,
                                                  test_client_ecdsap256_key_pem);
    TlsCertificate server_ecdsa =
        parse_test_cert(a, test_ecdsap256_cert_pem, test_ecdsap256_key_pem);
    client_cert =
        message_only_cert(&client_ecdsa, &client_signer, &client_key,
                          ecdsa_private_key_signer(client_ecdsa.private_key.data));
    server_cert =
        message_only_cert(&server_ecdsa, &server_signer, &server_key,
                          ecdsa_private_key_signer(server_ecdsa.private_key.data));

    test_handshake(t, &client_config, &server_config, &h);
    if (version < TLS_VERSION_TLS12) {
        if (!BURROW_FAILED(h.err))
            testing_t_fatal_v(t, "expected failure for TLS 1.0/1.1");
    } else {
        if (BURROW_FAILED(h.err))
            testing_t_fatalf_v(t, "unexpected failure: %s", error_text(h.err));
    }
    handshake_free(&h);

    tls_config_release(&server_config);
    tls_config_release(&client_config);
    arena_free(&ar);
}

static void message_signer_tls10(void *env, TestingT *t) {
    (void)env;
    test_message_signer(t, TLS_VERSION_TLS10);
}

static void message_signer_tls12(void *env, TestingT *t) {
    (void)env;
    test_message_signer(t, TLS_VERSION_TLS12);
}

static void message_signer_tls13(void *env, TestingT *t) {
    (void)env;
    test_message_signer(t, TLS_VERSION_TLS13);
}

static void TestMessageSigner(TestingT *t) {
    testing_t_run(t, BURROW_S("TLSv10"),
                  BURROW_FN(TestingTFunc, message_signer_tls10, NULL));
    testing_t_run(t, BURROW_S("TLSv12"),
                  BURROW_FN(TestingTFunc, message_signer_tls12, NULL));
    testing_t_run(t, BURROW_S("TLSv13"),
                  BURROW_FN(TestingTFunc, message_signer_tls13, NULL));
}

/* bitFlippingSigner wraps a crypto.Signer and flips a bit in the signature,
 * producing an invalid signature. */
typedef struct BitFlippingSigner {
    CryptoSigner signer;
} BitFlippingSigner;

static CryptoPublicKey bit_flipping_signer_public(void *self) {
    return crypto_signer_public(((BitFlippingSigner *)self)->signer);
}

static Slice bit_flipping_signer_sign(void *self, Alloc *a, IoReader rand, Slice digest,
                                      CryptoSignerOpts opts, Error *err) {
    Error e = BURROW_NO_ERROR;
    Slice sig = crypto_signer_sign(((BitFlippingSigner *)self)->signer, a, rand, digest,
                                   opts, &e);
    BURROW_OUT(err, e);
    if (BURROW_FAILED(e) || sig.len == 0)
        return slice_nil(TYPE_BYTE);
    ((Byte *)sig.p)[0] ^= 1;
    return sig;
}

static const CryptoSignerVT bit_flipping_signer_vt = {NULL, bit_flipping_signer_public,
                                                      bit_flipping_signer_sign};

/* Not in Go: the client goroutine of testInvalidSignatureHandshake. Its error
 * is kept in ar. */
typedef struct InvalidSigClient {
    NetConn c;
    TlsConfig *config;
    TlsConn *conn;
    Error err;
    Arena ar;
} InvalidSigClient;

static void invalid_sig_client(void *env) {
    InvalidSigClient *j = (InvalidSigClient *)env;
    j->conn = tls_client(heap_allocator(), j->c, j->config);
    if (j->conn == NULL)
        j->err = burrow_err_out_of_memory;
    else
        j->err = error_retain(arena_allocator(&j->ar), tls_conn_handshake(j->conn));
    (void)j->c.vt->closer.close(j->c.data);
}

/* testInvalidSignatureHandshake performs a TLS handshake and returns the
 * errors from both client and server. Unlike testHandshake, it doesn't try to
 * exchange data after the handshake.
 *
 * Not in Go: the errors are made in a. */
static void test_invalid_signature_handshake(Alloc *a, TlsConfig *client_config,
                                             TlsConfig *server_config,
                                             Error *client_err, Error *server_err) {
    NetConn c, s;
    local_pipe(a, &c, &s);
    InvalidSigClient j;
    memset(&j, 0, sizeof j);
    j.c = c;
    j.config = client_config;
    arena_init(&j.ar, NULL, 0);
    SyncWaitGroup wg = {0};
    sync_wait_group_go(&wg, BURROW_FN(Func, invalid_sig_client, &j));
    TlsConn *srv = tls_server(heap_allocator(), s, server_config);
    *server_err = burrow_err_out_of_memory;
    if (srv != NULL)
        *server_err = error_retain(a, tls_conn_handshake(srv));
    (void)s.vt->closer.close(s.data);
    sync_wait_group_wait(&wg);
    *client_err = error_retain(a, j.err);
    tls_conn_free(srv);
    tls_conn_free(j.conn);
    local_pipe_free(c);
    arena_free(&j.ar);
}

/* Not in Go: what the subtests of testInvalidHandshakeSignature share. */
typedef struct InvalidSig {
    TlsConfig *server_config, *client_config;
} InvalidSig;

static bool invalid_signature(Error err) {
    return strings_contains(error_text(err), BURROW_S("invalid signature"));
}

/* Test that the server rejects invalid client certificate signatures, even
 * when RequireAnyClientCert is used. */
static void invalid_sig_client_signature(void *env, TestingT *t) {
    const InvalidSig *e = (const InvalidSig *)env;
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    TlsConfig *server_config = tls_config_clone(e->server_config, a);
    TlsConfig *client_config = tls_config_clone(e->client_config, a);
    if (server_config == NULL || client_config == NULL)
        testing_t_fatal_v(t, "out of memory");
    server_config->client_auth = TLS_REQUIRE_ANY_CLIENT_CERT;
    TlsCertificate ecdsa = parse_test_cert(a, test_client_ecdsap256_cert_pem,
                                           test_client_ecdsap256_key_pem);
    BitFlippingSigner flip = {ecdsa_private_key_signer(ecdsa.private_key.data)};
    CryptoSigner signer = {&bit_flipping_signer_vt, &flip};
    TlsCertificate cert;
    memset(&cert, 0, sizeof cert);
    cert.certificate = ecdsa.certificate;
    cert.private_key = BURROW_ANY(TYPE_CRYPTO_SIGNER, &signer);
    client_config->certificates = slice_from(&cert, 1, 1, TYPE_TLS_CERTIFICATE);

    Error client_err, server_err;
    test_invalid_signature_handshake(a, client_config, server_config, &client_err,
                                     &server_err);
    if (!BURROW_FAILED(server_err))
        testing_t_fatalf_v(
            t, "expected server to reject invalid client signature; client err = %v",
            client_err);
    if (!invalid_signature(server_err))
        testing_t_errorf_v(t, "expected 'invalid signature' error, got: %v",
                           server_err);
    tls_config_release(server_config);
    tls_config_release(client_config);
    arena_free(&ar);
}

/* Not in Go: the two ServerSignature subtests, with insecure saying whether
 * the client sets InsecureSkipVerify. */
static void invalid_sig_server_signature(TestingT *t, const InvalidSig *e,
                                         bool insecure) {
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    TlsConfig *client_config = tls_config_clone(e->client_config, a);
    TlsConfig *server_config = tls_config_clone(e->server_config, a);
    if (server_config == NULL || client_config == NULL)
        testing_t_fatal_v(t, "out of memory");
    if (insecure)
        client_config->insecure_skip_verify = true;
    TlsCertificate rsa =
        parse_test_cert(a, test_rsa2048_cert_pem, test_rsa2048_key_pem);
    BitFlippingSigner flip = {rsa_private_key_signer(rsa.private_key.data)};
    CryptoSigner signer = {&bit_flipping_signer_vt, &flip};
    TlsCertificate cert;
    memset(&cert, 0, sizeof cert);
    cert.certificate = rsa.certificate;
    cert.private_key = BURROW_ANY(TYPE_CRYPTO_SIGNER, &signer);
    server_config->certificates = slice_from(&cert, 1, 1, TYPE_TLS_CERTIFICATE);

    Error client_err, server_err;
    test_invalid_signature_handshake(a, client_config, server_config, &client_err,
                                     &server_err);
    if (!BURROW_FAILED(client_err)) {
        if (insecure)
            testing_t_fatalf_v(t,
                               "expected client to reject invalid server signature "
                               "despite InsecureSkipVerify; server err = %v",
                               server_err);
        testing_t_fatalf_v(
            t, "expected client to reject invalid server signature; server err = %v",
            server_err);
    }
    if (!invalid_signature(client_err))
        testing_t_errorf_v(t, "expected 'invalid signature' error, got: %v",
                           client_err);
    tls_config_release(server_config);
    tls_config_release(client_config);
    arena_free(&ar);
}

/* Test that the client rejects invalid server certificate signatures. */
static void invalid_sig_server(void *env, TestingT *t) {
    invalid_sig_server_signature(t, (const InvalidSig *)env, false);
}

/* Test that InsecureSkipVerify doesn't disable server signature
 * verification. */
static void invalid_sig_server_insecure(void *env, TestingT *t) {
    invalid_sig_server_signature(t, (const InvalidSig *)env, true);
}

static void test_invalid_handshake_signature(TestingT *t, uint16_t version) {
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    TlsConfig server_config = test_config_server(a);
    server_config.max_version = version;
    server_config.min_version = version;
    server_config.session_tickets_disabled = true;
    TlsConfig client_config = test_config_client(a);
    client_config.max_version = version;
    client_config.min_version = version;

    InvalidSig e = {&server_config, &client_config};
    testing_t_run(t, BURROW_S("ClientSignature"),
                  BURROW_FN(TestingTFunc, invalid_sig_client_signature, &e));
    testing_t_run(t, BURROW_S("ServerSignature"),
                  BURROW_FN(TestingTFunc, invalid_sig_server, &e));
    testing_t_run(t, BURROW_S("ServerSignature/InsecureSkipVerify"),
                  BURROW_FN(TestingTFunc, invalid_sig_server_insecure, &e));

    tls_config_release(&server_config);
    tls_config_release(&client_config);
    arena_free(&ar);
}

static void invalid_handshake_signature_tls10(void *env, TestingT *t) {
    (void)env;
    test_invalid_handshake_signature(t, TLS_VERSION_TLS10);
}

static void invalid_handshake_signature_tls12(void *env, TestingT *t) {
    (void)env;
    test_invalid_handshake_signature(t, TLS_VERSION_TLS12);
}

static void invalid_handshake_signature_tls13(void *env, TestingT *t) {
    (void)env;
    test_invalid_handshake_signature(t, TLS_VERSION_TLS13);
}

/* TestInvalidHandshakeSignature tests that invalid handshake signatures are
 * rejected for all TLS versions, for both server and client certificates,
 * even when InsecureSkipVerify or RequireAnyClientCert are used (which disable
 * certificate chain verification, but not signature verification). */
static void TestInvalidHandshakeSignature(TestingT *t) {
    testing_t_run(t, BURROW_S("TLSv10"),
                  BURROW_FN(TestingTFunc, invalid_handshake_signature_tls10, NULL));
    testing_t_run(t, BURROW_S("TLSv12"),
                  BURROW_FN(TestingTFunc, invalid_handshake_signature_tls12, NULL));
    testing_t_run(t, BURROW_S("TLSv13"),
                  BURROW_FN(TestingTFunc, invalid_handshake_signature_tls13, NULL));
}

/* Not in Go: what the GetCertificate and GetClientCertificate closures of
 * testLocalCertificate and testLocalCertificateResumption return. */
typedef struct LocalCertCallbacks {
    TlsCertificate server, client;
} LocalCertCallbacks;

static TlsCertificate *local_cert_get_certificate(void *env, TlsClientHelloInfo *chi,
                                                  Error *err) {
    (void)chi;
    BURROW_OUT(err, BURROW_NO_ERROR);
    return &((LocalCertCallbacks *)env)->server;
}

static TlsCertificate *local_cert_get_client_certificate(void *env,
                                                         TlsCertificateRequestInfo *cri,
                                                         Error *err) {
    (void)cri;
    BURROW_OUT(err, BURROW_NO_ERROR);
    return &((LocalCertCallbacks *)env)->client;
}

/* Not in Go: slices.EqualFunc(a, b, bytes.Equal) for two [][]byte. */
static bool same_byte_slices(Slice a, Slice b) {
    if (a.len != b.len)
        return false;
    for (Int i = 0; i < a.len; i++)
        if (!bytes_equal(((const Slice *)a.p)[i], ((const Slice *)b.p)[i]))
            return false;
    return true;
}

/* Not in Go: the configurations both tests start from, with the callbacks set
 * up in cb when callback is set. */
static void local_cert_configs(Alloc *a, uint16_t version, bool callback,
                               LocalCertCallbacks *cb, TlsConfig *client_config,
                               TlsConfig *server_config, TlsCertificate *server_cert,
                               TlsCertificate *client_cert) {
    *client_config = test_config_client(a);
    *server_config = test_config_server(a);

    client_config->min_version = server_config->min_version = version;
    client_config->max_version = server_config->max_version = version;
    server_config->client_auth = TLS_REQUEST_CLIENT_CERT;

    *server_cert = ((const TlsCertificate *)server_config->certificates.p)[0];
    *client_cert = ((const TlsCertificate *)client_config->certificates.p)[0];

    if (callback) {
        memset(cb, 0, sizeof *cb);
        cb->client.certificate = client_cert->certificate;
        cb->client.private_key = client_cert->private_key;
        client_config->get_client_certificate = BURROW_FN(
            TlsGetClientCertificateFunc, local_cert_get_client_certificate, cb);
        client_config->certificates = slice_nil(TYPE_TLS_CERTIFICATE);
        cb->server.certificate = server_cert->certificate;
        cb->server.private_key = server_cert->private_key;
        server_config->get_certificate =
            BURROW_FN(TlsGetCertificateFunc, local_cert_get_certificate, cb);
        server_config->certificates = slice_nil(TYPE_TLS_CERTIFICATE);
    }
}

static void test_local_certificate(TestingT *t, uint16_t version, bool callback) {
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    LocalCertCallbacks cb;
    TlsConfig client_config, server_config;
    TlsCertificate server_cert, client_cert;
    local_cert_configs(a, version, callback, &cb, &client_config, &server_config,
                       &server_cert, &client_cert);

    Handshake h;
    test_handshake(t, &client_config, &server_config, &h);
    if (BURROW_FAILED(h.err))
        testing_t_fatalf_v(t, "unexpected failure: %s", error_text(h.err));

    if (!same_byte_slices(h.server_state.local_certificate, server_cert.certificate))
        testing_t_errorf_v(t, "unexpected server local certificate: %x, want %x",
                           h.server_state.local_certificate, server_cert.certificate);

    if (!same_byte_slices(h.client_state.local_certificate, client_cert.certificate))
        testing_t_errorf_v(t, "unexpected client local certificate: %x, want %x",
                           h.client_state.local_certificate, client_cert.certificate);
    handshake_free(&h);

    tls_config_release(&server_config);
    tls_config_release(&client_config);
    arena_free(&ar);
}

/* Not in Go: what the subtests of TestLocalCertificate and
 * TestLocalCertificateResumption get. */
typedef struct LocalCertTest {
    uint16_t version;
    bool callback;
    void (*test)(TestingT *t, uint16_t version, bool callback);
} LocalCertTest;

static void local_cert_version(void *env, TestingT *t) {
    const LocalCertTest *lt = (const LocalCertTest *)env;
    lt->test(t, lt->version, lt->callback);
}

static const uint16_t local_cert_versions[] = {TLS_VERSION_TLS10, TLS_VERSION_TLS11,
                                               TLS_VERSION_TLS12, TLS_VERSION_TLS13};

static void local_cert_callback(void *env, TestingT *t) {
    const LocalCertTest *base = (const LocalCertTest *)env;
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    for (int i = 0; i < 4; i++) {
        LocalCertTest lt = *base;
        lt.version = local_cert_versions[i];
        testing_t_run(t, tls_version_name(a, lt.version),
                      BURROW_FN(TestingTFunc, local_cert_version, &lt));
    }
    arena_free(&ar);
}

/* Not in Go: the loop both tests have over useCallback. */
static void local_cert_tests(TestingT *t, void (*test)(TestingT *t, uint16_t version,
                                                       bool callback)) {
    static const bool use_callbacks[] = {true, false};
    for (int i = 0; i < 2; i++) {
        Str name = BURROW_S("UseCertificates");
        if (use_callbacks[i])
            name = BURROW_S("UseCallbacks");
        LocalCertTest lt = {0, use_callbacks[i], test};
        testing_t_run(t, name, BURROW_FN(TestingTFunc, local_cert_callback, &lt));
    }
}

static void TestLocalCertificate(TestingT *t) {
    local_cert_tests(t, test_local_certificate);
}

static void test_local_certificate_resumption(TestingT *t, uint16_t version,
                                              bool callback) {
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    LocalCertCallbacks cb;
    TlsConfig client_config, server_config;
    TlsCertificate server_cert, client_cert;
    local_cert_configs(a, version, callback, &cb, &client_config, &server_config,
                       &server_cert, &client_cert);
    client_config.client_session_cache =
        tls_new_lru_client_session_cache(heap_allocator(), 1);
    if (client_config.client_session_cache.vt == NULL)
        testing_t_fatal_v(t, "out of memory");

    Handshake h;
    test_handshake(t, &client_config, &server_config, &h);
    if (BURROW_FAILED(h.err))
        testing_t_fatalf_v(t, "first handshake failed: %s", error_text(h.err));
    handshake_free(&h);

    test_handshake(t, &client_config, &server_config, &h);
    if (BURROW_FAILED(h.err))
        testing_t_fatalf_v(t, "second handshake failed: %s", error_text(h.err));
    if (!h.server_state.did_resume || !h.client_state.did_resume)
        testing_t_fatalf_v(t, "second handshake did not resume (server=%v client=%v)",
                           h.server_state.did_resume, h.client_state.did_resume);
    if (!slice_is_nil(h.server_state.local_certificate))
        testing_t_errorf_v(t,
                           "server LocalCertificate on resumed connection: got %x, "
                           "want nil",
                           h.server_state.local_certificate);
    if (!slice_is_nil(h.client_state.local_certificate))
        testing_t_errorf_v(t,
                           "client LocalCertificate on resumed connection: got %x, "
                           "want nil",
                           h.client_state.local_certificate);
    handshake_free(&h);

    tls_lru_client_session_cache_free(client_config.client_session_cache);
    tls_config_release(&server_config);
    tls_config_release(&client_config);
    arena_free(&ar);
}

static void TestLocalCertificateResumption(TestingT *t) {
    local_cert_tests(t, test_local_certificate_resumption);
}

/* Not in Go: a case of testVerifyCertificates. */
typedef struct VerifyCertsTest {
    const char *name;

    bool insecure_skip_verify;
    TlsClientAuthType client_auth;
    bool client_certificates;
} VerifyCertsTest;

static const VerifyCertsTest verify_certs_tests[] = {
    {"defaults", false, TLS_NO_CLIENT_CERT, false},
    {"InsecureSkipVerify", true, TLS_NO_CLIENT_CERT, false},
    {"RequestClientCert with no certs", false, TLS_REQUEST_CLIENT_CERT, false},
    {"RequestClientCert with certs", false, TLS_REQUEST_CLIENT_CERT, true},
    {"RequireAnyClientCert", false, TLS_REQUIRE_ANY_CLIENT_CERT, true},
    {"VerifyClientCertIfGiven with no certs", false, TLS_VERIFY_CLIENT_CERT_IF_GIVEN,
     false},
    {"VerifyClientCertIfGiven with certs", false, TLS_VERIFY_CLIENT_CERT_IF_GIVEN,
     true},
    {"RequireAndVerifyClientCert", false, TLS_REQUIRE_AND_VERIFY_CLIENT_CERT, true},
};

/* Not in Go: what each parallel subtest gets. It has to outlive the function
 * that starts the subtest, as Go's closure does, so it is a table. */
typedef struct VerifyCertsCase {
    uint16_t version;
    const VerifyCertsTest *test;
} VerifyCertsCase;

#define VERIFY_CERTS_CASES(v)                                                          \
    {v, &verify_certs_tests[0]}, {v, &verify_certs_tests[1]},                          \
        {v, &verify_certs_tests[2]}, {v, &verify_certs_tests[3]},                      \
        {v, &verify_certs_tests[4]}, {v, &verify_certs_tests[5]},                      \
        {v, &verify_certs_tests[6]}, {                                                 \
        v, &verify_certs_tests[7]                                                      \
    }

static const VerifyCertsCase verify_certs_tls12[] = {
    VERIFY_CERTS_CASES(TLS_VERSION_TLS12)};
static const VerifyCertsCase verify_certs_tls13[] = {
    VERIFY_CERTS_CASES(TLS_VERSION_TLS13)};

static Error verify_certs_connection(void *env, const TlsConnectionState *cs) {
    (void)cs;
    *(bool *)env = true;
    return BURROW_NO_ERROR;
}

static Error verify_certs_peer_certificates(void *env, Slice raw_certs,
                                            Slice verified_chains) {
    (void)raw_certs;
    (void)verified_chains;
    *(bool *)env = true;
    return BURROW_NO_ERROR;
}

static void verify_certificates_case(void *env, TestingT *t) {
    const VerifyCertsCase *c = (const VerifyCertsCase *)env;
    const VerifyCertsTest *test = c->test;
    uint16_t version = c->version;
    testing_t_parallel(t);

    bool server_verify_connection = false, client_verify_connection = false;
    bool server_verify_peer_certificates = false,
         client_verify_peer_certificates = false;

    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    TlsConfig client_config = test_config_client(a);
    client_config.max_version = version;
    client_config.min_version = version;
    client_config.client_session_cache =
        tls_new_lru_client_session_cache(heap_allocator(), 1);
    if (client_config.client_session_cache.vt == NULL)
        testing_t_fatal_v(t, "out of memory");
    TlsConfig server_config = test_config_server(a);
    server_config.max_version = version;
    server_config.min_version = version;

    client_config.verify_connection = BURROW_FN(
        TlsVerifyConnectionFunc, verify_certs_connection, &client_verify_connection);
    client_config.verify_peer_certificate =
        BURROW_FN(TlsVerifyPeerCertificateFunc, verify_certs_peer_certificates,
                  &client_verify_peer_certificates);
    server_config.verify_connection = BURROW_FN(
        TlsVerifyConnectionFunc, verify_certs_connection, &server_verify_connection);
    server_config.verify_peer_certificate =
        BURROW_FN(TlsVerifyPeerCertificateFunc, verify_certs_peer_certificates,
                  &server_verify_peer_certificates);

    client_config.insecure_skip_verify = test->insecure_skip_verify;
    server_config.client_auth = test->client_auth;
    if (!test->client_certificates)
        client_config.certificates = slice_nil(TYPE_TLS_CERTIFICATE);

    Handshake h;
    test_handshake(t, &client_config, &server_config, &h);
    if (BURROW_FAILED(h.err))
        testing_t_fatal_v(t, h.err);
    handshake_free(&h);

    bool want = server_config.client_auth != TLS_NO_CLIENT_CERT;
    if (server_verify_peer_certificates != want)
        testing_t_errorf_v(t, "VerifyPeerCertificates on the server: got %v, want %v",
                           server_verify_peer_certificates, want);
    if (!client_verify_peer_certificates)
        testing_t_errorf_v(t, "VerifyPeerCertificates not called on the client");
    if (!server_verify_connection)
        testing_t_error_v(t, "VerifyConnection did not get called on the server");
    if (!client_verify_connection)
        testing_t_error_v(t, "VerifyConnection did not get called on the client");

    server_verify_peer_certificates = client_verify_peer_certificates = false;
    server_verify_connection = client_verify_connection = false;
    test_handshake(t, &client_config, &server_config, &h);
    if (BURROW_FAILED(h.err))
        testing_t_fatal_v(t, h.err);
    /* Go names the server's state cs here, and checks that. */
    if (!h.server_state.did_resume)
        testing_t_error_v(t, "expected resumption");
    handshake_free(&h);

    if (server_verify_peer_certificates)
        testing_t_error_v(t, "VerifyPeerCertificates got called on the server on "
                             "resumption");
    if (client_verify_peer_certificates)
        testing_t_error_v(t, "VerifyPeerCertificates got called on the client on "
                             "resumption");
    if (!server_verify_connection)
        testing_t_error_v(t, "VerifyConnection did not get called on the server on "
                             "resumption");
    if (!client_verify_connection)
        testing_t_error_v(t, "VerifyConnection did not get called on the client on "
                             "resumption");

    tls_lru_client_session_cache_free(client_config.client_session_cache);
    tls_config_release(&server_config);
    tls_config_release(&client_config);
    arena_free(&ar);
}

static void test_verify_certificates(TestingT *t, const VerifyCertsCase *cases) {
    for (int i = 0; i < 8; i++)
        testing_t_run(t, str_from_cstr(cases[i].test->name),
                      BURROW_FN(TestingTFunc, verify_certificates_case,
                                (void *)(uintptr_t)&cases[i]));
}

static void verify_certificates_tls12(void *env, TestingT *t) {
    (void)env;
    test_verify_certificates(t, verify_certs_tls12);
}

static void verify_certificates_tls13(void *env, TestingT *t) {
    (void)env;
    test_verify_certificates(t, verify_certs_tls13);
}

static void TestVerifyCertificates(TestingT *t) {
    /* See https://go.dev/issue/31641. */
    testing_t_run(t, BURROW_S("TLSv12"),
                  BURROW_FN(TestingTFunc, verify_certificates_tls12, NULL));
    testing_t_run(t, BURROW_S("TLSv13"),
                  BURROW_FN(TestingTFunc, verify_certificates_tls13, NULL));
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
    X(TestKeyLogWriterErr)                                                             \
    X(TestDialTimeout)                                                                 \
    X(TestDeadlineOnWrite)                                                             \
    X(TestDialer)                                                                      \
    X(TestConnReadNonzeroAndEOF)                                                       \
    X(TestTLSUniqueMatches)                                                            \
    X(TestVerifyHostname)                                                              \
    X(TestRealResumption)                                                              \
    X(TestConnCloseBreakingWrite)                                                      \
    X(TestConnCloseWrite)                                                              \
    X(TestWarningAlertFlood)                                                           \
    X(TestConnectionState)                                                             \
    X(TestPKCS1OnlyCert)                                                               \
    X(TestEarlyLargeCertMsg)                                                           \
    X(TestLargeCertMsg)                                                                \
    X(TestMessageSigner)                                                               \
    X(TestInvalidHandshakeSignature)                                                   \
    X(TestLocalCertificate)                                                            \
    X(TestLocalCertificateResumption)                                                  \
    X(TestVerifyCertificates)

TESTING_MAIN(TESTS)
