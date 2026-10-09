/* Derived from Go's src/net/rpc/jsonrpc/all_test.go.
 *
 * Go's Arith is an int, and its BuiltinTypes an empty struct. A descriptor
 * here needs at least one field, so each has one nobody reads. Arith.Error,
 * which panics and which no test calls, is left out. BuiltinTypes.Slice takes
 * an allocator as its first argument, which is how a method appends to a reply
 * here.
 *
 * Go's TestMalformedOutput writes its response as soon as the client is made,
 * and gets away with it because the client's request is in its pending table
 * by the time the response is read. With threads that is a race, so the fake
 * server here reads the request first.
 *
 * Copyright 2010 The Go Authors. All rights reserved.
 * Copyright 2026 The burrow Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style licence that can be found
 * in the LICENSE file. */

#include "check.h"

#include "burrow/burrow.h"
#include "burrow/chan.h"
#include "burrow/declare.h"
#include "burrow/encoding.h"
#include "burrow/encoding/json.h"
#include "burrow/error.h"
#include "burrow/fmt.h"
#include "burrow/func.h"
#include "burrow/io.h"
#include "burrow/map.h"
#include "burrow/mem.h"
#include "burrow/mem/arena.h"
#include "burrow/mem/heap.h"
#include "burrow/net.h"
#include "burrow/net/rpc.h"
#include "burrow/net/rpc/jsonrpc.h"
#include "burrow/proc.h"
#include "burrow/slice.h"
#include "burrow/strings.h"
#include "burrow/sync.h"
#include "burrow/testing.h"

#include <stdint.h>
#include <string.h>

#define FATALF(...)                                                                    \
    do {                                                                               \
        testing_t_fatalf_v(t, __VA_ARGS__);                                            \
        return;                                                                        \
    } while (0)

/* ------------------------------------------------------------------ types */

#define ARGS_FIELDS(F, T) F(T, Int, A, "") F(T, Int, B, "")
BURROW_STRUCT(Args, ARGS_FIELDS);

#define REPLY_FIELDS(F, T) F(T, Int, C, "")
BURROW_STRUCT(Reply, REPLY_FIELDS);

BURROW_PTR_TYPE(ArgsPtr, Args);
BURROW_PTR_TYPE(ReplyPtr, Reply);
BURROW_MAP_TYPE(IntIntMap, Int, Int);
BURROW_PTR_TYPE(IntIntMapPtr, IntIntMap);
BURROW_SLICE_TYPE(IntSlice, Int);
BURROW_PTR_TYPE(IntSlicePtr, IntSlice);
BURROW_ARRAY_TYPE(IntArray1, Int, 1);
BURROW_PTR_TYPE(IntArray1Ptr, IntArray1);

#define ARITH_ADD_RESP_FIELDS(F, T)                                                    \
    F(T, Any, Id, "json:\"id\"")                                                       \
    F(T, Reply, Result, "json:\"result\"")                                             \
    F(T, Any, Error, "json:\"error\"")
BURROW_STRUCT(ArithAddResp, ARITH_ADD_RESP_FIELDS);

#define ARITH_FIELDS(F, T) F(T, Int, N, "")
BURROW_STRUCT_DECL(Arith, ARITH_FIELDS);

static Error arith_add(Arith *t, ArgsPtr args, ReplyPtr reply) {
    (void)t;
    reply->C = args->A + args->B;
    return BURROW_NO_ERROR;
}

static Error arith_mul(Arith *t, ArgsPtr args, ReplyPtr reply) {
    (void)t;
    reply->C = args->A * args->B;
    return BURROW_NO_ERROR;
}

static Error arith_div(Arith *t, ArgsPtr args, ReplyPtr reply) {
    (void)t;
    if (args->B == 0)
        return errors_new(error_allocator(), BURROW_S("divide by zero"));
    reply->C = args->A / args->B;
    return BURROW_NO_ERROR;
}

#define ARITH_SIG(IN, OUT) IN(0, ArgsPtr) IN(1, ReplyPtr) OUT(Error)
#define ARITH_METHODS(M, T)                                                            \
    M(T, Add, arith_add, ARITH_SIG)                                                    \
    M(T, Div, arith_div, ARITH_SIG)                                                    \
    M(T, Mul, arith_mul, ARITH_SIG)
BURROW_STRUCT_DEFINE_METHODS(Arith, ARITH_FIELDS, ARITH_METHODS);

#define BUILTIN_FIELDS(F, T) F(T, Int, unused, "")
BURROW_STRUCT_DECL(BuiltinTypes, BUILTIN_FIELDS);

static Error builtin_map(BuiltinTypes *t, Int i, IntIntMapPtr reply) {
    (void)t;
    if (!map_set(*reply, &i, &i))
        return burrow_err_out_of_memory;
    return BURROW_NO_ERROR;
}

static Error builtin_slice(BuiltinTypes *t, EncodingAllocArg a, Int i,
                           IntSlicePtr reply) {
    (void)t;
    *reply = slice_append(a, *reply, &i, 1);
    return BURROW_NO_ERROR;
}

static Error builtin_array(BuiltinTypes *t, Int i, IntArray1Ptr reply) {
    (void)t;
    reply->v[0] = i;
    return BURROW_NO_ERROR;
}

#define BUILTIN_ARRAY_SIG(IN, OUT) IN(0, Int) IN(1, IntArray1Ptr) OUT(Error)
#define BUILTIN_MAP_SIG(IN, OUT) IN(0, Int) IN(1, IntIntMapPtr) OUT(Error)
#define BUILTIN_SLICE_SIG(IN, OUT)                                                     \
    IN(0, EncodingAllocArg) IN(1, Int) IN(2, IntSlicePtr) OUT(Error)
#define BUILTIN_METHODS(M, T)                                                          \
    M(T, Array, builtin_array, BUILTIN_ARRAY_SIG)                                      \
    M(T, Map, builtin_map, BUILTIN_MAP_SIG)                                            \
    M(T, Slice, builtin_slice, BUILTIN_SLICE_SIG)
BURROW_STRUCT_DEFINE_METHODS(BuiltinTypes, BUILTIN_FIELDS, BUILTIN_METHODS);

static Arith arith;
static BuiltinTypes builtin_types;
static SyncOnce once;

static void init_server(void *env) {
    (void)env;
    (void)rpc_register(BURROW_ANY(TYPE_OF(Arith), &arith));
    (void)rpc_register(BURROW_ANY(TYPE_OF(BuiltinTypes), &builtin_types));
}

static void use_server(void) {
    sync_once_do(&once, BURROW_FN(Func, init_server, NULL));
}

/* ------------------------------------------------------------------ pipes */

/* A net.Pipe with ServeConn running on its server end. */
typedef struct Piped {
    NetConn cli, srv;
    SyncWaitGroup wg;
} Piped;

static void piped_serve(void *env) {
    Piped *p = (Piped *)env;
    jsonrpc_serve_conn(net_conn_as_io_read_write_closer(&p->srv));
    sync_wait_group_done(&p->wg);
}

static bool pipe_serve(TestingT *t, Piped *p) {
    use_server();
    memset(p, 0, sizeof *p);
    net_pipe(heap_allocator(), &p->cli, &p->srv);
    if (p->cli.vt == NULL) {
        testing_t_errorf_v(t, "net.Pipe: out of memory");
        return false;
    }
    sync_wait_group_add(&p->wg, 1);
    if (!go(BURROW_FN(Func, piped_serve, p))) {
        sync_wait_group_done(&p->wg);
        net_pipe_free(p->cli);
        testing_t_errorf_v(t, "no goroutine for ServeConn");
        return false;
    }
    return true;
}

/* Hangs up the client end, which ends ServeConn, and frees the pipe once it
 * has. */
static void pipe_stop(Piped *p) {
    (void)p->cli.vt->closer.close(p->cli.data);
    sync_wait_group_wait(&p->wg);
    net_pipe_free(p->cli);
}

static bool resp_has_error(const ArithAddResp *resp) {
    return resp->Error.t != NULL;
}

/* Sends one hand-written request and decodes the response to it. */
static Error round_trip(Piped *p, Alloc *a, Str req, ArithAddResp *resp) {
    memset(resp, 0, sizeof *resp);
    Error err = BURROW_NO_ERROR;
    (void)io_write_string(net_conn_as_io_writer(p->cli), req, &err);
    if (BURROW_FAILED(err))
        return err;
    JsonDecoder *dec = json_new_decoder(a, net_conn_as_io_reader(p->cli));
    if (dec == NULL)
        return burrow_err_out_of_memory;
    return json_decoder_decode(dec, BURROW_ANY(TYPE_OF(ArithAddResp), resp));
}

static void TestServerNoParams(TestingT *t) {
    SKIP_WITHOUT_THREADS(t);
    Piped p;
    if (!pipe_serve(t, &p))
        return;
    Arena ar;
    arena_init(&ar, heap_allocator(), 0);
    ArithAddResp resp;
    Error err =
        round_trip(&p, arena_allocator(&ar),
                   BURROW_S("{\"method\": \"Arith.Add\", \"id\": \"123\"}"), &resp);
    if (BURROW_FAILED(err))
        testing_t_errorf_v(t, "Decode after no params: %v", err);
    else if (!resp_has_error(&resp))
        testing_t_errorf_v(t, "Expected error, got nil");
    pipe_stop(&p);
    arena_free(&ar);
}

static void TestServerEmptyMessage(TestingT *t) {
    SKIP_WITHOUT_THREADS(t);
    Piped p;
    if (!pipe_serve(t, &p))
        return;
    Arena ar;
    arena_init(&ar, heap_allocator(), 0);
    ArithAddResp resp;
    Error err = round_trip(&p, arena_allocator(&ar), BURROW_S("{}"), &resp);
    if (BURROW_FAILED(err))
        testing_t_errorf_v(t, "Decode after empty: %v", err);
    else if (!resp_has_error(&resp))
        testing_t_errorf_v(t, "Expected error, got nil");
    pipe_stop(&p);
    arena_free(&ar);
}

static void TestServer(TestingT *t) {
    SKIP_WITHOUT_THREADS(t);
    Piped p;
    if (!pipe_serve(t, &p))
        return;
    Arena ar;
    arena_init(&ar, heap_allocator(), 0);
    Alloc *a = arena_allocator(&ar);
    JsonDecoder *dec = json_new_decoder(a, net_conn_as_io_reader(p.cli));
    IoWriter w = net_conn_as_io_writer(p.cli);

    /* Send hand-coded requests to server, parse responses. */
    for (Int i = 0; i < 10 && dec != NULL; i++) {
        fmt_fprintf_v(w,
                      "{\"method\": \"Arith.Add\", \"id\": \"\\u%04d\", "
                      "\"params\": [{\"A\": %d, \"B\": %d}]}",
                      i, i, i + 1);
        ArithAddResp resp;
        memset(&resp, 0, sizeof resp);
        Error err = json_decoder_decode(dec, BURROW_ANY(TYPE_OF(ArithAddResp), &resp));
        if (BURROW_FAILED(err)) {
            testing_t_errorf_v(t, "Decode: %v", err);
            break;
        }
        if (resp_has_error(&resp)) {
            testing_t_errorf_v(t, "resp.Error: %v", resp.Error);
            break;
        }
        Byte want = (Byte)i;
        if (resp.Id.t != TYPE_STRING ||
            !str_eq(*(const Str *)resp.Id.data, str_from_bytes(&want, 1))) {
            testing_t_errorf_v(t, "resp: bad id %v want %q", resp.Id,
                               str_from_bytes(&want, 1));
            break;
        }
        if (resp.Result.C != 2 * i + 1) {
            testing_t_errorf_v(t, "resp: bad result: %d+%d=%d", i, i + 1,
                               resp.Result.C);
            break;
        }
    }
    if (dec == NULL)
        testing_t_errorf_v(t, "NewDecoder: out of memory");
    pipe_stop(&p);
    arena_free(&ar);
}

#define ARGS(x) BURROW_ANY(TYPE_OF(Args), &(x))
#define REPLY(x) BURROW_ANY(TYPE_OF(Reply), &(x))

static Error call(RpcClient *c, Alloc *a, const char *method, Any args, Any reply) {
    return rpc_client_call(c, a, str_from_cstr(method), args, reply);
}

static void TestClient(TestingT *t) {
    SKIP_WITHOUT_THREADS(t);
    /* Assume server is okay (TestServer is above).
     * Test client against server. */
    Piped p;
    if (!pipe_serve(t, &p))
        return;
    RpcClient *client =
        jsonrpc_new_client(heap_allocator(), net_conn_as_io_read_write_closer(&p.cli));
    if (client == NULL) {
        pipe_stop(&p);
        FATALF("NewClient: out of memory");
    }
    Arena ar;
    arena_init(&ar, heap_allocator(), 0);
    Alloc *a = arena_allocator(&ar);

    /* Synchronous calls */
    Args args = {7, 8};
    Reply reply = {0};
    Error err = call(client, a, "Arith.Add", ARGS(args), REPLY(reply));
    if (BURROW_FAILED(err))
        testing_t_errorf_v(t, "Add: expected no error but got string %q",
                           error_text(err));
    if (reply.C != args.A + args.B)
        testing_t_errorf_v(t, "Add: got %d expected %d", reply.C, args.A + args.B);

    args = (Args){7, 8};
    reply = (Reply){0};
    err = call(client, a, "Arith.Mul", ARGS(args), REPLY(reply));
    if (BURROW_FAILED(err))
        testing_t_errorf_v(t, "Mul: expected no error but got string %q",
                           error_text(err));
    if (reply.C != args.A * args.B)
        testing_t_errorf_v(t, "Mul: got %d expected %d", reply.C, args.A * args.B);

    /* Out of order. */
    args = (Args){7, 8};
    Reply mul_reply = {0};
    RpcCall *mul_call = rpc_client_go(client, heap_allocator(), BURROW_S("Arith.Mul"),
                                      ARGS(args), REPLY(mul_reply), NULL);
    Reply add_reply = {0};
    RpcCall *add_call = rpc_client_go(client, heap_allocator(), BURROW_S("Arith.Add"),
                                      ARGS(args), REPLY(add_reply), NULL);
    if (mul_call != NULL && add_call != NULL) {
        void *v = NULL;
        (void)chan_recv(add_call->done, &v);
        if (BURROW_FAILED(add_call->error))
            testing_t_errorf_v(t, "Add: expected no error but got string %q",
                               error_text(add_call->error));
        if (add_reply.C != args.A + args.B)
            testing_t_errorf_v(t, "Add: got %d expected %d", add_reply.C,
                               args.A + args.B);

        (void)chan_recv(mul_call->done, &v);
        if (BURROW_FAILED(mul_call->error))
            testing_t_errorf_v(t, "Mul: expected no error but got string %q",
                               error_text(mul_call->error));
        if (mul_reply.C != args.A * args.B)
            testing_t_errorf_v(t, "Mul: got %d expected %d", mul_reply.C,
                               args.A * args.B);
    } else {
        testing_t_errorf_v(t, "Go: out of memory");
    }
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

    rpc_client_free(client);
    pipe_stop(&p);
    arena_free(&ar);
}

static void TestBuiltinTypes(TestingT *t) {
    SKIP_WITHOUT_THREADS(t);
    Piped p;
    if (!pipe_serve(t, &p))
        return;
    RpcClient *client =
        jsonrpc_new_client(heap_allocator(), net_conn_as_io_read_write_closer(&p.cli));
    if (client == NULL) {
        pipe_stop(&p);
        FATALF("NewClient: out of memory");
    }
    Arena ar;
    arena_init(&ar, heap_allocator(), 0);
    Alloc *a = arena_allocator(&ar);

    /* Map */
    Int arg = 7;
    IntIntMap reply_map = map_make(a, TYPE_INT, TYPE_INT, 0);
    Error err = call(client, a, "BuiltinTypes.Map", BURROW_ANY(TYPE_INT, &arg),
                     BURROW_ANY(TYPE_OF(IntIntMap), &reply_map));
    if (BURROW_FAILED(err))
        testing_t_errorf_v(t, "Map: expected no error but got string %q",
                           error_text(err));
    const Int *got = (const Int *)map_get(reply_map, &arg);
    if (got == NULL || *got != arg)
        testing_t_errorf_v(t, "Map: expected %d got %d", arg, got != NULL ? *got : 0);

    /* Slice */
    IntSlice reply_slice = slice_make(a, TYPE_INT, 0, 0);
    err = call(client, a, "BuiltinTypes.Slice", BURROW_ANY(TYPE_INT, &arg),
               BURROW_ANY(TYPE_OF(IntSlice), &reply_slice));
    if (BURROW_FAILED(err))
        testing_t_errorf_v(t, "Slice: expected no error but got string %q",
                           error_text(err));
    if (reply_slice.len != 1 || ((const Int *)reply_slice.p)[0] != arg)
        testing_t_errorf_v(t, "Slice: expected [%d] got %v", arg,
                           BURROW_ANY(TYPE_OF(IntSlice), &reply_slice));

    /* Array */
    IntArray1 reply_array = {{0}};
    err = call(client, a, "BuiltinTypes.Array", BURROW_ANY(TYPE_INT, &arg),
               BURROW_ANY(TYPE_OF(IntArray1), &reply_array));
    if (BURROW_FAILED(err))
        testing_t_errorf_v(t, "Array: expected no error but got string %q",
                           error_text(err));
    if (reply_array.v[0] != arg)
        testing_t_errorf_v(t, "Array: expected [%d] got [%d]", arg, reply_array.v[0]);

    rpc_client_free(client);
    pipe_stop(&p);
    arena_free(&ar);
}

/* ------------------------------------------------------ malformed streams */

typedef struct PipeWrite {
    NetConn c;
    Str s;
    SyncWaitGroup wg;
} PipeWrite;

static void pipe_write(void *env) {
    PipeWrite *pw = (PipeWrite *)env;
    Error err = BURROW_NO_ERROR;
    (void)io_write_string(net_conn_as_io_writer(pw->c), pw->s, &err);
    sync_wait_group_done(&pw->wg);
}

static void TestMalformedInput(TestingT *t) {
    SKIP_WITHOUT_THREADS(t);
    use_server();
    NetConn cli, srv;
    net_pipe(heap_allocator(), &cli, &srv);
    if (cli.vt == NULL)
        FATALF("net.Pipe: out of memory");
    PipeWrite pw;
    memset(&pw, 0, sizeof pw);
    pw.c = cli;
    pw.s = BURROW_S("{id:1}"); /* invalid json */
    sync_wait_group_add(&pw.wg, 1);
    if (!go(BURROW_FN(Func, pipe_write, &pw))) {
        net_pipe_free(cli);
        FATALF("no goroutine");
    }
    jsonrpc_serve_conn(
        net_conn_as_io_read_write_closer(&srv)); /* must return, not loop */
    sync_wait_group_wait(&pw.wg);
    (void)cli.vt->closer.close(cli.data);
    net_pipe_free(cli);
}

/* The other end of TestMalformedOutput's client: reads the request, answers it
 * with a response that has neither a result nor an error, and then reads
 * whatever comes until the client hangs up. */
typedef struct BadServer {
    NetConn c;
    SyncWaitGroup wg;
} BadServer;

static void bad_server(void *env) {
    BadServer *bs = (BadServer *)env;
    IoReader r = net_conn_as_io_reader(bs->c);
    Error err = BURROW_NO_ERROR;
    Byte b = 0;
    Slice one = {&b, 1, 1, TYPE_BYTE};
    while (BURROW_OK(err) && b != '\n')
        (void)r.vt->read(r.data, one, &err);
    if (BURROW_OK(err))
        (void)io_write_string(net_conn_as_io_writer(bs->c),
                              BURROW_S("{\"id\":0,\"result\":null,\"error\":null}"),
                              &err);
    Arena ar;
    arena_init(&ar, heap_allocator(), 0);
    (void)io_read_all(arena_allocator(&ar), r, &err);
    arena_free(&ar);
    sync_wait_group_done(&bs->wg);
}

static void TestMalformedOutput(TestingT *t) {
    SKIP_WITHOUT_THREADS(t);
    NetConn cli, srv;
    net_pipe(heap_allocator(), &cli, &srv);
    if (cli.vt == NULL)
        FATALF("net.Pipe: out of memory");
    BadServer bs;
    memset(&bs, 0, sizeof bs);
    bs.c = srv;
    sync_wait_group_add(&bs.wg, 1);
    if (!go(BURROW_FN(Func, bad_server, &bs))) {
        net_pipe_free(cli);
        FATALF("no goroutine");
    }

    RpcClient *client =
        jsonrpc_new_client(heap_allocator(), net_conn_as_io_read_write_closer(&cli));
    if (client != NULL) {
        Args args = {7, 8};
        Reply reply = {0};
        Error err =
            call(client, heap_allocator(), "Arith.Add", ARGS(args), REPLY(reply));
        if (BURROW_OK(err))
            testing_t_error_v(t, "expected error");
        rpc_client_free(client);
    } else {
        testing_t_errorf_v(t, "NewClient: out of memory");
        (void)cli.vt->closer.close(cli.data);
    }
    sync_wait_group_wait(&bs.wg);
    net_pipe_free(cli);
}

/* --------------------------------------------- TestServerErrorHasNullResult */

/* Go's struct of an io.Reader, an io.Writer and an io.Closer. */
typedef struct StringsRWC {
    StringsReader r;
    StringsBuilder w;
} StringsRWC;

static Int strings_rwc_read(void *self, Slice p, Error *err) {
    StringsRWC *c = (StringsRWC *)self;
    IoReader r = strings_reader_as_io_reader(&c->r);
    return r.vt->read(r.data, p, err);
}

static Int strings_rwc_write(void *self, Slice p, Error *err) {
    StringsRWC *c = (StringsRWC *)self;
    IoWriter w = strings_builder_as_io_writer(&c->w);
    return w.vt->write(w.data, p, err);
}

static Error strings_rwc_close(void *self) {
    (void)self;
    return BURROW_NO_ERROR;
}

static const IoReadWriteCloserVT strings_rwc_vt = {
    {NULL, strings_rwc_read},
    {NULL, strings_rwc_write},
    {NULL, strings_rwc_close},
};

#define VALUE_TEXT "the value we don't want to see"
#define ERROR_TEXT "some error"

static void TestServerErrorHasNullResult(TestingT *t) {
    Arena ar;
    arena_init(&ar, heap_allocator(), 0);
    Alloc *a = arena_allocator(&ar);
    StringsRWC rwc;
    memset(&rwc, 0, sizeof rwc);
    strings_reader_reset(
        &rwc.r,
        BURROW_S("{\"method\": \"Arith.Add\", \"id\": \"123\", \"params\": []}"));
    rwc.w = STRINGS_BUILDER(a);
    RpcServerCodec sc = jsonrpc_new_server_codec(
        heap_allocator(), (IoReadWriteCloser){&strings_rwc_vt, &rwc});
    if (sc.vt == NULL) {
        arena_free(&ar);
        FATALF("NewServerCodec: out of memory");
    }
    RpcRequest r;
    memset(&r, 0, sizeof r);
    Error err = sc.vt->read_request_header(sc.data, a, &r);
    if (BURROW_FAILED(err)) {
        sc.vt->free(sc.data);
        arena_free(&ar);
        FATALF("%v", err);
    }
    Str value = BURROW_S(VALUE_TEXT);
    RpcResponse resp;
    resp.service_method = BURROW_S("Method");
    resp.seq = 1;
    resp.error = BURROW_S(ERROR_TEXT);
    err = sc.vt->write_response(sc.data, &resp, BURROW_ANY(TYPE_STRING, &value));
    if (BURROW_FAILED(err)) {
        sc.vt->free(sc.data);
        arena_free(&ar);
        FATALF("%v", err);
    }
    Str out = strings_builder_string(&rwc.w);
    if (!strings_contains(out, BURROW_S(ERROR_TEXT)))
        testing_t_errorf_v(t, "Response didn't contain expected error %q: %s",
                           BURROW_S(ERROR_TEXT), out);
    if (strings_contains(out, BURROW_S(VALUE_TEXT)))
        testing_t_errorf_v(t, "Response contains both an error and value: %s", out);
    sc.vt->free(sc.data);
    arena_free(&ar);
}

/* ------------------------------------------------------ TestUnexpectedError */

/* Go's pipe, copied from package net: a reader from one io.Pipe and a writer
 * to the other. */
typedef struct PipePair {
    IoPipeReader *r;
    IoPipeWriter *w;
} PipePair;

static Int pipe_pair_read(void *self, Slice p, Error *err) {
    return io_pipe_reader_read(((PipePair *)self)->r, p, err);
}

static Int pipe_pair_write(void *self, Slice p, Error *err) {
    return io_pipe_writer_write(((PipePair *)self)->w, p, err);
}

static Error pipe_pair_close(void *self) {
    PipePair *p = (PipePair *)self;
    Error err = io_pipe_reader_close(p->r);
    Error err1 = io_pipe_writer_close(p->w);
    if (BURROW_OK(err))
        err = err1;
    return err;
}

static const IoReadWriteCloserVT pipe_pair_vt = {
    {NULL, pipe_pair_read},
    {NULL, pipe_pair_write},
    {NULL, pipe_pair_close},
};

BURROW_SENTINEL_ERROR(jsonrpc_test_unexpected, "unexpected error!");

typedef struct CloseWithError {
    IoPipeWriter *w;
    SyncWaitGroup wg;
} CloseWithError;

static void close_with_error(void *env) {
    CloseWithError *c = (CloseWithError *)env;
    /* reader will get this error */
    (void)io_pipe_writer_close_with_error(c->w, jsonrpc_test_unexpected);
    sync_wait_group_done(&c->wg);
}

static void TestUnexpectedError(TestingT *t) {
    SKIP_WITHOUT_THREADS(t);
    use_server();
    IoPipeReader *r1 = NULL, *r2 = NULL;
    IoPipeWriter *w1 = NULL, *w2 = NULL;
    io_pipe(heap_allocator(), &r1, &w1);
    io_pipe(heap_allocator(), &r2, &w2);
    if (r1 == NULL || r2 == NULL) {
        io_pipe_free(r1);
        io_pipe_free(r2);
        FATALF("io.Pipe: out of memory");
    }
    PipePair cli = {r1, w2};
    PipePair srv = {r2, w1};
    CloseWithError cwe;
    memset(&cwe, 0, sizeof cwe);
    cwe.w = cli.w;
    sync_wait_group_add(&cwe.wg, 1);
    if (!go(BURROW_FN(Func, close_with_error, &cwe))) {
        io_pipe_free(r1);
        io_pipe_free(r2);
        FATALF("no goroutine");
    }
    jsonrpc_serve_conn(
        (IoReadWriteCloser){&pipe_pair_vt, &srv}); /* must return, not loop */
    sync_wait_group_wait(&cwe.wg);
    (void)pipe_pair_close(&cli);
    io_pipe_free(r1);
    io_pipe_free(r2);
}

#define TESTS(X)                                                                       \
    X(TestServerNoParams)                                                              \
    X(TestServerEmptyMessage)                                                          \
    X(TestServer)                                                                      \
    X(TestClient)                                                                      \
    X(TestBuiltinTypes)                                                                \
    X(TestMalformedInput)                                                              \
    X(TestMalformedOutput)                                                             \
    X(TestServerErrorHasNullResult)                                                    \
    X(TestUnexpectedError)

TESTING_MAIN(TESTS)
