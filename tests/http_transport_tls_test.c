/* Derived from Go's src/net/http/transport_test.go.
 * Go source: go1.27.1.
 *
 * When a Transport turns HTTP/2 on by itself, the TLS handshake timeout, and
 * the tests of the transport over TLS, in Go's https1 and h2 modes. Go's
 * CondSkipHTTP2 is left out, since HTTP/2 can't be built out of burrow, and so
 * is afterTest, which looks for goroutines Go's transports leave behind.
 *
 * Copyright 2011 The Go Authors. All rights reserved.
 * Copyright 2026 The burrow Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style licence that can be found
 * in the LICENSE file. */

#include "check.h"

#include "burrow/bufio.h"
#include "burrow/burrow.h"
#include "burrow/chan.h"
#include "burrow/context.h"
#include "burrow/crypto/tls.h"
#include "burrow/crypto/x509.h"
#include "burrow/error.h"
#include "burrow/fmt.h"
#include "burrow/io.h"
#include "burrow/log.h"
#include "burrow/map.h"
#include "burrow/mem/arena.h"
#include "burrow/mem/heap.h"
#include "burrow/net.h"
#include "burrow/net/http.h"
#include "burrow/net/http/httptest.h"
#include "burrow/net/http/httptrace.h"
#include "burrow/net/url.h"
#include "burrow/netpoll.h"
#include "burrow/proc.h"
#include "burrow/strings.h"
#include "burrow/sync.h"
#include "burrow/time.h"

#include <stdint.h>
#include <string.h>

#if defined(BURROW_NETPOLL_READINESS) && !defined(BURROW_OS_WASI)
#define HAVE_TCP 1
#endif

/* Not in Go: the test below listens and dials on loopback, which needs the
 * readiness poll FD. */
static void need_tcp(TestingT *t) {
#if !defined(HAVE_TCP)
    testing_t_skip_v(t, "TCP here needs the readiness poll FD");
#else
    (void)t;
#endif
}

/* Not in Go: tr.TLSNextProto["h2"] != nil. */
static bool tls_next_proto_h2(HttpTransport *tr) {
    Str key = BURROW_S("h2");
    const HttpTransportTLSNextProtoFunc *fn =
        (const HttpTransportTLSNextProtoFunc *)map_get(tr->tls_next_proto, &key);
    return fn != NULL && fn->f != NULL;
}

static void test_transport_auto_http(TestingT *t, HttpTransport *tr, bool want_h2) {
    HttpRequest req;
    memset(&req, 0, sizeof req);
    Error err = BURROW_NO_ERROR;
    HttpResponse *res = http_transport_round_trip(tr, &req, &err);
    if (BURROW_OK(err)) {
        testing_t_error_v(t, "expected error from RoundTrip");
        if (res != NULL)
            http_response_free(res);
    }
    bool reg = tls_next_proto_h2(tr);
    if (reg != want_h2)
        testing_t_errorf_v(t, "HTTP/2 registered = %v; want %v", reg, want_h2);
}

/* Not in Go: &Transport{}, before the fields a test sets. */
static void zero_transport(HttpTransport *tr) {
    memset(tr, 0, sizeof *tr);
}

static void TestTransportAutomaticHTTP2(TestingT *t) {
    HttpTransport tr;
    zero_transport(&tr);
    test_transport_auto_http(t, &tr, true);
    http_transport_free(&tr);
}

static void
TestTransportAutomaticHTTP2_DialerAndTLSConfigSupportsHTTP2AndTLSConfig(TestingT *t) {
    TlsConfig conf;
    memset(&conf, 0, sizeof conf);
    HttpTransport tr;
    zero_transport(&tr);
    tr.force_attempt_http2 = true;
    tr.tls_client_config = &conf;
    test_transport_auto_http(t, &tr, true);
    http_transport_free(&tr);
}

/* golang.org/issue/14391: also check DefaultTransport */
static void TestTransportAutomaticHTTP2_DefaultTransport(TestingT *t) {
    test_transport_auto_http(t, http_default_transport, true);
}

static void TestTransportAutomaticHTTP2_TLSNextProto(TestingT *t) {
    Map *m = map_make(heap_allocator(), TYPE_STRING,
                      TYPE_HTTP_TRANSPORT_TLS_NEXT_PROTO_FUNC, 0);
    if (m == NULL)
        testing_t_fatal_v(t, "out of memory");
    HttpTransport tr;
    zero_transport(&tr);
    tr.tls_next_proto = m;
    test_transport_auto_http(t, &tr, false);
    http_transport_free(&tr);
    map_free(m);
}

static void TestTransportAutomaticHTTP2_TLSConfig(TestingT *t) {
    TlsConfig conf;
    memset(&conf, 0, sizeof conf);
    HttpTransport tr;
    zero_transport(&tr);
    tr.tls_client_config = &conf;
    test_transport_auto_http(t, &tr, false);
    http_transport_free(&tr);
}

static void TestTransportAutomaticHTTP2_ExpectContinueTimeout(TestingT *t) {
    HttpTransport tr;
    zero_transport(&tr);
    tr.expect_continue_timeout = 1 * TIME_SECOND;
    test_transport_auto_http(t, &tr, true);
    http_transport_free(&tr);
}

/* Not in Go: d.Dial, d.DialContext and the DialTLS that panics, as the
 * functions the transport calls. */
static NetConn dialer_dial(void *env, Str network, Str addr, Error *err) {
    return net_dialer_dial((const NetDialer *)env, heap_allocator(), network, addr,
                           err);
}

static NetConn dialer_dial_context(void *env, Context ctx, Str network, Str addr,
                                   Error *err) {
    return net_dialer_dial_context((const NetDialer *)env, heap_allocator(), ctx,
                                   network, addr, err);
}

static NetConn dial_tls_unused(void *env, Str network, Str addr, Error *err) {
    (void)env;
    (void)network;
    (void)addr;
    (void)err;
    runtime_panic(BURROW_S("unused"));
}

static void TestTransportAutomaticHTTP2_Dial(TestingT *t) {
    NetDialer d;
    memset(&d, 0, sizeof d);
    HttpTransport tr;
    zero_transport(&tr);
    tr.dial = BURROW_FN(HttpDialFunc, dialer_dial, &d);
    test_transport_auto_http(t, &tr, false);
    http_transport_free(&tr);
}

static void TestTransportAutomaticHTTP2_DialContext(TestingT *t) {
    NetDialer d;
    memset(&d, 0, sizeof d);
    HttpTransport tr;
    zero_transport(&tr);
    tr.dial_context = BURROW_FN(HttpDialContextFunc, dialer_dial_context, &d);
    test_transport_auto_http(t, &tr, false);
    http_transport_free(&tr);
}

static void TestTransportAutomaticHTTP2_DialTLS(TestingT *t) {
    HttpTransport tr;
    zero_transport(&tr);
    tr.dial_tls = BURROW_FN(HttpDialFunc, dial_tls_unused, NULL);
    test_transport_auto_http(t, &tr, false);
    http_transport_free(&tr);
}

/* ------------------------------------------------- the handshake timeout */

typedef struct AcceptJob {
    TestingT *t;
    NetListener ln;
    Chan *testdonec;
} AcceptJob;

static void accept_job(void *env) {
    AcceptJob *j = (AcceptJob *)env;
    Error err = BURROW_NO_ERROR;
    NetConn c = j->ln.vt->accept(j->ln.data, &err);
    if (BURROW_FAILED(err)) {
        testing_t_error_v(j->t, err);
        return;
    }
    (void)chan_recv(j->testdonec, NULL);
    net_conn_free(c);
}

typedef struct LnDial {
    Str addr;
} LnDial;

static NetConn ln_dial(void *env, Str network, Str addr, Error *err) {
    (void)network;
    (void)addr;
    return net_dial(heap_allocator(), BURROW_S("tcp"), ((LnDial *)env)->addr, err);
}

static void TestTransportTLSHandshakeTimeout(TestingT *t) {
    need_tcp(t);
    if (testing_short())
        testing_t_skip_v(t, "skipping in short mode");
    Arena ar;
    arena_init(&ar, heap_allocator(), 0);
    Alloc *a = arena_allocator(&ar);
    Error err = BURROW_NO_ERROR;
    NetListener ln =
        net_listen(heap_allocator(), BURROW_S("tcp"), BURROW_S("127.0.0.1:0"), &err);
    if (BURROW_FAILED(err)) {
        err = BURROW_NO_ERROR;
        ln = net_listen(heap_allocator(), BURROW_S("tcp6"), BURROW_S("[::1]:0"), &err);
    }
    if (BURROW_FAILED(err)) {
        arena_free(&ar);
        testing_t_fatal_v(t, err);
    }
    Chan *testdonec = chan_make(heap_allocator(), TYPE_BOOL, 0);
    if (testdonec == NULL) {
        net_listener_free(ln);
        arena_free(&ar);
        testing_t_fatal_v(t, "out of memory");
    }

    AcceptJob job = {t, ln, testdonec};
    SyncWaitGroup wg = {0};
    sync_wait_group_go(&wg, BURROW_FN(Func, accept_job, &job));

    NetAddr laddr = ln.vt->addr(ln.data);
    LnDial ld = {laddr.vt->string(laddr.data, a)};
    HttpTransport tr;
    memset(&tr, 0, sizeof tr);
    tr.dial = BURROW_FN(HttpDialFunc, ln_dial, &ld);
    tr.tls_handshake_timeout = 250 * TIME_MILLISECOND;
    HttpClient cl;
    memset(&cl, 0, sizeof cl);
    cl.transport = http_transport_as_round_tripper(&tr);
    HttpResponse *res = http_client_get(&cl, BURROW_S("https://dummy.tld/"), &err);
    /* Not in Go: the checks below skip to the end rather than return, so
     * that the goroutine is waited for and what the test made is given
     * back, which Go's deferred calls do for it. */
    if (res != NULL) {
        http_response_free(res);
        testing_t_error_v(t, "expected error");
        goto done;
    }
    const UrlError *ue = (const UrlError *)errors_as(err, TYPE_URL_ERROR);
    if (ue == NULL) {
        testing_t_errorf_v(t, "expected url.Error; got %#v", err);
        goto done;
    }
    if (!net_is_error(ue->err)) {
        testing_t_errorf_v(t, "expected net.Error; got %#v", err);
        goto done;
    }
    if (!net_error_timeout(ue->err))
        testing_t_errorf_v(t, "expected timeout error; got %v", err);
    if (!strings_contains(error_text(err), BURROW_S("handshake timeout")))
        testing_t_errorf_v(t, "expected 'handshake timeout' in error; got %v", err);

done:
    chan_close(testdonec);
    (void)ln.vt->closer.close(ln.data);
    sync_wait_group_wait(&wg);
    http_transport_free(&tr);
    net_listener_free(ln);
    chan_free(testdonec);
    arena_free(&ar);
}

/* ------------------------------------------- tests in the two TLS modes */

/* Not in Go: the mode a subtest runs in, of the two Go runs these tests in. */
typedef enum { HTTPS1_MODE, HTTP2_MODE } TestMode;

/* Not in Go: run's subtests, named as Go names them, for the modes given. */
static void run_modes(TestingT *t, void (*fn)(void *env, TestingT *t),
                      const TestMode *modes, int n) {
    for (int i = 0; i < n; i++) {
        Str name = modes[i] == HTTPS1_MODE ? BURROW_S("https1") : BURROW_S("h2");
        (void)testing_t_run(t, name,
                            BURROW_FN(TestingTFunc, fn, (void *)(uintptr_t)&modes[i]));
    }
}

/* newClientServerTest for an unstarted server, started by
 * start_client_server_test once the test has set what it sets. p is the
 * Protocols that go in the server and the transport, and has to outlive them.
 *
 * Not in Go: the ErrorLog that goes to t.Logf is left out, so the server logs
 * to the standard logger. */
static void start_client_server_test(HttptestServer *ts, TestMode mode,
                                     HttpProtocols *p) {
    memset(p, 0, sizeof *p);
    if (ts->config.protocols == NULL)
        ts->config.protocols = p;
    switch (mode) {
    case HTTPS1_MODE:
        http_protocols_set_http1(p, true);
        httptest_server_start_tls(ts);
        break;
    case HTTP2_MODE:
        http_protocols_set_http2(p, true);
        ts->enable_http2 = true;
        ts->tls = ts->config.tls_config;
        httptest_server_start_tls(ts);
        break;
    default:
        break;
    }
    if (ts->transport.protocols == NULL)
        ts->transport.protocols = p;
}

/* Not in Go: a client's read and write loops finish on their own after a
 * connection closes, and one still on its way out when the test binary exits
 * looks like a leak to ASAN, so the tests wait up to five seconds for the
 * count of goroutines to come back. */
static void wait_for_goroutines(int want) {
    for (int i = 0; i < 5000 && runtime_numgoroutine() > want; i++)
        time_sleep(TIME_MILLISECOND);
}

typedef struct ClosesConnection {
    Chan *closedc;
} ClosesConnection;

static void serve_keep_alive_then_die(void *env, HttpResponseWriter w, HttpRequest *r) {
    ClosesConnection *cc = (ClosesConnection *)env;
    if (strings_contains(r->url->path, BURROW_S("/keep-alive-then-die"))) {
        HttpResponseController rc = http_new_response_controller(w);
        BufioReadWriter buf;
        memset(&buf, 0, sizeof buf);
        Error err = BURROW_NO_ERROR;
        NetConn conn = http_response_controller_hijack(&rc, &buf, &err);
        if (conn.vt != NULL) {
            Byte resp[] = "HTTP/1.1 200 OK\r\nContent-Length: 3\r\n\r\nfoo";
            Int n = (Int)sizeof resp - 1;
            (void)conn.vt->writer.write(conn.data, slice_from(resp, n, n, TYPE_BYTE),
                                        &err);
            (void)conn.vt->closer.close(conn.data);
            /* Not in Go: the connection and the server's reader and writer
             * are given back. */
            tls_conn_free(tls_conn_from_net_conn(conn));
            bufio_reader_free(buf.reader);
            bufio_writer_free(buf.writer);
        }
        bool v = true;
        (void)chan_send(cc->closedc, &v);
        return;
    }
    (void)fmt_fprintf_v(http_response_writer_as_io_writer(w), "hello");
}

static void tls_server_closes_connection(void *env, TestingT *t) {
    need_tcp(t);
    int ng = runtime_numgoroutine();
    TestMode mode = *(const TestMode *)env;
    ClosesConnection cc = {chan_make(heap_allocator(), TYPE_BOOL, 1)};
    if (cc.closedc == NULL)
        testing_t_fatal_v(t, "out of memory");
    HttpHandlerFunc f = BURROW_FN(HttpHandlerFunc, serve_keep_alive_then_die, &cc);
    HttptestServer *ts =
        httptest_new_unstarted_server(NULL, http_handler_func_as_handler(&f));
    HttpProtocols p;
    start_client_server_test(ts, mode, &p);

    HttpClient *c = httptest_server_client(ts);
    HttpTransport *tr = &ts->transport;

    Arena ar;
    arena_init(&ar, heap_allocator(), 0);
    Alloc *a = arena_allocator(&ar);
    Int n_success = 0;
    Error errs[20];
    Int nerrs = 0;
    enum { TRIALS = 20 };
    for (int i = 0; i < TRIALS; i++) {
        http_transport_close_idle_connections(tr);
        Error err = BURROW_NO_ERROR;
        HttpResponse *res = http_client_get(
            c, fmt_sprintf_v(a, "%s/keep-alive-then-die", ts->url), &err);
        if (BURROW_FAILED(err)) {
            /* Not in Go: what the test made is given back first. */
            httptest_server_free(ts);
            chan_free(cc.closedc);
            arena_free(&ar);
            testing_t_fatal_v(t, err);
        }
        (void)chan_recv(cc.closedc, NULL);
        Slice slurp = io_read_all(a, io_read_closer_as_io_reader(res->body), &err);
        (void)res->body.vt->closer.close(res->body.data);
        http_response_free(res);
        if (BURROW_FAILED(err)) {
            httptest_server_free(ts);
            chan_free(cc.closedc);
            arena_free(&ar);
            testing_t_fatal_v(t, err);
        }
        Str got = str_from_bytes((const Byte *)slurp.p, slurp.len);
        if (!str_eq(got, BURROW_S("foo")))
            testing_t_errorf_v(t, "Got %q, want foo", got);

        /* Now try again and see if we successfully
         * pick a new connection. */
        res = http_client_get(c, fmt_sprintf_v(a, "%s/", ts->url), &err);
        if (BURROW_FAILED(err)) {
            errs[nerrs++] = err;
            continue;
        }
        (void)io_read_all(a, io_read_closer_as_io_reader(res->body), &err);
        (void)res->body.vt->closer.close(res->body.data);
        http_response_free(res);
        if (BURROW_FAILED(err)) {
            errs[nerrs++] = err;
            continue;
        }
        n_success++;
    }
    if (n_success > 0)
        testing_t_logf_v(t, "successes = %d of %d", n_success, (Int)TRIALS);
    else
        testing_t_errorf_v(t, "All runs failed:");
    for (Int i = 0; i < nerrs; i++)
        testing_t_logf_v(t, "  err: %v", errs[i]);

    httptest_server_free(ts);
    chan_free(cc.closedc);
    arena_free(&ar);
    wait_for_goroutines(ng);
}

/* Trying to repro golang.org/issue/3514 */
static void TestTLSServerClosesConnection(TestingT *t) {
    static const TestMode modes[1] = {HTTPS1_MODE};
    run_modes(t, tls_server_closes_connection, modes, 1);
}

typedef struct DialTLS {
    SyncMutex mu; /* guards following */
    bool got_req, did_dial;
    TlsConfig *config;
} DialTLS;

static void serve_got_req(void *env, HttpResponseWriter w, HttpRequest *r) {
    (void)w;
    (void)r;
    DialTLS *d = (DialTLS *)env;
    sync_mutex_lock(&d->mu);
    d->got_req = true;
    sync_mutex_unlock(&d->mu);
}

static NetConn dial_tls_hook(void *env, Str netw, Str addr, Error *err) {
    DialTLS *d = (DialTLS *)env;
    sync_mutex_lock(&d->mu);
    d->did_dial = true;
    sync_mutex_unlock(&d->mu);
    TlsConn *c = tls_dial(heap_allocator(), netw, addr, d->config, err);
    if (BURROW_FAILED(*err)) {
        NetConn none = {NULL, NULL};
        return none;
    }
    *err = tls_conn_handshake(c);
    return tls_conn_as_net_conn(c);
}

/* Not in Go: the transport gives back the connections the hook made with
 * this. */
static void free_tls_conn(void *env, NetConn c) {
    (void)env;
    tls_conn_free(tls_conn_from_net_conn(c));
}

static void transport_dial_tls(void *env, TestingT *t) {
    need_tcp(t);
    int ng = runtime_numgoroutine();
    TestMode mode = *(const TestMode *)env;
    DialTLS d;
    memset(&d, 0, sizeof d);

    HttpHandlerFunc f = BURROW_FN(HttpHandlerFunc, serve_got_req, &d);
    HttptestServer *ts =
        httptest_new_unstarted_server(NULL, http_handler_func_as_handler(&f));
    HttpProtocols p;
    start_client_server_test(ts, mode, &p);
    HttpClient *c = httptest_server_client(ts);

    /* Not in Go: Go's transport puts "h2" in TLSClientConfig.NextProtos when
     * it turns HTTP/2 on, and the hook's tls.Dial offers it from there. Here
     * it goes only in the clone each of the transport's own connections gets,
     * so the hook dials with a clone that has what Go's config would. */
    Arena ar;
    arena_init(&ar, heap_allocator(), 0);
    Alloc *a = arena_allocator(&ar);
    d.config = tls_config_clone(ts->transport.tls_client_config, a);
    if (d.config == NULL) {
        httptest_server_free(ts);
        arena_free(&ar);
        testing_t_fatal_v(t, "out of memory");
    }
    Str h2[1] = {BURROW_S("h2")};
    if (mode == HTTP2_MODE)
        d.config->next_protos = slice_from(h2, 1, 1, TYPE_STRING);

    ts->transport.dial_tls = BURROW_FN(HttpDialFunc, dial_tls_hook, &d);
    ts->transport.free_conn = BURROW_FN(HttpFreeConnFunc, free_tls_conn, NULL);

    Error err = BURROW_NO_ERROR;
    HttpResponse *res = http_client_get(c, ts->url, &err);
    if (BURROW_FAILED(err)) {
        httptest_server_free(ts);
        tls_config_release(d.config);
        arena_free(&ar);
        testing_t_fatal_v(t, err);
    }
    (void)res->body.vt->closer.close(res->body.data);
    http_response_free(res);
    sync_mutex_lock(&d.mu);
    if (!d.got_req)
        testing_t_error_v(t, "didn't get request");
    if (!d.did_dial)
        testing_t_error_v(t, "didn't use dial hook");
    sync_mutex_unlock(&d.mu);

    httptest_server_free(ts);
    tls_config_release(d.config);
    arena_free(&ar);
    wait_for_goroutines(ng);
}

static void TestTransportDialTLS(TestingT *t) {
    static const TestMode modes[2] = {HTTPS1_MODE, HTTP2_MODE};
    run_modes(t, transport_dial_tls, modes, 2);
}

typedef struct EventLog {
    SyncMutex mu;
    Arena ar;
    StringsBuilder buf;
} EventLog;

/* logf, for a format that is already done. */
static void event_log_str(EventLog *lg, Str s) {
    sync_mutex_lock(&lg->mu);
    (void)strings_builder_write_string(&lg->buf, s, NULL);
    (void)strings_builder_write_byte(&lg->buf, '\n');
    sync_mutex_unlock(&lg->mu);
}

/* funcWriter, which logs each write. */
static const Type event_log_type = {
    {(const Byte *)"funcWriter", 10},
    {(const Byte *)"http_test", 9},
    KIND_STRUCT,
    (uint32_t)sizeof(EventLog),
    (uint16_t)_Alignof(EventLog),
    0,
    0,
    NULL,
    NULL,
    NULL,
    NULL,
    0,
    0x66756e77U,
    NULL,
};

static Int event_log_write(void *self, Slice p, Error *err) {
    EventLog *lg = (EventLog *)self;
    sync_mutex_lock(&lg->mu);
    (void)fmt_fprintf_v(strings_builder_as_io_writer(&lg->buf), "%s",
                        str_from_bytes((const Byte *)p.p, p.len));
    (void)strings_builder_write_byte(&lg->buf, '\n');
    sync_mutex_unlock(&lg->mu);
    *err = BURROW_NO_ERROR;
    return p.len;
}

static const IoWriterVT event_log_vt = {&event_log_type, event_log_write};

static void serve_unexpected(void *env, HttpResponseWriter w, HttpRequest *r) {
    (void)w;
    (void)r;
    testing_t_error_v((TestingT *)env, "Unexpected request");
}

static void verify_handshake_start(void *env) {
    event_log_str((EventLog *)env, BURROW_S("TLSHandshakeStart"));
}

static void verify_handshake_done(void *env, TlsConnectionState s, Error err) {
    EventLog *lg = (EventLog *)env;
    Any state = BURROW_ANY(TYPE_TLS_CONNECTION_STATE, &s);
    sync_mutex_lock(&lg->mu);
    (void)fmt_fprintf_v(strings_builder_as_io_writer(&lg->buf),
                        "TLSHandshakeDone: ConnectionState = %v \n err = %v", state,
                        err);
    (void)strings_builder_write_byte(&lg->buf, '\n');
    sync_mutex_unlock(&lg->mu);
}

static void want_once(TestingT *t, Str got, Str sub) {
    if (strings_count(got, sub) != 1)
        testing_t_errorf_v(t, "expected substring %q exactly once in output.", sub);
}

static void transport_event_trace_tls_verify(void *env, TestingT *t) {
    need_tcp(t);
    int ng = runtime_numgoroutine();
    TestMode mode = *(const TestMode *)env;
    EventLog lg;
    memset(&lg, 0, sizeof lg);
    arena_init(&lg.ar, heap_allocator(), 0);
    lg.buf = STRINGS_BUILDER(arena_allocator(&lg.ar));

    HttpHandlerFunc f = BURROW_FN(HttpHandlerFunc, serve_unexpected, t);
    HttptestServer *ts =
        httptest_new_unstarted_server(NULL, http_handler_func_as_handler(&f));
    IoWriter fw = {&event_log_vt, &lg};
    LogLogger *error_log = log_new(heap_allocator(), fw, BURROW_S(""), 0);
    ts->config.error_log = error_log;
    HttpProtocols p;
    start_client_server_test(ts, mode, &p);

    Arena ar;
    arena_init(&ar, heap_allocator(), 0);
    Alloc *a = arena_allocator(&ar);
    X509CertPool *certpool = x509_new_cert_pool(a);
    if (certpool == NULL) {
        httptest_server_free(ts);
        log_logger_free(heap_allocator(), error_log);
        arena_free(&ar);
        arena_free(&lg.ar);
        testing_t_fatal_v(t, "out of memory");
    }
    x509_cert_pool_add_cert(certpool, httptest_server_certificate(ts));

    TlsConfig cfg;
    memset(&cfg, 0, sizeof cfg);
    cfg.server_name = BURROW_S("dns-is-faked.golang");
    cfg.root_cas = certpool;
    HttpTransport tr;
    memset(&tr, 0, sizeof tr);
    tr.tls_client_config = &cfg;
    HttpClient c;
    memset(&c, 0, sizeof c);
    c.transport = http_transport_as_round_tripper(&tr);

    HttptraceClientTrace trace;
    memset(&trace, 0, sizeof trace);
    trace.tls_handshake_start = BURROW_FN(Func, verify_handshake_start, &lg);
    trace.tls_handshake_done =
        BURROW_FN(HttptraceTLSHandshakeDoneFunc, verify_handshake_done, &lg);

    Error err = BURROW_NO_ERROR;
    IoReader none = {NULL, NULL};
    HttpRequest *req0 = http_new_request(a, BURROW_S("GET"), ts->url, none, &err);
    Context ctx = httptrace_with_client_trace(a, context_background(), &trace);
    HttpRequest *req = NULL;
    if (req0 != NULL && ctx.vt != NULL)
        req = http_request_with_context(req0, a, ctx);
    if (req == NULL) {
        /* Not in Go: NewRequest's error is not looked at there. */
        testing_t_error_v(t, "out of memory");
    } else {
        HttpResponse *res = http_client_do(&c, req, &err);
        if (BURROW_OK(err)) {
            testing_t_error_v(t, "Expected request to fail TLS verification");
            if (res != NULL) {
                (void)res->body.vt->closer.close(res->body.data);
                http_response_free(res);
            }
        }
    }

    sync_mutex_lock(&lg.mu);
    Str got = str_clone(a, strings_builder_string(&lg.buf));
    sync_mutex_unlock(&lg.mu);

    want_once(t, got, BURROW_S("TLSHandshakeStart"));
    want_once(t, got, BURROW_S("TLSHandshakeDone"));
    want_once(t, got,
              BURROW_S("err = tls: failed to verify certificate: x509: certificate is "
                       "valid for example.com"));

    if (testing_t_failed(t))
        testing_t_errorf_v(t, "Output:\n%s", got);

    http_transport_free(&tr);
    tls_config_release(&cfg);
    context_release(ctx);
    httptest_server_free(ts);
    log_logger_free(heap_allocator(), error_log);
    x509_cert_pool_free(certpool);
    arena_free(&ar);
    arena_free(&lg.ar);
    wait_for_goroutines(ng);
}

static void TestTransportEventTraceTLSVerify(TestingT *t) {
    static const TestMode modes[2] = {HTTPS1_MODE, HTTP2_MODE};
    run_modes(t, transport_event_trace_tls_verify, modes, 2);
}

typedef struct HandshakeTrace {
    TestingT *t;
    SyncMutex mu;
    bool start, done;
} HandshakeTrace;

static void serve_nothing(void *env, HttpResponseWriter w, HttpRequest *r) {
    (void)env;
    (void)w;
    (void)r;
}

static void trace_handshake_start(void *env) {
    HandshakeTrace *h = (HandshakeTrace *)env;
    sync_mutex_lock(&h->mu);
    h->start = true;
    sync_mutex_unlock(&h->mu);
}

static void trace_handshake_done(void *env, TlsConnectionState s, Error err) {
    (void)s;
    HandshakeTrace *h = (HandshakeTrace *)env;
    sync_mutex_lock(&h->mu);
    h->done = true;
    /* Not in Go: Error rather than Fatal, which can't be called from the
     * transport's goroutine the hook runs on. */
    if (BURROW_FAILED(err))
        testing_t_error_v(h->t, "Expected error to be nil but was:", err);
    sync_mutex_unlock(&h->mu);
}

static void tls_handshake_trace(void *env, TestingT *t) {
    need_tcp(t);
    int ng = runtime_numgoroutine();
    TestMode mode = *(const TestMode *)env;
    HttpHandlerFunc f = BURROW_FN(HttpHandlerFunc, serve_nothing, NULL);
    HttptestServer *ts =
        httptest_new_unstarted_server(NULL, http_handler_func_as_handler(&f));
    HttpProtocols p;
    start_client_server_test(ts, mode, &p);

    HandshakeTrace h;
    memset(&h, 0, sizeof h);
    h.t = t;
    HttptraceClientTrace trace;
    memset(&trace, 0, sizeof trace);
    trace.tls_handshake_start = BURROW_FN(Func, trace_handshake_start, &h);
    trace.tls_handshake_done =
        BURROW_FN(HttptraceTLSHandshakeDoneFunc, trace_handshake_done, &h);

    HttpClient *c = httptest_server_client(ts);
    Arena ar;
    arena_init(&ar, heap_allocator(), 0);
    Alloc *a = arena_allocator(&ar);
    Error err = BURROW_NO_ERROR;
    IoReader none = {NULL, NULL};
    HttpRequest *req0 = http_new_request(a, BURROW_S("GET"), ts->url, none, &err);
    if (req0 == NULL) {
        httptest_server_free(ts);
        arena_free(&ar);
        testing_t_fatal_v(t, "Unable to construct test request:", err);
    }
    Context ctx = httptrace_with_client_trace(a, http_request_context(req0), &trace);
    HttpRequest *req = http_request_with_context(req0, a, ctx);
    if (req == NULL) {
        httptest_server_free(ts);
        context_release(ctx);
        arena_free(&ar);
        testing_t_fatal_v(t, "out of memory");
    }

    HttpResponse *r = http_client_do(c, req, &err);
    if (r == NULL) {
        httptest_server_free(ts);
        context_release(ctx);
        arena_free(&ar);
        testing_t_fatal_v(t, "Unexpected error making request:", err);
    }
    (void)r->body.vt->closer.close(r->body.data);
    http_response_free(r);
    sync_mutex_lock(&h.mu);
    bool start = h.start, done = h.done;
    sync_mutex_unlock(&h.mu);
    httptest_server_free(ts);
    context_release(ctx);
    arena_free(&ar);
    wait_for_goroutines(ng);
    if (!start)
        testing_t_fatal_v(t, "Expected TLSHandshakeStart to be called, but wasn't");
    if (!done)
        testing_t_fatal_v(t, "Expected TLSHandshakeDone to be called, but wasn't");
}

/* Test the httptrace.TLSHandshake{Start,Done} hooks with an https http1
 * connections. The http2 test is done in TestTransportEventTrace_h2 */
static void TestTLSHandshakeTrace(TestingT *t) {
    static const TestMode modes[2] = {HTTPS1_MODE, HTTP2_MODE};
    run_modes(t, tls_handshake_trace, modes, 2);
}

#define TESTS(X)                                                                       \
    X(TestTransportAutomaticHTTP2)                                                     \
    X(TestTransportAutomaticHTTP2_DialerAndTLSConfigSupportsHTTP2AndTLSConfig)         \
    X(TestTransportAutomaticHTTP2_DefaultTransport)                                    \
    X(TestTransportAutomaticHTTP2_TLSNextProto)                                        \
    X(TestTransportAutomaticHTTP2_TLSConfig)                                           \
    X(TestTransportAutomaticHTTP2_ExpectContinueTimeout)                               \
    X(TestTransportAutomaticHTTP2_Dial)                                                \
    X(TestTransportAutomaticHTTP2_DialContext)                                         \
    X(TestTransportAutomaticHTTP2_DialTLS)                                             \
    X(TestTransportTLSHandshakeTimeout)                                                \
    X(TestTLSServerClosesConnection)                                                   \
    X(TestTransportDialTLS)                                                            \
    X(TestTransportEventTraceTLSVerify)                                                \
    X(TestTLSHandshakeTrace)
TESTING_MAIN(TESTS)
