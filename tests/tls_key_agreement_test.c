/* Not in Go. Go has no key_agreement_test.go and tests the key agreements
 * through its handshake tests, which are not here yet. These run each side of
 * a key agreement against the other with the certificates from
 * certificates_test.go, which are in tls_test_gen.h.
 *
 * Copyright 2026 The burrow Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style licence that can be found
 * in the LICENSE file. */

#include "check.h"

#include "../src/crypto/tls_internal.h"
#include "burrow/burrow.h"
#include "burrow/crypto/tls.h"
#include "burrow/strings.h"
#include "tls_test_gen.h"

#include <stdint.h>
#include <string.h>

/* Not in Go. A ClientHello and a ServerHello with what a key agreement reads,
 * and the curves the client offers. */
typedef struct Hellos {
    TlsClientHelloMsg client;
    TlsServerHelloMsg server;
    TlsCurveID curves[2];
    Byte client_random[32];
    Byte server_random[32];
} Hellos;

static void hellos_init(Alloc *a, Hellos *h, uint16_t vers) {
    memset(h, 0, sizeof *h);
    for (int i = 0; i < 32; i++) {
        h->client_random[i] = (Byte)i;
        h->server_random[i] = (Byte)(100 + i);
    }
    h->curves[0] = TLS_X25519;
    h->curves[1] = TLS_CURVE_P256;
    h->client.vers = vers;
    h->client.random = slice_from(h->client_random, 32, 32, TYPE_BYTE);
    h->client.supported_curves = slice_from(h->curves, 2, 2, TYPE_UINT16);
    h->client.supported_signature_algorithms =
        burrow__tls_supported_signature_algorithms(a, TLS_VERSION_TLS10, vers);
    h->server.random = slice_from(h->server_random, 32, 32, TYPE_BYTE);
}

/* Not in Go. Runs a whole key agreement between server and client: the
 * server's ServerKeyExchange, the client's answer, and the pre-master secret
 * each side gets. */
static void key_agreement_round(TestingT *t, Alloc *a, const char *name,
                                const TlsSuite *suite, uint16_t vers,
                                const TlsCertificate *cert, TlsKeyAgreement *server,
                                TlsKeyAgreement *client) {
    TlsConfig config;
    memset(&config, 0, sizeof config);
    Hellos h;
    hellos_init(a, &h, vers);

    Error err = BURROW_NO_ERROR;
    TlsServerKeyExchangeMsg *skx =
        burrow__tls_key_agreement_generate_server_key_exchange(
            server, a, &config, cert, &h.client, &h.server, &err);
    if (BURROW_FAILED(err)) {
        testing_t_errorf_v(t, "%s: generateServerKeyExchange: %v", name, err);
        return;
    }
    if (skx == NULL) {
        testing_t_errorf_v(t, "%s: no ServerKeyExchange", name);
        return;
    }
    err = burrow__tls_key_agreement_process_server_key_exchange(
        client, a, &config, &h.client, &h.server, cert->leaf, skx);
    if (BURROW_FAILED(err)) {
        testing_t_errorf_v(t, "%s: processServerKeyExchange: %v", name, err);
        return;
    }
    if (client->curve_id != TLS_X25519)
        testing_t_errorf_v(t, "%s: curve %d, want X25519", name, (Int)client->curve_id);
    TlsClientKeyExchangeMsg *ckx = NULL;
    Slice client_pms = burrow__tls_key_agreement_generate_client_key_exchange(
        client, a, &config, &h.client, cert->leaf, &ckx, &err);
    if (BURROW_FAILED(err)) {
        testing_t_errorf_v(t, "%s: generateClientKeyExchange: %v", name, err);
        return;
    }
    Slice server_pms = burrow__tls_key_agreement_process_client_key_exchange(
        server, a, &config, cert, ckx, vers, &err);
    if (BURROW_FAILED(err)) {
        testing_t_errorf_v(t, "%s: processClientKeyExchange: %v", name, err);
        return;
    }
    if (client_pms.len != 32 || !bytes_equal(client_pms, server_pms))
        testing_t_errorf_v(t, "%s: pre-master secrets differ: %x and %x", name,
                           client_pms, server_pms);

    /* A changed signature does not verify. */
    TlsKeyAgreement again;
    burrow__tls_key_agreement_init(&again, suite, vers);
    Byte *key = skx->key.p;
    key[skx->key.len - 1] ^= 0xff;
    err = burrow__tls_key_agreement_process_server_key_exchange(
        &again, a, &config, &h.client, &h.server, cert->leaf, skx);
    burrow__tls_key_agreement_free(&again);
    Str want = BURROW_S("tls: invalid signature by the server certificate: ");
    if (BURROW_OK(err) || !strings_has_prefix(error_text(err), want))
        testing_t_errorf_v(t, "%s: changed signature: %v", name, err);
}

/* Not in Go. */
static void run_key_agreement(TestingT *t, Alloc *a, const char *name,
                              uint16_t suite_id, uint16_t vers,
                              const TlsCertificate *cert) {
    const TlsSuite *suite = burrow__tls_cipher_suite_by_id(suite_id);
    TlsKeyAgreement server, client;
    burrow__tls_key_agreement_init(&server, suite, vers);
    burrow__tls_key_agreement_init(&client, suite, vers);
    key_agreement_round(t, a, name, suite, vers, cert, &server, &client);
    burrow__tls_key_agreement_free(&server);
    burrow__tls_key_agreement_free(&client);
}

/* Not in Go. */
static void TestKeyAgreementECDHE(TestingT *t) {
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    TlsCertificate rsa_cert =
        parse_test_cert(a, test_rsa2048_cert_pem, test_rsa2048_key_pem);
    TlsCertificate ecdsa_cert =
        parse_test_cert(a, test_ecdsap256_cert_pem, test_ecdsap256_key_pem);
    TlsCertificate ed25519_cert =
        parse_test_cert(a, test_ed25519_cert_pem, test_ed25519_key_pem);
    static const uint16_t rsa_suite = TLS_TLS_ECDHE_RSA_WITH_AES_128_GCM_SHA256;
    static const uint16_t ecdsa_suite = TLS_TLS_ECDHE_ECDSA_WITH_AES_128_GCM_SHA256;

    run_key_agreement(t, a, "RSA TLS 1.0", rsa_suite, TLS_VERSION_TLS10, &rsa_cert);
    run_key_agreement(t, a, "RSA TLS 1.2", rsa_suite, TLS_VERSION_TLS12, &rsa_cert);
    run_key_agreement(t, a, "ECDSA TLS 1.1", ecdsa_suite, TLS_VERSION_TLS11,
                      &ecdsa_cert);
    run_key_agreement(t, a, "ECDSA TLS 1.2", ecdsa_suite, TLS_VERSION_TLS12,
                      &ecdsa_cert);
    run_key_agreement(t, a, "Ed25519 TLS 1.2", ecdsa_suite, TLS_VERSION_TLS12,
                      &ed25519_cert);
    arena_free(&ar);
}

/* Not in Go. */
static void TestKeyAgreementRSA(TestingT *t) {
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    TlsCertificate cert =
        parse_test_cert(a, test_rsa2048_cert_pem, test_rsa2048_key_pem);
    TlsConfig config;
    memset(&config, 0, sizeof config);
    Hellos h;
    hellos_init(a, &h, TLS_VERSION_TLS12);
    const TlsSuite *suite =
        burrow__tls_cipher_suite_by_id(TLS_TLS_RSA_WITH_AES_128_GCM_SHA256);
    TlsKeyAgreement server, client;
    burrow__tls_key_agreement_init(&server, suite, TLS_VERSION_TLS12);
    burrow__tls_key_agreement_init(&client, suite, TLS_VERSION_TLS12);

    Error err = BURROW_NO_ERROR;
    TlsServerKeyExchangeMsg *skx =
        burrow__tls_key_agreement_generate_server_key_exchange(
            &server, a, &config, &cert, &h.client, &h.server, &err);
    if (skx != NULL || BURROW_FAILED(err))
        testing_t_errorf_v(t, "generateServerKeyExchange: %p, %v; want nil, nil",
                           (void *)skx, err);

    TlsServerKeyExchangeMsg empty = {0};
    err = burrow__tls_key_agreement_process_server_key_exchange(
        &client, a, &config, &h.client, &h.server, cert.leaf, &empty);
    if (!str_eq(error_text(err), BURROW_S("tls: unexpected ServerKeyExchange")))
        testing_t_errorf_v(t, "processServerKeyExchange: %v", err);

    TlsClientKeyExchangeMsg *ckx = NULL;
    err = BURROW_NO_ERROR;
    Slice client_pms = burrow__tls_key_agreement_generate_client_key_exchange(
        &client, a, &config, &h.client, cert.leaf, &ckx, &err);
    if (BURROW_FAILED(err))
        testing_t_fatalf_v(t, "generateClientKeyExchange: %v", err);
    const Byte *pms = client_pms.p;
    if (client_pms.len != 48 || pms[0] != 3 || pms[1] != 3)
        testing_t_errorf_v(t, "pre-master secret %x does not start with the version",
                           client_pms);
    Slice server_pms = burrow__tls_key_agreement_process_client_key_exchange(
        &server, a, &config, &cert, ckx, TLS_VERSION_TLS12, &err);
    if (BURROW_FAILED(err) || !bytes_equal(client_pms, server_pms))
        testing_t_errorf_v(t, "processClientKeyExchange: %x, %v; want %x", server_pms,
                           err, client_pms);

    /* A ClientKeyExchange whose length is wrong. */
    Slice ct = ckx->ciphertext;
    TlsClientKeyExchangeMsg bad = {slice_sub(ct, 0, ct.len - 1)};
    err = BURROW_NO_ERROR;
    (void)burrow__tls_key_agreement_process_client_key_exchange(
        &server, a, &config, &cert, &bad, TLS_VERSION_TLS12, &err);
    if (!str_eq(error_text(err), BURROW_S("tls: invalid ClientKeyExchange message")))
        testing_t_errorf_v(t, "short ClientKeyExchange: %v", err);

    /* Only an RSA key can decrypt. */
    TlsCertificate ecdsa_cert =
        parse_test_cert(a, test_ecdsap256_cert_pem, test_ecdsap256_key_pem);
    err = BURROW_NO_ERROR;
    (void)burrow__tls_key_agreement_process_client_key_exchange(
        &server, a, &config, &ecdsa_cert, ckx, TLS_VERSION_TLS12, &err);
    if (!str_eq(error_text(err), BURROW_S("tls: certificate private key does not "
                                          "implement crypto.Decrypter")))
        testing_t_errorf_v(t, "ECDSA key: %v", err);
    err = BURROW_NO_ERROR;
    (void)burrow__tls_key_agreement_generate_client_key_exchange(
        &client, a, &config, &h.client, ecdsa_cert.leaf, &ckx, &err);
    if (!str_eq(error_text(err),
                BURROW_S("tls: server certificate contains incorrect key type for "
                         "selected ciphersuite")))
        testing_t_errorf_v(t, "ECDSA certificate: %v", err);
    arena_free(&ar);
}

/* Not in Go. */
static void TestKeyAgreementErrors(TestingT *t) {
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    TlsCertificate rsa_cert =
        parse_test_cert(a, test_rsa2048_cert_pem, test_rsa2048_key_pem);
    TlsCertificate ecdsa_cert =
        parse_test_cert(a, test_ecdsap256_cert_pem, test_ecdsap256_key_pem);
    TlsConfig config;
    memset(&config, 0, sizeof config);
    const TlsSuite *suite =
        burrow__tls_cipher_suite_by_id(TLS_TLS_ECDHE_RSA_WITH_AES_128_GCM_SHA256);
    static const uint16_t versions[] = {TLS_VERSION_TLS10, TLS_VERSION_TLS12};

    for (size_t i = 0; i < sizeof versions / sizeof versions[0]; i++) {
        uint16_t vers = versions[i];
        Hellos h;
        hellos_init(a, &h, vers);
        TlsKeyAgreement ka;
        Error err = BURROW_NO_ERROR;

        /* An ECDSA certificate with an ECDHE_RSA suite. */
        burrow__tls_key_agreement_init(&ka, suite, vers);
        (void)burrow__tls_key_agreement_generate_server_key_exchange(
            &ka, a, &config, &ecdsa_cert, &h.client, &h.server, &err);
        burrow__tls_key_agreement_free(&ka);
        if (!str_eq(error_text(err), BURROW_S("tls: certificate cannot be used with "
                                              "the selected cipher suite")))
            testing_t_errorf_v(t, "%x: ECDSA certificate: %v", (Int)vers, err);

        /* No curves offered. */
        h.client.supported_curves.len = 0;
        err = BURROW_NO_ERROR;
        burrow__tls_key_agreement_init(&ka, suite, vers);
        (void)burrow__tls_key_agreement_generate_server_key_exchange(
            &ka, a, &config, &rsa_cert, &h.client, &h.server, &err);
        burrow__tls_key_agreement_free(&ka);
        if (!str_eq(error_text(err),
                    BURROW_S("tls: no supported elliptic curves offered")))
            testing_t_errorf_v(t, "%x: no curves: %v", (Int)vers, err);

        /* The server picks X25519, which the client then does not offer. */
        h.client.supported_curves.len = 2;
        err = BURROW_NO_ERROR;
        burrow__tls_key_agreement_init(&ka, suite, vers);
        TlsServerKeyExchangeMsg *skx =
            burrow__tls_key_agreement_generate_server_key_exchange(
                &ka, a, &config, &rsa_cert, &h.client, &h.server, &err);
        burrow__tls_key_agreement_free(&ka);
        if (skx == NULL)
            testing_t_fatalf_v(t, "%x: generateServerKeyExchange: %v", (Int)vers, err);
        h.client.supported_curves = slice_sub(h.client.supported_curves, 1, 2);
        burrow__tls_key_agreement_init(&ka, suite, vers);
        err = burrow__tls_key_agreement_process_server_key_exchange(
            &ka, a, &config, &h.client, &h.server, rsa_cert.leaf, skx);
        burrow__tls_key_agreement_free(&ka);
        if (!str_eq(error_text(err), BURROW_S("tls: server selected unoffered curve")))
            testing_t_errorf_v(t, "%x: unoffered curve: %v", (Int)vers, err);

        /* A ServerKeyExchange cut short. */
        TlsServerKeyExchangeMsg short_skx = {slice_sub(skx->key, 0, skx->key.len - 1)};
        burrow__tls_key_agreement_init(&ka, suite, vers);
        err = burrow__tls_key_agreement_process_server_key_exchange(
            &ka, a, &config, &h.client, &h.server, rsa_cert.leaf, &short_skx);
        burrow__tls_key_agreement_free(&ka);
        if (!str_eq(error_text(err),
                    BURROW_S("tls: invalid ServerKeyExchange message")))
            testing_t_errorf_v(t, "%x: short ServerKeyExchange: %v", (Int)vers, err);

        /* No ServerKeyExchange before the client's answer. */
        burrow__tls_key_agreement_init(&ka, suite, vers);
        TlsClientKeyExchangeMsg *ckx = NULL;
        err = BURROW_NO_ERROR;
        (void)burrow__tls_key_agreement_generate_client_key_exchange(
            &ka, a, &config, &h.client, rsa_cert.leaf, &ckx, &err);
        if (!str_eq(error_text(err),
                    BURROW_S("tls: missing ServerKeyExchange message")))
            testing_t_errorf_v(t, "%x: missing ServerKeyExchange: %v", (Int)vers, err);
    }
    arena_free(&ar);
}

/* Not in Go. */
static void TestCurveForCurveID(TestingT *t) {
    static const struct {
        TlsCurveID id;
        const char *name;
    } tests[] = {
        {TLS_X25519, "X25519"},     {TLS_CURVE_P256, "P-256"},
        {TLS_CURVE_P384, "P-384"},  {TLS_CURVE_P521, "P-521"},
        {TLS_X25519MLKEM768, NULL}, {0, NULL},
    };
    for (size_t i = 0; i < sizeof tests / sizeof tests[0]; i++) {
        const EcdhCurve *c = burrow__tls_curve_for_curve_id(tests[i].id);
        if (tests[i].name == NULL) {
            if (c != NULL)
                testing_t_errorf_v(t, "curveForCurveID(%d) = %s, want none",
                                   (Int)tests[i].id, ecdh_curve_string(c));
        } else if (c == NULL ||
                   !str_eq(ecdh_curve_string(c), str_from_cstr(tests[i].name))) {
            testing_t_errorf_v(t, "curveForCurveID(%d) is not %s", (Int)tests[i].id,
                               tests[i].name);
        }
    }
}

#define TESTS(X)                                                                       \
    X(TestKeyAgreementECDHE)                                                           \
    X(TestKeyAgreementRSA)                                                             \
    X(TestKeyAgreementErrors)                                                          \
    X(TestCurveForCurveID)

TESTING_MAIN(TESTS)
