/* crypto/tls: the test configurations and localPipe of handshake_test.go, and
 * testHandshake of handshake_server_test.go, which the tests of more than one
 * file run on.
 *
 * Copyright 2009 The Go Authors. All rights reserved.
 * Copyright 2026 The burrow Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style licence that can be found
 * in the LICENSE file. */

#ifndef BURROW_TESTS_TLS_HANDSHAKE_TEST_H
#define BURROW_TESTS_TLS_HANDSHAKE_TEST_H

#include "burrow/burrow.h"
#include "burrow/crypto/tls.h"
#include "burrow/crypto/x509.h"
#include "tls_test_gen.h"

#include <string.h>

static inline Time test_time(void *env) {
    (void)env;
    return time_from_unix(1476984729, 0);
}

/* Not in Go: testConfigServer, made in a. tls_config_release gives back the
 * session ticket keys it comes to hold. */
static inline TlsConfig test_config_server(Alloc *a) {
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
static inline TlsConfig test_config_client(Alloc *a) {
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

/* Not in Go: localPipe. Go's is a TCP connection on the loopback, and the
 * socket buffers let one side write a whole flight before the other side reads
 * any of it, which a TLS 1.3 server does when it sends its session tickets. The
 * tests have no sockets on Windows and WASI, so this is a pair of ends with the
 * same buffering: a write never waits, and a read waits for bytes or a close.
 * As with TCP, what is written after the other end closed is dropped, and a
 * read after it closed gets what is left and then io_eof. The ends have no
 * addresses or deadlines. */
typedef struct LocalPipe LocalPipe;

typedef struct LocalPipeEnd {
    LocalPipe *p;
    int i;
} LocalPipeEnd;

struct LocalPipe {
    SyncMutex mu;
    SyncCond cond;
    BytesBuffer in[2]; /* what end i has to read */
    bool closed[2];
    LocalPipeEnd end[2];
};

static inline Int local_pipe_read(void *self, Slice b, Error *err) {
    LocalPipeEnd *e = (LocalPipeEnd *)self;
    LocalPipe *p = e->p;
    BytesBuffer *in = &p->in[e->i];
    BURROW_OUT(err, BURROW_NO_ERROR);
    Int n = 0;
    sync_mutex_lock(&p->mu);
    while (!p->closed[0] && !p->closed[1] && bytes_buffer_len(in) == 0 && b.len > 0)
        sync_cond_wait(&p->cond);
    if (p->closed[e->i])
        BURROW_OUT(err, net_err_closed);
    else if (bytes_buffer_len(in) > 0)
        n = bytes_buffer_read(in, b, err);
    else if (b.len > 0)
        BURROW_OUT(err, io_eof);
    sync_mutex_unlock(&p->mu);
    return n;
}

static inline Int local_pipe_write(void *self, Slice b, Error *err) {
    LocalPipeEnd *e = (LocalPipeEnd *)self;
    LocalPipe *p = e->p;
    BURROW_OUT(err, BURROW_NO_ERROR);
    Int n = b.len;
    sync_mutex_lock(&p->mu);
    if (p->closed[e->i]) {
        BURROW_OUT(err, net_err_closed);
        n = 0;
    } else if (!p->closed[1 - e->i]) {
        n = bytes_buffer_write(&p->in[1 - e->i], b, err);
        sync_cond_broadcast(&p->cond);
    }
    sync_mutex_unlock(&p->mu);
    return n;
}

static inline Error local_pipe_close(void *self) {
    LocalPipeEnd *e = (LocalPipeEnd *)self;
    LocalPipe *p = e->p;
    Error err = BURROW_NO_ERROR;
    sync_mutex_lock(&p->mu);
    if (p->closed[e->i])
        err = net_err_closed;
    p->closed[e->i] = true;
    sync_cond_broadcast(&p->cond);
    sync_mutex_unlock(&p->mu);
    return err;
}

static inline NetAddr local_pipe_addr(void *self) {
    (void)self;
    NetAddr none = {NULL, NULL};
    return none;
}

static inline Error local_pipe_set_deadline(void *self, Time t) {
    (void)self;
    (void)t;
    return errors_err_unsupported;
}

static const Type local_pipe_type = {
    {(const Byte *)"localPipe", 9},
    {(const Byte *)"tls_test", 8},
    KIND_STRUCT,
    (uint32_t)sizeof(LocalPipeEnd),
    (uint16_t)_Alignof(LocalPipeEnd),
    0,
    0,
    NULL,
    NULL,
    NULL,
    NULL,
    0,
    0x6c706970U, /* "lpip" */
    NULL,
};

static const NetConnVT local_pipe_vt = {
    {&local_pipe_type, local_pipe_read},
    {&local_pipe_type, local_pipe_write},
    {&local_pipe_type, local_pipe_close},
    local_pipe_addr,
    local_pipe_addr,
    local_pipe_set_deadline,
    local_pipe_set_deadline,
    local_pipe_set_deadline,
};

/* The pair is made in a. What is written is kept on the heap, since both ends
 * grow it while the test goes on making things in a, and local_pipe_free gives
 * it back once nothing uses either end. */
static inline void local_pipe(Alloc *a, NetConn *c1, NetConn *c2) {
    LocalPipe *p = BURROW_NEW(a, LocalPipe);
    if (p == NULL)
        panic_str(BURROW_S("out of memory"));
    memset(p, 0, sizeof *p);
    p->cond = SYNC_COND(sync_mutex_locker(&p->mu));
    for (int i = 0; i < 2; i++) {
        p->in[i] = BYTES_BUFFER(heap_allocator());
        p->end[i].p = p;
        p->end[i].i = i;
    }
    *c1 = (NetConn){&local_pipe_vt, &p->end[0]};
    *c2 = (NetConn){&local_pipe_vt, &p->end[1]};
}

static inline void local_pipe_free(NetConn c) {
    LocalPipe *p = ((LocalPipeEnd *)c.data)->p;
    bytes_buffer_free(&p->in[0]);
    bytes_buffer_free(&p->in[1]);
}

/* Not in Go: the goroutine testHandshake starts for the client side. The
 * error it ends with is kept in ar. */
typedef struct HandshakeClient {
    TestingT *t;
    NetConn c;
    TlsConfig *client_config;
    TlsConfig *server_config;
    TlsConn *cli;
    TlsConnectionState client_state;
    Error err;
    Arena ar;
} HandshakeClient;

static const char sentinel[] = "SENTINEL\n";

static inline void handshake_client(void *env) {
    HandshakeClient *j = (HandshakeClient *)env;
    Alloc *a = arena_allocator(&j->ar);
    j->cli = tls_client(heap_allocator(), j->c, j->client_config);
    if (j->cli == NULL) {
        j->err = burrow_err_out_of_memory;
        (void)j->c.vt->closer.close(j->c.data);
        return;
    }
    Error err = tls_conn_handshake(j->cli);
    if (BURROW_FAILED(err)) {
        j->err = error_retain(a, fmt_errorf_v("client: %v", err));
        (void)j->c.vt->closer.close(j->c.data);
        return;
    }
    j->client_state = tls_conn_connection_state(j->cli);
    Slice buf =
        io_read_all(a, net_conn_as_io_reader(tls_conn_as_net_conn(j->cli)), &err);
    if (BURROW_FAILED(err)) {
        if (j->server_config->client_auth != TLS_NO_CLIENT_CERT &&
            j->client_state.version == TLS_VERSION_TLS13) {
            /* In TLS 1.3, client certificates are sent after the server's
             * handshake has completed, and the client only learns about it
             * reading the alert after the handshake. */
            j->err = error_retain(a, fmt_errorf_v("client (from Read): %v", err));
            (void)j->c.vt->closer.close(j->c.data);
            return;
        }
        testing_t_errorf_v(j->t, "failed to call cli.Read: %v", err);
    }
    Str got = {(const Byte *)buf.p, buf.len};
    if (!str_eq(got, str_from_cstr(sentinel)))
        testing_t_errorf_v(j->t, "read %q from TLS connection, but expected %q", got,
                           str_from_cstr(sentinel));
    /* We discard the error because after ReadAll returns the server must have
     * already closed the connection. Sending data (the closeNotify alert) can
     * cause a reset, that will make Close return an error. */
    (void)tls_conn_close(j->cli);
}

/* Not in Go: what testHandshake returns. The states and err can point into the
 * Conns, so handshake_free gives everything back once the test is done with
 * them. */
typedef struct Handshake {
    TlsConnectionState server_state;
    TlsConnectionState client_state;
    Error err;

    TlsConn *server;
    HandshakeClient client;
    NetConn c;
    Arena ar;
} Handshake;

static inline void test_handshake(TestingT *t, TlsConfig *client_config,
                                  TlsConfig *server_config, Handshake *h) {
    memset(h, 0, sizeof *h);
    arena_init(&h->ar, NULL, 0);
    Alloc *a = arena_allocator(&h->ar);
    NetConn c, s;
    local_pipe(a, &c, &s);
    h->c = c;
    HandshakeClient *j = &h->client;
    j->t = t;
    j->c = c;
    j->client_config = client_config;
    j->server_config = server_config;
    arena_init(&j->ar, NULL, 0);
    SyncWaitGroup wg = {0};
    sync_wait_group_go(&wg, BURROW_FN(Func, handshake_client, j));
    h->server = tls_server(heap_allocator(), s, server_config);
    if (h->server == NULL)
        testing_t_fatal_v(t, "out of memory");
    Error err = tls_conn_handshake(h->server);
    if (!BURROW_FAILED(err)) {
        h->server_state = tls_conn_connection_state(h->server);
        Error werr = BURROW_NO_ERROR;
        Int n = (Int)strlen(sentinel);
        (void)tls_conn_write(
            h->server, slice_from((Byte *)(uintptr_t)sentinel, n, n, TYPE_BYTE), &werr);
        if (BURROW_FAILED(werr))
            testing_t_errorf_v(t, "failed to call server.Write: %v", werr);
        Error cerr = tls_conn_close(h->server);
        if (BURROW_FAILED(cerr))
            testing_t_errorf_v(t, "failed to call server.Close: %v", cerr);
    } else {
        err = fmt_errorf_v("server: %v", err);
        (void)s.vt->closer.close(s.data);
    }
    sync_wait_group_wait(&wg);
    h->client_state = j->client_state;
    h->err = errors_join_v(a, 2, err, j->err);
}

static inline void handshake_free(Handshake *h) {
    tls_conn_free(h->server);
    tls_conn_free(h->client.cli);
    local_pipe_free(h->c);
    arena_free(&h->client.ar);
    arena_free(&h->ar);
}

#endif /* BURROW_TESTS_TLS_HANDSHAKE_TEST_H */
