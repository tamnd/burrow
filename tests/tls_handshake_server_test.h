/* crypto/tls: testClientHelloFailure and testClientHello of
 * handshake_server_test.go, which fips140_test.go runs on too.
 *
 * Copyright 2009 The Go Authors. All rights reserved.
 * Copyright 2026 The burrow Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style licence that can be found
 * in the LICENSE file. */

#ifndef BURROW_TESTS_TLS_HANDSHAKE_SERVER_TEST_H
#define BURROW_TESTS_TLS_HANDSHAKE_SERVER_TEST_H

#include "../src/crypto/tls_internal.h"
#include "burrow/burrow.h"
#include "burrow/crypto/tls.h"
#include "burrow/mem/heap.h"
#include "burrow/strings.h"
#include "tls_handshake_test.h"

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

static inline void client_job(void *env) {
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

static inline void client_job_init(ClientJob *j, NetConn c, TlsConfig *config,
                                   const TlsHandshakeMsg *m, ReplyKind reply_kind) {
    memset(j, 0, sizeof *j);
    j->c = c;
    j->config = config;
    j->m = m;
    j->reply_kind = reply_kind;
    arena_init(&j->ar, NULL, 0);
}

static inline void client_job_free(ClientJob *j) {
    if (j->cli != NULL)
        tls_conn_free(j->cli);
    arena_free(&j->ar);
}

static inline void test_client_hello_failure(TestingT *t, TlsConfig *server_config,
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

static inline void test_client_hello(TestingT *t, TlsConfig *server_config,
                                     const TlsHandshakeMsg *m) {
    test_client_hello_failure(t, server_config, m, "");
}

#endif /* BURROW_TESTS_TLS_HANDSHAKE_SERVER_TEST_H */
