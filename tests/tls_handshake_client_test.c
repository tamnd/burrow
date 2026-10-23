/* Derived from Go's src/crypto/tls/handshake_client_test.go.
 * Go source: go1.27.1.
 *
 * The tests that replay Go's recorded handshakes do it through
 * tls_replay_test.h, with the recordings tools/gen-tls-replay-tests.sh put in
 * tls_replay_client_gen.h. The ECH tests run against this package's own
 * server. TestResumption reaches into the LRU cache through
 * burrow__tls_lru_session_cache_front, and TestDowngradeCanary sets
 * burrow__tls_testing_only_force_downgrade_canary.
 *
 * Copyright 2010 The Go Authors. All rights reserved.
 * Copyright 2026 The burrow Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style licence that can be found
 * in the LICENSE file. */

#include "check.h"

#include "../src/crypto/tls_internal.h"
#include "burrow/burrow.h"
#include "burrow/crypto/ecdsa.h"
#include "burrow/crypto/elliptic.h"
#include "burrow/crypto/tls.h"
#include "burrow/crypto/x509.h"
#include "burrow/encoding/hex.h"
#include "burrow/encoding/pem.h"
#include "burrow/math/big.h"
#include "burrow/mem/heap.h"
#include "burrow/strings.h"
#include "tls_handshake_test.h"
#include "tls_replay_client_gen.h"
#include "tls_replay_test.h"
#include "tls_test_gen.h"

#include <string.h>

/* opensslSentinel, what the recordings have the server send once the client
 * has renegotiated or updated its keys. */
#define OPENSSL_SENTINEL "SENTINEL\n"

/* Not in Go: the checkRenegotiationError func(renegotiationNum int, err error)
 * error of a clientTest. */
BURROW_FUNC(RenegotiationErrorCheck, Error, int renegotiation_num, Error err);

/* clientTest. Not in Go: args, serverCert and extensions are left out, since
 * only -update uses them, to start OpenSSL with. */
typedef struct ClientTest {
    /* name is a freeform string identifying the test and the file in which
     * the expected results will be stored. */
    Str name;
    /* config, if not nil, contains a custom Config to use for this test. */
    TlsConfig *config;
    /* validate, if not nil, is a function that will be called with the
     * ConnectionState of the resulting connection. It returns a non-nil
     * error if the ConnectionState is unacceptable. */
    ConnectionStateCheck validate;
    /* numRenegotiations is the number of times that the connection will be
     * renegotiated. */
    int num_renegotiations;
    /* renegotiationExpectedToFail, if not zero, is the number of the
     * renegotiation attempt that is expected to fail. */
    int renegotiation_expected_to_fail;
    /* checkRenegotiationError, if not nil, is called with any error
     * arising from renegotiation. It can map expected errors to nil to
     * ignore them. */
    RenegotiationErrorCheck check_renegotiation_error;
    /* sendKeyUpdate will cause the server to send a KeyUpdate message. */
    bool send_key_update;
    /* Not in Go: where the test's allocations go. */
    Alloc *a;
} ClientTest;

static Str client_test_data_path(const ClientTest *test) {
    return fmt_sprintf_v(test->a, "testdata/Client-%s", test->name);
}

static Slice client_test_load_data(const ClientTest *test, Error *err) {
    Str name = fmt_sprintf_v(test->a, "Client-%s", test->name);
    return load_testdata(test->a, tls_client_testdata,
                         sizeof tls_client_testdata / sizeof tls_client_testdata[0],
                         name, err);
}

/* Not in Go: the part of clientTest.run between Client and the deferred
 * Close, which returns where Go's does. */
static void client_test_talk(const ClientTest *test, TestingT *t, TlsConn *client) {
    Error err = BURROW_NO_ERROR;
    Byte hello[] = "hello\n";
    (void)tls_conn_write(client, slice_from(hello, 6, 6, TYPE_BYTE), &err);
    if (BURROW_FAILED(err)) {
        testing_t_errorf_v(t, "Client.Write failed: %s", error_text(err));
        return;
    }

    Str openssl_sentinel = BURROW_S(OPENSSL_SENTINEL);
    for (int i = 1; i <= test->num_renegotiations; i++) {
        /* Not in Go: Go reads in a goroutine, and waits for it straight away
         * when it replays, there being no OpenSSL to talk to in between. So
         * the read is made here. */
        Byte buf[256];
        Error rerr = BURROW_NO_ERROR;
        Int n = tls_conn_read(client, slice_from(buf, 256, 256, TYPE_BYTE), &rerr);

        if (test->check_renegotiation_error.f != NULL) {
            Error new_err = test->check_renegotiation_error.f(
                test->check_renegotiation_error.env, i, rerr);
            if (BURROW_FAILED(rerr) && !BURROW_FAILED(new_err))
                continue;
            rerr = new_err;
        }

        if (BURROW_FAILED(rerr)) {
            testing_t_errorf_v(t, "Client.Read failed after renegotiation #%d: %s", i,
                               error_text(rerr));
            continue;
        }

        Str got = str_from_bytes(buf, n);
        if (!str_eq(openssl_sentinel, got))
            testing_t_errorf_v(t, "Client.Read returned %q, but wanted %q", got,
                               openssl_sentinel);

        Int expected = (Int)i + 1;
        if (client->handshakes != expected)
            testing_t_errorf_v(t,
                               "client should have recorded %d handshakes, but "
                               "believes that %d have occurred",
                               expected, client->handshakes);
    }

    if (test->send_key_update) {
        /* Not in Go: the read is inline, for the same reason. */
        Byte buf[256];
        Error rerr = BURROW_NO_ERROR;
        Int n = tls_conn_read(client, slice_from(buf, 256, 256, TYPE_BYTE), &rerr);
        if (BURROW_FAILED(rerr)) {
            testing_t_errorf_v(t, "Client.Read failed after KeyUpdate: %s",
                               error_text(rerr));
        } else {
            Str got = str_from_bytes(buf, n);
            if (!str_eq(openssl_sentinel, got))
                testing_t_errorf_v(t, "Client.Read returned %q, but wanted %q", got,
                                   openssl_sentinel);
        }

        Byte again[] = "hello again\n";
        (void)tls_conn_write(client, slice_from(again, 12, 12, TYPE_BYTE), &err);
        if (BURROW_FAILED(err)) {
            testing_t_errorf_v(t, "Client.Write failed: %s", error_text(err));
            return;
        }
    }

    if (test->validate.f != NULL) {
        TlsConnectionState cs = tls_conn_connection_state(client);
        Error verr = test->validate.f(test->validate.env, &cs);
        if (BURROW_FAILED(verr))
            testing_t_errorf_v(t, "validate callback returned error: %s",
                               error_text(verr));
    }
}

/* clientTest.run with write false, which is all of it there is without
 * -update. */
static void client_test_run(void *env, TestingT *t) {
    ClientTest *test = env;
    Alloc *a = test->a;

    Error err = BURROW_NO_ERROR;
    Slice flows = client_test_load_data(test, &err);
    if (BURROW_FAILED(err))
        testing_t_fatalf_v(t, "failed to load data from %s: %v",
                           client_test_data_path(test), error_text(err));
    ReplayingConn replay;
    memset(&replay, 0, sizeof replay);
    replay.t = t;
    replay.flows = flows;
    replay.reading = false;

    TlsConfig base;
    TlsConfig *config = test->config;
    if (config == NULL) {
        base = test_config_client(a);
        config = &base;
    }
    config = tls_config_clone(config, a);
    if (config == NULL)
        testing_t_fatal_v(t, "out of memory");
    TlsConn *client =
        tls_client(heap_allocator(), replaying_conn_as_net_conn(&replay), config);
    if (client == NULL)
        testing_t_fatal_v(t, "out of memory");
    client_test_talk(test, t, client);
    (void)tls_conn_close(client);
    tls_conn_free(client);
    tls_config_release(config);
}

static void run_client_test_for_version(TestingT *t, const ClientTest *tmpl,
                                        Str version) {
    ClientTest test = *tmpl;
    if (tmpl->config != NULL) {
        test.config = tls_config_clone(tmpl->config, test.a);
        if (test.config == NULL)
            testing_t_fatal_v(t, "out of memory");
    }
    test.name = fmt_sprintf_v(test.a, "%s-%s", version, test.name);

    run_test_and_update_if_needed(t, version,
                                  BURROW_FN(ReplayRunFunc, client_test_run, &test));
    if (test.config != NULL)
        tls_config_release(test.config);
}

static void run_client_test_tls10(TestingT *t, ClientTest *tmpl) {
    if (tmpl->config == NULL) {
        TlsConfig base = test_config_client(tmpl->a);
        tmpl->config = tls_config_clone(&base, tmpl->a);
        if (tmpl->config == NULL)
            testing_t_fatal_v(t, "out of memory");
    }
    if (tmpl->config->min_version == 0)
        tmpl->config->min_version = TLS_VERSION_TLS10;
    run_client_test_for_version(t, tmpl, BURROW_S("TLSv10"));
}

static void run_client_test_tls11(TestingT *t, ClientTest *tmpl) {
    if (tmpl->config == NULL) {
        TlsConfig base = test_config_client(tmpl->a);
        tmpl->config = tls_config_clone(&base, tmpl->a);
        if (tmpl->config == NULL)
            testing_t_fatal_v(t, "out of memory");
    }
    if (tmpl->config->min_version == 0)
        tmpl->config->min_version = TLS_VERSION_TLS11;
    run_client_test_for_version(t, tmpl, BURROW_S("TLSv11"));
}

static void run_client_test_tls12(TestingT *t, ClientTest *tmpl) {
    run_client_test_for_version(t, tmpl, BURROW_S("TLSv12"));
}

static void run_client_test_tls13(TestingT *t, ClientTest *tmpl) {
    run_client_test_for_version(t, tmpl, BURROW_S("TLSv13"));
}

/* Not in Go: testConfigClient.Clone(), made in a. */
static TlsConfig *test_config_client_clone(TestingT *t, Alloc *a) {
    TlsConfig base = test_config_client(a);
    TlsConfig *config = tls_config_clone(&base, a);
    if (config == NULL)
        testing_t_fatal_v(t, "out of memory");
    return config;
}

/* Not in Go: the end of a test that ran a clientTest, which gives back what
 * its config holds and frees ar. */
static void client_test_free(ClientTest *test, Arena *ar) {
    if (test->config != NULL)
        tls_config_release(test->config);
    arena_free(ar);
}

static void TestHandshakeClientRSARC4(TestingT *t) {
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    TlsConfig *config = test_config_client_clone(t, a);
    uint16_t suites[] = {TLS_TLS_RSA_WITH_RC4_128_SHA};
    config->cipher_suites = slice_from(suites, 1, 1, TYPE_UINT16);
    ClientTest test = {
        .name = BURROW_S("RSA-RC4"),
        .config = config,
        .validate = check_cipher_suite(a, TLS_TLS_RSA_WITH_RC4_128_SHA),
        .a = a,
    };
    run_client_test_tls10(t, &test);
    run_client_test_tls11(t, &test);
    run_client_test_tls12(t, &test);
    client_test_free(&test, &ar);
}

static void TestHandshakeClientRSAAES128GCM(TestingT *t) {
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    TlsConfig *config = test_config_client_clone(t, a);
    uint16_t suites[] = {TLS_TLS_RSA_WITH_AES_128_GCM_SHA256};
    config->cipher_suites = slice_from(suites, 1, 1, TYPE_UINT16);
    ClientTest test = {
        .name = BURROW_S("AES128-GCM-SHA256"),
        .config = config,
        .validate = check_cipher_suite(a, TLS_TLS_RSA_WITH_AES_128_GCM_SHA256),
        .a = a,
    };
    run_client_test_tls12(t, &test);
    client_test_free(&test, &ar);
}

static void TestHandshakeClientRSAAES256GCM(TestingT *t) {
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    TlsConfig *config = test_config_client_clone(t, a);
    uint16_t suites[] = {TLS_TLS_RSA_WITH_AES_256_GCM_SHA384};
    config->cipher_suites = slice_from(suites, 1, 1, TYPE_UINT16);
    ClientTest test = {
        .name = BURROW_S("AES256-GCM-SHA384"),
        .config = config,
        .validate = check_cipher_suite(a, TLS_TLS_RSA_WITH_AES_256_GCM_SHA384),
        .a = a,
    };
    run_client_test_tls12(t, &test);
    client_test_free(&test, &ar);
}

static void TestHandshakeClientECDHERSAAES(TestingT *t) {
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    ClientTest test = {
        .name = BURROW_S("ECDHE-RSA-AES"),
        .validate = check_cipher_suite(a, TLS_TLS_ECDHE_RSA_WITH_AES_128_CBC_SHA),
        .a = a,
    };
    run_client_test_tls10(t, &test);
    run_client_test_tls11(t, &test);
    run_client_test_tls12(t, &test);
    client_test_free(&test, &ar);
}

static void TestHandshakeClientECDHEECDSAAES(TestingT *t) {
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    ClientTest test = {
        .name = BURROW_S("ECDHE-ECDSA-AES"),
        .validate = check_cipher_suite(a, TLS_TLS_ECDHE_ECDSA_WITH_AES_128_CBC_SHA),
        .a = a,
    };
    run_client_test_tls10(t, &test);
    run_client_test_tls11(t, &test);
    run_client_test_tls12(t, &test);
    client_test_free(&test, &ar);
}

static void TestHandshakeClientECDHEECDSAAESGCM(TestingT *t) {
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    ClientTest test = {
        .name = BURROW_S("ECDHE-ECDSA-AES-GCM"),
        .validate = check_cipher_suite(a, TLS_TLS_ECDHE_ECDSA_WITH_AES_128_GCM_SHA256),
        .a = a,
    };
    run_client_test_tls12(t, &test);
    client_test_free(&test, &ar);
}

static void TestHandshakeClientAES256GCMSHA384(TestingT *t) {
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    ClientTest test = {
        .name = BURROW_S("ECDHE-ECDSA-AES256-GCM-SHA384"),
        .validate = check_cipher_suite(a, TLS_TLS_ECDHE_ECDSA_WITH_AES_256_GCM_SHA384),
        .a = a,
    };
    run_client_test_tls12(t, &test);
    client_test_free(&test, &ar);
}

static void TestHandshakeClientAES128CBCSHA256(TestingT *t) {
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    TlsConfig *config = test_config_client_clone(t, a);
    uint16_t suites[] = {TLS_TLS_RSA_WITH_AES_128_CBC_SHA256};
    config->cipher_suites = slice_from(suites, 1, 1, TYPE_UINT16);
    ClientTest test = {
        .name = BURROW_S("AES128-SHA256"),
        .config = config,
        .validate = check_cipher_suite(a, TLS_TLS_RSA_WITH_AES_128_CBC_SHA256),
        .a = a,
    };
    run_client_test_tls12(t, &test);
    client_test_free(&test, &ar);
}

static void TestHandshakeClientECDHERSAAES128CBCSHA256(TestingT *t) {
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    TlsConfig *config = test_config_client_clone(t, a);
    uint16_t suites[] = {TLS_TLS_ECDHE_RSA_WITH_AES_128_CBC_SHA256};
    config->cipher_suites = slice_from(suites, 1, 1, TYPE_UINT16);
    ClientTest test = {
        .name = BURROW_S("ECDHE-RSA-AES128-SHA256"),
        .config = config,
        .validate = check_cipher_suite(a, TLS_TLS_ECDHE_RSA_WITH_AES_128_CBC_SHA256),
        .a = a,
    };
    run_client_test_tls12(t, &test);
    client_test_free(&test, &ar);
}

static void TestHandshakeClientECDHEECDSAAES128CBCSHA256(TestingT *t) {
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    TlsConfig *config = test_config_client_clone(t, a);
    uint16_t suites[] = {TLS_TLS_ECDHE_ECDSA_WITH_AES_128_CBC_SHA256};
    config->cipher_suites = slice_from(suites, 1, 1, TYPE_UINT16);
    ClientTest test = {
        .name = BURROW_S("ECDHE-ECDSA-AES128-SHA256"),
        .config = config,
        .validate = check_cipher_suite(a, TLS_TLS_ECDHE_ECDSA_WITH_AES_128_CBC_SHA256),
        .a = a,
    };
    run_client_test_tls12(t, &test);
    client_test_free(&test, &ar);
}

static void TestHandshakeClientX25519(TestingT *t) {
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    ClientTest test = {
        .name = BURROW_S("X25519-ECDHE"),
        .validate = check_curve_id(a, TLS_X25519),
        .a = a,
    };
    run_client_test_tls12(t, &test);
    run_client_test_tls13(t, &test);
    client_test_free(&test, &ar);
}

static void TestHandshakeClientP256(TestingT *t) {
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    ClientTest test = {
        .name = BURROW_S("P256-ECDHE"),
        .validate = check_curve_id(a, TLS_CURVE_P256),
        .a = a,
    };
    run_client_test_tls12(t, &test);
    run_client_test_tls13(t, &test);
    client_test_free(&test, &ar);
}

static Error hello_retry_request_check(void *env, const TlsConnectionState *cs) {
    (void)env;
    if (!cs->hello_retry_request)
        return errors_new(heap_allocator(), BURROW_S("expected HelloRetryRequest"));
    return BURROW_NO_ERROR;
}

static void TestHandshakeClientHelloRetryRequest(TestingT *t) {
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    TlsConfig *config = test_config_client_clone(t, a);
    uint16_t curves[] = {TLS_X25519, TLS_CURVE_P256};
    config->curve_preferences = slice_from(curves, 2, 2, TYPE_UINT16);

    ClientTest test = {
        .name = BURROW_S("HelloRetryRequest"),
        .config = config,
        .validate = BURROW_FN(ConnectionStateCheck, hello_retry_request_check, NULL),
        .a = a,
    };

    run_client_test_tls13(t, &test);
    client_test_free(&test, &ar);
}

static void TestHandshakeClientECDHERSAChaCha20(TestingT *t) {
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    ClientTest test = {
        .name = BURROW_S("ECDHE-RSA-CHACHA20-POLY1305"),
        .validate =
            check_cipher_suite(a, TLS_TLS_ECDHE_RSA_WITH_CHACHA20_POLY1305_SHA256),
        .a = a,
    };
    run_client_test_tls12(t, &test);
    client_test_free(&test, &ar);
}

static void TestHandshakeClientECDHEECDSAChaCha20(TestingT *t) {
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    ClientTest test = {
        .name = BURROW_S("ECDHE-ECDSA-CHACHA20-POLY1305"),
        .validate =
            check_cipher_suite(a, TLS_TLS_ECDHE_ECDSA_WITH_CHACHA20_POLY1305_SHA256),
        .a = a,
    };
    run_client_test_tls12(t, &test);
    client_test_free(&test, &ar);
}

static void TestHandshakeClientAES128SHA256(TestingT *t) {
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    ClientTest test = {
        .name = BURROW_S("AES128-SHA256"),
        .validate = check_cipher_suite(a, TLS_TLS_AES_128_GCM_SHA256),
        .a = a,
    };
    run_client_test_tls13(t, &test);
    client_test_free(&test, &ar);
}

static void TestHandshakeClientAES256SHA384(TestingT *t) {
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    ClientTest test = {
        .name = BURROW_S("AES256-SHA384"),
        .validate = check_cipher_suite(a, TLS_TLS_AES_256_GCM_SHA384),
        .a = a,
    };
    run_client_test_tls13(t, &test);
    client_test_free(&test, &ar);
}

static void TestHandshakeClientCHACHA20SHA256(TestingT *t) {
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    ClientTest test = {
        .name = BURROW_S("CHACHA20-SHA256"),
        .validate = check_cipher_suite(a, TLS_TLS_CHACHA20_POLY1305_SHA256),
        .a = a,
    };
    run_client_test_tls13(t, &test);
    client_test_free(&test, &ar);
}

/* Not in Go: what checkPeerCertificateType's func closes over. */
typedef struct PeerCertificateTypeCheck {
    Alloc *a;
    X509PublicKeyAlgorithm expected;
} PeerCertificateTypeCheck;

static Error check_peer_certificate_type_fn(void *env, const TlsConnectionState *cs) {
    const PeerCertificateTypeCheck *c = env;
    if (cs->peer_certificates.len == 0)
        return errors_new(c->a, BURROW_S("no peer certificates"));
    X509Certificate *const *certs = cs->peer_certificates.p;
    if (certs[0]->public_key_algorithm != c->expected) {
        Str got =
            x509_public_key_algorithm_string(certs[0]->public_key_algorithm, c->a);
        Str want = x509_public_key_algorithm_string(c->expected, c->a);
        return fmt_errorf_v(
            "peer certificate public key algorithm is %v, but expected %v", got, want);
    }
    return BURROW_NO_ERROR;
}

static ConnectionStateCheck
check_peer_certificate_type(Alloc *a, X509PublicKeyAlgorithm expected) {
    PeerCertificateTypeCheck *c =
        mem_alloc(a, sizeof *c, _Alignof(PeerCertificateTypeCheck));
    if (c == NULL)
        panic_str(BURROW_S("out of memory"));
    c->a = a;
    c->expected = expected;
    return BURROW_FN(ConnectionStateCheck, check_peer_certificate_type_fn, c);
}

static void TestHandshakeClientRSATLS13(TestingT *t) {
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    ClientTest test = {
        .name = BURROW_S("RSA"),
        .validate = check_peer_certificate_type(a, X509_RSA),
        .a = a,
    };
    run_client_test_tls13(t, &test);
    client_test_free(&test, &ar);
}

static void TestHandshakeClientECDSATLS13(TestingT *t) {
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    ClientTest test = {
        .name = BURROW_S("ECDSA"),
        .validate = check_peer_certificate_type(a, X509_ECDSA),
        .a = a,
    };
    run_client_test_tls13(t, &test);
    client_test_free(&test, &ar);
}

static void TestHandshakeClientEd25519(TestingT *t) {
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    ClientTest test = {
        .name = BURROW_S("Ed25519"),
        .validate = check_peer_certificate_type(a, X509_ED25519),
        .a = a,
    };
    run_client_test_tls12(t, &test);
    run_client_test_tls13(t, &test);
    client_test_free(&test, &ar);
}

static void TestHandshakeClientCertEd25519(TestingT *t) {
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    ClientTest test = {
        .name = BURROW_S("ClientCert-Ed25519-Ed25519"),
        .a = a,
    };

    run_client_test_tls12(t, &test);
    run_client_test_tls13(t, &test);

    ClientTest test2 = {
        .name = BURROW_S("ClientCert-Ed25519-RSA-ECDHE"),
        .a = a,
    };

    run_client_test_tls12(t, &test2);
    run_client_test_tls13(t, &test2);

    TlsConfig *config = test_config_client_clone(t, a);
    uint16_t suites[] = {TLS_TLS_RSA_WITH_AES_128_CBC_SHA256};
    config->cipher_suites = slice_from(suites, 1, 1, TYPE_UINT16);
    ClientTest test3 = {
        .name = BURROW_S("ClientCert-Ed25519-RSA"),
        .config = config,
        .validate = check_cipher_suite(a, TLS_TLS_RSA_WITH_AES_128_CBC_SHA256),
        .a = a,
    };

    run_client_test_tls12(t, &test3);
    client_test_free(&test3, &ar);
}

/* Not in Go: []Certificate{c}, made in a. */
static Slice one_certificate(Alloc *a, TlsCertificate c) {
    Slice certs = slice_make(a, TYPE_TLS_CERTIFICATE, 1, 1);
    if (certs.p == NULL)
        panic_str(BURROW_S("out of memory"));
    *(TlsCertificate *)certs.p = c;
    return certs;
}

static void TestHandshakeClientCertRSA(TestingT *t) {
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    TlsConfig *config = test_config_client_clone(t, a);
    config->certificates =
        one_certificate(a, parse_test_cert(a, test_client_rsa2048_cert_pem,
                                           test_client_rsa2048_key_pem));

    ClientTest test = {
        .name = BURROW_S("ClientCert-RSA-RSA-ECDHE"),
        .config = config,
        .a = a,
    };

    run_client_test_tls10(t, &test);
    run_client_test_tls12(t, &test);
    run_client_test_tls13(t, &test);

    ClientTest test2 = {
        .name = BURROW_S("ClientCert-RSA-ECDSA"),
        .config = config,
        .a = a,
    };

    run_client_test_tls10(t, &test2);
    run_client_test_tls12(t, &test2);
    run_client_test_tls13(t, &test2);

    uint16_t suites[] = {TLS_TLS_RSA_WITH_AES_128_CBC_SHA};
    config->cipher_suites = slice_from(suites, 1, 1, TYPE_UINT16);
    ClientTest test3 = {
        .name = BURROW_S("ClientCert-RSA-RSA"),
        .config = config,
        .validate = check_cipher_suite(a, TLS_TLS_RSA_WITH_AES_128_CBC_SHA),
        .a = a,
    };

    run_client_test_tls10(t, &test3);
    run_client_test_tls12(t, &test3);
    client_test_free(&test3, &ar);
}

static void TestHandshakeClientCertECDSA(TestingT *t) {
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    TlsConfig *config = test_config_client_clone(t, a);
    config->certificates =
        one_certificate(a, parse_test_cert(a, test_client_ecdsap256_cert_pem,
                                           test_client_ecdsap256_key_pem));

    ClientTest test = {
        .name = BURROW_S("ClientCert-ECDSA-ECDSA"),
        .config = config,
        .a = a,
    };

    run_client_test_tls10(t, &test);
    run_client_test_tls12(t, &test);
    run_client_test_tls13(t, &test);

    ClientTest test2 = {
        .name = BURROW_S("ClientCert-ECDSA-RSA-ECDHE"),
        .config = config,
        .a = a,
    };

    run_client_test_tls10(t, &test2);
    run_client_test_tls12(t, &test2);
    run_client_test_tls13(t, &test2);

    uint16_t suites[] = {TLS_TLS_RSA_WITH_AES_128_CBC_SHA};
    config->cipher_suites = slice_from(suites, 1, 1, TYPE_UINT16);
    ClientTest test3 = {
        .name = BURROW_S("ClientCert-ECDSA-RSA"),
        .config = config,
        .validate = check_cipher_suite(a, TLS_TLS_RSA_WITH_AES_128_CBC_SHA),
        .a = a,
    };

    run_client_test_tls10(t, &test3);
    run_client_test_tls12(t, &test3);
    client_test_free(&test3, &ar);
}

static Error rsapss_signature_check(void *env, const TlsConnectionState *cs) {
    Alloc *a = env;
    if (cs->peer_certificates.len == 0)
        panic_str(BURROW_S("runtime error: index out of range [0] with length 0"));
    X509Certificate *const *certs = cs->peer_certificates.p;
    if (certs[0]->signature_algorithm != X509_SHA512_WITH_RSAPSS) {
        Str got = x509_signature_algorithm_string(certs[0]->signature_algorithm, a);
        Str want = x509_signature_algorithm_string(X509_SHA512_WITH_RSAPSS, a);
        return fmt_errorf_v(
            "peer certificate signature algorithm is %v, but expected %v", got, want);
    }
    return BURROW_NO_ERROR;
}

/* TestHandshakeClientCertRSAPSS tests rsa_pss_rsae_sha256 signatures from both
 * client and server certificates. It also serves from both sides a certificate
 * signed itself with RSA-PSS, mostly to check that crypto/x509 chain validation
 * works. */
static void TestHandshakeClientCertRSAPSS(TestingT *t) {
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    TlsConfig *config = test_config_client_clone(t, a);
    config->certificates = one_certificate(
        a, parse_test_cert(a, test_client_rsapss_cert_pem, test_client_rsapss_key_pem));
    ClientTest test = {
        .name = BURROW_S("ClientCert-RSA-RSAPSS"),
        .config = config,
        .validate = BURROW_FN(ConnectionStateCheck, rsapss_signature_check, a),
        .a = a,
    };
    run_client_test_tls12(t, &test);
    run_client_test_tls13(t, &test);
    client_test_free(&test, &ar);
}

static void TestHandshakeClientCertRSAPKCS1v15(TestingT *t) {
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    ClientTest test = {
        .name = BURROW_S("ClientCert-RSA-RSAPKCS1v15"),
        .a = a,
    };
    run_client_test_tls12(t, &test);
    client_test_free(&test, &ar);
}

static void TestClientKeyUpdate(TestingT *t) {
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    ClientTest test = {
        .name = BURROW_S("KeyUpdate"),
        .send_key_update = true,
        .a = a,
    };
    run_client_test_tls13(t, &test);
    client_test_free(&test, &ar);
}

/* serializingClientCache. Not in Go: ticket and state are made in ar, which
 * only the client side of one handshake at a time uses. */
typedef struct SerializingClientCache {
    TestingT *t;

    Slice ticket, state;
    Arena ar;
} SerializingClientCache;

static TlsClientSessionState *serializing_client_cache_get(void *self, Str session_key,
                                                           bool *ok) {
    SerializingClientCache *c = self;
    (void)session_key;
    *ok = false;
    if (slice_is_nil(c->ticket))
        return NULL;
    Error err = BURROW_NO_ERROR;
    TlsSessionState *state = tls_parse_session_state(heap_allocator(), c->state, &err);
    if (BURROW_FAILED(err)) {
        testing_t_error_v(c->t, err);
        return NULL;
    }
    TlsClientSessionState *cs = tls_new_resumption_state(c->ticket, state, &err);
    if (BURROW_FAILED(err)) {
        testing_t_error_v(c->t, err);
        return NULL;
    }
    *ok = true;
    return cs;
}

static void serializing_client_cache_put(void *self, Str session_key,
                                         TlsClientSessionState *cs) {
    SerializingClientCache *c = self;
    (void)session_key;
    if (cs == NULL) {
        c->ticket = slice_nil(TYPE_BYTE);
        c->state = slice_nil(TYPE_BYTE);
        return;
    }
    Error err = BURROW_NO_ERROR;
    TlsSessionState *state = NULL;
    Slice ticket = tls_client_session_state_resumption_state(cs, &state, &err);
    if (BURROW_FAILED(err)) {
        testing_t_error_v(c->t, err);
        return;
    }
    Alloc *a = arena_allocator(&c->ar);
    Slice state_bytes = tls_session_state_bytes(state, a, &err);
    if (BURROW_FAILED(err)) {
        testing_t_error_v(c->t, err);
        return;
    }
    Slice ticket_copy = bytes_clone(a, ticket);
    if (ticket_copy.p == NULL && ticket.len > 0) {
        testing_t_error_v(c->t, burrow_err_out_of_memory);
        return;
    }
    c->ticket = ticket_copy;
    c->state = state_bytes;
}

static const TlsClientSessionCacheVT serializing_client_cache_vt = {
    NULL, serializing_client_cache_get, serializing_client_cache_put};

/* Not in Go: a client and a server config for testHandshake, and what the
 * last one returned. The tests keep one on the heap with a cleanup that gives
 * it back, so a Fatalf leaks nothing. */
typedef struct HandshakePair {
    TestingT *t;
    TlsConfig server_config;
    TlsConfig client_config;
    /* The client's NewLRUClientSessionCache(32), when it has one. */
    TlsClientSessionCache lru;
    /* What the last testHandshake returned, kept until the next one. */
    Handshake h;
    bool h_live;
    Arena ar;
} HandshakePair;

static void handshake_pair_init(HandshakePair *p, TestingT *t) {
    memset(p, 0, sizeof *p);
    p->t = t;
    arena_init(&p->ar, NULL, 0);
}

static void handshake_pair_release(HandshakePair *p) {
    if (p->h_live)
        handshake_free(&p->h);
    tls_config_release(&p->server_config);
    tls_config_release(&p->client_config);
    tls_lru_client_session_cache_free(p->lru);
    arena_free(&p->ar);
}

/* NewLRUClientSessionCache(32), which p gives back. */
static TlsClientSessionCache handshake_pair_lru(HandshakePair *p) {
    p->lru = tls_new_lru_client_session_cache(heap_allocator(), 32);
    if (p->lru.vt == NULL)
        testing_t_fatal_v(p->t, "out of memory");
    return p->lru;
}

/* testHandshake(t, clientConfig, serverConfig), with what it returns in
 * p->h. */
static void handshake_pair_run(HandshakePair *p) {
    if (p->h_live) {
        handshake_free(&p->h);
        p->h_live = false;
    }
    test_handshake(p->t, &p->client_config, &p->server_config, &p->h);
    p->h_live = true;
}

/* Not in Go: testRootCertPool and testClientRootCertPool, made in a. */
static X509CertPool *new_test_cert_pool(TestingT *t, Alloc *a,
                                        const char *const *cert_pem,
                                        const char *const *key_pem) {
    X509CertPool *pool = x509_new_cert_pool(a);
    if (pool == NULL)
        testing_t_fatal_v(t, "out of memory");
    x509_cert_pool_add_cert(pool, parse_test_cert(a, cert_pem, key_pem).leaf);
    return pool;
}

/* Not in Go: what the closures of testResumption share. */
typedef struct Resumption {
    HandshakePair p;
    uint16_t version;
    SerializingClientCache serializing;
    /* What resumption_time adds to testTime(). */
    Duration d;
    TlsCertificate test_certificates[1];
    TlsCertificate client_certificates[1];
    uint16_t server_cipher_suites[2];
    uint16_t client_cipher_suites[1];
    uint16_t curve_preferences[3];
    Byte keys[2][32];
} Resumption;

static void resumption_free(void *env) {
    Resumption *r = env;
    handshake_pair_release(&r->p);
    arena_free(&r->serializing.ar);
    mem_free(heap_allocator(), r, sizeof *r, _Alignof(Resumption));
}

/* Not in Go: the func() time.Time the test gives serverConfig.Time, testTime()
 * and r->d. */
static Time resumption_time(void *env) {
    const Resumption *r = env;
    return time_add(test_time(NULL), r->d);
}

/* Not in Go: the serverConfig the test starts with, and makes again. */
static void resumption_server_config(Resumption *r) {
    tls_config_release(&r->p.server_config);
    memset(&r->p.server_config, 0, sizeof r->p.server_config);
    r->p.server_config.max_version = r->version;
    r->p.server_config.cipher_suites =
        slice_from(r->server_cipher_suites, 2, 2, TYPE_UINT16);
    r->p.server_config.certificates =
        slice_from(r->test_certificates, 1, 1, TYPE_TLS_CERTIFICATE);
    r->p.server_config.time = (TlsTimeFunc){test_time, NULL};
}

static void test_resume_state(Resumption *r, const char *test, bool did_resume) {
    TestingT *t = r->p.t;
    testing_t_helper(t);
    handshake_pair_run(&r->p);
    const TlsConnectionState *hs = &r->p.h.client_state;
    if (BURROW_FAILED(r->p.h.err))
        testing_t_fatalf_v(t, "%s: handshake failed: %s", test, r->p.h.err);
    if (hs->did_resume != did_resume)
        testing_t_fatalf_v(t, "%s resumed: %v, expected: %v", test, hs->did_resume,
                           did_resume);
    if (did_resume &&
        (slice_is_nil(hs->peer_certificates) || slice_is_nil(hs->verified_chains)))
        testing_t_fatalf_v(t,
                           "expected non-nil certificates after resumption. Got "
                           "peerCertificates: %#v, verifiedCertificates: %#v",
                           hs->peer_certificates, hs->verified_chains);
    Str got = hs->server_name, want = r->p.client_config.server_name;
    if (!str_eq(got, want))
        testing_t_errorf_v(t, "%s: server name %s, want %s", test, got, want);
}

/* Not in Go: q.Front() of the lruSessionCache, the state of the entry and its
 * key. Go's would panic on an empty cache. */
static TlsClientSessionState *resumption_front(Resumption *r, Str *key) {
    TlsClientSessionState *cs = burrow__tls_lru_session_cache_front(
        r->p.client_config.client_session_cache, key);
    if (cs == NULL)
        testing_t_fatal_v(r->p.t, "the session cache is empty");
    return cs;
}

/* getTicket. Not in Go: the ticket is copied into r->p.ar, since the session it
 * belongs to can be gone after the next handshake. */
static Slice get_ticket(Resumption *r) {
    Str key;
    TlsClientSessionState *cs = resumption_front(r, &key);
    Slice ticket = bytes_clone(arena_allocator(&r->p.ar), cs->session->ticket);
    if (ticket.p == NULL && cs->session->ticket.len > 0)
        testing_t_fatal_v(r->p.t, "out of memory");
    return ticket;
}

static void delete_ticket(Resumption *r) {
    Str key;
    resumption_front(r, &key);
    Str ticket_key = str_clone(arena_allocator(&r->p.ar), key);
    if (ticket_key.len != key.len)
        testing_t_fatal_v(r->p.t, "out of memory");
    tls_client_session_cache_put(r->p.client_config.client_session_cache, ticket_key,
                                 NULL);
}

static void corrupt_ticket(Resumption *r) {
    Str key;
    TlsClientSessionState *cs = resumption_front(r, &key);
    ((Byte *)cs->session->secret.p)[0] ^= 0xff;
}

static void random_key(Resumption *r, Byte k[32]) {
    Error err = BURROW_NO_ERROR;
    (void)io_read_full(burrow__tls_config_rand(&r->p.server_config),
                       slice_from(k, 32, 32, TYPE_BYTE), &err);
    if (BURROW_FAILED(err))
        testing_t_fatalf_v(r->p.t, "Failed to read new SessionTicketKey: %s", err);
}

/* Not in Go: serverConfig.SetSessionTicketKeys with the first n of r->keys,
 * which can run out of memory. */
static void resumption_set_session_ticket_keys(Resumption *r, Int n) {
    Error err = tls_config_set_session_ticket_keys(&r->p.server_config,
                                                   (const Byte(*)[32])r->keys, n);
    if (BURROW_FAILED(err))
        testing_t_fatal_v(r->p.t, err);
}

static void test_resumption(TestingT *t, uint16_t version) {
    if (testing_short())
        testing_t_skip_v(t, "skipping in -short mode");

    Resumption *r = BURROW_NEW(heap_allocator(), Resumption);
    if (r == NULL)
        testing_t_fatal_v(t, "out of memory");
    memset(r, 0, sizeof *r);
    handshake_pair_init(&r->p, t);
    r->version = version;
    arena_init(&r->serializing.ar, NULL, 0);
    testing_t_cleanup(t, BURROW_FN(Func, resumption_free, r));
    Alloc *a = arena_allocator(&r->p.ar);

    r->test_certificates[0] =
        parse_test_cert(a, test_rsa2048_cert_pem, test_rsa2048_key_pem);
    r->server_cipher_suites[0] = TLS_TLS_ECDHE_RSA_WITH_AES_128_GCM_SHA256;
    r->server_cipher_suites[1] = TLS_TLS_ECDHE_RSA_WITH_AES_256_GCM_SHA384;
    resumption_server_config(r);

    r->client_cipher_suites[0] = TLS_TLS_ECDHE_RSA_WITH_AES_128_GCM_SHA256;
    TlsConfig *client_config = &r->p.client_config;
    client_config->max_version = version;
    client_config->cipher_suites =
        slice_from(r->client_cipher_suites, 1, 1, TYPE_UINT16);
    client_config->client_session_cache = handshake_pair_lru(&r->p);
    client_config->root_cas =
        new_test_cert_pool(t, a, test_root_cert_pem, test_root_key_pem);
    client_config->server_name = BURROW_S("test.golang.example");
    client_config->time = (TlsTimeFunc){test_time, NULL};

    TlsConfig *server_config = &r->p.server_config;
    test_resume_state(r, "Handshake", false);
    Slice ticket = get_ticket(r);
    test_resume_state(r, "Resume", true);
    if (bytes_equal(ticket, get_ticket(r)))
        testing_t_fatal_v(t, "ticket didn't change after resumption");

    /* An old session ticket is replaced with a ticket encrypted with a fresh
     * key. */
    ticket = get_ticket(r);
    r->d = 24 * TIME_HOUR + TIME_MINUTE;
    server_config->time = (TlsTimeFunc){resumption_time, r};
    test_resume_state(r, "ResumeWithOldTicket", true);
    if (bytes_equal(ticket, get_ticket(r)))
        testing_t_fatal_v(t, "old first ticket matches the fresh one");

    /* Once the session master secret is expired, a full handshake should
     * occur. */
    ticket = get_ticket(r);
    r->d = 24 * 8 * TIME_HOUR + TIME_MINUTE;
    test_resume_state(r, "ResumeWithExpiredTicket", false);
    if (bytes_equal(ticket, get_ticket(r)))
        testing_t_fatal_v(t, "expired first ticket matches the fresh one");

    server_config->time = (TlsTimeFunc){test_time, NULL}; /* reset the time back */
    Byte key1[32], key2[32];
    random_key(r, key1);
    memcpy(r->keys[0], key1, 32);
    resumption_set_session_ticket_keys(r, 1);

    test_resume_state(r, "InvalidSessionTicketKey", false);
    test_resume_state(r, "ResumeAfterInvalidSessionTicketKey", true);

    random_key(r, key2);
    memcpy(r->keys[0], key2, 32);
    memcpy(r->keys[1], key1, 32);
    resumption_set_session_ticket_keys(r, 2);
    ticket = get_ticket(r);
    test_resume_state(r, "KeyChange", true);
    if (bytes_equal(ticket, get_ticket(r)))
        testing_t_fatal_v(t, "new ticket wasn't included while resuming");
    test_resume_state(r, "KeyChangeFinish", true);

    /* Age the session ticket a bit, but not yet expired. */
    r->d = 24 * TIME_HOUR + TIME_MINUTE;
    server_config->time = (TlsTimeFunc){resumption_time, r};
    test_resume_state(r, "OldSessionTicket", true);
    ticket = get_ticket(r);
    /* Expire the session ticket, which would force a full handshake. */
    r->d = 24 * 8 * TIME_HOUR + 2 * TIME_MINUTE;
    test_resume_state(r, "ExpiredSessionTicket", false);
    if (bytes_equal(ticket, get_ticket(r)))
        testing_t_fatal_v(t, "new ticket wasn't provided after old ticket expired");

    /* Age the session ticket a bit at a time, but don't expire it. */
    r->d = 0 * TIME_HOUR;
    delete_ticket(r);
    test_resume_state(r, "GetFreshSessionTicket", false);
    for (int i = 0; i < 13; i++) {
        r->d += 12 * TIME_HOUR;
        test_resume_state(r, "OldSessionTicket", true);
    }
    /* Expire it (now a little more than 7 days) and make sure a full
     * handshake occurs for TLS 1.2. Resumption should still occur for TLS 1.3
     * since the client should be using a fresh ticket sent over by the
     * server. */
    r->d += 12 * TIME_HOUR + TIME_MINUTE;
    if (version == TLS_VERSION_TLS13)
        test_resume_state(r, "ExpiredSessionTicket", true);
    else
        test_resume_state(r, "ExpiredSessionTicket", false);
    if (bytes_equal(ticket, get_ticket(r)))
        testing_t_fatal_v(t, "new ticket wasn't provided after old ticket expired");

    /* Reset serverConfig to ensure that calling SetSessionTicketKeys before
     * the serverConfig is used works. */
    resumption_server_config(r);
    memcpy(r->keys[0], key2, 32);
    resumption_set_session_ticket_keys(r, 1);

    test_resume_state(r, "FreshConfig", true);

    /* In TLS 1.3, cross-cipher suite resumption is allowed as long as the KDF
     * hash matches. Also, Config.CipherSuites does not apply to TLS 1.3. */
    if (version != TLS_VERSION_TLS13) {
        r->client_cipher_suites[0] = TLS_TLS_ECDHE_RSA_WITH_AES_256_GCM_SHA384;
        test_resume_state(r, "DifferentCipherSuite", false);
        test_resume_state(r, "DifferentCipherSuiteRecovers", true);
    }

    delete_ticket(r);
    test_resume_state(r, "WithoutSessionTicket", false);

    /* In TLS 1.3, HelloRetryRequest is sent after incorrect key share. See
     * https://www.rfc-editor.org/rfc/rfc8446#page-14. */
    if (version == TLS_VERSION_TLS13) {
        delete_ticket(r);
        resumption_server_config(r);
        server_config->cipher_suites = slice_nil(TYPE_UINT16);
        /* Use a different curve than the client to force a
         * HelloRetryRequest. */
        r->curve_preferences[0] = TLS_CURVE_P521;
        r->curve_preferences[1] = TLS_CURVE_P384;
        r->curve_preferences[2] = TLS_CURVE_P256;
        server_config->curve_preferences =
            slice_from(r->curve_preferences, 3, 3, TYPE_UINT16);
        test_resume_state(r, "InitialHandshake", false);
        test_resume_state(r, "WithHelloRetryRequest", true);

        /* Reset serverConfig back. */
        resumption_server_config(r);
    }

    /* Session resumption should work when using client certificates */
    delete_ticket(r);
    server_config->client_cas =
        new_test_cert_pool(t, a, test_client_root_cert_pem, test_client_root_key_pem);
    server_config->client_auth = TLS_REQUIRE_AND_VERIFY_CLIENT_CERT;
    r->client_certificates[0] =
        parse_test_cert(a, test_client_rsa2048_cert_pem, test_client_rsa2048_key_pem);
    client_config->certificates =
        slice_from(r->client_certificates, 1, 1, TYPE_TLS_CERTIFICATE);
    test_resume_state(r, "InitialHandshake", false);
    test_resume_state(r, "WithClientCertificates", true);
    server_config->client_auth = TLS_NO_CLIENT_CERT;

    /* Tickets should be removed from the session cache on TLS handshake
     * failure, and the client should recover from a corrupted PSK */
    test_resume_state(r, "FetchTicketToCorrupt", false);
    corrupt_ticket(r);
    handshake_pair_run(&r->p);
    if (BURROW_OK(r->p.h.err))
        testing_t_fatalf_v(t, "handshake did not fail with a corrupted client secret");
    test_resume_state(r, "AfterHandshakeFailure", false);

    client_config->client_session_cache = (TlsClientSessionCache){NULL, NULL};
    test_resume_state(r, "WithoutSessionCache", false);

    r->serializing.t = t;
    client_config->client_session_cache =
        (TlsClientSessionCache){&serializing_client_cache_vt, &r->serializing};
    test_resume_state(r, "BeforeSerializingCache", false);
    test_resume_state(r, "WithSerializingCache", true);
}

static void test_resumption_run(void *env, TestingT *t) {
    test_resumption(t, *(const uint16_t *)env);
}

static void TestResumption(TestingT *t) {
    uint16_t tls12 = TLS_VERSION_TLS12, tls13 = TLS_VERSION_TLS13;
    testing_t_run(t, BURROW_S("TLSv12"),
                  BURROW_FN(TestingTFunc, test_resumption_run, &tls12));
    testing_t_run(t, BURROW_S("TLSv13"),
                  BURROW_FN(TestingTFunc, test_resumption_run, &tls13));
}

static void TestHandshakeClientALPNMatch(TestingT *t) {
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    TlsConfig *config = test_config_client_clone(t, a);
    Str protos[] = {BURROW_S_INIT("proto2"), BURROW_S_INIT("proto1")};
    config->next_protos = slice_from(protos, 2, 2, TYPE_STRING);
    ClientTest test = {
        .name = BURROW_S("ALPN"),
        .config = config,
        /* The server's preferences should override the client. */
        .validate = check_negotiated_protocol(a, BURROW_S("proto1")),
        .a = a,
    };
    run_client_test_tls12(t, &test);
    run_client_test_tls13(t, &test);
    client_test_free(&test, &ar);
}

static const char scts_base64[] =
    "ABIBaQFnAHUApLkJkLQYWBSHuxOizGdwCjw1mAT5G9+443fNDsgN3BAAAAFHl5nuFgAABAMARjBEAiAc"
    "S4JdlW5nW9sElUv2zvQyPoZ6ejKrGGB03gjaBZFMLwIgc1Qbbn+hsH0RvObzhS+XZhr3iuQQJY8S9G85"
    "D9KeGPAAdgBo9pj4H2SCvjqM7rkoHUz8cVFdZ5PURNEKZ6y7T0/7xAAAAUeX4bVwAAAEAwBHMEUCIDIh"
    "FDgG2HIuADBkGuLobU5a4dlCHoJLliWJ1SYT05z6AiEAjxIoZFFPRNWMGGIjskOTMwXzQ1Wh2e7NxXE1"
    "kd1J0QsAdgDuS723dc5guuFCaR+r4Z5mow9+X7By2IMAxHuJeqj9ywAAAUhcZIqHAAAEAwBHMEUCICmJ"
    "1rBT09LpkbzxtUC+Hi7nXLR0J+2PmwLp+sJMuqK+AiEAr0NkUnEVKVhAkccIFpYDqHOlZaBsuEhWWrYp"
    "g2RtKp0=";

static Error scts_check(void *env, const TlsConnectionState *state) {
    const Slice *scts = env;
    Slice expected_scts[3] = {
        slice_sub(*scts, 8, 125),
        slice_sub(*scts, 127, 245),
        slice_sub(*scts, 247, scts->len),
    };
    Int n = state->signed_certificate_timestamps.len;
    if (n != 3)
        return fmt_errorf_v("Got %d scts, wanted %d", n, 3);
    const Slice *got = state->signed_certificate_timestamps.p;
    for (int i = 0; i < 3; i++) {
        if (!bytes_equal(got[i], expected_scts[i]))
            return fmt_errorf_v("SCT #%d contained %x, expected %x", i, got[i],
                                expected_scts[i]);
    }
    return BURROW_NO_ERROR;
}

static void TestHandshakeClientSCTs(TestingT *t) {
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    Error err = BURROW_NO_ERROR;
    Slice scts = base64_encoding_decode_string(base64_std_encoding, a,
                                               str_from_cstr(scts_base64), &err);
    if (BURROW_FAILED(err)) {
        arena_free(&ar);
        testing_t_fatal_v(t, err);
    }

    ClientTest test = {
        .name = BURROW_S("SCT"),
        .validate = BURROW_FN(ConnectionStateCheck, scts_check, &scts),
        .a = a,
    };
    run_client_test_tls12(t, &test);

    /* TLS 1.3 moved SCTs to the Certificate extensions and -serverinfo only
     * supports ServerHello extensions. */
    client_test_free(&test, &ar);
}

static Error renegotiation_rejected_check(void *env, int renegotiation_num, Error err) {
    (void)env;
    (void)renegotiation_num;
    if (!BURROW_FAILED(err))
        return errors_new(heap_allocator(),
                          BURROW_S("expected error from renegotiation but got nil"));
    if (!strings_contains(error_text(err), BURROW_S("no renegotiation")))
        return fmt_errorf_v("expected renegotiation to be rejected but got %q",
                            error_text(err));
    return BURROW_NO_ERROR;
}

static void TestRenegotiationRejected(TestingT *t) {
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    ClientTest test = {
        .name = BURROW_S("RenegotiationRejected"),
        .num_renegotiations = 1,
        .renegotiation_expected_to_fail = 1,
        .check_renegotiation_error =
            BURROW_FN(RenegotiationErrorCheck, renegotiation_rejected_check, NULL),
        .a = a,
    };
    run_client_test_tls12(t, &test);
    client_test_free(&test, &ar);
}

static void TestRenegotiateOnce(TestingT *t) {
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    TlsConfig *config = test_config_client_clone(t, a);
    config->renegotiation = TLS_RENEGOTIATE_ONCE_AS_CLIENT;

    ClientTest test = {
        .name = BURROW_S("RenegotiateOnce"),
        .config = config,
        .num_renegotiations = 1,
        .a = a,
    };

    run_client_test_tls12(t, &test);
    client_test_free(&test, &ar);
}

static void TestRenegotiateTwice(TestingT *t) {
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    TlsConfig *config = test_config_client_clone(t, a);
    config->renegotiation = TLS_RENEGOTIATE_FREELY_AS_CLIENT;

    ClientTest test = {
        .name = BURROW_S("RenegotiateTwice"),
        .config = config,
        .num_renegotiations = 2,
        .a = a,
    };

    run_client_test_tls12(t, &test);
    client_test_free(&test, &ar);
}

static Error renegotiate_twice_rejected_check(void *env, int renegotiation_num,
                                              Error err) {
    (void)env;
    if (renegotiation_num == 1)
        return err;

    if (!BURROW_FAILED(err))
        return errors_new(heap_allocator(),
                          BURROW_S("expected error from renegotiation but got nil"));
    if (!strings_contains(error_text(err), BURROW_S("no renegotiation")))
        return fmt_errorf_v("expected renegotiation to be rejected but got %q",
                            error_text(err));
    return BURROW_NO_ERROR;
}

static void TestRenegotiateTwiceRejected(TestingT *t) {
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    TlsConfig *config = test_config_client_clone(t, a);
    config->renegotiation = TLS_RENEGOTIATE_ONCE_AS_CLIENT;

    ClientTest test = {
        .name = BURROW_S("RenegotiateTwiceRejected"),
        .config = config,
        .num_renegotiations = 2,
        .renegotiation_expected_to_fail = 2,
        .check_renegotiation_error =
            BURROW_FN(RenegotiationErrorCheck, renegotiate_twice_rejected_check, NULL),
        .a = a,
    };

    run_client_test_tls12(t, &test);
    client_test_free(&test, &ar);
}

static Error export_keying_material_check(void *env, const TlsConnectionState *state) {
    Alloc *a = env;
    Error err = BURROW_NO_ERROR;
    Slice km = tls_connection_state_export_keying_material(
        state, a, BURROW_S("test"), slice_nil(TYPE_BYTE), 42, &err);
    if (BURROW_FAILED(err))
        return fmt_errorf_v("ExportKeyingMaterial failed: %v", error_text(err));
    if (km.len != 42)
        return fmt_errorf_v("Got %d bytes from ExportKeyingMaterial, wanted %d", km.len,
                            42);
    return BURROW_NO_ERROR;
}

static void TestHandshakeClientExportKeyingMaterial(TestingT *t) {
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    ClientTest test = {
        .name = BURROW_S("ExportKeyingMaterial"),
        .validate = BURROW_FN(ConnectionStateCheck, export_keying_material_check, a),
        .a = a,
    };
    run_client_test_tls10(t, &test);
    run_client_test_tls12(t, &test);
    run_client_test_tls13(t, &test);
    client_test_free(&test, &ar);
}

static TlsClientSessionState *cache_get(TlsClientSessionCache cache, Str key,
                                        bool *ok) {
    return cache.vt->get(cache.data, key, ok);
}

static void cache_put(TlsClientSessionCache cache, Str key, TlsClientSessionState *cs) {
    cache.vt->put(cache.data, key, cs);
}

static void TestLRUClientSessionCache(TestingT *t) {
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);

    /* Initialize cache of capacity 4. */
    TlsClientSessionCache cache = tls_new_lru_client_session_cache(a, 4);
    if (cache.vt == NULL)
        testing_t_fatal_v(t, "NewLRUClientSessionCache: out of memory");
    TlsClientSessionState cs[6] = {{0}};
    const Str keys[] = {BURROW_S("0"), BURROW_S("1"), BURROW_S("2"), BURROW_S("3"),
                        BURROW_S("4"), BURROW_S("5"), BURROW_S("6")};
    TlsClientSessionState *s;
    bool ok;

    /* Add 4 entries to the cache and look them up. */
    for (int i = 0; i < 4; i++)
        cache_put(cache, keys[i], &cs[i]);
    for (int i = 0; i < 4; i++) {
        s = cache_get(cache, keys[i], &ok);
        if (!ok || s != &cs[i])
            testing_t_fatalf_v(t, "session cache failed lookup for added key: %s",
                               keys[i]);
    }

    /* Add 2 more entries to the cache. First 2 should be evicted. */
    for (int i = 4; i < 6; i++)
        cache_put(cache, keys[i], &cs[i]);
    for (int i = 0; i < 2; i++) {
        s = cache_get(cache, keys[i], &ok);
        if (ok || s != NULL)
            testing_t_fatalf_v(t, "session cache should have evicted key: %s", keys[i]);
    }

    /* Touch entry 2. LRU should evict 3 next. */
    cache_get(cache, keys[2], &ok);
    cache_put(cache, keys[0], &cs[0]);
    s = cache_get(cache, keys[3], &ok);
    if (ok || s != NULL)
        testing_t_fatalf_v(t, "session cache should have evicted key 3");

    /* Update entry 0 in place. */
    cache_put(cache, keys[0], &cs[3]);
    s = cache_get(cache, keys[0], &ok);
    if (!ok || s != &cs[3])
        testing_t_fatalf_v(t, "session cache failed update for key 0");

    /* Calling Put with a nil entry deletes the key. */
    cache_put(cache, keys[0], NULL);
    cache_get(cache, keys[0], &ok);
    if (ok)
        testing_t_fatalf_v(t, "session cache failed to delete key 0");

    /* Delete entry 2. LRU should keep 4 and 5 */
    cache_put(cache, keys[2], NULL);
    cache_get(cache, keys[2], &ok);
    if (ok)
        testing_t_fatalf_v(t, "session cache failed to delete key 4");
    for (int i = 4; i < 6; i++) {
        s = cache_get(cache, keys[i], &ok);
        if (!ok || s != &cs[i])
            testing_t_fatalf_v(t, "session cache should not have deleted key: %s",
                               keys[i]);
    }
    arena_free(&ar);
}

/* Not in Go: what TestKeyLogTLS12 and TestKeyLogTLS13 hold. It is on the heap,
 * and a cleanup gives it back with everything in it, so a Fatalf leaks
 * nothing. The buffers are on the heap too, since the server writes its own
 * from another goroutine. */
typedef struct KeyLogTest {
    TestingT *t;
    BytesBuffer server_buf, client_buf;
    TlsConfig client_config;
    TlsConfig server_config;
    NetConn c, s;
    bool piped;
    TlsConn *client;
    TlsConn *server;
    Arena ar;
} KeyLogTest;

static void key_log_test_free(void *env) {
    KeyLogTest *k = env;
    tls_conn_free(k->client);
    tls_conn_free(k->server);
    if (k->piped)
        local_pipe_free(k->c);
    bytes_buffer_free(&k->server_buf);
    bytes_buffer_free(&k->client_buf);
    tls_config_release(&k->client_config);
    tls_config_release(&k->server_config);
    arena_free(&k->ar);
    mem_free(heap_allocator(), k, sizeof *k, _Alignof(KeyLogTest));
}

/* Not in Go: the two buffers, and the clones of testConfigClient and
 * testConfigServer that write their keys to them. */
static KeyLogTest *key_log_test_new(TestingT *t) {
    KeyLogTest *k = BURROW_NEW(heap_allocator(), KeyLogTest);
    if (k == NULL)
        testing_t_fatal_v(t, "out of memory");
    memset(k, 0, sizeof *k);
    k->t = t;
    arena_init(&k->ar, NULL, 0);
    testing_t_cleanup(t, BURROW_FN(Func, key_log_test_free, k));
    Alloc *a = arena_allocator(&k->ar);
    k->server_buf = BYTES_BUFFER(heap_allocator());
    k->client_buf = BYTES_BUFFER(heap_allocator());

    k->client_config = test_config_client(a);
    k->client_config.key_log_writer = bytes_buffer_as_io_writer(&k->client_buf);

    k->server_config = test_config_server(a);
    k->server_config.key_log_writer = bytes_buffer_as_io_writer(&k->server_buf);
    return k;
}

static void key_log_server(void *env) {
    KeyLogTest *k = env;
    Error err = tls_conn_handshake(k->server);
    if (BURROW_FAILED(err)) {
        testing_t_errorf_v(k->t, "server: %s", err);
        return;
    }
    (void)k->s.vt->closer.close(k->s.data);
}

/* Not in Go: the handshake both tests do, from localPipe to <-done. */
static void key_log_handshake(KeyLogTest *k) {
    TestingT *t = k->t;
    local_pipe(arena_allocator(&k->ar), &k->c, &k->s);
    k->piped = true;
    k->server = tls_server(heap_allocator(), k->s, &k->server_config);
    k->client = tls_client(heap_allocator(), k->c, &k->client_config);
    if (k->server == NULL || k->client == NULL)
        testing_t_fatal_v(t, "out of memory");
    SyncWaitGroup done = {0};
    sync_wait_group_go(&done, BURROW_FN(Func, key_log_server, k));

    Error err = tls_conn_handshake(k->client);
    if (BURROW_FAILED(err)) {
        /* Not in Go: the server goroutine is waited for first, since the
         * cleanup frees what it uses. */
        (void)k->c.vt->closer.close(k->c.data);
        sync_wait_group_wait(&done);
        testing_t_fatalf_v(t, "client: %s", err);
    }

    (void)k->c.vt->closer.close(k->c.data);
    sync_wait_group_wait(&done);
}

static void check_keylog_line(TestingT *t, Alloc *a, const char *side,
                              Str logged_line) {
    if (logged_line.len == 0)
        testing_t_fatalf_v(t, "%s: no keylog line was produced", side);
    bool ok;
    Str rest = strings_cut_suffix(logged_line, BURROW_S("\n"), &ok);
    if (!ok)
        testing_t_fatalf_v(t, "%s: keylog line is missing trailing newline: %q", side,
                           logged_line);
    Str label = strings_cut(rest, BURROW_S(" "), &rest, &ok);
    if (!ok || !str_eq(label, BURROW_S("CLIENT_RANDOM")))
        testing_t_fatalf_v(t, "%s: keylog line has incorrect label: %q", side,
                           logged_line);
    Str master_secret;
    Str client_random = strings_cut(rest, BURROW_S(" "), &master_secret, &ok);
    if (!ok)
        testing_t_fatalf_v(t, "%s: keylog line is missing master secret: %q", side,
                           logged_line);
    Error err = BURROW_NO_ERROR;
    Slice b = hex_decode_string(a, client_random, &err);
    if (BURROW_FAILED(err) || b.len != 32)
        testing_t_fatalf_v(t, "%s: keylog line has invalid client random: %q", side,
                           logged_line);
    b = hex_decode_string(a, master_secret, &err);
    if (BURROW_FAILED(err) || b.len != 48)
        testing_t_fatalf_v(t, "%s: keylog line has invalid master secret: %q", side,
                           logged_line);
}

static void TestKeyLogTLS12(TestingT *t) {
    KeyLogTest *k = key_log_test_new(t);
    Alloc *a = arena_allocator(&k->ar);
    k->client_config.max_version = TLS_VERSION_TLS12;
    k->server_config.max_version = TLS_VERSION_TLS12;

    key_log_handshake(k);

    check_keylog_line(t, a, "client", bytes_buffer_string(&k->client_buf, a));
    check_keylog_line(t, a, "server", bytes_buffer_string(&k->server_buf, a));
}

static void check_keylog_lines(TestingT *t, Alloc *a, const char *side,
                               Str logged_lines) {
    logged_lines = strings_trim_space(logged_lines);
    Slice lines = strings_split(a, logged_lines, BURROW_S("\n"));
    if (lines.len != 4)
        testing_t_errorf_v(t, "Expected the %s to log 4 lines, got %d", side,
                           lines.len);
}

static void TestKeyLogTLS13(TestingT *t) {
    KeyLogTest *k = key_log_test_new(t);
    Alloc *a = arena_allocator(&k->ar);

    key_log_handshake(k);

    check_keylog_lines(t, a, "client", bytes_buffer_string(&k->client_buf, a));
    check_keylog_lines(t, a, "server", bytes_buffer_string(&k->server_buf, a));
}

/* Not in Go: a Client handshake run on its own goroutine. */
typedef struct ClientJob {
    TlsConn *c;
    Error err;
} ClientJob;

static void client_handshake_job(void *env) {
    ClientJob *j = (ClientJob *)env;
    j->err = tls_conn_handshake(j->c);
}

/* Not in Go. The record the client sends first, read whole from s into a. */
static Slice read_record(TestingT *t, Alloc *a, NetConn s) {
    Byte header[5];
    Error err = BURROW_NO_ERROR;
    io_read_full((IoReader){&s.vt->reader, s.data}, slice_from(header, 5, 5, TYPE_BYTE),
                 &err);
    if (BURROW_FAILED(err))
        testing_t_fatal_v(t, "%v", err);
    Int record_len = (Int)header[3] << 8 | (Int)header[4];

    Slice record = slice_make(a, TYPE_BYTE, record_len, record_len);
    io_read_full((IoReader){&s.vt->reader, s.data}, record, &err);
    if (BURROW_FAILED(err))
        testing_t_fatal_v(t, "%v", err);
    return record;
}

static void TestServerSelectingUnconfiguredApplicationProtocol(TestingT *t) {
    /* This checks that the server can't select an application protocol that
     * the client didn't offer. */

    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    NetConn c, s;
    net_pipe(a, &c, &s);

    uint16_t suites[1] = {TLS_TLS_ECDHE_RSA_WITH_AES_128_GCM_SHA256};
    Str protos[2] = {BURROW_S_INIT("http"), BURROW_S_INIT("something-else")};
    TlsConfig config;
    memset(&config, 0, sizeof config);
    config.server_name = BURROW_S("foo");
    config.cipher_suites = slice_from(suites, 1, 1, TYPE_UINT16);
    config.next_protos = slice_from(protos, 2, 2, TYPE_STRING);
    ClientJob job = {tls_client(heap_allocator(), c, &config), BURROW_NO_ERROR};
    if (job.c == NULL)
        testing_t_fatal_v(t, "out of memory");
    SyncWaitGroup wg = {0};
    sync_wait_group_go(&wg, BURROW_FN(Func, client_handshake_job, &job));

    (void)read_record(t, a, s);

    TlsServerHelloMsg server_hello = burrow__tls_server_hello_msg_zero();
    server_hello.vers = TLS_VERSION_TLS12;
    server_hello.random = slice_make(a, TYPE_BYTE, 32, 32);
    server_hello.cipher_suite = TLS_TLS_ECDHE_RSA_WITH_AES_128_GCM_SHA256;
    server_hello.alpn_protocol = BURROW_S("how-about-this");
    Error err = BURROW_NO_ERROR;
    Slice server_hello_bytes =
        burrow__tls_server_hello_msg_marshal(&server_hello, a, &err);
    if (BURROW_FAILED(err))
        testing_t_fatal_v(t, "%v", err);

    Byte header[5] = {
        TLS_RECORD_TYPE_HANDSHAKE,        (Byte)(TLS_VERSION_TLS12 >> 8),
        (Byte)(TLS_VERSION_TLS12 & 0xff), (Byte)(server_hello_bytes.len >> 8),
        (Byte)server_hello_bytes.len,
    };
    (void)s.vt->writer.write(s.data, slice_from(header, 5, 5, TYPE_BYTE), NULL);
    (void)s.vt->writer.write(s.data, server_hello_bytes, NULL);
    (void)s.vt->closer.close(s.data);

    sync_wait_group_wait(&wg);
    if (!strings_contains(error_text(job.err),
                          BURROW_S("server selected unadvertised ALPN protocol")))
        testing_t_errorf_v(t,
                           "Expected error about unconfigured ALPN protocol but got %q",
                           error_text(job.err));

    tls_conn_free(job.c);
    (void)c.vt->closer.close(c.data);
    net_pipe_free(c);
    arena_free(&ar);
}

typedef struct HostnameInSNITest {
    Str in, out;
} HostnameInSNITest;

static const HostnameInSNITest hostname_in_sni_tests[] = {
    /* Opaque string */
    {BURROW_S_INIT(""), BURROW_S_INIT("")},
    {BURROW_S_INIT("localhost"), BURROW_S_INIT("localhost")},
    {BURROW_S_INIT("foo, bar, baz and qux"), BURROW_S_INIT("foo, bar, baz and qux")},

    /* DNS hostname */
    {BURROW_S_INIT("golang.org"), BURROW_S_INIT("golang.org")},
    {BURROW_S_INIT("golang.org."), BURROW_S_INIT("golang.org")},

    /* Literal IPv4 address */
    {BURROW_S_INIT("1.2.3.4"), BURROW_S_INIT("")},

    /* Literal IPv6 address */
    {BURROW_S_INIT("::1"), BURROW_S_INIT("")},
    /* with zone identifier */
    {BURROW_S_INIT("::1%lo0"), BURROW_S_INIT("")},
    /* as per RFC 5952 we allow the [] style as IPv6 literal */
    {BURROW_S_INIT("[::1]"), BURROW_S_INIT("")},
    {BURROW_S_INIT("[::1%lo0]"), BURROW_S_INIT("")},
};

/* localPipe is a net.Pipe. Go leaves the client goroutine running, and here it
 * is waited for before its Conn is freed. */
static void TestHostnameInSNI(TestingT *t) {
    Alloc *heap = heap_allocator();
    for (size_t i = 0;
         i < sizeof hostname_in_sni_tests / sizeof hostname_in_sni_tests[0]; i++) {
        const HostnameInSNITest *tt = &hostname_in_sni_tests[i];
        Arena ar;
        arena_init(&ar, NULL, 0);
        Alloc *a = arena_allocator(&ar);
        NetConn c, s;
        net_pipe(a, &c, &s);

        TlsConfig config;
        memset(&config, 0, sizeof config);
        config.server_name = tt->in;
        config.insecure_skip_verify = true;
        ClientJob job = {tls_client(heap, c, &config), BURROW_NO_ERROR};
        if (job.c == NULL)
            testing_t_fatal_v(t, "out of memory");
        SyncWaitGroup wg = {0};
        sync_wait_group_go(&wg, BURROW_FN(Func, client_handshake_job, &job));

        Slice record = read_record(t, a, s);

        (void)c.vt->closer.close(c.data);
        (void)s.vt->closer.close(s.data);
        sync_wait_group_wait(&wg);
        tls_conn_free(job.c);

        TlsClientHelloMsg m = burrow__tls_client_hello_msg_zero();
        if (!burrow__tls_client_hello_msg_unmarshal(&m, a, record)) {
            testing_t_errorf_v(t, "unmarshaling ClientHello for %q failed", tt->in);
        } else {
            if (!str_eq(tt->in, tt->out) && str_eq(m.server_name, tt->in))
                testing_t_errorf_v(t, "prohibited %q found in ClientHello: %x", tt->in,
                                   record);
            if (!str_eq(m.server_name, tt->out))
                testing_t_errorf_v(t, "expected %q not found in ClientHello: %x",
                                   tt->out, record);
        }
        net_pipe_free(c);
        arena_free(&ar);
    }
}

static void TestServerSelectingUnconfiguredCipherSuite(TestingT *t) {
    /* This checks that the server can't select a cipher suite that the client
     * didn't offer. See #13174. */

    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    NetConn c, s;
    net_pipe(a, &c, &s);

    uint16_t suites[1] = {TLS_TLS_RSA_WITH_AES_128_GCM_SHA256};
    TlsConfig config;
    memset(&config, 0, sizeof config);
    config.server_name = BURROW_S("foo");
    config.cipher_suites = slice_from(suites, 1, 1, TYPE_UINT16);
    ClientJob job = {tls_client(heap_allocator(), c, &config), BURROW_NO_ERROR};
    if (job.c == NULL)
        testing_t_fatal_v(t, "out of memory");
    SyncWaitGroup wg = {0};
    sync_wait_group_go(&wg, BURROW_FN(Func, client_handshake_job, &job));

    (void)read_record(t, a, s);

    /* Create a ServerHello that selects a different cipher suite than the sole
     * one that the client offered. */
    TlsServerHelloMsg server_hello = burrow__tls_server_hello_msg_zero();
    server_hello.vers = TLS_VERSION_TLS12;
    server_hello.random = slice_make(a, TYPE_BYTE, 32, 32);
    server_hello.cipher_suite = TLS_TLS_RSA_WITH_AES_256_GCM_SHA384;
    Error err = BURROW_NO_ERROR;
    Slice server_hello_bytes =
        burrow__tls_server_hello_msg_marshal(&server_hello, a, &err);
    if (BURROW_FAILED(err))
        testing_t_fatal_v(t, "%v", err);

    Byte header[5] = {
        TLS_RECORD_TYPE_HANDSHAKE,        (Byte)(TLS_VERSION_TLS12 >> 8),
        (Byte)(TLS_VERSION_TLS12 & 0xff), (Byte)(server_hello_bytes.len >> 8),
        (Byte)server_hello_bytes.len,
    };
    (void)s.vt->writer.write(s.data, slice_from(header, 5, 5, TYPE_BYTE), NULL);
    (void)s.vt->writer.write(s.data, server_hello_bytes, NULL);
    (void)s.vt->closer.close(s.data);

    sync_wait_group_wait(&wg);
    if (!strings_contains(error_text(job.err), BURROW_S("unconfigured cipher")))
        testing_t_errorf_v(t,
                           "Expected error about unconfigured cipher suite but got %q",
                           error_text(job.err));

    tls_conn_free(job.c);
    (void)c.vt->closer.close(c.data);
    net_pipe_free(c);
    arena_free(&ar);
}

/* Not in Go: what the closures of testVerifyConnection capture, the version
 * and the count of calls to one side's VerifyConnection. */
typedef struct VerifyConnectionEnv {
    uint16_t version;
    int called;
} VerifyConnectionEnv;

/* checkFields. */
static Error verify_connection_check_fields(const TlsConnectionState *c,
                                            const VerifyConnectionEnv *e,
                                            const char *error_type) {
    if (c->version != e->version)
        return fmt_errorf_v("%s: got Version %v, want %v", error_type, c->version,
                            e->version);
    if (c->handshake_complete)
        return fmt_errorf_v("%s: got HandshakeComplete, want false", error_type);
    if (!str_eq(c->server_name, BURROW_S("test.golang.example")))
        return fmt_errorf_v("%s: got ServerName %s, want %s", error_type,
                            c->server_name, "test.golang.example");
    if (!str_eq(c->negotiated_protocol, BURROW_S("protocol1")))
        return fmt_errorf_v("%s: got NegotiatedProtocol %s, want %s", error_type,
                            c->negotiated_protocol, "protocol1");
    if (c->cipher_suite == 0)
        return fmt_errorf_v("%s: got CipherSuite 0, want non-zero", error_type);
    bool want_did_resume = false;
    if (e->called == 2) /* if this is the second time, then it should be a resumption */
        want_did_resume = true;
    if (c->did_resume != want_did_resume)
        return fmt_errorf_v("%s: got DidResume %t, want %t", error_type, c->did_resume,
                            want_did_resume);
    return BURROW_NO_ERROR;
}

static Error verify_connection_server_verified(void *env, const TlsConnectionState *c) {
    VerifyConnectionEnv *e = env;
    e->called++;
    Int l = c->peer_certificates.len;
    if (l != 1)
        return fmt_errorf_v("server: got len(PeerCertificates) = %d, wanted 1", l);
    if (c->verified_chains.len == 0)
        return fmt_errorf_v("server: got len(VerifiedChains) = 0, wanted non-zero");
    return verify_connection_check_fields(c, e, "server");
}

static Error verify_connection_client_verified(void *env, const TlsConnectionState *c) {
    VerifyConnectionEnv *e = env;
    e->called++;
    Int l = c->peer_certificates.len;
    if (l != 1)
        return fmt_errorf_v("client: got len(PeerCertificates) = %d, wanted 1", l);
    if (c->verified_chains.len == 0)
        return fmt_errorf_v("client: got len(VerifiedChains) = 0, wanted non-zero");
    if (c->did_resume)
        return BURROW_NO_ERROR;
    /* The SCTs and OCSP Response are dropped on resumption. See
     * http://golang.org/issue/39075. */
    if (c->ocsp_response.len == 0)
        return fmt_errorf_v("client: got len(OCSPResponse) = 0, wanted non-zero");
    if (c->signed_certificate_timestamps.len == 0)
        return fmt_errorf_v(
            "client: got len(SignedCertificateTimestamps) = 0, wanted non-zero");
    return verify_connection_check_fields(c, e, "client");
}

static Error verify_connection_server_insecure(void *env, const TlsConnectionState *c) {
    VerifyConnectionEnv *e = env;
    e->called++;
    Int l = c->peer_certificates.len;
    if (l != 1)
        return fmt_errorf_v("server: got len(PeerCertificates) = %d, wanted 1", l);
    if (!slice_is_nil(c->verified_chains))
        return fmt_errorf_v("server: got Verified Chains %v, want nil",
                            c->verified_chains);
    return verify_connection_check_fields(c, e, "server");
}

static Error verify_connection_client_insecure(void *env, const TlsConnectionState *c) {
    VerifyConnectionEnv *e = env;
    e->called++;
    Int l = c->peer_certificates.len;
    if (l != 1)
        return fmt_errorf_v("client: got len(PeerCertificates) = %d, wanted 1", l);
    if (!slice_is_nil(c->verified_chains))
        return fmt_errorf_v("server: got Verified Chains %v, want nil",
                            c->verified_chains);
    if (c->did_resume)
        return BURROW_NO_ERROR;
    /* The SCTs and OCSP Response are dropped on resumption. See
     * http://golang.org/issue/39075. */
    if (c->ocsp_response.len == 0)
        return fmt_errorf_v("client: got len(OCSPResponse) = 0, wanted non-zero");
    if (c->signed_certificate_timestamps.len == 0)
        return fmt_errorf_v(
            "client: got len(SignedCertificateTimestamps) = 0, wanted non-zero");
    return verify_connection_check_fields(c, e, "client");
}

static Error verify_connection_server_fields(void *env, const TlsConnectionState *c) {
    VerifyConnectionEnv *e = env;
    e->called++;
    return verify_connection_check_fields(c, e, "server");
}

static Error verify_connection_client_fields(void *env, const TlsConnectionState *c) {
    VerifyConnectionEnv *e = env;
    e->called++;
    return verify_connection_check_fields(c, e, "client");
}

static void require_and_verify_server(TlsConfig *config, VerifyConnectionEnv *called) {
    config->client_auth = TLS_REQUIRE_AND_VERIFY_CLIENT_CERT;
    config->verify_connection =
        BURROW_FN(TlsVerifyConnectionFunc, verify_connection_server_verified, called);
}

static void require_and_verify_client(TlsConfig *config, VerifyConnectionEnv *called) {
    config->verify_connection =
        BURROW_FN(TlsVerifyConnectionFunc, verify_connection_client_verified, called);
}

static void insecure_skip_verify_server(TlsConfig *config,
                                        VerifyConnectionEnv *called) {
    config->client_auth = TLS_REQUIRE_ANY_CLIENT_CERT;
    config->insecure_skip_verify = true;
    config->verify_connection =
        BURROW_FN(TlsVerifyConnectionFunc, verify_connection_server_insecure, called);
}

static void insecure_skip_verify_client(TlsConfig *config,
                                        VerifyConnectionEnv *called) {
    config->insecure_skip_verify = true;
    config->verify_connection =
        BURROW_FN(TlsVerifyConnectionFunc, verify_connection_client_insecure, called);
}

static void no_client_cert_server(TlsConfig *config, VerifyConnectionEnv *called) {
    config->client_auth = TLS_NO_CLIENT_CERT;
    config->verify_connection =
        BURROW_FN(TlsVerifyConnectionFunc, verify_connection_server_fields, called);
}

static void no_client_cert_client(TlsConfig *config, VerifyConnectionEnv *called) {
    config->verify_connection =
        BURROW_FN(TlsVerifyConnectionFunc, verify_connection_client_fields, called);
}

static void request_client_cert_server(TlsConfig *config, VerifyConnectionEnv *called) {
    config->client_auth = TLS_REQUEST_CLIENT_CERT;
    config->verify_connection =
        BURROW_FN(TlsVerifyConnectionFunc, verify_connection_server_fields, called);
}

static void request_client_cert_client(TlsConfig *config, VerifyConnectionEnv *called) {
    config->certificates = slice_nil(TYPE_TLS_CERTIFICATE); /* clear the client cert */
    config->verify_connection =
        BURROW_FN(TlsVerifyConnectionFunc, verify_connection_client_verified, called);
}

typedef struct VerifyConnectionTest {
    const char *name;
    void (*configure_server)(TlsConfig *config, VerifyConnectionEnv *called);
    void (*configure_client)(TlsConfig *config, VerifyConnectionEnv *called);
} VerifyConnectionTest;

static const VerifyConnectionTest verify_connection_tests[] = {
    {"RequireAndVerifyClientCert", require_and_verify_server,
     require_and_verify_client},
    {"InsecureSkipVerify", insecure_skip_verify_server, insecure_skip_verify_client},
    {"NoClientCert", no_client_cert_server, no_client_cert_client},
    {"RequestClientCert", request_client_cert_server, request_client_cert_client},
};

/* Not in Go: what one test of testVerifyConnection holds. */
typedef struct VerifyConnection {
    HandshakePair p;
    VerifyConnectionEnv server_called, client_called;
    TlsCertificate server_certificates[1];
    TlsCertificate client_certificates[1];
    Slice scts[2];
    Str next_protos[1];
} VerifyConnection;

static void verify_connection_free(void *env) {
    VerifyConnection *v = env;
    handshake_pair_release(&v->p);
    mem_free(heap_allocator(), v, sizeof *v, _Alignof(VerifyConnection));
}

static void test_handshake_state(VerifyConnection *v, Str name, bool did_resume) {
    TestingT *t = v->p.t;
    handshake_pair_run(&v->p);
    const TlsConnectionState *hs = &v->p.h.client_state;
    if (BURROW_FAILED(v->p.h.err))
        testing_t_fatalf_v(t, "%s: handshake failed: %s", name, v->p.h.err);
    if (hs->did_resume != did_resume)
        testing_t_errorf_v(t, "%s: resumed: %v, expected: %v", name, hs->did_resume,
                           did_resume);
    int want_called = 1;
    if (did_resume)
        want_called = 2; /* resumption would mean this is the second time it was
                            called in this test */
    if (v->client_called.called != want_called)
        testing_t_errorf_v(
            t, "%s: expected client VerifyConnection called %d times, did %d times",
            name, want_called, v->client_called.called);
    if (v->server_called.called != want_called)
        testing_t_errorf_v(
            t, "%s: expected server VerifyConnection called %d times, did %d times",
            name, want_called, v->server_called.called);
}

static void test_verify_connection(TestingT *t, uint16_t version) {
    for (size_t i = 0;
         i < sizeof verify_connection_tests / sizeof verify_connection_tests[0]; i++) {
        const VerifyConnectionTest *test = &verify_connection_tests[i];
        VerifyConnection *v = BURROW_NEW(heap_allocator(), VerifyConnection);
        if (v == NULL)
            testing_t_fatal_v(t, "out of memory");
        memset(v, 0, sizeof *v);
        handshake_pair_init(&v->p, t);
        testing_t_cleanup(t, BURROW_FN(Func, verify_connection_free, v));
        Alloc *a = arena_allocator(&v->p.ar);
        v->server_called.version = version;
        v->client_called.version = version;
        v->next_protos[0] = BURROW_S("protocol1");

        TlsConfig *server_config = &v->p.server_config;
        server_config->max_version = version;
        v->server_certificates[0] =
            parse_test_cert(a, test_rsa2048_cert_pem, test_rsa2048_key_pem);
        server_config->certificates =
            slice_from(v->server_certificates, 1, 1, TYPE_TLS_CERTIFICATE);
        server_config->time = (TlsTimeFunc){test_time, NULL};
        server_config->client_cas = new_test_cert_pool(t, a, test_client_root_cert_pem,
                                                       test_client_root_key_pem);
        server_config->next_protos = slice_from(v->next_protos, 1, 1, TYPE_STRING);
        static const char sct1[] = "dummy sct 1", sct2[] = "dummy sct 2",
                          ocsp[] = "dummy ocsp";
        v->scts[0] = slice_from((Byte *)(uintptr_t)sct1, 11, 11, TYPE_BYTE);
        v->scts[1] = slice_from((Byte *)(uintptr_t)sct2, 11, 11, TYPE_BYTE);
        v->server_certificates[0].signed_certificate_timestamps =
            slice_from(v->scts, 2, 2, TYPE_BYTES);
        v->server_certificates[0].ocsp_staple =
            slice_from((Byte *)(uintptr_t)ocsp, 10, 10, TYPE_BYTE);
        test->configure_server(server_config, &v->server_called);

        TlsConfig *client_config = &v->p.client_config;
        client_config->max_version = version;
        client_config->client_session_cache = handshake_pair_lru(&v->p);
        client_config->root_cas =
            new_test_cert_pool(t, a, test_root_cert_pem, test_root_key_pem);
        client_config->server_name = BURROW_S("test.golang.example");
        v->client_certificates[0] = parse_test_cert(a, test_client_rsa2048_cert_pem,
                                                    test_client_rsa2048_key_pem);
        client_config->certificates =
            slice_from(v->client_certificates, 1, 1, TYPE_TLS_CERTIFICATE);
        client_config->time = (TlsTimeFunc){test_time, NULL};
        client_config->next_protos = slice_from(v->next_protos, 1, 1, TYPE_STRING);
        test->configure_client(client_config, &v->client_called);

        test_handshake_state(v, fmt_sprintf_v(a, "%s-FullHandshake", test->name),
                             false);
        test_handshake_state(v, fmt_sprintf_v(a, "%s-Resumption", test->name), true);
    }
}

static void test_verify_connection_run(void *env, TestingT *t) {
    test_verify_connection(t, *(const uint16_t *)env);
}

static void TestVerifyConnection(TestingT *t) {
    uint16_t tls12 = TLS_VERSION_TLS12, tls13 = TLS_VERSION_TLS13;
    testing_t_run(t, BURROW_S("TLSv12"),
                  BURROW_FN(TestingTFunc, test_verify_connection_run, &tls12));
    testing_t_run(t, BURROW_S("TLSv13"),
                  BURROW_FN(TestingTFunc, test_verify_connection_run, &tls13));
}

/* Not in Go: sentinelErr is errors.New in Go. Here it is a sentinel made with
 * BURROW_SENTINEL_ERROR, which error_retain hands back as it is, so the copy
 * of the error a Conn keeps is still the same error. */
BURROW_SENTINEL_ERROR(verify_peer_certificate_sentinel_err,
                      "TestVerifyPeerCertificate");

/* Not in Go: err == sentinelErr. */
static bool is_sentinel_err(Error err) {
    return err.vt == verify_peer_certificate_sentinel_err.vt &&
           err.data == verify_peer_certificate_sentinel_err.data;
}

static Error verify_peer_certificate_callback(void *env, Slice raw_certs,
                                              Slice validated_chains) {
    bool *called = env;
    Int l = raw_certs.len;
    if (l != 1)
        return fmt_errorf_v("got len(rawCerts) = %d, wanted 1", l);
    if (validated_chains.len == 0)
        return errors_new(error_allocator(),
                          BURROW_S("got len(validatedChains) = 0, wanted non-zero"));
    *called = true;
    return BURROW_NO_ERROR;
}

/* Not in Go: verifyConnectionCallback, with isClient false and true. */
static Error verify_connection_callback(bool *called, bool is_client,
                                        const TlsConnectionState *c) {
    Int l = c->peer_certificates.len;
    if (l != 1)
        return fmt_errorf_v("got len(PeerCertificates) = %d, wanted 1", l);
    if (c->verified_chains.len == 0)
        return fmt_errorf_v("got len(VerifiedChains) = 0, wanted non-zero");
    if (is_client && c->ocsp_response.len == 0)
        return fmt_errorf_v("got len(OCSPResponse) = 0, wanted non-zero");
    *called = true;
    return BURROW_NO_ERROR;
}

static Error verify_connection_callback_server(void *env, const TlsConnectionState *c) {
    return verify_connection_callback(env, false, c);
}

static Error verify_connection_callback_client(void *env, const TlsConnectionState *c) {
    return verify_connection_callback(env, true, c);
}

static Error verify_peer_certificate_sentinel(void *env, Slice raw_certs,
                                              Slice validated_chains) {
    (void)env;
    (void)raw_certs;
    (void)validated_chains;
    return verify_peer_certificate_sentinel_err;
}

static Error verify_connection_sentinel(void *env, const TlsConnectionState *c) {
    (void)env;
    (void)c;
    return verify_peer_certificate_sentinel_err;
}

static Error verify_peer_certificate_insecure(void *env, Slice raw_certs,
                                              Slice validated_chains) {
    bool *called = env;
    Int l = raw_certs.len;
    if (l != 1)
        return fmt_errorf_v("got len(rawCerts) = %d, wanted 1", l);
    /* With InsecureSkipVerify set, this callback should still be called but
     * validatedChains must be empty. */
    l = validated_chains.len;
    if (l != 0)
        return fmt_errorf_v("got len(validatedChains) = %d, wanted zero", l);
    *called = true;
    return BURROW_NO_ERROR;
}

#define VPC_NONE ((TlsVerifyPeerCertificateFunc){NULL, NULL})
#define VC_NONE ((TlsVerifyConnectionFunc){NULL, NULL})

static void vpc_server_0(TlsConfig *config, bool *called) {
    config->insecure_skip_verify = false;
    config->verify_peer_certificate = BURROW_FN(
        TlsVerifyPeerCertificateFunc, verify_peer_certificate_callback, called);
}

static void vpc_client_0(TlsConfig *config, bool *called) {
    config->insecure_skip_verify = false;
    config->verify_peer_certificate = BURROW_FN(
        TlsVerifyPeerCertificateFunc, verify_peer_certificate_callback, called);
}

static void vpc_server_1(TlsConfig *config, bool *called) {
    (void)called;
    config->insecure_skip_verify = false;
    config->verify_peer_certificate =
        BURROW_FN(TlsVerifyPeerCertificateFunc, verify_peer_certificate_sentinel, NULL);
}

static void vpc_client_1(TlsConfig *config, bool *called) {
    (void)called;
    config->verify_peer_certificate = VPC_NONE;
}

static void vpc_server_2(TlsConfig *config, bool *called) {
    (void)called;
    config->insecure_skip_verify = false;
}

static void vpc_client_2(TlsConfig *config, bool *called) {
    (void)called;
    config->verify_peer_certificate =
        BURROW_FN(TlsVerifyPeerCertificateFunc, verify_peer_certificate_sentinel, NULL);
}

static void vpc_client_3(TlsConfig *config, bool *called) {
    config->insecure_skip_verify = true;
    config->verify_peer_certificate = BURROW_FN(
        TlsVerifyPeerCertificateFunc, verify_peer_certificate_insecure, called);
}

static void vpc_server_4(TlsConfig *config, bool *called) {
    config->insecure_skip_verify = false;
    config->verify_connection =
        BURROW_FN(TlsVerifyConnectionFunc, verify_connection_callback_server, called);
}

static void vpc_client_4(TlsConfig *config, bool *called) {
    config->insecure_skip_verify = false;
    config->verify_connection =
        BURROW_FN(TlsVerifyConnectionFunc, verify_connection_callback_client, called);
}

static void vpc_server_5(TlsConfig *config, bool *called) {
    (void)called;
    config->insecure_skip_verify = false;
    config->verify_connection =
        BURROW_FN(TlsVerifyConnectionFunc, verify_connection_sentinel, NULL);
}

static void vpc_client_5(TlsConfig *config, bool *called) {
    (void)called;
    config->insecure_skip_verify = false;
    config->verify_connection = VC_NONE;
}

static void vpc_server_6(TlsConfig *config, bool *called) {
    (void)called;
    config->insecure_skip_verify = false;
    config->verify_connection = VC_NONE;
}

static void vpc_client_6(TlsConfig *config, bool *called) {
    (void)called;
    config->insecure_skip_verify = false;
    config->verify_connection =
        BURROW_FN(TlsVerifyConnectionFunc, verify_connection_sentinel, NULL);
}

static void vpc_server_7(TlsConfig *config, bool *called) {
    config->insecure_skip_verify = false;
    config->verify_peer_certificate = BURROW_FN(
        TlsVerifyPeerCertificateFunc, verify_peer_certificate_callback, called);
    config->verify_connection =
        BURROW_FN(TlsVerifyConnectionFunc, verify_connection_sentinel, NULL);
}

static void vpc_client_7(TlsConfig *config, bool *called) {
    (void)called;
    config->insecure_skip_verify = false;
    config->verify_peer_certificate = VPC_NONE;
    config->verify_connection = VC_NONE;
}

static void vpc_server_8(TlsConfig *config, bool *called) {
    (void)called;
    config->insecure_skip_verify = false;
    config->verify_peer_certificate = VPC_NONE;
    config->verify_connection = VC_NONE;
}

static void vpc_client_8(TlsConfig *config, bool *called) {
    config->insecure_skip_verify = false;
    config->verify_peer_certificate = BURROW_FN(
        TlsVerifyPeerCertificateFunc, verify_peer_certificate_callback, called);
    config->verify_connection =
        BURROW_FN(TlsVerifyConnectionFunc, verify_connection_sentinel, NULL);
}

/* The validate funcs. Not in Go: these take the errors as Errors. */
static void vpc_validate_both_called(TestingT *t, int test_no, bool client_called,
                                     bool server_called, Error client_err,
                                     Error server_err) {
    if (BURROW_FAILED(client_err))
        testing_t_errorf_v(t, "test[%d]: client handshake failed: %v", test_no,
                           client_err);
    if (BURROW_FAILED(server_err))
        testing_t_errorf_v(t, "test[%d]: server handshake failed: %v", test_no,
                           server_err);
    if (!client_called)
        testing_t_errorf_v(t, "test[%d]: client did not call callback", test_no);
    if (!server_called)
        testing_t_errorf_v(t, "test[%d]: server did not call callback", test_no);
}

static void vpc_validate_server_sentinel(TestingT *t, int test_no, bool client_called,
                                         bool server_called, Error client_err,
                                         Error server_err) {
    (void)client_called;
    (void)server_called;
    (void)client_err;
    if (!is_sentinel_err(server_err))
        testing_t_errorf_v(t, "#%d: got server error %v, wanted sentinelErr", test_no,
                           server_err);
}

static void vpc_validate_client_sentinel(TestingT *t, int test_no, bool client_called,
                                         bool server_called, Error client_err,
                                         Error server_err) {
    (void)client_called;
    (void)server_called;
    (void)server_err;
    if (!is_sentinel_err(client_err))
        testing_t_errorf_v(t, "#%d: got client error %v, wanted sentinelErr", test_no,
                           client_err);
}

static void vpc_validate_client_called(TestingT *t, int test_no, bool client_called,
                                       bool server_called, Error client_err,
                                       Error server_err) {
    (void)server_called;
    if (BURROW_FAILED(client_err))
        testing_t_errorf_v(t, "test[%d]: client handshake failed: %v", test_no,
                           client_err);
    if (BURROW_FAILED(server_err))
        testing_t_errorf_v(t, "test[%d]: server handshake failed: %v", test_no,
                           server_err);
    if (!client_called)
        testing_t_errorf_v(t, "test[%d]: client did not call callback", test_no);
}

static void vpc_validate_server_sentinel_called(TestingT *t, int test_no,
                                                bool client_called, bool server_called,
                                                Error client_err, Error server_err) {
    (void)client_called;
    (void)client_err;
    if (!is_sentinel_err(server_err))
        testing_t_errorf_v(t, "#%d: got server error %v, wanted sentinelErr", test_no,
                           server_err);
    if (!server_called)
        testing_t_errorf_v(t, "test[%d]: server did not call callback", test_no);
}

static void vpc_validate_client_sentinel_called(TestingT *t, int test_no,
                                                bool client_called, bool server_called,
                                                Error client_err, Error server_err) {
    (void)server_called;
    (void)server_err;
    if (!is_sentinel_err(client_err))
        testing_t_errorf_v(t, "#%d: got client error %v, wanted sentinelErr", test_no,
                           client_err);
    if (!client_called)
        testing_t_errorf_v(t, "test[%d]: client did not call callback", test_no);
}

typedef struct VerifyPeerCertificateTest {
    void (*configure_server)(TlsConfig *config, bool *called);
    void (*configure_client)(TlsConfig *config, bool *called);
    void (*validate)(TestingT *t, int test_no, bool client_called, bool server_called,
                     Error client_err, Error server_err);
} VerifyPeerCertificateTest;

static const VerifyPeerCertificateTest verify_peer_certificate_tests[] = {
    {vpc_server_0, vpc_client_0, vpc_validate_both_called},
    {vpc_server_1, vpc_client_1, vpc_validate_server_sentinel},
    {vpc_server_2, vpc_client_2, vpc_validate_client_sentinel},
    {vpc_server_2, vpc_client_3, vpc_validate_client_called},
    {vpc_server_4, vpc_client_4, vpc_validate_both_called},
    {vpc_server_5, vpc_client_5, vpc_validate_server_sentinel},
    {vpc_server_6, vpc_client_6, vpc_validate_client_sentinel},
    {vpc_server_7, vpc_client_7, vpc_validate_server_sentinel_called},
    {vpc_server_8, vpc_client_8, vpc_validate_client_sentinel_called},
};

/* Not in Go: the server goroutine of testVerifyPeerCertificate. */
typedef struct VerifyPeerCertificateServer {
    TlsConn *server;
    NetConn s;
    Error err;
} VerifyPeerCertificateServer;

static void verify_peer_certificate_server(void *env) {
    VerifyPeerCertificateServer *j = env;
    j->err = tls_conn_handshake(j->server);
    (void)j->s.vt->closer.close(j->s.data);
}

static void test_verify_peer_certificate(TestingT *t, uint16_t version) {
    for (size_t i = 0; i < sizeof verify_peer_certificate_tests /
                               sizeof verify_peer_certificate_tests[0];
         i++) {
        const VerifyPeerCertificateTest *test = &verify_peer_certificate_tests[i];
        Arena ar;
        arena_init(&ar, NULL, 0);
        Alloc *a = arena_allocator(&ar);
        NetConn c, s;
        local_pipe(a, &c, &s);

        bool client_called = false, server_called = false;

        /* Not in Go: the server's config is made here, before its goroutine
         * starts, rather than in it. */
        TlsConfig server_config = test_config_server(a);
        server_config.client_auth = TLS_REQUIRE_AND_VERIFY_CLIENT_CERT;
        server_config.max_version = version;
        TlsCertificate server_cert =
            parse_test_cert(a, test_rsa2048_cert_pem, test_rsa2048_key_pem);
        static const char sct1[] = "dummy sct 1", sct2[] = "dummy sct 2",
                          ocsp[] = "dummy ocsp";
        Slice scts[2] = {slice_from((Byte *)(uintptr_t)sct1, 11, 11, TYPE_BYTE),
                         slice_from((Byte *)(uintptr_t)sct2, 11, 11, TYPE_BYTE)};
        server_cert.signed_certificate_timestamps = slice_from(scts, 2, 2, TYPE_BYTES);
        server_cert.ocsp_staple =
            slice_from((Byte *)(uintptr_t)ocsp, 10, 10, TYPE_BYTE);
        server_config.certificates =
            slice_from(&server_cert, 1, 1, TYPE_TLS_CERTIFICATE);
        test->configure_server(&server_config, &server_called);

        VerifyPeerCertificateServer job = {
            tls_server(heap_allocator(), s, &server_config), s, BURROW_NO_ERROR};
        if (job.server == NULL)
            testing_t_fatal_v(t, "out of memory");
        SyncWaitGroup done = {0};
        sync_wait_group_go(&done,
                           BURROW_FN(Func, verify_peer_certificate_server, &job));

        TlsConfig config = test_config_client(a);
        TlsCertificate client_cert = parse_test_cert(a, test_client_rsa2048_cert_pem,
                                                     test_client_rsa2048_key_pem);
        config.certificates = slice_from(&client_cert, 1, 1, TYPE_TLS_CERTIFICATE);
        config.max_version = version;
        test->configure_client(&config, &client_called);
        TlsConn *client = tls_client(heap_allocator(), c, &config);
        if (client == NULL)
            testing_t_fatal_v(t, "out of memory");
        Error client_err = tls_conn_handshake(client);
        (void)c.vt->closer.close(c.data);
        sync_wait_group_wait(&done);
        Error server_err = job.err;

        test->validate(t, (int)i, client_called, server_called, client_err, server_err);

        tls_conn_free(client);
        tls_conn_free(job.server);
        local_pipe_free(c);
        tls_config_release(&config);
        tls_config_release(&server_config);
        arena_free(&ar);
    }
}

static void test_verify_peer_certificate_run(void *env, TestingT *t) {
    test_verify_peer_certificate(t, *(const uint16_t *)env);
}

static void TestVerifyPeerCertificate(TestingT *t) {
    uint16_t tls12 = TLS_VERSION_TLS12, tls13 = TLS_VERSION_TLS13;
    testing_t_run(t, BURROW_S("TLSv12"),
                  BURROW_FN(TestingTFunc, test_verify_peer_certificate_run, &tls12));
    testing_t_run(t, BURROW_S("TLSv13"),
                  BURROW_FN(TestingTFunc, test_verify_peer_certificate_run, &tls13));
}

/* Not in Go: what brokenConn and writeCountingConn get from the net.Conn they
 * embed. Both start with the conn, and everything but Write goes to it. */
typedef struct EmbeddedConn {
    NetConn conn;
} EmbeddedConn;

static Int embedded_conn_read(void *self, Slice b, Error *err) {
    EmbeddedConn *e = self;
    return e->conn.vt->reader.read(e->conn.data, b, err);
}

static Error embedded_conn_close(void *self) {
    EmbeddedConn *e = self;
    return e->conn.vt->closer.close(e->conn.data);
}

static NetAddr embedded_conn_local_addr(void *self) {
    EmbeddedConn *e = self;
    return e->conn.vt->local_addr(e->conn.data);
}

static NetAddr embedded_conn_remote_addr(void *self) {
    EmbeddedConn *e = self;
    return e->conn.vt->remote_addr(e->conn.data);
}

static Error embedded_conn_set_deadline(void *self, Time t) {
    EmbeddedConn *e = self;
    return e->conn.vt->set_deadline(e->conn.data, t);
}

static Error embedded_conn_set_read_deadline(void *self, Time t) {
    EmbeddedConn *e = self;
    return e->conn.vt->set_read_deadline(e->conn.data, t);
}

static Error embedded_conn_set_write_deadline(void *self, Time t) {
    EmbeddedConn *e = self;
    return e->conn.vt->set_write_deadline(e->conn.data, t);
}

/* brokenConn wraps a net.Conn and causes all Writes after a certain number to
 * fail with brokenConnErr. */
typedef struct BrokenConn {
    NetConn conn;

    /* breakAfter is the number of successful writes that will be allowed
     * before all subsequent writes fail. */
    int break_after;

    /* numWrites is the number of writes that have been done. */
    int num_writes;
} BrokenConn;

/* brokenConnErr is the error that brokenConn returns once exhausted. */
BURROW_SENTINEL_ERROR(broken_conn_err, "too many writes to brokenConn");

static Int broken_conn_write(void *self, Slice data, Error *err) {
    BrokenConn *b = self;
    if (b->num_writes >= b->break_after) {
        BURROW_OUT(err, broken_conn_err);
        return 0;
    }

    b->num_writes++;
    return b->conn.vt->writer.write(b->conn.data, data, err);
}

static const NetConnVT broken_conn_vt = {
    {NULL, embedded_conn_read},      {NULL, broken_conn_write},
    {NULL, embedded_conn_close},     embedded_conn_local_addr,
    embedded_conn_remote_addr,       embedded_conn_set_deadline,
    embedded_conn_set_read_deadline, embedded_conn_set_write_deadline,
};

/* Not in Go: the server goroutine of TestFailedWrite, TestBuffering and
 * TestAlertFlushing, which handshakes and closes s. */
typedef struct ServerThenClose {
    NetConn s;
    TlsConn *server;
} ServerThenClose;

static void server_then_close(void *env) {
    ServerThenClose *j = env;
    (void)tls_conn_handshake(j->server);
    (void)j->s.vt->closer.close(j->s.data);
}

/* Not in Go: makes the Server and starts the goroutine in wg. */
static void server_then_close_go(TestingT *t, ServerThenClose *j, SyncWaitGroup *wg,
                                 NetConn s, TlsConfig *config) {
    j->s = s;
    j->server = tls_server(heap_allocator(), s, config);
    if (j->server == NULL)
        testing_t_fatal_v(t, "out of memory");
    sync_wait_group_go(wg, BURROW_FN(Func, server_then_close, j));
}

static void TestFailedWrite(TestingT *t) {
    /* Test that a write error during the handshake is returned. */
    static const int break_afters[] = {0, 1};
    for (size_t i = 0; i < sizeof break_afters / sizeof break_afters[0]; i++) {
        int break_after = break_afters[i];
        Arena ar;
        arena_init(&ar, NULL, 0);
        Alloc *a = arena_allocator(&ar);
        NetConn c, s;
        local_pipe(a, &c, &s);
        SyncWaitGroup done = {0};

        TlsConfig server_config = test_config_server(a);
        ServerThenClose job;
        server_then_close_go(t, &job, &done, s, &server_config);

        BrokenConn broken_c = {c, break_after, 0};
        TlsConfig client_config = test_config_client(a);
        TlsConn *client = tls_client(
            heap_allocator(), (NetConn){&broken_conn_vt, &broken_c}, &client_config);
        if (client == NULL)
            testing_t_fatal_v(t, "out of memory");
        Error err = tls_conn_handshake(client);
        if (err.vt != broken_conn_err.vt || err.data != broken_conn_err.data)
            testing_t_errorf_v(t, "#%d: expected error from brokenConn but got %q",
                               break_after, err);
        (void)broken_conn_vt.closer.close(&broken_c);

        sync_wait_group_wait(&done);
        tls_conn_free(client);
        tls_conn_free(job.server);
        local_pipe_free(c);
        tls_config_release(&client_config);
        tls_config_release(&server_config);
        arena_free(&ar);
    }
}

/* writeCountingConn wraps a net.Conn and counts the number of Write calls. */
typedef struct WriteCountingConn {
    NetConn conn;

    /* numWrites is the number of writes that have been done. */
    int num_writes;
} WriteCountingConn;

static Int write_counting_conn_write(void *self, Slice data, Error *err) {
    WriteCountingConn *wcc = self;
    wcc->num_writes++;
    return wcc->conn.vt->writer.write(wcc->conn.data, data, err);
}

static const NetConnVT write_counting_conn_vt = {
    {NULL, embedded_conn_read},      {NULL, write_counting_conn_write},
    {NULL, embedded_conn_close},     embedded_conn_local_addr,
    embedded_conn_remote_addr,       embedded_conn_set_deadline,
    embedded_conn_set_read_deadline, embedded_conn_set_write_deadline,
};

/* Not in Go: what testBuffering and TestAlertFlushing hold. It is on the heap,
 * and a cleanup gives it back, so a Fatal leaks nothing. The cleanup closes
 * the client's end and waits for the server first. */
typedef struct WriteCountingPipe {
    NetConn c, s;
    WriteCountingConn client_wcc, server_wcc;
    TlsConfig server_config, client_config;
    ServerThenClose server;
    SyncWaitGroup done;
    bool started;
    TlsConn *client;
    Arena ar;
} WriteCountingPipe;

static void write_counting_pipe_free(void *env) {
    WriteCountingPipe *p = env;
    if (p->started) {
        (void)write_counting_conn_vt.closer.close(&p->client_wcc);
        sync_wait_group_wait(&p->done);
        tls_conn_free(p->server.server);
    }
    tls_conn_free(p->client);
    local_pipe_free(p->c);
    tls_config_release(&p->client_config);
    tls_config_release(&p->server_config);
    arena_free(&p->ar);
    mem_free(heap_allocator(), p, sizeof *p, _Alignof(WriteCountingPipe));
}

static WriteCountingPipe *write_counting_pipe_new(TestingT *t) {
    WriteCountingPipe *p = BURROW_NEW(heap_allocator(), WriteCountingPipe);
    if (p == NULL)
        testing_t_fatal_v(t, "out of memory");
    memset(p, 0, sizeof *p);
    arena_init(&p->ar, NULL, 0);
    testing_t_cleanup(t, BURROW_FN(Func, write_counting_pipe_free, p));
    Alloc *a = arena_allocator(&p->ar);
    local_pipe(a, &p->c, &p->s);
    p->client_wcc.conn = p->c;
    p->server_wcc.conn = p->s;
    p->server_config = test_config_server(a);
    p->client_config = test_config_client(a);
    return p;
}

/* Not in Go: the server goroutine, then the client's Handshake. */
static Error write_counting_pipe_handshake(TestingT *t, WriteCountingPipe *p) {
    server_then_close_go(t, &p->server, &p->done,
                         (NetConn){&write_counting_conn_vt, &p->server_wcc},
                         &p->server_config);
    p->started = true;
    p->client =
        tls_client(heap_allocator(), (NetConn){&write_counting_conn_vt, &p->client_wcc},
                   &p->client_config);
    if (p->client == NULL)
        testing_t_fatal_v(t, "out of memory");
    return tls_conn_handshake(p->client);
}

/* Not in Go: clientWCC.Close() and <-done. */
static void write_counting_pipe_wait(WriteCountingPipe *p) {
    (void)write_counting_conn_vt.closer.close(&p->client_wcc);
    sync_wait_group_wait(&p->done);
    tls_conn_free(p->server.server);
    p->server.server = NULL;
    p->started = false;
}

static void test_buffering(TestingT *t, uint16_t version) {
    WriteCountingPipe *p = write_counting_pipe_new(t);
    p->server_config.max_version = version;

    Error err = write_counting_pipe_handshake(t, p);
    if (BURROW_FAILED(err))
        testing_t_fatal_v(t, err);
    write_counting_pipe_wait(p);

    int expected_client, expected_server;
    if (version == TLS_VERSION_TLS13) {
        expected_client = 2;
        expected_server = 1;
    } else {
        expected_client = 2;
        expected_server = 2;
    }

    int n = p->client_wcc.num_writes;
    if (n != expected_client)
        testing_t_errorf_v(
            t, "expected client handshake to complete with %d writes, but saw %d",
            expected_client, n);

    n = p->server_wcc.num_writes;
    if (n != expected_server)
        testing_t_errorf_v(
            t, "expected server handshake to complete with %d writes, but saw %d",
            expected_server, n);
}

static void test_buffering_run(void *env, TestingT *t) {
    test_buffering(t, *(const uint16_t *)env);
}

static void TestBuffering(TestingT *t) {
    uint16_t tls12 = TLS_VERSION_TLS12, tls13 = TLS_VERSION_TLS13;
    testing_t_run(t, BURROW_S("TLSv12"),
                  BURROW_FN(TestingTFunc, test_buffering_run, &tls12));
    testing_t_run(t, BURROW_S("TLSv13"),
                  BURROW_FN(TestingTFunc, test_buffering_run, &tls13));
}

static void TestAlertFlushing(TestingT *t) {
    WriteCountingPipe *p = write_counting_pipe_new(t);
    Alloc *a = arena_allocator(&p->ar);

    /* Cause a signature-time error */
    TlsCertificate rsa2048 =
        parse_test_cert(a, test_rsa2048_cert_pem, test_rsa2048_key_pem);
    const RsaPrivateKey *test_rsa2048_key = rsa2048.private_key.data;
    RsaPrivateKey *broken_key = BURROW_NEW(a, RsaPrivateKey);
    if (broken_key == NULL)
        testing_t_fatal_v(t, "out of memory");
    memset(broken_key, 0, sizeof *broken_key);
    broken_key->public_key = test_rsa2048_key->public_key;
    broken_key->d = big_new_int(a, 42);
    TlsCertificate *cert = BURROW_NEW(a, TlsCertificate);
    if (cert == NULL)
        testing_t_fatal_v(t, "out of memory");
    memset(cert, 0, sizeof *cert);
    cert->certificate = rsa2048.certificate;
    cert->private_key = BURROW_ANY(TYPE_RSA_PRIVATE_KEY, (uintptr_t)broken_key);
    p->server_config.certificates = slice_from(cert, 1, 1, TYPE_TLS_CERTIFICATE);

    Error err = write_counting_pipe_handshake(t, p);
    if (!BURROW_FAILED(err))
        testing_t_fatal_v(t, "client unexpectedly returned no error");

    Str expected_error = BURROW_S("remote error: tls: internal error");
    Str e = error_text(err);
    if (!strings_contains(e, expected_error))
        testing_t_fatalf_v(t, "expected to find %q in error but error was %q",
                           expected_error, e);
    write_counting_pipe_wait(p);

    int n = p->server_wcc.num_writes;
    if (n != 1)
        testing_t_errorf_v(
            t, "expected server handshake to complete with one write, but saw %d", n);
}

/* Not in Go: what the goroutines of one round of TestHandshakeRace share. */
typedef struct HandshakeRace {
    NetConn c, s;
    TlsConn *server;
    TlsConn *client;
    Chan *start_write;
    Chan *start_read;
    Chan *read_done;
} HandshakeRace;

static void handshake_race_server(void *env) {
    HandshakeRace *r = env;
    Error err = tls_conn_handshake(r->server);
    if (BURROW_FAILED(err))
        panic(BURROW_ANY(TYPE_ERROR, &err));

    Byte request[1] = {0};
    Int n = tls_conn_read(r->server, slice_from(request, 1, 1, TYPE_BYTE), &err);
    if (BURROW_FAILED(err) || n != 1)
        panic(BURROW_ANY(TYPE_ERROR, &err));

    (void)tls_conn_write(r->server, slice_from(request, 1, 1, TYPE_BYTE), NULL);
    (void)tls_conn_close(r->server);
}

static void handshake_race_write(void *env) {
    HandshakeRace *r = env;
    bool v;
    (void)chan_recv(r->start_write, &v);
    Byte request[1] = {0};
    (void)tls_conn_write(r->client, slice_from(request, 1, 1, TYPE_BYTE), NULL);
}

static void handshake_race_read(void *env) {
    HandshakeRace *r = env;
    bool v;
    (void)chan_recv(r->start_read, &v);
    Byte reply[1];
    Error err = BURROW_NO_ERROR;
    (void)io_read_full(net_conn_as_io_reader(tls_conn_as_net_conn(r->client)),
                       slice_from(reply, 1, 1, TYPE_BYTE), &err);
    if (BURROW_FAILED(err))
        panic(BURROW_ANY(TYPE_ERROR, &err));
    (void)r->c.vt->closer.close(r->c.data);
    v = true;
    chan_send(r->read_done, &v);
}

static void TestHandshakeRace(TestingT *t) {
    if (testing_short())
        testing_t_skip_v(t, "skipping in -short mode");
    testing_t_parallel(t);
    /* This test races a Read and Write to try and complete a handshake in
     * order to provide some evidence that there are no races or deadlocks
     * in the handshake locking. */
    for (int i = 0; i < 32; i++) {
        Arena ar;
        arena_init(&ar, NULL, 0);
        Alloc *a = arena_allocator(&ar);
        HandshakeRace r;
        memset(&r, 0, sizeof r);
        local_pipe(a, &r.c, &r.s);
        /* Not in Go: the goroutines are all waited for, in wg, before what
         * they use is given back. */
        SyncWaitGroup wg = {0};

        TlsConfig server_config = test_config_server(a);
        r.server = tls_server(heap_allocator(), r.s, &server_config);
        if (r.server == NULL)
            testing_t_fatal_v(t, "out of memory");
        sync_wait_group_go(&wg, BURROW_FN(Func, handshake_race_server, &r));

        r.start_write = chan_make(a, TYPE_BOOL, 0);
        r.start_read = chan_make(a, TYPE_BOOL, 0);
        r.read_done = chan_make(a, TYPE_BOOL, 1);
        if (r.start_write == NULL || r.start_read == NULL || r.read_done == NULL)
            testing_t_fatal_v(t, "out of memory");

        TlsConfig client_config = test_config_client(a);
        r.client = tls_client(heap_allocator(), r.c, &client_config);
        if (r.client == NULL)
            testing_t_fatal_v(t, "out of memory");
        sync_wait_group_go(&wg, BURROW_FN(Func, handshake_race_write, &r));

        sync_wait_group_go(&wg, BURROW_FN(Func, handshake_race_read, &r));

        bool v = true;
        if ((i & 1) == 1) {
            chan_send(r.start_write, &v);
            chan_send(r.start_read, &v);
        } else {
            chan_send(r.start_read, &v);
            chan_send(r.start_write, &v);
        }
        (void)chan_recv(r.read_done, &v);

        sync_wait_group_wait(&wg);
        tls_conn_free(r.client);
        tls_conn_free(r.server);
        chan_free(r.start_write);
        chan_free(r.start_read);
        chan_free(r.read_done);
        local_pipe_free(r.c);
        tls_config_release(&client_config);
        tls_config_release(&server_config);
        arena_free(&ar);
    }
}

/* Not in Go: what the GetClientCertificate callbacks below hand back. Go makes
 * these with new(Certificate) and &testClientRSA2048Cert. */
typedef struct GetClientCertificateEnv {
    TlsCertificate *empty;
    TlsCertificate *client_rsa2048;
} GetClientCertificateEnv;

static TlsCertificate *
get_client_certificate_empty(void *env, TlsCertificateRequestInfo *cri, Error *err) {
    GetClientCertificateEnv *e = env;
    (void)err;
    if (cri->signature_schemes.len == 0)
        panic_str(BURROW_S("empty SignatureSchemes"));
    if (cri->acceptable_cas.len != 0)
        panic_str(BURROW_S("AcceptableCAs should have been empty"));
    return e->empty;
}

static TlsCertificate *
get_client_certificate_tls11(void *env, TlsCertificateRequestInfo *cri, Error *err) {
    GetClientCertificateEnv *e = env;
    (void)err;
    if (cri->signature_schemes.len == 0)
        panic_str(BURROW_S("empty SignatureSchemes"));
    return e->empty;
}

static TlsCertificate *
get_client_certificate_error(void *env, TlsCertificateRequestInfo *cri, Error *err) {
    (void)env;
    (void)cri;
    BURROW_OUT(err, errors_new(error_allocator(), BURROW_S("GetClientCertificate")));
    return NULL;
}

static TlsCertificate *
get_client_certificate_rsa2048(void *env, TlsCertificateRequestInfo *cri, Error *err) {
    GetClientCertificateEnv *e = env;
    (void)err;
    if (cri->acceptable_cas.len == 0)
        panic_str(BURROW_S("empty AcceptableCAs"));
    TlsCertificate *cert = e->client_rsa2048;
    return cert;
}

static void get_client_certificate_setup_0(TlsConfig *client_config,
                                           TlsConfig *server_config,
                                           GetClientCertificateEnv *e) {
    /* Returning a Certificate with no certificate data
     * should result in an empty message being sent to the
     * server. */
    server_config->client_cas = NULL;
    client_config->get_client_certificate =
        BURROW_FN(TlsGetClientCertificateFunc, get_client_certificate_empty, e);
}

static void get_client_certificate_setup_1(TlsConfig *client_config,
                                           TlsConfig *server_config,
                                           GetClientCertificateEnv *e) {
    (void)server_config;
    /* With TLS 1.1, the SignatureSchemes should be
     * synthesised from the supported certificate types. */
    client_config->max_version = TLS_VERSION_TLS11;
    client_config->get_client_certificate =
        BURROW_FN(TlsGetClientCertificateFunc, get_client_certificate_tls11, e);
}

static void get_client_certificate_setup_2(TlsConfig *client_config,
                                           TlsConfig *server_config,
                                           GetClientCertificateEnv *e) {
    (void)server_config;
    /* Returning an error should abort the handshake with
     * that error. */
    client_config->get_client_certificate =
        BURROW_FN(TlsGetClientCertificateFunc, get_client_certificate_error, e);
}

static void get_client_certificate_setup_3(TlsConfig *client_config,
                                           TlsConfig *server_config,
                                           GetClientCertificateEnv *e) {
    (void)server_config;
    client_config->get_client_certificate =
        BURROW_FN(TlsGetClientCertificateFunc, get_client_certificate_rsa2048, e);
}

static void get_client_certificate_no_certificates(TestingT *t, int test_num,
                                                   const TlsConnectionState *cs) {
    Int l = cs->peer_certificates.len;
    if (l != 0)
        testing_t_errorf_v(t, "#%d: expected no certificates but got %d", test_num, l);
}

static void get_client_certificate_nothing(TestingT *t, int test_num,
                                           const TlsConnectionState *cs) {
    (void)t;
    (void)test_num;
    (void)cs;
}

static void get_client_certificate_verified_chains(TestingT *t, int test_num,
                                                   const TlsConnectionState *cs) {
    if (cs->verified_chains.len == 0)
        testing_t_errorf_v(t, "#%d: expected some verified chains, but found none",
                           test_num);
}

typedef struct GetClientCertificateTest {
    void (*setup)(TlsConfig *client_config, TlsConfig *server_config,
                  GetClientCertificateEnv *e);
    const char *expected_client_error;
    void (*verify)(TestingT *t, int test_num, const TlsConnectionState *cs);
} GetClientCertificateTest;

static const GetClientCertificateTest get_client_certificate_tests[] = {
    {get_client_certificate_setup_0, "", get_client_certificate_no_certificates},
    {get_client_certificate_setup_1, "", get_client_certificate_no_certificates},
    {get_client_certificate_setup_2, "GetClientCertificate",
     get_client_certificate_nothing},
    {get_client_certificate_setup_3, "", get_client_certificate_verified_chains},
};

/* Not in Go: the server goroutine of testGetClientCertificate, and the
 * serverResult it sends. */
typedef struct GetClientCertificateServer {
    NetConn s;
    TlsConn *server;
    TlsConnectionState cs;
    Error err;
} GetClientCertificateServer;

static void get_client_certificate_server(void *env) {
    GetClientCertificateServer *j = env;
    Error err = tls_conn_handshake(j->server);

    if (!BURROW_FAILED(err))
        j->cs = tls_conn_connection_state(j->server);
    j->err = err;
    (void)j->s.vt->closer.close(j->s.data);
}

static void test_get_client_certificate(TestingT *t, uint16_t version) {
    for (size_t i = 0; i < sizeof get_client_certificate_tests /
                               sizeof get_client_certificate_tests[0];
         i++) {
        const GetClientCertificateTest *test = &get_client_certificate_tests[i];
        Arena ar;
        arena_init(&ar, NULL, 0);
        Alloc *a = arena_allocator(&ar);

        TlsConfig server_config = test_config_server(a);
        TlsCertificate server_cert =
            parse_test_cert(a, test_rsa2048_cert_pem, test_rsa2048_key_pem);
        server_config.certificates =
            slice_from(&server_cert, 1, 1, TYPE_TLS_CERTIFICATE);
        server_config.client_auth = TLS_VERIFY_CLIENT_CERT_IF_GIVEN;
        server_config.min_version = TLS_VERSION_TLS10;
        server_config.max_version = version;

        TlsConfig client_config = test_config_client(a);
        TlsCertificate client_cert = parse_test_cert(a, test_client_rsa2048_cert_pem,
                                                     test_client_rsa2048_key_pem);
        client_config.certificates =
            slice_from(&client_cert, 1, 1, TYPE_TLS_CERTIFICATE);
        client_config.min_version = TLS_VERSION_TLS10;
        client_config.max_version = version;

        TlsCertificate empty;
        memset(&empty, 0, sizeof empty);
        GetClientCertificateEnv e = {&empty, &client_cert};
        test->setup(&client_config, &server_config, &e);

        /* TLS 1.1 isn't available for FIPS required */
        if (burrow__tls_fips140_required() &&
            client_config.max_version == TLS_VERSION_TLS11) {
            testing_t_logf_v(t, "skipping test %d for FIPS mode", i);
            tls_config_release(&client_config);
            tls_config_release(&server_config);
            arena_free(&ar);
            continue;
        }

        NetConn c, s;
        local_pipe(a, &c, &s);
        GetClientCertificateServer result;
        memset(&result, 0, sizeof result);
        result.s = s;
        result.server = tls_server(heap_allocator(), s, &server_config);
        if (result.server == NULL)
            testing_t_fatal_v(t, "out of memory");
        SyncWaitGroup done = {0};
        sync_wait_group_go(&done,
                           BURROW_FN(Func, get_client_certificate_server, &result));

        TlsConn *client = tls_client(heap_allocator(), c, &client_config);
        if (client == NULL)
            testing_t_fatal_v(t, "out of memory");
        Error client_err = tls_conn_handshake(client);
        (void)c.vt->closer.close(c.data);

        sync_wait_group_wait(&done);

        int n = (int)i;
        Str expected_client_error = str_from_cstr(test->expected_client_error);
        if (BURROW_FAILED(client_err)) {
            Str got = error_text(client_err);
            if (expected_client_error.len == 0)
                testing_t_errorf_v(t, "#%d: client error: %v", n, client_err);
            else if (!str_eq(got, expected_client_error))
                testing_t_errorf_v(t, "#%d: expected client error %q, but got %q", n,
                                   expected_client_error, got);
            else
                test->verify(t, n, &result.cs);
        } else if (expected_client_error.len > 0) {
            testing_t_errorf_v(t, "#%d: expected client error %q, but got no error", n,
                               expected_client_error);
        } else if (BURROW_FAILED(result.err)) {
            testing_t_errorf_v(t, "#%d: server error: %v", n, result.err);
        } else {
            test->verify(t, n, &result.cs);
        }

        tls_conn_free(client);
        tls_conn_free(result.server);
        local_pipe_free(c);
        tls_config_release(&client_config);
        tls_config_release(&server_config);
        arena_free(&ar);
    }
}

static void test_get_client_certificate_run(void *env, TestingT *t) {
    test_get_client_certificate(t, *(const uint16_t *)env);
}

static void TestGetClientCertificate(TestingT *t) {
    uint16_t tls12 = TLS_VERSION_TLS12, tls13 = TLS_VERSION_TLS13;
    testing_t_run(t, BURROW_S("TLSv12"),
                  BURROW_FN(TestingTFunc, test_get_client_certificate_run, &tls12));
    testing_t_run(t, BURROW_S("TLSv13"),
                  BURROW_FN(TestingTFunc, test_get_client_certificate_run, &tls13));
}

static const char rsa_pss_key_error_pem[] =
    "\n"
    "-----BEGIN CERTIFICATE-----\n"
    "MIIDZTCCAhygAwIBAgIUCF2x0FyTgZG0CC9QTDjGWkB5vgEwPgYJKoZIhvcNAQEK\n"
    "MDGgDTALBglghkgBZQMEAgGhGjAYBgkqhkiG9w0BAQgwCwYJYIZIAWUDBAIBogQC\n"
    "AgDeMBIxEDAOBgNVBAMMB1JTQS1QU1MwHhcNMTgwNjI3MjI0NDM2WhcNMTgwNzI3\n"
    "MjI0NDM2WjASMRAwDgYDVQQDDAdSU0EtUFNTMIIBIDALBgkqhkiG9w0BAQoDggEP\n"
    "ADCCAQoCggEBANxDm0f76JdI06YzsjB3AmmjIYkwUEGxePlafmIASFjDZl/elD0Z\n"
    "/a7xLX468b0qGxLS5al7XCcEprSdsDR6DF5L520+pCbpfLyPOjuOvGmk9KzVX4x5\n"
    "b05YXYuXdsQ0Kjxcx2i3jjCday6scIhMJVgBZxTEyMj1thPQM14SHzKCd/m6HmCL\n"
    "QmswpH2yMAAcBRWzRpp/vdH5DeOJEB3aelq7094no731mrLUCHRiZ1htq8BDB3ou\n"
    "czwqgwspbqZ4dnMXl2MvfySQ5wJUxQwILbiuAKO2lVVPUbFXHE9pgtznNoPvKwQT\n"
    "JNcX8ee8WIZc2SEGzofjk3NpjR+2ADB2u3sCAwEAAaNTMFEwHQYDVR0OBBYEFNEz\n"
    "AdyJ2f+fU+vSCS6QzohnOnprMB8GA1UdIwQYMBaAFNEzAdyJ2f+fU+vSCS6Qzohn\n"
    "OnprMA8GA1UdEwEB/wQFMAMBAf8wPgYJKoZIhvcNAQEKMDGgDTALBglghkgBZQME\n"
    "AgGhGjAYBgkqhkiG9w0BAQgwCwYJYIZIAWUDBAIBogQCAgDeA4IBAQCjEdrR5aab\n"
    "sZmCwrMeKidXgfkmWvfuLDE+TCbaqDZp7BMWcMQXT9O0UoUT5kqgKj2ARm2pEW0Z\n"
    "H3Z1vj3bbds72qcDIJXp+l0fekyLGeCrX/CbgnMZXEP7+/+P416p34ChR1Wz4dU1\n"
    "KD3gdsUuTKKeMUog3plxlxQDhRQmiL25ygH1LmjLd6dtIt0GVRGr8lj3euVeprqZ\n"
    "bZ3Uq5eLfsn8oPgfC57gpO6yiN+UURRTlK3bgYvLh4VWB3XXk9UaQZ7Mq1tpXjoD\n"
    "HYFybkWzibkZp4WRo+Fa28rirH+/wHt0vfeN7UCceURZEx4JaxIIfe4ku7uDRhJi\n"
    "RwBA9Xk1KBNF\n"
    "-----END CERTIFICATE-----";

static void TestRSAPSSKeyError(TestingT *t) {
    /* crypto/tls does not support the rsa_pss_pss_* SignatureSchemes. If support for
     * public keys with OID RSASSA-PSS is added to crypto/x509, they will be misused
     * with the rsa_pss_rsae_* SignatureSchemes. Assert that RSASSA-PSS certificates
     * don't parse, or that they don't carry *rsa.PublicKey keys. */
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    Slice rest;
    PemBlock *b =
        pem_decode(a,
                   slice_from((Byte *)(uintptr_t)rsa_pss_key_error_pem,
                              (Int)sizeof rsa_pss_key_error_pem - 1,
                              (Int)sizeof rsa_pss_key_error_pem - 1, TYPE_BYTE),
                   &rest);
    if (b == NULL) {
        arena_free(&ar);
        testing_t_fatal_v(t, "Failed to decode certificate");
    }
    Error err = BURROW_NO_ERROR;
    X509Certificate *cert = x509_parse_certificate(a, b->bytes, &err);
    if (BURROW_FAILED(err) || cert == NULL) {
        arena_free(&ar);
        return;
    }
    if (cert->public_key.t == TYPE_RSA_PUBLIC_KEY)
        testing_t_error_v(t, "A RSASSA-PSS certificate was parsed like a PKCS#1 v1.5 "
                             "one, and it will be mistakenly used with rsa_pss_rsae_* "
                             "signature algorithms");
    arena_free(&ar);
}

/* Not in Go: the goroutine of TestCloseClientConnectionOnIdleServer. */
typedef struct CloseOnIdle {
    NetConn server_conn;
    TlsConn *client;
} CloseOnIdle;

static void close_on_idle(void *env) {
    CloseOnIdle *j = env;
    Byte b[1];
    (void)j->server_conn.vt->reader.read(j->server_conn.data,
                                         slice_from(b, 1, 1, TYPE_BYTE), NULL);
    (void)tls_conn_close(j->client);
}

static void TestCloseClientConnectionOnIdleServer(TestingT *t) {
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    NetConn client_conn, server_conn;
    local_pipe(a, &client_conn, &server_conn);
    TlsConfig config = test_config_client(a);
    TlsConn *client = tls_client(heap_allocator(), client_conn, &config);
    if (client == NULL) {
        arena_free(&ar);
        testing_t_fatal_v(t, "out of memory");
    }
    CloseOnIdle job = {server_conn, client};
    SyncWaitGroup wg = {0};
    sync_wait_group_go(&wg, BURROW_FN(Func, close_on_idle, &job));
    (void)tls_conn_set_write_deadline(client, time_add(time_now(), TIME_MINUTE));
    Error err = tls_conn_handshake(client);
    if (BURROW_FAILED(err)) {
        if (net_is_error(err) && net_error_timeout(err))
            testing_t_errorf_v(
                t, "Expected a closed network connection error but got '%s'", err);
    } else {
        testing_t_errorf_v(t, "Error expected, but no error returned");
    }
    /* Not in Go: the goroutine is waited for before what it uses is given
     * back. */
    sync_wait_group_wait(&wg);
    tls_conn_free(client);
    local_pipe_free(client_conn);
    tls_config_release(&config);
    arena_free(&ar);
}

/* Not in Go: this says whether testHandshake failed, rather than returning its
 * error, which goes when the handshake is given back. */
static bool test_downgrade_canary(TestingT *t, uint16_t client_version,
                                  uint16_t server_version) {
    burrow__tls_testing_only_force_downgrade_canary = true;

    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    TlsConfig client_config = test_config_client(a);
    client_config.min_version = TLS_VERSION_TLS10;
    client_config.max_version = client_version;
    TlsConfig server_config = test_config_server(a);
    server_config.min_version = TLS_VERSION_TLS10;
    server_config.max_version = server_version;
    Handshake h;
    test_handshake(t, &client_config, &server_config, &h);
    bool failed = BURROW_FAILED(h.err);
    handshake_free(&h);
    tls_config_release(&client_config);
    tls_config_release(&server_config);
    arena_free(&ar);

    burrow__tls_testing_only_force_downgrade_canary = false;
    return failed;
}

static void TestDowngradeCanary(TestingT *t) {
    if (!test_downgrade_canary(t, TLS_VERSION_TLS13, TLS_VERSION_TLS12))
        testing_t_errorf_v(t, "downgrade from TLS 1.3 to TLS 1.2 was not detected");
    if (testing_short())
        testing_t_skip_v(t, "skipping the rest of the checks in short mode");
    if (!test_downgrade_canary(t, TLS_VERSION_TLS13, TLS_VERSION_TLS11))
        testing_t_errorf_v(t, "downgrade from TLS 1.3 to TLS 1.1 was not detected");
    if (!test_downgrade_canary(t, TLS_VERSION_TLS13, TLS_VERSION_TLS10))
        testing_t_errorf_v(t, "downgrade from TLS 1.3 to TLS 1.0 was not detected");
    if (!test_downgrade_canary(t, TLS_VERSION_TLS12, TLS_VERSION_TLS11))
        testing_t_errorf_v(t, "downgrade from TLS 1.2 to TLS 1.1 was not detected");
    if (!test_downgrade_canary(t, TLS_VERSION_TLS12, TLS_VERSION_TLS10))
        testing_t_errorf_v(t, "downgrade from TLS 1.2 to TLS 1.0 was not detected");
    if (test_downgrade_canary(t, TLS_VERSION_TLS13, TLS_VERSION_TLS13))
        testing_t_errorf_v(t, "server unexpectedly sent downgrade canary for TLS 1.3");
    if (test_downgrade_canary(t, TLS_VERSION_TLS12, TLS_VERSION_TLS12))
        testing_t_errorf_v(t, "client didn't ignore expected TLS 1.2 canary");
    if (!burrow__tls_fips140_required()) {
        if (test_downgrade_canary(t, TLS_VERSION_TLS11, TLS_VERSION_TLS11))
            testing_t_errorf_v(t, "client unexpectedly reacted to a canary in TLS 1.1");
        if (test_downgrade_canary(t, TLS_VERSION_TLS10, TLS_VERSION_TLS10))
            testing_t_errorf_v(t, "client unexpectedly reacted to a canary in TLS 1.0");
    } else {
        testing_t_logf_v(t, "skipping TLS 1.1 and TLS 1.0 downgrade canary checks in "
                            "FIPS mode");
    }
}

/* Not in Go: reflect.DeepEqual on two [][]byte, which is all the test below
 * asks it. A nil and an empty one are equal here. */
static bool byte_slices_equal(Slice x, Slice y) {
    if (x.len != y.len)
        return false;
    for (Int i = 0; i < x.len; i++) {
        if (!bytes_equal(((const Slice *)x.p)[i], ((const Slice *)y.p)[i]))
            return false;
    }
    return true;
}

/* Not in Go: what testResumptionKeepsOCSPAndSCT holds. It is on the heap, and
 * a cleanup gives it back, so a Fatalf leaks nothing. */
typedef struct KeepsOCSPAndSCT {
    HandshakePair p;
    TlsCertificate certificates[1];
    Slice scts[1];
    Slice new_scts[1];
} KeepsOCSPAndSCT;

static void keeps_ocsp_and_sct_free(void *env) {
    KeepsOCSPAndSCT *k = env;
    handshake_pair_release(&k->p);
    mem_free(heap_allocator(), k, sizeof *k, _Alignof(KeepsOCSPAndSCT));
}

static void test_resumption_keeps_ocsp_and_sct(TestingT *t, uint16_t ver) {
    static const Byte ocsp_staple[] = {1, 2, 3};
    static const Byte sct[] = {4, 5, 6};
    static const Byte new_sct[] = {7, 8, 9};
    KeepsOCSPAndSCT *k = BURROW_NEW(heap_allocator(), KeepsOCSPAndSCT);
    if (k == NULL)
        testing_t_fatal_v(t, "out of memory");
    memset(k, 0, sizeof *k);
    handshake_pair_init(&k->p, t);
    testing_t_cleanup(t, BURROW_FN(Func, keeps_ocsp_and_sct_free, k));
    Alloc *a = arena_allocator(&k->p.ar);

    TlsConfig *client_config = &k->p.client_config;
    client_config->max_version = ver;
    client_config->client_session_cache = handshake_pair_lru(&k->p);
    client_config->server_name = BURROW_S("test.golang.example");
    client_config->root_cas =
        new_test_cert_pool(t, a, test_root_cert_pem, test_root_key_pem);
    client_config->time = (TlsTimeFunc){test_time, NULL};
    TlsConfig *server_config = &k->p.server_config;
    *server_config = test_config_server(a);
    k->certificates[0] =
        parse_test_cert(a, test_rsa2048_cert_pem, test_rsa2048_key_pem);
    server_config->certificates =
        slice_from(k->certificates, 1, 1, TYPE_TLS_CERTIFICATE);
    server_config->max_version = ver;
    TlsCertificate *cert = &k->certificates[0];
    cert->ocsp_staple = slice_from((Byte *)(uintptr_t)ocsp_staple, 3, 3, TYPE_BYTE);
    k->scts[0] = slice_from((Byte *)(uintptr_t)sct, 3, 3, TYPE_BYTE);
    cert->signed_certificate_timestamps = slice_from(k->scts, 1, 1, TYPE_BYTES);

    handshake_pair_run(&k->p);
    const TlsConnectionState *ccs = &k->p.h.client_state;
    if (BURROW_FAILED(k->p.h.err))
        testing_t_fatalf_v(t, "handshake failed: %s", k->p.h.err);
    /* after a new session we expect to see OCSPResponse and
     * SignedCertificateTimestamps populated as usual */
    if (!bytes_equal(ccs->ocsp_response, cert->ocsp_staple))
        testing_t_errorf_v(t,
                           "client ConnectionState contained unexpected OCSPResponse: "
                           "wanted %v, got %v",
                           cert->ocsp_staple, ccs->ocsp_response);
    if (!byte_slices_equal(ccs->signed_certificate_timestamps,
                           cert->signed_certificate_timestamps))
        testing_t_errorf_v(t,
                           "client ConnectionState contained unexpected "
                           "SignedCertificateTimestamps: wanted %v, got %v",
                           cert->signed_certificate_timestamps,
                           ccs->signed_certificate_timestamps);

    /* if the server doesn't send any SCTs, repopulate the old SCTs */
    Slice old_scts = cert->signed_certificate_timestamps;
    cert->signed_certificate_timestamps = slice_nil(TYPE_BYTES);
    handshake_pair_run(&k->p);
    ccs = &k->p.h.client_state;
    if (BURROW_FAILED(k->p.h.err))
        testing_t_fatalf_v(t, "handshake failed: %s", k->p.h.err);
    if (!ccs->did_resume)
        testing_t_fatalf_v(t, "expected session to be resumed");
    /* after a resumed session we also expect to see OCSPResponse
     * and SignedCertificateTimestamps populated */
    if (!bytes_equal(ccs->ocsp_response, cert->ocsp_staple))
        testing_t_errorf_v(t,
                           "client ConnectionState contained unexpected OCSPResponse "
                           "after resumption: wanted %v, got %v",
                           cert->ocsp_staple, ccs->ocsp_response);
    if (!byte_slices_equal(ccs->signed_certificate_timestamps, old_scts))
        testing_t_errorf_v(t,
                           "client ConnectionState contained unexpected "
                           "SignedCertificateTimestamps after resumption: wanted %v, "
                           "got %v",
                           old_scts, ccs->signed_certificate_timestamps);

    /*  Only test overriding the SCTs for TLS 1.2, since in 1.3
     * the server won't send the message containing them */
    if (ver == TLS_VERSION_TLS13)
        return;

    /* if the server changes the SCTs it sends, they should override the saved
     * SCTs */
    k->new_scts[0] = slice_from((Byte *)(uintptr_t)new_sct, 3, 3, TYPE_BYTE);
    cert->signed_certificate_timestamps = slice_from(k->new_scts, 1, 1, TYPE_BYTES);
    handshake_pair_run(&k->p);
    ccs = &k->p.h.client_state;
    if (BURROW_FAILED(k->p.h.err))
        testing_t_fatalf_v(t, "handshake failed: %s", k->p.h.err);
    if (!ccs->did_resume)
        testing_t_fatalf_v(t, "expected session to be resumed");
    if (!byte_slices_equal(ccs->signed_certificate_timestamps,
                           cert->signed_certificate_timestamps))
        testing_t_errorf_v(t,
                           "client ConnectionState contained unexpected "
                           "SignedCertificateTimestamps after resumption: wanted %v, "
                           "got %v",
                           cert->signed_certificate_timestamps,
                           ccs->signed_certificate_timestamps);
}

static void test_resumption_keeps_ocsp_and_sct_run(void *env, TestingT *t) {
    test_resumption_keeps_ocsp_and_sct(t, *(const uint16_t *)env);
}

static void TestResumptionKeepsOCSPAndSCT(TestingT *t) {
    uint16_t tls12 = TLS_VERSION_TLS12, tls13 = TLS_VERSION_TLS13;
    testing_t_run(
        t, BURROW_S("TLSv12"),
        BURROW_FN(TestingTFunc, test_resumption_keeps_ocsp_and_sct_run, &tls12));
    testing_t_run(
        t, BURROW_S("TLSv13"),
        BURROW_FN(TestingTFunc, test_resumption_keeps_ocsp_and_sct_run, &tls13));
}

/* Not in Go: the goroutine of TestClientHandshakeContextCancellation. Go
 * closes unblockServer when the test returns. Here the test calls
 * sync_wait_group_done on it. */
typedef struct ClientCancelJob {
    ContextCancelFunc cancel;
    SyncWaitGroup *unblock_server;
    NetConn s;
} ClientCancelJob;

static void client_cancel_job(void *env) {
    ClientCancelJob *j = env;
    BURROW_CALLF0(j->cancel);
    sync_wait_group_wait(j->unblock_server);
    (void)j->s.vt->closer.close(j->s.data);
}

/* TestClientHandshakeContextCancellation tests that canceling
 * the context given to the client side conn.HandshakeContext
 * interrupts the in-progress handshake. */
static void TestClientHandshakeContextCancellation(TestingT *t) {
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    NetConn c, s;
    local_pipe(a, &c, &s);
    ContextCancelFunc cancel;
    Context ctx = context_with_cancel(a, context_background(), &cancel);
    if (BURROW_CONTEXT_IS_NIL(ctx)) {
        arena_free(&ar);
        testing_t_fatal_v(t, "out of memory");
    }
    SyncWaitGroup unblock_server = {0};
    sync_wait_group_add(&unblock_server, 1);
    ClientCancelJob job = {cancel, &unblock_server, s};
    SyncWaitGroup wg = {0};
    sync_wait_group_go(&wg, BURROW_FN(Func, client_cancel_job, &job));
    TlsConfig config = test_config_client(a);
    TlsConn *cli = tls_client(heap_allocator(), c, &config);
    if (cli == NULL) {
        sync_wait_group_done(&unblock_server);
        sync_wait_group_wait(&wg);
        testing_t_fatal_v(t, "out of memory");
    }
    /* Initiates client side handshake, which will block until the client hello
     * is read by the server, unless the cancellation works. */
    Error err = tls_conn_handshake_context(cli, ctx);
    if (!BURROW_FAILED(err)) {
        sync_wait_group_done(&unblock_server);
        sync_wait_group_wait(&wg);
        testing_t_fatal_v(t, "Client handshake did not error when the context was "
                             "canceled");
    }
    if (err.vt != context_canceled.vt || err.data != context_canceled.data)
        testing_t_errorf_v(t, "Unexpected client handshake error: %v", err);
    /* Not in Go: the skip on js and wasip1, where Go's localPipe does not
     * error on a second Close. This one does everywhere. */
    err = tls_conn_close(cli);
    if (!BURROW_FAILED(err))
        testing_t_error_v(t, "Client connection was not closed when the context was "
                             "canceled");

    sync_wait_group_done(&unblock_server);
    sync_wait_group_wait(&wg);
    tls_conn_free(cli);
    local_pipe_free(c);
    context_release(ctx);
    tls_config_release(&config);
    arena_free(&ar);
}

/* Not in Go: one of tls13Tests. ciphers is nil when nil is true, and empty
 * but not nil when n is 0 and nil is false. */
typedef struct TLS13OnlyTest {
    const char *name;
    bool nil;
    uint16_t ciphers[2];
    Int n;
} TLS13OnlyTest;

static const TLS13OnlyTest tls13_only_tests[] = {
    {"nil", true, {0}, 0},
    {"empty", false, {0}, 0},
    {"some TLS 1.2 cipher", false, {TLS_TLS_ECDHE_ECDSA_WITH_AES_128_GCM_SHA256}, 1},
    {"some TLS 1.3 cipher", false, {TLS_TLS_AES_128_GCM_SHA256}, 1},
    {"some TLS 1.2 and 1.3 ciphers",
     false,
     {TLS_TLS_ECDHE_ECDSA_WITH_AES_256_GCM_SHA384, TLS_TLS_AES_256_GCM_SHA384},
     2},
};

static TlsConfig *tls13_only_get_config_for_client(void *env, TlsClientHelloInfo *chi,
                                                   Error *err) {
    TestingT *t = env;
    (void)err;
    const uint16_t *expected_ciphersuites =
        burrow__tls_default_cipher_suites_tls13_no_aes;
    Int expected_len = 3;
    if (burrow__tls_fips140_required()) {
        expected_ciphersuites = burrow__tls_allowed_cipher_suites_tls13_fips;
        expected_len = 2;
    }
    const uint16_t *got_suites = chi->cipher_suites.p;
    if (chi->cipher_suites.len != expected_len) {
        testing_t_errorf_v(t, "only TLS 1.3 suites should be advertised, got=%x",
                           chi->cipher_suites);
    } else {
        for (Int i = 0; i < expected_len; i++) {
            uint16_t want = expected_ciphersuites[i], got = got_suites[i];
            if (want != got)
                testing_t_errorf_v(t,
                                   "cipher at index %d does not match, want=%x, got=%x",
                                   i, want, got);
        }
    }
    return NULL;
}

static void test_tls13_only_client_hello_cipher_suite(TestingT *t, Slice ciphers) {
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    TlsConfig base = test_config_server(a);
    TlsConfig server_config;
    memset(&server_config, 0, sizeof server_config);
    server_config.certificates = base.certificates;
    server_config.get_config_for_client =
        BURROW_FN(TlsGetConfigForClientFunc, tls13_only_get_config_for_client, t);
    TlsConfig client_config;
    memset(&client_config, 0, sizeof client_config);
    client_config.min_version = TLS_VERSION_TLS13; /* client only supports TLS 1.3 */
    client_config.cipher_suites = ciphers;
    client_config.insecure_skip_verify = true;
    Handshake h;
    test_handshake(t, &client_config, &server_config, &h);
    /* Not in Go: Errorf where Go has Fatalf. Nothing comes after it, and this
     * way what the handshake made is still given back. */
    if (BURROW_FAILED(h.err))
        testing_t_errorf_v(t, "handshake failed: %s", h.err);
    handshake_free(&h);
    tls_config_release(&client_config);
    tls_config_release(&server_config);
    tls_config_release(&base);
    arena_free(&ar);
}

static void test_tls13_only_client_hello_cipher_suite_run(void *env, TestingT *t) {
    const TLS13OnlyTest *tt = env;
    testing_t_parallel(t);
    Slice ciphers = slice_nil(TYPE_UINT16);
    if (!tt->nil)
        ciphers =
            slice_from((uint16_t *)(uintptr_t)tt->ciphers, tt->n, tt->n, TYPE_UINT16);
    test_tls13_only_client_hello_cipher_suite(t, ciphers);
}

/* Not in Go: the cleanup below. */
static void aes_gcm_hardware_from_cpu(void *env) {
    (void)env;
    burrow__tls_aes_gcm_hardware_override = -1;
}

/* TestTLS13OnlyClientHelloCipherSuite tests that when a client states that
 * it only supports TLS 1.3, it correctly advertises only TLS 1.3 ciphers. */
static void TestTLS13OnlyClientHelloCipherSuite(TestingT *t) {
    /* Not in Go: Go's TestMain turns off hasAESGCMHardwareSupport for the
     * whole binary, and the order checked below is the one without it. This
     * binary has no TestMain, so the test does it, as the replays do. The
     * cleanup puts it back once the parallel subtests are done. */
    burrow__tls_aes_gcm_hardware_override = 0;
    testing_t_cleanup(t, BURROW_FN(Func, aes_gcm_hardware_from_cpu, NULL));
    for (size_t i = 0; i < sizeof tls13_only_tests / sizeof tls13_only_tests[0]; i++) {
        const TLS13OnlyTest *tt = &tls13_only_tests[i];
        testing_t_run(t, str_from_cstr(tt->name),
                      BURROW_FN(TestingTFunc,
                                test_tls13_only_client_hello_cipher_suite_run,
                                (void *)(uintptr_t)tt));
    }
}

/* Not in Go. Reads s until the client closes its end, so that what the
 * client writes after the ServerHello does not block it. */
static void drain_job(void *env) {
    NetConn *s = (NetConn *)env;
    Byte buf[512];
    Error err = BURROW_NO_ERROR;
    while (BURROW_OK(err))
        (void)s->vt->reader.read(s->data, slice_from(buf, 512, 512, TYPE_BYTE), &err);
}

/* Not in Go. Each mutation turns a good TLS 1.3 ServerHello for hello into one
 * the client has to reject. */
static void sh_legacy_version_field(TlsServerHelloMsg *m, const TlsClientHelloMsg *h) {
    (void)h;
    m->supported_version = 0;
    m->vers = TLS_VERSION_TLS13;
}

static void sh_legacy_version(TlsServerHelloMsg *m, const TlsClientHelloMsg *h) {
    (void)h;
    m->vers = TLS_VERSION_TLS11;
}

static void sh_ticket(TlsServerHelloMsg *m, const TlsClientHelloMsg *h) {
    (void)h;
    m->ticket_supported = true;
}

static void sh_session_id(TlsServerHelloMsg *m, const TlsClientHelloMsg *h) {
    (void)h;
    m->session_id = slice_nil(TYPE_BYTE);
}

static void sh_compression(TlsServerHelloMsg *m, const TlsClientHelloMsg *h) {
    (void)h;
    m->compression_method = 1;
}

static void sh_tls12_suite(TlsServerHelloMsg *m, const TlsClientHelloMsg *h) {
    (void)h;
    m->cipher_suite = TLS_TLS_RSA_WITH_AES_128_GCM_SHA256;
}

/* Any non-empty bytes do for the cookie and the ECH extension, so these
 * borrow the client's legacy session ID. */
static void sh_cookie(TlsServerHelloMsg *m, const TlsClientHelloMsg *h) {
    m->cookie = h->session_id;
}

static void sh_no_key_share(TlsServerHelloMsg *m, const TlsClientHelloMsg *h) {
    (void)h;
    m->server_share.group = 0;
}

static void sh_other_group(TlsServerHelloMsg *m, const TlsClientHelloMsg *h) {
    (void)h;
    m->server_share.group = TLS_CURVE_P521;
}

static void sh_psk(TlsServerHelloMsg *m, const TlsClientHelloMsg *h) {
    (void)h;
    m->selected_identity_present = true;
}

static void sh_hrr(TlsServerHelloMsg *m) {
    memcpy(m->random.p, burrow__tls_hello_retry_request_random, 32);
    m->server_share.group = 0;
    m->server_share.data = slice_nil(TYPE_BYTE);
}

static void hrr_unnecessary(TlsServerHelloMsg *m, const TlsClientHelloMsg *h) {
    (void)h;
    sh_hrr(m);
}

static void hrr_same_group(TlsServerHelloMsg *m, const TlsClientHelloMsg *h) {
    sh_hrr(m);
    m->selected_group = ((const TlsKeyShare *)h->key_shares.p)[0].group;
}

static void hrr_other_group(TlsServerHelloMsg *m, const TlsClientHelloMsg *h) {
    (void)h;
    sh_hrr(m);
    m->selected_group = 0x0a0a;
}

static void hrr_server_share(TlsServerHelloMsg *m, const TlsClientHelloMsg *h) {
    TlsKeyShare share = m->server_share;
    sh_hrr(m);
    m->server_share = share;
    sh_cookie(m, h);
}

static void hrr_ech(TlsServerHelloMsg *m, const TlsClientHelloMsg *h) {
    sh_hrr(m);
    m->encrypted_client_hello = slice_sub(h->session_id, 0, 8);
}

typedef struct ServerHelloTLS13Test {
    const char *name;
    void (*mutate)(TlsServerHelloMsg *m, const TlsClientHelloMsg *h);
    Str want;
} ServerHelloTLS13Test;

static const ServerHelloTLS13Test server_hello_tls13_tests[] = {
    {"legacy version field", sh_legacy_version_field,
     BURROW_S_INIT("tls: server selected TLS 1.3 using the legacy version field")},
    {"legacy version", sh_legacy_version,
     BURROW_S_INIT("tls: server sent an incorrect legacy version")},
    {"ticket extension", sh_ticket,
     BURROW_S_INIT("tls: server sent a ServerHello extension forbidden in TLS 1.3")},
    {"session ID", sh_session_id,
     BURROW_S_INIT("tls: server did not echo the legacy session ID")},
    {"compression", sh_compression,
     BURROW_S_INIT("tls: server sent non-zero legacy TLS compression method")},
    {"TLS 1.2 suite", sh_tls12_suite,
     BURROW_S_INIT("tls: server chose an unconfigured cipher suite")},
    {"cookie", sh_cookie,
     BURROW_S_INIT("tls: server sent a cookie in a normal ServerHello")},
    {"no key share", sh_no_key_share,
     BURROW_S_INIT("tls: server did not send a key share")},
    {"other group", sh_other_group,
     BURROW_S_INIT("tls: server selected unsupported group")},
    {"PSK", sh_psk, BURROW_S_INIT("tls: server selected an invalid PSK")},
    {"unnecessary HRR", hrr_unnecessary,
     BURROW_S_INIT("tls: server sent an unnecessary HelloRetryRequest message")},
    {"HRR for a sent group", hrr_same_group,
     BURROW_S_INIT("tls: server sent an unnecessary HelloRetryRequest key_share")},
    {"HRR for another group", hrr_other_group,
     BURROW_S_INIT("tls: server selected unsupported group")},
    {"HRR with a key share", hrr_server_share,
     BURROW_S_INIT("tls: received malformed key_share extension")},
    {"HRR with ECH", hrr_ech,
     BURROW_S_INIT("tls: unexpected encrypted client hello extension in serverHello")},
};

/* Not in Go. Go checks these through its recorded handshakes and its own
 * server. This sends the client a TLS 1.3 ServerHello, or HelloRetryRequest,
 * with one thing wrong, and checks the error the handshake gives. */
static void TestClientTLS13ServerHelloErrors(TestingT *t) {
    for (size_t i = 0;
         i < sizeof server_hello_tls13_tests / sizeof server_hello_tls13_tests[0];
         i++) {
        const ServerHelloTLS13Test *tt = &server_hello_tls13_tests[i];
        Arena ar;
        arena_init(&ar, NULL, 0);
        Alloc *a = arena_allocator(&ar);
        NetConn c, s;
        net_pipe(a, &c, &s);

        TlsConfig config;
        memset(&config, 0, sizeof config);
        config.server_name = BURROW_S("foo");
        ClientJob job = {tls_client(heap_allocator(), c, &config), BURROW_NO_ERROR};
        if (job.c == NULL)
            testing_t_fatal_v(t, "out of memory");
        SyncWaitGroup wg = {0};
        sync_wait_group_go(&wg, BURROW_FN(Func, client_handshake_job, &job));

        Slice record = read_record(t, a, s);
        TlsClientHelloMsg hello = burrow__tls_client_hello_msg_zero();
        if (!burrow__tls_client_hello_msg_unmarshal(&hello, a, record) ||
            hello.key_shares.len == 0)
            testing_t_fatalf_v(t, "%s: unmarshaling ClientHello failed", tt->name);

        TlsServerHelloMsg server_hello = burrow__tls_server_hello_msg_zero();
        server_hello.vers = TLS_VERSION_TLS12;
        server_hello.random = slice_make(a, TYPE_BYTE, 32, 32);
        server_hello.session_id = hello.session_id;
        server_hello.cipher_suite = TLS_TLS_AES_128_GCM_SHA256;
        server_hello.supported_version = TLS_VERSION_TLS13;
        server_hello.server_share = ((const TlsKeyShare *)hello.key_shares.p)[0];
        tt->mutate(&server_hello, &hello);
        Error err = BURROW_NO_ERROR;
        Slice server_hello_bytes =
            burrow__tls_server_hello_msg_marshal(&server_hello, a, &err);
        if (BURROW_FAILED(err))
            testing_t_fatal_v(t, "%v", err);

        Byte header[5] = {
            TLS_RECORD_TYPE_HANDSHAKE,        (Byte)(TLS_VERSION_TLS12 >> 8),
            (Byte)(TLS_VERSION_TLS12 & 0xff), (Byte)(server_hello_bytes.len >> 8),
            (Byte)server_hello_bytes.len,
        };
        (void)s.vt->writer.write(s.data, slice_from(header, 5, 5, TYPE_BYTE), NULL);
        (void)s.vt->writer.write(s.data, server_hello_bytes, NULL);
        SyncWaitGroup drain = {0};
        sync_wait_group_go(&drain, BURROW_FN(Func, drain_job, &s));

        sync_wait_group_wait(&wg);
        if (!str_eq(error_text(job.err), tt->want))
            testing_t_errorf_v(t, "%s: got error %q, want %q", tt->name,
                               error_text(job.err), tt->want);

        (void)c.vt->closer.close(c.data);
        sync_wait_group_wait(&drain);
        (void)s.vt->closer.close(s.data);
        tls_conn_free(job.c);
        net_pipe_free(c);
        arena_free(&ar);
    }
}

/* discardConn is a net.Conn whose writes go nowhere. Not in Go: nothing reads
 * from it, and a read gets io.EOF. */
static Int discard_read(void *self, Slice b, Error *err) {
    (void)self;
    (void)b;
    BURROW_OUT(err, io_eof);
    return 0;
}

static Int discard_write(void *self, Slice b, Error *err) {
    (void)self;
    (void)err;
    return b.len;
}

static Error discard_close(void *self) {
    (void)self;
    return BURROW_NO_ERROR;
}

static const NetConnVT discard_vt = {
    {NULL, discard_read},
    {NULL, discard_write},
    {NULL, discard_close},
    NULL,
    NULL,
    NULL,
    NULL,
    NULL,
};

/* testConfigClient is not here yet, so the config only has its ServerName. The
 * key size check comes before anything else in it is looked at. Only the
 * client half is here; the second half, processCertsFromClient, is the server
 * handshake's. */
static void TestHandshakeRSATooBig(TestingT *t) {
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);

    /* largeRSAKeyCertPEM contains a 8193 bit RSA key */
    Slice rest;
    PemBlock *test_cert = pem_decode(a, tls_test_pem(a, large_rsa_key_cert_pem), &rest);
    if (test_cert == NULL)
        testing_t_fatal_v(t, "failed to decode largeRSAKeyCertPEM");

    TlsConfig config;
    memset(&config, 0, sizeof config);
    config.server_name = BURROW_S("test.golang.example");
    TlsConn *c = tls_client(heap_allocator(), (NetConn){&discard_vt, NULL}, &config);
    if (c == NULL)
        testing_t_fatal_v(t, "out of memory");
    Str expected_err = BURROW_S(
        "tls: server sent certificate containing RSA key larger than 8192 bits");
    Error err = burrow__tls_conn_verify_server_certificate(
        c, slice_from(&test_cert->bytes, 1, 1, TYPE_BYTES));
    if (BURROW_OK(err) || !str_eq(error_text(err), expected_err))
        testing_t_errorf_v(
            t, "Conn.verifyServerCertificate unexpected error: want %q, got %q",
            expected_err, error_text(err));
    tls_conn_free(c);
    arena_free(&ar);
}

static const char ech_test_config_list[] =
    "0041fe0d003d0100200020204bed0a11fc0dde595a9b78d966b0011128eb83f65d3c91c1cc5ac786c"
    "d246f000400010001ff0e6578616d706c652e676f6c616e670000";

static Slice ech_config_list(TestingT *t, Alloc *a) {
    Error err = BURROW_NO_ERROR;
    Slice b = hex_decode_string(a, str_from_cstr(ech_test_config_list), &err);
    if (BURROW_FAILED(err))
        testing_t_fatal_v(t, err);
    return b;
}

BURROW_SENTINEL_ERROR(ech_callback_err, "callback err");

static Error ech_verify_connection_ok(void *env, const TlsConnectionState *cs) {
    (void)env;
    (void)cs;
    return BURROW_NO_ERROR;
}

static Error ech_verify_connection_err(void *env, const TlsConnectionState *cs) {
    (void)env;
    (void)cs;
    return ech_callback_err;
}

static Error ech_verify_peer_certificate_err(void *env, Slice raw_certs,
                                             Slice verified_chains) {
    (void)env;
    (void)raw_certs;
    (void)verified_chains;
    return ech_callback_err;
}

typedef struct ECHRejectionCallbacksCase {
    const char *name;
    const char *expected_err;

    TlsVerifyConnectionFunc verify_connection;
    TlsVerifyPeerCertificateFunc verify_peer_certificate;
    TlsVerifyConnectionFunc encrypted_client_hello_rejection_verify;
} ECHRejectionCallbacksCase;

typedef struct ECHRejectionCallbacks {
    const ECHRejectionCallbacksCase *tc;
    TlsConfig *client_config, *server_config;
} ECHRejectionCallbacks;

/* Not in Go: a Server handshake run on its own goroutine, which closes its end
 * of the pipe after. */
typedef struct ServerJob {
    TlsConn *s;
    NetConn conn;
    Error err;
} ServerJob;

static void server_handshake_job(void *env) {
    ServerJob *j = (ServerJob *)env;
    j->err = tls_conn_handshake(j->s);
    (void)j->conn.vt->closer.close(j->conn.data);
}

static void ech_rejection_callbacks(void *env, TestingT *t) {
    const ECHRejectionCallbacks *e = env;
    const ECHRejectionCallbacksCase *tc = e->tc;
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    NetConn c, s;
    local_pipe(a, &c, &s);
    ServerJob job = {tls_server(heap_allocator(), s, e->server_config), s,
                     BURROW_NO_ERROR};
    if (job.s == NULL)
        testing_t_fatal_v(t, "out of memory");
    SyncWaitGroup wg = {0};
    sync_wait_group_go(&wg, BURROW_FN(Func, server_handshake_job, &job));

    TlsConfig c_config = *e->client_config;
    c_config.verify_connection = tc->verify_connection;
    c_config.verify_peer_certificate = tc->verify_peer_certificate;
    c_config.encrypted_client_hello_rejection_verify =
        tc->encrypted_client_hello_rejection_verify;

    TlsConn *cli = tls_client(heap_allocator(), c, &c_config);
    if (cli == NULL)
        testing_t_fatal_v(t, "out of memory");
    Error client_err = tls_conn_handshake(cli);
    (void)c.vt->closer.close(c.data);
    sync_wait_group_wait(&wg);

    Str expected_err = str_from_cstr(tc->expected_err);
    if (expected_err.len == 0 && BURROW_FAILED(client_err))
        testing_t_errorf_v(t, "unexpected err: %s", error_text(client_err));
    else if (BURROW_FAILED(client_err) && !str_eq(expected_err, error_text(client_err)))
        testing_t_errorf_v(t, "unexpected err: got %q, want %q", error_text(client_err),
                           expected_err);
    tls_conn_free(cli);
    tls_conn_free(job.s);
    local_pipe_free(c);
    arena_free(&ar);
}

static void TestTLS13ECHRejectionCallbacks(TestingT *t) {
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    Error err = BURROW_NO_ERROR;
    EcdsaPrivateKey *k = ecdsa_generate_key(a, elliptic_p256(), (IoReader){0}, &err);
    if (BURROW_FAILED(err)) {
        arena_free(&ar);
        testing_t_fatal_v(t, err);
    }
    Str dns_names[1] = {BURROW_S("example.golang")};
    X509Certificate tmpl;
    memset(&tmpl, 0, sizeof tmpl);
    tmpl.serial_number = big_new_int(a, 1);
    tmpl.subject.common_name = BURROW_S("test");
    tmpl.dns_names = slice_from(dns_names, 1, 1, TYPE_STRING);
    tmpl.not_before = time_add(test_time(NULL), -TIME_HOUR);
    tmpl.not_after = time_add(test_time(NULL), TIME_HOUR);
    Slice cert_der = x509_create_certificate(a, (IoReader){0}, &tmpl, &tmpl,
                                             ecdsa_private_key_public(k),
                                             ecdsa_private_key_signer(k), &err);
    X509Certificate *cert = NULL;
    if (!BURROW_FAILED(err))
        cert = x509_parse_certificate(a, cert_der, &err);
    if (BURROW_FAILED(err)) {
        arena_free(&ar);
        testing_t_fatal_v(t, err);
    }

    TlsConfig client_config = test_config_client(a);
    TlsConfig server_config = test_config_server(a);
    TlsCertificate certificate;
    memset(&certificate, 0, sizeof certificate);
    certificate.certificate = slice_from(&cert_der, 1, 1, TYPE_BYTES);
    certificate.private_key = BURROW_ANY(TYPE_ECDSA_PRIVATE_KEY, k);
    server_config.certificates = slice_from(&certificate, 1, 1, TYPE_TLS_CERTIFICATE);
    server_config.min_version = TLS_VERSION_TLS13;
    client_config.root_cas = x509_new_cert_pool(a);
    if (client_config.root_cas == NULL) {
        arena_free(&ar);
        testing_t_fatal_v(t, "out of memory");
    }
    x509_cert_pool_add_cert(client_config.root_cas, cert);
    client_config.min_version = TLS_VERSION_TLS13;
    client_config.encrypted_client_hello_config_list = ech_config_list(t, a);
    client_config.server_name = BURROW_S("example.golang");

    static const ECHRejectionCallbacksCase cases[] = {
        {
            .name = "no callbacks",
            .expected_err = "tls: server rejected ECH",
        },
        {
            .name = "EncryptedClientHelloRejectionVerify, no err",
            .encrypted_client_hello_rejection_verify = {ech_verify_connection_ok, NULL},
            .expected_err = "tls: server rejected ECH",
        },
        {
            .name = "EncryptedClientHelloRejectionVerify, err",
            .encrypted_client_hello_rejection_verify = {ech_verify_connection_err,
                                                        NULL},
            /* testHandshake returns the server side error, so we just need to
             * check alertBadCertificate was sent */
            .expected_err = "callback err",
        },
        {
            .name = "VerifyConnection, err",
            .verify_connection = {ech_verify_connection_err, NULL},
            .expected_err = "tls: server rejected ECH",
        },
        {
            .name = "VerifyPeerCertificate, err",
            .verify_peer_certificate = {ech_verify_peer_certificate_err, NULL},
            .expected_err = "tls: server rejected ECH",
        },
    };
    for (size_t i = 0; i < sizeof cases / sizeof cases[0]; i++) {
        ECHRejectionCallbacks env = {&cases[i], &client_config, &server_config};
        testing_t_run(t, str_from_cstr(cases[i].name),
                      BURROW_FN(TestingTFunc, ech_rejection_callbacks, &env));
    }

    tls_config_release(&server_config);
    tls_config_release(&client_config);
    arena_free(&ar);
}

static void TestECHTLS12Server(TestingT *t) {
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    TlsConfig client_config = test_config_client(a);
    TlsConfig server_config = test_config_server(a);

    server_config.max_version = TLS_VERSION_TLS12;
    client_config.min_version = 0;

    client_config.encrypted_client_hello_config_list = ech_config_list(t, a);

    Str expected_err = BURROW_S(
        "server: tls: client offered only unsupported versions: [304]\nclient: "
        "remote error: tls: protocol version not supported");
    Handshake h;
    test_handshake(t, &client_config, &server_config, &h);
    if (BURROW_OK(h.err) || !str_eq(error_text(h.err), expected_err)) {
        testing_t_errorf_v(t, "unexpected handshake error: got %q, want %q",
                           BURROW_OK(h.err) ? BURROW_S("<nil>") : error_text(h.err),
                           expected_err);
        handshake_free(&h);
        tls_config_release(&server_config);
        tls_config_release(&client_config);
        arena_free(&ar);
        testing_t_fail_now(t);
    }
    handshake_free(&h);
    tls_config_release(&server_config);
    tls_config_release(&client_config);
    arena_free(&ar);
}

#define TESTS(X)                                                                       \
    X(TestHandshakeClientRSARC4)                                                       \
    X(TestHandshakeClientRSAAES128GCM)                                                 \
    X(TestHandshakeClientRSAAES256GCM)                                                 \
    X(TestHandshakeClientECDHERSAAES)                                                  \
    X(TestHandshakeClientECDHEECDSAAES)                                                \
    X(TestHandshakeClientECDHEECDSAAESGCM)                                             \
    X(TestHandshakeClientAES256GCMSHA384)                                              \
    X(TestHandshakeClientAES128CBCSHA256)                                              \
    X(TestHandshakeClientECDHERSAAES128CBCSHA256)                                      \
    X(TestHandshakeClientECDHEECDSAAES128CBCSHA256)                                    \
    X(TestHandshakeClientX25519)                                                       \
    X(TestHandshakeClientP256)                                                         \
    X(TestHandshakeClientHelloRetryRequest)                                            \
    X(TestHandshakeClientECDHERSAChaCha20)                                             \
    X(TestHandshakeClientECDHEECDSAChaCha20)                                           \
    X(TestHandshakeClientAES128SHA256)                                                 \
    X(TestHandshakeClientAES256SHA384)                                                 \
    X(TestHandshakeClientCHACHA20SHA256)                                               \
    X(TestHandshakeClientRSATLS13)                                                     \
    X(TestHandshakeClientECDSATLS13)                                                   \
    X(TestHandshakeClientEd25519)                                                      \
    X(TestHandshakeClientCertEd25519)                                                  \
    X(TestHandshakeClientCertRSA)                                                      \
    X(TestHandshakeClientCertECDSA)                                                    \
    X(TestHandshakeClientCertRSAPSS)                                                   \
    X(TestHandshakeClientCertRSAPKCS1v15)                                              \
    X(TestClientKeyUpdate)                                                             \
    X(TestResumption)                                                                  \
    X(TestLRUClientSessionCache)                                                       \
    X(TestKeyLogTLS12)                                                                 \
    X(TestKeyLogTLS13)                                                                 \
    X(TestHandshakeClientALPNMatch)                                                    \
    X(TestServerSelectingUnconfiguredApplicationProtocol)                              \
    X(TestHandshakeClientSCTs)                                                         \
    X(TestRenegotiationRejected)                                                       \
    X(TestRenegotiateOnce)                                                             \
    X(TestRenegotiateTwice)                                                            \
    X(TestRenegotiateTwiceRejected)                                                    \
    X(TestHandshakeClientExportKeyingMaterial)                                         \
    X(TestHostnameInSNI)                                                               \
    X(TestServerSelectingUnconfiguredCipherSuite)                                      \
    X(TestVerifyConnection)                                                            \
    X(TestVerifyPeerCertificate)                                                       \
    X(TestFailedWrite)                                                                 \
    X(TestBuffering)                                                                   \
    X(TestAlertFlushing)                                                               \
    X(TestHandshakeRace)                                                               \
    X(TestGetClientCertificate)                                                        \
    X(TestRSAPSSKeyError)                                                              \
    X(TestCloseClientConnectionOnIdleServer)                                           \
    X(TestDowngradeCanary)                                                             \
    X(TestResumptionKeepsOCSPAndSCT)                                                   \
    X(TestClientHandshakeContextCancellation)                                          \
    X(TestTLS13OnlyClientHelloCipherSuite)                                             \
    X(TestClientTLS13ServerHelloErrors)                                                \
    X(TestHandshakeRSATooBig)                                                          \
    X(TestTLS13ECHRejectionCallbacks)                                                  \
    X(TestECHTLS12Server)

TESTING_MAIN(TESTS)
