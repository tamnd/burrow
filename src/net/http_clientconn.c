/* Derived from Go's src/net/http/clientconn.go.
 * Go source: go1.27.3.
 *
 * Copyright 2025 The Go Authors. All rights reserved.
 * Copyright 2026 The burrow Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style licence that can be found
 * in the LICENSE file. */

#include "burrow/net/http.h"

#include "http_internal.h"

#include "burrow/context.h"
#include "burrow/core.h"
#include "burrow/error.h"
#include "burrow/fmt.h"
#include "burrow/func.h"
#include "burrow/mem.h"
#include "burrow/mem/arena.h"
#include "burrow/mem/heap.h"
#include "burrow/net.h"
#include "burrow/net/url.h"
#include "burrow/proc.h"
#include "burrow/sync.h"
#include "burrow/sync/atomic.h"

#include <stdbool.h>
#include <stdint.h>
#include <string.h>

BURROW_SENTINEL_ERROR(cc_err_nil_url, "http: nil Request.URL");
BURROW_SENTINEL_ERROR(cc_err_nil_header, "http: nil Request.Header");
BURROW_SENTINEL_ERROR(cc_err_no_host, "http: no Host in request URL");
BURROW_SENTINEL_ERROR(cc_err_no_ping, "http: ClientConn does not support PING");

/* A ClientConn is a client connection to an HTTP server.
 *
 * Unlike a [Transport], a ClientConn represents a single connection.
 * Most users should use a Transport rather than creating client connections
 * directly. */
struct HttpClientConn {
    burrow__HttpGenericClientConn cc; /* set under state_hook_mu, then fixed */
    Alloc *a;

    SyncMutex state_hook_mu;
    HttpClientConnStateHook user_state_hook;
    bool state_hook_running;
    Int last_available;
    Int last_in_flight;
    bool last_closed;

    /* Not in Go: one for the caller, one for the connection, which done gives
     * back, and one for each goroutine that runs the hook. */
    SyncAtomicInt32 refs;
    SyncCond hook_idle; /* broadcast when state_hook_running goes false */
    bool freed;         /* under state_hook_mu */
    /* The hook the goroutine maybeRunStateHook starts is to run first. There
     * is one such goroutine at most, as state_hook_running is set for it. */
    HttpClientConnStateHook loop_hook;
};

static void cc_unref(HttpClientConn *cc) {
    if (sync_atomic_int32_add(&cc->refs, -1) == 0)
        mem_free(cc->a, cc, sizeof *cc, _Alignof(HttpClientConn));
}

/* The connection's done: it calls the hook no more. */
static void cc_conn_done(void *env) {
    cc_unref((HttpClientConn *)env);
}

/* shouldRunStateHook returns the user's state hook if we should call it,
 * or nil if we don't need to call it at this time. */
static HttpClientConnStateHook cc_should_run_state_hook(HttpClientConn *cc,
                                                        bool stop_running) {
    HttpClientConnStateHook hook = {0};
    sync_mutex_lock(&cc->state_hook_mu);
    if (cc->cc.vt == NULL) {
        sync_mutex_unlock(&cc->state_hook_mu);
        return hook;
    }
    if (stop_running) {
        cc->state_hook_running = false;
        sync_cond_broadcast(&cc->hook_idle);
    }
    if (BURROW_FUNC_IS_NIL(cc->user_state_hook) || cc->state_hook_running) {
        sync_mutex_unlock(&cc->state_hook_mu);
        return hook;
    }
    ArenaMark m = error_mark();
    Int available = cc->cc.vt->available(cc->cc.self);
    Int in_flight = cc->cc.vt->in_flight(cc->cc.self);
    bool closed = BURROW_FAILED(cc->cc.vt->err(cc->cc.self));
    error_release(m);
    if (available > cc->last_available || in_flight < cc->last_in_flight ||
        closed != cc->last_closed) {
        hook = cc->user_state_hook;
        cc->state_hook_running = true;
    }
    cc->last_available = available;
    cc->last_in_flight = in_flight;
    cc->last_closed = closed;
    sync_mutex_unlock(&cc->state_hook_mu);
    return hook;
}

/* The goroutine maybeRunStateHook starts, which holds a reference. */
static void cc_run_state_hook_loop(void *env) {
    HttpClientConn *cc = (HttpClientConn *)env;
    HttpClientConnStateHook hook = cc->loop_hook;
    while (!BURROW_FUNC_IS_NIL(hook)) {
        BURROW_CALLF(hook, cc);
        hook = cc_should_run_state_hook(cc, true);
    }
    cc_unref(cc);
}

static void cc_maybe_run_state_hook(HttpClientConn *cc) {
    HttpClientConnStateHook hook = cc_should_run_state_hook(cc, false);
    if (BURROW_FUNC_IS_NIL(hook))
        return;
    /* Run the hook synchronously.
     *
     * This means that if, for example, the user calls resp.Body.Close to
     * finish a request, the Close call will synchronously run the hook,
     * giving the hook the chance to return the ClientConn to a connection
     * pool before the next request is made. */
    BURROW_CALLF(hook, cc);
    /* The connection state may have changed while the hook was running, in
     * which case we need to run it again.
     *
     * If we do need to run the hook again, do so in a new goroutine to avoid
     * blocking the current goroutine indefinitely. */
    hook = cc_should_run_state_hook(cc, true);
    if (!BURROW_FUNC_IS_NIL(hook)) {
        cc->loop_hook = hook;
        (void)sync_atomic_int32_add(&cc->refs, 1);
        if (!go(BURROW_FN(Func, cc_run_state_hook_loop, cc))) {
            /* Not in Go: with no goroutine to be had, the hook runs here. */
            cc_run_state_hook_loop(cc);
        }
    }
}

/* The internal state hook the connection calls. */
static void cc_internal_state_hook(void *env) {
    cc_maybe_run_state_hook((HttpClientConn *)env);
}

HttpClientConn *http_transport_new_client_conn(HttpTransport *t, Context ctx,
                                               Str scheme, Str address, Error *err) {
    if (!str_eq(scheme, BURROW_S("http")) && !str_eq(scheme, BURROW_S("https"))) {
        BURROW_OUT(err, fmt_errorf_v("net/http: invalid scheme %q", scheme));
        return NULL;
    }

    Error e = BURROW_NO_ERROR;
    Str port = {0};
    Str host = net_split_host_port(address, &port, &e);
    if (BURROW_FAILED(e)) {
        BURROW_OUT(err, e);
        return NULL;
    }
    if (port.len == 0)
        port = str_eq(scheme, BURROW_S("http")) ? BURROW_S("80") : BURROW_S("443");

    Alloc *a = t->a != NULL ? t->a : heap_allocator();
    Arena scratch;
    arena_init(&scratch, a, 0);
    Alloc *sa = arena_allocator(&scratch);
    Url *proxy_url = NULL;
    if (t->proxy.f != NULL) {
        /* Transport.Proxy takes a *Request, so create a fake one to pass it. */
        Url u;
        memset(&u, 0, sizeof u);
        u.scheme = scheme;
        u.host = host;
        u.path = BURROW_S("/");
        HttpRequest req;
        memset(&req, 0, sizeof req);
        req.ctx = ctx;
        req.method = BURROW_S("GET");
        req.url = &u;
        req.proto = BURROW_S("HTTP/1.1");
        req.proto_major = 1;
        req.proto_minor = 1;
        req.header = http_header_make(sa);
        req.body = http_no_body;
        req.host = host;
        if (req.header == NULL) {
            e = burrow_err_out_of_memory;
        } else {
            proxy_url = BURROW_CALLF(t->proxy, &req, sa, &e);
            if (BURROW_FAILED(e))
                e = error_retain(error_allocator(), e);
        }
        if (BURROW_FAILED(e)) {
            arena_free(&scratch);
            BURROW_OUT(err, e);
            return NULL;
        }
    }
    Str target_addr = net_join_host_port(sa, host, port);
    HttpClientConn *cc =
        target_addr.len == 0
            ? NULL
            : (HttpClientConn *)mem_alloc(a, sizeof *cc, _Alignof(HttpClientConn));
    if (cc == NULL) {
        arena_free(&scratch);
        BURROW_OUT(err, burrow_err_out_of_memory);
        return NULL;
    }
    memset(cc, 0, sizeof *cc);
    cc->a = a;
    cc->refs.v = 2;
    cc->hook_idle = SYNC_COND(sync_mutex_locker(&cc->state_hook_mu));

    /* The state hook is a bit tricky:
     * The persistConn has a state hook which calls
     * ClientConn.maybeRunStateHook, which in turn calls the user-provided
     * state hook (if any).
     *
     * ClientConn.maybeRunStateHook handles debouncing hook calls for both
     * HTTP/1 and HTTP/2.
     *
     * Since there's no need to change the persistConn's hook, we set it at
     * creation time. */
    burrow__HttpGenericClientConn gc = {0};
    e = burrow__http_transport_dial_client_conn(
        t, ctx, scheme, target_addr, proxy_url,
        BURROW_FN(Func, cc_internal_state_hook, cc), BURROW_FN(Func, cc_conn_done, cc),
        &gc);
    arena_free(&scratch);
    if (BURROW_FAILED(e)) {
        cc_unref(cc);
        BURROW_OUT(err, e);
        return NULL;
    }

    /* Note that cc.maybeRunStateHook may have been called
     * in the short window between dialConn and now.
     * This is fine. */
    sync_mutex_lock(&cc->state_hook_mu);
    cc->cc = gc;
    ArenaMark m = error_mark();
    cc->last_available = gc.vt->available(gc.self);
    error_release(m);
    sync_mutex_unlock(&cc->state_hook_mu);
    BURROW_OUT(err, BURROW_NO_ERROR);
    return cc;
}

Error http_client_conn_close(HttpClientConn *cc) {
    Error e = cc->cc.vt->close(cc->cc.self);
    cc_maybe_run_state_hook(cc);
    return e;
}

Error http_client_conn_err(HttpClientConn *cc) {
    return cc->cc.vt->err(cc->cc.self);
}

static Error cc_validate_client_conn_request(const HttpRequest *req) {
    if (req->url == NULL)
        return cc_err_nil_url;
    if (req->header == NULL)
        return cc_err_nil_header;
    /* Validate the outgoing headers. */
    Str bad = burrow__http_validate_headers(error_allocator(), req->header);
    if (bad.len > 0)
        return fmt_errorf_v("http: invalid header %s", bad);
    /* Validate the outgoing trailers too. */
    bad = burrow__http_validate_headers(error_allocator(), req->trailer);
    if (bad.len > 0)
        return fmt_errorf_v("http: invalid trailer %s", bad);
    if (req->method.len != 0 && !burrow__http_valid_method(req->method))
        return fmt_errorf_v("http: invalid method %q", req->method);
    if (req->url->host.len == 0)
        return cc_err_no_host;
    return BURROW_NO_ERROR;
}

HttpResponse *http_client_conn_round_trip(HttpClientConn *cc, HttpRequest *req,
                                          Error *err) {
    if (req->url == NULL && str_eq(req->method, BURROW_S(":ping"))) {
        /* Undocumented feature for sending a PING frame to a HTTP/2
         * connection, included to support x/net/http2.ClientConn.Ping. */
        Error e = cc_err_no_ping;
        if (cc->cc.vt->ping != NULL)
            e = cc->cc.vt->ping(cc->cc.self, http_request_context(req));
        cc_maybe_run_state_hook(cc);
        BURROW_OUT(err, e);
        return NULL;
    }
    Error e = cc_validate_client_conn_request(req);
    if (BURROW_FAILED(e)) {
        /* Not in Go: the body is closed, as a RoundTripper's is. */
        if (req->body.vt != NULL)
            (void)req->body.vt->closer.close(req->body.data);
        http_client_conn_release(cc);
        cc_maybe_run_state_hook(cc);
        BURROW_OUT(err, e);
        return NULL;
    }
    HttpResponse *res = cc->cc.vt->round_trip(cc->cc.self, req, &e);
    cc_maybe_run_state_hook(cc);
    BURROW_OUT(err, e);
    return res;
}

Int http_client_conn_available(HttpClientConn *cc) {
    return cc->cc.vt->available(cc->cc.self);
}

Int http_client_conn_in_flight(HttpClientConn *cc) {
    return cc->cc.vt->in_flight(cc->cc.self);
}

Error http_client_conn_reserve(HttpClientConn *cc) {
    Error e = cc->cc.vt->reserve(cc->cc.self);
    cc_maybe_run_state_hook(cc);
    return e;
}

void http_client_conn_release(HttpClientConn *cc) {
    cc->cc.vt->release(cc->cc.self);
    cc_maybe_run_state_hook(cc);
}

void http_client_conn_set_state_hook(HttpClientConn *cc, HttpClientConnStateHook f) {
    sync_mutex_lock(&cc->state_hook_mu);
    if (!cc->freed)
        cc->user_state_hook = f;
    sync_mutex_unlock(&cc->state_hook_mu);
    cc_maybe_run_state_hook(cc);
}

static HttpResponse *cc_rt_round_trip(void *self, HttpRequest *req, Error *err) {
    return http_client_conn_round_trip((HttpClientConn *)self, req, err);
}

static const HttpRoundTripperVT cc_round_tripper_vt = {NULL, cc_rt_round_trip};

HttpRoundTripper http_client_conn_as_round_tripper(HttpClientConn *cc) {
    return (HttpRoundTripper){&cc_round_tripper_vt, cc};
}

void http_client_conn_free(HttpClientConn *cc) {
    if (cc == NULL)
        return;
    sync_mutex_lock(&cc->state_hook_mu);
    cc->freed = true;
    cc->user_state_hook = (HttpClientConnStateHook){0};
    while (cc->state_hook_running)
        sync_cond_wait(&cc->hook_idle);
    sync_mutex_unlock(&cc->state_hook_mu);
    cc->cc.vt->free(cc->cc.self);
    cc_unref(cc);
}
