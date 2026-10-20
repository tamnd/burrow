/* crypto/tls: handshake_server_test.go, the tests that need no TLS 1.3 server
 * and no recorded handshakes. The others come with handshake_server_tls13.go
 * and the testdata they replay.
 *
 * Copyright 2009 The Go Authors. All rights reserved.
 * Copyright 2026 The burrow Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style licence that can be found
 * in the LICENSE file. */

#include "check.h"

#include "../src/crypto/tls_internal.h"
#include "burrow/burrow.h"
#include "burrow/crypto/tls.h"
#include "burrow/encoding/pem.h"
#include "burrow/mem/heap.h"
#include "burrow/strings.h"
#include "tls_test_gen.h"

#include <string.h>

static Time test_time(void *env) {
    (void)env;
    return time_from_unix(1476984729, 0);
}

/* Not in Go: testConfigServer, made in a. tls_config_release gives back the
 * session ticket keys it comes to hold. */
static TlsConfig test_config_server(Alloc *a) {
    TlsConfig config;
    memset(&config, 0, sizeof config);
    config.time = (TlsTimeFunc){test_time, NULL};
    Slice certs = slice_make(a, TYPE_TLS_CERTIFICATE, 7, 7);
    TlsCertificate *c = certs.p;
    c[0] = parse_test_cert(a, test_ecdsap256_cert_pem, test_ecdsap256_key_pem);
    c[1] = parse_test_cert(a, test_rsa2048_cert_pem, test_rsa2048_key_pem);
    c[2] = parse_test_cert(a, test_ed25519_cert_pem, test_ed25519_key_pem);
    c[3] = parse_test_cert(a, test_sni_cert_pem, test_sni_key_pem);
    c[4] = parse_test_cert(a, test_mldsa44_cert_pem, test_mldsa44_key_pem);
    c[5] = parse_test_cert(a, test_mldsa65_cert_pem, test_mldsa65_key_pem);
    c[6] = parse_test_cert(a, test_mldsa87_cert_pem, test_mldsa87_key_pem);
    config.certificates = certs;
    config.client_cas = x509_new_cert_pool(a);
    x509_cert_pool_add_cert(
        config.client_cas,
        parse_test_cert(a, test_client_root_cert_pem, test_client_root_key_pem).leaf);
    return config;
}

/* Not in Go: testConfigClient, made in a. */
static TlsConfig test_config_client(Alloc *a) {
    TlsConfig config;
    memset(&config, 0, sizeof config);
    config.time = (TlsTimeFunc){test_time, NULL};
    Slice certs = slice_make(a, TYPE_TLS_CERTIFICATE, 6, 6);
    TlsCertificate *c = certs.p;
    c[0] = parse_test_cert(a, test_client_ecdsap256_cert_pem,
                           test_client_ecdsap256_key_pem);
    c[1] =
        parse_test_cert(a, test_client_rsa2048_cert_pem, test_client_rsa2048_key_pem);
    c[2] =
        parse_test_cert(a, test_client_ed25519_cert_pem, test_client_ed25519_key_pem);
    c[3] =
        parse_test_cert(a, test_client_mldsa44_cert_pem, test_client_mldsa44_key_pem);
    c[4] =
        parse_test_cert(a, test_client_mldsa65_cert_pem, test_client_mldsa65_key_pem);
    c[5] =
        parse_test_cert(a, test_client_mldsa87_cert_pem, test_client_mldsa87_key_pem);
    config.certificates = certs;
    config.root_cas = x509_new_cert_pool(a);
    x509_cert_pool_add_cert(
        config.root_cas,
        parse_test_cert(a, test_root_cert_pem, test_root_key_pem).leaf);
    config.server_name = BURROW_S("test.golang.example");
    return config;
}

/* Not in Go: the goroutine the tests start for the client side. It writes m
 * from a Client whose vers is the ClientHello's, then reads the reply when
 * reply_kind says how, and closes c. The Client is left for the test to free,
 * since the errors it keeps go with it. */
typedef enum ReplyKind { REPLY_NONE, REPLY_MESSAGE, REPLY_BYTES } ReplyKind;

typedef struct ClientJob {
    NetConn c;
    TlsConfig *config;
    const TlsHandshakeMsg *m;
    ReplyKind reply_kind;

    TlsConn *cli;
    Error write_err;
    Arena ar; /* what the reply is made in */
    TlsHandshakeMsg reply;
    Error reply_err;
    Byte buf[1024];
    Int n;
} ClientJob;

static void client_job(void *env) {
    ClientJob *j = (ClientJob *)env;
    j->cli = tls_client(heap_allocator(), j->c, j->config);
    if (j->cli == NULL) {
        j->write_err = burrow_err_out_of_memory;
        (void)j->c.vt->closer.close(j->c.data);
        return;
    }
    if (j->m->kind == TLS_MSG_CLIENT_HELLO)
        j->cli->vers = j->m->client_hello.vers;
    IoWriter none = {NULL, NULL};
    (void)burrow__tls_conn_write_handshake_record(j->cli, j->m, none, &j->write_err);
    if (!BURROW_FAILED(j->write_err)) {
        if (j->reply_kind == REPLY_MESSAGE)
            j->reply_err = burrow__tls_conn_read_handshake(
                j->cli, none, arena_allocator(&j->ar), &j->reply);
        else if (j->reply_kind == REPLY_BYTES)
            j->n = j->c.vt->reader.read(
                j->c.data, slice_from(j->buf, 1024, 1024, TYPE_BYTE), &j->reply_err);
    }
    (void)j->c.vt->closer.close(j->c.data);
}

static void client_job_init(ClientJob *j, NetConn c, TlsConfig *config,
                            const TlsHandshakeMsg *m, ReplyKind reply_kind) {
    memset(j, 0, sizeof *j);
    j->c = c;
    j->config = config;
    j->m = m;
    j->reply_kind = reply_kind;
    arena_init(&j->ar, NULL, 0);
}

static void client_job_free(ClientJob *j) {
    if (j->cli != NULL)
        tls_conn_free(j->cli);
    arena_free(&j->ar);
}

/* Not in Go: the TLS 1.3 half of testClientHelloFailure comes with
 * handshake_server_tls13.go. */
static void test_client_hello_failure(TestingT *t, TlsConfig *server_config,
                                      const TlsHandshakeMsg *m,
                                      const char *expected_sub_str) {
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    NetConn c, s;
    net_pipe(a, &c, &s);
    TlsConfig client_config = test_config_client(a);
    ClientJob job;
    client_job_init(&job, c, &client_config, m, REPLY_NONE);
    SyncWaitGroup wg = {0};
    sync_wait_group_go(&wg, BURROW_FN(Func, client_job, &job));

    Context ctx = context_background();
    TlsConn *conn = tls_server(heap_allocator(), s, server_config);
    if (conn == NULL)
        testing_t_fatal_v(t, "out of memory");
    TlsHandshakeMsg *ch = NULL;
    Error err = burrow__tls_conn_read_client_hello(conn, ctx, a, &ch);
    TlsServerHandshakeState hs;
    memset(&hs, 0, sizeof hs);
    hs.c = conn;
    hs.ctx = ctx;
    hs.master_secret = slice_nil(TYPE_BYTE);
    hs.a = a;
    if (!BURROW_FAILED(err)) {
        hs.client_hello_msg = ch;
        hs.client_hello = &ch->client_hello;
        err = burrow__tls_server_handshake_state_process_client_hello(&hs);
    }
    if (!BURROW_FAILED(err))
        err = burrow__tls_server_handshake_state_pick_cipher_suite(&hs);
    (void)s.vt->closer.close(s.data);
    sync_wait_group_wait(&wg);
    if (BURROW_FAILED(job.write_err))
        testing_t_fatal_v(t, "%v", job.write_err);
    if (expected_sub_str[0] == '\0') {
        if (BURROW_FAILED(err) && !errors_is(err, io_eof))
            testing_t_errorf_v(t, "Got error: %s; expected to succeed",
                               error_text(err));
    } else if (!BURROW_FAILED(err) ||
               !strings_contains(error_text(err), str_from_cstr(expected_sub_str))) {
        testing_t_errorf_v(t, "Got error: %v; expected to match substring '%s'", err,
                           str_from_cstr(expected_sub_str));
    }

    tls_conn_free(conn);
    client_job_free(&job);
    net_pipe_free(c);
    tls_config_release(&client_config);
    arena_free(&ar);
}

static void test_client_hello(TestingT *t, TlsConfig *server_config,
                              const TlsHandshakeMsg *m) {
    test_client_hello_failure(t, server_config, m, "");
}

/* Not in Go: a ClientHello of version vers, with a random of 32 zero bytes
 * from a, as &clientHelloMsg{vers: vers, random: make([]byte, 32)} is. */
static TlsHandshakeMsg client_hello(Alloc *a, uint16_t vers) {
    TlsHandshakeMsg m;
    burrow__tls_handshake_msg_init(&m, TLS_MSG_CLIENT_HELLO);
    m.client_hello.vers = vers;
    m.client_hello.random = slice_make(a, TYPE_BYTE, 32, 32);
    return m;
}

static void TestSimpleError(TestingT *t) {
    Arena ar;
    arena_init(&ar, NULL, 0);
    TlsConfig config = test_config_server(arena_allocator(&ar));
    TlsHandshakeMsg m;
    burrow__tls_handshake_msg_init(&m, TLS_MSG_SERVER_HELLO_DONE);
    test_client_hello_failure(t, &config, &m, "unexpected handshake message");
    tls_config_release(&config);
    arena_free(&ar);
}

static const uint16_t bad_protocol_versions[] = {
    0x0000, 0x0005, 0x0100, 0x0105, 0x0200, 0x0205, TLS_VERSION_SSL30,
};

static void TestRejectBadProtocolVersion(TestingT *t) {
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    TlsConfig config = test_config_server(a);
    config.min_version = TLS_VERSION_SSL30;
    Int n = (Int)(sizeof bad_protocol_versions / sizeof bad_protocol_versions[0]);
    for (Int i = 0; i < n; i++) {
        TlsHandshakeMsg m = client_hello(a, bad_protocol_versions[i]);
        test_client_hello_failure(t, &config, &m, "unsupported versions");
    }
    TlsHandshakeMsg m = client_hello(a, TLS_VERSION_TLS12);
    m.client_hello.supported_versions =
        slice_from((uint16_t *)(uintptr_t)bad_protocol_versions, n, n, TYPE_UINT16);
    test_client_hello_failure(t, &config, &m, "unsupported versions");
    tls_config_release(&config);
    arena_free(&ar);
}

static Byte compression_none[1] = {TLS_COMPRESSION_NONE};

static void TestNoSuiteOverlap(TestingT *t) {
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    uint16_t suites[1] = {0xff00};
    TlsHandshakeMsg m = client_hello(a, TLS_VERSION_TLS12);
    m.client_hello.cipher_suites = slice_from(suites, 1, 1, TYPE_UINT16);
    m.client_hello.compression_methods = slice_from(compression_none, 1, 1, TYPE_BYTE);
    TlsConfig config = test_config_server(a);
    test_client_hello_failure(t, &config, &m,
                              "no cipher suite supported by both client and server");
    tls_config_release(&config);
    arena_free(&ar);
}

static void TestNoCompressionOverlap(TestingT *t) {
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    uint16_t suites[1] = {TLS_TLS_ECDHE_RSA_WITH_AES_128_GCM_SHA256};
    Byte compression[1] = {0xff};
    TlsHandshakeMsg m = client_hello(a, TLS_VERSION_TLS12);
    m.client_hello.cipher_suites = slice_from(suites, 1, 1, TYPE_UINT16);
    m.client_hello.compression_methods = slice_from(compression, 1, 1, TYPE_BYTE);
    TlsConfig config = test_config_server(a);
    test_client_hello_failure(t, &config, &m,
                              "client does not support uncompressed connections");
    tls_config_release(&config);
    arena_free(&ar);
}

static void TestNoRC4ByDefault(TestingT *t) {
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    uint16_t suites[1] = {TLS_TLS_RSA_WITH_RC4_128_SHA};
    TlsHandshakeMsg m = client_hello(a, TLS_VERSION_TLS12);
    m.client_hello.cipher_suites = slice_from(suites, 1, 1, TYPE_UINT16);
    m.client_hello.compression_methods = slice_from(compression_none, 1, 1, TYPE_BYTE);
    TlsConfig server_config = test_config_server(a);
    /* Reset the enabled cipher suites to nil in order to test the defaults. */
    server_config.cipher_suites = slice_nil(TYPE_UINT16);
    test_client_hello_failure(t, &server_config, &m,
                              "no cipher suite supported by both client and server");
    tls_config_release(&server_config);
    arena_free(&ar);
}

static void TestRejectSNIWithTrailingDot(TestingT *t) {
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    TlsHandshakeMsg m = client_hello(a, TLS_VERSION_TLS12);
    m.client_hello.server_name = BURROW_S("foo.com.");
    TlsConfig config = test_config_server(a);
    test_client_hello_failure(t, &config, &m, "decoding message");
    tls_config_release(&config);
    arena_free(&ar);
}

static uint16_t curves_p256[1] = {TLS_CURVE_P256};
static Byte points_uncompressed[1] = {TLS_POINT_FORMAT_UNCOMPRESSED};

static void TestDontSelectECDSAWithRSAKey(TestingT *t) {
    /* Test that, even when both sides support an ECDSA cipher suite, it won't
     * be selected if the server's private key doesn't support it. */
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    uint16_t suites[1] = {TLS_TLS_ECDHE_ECDSA_WITH_AES_256_GCM_SHA384};
    TlsHandshakeMsg m = client_hello(a, TLS_VERSION_TLS12);
    m.client_hello.cipher_suites = slice_from(suites, 1, 1, TYPE_UINT16);
    m.client_hello.compression_methods = slice_from(compression_none, 1, 1, TYPE_BYTE);
    m.client_hello.supported_curves = slice_from(curves_p256, 1, 1, TYPE_UINT16);
    m.client_hello.supported_points = slice_from(points_uncompressed, 1, 1, TYPE_BYTE);
    TlsConfig server_config = test_config_server(a);
    server_config.cipher_suites = m.client_hello.cipher_suites;
    TlsCertificate ecdsa_cert =
        parse_test_cert(a, test_ecdsap256_cert_pem, test_ecdsap256_key_pem);
    server_config.certificates = slice_from(&ecdsa_cert, 1, 1, TYPE_TLS_CERTIFICATE);
    Error err = tls_config_build_name_to_certificate(&server_config, a);
    if (BURROW_FAILED(err))
        testing_t_fatal_v(t, "%v", err);
    /* First test that it *does* work when the server's key is ECDSA. */
    test_client_hello(t, &server_config, &m);

    /* Now test that switching to an RSA key causes the expected error (and not
     * an internal error about a signing failure). */
    TlsCertificate rsa_cert =
        parse_test_cert(a, test_rsa2048_cert_pem, test_rsa2048_key_pem);
    server_config.certificates = slice_from(&rsa_cert, 1, 1, TYPE_TLS_CERTIFICATE);
    test_client_hello_failure(t, &server_config, &m,
                              "no cipher suite supported by both client and server");
    tls_config_release(&server_config);
    arena_free(&ar);
}

static void TestDontSelectRSAWithECDSAKey(TestingT *t) {
    /* Test that, even when both sides support an RSA cipher suite, it won't be
     * selected if the server's private key doesn't support it. */
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    uint16_t suites[1] = {TLS_TLS_ECDHE_RSA_WITH_AES_128_GCM_SHA256};
    TlsHandshakeMsg m = client_hello(a, TLS_VERSION_TLS12);
    m.client_hello.cipher_suites = slice_from(suites, 1, 1, TYPE_UINT16);
    m.client_hello.compression_methods = slice_from(compression_none, 1, 1, TYPE_BYTE);
    m.client_hello.supported_curves = slice_from(curves_p256, 1, 1, TYPE_UINT16);
    m.client_hello.supported_points = slice_from(points_uncompressed, 1, 1, TYPE_BYTE);
    TlsConfig server_config = test_config_server(a);
    server_config.cipher_suites = m.client_hello.cipher_suites;
    /* First test that it *does* work when the server's key is RSA. */
    test_client_hello(t, &server_config, &m);

    /* Now test that switching to an ECDSA key causes the expected error (and
     * not an internal error about a signing failure). */
    TlsCertificate ecdsa_cert =
        parse_test_cert(a, test_ecdsap256_cert_pem, test_ecdsap256_key_pem);
    server_config.certificates = slice_from(&ecdsa_cert, 1, 1, TYPE_TLS_CERTIFICATE);
    Error err = tls_config_build_name_to_certificate(&server_config, a);
    if (BURROW_FAILED(err))
        testing_t_fatal_v(t, "%v", err);
    test_client_hello_failure(t, &server_config, &m,
                              "no cipher suite supported by both client and server");
    tls_config_release(&server_config);
    arena_free(&ar);
}

static void TestRenegotiationExtension(TestingT *t) {
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    uint16_t suites[1] = {TLS_TLS_ECDHE_RSA_WITH_AES_128_GCM_SHA256};
    TlsHandshakeMsg m = client_hello(a, TLS_VERSION_TLS12);
    m.client_hello.compression_methods = slice_from(compression_none, 1, 1, TYPE_BYTE);
    m.client_hello.secure_renegotiation_supported = true;
    m.client_hello.cipher_suites = slice_from(suites, 1, 1, TYPE_UINT16);
    m.client_hello.supported_curves = slice_from(curves_p256, 1, 1, TYPE_UINT16);
    m.client_hello.supported_points = slice_from(points_uncompressed, 1, 1, TYPE_BYTE);

    NetConn c, s;
    net_pipe(a, &c, &s);
    TlsConfig client_config = test_config_client(a);
    ClientJob job;
    client_job_init(&job, c, &client_config, &m, REPLY_BYTES);
    SyncWaitGroup wg = {0};
    sync_wait_group_go(&wg, BURROW_FN(Func, client_job, &job));

    TlsConfig server_config = test_config_server(a);
    TlsConn *srv = tls_server(heap_allocator(), s, &server_config);
    if (srv == NULL)
        testing_t_fatal_v(t, "out of memory");
    (void)tls_conn_handshake(srv);
    sync_wait_group_wait(&wg);
    if (BURROW_FAILED(job.write_err))
        testing_t_fatal_v(t, "%v", job.write_err);
    if (BURROW_FAILED(job.reply_err))
        testing_t_errorf_v(t, "Server read returned error: %s",
                           error_text(job.reply_err));
    Slice buf = slice_from(job.buf, job.n, job.n, TYPE_BYTE);

    if (buf.len < 5 + 4)
        testing_t_fatalf_v(t, "Server returned short message of length %d", buf.len);
    /* buf contains a TLS record, with a 5 byte record header and a 4 byte
     * handshake header. The length of the ServerHello is taken from the
     * handshake header. */
    Int server_hello_len =
        (Int)job.buf[6] << 16 | (Int)job.buf[7] << 8 | (Int)job.buf[8];

    TlsServerHelloMsg server_hello = burrow__tls_server_hello_msg_zero();
    /* unmarshal expects to be given the handshake header, but serverHelloLen
     * doesn't include it.
     *
     * Not in Go: a ServerHello longer than what was read fails to parse rather
     * than panic. */
    if (9 + server_hello_len > buf.len ||
        !burrow__tls_server_hello_msg_unmarshal(
            &server_hello, a, slice_sub(buf, 5, 9 + server_hello_len)))
        testing_t_fatalf_v(t, "Failed to parse ServerHello");

    if (!server_hello.secure_renegotiation_supported)
        testing_t_errorf_v(t, "Secure renegotiation extension was not echoed.");

    tls_conn_free(srv);
    client_job_free(&job);
    (void)s.vt->closer.close(s.data);
    net_pipe_free(c);
    tls_config_release(&server_config);
    tls_config_release(&client_config);
    arena_free(&ar);
}

/* Not in Go: what TestTLS12OnlyCipherSuites and TestTLSPointFormats share.
 * The ClientHello m goes to a Server with server_config, and the first
 * message the Server answers with comes back in job, which the caller frees
 * with client_job_free. */
static void server_reply(TestingT *t, Alloc *a, TlsConfig *client_config,
                         TlsConfig *server_config, const TlsHandshakeMsg *m,
                         ClientJob *job) {
    NetConn c, s;
    net_pipe(a, &c, &s);
    client_job_init(job, c, client_config, m, REPLY_MESSAGE);
    SyncWaitGroup wg = {0};
    sync_wait_group_go(&wg, BURROW_FN(Func, client_job, job));
    TlsConn *srv = tls_server(heap_allocator(), s, server_config);
    if (srv == NULL)
        testing_t_fatal_v(t, "out of memory");
    (void)tls_conn_handshake(srv);
    (void)s.vt->closer.close(s.data);
    sync_wait_group_wait(&wg);
    tls_conn_free(srv);
    net_pipe_free(c);
    if (BURROW_FAILED(job->write_err))
        testing_t_fatal_v(t, "%v", job->write_err);
}

static void TestTLS12OnlyCipherSuites(TestingT *t) {
    /* Test that a Server doesn't select a TLS 1.2-only cipher suite when the
     * client negotiates TLS 1.1. */
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    uint16_t suites[2] = {
        /* The Server, by default, will use the client's preference order. So
         * the GCM cipher suite will be selected unless it's excluded because
         * of the version in this ClientHello. */
        TLS_TLS_ECDHE_RSA_WITH_AES_128_GCM_SHA256,
        TLS_TLS_RSA_WITH_RC4_128_SHA,
    };
    uint16_t curves[3] = {TLS_CURVE_P256, TLS_CURVE_P384, TLS_CURVE_P521};
    TlsHandshakeMsg m = client_hello(a, TLS_VERSION_TLS11);
    m.client_hello.cipher_suites = slice_from(suites, 2, 2, TYPE_UINT16);
    m.client_hello.compression_methods = slice_from(compression_none, 1, 1, TYPE_BYTE);
    m.client_hello.supported_curves = slice_from(curves, 3, 3, TYPE_UINT16);
    m.client_hello.supported_points = slice_from(points_uncompressed, 1, 1, TYPE_BYTE);

    TlsConfig client_config = test_config_client(a);
    TlsConfig config = test_config_server(a);
    config.cipher_suites = m.client_hello.cipher_suites;
    config.min_version = TLS_VERSION_TLS10;
    ClientJob job;
    server_reply(t, a, &client_config, &config, &m, &job);
    if (BURROW_FAILED(job.reply_err))
        testing_t_fatal_v(t, "%v", job.reply_err);
    if (job.reply.kind != TLS_MSG_SERVER_HELLO)
        testing_t_fatalf_v(t, "didn't get ServerHello message in reply. Got %s\n",
                           burrow__tls_handshake_msg_type_name(job.reply.kind));
    uint16_t suite = job.reply.server_hello.cipher_suite;
    if (suite != TLS_TLS_RSA_WITH_RC4_128_SHA)
        testing_t_fatalf_v(t, "bad cipher suite from server: %x", suite);

    client_job_free(&job);
    tls_config_release(&config);
    tls_config_release(&client_config);
    arena_free(&ar);
}

typedef struct PointFormatsTest {
    const char *name;
    uint16_t cipher_suite;
    int ncurves; /* -1 for nil */
    const Byte *supported_points;
    int npoints; /* -1 for nil */
    bool want_supported_points;
} PointFormatsTest;

static const Byte extra_points[4] = {13, 37, TLS_POINT_FORMAT_UNCOMPRESSED, 42};

static const PointFormatsTest point_formats_tests[] = {
    {"ECC", TLS_TLS_ECDHE_RSA_WITH_AES_128_GCM_SHA256, 1, points_uncompressed, 1, true},
    {"ECC without ec_point_format", TLS_TLS_ECDHE_RSA_WITH_AES_128_GCM_SHA256, 1, NULL,
     -1, false},
    {"ECC with extra values", TLS_TLS_ECDHE_RSA_WITH_AES_128_GCM_SHA256, 1,
     extra_points, 4, true},
    {"RSA", TLS_TLS_RSA_WITH_AES_256_GCM_SHA384, -1, NULL, -1, false},
    {"RSA with ec_point_format", TLS_TLS_RSA_WITH_AES_256_GCM_SHA384, -1,
     points_uncompressed, 1, false},
};

static void point_formats(void *env, TestingT *t) {
    const PointFormatsTest *tt = (const PointFormatsTest *)env;
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    uint16_t suites[1] = {tt->cipher_suite};
    TlsHandshakeMsg m = client_hello(a, TLS_VERSION_TLS12);
    m.client_hello.cipher_suites = slice_from(suites, 1, 1, TYPE_UINT16);
    m.client_hello.compression_methods = slice_from(compression_none, 1, 1, TYPE_BYTE);
    if (tt->ncurves >= 0)
        m.client_hello.supported_curves =
            slice_from(curves_p256, tt->ncurves, tt->ncurves, TYPE_UINT16);
    if (tt->npoints >= 0)
        m.client_hello.supported_points =
            slice_from((Byte *)(uintptr_t)tt->supported_points, tt->npoints,
                       tt->npoints, TYPE_BYTE);

    TlsCertificate rsa_cert =
        parse_test_cert(a, test_rsa2048_cert_pem, test_rsa2048_key_pem);
    TlsConfig client_config = test_config_client(a);
    client_config.certificates = slice_from(&rsa_cert, 1, 1, TYPE_TLS_CERTIFICATE);
    TlsConfig server_config = test_config_server(a);
    server_config.certificates = slice_from(&rsa_cert, 1, 1, TYPE_TLS_CERTIFICATE);
    server_config.cipher_suites = m.client_hello.cipher_suites;
    ClientJob job;
    server_reply(t, a, &client_config, &server_config, &m, &job);
    if (BURROW_FAILED(job.reply_err))
        testing_t_fatal_v(t, "%v", job.reply_err);
    if (job.reply.kind != TLS_MSG_SERVER_HELLO)
        testing_t_fatalf_v(t, "didn't get ServerHello message in reply. Got %s\n",
                           burrow__tls_handshake_msg_type_name(job.reply.kind));
    Slice points = job.reply.server_hello.supported_points;
    if (tt->want_supported_points) {
        if (points.len != 1 ||
            ((const Byte *)points.p)[0] != TLS_POINT_FORMAT_UNCOMPRESSED)
            testing_t_fatal_v(t, "incorrect ec_point_format extension from server");
    } else if (points.len != 0) {
        testing_t_fatalf_v(t, "unexpected ec_point_format extension from server: %v",
                           points);
    }

    client_job_free(&job);
    tls_config_release(&server_config);
    tls_config_release(&client_config);
    arena_free(&ar);
}

static void TestTLSPointFormats(TestingT *t) {
    /* Test that a Server returns the ec_point_format extension when ECC is
     * negotiated, and not on a RSA handshake or if ec_point_format is
     * missing. */
    Int n = (Int)(sizeof point_formats_tests / sizeof point_formats_tests[0]);
    for (Int i = 0; i < n; i++) {
        const PointFormatsTest *tt = &point_formats_tests[i];
        testing_t_run(t, str_from_cstr(tt->name),
                      BURROW_FN(TestingTFunc, point_formats, (void *)(uintptr_t)tt));
    }
}

/* Not in Go: the goroutine of TestAlertForwarding. */
static void alert_job(void *env) {
    ClientJob *j = (ClientJob *)env;
    j->cli = tls_client(heap_allocator(), j->c, j->config);
    if (j->cli != NULL)
        (void)burrow__tls_conn_send_alert(j->cli, TLS_ALERT_UNKNOWN_CA);
    (void)j->c.vt->closer.close(j->c.data);
}

static void TestAlertForwarding(TestingT *t) {
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    NetConn c, s;
    net_pipe(a, &c, &s);
    TlsConfig client_config = test_config_client(a);
    ClientJob job;
    client_job_init(&job, c, &client_config, NULL, REPLY_NONE);
    SyncWaitGroup wg = {0};
    sync_wait_group_go(&wg, BURROW_FN(Func, alert_job, &job));

    TlsConfig server_config = test_config_server(a);
    TlsConn *srv = tls_server(heap_allocator(), s, &server_config);
    if (srv == NULL)
        testing_t_fatal_v(t, "out of memory");
    Error err = tls_conn_handshake(srv);
    (void)s.vt->closer.close(s.data);
    sync_wait_group_wait(&wg);
    const NetOpError *op_err = errors_as(err, TYPE_NET_OP_ERROR);
    uint8_t alert = 0;
    if (op_err == NULL || !burrow__tls_is_alert(op_err->err, &alert) ||
        alert != TLS_ALERT_UNKNOWN_CA)
        testing_t_errorf_v(t, "Got error: %s; expected: %s", error_text(err),
                           burrow__tls_alert_text(TLS_ALERT_UNKNOWN_CA));

    tls_conn_free(srv);
    client_job_free(&job);
    net_pipe_free(c);
    tls_config_release(&server_config);
    tls_config_release(&client_config);
    arena_free(&ar);
}

/* Not in Go: go c.Close(). */
static void close_job(void *env) {
    NetConn *c = (NetConn *)env;
    (void)c->vt->closer.close(c->data);
}

static void TestClose(TestingT *t) {
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    NetConn c, s;
    net_pipe(a, &c, &s);
    SyncWaitGroup wg = {0};
    sync_wait_group_go(&wg, BURROW_FN(Func, close_job, &c));

    TlsConfig server_config = test_config_server(a);
    TlsConn *srv = tls_server(heap_allocator(), s, &server_config);
    if (srv == NULL)
        testing_t_fatal_v(t, "out of memory");
    Error err = tls_conn_handshake(srv);
    (void)s.vt->closer.close(s.data);
    sync_wait_group_wait(&wg);
    if (!errors_is(err, io_eof))
        testing_t_errorf_v(t, "Got error: %s; expected: %s", error_text(err),
                           error_text(io_eof));

    tls_conn_free(srv);
    net_pipe_free(c);
    tls_config_release(&server_config);
    arena_free(&ar);
}

#define TESTS(X)                                                                       \
    X(TestSimpleError)                                                                 \
    X(TestRejectBadProtocolVersion)                                                    \
    X(TestNoSuiteOverlap)                                                              \
    X(TestNoCompressionOverlap)                                                        \
    X(TestNoRC4ByDefault)                                                              \
    X(TestRejectSNIWithTrailingDot)                                                    \
    X(TestDontSelectECDSAWithRSAKey)                                                   \
    X(TestDontSelectRSAWithECDSAKey)                                                   \
    X(TestRenegotiationExtension)                                                      \
    X(TestTLS12OnlyCipherSuites)                                                       \
    X(TestTLSPointFormats)                                                             \
    X(TestAlertForwarding)                                                             \
    X(TestClose)

TESTING_MAIN(TESTS)
