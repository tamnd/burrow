/* Derived from Go's src/crypto/tls/handshake_test.go.
 * Go source: go1.27.1.
 *
 * The test configurations and localPipe of handshake_test.go are in
 * tls_handshake_test.h, since the tests of the other files run on them too.
 * The tests here reach into the package for what Go's tests call: readClientHello,
 * the TLS 1.3 server's processClientHello, makeClientHello and
 * writeRecordLocked.
 *
 * Copyright 2013 The Go Authors. All rights reserved.
 * Copyright 2026 The burrow Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style licence that can be found
 * in the LICENSE file. */

#include "check.h"

#include "../src/crypto/tls_internal.h"
#include "burrow/burrow.h"
#include "burrow/crypto/tls.h"
#include "burrow/strings.h"
#include "tls_handshake_test.h"

#include <string.h>

/* concatHandshakeMessages marshals and concatenates the given handshake
 * messages into a single record.
 *
 * Not in Go: msgs holds n messages, at most four, and the record is made in
 * a. */
static Slice concat_handshake_messages(Alloc *a, const TlsHandshakeMsg *const *msgs,
                                       Int n, Error *err) {
    Slice data[4];
    Int m = 0;
    for (Int i = 0; i < n; i++) {
        data[i] = burrow__tls_handshake_msg_marshal(msgs[i], a, err);
        if (BURROW_FAILED(*err))
            return slice_nil(TYPE_BYTE);
        m += data[i].len;
    }
    Slice out_buf =
        slice_make(a, TYPE_BYTE, TLS_RECORD_HEADER_LEN + m, TLS_RECORD_HEADER_LEN + m);
    if (out_buf.p == NULL) {
        *err = burrow_err_out_of_memory;
        return slice_nil(TYPE_BYTE);
    }
    Byte *b = out_buf.p;
    b[0] = TLS_RECORD_TYPE_HANDSHAKE;
    uint16_t vers = TLS_VERSION_TLS12;
    b[1] = (Byte)(vers >> 8);
    b[2] = (Byte)vers;
    b[3] = (Byte)(m >> 8);
    b[4] = (Byte)m;
    Int off = TLS_RECORD_HEADER_LEN;
    for (Int i = 0; i < n; i++) {
        if (data[i].len > 0)
            memcpy(b + off, data[i].p, (size_t)data[i].len);
        off += data[i].len;
    }
    return out_buf;
}

/* Not in Go: what the goroutine of a trailing message test works with. The
 * test frees it once the goroutine is done. */
typedef struct TrailingJob {
    TestingT *t;
    NetConn conn;
    TlsConfig config;
    TlsConn *tls;
    Arena ar;
} TrailingJob;

static void trailing_job_init(TrailingJob *j, TestingT *t, NetConn conn, bool server) {
    memset(j, 0, sizeof *j);
    j->t = t;
    j->conn = conn;
    arena_init(&j->ar, NULL, 0);
    if (server)
        j->config = test_config_server(arena_allocator(&j->ar));
    else
        j->config = test_config_client(arena_allocator(&j->ar));
}

static void trailing_job_free(TrailingJob *j) {
    if (j->tls != NULL)
        tls_conn_free(j->tls);
    tls_config_release(&j->config);
    arena_free(&j->ar);
}

/* Not in Go: the goroutine of TestServerHelloTrailingMessage. */
static void server_hello_trailing_server(void *env) {
    TrailingJob *j = (TrailingJob *)env;
    Alloc *a = arena_allocator(&j->ar);
    Context ctx = context_background();
    j->tls = tls_server(heap_allocator(), j->conn, &j->config);
    if (j->tls == NULL)
        testing_t_fatal_v(j->t, "out of memory");
    TlsConn *srv = j->tls;
    TlsHandshakeMsg *client_hello = NULL;
    TlsECHServerContext *ech = NULL;
    Error err = burrow__tls_conn_read_client_hello(srv, ctx, a, &client_hello, &ech);
    if (BURROW_FAILED(err))
        testing_t_fatal_v(j->t, "%v", err);

    TlsServerHandshakeStateTLS13 hs;
    memset(&hs, 0, sizeof hs);
    hs.c = srv;
    hs.ctx = ctx;
    hs.client_hello = &client_hello->client_hello;
    hs.client_hello_msg = client_hello;
    hs.a = a;
    err = burrow__tls_server_handshake_state_tls13_process_client_hello(&hs);
    if (BURROW_FAILED(err))
        testing_t_fatal_v(j->t, "%v", err);
    err = burrow__tls_transcript_msg(hs.client_hello_msg,
                                     hash_as_io_writer(hs.transcript), a);
    if (BURROW_FAILED(err))
        testing_t_fatal_v(j->t, "%v", err);

    TlsHandshakeMsg ee;
    burrow__tls_handshake_msg_init(&ee, TLS_MSG_ENCRYPTED_EXTENSIONS);
    ee.encrypted_extensions.alpn_protocol = BURROW_S("h2");
    const TlsHandshakeMsg *msgs[] = {hs.hello_msg, &ee};
    Slice record = concat_handshake_messages(a, msgs, 2, &err);
    if (BURROW_FAILED(err))
        testing_t_fatal_v(j->t, "%v", err);

    (void)j->conn.vt->writer.write(j->conn.data, record, &err);
    if (BURROW_FAILED(err))
        testing_t_fatal_v(j->t, "%v", err);
    (void)tls_conn_close(srv);
}

static void TestServerHelloTrailingMessage(TestingT *t) {
    /* In TLS 1.3 the change cipher spec message is optional. If a CCS message
     * is not sent, after reading the ServerHello, the read traffic secret is
     * set, and all following messages must be encrypted. If the server sends
     * additional unencrypted messages in a record with the ServerHello, the
     * client must either fail or ignore the additional messages. */

    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    NetConn c, s;
    local_pipe(a, &c, &s);
    TlsConfig client_config = test_config_client(a);
    TlsConn *cli = tls_client(heap_allocator(), c, &client_config);
    if (cli == NULL) {
        local_pipe_free(c);
        tls_config_release(&client_config);
        arena_free(&ar);
        testing_t_fatal_v(t, "out of memory");
    }
    TrailingJob job;
    trailing_job_init(&job, t, s, true);
    SyncWaitGroup wg = {0};
    sync_wait_group_go(&wg, BURROW_FN(Func, server_hello_trailing_server, &job));

    /* Not in Go: Errorf stands for Fatalf, so that the goroutine is waited for
     * and everything freed after it. */
    Str expected_err =
        BURROW_S("tls: handshake buffer not empty before setting read traffic secret");
    Error err = tls_conn_handshake(cli);
    if (!BURROW_FAILED(err))
        testing_t_errorf_v(t, "expected error from incomplete handshake, got nil");
    else if (!str_eq(error_text(err), expected_err))
        testing_t_errorf_v(t, "expected error %q, got %q", expected_err,
                           error_text(err));

    sync_wait_group_wait(&wg);
    tls_conn_free(cli);
    trailing_job_free(&job);
    local_pipe_free(c);
    tls_config_release(&client_config);
    arena_free(&ar);
}

/* Not in Go: the goroutine of TestClientHelloTrailingMessage. */
static void client_hello_trailing_client(void *env) {
    TrailingJob *j = (TrailingJob *)env;
    Alloc *a = arena_allocator(&j->ar);
    j->tls = tls_client(heap_allocator(), j->conn, &j->config);
    if (j->tls == NULL)
        testing_t_fatal_v(j->t, "out of memory");
    TlsConn *cli = j->tls;

    TlsMadeClientHello hello;
    memset(&hello, 0, sizeof hello);
    Error err = burrow__tls_conn_make_client_hello(cli, a, &hello);
    burrow__tls_key_share_private_keys_free(&hello.key_share_keys);
    if (BURROW_FAILED(err))
        testing_t_fatal_v(j->t, "%v", err);

    TlsHandshakeMsg cert;
    burrow__tls_handshake_msg_init(&cert, TLS_MSG_CERTIFICATE_TLS13);
    const TlsHandshakeMsg *msgs[] = {&hello.msg, &cert};
    Slice record = concat_handshake_messages(a, msgs, 2, &err);
    if (BURROW_FAILED(err))
        testing_t_fatal_v(j->t, "%v", err);

    (void)j->conn.vt->writer.write(j->conn.data, record, &err);
    if (BURROW_FAILED(err))
        testing_t_fatal_v(j->t, "%v", err);
    (void)tls_conn_close(cli);
}

/* Not in Go. Starts goroutine on the client end of a pipe, runs the handshake
 * of a server on the other end and checks that it fails with expected_err.
 * Errorf stands for Fatalf, so that the goroutine is waited for and everything
 * freed after it. */
static void test_client_hello_trailing(TestingT *t, void (*goroutine)(void *env),
                                       Str expected_err) {
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    NetConn c, s;
    local_pipe(a, &c, &s);
    TlsConfig server_config = test_config_server(a);
    TlsConn *srv = tls_server(heap_allocator(), s, &server_config);
    if (srv == NULL) {
        local_pipe_free(c);
        tls_config_release(&server_config);
        arena_free(&ar);
        testing_t_fatal_v(t, "out of memory");
    }
    TrailingJob job;
    trailing_job_init(&job, t, c, false);
    SyncWaitGroup wg = {0};
    sync_wait_group_go(&wg, BURROW_FN(Func, goroutine, &job));

    Error err = tls_conn_handshake(srv);
    if (!BURROW_FAILED(err))
        testing_t_errorf_v(t, "expected error from incomplete handshake, got nil");
    else if (!str_eq(error_text(err), expected_err))
        testing_t_errorf_v(t, "expected error %q, got %q", expected_err,
                           error_text(err));

    sync_wait_group_wait(&wg);
    tls_conn_free(srv);
    trailing_job_free(&job);
    local_pipe_free(c);
    tls_config_release(&server_config);
    arena_free(&ar);
}

static void TestClientHelloTrailingMessage(TestingT *t) {
    /* Same as TestServerHelloTrailingMessage but for the client side. */

    test_client_hello_trailing(
        t, client_hello_trailing_client,
        BURROW_S("tls: handshake buffer not empty before setting read traffic secret"));
}

/* Not in Go: the goroutine of TestDoubleClientHelloHRR. */
static void double_client_hello_client(void *env) {
    TrailingJob *j = (TrailingJob *)env;
    Alloc *a = arena_allocator(&j->ar);
    j->tls = tls_client(heap_allocator(), j->conn, &j->config);
    if (j->tls == NULL)
        testing_t_fatal_v(j->t, "out of memory");
    TlsConn *cli = j->tls;

    TlsMadeClientHello hello;
    memset(&hello, 0, sizeof hello);
    Error err = burrow__tls_conn_make_client_hello(cli, a, &hello);
    burrow__tls_key_share_private_keys_free(&hello.key_share_keys);
    if (BURROW_FAILED(err))
        testing_t_fatal_v(j->t, "%v", err);
    hello.msg.client_hello.key_shares = slice_nil(&burrow__tls_key_share_type);

    const TlsHandshakeMsg *msgs[] = {&hello.msg, &hello.msg};
    Slice record = concat_handshake_messages(a, msgs, 2, &err);
    if (BURROW_FAILED(err))
        testing_t_fatal_v(j->t, "%v", err);

    (void)j->conn.vt->writer.write(j->conn.data, record, &err);
    if (BURROW_FAILED(err))
        testing_t_fatal_v(j->t, "%v", err);
    (void)tls_conn_close(cli);
}

static void TestDoubleClientHelloHRR(TestingT *t) {
    /* If a client sends two ClientHello messages in a single record, and the
     * server sends a HRR after reading the first ClientHello, the server must
     * either fail or ignore the trailing ClientHello. */

    test_client_hello_trailing(
        t, double_client_hello_client,
        BURROW_S("tls: handshake buffer not empty before HelloRetryRequest"));
}

/* Not in Go: what the goroutine of TestMultipleKeyUpdate works with. */
typedef struct MultipleKeyUpdateJob {
    TlsConn *client;
    TlsConn *server;
    Chan *client_handshake_done;
} MultipleKeyUpdateJob;

static void multiple_key_update_client(void *env) {
    MultipleKeyUpdateJob *j = (MultipleKeyUpdateJob *)env;
    /* Go checks the error and does nothing with it. */
    (void)tls_conn_handshake(j->client);
    chan_close(j->client_handshake_done);
    Error err;
    (void)io_copy(heap_allocator(), io_discard,
                  net_conn_as_io_reader(tls_conn_as_net_conn(j->server)), &err);
}

/* Not in Go: the steps of a TestMultipleKeyUpdate subtest once the goroutine
 * is started. Errorf and a return stand for Fatalf, so that the goroutine is
 * waited for and everything freed after it. */
static void multiple_key_update_steps(TestingT *t, Alloc *a, MultipleKeyUpdateJob *job,
                                      NetConn c, NetConn s, bool request_update) {
    TlsConn *client = job->client;
    Error err = tls_conn_handshake(job->server);
    if (BURROW_FAILED(err)) {
        testing_t_errorf_v(t, "server handshake failed: %v\n", err);
        /* Not in Go: closing the pipe ends the goroutine. */
        (void)c.vt->closer.close(c.data);
        (void)s.vt->closer.close(s.data);
        return;
    }
    bool done;
    (void)chan_recv(job->client_handshake_done, &done);

    (void)c.vt->set_read_deadline(c.data, time_add(time_now(), 1 * TIME_SECOND));
    (void)s.vt->set_read_deadline(s.data, time_add(time_now(), 1 * TIME_SECOND));

    TlsKeyUpdateMsg ku = {request_update};
    Slice ku_msg = burrow__tls_key_update_msg_marshal(&ku, a, &err);
    if (BURROW_FAILED(err)) {
        testing_t_errorf_v(t, "failed to marshal key update message: %v", err);
        return;
    }
    Slice both = slice_make(a, TYPE_BYTE, 2 * ku_msg.len, 2 * ku_msg.len);
    if (both.p == NULL) {
        testing_t_errorf_v(t, "out of memory");
        return;
    }
    memcpy(both.p, ku_msg.p, (size_t)ku_msg.len);
    memcpy((Byte *)both.p + ku_msg.len, ku_msg.p, (size_t)ku_msg.len);

    sync_mutex_lock(&client->out.mu);
    (void)burrow__tls_conn_write_record_locked(client, TLS_RECORD_TYPE_HANDSHAKE, both,
                                               &err);
    sync_mutex_unlock(&client->out.mu);
    if (BURROW_FAILED(err)) {
        testing_t_errorf_v(t, "failed to write key update messages: %v", err);
        return;
    }

    (void)io_copy(heap_allocator(), io_discard,
                  net_conn_as_io_reader(tls_conn_as_net_conn(client)), &err);
    if (!BURROW_FAILED(err))
        testing_t_errorf_v(
            t, "expected multiple key update messages to cause an error, got nil");
    else if (!strings_has_suffix(error_text(err), BURROW_S("tls: unexpected message")))
        testing_t_errorf_v(t, "unexpected error: %v", err);
}

static void test_multiple_key_update_run(void *env, TestingT *t) {
    bool request_update = *(const bool *)env;
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);

    NetConn c, s;
    local_pipe(a, &c, &s);
    TlsConfig client_config = test_config_client(a);
    client_config.min_version = TLS_VERSION_TLS13;
    client_config.max_version = TLS_VERSION_TLS13;
    TlsConfig server_config = test_config_server(a);
    server_config.min_version = TLS_VERSION_TLS13;
    server_config.max_version = TLS_VERSION_TLS13;
    MultipleKeyUpdateJob job;
    job.client = tls_client(heap_allocator(), c, &client_config);
    job.server = tls_server(heap_allocator(), s, &server_config);
    job.client_handshake_done = chan_make(a, TYPE_BOOL, 0);
    if (job.client == NULL || job.server == NULL || job.client_handshake_done == NULL)
        panic_str(BURROW_S("out of memory"));

    SyncWaitGroup wg = {0};
    sync_wait_group_go(&wg, BURROW_FN(Func, multiple_key_update_client, &job));
    multiple_key_update_steps(t, a, &job, c, s, request_update);
    sync_wait_group_wait(&wg);

    tls_conn_free(job.client);
    tls_conn_free(job.server);
    chan_free(job.client_handshake_done);
    local_pipe_free(c);
    tls_config_release(&client_config);
    tls_config_release(&server_config);
    arena_free(&ar);
}

static void TestMultipleKeyUpdate(TestingT *t) {
    bool request_updates[] = {true, false};
    for (int i = 0; i < 2; i++) {
        bool *request_update = &request_updates[i];
        testing_t_run(
            t,
            *request_update ? BURROW_S("requestUpdate=true")
                            : BURROW_S("requestUpdate=false"),
            BURROW_FN(TestingTFunc, test_multiple_key_update_run, request_update));
    }
}

#define TESTS(X)                                                                       \
    X(TestServerHelloTrailingMessage)                                                  \
    X(TestClientHelloTrailingMessage)                                                  \
    X(TestDoubleClientHelloHRR)                                                        \
    X(TestMultipleKeyUpdate)

TESTING_MAIN(TESTS)
