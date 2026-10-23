/* Derived from Go's src/net/http/transport_test.go.
 * Go source: go1.27.1.
 *
 * When a Transport turns HTTP/2 on by itself, and the TLS handshake timeout.
 * Go's CondSkipHTTP2 is left out, since HTTP/2 can't be built out of burrow,
 * and so is afterTest, which looks for goroutines Go's transports leave
 * behind.
 *
 * Copyright 2011 The Go Authors. All rights reserved.
 * Copyright 2026 The burrow Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style licence that can be found
 * in the LICENSE file. */

#include "check.h"

#include "burrow/burrow.h"
#include "burrow/chan.h"
#include "burrow/crypto/tls.h"
#include "burrow/error.h"
#include "burrow/map.h"
#include "burrow/mem/arena.h"
#include "burrow/mem/heap.h"
#include "burrow/net.h"
#include "burrow/net/http.h"
#include "burrow/net/url.h"
#include "burrow/netpoll.h"
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
    X(TestTransportTLSHandshakeTimeout)
TESTING_MAIN(TESTS)
