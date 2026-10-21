/* Derived from Go's src/net/http/serve_test.go.
 * Go source: go1.27.1.
 *
 * Server.ServeTLS, and how Serve and ListenAndServeTLS turn HTTP/2 on. Go's
 * CondSkipHTTP2 is left out, since HTTP/2 can't be built out of burrow, and so
 * is afterTest, which looks for goroutines Go's transports leave behind.
 *
 * Copyright 2010 The Go Authors. All rights reserved.
 * Copyright 2026 The burrow Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style licence that can be found
 * in the LICENSE file. */

#include "check.h"

#include "../src/net/http_internal.h"

#include "burrow/burrow.h"
#include "burrow/chan.h"
#include "burrow/crypto/tls.h"
#include "burrow/map.h"
#include "burrow/mem/arena.h"
#include "burrow/mem/heap.h"
#include "burrow/net.h"
#include "burrow/net/http.h"
#include "burrow/sync.h"

#include <stdint.h>
#include <string.h>

#if defined(BURROW_NETPOLL_READINESS) && !defined(BURROW_OS_WASI)
#define HAVE_TCP 1
#endif

/* Not in Go: the tests below listen and dial on loopback, which needs the
 * readiness poll FD. */
static void need_tcp(TestingT *t) {
#if !defined(HAVE_TCP)
    testing_t_skip_v(t, "TCP here needs the readiness poll FD");
#else
    (void)t;
#endif
}

static void set_parallel(TestingT *t) {
    if (testing_short())
        testing_t_parallel(t);
}

static NetListener new_local_listener(TestingT *t) {
    testing_t_helper(t);
    Error err = BURROW_NO_ERROR;
    NetListener ln =
        net_listen(heap_allocator(), BURROW_S("tcp"), BURROW_S("127.0.0.1:0"), &err);
    if (BURROW_FAILED(err)) {
        err = BURROW_NO_ERROR;
        ln = net_listen(heap_allocator(), BURROW_S("tcp6"), BURROW_S("[::1]:0"), &err);
    }
    if (BURROW_FAILED(err)) {
        testing_t_fatal_v(t, err);
        return (NetListener){NULL, NULL};
    }
    return ln;
}

/* Not in Go: ln.Addr().String(), made in a. */
static Str listener_addr(Alloc *a, NetListener ln) {
    NetAddr addr = ln.vt->addr(ln.data);
    return addr.vt->string(addr.data, a);
}

/* Not in Go: a Str as the []byte Go would convert it to. */
static Slice bytes_of(Str s) {
    return slice_from((void *)(uintptr_t)s.p, s.len, s.len, TYPE_BYTE);
}

/* Not in Go: tls.X509KeyPair(testcert.LocalhostCert, testcert.LocalhostKey),
 * made in a. */
static TlsCertificate localhost_key_pair(Alloc *a, Error *err) {
    return tls_x509_key_pair(a, bytes_of(burrow__http_testcert_localhost_cert),
                             bytes_of(burrow__http_testcert_localhost_key), err);
}

/* Not in Go: SetTestHookServerServe(nil). */
static void clear_test_hook(void) {
    burrow__HttpTestHookServerServe none = {NULL, NULL};
    burrow__http_set_test_hook_server_serve(none);
}

/* Not in Go: s.TLSNextProto["h2"] != nil. */
static bool tls_next_proto_h2(HttpServer *s) {
    Str key = BURROW_S("h2");
    const HttpTLSNextProtoFunc *fn =
        (const HttpTLSNextProtoFunc *)map_get(s->tls_next_proto, &key);
    return fn != NULL && fn->f != NULL;
}

/* Not in Go: the tls.Config the tests dial with, which skips verifying and
 * offers h2 and http/1.1. next_protos is two Strs that live as long as it. */
static void insecure_client_config(TlsConfig *c, Str next_protos[2]) {
    memset(c, 0, sizeof *c);
    next_protos[0] = BURROW_S("h2");
    next_protos[1] = BURROW_S("http/1.1");
    c->insecure_skip_verify = true;
    c->next_protos = slice_from(next_protos, 2, 2, TYPE_STRING);
}

/* Not in Go: the checks on the client's ConnectionState that TestServeTLS and
 * testAutomaticHTTP2_ListenAndServe share. */
static void check_negotiated_h2(TestingT *t, TlsConn *c) {
    TlsConnectionState cs = tls_conn_connection_state(c);
    Str got = cs.negotiated_protocol;
    Str want = BURROW_S("h2");
    if (!str_eq(got, want))
        testing_t_errorf_v(t, "NegotiatedProtocol = %q; want %q", got, want);
    bool got_mutual = cs.negotiated_protocol_is_mutual;
    bool want_mutual = true;
    if (got_mutual != want_mutual)
        testing_t_errorf_v(t, "NegotiatedProtocolIsMutual = %v; want %v", got_mutual,
                           want_mutual);
}

/* ------------------------------------------------------------- ServeTLS */

/* Not in Go: the goroutine of TestServeTLS. */
typedef struct ServeTLSJob {
    HttpServer *s;
    NetListener ln;
    Chan *errc;
} ServeTLSJob;

static void serve_tls_job(void *env) {
    ServeTLSJob *j = (ServeTLSJob *)env;
    Error err = http_server_serve_tls(j->s, j->ln, BURROW_STR_EMPTY, BURROW_STR_EMPTY);
    chan_send(j->errc, &err);
}

static void serving_hook(void *env, HttpServer *s, NetListener ln) {
    (void)s;
    (void)ln;
    bool v = true;
    chan_send((Chan *)env, &v);
}

static void handler_nothing(void *env, HttpResponseWriter w, HttpRequest *r) {
    (void)env;
    (void)w;
    (void)r;
}

static void TestServeTLS(TestingT *t) {
    need_tcp(t);
    /* Not parallel: uses global test hooks. */

    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    Error err = BURROW_NO_ERROR;
    TlsCertificate cert = localhost_key_pair(a, &err);
    if (BURROW_FAILED(err)) {
        arena_free(&ar);
        testing_t_fatal_v(t, err);
    }
    TlsConfig tls_conf;
    memset(&tls_conf, 0, sizeof tls_conf);
    tls_conf.certificates = slice_from(&cert, 1, 1, TYPE_TLS_CERTIFICATE);

    NetListener ln = new_local_listener(t);
    Str addr = listener_addr(a, ln);

    Chan *serving = chan_make(heap_allocator(), TYPE_BOOL, 1);
    Chan *errc = chan_make(heap_allocator(), TYPE_ERROR, 1);
    if (serving == NULL || errc == NULL)
        testing_t_fatal_v(t, "out of memory");
    burrow__http_set_test_hook_server_serve(
        BURROW_FN(burrow__HttpTestHookServerServe, serving_hook, serving));
    HttpHandlerFunc handler = BURROW_FN(HttpHandlerFunc, handler_nothing, NULL);
    HttpServer s;
    memset(&s, 0, sizeof s);
    s.addr = addr;
    s.tls_config = &tls_conf;
    s.handler = http_handler_func_as_handler(&handler);
    ServeTLSJob job = {&s, ln, errc};
    SyncWaitGroup wg = {0};
    sync_wait_group_go(&wg, BURROW_FN(Func, serve_tls_job, &job));

    /* Not in Go: the failures below are errors that skip to the end rather
     * than fatal ones, so that the goroutine is waited for before the server it
     * uses goes away, which Go's deferred calls do not have to do. */
    TlsConn *c = NULL;
    TlsConfig client_conf;
    Str client_protos[2];
    insecure_client_config(&client_conf, client_protos);
    Error serve_err = BURROW_NO_ERROR;
    SelectCase cases[2] = {BURROW_RECV(errc, &serve_err), BURROW_RECV(serving, NULL)};
    if (chan_select(cases, 2) == 0) {
        testing_t_errorf_v(t, "ServeTLS: %v", serve_err);
        goto done;
    }

    c = tls_dial(heap_allocator(), BURROW_S("tcp"), listener_addr(a, ln), &client_conf,
                 &err);
    if (BURROW_FAILED(err)) {
        testing_t_error_v(t, err);
        c = NULL;
        goto done;
    }
    check_negotiated_h2(t, c);

done:
    if (c != NULL) {
        (void)tls_conn_close(c);
        tls_conn_free(c);
    }
    (void)ln.vt->closer.close(ln.data);
    clear_test_hook();
    /* Not in Go: waits for ServeTLS and the connection it served, and gives
     * back what they used. */
    sync_wait_group_wait(&wg);
    http_server_free(&s);
    net_listener_free(ln);
    tls_config_release(&client_conf);
    tls_config_release(&tls_conf);
    chan_free(errc);
    chan_free(serving);
    arena_free(&ar);
}

/* ------------------------------------------------ Serve turns HTTP/2 on */

static void test_automatic_http2_serve(TestingT *t, TlsConfig *tls_conf, bool want_h2) {
    set_parallel(t);
    need_tcp(t);
    NetListener ln = new_local_listener(t);
    (void)ln.vt->closer.close(ln.data); /* immediately (not a defer!) */
    HttpServer s;
    memset(&s, 0, sizeof s);
    s.tls_config = tls_conf;
    if (!BURROW_FAILED(http_server_serve(&s, ln)))
        testing_t_fatal_v(t, "expected an error");
    bool got_h2 = tls_next_proto_h2(&s);
    if (got_h2 != want_h2)
        testing_t_errorf_v(t, "http2 configured = %v; want %v", got_h2, want_h2);
    http_server_free(&s);
    net_listener_free(ln);
}

/* Issue 15908 */
static void TestAutomaticHTTP2_Serve_NoTLSConfig(TestingT *t) {
    test_automatic_http2_serve(t, NULL, true);
}

static void TestAutomaticHTTP2_Serve_NonH2TLSConfig(TestingT *t) {
    TlsConfig conf;
    memset(&conf, 0, sizeof conf);
    test_automatic_http2_serve(t, &conf, false);
}

static void TestAutomaticHTTP2_Serve_H2TLSConfig(TestingT *t) {
    Str protos[1] = {BURROW_S("h2")};
    TlsConfig conf;
    memset(&conf, 0, sizeof conf);
    conf.next_protos = slice_from(protos, 1, 1, TYPE_STRING);
    test_automatic_http2_serve(t, &conf, true);
}

static void TestAutomaticHTTP2_Serve_WithTLSConfig(TestingT *t) {
    set_parallel(t);
    need_tcp(t);
    NetListener ln = new_local_listener(t);
    (void)ln.vt->closer.close(ln.data); /* immediately (not a defer!) */
    HttpServer s;
    memset(&s, 0, sizeof s);
    /* Set the TLSConfig. In reality, this would be the
     * *tls.Config given to tls.NewListener. */
    Str protos[1] = {BURROW_S("h2")};
    TlsConfig conf;
    memset(&conf, 0, sizeof conf);
    conf.next_protos = slice_from(protos, 1, 1, TYPE_STRING);
    s.tls_config = &conf;
    if (!BURROW_FAILED(http_server_serve(&s, ln)))
        testing_t_fatal_v(t, "expected an error");
    bool on = tls_next_proto_h2(&s);
    if (!on)
        testing_t_errorf_v(t, "http2 wasn't automatically enabled");
    http_server_free(&s);
    net_listener_free(ln);
}

/* ------------------------------------- ListenAndServeTLS turns HTTP/2 on */

/* Not in Go: one try of testAutomaticHTTP2_ListenAndServe, with the Server it
 * makes and the goroutine that runs ListenAndServeTLS. A NetListener is two
 * words, so the hook keeps the one it is given here and lnc only says that it
 * has. */
typedef struct ListenAndServeTry {
    HttpServer s;
    Chan *errc;
    Chan *lnc;
    NetListener ln;
    SyncWaitGroup wg;
} ListenAndServeTry;

static void listen_and_serve_tls_job(void *env) {
    ListenAndServeTry *j = (ListenAndServeTry *)env;
    Error err =
        http_server_listen_and_serve_tls(&j->s, BURROW_STR_EMPTY, BURROW_STR_EMPTY);
    chan_send(j->errc, &err);
}

static void lnc_hook(void *env, HttpServer *s, NetListener ln) {
    ListenAndServeTry *j = (ListenAndServeTry *)env;
    (void)s;
    j->ln = ln;
    bool v = true;
    chan_send(j->lnc, &v);
}

/* Not in Go: waits for the try's goroutine and gives back all it holds. */
static void try_free(ListenAndServeTry *j) {
    sync_wait_group_wait(&j->wg);
    http_server_free(&j->s);
    chan_free(j->errc);
    chan_free(j->lnc);
    mem_free(heap_allocator(), j, sizeof *j, _Alignof(ListenAndServeTry));
}

static void test_automatic_http2_listen_and_serve(TestingT *t, TlsConfig *tls_conf) {
    need_tcp(t);
    /* Not parallel: uses global test hooks. */
    bool ok = false;
    ListenAndServeTry *s = NULL;
    enum { max_tries = 5 };
    NetListener ln = {NULL, NULL};
    Arena ar;
    arena_init(&ar, NULL, 0);
    for (int try = 0; try < max_tries; try++) {
        ln = new_local_listener(t);
        Str addr = listener_addr(arena_allocator(&ar), ln);
        net_listener_free(ln);
        testing_t_logf_v(t, "Got %v", addr);
        s = (ListenAndServeTry *)mem_alloc(heap_allocator(), sizeof *s,
                                           _Alignof(ListenAndServeTry));
        if (s == NULL)
            testing_t_fatal_v(t, "out of memory");
        memset(s, 0, sizeof *s);
        s->lnc = chan_make(heap_allocator(), TYPE_BOOL, 1);
        s->errc = chan_make(heap_allocator(), TYPE_ERROR, 1);
        if (s->lnc == NULL || s->errc == NULL)
            testing_t_fatal_v(t, "out of memory");
        burrow__http_set_test_hook_server_serve(
            BURROW_FN(burrow__HttpTestHookServerServe, lnc_hook, s));
        s->s.addr = addr;
        s->s.tls_config = tls_conf;
        sync_wait_group_go(&s->wg, BURROW_FN(Func, listen_and_serve_tls_job, s));
        Error err = BURROW_NO_ERROR;
        SelectCase cases[2] = {BURROW_RECV(s->errc, &err), BURROW_RECV(s->lnc, NULL)};
        if (chan_select(cases, 2) == 0) {
            int n = try + 1;
            testing_t_logf_v(t, "On try #%v: %v", n, err);
            try_free(s);
            s = NULL;
            continue;
        }
        ln = s->ln;
        ok = true;
        Str laddr = listener_addr(arena_allocator(&ar), ln);
        testing_t_logf_v(t, "Listening on %v", laddr);
        break;
    }
    if (!ok) {
        clear_test_hook();
        arena_free(&ar);
        testing_t_fatalf_v(t, "Failed to start up after %d tries", (int)max_tries);
    }

    /* Not in Go: as in TestServeTLS, a failure skips to the end so that the
     * goroutine is waited for. */
    TlsConfig client_conf;
    Str client_protos[2];
    insecure_client_config(&client_conf, client_protos);
    Error err = BURROW_NO_ERROR;
    TlsConn *c = tls_dial(heap_allocator(), BURROW_S("tcp"),
                          listener_addr(arena_allocator(&ar), ln), &client_conf, &err);
    if (BURROW_FAILED(err)) {
        testing_t_error_v(t, err);
        c = NULL;
    } else {
        check_negotiated_h2(t, c);
    }

    if (c != NULL) {
        (void)tls_conn_close(c);
        tls_conn_free(c);
    }
    (void)ln.vt->closer.close(ln.data);
    clear_test_hook();
    try_free(s);
    tls_config_release(&client_conf);
    arena_free(&ar);
}

static void TestAutomaticHTTP2_ListenAndServe(TestingT *t) {
    Arena ar;
    arena_init(&ar, NULL, 0);
    Error err = BURROW_NO_ERROR;
    TlsCertificate cert = localhost_key_pair(arena_allocator(&ar), &err);
    if (BURROW_FAILED(err)) {
        arena_free(&ar);
        testing_t_fatal_v(t, err);
    }
    TlsConfig conf;
    memset(&conf, 0, sizeof conf);
    conf.certificates = slice_from(&cert, 1, 1, TYPE_TLS_CERTIFICATE);
    test_automatic_http2_listen_and_serve(t, &conf);
    tls_config_release(&conf);
    arena_free(&ar);
}

static TlsCertificate *get_certificate(void *env, TlsClientHelloInfo *client_hello,
                                       Error *err) {
    (void)client_hello;
    (void)err;
    return (TlsCertificate *)env;
}

static void TestAutomaticHTTP2_ListenAndServe_GetCertificate(TestingT *t) {
    Arena ar;
    arena_init(&ar, NULL, 0);
    Error err = BURROW_NO_ERROR;
    TlsCertificate cert = localhost_key_pair(arena_allocator(&ar), &err);
    if (BURROW_FAILED(err)) {
        arena_free(&ar);
        testing_t_fatal_v(t, err);
    }
    TlsConfig conf;
    memset(&conf, 0, sizeof conf);
    conf.get_certificate = BURROW_FN(TlsGetCertificateFunc, get_certificate, &cert);
    test_automatic_http2_listen_and_serve(t, &conf);
    tls_config_release(&conf);
    arena_free(&ar);
}

static TlsConfig *get_config_for_client(void *env, TlsClientHelloInfo *client_hello,
                                        Error *err) {
    (void)client_hello;
    (void)err;
    return (TlsConfig *)env;
}

static void TestAutomaticHTTP2_ListenAndServe_GetConfigForClient(TestingT *t) {
    Arena ar;
    arena_init(&ar, NULL, 0);
    Error err = BURROW_NO_ERROR;
    TlsCertificate cert = localhost_key_pair(arena_allocator(&ar), &err);
    if (BURROW_FAILED(err)) {
        arena_free(&ar);
        testing_t_fatal_v(t, err);
    }
    Str protos[1] = {BURROW_S("h2")};
    TlsConfig conf;
    memset(&conf, 0, sizeof conf);
    /* GetConfigForClient requires specifying a full tls.Config so we must set
     * NextProtos ourselves. */
    conf.next_protos = slice_from(protos, 1, 1, TYPE_STRING);
    conf.certificates = slice_from(&cert, 1, 1, TYPE_TLS_CERTIFICATE);
    TlsConfig outer;
    memset(&outer, 0, sizeof outer);
    outer.get_config_for_client =
        BURROW_FN(TlsGetConfigForClientFunc, get_config_for_client, &conf);
    test_automatic_http2_listen_and_serve(t, &outer);
    tls_config_release(&outer);
    tls_config_release(&conf);
    arena_free(&ar);
}

#define TESTS(X)                                                                       \
    X(TestServeTLS)                                                                    \
    X(TestAutomaticHTTP2_Serve_NoTLSConfig)                                            \
    X(TestAutomaticHTTP2_Serve_NonH2TLSConfig)                                         \
    X(TestAutomaticHTTP2_Serve_H2TLSConfig)                                            \
    X(TestAutomaticHTTP2_Serve_WithTLSConfig)                                          \
    X(TestAutomaticHTTP2_ListenAndServe)                                               \
    X(TestAutomaticHTTP2_ListenAndServe_GetCertificate)                                \
    X(TestAutomaticHTTP2_ListenAndServe_GetConfigForClient)
TESTING_MAIN(TESTS)
