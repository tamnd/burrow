/* The lookups through the system's resolver, from Go's cgo_unix_test.go, and
 * a few of burrow's own for the errors Go checks by reading the code and for
 * the threads the calls run on.
 *
 * Copyright 2013 The Go Authors. All rights reserved.
 * Copyright 2026 The burrow Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style licence that can be found
 * in the LICENSE file. */

#include "burrow/burrow.h"
#include "burrow/context.h"
#include "burrow/error.h"
#include "burrow/mem/arena.h"
#include "burrow/net.h"
#include "burrow/os.h"
#include "burrow/strconv.h"
#include "burrow/sync.h"
#include "burrow/testing.h"

#include "../src/net/internal.h"
#include "check.h"

#include <stdint.h>
#include <string.h>

/* Go's file builds only where cgo is, which is everywhere burrow has the
 * system's resolver. */
static bool cgo_only(TestingT *t) {
    if (!burrow__net_system_conf()->cgo_available) {
        testing_t_skip_v(t, "no system resolver here");
        return false;
    }
    return true;
}

/* A context for the WithCancel tests, cancelled by the test when it is
 * done. */
typedef struct CancelCtx {
    Arena ar;
    Context ctx;
    ContextCancelFunc cancel;
} CancelCtx;

static void cancel_ctx_init(CancelCtx *c) {
    arena_init(&c->ar, NULL, 0);
    c->ctx =
        context_with_cancel(arena_allocator(&c->ar), context_background(), &c->cancel);
}

static void cancel_ctx_free(CancelCtx *c) {
    BURROW_CALLF0(c->cancel);
    context_release(c->ctx);
    arena_free(&c->ar);
}

static void lookup_ip(TestingT *t, Context ctx) {
    Arena ar;
    arena_init(&ar, NULL, 0);
    Error err = BURROW_NO_ERROR;
    (void)burrow__net_cgo_lookup_ip(arena_allocator(&ar), ctx, BURROW_S("ip"),
                                    BURROW_S("localhost"), &err);
    if (BURROW_FAILED(err))
        testing_t_errorf_v(t, "%v", err);
    arena_free(&ar);
}

static void TestCgoLookupIP(TestingT *t) {
    if (!cgo_only(t))
        return;
    lookup_ip(t, context_background());
}

static void TestCgoLookupIPWithCancel(TestingT *t) {
    if (!cgo_only(t))
        return;
    CancelCtx c;
    cancel_ctx_init(&c);
    lookup_ip(t, c.ctx);
    cancel_ctx_free(&c);
}

static void lookup_port(TestingT *t, Context ctx) {
    Error err = BURROW_NO_ERROR;
    (void)burrow__net_cgo_lookup_port(ctx, BURROW_S("tcp"), BURROW_S("smtp"), &err);
    if (BURROW_FAILED(err))
        testing_t_errorf_v(t, "%v", err);
}

static void TestCgoLookupPort(TestingT *t) {
    if (!cgo_only(t))
        return;
    lookup_port(t, context_background());
}

static void TestCgoLookupPortWithCancel(TestingT *t) {
    if (!cgo_only(t))
        return;
    CancelCtx c;
    cancel_ctx_init(&c);
    lookup_port(t, c.ctx);
    cancel_ctx_free(&c);
}

static void lookup_ptr(TestingT *t, Context ctx) {
    Arena ar;
    arena_init(&ar, NULL, 0);
    Error err = BURROW_NO_ERROR;
    (void)burrow__net_cgo_lookup_ptr(arena_allocator(&ar), ctx, BURROW_S("127.0.0.1"),
                                     &err);
    if (BURROW_FAILED(err))
        testing_t_errorf_v(t, "%v", err);
    arena_free(&ar);
}

static void TestCgoLookupPTR(TestingT *t) {
    if (!cgo_only(t))
        return;
    lookup_ptr(t, context_background());
}

static void TestCgoLookupPTRWithCancel(TestingT *t) {
    if (!cgo_only(t))
        return;
    CancelCtx c;
    cancel_ctx_init(&c);
    lookup_ptr(t, c.ctx);
    cancel_ctx_free(&c);
}

static void TestCgoLookupCNAME(TestingT *t) {
    if (!cgo_only(t))
        return;
    /* mustHaveExternalNetwork and testenv.SkipFlakyNet. */
    if (testing_short()) {
        testing_t_skip_v(t, "avoid external network");
        return;
    }
    Arena env;
    arena_init(&env, NULL, 0);
    Error e = BURROW_NO_ERROR;
    bool flaky = strconv_parse_bool(
        os_getenv(arena_allocator(&env), BURROW_S("GO_BUILDER_FLAKY_NET")), &e);
    arena_free(&env);
    if (flaky) {
        testing_t_skip_v(t, "skipping test on builder known to have frequent network "
                            "failures");
        return;
    }
    CancelCtx c;
    cancel_ctx_init(&c);
    Arena ar;
    arena_init(&ar, NULL, 0);
    Error err = BURROW_NO_ERROR;
    (void)burrow__net_cgo_lookup_cname(arena_allocator(&ar), c.ctx,
                                       BURROW_S("www.iana.org."), &err);
    if (BURROW_FAILED(err))
        testing_t_errorf_v(t, "%v", err);
    arena_free(&ar);
    cancel_ctx_free(&c);
}

/* The errors Go gives before it asks the system anything. */
static Error cgo_error_case(Alloc *a, int i) {
    Context bg = context_background();
    Error err = BURROW_NO_ERROR;
    switch (i) {
    case 0:
        (void)burrow__net_cgo_lookup_port(bg, BURROW_S("sctp"), BURROW_S("http"), &err);
        break;
    case 1:
        (void)burrow__net_cgo_lookup_port(
            bg, BURROW_S("tcp"), str_from_bytes((const Byte *)"ht\0tp", 5), &err);
        break;
    case 2:
        (void)burrow__net_cgo_lookup_ip(a, bg, BURROW_S("ip"),
                                        str_from_bytes((const Byte *)"local\0host", 10),
                                        &err);
        break;
    default:
        (void)burrow__net_cgo_lookup_ptr(a, bg, BURROW_S("foo"), &err);
        break;
    }
    return err;
}

static void TestCgoLookupErrors(TestingT *t) {
    if (!cgo_only(t))
        return;
    static const struct {
        const char *want;
        Int len;
    } tests[] = {
        {"lookup sctp/http: unknown network", 33},
        {"lookup tcp/ht\0tp: invalid argument", 34},
        {"lookup local\0host: invalid argument", 35},
        {"lookup foo: invalid address", 27},
    };
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    for (int i = 0; i < (int)(sizeof tests / sizeof tests[0]); i++) {
        Error err = cgo_error_case(a, i);
        Str want = str_from_bytes((const Byte *)tests[i].want, tests[i].len);
        if (burrow__net_as_dns_error(err) == NULL)
            testing_t_errorf_v(t, "#%d: got %v; want a DNSError", i, err);
        else if (!str_eq(error_text(err), want))
            testing_t_errorf_v(t, "#%d: got %s; want %s", i,
                               strconv_quote(a, error_text(err)),
                               strconv_quote(a, want));
    }
    arena_free(&ar);
}

/* Lookups at once from many goroutines, which take threads from the pool and
 * give them back while others are still asking. */
typedef struct CgoJob {
    Int n;
    Int port;
    char err[200];
} CgoJob;

static void cgo_job(void *env) {
    CgoJob *j = (CgoJob *)env;
    Arena ar;
    arena_init(&ar, NULL, 0);
    Error e = BURROW_NO_ERROR;
    Slice hosts = burrow__net_cgo_lookup_host(
        arena_allocator(&ar), context_background(), BURROW_S("localhost"), &e);
    j->n = hosts.len;
    if (BURROW_OK(e))
        j->port = burrow__net_cgo_lookup_port(context_background(), BURROW_S("tcp"),
                                              BURROW_S("HTTP"), &e);
    if (BURROW_FAILED(e)) {
        Str msg = error_text(e);
        size_t n =
            (size_t)msg.len < sizeof j->err - 1 ? (size_t)msg.len : sizeof j->err - 1;
        memcpy(j->err, msg.p, n);
        j->err[n] = 0;
    }
    arena_free(&ar);
}

static void TestCgoLookupConcurrent(TestingT *t) {
    if (!cgo_only(t))
        return;
    enum { N = 32 };
    CgoJob jobs[N];
    SyncWaitGroup wg;
    memset(&wg, 0, sizeof wg);
    memset(jobs, 0, sizeof jobs);
    for (int i = 0; i < N; i++)
        sync_wait_group_go(&wg, BURROW_FN(Func, cgo_job, &jobs[i]));
    sync_wait_group_wait(&wg);
    for (int i = 0; i < N; i++) {
        if (jobs[i].err[0] != 0) {
            testing_t_errorf_v(t, "#%d: %s", i, jobs[i].err);
            continue;
        }
        if (jobs[i].n == 0)
            testing_t_errorf_v(t, "#%d: no addresses for localhost", i);
        if (jobs[i].port != 80)
            testing_t_errorf_v(t, "#%d: port of HTTP is %d; want 80", i,
                               (int)jobs[i].port);
    }
}

#define TESTS(X)                                                                       \
    X(TestCgoLookupIP)                                                                 \
    X(TestCgoLookupIPWithCancel)                                                       \
    X(TestCgoLookupPort)                                                               \
    X(TestCgoLookupPortWithCancel)                                                     \
    X(TestCgoLookupPTR)                                                                \
    X(TestCgoLookupPTRWithCancel)                                                      \
    X(TestCgoLookupCNAME)                                                              \
    X(TestCgoLookupErrors)                                                             \
    X(TestCgoLookupConcurrent)

TESTING_MAIN(TESTS)
