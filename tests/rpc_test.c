/* Derived from Go's src/net/rpc/server_test.go and client_test.go.
 *
 * Go starts its servers once, behind a sync.Once, and leaves them running for
 * the rest of the tests. The registrations and HTTP handlers are made once
 * here too, since neither can be taken back, but each test listens on a port
 * of its own and closes it at the end, so that nothing is left accepting when
 * the program exits.
 *
 * Go's Arith is an int, and its BuiltinTypes an empty struct. A descriptor
 * here needs at least one field, so each has one nobody reads. Go's
 * TestRegistrationError registers a type whose methods have a pointer
 * receiver by value and expects a hint about pointers back. A receiver in C is
 * always a pointer, so that registration works, and that case is left out.
 * BuiltinTypes.Slice takes an allocator as its first argument, which is how a
 * method appends to a reply here. The malloc counts and the benchmarks are
 * left out.
 *
 * TestCloseCodec and TestGobError are client_test.go's.
 *
 * Copyright 2009 The Go Authors. All rights reserved.
 * Copyright 2026 The burrow Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style licence that can be found
 * in the LICENSE file. */

#include "check.h"

#include "burrow/burrow.h"
#include "burrow/chan.h"
#include "burrow/declare.h"
#include "burrow/encoding.h"
#include "burrow/error.h"
#include "burrow/fmt.h"
#include "burrow/func.h"
#include "burrow/io.h"
#include "burrow/map.h"
#include "burrow/mem.h"
#include "burrow/mem/arena.h"
#include "burrow/mem/heap.h"
#include "burrow/net.h"
#include "burrow/net/http.h"
#include "burrow/net/http/httptest.h"
#include "burrow/net/rpc.h"
#include "burrow/netpoll.h"
#include "burrow/proc.h"
#include "burrow/slice.h"
#include "burrow/strings.h"
#include "burrow/sync.h"
#include "burrow/testing.h"
#include "burrow/time.h"

#include <stdint.h>
#include <string.h>

#if defined(BURROW_NETPOLL_READINESS) && !defined(BURROW_OS_WASI)
#define HAVE_TCP 1
#endif

#define FATALF(...)                                                                    \
    do {                                                                               \
        testing_t_fatalf_v(t, __VA_ARGS__);                                            \
        return;                                                                        \
    } while (0)

static bool need_tcp(TestingT *t) {
#if !defined(HAVE_TCP)
    testing_t_skip_v(t, "TCP here needs the readiness poll FD");
    return false;
#else
    (void)t;
    return true;
#endif
}

static void check_str(TestingT *t, Str got, Str want) {
    if (!str_eq(got, want))
        testing_t_errorf_v(t, "got %q, want %q", got, want);
}

/* ------------------------------------------------------------------ types */

#define ARGS_FIELDS(F, T) F(T, Int, A, "") F(T, Int, B, "")
BURROW_STRUCT(Args, ARGS_FIELDS);

#define REPLY_FIELDS(F, T) F(T, Int, C, "")
BURROW_STRUCT(Reply, REPLY_FIELDS);

BURROW_PTR_TYPE(ArgsPtr, Args);
BURROW_PTR_TYPE(ReplyPtr, Reply);
BURROW_PTR_TYPE(StrPtr, Str);
BURROW_MAP_TYPE(IntIntMap, Int, Int);
BURROW_PTR_TYPE(IntIntMapPtr, IntIntMap);
BURROW_SLICE_TYPE(IntSlice, Int);
BURROW_PTR_TYPE(IntSlicePtr, IntSlice);
BURROW_ARRAY_TYPE(IntArray2, Int, 2);
BURROW_PTR_TYPE(IntArray2Ptr, IntArray2);

#define ARITH_FIELDS(F, T) F(T, Int, N, "")
BURROW_STRUCT_DECL(Arith, ARITH_FIELDS);

/* Some of Arith's methods have value args, some have pointer args. That's
 * deliberate. */

static Error arith_add(Arith *t, Args args, ReplyPtr reply) {
    (void)t;
    reply->C = args.A + args.B;
    return BURROW_NO_ERROR;
}

static Error arith_mul(Arith *t, ArgsPtr args, ReplyPtr reply) {
    (void)t;
    reply->C = args->A * args->B;
    return BURROW_NO_ERROR;
}

static Error arith_div(Arith *t, Args args, ReplyPtr reply) {
    (void)t;
    if (args.B == 0)
        return errors_new(error_allocator(), BURROW_S("divide by zero"));
    reply->C = args.A / args.B;
    return BURROW_NO_ERROR;
}

static Error arith_string(Arith *t, EncodingAllocArg a, ArgsPtr args, StrPtr reply) {
    (void)t;
    *reply = fmt_sprintf_v(a, "%d+%d=%d", args->A, args->B, args->A + args->B);
    return BURROW_NO_ERROR;
}

static Error arith_scan(Arith *t, Str args, ReplyPtr reply) {
    (void)t;
    Error err = BURROW_NO_ERROR;
    (void)fmt_sscan_v(error_allocator(), &err, args, &reply->C);
    return err;
}

static Error arith_sleep_milli(Arith *t, ArgsPtr args, ReplyPtr reply) {
    (void)t;
    (void)reply;
    time_sleep(args->A * TIME_MILLISECOND);
    return BURROW_NO_ERROR;
}

#define ARITH_ADD_SIG(IN, OUT) IN(0, Args) IN(1, ReplyPtr) OUT(Error)
#define ARITH_DIV_SIG(IN, OUT) IN(0, Args) IN(1, ReplyPtr) OUT(Error)
#define ARITH_MUL_SIG(IN, OUT) IN(0, ArgsPtr) IN(1, ReplyPtr) OUT(Error)
#define ARITH_SCAN_SIG(IN, OUT) IN(0, Str) IN(1, ReplyPtr) OUT(Error)
#define ARITH_SLEEP_SIG(IN, OUT) IN(0, ArgsPtr) IN(1, ReplyPtr) OUT(Error)
#define ARITH_STRING_SIG(IN, OUT)                                                      \
    IN(0, EncodingAllocArg) IN(1, ArgsPtr) IN(2, StrPtr) OUT(Error)

#define ARITH_METHODS(M, T)                                                            \
    M(T, Add, arith_add, ARITH_ADD_SIG)                                                \
    M(T, Div, arith_div, ARITH_DIV_SIG)                                                \
    M(T, Mul, arith_mul, ARITH_MUL_SIG)                                                \
    M(T, Scan, arith_scan, ARITH_SCAN_SIG)                                             \
    M(T, SleepMilli, arith_sleep_milli, ARITH_SLEEP_SIG)                               \
    M(T, String, arith_string, ARITH_STRING_SIG)
BURROW_STRUCT_DEFINE_METHODS(Arith, ARITH_FIELDS, ARITH_METHODS);

/* Go's Embed has Exported through an unexported embedded type. */
#define EMBED_FIELDS(F, T) F(T, Int, hidden, "")
BURROW_STRUCT_DECL(Embed, EMBED_FIELDS);

static Error embed_exported(Embed *t, Args args, ReplyPtr reply) {
    (void)t;
    reply->C = args.A + args.B;
    return BURROW_NO_ERROR;
}

#define EMBED_EXPORTED_SIG(IN, OUT) IN(0, Args) IN(1, ReplyPtr) OUT(Error)
#define EMBED_METHODS(M, T) M(T, Exported, embed_exported, EMBED_EXPORTED_SIG)
BURROW_STRUCT_DEFINE_METHODS(Embed, EMBED_FIELDS, EMBED_METHODS);

#define BUILTIN_FIELDS(F, T) F(T, Int, unused, "")
BURROW_STRUCT_DECL(BuiltinTypes, BUILTIN_FIELDS);

static Error builtin_map(BuiltinTypes *t, ArgsPtr args, IntIntMapPtr reply) {
    (void)t;
    if (!map_set(*reply, &args->A, &args->B))
        return burrow_err_out_of_memory;
    return BURROW_NO_ERROR;
}

static Error builtin_slice(BuiltinTypes *t, EncodingAllocArg a, ArgsPtr args,
                           IntSlicePtr reply) {
    (void)t;
    Int v[2];
    v[0] = args->A;
    v[1] = args->B;
    *reply = slice_append(a, *reply, v, 2);
    return BURROW_NO_ERROR;
}

static Error builtin_array(BuiltinTypes *t, ArgsPtr args, IntArray2Ptr reply) {
    (void)t;
    reply->v[0] = args->A;
    reply->v[1] = args->B;
    return BURROW_NO_ERROR;
}

#define BUILTIN_ARRAY_SIG(IN, OUT) IN(0, ArgsPtr) IN(1, IntArray2Ptr) OUT(Error)
#define BUILTIN_MAP_SIG(IN, OUT) IN(0, ArgsPtr) IN(1, IntIntMapPtr) OUT(Error)
#define BUILTIN_SLICE_SIG(IN, OUT)                                                     \
    IN(0, EncodingAllocArg) IN(1, ArgsPtr) IN(2, IntSlicePtr) OUT(Error)
#define BUILTIN_METHODS(M, T)                                                          \
    M(T, Array, builtin_array, BUILTIN_ARRAY_SIG)                                      \
    M(T, Map, builtin_map, BUILTIN_MAP_SIG)                                            \
    M(T, Slice, builtin_slice, BUILTIN_SLICE_SIG)
BURROW_STRUCT_DEFINE_METHODS(BuiltinTypes, BUILTIN_FIELDS, BUILTIN_METHODS);

/* ---------------------------------------------------------------- servers */

#define NEW_HTTP_PATH "/foo"

static Arith arith;
static Embed embed;
static BuiltinTypes builtin_types;
static RpcServer *new_server;

static SyncOnce once, new_once;

static void start_server(void *env) {
    (void)env;
    (void)rpc_register(BURROW_ANY(TYPE_OF(Arith), &arith));
    (void)rpc_register(BURROW_ANY(TYPE_OF(Embed), &embed));
    (void)rpc_register_name(BURROW_S("net.rpc.Arith"),
                            BURROW_ANY(TYPE_OF(Arith), &arith));
    (void)rpc_register(BURROW_ANY(TYPE_OF(BuiltinTypes), &builtin_types));
    rpc_handle_http();
}

static void start_new_server(void *env) {
    (void)env;
    new_server = rpc_new_server(heap_allocator());
    (void)rpc_server_register(new_server, BURROW_ANY(TYPE_OF(Arith), &arith));
    (void)rpc_server_register(new_server, BURROW_ANY(TYPE_OF(Embed), &embed));
    (void)rpc_server_register_name(new_server, BURROW_S("net.rpc.Arith"),
                                   BURROW_ANY(TYPE_OF(Arith), &arith));
    (void)rpc_server_register_name(new_server, BURROW_S("newServer.Arith"),
                                   BURROW_ANY(TYPE_OF(Arith), &arith));
    rpc_server_handle_http(new_server, BURROW_S(NEW_HTTP_PATH), BURROW_S("/bar"));
}

static void use_server(void) {
    sync_once_do(&once, BURROW_FN(Func, start_server, NULL));
}

static void use_new_server(void) {
    sync_once_do(&new_once, BURROW_FN(Func, start_new_server, NULL));
}

/* A listener with Accept running on it, for one server. */
typedef struct Served {
    RpcServer *s; /* NULL for the default */
    NetListener l;
    Str addr;
    SyncWaitGroup wg;
    Arena ar;
} Served;

static void served_accept(void *env) {
    Served *sv = (Served *)env;
    if (sv->s == NULL)
        rpc_accept(sv->l);
    else
        rpc_server_accept(sv->s, sv->l);
    sync_wait_group_done(&sv->wg);
}

/* listenTCP. */
static bool listen_tcp(TestingT *t, Served *sv) {
    memset(sv, 0, sizeof *sv);
    arena_init(&sv->ar, heap_allocator(), 0);
    Error err = BURROW_NO_ERROR;
    sv->l =
        net_listen(heap_allocator(), BURROW_S("tcp"), BURROW_S("127.0.0.1:0"), &err);
    if (BURROW_FAILED(err)) {
        testing_t_errorf_v(t, "net.Listen tcp :0: %v", err);
        arena_free(&sv->ar);
        return false;
    }
    NetAddr la = sv->l.vt->addr(sv->l.data);
    sv->addr = la.vt->string(la.data, arena_allocator(&sv->ar));
    return true;
}

static bool serve_start(TestingT *t, Served *sv, RpcServer *s) {
    if (!listen_tcp(t, sv))
        return false;
    sv->s = s;
    sync_wait_group_add(&sv->wg, 1);
    if (!go(BURROW_FN(Func, served_accept, sv))) {
        sync_wait_group_done(&sv->wg);
        testing_t_errorf_v(t, "no goroutine for Accept");
        net_listener_free(sv->l);
        arena_free(&sv->ar);
        return false;
    }
    return true;
}

static void listener_stop(Served *sv) {
    net_listener_free(sv->l);
    arena_free(&sv->ar);
}

static void serve_stop(Served *sv) {
    (void)sv->l.vt->closer.close(sv->l.data);
    sync_wait_group_wait(&sv->wg);
    listener_stop(sv);
}

/* startHttpServer, on http_default_serve_mux. */
static HttptestServer *start_http_server(void) {
    HttpHandler none = {NULL, NULL};
    return httptest_new_server(NULL, none);
}

/* The host and port of an httptest server. */
static Str http_addr(HttptestServer *ts) {
    Str u = ts->url;
    Int i = strings_index(u, BURROW_S("://"));
    return i < 0 ? u : str_from_bytes(u.p + i + 3, u.len - i - 3);
}

/* ----------------------------------------------------------------- calls */

static void check_add(TestingT *t, const char *what, Error err, Args args,
                      Reply reply) {
    if (BURROW_FAILED(err))
        testing_t_errorf_v(t, "%s: expected no error but got string %q", what,
                           error_text(err));
    if (reply.C != args.A + args.B)
        testing_t_errorf_v(t, "%s: expected %d got %d", what, reply.C, args.A + args.B);
}

static Error call(RpcClient *c, Alloc *a, const char *method, Any args, Any reply) {
    return rpc_client_call(c, a, str_from_cstr(method), args, reply);
}

#define ARGS(x) BURROW_ANY(TYPE_OF(Args), &(x))
#define REPLY(x) BURROW_ANY(TYPE_OF(Reply), &(x))

static void test_rpc(TestingT *t, Str addr) {
    Error err = BURROW_NO_ERROR;
    RpcClient *client = rpc_dial(heap_allocator(), BURROW_S("tcp"), addr, &err);
    if (BURROW_FAILED(err))
        FATALF("dialing %v", err);
    Arena ar;
    arena_init(&ar, heap_allocator(), 0);
    Alloc *a = arena_allocator(&ar);

    /* Synchronous calls */
    Args args = {7, 8};
    Reply reply = {0};
    err = call(client, a, "Arith.Add", ARGS(args), REPLY(reply));
    check_add(t, "Add", err, args, reply);

    /* Methods exported from unexported embedded structs */
    args = (Args){7, 0};
    reply = (Reply){0};
    err = call(client, a, "Embed.Exported", ARGS(args), REPLY(reply));
    check_add(t, "Add", err, args, reply);

    /* Nonexistent method */
    args = (Args){7, 0};
    reply = (Reply){0};
    err = call(client, a, "Arith.BadOperation", ARGS(args), REPLY(reply));
    /* expect an error */
    if (BURROW_OK(err))
        testing_t_error_v(t, "BadOperation: expected error");
    else if (!strings_has_prefix(error_text(err), BURROW_S("rpc: can't find method ")))
        testing_t_errorf_v(t, "BadOperation: expected can't find method error; got %q",
                           error_text(err));

    /* Unknown service */
    args = (Args){7, 8};
    reply = (Reply){0};
    err = call(client, a, "Arith.Unknown", ARGS(args), REPLY(reply));
    if (BURROW_OK(err))
        testing_t_error_v(t, "expected error calling unknown service");
    else if (!strings_contains(error_text(err), BURROW_S("method")))
        testing_t_error_v(t, "expected error about method; got", err);

    /* Out of order. */
    args = (Args){7, 8};
    Reply mul_reply = {0};
    RpcCall *mul_call = rpc_client_go(client, heap_allocator(), BURROW_S("Arith.Mul"),
                                      ARGS(args), REPLY(mul_reply), NULL);
    Reply add_reply = {0};
    RpcCall *add_call = rpc_client_go(client, heap_allocator(), BURROW_S("Arith.Add"),
                                      ARGS(args), REPLY(add_reply), NULL);
    if (mul_call == NULL || add_call == NULL) {
        rpc_call_free(mul_call);
        rpc_call_free(add_call);
        rpc_client_free(client);
        arena_free(&ar);
        FATALF("Go: out of memory");
    }

    void *v = NULL;
    (void)chan_recv(add_call->done, &v);
    check_add(t, "Add", add_call->error, args, add_reply);

    (void)chan_recv(mul_call->done, &v);
    if (BURROW_FAILED(mul_call->error))
        testing_t_errorf_v(t, "Mul: expected no error but got string %q",
                           error_text(mul_call->error));
    if (mul_reply.C != args.A * args.B)
        testing_t_errorf_v(t, "Mul: expected %d got %d", mul_reply.C, args.A * args.B);
    rpc_call_free(add_call);
    rpc_call_free(mul_call);

    /* Error test */
    args = (Args){7, 0};
    reply = (Reply){0};
    err = call(client, a, "Arith.Div", ARGS(args), REPLY(reply));
    /* expect an error: zero divide */
    if (BURROW_OK(err))
        testing_t_error_v(t, "Div: expected error");
    else if (!str_eq(error_text(err), BURROW_S("divide by zero")))
        testing_t_error_v(t, "Div: expected divide by zero error; got", err);

    /* Bad type. */
    reply = (Reply){0};
    err = call(client, a, "Arith.Add", REPLY(reply), REPLY(reply));
    if (BURROW_OK(err))
        testing_t_error_v(t, "expected error calling Arith.Add with wrong arg type");
    else if (!strings_contains(error_text(err), BURROW_S("type")))
        testing_t_error_v(t, "expected error about type; got", err);

    /* Non-struct argument */
    Int val = 12345;
    Str str = fmt_sprint_v(a, val);
    reply = (Reply){0};
    err = call(client, a, "Arith.Scan", BURROW_ANY(TYPE_STRING, &str), REPLY(reply));
    if (BURROW_FAILED(err))
        testing_t_errorf_v(t, "Scan: expected no error but got string %q",
                           error_text(err));
    else if (reply.C != val)
        testing_t_errorf_v(t, "Scan: expected %d got %d", val, reply.C);

    /* Non-struct reply */
    args = (Args){27, 35};
    str = BURROW_STR_EMPTY;
    err = call(client, a, "Arith.String", ARGS(args), BURROW_ANY(TYPE_STRING, &str));
    if (BURROW_FAILED(err))
        testing_t_errorf_v(t, "String: expected no error but got string %q",
                           error_text(err));
    Str expect = fmt_sprintf_v(a, "%d+%d=%d", args.A, args.B, args.A + args.B);
    if (!str_eq(str, expect))
        testing_t_errorf_v(t, "String: expected %s got %s", expect, str);

    args = (Args){7, 8};
    reply = (Reply){0};
    err = call(client, a, "Arith.Mul", ARGS(args), REPLY(reply));
    if (BURROW_FAILED(err))
        testing_t_errorf_v(t, "Mul: expected no error but got string %q",
                           error_text(err));
    if (reply.C != args.A * args.B)
        testing_t_errorf_v(t, "Mul: expected %d got %d", reply.C, args.A * args.B);

    /* ServiceName contain "." character */
    args = (Args){7, 8};
    reply = (Reply){0};
    err = call(client, a, "net.rpc.Arith.Add", ARGS(args), REPLY(reply));
    check_add(t, "Add", err, args, reply);

    rpc_client_free(client);
    arena_free(&ar);
}

static void test_new_server_rpc(TestingT *t, Str addr) {
    Error err = BURROW_NO_ERROR;
    RpcClient *client = rpc_dial(heap_allocator(), BURROW_S("tcp"), addr, &err);
    if (BURROW_FAILED(err))
        FATALF("dialing %v", err);

    /* Synchronous calls */
    Args args = {7, 8};
    Reply reply = {0};
    err =
        call(client, heap_allocator(), "newServer.Arith.Add", ARGS(args), REPLY(reply));
    check_add(t, "Add", err, args, reply);
    rpc_client_free(client);
}

static void TestRPC(TestingT *t) {
    if (!need_tcp(t))
        return;
    use_server();
    Served sv;
    if (!serve_start(t, &sv, NULL))
        return;
    test_rpc(t, sv.addr);
    serve_stop(&sv);

    use_new_server();
    Served nsv;
    if (!serve_start(t, &nsv, new_server))
        return;
    test_rpc(t, nsv.addr);
    test_new_server_rpc(t, nsv.addr);
    serve_stop(&nsv);
}

static void test_http_rpc(TestingT *t, HttptestServer *ts, Str path) {
    Error err = BURROW_NO_ERROR;
    RpcClient *client;
    if (path.len == 0)
        client = rpc_dial_http(heap_allocator(), BURROW_S("tcp"), http_addr(ts), &err);
    else
        client = rpc_dial_http_path(heap_allocator(), BURROW_S("tcp"), http_addr(ts),
                                    path, &err);
    if (BURROW_FAILED(err))
        FATALF("dialing %v", err);

    /* Synchronous calls */
    Args args = {7, 8};
    Reply reply = {0};
    err = call(client, heap_allocator(), "Arith.Add", ARGS(args), REPLY(reply));
    check_add(t, "Add", err, args, reply);
    rpc_client_free(client);
}

static void TestHTTP(TestingT *t) {
    if (!need_tcp(t))
        return;
    use_server();
    HttptestServer *ts = start_http_server();
    test_http_rpc(t, ts, BURROW_STR_EMPTY);
    use_new_server();
    test_http_rpc(t, ts, BURROW_S(NEW_HTTP_PATH));
    httptest_server_free(ts);
}

/* Not one of Go's: a CONNECT to a path nothing is registered at, and a GET of
 * the RPC path, which wants a CONNECT. */
static void TestHTTPErrors(TestingT *t) {
    if (!need_tcp(t))
        return;
    use_server();
    HttptestServer *ts = start_http_server();
    Error err = BURROW_NO_ERROR;
    RpcClient *client = rpc_dial_http_path(heap_allocator(), BURROW_S("tcp"),
                                           http_addr(ts), BURROW_S("/nope"), &err);
    if (client != NULL) {
        rpc_client_free(client);
        testing_t_error_v(t, "DialHTTPPath /nope: expected error");
    } else {
        check_str(t, error_text(err),
                  fmt_sprintf_v(error_allocator(),
                                "dial-http tcp %s: unexpected HTTP response: "
                                "404 Not Found",
                                http_addr(ts)));
    }

    Str url = fmt_sprintf_v(error_allocator(), "%s%s", ts->url, RPC_DEFAULT_RPC_PATH);
    HttpResponse *res = http_client_get(httptest_server_client(ts), url, &err);
    if (BURROW_FAILED(err)) {
        httptest_server_free(ts);
        FATALF("Get: %v", err);
    }
    CHECK_INT_EQ(res->status_code, HTTP_STATUS_METHOD_NOT_ALLOWED);
    Arena ar;
    arena_init(&ar, heap_allocator(), 0);
    Slice body = io_read_all(arena_allocator(&ar),
                             (IoReader){&res->body.vt->reader, res->body.data}, &err);
    CHECK(BURROW_OK(err));
    check_str(t, str_from_bytes((const Byte *)body.p, body.len),
              BURROW_S("405 must CONNECT\n"));
    http_response_free(res);
    arena_free(&ar);
    httptest_server_free(ts);
}

/* Not one of Go's: the page at the debug path, for a server with one service. */
static void TestDebugHTTP(TestingT *t) {
    if (!need_tcp(t))
        return;
    RpcServer *s = rpc_new_server(heap_allocator());
    if (s == NULL)
        FATALF("NewServer: out of memory");
    Error err = rpc_server_register(s, BURROW_ANY(TYPE_OF(Embed), &embed));
    CHECK(BURROW_OK(err));
    rpc_server_handle_http(s, BURROW_S("/debug-test"), BURROW_S("/debug-test/debug"));
    HttptestServer *ts = start_http_server();
    Str url = fmt_sprintf_v(error_allocator(), "%s/debug-test/debug", ts->url);
    HttpResponse *res = http_client_get(httptest_server_client(ts), url, &err);
    if (BURROW_FAILED(err)) {
        httptest_server_free(ts);
        FATALF("Get: %v", err);
    }
    CHECK_INT_EQ(res->status_code, 200);
    Arena ar;
    arena_init(&ar, heap_allocator(), 0);
    Slice body = io_read_all(arena_allocator(&ar),
                             (IoReader){&res->body.vt->reader, res->body.data}, &err);
    CHECK(BURROW_OK(err));
    Str page = str_from_bytes((const Byte *)body.p, body.len);
    CHECK(strings_has_prefix(
        page, BURROW_S("<html>\n\t<body>\n\t<title>Services</title>\n")));
    CHECK(strings_contains(page, BURROW_S("\tService Embed\n")));
    CHECK(strings_contains(page, BURROW_S("<td align=left font=fixed>Exported(")));
    CHECK(strings_contains(page, BURROW_S("<td align=center>0</td>")));
    CHECK(strings_has_suffix(page, BURROW_S("\t</body>\n\t</html>")));
    http_response_free(res);
    arena_free(&ar);
    httptest_server_free(ts);
    /* s stays, since the mux still points at it. */
}

static void TestBuiltinTypes(TestingT *t) {
    if (!need_tcp(t))
        return;
    use_server();
    HttptestServer *ts = start_http_server();
    Error err = BURROW_NO_ERROR;
    RpcClient *client =
        rpc_dial_http(heap_allocator(), BURROW_S("tcp"), http_addr(ts), &err);
    if (BURROW_FAILED(err)) {
        httptest_server_free(ts);
        FATALF("dialing %v", err);
    }
    Arena ar;
    arena_init(&ar, heap_allocator(), 0);
    Alloc *a = arena_allocator(&ar);

    /* Map */
    Args args = {7, 8};
    IntIntMap reply_map = map_make(a, TYPE_INT, TYPE_INT, 0);
    err = call(client, a, "BuiltinTypes.Map", ARGS(args),
               BURROW_ANY(TYPE_OF(IntIntMap), &reply_map));
    if (BURROW_FAILED(err))
        testing_t_errorf_v(t, "Map: expected no error but got string %q",
                           error_text(err));
    const Int *got = (const Int *)map_get(reply_map, &args.A);
    if (got == NULL || *got != args.B)
        testing_t_errorf_v(t, "Map: expected %d got %d", args.B,
                           got != NULL ? *got : 0);

    /* Slice */
    args = (Args){7, 8};
    IntSlice reply_slice = slice_make(a, TYPE_INT, 0, 0);
    err = call(client, a, "BuiltinTypes.Slice", ARGS(args),
               BURROW_ANY(TYPE_OF(IntSlice), &reply_slice));
    if (BURROW_FAILED(err))
        testing_t_errorf_v(t, "Slice: expected no error but got string %q",
                           error_text(err));
    if (reply_slice.len != 2 || ((Int *)reply_slice.p)[0] != args.A ||
        ((Int *)reply_slice.p)[1] != args.B)
        testing_t_errorf_v(t, "Slice: expected [%d %d] got %v", args.A, args.B,
                           BURROW_ANY(TYPE_OF(IntSlice), &reply_slice));

    /* Array */
    args = (Args){7, 8};
    IntArray2 reply_array = {{0, 0}};
    err = call(client, a, "BuiltinTypes.Array", ARGS(args),
               BURROW_ANY(TYPE_OF(IntArray2), &reply_array));
    if (BURROW_FAILED(err))
        testing_t_errorf_v(t, "Array: expected no error but got string %q",
                           error_text(err));
    if (reply_array.v[0] != args.A || reply_array.v[1] != args.B)
        testing_t_errorf_v(t, "Array: expected [%d %d] got [%d %d]", args.A, args.B,
                           reply_array.v[0], reply_array.v[1]);

    rpc_client_free(client);
    arena_free(&ar);
    httptest_server_free(ts);
}

/* ---------------------------------------------------------- CodecEmulator */

/* CodecEmulator provides a client-like api and a ServerCodec interface. Can
 * be used to test ServeRequest. The error a response carries is kept as text
 * in buf, since the server gives back its error arena once a call is
 * answered. */
typedef struct CodecEmulator {
    RpcServer *server;
    Str service_method;
    Args *args;
    Reply *reply;
    bool failed;
    char buf[128];
    Int n;
} CodecEmulator;

static Error codec_emulator_read_request_header(void *self, Alloc *a, RpcRequest *req) {
    CodecEmulator *codec = (CodecEmulator *)self;
    (void)a;
    req->service_method = codec->service_method;
    req->seq = 0;
    return BURROW_NO_ERROR;
}

static Error codec_emulator_read_request_body(void *self, Alloc *a, Any argv) {
    CodecEmulator *codec = (CodecEmulator *)self;
    (void)a;
    if (codec->args == NULL)
        return io_err_unexpected_eof;
    if (argv.t != NULL)
        *(Args *)argv.data = *codec->args;
    return BURROW_NO_ERROR;
}

static Error codec_emulator_write_response(void *self, const RpcResponse *resp,
                                           Any reply) {
    CodecEmulator *codec = (CodecEmulator *)self;
    if (resp->error.len != 0) {
        codec->failed = true;
        codec->n = resp->error.len < (Int)sizeof codec->buf ? resp->error.len
                                                            : (Int)sizeof codec->buf;
        memcpy(codec->buf, resp->error.p, (size_t)codec->n);
    } else {
        *codec->reply = *(const Reply *)reply.data;
    }
    return BURROW_NO_ERROR;
}

static Error codec_emulator_close(void *self) {
    (void)self;
    return BURROW_NO_ERROR;
}

static const RpcServerCodecVT codec_emulator_vt = {
    NULL,
    codec_emulator_read_request_header,
    codec_emulator_read_request_body,
    codec_emulator_write_response,
    codec_emulator_close,
    NULL,
};

static Error codec_emulator_call(CodecEmulator *codec, Str service_method, Args *args,
                                 Reply *reply) {
    codec->service_method = service_method;
    codec->args = args;
    codec->reply = reply;
    codec->failed = false;
    RpcServerCodec sc = {&codec_emulator_vt, codec};
    Error server_error = codec->server == NULL
                             ? rpc_serve_request(sc)
                             : rpc_server_serve_request(codec->server, sc);
    if (codec->failed)
        return errors_new(error_allocator(),
                          str_from_bytes((const Byte *)codec->buf, codec->n));
    return server_error;
}

static void test_serve_request(TestingT *t, RpcServer *server) {
    CodecEmulator client;
    memset(&client, 0, sizeof client);
    client.server = server;

    Args args = {7, 8};
    Reply reply = {0};
    Error err = codec_emulator_call(&client, BURROW_S("Arith.Add"), &args, &reply);
    check_add(t, "Add", err, args, reply);

    err = codec_emulator_call(&client, BURROW_S("Arith.Add"), NULL, &reply);
    if (BURROW_OK(err))
        testing_t_errorf_v(t, "expected error calling Arith.Add with nil arg");
}

static void TestServeRequest(TestingT *t) {
    use_server();
    test_serve_request(t, NULL);
    use_new_server();
    test_serve_request(t, new_server);
}

/* --------------------------------------------------- registration errors */

#define LOCAL_FIELDS(F, T) F(T, Int, x, "")
BURROW_STRUCT(local, LOCAL_FIELDS);
BURROW_PTR_TYPE(localPtr, local);

#define ONE_FIELD(F, T) F(T, Int, n, "")

BURROW_STRUCT_DECL(ReplyNotPointer, ONE_FIELD);
BURROW_STRUCT_DECL(ArgNotPublic, ONE_FIELD);
BURROW_STRUCT_DECL(ReplyNotPublic, ONE_FIELD);

static Error reply_not_pointer(ReplyNotPointer *t, ArgsPtr args, Reply reply) {
    (void)t;
    (void)args;
    (void)reply;
    return BURROW_NO_ERROR;
}

static Error arg_not_public(ArgNotPublic *t, localPtr args, ReplyPtr reply) {
    (void)t;
    (void)args;
    (void)reply;
    return BURROW_NO_ERROR;
}

static Error reply_not_public(ReplyNotPublic *t, ArgsPtr args, localPtr reply) {
    (void)t;
    (void)args;
    (void)reply;
    return BURROW_NO_ERROR;
}

#define REPLY_NOT_POINTER_SIG(IN, OUT) IN(0, ArgsPtr) IN(1, Reply) OUT(Error)
#define ARG_NOT_PUBLIC_SIG(IN, OUT) IN(0, localPtr) IN(1, ReplyPtr) OUT(Error)
#define REPLY_NOT_PUBLIC_SIG(IN, OUT) IN(0, ArgsPtr) IN(1, localPtr) OUT(Error)
#define REPLY_NOT_POINTER_METHODS(M, T)                                                \
    M(T, ReplyNotPointer, reply_not_pointer, REPLY_NOT_POINTER_SIG)
#define ARG_NOT_PUBLIC_METHODS(M, T)                                                   \
    M(T, ArgNotPublic, arg_not_public, ARG_NOT_PUBLIC_SIG)
#define REPLY_NOT_PUBLIC_METHODS(M, T)                                                 \
    M(T, ReplyNotPublic, reply_not_public, REPLY_NOT_PUBLIC_SIG)
BURROW_STRUCT_DEFINE_METHODS(ReplyNotPointer, ONE_FIELD, REPLY_NOT_POINTER_METHODS);
BURROW_STRUCT_DEFINE_METHODS(ArgNotPublic, ONE_FIELD, ARG_NOT_PUBLIC_METHODS);
BURROW_STRUCT_DEFINE_METHODS(ReplyNotPublic, ONE_FIELD, REPLY_NOT_PUBLIC_METHODS);

static void TestRegistrationError(TestingT *t) {
    ReplyNotPointer rnp = {0};
    Error err = rpc_register(BURROW_ANY(TYPE_OF(ReplyNotPointer), &rnp));
    if (BURROW_OK(err))
        testing_t_error_v(t, "expected error registering ReplyNotPointer");
    ArgNotPublic anp = {0};
    err = rpc_register(BURROW_ANY(TYPE_OF(ArgNotPublic), &anp));
    if (BURROW_OK(err))
        testing_t_error_v(t, "expected error registering ArgNotPublic");
    ReplyNotPublic rnpub = {0};
    err = rpc_register(BURROW_ANY(TYPE_OF(ReplyNotPublic), &rnpub));
    if (BURROW_OK(err))
        testing_t_error_v(t, "expected error registering ReplyNotPublic");
    else
        check_str(t, error_text(err),
                  BURROW_S("rpc.Register: type ReplyNotPublic has no "
                           "exported methods of suitable type"));
}

/* Not one of Go's: the other errors Register gives. */
static void TestRegistrationErrors(TestingT *t) {
    RpcServer *s = rpc_new_server(heap_allocator());
    if (s == NULL)
        FATALF("NewServer: out of memory");
    local l = {0};
    Error err = rpc_server_register(s, BURROW_ANY(TYPE_OF(local), &l));
    check_str(t, error_text(err), BURROW_S("rpc.Register: type local is not exported"));
    err = rpc_server_register(s, BURROW_ANY(TYPE_OF(Arith), &arith));
    CHECK(BURROW_OK(err));
    err = rpc_server_register(s, BURROW_ANY(TYPE_OF(Arith), &arith));
    check_str(t, error_text(err), BURROW_S("rpc: service already defined: Arith"));
    /* A name of one's own does not have to be exported. */
    err = rpc_server_register_name(s, BURROW_S("arith"),
                                   BURROW_ANY(TYPE_OF(Arith), &arith));
    CHECK(BURROW_OK(err));
    rpc_server_free(s);
}

/* --------------------------------------------------------- WriteFailCodec */

/* Go's blocks forever in the reads. These block until the codec is closed, so
 * that the client can be given back. */
typedef struct WriteFailCodec {
    Chan *closed;
} WriteFailCodec;

static Error write_fail_write_request(void *self, const RpcRequest *r, Any body) {
    (void)self;
    (void)r;
    (void)body;
    return errors_new(error_allocator(), BURROW_S("fail"));
}

static Error write_fail_read_response_header(void *self, Alloc *a, RpcResponse *r) {
    WriteFailCodec *c = (WriteFailCodec *)self;
    (void)a;
    (void)r;
    Byte v = 0;
    (void)chan_recv(c->closed, &v);
    return io_eof;
}

static Error write_fail_read_response_body(void *self, Alloc *a, Any body) {
    (void)a;
    (void)body;
    return write_fail_read_response_header(self, NULL, NULL);
}

static Error write_fail_close(void *self) {
    WriteFailCodec *c = (WriteFailCodec *)self;
    chan_close(c->closed);
    return BURROW_NO_ERROR;
}

static const RpcClientCodecVT write_fail_vt = {
    NULL,
    write_fail_write_request,
    write_fail_read_response_header,
    write_fail_read_response_body,
    write_fail_close,
    NULL,
};

static void test_send_deadlock(RpcClient *client) {
    Args args = {7, 8};
    Reply reply = {0};
    (void)call(client, heap_allocator(), "Arith.Add", ARGS(args), REPLY(reply));
}

typedef struct SendDeadlock {
    RpcClient *client;
    Chan *done;
} SendDeadlock;

static void send_deadlock(void *env) {
    SendDeadlock *sd = (SendDeadlock *)env;
    test_send_deadlock(sd->client);
    test_send_deadlock(sd->client);
    bool v = true;
    chan_send(sd->done, &v);
}

static void TestSendDeadlock(TestingT *t) {
    SKIP_WITHOUT_THREADS(t);
    WriteFailCodec codec;
    codec.closed = chan_make(heap_allocator(), TYPE_BYTE, 0);
    if (codec.closed == NULL)
        FATALF("out of memory");
    RpcClient *client = rpc_new_client_with_codec(
        heap_allocator(), (RpcClientCodec){&write_fail_vt, &codec});
    if (client == NULL) {
        chan_free(codec.closed);
        FATALF("NewClientWithCodec: out of memory");
    }

    SendDeadlock sd = {client, chan_make(heap_allocator(), TYPE_BOOL, 0)};
    if (sd.done == NULL || !go(BURROW_FN(Func, send_deadlock, &sd))) {
        rpc_client_free(client);
        chan_free(sd.done);
        chan_free(codec.closed);
        FATALF("no goroutine");
    }
    TimeTimer *timer = time_new_timer(heap_allocator(), 5 * TIME_SECOND);
    SelectCase cases[2] = {
        BURROW_RECV(sd.done, NULL),
        BURROW_RECV(timer != NULL ? time_timer_c(timer) : NULL, NULL),
    };
    Int chosen = chan_select(cases, 2);
    (void)time_timer_stop(timer);
    time_timer_free(timer);
    if (chosen != 0) {
        /* The goroutine still has the client, so it is not given back. */
        FATALF("deadlock");
    }
    rpc_client_free(client);
    chan_free(sd.done);
    chan_free(codec.closed);
}

/* ----------------------------------------------------------- writeCrasher */

typedef struct WriteCrasher {
    Chan *done;
} WriteCrasher;

static Int write_crasher_read(void *self, Slice p, Error *err) {
    WriteCrasher *w = (WriteCrasher *)self;
    (void)p;
    bool v = false;
    (void)chan_recv(w->done, &v);
    BURROW_OUT(err, io_eof);
    return 0;
}

static Int write_crasher_write(void *self, Slice p, Error *err) {
    (void)self;
    (void)p;
    BURROW_OUT(err, errors_new(error_allocator(), BURROW_S("fake write failure")));
    return 0;
}

static Error write_crasher_close(void *self) {
    (void)self;
    return BURROW_NO_ERROR;
}

static const IoReadWriteCloserVT write_crasher_vt = {
    {NULL, write_crasher_read},
    {NULL, write_crasher_write},
    {NULL, write_crasher_close},
};

static void TestClientWriteError(TestingT *t) {
    SKIP_WITHOUT_THREADS(t);
    WriteCrasher w = {chan_make(heap_allocator(), TYPE_BOOL, 0)};
    if (w.done == NULL)
        FATALF("out of memory");
    RpcClient *c =
        rpc_new_client(heap_allocator(), (IoReadWriteCloser){&write_crasher_vt, &w});
    if (c == NULL) {
        chan_free(w.done);
        FATALF("NewClient: out of memory");
    }

    bool res = false;
    Int one = 1;
    Error err = call(c, heap_allocator(), "foo", BURROW_ANY(TYPE_INT, &one),
                     BURROW_ANY(TYPE_BOOL, &res));
    if (BURROW_OK(err)) {
        testing_t_error_v(t, "expected error");
    } else if (!str_eq(error_text(err), BURROW_S("fake write failure"))) {
        testing_t_error_v(t, "unexpected value of error:", err);
    }
    bool v = true;
    chan_send(w.done, &v);
    rpc_client_free(c);
    chan_free(w.done);
}

static void TestTCPClose(TestingT *t) {
    if (!need_tcp(t))
        return;
    use_server();
    HttptestServer *ts = start_http_server();
    Error err = BURROW_NO_ERROR;
    RpcClient *client =
        rpc_dial_http(heap_allocator(), BURROW_S("tcp"), http_addr(ts), &err);
    if (BURROW_FAILED(err)) {
        httptest_server_free(ts);
        FATALF("dialing: %v", err);
    }

    Args args = {17, 8};
    Reply reply = {0};
    err = call(client, heap_allocator(), "Arith.Mul", ARGS(args), REPLY(reply));
    if (BURROW_FAILED(err)) {
        rpc_client_free(client);
        httptest_server_free(ts);
        FATALF("arith error: %v", err);
    }
    testing_t_logf_v(t, "Arith: %d*%d=%v\n", args.A, args.B, REPLY(reply));
    if (reply.C != args.A * args.B)
        testing_t_errorf_v(t, "Add: expected %d got %d", reply.C, args.A * args.B);
    rpc_client_free(client);
    httptest_server_free(ts);
}

static void TestErrorAfterClientClose(TestingT *t) {
    if (!need_tcp(t))
        return;
    use_server();
    HttptestServer *ts = start_http_server();
    Error err = BURROW_NO_ERROR;
    RpcClient *client =
        rpc_dial_http(heap_allocator(), BURROW_S("tcp"), http_addr(ts), &err);
    if (BURROW_FAILED(err)) {
        httptest_server_free(ts);
        FATALF("dialing: %v", err);
    }
    err = rpc_client_close(client);
    if (BURROW_FAILED(err)) {
        rpc_client_free(client);
        httptest_server_free(ts);
        FATALF("close error: %v", err);
    }
    Args args = {7, 9};
    Reply reply = {0};
    err = call(client, heap_allocator(), "Arith.Add", ARGS(args), REPLY(reply));
    if (!errors_is(err, rpc_err_shutdown))
        testing_t_errorf_v(t, "Forever: expected ErrShutdown got %v", err);
    rpc_client_free(client);
    httptest_server_free(ts);
}

/* Tests the fix to issue 11221. Without the fix, this loops forever or
 * crashes. */
static void TestAcceptExitAfterListenerClose(TestingT *t) {
    if (!need_tcp(t))
        return;
    RpcServer *s = rpc_new_server(heap_allocator());
    if (s == NULL)
        FATALF("NewServer: out of memory");
    (void)rpc_server_register(s, BURROW_ANY(TYPE_OF(Arith), &arith));
    (void)rpc_server_register_name(s, BURROW_S("net.rpc.Arith"),
                                   BURROW_ANY(TYPE_OF(Arith), &arith));
    (void)rpc_server_register_name(s, BURROW_S("newServer.Arith"),
                                   BURROW_ANY(TYPE_OF(Arith), &arith));

    Served sv;
    if (!listen_tcp(t, &sv)) {
        rpc_server_free(s);
        return;
    }
    (void)sv.l.vt->closer.close(sv.l.data);
    rpc_server_accept(s, sv.l);
    listener_stop(&sv);
    rpc_server_free(s);
}

typedef struct ShutdownAccept {
    NetListener l;
    Chan *ch;
    NetConn c;
    Error err;
} ShutdownAccept;

static void shutdown_accept(void *env) {
    ShutdownAccept *sa = (ShutdownAccept *)env;
    sa->c = sa->l.vt->accept(sa->l.data, &sa->err);
    (void)sa->l.vt->closer.close(sa->l.data);
    void *v = sa;
    chan_send(sa->ch, &v);
}

typedef struct ShutdownServe {
    RpcServer *s;
    NetConn c;
    SyncWaitGroup wg;
} ShutdownServe;

static void shutdown_serve(void *env) {
    ShutdownServe *ss = (ShutdownServe *)env;
    rpc_server_serve_conn(ss->s, net_conn_as_io_read_write_closer(&ss->c));
    sync_wait_group_done(&ss->wg);
}

static void TestShutdown(TestingT *t) {
    if (!need_tcp(t))
        return;
    Served sv;
    if (!listen_tcp(t, &sv))
        return;
    ShutdownAccept sa;
    memset(&sa, 0, sizeof sa);
    sa.l = sv.l;
    sa.ch = chan_make(heap_allocator(), TYPE_UNSAFE_POINTER, 1);
    NetConn c1 = {NULL, NULL};
    if (sa.ch == NULL || !go(BURROW_FN(Func, shutdown_accept, &sa))) {
        chan_free(sa.ch);
        listener_stop(&sv);
        FATALF("no goroutine");
    }
    Error err = BURROW_NO_ERROR;
    NetConn c = net_dial(heap_allocator(), BURROW_S("tcp"), sv.addr, &err);
    void *got = NULL;
    (void)chan_recv(sa.ch, &got);
    chan_free(sa.ch);
    c1 = sa.c;
    if (BURROW_FAILED(err)) {
        net_conn_free(c1);
        listener_stop(&sv);
        FATALF("%v", err);
    }
    if (c1.vt == NULL) {
        net_conn_free(c);
        listener_stop(&sv);
        FATALF("%v", sa.err);
    }

    ShutdownServe ss;
    memset(&ss, 0, sizeof ss);
    ss.s = rpc_new_server(heap_allocator());
    ss.c = c1;
    (void)rpc_server_register(ss.s, BURROW_ANY(TYPE_OF(Arith), &arith));
    sync_wait_group_add(&ss.wg, 1);
    if (!go(BURROW_FN(Func, shutdown_serve, &ss)))
        sync_wait_group_done(&ss.wg);

    Args args = {7, 8};
    Reply reply = {0};
    RpcClient *client =
        rpc_new_client(heap_allocator(), net_conn_as_io_read_write_closer(&c));
    err = call(client, heap_allocator(), "Arith.Add", ARGS(args), REPLY(reply));
    if (BURROW_FAILED(err))
        testing_t_errorf_v(t, "%v", err);

    args.A = 10; /* 10 ms */
    Chan *done = chan_make(heap_allocator(), TYPE_UNSAFE_POINTER, 1);
    RpcCall *rc = rpc_client_go(client, heap_allocator(), BURROW_S("Arith.SleepMilli"),
                                ARGS(args), REPLY(reply), done);
    (void)net_tcp_conn_close_write(net_conn_as_tcp_conn(c));
    void *v = NULL;
    (void)chan_recv(done, &v);
    if (BURROW_FAILED(rc->error))
        testing_t_errorf_v(t, "%v", rc->error);
    rpc_call_free(rc);
    chan_free(done);

    rpc_client_free(client);
    sync_wait_group_wait(&ss.wg);
    rpc_server_free(ss.s);
    net_conn_free(c1);
    net_conn_free(c);
    listener_stop(&sv);
}

/* ------------------------------------------------------- client_test.go */

typedef struct ShutdownCodec {
    Chan *responded;
    bool closed;
} ShutdownCodec;

static Error shutdown_codec_write_request(void *self, const RpcRequest *r, Any body) {
    (void)self;
    (void)r;
    (void)body;
    return BURROW_NO_ERROR;
}

static Error shutdown_codec_read_response_body(void *self, Alloc *a, Any body) {
    (void)self;
    (void)a;
    (void)body;
    return BURROW_NO_ERROR;
}

static Error shutdown_codec_read_response_header(void *self, Alloc *a, RpcResponse *r) {
    ShutdownCodec *c = (ShutdownCodec *)self;
    (void)a;
    (void)r;
    Int v = 1;
    chan_send(c->responded, &v);
    return errors_new(error_allocator(), BURROW_S("shutdownCodec ReadResponseHeader"));
}

static Error shutdown_codec_close(void *self) {
    ShutdownCodec *c = (ShutdownCodec *)self;
    c->closed = true;
    return BURROW_NO_ERROR;
}

static const RpcClientCodecVT shutdown_codec_vt = {
    NULL,
    shutdown_codec_write_request,
    shutdown_codec_read_response_header,
    shutdown_codec_read_response_body,
    shutdown_codec_close,
    NULL,
};

static void TestCloseCodec(TestingT *t) {
    SKIP_WITHOUT_THREADS(t);
    ShutdownCodec codec = {chan_make(heap_allocator(), TYPE_INT, 0), false};
    if (codec.responded == NULL)
        FATALF("out of memory");
    RpcClient *client = rpc_new_client_with_codec(
        heap_allocator(), (RpcClientCodec){&shutdown_codec_vt, &codec});
    if (client == NULL) {
        chan_free(codec.responded);
        FATALF("NewClientWithCodec: out of memory");
    }
    Int v = 0;
    (void)chan_recv(codec.responded, &v);
    (void)rpc_client_close(client);
    if (!codec.closed)
        testing_t_error_v(t, "client.Close did not close codec");
    rpc_client_free(client);
    chan_free(codec.responded);
}

/* Test that errors in gob shut down the connection. Issue 7689. */

#define R_FIELDS(F, T)                                                                 \
    F(T, Bytes, msg, "") /* Not exported, so R does not work with gob. */
BURROW_STRUCT(R, R_FIELDS);
BURROW_PTR_TYPE(RPtr, R);

/* struct{}, which C cannot write, so it is a descriptor of no size over a
 * struct that is never completed. */
typedef struct EmptyStruct EmptyStruct;
static const Type burrow_type_EmptyStruct = {
    {NULL, 0}, {NULL, 0}, KIND_STRUCT, 0, 1, 0, 0, NULL, NULL, NULL, NULL, 0, 0, NULL,
};
BURROW_PTR_TYPE(EmptyStructPtr, EmptyStruct);

#define S_FIELDS(F, T) F(T, Int, unused, "")
BURROW_STRUCT_DECL(S, S_FIELDS);

static Error s_recv(S *s, EmptyStructPtr nul, RPtr reply) {
    (void)s;
    (void)nul;
    reply->msg = BURROW_B("foo");
    return BURROW_NO_ERROR;
}

#define S_RECV_SIG(IN, OUT) IN(0, EmptyStructPtr) IN(1, RPtr) OUT(Error)
#define S_METHODS(M, T) M(T, Recv, s_recv, S_RECV_SIG)
BURROW_STRUCT_DEFINE_METHODS(S, S_FIELDS, S_METHODS);

static S s_service;

static void TestGobError(TestingT *t) {
    if (!need_tcp(t))
        return;
    (void)rpc_register(BURROW_ANY(TYPE_OF(S), &s_service));
    Served sv;
    if (!serve_start(t, &sv, NULL))
        return;

    Error err = BURROW_NO_ERROR;
    RpcClient *client = rpc_dial(heap_allocator(), BURROW_S("tcp"), sv.addr, &err);
    if (BURROW_FAILED(err)) {
        serve_stop(&sv);
        FATALF("%v", err);
    }
    Reply reply = {0};
    Byte nothing = 0;
    err = call(client, heap_allocator(), "S.Recv",
               BURROW_ANY(TYPE_OF(EmptyStruct), &nothing), REPLY(reply));
    if (BURROW_OK(err))
        testing_t_fatalf_v(t, "no error");
    else if (!strings_contains(error_text(err),
                               BURROW_S("reading body unexpected EOF")))
        testing_t_error_v(t, "expected `reading body unexpected EOF', got", err);
    rpc_client_free(client);
    serve_stop(&sv);
}

#define TESTS(X)                                                                       \
    X(TestRPC)                                                                         \
    X(TestHTTP)                                                                        \
    X(TestHTTPErrors)                                                                  \
    X(TestDebugHTTP)                                                                   \
    X(TestBuiltinTypes)                                                                \
    X(TestServeRequest)                                                                \
    X(TestRegistrationError)                                                           \
    X(TestRegistrationErrors)                                                          \
    X(TestSendDeadlock)                                                                \
    X(TestClientWriteError)                                                            \
    X(TestTCPClose)                                                                    \
    X(TestErrorAfterClientClose)                                                       \
    X(TestAcceptExitAfterListenerClose)                                                \
    X(TestShutdown)                                                                    \
    X(TestCloseCodec)                                                                  \
    X(TestGobError)

TESTING_MAIN(TESTS)
