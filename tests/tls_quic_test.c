/* Derived from Go's src/crypto/tls/quic_test.go.
 * Go source: go1.27.1.
 *
 * Go's tests leave what they make to the collector. Here what a test makes for
 * the QUICConns to use is in an arena, given back by a cleanup once the
 * QUICConns are, since testing_t_cleanup runs the cleanups last in, first out.
 *
 * Copyright 2023 The Go Authors. All rights reserved.
 * Copyright 2026 The burrow Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style licence that can be found
 * in the LICENSE file. */

#include "check.h"

#include "../src/crypto/tls_internal.h"
#include "burrow/burrow.h"
#include "burrow/bytes.h"
#include "burrow/context.h"
#include "burrow/crypto/tls.h"
#include "burrow/mem/heap.h"
#include "burrow/net.h"
#include "burrow/strings.h"
#include "burrow/sync.h"
#include "tls_handshake_test.h"

#include <stdint.h>
#include <string.h>

/* ------------------------------------------------------------- not in Go */

static void free_test_arena(void *env) {
    Arena *ar = (Arena *)env;
    arena_free(ar);
    mem_free(heap_allocator(), ar, sizeof *ar, _Alignof(Arena));
}

/* Not in Go: an arena for what a test makes, given back by a cleanup after
 * those of everything made later. */
static Alloc *test_arena(TestingT *t) {
    Arena *ar = BURROW_NEW(heap_allocator(), Arena);
    if (ar == NULL)
        testing_t_fatal_v(t, "out of memory");
    arena_init(ar, NULL, 0);
    testing_t_cleanup(t, BURROW_FN(Func, free_test_arena, ar));
    return arena_allocator(ar);
}

static void release_test_config(void *env) {
    tls_config_release((TlsConfig *)env);
}

/* Not in Go: testConfigServer.Clone() or testConfigClient.Clone(), in a, with
 * a cleanup that gives back what the config comes to hold. */
static TlsConfig *test_config(TestingT *t, Alloc *a, bool server) {
    TlsConfig *c = BURROW_NEW(a, TlsConfig);
    if (c == NULL)
        testing_t_fatal_v(t, "out of memory");
    *c = server ? test_config_server(a) : test_config_client(a);
    testing_t_cleanup(t, BURROW_FN(Func, release_test_config, c));
    return c;
}

/* Not in Go: &QUICConfig{TLSConfig: c}. */
static TlsQUICConfig quic_config(TlsConfig *c) {
    TlsQUICConfig q;
    memset(&q, 0, sizeof q);
    q.tls_config = c;
    return q;
}

static void free_test_cache(void *env) {
    tls_lru_client_session_cache_free(*(TlsClientSessionCache *)env);
}

/* Not in Go: NewLRUClientSessionCache(capacity), with a cleanup that gives it
 * back. */
static TlsClientSessionCache new_lru_cache(TestingT *t, Alloc *a, Int capacity) {
    TlsClientSessionCache *c = BURROW_NEW(a, TlsClientSessionCache);
    if (c == NULL)
        testing_t_fatal_v(t, "out of memory");
    *c = tls_new_lru_client_session_cache(heap_allocator(), capacity);
    if (c->vt == NULL)
        testing_t_fatal_v(t, "out of memory");
    testing_t_cleanup(t, BURROW_FN(Func, free_test_cache, c));
    return *c;
}

/* ---------------------------------------------------------- testQUICConn */

typedef struct SuiteSecret {
    uint16_t suite;
    Slice secret;
} SuiteSecret;

/* Not in Go: map[QUICEncryptionLevel]suiteSecret, with a slot for each of the
 * four levels. */
typedef struct SecretMap {
    SuiteSecret v[4];
    bool ok[4];
} SecretMap;

static bool secret_ok(const SecretMap *m, TlsQUICEncryptionLevel level) {
    return level >= 0 && level < 4 && m->ok[level];
}

static SuiteSecret secret_get(const SecretMap *m, TlsQUICEncryptionLevel level) {
    SuiteSecret none = {0, {NULL, 0, 0, NULL}};
    return secret_ok(m, level) ? m->v[level] : none;
}

/* Not in Go: reflect.DeepEqual on two suiteSecrets. */
static bool suite_secret_equal(SuiteSecret x, SuiteSecret y) {
    return x.suite == y.suite && (x.secret.p == NULL) == (y.secret.p == NULL) &&
           bytes_equal(x.secret, y.secret);
}

typedef struct TestQUICConn TestQUICConn;

struct TestQUICConn {
    TestingT *t;
    TlsQUICConn *conn;
    SecretMap read_secret;
    SecretMap write_secret;
    TlsQUICSessionTicketOptions ticket_opts;
    void (*on_resume_session)(TlsSessionState *);
    Slice got_params;
    Error got_error;
    bool early_data_rejected;
    bool complete;

    /* Not in Go: what the copies of the secrets and parameters are made in. */
    Arena ar;
};

static void close_test_quic_conn(void *env) {
    TestQUICConn *q = (TestQUICConn *)env;
    tls_quic_conn_free(q->conn);
    arena_free(&q->ar);
    mem_free(heap_allocator(), q, sizeof *q, _Alignof(TestQUICConn));
}

/* Not in Go: what newTestQUICClient and newTestQUICServer share. The cleanup
 * closes the QUICConn as Go's does, and gives everything back too. */
static TestQUICConn *new_test_quic_conn(TestingT *t, const TlsQUICConfig *config,
                                        bool server) {
    TestQUICConn *q = BURROW_NEW(heap_allocator(), TestQUICConn);
    if (q == NULL)
        testing_t_fatal_v(t, "out of memory");
    memset(q, 0, sizeof *q);
    q->t = t;
    q->conn = server ? tls_quic_server(heap_allocator(), config)
                     : tls_quic_client(heap_allocator(), config);
    if (q->conn == NULL) {
        mem_free(heap_allocator(), q, sizeof *q, _Alignof(TestQUICConn));
        testing_t_fatal_v(t, "out of memory");
    }
    q->got_params = slice_nil(TYPE_BYTE);
    arena_init(&q->ar, NULL, 0);
    testing_t_cleanup(t, BURROW_FN(Func, close_test_quic_conn, q));
    return q;
}

static TestQUICConn *new_test_quic_client(TestingT *t, const TlsQUICConfig *config) {
    return new_test_quic_conn(t, config, false);
}

static TestQUICConn *new_test_quic_server(TestingT *t, const TlsQUICConfig *config) {
    return new_test_quic_conn(t, config, true);
}

/* Not in Go: a copy of b in q's arena, since what an event points at does not
 * outlive the next NextEvent. */
static Slice keep_bytes(TestQUICConn *q, Slice b) {
    Slice c = bytes_clone(arena_allocator(&q->ar), b);
    if (c.len != b.len)
        testing_t_error_v(q->t, "out of memory");
    return c;
}

static void set_read_secret(TestQUICConn *q, TlsQUICEncryptionLevel level,
                            uint16_t suite, Slice secret) {
    Str l = tls_quic_encryption_level_string(level, arena_allocator(&q->ar));
    if (!secret_ok(&q->write_secret, level) && level != TLS_QUIC_ENCRYPTION_LEVEL_EARLY)
        testing_t_errorf_v(
            q->t, "SetReadSecret for level %v called before SetWriteSecret", l);
    if (level == TLS_QUIC_ENCRYPTION_LEVEL_APPLICATION && !q->complete)
        testing_t_errorf_v(
            q->t, "SetReadSecret for level %v called before HandshakeComplete", l);
    if (secret_ok(&q->read_secret, level))
        testing_t_errorf_v(q->t, "SetReadSecret for level %v called twice", l);
    switch (level) {
    case TLS_QUIC_ENCRYPTION_LEVEL_HANDSHAKE:
    case TLS_QUIC_ENCRYPTION_LEVEL_EARLY:
    case TLS_QUIC_ENCRYPTION_LEVEL_APPLICATION:
        q->read_secret.v[level] = (SuiteSecret){suite, keep_bytes(q, secret)};
        q->read_secret.ok[level] = true;
        break;
    default:
        testing_t_errorf_v(q->t, "SetReadSecret for unexpected level %v", l);
    }
}

static void set_write_secret(TestQUICConn *q, TlsQUICEncryptionLevel level,
                             uint16_t suite, Slice secret) {
    Str l = tls_quic_encryption_level_string(level, arena_allocator(&q->ar));
    if (secret_ok(&q->write_secret, level))
        testing_t_errorf_v(q->t, "SetWriteSecret for level %v called twice", l);
    switch (level) {
    case TLS_QUIC_ENCRYPTION_LEVEL_HANDSHAKE:
    case TLS_QUIC_ENCRYPTION_LEVEL_EARLY:
    case TLS_QUIC_ENCRYPTION_LEVEL_APPLICATION:
        q->write_secret.v[level] = (SuiteSecret){suite, keep_bytes(q, secret)};
        q->write_secret.ok[level] = true;
        break;
    default:
        testing_t_errorf_v(q->t, "SetWriteSecret for unexpected level %v", l);
    }
}

BURROW_SENTINEL_ERROR(err_transport_parameters_required,
                      "transport parameters required");

/* What []byte{} points at. */
static const Byte empty_bytes[1] = {0};

BURROW_FUNC(OnEventFunc, bool, TlsQUICEvent e, TestQUICConn *src, TestQUICConn *dst);

static Error run_test_quic_connection(Context ctx, TestQUICConn *cli, TestQUICConn *srv,
                                      OnEventFunc on_event) {
    TestQUICConn *a = cli, *b = srv;
    TestQUICConn *both[2] = {a, b};
    for (int i = 0; i < 2; i++) {
        TestQUICConn *c = both[i];
        if (!burrow__tls_quic_conn_conn(c->conn)->quic->started) {
            Error err = tls_quic_conn_start(c->conn, ctx);
            if (BURROW_FAILED(err))
                return err;
        }
    }
    Int idle_count = 0;
    for (;;) {
        TlsQUICEvent e = tls_quic_conn_next_event(a->conn);
        if (!BURROW_FUNC_IS_NIL(on_event) && BURROW_CALLF(on_event, e, a, b))
            continue;
        if (BURROW_FAILED(a->got_error) && e.kind != TLS_QUIC_NO_EVENT)
            return fmt_errorf_v("unexpected event %v after QUICErrorEvent", e.kind);
        switch (e.kind) {
        case TLS_QUIC_NO_EVENT:
            idle_count++;
            if (idle_count == 2) {
                if (!a->complete || !b->complete)
                    return fmt_errorf_v("handshake incomplete");
                return BURROW_NO_ERROR;
            }
            {
                TestQUICConn *x = a;
                a = b;
                b = x;
            }
            break;
        case TLS_QUIC_SET_READ_SECRET:
            set_read_secret(a, e.level, e.suite, e.data);
            break;
        case TLS_QUIC_SET_WRITE_SECRET:
            set_write_secret(a, e.level, e.suite, e.data);
            break;
        case TLS_QUIC_WRITE_DATA: {
            Error err = tls_quic_conn_handle_data(b->conn, e.level, e.data);
            if (BURROW_FAILED(err))
                return err;
            break;
        }
        case TLS_QUIC_TRANSPORT_PARAMETERS:
            a->got_params = keep_bytes(a, e.data);
            if (a->got_params.p == NULL)
                a->got_params =
                    slice_from((Byte *)(uintptr_t)empty_bytes, 0, 0, TYPE_BYTE);
            break;
        case TLS_QUIC_TRANSPORT_PARAMETERS_REQUIRED:
            return err_transport_parameters_required;
        case TLS_QUIC_HANDSHAKE_DONE:
            a->complete = true;
            if (a == srv) {
                Error err =
                    tls_quic_conn_send_session_ticket(srv->conn, srv->ticket_opts);
                if (BURROW_FAILED(err))
                    return err;
            }
            break;
        case TLS_QUIC_STORE_SESSION:
            if (a != cli)
                return fmt_errorf_v(
                    "unexpected QUICStoreSession event received by server");
            (void)tls_quic_conn_store_session(a->conn, e.session_state);
            break;
        case TLS_QUIC_RESUME_SESSION:
            if (a->on_resume_session != NULL)
                a->on_resume_session(e.session_state);
            break;
        case TLS_QUIC_REJECTED_EARLY_DATA:
            a->early_data_rejected = true;
            break;
        case TLS_QUIC_ERROR_EVENT:
            if (BURROW_OK(e.err))
                return fmt_errorf_v("unexpected QUICErrorEvent with no Err");
            a->got_error = e.err;
            break;
        default:
            break;
        }
        if (e.kind != TLS_QUIC_NO_EVENT)
            idle_count = 0;
    }
}

static const OnEventFunc no_on_event = {NULL, NULL};

/* Not in Go: a QUICConfig with MinVersion set to TLS 1.3, which nearly every
 * test starts with. */
static TlsQUICConfig tls13_config(TestingT *t, Alloc *a, bool server) {
    TlsQUICConfig c = quic_config(test_config(t, a, server));
    c.tls_config->min_version = TLS_VERSION_TLS13;
    return c;
}

/* Not in Go: []byte(s), which s outlives here. */
static Slice str_bytes(Str s) {
    return slice_from((Byte *)(uintptr_t)s.p, s.len, s.len, TYPE_BYTE);
}

/* ----------------------------------------------------------------- tests */

static void TestQUICConnection(TestingT *t) {
    Alloc *a = test_arena(t);
    TlsQUICConfig client_config = tls13_config(t, a, false);
    TlsQUICConfig server_config = tls13_config(t, a, true);

    TestQUICConn *cli = new_test_quic_client(t, &client_config);
    tls_quic_conn_set_transport_parameters(cli->conn, slice_nil(TYPE_BYTE));

    TestQUICConn *srv = new_test_quic_server(t, &server_config);
    tls_quic_conn_set_transport_parameters(srv->conn, slice_nil(TYPE_BYTE));

    Error err = run_test_quic_connection(context_background(), cli, srv, no_on_event);
    if (BURROW_FAILED(err))
        testing_t_fatalf_v(t, "error during connection handshake: %v", err);

    if (!secret_ok(&cli->read_secret, TLS_QUIC_ENCRYPTION_LEVEL_HANDSHAKE))
        testing_t_errorf_v(t, "client has no Handshake secret");
    if (!secret_ok(&cli->read_secret, TLS_QUIC_ENCRYPTION_LEVEL_APPLICATION))
        testing_t_errorf_v(t, "client has no Application secret");
    if (!secret_ok(&srv->read_secret, TLS_QUIC_ENCRYPTION_LEVEL_HANDSHAKE))
        testing_t_errorf_v(t, "server has no Handshake secret");
    if (!secret_ok(&srv->read_secret, TLS_QUIC_ENCRYPTION_LEVEL_APPLICATION))
        testing_t_errorf_v(t, "server has no Application secret");
    static const TlsQUICEncryptionLevel levels[2] = {
        TLS_QUIC_ENCRYPTION_LEVEL_HANDSHAKE, TLS_QUIC_ENCRYPTION_LEVEL_APPLICATION};
    for (int i = 0; i < 2; i++) {
        TlsQUICEncryptionLevel level = levels[i];
        Str l = tls_quic_encryption_level_string(level, a);
        if (!secret_ok(&cli->read_secret, level))
            testing_t_errorf_v(t, "client has no %v read secret", l);
        if (!secret_ok(&srv->read_secret, level))
            testing_t_errorf_v(t, "server has no %v read secret", l);
        if (!suite_secret_equal(secret_get(&cli->read_secret, level),
                                secret_get(&srv->write_secret, level)))
            testing_t_errorf_v(
                t, "client read secret does not match server write secret for level %v",
                l);
        if (!suite_secret_equal(secret_get(&cli->write_secret, level),
                                secret_get(&srv->read_secret, level)))
            testing_t_errorf_v(
                t, "client write secret does not match server read secret for level %v",
                l);
    }
}

typedef struct QUICVersionsCase {
    const char *name;
    uint16_t client_min;
    uint16_t client_max;
    uint16_t server_min;
    uint16_t server_max;
    bool want_err;
} QUICVersionsCase;

static void quic_versions(void *env, TestingT *t) {
    const QUICVersionsCase *tc = (const QUICVersionsCase *)env;
    Alloc *a = test_arena(t);
    TlsConfig *client = test_config(t, a, false);
    client->min_version = tc->client_min;
    client->max_version = tc->client_max;
    TlsConfig *server = test_config(t, a, true);
    server->min_version = tc->server_min;
    server->max_version = tc->server_max;

    TlsQUICConfig client_config = quic_config(client);
    TestQUICConn *cli = new_test_quic_client(t, &client_config);
    tls_quic_conn_set_transport_parameters(cli->conn, slice_nil(TYPE_BYTE));
    TlsQUICConfig server_config = quic_config(server);
    TestQUICConn *srv = new_test_quic_server(t, &server_config);
    tls_quic_conn_set_transport_parameters(srv->conn, slice_nil(TYPE_BYTE));
    Error err = run_test_quic_connection(context_background(), cli, srv, no_on_event);
    if (tc->want_err == BURROW_OK(err))
        testing_t_errorf_v(t, "got err=%v, wantErr=%v", err, tc->want_err);
}

static void TestQUICVersions(TestingT *t) {
    static const QUICVersionsCase cases[] = {
        {"defaults", 0, 0, 0, 0, false},
        {"MinVersion TLS 1.2", TLS_VERSION_TLS12, 0, TLS_VERSION_TLS12, 0, false},
        {"MinVersion TLS 1.3", TLS_VERSION_TLS13, 0, TLS_VERSION_TLS13, 0, false},
        {"client MaxVersion TLS 1.2", 0, TLS_VERSION_TLS12, 0, 0, true},
        {"server MaxVersion TLS 1.2", 0, 0, 0, TLS_VERSION_TLS12, true},
    };
    for (size_t i = 0; i < sizeof cases / sizeof cases[0]; i++)
        testing_t_run(
            t, str_from_cstr(cases[i].name),
            BURROW_FN(TestingTFunc, quic_versions, (void *)(uintptr_t)&cases[i]));
}

static void TestQUICSessionResumption(TestingT *t) {
    Alloc *a = test_arena(t);
    TlsQUICConfig client_config = tls13_config(t, a, false);
    client_config.tls_config->client_session_cache = new_lru_cache(t, a, 1);

    TlsQUICConfig server_config = tls13_config(t, a, true);

    TestQUICConn *cli = new_test_quic_client(t, &client_config);
    tls_quic_conn_set_transport_parameters(cli->conn, slice_nil(TYPE_BYTE));
    TestQUICConn *srv = new_test_quic_server(t, &server_config);
    tls_quic_conn_set_transport_parameters(srv->conn, slice_nil(TYPE_BYTE));
    Error err = run_test_quic_connection(context_background(), cli, srv, no_on_event);
    if (BURROW_FAILED(err))
        testing_t_fatalf_v(t, "error during first connection handshake: %v", err);
    if (tls_quic_conn_connection_state(cli->conn).did_resume)
        testing_t_errorf_v(t, "first connection unexpectedly used session resumption");

    TestQUICConn *cli2 = new_test_quic_client(t, &client_config);
    tls_quic_conn_set_transport_parameters(cli2->conn, slice_nil(TYPE_BYTE));
    TestQUICConn *srv2 = new_test_quic_server(t, &server_config);
    tls_quic_conn_set_transport_parameters(srv2->conn, slice_nil(TYPE_BYTE));
    err = run_test_quic_connection(context_background(), cli2, srv2, no_on_event);
    if (BURROW_FAILED(err))
        testing_t_fatalf_v(t, "error during second connection handshake: %v", err);
    if (!tls_quic_conn_connection_state(cli2->conn).did_resume)
        testing_t_errorf_v(t, "second connection did not use session resumption");

    client_config.tls_config->session_tickets_disabled = true;
    TestQUICConn *cli3 = new_test_quic_client(t, &client_config);
    tls_quic_conn_set_transport_parameters(cli3->conn, slice_nil(TYPE_BYTE));
    TestQUICConn *srv3 = new_test_quic_server(t, &server_config);
    tls_quic_conn_set_transport_parameters(srv3->conn, slice_nil(TYPE_BYTE));
    err = run_test_quic_connection(context_background(), cli3, srv3, no_on_event);
    if (BURROW_FAILED(err))
        testing_t_fatalf_v(t, "error during third connection handshake: %v", err);
    if (tls_quic_conn_connection_state(cli3->conn).did_resume)
        testing_t_errorf_v(t, "third connection unexpectedly used session resumption");
}

static bool fragmentary_data(void *env, TlsQUICEvent e, TestQUICConn *src,
                             TestQUICConn *dst) {
    TestingT *t = (TestingT *)env;
    (void)src;
    if (e.kind == TLS_QUIC_WRITE_DATA) {
        /* Provide the data one byte at a time. */
        for (Int i = 0; i < e.data.len; i++) {
            Error err = tls_quic_conn_handle_data(dst->conn, e.level,
                                                  slice_sub(e.data, i, i + 1));
            if (BURROW_FAILED(err)) {
                testing_t_errorf_v(t, "HandleData: %v", err);
                break;
            }
        }
        return true;
    }
    return false;
}

static void TestQUICFragmentaryData(TestingT *t) {
    Alloc *a = test_arena(t);
    TlsQUICConfig client_config = tls13_config(t, a, false);
    client_config.tls_config->client_session_cache = new_lru_cache(t, a, 1);

    TlsQUICConfig server_config = tls13_config(t, a, true);

    TestQUICConn *cli = new_test_quic_client(t, &client_config);
    tls_quic_conn_set_transport_parameters(cli->conn, slice_nil(TYPE_BYTE));
    TestQUICConn *srv = new_test_quic_server(t, &server_config);
    tls_quic_conn_set_transport_parameters(srv->conn, slice_nil(TYPE_BYTE));
    Error err = run_test_quic_connection(context_background(), cli, srv,
                                         BURROW_FN(OnEventFunc, fragmentary_data, t));
    if (BURROW_FAILED(err))
        testing_t_fatalf_v(t, "error during first connection handshake: %v", err);
}

/* Not in Go: the handshake every post-handshake test starts with. */
static void connect_tls13(TestingT *t, TestQUICConn **cli, TestQUICConn **srv) {
    Alloc *a = test_arena(t);
    TlsQUICConfig client_config = tls13_config(t, a, false);
    TlsQUICConfig server_config = tls13_config(t, a, true);
    *cli = new_test_quic_client(t, &client_config);
    tls_quic_conn_set_transport_parameters((*cli)->conn, slice_nil(TYPE_BYTE));
    *srv = new_test_quic_server(t, &server_config);
    tls_quic_conn_set_transport_parameters((*srv)->conn, slice_nil(TYPE_BYTE));
    Error err = run_test_quic_connection(context_background(), *cli, *srv, no_on_event);
    if (BURROW_FAILED(err))
        testing_t_fatalf_v(t, "error during connection handshake: %v", err);
}

static void TestQUICPostHandshakeClientAuthentication(TestingT *t) {
    /* RFC 9001, Section 4.4. */
    TestQUICConn *cli, *srv;
    connect_tls13(t, &cli, &srv);
    Alloc *a = test_arena(t);

    TlsCertificateRequestMsgTLS13 cert_req =
        burrow__tls_certificate_request_msg_tls13_zero();
    cert_req.ocsp_stapling = true;
    cert_req.scts = true;
    cert_req.supported_signature_algorithms =
        burrow__tls_supported_signature_algorithms(a, TLS_VERSION_TLS13,
                                                   TLS_VERSION_TLS13);
    Error err = BURROW_NO_ERROR;
    Slice cert_req_bytes =
        burrow__tls_certificate_request_msg_tls13_marshal(&cert_req, a, &err);
    if (BURROW_FAILED(err))
        testing_t_fatal_v(t, err);
    Byte head[4] = {TLS_TYPE_CERTIFICATE_REQUEST, 0, 0, (Byte)cert_req_bytes.len};
    Slice msg = slice_append(a, slice_nil(TYPE_BYTE), head, 4);
    msg = slice_append(a, msg, cert_req_bytes.p, cert_req_bytes.len);
    if (msg.len != 4 + cert_req_bytes.len)
        testing_t_fatal_v(t, "out of memory");
    err = tls_quic_conn_handle_data(cli->conn, TLS_QUIC_ENCRYPTION_LEVEL_APPLICATION,
                                    msg);
    if (BURROW_OK(err))
        testing_t_fatalf_v(
            t, "post-handshake authentication request: got no error, want one");
}

static void TestQUICPostHandshakeKeyUpdate(TestingT *t) {
    /* RFC 9001, Section 6. */
    TestQUICConn *cli, *srv;
    connect_tls13(t, &cli, &srv);
    Alloc *a = test_arena(t);

    TlsKeyUpdateMsg key_update = {false};
    Error err = BURROW_NO_ERROR;
    Slice key_update_bytes = burrow__tls_key_update_msg_marshal(&key_update, a, &err);
    if (BURROW_FAILED(err))
        testing_t_fatal_v(t, err);
    Str expected_err = BURROW_S("unexpected key update message");
    err = tls_quic_conn_handle_data(cli->conn, TLS_QUIC_ENCRYPTION_LEVEL_APPLICATION,
                                    key_update_bytes);
    if (BURROW_OK(err))
        testing_t_fatalf_v(t, "key update request: expected error from post-handshake "
                              "key update, got nil");
    else if (!strings_contains(error_text(err), expected_err))
        testing_t_fatalf_v(t, "key update request: got error %v, expected substring %q",
                           err, expected_err);
}

static void TestQUICPostHandshakeMessageTooLarge(TestingT *t) {
    TestQUICConn *cli, *srv;
    connect_tls13(t, &cli, &srv);

    Int size = TLS_MAX_HANDSHAKE + 1;
    Byte msg[4] = {TLS_TYPE_NEW_SESSION_TICKET, (Byte)(size >> 16), (Byte)(size >> 8),
                   (Byte)size};
    Error err =
        tls_quic_conn_handle_data(cli->conn, TLS_QUIC_ENCRYPTION_LEVEL_APPLICATION,
                                  slice_from(msg, 4, 4, TYPE_BYTE));
    if (BURROW_OK(err))
        testing_t_fatalf_v(t, "%v-byte post-handshake message: got no error, want one",
                           size);
}

/* Not in Go: ev.Err != err, Go's != on two errors. */
static bool same_error(Error x, Error y) {
    return x.vt == y.vt && x.data == y.data;
}

static void TestQUICHandshakeError(TestingT *t) {
    Alloc *a = test_arena(t);
    TlsQUICConfig client_config = tls13_config(t, a, false);
    client_config.tls_config->insecure_skip_verify = false;
    client_config.tls_config->server_name = BURROW_S("name");

    TlsQUICConfig server_config = tls13_config(t, a, true);

    TestQUICConn *cli = new_test_quic_client(t, &client_config);
    tls_quic_conn_set_transport_parameters(cli->conn, slice_nil(TYPE_BYTE));
    TestQUICConn *srv = new_test_quic_server(t, &server_config);
    tls_quic_conn_set_transport_parameters(srv->conn, slice_nil(TYPE_BYTE));
    Error err = run_test_quic_connection(context_background(), cli, srv, no_on_event);
    Error bad_certificate = tls_alert_error_as_error(TLS_ALERT_BAD_CERTIFICATE, a);
    if (!errors_is(err, bad_certificate))
        testing_t_errorf_v(
            t,
            "connection handshake terminated with error %q, want alertBadCertificate",
            err);
    if (errors_as(err, TYPE_TLS_CERTIFICATE_VERIFICATION_ERROR) == NULL)
        testing_t_errorf_v(t,
                           "connection handshake terminated with error %q, want "
                           "CertificateVerificationError",
                           err);

    TlsQUICEvent ev = tls_quic_conn_next_event(cli->conn);
    if (ev.kind != TLS_QUIC_ERROR_EVENT)
        testing_t_errorf_v(t, "client.NextEvent: no QUICErrorEvent, want one");
    if (!same_error(ev.err, err))
        testing_t_errorf_v(
            t, "client.NextEvent: want same error returned by Start, got %v", ev.err);
}

BURROW_SENTINEL_ERROR(get_ech_keys_error,
                      "error returned by GetEncryptedClientHelloKeys");

static Slice get_ech_keys(void *env, TlsClientHelloInfo *chi, Error *err) {
    (void)env;
    (void)chi;
    BURROW_OUT(err, get_ech_keys_error);
    return slice_nil(TYPE_TLS_ENCRYPTED_CLIENT_HELLO_KEY);
}

/* Not in Go: []string{"h3"}. */
static Slice h3(void) {
    static const Str protos[1] = {BURROW_S_INIT("h3")};
    return slice_from((Str *)(uintptr_t)protos, 1, 1, TYPE_STRING);
}

/* Test that we can report an error produced by the GetEncryptedClientHelloKeys
 * function. */
static void TestQUICECHKeyError(TestingT *t) {
    Alloc *a = test_arena(t);
    TlsQUICConfig client_config = tls13_config(t, a, false);
    client_config.tls_config->next_protos = h3();
    TlsQUICConfig server_config = tls13_config(t, a, true);
    server_config.tls_config->next_protos = h3();
    server_config.tls_config->get_encrypted_client_hello_keys =
        BURROW_FN(TlsGetEncryptedClientHelloKeysFunc, get_ech_keys, NULL);
    TestQUICConn *cli = new_test_quic_client(t, &client_config);
    tls_quic_conn_set_transport_parameters(cli->conn, slice_nil(TYPE_BYTE));
    TestQUICConn *srv = new_test_quic_server(t, &server_config);

    Error err = run_test_quic_connection(context_background(), cli, srv, no_on_event);
    if (!same_error(err, err_transport_parameters_required))
        testing_t_fatalf_v(t,
                           "handshake with no client parameters: %v; want "
                           "errTransportParametersRequired",
                           err);
    tls_quic_conn_set_transport_parameters(srv->conn, slice_nil(TYPE_BYTE));
    err = run_test_quic_connection(context_background(), cli, srv, no_on_event);
    if (BURROW_OK(err))
        testing_t_fatalf_v(t, "handshake with GetEncryptedClientHelloKeys errors: nil, "
                              "want error");
    if (BURROW_OK(srv->got_error))
        testing_t_fatalf_v(t, "after GetEncryptedClientHelloKeys error, server did not "
                              "see QUICErrorEvent");
    if (errors_as(srv->got_error, TYPE_TLS_ALERT_ERROR) == NULL)
        testing_t_errorf_v(
            t, "connection handshake terminated with error %T, want AlertError",
            srv->got_error);
    if (!errors_is(srv->got_error, get_ech_keys_error))
        testing_t_errorf_v(t,
                           "connection handshake terminated with error %v, want error "
                           "returned by GetEncryptedClientHelloKeys",
                           srv->got_error);
}

typedef struct ConnectionStateEnv {
    TestingT *t;
    TestQUICConn *cli;
    TestQUICConn *srv;
} ConnectionStateEnv;

static bool check_connection_state(void *env, TlsQUICEvent e, TestQUICConn *src,
                                   TestQUICConn *dst) {
    const ConnectionStateEnv *x = (const ConnectionStateEnv *)env;
    (void)e;
    (void)src;
    (void)dst;
    TlsConnectionState cli_cs = tls_quic_conn_connection_state(x->cli->conn);
    if (secret_ok(&x->cli->read_secret, TLS_QUIC_ENCRYPTION_LEVEL_APPLICATION)) {
        Str want = cli_cs.negotiated_protocol, got = BURROW_S("h3");
        if (!str_eq(want, got))
            testing_t_errorf_v(x->t,
                               "cli.ConnectionState().NegotiatedProtocol = %q, want %q",
                               want, got);
    }
    TlsConnectionState srv_cs = tls_quic_conn_connection_state(x->srv->conn);
    if (secret_ok(&x->srv->read_secret, TLS_QUIC_ENCRYPTION_LEVEL_HANDSHAKE)) {
        Str want = srv_cs.negotiated_protocol, got = BURROW_S("h3");
        if (!str_eq(want, got))
            testing_t_errorf_v(x->t,
                               "srv.ConnectionState().NegotiatedProtocol = %q, want %q",
                               want, got);
    }
    return false;
}

/* Test that QUICConn.ConnectionState can be used during the handshake, and that
 * it reports the application protocol as soon as it has been negotiated. */
static void TestQUICConnectionState(TestingT *t) {
    Alloc *a = test_arena(t);
    TlsQUICConfig client_config = tls13_config(t, a, false);
    client_config.tls_config->next_protos = h3();
    TlsQUICConfig server_config = tls13_config(t, a, true);
    server_config.tls_config->next_protos = h3();
    TestQUICConn *cli = new_test_quic_client(t, &client_config);
    tls_quic_conn_set_transport_parameters(cli->conn, slice_nil(TYPE_BYTE));
    TestQUICConn *srv = new_test_quic_server(t, &server_config);
    tls_quic_conn_set_transport_parameters(srv->conn, slice_nil(TYPE_BYTE));
    ConnectionStateEnv env = {t, cli, srv};
    Error err =
        run_test_quic_connection(context_background(), cli, srv,
                                 BURROW_FN(OnEventFunc, check_connection_state, &env));
    if (BURROW_FAILED(err))
        testing_t_fatalf_v(t, "error during connection handshake: %v", err);
}

static const Str context_key = BURROW_S_INIT("key");
static const Str context_value_want = BURROW_S_INIT("value");

typedef struct ContextPropagationEnv {
    TestingT *t;
    Int calls;
} ContextPropagationEnv;

static TlsConfig *check_context_value(void *env, TlsClientHelloInfo *info, Error *err) {
    ContextPropagationEnv *x = (ContextPropagationEnv *)env;
    (void)err;
    x->calls++;
    Any key = BURROW_ANY(TYPE_STRING, (void *)(uintptr_t)&context_key);
    Any v = context_value(tls_client_hello_info_context(info), key);
    Str got = BURROW_STR_EMPTY;
    if (v.t == TYPE_STRING)
        got = *(const Str *)v.data;
    if (!str_eq(got, context_value_want))
        testing_t_errorf_v(x->t,
                           "GetConfigForClient context key %q has value %q, want %q",
                           context_key, got, context_value_want);
    return NULL;
}

static void TestQUICStartContextPropagation(TestingT *t) {
    Alloc *a = test_arena(t);
    Any key = BURROW_ANY(TYPE_STRING, (void *)(uintptr_t)&context_key);
    Any value = BURROW_ANY(TYPE_STRING, (void *)(uintptr_t)&context_value_want);
    Context ctx = context_with_value(a, context_background(), key, value);
    if (BURROW_CONTEXT_IS_NIL(ctx))
        testing_t_fatal_v(t, "out of memory");
    TlsQUICConfig client_config = tls13_config(t, a, false);
    TlsQUICConfig server_config = tls13_config(t, a, true);
    ContextPropagationEnv env = {t, 0};
    server_config.tls_config->get_config_for_client =
        BURROW_FN(TlsGetConfigForClientFunc, check_context_value, &env);
    TestQUICConn *cli = new_test_quic_client(t, &client_config);
    tls_quic_conn_set_transport_parameters(cli->conn, slice_nil(TYPE_BYTE));
    TestQUICConn *srv = new_test_quic_server(t, &server_config);
    tls_quic_conn_set_transport_parameters(srv->conn, slice_nil(TYPE_BYTE));
    Error err = run_test_quic_connection(ctx, cli, srv, no_on_event);
    if (BURROW_FAILED(err))
        testing_t_fatalf_v(t, "error during connection handshake: %v", err);
    if (env.calls != 1)
        testing_t_errorf_v(t, "GetConfigForClient called %v times, want 1", env.calls);
}

static void close_pipe(void *env) {
    NetConn *ends = (NetConn *)env;
    (void)ends[0].vt->closer.close(ends[0].data);
    (void)ends[1].vt->closer.close(ends[1].data);
    net_pipe_free(ends[0]);
}

typedef struct HelloInfoConnEnv {
    TestingT *t;
    NetConn want;
    bool called;
} HelloInfoConnEnv;

static TlsConfig *check_hello_info_conn(void *env, TlsClientHelloInfo *info,
                                        Error *err) {
    HelloInfoConnEnv *x = (HelloInfoConnEnv *)env;
    (void)err;
    x->called = true;
    if (info->conn.vt != x->want.vt || info->conn.data != x->want.data) {
        /* Not in Go: the ends are printed as the pointers they hold. */
        UnsafePointer got = info->conn.data, want = x->want.data;
        Any g = BURROW_ANY(TYPE_UNSAFE_POINTER, &got);
        Any w = BURROW_ANY(TYPE_UNSAFE_POINTER, &want);
        testing_t_errorf_v(x->t, "ClientHelloInfo.Conn = %v, want %v", g, w);
    }
    return NULL;
}

static void TestQUICClientHelloInfoConn(TestingT *t) {
    Alloc *a = test_arena(t);
    NetConn *ends = BURROW_NEW_N(a, NetConn, 2);
    if (ends == NULL)
        testing_t_fatal_v(t, "out of memory");
    net_pipe(a, &ends[0], &ends[1]);
    if (ends[0].data == NULL)
        testing_t_fatal_v(t, "out of memory");
    NetConn client_hello_info_conn = ends[0];
    testing_t_cleanup(t, BURROW_FN(Func, close_pipe, ends));
    TlsQUICConfig client_config = tls13_config(t, a, false);
    TlsQUICConfig server_config = tls13_config(t, a, true);
    server_config.client_hello_info_conn = client_hello_info_conn;
    HelloInfoConnEnv env = {t, client_hello_info_conn, false};
    server_config.tls_config->get_config_for_client =
        BURROW_FN(TlsGetConfigForClientFunc, check_hello_info_conn, &env);
    TestQUICConn *cli = new_test_quic_client(t, &client_config);
    tls_quic_conn_set_transport_parameters(cli->conn, slice_nil(TYPE_BYTE));
    TestQUICConn *srv = new_test_quic_server(t, &server_config);
    tls_quic_conn_set_transport_parameters(srv->conn, slice_nil(TYPE_BYTE));
    Error err = run_test_quic_connection(context_background(), cli, srv, no_on_event);
    if (BURROW_FAILED(err))
        testing_t_fatalf_v(t, "error during connection handshake: %v", err);
    if (!env.called)
        testing_t_fatal_v(t, "GetConfigForClient was not called");
}

typedef struct CancelationEnv {
    Context ctx;
    TestQUICConn *cli;
    TestQUICConn *srv;
} CancelationEnv;

static void run_canceled(void *env) {
    CancelationEnv *x = (CancelationEnv *)env;
    (void)run_test_quic_connection(x->ctx, x->cli, x->srv, no_on_event);
}

static void TestQUICContextCancelation(TestingT *t) {
    Alloc *a = test_arena(t);
    ContextCancelFunc cancel;
    Context ctx = context_with_cancel(a, context_background(), &cancel);
    if (BURROW_CONTEXT_IS_NIL(ctx))
        testing_t_fatal_v(t, "out of memory");
    TlsQUICConfig client_config = tls13_config(t, a, false);
    TlsQUICConfig server_config = tls13_config(t, a, true);
    TestQUICConn *cli = new_test_quic_client(t, &client_config);
    tls_quic_conn_set_transport_parameters(cli->conn, slice_nil(TYPE_BYTE));
    TestQUICConn *srv = new_test_quic_server(t, &server_config);
    tls_quic_conn_set_transport_parameters(srv->conn, slice_nil(TYPE_BYTE));
    /* Verify that canceling the connection context concurrently does not cause
     * any races. See https://go.dev/issue/77274. */
    SyncWaitGroup wg = {0};
    CancelationEnv env = {ctx, cli, srv};
    if (!sync_wait_group_go(&wg, BURROW_FN(Func, run_canceled, &env)))
        testing_t_fatal_v(t, "out of memory");
    if (!sync_wait_group_go(&wg, cancel)) {
        sync_wait_group_wait(&wg);
        testing_t_fatal_v(t, "out of memory");
    }
    sync_wait_group_wait(&wg);
}

static void TestQUICDelayedTransportParameters(TestingT *t) {
    Alloc *a = test_arena(t);
    TlsQUICConfig client_config = tls13_config(t, a, false);
    client_config.tls_config->client_session_cache = new_lru_cache(t, a, 1);

    TlsQUICConfig server_config = tls13_config(t, a, true);

    Str cli_params = BURROW_S("client params");
    Str srv_params = BURROW_S("server params");

    TestQUICConn *cli = new_test_quic_client(t, &client_config);
    TestQUICConn *srv = new_test_quic_server(t, &server_config);
    Error err = run_test_quic_connection(context_background(), cli, srv, no_on_event);
    if (!same_error(err, err_transport_parameters_required))
        testing_t_fatalf_v(t,
                           "handshake with no client parameters: %v; want "
                           "errTransportParametersRequired",
                           err);
    tls_quic_conn_set_transport_parameters(cli->conn, str_bytes(cli_params));
    err = run_test_quic_connection(context_background(), cli, srv, no_on_event);
    if (!same_error(err, err_transport_parameters_required))
        testing_t_fatalf_v(t,
                           "handshake with no server parameters: %v; want "
                           "errTransportParametersRequired",
                           err);
    tls_quic_conn_set_transport_parameters(srv->conn, str_bytes(srv_params));
    err = run_test_quic_connection(context_background(), cli, srv, no_on_event);
    if (BURROW_FAILED(err))
        testing_t_fatalf_v(t, "error during connection handshake: %v", err);

    Str got = str_from_bytes(cli->got_params.p, cli->got_params.len), want = srv_params;
    if (!str_eq(got, want))
        testing_t_errorf_v(t, "client got transport params: %q, want %q", got, want);
    got = str_from_bytes(srv->got_params.p, srv->got_params.len);
    want = cli_params;
    if (!str_eq(got, want))
        testing_t_errorf_v(t, "server got transport params: %q, want %q", got, want);
}

static void TestQUICEmptyTransportParameters(TestingT *t) {
    Alloc *a = test_arena(t);
    TlsQUICConfig client_config = tls13_config(t, a, false);
    TlsQUICConfig server_config = tls13_config(t, a, true);

    TestQUICConn *cli = new_test_quic_client(t, &client_config);
    tls_quic_conn_set_transport_parameters(cli->conn, slice_nil(TYPE_BYTE));
    TestQUICConn *srv = new_test_quic_server(t, &server_config);
    tls_quic_conn_set_transport_parameters(srv->conn, slice_nil(TYPE_BYTE));
    Error err = run_test_quic_connection(context_background(), cli, srv, no_on_event);
    if (BURROW_FAILED(err))
        testing_t_fatalf_v(t, "error during connection handshake: %v", err);

    if (cli->got_params.p == NULL)
        testing_t_errorf_v(t, "client did not get transport params");
    if (srv->got_params.p == NULL)
        testing_t_errorf_v(t, "server did not get transport params");
    if (cli->got_params.len != 0)
        testing_t_errorf_v(t, "client got transport params: %v, want empty",
                           cli->got_params);
    if (srv->got_params.len != 0)
        testing_t_errorf_v(t, "server got transport params: %v, want empty",
                           srv->got_params);
}

static void TestQUICCanceledWaitingForData(TestingT *t) {
    Alloc *a = test_arena(t);
    TlsQUICConfig client_config = tls13_config(t, a, false);
    TestQUICConn *cli = new_test_quic_client(t, &client_config);
    tls_quic_conn_set_transport_parameters(cli->conn, slice_nil(TYPE_BYTE));
    (void)tls_quic_conn_start(cli->conn, context_background());
    while (tls_quic_conn_next_event(cli->conn).kind != TLS_QUIC_NO_EVENT) {
    }
    Error err = tls_quic_conn_close(cli->conn);
    if (!errors_is(err, burrow__tls_alert(a, TLS_ALERT_CLOSE_NOTIFY)))
        testing_t_errorf_v(t, "conn.Close() = %v, want alertCloseNotify", err);
}

static void TestQUICCanceledWaitingForTransportParams(TestingT *t) {
    Alloc *a = test_arena(t);
    TlsQUICConfig client_config = tls13_config(t, a, false);
    TestQUICConn *cli = new_test_quic_client(t, &client_config);
    (void)tls_quic_conn_start(cli->conn, context_background());
    while (tls_quic_conn_next_event(cli->conn).kind !=
           TLS_QUIC_TRANSPORT_PARAMETERS_REQUIRED) {
    }
    Error err = tls_quic_conn_close(cli->conn);
    if (!errors_is(err, burrow__tls_alert(a, TLS_ALERT_CLOSE_NOTIFY)))
        testing_t_errorf_v(t, "conn.Close() = %v, want alertCloseNotify", err);
}

static bool no_session_events(void *env, TlsQUICEvent e, TestQUICConn *src,
                              TestQUICConn *dst) {
    TestingT *t = (TestingT *)env;
    (void)src;
    (void)dst;
    switch (e.kind) {
    case TLS_QUIC_STORE_SESSION:
    case TLS_QUIC_RESUME_SESSION:
        testing_t_errorf_v(t, "with EnableSessionEvents=false, got unexpected event %v",
                           e.kind);
        break;
    default:
        break;
    }
    return false;
}

static void TestQUICEarlyData(TestingT *t) {
    Alloc *a = test_arena(t);
    TlsQUICConfig client_config = tls13_config(t, a, false);
    client_config.tls_config->client_session_cache = new_lru_cache(t, a, 1);
    client_config.tls_config->next_protos = h3();

    TlsQUICConfig server_config = tls13_config(t, a, true);
    server_config.tls_config->next_protos = h3();

    TestQUICConn *cli = new_test_quic_client(t, &client_config);
    tls_quic_conn_set_transport_parameters(cli->conn, slice_nil(TYPE_BYTE));
    TestQUICConn *srv = new_test_quic_server(t, &server_config);
    tls_quic_conn_set_transport_parameters(srv->conn, slice_nil(TYPE_BYTE));
    srv->ticket_opts.early_data = true;
    Error err = run_test_quic_connection(context_background(), cli, srv, no_on_event);
    if (BURROW_FAILED(err))
        testing_t_fatalf_v(t, "error during first connection handshake: %v", err);
    if (tls_quic_conn_connection_state(cli->conn).did_resume)
        testing_t_errorf_v(t, "first connection unexpectedly used session resumption");

    TestQUICConn *cli2 = new_test_quic_client(t, &client_config);
    tls_quic_conn_set_transport_parameters(cli2->conn, slice_nil(TYPE_BYTE));
    TestQUICConn *srv2 = new_test_quic_server(t, &server_config);
    tls_quic_conn_set_transport_parameters(srv2->conn, slice_nil(TYPE_BYTE));
    err = run_test_quic_connection(context_background(), cli2, srv2,
                                   BURROW_FN(OnEventFunc, no_session_events, t));
    if (BURROW_FAILED(err))
        testing_t_fatalf_v(t, "error during second connection handshake: %v", err);
    if (!tls_quic_conn_connection_state(cli2->conn).did_resume)
        testing_t_errorf_v(t, "second connection did not use session resumption");
    SuiteSecret cli_secret =
        secret_get(&cli2->write_secret, TLS_QUIC_ENCRYPTION_LEVEL_EARLY);
    if (cli_secret.secret.p == NULL)
        testing_t_errorf_v(t, "client did not receive early data write secret");
    SuiteSecret srv_secret =
        secret_get(&srv2->read_secret, TLS_QUIC_ENCRYPTION_LEVEL_EARLY);
    if (srv_secret.secret.p == NULL)
        testing_t_errorf_v(t, "server did not receive early data read secret");
    if (cli_secret.suite != srv_secret.suite ||
        !bytes_equal(cli_secret.secret, srv_secret.secret))
        testing_t_errorf_v(t, "client early data secret does not match server");
}

static void decline_early_data(TlsSessionState *state) {
    state->early_data = false;
}

static void test_quic_early_data_declined(TestingT *t, bool server) {
    Alloc *a = test_arena(t);
    TlsQUICConfig client_config = tls13_config(t, a, false);
    client_config.enable_session_events = true;
    client_config.tls_config->client_session_cache = new_lru_cache(t, a, 1);
    client_config.tls_config->next_protos = h3();

    TlsQUICConfig server_config = tls13_config(t, a, true);
    server_config.enable_session_events = true;
    server_config.tls_config->next_protos = h3();

    TestQUICConn *cli = new_test_quic_client(t, &client_config);
    tls_quic_conn_set_transport_parameters(cli->conn, slice_nil(TYPE_BYTE));
    TestQUICConn *srv = new_test_quic_server(t, &server_config);
    tls_quic_conn_set_transport_parameters(srv->conn, slice_nil(TYPE_BYTE));
    srv->ticket_opts.early_data = true;
    Error err = run_test_quic_connection(context_background(), cli, srv, no_on_event);
    if (BURROW_FAILED(err))
        testing_t_fatalf_v(t, "error during first connection handshake: %v", err);
    if (tls_quic_conn_connection_state(cli->conn).did_resume)
        testing_t_errorf_v(t, "first connection unexpectedly used session resumption");

    TestQUICConn *cli2 = new_test_quic_client(t, &client_config);
    tls_quic_conn_set_transport_parameters(cli2->conn, slice_nil(TYPE_BYTE));
    TestQUICConn *srv2 = new_test_quic_server(t, &server_config);
    tls_quic_conn_set_transport_parameters(srv2->conn, slice_nil(TYPE_BYTE));
    if (server)
        srv2->on_resume_session = decline_early_data;
    else
        cli2->on_resume_session = decline_early_data;
    err = run_test_quic_connection(context_background(), cli2, srv2, no_on_event);
    if (BURROW_FAILED(err))
        testing_t_fatalf_v(t, "error during second connection handshake: %v", err);
    if (!tls_quic_conn_connection_state(cli2->conn).did_resume)
        testing_t_errorf_v(t, "second connection did not use session resumption");
    bool cli_early_data =
        secret_ok(&cli2->write_secret, TLS_QUIC_ENCRYPTION_LEVEL_EARLY);
    if (server) {
        if (!cli_early_data)
            testing_t_errorf_v(t, "client did not receive early data write secret");
        if (!cli2->early_data_rejected)
            testing_t_errorf_v(t, "client did not receive QUICEarlyDataRejected");
    }
    if (secret_ok(&srv2->read_secret, TLS_QUIC_ENCRYPTION_LEVEL_EARLY))
        testing_t_errorf_v(t, "server received early data read secret");
}

static void early_data_declined_server(void *env, TestingT *t) {
    (void)env;
    test_quic_early_data_declined(t, true);
}

static void early_data_declined_client(void *env, TestingT *t) {
    (void)env;
    test_quic_early_data_declined(t, false);
}

static void TestQUICEarlyDataDeclined(TestingT *t) {
    testing_t_run(t, BURROW_S("server"),
                  BURROW_FN(TestingTFunc, early_data_declined_server, NULL));
    testing_t_run(t, BURROW_S("client"),
                  BURROW_FN(TestingTFunc, early_data_declined_client, NULL));
}

#define TESTS(X)                                                                       \
    X(TestQUICConnection)                                                              \
    X(TestQUICVersions)                                                                \
    X(TestQUICSessionResumption)                                                       \
    X(TestQUICFragmentaryData)                                                         \
    X(TestQUICPostHandshakeClientAuthentication)                                       \
    X(TestQUICPostHandshakeKeyUpdate)                                                  \
    X(TestQUICPostHandshakeMessageTooLarge)                                            \
    X(TestQUICHandshakeError)                                                          \
    X(TestQUICECHKeyError)                                                             \
    X(TestQUICConnectionState)                                                         \
    X(TestQUICStartContextPropagation)                                                 \
    X(TestQUICClientHelloInfoConn)                                                     \
    X(TestQUICContextCancelation)                                                      \
    X(TestQUICDelayedTransportParameters)                                              \
    X(TestQUICEmptyTransportParameters)                                                \
    X(TestQUICCanceledWaitingForData)                                                  \
    X(TestQUICCanceledWaitingForTransportParams)                                       \
    X(TestQUICEarlyData)                                                               \
    X(TestQUICEarlyDataDeclined)

TESTING_MAIN(TESTS)
