/* Derived from Go's src/net/http/fcgi/fcgi_test.go.
 *
 * Go reads the response's header after the handler is done with it, which is
 * after the request's goroutine may have freed it here, so
 * TestResponseWriterSniffsContentType reads it from inside the handler. The
 * connections a child is handed outlive the test that made them in Go; here
 * the parts the child keeps using after child.serve returns are static ones.
 * Go source: go1.27.1.
 *
 * Copyright 2011 The Go Authors. All rights reserved.
 * Copyright 2026 The burrow Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style licence that can be found
 * in the LICENSE file. */

#include "check.h"

#include "../src/net/fcgi_internal.h"

#include "burrow/bytes.h"
#include "burrow/chan.h"
#include "burrow/core.h"
#include "burrow/error.h"
#include "burrow/fmt.h"
#include "burrow/func.h"
#include "burrow/io.h"
#include "burrow/map.h"
#include "burrow/mem.h"
#include "burrow/mem/arena.h"
#include "burrow/mem/heap.h"
#include "burrow/net/http.h"
#include "burrow/net/http/fcgi.h"
#include "burrow/proc.h"
#include "burrow/strings.h"
#include "burrow/sync.h"
#include "burrow/time.h"
#include "burrow/type.h"

#include <stdint.h>
#include <string.h>

#define S BURROW_S

/* t.Fatalf, with a return the analyzer can see. */
#define FATALF(...)                                                                    \
    do {                                                                               \
        testing_t_fatalf_v(t, __VA_ARGS__);                                            \
        return;                                                                        \
    } while (0)

#define ARENA_BEGIN                                                                    \
    Arena ar;                                                                          \
    arena_init(&ar, NULL, 0);                                                          \
    Alloc *a = arena_allocator(&ar)
#define ARENA_END arena_free(&ar)

static Slice bytes_of(Str s) {
    return (Slice){(void *)(uintptr_t)s.p, s.len, s.len, TYPE_BYTE};
}

static bool slice_eq(Slice x, Slice y) {
    return x.len == y.len && (x.len == 0 || memcmp(x.p, y.p, (size_t)x.len) == 0);
}

/* The Close of nilCloser, nopWriteCloser and rwNopCloser. */
static Error nop_close(void *self) {
    (void)self;
    return BURROW_NO_ERROR;
}

static const IoCloserVT nop_closer_vt = {NULL, nop_close};

static const IoCloser nop_closer = {&nop_closer_vt, NULL};

/* --------------------------------------------------------------- TestSize */

static const struct {
    uint32_t size;
    const char *bytes;
    Int n;
} size_tests[] = {
    {0, "\x00", 1},
    {127, "\x7F", 1},
    {128, "\x80\x00\x00\x80", 4},
    {1000, "\x80\x00\x03\xE8", 4},
    {33554431, "\x81\xFF\xFF\xFF", 4},
};

static void TestSize(TestingT *t) {
    Byte b[4];
    for (size_t i = 0; i < sizeof size_tests / sizeof size_tests[0]; i++) {
        Slice want = {(void *)(uintptr_t)size_tests[i].bytes, size_tests[i].n,
                      size_tests[i].n, TYPE_BYTE};
        Int n = burrow__fcgi_encode_size(b, size_tests[i].size);
        Slice got = {b, n, n, TYPE_BYTE}, all = {b, 4, 4, TYPE_BYTE};
        if (!slice_eq(got, want))
            testing_t_errorf_v(t, "%d expected %x, encoded %x", (Int)i, want, all);
        uint32_t size = burrow__fcgi_read_size(want, &n);
        if (size != size_tests[i].size)
            testing_t_errorf_v(t, "%d expected %d, read %d", (Int)i,
                               (int64_t)size_tests[i].size, (int64_t)size);
        if (want.len != n)
            testing_t_errorf_v(t, "%d did not consume all the bytes", (Int)i);
    }
}

/* ------------------------------------------------------------ TestStreams */

static void put_bytes(BytesBuffer *b, const void *p, Int n) {
    (void)bytes_buffer_write(b, (Slice){(void *)(uintptr_t)p, n, n, TYPE_BYTE}, NULL);
}

static void put_zeros(BytesBuffer *b, Int n) {
    static const Byte zeros[1024];
    while (n > 0) {
        Int k = n < (Int)sizeof zeros ? n : (Int)sizeof zeros;
        put_bytes(b, zeros, k);
        n -= k;
    }
}

typedef struct StreamTest {
    const char *desc;
    uint8_t rec_type;
    uint16_t req_id;
    Slice content;
    Slice raw;
} StreamTest;

static void stream_case(TestingT *t, StreamTest *test, burrow__FcgiRecord *rec) {
    Alloc *a = heap_allocator();
    /* Go's bytes.NewBuffer(test.raw) shares the slice, but there it is a
     * fresh []byte. Here raw may be a string literal, which the writes
     * below would land on, so the buffer gets its own copy. */
    BytesBuffer *buf = bytes_new_buffer(a, (Slice){0});
    BytesBuffer *content = bytes_new_buffer(a, (Slice){0});
    if (buf == NULL || content == NULL)
        FATALF("%s: out of memory", test->desc);
    (void)bytes_buffer_write(buf, test->raw, NULL);
    while (bytes_buffer_len(buf) > 0) {
        Error err = burrow__fcgi_record_read(rec, bytes_buffer_as_io_reader(buf));
        if (BURROW_FAILED(err)) {
            testing_t_errorf_v(t, "%s: error reading record: %v", test->desc, err);
            goto out;
        }
        (void)bytes_buffer_write(content, burrow__fcgi_record_content(rec), NULL);
    }
    if (rec->h.type != test->rec_type) {
        testing_t_errorf_v(t, "%s: got type %d expected %d", test->desc,
                           (int64_t)rec->h.type, (int64_t)test->rec_type);
        goto out;
    }
    if (rec->h.id != test->req_id) {
        testing_t_errorf_v(t, "%s: got request ID %d expected %d", test->desc,
                           (int64_t)rec->h.id, (int64_t)test->req_id);
        goto out;
    }
    if (!slice_eq(bytes_buffer_bytes(content), test->content)) {
        testing_t_errorf_v(t, "%s: read wrong content", test->desc);
        goto out;
    }
    bytes_buffer_reset(buf);
    burrow__FcgiConn *c = burrow__fcgi_new_conn(
        a, bytes_buffer_as_io_reader(buf), bytes_buffer_as_io_writer(buf), nop_closer);
    burrow__FcgiWriter *w = burrow__fcgi_new_writer(a, c, test->rec_type, test->req_id);
    if (c == NULL || w == NULL)
        FATALF("%s: out of memory", test->desc);
    Error err = BURROW_NO_ERROR;
    (void)burrow__fcgi_writer_write(w, test->content, &err);
    if (BURROW_FAILED(err)) {
        testing_t_errorf_v(t, "%s: error writing record: %v", test->desc, err);
        goto free_writer;
    }
    err = burrow__fcgi_writer_close(w);
    if (BURROW_FAILED(err)) {
        testing_t_errorf_v(t, "%s: error closing stream: %v", test->desc, err);
    } else if (!slice_eq(bytes_buffer_bytes(buf), test->raw)) {
        testing_t_errorf_v(t, "%s: wrote wrong content", test->desc);
    }
free_writer:
    burrow__fcgi_writer_free(a, w);
    mem_free(a, c, sizeof *c, _Alignof(burrow__FcgiConn));
out:
    bytes_buffer_free(content);
    bytes_buffer_free(buf);
}

static void TestStreams(TestingT *t) {
    Alloc *a = heap_allocator();
    burrow__FcgiRecord *rec =
        (burrow__FcgiRecord *)mem_alloc(a, sizeof *rec, _Alignof(burrow__FcgiRecord));
    BytesBuffer *two = bytes_new_buffer(a, (Slice){0});
    Byte *zeros = (Byte *)mem_alloc(a, 66000, 1);
    if (rec == NULL || two == NULL || zeros == NULL)
        FATALF("out of memory");
    memset(zeros, 0, 66000);

    /* this data will have to be split into two records */
    /* header for the first record */
    put_bytes(two, "\x01\x05\x01\x2C\xFF\xFF\x01\x00", 8);
    put_zeros(two, 65536);
    /* header for the second */
    put_bytes(two, "\x01\x05\x01\x2C\x01\xD1\x07\x00", 8);
    put_zeros(two, 472);
    /* header for the empty record */
    put_bytes(two, "\x01\x05\x01\x2C\x00\x00\x00\x00", 8);

    StreamTest tests[] = {
        {"single record",
         BURROW__FCGI_TYPE_STDOUT,
         1,
         {0},
         bytes_of(S("\x01\x06\x00\x01\x00\x00\x00\x00"))},
        {"two records",
         BURROW__FCGI_TYPE_STDIN,
         300,
         {zeros, 66000, 66000, NULL},
         bytes_buffer_bytes(two)},
    };
    for (size_t i = 0; i < sizeof tests / sizeof tests[0]; i++)
        stream_case(t, &tests[i], rec);

    mem_free(a, zeros, 66000, 1);
    bytes_buffer_free(two);
    mem_free(a, rec, sizeof *rec, _Alignof(burrow__FcgiRecord));
}

/* ---------------------------------------------------------- TestGetValues */

BURROW_SENTINEL_ERROR(write_only_err, "conn is write-only");

/* The Read of writeOnlyConn. Its Write is a BytesBuffer's. */
static Int write_only_read(void *self, Slice p, Error *err) {
    (void)self;
    (void)p;
    BURROW_OUT(err, write_only_err);
    return 0;
}

static const IoReaderVT write_only_reader_vt = {NULL, write_only_read};

static void TestGetValues(TestingT *t) {
    Alloc *a = heap_allocator();
    burrow__FcgiRecord *rec =
        (burrow__FcgiRecord *)mem_alloc(a, sizeof *rec, _Alignof(burrow__FcgiRecord));
    BytesBuffer *wc = bytes_new_buffer(a, (Slice){0});
    if (rec == NULL || wc == NULL)
        FATALF("out of memory");
    memset(rec, 0, sizeof *rec);
    rec->h.type = BURROW__FCGI_TYPE_GET_VALUES;

    burrow__FcgiChild *c = burrow__fcgi_new_child(
        (IoReader){&write_only_reader_vt, NULL}, bytes_buffer_as_io_writer(wc),
        nop_closer, (HttpHandler){0});
    if (c == NULL)
        FATALF("out of memory");
    Error err = burrow__fcgi_child_handle_record(c, rec);
    if (BURROW_FAILED(err))
        FATALF("handleRecord: %v", err);

    Str want = S("\x01\n\x00\x00\x00\x12\x06\x00"
                 "\x0f\x01"
                 "FCGI_MPXS_CONNS1"
                 "\x00\x00\x00\x00\x00\x00\x01\n\x00\x00\x00\x00\x00\x00");
    Slice got = bytes_buffer_bytes(wc);
    if (!slice_eq(got, bytes_of(want)))
        testing_t_errorf_v(t, " got: %q\nwant: %q\n", str_from_bytes(got.p, got.len),
                           want);
    burrow__fcgi_child_release(c);
    bytes_buffer_free(wc);
    mem_free(a, rec, sizeof *rec, _Alignof(burrow__FcgiRecord));
}

/* ----------------------------------------------------- the request streams */

/* nameValuePair11 and makeRecord, into b. */
static void make_record(BytesBuffer *b, uint8_t record_type, uint16_t request_id,
                        Slice content_data) {
    Byte h[8] = {1,
                 record_type,
                 (Byte)(request_id >> 8),
                 (Byte)request_id,
                 (Byte)(content_data.len >> 8),
                 (Byte)content_data.len,
                 0,
                 0};
    put_bytes(b, h, 8);
    if (content_data.len > 0)
        put_bytes(b, content_data.p, content_data.len);
}

static void make_pair_record(BytesBuffer *b, uint16_t request_id, Str name, Str value) {
    Byte content[256];
    content[0] = (Byte)name.len;
    content[1] = (Byte)value.len;
    memcpy(content + 2, name.p, (size_t)name.len);
    memcpy(content + 2 + name.len, value.p, (size_t)value.len);
    Int n = 2 + name.len + value.len;
    make_record(b, BURROW__FCGI_TYPE_PARAMS, request_id,
                (Slice){content, n, n, TYPE_BYTE});
}

static const Byte begin_responder[8] = {0, BURROW__FCGI_ROLE_RESPONDER, 0, 0, 0, 0, 0,
                                        0};

/* a series of FastCGI records that start a request and begin sending the
 * request body */
static void stream_begin_type_stdin(BytesBuffer *b) {
    /* set up request 1 */
    make_record(b, BURROW__FCGI_TYPE_BEGIN_REQUEST, 1,
                (Slice){(void *)(uintptr_t)begin_responder, 8, 8, TYPE_BYTE});
    /* add required parameters to request 1 */
    make_pair_record(b, 1, S("REQUEST_METHOD"), S("GET"));
    make_pair_record(b, 1, S("SERVER_PROTOCOL"), S("HTTP/1.1"));
    make_record(b, BURROW__FCGI_TYPE_PARAMS, 1, (Slice){0});
    /* begin sending body of request 1 */
    make_record(b, BURROW__FCGI_TYPE_STDIN, 1, bytes_of(S("0123456789abcdef")));
}

/* a series of FastCGI records that start and end a request */
static void stream_full_request_stdin(BytesBuffer *b) {
    /* set up request */
    make_record(b, BURROW__FCGI_TYPE_BEGIN_REQUEST, 1,
                (Slice){(void *)(uintptr_t)begin_responder, 8, 8, TYPE_BYTE});
    /* add required parameters */
    make_pair_record(b, 1, S("REQUEST_METHOD"), S("GET"));
    make_pair_record(b, 1, S("SERVER_PROTOCOL"), S("HTTP/1.1"));
    /* set optional parameters */
    make_pair_record(b, 1, S("REMOTE_USER"), S("jane.doe"));
    make_pair_record(b, 1, S("QUERY_STRING"), S("/foo/bar"));
    make_record(b, BURROW__FCGI_TYPE_PARAMS, 1, (Slice){0});
    /* begin sending body of request */
    make_record(b, BURROW__FCGI_TYPE_STDIN, 1, bytes_of(S("0123456789abcdef")));
    /* end request */
    make_record(b, BURROW__FCGI_TYPE_END_REQUEST, 1, (Slice){0});
}

/* newChild over a reader of input, with io_discard to write to and nothing to
 * close: nopWriteCloser. Then c.serve(). */
static burrow__FcgiChild *serve_input(TestingT *t, BytesBuffer *input, HttpHandler h) {
    burrow__FcgiChild *c = burrow__fcgi_new_child(bytes_buffer_as_io_reader(input),
                                                  io_discard, nop_closer, h);
    if (c == NULL) {
        testing_t_errorf_v(t, "out of memory");
        return NULL;
    }
    burrow__fcgi_child_serve(c);
    return c;
}

/* ------------------------------------------------- TestChildServeCleansUp */

typedef struct CleanUpTest {
    TestingT *t;
    Error err;
    Chan *done;
} CleanUpTest;

static void clean_up_handler(void *env, HttpResponseWriter w, HttpRequest *r) {
    CleanUpTest *tt = (CleanUpTest *)env;
    (void)w;
    /* block on reading body of request */
    Error err = BURROW_NO_ERROR;
    (void)io_copy(heap_allocator(), io_discard, io_read_closer_as_io_reader(r->body),
                  &err);
    if (!errors_is(err, tt->err))
        testing_t_errorf_v(tt->t, "Expected %v, got %v", tt->err, err);
    /* not reached if body of request isn't closed */
    chan_close(tt->done);
}

/* Test that child.serve closes the bodies of aborted requests and closes the
 * bodies of all requests before returning. Causes deadlock if either condition
 * isn't met. See issue 6934. */
static void TestChildServeCleansUp(TestingT *t) {
    Alloc *a = heap_allocator();
    for (int i = 0; i < 2; i++) {
        BytesBuffer *input = bytes_new_buffer(a, (Slice){0});
        stream_begin_type_stdin(input);
        CleanUpTest tt = {t, BURROW_NO_ERROR, chan_make(a, TYPE_BOOL, 0)};
        if (i == 0) {
            /* confirm that child.handleRecord closes req.pw after aborting
             * req */
            make_record(input, BURROW__FCGI_TYPE_ABORT_REQUEST, 1, (Slice){0});
            tt.err = fcgi_err_request_aborted;
        } else {
            /* confirm that child.serve closes all pipes after error reading
             * record */
            tt.err = fcgi_err_conn_closed;
        }
        HttpHandlerFunc f = BURROW_FN(HttpHandlerFunc, clean_up_handler, &tt);
        burrow__FcgiChild *c = serve_input(t, input, http_handler_func_as_handler(&f));
        /* wait for body of request to be closed or all goroutines to block */
        if (c != NULL)
            (void)chan_recv(tt.done, NULL);
        burrow__fcgi_child_release(c);
        chan_free(tt.done);
        bytes_buffer_free(input);
    }
}

/* ----------------------------------------------------- TestMalformedParams */

/* Verifies it doesn't crash. Issue 11824. */
static void TestMalformedParams(TestingT *t) {
    static const Byte input[] = {
        /* beginRequest, requestId=1, contentLength=8, role=1, keepConn=1 */
        1,
        1,
        0,
        1,
        0,
        8,
        0,
        0,
        0,
        1,
        1,
        0,
        0,
        0,
        0,
        0,
        /* params, requestId=1, contentLength=10, k1Len=50, v1Len=50
         * (malformed, wrong length) */
        1,
        4,
        0,
        1,
        0,
        10,
        0,
        0,
        50,
        50,
        3,
        4,
        5,
        6,
        7,
        8,
        9,
        10,
        /* end of params */
        1,
        4,
        0,
        1,
        0,
        0,
        0,
        0,
    };
    BytesBuffer *rw = bytes_new_buffer(
        heap_allocator(), (Slice){(void *)(uintptr_t)input, (Int)sizeof input,
                                  (Int)sizeof input, TYPE_BYTE});
    burrow__FcgiChild *c =
        serve_input(t, rw, http_serve_mux_as_handler(http_default_serve_mux));
    burrow__fcgi_child_release(c);
    bytes_buffer_free(rw);
}

/* ------------------------------------------------ TestChildServeReadsEnvVars */

typedef struct EnvVarTest {
    TestingT *t;
    Str env_var;
    Str expected_val;
    bool expected_filtered_out;
    Chan *done;
} EnvVarTest;

static void env_var_handler(void *env, HttpResponseWriter w, HttpRequest *r) {
    EnvVarTest *tt = (EnvVarTest *)env;
    (void)w;
    Map *process_env = fcgi_process_env(r);
    const Str *v =
        process_env != NULL ? (const Str *)map_get(process_env, &tt->env_var) : NULL;
    Str got = v != NULL ? *v : BURROW_STR_EMPTY;
    if (v != NULL && tt->expected_filtered_out)
        testing_t_errorf_v(
            tt->t, "Expected environment variable %s to not be set, but set to %s",
            tt->env_var, got);
    else if (!str_eq(got, tt->expected_val))
        testing_t_errorf_v(tt->t, "Expected %s, got %s", tt->expected_val, got);
    chan_close(tt->done);
}

/* Test that environment variables set for a request can be read by a handler.
 * Ensures that variables not set will not be exposed to a handler. */
static void TestChildServeReadsEnvVars(TestingT *t) {
    Alloc *a = heap_allocator();
    EnvVarTest tests[] = {
        {t, S("REMOTE_USER"), S("jane.doe"), false, NULL},
        {t, S("QUERY_STRING"), S(""), true, NULL},
    };
    for (size_t i = 0; i < sizeof tests / sizeof tests[0]; i++) {
        EnvVarTest *tt = &tests[i];
        BytesBuffer *input = bytes_new_buffer(a, (Slice){0});
        stream_full_request_stdin(input);
        tt->done = chan_make(a, TYPE_BOOL, 0);
        HttpHandlerFunc f = BURROW_FN(HttpHandlerFunc, env_var_handler, tt);
        burrow__FcgiChild *c = serve_input(t, input, http_handler_func_as_handler(&f));
        if (c != NULL)
            (void)chan_recv(tt->done, NULL);
        burrow__fcgi_child_release(c);
        chan_free(tt->done);
        bytes_buffer_free(input);
    }
}

/* ----------------------------------------- TestResponseWriterSniffsContentType */

typedef struct SniffTest {
    const char *name;
    Str body;
    Str want_ct;
    Byte got[64];
    Int got_len;
    Chan *done;
} SniffTest;

static void sniff_handler(void *env, HttpResponseWriter w, HttpRequest *r) {
    SniffTest *tt = (SniffTest *)env;
    (void)r;
    (void)io_write_string(http_response_writer_as_io_writer(w), tt->body, NULL);
    Str ct = http_header_get(http_response_writer_header(w), S("Content-Type"));
    tt->got_len = ct.len < (Int)sizeof tt->got ? ct.len : (Int)sizeof tt->got;
    memcpy(tt->got, ct.p, (size_t)tt->got_len);
    chan_close(tt->done);
}

static void sniff_case(void *env, TestingT *t) {
    SniffTest *tt = (SniffTest *)env;
    Alloc *a = heap_allocator();
    BytesBuffer *input = bytes_new_buffer(a, (Slice){0});
    stream_full_request_stdin(input);
    tt->done = chan_make(a, TYPE_BOOL, 0);
    HttpHandlerFunc f = BURROW_FN(HttpHandlerFunc, sniff_handler, tt);
    burrow__FcgiChild *c = serve_input(t, input, http_handler_func_as_handler(&f));
    if (c != NULL) {
        (void)chan_recv(tt->done, NULL);
        Str got = str_from_bytes(tt->got, tt->got_len);
        if (!str_eq(got, tt->want_ct))
            testing_t_errorf_v(t,
                               "got a Content-Type of %q; expected it to start with %q",
                               got, tt->want_ct);
    }
    burrow__fcgi_child_release(c);
    chan_free(tt->done);
    bytes_buffer_free(input);
}

static void TestResponseWriterSniffsContentType(TestingT *t) {
    ARENA_BEGIN;
    SniffTest tests[] = {
        {"no body", BURROW_STR_EMPTY, S("text/plain; charset=utf-8"), {0}, 0, NULL},
        {"html",
         S("<html><head><title>test page</title></head><body>This is a "
           "body</body></html>"),
         S("text/html; charset=utf-8"),
         {0},
         0,
         NULL},
        {"text",
         strings_repeat(a, S("gopher"), 86),
         S("text/plain; charset=utf-8"),
         {0},
         0,
         NULL},
        {"jpg",
         fmt_sprintf_v(a, "%s%s", S("\xFF\xD8\xFF"), strings_repeat(a, S("B"), 1024)),
         S("image/jpeg"),
         {0},
         0,
         NULL},
    };
    for (size_t i = 0; i < sizeof tests / sizeof tests[0]; i++)
        testing_t_run(t, str_from_cstr(tests[i].name),
                      BURROW_FN(TestingTFunc, sniff_case, &tests[i]));
    ARENA_END;
}

/* --------------------------------------------------------- TestSlowRequest */

/* signalingNopWriteCloser. It closes the reader before it says so, where Go
 * says so first, since the test gives the pipe back once it hears. */
typedef struct SignalingCloser {
    IoPipeReader *pr;
    Chan *closed;
} SignalingCloser;

static Error signaling_close(void *self) {
    SignalingCloser *rc = (SignalingCloser *)self;
    Error err = io_pipe_reader_close(rc->pr);
    chan_close(rc->closed);
    return err;
}

static const IoCloserVT signaling_closer_vt = {NULL, signaling_close};

typedef struct SlowWriter {
    IoPipeWriter *pw;
    Slice bufs[2];
    Chan *writer_done;
} SlowWriter;

static void slow_write(void *env) {
    SlowWriter *sw = (SlowWriter *)env;
    for (int i = 0; i < 2; i++) {
        (void)io_pipe_writer_write(sw->pw, sw->bufs[i], NULL);
        time_sleep(100 * TIME_MILLISECOND);
    }
    chan_close(sw->writer_done);
}

static void slow_handler(void *env, HttpResponseWriter w, HttpRequest *r) {
    (void)r;
    http_response_writer_write_header(w, 200);
    chan_close((Chan *)env);
}

/* Test whether server properly closes connection when processing slow
 * requests */
static void TestSlowRequest(TestingT *t) {
    Alloc *a = heap_allocator();
    IoPipeReader *pr;
    IoPipeWriter *pw;
    io_pipe(a, &pr, &pw);
    BytesBuffer *begin = bytes_new_buffer(a, (Slice){0});
    BytesBuffer *end = bytes_new_buffer(a, (Slice){0});
    stream_begin_type_stdin(begin);
    make_record(end, BURROW__FCGI_TYPE_STDIN, 1, (Slice){0});

    SlowWriter sw = {pw,
                     {bytes_buffer_bytes(begin), bytes_buffer_bytes(end)},
                     chan_make(a, TYPE_BOOL, 0)};
    if (!go(BURROW_FN(Func, slow_write, &sw)))
        FATALF("no goroutine for the writer");

    SignalingCloser rc = {pr, chan_make(a, TYPE_BOOL, 0)};
    Chan *handler_done = chan_make(a, TYPE_BOOL, 0);

    HttpHandlerFunc f = BURROW_FN(HttpHandlerFunc, slow_handler, handler_done);
    burrow__FcgiChild *c = burrow__fcgi_new_child(
        io_pipe_reader_as_io_reader(pr), io_discard,
        (IoCloser){&signaling_closer_vt, &rc}, http_handler_func_as_handler(&f));
    if (c != NULL) {
        burrow__fcgi_child_serve(c);
        (void)chan_recv(handler_done, NULL);
        (void)chan_recv(rc.closed, NULL);
        testing_t_log_v(t, "FastCGI child closed connection");
    } else {
        testing_t_errorf_v(t, "out of memory");
        (void)io_pipe_reader_close(pr);
    }
    burrow__fcgi_child_release(c);

    (void)chan_recv(sw.writer_done, NULL);
    (void)io_pipe_writer_close(pw);
    chan_free(handler_done);
    chan_free(rc.closed);
    chan_free(sw.writer_done);
    io_pipe_free(pr);
    bytes_buffer_free(end);
    bytes_buffer_free(begin);
}

#define TESTS(X)                                                                       \
    X(TestSize)                                                                        \
    X(TestStreams)                                                                     \
    X(TestGetValues)                                                                   \
    X(TestChildServeCleansUp)                                                          \
    X(TestMalformedParams)                                                             \
    X(TestChildServeReadsEnvVars)                                                      \
    X(TestResponseWriterSniffsContentType)                                             \
    X(TestSlowRequest)

TESTING_MAIN(TESTS)
