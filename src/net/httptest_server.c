/* net/http/httptest's Server: a server on a port of the loopback address for a
 * test to talk to.
 *
 * Derived from Go's src/net/http/httptest/server.go. Go source: go1.27.1.
 *
 * Left out until crypto/tls is here: StartTLS, NewTLSServer, Certificate and
 * NewTestServer, which serves on a network in memory and needs TLS as well.
 * The -httptest.serve flag is left out too.
 *
 * Copyright 2011 The Go Authors. All rights reserved.
 * Copyright 2026 The burrow Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style licence that can be found
 * in the LICENSE file. */

#include "burrow/net/http/httptest.h"

#include "burrow/context.h"
#include "burrow/core.h"
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
#include "burrow/strings.h"
#include "burrow/sync.h"
#include "burrow/time.h"

#include <stdbool.h>
#include <string.h>

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
    if (str_eq(addr, BURROW_S("example.com:80")) ||
        strings_has_suffix(addr, BURROW_S(".example.com:80"))) {
        NetAddr la = s->listener.vt->addr(s->listener.data);
        addr = la.vt->string(la.data, arena_allocator(&ar));
    }
    NetDialer d;
    memset(&d, 0, sizeof d);
    NetConn c = net_dialer_dial_context(&d, heap_allocator(), ctx, network, addr, err);
    arena_free(&ar);
    return c;
}

HttptestServer *httptest_new_unstarted_server(Alloc *a, HttpHandler handler) {
    Alloc *sa = a != NULL ? a : heap_allocator();
    HttptestServer *s =
        (HttptestServer *)mem_alloc(sa, sizeof *s, _Alignof(HttptestServer));
    if (s == NULL)
        panic_str(BURROW_S("httptest: out of memory"));
    s->a = a;
    arena_init(&s->arena, sa, 0);
    s->listener = ts_new_local_listener(sa);
    s->config.handler = handler;
    s->config.a = a;
    return s;
}

/* goServe's goroutine. */
static void ts_serve(void *env) {
    HttptestServer *s = (HttptestServer *)env;
    (void)http_server_serve(&s->config, s->listener);
    sync_wait_group_done(&s->wg);
}

void httptest_server_start(HttptestServer *s) {
    /* startCommon. */
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
    /* wrap. */
    s->old_hook = s->config.conn_state;
    s->config.conn_state = BURROW_FN(HttpConnStateFunc, ts_conn_state, s);
    sync_mutex_unlock(&s->mu);

    s->transport.a = s->a;
    s->transport.dial_context = BURROW_FN(HttpDialContextFunc, ts_dial, s);
    s->client.transport = http_transport_as_round_tripper(&s->transport);
    s->client.a = s->a;

    Alloc *ua = arena_allocator(&s->arena);
    NetAddr la = s->listener.vt->addr(s->listener.data);
    Str addr = la.vt->string(la.data, ua);
    Str url = fmt_sprintf_v(ua, "http://%s", addr);
    if (addr.len == 0 || url.len == 0)
        panic_str(BURROW_S("httptest: out of memory"));
    s->url = url;

    /* goServe. */
    sync_wait_group_add(&s->wg, 1);
    if (!go(BURROW_FN(Func, ts_serve, s))) {
        sync_wait_group_done(&s->wg);
        panic_str(BURROW_S("httptest: out of memory"));
    }
}

HttptestServer *httptest_new_server(Alloc *a, HttpHandler handler) {
    HttptestServer *s = httptest_new_unstarted_server(a, handler);
    httptest_server_start(s);
    return s;
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
        text =
            fmt_sprintf_v(a, "%s  %s %p %s in state %s\n", text, tn, c.data,
                          ra.vt != NULL ? ra.vt->string(ra.data, a) : BURROW_S("<nil>"),
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

HttpClient *httptest_server_client(HttptestServer *s) {
    return &s->client;
}

void httptest_server_free(HttptestServer *s) {
    if (s == NULL)
        return;
    httptest_server_close(s);
    Alloc *a = ts_alloc(s);
    http_server_free(&s->config);
    http_transport_free(&s->transport);
    net_listener_free(s->listener);
    if (s->conns != NULL)
        mem_free(a, s->conns, (size_t)s->cap_conns * sizeof *s->conns,
                 _Alignof(ts_Conn));
    arena_free(&s->arena);
    mem_free(a, s, sizeof *s, _Alignof(HttptestServer));
}
