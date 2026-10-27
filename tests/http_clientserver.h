/* Derived from Go's src/net/http/clientserver_test.go.
 * Go source: go1.27.3.
 *
 * The client and server test helpers the net/http tests share: the test
 * modes, run and run_synctest, which run a test once for each mode, and
 * new_client_server_test, which starts an httptest server in a mode with a
 * client to talk to it.
 *
 * Not in Go: the helpers take a TestingT, where Go's take any testing.TB,
 * and there is no HTTP/3 mode, which Go skips for now as well. Go's options
 * are a list of any, and here they are an RunOpts or a CSTOpts. A test whose
 * server is on the real network is skipped where burrow has no TCP. A handler
 * is still called while the server's cleanup closes it, after the test
 * function has returned, so what a handler uses has to come from test_alloc,
 * or be something else that lasts as long.
 *
 * Copyright 2015 The Go Authors. All rights reserved.
 * Copyright 2026 The burrow Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style licence that can be found
 * in the LICENSE file. */

#ifndef BURROW_TESTS_HTTP_CLIENTSERVER_H
#define BURROW_TESTS_HTTP_CLIENTSERVER_H

#include "burrow/burrow.h"
#include "burrow/io.h"
#include "burrow/log.h"
#include "burrow/mem/arena.h"
#include "burrow/mem/heap.h"
#include "burrow/net/http.h"
#include "burrow/net/http/httptest.h"
#include "burrow/strings.h"
#include "burrow/synctest.h"
#include "burrow/testing.h"

#include <stddef.h>
#include <stdint.h>
#include <string.h>

#if defined(BURROW_NETPOLL_READINESS) && !defined(BURROW_OS_WASI)
#define CST_HAVE_TCP 1
#endif

typedef enum {
    HTTP1_MODE,             /* HTTP/1.1 */
    HTTPS1_MODE,            /* HTTPS/1.1 */
    HTTP2_MODE,             /* HTTP/2 */
    HTTP2_UNENCRYPTED_MODE, /* HTTP/2 */
} TestMode;

/* http3SkippedMode is a convenient alias for []testMode{http1Mode, http2Mode},
 * which was the default test mode used by run and runSynctest prior to HTTP/3
 * development. */
static const TestMode http3_skipped_mode[2] = {HTTP1_MODE, HTTP2_MODE};

/* Not in Go: the mode's name, as Go spells the testMode string. */
static inline Str test_mode_string(TestMode m) {
    switch (m) {
    case HTTP1_MODE:
        return BURROW_S("h1");
    case HTTPS1_MODE:
        return BURROW_S("https1");
    case HTTP2_MODE:
        return BURROW_S("h2");
    case HTTP2_UNENCRYPTED_MODE:
        return BURROW_S("h2unencrypted");
    default:
        break;
    }
    panic_str(BURROW_S("unknown testMode"));
}

static inline Str test_mode_scheme(TestMode m) {
    switch (m) {
    case HTTP1_MODE:
    case HTTP2_UNENCRYPTED_MODE:
        return BURROW_S("http");
    case HTTPS1_MODE:
    case HTTP2_MODE:
        return BURROW_S("https");
    default:
        break;
    }
    panic_str(BURROW_S("unknown testMode"));
}

/* Not in Go: the TCP the real network needs, which burrow has only with the
 * readiness poll FD. */
static inline void cst_need_tcp(TestingT *t) {
#if !defined(CST_HAVE_TCP)
    testing_t_skip_v(t, "TCP here needs the readiness poll FD");
#else
    (void)t;
#endif
}

/* ------------------------------------------------------- test memory */

static inline void cst_free_alloc(void *env) {
    void **p = (void **)env;
    mem_free(heap_allocator(), p[0], (size_t)(uintptr_t)p[1], _Alignof(max_align_t));
    mem_free(heap_allocator(), p, 2 * sizeof *p, _Alignof(void *));
}

/* Not in Go: n zeroed bytes that last until the cleanups registered after
 * this call have run, which is what a Go test's closures get from the
 * garbage collector. */
static inline void *test_alloc(TestingT *t, size_t n) {
    void **p = (void **)mem_alloc(heap_allocator(), 2 * sizeof *p, _Alignof(void *));
    void *v = mem_alloc(heap_allocator(), n, _Alignof(max_align_t));
    if (p == NULL || v == NULL)
        testing_t_fatal_v(t, "out of memory");
    memset(v, 0, n);
    p[0] = v;
    p[1] = (void *)(uintptr_t)n;
    testing_t_cleanup(t, BURROW_FN(Func, cst_free_alloc, p));
    return v;
}

static inline void cst_free_arena(void *env) {
    Arena *ar = (Arena *)env;
    arena_free(ar);
    mem_free(heap_allocator(), ar, sizeof *ar, _Alignof(Arena));
}

/* Not in Go: an arena for what the test makes, given back as test_alloc's
 * memory is. Only the test's own goroutine may use it. */
static inline Alloc *test_arena(TestingT *t) {
    Arena *ar = (Arena *)mem_alloc(heap_allocator(), sizeof *ar, _Alignof(Arena));
    if (ar == NULL)
        testing_t_fatal_v(t, "out of memory");
    arena_init(ar, heap_allocator(), 0);
    testing_t_cleanup(t, BURROW_FN(Func, cst_free_arena, ar));
    return arena_allocator(ar);
}

/* ----------------------------------------------------------------- run */

typedef void (*CSTRunFunc)(TestingT *t, TestMode mode, void *env);

/* Not in Go: run's options. modes is the []testMode option, and when it is
 * NULL the tests run in Go's default modes less HTTP/3, which are h1 and h2.
 * not_parallel is testNotParallel. Go's testAddMode and testSkipMode are
 * not here. */
typedef struct RunOpts {
    const TestMode *modes;
    int nmodes;
    bool not_parallel;
} RunOpts;

typedef struct CSTRun {
    CSTRunFunc f;
    void *env;
    TestMode mode;
    bool parallel;
    bool sync;
} CSTRun;

/* afterTest, of which only the start is here: the goroutine leak check after
 * it needs stack traces burrow doesn't have, and Go skips it in short mode
 * anyway. */
static inline void cst_after_test(void *env) {
    (void)env;
    http_transport_close_idle_connections(http_default_transport);
}

static inline void set_parallel(TestingT *t) {
    if (testing_short())
        testing_t_parallel(t);
}

static inline void cst_run_bubble(void *env, TestingT *t) {
    CSTRun *r = (CSTRun *)env;
    r->f(t, r->mode, r->env);
}

static inline void cst_run_mode(void *env, TestingT *t) {
    CSTRun *r = (CSTRun *)env;
    testing_t_helper(t);
    if (r->parallel)
        set_parallel(t);
    testing_t_cleanup(t, BURROW_FN(Func, cst_after_test, NULL));
    if (r->sync)
        synctest_test(t, BURROW_FN(TestingTFunc, cst_run_bubble, r));
    else
        r->f(t, r->mode, r->env);
}

static inline void cst_run(TestingT *t, CSTRunFunc f, void *env, const RunOpts *opts,
                           bool sync) {
    testing_t_helper(t);
    const TestMode *modes = http3_skipped_mode;
    int n = 2;
    bool parallel = true;
    if (opts != NULL) {
        if (opts->modes != NULL) {
            modes = opts->modes;
            n = opts->nmodes;
        }
        parallel = !opts->not_parallel;
    }
    if (parallel)
        set_parallel(t);
    for (int i = 0; i < n; i++) {
        CSTRun r = {f, env, modes[i], parallel, sync};
        (void)testing_t_run(t, test_mode_string(modes[i]),
                            BURROW_FN(TestingTFunc, cst_run_mode, &r));
    }
}

/* run runs a client/server test in a variety of test configurations.
 *
 * Tests execute in HTTP/1.1 and HTTP/2 modes by default.
 * To run in a different set of configurations, pass a []testMode option.
 *
 * Tests call t.Parallel() by default.
 * To disable parallel execution, pass the testNotParallel option. */
static inline void run(TestingT *t, CSTRunFunc f, void *env, const RunOpts *opts) {
    cst_run(t, f, env, opts, false);
}

/* runSynctest is run combined with synctest.Run.
 *
 * The TB passed to f arranges for cleanup functions to be run in the synctest bubble. */
static inline void run_synctest(TestingT *t, CSTRunFunc f, void *env,
                                const RunOpts *opts) {
    cst_run(t, f, env, opts, true);
}

/* ---------------------------------------------------- clientServerTest */

typedef struct ClientServerTest {
    TestingT *t;
    bool h2;
    HttpHandler h;
    HttptestServer *ts;
    HttpTransport *tr;
    HttpClient *c;

    /* Not in Go: what the server and the transport point at, which lasts as
     * long as they do. */
    HttpProtocols p;
    LogLogger *log;
    bool fake;
} ClientServerTest;

static inline void cst_close(ClientServerTest *cst) {
    http_transport_close_idle_connections(cst->tr);
    httptest_server_close(cst->ts);
}

/* Not in Go: the response's body, as an io.Reader. */
static inline IoReader cst_body(HttpResponse *res) {
    return (IoReader){&res->body.vt->reader, res->body.data};
}

/* Not in Go: closes the body and frees the response, which is Go's
 * res.Body.Close() with the garbage collector after it. */
static inline void cst_close_response(HttpResponse *res) {
    (void)res->body.vt->closer.close(res->body.data);
    http_response_free(res);
}

/* Not in Go: the body is in a. */
static inline Str cst_get_url(ClientServerTest *cst, Alloc *a, Str u) {
    Error err = BURROW_NO_ERROR;
    HttpResponse *res = http_client_get(cst->c, u, &err);
    if (res == NULL)
        testing_t_fatal_v(cst->t, err);
    Slice slurp = io_read_all(a, cst_body(res), &err);
    cst_close_response(res);
    if (BURROW_FAILED(err))
        testing_t_fatal_v(cst->t, err);
    return str_from_bytes(slurp.p, slurp.len);
}

static inline Str cst_scheme(ClientServerTest *cst) {
    if (cst->h2)
        return BURROW_S("https");
    return BURROW_S("http");
}

static inline Int cst_test_log_write(void *self, Slice b, Error *err) {
    (void)err;
    testing_t_logf_v((TestingT *)self, "server log: %v",
                     strings_trim_space(str_from_bytes(b.p, b.len)));
    return b.len;
}

static const IoWriterVT cst_test_log_writer_vt = {NULL, cst_test_log_write};

/* quietLog, a logger that writes nowhere, made in a. */
static inline LogLogger *cst_quiet_log(TestingT *t, Alloc *a) {
    LogLogger *l = log_new(a, io_discard, BURROW_S(""), 0);
    if (l == NULL)
        testing_t_fatal_v(t, "out of memory");
    return l;
}

/* Not in Go: newClientServerTest's options, which Go passes as a list of
 * any. fake_net is optFakeNet. server is a func(*httptest.Server), run
 * before the server starts, and transport a func(*Transport), each called
 * with env. error_log is optQuietLog or optWithServerLog, and when it is
 * NULL the server logs to the test, as Go's does. */
typedef struct CSTOpts {
    bool fake_net;
    void (*server)(HttptestServer *ts, void *env);
    void (*transport)(HttpTransport *tr, void *env);
    void *env;
    LogLogger *error_log;
} CSTOpts;

static inline void cst_free(void *env) {
    ClientServerTest *cst = (ClientServerTest *)env;
    if (cst->ts != NULL && !cst->fake)
        httptest_server_free(cst->ts);
    if (cst->log != NULL)
        log_logger_free(heap_allocator(), cst->log);
    mem_free(heap_allocator(), cst, sizeof *cst, _Alignof(ClientServerTest));
}

static inline void cst_cleanup(void *env) {
    cst_close((ClientServerTest *)env);
}

/* newClientServerTest creates and starts an httptest.Server.
 *
 * The mode parameter selects the implementation to test:
 * HTTP/1, HTTP/2, etc. Tests using newClientServerTest should use
 * the 'run' function, which will start a subtests for each tested mode.
 *
 * The vararg opts parameter can include functions to configure the
 * test server or transport.
 *
 *	func(*httptest.Server) // run before starting the server
 *	func(*http.Transport)
 *
 * The optFakeNet option configures the server and client to use a fake network implementation,
 * suitable for use in testing/synctest tests.
 *
 * Not in Go: opts may be NULL, and the server, its client and the rest are
 * given back by cleanups registered on t. */
static inline ClientServerTest *
new_client_server_test(TestingT *t, TestMode mode, HttpHandler h, const CSTOpts *opts) {
    static const CSTOpts no_opts = {0};
    if (opts == NULL)
        opts = &no_opts;
    if (!opts->fake_net)
        cst_need_tcp(t);
    ClientServerTest *cst = (ClientServerTest *)mem_alloc(heap_allocator(), sizeof *cst,
                                                          _Alignof(ClientServerTest));
    if (cst == NULL)
        testing_t_fatal_v(t, "out of memory");
    memset(cst, 0, sizeof *cst);
    cst->t = t;
    cst->h2 = mode == HTTP2_MODE;
    cst->h = h;
    cst->fake = opts->fake_net;
    /* Not in Go: registered first, so that it runs after the server's. */
    testing_t_cleanup(t, BURROW_FN(Func, cst_free, cst));

    if (opts->fake_net)
        cst->ts = httptest_new_test_server(NULL, testing_t_as_testing_tb(t), h);
    else
        cst->ts = httptest_new_unstarted_server(NULL, h);

    if (opts->server != NULL)
        opts->server(cst->ts, opts->env);
    if (opts->error_log != NULL)
        cst->ts->config.error_log = opts->error_log;

    if (cst->ts->config.error_log == NULL) {
        IoWriter w = {&cst_test_log_writer_vt, t};
        cst->log = log_new(heap_allocator(), w, BURROW_S(""), 0);
        if (cst->log == NULL)
            testing_t_fatal_v(t, "out of memory");
        cst->ts->config.error_log = cst->log;
    }

    HttpProtocols *p = &cst->p;
    if (cst->ts->config.protocols == NULL)
        cst->ts->config.protocols = p;
    switch (mode) {
    case HTTP1_MODE:
        http_protocols_set_http1(p, true);
        if (!opts->fake_net)
            httptest_server_start(cst->ts);
        break;
    case HTTPS1_MODE:
        http_protocols_set_http1(p, true);
        if (!opts->fake_net)
            httptest_server_start_tls(cst->ts);
        break;
    case HTTP2_UNENCRYPTED_MODE:
        http_protocols_set_unencrypted_http2(p, true);
        if (!opts->fake_net)
            httptest_server_start(cst->ts);
        break;
    case HTTP2_MODE:
        http_protocols_set_http2(p, true);
        cst->ts->enable_http2 = true;
        cst->ts->tls = cst->ts->config.tls_config;
        if (!opts->fake_net)
            httptest_server_start_tls(cst->ts);
        break;
    default:
        testing_t_fatalf_v(t, "unknown test mode %v", (int)mode);
    }
    cst->c = httptest_server_client(cst->ts);
    /* Not in Go: the server on the network in memory takes both, and its
     * url is the plain one until it is told otherwise. */
    if (opts->fake_net && str_eq(test_mode_scheme(mode), BURROW_S("https")))
        cst->ts->url = BURROW_S("https://example.com");
    cst->tr = &cst->ts->transport;
    if (opts->transport != NULL)
        opts->transport(cst->tr, opts->env);
    if (cst->tr->protocols == NULL)
        cst->tr->protocols = p;

    testing_t_cleanup(t, BURROW_FN(Func, cst_cleanup, cst));
    return cst;
}

/* Not in Go: new_client_server_test for a handler function, which is kept
 * in the ClientServerTest so that it lasts as long as the server. */
static inline ClientServerTest *new_client_server_test_func(TestingT *t, TestMode mode,
                                                            HttpHandlerFunc f,
                                                            const CSTOpts *opts) {
    HttpHandlerFunc *hf = (HttpHandlerFunc *)test_alloc(t, sizeof *hf);
    *hf = f;
    return new_client_server_test(t, mode, http_handler_func_as_handler(hf), opts);
}

#endif
