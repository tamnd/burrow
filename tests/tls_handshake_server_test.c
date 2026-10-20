/* crypto/tls: handshake_server_test.go, the tests that need no recorded
 * handshakes, with testHandshake from handshake_test.go that many of them run
 * on. The tests that replay testdata come with it, and the rest of the tests
 * that need none come in later changes.
 *
 * Copyright 2009 The Go Authors. All rights reserved.
 * Copyright 2026 The burrow Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style licence that can be found
 * in the LICENSE file. */

#include "check.h"

#include "../src/crypto/rsa_internal.h"
#include "../src/crypto/tls_internal.h"
#include "burrow/burrow.h"
#include "burrow/crypto/tls.h"
#include "burrow/encoding/pem.h"
#include "burrow/mem/heap.h"
#include "burrow/strings.h"
#include "tls_handshake_test.h"
#include "tls_test_gen.h"

#include <string.h>

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
    if (conn == NULL) {
        testing_t_fatal_v(t, "out of memory");
        return;
    }
    TlsHandshakeMsg *ch = NULL;
    TlsECHServerContext *ech = NULL;
    Error err = burrow__tls_conn_read_client_hello(conn, ctx, a, &ch, &ech);
    if (conn->vers == TLS_VERSION_TLS13) {
        TlsServerHandshakeStateTLS13 hs;
        memset(&hs, 0, sizeof hs);
        hs.c = conn;
        hs.ctx = ctx;
        hs.a = a;
        if (!BURROW_FAILED(err)) {
            hs.client_hello_msg = ch;
            hs.client_hello = &ch->client_hello;
            hs.ech_context = ech;
            err = burrow__tls_server_handshake_state_tls13_process_client_hello(&hs);
        }
        if (!BURROW_FAILED(err))
            err = burrow__tls_server_handshake_state_tls13_check_for_resumption(&hs);
        if (!BURROW_FAILED(err))
            err = burrow__tls_server_handshake_state_tls13_pick_certificate(&hs);
    } else {
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
    }
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

static void TestVersion(TestingT *t) {
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    TlsConfig test_server = test_config_server(a);
    TlsConfig server_config;
    memset(&server_config, 0, sizeof server_config);
    server_config.certificates = test_server.certificates;
    server_config.max_version = TLS_VERSION_TLS13;
    TlsConfig client_config;
    memset(&client_config, 0, sizeof client_config);
    client_config.insecure_skip_verify = true;
    client_config.min_version = TLS_VERSION_TLS12;
    Handshake h;
    test_handshake(t, &client_config, &server_config, &h);
    if (BURROW_FAILED(h.err))
        testing_t_fatalf_v(t, "handshake failed: %s", error_text(h.err));
    if (h.server_state.version != TLS_VERSION_TLS13)
        testing_t_fatalf_v(t, "incorrect version %x, should be %x",
                           h.server_state.version, TLS_VERSION_TLS11);
    handshake_free(&h);

    client_config.min_version = 0;
    server_config.max_version = TLS_VERSION_TLS11;
    test_handshake(t, &client_config, &server_config, &h);
    if (!BURROW_FAILED(h.err))
        testing_t_fatalf_v(t, "expected failure to connect with TLS 1.0/1.1");
    handshake_free(&h);

    tls_config_release(&server_config);
    tls_config_release(&client_config);
    tls_config_release(&test_server);
    arena_free(&ar);
}

/* Not in Go: the GetConfigForClient of TestCipherSuitePreference. */
static TlsConfig *suite_preference_config(void *env, TlsClientHelloInfo *chi,
                                          Error *err) {
    (void)err;
    TestingT *t = (TestingT *)env;
    if (((const uint16_t *)chi->cipher_suites.p)[0] !=
        TLS_TLS_ECDHE_RSA_WITH_CHACHA20_POLY1305_SHA256)
        testing_t_error_v(t, "the advertised order should not depend on "
                             "Config.CipherSuites");
    if (chi->cipher_suites.len != 2 + 3)
        testing_t_error_v(t, "the advertised TLS 1.2 suites should be filtered by "
                             "Config.CipherSuites");
    return NULL;
}

static void TestCipherSuitePreference(TestingT *t) {
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    TlsConfig test_server = test_config_server(a);
    uint16_t server_suites[3] = {TLS_TLS_RSA_WITH_RC4_128_SHA,
                                 TLS_TLS_AES_128_GCM_SHA256,
                                 TLS_TLS_ECDHE_RSA_WITH_CHACHA20_POLY1305_SHA256};
    TlsConfig server_config;
    memset(&server_config, 0, sizeof server_config);
    server_config.cipher_suites = slice_from(server_suites, 3, 3, TYPE_UINT16);
    server_config.certificates = test_server.certificates;
    server_config.max_version = TLS_VERSION_TLS12;
    server_config.get_config_for_client =
        (TlsGetConfigForClientFunc){suite_preference_config, t};
    uint16_t client_suites[2] = {TLS_TLS_RSA_WITH_AES_128_CBC_SHA,
                                 TLS_TLS_ECDHE_RSA_WITH_CHACHA20_POLY1305_SHA256};
    TlsConfig client_config;
    memset(&client_config, 0, sizeof client_config);
    client_config.cipher_suites = slice_from(client_suites, 2, 2, TYPE_UINT16);
    client_config.insecure_skip_verify = true;
    Handshake h;
    test_handshake(t, &client_config, &server_config, &h);
    if (BURROW_FAILED(h.err))
        testing_t_fatalf_v(t, "handshake failed: %s", error_text(h.err));
    if (h.server_state.cipher_suite != TLS_TLS_ECDHE_RSA_WITH_CHACHA20_POLY1305_SHA256)
        testing_t_error_v(t, "the preference order should not depend on "
                             "Config.CipherSuites");
    handshake_free(&h);

    tls_config_release(&server_config);
    tls_config_release(&client_config);
    tls_config_release(&test_server);
    arena_free(&ar);
}

/* Not in Go: []byte(s) for a C string s. */
static Slice bytes_of(const char *s) {
    Int n = (Int)strlen(s);
    return slice_from((Byte *)(uintptr_t)s, n, n, TYPE_BYTE);
}

static void test_sct_handshake(TestingT *t, uint16_t version) {
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    Slice expected = slice_make(a, TYPE_BYTES, 2, 2);
    ((Slice *)expected.p)[0] = bytes_of("certificate");
    ((Slice *)expected.p)[1] = bytes_of("transparency");
    TlsCertificate cert =
        parse_test_cert(a, test_rsa2048_cert_pem, test_rsa2048_key_pem);
    cert.signed_certificate_timestamps = expected;
    TlsConfig server_config;
    memset(&server_config, 0, sizeof server_config);
    server_config.certificates = slice_from(&cert, 1, 1, TYPE_TLS_CERTIFICATE);
    server_config.max_version = version;
    TlsConfig client_config;
    memset(&client_config, 0, sizeof client_config);
    client_config.insecure_skip_verify = true;
    Handshake h;
    test_handshake(t, &client_config, &server_config, &h);
    if (BURROW_FAILED(h.err))
        testing_t_fatalf_v(t, "handshake failed: %s", error_text(h.err));
    Slice actual = h.client_state.signed_certificate_timestamps;
    if (actual.len != expected.len)
        testing_t_fatalf_v(t, "got %d scts, want %d", actual.len, expected.len);
    for (Int i = 0; i < expected.len; i++) {
        Slice sct = ((const Slice *)expected.p)[i];
        Slice got = ((const Slice *)actual.p)[i];
        if (!bytes_equal(sct, got))
            testing_t_fatalf_v(t, "SCT #%d was %x, but expected %x", i, got, sct);
    }
    handshake_free(&h);

    tls_config_release(&server_config);
    tls_config_release(&client_config);
    arena_free(&ar);
}

static void sct_handshake_tls12(void *env, TestingT *t) {
    (void)env;
    test_sct_handshake(t, TLS_VERSION_TLS12);
}

static void sct_handshake_tls13(void *env, TestingT *t) {
    (void)env;
    test_sct_handshake(t, TLS_VERSION_TLS13);
}

static void TestSCTHandshake(TestingT *t) {
    testing_t_run(t, BURROW_S("TLSv12"),
                  BURROW_FN(TestingTFunc, sct_handshake_tls12, NULL));
    testing_t_run(t, BURROW_S("TLSv13"),
                  BURROW_FN(TestingTFunc, sct_handshake_tls13, NULL));
}

static void test_cross_version_resume(TestingT *t, uint16_t version) {
    (void)version;
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    uint16_t suites[1] = {TLS_TLS_ECDHE_RSA_WITH_AES_128_GCM_SHA256};
    TlsCertificate cert =
        parse_test_cert(a, test_rsa2048_cert_pem, test_rsa2048_key_pem);
    TlsConfig server_config;
    memset(&server_config, 0, sizeof server_config);
    server_config.cipher_suites = slice_from(suites, 1, 1, TYPE_UINT16);
    server_config.certificates = slice_from(&cert, 1, 1, TYPE_TLS_CERTIFICATE);
    server_config.time = (TlsTimeFunc){test_time, NULL};
    TlsConfig client_config;
    memset(&client_config, 0, sizeof client_config);
    client_config.cipher_suites = slice_from(suites, 1, 1, TYPE_UINT16);
    client_config.insecure_skip_verify = true;
    client_config.client_session_cache =
        tls_new_lru_client_session_cache(heap_allocator(), 1);
    if (client_config.client_session_cache.vt == NULL)
        testing_t_fatal_v(t, "out of memory");
    client_config.server_name = BURROW_S("servername");
    client_config.min_version = TLS_VERSION_TLS12;
    client_config.time = (TlsTimeFunc){test_time, NULL};

    /* Establish a session at TLS 1.3. */
    client_config.max_version = TLS_VERSION_TLS13;
    Handshake h;
    test_handshake(t, &client_config, &server_config, &h);
    if (BURROW_FAILED(h.err))
        testing_t_fatalf_v(t, "handshake failed: %s", error_text(h.err));
    handshake_free(&h);

    /* The client session cache now contains a TLS 1.3 session. */
    test_handshake(t, &client_config, &server_config, &h);
    if (BURROW_FAILED(h.err))
        testing_t_fatalf_v(t, "handshake failed: %s", error_text(h.err));
    if (!h.server_state.did_resume)
        testing_t_fatalf_v(t, "handshake did not resume at the same version");
    handshake_free(&h);

    /* Test that the server will decline to resume at a lower version. */
    client_config.max_version = TLS_VERSION_TLS12;
    test_handshake(t, &client_config, &server_config, &h);
    if (BURROW_FAILED(h.err))
        testing_t_fatalf_v(t, "handshake failed: %s", error_text(h.err));
    if (h.server_state.did_resume)
        testing_t_fatalf_v(t, "handshake resumed at a lower version");
    handshake_free(&h);

    /* The client session cache now contains a TLS 1.2 session. */
    test_handshake(t, &client_config, &server_config, &h);
    if (BURROW_FAILED(h.err))
        testing_t_fatalf_v(t, "handshake failed: %s", error_text(h.err));
    if (!h.server_state.did_resume)
        testing_t_fatalf_v(t, "handshake did not resume at the same version");
    handshake_free(&h);

    /* Test that the server will decline to resume at a higher version. */
    client_config.max_version = TLS_VERSION_TLS13;
    test_handshake(t, &client_config, &server_config, &h);
    if (BURROW_FAILED(h.err))
        testing_t_fatalf_v(t, "handshake failed: %s", error_text(h.err));
    if (h.server_state.did_resume)
        testing_t_fatalf_v(t, "handshake resumed at a higher version");
    handshake_free(&h);

    tls_lru_client_session_cache_free(client_config.client_session_cache);
    tls_config_release(&server_config);
    tls_config_release(&client_config);
    arena_free(&ar);
}

static void cross_version_resume_tls12(void *env, TestingT *t) {
    (void)env;
    test_cross_version_resume(t, TLS_VERSION_TLS12);
}

static void cross_version_resume_tls13(void *env, TestingT *t) {
    (void)env;
    test_cross_version_resume(t, TLS_VERSION_TLS13);
}

static void TestCrossVersionResume(TestingT *t) {
    testing_t_run(t, BURROW_S("TLSv12"),
                  BURROW_FN(TestingTFunc, cross_version_resume_tls12, NULL));
    testing_t_run(t, BURROW_S("TLSv13"),
                  BURROW_FN(TestingTFunc, cross_version_resume_tls13, NULL));
}

/* Not in Go: the GetCertificate of TestHandshakeServerGetCertificateExtensions,
 * with what it checks in env. */
typedef struct CertificateExtensions {
    TestingT *t;
    Slice expected_extensions;
    SyncAtomicInt32 *called;
} CertificateExtensions;

static const char get_certificate_extensions_err[] =
    "TestHandshakeServerGetCertificateExtensions error";

static TlsCertificate *get_certificate_extensions(void *env, TlsClientHelloInfo *chi,
                                                  Error *err) {
    CertificateExtensions *e = (CertificateExtensions *)env;
    if (!slices_equal(e->expected_extensions, chi->extensions))
        testing_t_errorf_v(e->t,
                           "expected extensions on ClientHelloInfo (%v) to match "
                           "clientHelloMsg (%v)",
                           e->expected_extensions, chi->extensions);
    (void)sync_atomic_int32_add(e->called, 1);

    BURROW_OUT(err, errors_new(error_allocator(),
                               str_from_cstr(get_certificate_extensions_err)));
    return NULL;
}

/* Not in Go: a subtest of TestHandshakeServerGetCertificateExtensions, for the
 * version in env. */
typedef struct GetCertificateExtensionsCase {
    uint16_t vers;
    SyncAtomicInt32 *called;
} GetCertificateExtensionsCase;

static void get_certificate_extensions_case(void *env, TestingT *t) {
    const GetCertificateExtensionsCase *tc = (const GetCertificateExtensionsCase *)env;
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    Error err = BURROW_NO_ERROR;
    EcdhPrivateKey *pk =
        ecdh_curve_generate_key(ecdh_p256(), a, crypto_rand_reader, &err);
    if (pk == NULL)
        testing_t_fatal_v(t, "%v", err);
    static uint16_t suites[1] = {TLS_TLS_AES_128_GCM_SHA256};
    static Byte compression[1] = {TLS_COMPRESSION_NONE};
    static uint16_t schemes[1] = {TLS_ECDSA_WITH_P256_AND_SHA256};
    static uint16_t versions[1] = {TLS_VERSION_TLS13};
    TlsKeyShare ks = {TLS_CURVE_P256,
                      ecdh_public_key_bytes(ecdh_private_key_public_key(pk), a)};
    TlsHandshakeMsg m = client_hello(a, tc->vers);
    m.client_hello.cipher_suites = slice_from(suites, 1, 1, TYPE_UINT16);
    m.client_hello.compression_methods = slice_from(compression, 1, 1, TYPE_BYTE);
    m.client_hello.server_name = BURROW_S("test");
    m.client_hello.key_shares = slice_from(&ks, 1, 1, &burrow__tls_key_share_type);
    m.client_hello.supported_curves = slice_from(curves_p256, 1, 1, TYPE_UINT16);
    m.client_hello.supported_signature_algorithms =
        slice_from(schemes, 1, 1, TYPE_UINT16);

    /* the clientHelloMsg initialized just above is serialized with two
     * extensions: server_name(0) and application_layer_protocol_negotiation(16) */
    uint16_t extensions[5] = {TLS_EXTENSION_SERVER_NAME, TLS_EXTENSION_SUPPORTED_CURVES,
                              TLS_EXTENSION_SIGNATURE_ALGORITHMS,
                              TLS_EXTENSION_KEY_SHARE};
    Slice expected_extensions = slice_from(extensions, 4, 5, TYPE_UINT16);

    if (tc->vers == TLS_VERSION_TLS13) {
        m.client_hello.supported_versions = slice_from(versions, 1, 1, TYPE_UINT16);
        extensions[4] = TLS_EXTENSION_SUPPORTED_VERSIONS;
        expected_extensions.len = 5;
    }

    /* Go's TLS client presents extensions in the ClientHello sorted by extension
     * ID */
    slices_sort(expected_extensions);

    TlsConfig server_config = test_config_server(a);
    CertificateExtensions env2 = {t, expected_extensions, tc->called};
    server_config.get_certificate =
        (TlsGetCertificateFunc){get_certificate_extensions, &env2};
    test_client_hello_failure(t, &server_config, &m, get_certificate_extensions_err);
    tls_config_release(&server_config);
    arena_free(&ar);
}

static void TestHandshakeServerGetCertificateExtensions(TestingT *t) {
    /* ensure the test condition inside our GetCertificate callback is actually
     * invoked */
    SyncAtomicInt32 called = {0};

    static const uint16_t test_versions[2] = {TLS_VERSION_TLS12, TLS_VERSION_TLS13};
    for (int i = 0; i < 2; i++) {
        GetCertificateExtensionsCase tc = {test_versions[i], &called};
        Str name =
            fmt_sprintf_v(error_allocator(), "TLS version %04x", test_versions[i]);
        testing_t_run(t, name,
                      BURROW_FN(TestingTFunc, get_certificate_extensions_case, &tc));
    }

    if (sync_atomic_int32_load(&called) != 2)
        testing_t_error_v(t, "expected our GetCertificate test to be called twice");
}

/* Not in Go: the GetCertificate of TestHandshakeServerSNIGetCertificateError
 * and TestHandshakeServerEmptyCertificates, which fails with the message in
 * env. */
static TlsCertificate *get_certificate_error(void *env, TlsClientHelloInfo *chi,
                                             Error *err) {
    (void)chi;
    BURROW_OUT(err, errors_new(error_allocator(), str_from_cstr((const char *)env)));
    return NULL;
}

/* TestHandshakeServerSNIGetCertificateError tests to make sure that errors in
 * GetCertificate result in a tls alert. */
static void TestHandshakeServerSNIGetCertificateError(TestingT *t) {
    static char err_msg[] = "TestHandshakeServerSNIGetCertificateError error";

    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    TlsConfig server_config = test_config_server(a);
    server_config.get_certificate =
        (TlsGetCertificateFunc){get_certificate_error, err_msg};

    static uint16_t suites[1] = {TLS_TLS_ECDHE_RSA_WITH_AES_128_GCM_SHA256};
    static Byte compression[1] = {TLS_COMPRESSION_NONE};
    TlsHandshakeMsg m = client_hello(a, TLS_VERSION_TLS12);
    m.client_hello.cipher_suites = slice_from(suites, 1, 1, TYPE_UINT16);
    m.client_hello.compression_methods = slice_from(compression, 1, 1, TYPE_BYTE);
    m.client_hello.server_name = BURROW_S("test");
    test_client_hello_failure(t, &server_config, &m, err_msg);
    tls_config_release(&server_config);
    arena_free(&ar);
}

/* TestHandshakeServerEmptyCertificates tests that GetCertificates is called in
 * the case that Certificates is empty, even without SNI. */
static void TestHandshakeServerEmptyCertificates(TestingT *t) {
    static char err_msg[] = "TestHandshakeServerEmptyCertificates error";

    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    TlsConfig server_config = test_config_server(a);
    server_config.get_certificate =
        (TlsGetCertificateFunc){get_certificate_error, err_msg};
    server_config.certificates = slice_nil(TYPE_TLS_CERTIFICATE);

    static uint16_t suites[1] = {TLS_TLS_ECDHE_RSA_WITH_AES_128_GCM_SHA256};
    static Byte compression[1] = {TLS_COMPRESSION_NONE};
    TlsHandshakeMsg m = client_hello(a, TLS_VERSION_TLS12);
    m.client_hello.cipher_suites = slice_from(suites, 1, 1, TYPE_UINT16);
    m.client_hello.compression_methods = slice_from(compression, 1, 1, TYPE_BYTE);
    test_client_hello_failure(t, &server_config, &m, err_msg);

    /* With an empty Certificates and a nil GetCertificate, the server should
     * always return a “no certificates” error. */
    server_config.get_certificate = (TlsGetCertificateFunc){NULL, NULL};

    m = client_hello(a, TLS_VERSION_TLS12);
    m.client_hello.cipher_suites = slice_from(suites, 1, 1, TYPE_UINT16);
    m.client_hello.compression_methods = slice_from(compression, 1, 1, TYPE_BYTE);
    test_client_hello_failure(t, &server_config, &m, "no certificates");
    tls_config_release(&server_config);
    arena_free(&ar);
}

static void TestSNIGivenOnFailure(TestingT *t) {
    static const char expected_server_name[] = "test.testing";

    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    static uint16_t suites[1] = {TLS_TLS_ECDHE_RSA_WITH_AES_128_GCM_SHA256};
    TlsHandshakeMsg m = client_hello(a, TLS_VERSION_TLS12);
    m.client_hello.cipher_suites = slice_from(suites, 1, 1, TYPE_UINT16);
    m.client_hello.compression_methods = slice_from(compression_none, 1, 1, TYPE_BYTE);
    m.client_hello.server_name = str_from_cstr(expected_server_name);

    TlsConfig server_config = test_config_server(a);
    /* Erase the server's cipher suites to ensure the handshake fails. */
    server_config.cipher_suites = slice_nil(TYPE_UINT16);

    NetConn c, s;
    local_pipe(a, &c, &s);
    TlsConfig client_config = test_config_client(a);
    ClientJob job;
    client_job_init(&job, c, &client_config, &m, REPLY_NONE);
    SyncWaitGroup wg = {0};
    sync_wait_group_go(&wg, BURROW_FN(Func, client_job, &job));
    TlsConn *conn = tls_server(heap_allocator(), s, &server_config);
    if (conn == NULL) {
        testing_t_fatal_v(t, "out of memory");
        return;
    }
    Context ctx = context_background();
    TlsHandshakeMsg *ch = NULL;
    TlsECHServerContext *ech = NULL;
    Error err = burrow__tls_conn_read_client_hello(conn, ctx, a, &ch, &ech);
    (void)ech;
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

    if (!BURROW_FAILED(err))
        testing_t_error_v(t, "No error reported from server");

    TlsConnectionState cs = tls_conn_connection_state(hs.c);
    if (cs.handshake_complete)
        testing_t_error_v(t, "Handshake registered as complete");

    if (!str_eq(cs.server_name, str_from_cstr(expected_server_name)))
        testing_t_errorf_v(t, "Expected ServerName of %q, but got %q",
                           str_from_cstr(expected_server_name), cs.server_name);

    (void)s.vt->closer.close(s.data);
    sync_wait_group_wait(&wg);
    if (BURROW_FAILED(job.write_err))
        testing_t_errorf_v(t, "%v", job.write_err);
    tls_conn_free(conn);
    client_job_free(&job);
    local_pipe_free(c);
    tls_config_release(&client_config);
    tls_config_release(&server_config);
    arena_free(&ar);
}

/* Not in Go: one of getConfigForClientTests. The Config the callback returns
 * is made in a, which the test keeps until the Conn that used it is gone. */
typedef struct GetConfigForClientTest {
    void (*setup)(TlsConfig *config);
    TlsConfig *(*callback)(Alloc *a, TlsClientHelloInfo *client_hello, Error *err);
    const char *error_substring;
    Error (*verify)(TlsConfig *config);
} GetConfigForClientTest;

static TlsConfig *config_for_client_nil(Alloc *a, TlsClientHelloInfo *client_hello,
                                        Error *err) {
    (void)a;
    (void)client_hello;
    (void)err;
    return NULL;
}

static TlsConfig *config_for_client_error(Alloc *a, TlsClientHelloInfo *client_hello,
                                          Error *err) {
    (void)a;
    (void)client_hello;
    BURROW_OUT(err, errors_new(error_allocator(), BURROW_S("should bubble up")));
    return NULL;
}

static TlsConfig *config_for_client_tls11(Alloc *a, TlsClientHelloInfo *client_hello,
                                          Error *err) {
    (void)client_hello;
    TlsConfig *config = BURROW_NEW(a, TlsConfig);
    if (config == NULL) {
        BURROW_OUT(err, burrow_err_out_of_memory);
        return NULL;
    }
    *config = test_config_server(a);
    /* Setting a maximum version of TLS 1.1 should cause the handshake to fail,
     * as the client MinVersion is TLS 1.2. */
    config->max_version = TLS_VERSION_TLS11;
    return config;
}

static void config_for_client_set_key(TlsConfig *config) {
    for (int i = 0; i < 32; i++)
        config->session_ticket_key[i] = (Byte)i;
    burrow__tls_ticket_keys_release(config->session_ticket_keys);
    config->session_ticket_keys = NULL;
}

static TlsConfig *
config_for_client_clear_key(Alloc *a, TlsClientHelloInfo *client_hello, Error *err) {
    (void)client_hello;
    TlsConfig *config = BURROW_NEW(a, TlsConfig);
    if (config == NULL) {
        BURROW_OUT(err, burrow_err_out_of_memory);
        return NULL;
    }
    *config = test_config_server(a);
    memset(config->session_ticket_key, 0, sizeof config->session_ticket_key);
    config->session_ticket_keys = NULL;
    return config;
}

static Error config_for_client_verify_key(TlsConfig *config) {
    static const Byte zero[32];
    if (memcmp(config->session_ticket_key, zero, 32) == 0)
        return fmt_errorf_v("expected SessionTicketKey to be set");
    return BURROW_NO_ERROR;
}

static void config_for_client_set_keys(TlsConfig *config) {
    Byte dummy_key[1][32];
    for (int i = 0; i < 32; i++)
        dummy_key[0][i] = (Byte)i;

    (void)tls_config_set_session_ticket_keys(config, (const Byte(*)[32])dummy_key, 1);
}

static TlsConfig *config_for_client_no_keys(Alloc *a, TlsClientHelloInfo *client_hello,
                                            Error *err) {
    (void)client_hello;
    TlsConfig *config = BURROW_NEW(a, TlsConfig);
    if (config == NULL) {
        BURROW_OUT(err, burrow_err_out_of_memory);
        return NULL;
    }
    *config = test_config_server(a);
    config->session_ticket_keys = NULL;
    return config;
}

static const GetConfigForClientTest get_config_for_client_tests[] = {
    {NULL, config_for_client_nil, "", NULL},
    {NULL, config_for_client_error, "should bubble up", NULL},
    {NULL, config_for_client_tls11, "client offered only unsupported versions", NULL},
    {config_for_client_set_key, config_for_client_clear_key, "",
     config_for_client_verify_key},
    {config_for_client_set_keys, config_for_client_no_keys, "",
     config_for_client_verify_key},
};

/* Not in Go: what the GetConfigForClient of TestGetConfigForClient closes
 * over. */
typedef struct ConfigForClientEnv {
    const GetConfigForClientTest *test;
    Alloc *a;
    TlsConfig *config_returned;
} ConfigForClientEnv;

static TlsConfig *config_for_client(void *env, TlsClientHelloInfo *client_hello,
                                    Error *err) {
    ConfigForClientEnv *e = (ConfigForClientEnv *)env;
    TlsConfig *config = e->test->callback(e->a, client_hello, err);
    e->config_returned = config;
    return config;
}

/* Not in Go: the goroutine of TestGetConfigForClient. */
typedef struct ServerJob {
    NetConn s;
    TlsConfig *config;
    TlsConn *server;
    Error err;
} ServerJob;

static void server_job(void *env) {
    ServerJob *j = (ServerJob *)env;
    j->server = tls_server(heap_allocator(), j->s, j->config);
    if (j->server == NULL)
        j->err = burrow_err_out_of_memory;
    else
        j->err = tls_conn_handshake(j->server);
    (void)j->s.vt->closer.close(j->s.data);
}

static void TestGetConfigForClient(TestingT *t) {
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    TlsConfig server_config = test_config_server(a);
    TlsConfig client_config = test_config_client(a);
    client_config.min_version = TLS_VERSION_TLS12;

    Int n = (Int)(sizeof get_config_for_client_tests /
                  sizeof get_config_for_client_tests[0]);
    for (Int i = 0; i < n; i++) {
        const GetConfigForClientTest *test = &get_config_for_client_tests[i];
        if (test->setup != NULL)
            test->setup(&server_config);

        Arena returned;
        arena_init(&returned, NULL, 0);
        ConfigForClientEnv env = {test, arena_allocator(&returned), NULL};
        server_config.get_config_for_client =
            (TlsGetConfigForClientFunc){config_for_client, &env};
        NetConn c, s;
        local_pipe(a, &c, &s);
        ServerJob job = {s, &server_config, NULL, BURROW_NO_ERROR};
        SyncWaitGroup done = {0};
        sync_wait_group_go(&done, BURROW_FN(Func, server_job, &job));

        TlsConn *client = tls_client(heap_allocator(), c, &client_config);
        if (client == NULL)
            testing_t_fatal_v(t, "out of memory");
        Error client_err = tls_conn_handshake(client);
        (void)c.vt->closer.close(c.data);

        sync_wait_group_wait(&done);
        Error server_err = job.err;

        if (test->error_substring[0] == '\0') {
            if (BURROW_FAILED(server_err) || BURROW_FAILED(client_err))
                testing_t_errorf_v(t,
                                   "test[%d]: expected no error but got serverErr: "
                                   "%q, clientErr: %q",
                                   i, server_err, client_err);
            if (test->verify != NULL) {
                Error err = test->verify(env.config_returned);
                if (BURROW_FAILED(err))
                    testing_t_errorf_v(t, "test[%d]: verify returned error: %v", i,
                                       err);
            }
        } else {
            if (!BURROW_FAILED(server_err))
                testing_t_errorf_v(t,
                                   "test[%d]: expected error containing %q but got "
                                   "no error",
                                   i, str_from_cstr(test->error_substring));
            else if (!strings_contains(error_text(server_err),
                                       str_from_cstr(test->error_substring)))
                testing_t_errorf_v(t,
                                   "test[%d]: expected error to contain %q but it "
                                   "was %q",
                                   i, str_from_cstr(test->error_substring), server_err);
        }

        tls_conn_free(client);
        tls_conn_free(job.server);
        local_pipe_free(c);
        if (env.config_returned != NULL)
            tls_config_release(env.config_returned);
        arena_free(&returned);
    }
    tls_config_release(&client_config);
    tls_config_release(&server_config);
    arena_free(&ar);
}

/* Not in Go: the goroutine of TestCloseServerConnectionOnIdleClient. */
typedef struct IdleClient {
    NetConn client_conn;
    TlsConn *server;
} IdleClient;

static void idle_client(void *env) {
    IdleClient *j = (IdleClient *)env;
    Byte zero = '0';
    Error err;
    (void)j->client_conn.vt->writer.write(j->client_conn.data,
                                          slice_from(&zero, 1, 1, TYPE_BYTE), &err);
    (void)tls_conn_close(j->server);
}

static void TestCloseServerConnectionOnIdleClient(TestingT *t) {
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    NetConn client_conn, server_conn;
    local_pipe(a, &client_conn, &server_conn);
    TlsConfig config = test_config_server(a);
    TlsConn *server = tls_server(heap_allocator(), server_conn, &config);
    if (server == NULL)
        testing_t_fatal_v(t, "out of memory");
    IdleClient job = {client_conn, server};
    SyncWaitGroup wg = {0};
    sync_wait_group_go(&wg, BURROW_FN(Func, idle_client, &job));
    (void)tls_conn_set_read_deadline(server, time_add(time_now(), TIME_MINUTE));
    Error err = tls_conn_handshake(server);
    if (BURROW_FAILED(err)) {
        if (net_is_error(err) && net_error_timeout(err))
            testing_t_errorf_v(
                t, "Expected a closed network connection error but got '%s'",
                error_text(err));
    } else {
        testing_t_errorf_v(t, "Error expected, but no error returned");
    }

    sync_wait_group_wait(&wg);
    tls_conn_free(server);
    local_pipe_free(client_conn);
    tls_config_release(&config);
    arena_free(&ar);
}

static void expect_error(TestingT *t, Error err, const char *sub) {
    if (!BURROW_FAILED(err))
        testing_t_errorf_v(t, "expected error %q, got nil", str_from_cstr(sub));
    else if (!strings_contains(error_text(err), str_from_cstr(sub)))
        testing_t_errorf_v(t, "expected error %q, got %q", str_from_cstr(sub),
                           error_text(err));
}

static void TestCloneHash(TestingT *t) {
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    Hash h1 = crypto_hash_new(CRYPTO_SHA256, a);
    if (h1.vt == NULL) {
        arena_free(&ar);
        testing_t_fatal_v(t, "out of memory");
    }
    static Byte test[] = "test";
    hash_write(h1, slice_from(test, 4, 4, TYPE_BYTE), NULL);
    Slice s1 = hash_sum(a, h1, slice_nil(TYPE_BYTE));
    Hash h2 = burrow__tls_clone_hash(a, h1, CRYPTO_SHA256);
    /* Not in Go: a nil h2 would panic there. */
    if (h2.vt == NULL) {
        arena_free(&ar);
        testing_t_fatal_v(t, "cloneHash returned nil");
    }
    Slice s2 = hash_sum(a, h2, slice_nil(TYPE_BYTE));
    if (!bytes_equal(s1, s2))
        testing_t_error_v(t, "cloned hash generated a different sum");
    arena_free(&ar);
}

/* Not in Go: the goroutine of TestKeyTooSmallForRSAPSS. */
typedef struct KeyTooSmallServer {
    TestingT *t;
    NetConn conn;
    TlsConfig *config;
} KeyTooSmallServer;

static void key_too_small_server(void *env) {
    KeyTooSmallServer *j = (KeyTooSmallServer *)env;
    TlsConn *server = tls_server(heap_allocator(), j->conn, j->config);
    if (server == NULL) {
        testing_t_error_v(j->t, "out of memory");
        (void)j->conn.vt->closer.close(j->conn.data);
        return;
    }
    Error err = tls_conn_handshake(server);
    expect_error(j->t, err, "key size too small");
    (void)j->conn.vt->closer.close(j->conn.data);
    tls_conn_free(server);
}

static void TestKeyTooSmallForRSAPSS(TestingT *t) {
    burrow__rsa_godebug_set("rsa1024min=0");
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    NetConn client_conn, server_conn;
    net_pipe(a, &client_conn, &server_conn);
    TlsConfig client_config = test_config_client(a);
    TlsConn *client = tls_client(heap_allocator(), client_conn, &client_config);
    if (client == NULL)
        testing_t_fatal_v(t, "out of memory");
    TlsConfig config = test_config_server(a);
    TlsCertificate rsa512 =
        parse_test_cert(a, test_rsa512_cert_pem, test_rsa512_key_pem);
    config.certificates = slice_from(&rsa512, 1, 1, TYPE_TLS_CERTIFICATE);
    config.min_version = TLS_VERSION_TLS13;
    KeyTooSmallServer job = {t, server_conn, &config};
    SyncWaitGroup done = {0};
    sync_wait_group_go(&done, BURROW_FN(Func, key_too_small_server, &job));
    Error err = tls_conn_handshake(client);
    expect_error(t, err, "handshake failure");
    sync_wait_group_wait(&done);

    tls_conn_free(client);
    net_pipe_free(client_conn);
    tls_config_release(&config);
    tls_config_release(&client_config);
    arena_free(&ar);
    burrow__rsa_godebug_set(NULL);
}

static void TestMultipleCertificates(TestingT *t) {
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    uint16_t suites[1] = {TLS_TLS_ECDHE_RSA_WITH_AES_128_GCM_SHA256};
    TlsConfig client_config = test_config_client(a);
    client_config.cipher_suites = slice_from(suites, 1, 1, TYPE_UINT16);
    client_config.max_version = TLS_VERSION_TLS12;

    TlsConfig server_config = test_config_server(a);
    Slice certs = slice_make(a, TYPE_TLS_CERTIFICATE, 2, 2);
    ((TlsCertificate *)certs.p)[0] =
        parse_test_cert(a, test_ecdsap256_cert_pem, test_ecdsap256_key_pem);
    ((TlsCertificate *)certs.p)[1] =
        parse_test_cert(a, test_rsa2048_cert_pem, test_rsa2048_key_pem);
    server_config.certificates = certs;

    Handshake h;
    test_handshake(t, &client_config, &server_config, &h);
    if (BURROW_FAILED(h.err))
        testing_t_fatal_v(t, "%v", h.err);
    X509PublicKeyAlgorithm got =
        (*(X509Certificate *const *)h.client_state.peer_certificates.p)
            ->public_key_algorithm;
    if (got != X509_RSA)
        testing_t_errorf_v(t, "expected RSA certificate, got %v",
                           x509_public_key_algorithm_string(got, a));
    handshake_free(&h);

    tls_config_release(&server_config);
    tls_config_release(&client_config);
    arena_free(&ar);
}

/* Not in Go: one of the cases of TestAESCipherReordering. The lists of
 * ciphers end at the first zero. */
typedef struct AESCipherReorderingTest {
    const char *name;
    uint16_t client_ciphers[5];
    bool server_has_aesgcm;
    uint16_t server_ciphers[3];
    uint16_t expected_cipher;
} AESCipherReorderingTest;

/* Not in Go: the ciphers in p, up to the first zero, and nil for none. */
static Slice ciphers_of(const uint16_t *p) {
    Int n = 0;
    while (p[n] != 0)
        n++;
    if (n == 0)
        return slice_nil(TYPE_UINT16);
    return slice_from((uint16_t *)(uintptr_t)p, n, n, TYPE_UINT16);
}

static const AESCipherReorderingTest aes_cipher_reordering_tests[] = {
    {"server has hardware AES, client doesn't (pick ChaCha)",
     {TLS_TLS_ECDHE_RSA_WITH_CHACHA20_POLY1305_SHA256,
      TLS_TLS_ECDHE_RSA_WITH_AES_128_GCM_SHA256, TLS_TLS_RSA_WITH_AES_128_CBC_SHA},
     true,
     {0},
     TLS_TLS_ECDHE_RSA_WITH_CHACHA20_POLY1305_SHA256},
    {"client prefers AES-GCM, server doesn't have hardware AES (pick ChaCha)",
     {TLS_TLS_ECDHE_RSA_WITH_AES_128_GCM_SHA256,
      TLS_TLS_ECDHE_RSA_WITH_CHACHA20_POLY1305_SHA256,
      TLS_TLS_RSA_WITH_AES_128_CBC_SHA},
     false,
     {0},
     TLS_TLS_ECDHE_RSA_WITH_CHACHA20_POLY1305_SHA256},
    {"client prefers AES-GCM, server has hardware AES (pick AES-GCM)",
     {TLS_TLS_ECDHE_RSA_WITH_AES_128_GCM_SHA256,
      TLS_TLS_ECDHE_RSA_WITH_CHACHA20_POLY1305_SHA256,
      TLS_TLS_RSA_WITH_AES_128_CBC_SHA},
     true,
     {0},
     TLS_TLS_ECDHE_RSA_WITH_AES_128_GCM_SHA256},
    {"client prefers AES-GCM and sends GREASE, server has hardware AES (pick AES-GCM)",
     {0x0A0A, /* GREASE value */
      TLS_TLS_ECDHE_RSA_WITH_AES_128_GCM_SHA256,
      TLS_TLS_ECDHE_RSA_WITH_CHACHA20_POLY1305_SHA256,
      TLS_TLS_RSA_WITH_AES_128_CBC_SHA},
     true,
     {0},
     TLS_TLS_ECDHE_RSA_WITH_AES_128_GCM_SHA256},
    {"client prefers AES-GCM and doesn't support ChaCha, server doesn't have hardware "
     "AES (pick AES-GCM)",
     {TLS_TLS_ECDHE_RSA_WITH_AES_128_GCM_SHA256,
      TLS_TLS_ECDHE_ECDSA_WITH_AES_128_GCM_SHA256, TLS_TLS_RSA_WITH_AES_128_CBC_SHA},
     false,
     {0},
     TLS_TLS_ECDHE_RSA_WITH_AES_128_GCM_SHA256},
    {"client prefers AES-GCM and AES-CBC over ChaCha, server doesn't have hardware AES "
     "(pick ChaCha)",
     {TLS_TLS_ECDHE_RSA_WITH_AES_128_GCM_SHA256, TLS_TLS_RSA_WITH_AES_128_CBC_SHA,
      TLS_TLS_ECDHE_RSA_WITH_CHACHA20_POLY1305_SHA256},
     false,
     {0},
     TLS_TLS_ECDHE_RSA_WITH_CHACHA20_POLY1305_SHA256},
    {"client prefers AES-GCM over ChaCha and sends GREASE, server doesn't have "
     "hardware AES (pick ChaCha)",
     {0x0A0A, /* GREASE value */
      TLS_TLS_ECDHE_RSA_WITH_AES_128_GCM_SHA256,
      TLS_TLS_ECDHE_RSA_WITH_CHACHA20_POLY1305_SHA256,
      TLS_TLS_RSA_WITH_AES_128_CBC_SHA},
     false,
     {0},
     TLS_TLS_ECDHE_RSA_WITH_CHACHA20_POLY1305_SHA256},
    {"client supports multiple AES-GCM, server doesn't have hardware AES and doesn't "
     "support ChaCha (AES-GCM)",
     {TLS_TLS_ECDHE_RSA_WITH_AES_256_GCM_SHA384,
      TLS_TLS_ECDHE_RSA_WITH_CHACHA20_POLY1305_SHA256,
      TLS_TLS_ECDHE_RSA_WITH_AES_128_GCM_SHA256},
     false,
     {TLS_TLS_ECDHE_RSA_WITH_AES_256_GCM_SHA384,
      TLS_TLS_ECDHE_RSA_WITH_AES_128_GCM_SHA256},
     TLS_TLS_ECDHE_RSA_WITH_AES_128_GCM_SHA256},
    {"client prefers AES-GCM, server has hardware but doesn't support AES (pick "
     "ChaCha)",
     {TLS_TLS_ECDHE_RSA_WITH_AES_128_GCM_SHA256,
      TLS_TLS_ECDHE_RSA_WITH_CHACHA20_POLY1305_SHA256,
      TLS_TLS_RSA_WITH_AES_128_CBC_SHA},
     true,
     {TLS_TLS_ECDHE_RSA_WITH_CHACHA20_POLY1305_SHA256},
     TLS_TLS_ECDHE_RSA_WITH_CHACHA20_POLY1305_SHA256},
};

static void aes_cipher_reordering(void *env, TestingT *t) {
    const AESCipherReorderingTest *tc = (const AESCipherReorderingTest *)env;
    burrow__tls_aes_gcm_hardware_override = tc->server_has_aesgcm ? 1 : 0;
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    TlsConfig config;
    memset(&config, 0, sizeof config);
    config.cipher_suites = ciphers_of(tc->server_ciphers);
    NetConn c, s;
    local_pipe(a, &c, &s);
    TlsConn *conn = tls_server(heap_allocator(), s, &config);
    if (conn == NULL) {
        testing_t_fatal_v(t, "out of memory");
        return;
    }
    conn->vers = TLS_VERSION_TLS12;
    TlsHandshakeMsg ch;
    burrow__tls_handshake_msg_init(&ch, TLS_MSG_CLIENT_HELLO);
    ch.client_hello.cipher_suites = ciphers_of(tc->client_ciphers);
    ch.client_hello.vers = TLS_VERSION_TLS12;
    TlsServerHandshakeState hs;
    memset(&hs, 0, sizeof hs);
    hs.c = conn;
    hs.ctx = context_background();
    hs.client_hello_msg = &ch;
    hs.client_hello = &ch.client_hello;
    hs.ecdhe_ok = true;
    hs.rsa_sign_ok = true;
    hs.rsa_decrypt_ok = true;
    hs.master_secret = slice_nil(TYPE_BYTE);
    hs.a = a;

    Error err = burrow__tls_server_handshake_state_pick_cipher_suite(&hs);
    if (BURROW_FAILED(err))
        testing_t_errorf_v(t, "pickCipherSuite failed: %s", error_text(err));

    uint16_t got = hs.suite == NULL ? 0 : hs.suite->id;
    if (tc->expected_cipher != got)
        testing_t_errorf_v(t, "unexpected cipher chosen: want %d, got %d",
                           tc->expected_cipher, got);

    tls_conn_free(conn);
    local_pipe_free(c);
    tls_config_release(&config);
    arena_free(&ar);
}

static void TestAESCipherReordering(TestingT *t) {
    Int n = (Int)(sizeof aes_cipher_reordering_tests /
                  sizeof aes_cipher_reordering_tests[0]);
    for (Int i = 0; i < n; i++) {
        const AESCipherReorderingTest *tc = &aes_cipher_reordering_tests[i];
        testing_t_run(
            t, str_from_cstr(tc->name),
            BURROW_FN(TestingTFunc, aes_cipher_reordering, (void *)(uintptr_t)tc));
    }
    burrow__tls_aes_gcm_hardware_override = -1;
}

/* Not in Go: one of the cases of TestAESCipherReorderingTLS13. The list of
 * ciphers ends at the first zero. */
typedef struct AESCipherReorderingTLS13Test {
    const char *name;
    uint16_t client_ciphers[4];
    bool server_has_aesgcm;
    uint16_t expected_cipher;
} AESCipherReorderingTLS13Test;

static const AESCipherReorderingTLS13Test aes_cipher_reordering_tls13_tests[] = {
    {"server has hardware AES, client doesn't (pick ChaCha)",
     {TLS_TLS_CHACHA20_POLY1305_SHA256, TLS_TLS_AES_128_GCM_SHA256},
     true,
     TLS_TLS_CHACHA20_POLY1305_SHA256},
    {"neither server nor client have hardware AES (pick ChaCha)",
     {TLS_TLS_CHACHA20_POLY1305_SHA256, TLS_TLS_AES_128_GCM_SHA256},
     false,
     TLS_TLS_CHACHA20_POLY1305_SHA256},
    {"client prefers AES, server doesn't have hardware (pick ChaCha)",
     {TLS_TLS_AES_128_GCM_SHA256, TLS_TLS_CHACHA20_POLY1305_SHA256},
     false,
     TLS_TLS_CHACHA20_POLY1305_SHA256},
    {"client prefers AES and sends GREASE, server doesn't have hardware (pick ChaCha)",
     {0x0A0A, /* GREASE value */
      TLS_TLS_AES_128_GCM_SHA256, TLS_TLS_CHACHA20_POLY1305_SHA256},
     false,
     TLS_TLS_CHACHA20_POLY1305_SHA256},
    {"client prefers AES, server has hardware AES (pick AES)",
     {TLS_TLS_AES_128_GCM_SHA256, TLS_TLS_CHACHA20_POLY1305_SHA256},
     true,
     TLS_TLS_AES_128_GCM_SHA256},
    {"client prefers AES and sends GREASE, server has hardware AES (pick AES)",
     {0x0A0A, /* GREASE value */
      TLS_TLS_AES_128_GCM_SHA256, TLS_TLS_CHACHA20_POLY1305_SHA256},
     true,
     TLS_TLS_AES_128_GCM_SHA256},
};

static uint16_t versions_tls13[1] = {TLS_VERSION_TLS13};
static uint16_t curves_x25519[1] = {TLS_X25519};

static void aes_cipher_reordering_tls13(void *env, TestingT *t) {
    const AESCipherReorderingTLS13Test *tc = (const AESCipherReorderingTLS13Test *)env;
    burrow__tls_aes_gcm_hardware_override = tc->server_has_aesgcm ? 1 : 0;
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    Error err = BURROW_NO_ERROR;
    IoReader rand = {NULL, NULL};
    EcdhPrivateKey *pk = ecdh_curve_generate_key(ecdh_x25519(), a, rand, &err);
    if (pk == NULL) {
        testing_t_fatal_v(t, "%v", err);
        return;
    }
    TlsKeyShare share = {TLS_X25519,
                         ecdh_public_key_bytes(ecdh_private_key_public_key(pk), a)};
    TlsConfig config;
    memset(&config, 0, sizeof config);
    NetConn c, s;
    local_pipe(a, &c, &s);
    TlsConn *conn = tls_server(heap_allocator(), s, &config);
    if (conn == NULL) {
        testing_t_fatal_v(t, "out of memory");
        return;
    }
    conn->vers = TLS_VERSION_TLS13;
    TlsHandshakeMsg ch;
    burrow__tls_handshake_msg_init(&ch, TLS_MSG_CLIENT_HELLO);
    ch.client_hello.cipher_suites = ciphers_of(tc->client_ciphers);
    ch.client_hello.supported_versions = slice_from(versions_tls13, 1, 1, TYPE_UINT16);
    ch.client_hello.compression_methods = slice_from(compression_none, 1, 1, TYPE_BYTE);
    ch.client_hello.key_shares = slice_from(&share, 1, 1, &burrow__tls_key_share_type);
    ch.client_hello.supported_curves = slice_from(curves_x25519, 1, 1, TYPE_UINT16);
    TlsServerHandshakeStateTLS13 hs;
    memset(&hs, 0, sizeof hs);
    hs.c = conn;
    hs.ctx = context_background();
    hs.client_hello_msg = &ch;
    hs.client_hello = &ch.client_hello;
    hs.a = a;

    err = burrow__tls_server_handshake_state_tls13_process_client_hello(&hs);
    if (BURROW_FAILED(err))
        testing_t_errorf_v(t, "pickCipherSuite failed: %s", error_text(err));

    uint16_t got = hs.suite == NULL ? 0 : hs.suite->id;
    if (tc->expected_cipher != got)
        testing_t_errorf_v(t, "unexpected cipher chosen: want %d, got %d",
                           tc->expected_cipher, got);

    tls_conn_free(conn);
    local_pipe_free(c);
    tls_config_release(&config);
    ecdh_private_key_free(pk);
    arena_free(&ar);
}

static void TestAESCipherReorderingTLS13(TestingT *t) {
    Int n = (Int)(sizeof aes_cipher_reordering_tls13_tests /
                  sizeof aes_cipher_reordering_tls13_tests[0]);
    for (Int i = 0; i < n; i++) {
        const AESCipherReorderingTLS13Test *tc = &aes_cipher_reordering_tls13_tests[i];
        testing_t_run(t, str_from_cstr(tc->name),
                      BURROW_FN(TestingTFunc, aes_cipher_reordering_tls13,
                                (void *)(uintptr_t)tc));
    }
    burrow__tls_aes_gcm_hardware_override = -1;
}

/* Not in Go: the goroutine of TestServerHandshakeContextCancellation. */
typedef struct CancelJob {
    ContextCancelFunc cancel;
    SyncWaitGroup *unblock_client;
    NetConn c;
} CancelJob;

static void cancel_job(void *env) {
    CancelJob *j = (CancelJob *)env;
    BURROW_CALLF0(j->cancel);
    sync_wait_group_wait(j->unblock_client);
    (void)j->c.vt->closer.close(j->c.data);
}

/* TestServerHandshakeContextCancellation tests that canceling the context given
 * to the server side conn.HandshakeContext interrupts the in-progress
 * handshake. */
static void TestServerHandshakeContextCancellation(TestingT *t) {
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    NetConn c, s;
    local_pipe(a, &c, &s);
    ContextCancelFunc cancel;
    Context ctx = context_with_cancel(a, context_background(), &cancel);
    if (BURROW_CONTEXT_IS_NIL(ctx)) {
        testing_t_fatal_v(t, "out of memory");
        return;
    }
    SyncWaitGroup unblock_client = {0};
    sync_wait_group_add(&unblock_client, 1);
    CancelJob job = {cancel, &unblock_client, c};
    SyncWaitGroup wg = {0};
    sync_wait_group_go(&wg, BURROW_FN(Func, cancel_job, &job));
    TlsConfig config = test_config_server(a);
    TlsConn *conn = tls_server(heap_allocator(), s, &config);
    if (conn == NULL) {
        testing_t_fatal_v(t, "out of memory");
        return;
    }
    /* Initiates server side handshake, which will block until a client hello
     * is read unless the cancellation works. */
    Error err = tls_conn_handshake_context(conn, ctx);
    if (!BURROW_FAILED(err)) {
        sync_wait_group_done(&unblock_client);
        sync_wait_group_wait(&wg);
        testing_t_fatal_v(t, "Server handshake did not error when the context was "
                             "canceled");
        return;
    }
    if (err.vt != context_canceled.vt || err.data != context_canceled.data)
        testing_t_errorf_v(t, "Unexpected server handshake error: %v", err);
    /* Not in Go: the skip on js and wasip1, where Go's localPipe does not
     * error on a second Close. This one does everywhere. */
    err = tls_conn_close(conn);
    if (!BURROW_FAILED(err))
        testing_t_error_v(t, "Server connection was not closed when the context was "
                             "canceled");

    sync_wait_group_done(&unblock_client);
    sync_wait_group_wait(&wg);
    tls_conn_free(conn);
    local_pipe_free(c);
    context_release(ctx);
    tls_config_release(&config);
    arena_free(&ar);
}

/* Not in Go: the type of the key TestHandshakeContextHierarchy keeps its value
 * under, which is struct{}{} there. */
static const Type hierarchy_key_type = {
    {(const Byte *)"hierarchyKey", 12},
    {(const Byte *)"tls_test", 8},
    KIND_INT,
    (uint32_t)sizeof(Int),
    (uint16_t)_Alignof(Int),
    0,
    0,
    NULL,
    NULL,
    NULL,
    NULL,
    0,
    0x686b6579U, /* "hkey" */
    NULL,
};

/* Not in Go: what the callbacks and the client goroutine of
 * TestHandshakeContextHierarchy close over. */
typedef struct HierarchyEnv {
    TestingT *t;
    Context ctx;
    NetConn c;
    TlsConfig *client_config;
    Any key;
    TlsCertificate *cert;
    TlsConn *cli;
    Error client_err;
} HierarchyEnv;

static bool hierarchy_has_key(Context ctx, Any key) {
    Any val = context_value(ctx, key);
    return !BURROW_ANY_IS_NIL(val) && val.t == TYPE_BOOL && *(const bool *)val.data;
}

static TlsCertificate *hierarchy_get_client_certificate(
    void *env, TlsCertificateRequestInfo *certificate_request, Error *err) {
    (void)err;
    HierarchyEnv *e = (HierarchyEnv *)env;
    if (!hierarchy_has_key(tls_certificate_request_info_context(certificate_request),
                           e->key))
        testing_t_error_v(e->t, "GetClientCertificate context was not child of "
                                "HandshakeContext");
    return e->cert;
}

static TlsCertificate *
hierarchy_get_certificate(void *env, TlsClientHelloInfo *client_hello, Error *err) {
    (void)err;
    HierarchyEnv *e = (HierarchyEnv *)env;
    if (!hierarchy_has_key(tls_client_hello_info_context(client_hello), e->key))
        testing_t_error_v(e->t, "GetClientCertificate context was not child of "
                                "HandshakeContext");
    return e->cert;
}

static void hierarchy_client(void *env) {
    HierarchyEnv *e = (HierarchyEnv *)env;
    e->client_config->certificates = slice_nil(TYPE_TLS_CERTIFICATE);
    e->client_config->get_client_certificate =
        (TlsGetClientCertificateFunc){hierarchy_get_client_certificate, e};
    e->cli = tls_client(heap_allocator(), e->c, e->client_config);
    if (e->cli == NULL)
        e->client_err = burrow_err_out_of_memory;
    else
        e->client_err = tls_conn_handshake_context(e->cli, e->ctx);
    (void)e->c.vt->closer.close(e->c.data);
}

/* TestHandshakeContextHierarchy tests whether the contexts available to
 * GetClientCertificate and GetCertificate are derived from the context provided
 * to HandshakeContext, and that those contexts are canceled after
 * HandshakeContext has returned.
 *
 * Not in Go: the checks that the inner contexts are done after
 * HandshakeContext returned are left out, as HandshakeContext gives them back
 * before it returns. */
static void TestHandshakeContextHierarchy(TestingT *t) {
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    NetConn c, s;
    local_pipe(a, &c, &s);
    TlsConfig client_config = test_config_client(a);
    TlsConfig server_config = test_config_server(a);
    ContextCancelFunc cancel;
    Context cancel_ctx = context_with_cancel(a, context_background(), &cancel);
    if (BURROW_CONTEXT_IS_NIL(cancel_ctx)) {
        testing_t_fatal_v(t, "out of memory");
        return;
    }
    Int key_value = 0;
    Any key = BURROW_ANY(&hierarchy_key_type, &key_value);
    bool yes = true;
    Context ctx = context_with_value(a, cancel_ctx, key, BURROW_ANY(TYPE_BOOL, &yes));
    if (BURROW_CONTEXT_IS_NIL(ctx)) {
        testing_t_fatal_v(t, "out of memory");
        return;
    }
    TlsCertificate cert =
        parse_test_cert(a, test_rsa2048_cert_pem, test_rsa2048_key_pem);
    HierarchyEnv env = {t, ctx, c, &client_config, key, &cert, NULL, BURROW_NO_ERROR};
    SyncWaitGroup wg = {0};
    sync_wait_group_go(&wg, BURROW_FN(Func, hierarchy_client, &env));
    server_config.certificates = slice_nil(TYPE_TLS_CERTIFICATE);
    server_config.client_auth = TLS_REQUEST_CLIENT_CERT;
    server_config.get_certificate =
        (TlsGetCertificateFunc){hierarchy_get_certificate, &env};
    TlsConn *conn = tls_server(heap_allocator(), s, &server_config);
    if (conn == NULL) {
        testing_t_fatal_v(t, "out of memory");
        return;
    }
    Error err = tls_conn_handshake_context(conn, ctx);
    if (BURROW_FAILED(err))
        testing_t_errorf_v(t, "Unexpected server handshake error: %v", err);
    sync_wait_group_wait(&wg);
    if (BURROW_FAILED(env.client_err))
        testing_t_errorf_v(t, "Unexpected client error: %v", env.client_err);

    (void)s.vt->closer.close(s.data);
    tls_conn_free(conn);
    tls_conn_free(env.cli);
    local_pipe_free(c);
    BURROW_CALLF0(cancel);
    context_release(ctx);
    context_release(cancel_ctx);
    tls_config_release(&server_config);
    tls_config_release(&client_config);
    arena_free(&ar);
}

/* Not in Go: a Config.Time that answers the Time at env. */
static Time fixed_time(void *env) {
    return *(const Time *)env;
}

/* Not in Go: testResume, which the tests below each declare. */
static void test_resume(TestingT *t, TlsConfig *sc, TlsConfig *cc, bool expect_resume) {
    Handshake h;
    test_handshake(t, cc, sc, &h);
    if (BURROW_FAILED(h.err))
        testing_t_fatalf_v(t, "handshake: %v", h.err);
    if (h.client_state.did_resume != expect_resume)
        testing_t_fatalf_v(t, "DidResume = %v; want %v", h.client_state.did_resume,
                           expect_resume);
    if (h.server_state.did_resume != expect_resume)
        testing_t_fatalf_v(t, "DidResume = %v; want %v", h.client_state.did_resume,
                           expect_resume);
    handshake_free(&h);
}

/* Not in Go: the keys of the certificates the tests below make, which are
 * testECDSAP256Key, testECDSAP521Key and testRSA2048Key there. */
typedef struct ResumptionKeys {
    EcdsaPrivateKey *p256;
    EcdsaPrivateKey *p521;
    RsaPrivateKey *rsa2048;
    CryptoPrivateKey p256_any;
} ResumptionKeys;

static ResumptionKeys resumption_keys(Alloc *a) {
    ResumptionKeys k;
    k.p256_any =
        parse_test_cert(a, test_ecdsap256_cert_pem, test_ecdsap256_key_pem).private_key;
    k.p256 = (EcdsaPrivateKey *)k.p256_any.data;
    k.p521 = (EcdsaPrivateKey *)parse_test_cert(a, test_ecdsap521_cert_pem,
                                                test_ecdsap521_key_pem)
                 .private_key.data;
    k.rsa2048 =
        (RsaPrivateKey *)parse_test_cert(a, test_rsa2048_cert_pem, test_rsa2048_key_pem)
            .private_key.data;
    return k;
}

/* Not in Go: a Certificate of the DER certificates in ders, with key. */
static TlsCertificate der_certificate(Alloc *a, const Slice *ders, Int n,
                                      CryptoPrivateKey key) {
    TlsCertificate c;
    memset(&c, 0, sizeof c);
    c.certificate = slice_make(a, TYPE_BYTES, n, n);
    for (Int i = 0; i < n; i++)
        ((Slice *)c.certificate.p)[i] = ders[i];
    c.private_key = key;
    return c;
}

/* Not in Go: createChain of testHandshakeChainExpiryResumption. */
static bool create_chain(TestingT *t, Alloc *a, const ResumptionKeys *k,
                         Time leaf_not_after, Time root_not_after, Slice *leaf_der,
                         Slice *expired_leaf_der, X509Certificate **root) {
    Error err = BURROW_NO_ERROR;
    X509Certificate tmpl;
    memset(&tmpl, 0, sizeof tmpl);
    tmpl.subject.common_name = BURROW_S("root");
    tmpl.not_before = time_add(root_not_after, -TIME_HOUR * 24);
    tmpl.not_after = root_not_after;
    tmpl.is_ca = true;
    tmpl.basic_constraints_valid = true;
    Slice root_der = x509_create_certificate(a, (IoReader){0}, &tmpl, &tmpl,
                                             ecdsa_private_key_public(k->p521),
                                             ecdsa_private_key_signer(k->p521), &err);
    if (BURROW_FAILED(err)) {
        testing_t_fatalf_v(t, "CreateCertificate: %v", err);
        return false;
    }
    *root = x509_parse_certificate(a, root_der, &err);
    if (BURROW_FAILED(err)) {
        testing_t_fatalf_v(t, "ParseCertificate: %v", err);
        return false;
    }

    memset(&tmpl, 0, sizeof tmpl);
    Str dns_name = BURROW_S("expired-resume.example.com");
    tmpl.dns_names = slice_from(&dns_name, 1, 1, TYPE_STRING);
    tmpl.not_before = time_add(leaf_not_after, -TIME_HOUR * 24);
    tmpl.not_after = leaf_not_after;
    tmpl.key_usage = X509_KEY_USAGE_DIGITAL_SIGNATURE;
    Slice leaf_cert_der = x509_create_certificate(
        a, (IoReader){0}, &tmpl, *root, ecdsa_private_key_public(k->p256),
        ecdsa_private_key_signer(k->p521), &err);
    if (BURROW_FAILED(err)) {
        testing_t_fatalf_v(t, "CreateCertificate: %v", err);
        return false;
    }
    tmpl.not_before = time_add(leaf_not_after, -TIME_HOUR * 24 * 365);
    tmpl.not_after = time_add(leaf_not_after, -TIME_HOUR * 24 * 364);
    Slice expired_leaf_der_cert_der = x509_create_certificate(
        a, (IoReader){0}, &tmpl, *root, ecdsa_private_key_public(k->p256),
        ecdsa_private_key_signer(k->p521), &err);
    if (BURROW_FAILED(err)) {
        testing_t_fatalf_v(t, "CreateCertificate: %v", err);
        return false;
    }

    *leaf_der = leaf_cert_der;
    *expired_leaf_der = expired_leaf_der_cert_der;
    return true;
}

/* Not in Go: what testExpiration of testHandshakeChainExpiryResumption closes
 * over, and its two arguments. */
typedef struct ChainExpiryResumption {
    uint16_t version;
    Time now;
    Time leaf_not_after;
    Time root_not_after;
} ChainExpiryResumption;

static void test_expiration(void *env, TestingT *t) {
    const ChainExpiryResumption *x = (const ChainExpiryResumption *)env;
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    ResumptionKeys k = resumption_keys(a);
    Time now = x->now;
    Slice initial_leaf_der, expired_leaf_der;
    X509Certificate *initial_root;
    if (!create_chain(t, a, &k, x->leaf_not_after, x->root_not_after, &initial_leaf_der,
                      &expired_leaf_der, &initial_root))
        return;

    Slice ders[2] = {initial_leaf_der, expired_leaf_der};
    TlsCertificate server_cert = der_certificate(a, ders, 2, k.p256_any);
    TlsConfig server_config = test_config_server(a);
    server_config.max_version = x->version;
    server_config.certificates = slice_from(&server_cert, 1, 1, TYPE_TLS_CERTIFICATE);
    server_config.client_cas = x509_new_cert_pool(a);
    x509_cert_pool_add_cert(server_config.client_cas, initial_root);
    server_config.client_auth = TLS_REQUIRE_AND_VERIFY_CLIENT_CERT;
    server_config.time = (TlsTimeFunc){fixed_time, &now};
    server_config.insecure_skip_verify = false;
    server_config.server_name = BURROW_S("expired-resume.example.com");

    TlsCertificate client_cert = der_certificate(a, ders, 2, k.p256_any);
    TlsConfig client_config = test_config_client(a);
    client_config.max_version = x->version;
    client_config.certificates = slice_from(&client_cert, 1, 1, TYPE_TLS_CERTIFICATE);
    client_config.root_cas = x509_new_cert_pool(a);
    x509_cert_pool_add_cert(client_config.root_cas, initial_root);
    client_config.server_name = BURROW_S("expired-resume.example.com");
    client_config.client_session_cache =
        tls_new_lru_client_session_cache(heap_allocator(), 32);
    if (client_config.client_session_cache.vt == NULL)
        testing_t_fatal_v(t, "out of memory");
    client_config.insecure_skip_verify = false;
    client_config.server_name = BURROW_S("expired-resume.example.com");
    client_config.time = (TlsTimeFunc){fixed_time, &now};

    test_resume(t, &server_config, &client_config, false);
    test_resume(t, &server_config, &client_config, true);

    int64_t leaf_nano = time_unix_nano(x->leaf_not_after);
    int64_t root_nano = time_unix_nano(x->root_not_after);
    Time expired_now = time_add(
        time_from_unix(0, leaf_nano < root_nano ? leaf_nano : root_nano), TIME_MINUTE);

    Slice fresh_leaf_der;
    X509Certificate *fresh_root;
    if (!create_chain(t, a, &k, time_add(expired_now, TIME_HOUR),
                      time_add(expired_now, TIME_HOUR), &fresh_leaf_der,
                      &expired_leaf_der, &fresh_root))
        return;
    Slice fresh_ders[2] = {fresh_leaf_der, expired_leaf_der};
    client_cert = der_certificate(a, fresh_ders, 2, k.p256_any);
    server_config.time = (TlsTimeFunc){fixed_time, &expired_now};
    server_config.client_cas = x509_new_cert_pool(a);
    x509_cert_pool_add_cert(server_config.client_cas, fresh_root);

    test_resume(t, &server_config, &client_config, false);

    tls_lru_client_session_cache_free(client_config.client_session_cache);
    tls_config_release(&server_config);
    tls_config_release(&client_config);
    arena_free(&ar);
}

static void test_handshake_chain_expiry_resumption(TestingT *t, uint16_t version) {
    Time now = time_now();

    ChainExpiryResumption before = {version, now, time_add(now, 2 * TIME_HOUR),
                                    time_add(now, 3 * TIME_HOUR)};
    testing_t_run(t, BURROW_S("LeafExpiresBeforeRoot"),
                  BURROW_FN(TestingTFunc, test_expiration, &before));
    ChainExpiryResumption after = {version, now, time_add(now, 2 * TIME_HOUR),
                                   time_add(now, TIME_HOUR)};
    testing_t_run(t, BURROW_S("LeafExpiresAfterRoot"),
                  BURROW_FN(TestingTFunc, test_expiration, &after));
}

static void chain_expiry_resumption_tls12(void *env, TestingT *t) {
    (void)env;
    test_handshake_chain_expiry_resumption(t, TLS_VERSION_TLS12);
}

static void chain_expiry_resumption_tls13(void *env, TestingT *t) {
    (void)env;
    test_handshake_chain_expiry_resumption(t, TLS_VERSION_TLS13);
}

static void TestHandshakeChainExpiryResumption(TestingT *t) {
    testing_t_run(t, BURROW_S("TLS1.2"),
                  BURROW_FN(TestingTFunc, chain_expiry_resumption_tls12, NULL));
    testing_t_run(t, BURROW_S("TLS1.3"),
                  BURROW_FN(TestingTFunc, chain_expiry_resumption_tls13, NULL));
}

/* Not in Go: the roots and certificates both tests below make first. */
typedef struct TwoRoots {
    X509Certificate *root_a;
    X509Certificate *root_b;
    Slice cert_a;
    Slice cert_b;
} TwoRoots;

static bool two_roots(TestingT *t, Alloc *a, const ResumptionKeys *k, Time now,
                      TwoRoots *r) {
    Error err = BURROW_NO_ERROR;
    X509Certificate tmpl;
    memset(&tmpl, 0, sizeof tmpl);
    tmpl.subject.common_name = BURROW_S("root");
    tmpl.not_before = time_add(now, -TIME_HOUR * 24);
    tmpl.not_after = time_add(now, TIME_HOUR * 24);
    tmpl.is_ca = true;
    tmpl.basic_constraints_valid = true;
    Slice root_der = x509_create_certificate(a, (IoReader){0}, &tmpl, &tmpl,
                                             ecdsa_private_key_public(k->p521),
                                             ecdsa_private_key_signer(k->p521), &err);
    if (BURROW_FAILED(err)) {
        testing_t_fatalf_v(t, "CreateCertificate: %v", err);
        return false;
    }
    r->root_a = x509_parse_certificate(a, root_der, &err);
    if (BURROW_FAILED(err)) {
        testing_t_fatalf_v(t, "ParseCertificate: %v", err);
        return false;
    }
    root_der = x509_create_certificate(a, (IoReader){0}, &tmpl, &tmpl,
                                       rsa_private_key_public(k->rsa2048),
                                       rsa_private_key_signer(k->rsa2048), &err);
    if (BURROW_FAILED(err)) {
        testing_t_fatalf_v(t, "CreateCertificate: %v", err);
        return false;
    }
    r->root_b = x509_parse_certificate(a, root_der, &err);
    if (BURROW_FAILED(err)) {
        testing_t_fatalf_v(t, "ParseCertificate: %v", err);
        return false;
    }

    memset(&tmpl, 0, sizeof tmpl);
    Str dns_name = BURROW_S("example.com");
    tmpl.dns_names = slice_from(&dns_name, 1, 1, TYPE_STRING);
    tmpl.not_before = time_add(now, -TIME_HOUR * 24);
    tmpl.not_after = time_add(now, TIME_HOUR * 24);
    tmpl.key_usage = X509_KEY_USAGE_DIGITAL_SIGNATURE;
    r->cert_a = x509_create_certificate(a, (IoReader){0}, &tmpl, r->root_a,
                                        ecdsa_private_key_public(k->p256),
                                        ecdsa_private_key_signer(k->p521), &err);
    if (BURROW_FAILED(err)) {
        testing_t_fatalf_v(t, "CreateCertificate: %v", err);
        return false;
    }
    r->cert_b = x509_create_certificate(a, (IoReader){0}, &tmpl, r->root_b,
                                        ecdsa_private_key_public(k->p256),
                                        rsa_private_key_signer(k->rsa2048), &err);
    if (BURROW_FAILED(err)) {
        testing_t_fatalf_v(t, "CreateCertificate: %v", err);
        return false;
    }
    return true;
}

/* Not in Go: what the GetConfigForClient of
 * testHandshakeGetConfigForClientDifferentClientCAs closes over. The clones
 * it makes are kept to be given back at the end. */
typedef struct SwitchConfig {
    bool switch_config;
    TlsConfig *server_config;
    X509Certificate *root_b;
    Alloc *a;
    TlsConfig *clones[2];
    int nclones;
} SwitchConfig;

static TlsConfig *switch_config_for_client(void *env, TlsClientHelloInfo *client_hello,
                                           Error *err) {
    (void)client_hello;
    SwitchConfig *e = (SwitchConfig *)env;
    if (!e->switch_config)
        return NULL;
    if (e->nclones == 2) {
        BURROW_OUT(err, fmt_errorf_v("more handshakes than the test makes"));
        return NULL;
    }
    TlsConfig *cfg = tls_config_clone(e->server_config, e->a);
    if (cfg == NULL) {
        BURROW_OUT(err, burrow_err_out_of_memory);
        return NULL;
    }
    e->clones[e->nclones++] = cfg;
    cfg->client_cas = x509_new_cert_pool(e->a);
    x509_cert_pool_add_cert(cfg->client_cas, e->root_b);
    return cfg;
}

static void
test_handshake_get_config_for_client_different_client_cas(TestingT *t,
                                                          uint16_t version) {
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    ResumptionKeys k = resumption_keys(a);
    Time now = time_now();
    TwoRoots r;
    if (!two_roots(t, a, &k, now, &r))
        return;

    TlsCertificate server_cert = der_certificate(a, &r.cert_a, 1, k.p256_any);
    TlsConfig server_config = test_config_server(a);
    server_config.max_version = version;
    server_config.certificates = slice_from(&server_cert, 1, 1, TYPE_TLS_CERTIFICATE);
    server_config.time = (TlsTimeFunc){fixed_time, &now};
    server_config.client_cas = x509_new_cert_pool(a);
    x509_cert_pool_add_cert(server_config.client_cas, r.root_a);
    server_config.client_auth = TLS_REQUIRE_AND_VERIFY_CLIENT_CERT;
    SwitchConfig sw;
    memset(&sw, 0, sizeof sw);
    sw.server_config = &server_config;
    sw.root_b = r.root_b;
    sw.a = a;
    server_config.get_config_for_client =
        (TlsGetConfigForClientFunc){switch_config_for_client, &sw};
    server_config.insecure_skip_verify = false;
    server_config.server_name = BURROW_S("example.com");

    TlsCertificate client_cert = der_certificate(a, &r.cert_a, 1, k.p256_any);
    TlsConfig client_config = test_config_client(a);
    client_config.max_version = version;
    client_config.certificates = slice_from(&client_cert, 1, 1, TYPE_TLS_CERTIFICATE);
    client_config.client_session_cache =
        tls_new_lru_client_session_cache(heap_allocator(), 32);
    if (client_config.client_session_cache.vt == NULL)
        testing_t_fatal_v(t, "out of memory");
    client_config.root_cas = x509_new_cert_pool(a);
    x509_cert_pool_add_cert(client_config.root_cas, r.root_a);
    client_config.time = (TlsTimeFunc){fixed_time, &now};
    client_config.insecure_skip_verify = false;
    client_config.server_name = BURROW_S("example.com");

    test_resume(t, &server_config, &client_config, false);
    test_resume(t, &server_config, &client_config, true);

    client_cert.certificate = slice_make(a, TYPE_BYTES, 1, 1);
    ((Slice *)client_cert.certificate.p)[0] = r.cert_b;

    /* Cause GetConfigForClient to return a config cloned from the base config,
     * but with a different ClientCAs pool. This should cause resumption to
     * fail. */
    sw.switch_config = true;

    test_resume(t, &server_config, &client_config, false);
    test_resume(t, &server_config, &client_config, true);

    for (int i = 0; i < sw.nclones; i++)
        tls_config_release(sw.clones[i]);
    tls_lru_client_session_cache_free(client_config.client_session_cache);
    tls_config_release(&server_config);
    tls_config_release(&client_config);
    arena_free(&ar);
}

static void get_config_for_client_different_client_cas_tls12(void *env, TestingT *t) {
    (void)env;
    test_handshake_get_config_for_client_different_client_cas(t, TLS_VERSION_TLS12);
}

static void get_config_for_client_different_client_cas_tls13(void *env, TestingT *t) {
    (void)env;
    test_handshake_get_config_for_client_different_client_cas(t, TLS_VERSION_TLS13);
}

static void TestHandshakeGetConfigForClientDifferentClientCAs(TestingT *t) {
    testing_t_run(t, BURROW_S("TLS1.2"),
                  BURROW_FN(TestingTFunc,
                            get_config_for_client_different_client_cas_tls12, NULL));
    testing_t_run(t, BURROW_S("TLS1.3"),
                  BURROW_FN(TestingTFunc,
                            get_config_for_client_different_client_cas_tls13, NULL));
}

static void test_handshake_change_root_cas_resumption(TestingT *t, uint16_t version) {
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    ResumptionKeys k = resumption_keys(a);
    Time now = time_now();
    TwoRoots r;
    if (!two_roots(t, a, &k, now, &r))
        return;

    TlsCertificate server_cert = der_certificate(a, &r.cert_a, 1, k.p256_any);
    TlsConfig server_config = test_config_server(a);
    server_config.max_version = version;
    server_config.certificates = slice_from(&server_cert, 1, 1, TYPE_TLS_CERTIFICATE);
    server_config.time = (TlsTimeFunc){fixed_time, &now};
    server_config.client_cas = x509_new_cert_pool(a);
    x509_cert_pool_add_cert(server_config.client_cas, r.root_a);
    server_config.client_auth = TLS_REQUIRE_AND_VERIFY_CLIENT_CERT;
    server_config.insecure_skip_verify = false;
    server_config.server_name = BURROW_S("example.com");

    TlsCertificate client_cert = der_certificate(a, &r.cert_a, 1, k.p256_any);
    TlsConfig client_config = test_config_client(a);
    client_config.max_version = version;
    client_config.certificates = slice_from(&client_cert, 1, 1, TYPE_TLS_CERTIFICATE);
    client_config.client_session_cache =
        tls_new_lru_client_session_cache(heap_allocator(), 32);
    if (client_config.client_session_cache.vt == NULL)
        testing_t_fatal_v(t, "out of memory");
    client_config.root_cas = x509_new_cert_pool(a);
    x509_cert_pool_add_cert(client_config.root_cas, r.root_a);
    client_config.time = (TlsTimeFunc){fixed_time, &now};
    client_config.insecure_skip_verify = false;
    client_config.server_name = BURROW_S("example.com");

    test_resume(t, &server_config, &client_config, false);
    test_resume(t, &server_config, &client_config, true);

    TlsConfig *client_config2 = tls_config_clone(&client_config, a);
    if (client_config2 == NULL) {
        testing_t_fatal_v(t, "out of memory");
        return;
    }
    client_config2->root_cas = x509_new_cert_pool(a);
    x509_cert_pool_add_cert(client_config2->root_cas, r.root_b);

    server_cert.certificate = slice_make(a, TYPE_BYTES, 1, 1);
    ((Slice *)server_cert.certificate.p)[0] = r.cert_b;

    test_resume(t, &server_config, client_config2, false);
    test_resume(t, &server_config, client_config2, true);

    tls_lru_client_session_cache_free(client_config.client_session_cache);
    tls_config_release(client_config2);
    tls_config_release(&server_config);
    tls_config_release(&client_config);
    arena_free(&ar);
}

static void change_root_cas_resumption_tls12(void *env, TestingT *t) {
    (void)env;
    test_handshake_change_root_cas_resumption(t, TLS_VERSION_TLS12);
}

static void change_root_cas_resumption_tls13(void *env, TestingT *t) {
    (void)env;
    test_handshake_change_root_cas_resumption(t, TLS_VERSION_TLS13);
}

static void TestHandshakeChangeRootCAsResumption(TestingT *t) {
    testing_t_run(t, BURROW_S("TLS1.2"),
                  BURROW_FN(TestingTFunc, change_root_cas_resumption_tls12, NULL));
    testing_t_run(t, BURROW_S("TLS1.3"),
                  BURROW_FN(TestingTFunc, change_root_cas_resumption_tls13, NULL));
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
    X(TestClose)                                                                       \
    X(TestVersion)                                                                     \
    X(TestCipherSuitePreference)                                                       \
    X(TestSCTHandshake)                                                                \
    X(TestCrossVersionResume)                                                          \
    X(TestHandshakeServerGetCertificateExtensions)                                     \
    X(TestHandshakeServerSNIGetCertificateError)                                       \
    X(TestHandshakeServerEmptyCertificates)                                            \
    X(TestSNIGivenOnFailure)                                                           \
    X(TestGetConfigForClient)                                                          \
    X(TestCloseServerConnectionOnIdleClient)                                           \
    X(TestCloneHash)                                                                   \
    X(TestKeyTooSmallForRSAPSS)                                                        \
    X(TestMultipleCertificates)                                                        \
    X(TestAESCipherReordering)                                                         \
    X(TestAESCipherReorderingTLS13)                                                    \
    X(TestServerHandshakeContextCancellation)                                          \
    X(TestHandshakeContextHierarchy)                                                   \
    X(TestHandshakeChainExpiryResumption)                                              \
    X(TestHandshakeGetConfigForClientDifferentClientCAs)                               \
    X(TestHandshakeChangeRootCAsResumption)

TESTING_MAIN(TESTS)
