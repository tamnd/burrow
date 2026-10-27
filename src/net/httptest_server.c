/* net/http/httptest's Server: a server on a port of the loopback address for a
 * test to talk to.
 *
 * Derived from Go's src/net/http/httptest/server.go. Go source: go1.27.3.
 *
 * The -httptest.serve flag is left out.
 *
 * Copyright 2011 The Go Authors. All rights reserved.
 * Copyright 2026 The burrow Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style licence that can be found
 * in the LICENSE file. */

#include "burrow/net/http/httptest.h"

#include "http_internal.h"
#include "nettest.h"

#include "burrow/context.h"
#include "burrow/core.h"
#include "burrow/crypto/tls.h"
#include "burrow/crypto/x509.h"
#include "burrow/error.h"
#include "burrow/fmt.h"
#include "burrow/func.h"
#include "burrow/log.h"
#include "burrow/mem.h"
#include "burrow/mem/arena.h"
#include "burrow/mem/heap.h"
#include "burrow/net.h"
#include "burrow/net/http.h"
#include "burrow/panic.h"
#include "burrow/proc.h"
#include "burrow/runtime.h"
#include "burrow/slice.h"
#include "burrow/strings.h"
#include "burrow/sync.h"
#include "burrow/testing.h"
#include "burrow/time.h"

#include <stdbool.h>
#include <stdint.h>
#include <string.h>

/* A Serve on one of the listeners of the network in memory. */
typedef struct ts_Serve {
    HttptestServer *s;
    NetListener l;
} ts_Serve;

/* One of the listeners of the network in memory as a net.Listener, which
 * keeps the connections it accepts. */
typedef struct ts_FakeListener {
    struct burrow__HttptestFake *f;
    NettestListener *li;
} ts_FakeListener;

/* What a server from httptest_new_test_server has that the others do not:
 * testServerHandler, and the network in memory once it is started.
 *
 * Not in Go: srv keeps the server end of every connection the client dialed,
 * which the server does not give back, since it gives back only the kinds of
 * connection it knows. httptest_server_free gives them back once the server
 * is done with them. The client ends go back through the transport's
 * free_conn. */
typedef struct burrow__HttptestFake {
    HttptestServer *s;
    HttpHandler h;          /* testServerHandler.h */
    HttpHandlerFunc fn;     /* testServerHandler */
    ts_FakeListener li;     /* fakeListener */
    ts_FakeListener tls_li; /* fakeTLSListener */
    NetListener tls;        /* the TLS listener over tls_li */
    ts_Serve serve[2];
    SyncMutex mu; /* guards srv */
    NettestConn **srv;
    Int nsrv, cap_srv;
} ts_Fake;

/* A connection the server has and the state it is in, one of new, active and
 * idle. */
typedef struct burrow__HttptestConn {
    NetConn c;
    HttpConnState st;
} ts_Conn;

static Alloc *ts_alloc(const HttptestServer *s) {
    return s->a != NULL ? s->a : heap_allocator();
}

/* newLocalListener. */
static NetListener ts_new_local_listener(Alloc *a) {
    Error err = BURROW_NO_ERROR;
    NetListener l = net_listen(a, BURROW_S("tcp"), BURROW_S("127.0.0.1:0"), &err);
    if (BURROW_FAILED(err)) {
        l = net_listen(a, BURROW_S("tcp6"), BURROW_S("[::1]:0"), &err);
        if (BURROW_FAILED(err)) {
            Str msg = fmt_sprintf_v(heap_allocator(),
                                    "httptest: failed to listen on a port: %v", err);
            panic_str(msg);
        }
    }
    return l;
}

/* The index of c in s->conns, or -1. Called with s->mu held. */
static Int ts_find(const HttptestServer *s, NetConn c) {
    for (Int i = 0; i < s->nconns; i++) {
        if (s->conns[i].c.data == c.data)
            return i;
    }
    return -1;
}

/* closeConn. Called with s->mu held, which keeps c from going away, since the
 * server says a connection is closed before it frees it. */
static void ts_close_conn(NetConn c) {
    (void)c.vt->closer.close(c.data);
}

/* Adds c in state st. False when the allocator says no. Called with s->mu
 * held. */
static bool ts_add(HttptestServer *s, NetConn c, HttpConnState st) {
    if (s->nconns == s->cap_conns) {
        Alloc *a = ts_alloc(s);
        Int ncap = s->cap_conns == 0 ? 8 : s->cap_conns * 2;
        ts_Conn *conns =
            (ts_Conn *)mem_alloc(a, (size_t)ncap * sizeof *conns, _Alignof(ts_Conn));
        if (conns == NULL)
            return false;
        if (s->nconns > 0)
            memcpy(conns, s->conns, (size_t)s->nconns * sizeof *conns);
        if (s->conns != NULL)
            mem_free(a, s->conns, (size_t)s->cap_conns * sizeof *conns,
                     _Alignof(ts_Conn));
        s->conns = conns;
        s->cap_conns = ncap;
    }
    s->conns[s->nconns].c = c;
    s->conns[s->nconns].st = st;
    s->nconns++;
    return true;
}

/* The hook wrap puts in config.conn_state, which keeps track of the
 * connections so that Close can close the idle ones and wait for the rest. */
static void ts_conn_state(void *env, NetConn c, HttpConnState cs) {
    HttptestServer *s = (HttptestServer *)env;
    bool done = false;
    sync_mutex_lock(&s->mu);
    Int i = ts_find(s, c);
    switch (cs) {
    case HTTP_STATE_NEW:
        if (i >= 0) {
            sync_mutex_unlock(&s->mu);
            panic_str(BURROW_S("invalid state transition"));
        }
        if (!ts_add(s, c, cs)) {
            /* Not tracked, so closed now rather than waited for. */
            ts_close_conn(c);
            break;
        }
        sync_wait_group_add(&s->wg, 1);
        if (s->closed)
            ts_close_conn(c);
        break;
    case HTTP_STATE_ACTIVE:
        if (i >= 0) {
            HttpConnState old = s->conns[i].st;
            if (old != HTTP_STATE_NEW && old != HTTP_STATE_IDLE) {
                sync_mutex_unlock(&s->mu);
                panic_str(BURROW_S("invalid state transition"));
            }
            s->conns[i].st = cs;
        }
        break;
    case HTTP_STATE_IDLE:
        if (i >= 0) {
            if (s->conns[i].st != HTTP_STATE_ACTIVE) {
                sync_mutex_unlock(&s->mu);
                panic_str(BURROW_S("invalid state transition"));
            }
            s->conns[i].st = cs;
        }
        if (s->closed)
            ts_close_conn(c);
        break;
    case HTTP_STATE_HIJACKED:
    case HTTP_STATE_CLOSED:
        if (i >= 0) {
            s->conns[i] = s->conns[s->nconns - 1];
            s->nconns--;
            done = true;
        }
        break;
    default:
        break;
    }
    if (!BURROW_FUNC_IS_NIL(s->old_hook))
        BURROW_CALLF(s->old_hook, c, cs);
    sync_mutex_unlock(&s->mu);
    /* After the unlock, since Close can free s once the count is down. */
    if (done)
        sync_wait_group_done(&s->wg);
}

/* The transport's dial, which sends example.com to the server. */
static NetConn ts_dial(void *env, Context ctx, Str network, Str addr, Error *err) {
    HttptestServer *s = (HttptestServer *)env;
    if (!BURROW_FUNC_IS_NIL(s->transport.dial))
        return BURROW_CALLF(s->transport.dial, network, addr, err);
    Arena ar;
    arena_init(&ar, heap_allocator(), 0);
    Str host = s->use_tls ? BURROW_S("example.com:443") : BURROW_S("example.com:80");
    Str sub = s->use_tls ? BURROW_S(".example.com:443") : BURROW_S(".example.com:80");
    if (str_eq(addr, host) || strings_has_suffix(addr, sub)) {
        NetAddr la = s->listener.vt->addr(s->listener.data);
        addr = la.vt->string(la.data, arena_allocator(&ar));
    }
    NetDialer d;
    memset(&d, 0, sizeof d);
    NetConn c = net_dialer_dial_context(&d, heap_allocator(), ctx, network, addr, err);
    arena_free(&ar);
    return c;
}

/* A server with nothing started and no listener. */
static HttptestServer *ts_new(Alloc *a, HttpHandler handler) {
    Alloc *sa = a != NULL ? a : heap_allocator();
    HttptestServer *s =
        (HttptestServer *)mem_alloc(sa, sizeof *s, _Alignof(HttptestServer));
    if (s == NULL)
        panic_str(BURROW_S("httptest: out of memory"));
    s->a = a;
    arena_init(&s->arena, sa, 0);
    s->config.handler = handler;
    s->config.a = a;
    return s;
}

HttptestServer *httptest_new_unstarted_server(Alloc *a, HttpHandler handler) {
    HttptestServer *s = ts_new(a, handler);
    s->listener = ts_new_local_listener(ts_alloc(s));
    return s;
}

static bool ts_is_abort_handler(Any p) {
    if (p.t != TYPE_ERROR || p.data == NULL)
        return false;
    const Error *e = (const Error *)p.data;
    return e->vt == http_err_abort_handler.vt && e->data == http_err_abort_handler.data;
}

/* testServerHandler.ServeHTTP. */
static void ts_test_handler(void *env, HttpResponseWriter w, HttpRequest *req) {
    ts_Fake *f = (ts_Fake *)env;
    Alloc *h = heap_allocator();
    volatile bool panicked = false;
    volatile bool abort_handler = false;
    Str volatile text = {NULL, 0};
    BURROW_TRY {
        if (f->h.vt != NULL)
            http_handler_serve_http(f->h, w, req);
        else
            http_response_writer_write_header(w, 500);
    }
    BURROW_CATCH(p) {
        panicked = true;
        abort_handler = ts_is_abort_handler(p);
        if (!abort_handler)
            text = str_clone(h, panic_text(p));
    }
    BURROW_TRY_END;
    if (!panicked)
        return;
    if (!abort_handler) {
        /* This is the same logging http.Server would do,
         * but we can put it into the test output rather than stderr. */
        enum { size = 64 << 10 };
        Byte *buf = (Byte *)mem_alloc(h, size, 1);
        Int n = buf == NULL
                    ? 0
                    : runtime_stack(slice_from(buf, size, size, TYPE_BYTE), false);
        Str msg = text;
        TestingTB t = f->s->t;
        t.vt->errorf(t.data,
                     BURROW__FMT_FARGS(BURROW_ANY_OF,
                                       "httptest: panic in server handler: %v\n%s", msg,
                                       str_from_bytes(buf, n)));
        mem_free(h, buf, size, 1);
        if (msg.p != NULL)
            mem_free(h, (void *)(uintptr_t)msg.p, (size_t)msg.len, 1);
    }
    /* Convert panic to ErrAbortHandler to suppress http.Server's logging. */
    panic(BURROW_ANY(TYPE_ERROR, (void *)(uintptr_t)&http_err_abort_handler));
}

static void ts_free(HttptestServer *s);

static void ts_cleanup(void *env) {
    ts_free((HttptestServer *)env);
}

HttptestServer *httptest_new_test_server(Alloc *a, TestingTB t, HttpHandler handler) {
    HttptestServer *s = ts_new(a, handler);
    Alloc *sa = ts_alloc(s);
    ts_Fake *f = (ts_Fake *)mem_alloc(sa, sizeof *f, _Alignof(ts_Fake));
    if (f == NULL) {
        arena_free(&s->arena);
        mem_free(sa, s, sizeof *s, _Alignof(HttptestServer));
        panic_str(BURROW_S("httptest: out of memory"));
    }
    f->s = s;
    f->h = handler;
    f->fn = BURROW_FN(HttpHandlerFunc, ts_test_handler, f);
    f->li.f = f;
    f->tls_li.f = f;
    s->t = t;
    s->fake = f;
    s->config.handler = http_handler_func_as_handler(&f->fn);
    t.vt->cleanup(t.data, BURROW_FN(Func, ts_cleanup, s));
    return s;
}

/* goServe's goroutine. */
static void ts_serve(void *env) {
    HttptestServer *s = (HttptestServer *)env;
    (void)http_server_serve(&s->config, s->listener);
    sync_wait_group_done(&s->wg);
}

/* goServe's goroutine, for a listener of the network in memory. */
static void ts_serve_fake(void *env) {
    ts_Serve *sv = (ts_Serve *)env;
    (void)http_server_serve(&sv->s->config, sv->l);
    sync_wait_group_done(&sv->s->wg);
}

static void ts_nothing(void *env) {
    (void)env;
}

/* startCommon. */
static void ts_start_common(HttptestServer *s, bool use_loopback) {
    sync_mutex_lock(&s->mu);
    if (s->started) {
        sync_mutex_unlock(&s->mu);
        panic_str(BURROW_S("Server already started"));
    }
    if (s->closed) {
        sync_mutex_unlock(&s->mu);
        panic_str(BURROW_S("Start of closed Server"));
    }
    s->started = true;
    if (s->fake != NULL && use_loopback) {
        /* We're being called from Start or StartTLS.
         * Don't try to start the server again when Client is called. */
        sync_once_do(&s->start_once, BURROW_FN(Func, ts_nothing, NULL));

        /* NewTestServer servers create their listener at start time.
         *
         * We might want to permit the user to provide their own Listener
         * in the future. For now, we panic. */
        if (s->listener.vt != NULL) {
            sync_mutex_unlock(&s->mu);
            panic_str(BURROW_S("Server.Listener is unexpectedly set"));
        }
        s->listener = ts_new_local_listener(ts_alloc(s));
    }
    /* wrap. */
    s->old_hook = s->config.conn_state;
    s->config.conn_state = BURROW_FN(HttpConnStateFunc, ts_conn_state, s);
    sync_mutex_unlock(&s->mu);
}

/* Sets url to scheme, "://" and the address the listener is on. */
static void ts_set_url(HttptestServer *s, Str scheme) {
    Alloc *ua = arena_allocator(&s->arena);
    NetAddr la = s->listener.vt->addr(s->listener.data);
    Str addr = la.vt->string(la.data, ua);
    Str url = fmt_sprintf_v(ua, "%s://%s", scheme, addr);
    if (addr.len == 0 || url.len == 0)
        panic_str(BURROW_S("httptest: out of memory"));
    s->url = url;
}

/* goServe. */
static void ts_go_serve(HttptestServer *s) {
    sync_wait_group_add(&s->wg, 1);
    if (!go(BURROW_FN(Func, ts_serve, s))) {
        sync_wait_group_done(&s->wg);
        panic_str(BURROW_S("httptest: out of memory"));
    }
}

/* goServe, for a listener of the network in memory. */
static void ts_go_serve_fake(HttptestServer *s, ts_Serve *sv, NetListener l) {
    sv->s = s;
    sv->l = l;
    sync_wait_group_add(&s->wg, 1);
    if (!go(BURROW_FN(Func, ts_serve_fake, sv))) {
        sync_wait_group_done(&s->wg);
        panic_str(BURROW_S("httptest: out of memory"));
    }
}

void httptest_server_start(HttptestServer *s) {
    ts_start_common(s, true);

    s->transport.a = s->a;
    s->transport.dial_context = BURROW_FN(HttpDialContextFunc, ts_dial, s);
    s->client.transport = http_transport_as_round_tripper(&s->transport);
    s->client.a = s->a;

    ts_set_url(s, BURROW_S("http"));
    ts_go_serve(s);
}

/* Not in Go: a Str as the []byte Go would convert it to. */
static Slice ts_bytes_of(Str s) {
    return slice_from((void *)(uintptr_t)s.p, s.len, s.len, TYPE_BYTE);
}

/* initTLS. The client's config, or NULL with *err set. What it makes is made
 * in the server's arena. */
static TlsConfig *ts_init_tls(HttptestServer *s, Error *err) {
    BURROW_OUT(err, BURROW_NO_ERROR);
    Alloc *a = arena_allocator(&s->arena);
    TlsCertificate cert =
        tls_x509_key_pair(a, ts_bytes_of(burrow__http_testcert_localhost_cert),
                          ts_bytes_of(burrow__http_testcert_localhost_key), err);
    if (BURROW_FAILED(*err))
        return NULL;

    TlsConfig *existing_config = s->tls;
    TlsConfig *config;
    if (existing_config != NULL) {
        config = tls_config_clone(existing_config, a);
    } else {
        config = (TlsConfig *)mem_alloc(a, sizeof *config, _Alignof(TlsConfig));
        if (config != NULL)
            memset(config, 0, sizeof *config);
    }
    if (config == NULL) {
        *err = burrow_err_out_of_memory;
        return NULL;
    }
    s->tls = config;
    s->own_tls = config;
    if (s->tls->next_protos.p == NULL) {
        Str *next_protos = (Str *)mem_alloc(a, sizeof *next_protos, _Alignof(Str));
        if (next_protos == NULL) {
            *err = burrow_err_out_of_memory;
            return NULL;
        }
        *next_protos = BURROW_S("http/1.1");
        if (s->enable_http2)
            *next_protos = BURROW_S("h2");
        s->tls->next_protos = slice_from(next_protos, 1, 1, TYPE_STRING);
    }
    if (s->tls->certificates.len == 0) {
        TlsCertificate *certs =
            (TlsCertificate *)mem_alloc(a, sizeof *certs, _Alignof(TlsCertificate));
        if (certs == NULL) {
            *err = burrow_err_out_of_memory;
            return NULL;
        }
        *certs = cert;
        s->tls->certificates = slice_from(certs, 1, 1, TYPE_TLS_CERTIFICATE);
    }
    const TlsCertificate *c0 =
        (const TlsCertificate *)slice_at(s->tls->certificates, 0);
    s->certificate =
        x509_parse_certificate(a, *(const Slice *)slice_at(c0->certificate, 0), err);
    if (BURROW_FAILED(*err))
        return NULL;
    X509CertPool *certpool = x509_new_cert_pool(a);
    TlsConfig *client = (TlsConfig *)mem_alloc(a, sizeof *client, _Alignof(TlsConfig));
    if (certpool == NULL || client == NULL) {
        x509_cert_pool_free(certpool);
        *err = burrow_err_out_of_memory;
        return NULL;
    }
    s->certpool = certpool;
    x509_cert_pool_add_cert(certpool, s->certificate);
    memset(client, 0, sizeof *client);
    client->root_cas = certpool;
    s->client_tls = client;
    return client;
}

void httptest_server_start_tls(HttptestServer *s) {
    ts_start_common(s, true);

    Error err = BURROW_NO_ERROR;
    TlsConfig *tls_client_config = ts_init_tls(s, &err);
    if (BURROW_FAILED(err)) {
        Str msg = fmt_sprintf_v(heap_allocator(), "httptest: NewTLSServer: %v", err);
        panic_str(msg);
    }

    s->transport.a = s->a;
    s->transport.tls_client_config = tls_client_config;
    s->transport.force_attempt_http2 = s->enable_http2;
    s->client.transport = http_transport_as_round_tripper(&s->transport);
    s->client.a = s->a;

    s->use_tls = true;
    s->transport.dial_context = BURROW_FN(HttpDialContextFunc, ts_dial, s);
    NetListener l = tls_new_listener(ts_alloc(s), s->listener, s->tls);
    if (l.vt == NULL)
        panic_str(BURROW_S("httptest: out of memory"));
    s->inner = s->listener;
    s->listener = l;
    ts_set_url(s, BURROW_S("https"));
    ts_go_serve(s);
}

/* Keeps c, the server end of a connection of the network in memory, for
 * httptest_server_free. False when the allocator says no. */
static bool ts_fake_keep(ts_Fake *f, NettestConn *c) {
    bool ok = true;
    sync_mutex_lock(&f->mu);
    if (f->nsrv == f->cap_srv) {
        Alloc *a = ts_alloc(f->s);
        Int ncap = f->cap_srv == 0 ? 8 : f->cap_srv * 2;
        NettestConn **srv = (NettestConn **)mem_alloc(a, (size_t)ncap * sizeof *srv,
                                                      _Alignof(NettestConn *));
        if (srv == NULL) {
            ok = false;
        } else {
            if (f->nsrv > 0)
                memcpy(srv, f->srv, (size_t)f->nsrv * sizeof *srv);
            if (f->srv != NULL)
                mem_free(a, f->srv, (size_t)f->cap_srv * sizeof *srv,
                         _Alignof(NettestConn *));
            f->srv = srv;
            f->cap_srv = ncap;
        }
    }
    if (ok)
        f->srv[f->nsrv++] = c;
    sync_mutex_unlock(&f->mu);
    return ok;
}

static Error ts_fl_close(void *self) {
    return burrow__nettest_listener_close(((ts_FakeListener *)self)->li);
}

static NetConn ts_fl_accept(void *self, Error *err) {
    ts_FakeListener *fl = (ts_FakeListener *)self;
    for (;;) {
        NettestConn *c = burrow__nettest_listener_accept(fl->li, err);
        if (c == NULL || ts_fake_keep(fl->f, c))
            return burrow__nettest_conn_as_net_conn(c);
        /* Not kept, so closed now rather than served. */
        (void)burrow__nettest_conn_close(c);
        burrow__nettest_conn_free(c);
    }
}

static NetAddr ts_fl_addr(void *self) {
    return burrow__nettest_listener_addr(((ts_FakeListener *)self)->li);
}

static const Type ts_fake_listener_desc = {
    BURROW_S_INIT("fakeListener"),
    BURROW_S_INIT("net/http/httptest"),
    KIND_STRUCT,
    (uint32_t)sizeof(ts_FakeListener),
    (uint16_t)_Alignof(ts_FakeListener),
    0,
    0,
    NULL,
    NULL,
    NULL,
    NULL,
    0,
    0x6874666cU, /* "htfl" */
    NULL,
};

static const NetListenerVT ts_fake_listener_vt = {
    {&ts_fake_listener_desc, ts_fl_close},
    ts_fl_accept,
    ts_fl_addr,
};

static NetListener ts_fl_as_listener(ts_FakeListener *fl) {
    NetListener l = {&ts_fake_listener_vt, fl};
    return l;
}

/* The transport's DialContext on the network in memory. */
static NetConn ts_fake_dial(void *env, Context ctx, Str network, Str address,
                            Error *err) {
    ts_Fake *f = (ts_Fake *)env;
    (void)ctx;
    (void)network;
    (void)address;
    BURROW_OUT(err, BURROW_NO_ERROR);
    NettestConn *c = burrow__nettest_listener_new_conn(f->li.li);
    if (c == NULL)
        BURROW_OUT(err, burrow_err_out_of_memory);
    return burrow__nettest_conn_as_net_conn(c);
}

/* The transport's DialTLSContext on the network in memory. */
static NetConn ts_fake_dial_tls(void *env, Context ctx, Str network, Str address,
                                Error *err) {
    ts_Fake *f = (ts_Fake *)env;
    (void)ctx;
    (void)network;
    (void)address;
    BURROW_OUT(err, BURROW_NO_ERROR);
    NetConn nc = {NULL, NULL};
    NettestConn *c = burrow__nettest_listener_new_conn(f->tls_li.li);
    if (c == NULL) {
        BURROW_OUT(err, burrow_err_out_of_memory);
        return nc;
    }
    TlsConn *tc = tls_client(ts_alloc(f->s), burrow__nettest_conn_as_net_conn(c),
                             f->s->transport.tls_client_config);
    if (tc == NULL) {
        (void)burrow__nettest_conn_close(c);
        burrow__nettest_conn_free(c);
        BURROW_OUT(err, burrow_err_out_of_memory);
        return nc;
    }
    return tls_conn_as_net_conn(tc);
}

/* Not in Go: the transport's free_conn, which gives back the client end of a
 * connection of the network in memory, and the TLS client over it. */
static void ts_fake_free_conn(void *env, NetConn c) {
    (void)env;
    TlsConn *tc = tls_conn_from_net_conn(c);
    if (tc != NULL) {
        c = tls_conn_net_conn(tc);
        tls_conn_free(tc);
    }
    NettestConn *nc = burrow__nettest_conn_from_net_conn(c);
    if (nc != NULL) {
        (void)burrow__nettest_conn_close(nc);
        burrow__nettest_conn_free(nc);
    } else {
        net_conn_free(c);
    }
}

/* startFakeNet. */
static void ts_start_fake_net(void *env) {
    HttptestServer *s = (HttptestServer *)env;
    ts_Fake *f = s->fake;
    ts_start_common(s, false);

    Error err = BURROW_NO_ERROR;
    TlsConfig *tls_client_config = ts_init_tls(s, &err);
    if (BURROW_FAILED(err)) {
        Str msg = fmt_sprintf_v(heap_allocator(), "httptest: NewTestServer: %v", err);
        panic_str(msg);
    }

    s->transport.a = s->a;
    s->transport.tls_client_config = tls_client_config;
    s->transport.force_attempt_http2 = s->enable_http2;
    s->client.transport = http_transport_as_round_tripper(&s->transport);
    s->client.a = s->a;

    Alloc *a = ts_alloc(s);
    f->li.li = burrow__nettest_new_listener(a);
    f->tls_li.li = burrow__nettest_new_listener(a);
    if (f->li.li == NULL || f->tls_li.li == NULL)
        panic_str(BURROW_S("httptest: out of memory"));

    /* Set InsecureSkipVerify rather than depending on a specific server hostname. */
    tls_client_config->insecure_skip_verify = true;
    if (s->enable_http2) {
        /* Not in Go: Go's transport puts "h2" in TLSClientConfig.NextProtos
         * for the TLS connections DialTLSContext makes, and the transport here
         * leaves the config alone, so it is done here. */
        Str *next_protos = (Str *)mem_alloc(arena_allocator(&s->arena),
                                            2 * sizeof *next_protos, _Alignof(Str));
        if (next_protos == NULL)
            panic_str(BURROW_S("httptest: out of memory"));
        next_protos[0] = BURROW_S("h2");
        next_protos[1] = BURROW_S("http/1.1");
        tls_client_config->next_protos = slice_from(next_protos, 2, 2, TYPE_STRING);
    }
    s->transport.dial_context = BURROW_FN(HttpDialContextFunc, ts_fake_dial, f);
    s->transport.dial_tls_context = BURROW_FN(HttpDialContextFunc, ts_fake_dial_tls, f);
    s->transport.free_conn = BURROW_FN(HttpFreeConnFunc, ts_fake_free_conn, NULL);
    s->url = BURROW_S("http://example.com");
    ts_go_serve_fake(s, &f->serve[0], ts_fl_as_listener(&f->li));
    f->tls = tls_new_listener(a, ts_fl_as_listener(&f->tls_li), s->tls);
    if (f->tls.vt == NULL)
        panic_str(BURROW_S("httptest: out of memory"));
    ts_go_serve_fake(s, &f->serve[1], f->tls);
}

HttptestServer *httptest_new_server(Alloc *a, HttpHandler handler) {
    HttptestServer *s = httptest_new_unstarted_server(a, handler);
    httptest_server_start(s);
    return s;
}

HttptestServer *httptest_new_tls_server(Alloc *a, HttpHandler handler) {
    HttptestServer *ts = httptest_new_unstarted_server(a, handler);
    httptest_server_start_tls(ts);
    return ts;
}

/* logCloseHangDebugInfo, and what Close needs to know it is over. */
typedef struct ts_Hang {
    HttptestServer *s;
    SyncWaitGroup wg;
} ts_Hang;

static void ts_log_close_hang(void *env) {
    ts_Hang *h = (ts_Hang *)env;
    HttptestServer *s = h->s;
    Arena ar;
    arena_init(&ar, heap_allocator(), 0);
    Alloc *a = arena_allocator(&ar);
    sync_mutex_lock(&s->mu);
    Str text = BURROW_S(
        "httptest.Server blocked in Close after 5 seconds, waiting for connections:\n");
    for (Int i = 0; i < s->nconns; i++) {
        NetConn c = s->conns[i].c;
        NetAddr ra = c.vt->remote_addr(c.data);
        Str tn = c.vt->reader.self_type != NULL ? type_name(c.vt->reader.self_type)
                                                : BURROW_S("NetConn");
        Str ras = ra.vt != NULL ? ra.vt->string(ra.data, a) : BURROW_S("<nil>");
        text = fmt_sprintf_v(a, "%s  %s %p %s in state %s\n", text, tn, c.data, ras,
                             http_conn_state_string(s->conns[i].st));
    }
    sync_mutex_unlock(&s->mu);
    log_print_v("%s", text);
    arena_free(&ar);
    sync_wait_group_done(&h->wg);
}

void httptest_server_close(HttptestServer *s) {
    ts_Hang hang;
    memset(&hang, 0, sizeof hang);
    hang.s = s;
    TimeTimer *t = NULL;
    sync_mutex_lock(&s->mu);
    if (!s->closed) {
        s->closed = true;
        if (s->listener.vt != NULL)
            (void)s->listener.vt->closer.close(s->listener.data);
        if (s->fake != NULL && s->fake->li.li != NULL) {
            (void)burrow__nettest_listener_close(s->fake->li.li);
            (void)burrow__nettest_listener_close(s->fake->tls_li.li);
        }
        http_server_set_keep_alives_enabled(&s->config, false);
        for (Int i = 0; i < s->nconns; i++) {
            HttpConnState st = s->conns[i].st;
            if (st == HTTP_STATE_IDLE || st == HTTP_STATE_NEW)
                ts_close_conn(s->conns[i].c);
        }
        sync_wait_group_add(&hang.wg, 1);
        t = time_after_func(heap_allocator(), 5 * TIME_SECOND,
                            BURROW_FN(Func, ts_log_close_hang, &hang));
        if (t == NULL)
            sync_wait_group_done(&hang.wg);
    }
    sync_mutex_unlock(&s->mu);

    http_transport_close_idle_connections(http_default_transport);
    if (s->started)
        http_transport_close_idle_connections(&s->transport);
    sync_wait_group_wait(&s->wg);

    if (t != NULL) {
        if (time_timer_stop(t))
            sync_wait_group_done(&hang.wg);
        time_timer_free(t);
    }
    sync_wait_group_wait(&hang.wg);
}

void httptest_server_close_client_connections(HttptestServer *s) {
    sync_mutex_lock(&s->mu);
    for (Int i = 0; i < s->nconns; i++)
        ts_close_conn(s->conns[i].c);
    sync_mutex_unlock(&s->mu);
}

X509Certificate *httptest_server_certificate(HttptestServer *s) {
    return s->certificate;
}

HttpClient *httptest_server_client(HttptestServer *s) {
    if (s->fake != NULL)
        sync_once_do(&s->start_once, BURROW_FN(Func, ts_start_fake_net, s));
    return &s->client;
}

/* Gives back what httptest_new_test_server added: the network in memory and
 * the server ends of its connections, which the server is done with by now. */
static void ts_fake_free(HttptestServer *s, Alloc *a) {
    ts_Fake *f = s->fake;
    for (Int i = 0; i < f->nsrv; i++)
        burrow__nettest_conn_free(f->srv[i]);
    if (f->srv != NULL)
        mem_free(a, f->srv, (size_t)f->cap_srv * sizeof *f->srv,
                 _Alignof(NettestConn *));
    tls_listener_free(f->tls);
    burrow__nettest_listener_free(f->li.li);
    burrow__nettest_listener_free(f->tls_li.li);
    mem_free(a, f, sizeof *f, _Alignof(ts_Fake));
}

void httptest_server_free(HttptestServer *s) {
    if (s == NULL)
        return;
    if (s->fake != NULL) {
        /* The cleanup gives it back. */
        httptest_server_close(s);
        return;
    }
    ts_free(s);
}

static void ts_free(HttptestServer *s) {
    httptest_server_close(s);
    Alloc *a = ts_alloc(s);
    http_server_free(&s->config);
    http_transport_free(&s->transport);
    if (s->fake != NULL)
        ts_fake_free(s, a);
    if (s->use_tls) {
        tls_listener_free(s->listener);
        net_listener_free(s->inner);
    } else {
        net_listener_free(s->listener);
    }
    x509_cert_pool_free(s->certpool);
    if (s->client_tls != NULL)
        tls_config_release(s->client_tls);
    if (s->own_tls != NULL)
        tls_config_release(s->own_tls);
    if (s->conns != NULL)
        mem_free(a, s->conns, (size_t)s->cap_conns * sizeof *s->conns,
                 _Alignof(ts_Conn));
    arena_free(&s->arena);
    mem_free(a, s, sizeof *s, _Alignof(HttptestServer));
}
