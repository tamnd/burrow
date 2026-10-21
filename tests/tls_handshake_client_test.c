/* Derived from Go's src/crypto/tls/handshake_client_test.go.
 * Go source: go1.27.1.
 *
 * The tests that replay Go's recorded handshakes do it through
 * tls_replay_test.h, with the recordings tools/gen-tls-replay-tests.sh put in
 * tls_replay_client_gen.h. The ECH tests run against this package's own
 * server.
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
    X(TestLRUClientSessionCache)                                                       \
    X(TestHandshakeClientALPNMatch)                                                    \
    X(TestHandshakeClientSCTs)                                                         \
    X(TestRenegotiationRejected)                                                       \
    X(TestRenegotiateOnce)                                                             \
    X(TestRenegotiateTwice)                                                            \
    X(TestRenegotiateTwiceRejected)                                                    \
    X(TestHandshakeClientExportKeyingMaterial)                                         \
    X(TestHostnameInSNI)                                                               \
    X(TestServerSelectingUnconfiguredCipherSuite)                                      \
    X(TestClientTLS13ServerHelloErrors)                                                \
    X(TestHandshakeRSATooBig)                                                          \
    X(TestTLS13ECHRejectionCallbacks)                                                  \
    X(TestECHTLS12Server)

TESTING_MAIN(TESTS)
