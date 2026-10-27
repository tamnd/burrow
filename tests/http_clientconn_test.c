/* Derived from Go's src/net/http/clientconn_test.go.
 * Go source: go1.27.3.
 *
 * The tests run in Go's http1 and h2 modes, on the network in memory
 * httptest_new_test_server has, which is Go's optFakeNet.
 *
 * Copyright 2025 The Go Authors. All rights reserved.
 * Copyright 2026 The burrow Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style licence that can be found
 * in the LICENSE file. */

#include "check.h"

#include "../src/net/http_internal.h"

#include "burrow/burrow.h"
#include "burrow/chan.h"
#include "burrow/context.h"
#include "burrow/error.h"
#include "burrow/fmt.h"
#include "burrow/io.h"
#include "burrow/mem/arena.h"
#include "burrow/mem/heap.h"
#include "burrow/net/http.h"
#include "burrow/net/http/httptest.h"
#include "burrow/proc.h"
#include "burrow/sync.h"
#include "burrow/sync/atomic.h"
#include "burrow/synctest.h"
#include "burrow/testing.h"
#include "burrow/time.h"

#include <stdint.h>
#include <string.h>

/* Not in Go: the mode a subtest runs in, of the two http3SkippedMode has. */
typedef enum { HTTP1_MODE, HTTP2_MODE } TestMode;

static const TestMode http3_skipped_mode[2] = {HTTP1_MODE, HTTP2_MODE};

static Str mode_scheme(TestMode mode) {
    return mode == HTTP1_MODE ? BURROW_S("http") : BURROW_S("https");
}

/* Not in Go: the network in memory has no listener address, and a dial goes
 * to the server whatever the address is. */
static Str mode_addr(TestMode mode) {
    return mode == HTTP1_MODE ? BURROW_S("example.tld:80")
                              : BURROW_S("example.tld:443");
}

/* What a test in one mode gets, made outside the bubble, since the server's
 * cleanup runs after the function the bubble runs has returned. */
typedef struct CCTest {
    TestMode mode;
    void (*fn)(struct CCTest *ct, TestingT *t);
    bool sync;       /* run in a bubble, as runSynctest does */
    const void *arg; /* the test's own */
    HttpHandlerFunc hf;
    void (*handler)(struct CCTest *ct, HttpResponseWriter w, HttpRequest *r);
    Chan *donec;    /* for the handler to wait on */
    Chan *requestc; /* for the handler to wait on */
    SyncAtomicInt64 handler_calls;
    SyncAtomicInt64 state_hook_calls;
    HttpHTTP2Config h2;
} CCTest;

static void cc_test_bubble(void *env, TestingT *t) {
    CCTest *ct = (CCTest *)env;
    ct->fn(ct, t);
}

static void cc_test_mode(void *env, TestingT *t) {
    CCTest *ct = (CCTest *)env;
    if (ct->sync)
        synctest_test(t, BURROW_FN(TestingTFunc, cc_test_bubble, ct));
    else
        ct->fn(ct, t);
}

/* run and runSynctest, for the modes in http3SkippedMode. */
static void run_modes(TestingT *t, void (*fn)(CCTest *ct, TestingT *t), bool sync,
                      const void *arg) {
    for (int i = 0; i < 2; i++) {
        CCTest ct;
        memset(&ct, 0, sizeof ct);
        ct.mode = http3_skipped_mode[i];
        ct.fn = fn;
        ct.sync = sync;
        ct.arg = arg;
        Str name = ct.mode == HTTP1_MODE ? BURROW_S("h1") : BURROW_S("h2");
        (void)testing_t_run(t, name, BURROW_FN(TestingTFunc, cc_test_mode, &ct));
    }
}

static void serve_cc_test(void *env, HttpResponseWriter w, HttpRequest *r) {
    CCTest *ct = (CCTest *)env;
    if (ct->handler != NULL)
        ct->handler(ct, w, r);
}

/* defer close(donec), as a cleanup that runs before the ClientConn's and the
 * server's, so that a test that stops early lets the handlers go. */
static void close_donec(void *env) {
    chan_close(((CCTest *)env)->donec);
}

/* Not in Go: frees the channels once the server is gone. */
static void free_chans(void *env) {
    CCTest *ct = (CCTest *)env;
    chan_free(ct->donec);
    chan_free(ct->requestc);
    ct->donec = NULL;
    ct->requestc = NULL;
}

static void free_client_conn(void *env) {
    http_client_conn_free((HttpClientConn *)env);
}

/* newClientServerTest with optFakeNet, and h2 for a test that sets
 * Server.HTTP2.MaxConcurrentStreams. */
static HttptestServer *new_client_server_test(CCTest *ct, TestingT *t,
                                              Int max_streams) {
    ct->hf = BURROW_FN(HttpHandlerFunc, serve_cc_test, ct);
    testing_t_cleanup(t, BURROW_FN(Func, free_chans, ct));
    HttptestServer *ts = httptest_new_test_server(
        NULL, testing_t_as_testing_tb(t), http_handler_func_as_handler(&ct->hf));
    if (max_streams > 0) {
        ct->h2.max_concurrent_streams = max_streams;
        ts->config.http2 = &ct->h2;
    }
    ts->enable_http2 = ct->mode == HTTP2_MODE;
    (void)httptest_server_client(ts);
    return ts;
}

/* newClientConnTest. The ClientConn is freed, which closes it, in a cleanup
 * that runs before the server's. */
static HttpClientConn *new_client_conn_test(CCTest *ct, TestingT *t, Int max_streams,
                                            HttptestServer **tsp) {
    HttptestServer *ts = new_client_server_test(ct, t, max_streams);
    Error err = BURROW_NO_ERROR;
    HttpClientConn *cc = http_transport_new_client_conn(
        &ts->transport, testing_t_context(t), mode_scheme(ct->mode),
        mode_addr(ct->mode), &err);
    if (cc == NULL)
        testing_t_fatal_v(t, err);
    testing_t_cleanup(t, BURROW_FN(Func, free_client_conn, cc));
    synctest_wait();
    if (tsp != NULL)
        *tsp = ts;
    return cc;
}

/* Not in Go: a request in a, for a test that cannot fail to make one. */
static HttpRequest *new_request(TestingT *t, Alloc *a, Context ctx, Str url) {
    Error err = BURROW_NO_ERROR;
    HttpRequest *req = http_new_request_with_context(a, ctx, BURROW_S("GET"), url,
                                                     (IoReader){0}, &err);
    if (req == NULL)
        testing_t_fatalf_v(t, "NewRequest: %v", err);
    return req;
}

static Str scheme_url(Alloc *a, TestMode mode, Str host) {
    return fmt_sprintf_v(a, "%v://%v/", mode_scheme(mode), host);
}

/* Not in Go: closes the body and frees the response. */
static void close_response(HttpResponse *res) {
    (void)res->body.vt->closer.close(res->body.data);
    http_response_free(res);
}

/* -------------------------------------------------------------- RoundTrip */

static void write_host(CCTest *ct, HttpResponseWriter w, HttpRequest *r) {
    (void)ct;
    (void)io_write_string(http_response_writer_as_io_writer(w), r->host, NULL);
}

static void transport_new_client_conn_round_trip(CCTest *ct, TestingT *t) {
    ct->handler = write_host;
    HttptestServer *ts = new_client_server_test(ct, t, 0);

    Str scheme = mode_scheme(ct->mode); /* http or https */
    Error err = BURROW_NO_ERROR;
    HttpClientConn *cc = http_transport_new_client_conn(
        &ts->transport, testing_t_context(t), scheme, mode_addr(ct->mode), &err);
    if (cc == NULL)
        testing_t_fatal_v(t, err);
    testing_t_cleanup(t, BURROW_FN(Func, free_client_conn, cc));

    Arena ar;
    arena_init(&ar, heap_allocator(), 0);
    Alloc *a = arena_allocator(&ar);

    /* Send requests for a couple different domains.
     * All use the same connection. */
    static const char *const hosts[2] = {"example.tld", "go.dev"};
    for (int i = 0; i < 2; i++) {
        Str host = str_from_cstr(hosts[i]);
        HttpRequest *req =
            new_request(t, a, context_background(), scheme_url(a, ct->mode, host));
        HttpResponse *resp = http_client_conn_round_trip(cc, req, &err);
        if (resp == NULL) {
            arena_free(&ar);
            testing_t_fatal_v(t, err);
        }
        IoReader body = {&resp->body.vt->reader, resp->body.data};
        Slice got = io_read_all(a, body, NULL);
        Str gs = str_from_bytes(got.p, got.len);
        if (!str_eq(gs, host))
            testing_t_errorf_v(t, "got response body %q, want %v", gs, host);
        close_response(resp);

        /* CloseIdleConnections does not close connections created by
         * NewClientConn. */
        http_transport_close_idle_connections(&ts->transport);
    }

    err = http_client_conn_err(cc);
    if (BURROW_FAILED(err))
        testing_t_errorf_v(t, "before close: ClientConn.Err() = %v, want nil", err);

    (void)http_client_conn_close(cc);
    err = http_client_conn_err(cc);
    if (BURROW_OK(err))
        testing_t_errorf_v(t, "after close: ClientConn.Err() = nil, want error");

    HttpRequest *req = new_request(t, a, context_background(),
                                   scheme_url(a, ct->mode, BURROW_S("example.tld")));
    HttpResponse *resp = http_client_conn_round_trip(cc, req, &err);
    if (resp != NULL) {
        close_response(resp);
        testing_t_errorf_v(t, "after close: cc.RoundTrip succeeded, want error");
    }
    testing_t_log_v(t, err);
    arena_free(&ar);
}

static void TestTransportNewClientConnRoundTrip(TestingT *t) {
    run_modes(t, transport_new_client_conn_round_trip, false, NULL);
}

/* ------------------------------------------------------------- ReserveAll */

/* TestClientConnReserveAll reserves every concurrency slot on a connection. */
static void client_conn_reserve_all(CCTest *ct, TestingT *t) {
    HttptestServer *ts = NULL;
    HttpClientConn *cc = new_client_conn_test(ct, t, 3, &ts);

    Int want = 1;
    if (ct->mode == HTTP2_MODE)
        want = ts->config.http2->max_concurrent_streams;
    Int available = http_client_conn_available(cc);
    if (available != want)
        testing_t_fatalf_v(t, "cc.Available() = %v, want %v", available, want);

    /* Reserve every available concurrency slot on the connection. */
    for (Int i = 0; i < available; i++) {
        Error err = http_client_conn_reserve(cc);
        if (BURROW_FAILED(err))
            testing_t_fatalf_v(t, "cc.Reserve() #%v = %v, want nil", i, err);
        Int got = http_client_conn_available(cc), w = available - i - 1;
        if (got != w)
            testing_t_fatalf_v(t, "cc.Available() = %v, want %v", got, w);
        got = http_client_conn_in_flight(cc);
        w = i + 1;
        if (got != w)
            testing_t_fatalf_v(t, "cc.InFlight() = %v, want %v", got, w);
    }

    /* The next reservation attempt should fail, since every slot is consumed. */
    if (BURROW_OK(http_client_conn_reserve(cc)))
        testing_t_fatalf_v(t, "cc.Reserve() = nil, want error");
}

static void TestClientConnReserveAll(TestingT *t) {
    run_modes(t, client_conn_reserve_all, true, NULL);
}

/* -------------------------------------------------------- ReserveParallel */

typedef struct ReserveParallel {
    HttpClientConn *cc;
    SyncMutex mu;
    Int success;
    Int failure;
} ReserveParallel;

static void reserve_parallel(void *env) {
    ReserveParallel *rp = (ReserveParallel *)env;
    Error err = http_client_conn_reserve(rp->cc);
    sync_mutex_lock(&rp->mu);
    if (BURROW_OK(err))
        rp->success++;
    else
        rp->failure++;
    sync_mutex_unlock(&rp->mu);
}

/* TestClientConnReserveParallel starts concurrent goroutines which reserve
 * every concurrency slot on a connection. */
static void client_conn_reserve_parallel(CCTest *ct, TestingT *t) {
    HttpClientConn *cc = new_client_conn_test(ct, t, 3, NULL);
    SyncWaitGroup wg;
    memset(&wg, 0, sizeof wg);
    ReserveParallel rp;
    memset(&rp, 0, sizeof rp);
    rp.cc = cc;
    Int available = http_client_conn_available(cc);
    const Int extra = 2;
    for (Int i = 0; i < available + extra; i++)
        sync_wait_group_go(&wg, BURROW_FN(Func, reserve_parallel, &rp));
    sync_wait_group_wait(&wg);

    if (rp.success != available)
        testing_t_errorf_v(t, "%v successful reservations, want %v", rp.success,
                           available);
    if (rp.failure != extra)
        testing_t_errorf_v(t, "%v failed reservations, want %v", rp.failure, extra);
}

static void TestClientConnReserveParallel(TestingT *t) {
    run_modes(t, client_conn_reserve_parallel, true, NULL);
}

/* --------------------------------------------------------- ReserveRelease */

/* TestClientConnReserveRelease repeatedly reserves and releases concurrency
 * slots. */
static void client_conn_reserve_release(CCTest *ct, TestingT *t) {
    HttpClientConn *cc = new_client_conn_test(ct, t, 3, NULL);

    Int available = http_client_conn_available(cc);
    for (Int i = 0; i < 2 * available; i++) {
        Error err = http_client_conn_reserve(cc);
        if (BURROW_FAILED(err))
            testing_t_fatalf_v(t, "cc.Reserve() #%v = %v, want nil", i, err);
        http_client_conn_release(cc);
    }

    Int got = http_client_conn_available(cc);
    if (got != available)
        testing_t_fatalf_v(t, "cc.Available() = %v, want %v", available, available);
}

static void TestClientConnReserveRelease(TestingT *t) {
    run_modes(t, client_conn_reserve_release, true, NULL);
}

/* ----------------------------------------------------- ReserveAndConsume */

/* A request on a goroutine of its own, for the cancel case. */
typedef struct CancelRT {
    TestingT *t;
    HttpClientConn *cc;
    HttpRequest *req;
} CancelRT;

static void cancel_rt(void *env) {
    CancelRT *c = (CancelRT *)env;
    Error err = BURROW_NO_ERROR;
    HttpResponse *res = http_client_conn_round_trip(c->cc, c->req, &err);
    if (res != NULL) {
        close_response(res); /* Not in Go. */
        testing_t_errorf_v(c->t, "RoundTrip succeeded, want failure");
    }
}

/* What a consume gets. Not in Go: a for the requests, and wg for a goroutine
 * to be waited for. */
typedef struct Consume {
    TestingT *t;
    HttpClientConn *cc;
    TestMode mode;
    Alloc *a;
    SyncWaitGroup wg;
    CancelRT crt;
    Context ctx;
    Func cancel;
} Consume;

typedef struct ConsumeCase {
    const char *name;
    void (*consume)(Consume *c);
    void (*handler)(CCTest *ct, HttpResponseWriter w, HttpRequest *r);
    bool h1_closed;
} ConsumeCase;

/* Explicit release. */
static void consume_release(Consume *c) {
    http_client_conn_release(c->cc);
}

/* Invalid request sent to RoundTrip. */
static void consume_invalid_field_name(Consume *c) {
    HttpRequest *req = new_request(c->t, c->a, context_background(),
                                   scheme_url(c->a, c->mode, BURROW_S("example.tld")));
    (void)http_header_set(req->header, BURROW_S("invalid field name"), BURROW_S("x"));
    Error err = BURROW_NO_ERROR;
    HttpResponse *res = http_client_conn_round_trip(c->cc, req, &err);
    if (res != NULL) {
        close_response(res); /* Not in Go. */
        testing_t_fatalf_v(c->t, "RoundTrip succeeded, want failure");
    }
}

/* Successful request/response cycle. */
static void consume_body_close(Consume *c) {
    HttpRequest *req = new_request(c->t, c->a, context_background(),
                                   scheme_url(c->a, c->mode, BURROW_S("example.tld")));
    Error err = BURROW_NO_ERROR;
    HttpResponse *resp = http_client_conn_round_trip(c->cc, req, &err);
    if (resp == NULL)
        testing_t_fatalf_v(c->t, "RoundTrip: %v", err);
    close_response(resp);
}

/* Request context canceled before headers received. */
static void consume_cancel(Consume *c) {
    c->ctx = context_with_cancel(c->a, testing_t_context(c->t), &c->cancel);
    c->crt.t = c->t;
    c->crt.cc = c->cc;
    c->crt.req = new_request(c->t, c->a, c->ctx,
                             scheme_url(c->a, c->mode, BURROW_S("example.tld")));
    sync_wait_group_go(&c->wg, BURROW_FN(Func, cancel_rt, &c->crt));
    synctest_wait();
    BURROW_CALLF0(c->cancel);
}

static void handler_wait(CCTest *ct, HttpResponseWriter w, HttpRequest *r) {
    (void)w;
    (void)r;
    (void)chan_recv(ct->donec, NULL);
}

/* Response body closed before full response received. */
static void consume_early_body_close(Consume *c) {
    HttpRequest *req = new_request(c->t, c->a, context_background(),
                                   scheme_url(c->a, c->mode, BURROW_S("example.tld")));
    Error err = BURROW_NO_ERROR;
    HttpResponse *resp = http_client_conn_round_trip(c->cc, req, &err);
    if (resp == NULL)
        testing_t_fatalf_v(c->t, "RoundTrip: %v", err);
    close_response(resp);
}

static void handler_flush_wait(CCTest *ct, HttpResponseWriter w, HttpRequest *r) {
    (void)r;
    http_response_writer_write_header(w, 200);
    HttpResponseController rc = http_new_response_controller(w);
    (void)http_response_controller_flush(&rc);
    (void)chan_recv(ct->donec, NULL);
}

static void count_state_hook(void *env, HttpClientConn *cc) {
    (void)cc;
    (void)sync_atomic_int64_add((SyncAtomicInt64 *)env, 1);
}

static void reserve_and_consume(CCTest *ct, TestingT *t) {
    const ConsumeCase *test = (const ConsumeCase *)ct->arg;
    ct->donec = chan_make(heap_allocator(), TYPE_BOOL, 0);
    if (ct->donec == NULL)
        testing_t_fatal_v(t, burrow_err_out_of_memory);
    ct->handler = test->handler;

    HttpClientConn *cc = new_client_conn_test(ct, t, 0, NULL);
    testing_t_cleanup(t, BURROW_FN(Func, close_donec, ct));
    /* Not in Go: an atomic, since the hook runs on other goroutines. */
    http_client_conn_set_state_hook(
        cc,
        BURROW_FN(HttpClientConnStateHook, count_state_hook, &ct->state_hook_calls));
    synctest_wait();
    sync_atomic_int64_store(&ct->state_hook_calls,
                            0); /* ignore any initial update call */

    Int avail = http_client_conn_available(cc);
    Error err = http_client_conn_reserve(cc);
    if (BURROW_FAILED(err))
        testing_t_fatalf_v(t, "cc.Reserve() = %v, want nil", err);
    synctest_wait();
    int64_t calls = sync_atomic_int64_load(&ct->state_hook_calls);
    if (calls != 0)
        testing_t_errorf_v(t, "connection state hook calls: %v, want %v", calls, 0);

    Arena ar;
    arena_init(&ar, heap_allocator(), 0);
    Consume c;
    memset(&c, 0, sizeof c);
    c.t = t;
    c.cc = cc;
    c.mode = ct->mode;
    c.a = arena_allocator(&ar);
    test->consume(&c);
    if (ct->mode == HTTP1_MODE)
        time_sleep(burrow__http_max_post_close_read_time);
    synctest_wait();

    /* State hook should be called, either to report the
     * connection availability increasing or the connection closing,
     * or both. */
    calls = sync_atomic_int64_load(&ct->state_hook_calls);
    if (calls == 0)
        testing_t_errorf_v(t, "connection state hook calls: %v, want >1", calls);

    if (test->h1_closed && ct->mode == HTTP1_MODE) {
        Int got = http_client_conn_available(cc);
        if (got != 0)
            testing_t_errorf_v(t, "cc.Available() = %v, want %v", got, 0);
        got = http_client_conn_in_flight(cc);
        if (got != 0)
            testing_t_errorf_v(t, "cc.InFlight() = %v, want %v", got, 0);
        if (BURROW_OK(http_client_conn_err(cc)))
            testing_t_errorf_v(t, "cc.Err() = nil, want closed connection");
    } else {
        Int got = http_client_conn_available(cc);
        if (got != avail)
            testing_t_errorf_v(t, "cc.Available() = %v, want %v", got, avail);
        got = http_client_conn_in_flight(cc);
        if (got != 0)
            testing_t_errorf_v(t, "cc.InFlight() = %v, want %v", got, 0);
        err = http_client_conn_err(cc);
        if (BURROW_FAILED(err))
            testing_t_errorf_v(t, "cc.Err() = %v, want nil", err);
    }

    if (http_client_conn_available(cc) > 0) {
        err = http_client_conn_reserve(cc);
        if (BURROW_FAILED(err))
            testing_t_errorf_v(t, "cc.Reserve() = %v, want success", err);
    }

    /* Not in Go: the request's goroutine is done before its memory goes. */
    sync_wait_group_wait(&c.wg);
    if (!BURROW_CONTEXT_IS_NIL(c.ctx))
        context_release(c.ctx);
    arena_free(&ar);
}

static void reserve_and_consume_case(void *env, TestingT *t) {
    run_modes(t, reserve_and_consume, true, env);
}

/* TestClientConnReserveAndConsume reserves a concurrency slot on a
 * connection, and then verifies that various events consume the
 * reservation. */
static void TestClientConnReserveAndConsume(TestingT *t) {
    static const ConsumeCase tests[] = {
        {"release", consume_release, NULL, false},
        {"invalid field name", consume_invalid_field_name, NULL, false},
        {"body close", consume_body_close, NULL, false},
        /* An HTTP/1 connection is closed after a request is canceled on it. */
        {"cancel", consume_cancel, handler_wait, true},
        /* An HTTP/1 connection is closed after a request is canceled on it. */
        {"early body close", consume_early_body_close, handler_flush_wait, true},
    };
    for (size_t i = 0; i < sizeof tests / sizeof tests[0]; i++)
        (void)testing_t_run(t, str_from_cstr(tests[i].name),
                            BURROW_FN(TestingTFunc, reserve_and_consume_case,
                                      (void *)(uintptr_t)&tests[i]));
}

/* --------------------------------------------------------- RoundTripBlocks */

static void handler_count_wait(CCTest *ct, HttpResponseWriter w, HttpRequest *r) {
    (void)w;
    (void)r;
    (void)sync_atomic_int64_add(&ct->handler_calls, 1);
    (void)chan_recv(ct->requestc, NULL);
}

typedef struct BlockRT {
    TestingT *t;
    HttpClientConn *cc;
    HttpRequest *req;
    SyncAtomicInt64 *responses;
} BlockRT;

static void block_rt(void *env) {
    BlockRT *b = (BlockRT *)env;
    Error err = BURROW_NO_ERROR;
    HttpResponse *resp = http_client_conn_round_trip(b->cc, b->req, &err);
    (void)sync_atomic_int64_add(b->responses, 1);
    if (resp == NULL) {
        testing_t_errorf_v(b->t, "RoundTrip: %v", err);
        return;
    }
    close_response(resp);
}

/* TestClientConnRoundTripBlocks verifies that RoundTrip blocks until a
 * concurrency slot is available on a connection. */
static void client_conn_round_trip_blocks(CCTest *ct, TestingT *t) {
    ct->requestc = chan_make(heap_allocator(), TYPE_BOOL, 0);
    if (ct->requestc == NULL)
        testing_t_fatal_v(t, burrow_err_out_of_memory);
    ct->handler = handler_count_wait;
    HttpClientConn *cc = new_client_conn_test(ct, t, 3, NULL);

    Int available = http_client_conn_available(cc);
    SyncAtomicInt64 responses;
    memset(&responses, 0, sizeof responses);
    enum { EXTRA = 2, MAX_RT = 3 + EXTRA };
    if (available + EXTRA > MAX_RT)
        testing_t_fatalf_v(t, "cc.Available() = %v, want at most 3", available);
    Arena ar;
    arena_init(&ar, heap_allocator(), 0);
    Alloc *a = arena_allocator(&ar);
    SyncWaitGroup wg;
    memset(&wg, 0, sizeof wg);
    BlockRT rts[MAX_RT];
    for (Int i = 0; i < available + EXTRA; i++) {
        rts[i].t = t;
        rts[i].cc = cc;
        rts[i].req = new_request(t, a, context_background(),
                                 scheme_url(a, ct->mode, BURROW_S("example.tld")));
        rts[i].responses = &responses;
        sync_wait_group_go(&wg, BURROW_FN(Func, block_rt, &rts[i]));
    }

    synctest_wait();
    int64_t got = sync_atomic_int64_load(&ct->handler_calls);
    if (got != available)
        testing_t_errorf_v(t, "got %v handler calls, want %v", got, available);
    got = sync_atomic_int64_load(&responses);
    if (got != 0)
        testing_t_errorf_v(t, "got %v responses, want %v", got, 0);

    for (Int i = 0; i < available + EXTRA; i++) {
        bool v = true;
        (void)chan_send(ct->requestc, &v);
        synctest_wait();
        got = sync_atomic_int64_load(&responses);
        if (got != i + 1)
            testing_t_errorf_v(t, "got %v responses, want %v", got, i + 1);
    }
    sync_wait_group_wait(&wg);
    arena_free(&ar);
}

static void TestClientConnRoundTripBlocks(TestingT *t) {
    run_modes(t, client_conn_round_trip_blocks, true, NULL);
}

#define TESTS(X)                                                                       \
    X(TestTransportNewClientConnRoundTrip)                                             \
    X(TestClientConnReserveAll)                                                        \
    X(TestClientConnReserveParallel)                                                   \
    X(TestClientConnReserveRelease)                                                    \
    X(TestClientConnReserveAndConsume)                                                 \
    X(TestClientConnRoundTripBlocks)
TESTING_MAIN(TESTS)
