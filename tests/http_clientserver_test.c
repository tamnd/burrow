/* Derived from Go's src/net/http/clientserver_test.go.
 * Go source: go1.27.3.
 *
 * Tests that use both the client & server, in both HTTP/1 and HTTP/2 mode.
 *
 * Copyright 2015 The Go Authors. All rights reserved.
 * Copyright 2026 The burrow Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style licence that can be found
 * in the LICENSE file. */

#include "check.h"
#include "http_clientserver.h"

#include "../src/net/http_internal.h"

#include "burrow/burrow.h"
#include "burrow/bytes.h"
#include "burrow/chan.h"
#include "burrow/compress/gzip.h"
#include "burrow/crypto/tls.h"
#include "burrow/error.h"
#include "burrow/fmt.h"
#include "burrow/io.h"
#include "burrow/log.h"
#include "burrow/map.h"
#include "burrow/mem/arena.h"
#include "burrow/mem/heap.h"
#include "burrow/net.h"
#include "burrow/net/http.h"
#include "burrow/net/http/httptest.h"
#include "burrow/net/http/httputil.h"
#include "burrow/net/url.h"
#include "burrow/os.h"
#include "burrow/panic.h"
#include "burrow/sort.h"
#include "burrow/strings.h"
#include "burrow/sync.h"
#include "burrow/sync/atomic.h"
#include "burrow/testing.h"
#include "burrow/time.h"

#include <stdint.h>
#include <string.h>

/* Not in Go: s as the []byte Go would convert it to. */
static Slice bytes_of(Str s) {
    return slice_from((void *)(uintptr_t)s.p, s.len, s.len, TYPE_BYTE);
}

static void write_string(HttpResponseWriter w, Str s) {
    Error err = BURROW_NO_ERROR;
    (void)io_write_string(http_response_writer_as_io_writer(w), s, &err);
}

/* Testing the newClientServerTest helper itself. */
typedef struct NewCSTGot {
    SyncMutex mu;
    char proto[16];
    bool has_tls;
} NewCSTGot;

static void new_cst_handler(void *env, HttpResponseWriter w, HttpRequest *r) {
    (void)w;
    NewCSTGot *got = (NewCSTGot *)env;
    sync_mutex_lock(&got->mu);
    size_t n = (size_t)r->proto.len < sizeof got->proto - 1 ? (size_t)r->proto.len
                                                            : sizeof got->proto - 1;
    memcpy(got->proto, r->proto.p, n);
    got->proto[n] = 0;
    got->has_tls = r->tls != NULL;
    sync_mutex_unlock(&got->mu);
}

static void test_new_client_server_test(TestingT *t, TestMode mode,
                                        const CSTOpts *opts) {
    NewCSTGot *got = (NewCSTGot *)test_alloc(t, sizeof *got);
    ClientServerTest *cst = new_client_server_test_func(
        t, mode, BURROW_FN(HttpHandlerFunc, new_cst_handler, got), opts);
    Error err = BURROW_NO_ERROR;
    HttpResponse *res = http_client_head(cst->c, cst->ts->url, &err);
    if (res == NULL)
        testing_t_fatal_v(t, err);
    /* Not in Go: the response is given back. */
    cst_close_response(res);
    Str want_proto = BURROW_S("");
    bool want_tls = false;
    switch (mode) {
    case HTTP1_MODE:
        want_proto = BURROW_S("HTTP/1.1");
        want_tls = false;
        break;
    case HTTPS1_MODE:
        want_proto = BURROW_S("HTTP/1.1");
        want_tls = true;
        break;
    case HTTP2_MODE:
        want_proto = BURROW_S("HTTP/2.0");
        want_tls = true;
        break;
    case HTTP2_UNENCRYPTED_MODE:
    default:
        break;
    }
    Str got_proto = str_from_cstr(got->proto);
    if (!str_eq(got_proto, want_proto))
        testing_t_errorf_v(t, "req.Proto = %q, want %q", got_proto, want_proto);
    if (got->has_tls != want_tls)
        testing_t_errorf_v(t, "req.TLS set: %v, want %v", got->has_tls, want_tls);
}

static void new_client_server_test_realnet(TestingT *t, TestMode mode, void *env) {
    (void)env;
    test_new_client_server_test(t, mode, NULL);
}

static void new_client_server_test_synctest(TestingT *t, TestMode mode, void *env) {
    (void)env;
    static const CSTOpts fake = {.fake_net = true};
    test_new_client_server_test(t, mode, &fake);
}

static const TestMode new_cst_modes[3] = {HTTP1_MODE, HTTPS1_MODE, HTTP2_MODE};

static void new_cst_realnet(void *env, TestingT *t) {
    (void)env;
    RunOpts o = {new_cst_modes, 3, false};
    run(t, new_client_server_test_realnet, NULL, &o);
}

static void new_cst_synctest(void *env, TestingT *t) {
    (void)env;
    RunOpts o = {new_cst_modes, 3, false};
    run_synctest(t, new_client_server_test_synctest, NULL, &o);
}

static void TestNewClientServerTest(TestingT *t) {
    (void)testing_t_run(t, BURROW_S("realnet"),
                        BURROW_FN(TestingTFunc, new_cst_realnet, NULL));
    (void)testing_t_run(t, BURROW_S("synctest"),
                        BURROW_FN(TestingTFunc, new_cst_synctest, NULL));
}

static void chunked_response_headers_handler(void *env, HttpResponseWriter w,
                                             HttpRequest *r) {
    (void)env;
    (void)r;
    (void)http_header_set(
        w.vt->header(w.data), BURROW_S("Content-Length"),
        BURROW_S("intentional gibberish")); /* we check that this is deleted */
    (void)w.vt->flush(w.data);
    (void)fmt_fprintf_v(http_response_writer_as_io_writer(w),
                        "I am a chunked response.");
}

static void restore_log_output(void *env) {
    (void)env;
    log_set_output(os_file_as_io_writer(os_stderr));
}

static void test_chunked_response_headers(TestingT *t, TestMode mode, void *env) {
    (void)env;
    log_set_output(io_discard); /* is noisy otherwise */
    testing_t_cleanup(t, BURROW_FN(Func, restore_log_output, NULL));
    ClientServerTest *cst = new_client_server_test_func(
        t, mode, BURROW_FN(HttpHandlerFunc, chunked_response_headers_handler, NULL),
        NULL);

    Error err = BURROW_NO_ERROR;
    HttpResponse *res = http_client_get(cst->c, cst->ts->url, &err);
    if (res == NULL)
        testing_t_fatalf_v(t, "Get error: %v", err);
    if (res->content_length != -1)
        testing_t_errorf_v(t, "expected ContentLength of %d; got %d", (int64_t)-1,
                           res->content_length);
    Slice te = res->transfer_encoding;
    bool te_ok;
    if (mode == HTTP2_MODE)
        te_ok = te.len == 0;
    else
        te_ok = te.len == 1 && str_eq(((const Str *)te.p)[0], BURROW_S("chunked"));
    if (!te_ok)
        testing_t_errorf_v(t, "TransferEncoding = %v; want %v", te,
                           mode == HTTP2_MODE ? BURROW_S("[]") : BURROW_S("[chunked]"));
    Slice got = http_header_values(res->header, BURROW_S("Content-Length"));
    if (got.len > 0)
        testing_t_errorf_v(t, "Unexpected Content-Length: %q", got);
    cst_close_response(res);
}

static void TestChunkedResponseHeaders(TestingT *t) {
    RunOpts o = {http3_skipped_mode, 2, false};
    run(t, test_chunked_response_headers, NULL, &o);
}

/* ---------------------------------------------------------- h12Compare */

/* Not in Go: env is the test's, since there are no closures. */
typedef HttpResponse *(*ReqFunc)(void *env, HttpClient *c, Str url, Error *err);

static HttpResponse *req_get(void *env, HttpClient *c, Str url, Error *err) {
    (void)env;
    return http_client_get(c, url, err);
}

static HttpResponse *req_head(void *env, HttpClient *c, Str url, Error *err) {
    (void)env;
    return http_client_head(c, url, err);
}

/* h12Compare is a test that compares HTTP/1 and HTTP/2 behavior
 * against each other.
 *
 * Not in Go: the functions take env, which is the test's, and t, and the
 * body a response has after normalizeRes is the slurpResult beside it. */
typedef struct H12Res {
    HttpResponse *res;
    Slice body;
    Error err;
} H12Res;

typedef struct H12Compare {
    void (*handler)(void *env, HttpResponseWriter w, HttpRequest *r); /* required */
    ReqFunc req_func;                                                 /* optional */
    void (*check_response)(TestingT *t, void *env, Str proto,
                           H12Res *res); /* optional */
    void (*early_check_response)(TestingT *t, void *env, Str proto,
                                 H12Res *res); /* optional; pre-normalize */
    const CSTOpts *opts;
    void *env;
} H12Compare;

static ReqFunc h12_req_func(const H12Compare *tt) {
    if (tt->req_func == NULL)
        return req_get;
    return tt->req_func;
}

/* Not in Go: reflect.DeepEqual on two Headers. */
static bool header_equal(HttpHeader a, HttpHeader b) {
    if ((a == NULL) != (b == NULL))
        return false;
    if (a == NULL)
        return true;
    if (map_len(a) != map_len(b))
        return false;
    MapIter it = map_iter(b);
    const void *k;
    void *v;
    while (map_next(&it, &k, &v)) {
        Slice w = http_header_values(b, *(const Str *)k);
        Slice g = http_header_values(a, *(const Str *)k);
        if (g.len != w.len)
            return false;
        for (Int i = 0; i < w.len; i++) {
            if (!str_eq(((const Str *)g.p)[i], ((const Str *)w.p)[i]))
                return false;
        }
    }
    return true;
}

/* Not in Go: reflect.DeepEqual on what mostlyCopy keeps of two responses,
 * which is all but Body, TransferEncoding, TLS and Request. */
static bool mostly_equal(const HttpResponse *a, const HttpResponse *b) {
    return str_eq(a->status, b->status) && a->status_code == b->status_code &&
           str_eq(a->proto, b->proto) && a->proto_major == b->proto_major &&
           a->proto_minor == b->proto_minor && header_equal(a->header, b->header) &&
           a->content_length == b->content_length &&
           header_equal(a->trailer, b->trailer) && a->close == b->close &&
           a->uncompressed == b->uncompressed;
}

/* Not in Go: what mostlyCopy keeps of r, for a message, since there is no
 * %#v. */
static Str mostly_string(Alloc *a, const HttpResponse *r) {
    BytesBuffer b = BYTES_BUFFER(a);
    IoWriter w = bytes_buffer_as_io_writer(&b);
    (void)fmt_fprintf_v(w,
                        "Status:%q StatusCode:%d Proto:%q ContentLength:%d Close:%v "
                        "Uncompressed:%v Header:",
                        r->status, r->status_code, r->proto, r->content_length,
                        r->close, r->uncompressed);
    if (r->header != NULL)
        (void)http_header_write(r->header, w);
    Slice p = bytes_buffer_bytes(&b);
    return str_from_bytes(p.p, p.len);
}

/* Not in Go: reflect.DeepEqual on two slurpResults, with the errors equal
 * when they are the same error or say the same thing. */
static bool slurp_equal(const H12Res *a, const H12Res *b) {
    if (!bytes_equal(a->body, b->body))
        return false;
    if (BURROW_OK(a->err) || BURROW_OK(b->err))
        return BURROW_OK(a->err) && BURROW_OK(b->err);
    return errors_is(a->err, b->err) || str_eq(error_text(a->err), error_text(b->err));
}

static void h12_normalize_res(TestingT *t, Alloc *a, H12Res *r, Str want_proto) {
    HttpResponse *res = r->res;
    if (str_eq(res->proto, want_proto) || str_eq(res->proto, BURROW_S("HTTP/IGNORE"))) {
        res->proto = BURROW_S("");
        res->proto_major = 0;
        res->proto_minor = 0;
    } else {
        testing_t_errorf_v(t, "got %q response; want %q", res->proto, want_proto);
    }
    Error err = BURROW_NO_ERROR;
    Slice slurp = io_read_all(a, cst_body(res), &err);

    (void)res->body.vt->closer.close(res->body.data);
    r->body = slurp;
    r->err = err;
    Slice dates = http_header_values(res->header, BURROW_S("Date"));
    for (Int i = 0; i < dates.len; i++) {
        Str *v = &((Str *)dates.p)[i];
        *v = strings_repeat(a, BURROW_S("x"), v->len);
    }
    if (res->request == NULL)
        testing_t_errorf_v(t, "for %s, no request", want_proto);
    if ((res->tls != NULL) != str_eq(want_proto, BURROW_S("HTTP/2.0")))
        testing_t_errorf_v(t, "TLS set = %v; want %v", res->tls != NULL,
                           res->tls == NULL);
}

static void h12_run(TestingT *t, const H12Compare *tt) {
    set_parallel(t);
    Alloc *a = test_arena(t);
    HttpHandlerFunc hf = BURROW_FN(HttpHandlerFunc, tt->handler, tt->env);
    ClientServerTest *cst1 = new_client_server_test_func(t, HTTP1_MODE, hf, tt->opts);
    ClientServerTest *cst2 = new_client_server_test_func(t, HTTP2_MODE, hf, tt->opts);

    Error err = BURROW_NO_ERROR;
    H12Res res1 = {h12_req_func(tt)(tt->env, cst1->c, cst1->ts->url, &err), {0}, {0}};
    if (res1.res == NULL) {
        testing_t_errorf_v(t, "HTTP/1 request: %v", err);
        cst_close(cst2);
        cst_close(cst1);
        return;
    }
    H12Res res2 = {h12_req_func(tt)(tt->env, cst2->c, cst2->ts->url, &err), {0}, {0}};
    if (res2.res == NULL) {
        testing_t_errorf_v(t, "HTTP/2 request: %v", err);
        cst_close_response(res1.res);
        cst_close(cst2);
        cst_close(cst1);
        return;
    }

    if (tt->early_check_response != NULL) {
        tt->early_check_response(t, tt->env, BURROW_S("HTTP/1.1"), &res1);
        tt->early_check_response(t, tt->env, BURROW_S("HTTP/2.0"), &res2);
    }

    h12_normalize_res(t, a, &res1, BURROW_S("HTTP/1.1"));
    h12_normalize_res(t, a, &res2, BURROW_S("HTTP/2.0"));

    if (!mostly_equal(res1.res, res2.res))
        testing_t_errorf_v(t,
                           "Response headers to handler differed:\nhttp/1 "
                           "(%v):\n\t%v\nhttp/2 (%v):\n\t%v",
                           cst1->ts->url, mostly_string(a, res1.res), cst2->ts->url,
                           mostly_string(a, res2.res));
    if (!slurp_equal(&res1, &res2))
        testing_t_errorf_v(
            t,
            "Response bodies to handler differed.\nhttp1: body %q; err %v\n"
            "http2: body %q; err %v\n",
            str_from_bytes(res1.body.p, res1.body.len), res1.err,
            str_from_bytes(res2.body.p, res2.body.len), res2.err);
    if (tt->check_response != NULL) {
        tt->check_response(t, tt->env, BURROW_S("HTTP/1.1"), &res1);
        tt->check_response(t, tt->env, BURROW_S("HTTP/2.0"), &res2);
    }
    /* Not in Go: the responses are given back, their bodies closed above. */
    http_response_free(res1.res);
    http_response_free(res2.res);
    cst_close(cst2);
    cst_close(cst1);
}

static void handler_nothing(void *env, HttpResponseWriter w, HttpRequest *r) {
    (void)env;
    (void)w;
    (void)r;
}

/* Issue 13532 */
static void TestH12_HeadContentLengthNoBody(TestingT *t) {
    H12Compare tt = {.req_func = req_head, .handler = handler_nothing};
    h12_run(t, &tt);
}

static void handler_small(void *env, HttpResponseWriter w, HttpRequest *r) {
    (void)env;
    (void)r;
    write_string(w, BURROW_S("small"));
}

static void TestH12_HeadContentLengthSmallBody(TestingT *t) {
    H12Compare tt = {.req_func = req_head, .handler = handler_small};
    h12_run(t, &tt);
}

static void handler_large(void *env, HttpResponseWriter w, HttpRequest *r) {
    (void)env;
    Str chunk = strings_repeat(arena_allocator(&r->arena), BURROW_S("x"), 512 * 1024);
    for (int i = 0; i < 10; i++)
        write_string(w, chunk);
}

static void TestH12_HeadContentLengthLargeBody(TestingT *t) {
    H12Compare tt = {.req_func = req_head, .handler = handler_large};
    h12_run(t, &tt);
}

static void TestH12_200NoBody(TestingT *t) {
    H12Compare tt = {.handler = handler_nothing};
    h12_run(t, &tt);
}

static void handler_status(void *env, HttpResponseWriter w, HttpRequest *r) {
    (void)r;
    w.vt->write_header(w.data, *(const Int *)env);
}

static void test_h12_no_body(TestingT *t, Int status) {
    /* Not in Go: the status lasts as long as the server. */
    Int *st = (Int *)test_alloc(t, sizeof *st);
    *st = status;
    H12Compare tt = {.handler = handler_status, .env = st};
    h12_run(t, &tt);
}

static void TestH2_204NoBody(TestingT *t) {
    test_h12_no_body(t, 204);
}
static void TestH2_304NoBody(TestingT *t) {
    test_h12_no_body(t, 304);
}
static void TestH2_404NoBody(TestingT *t) {
    test_h12_no_body(t, 404);
}

static void handler_small_body(void *env, HttpResponseWriter w, HttpRequest *r) {
    (void)env;
    (void)r;
    write_string(w, BURROW_S("small body"));
}

static void TestH12_SmallBody(TestingT *t) {
    H12Compare tt = {.handler = handler_small_body};
    h12_run(t, &tt);
}

static void handler_explicit_cl(void *env, HttpResponseWriter w, HttpRequest *r) {
    (void)env;
    (void)r;
    (void)http_header_set(w.vt->header(w.data), BURROW_S("Content-Length"),
                          BURROW_S("3"));
    write_string(w, BURROW_S("foo"));
}

static void TestH12_ExplicitContentLength(TestingT *t) {
    H12Compare tt = {.handler = handler_explicit_cl};
    h12_run(t, &tt);
}

static void handler_flush_before_body(void *env, HttpResponseWriter w, HttpRequest *r) {
    (void)env;
    (void)r;
    (void)w.vt->flush(w.data);
    write_string(w, BURROW_S("foo"));
}

static void TestH12_FlushBeforeBody(TestingT *t) {
    H12Compare tt = {.handler = handler_flush_before_body};
    h12_run(t, &tt);
}

static void handler_flush_mid_body(void *env, HttpResponseWriter w, HttpRequest *r) {
    (void)env;
    (void)r;
    write_string(w, BURROW_S("foo"));
    (void)w.vt->flush(w.data);
    write_string(w, BURROW_S("bar"));
}

static void TestH12_FlushMidBody(TestingT *t) {
    H12Compare tt = {.handler = handler_flush_mid_body};
    h12_run(t, &tt);
}

static void handler_head_explicit_len(void *env, HttpResponseWriter w, HttpRequest *r) {
    TestingT *t = (TestingT *)env;
    if (!str_eq(r->method, BURROW_S("HEAD")))
        testing_t_errorf_v(t, "unexpected method %q", r->method);
    (void)http_header_set(w.vt->header(w.data), BURROW_S("Content-Length"),
                          BURROW_S("1235"));
}

static void TestH12_Head_ExplicitLen(TestingT *t) {
    H12Compare tt = {
        .req_func = req_head, .handler = handler_head_explicit_len, .env = t};
    h12_run(t, &tt);
}

static void handler_head_implicit_len(void *env, HttpResponseWriter w, HttpRequest *r) {
    TestingT *t = (TestingT *)env;
    if (!str_eq(r->method, BURROW_S("HEAD")))
        testing_t_errorf_v(t, "unexpected method %q", r->method);
    write_string(w, BURROW_S("foo"));
}

static void TestH12_Head_ImplicitLen(TestingT *t) {
    H12Compare tt = {
        .req_func = req_head, .handler = handler_head_implicit_len, .env = t};
    h12_run(t, &tt);
}

static void handler_writes_too_little(void *env, HttpResponseWriter w, HttpRequest *r) {
    (void)env;
    (void)r;
    (void)http_header_set(w.vt->header(w.data), BURROW_S("Content-Length"),
                          BURROW_S("3"));
    write_string(w, BURROW_S("12")); /* one byte short */
}

static void check_writes_too_little(TestingT *t, void *env, Str proto, H12Res *sr) {
    (void)env;
    if (!errors_is(sr->err, io_err_unexpected_eof))
        testing_t_errorf_v(t, "%s read error = %v; want io.ErrUnexpectedEOF", proto,
                           sr->err);
    Str body = str_from_bytes(sr->body.p, sr->body.len);
    if (!str_eq(body, BURROW_S("12")))
        testing_t_errorf_v(t, "%s body = %q; want %q", proto, body, BURROW_S("12"));
}

static void TestH12_HandlerWritesTooLittle(TestingT *t) {
    H12Compare tt = {.handler = handler_writes_too_little,
                     .check_response = check_writes_too_little};
    h12_run(t, &tt);
}

/* Tests that the HTTP/1 and HTTP/2 servers prevent handlers from
 * writing more than they declared. This test does not test whether
 * the transport deals with too much data, though, since the server
 * doesn't make it possible to send bogus data. For those tests, see
 * transport_test.go (for HTTP/1) or x/net/http2/transport_test.go
 * (for HTTP/2). */
static void handler_writes_too_much(void *env, HttpResponseWriter w, HttpRequest *r) {
    TestingT *t = (TestingT *)env;
    Str want_body = BURROW_S("123");
    HttpResponseController rc = http_new_response_controller(w);
    (void)http_header_set(
        w.vt->header(w.data), BURROW_S("Content-Length"),
        fmt_sprintf_v(arena_allocator(&r->arena), "%v", want_body.len));
    (void)http_response_controller_flush(&rc);
    IoWriter ww = http_response_writer_as_io_writer(w);
    Error err = BURROW_NO_ERROR;
    (void)ww.vt->write(ww.data, bytes_of(want_body), &err);
    (void)http_response_controller_flush(&rc);
    err = BURROW_NO_ERROR;
    Int n = io_write_string(ww, BURROW_S("x"), &err); /* too many */
    if (BURROW_OK(err))
        err = http_response_controller_flush(&rc);
    /* TODO: Check that this is ErrContentLength, not just any error. */
    if (BURROW_OK(err))
        testing_t_errorf_v(t, "for proto %q, final write = %v, %v; want _, some error",
                           r->proto, n, err);
}

static void test_handler_writes_too_much(TestingT *t, TestMode mode, void *env) {
    (void)env;
    Str want_body = BURROW_S("123");
    ClientServerTest *cst = new_client_server_test_func(
        t, mode, BURROW_FN(HttpHandlerFunc, handler_writes_too_much, t), NULL);

    Error err = BURROW_NO_ERROR;
    HttpResponse *res = http_client_get(cst->c, cst->ts->url, &err);
    if (res == NULL)
        testing_t_fatal_v(t, err);

    Alloc *a = test_arena(t);
    Slice got_body = io_read_all(a, cst_body(res), &err);
    cst_close_response(res);
    if (!bytes_equal(got_body, bytes_of(want_body)))
        testing_t_fatalf_v(t, "got response body: %q; want %q",
                           str_from_bytes(got_body.p, got_body.len), want_body);
}

static void TestHandlerWritesTooMuch(TestingT *t) {
    run(t, test_handler_writes_too_much, NULL, NULL);
}

/* Verify that both our HTTP/1 and HTTP/2 request and auto-decompress gzip.
 * Some hosts send gzip even if you don't ask for it; see golang.org/issue/13298 */
static void handler_auto_gzip(void *env, HttpResponseWriter w, HttpRequest *r) {
    TestingT *t = (TestingT *)env;
    Str ae = http_header_get(r->header, BURROW_S("Accept-Encoding"));
    if (!str_eq(ae, BURROW_S("gzip")))
        testing_t_errorf_v(t, "%s Accept-Encoding = %q; want gzip", r->proto, ae);
    (void)http_header_set(w.vt->header(w.data), BURROW_S("Content-Encoding"),
                          BURROW_S("gzip"));
    Error err = BURROW_NO_ERROR;
    GzipWriter *gz = gzip_new_writer(arena_allocator(&r->arena),
                                     http_response_writer_as_io_writer(w));
    if (gz == NULL) {
        testing_t_errorf_v(t, "out of memory");
        return;
    }
    (void)io_write_string(
        gzip_writer_as_io_writer(gz),
        BURROW_S("I am some gzipped content. Go go go go go go go go go "
                 "go go go should compress well."),
        &err);
    (void)gzip_writer_close(gz);
    gzip_writer_free(gz);
}

static void TestH12_AutoGzip(TestingT *t) {
    H12Compare tt = {.handler = handler_auto_gzip, .env = t};
    h12_run(t, &tt);
}

static void disable_compression(HttpTransport *tr, void *env) {
    (void)env;
    tr->disable_compression = true;
}

static void handler_auto_gzip_disabled(void *env, HttpResponseWriter w,
                                       HttpRequest *r) {
    TestingT *t = (TestingT *)env;
    (void)fmt_fprintf_v(http_response_writer_as_io_writer(w), "%q",
                        http_header_values(r->header, BURROW_S("Accept-Encoding")));
    Str ae = http_header_get(r->header, BURROW_S("Accept-Encoding"));
    if (ae.len != 0)
        testing_t_errorf_v(t, "%s Accept-Encoding = %q; want empty", r->proto, ae);
}

static void TestH12_AutoGzip_Disabled(TestingT *t) {
    static const CSTOpts opts = {.transport = disable_compression};
    H12Compare tt = {.opts = &opts, .handler = handler_auto_gzip_disabled, .env = t};
    h12_run(t, &tt);
}

/* Test304Responses verifies that 304s don't declare that they're
 * chunking in their response headers and aren't allowed to produce
 * output. */
static void handler_304(void *env, HttpResponseWriter w, HttpRequest *r) {
    (void)r;
    TestingT *t = (TestingT *)env;
    w.vt->write_header(w.data, HTTP_STATUS_NOT_MODIFIED);
    Error err = BURROW_NO_ERROR;
    IoWriter ww = http_response_writer_as_io_writer(w);
    (void)ww.vt->write(ww.data, bytes_of(BURROW_S("illegal body")), &err);
    if (!errors_is(err, http_err_body_not_allowed))
        testing_t_errorf_v(t, "on Write, expected ErrBodyNotAllowed, got %v", err);
}

static void test_304_responses(TestingT *t, TestMode mode, void *env) {
    (void)env;
    ClientServerTest *cst = new_client_server_test_func(
        t, mode, BURROW_FN(HttpHandlerFunc, handler_304, t), NULL);
    Error err = BURROW_NO_ERROR;
    HttpResponse *res = http_client_get(cst->c, cst->ts->url, &err);
    if (res == NULL) {
        cst_close(cst);
        testing_t_fatal_v(t, err);
    }
    if (res->transfer_encoding.len > 0)
        testing_t_errorf_v(t, "expected no TransferEncoding; got %v",
                           res->transfer_encoding);
    Alloc *a = test_arena(t);
    Slice body = io_read_all(a, cst_body(res), &err);
    if (BURROW_FAILED(err))
        testing_t_error_v(t, err);
    if (body.len > 0)
        testing_t_errorf_v(t, "got unexpected body %q",
                           str_from_bytes(body.p, body.len));
    cst_close_response(res);
    cst_close(cst);
}

static void Test304Responses(TestingT *t) {
    run(t, test_304_responses, NULL, NULL);
}

static void handler_empty_content_type(void *env, HttpResponseWriter w,
                                       HttpRequest *r) {
    (void)env;
    (void)r;
    (void)http_header_set(w.vt->header(w.data), BURROW_S("Content-Type"), BURROW_S(""));
    write_string(w, BURROW_S("<html><body>hi</body></html>"));
}

static void TestH12_ServerEmptyContentLength(TestingT *t) {
    H12Compare tt = {.handler = handler_empty_content_type};
    h12_run(t, &tt);
}

/* Not in Go: h12requestContentLength's bodyfn, and the length it wants. */
typedef struct H12ReqCL {
    IoReader (*bodyfn)(Alloc *a);
    int64_t want_len;
    Alloc *a;
} H12ReqCL;

static IoReader body_four(Alloc *a) {
    return strings_reader_as_io_reader(strings_new_reader(a, BURROW_S("FOUR")));
}

static IoReader body_nil(Alloc *a) {
    (void)a;
    return (IoReader){0};
}

static Int hidden_read(void *self, Slice p, Error *err) {
    IoReader *r = (IoReader *)self;
    return r->vt->read(r->data, p, err);
}

static const IoReaderVT hidden_reader_vt = {NULL, hidden_read};

/* struct{ io.Reader }{strings.NewReader("Stuff")}, a reader that hides what
 * it is. */
static IoReader body_hidden(Alloc *a) {
    IoReader *inner = (IoReader *)mem_alloc(a, sizeof *inner, _Alignof(IoReader));
    *inner = strings_reader_as_io_reader(strings_new_reader(a, BURROW_S("Stuff")));
    return (IoReader){&hidden_reader_vt, inner};
}

static void handler_request_content_length(void *env, HttpResponseWriter w,
                                           HttpRequest *r) {
    (void)env;
    (void)http_header_set(w.vt->header(w.data), BURROW_S("Got-Length"),
                          fmt_sprint_v(arena_allocator(&r->arena), r->content_length));
    (void)fmt_fprintf_v(http_response_writer_as_io_writer(w), "Req.ContentLength=%v",
                        r->content_length);
}

static HttpResponse *post_body(void *env, HttpClient *c, Str url, Error *err) {
    const H12ReqCL *cl = (const H12ReqCL *)env;
    return http_client_post(c, url, BURROW_S("text/plain"), cl->bodyfn(cl->a), err);
}

static void check_request_content_length(TestingT *t, void *env, Str proto,
                                         H12Res *res) {
    const H12ReqCL *cl = (const H12ReqCL *)env;
    Str got = http_header_get(res->res->header, BURROW_S("Got-Length"));
    Str want = fmt_sprint_v(cl->a, cl->want_len);
    if (!str_eq(got, want))
        testing_t_errorf_v(t, "Proto %q got length %q; want %q", proto, got, want);
}

static void h12_request_content_length(TestingT *t, IoReader (*bodyfn)(Alloc *a),
                                       int64_t want_len) {
    H12ReqCL *cl = (H12ReqCL *)test_alloc(t, sizeof *cl);
    cl->bodyfn = bodyfn;
    cl->want_len = want_len;
    cl->a = test_arena(t);
    H12Compare tt = {.handler = handler_request_content_length,
                     .req_func = post_body,
                     .check_response = check_request_content_length,
                     .env = cl};
    h12_run(t, &tt);
}

static void TestH12_RequestContentLength_Known_NonZero(TestingT *t) {
    h12_request_content_length(t, body_four, 4);
}

static void TestH12_RequestContentLength_Known_Zero(TestingT *t) {
    h12_request_content_length(t, body_nil, 0);
}

static void TestH12_RequestContentLength_Unknown(TestingT *t) {
    h12_request_content_length(t, body_hidden, -1);
}

/* Not in Go: wantBody is from transport_test.go. */
static Error want_body(Alloc *a, HttpResponse *res, Error err, Str want) {
    if (BURROW_FAILED(err))
        return err;
    Slice slurp = io_read_all(a, cst_body(res), &err);
    if (BURROW_FAILED(err))
        return fmt_errorf_v("error reading body: %v", err);
    Str got = str_from_bytes(slurp.p, slurp.len);
    if (!str_eq(got, want))
        return fmt_errorf_v("body = %q; want %q", got, want);
    err = res->body.vt->closer.close(res->body.data);
    if (BURROW_FAILED(err))
        return fmt_errorf_v("body Close = %v", err);
    return BURROW_NO_ERROR;
}

/* Not in Go: h[key] = vs, with the values copied into a, and n < 0 for a nil
 * slice. */
static void header_set_values(TestingT *t, HttpHeader h, Alloc *a, Str key,
                              const char *const *vs, Int n) {
    Slice s = slice_from(NULL, 0, 0, TYPE_STRING);
    if (n > 0) {
        Str *p = (Str *)mem_alloc(a, (size_t)n * sizeof *p, _Alignof(Str));
        if (p == NULL)
            testing_t_fatal_v(t, "out of memory");
        for (Int i = 0; i < n; i++)
            p[i] = str_from_cstr(vs[i]);
        s = slice_from(p, n, n, TYPE_STRING);
    }
    if (!map_set(h, &key, &s))
        testing_t_fatal_v(t, "out of memory");
}

/* Not in Go: h as Go prints it with %v, near enough for a message. */
static Str header_string(Alloc *a, HttpHeader h) {
    if (h == NULL)
        return BURROW_S("map[]");
    BytesBuffer b = BYTES_BUFFER(a);
    (void)http_header_write(h, bytes_buffer_as_io_writer(&b));
    Slice p = bytes_buffer_bytes(&b);
    return str_from_bytes(p.p, p.len);
}

static void cst_free_request(void *env) {
    http_request_free((HttpRequest *)env);
}

/* Tests that closing the Request.Cancel channel also while still
 * reading the response body. Issue 13159. */
typedef struct CancelMidBody {
    Chan *unblock;
    Chan *did_flush;
    Chan *cancel;
} CancelMidBody;

static void cancel_mid_body_handler(void *env, HttpResponseWriter w, HttpRequest *r) {
    (void)r;
    CancelMidBody *cm = (CancelMidBody *)env;
    write_string(w, BURROW_S("Hello"));
    (void)w.vt->flush(w.data);
    bool v = true;
    chan_send(cm->did_flush, &v);
    (void)chan_recv(cm->unblock, NULL);
    write_string(w, BURROW_S(", world."));
}

static void cancel_mid_body_free(void *env) {
    CancelMidBody *cm = (CancelMidBody *)env;
    chan_free(cm->unblock);
    chan_free(cm->did_flush);
    chan_free(cm->cancel);
}

static void cancel_mid_body_unblock(void *env) {
    chan_close(((CancelMidBody *)env)->unblock);
}

static void test_cancel_request_mid_body(TestingT *t, TestMode mode, void *env) {
    (void)env;
    CancelMidBody *cm = (CancelMidBody *)test_alloc(t, sizeof *cm);
    testing_t_cleanup(t, BURROW_FN(Func, cancel_mid_body_free, cm));
    cm->unblock = chan_make(heap_allocator(), TYPE_BOOL, 0);
    cm->did_flush = chan_make(heap_allocator(), TYPE_BOOL, 1);
    cm->cancel = chan_make(heap_allocator(), TYPE_BOOL, 0);
    if (cm->unblock == NULL || cm->did_flush == NULL || cm->cancel == NULL)
        testing_t_fatal_v(t, "out of memory");
    ClientServerTest *cst = new_client_server_test_func(
        t, mode, BURROW_FN(HttpHandlerFunc, cancel_mid_body_handler, cm), NULL);
    /* Not in Go: a cleanup registered after the server's runs before it, as
     * Go's defer does. */
    testing_t_cleanup(t, BURROW_FN(Func, cancel_mid_body_unblock, cm));

    Alloc *a = test_arena(t);
    Error err = BURROW_NO_ERROR;
    HttpRequest *req =
        http_new_request(a, BURROW_S("GET"), cst->ts->url, (IoReader){0}, &err);
    if (req == NULL)
        testing_t_fatal_v(t, err);
    testing_t_cleanup(t, BURROW_FN(Func, cst_free_request, req));
    req->cancel = cm->cancel;

    HttpResponse *res = http_client_do(cst->c, req, &err);
    if (res == NULL)
        testing_t_fatal_v(t, err);
    (void)chan_recv(cm->did_flush, NULL);

    /* Read a bit before we cancel. (Issue 13626)
     * We should have "Hello" at least sitting there. */
    uint8_t buf[10];
    Int n = res->body.vt->reader.read(res->body.data,
                                      slice_from(buf, 10, 10, TYPE_BYTE), &err);
    if (BURROW_FAILED(err)) {
        http_response_free(res);
        testing_t_fatal_v(t, err);
    }
    Str first_read = str_from_bytes(buf, n);

    chan_close(cm->cancel);

    Slice rest = io_read_all(a, cst_body(res), &err);
    Str rest_s = str_from_bytes(rest.p, rest.len);
    Str all = fmt_sprintf_v(a, "%s%s", first_read, rest_s);
    if (!str_eq(all, BURROW_S("Hello")))
        testing_t_errorf_v(t, "Read %q (%q + %q); want Hello", all, first_read, rest_s);
    if (!errors_is(err, burrow__http_err_request_canceled))
        testing_t_errorf_v(t, "ReadAll error = %v; want %v", err,
                           burrow__http_err_request_canceled);
    http_response_free(res);
}

static void TestCancelRequestMidBody(TestingT *t) {
    RunOpts o = {http3_skipped_mode, 2, false};
    run(t, test_cancel_request_mid_body, NULL, &o);
}

/* Tests that clients can send trailers to a server and that the server can read them. */
static void trailers_client_to_server_handler(void *env, HttpResponseWriter w,
                                              HttpRequest *r) {
    TestingT *t = (TestingT *)env;
    Error err = BURROW_NO_ERROR;
    Slice slurp = io_read_all(arena_allocator(&r->arena),
                              (IoReader){&r->body.vt->reader, r->body.data}, &err);
    if (BURROW_FAILED(err))
        testing_t_errorf_v(t, "Server reading request body: %v", err);
    Str s = str_from_bytes(slurp.p, slurp.len);
    if (!str_eq(s, BURROW_S("foo")))
        testing_t_errorf_v(t, "Server read request body %q; want foo", s);
    if (r->trailer == NULL) {
        write_string(w, BURROW_S("nil Trailer"));
    } else {
        Int n = map_len(r->trailer);
        Str *keys = (Str *)mem_alloc(arena_allocator(&r->arena),
                                     (size_t)(n + 1) * sizeof *keys, _Alignof(Str));
        if (keys == NULL) {
            testing_t_errorf_v(t, "out of memory");
            return;
        }
        Int i = 0;
        MapIter it = map_iter(r->trailer);
        const void *k;
        void *v;
        while (map_next(&it, &k, &v))
            keys[i++] = *(const Str *)k;
        Slice decl = slice_from(keys, n, n, TYPE_STRING);
        sort_strings(decl);
        (void)fmt_fprintf_v(http_response_writer_as_io_writer(w),
                            "decl: %v, vals: %s, %s", decl,
                            http_header_get(r->trailer, BURROW_S("Client-Trailer-A")),
                            http_header_get(r->trailer, BURROW_S("Client-Trailer-B")));
    }
}

/* eofReaderFunc, which sets a trailer the first time it is read. */
typedef struct EofSetTrailer {
    TestingT *t;
    HttpRequest **req;
    const char *key;
    const char *value;
} EofSetTrailer;

static Int eof_set_trailer_read(void *self, Slice p, Error *err) {
    (void)p;
    EofSetTrailer *e = (EofSetTrailer *)self;
    HttpRequest *req = *e->req;
    header_set_values(e->t, req->trailer, arena_allocator(&req->arena),
                      str_from_cstr(e->key), &e->value, 1);
    if (err != NULL)
        *err = io_eof;
    return 0;
}

static const IoReaderVT eof_set_trailer_vt = {NULL, eof_set_trailer_read};

static void test_trailers_client_to_server(TestingT *t, TestMode mode, void *env) {
    (void)env;
    ClientServerTest *cst = new_client_server_test_func(
        t, mode, BURROW_FN(HttpHandlerFunc, trailers_client_to_server_handler, t),
        NULL);

    Alloc *a = test_arena(t);
    HttpRequest **reqp = (HttpRequest **)test_alloc(t, sizeof *reqp);
    EofSetTrailer *ea = (EofSetTrailer *)test_alloc(t, sizeof *ea);
    EofSetTrailer *eb = (EofSetTrailer *)test_alloc(t, sizeof *eb);
    *ea = (EofSetTrailer){t, reqp, "Client-Trailer-A", "valuea"};
    *eb = (EofSetTrailer){t, reqp, "Client-Trailer-B", "valueb"};
    IoReader parts[3] = {
        {&eof_set_trailer_vt, ea},
        strings_reader_as_io_reader(strings_new_reader(a, BURROW_S("foo"))),
        {&eof_set_trailer_vt, eb},
    };
    IoReader body = io_multi_reader(a, parts, 3);
    Error err = BURROW_NO_ERROR;
    HttpRequest *req = http_new_request(a, BURROW_S("POST"), cst->ts->url, body, &err);
    if (req == NULL)
        testing_t_fatal_v(t, err);
    testing_t_cleanup(t, BURROW_FN(Func, cst_free_request, req));
    *reqp = req;
    req->trailer = http_header_make(arena_allocator(&req->arena));
    if (req->trailer == NULL)
        testing_t_fatal_v(t, "out of memory");
    header_set_values(t, req->trailer, arena_allocator(&req->arena),
                      BURROW_S("Client-Trailer-A"), NULL, -1); /*  to be set later */
    header_set_values(t, req->trailer, arena_allocator(&req->arena),
                      BURROW_S("Client-Trailer-B"), NULL, -1); /*  to be set later */
    req->content_length = -1;
    HttpResponse *res = http_client_do(cst->c, req, &err);
    if (res == NULL)
        testing_t_fatal_v(t, err);
    err = want_body(
        a, res, BURROW_NO_ERROR,
        BURROW_S("decl: [Client-Trailer-A Client-Trailer-B], vals: valuea, valueb"));
    if (BURROW_FAILED(err))
        testing_t_error_v(t, err);
    http_response_free(res);
}

static void TestTrailersClientToServer(TestingT *t) {
    run(t, test_trailers_client_to_server, NULL, NULL);
}

/* Tests that servers send trailers to a client and that the client can read them. */
static const char trailers_body[] = "Some body";

static void trailers_server_to_client_handler(void *env, HttpResponseWriter w,
                                              HttpRequest *r) {
    (void)r;
    bool flush = *(const bool *)env;
    HttpHeader h = w.vt->header(w.data);
    (void)http_header_set(h, BURROW_S("Trailer"),
                          BURROW_S("Server-Trailer-A, Server-Trailer-B"));
    (void)http_header_add(h, BURROW_S("Trailer"), BURROW_S("Server-Trailer-C"));

    write_string(w, str_from_cstr(trailers_body));
    if (flush)
        (void)w.vt->flush(w.data);

    /* How handlers set Trailers: declare it ahead of time
     * with the Trailer header, and then mutate the
     * Header() of those values later, after the response
     * has been written (we wrote to w above). */
    h = w.vt->header(w.data);
    (void)http_header_set(h, BURROW_S("Server-Trailer-A"), BURROW_S("valuea"));
    (void)http_header_set(h, BURROW_S("Server-Trailer-C"),
                          BURROW_S("valuec")); /* skipping B */
    (void)http_header_set(h, BURROW_S("Server-Trailer-NotDeclared"),
                          BURROW_S("should be omitted"));
}

static void test_trailers_server_to_client(TestingT *t, TestMode mode, bool flush) {
    Str body = str_from_cstr(trailers_body);
    bool *flushp = (bool *)test_alloc(t, sizeof *flushp);
    *flushp = flush;
    ClientServerTest *cst = new_client_server_test_func(
        t, mode, BURROW_FN(HttpHandlerFunc, trailers_server_to_client_handler, flushp),
        NULL);

    Error err = BURROW_NO_ERROR;
    HttpResponse *res = http_client_get(cst->c, cst->ts->url, &err);
    if (res == NULL)
        testing_t_fatal_v(t, err);

    Alloc *a = test_arena(t);
    HttpHeader want_header = http_header_make(a);
    if (want_header == NULL)
        testing_t_fatal_v(t, "out of memory");
    static const char *const content_type[] = {"text/plain; charset=utf-8"};
    header_set_values(t, want_header, a, BURROW_S("Content-Type"), content_type, 1);
    int64_t want_len = -1;
    if (mode == HTTP2_MODE && !flush) {
        /* In HTTP/1.1, any use of trailers forces HTTP/1.1
         * chunking and a flush at the first write. That's
         * unnecessary with HTTP/2's framing, so the server
         * is able to calculate the length while still sending
         * trailers afterwards. */
        want_len = body.len;
        (void)http_header_set(want_header, BURROW_S("Content-Length"),
                              fmt_sprint_v(a, want_len));
    }
    if (res->content_length != want_len)
        testing_t_errorf_v(t, "ContentLength = %v; want %v", res->content_length,
                           want_len);

    http_header_del(res->header, BURROW_S("Date")); /* irrelevant for test */
    if (!header_equal(res->header, want_header))
        testing_t_errorf_v(t, "Header = %v; want %v", header_string(a, res->header),
                           header_string(a, want_header));

    HttpHeader want = http_header_make(a);
    if (want == NULL)
        testing_t_fatal_v(t, "out of memory");
    header_set_values(t, want, a, BURROW_S("Server-Trailer-A"), NULL, -1);
    header_set_values(t, want, a, BURROW_S("Server-Trailer-B"), NULL, -1);
    header_set_values(t, want, a, BURROW_S("Server-Trailer-C"), NULL, -1);
    if (!header_equal(res->trailer, want))
        testing_t_errorf_v(t, "Trailer before body read = %v; want %v",
                           header_string(a, res->trailer), header_string(a, want));

    err = want_body(a, res, BURROW_NO_ERROR, body);
    if (BURROW_FAILED(err)) {
        http_response_free(res);
        testing_t_fatal_v(t, err);
    }

    static const char *const valuea[] = {"valuea"};
    static const char *const valuec[] = {"valuec"};
    want = http_header_make(a);
    if (want == NULL)
        testing_t_fatal_v(t, "out of memory");
    header_set_values(t, want, a, BURROW_S("Server-Trailer-A"), valuea, 1);
    header_set_values(t, want, a, BURROW_S("Server-Trailer-B"), NULL, -1);
    header_set_values(t, want, a, BURROW_S("Server-Trailer-C"), valuec, 1);
    if (!header_equal(res->trailer, want))
        testing_t_errorf_v(t, "Trailer after body read = %v; want %v",
                           header_string(a, res->trailer), header_string(a, want));
    http_response_free(res);
}

static void trailers_server_to_client(TestingT *t, TestMode mode, void *env) {
    (void)env;
    test_trailers_server_to_client(t, mode, false);
}

static void trailers_server_to_client_flush(TestingT *t, TestMode mode, void *env) {
    (void)env;
    test_trailers_server_to_client(t, mode, true);
}

static void TestTrailersServerToClient(TestingT *t) {
    RunOpts o = {http3_skipped_mode, 2, false};
    run(t, trailers_server_to_client, NULL, &o);
}

static void TestTrailersServerToClientFlush(TestingT *t) {
    run(t, trailers_server_to_client_flush, NULL, NULL);
}

/* Don't allow a Body.Read after Body.Close. Issue 13648. */
static void some_body_handler(void *env, HttpResponseWriter w, HttpRequest *r) {
    (void)env;
    (void)r;
    write_string(w, str_from_cstr(trailers_body));
}

static void test_response_body_read_after_close(TestingT *t, TestMode mode, void *env) {
    (void)env;
    ClientServerTest *cst = new_client_server_test_func(
        t, mode, BURROW_FN(HttpHandlerFunc, some_body_handler, NULL), NULL);
    Error err = BURROW_NO_ERROR;
    HttpResponse *res = http_client_get(cst->c, cst->ts->url, &err);
    if (res == NULL)
        testing_t_fatal_v(t, err);
    (void)res->body.vt->closer.close(res->body.data);
    Alloc *a = test_arena(t);
    Slice data = io_read_all(a, cst_body(res), &err);
    http_response_free(res);
    if (data.len != 0 || BURROW_OK(err))
        testing_t_fatalf_v(t, "ReadAll returned %q, %v; want error",
                           str_from_bytes(data.p, data.len), err);
}

static void TestResponseBodyReadAfterClose(TestingT *t) {
    run(t, test_response_body_read_after_close, NULL, NULL);
}

static const char concurrent_req_body[] = "some request body";
static const char concurrent_res_body[] = "some response body";

/* Not in Go: what the two goroutines in the handler share. */
typedef struct ConcurrentRW {
    TestingT *t;
    TestMode mode;
    HttpResponseWriter w;
    HttpRequest *r;
    Chan *did_read;
} ConcurrentRW;

/* Read in one goroutine. */
static void concurrent_rw_read(void *env) {
    ConcurrentRW *c = (ConcurrentRW *)env;
    Error err = BURROW_NO_ERROR;
    Slice data = io_read_all(heap_allocator(),
                             (IoReader){&c->r->body.vt->reader, c->r->body.data}, &err);
    Str got = str_from_bytes(data.p, data.len);
    if (!str_eq(got, str_from_cstr(concurrent_req_body)))
        testing_t_errorf_v(c->t, "Handler read %q; want %q", got,
                           str_from_cstr(concurrent_req_body));
    if (BURROW_FAILED(err))
        testing_t_errorf_v(c->t, "Handler Read: %v", err);
    mem_free(heap_allocator(), data.p, (size_t)data.cap, 1);
    bool v = true;
    chan_send(c->did_read, &v);
}

/* Write in another goroutine. */
static void concurrent_rw_write(void *env) {
    ConcurrentRW *c = (ConcurrentRW *)env;
    if (c->mode != HTTP2_MODE) {
        /* our HTTP/1 implementation intentionally
         * doesn't permit writes during read (mostly
         * due to it being undefined); if that is ever
         * relaxed, change this. */
        (void)chan_recv(c->did_read, NULL);
    }
    write_string(c->w, str_from_cstr(concurrent_res_body));
}

static void concurrent_rw_handler(void *env, HttpResponseWriter w, HttpRequest *r) {
    ConcurrentRW c = *(const ConcurrentRW *)env;
    c.w = w;
    c.r = r;
    c.did_read = chan_make(heap_allocator(), TYPE_BOOL, 1);
    if (c.did_read == NULL) {
        testing_t_errorf_v(c.t, "out of memory");
        return;
    }
    SyncWaitGroup wg = {0};
    if (!sync_wait_group_go(&wg, BURROW_FN(Func, concurrent_rw_read, &c)))
        testing_t_errorf_v(c.t, "cannot start a goroutine");
    if (!sync_wait_group_go(&wg, BURROW_FN(Func, concurrent_rw_write, &c)))
        testing_t_errorf_v(c.t, "cannot start a goroutine");
    sync_wait_group_wait(&wg);
    chan_free(c.did_read);
}

static void test_concurrent_read_write_req_body(TestingT *t, TestMode mode, void *env) {
    (void)env;
    ConcurrentRW *c = (ConcurrentRW *)test_alloc(t, sizeof *c);
    c->t = t;
    c->mode = mode;
    ClientServerTest *cst = new_client_server_test_func(
        t, mode, BURROW_FN(HttpHandlerFunc, concurrent_rw_handler, c), NULL);
    Alloc *a = test_arena(t);
    Error err = BURROW_NO_ERROR;
    HttpRequest *req = http_new_request(a, BURROW_S("POST"), cst->ts->url,
                                        strings_reader_as_io_reader(strings_new_reader(
                                            a, str_from_cstr(concurrent_req_body))),
                                        &err);
    if (req == NULL)
        testing_t_fatal_v(t, err);
    testing_t_cleanup(t, BURROW_FN(Func, cst_free_request, req));
    (void)http_header_add(req->header, BURROW_S("Expect"),
                          BURROW_S("100-continue")); /* just to complicate things */
    HttpResponse *res = http_client_do(cst->c, req, &err);
    if (res == NULL)
        testing_t_fatal_v(t, err);
    Slice data = io_read_all(a, cst_body(res), &err);
    http_response_free(res);
    if (BURROW_FAILED(err))
        testing_t_fatal_v(t, err);
    Str got = str_from_bytes(data.p, data.len);
    if (!str_eq(got, str_from_cstr(concurrent_res_body)))
        testing_t_errorf_v(t, "read %q; want %q", got,
                           str_from_cstr(concurrent_res_body));
}

static void TestConcurrentReadWriteReqBody(TestingT *t) {
    run(t, test_concurrent_read_write_req_body, NULL, NULL);
}

/* Not in Go: what the handler saw of a request, copied before the request
 * goes away, since the server frees it once the handler returns. */
typedef struct GotReq {
    Alloc *a;
    Chan *gotc;
    Str method;
    Str host;
    Str url_host;
    Str url_path;
    Str request_uri;
} GotReq;

static void got_req_free(void *env) {
    chan_free(((GotReq *)env)->gotc);
}

static GotReq *new_got_req(TestingT *t) {
    GotReq *g = (GotReq *)test_alloc(t, sizeof *g);
    g->a = test_arena(t);
    g->gotc = chan_make(heap_allocator(), TYPE_BOOL, 1);
    if (g->gotc == NULL)
        testing_t_fatal_v(t, "out of memory");
    testing_t_cleanup(t, BURROW_FN(Func, got_req_free, g));
    return g;
}

/* gotc <- r */
static void got_req_send(GotReq *g, HttpRequest *r) {
    g->method = strings_clone(g->a, r->method);
    g->host = strings_clone(g->a, r->host);
    g->url_host = strings_clone(g->a, r->url->host);
    g->url_path = strings_clone(g->a, r->url->path);
    g->request_uri = strings_clone(g->a, r->request_uri);
    bool v = true;
    chan_send(g->gotc, &v);
}

static void connect_request_handler(void *env, HttpResponseWriter w, HttpRequest *r) {
    (void)w;
    got_req_send((GotReq *)env, r);
}

static void test_connect_request(TestingT *t, TestMode mode, void *env) {
    (void)env;
    GotReq *got = new_got_req(t);
    ClientServerTest *cst = new_client_server_test_func(
        t, mode, BURROW_FN(HttpHandlerFunc, connect_request_handler, got), NULL);

    Alloc *a = test_arena(t);
    Error err = BURROW_NO_ERROR;
    Url *u = url_parse(a, cst->ts->url, &err);
    if (u == NULL)
        testing_t_fatal_v(t, err);

    struct {
        Str host;
        Str want;
    } tests[] = {
        {BURROW_S(""), u->host},
        {BURROW_S("example.com:123"), BURROW_S("example.com:123")},
    };

    for (size_t i = 0; i < sizeof tests / sizeof tests[0]; i++) {
        HttpRequest req = {0};
        req.method = BURROW_S("CONNECT");
        req.header = http_header_make(a);
        req.url = u;
        req.host = tests[i].host;
        HttpResponse *res = http_client_do(cst->c, &req, &err);
        if (res == NULL) {
            testing_t_errorf_v(t, "%d. RoundTrip = %v", (Int)i, err);
            arena_free(&req.arena);
            continue;
        }
        http_response_free(res);
        arena_free(&req.arena);
        (void)chan_recv(got->gotc, NULL);
        if (!str_eq(got->method, BURROW_S("CONNECT")))
            testing_t_errorf_v(t, "method = %q; want CONNECT", got->method);
        if (!str_eq(got->host, tests[i].want))
            testing_t_errorf_v(t, "Host = %q; want %q", got->host, tests[i].want);
        if (!str_eq(got->url_host, tests[i].want))
            testing_t_errorf_v(t, "URL.Host = %q; want %q", got->url_host,
                               tests[i].want);
    }
}

static void TestConnectRequest(TestingT *t) {
    run(t, test_connect_request, NULL, NULL);
}

static void user_agent_handler(void *env, HttpResponseWriter w, HttpRequest *r) {
    (void)env;
    (void)fmt_fprintf_v(http_response_writer_as_io_writer(w), "%q",
                        http_header_values(r->header, BURROW_S("User-Agent")));
}

static void test_transport_user_agent(TestingT *t, TestMode mode, void *env) {
    (void)env;
    ClientServerTest *cst = new_client_server_test_func(
        t, mode, BURROW_FN(HttpHandlerFunc, user_agent_handler, NULL), NULL);

    static const char *const multiple[] = {"single", "or", "multiple"};
    Str want0 = mode == HTTP2_MODE ? BURROW_S("[\"Go-http-client/2.0\"]")
                                   : BURROW_S("[\"Go-http-client/1.1\"]");
    Str wants[] = {
        want0,
        BURROW_S("[\"foo/1.2.3\"]"),
        BURROW_S("[\"single\"]"),
        BURROW_S("[]"),
        BURROW_S("[]"),
    };
    Alloc *a = test_arena(t);
    for (int i = 0; i < 5; i++) {
        Error err = BURROW_NO_ERROR;
        HttpRequest *req =
            http_new_request(a, BURROW_S("GET"), cst->ts->url, (IoReader){0}, &err);
        if (req == NULL)
            testing_t_fatal_v(t, err);
        switch (i) {
        case 1:
            (void)http_header_set(req->header, BURROW_S("User-Agent"),
                                  BURROW_S("foo/1.2.3"));
            break;
        case 2:
            header_set_values(t, req->header, arena_allocator(&req->arena),
                              BURROW_S("User-Agent"), multiple, 3);
            break;
        case 3:
            (void)http_header_set(req->header, BURROW_S("User-Agent"), BURROW_S(""));
            break;
        case 4:
            header_set_values(t, req->header, arena_allocator(&req->arena),
                              BURROW_S("User-Agent"), NULL, -1);
            break;
        default:
            break;
        }
        HttpResponse *res = http_client_do(cst->c, req, &err);
        if (res == NULL) {
            testing_t_errorf_v(t, "%d. RoundTrip = %v", i, err);
            http_request_free(req);
            continue;
        }
        Slice slurp = io_read_all(a, cst_body(res), &err);
        http_response_free(res);
        http_request_free(req);
        if (BURROW_FAILED(err)) {
            testing_t_errorf_v(t, "%d. read body = %v", i, err);
            continue;
        }
        Str got = str_from_bytes(slurp.p, slurp.len);
        if (!str_eq(got, wants[i]))
            testing_t_errorf_v(t, "%d. body mismatch.\n got: %s\nwant: %s\n", i, got,
                               wants[i]);
    }
}

static void TestTransportUserAgent(TestingT *t) {
    RunOpts o = {http3_skipped_mode, 2, false};
    run(t, test_transport_user_agent, NULL, &o);
}

static void star_request_handler(void *env, HttpResponseWriter w, HttpRequest *r) {
    (void)http_header_set(w.vt->header(w.data), BURROW_S("foo"), BURROW_S("bar"));
    got_req_send((GotReq *)env, r);
    (void)w.vt->flush(w.data);
}

static void test_star_request(TestingT *t, TestMode mode, void *env) {
    Str method = *(const Str *)env;
    GotReq *got = new_got_req(t);
    ClientServerTest *cst = new_client_server_test_func(
        t, mode, BURROW_FN(HttpHandlerFunc, star_request_handler, got), NULL);

    Alloc *a = test_arena(t);
    Error err = BURROW_NO_ERROR;
    Url *u = url_parse(a, cst->ts->url, &err);
    if (u == NULL)
        testing_t_fatal_v(t, err);
    u->path = BURROW_S("*");

    HttpRequest req = {0};
    req.method = method;
    req.header = http_header_make(a);
    req.url = u;

    HttpResponse *res = http_client_do(cst->c, &req, &err);
    if (res == NULL) {
        arena_free(&req.arena);
        testing_t_fatalf_v(t, "RoundTrip = %v", err);
    }
    (void)res->body.vt->closer.close(res->body.data);

    Str want_foo = BURROW_S("bar");
    int64_t want_len = -1;
    if (str_eq(method, BURROW_S("OPTIONS"))) {
        want_foo = BURROW_S("");
        want_len = 0;
    }
    if (res->status_code != 200)
        testing_t_errorf_v(t, "status code = %v; want %d", res->status, 200);
    if (res->content_length != want_len)
        testing_t_errorf_v(t, "content length = %v; want %d", res->content_length,
                           want_len);
    Str foo = http_header_get(res->header, BURROW_S("foo"));
    if (!str_eq(foo, want_foo))
        testing_t_errorf_v(t, "response \"foo\" header = %q; want %q", foo, want_foo);
    http_response_free(res);
    arena_free(&req.arena);
    bool v;
    bool ok;
    if (!chan_try_recv(got->gotc, &v, &ok)) {
        if (!str_eq(method, BURROW_S("OPTIONS")))
            testing_t_fatalf_v(t, "handler never got request");
        return;
    }
    if (!str_eq(got->method, method))
        testing_t_errorf_v(t, "method = %q; want %q", got->method, method);
    if (!str_eq(got->url_path, BURROW_S("*")))
        testing_t_errorf_v(t, "URL.Path = %q; want *", got->url_path);
    if (!str_eq(got->request_uri, BURROW_S("*")))
        testing_t_errorf_v(t, "RequestURI = %q; want *", got->request_uri);
}

static void star_request_method(void *env, TestingT *t) {
    run(t, test_star_request, env, NULL);
}

static void TestStarRequestMethod(TestingT *t) {
    static Str methods[] = {BURROW_S("FOO"), BURROW_S("OPTIONS")};
    for (size_t i = 0; i < 2; i++)
        (void)testing_t_run(t, methods[i],
                            BURROW_FN(TestingTFunc, star_request_method, &methods[i]));
}

static void hello_remote_addr_handler(void *env, HttpResponseWriter w, HttpRequest *r) {
    (void)env;
    (void)fmt_fprintf_v(http_response_writer_as_io_writer(w), "Hello, %v",
                        r->remote_addr);
}

/* Not in Go: the connection under it, the TLS one over it and the next one
 * dialed, so that the transport's free_conn can give back all three. */
typedef struct NoteCloseConn {
    NetConn conn;
    void (*close_func)(void *env);
    void *env;
    TlsConn *tls;
    struct NoteCloseConn *next;
} NoteCloseConn;

static Int note_close_read(void *self, Slice p, Error *err) {
    NetConn c = ((NoteCloseConn *)self)->conn;
    return c.vt->reader.read(c.data, p, err);
}

static Int note_close_write(void *self, Slice p, Error *err) {
    NetConn c = ((NoteCloseConn *)self)->conn;
    return c.vt->writer.write(c.data, p, err);
}

static Error note_close_close(void *self) {
    NoteCloseConn *x = (NoteCloseConn *)self;
    x->close_func(x->env);
    return x->conn.vt->closer.close(x->conn.data);
}

static NetAddr note_close_local_addr(void *self) {
    NetConn c = ((NoteCloseConn *)self)->conn;
    return c.vt->local_addr(c.data);
}

static NetAddr note_close_remote_addr(void *self) {
    NetConn c = ((NoteCloseConn *)self)->conn;
    return c.vt->remote_addr(c.data);
}

static Error note_close_set_deadline(void *self, Time d) {
    NetConn c = ((NoteCloseConn *)self)->conn;
    return c.vt->set_deadline(c.data, d);
}

static Error note_close_set_read_deadline(void *self, Time d) {
    NetConn c = ((NoteCloseConn *)self)->conn;
    return c.vt->set_read_deadline(c.data, d);
}

static Error note_close_set_write_deadline(void *self, Time d) {
    NetConn c = ((NoteCloseConn *)self)->conn;
    return c.vt->set_write_deadline(c.data, d);
}

static const NetConnVT note_close_conn_vt = {
    .reader = {NULL, note_close_read},
    .writer = {NULL, note_close_write},
    .closer = {NULL, note_close_close},
    .local_addr = note_close_local_addr,
    .remote_addr = note_close_remote_addr,
    .set_deadline = note_close_set_deadline,
    .set_read_deadline = note_close_set_read_deadline,
    .set_write_deadline = note_close_set_write_deadline,
};

enum { DISCARD_N = 10 };

/* Not in Go: what the test's closures share. */
typedef struct DiscardConns {
    TestingT *t;
    SyncAtomicInt32 num_open, num_close; /* atomic */
    TlsConfig tls_config;
    Str next_protos[1];
    HttpProtocols protocols;
    HttpTransport tr;
    HttpClient c;
    Str url;
    Chan *got_body;
    Slice bodies[DISCARD_N];
    SyncMutex mu; /* guards conns */
    NoteCloseConn *conns;
} DiscardConns;

static void discard_note_close(void *env) {
    (void)sync_atomic_int32_add(&((DiscardConns *)env)->num_close, 1);
}

static NetConn discard_dial_tls(void *env, Str network, Str addr, Error *err) {
    (void)network;
    DiscardConns *d = (DiscardConns *)env;
    NetConn none = {NULL, NULL};
    time_sleep(10 * TIME_MILLISECOND);
    NetConn rc = net_dial(heap_allocator(), BURROW_S("tcp"), addr, err);
    if (BURROW_FAILED(*err))
        return none;
    (void)sync_atomic_int32_add(&d->num_open, 1);
    NoteCloseConn *c = (NoteCloseConn *)mem_alloc(heap_allocator(), sizeof *c,
                                                  _Alignof(NoteCloseConn));
    if (c == NULL) {
        net_conn_free(rc);
        *err = burrow_err_out_of_memory;
        return none;
    }
    *c = (NoteCloseConn){rc, discard_note_close, d, NULL, NULL};
    c->tls =
        tls_client(heap_allocator(), (NetConn){&note_close_conn_vt, c}, &d->tls_config);
    if (c->tls == NULL) {
        net_conn_free(rc);
        mem_free(heap_allocator(), c, sizeof *c, _Alignof(NoteCloseConn));
        *err = burrow_err_out_of_memory;
        return none;
    }
    sync_mutex_lock(&d->mu);
    c->next = d->conns;
    d->conns = c;
    sync_mutex_unlock(&d->mu);
    return tls_conn_as_net_conn(c->tls);
}

/* Not in Go: the transport gives back the connections the hook made with
 * this. */
static void discard_free_conn(void *env, NetConn conn) {
    DiscardConns *d = (DiscardConns *)env;
    TlsConn *tc = tls_conn_from_net_conn(conn);
    NoteCloseConn *c = NULL;
    sync_mutex_lock(&d->mu);
    for (NoteCloseConn **p = &d->conns; *p != NULL; p = &(*p)->next) {
        if ((*p)->tls == tc) {
            c = *p;
            *p = c->next;
            break;
        }
    }
    sync_mutex_unlock(&d->mu);
    tls_conn_free(tc);
    if (c != NULL) {
        net_conn_free(c->conn);
        mem_free(heap_allocator(), c, sizeof *c, _Alignof(NoteCloseConn));
    }
}

/* Not in Go: tr.CloseIdleConnections, which Go defers, and then what the
 * garbage collector would do. */
static void discard_conns_free(void *env) {
    DiscardConns *d = (DiscardConns *)env;
    http_transport_close_idle_connections(&d->tr);
    http_transport_free(&d->tr);
    chan_free(d->got_body);
    for (Int i = 0; i < DISCARD_N; i++)
        mem_free(heap_allocator(), d->bodies[i].p, (size_t)d->bodies[i].cap, 1);
}

typedef struct DiscardGet {
    DiscardConns *d;
    Int i;
} DiscardGet;

static void discard_get(void *env) {
    DiscardGet *g = (DiscardGet *)env;
    DiscardConns *d = g->d;
    Error err = BURROW_NO_ERROR;
    HttpResponse *resp = http_client_get(&d->c, d->url, &err);
    if (resp == NULL) {
        /* Try to work around spurious connection reset on loaded system.
         * See golang.org/issue/33585 and golang.org/issue/36797. */
        time_sleep(10 * TIME_MILLISECOND);
        err = BURROW_NO_ERROR;
        resp = http_client_get(&d->c, d->url, &err);
        if (resp == NULL) {
            testing_t_errorf_v(d->t, "Get: %v", err);
            return;
        }
    }
    Slice slurp = io_read_all(heap_allocator(), cst_body(resp), &err);
    cst_close_response(resp);
    if (BURROW_FAILED(err))
        testing_t_error_v(d->t, err);
    d->bodies[g->i] = slurp;
    chan_send(d->got_body, &g->i);
}

static void test_transport_discards_unneeded_conns(TestingT *t, TestMode mode,
                                                   void *env) {
    (void)env;
    ClientServerTest *cst = new_client_server_test_func(
        t, mode, BURROW_FN(HttpHandlerFunc, hello_remote_addr_handler, NULL), NULL);

    DiscardConns *d = (DiscardConns *)test_alloc(t, sizeof *d);
    d->t = t;
    d->url = cst->ts->url;
    d->got_body = chan_make(heap_allocator(), TYPE_INT, DISCARD_N);
    if (d->got_body == NULL)
        testing_t_fatal_v(t, "out of memory");

    d->tls_config.insecure_skip_verify = true;
    /* Not in Go: Go's transport puts "h2" in TLSClientConfig.NextProtos when
     * it turns HTTP/2 on, and DialTLS's tls.Client offers it from there. Here
     * it goes only in the clone each of the transport's own connections gets,
     * so the config has what Go's would. */
    d->next_protos[0] = BURROW_S("h2");
    d->tls_config.next_protos = slice_from(d->next_protos, 1, 1, TYPE_STRING);
    d->tr.tls_client_config = &d->tls_config;
    d->tr.dial_tls = BURROW_FN(HttpDialFunc, discard_dial_tls, d);
    d->tr.free_conn = BURROW_FN(HttpFreeConnFunc, discard_free_conn, d);
    d->tr.protocols = &d->protocols;
    http_protocols_set_http2(&d->protocols, true);
    testing_t_cleanup(t, BURROW_FN(Func, discard_conns_free, d));

    d->c.transport = http_transport_as_round_tripper(&d->tr);

    DiscardGet gets[DISCARD_N];
    SyncWaitGroup wg = {0};
    for (Int i = 0; i < DISCARD_N; i++) {
        gets[i] = (DiscardGet){d, i};
        if (!sync_wait_group_go(&wg, BURROW_FN(Func, discard_get, &gets[i])))
            testing_t_errorf_v(t, "cannot start a goroutine");
    }
    sync_wait_group_wait(&wg);
    chan_close(d->got_body);

    Str last = BURROW_S("");
    Int i;
    while (chan_recv(d->got_body, &i)) {
        Str got = str_from_bytes(d->bodies[i].p, d->bodies[i].len);
        if (last.len == 0) {
            last = got;
            continue;
        }
        if (!str_eq(got, last))
            testing_t_errorf_v(t, "Response body changed: %q -> %q", last, got);
    }

    int32_t open = 0;
    int32_t close = 0;
    for (int n = 0; n < 150; n++) {
        open = sync_atomic_int32_load(&d->num_open);
        close = sync_atomic_int32_load(&d->num_close);
        if (open < 1)
            testing_t_fatalf_v(t, "open = %d; want at least", open);
        if (close == open - 1) {
            /* Success */
            return;
        }
        time_sleep(10 * TIME_MILLISECOND);
    }
    testing_t_errorf_v(t, "%d connections opened, %d closed; want %d to close", open,
                       close, open - 1);
}

static void TestTransportDiscardsUnneededConns(TestingT *t) {
    static const TestMode modes[1] = {HTTP2_MODE};
    RunOpts o = {modes, 1, false};
    run(t, test_transport_discards_unneeded_conns, NULL, &o);
}

/* tests that Transport doesn't retain a pointer to the provided request.
 *
 * Not in Go: there is no garbage collector to watch the request go. The
 * request is freed as soon as its response is, so a transport that kept a
 * pointer to it would use freed memory, which the sanitizer builds catch. */
static void gc_request_handler(void *env, HttpResponseWriter w, HttpRequest *r) {
    bool body = *(const bool *)env;
    Error err = BURROW_NO_ERROR;
    (void)io_read_all(arena_allocator(&r->arena),
                      (IoReader){&r->body.vt->reader, r->body.data}, &err);
    if (body)
        write_string(w, BURROW_S("Hello."));
}

static void test_transport_gc_request(TestingT *t, TestMode mode, bool body) {
    bool *bodyp = (bool *)test_alloc(t, sizeof *bodyp);
    *bodyp = body;
    ClientServerTest *cst = new_client_server_test_func(
        t, mode, BURROW_FN(HttpHandlerFunc, gc_request_handler, bodyp), NULL);

    Arena ar;
    arena_init(&ar, heap_allocator(), 0);
    Alloc *a = arena_allocator(&ar);
    Error err = BURROW_NO_ERROR;
    IoReader rbody =
        strings_reader_as_io_reader(strings_new_reader(a, BURROW_S("some body")));
    HttpRequest *req = http_new_request(a, BURROW_S("POST"), cst->ts->url, rbody, &err);
    if (req == NULL) {
        arena_free(&ar);
        testing_t_fatal_v(t, err);
    }
    HttpResponse *res = http_client_do(cst->c, req, &err);
    if (res == NULL) {
        http_request_free(req);
        arena_free(&ar);
        testing_t_fatal_v(t, err);
    }
    (void)io_read_all(a, cst_body(res), &err);
    Error cerr = res->body.vt->closer.close(res->body.data);
    http_response_free(res);
    http_request_free(req);
    arena_free(&ar);
    if (BURROW_FAILED(err))
        testing_t_fatal_v(t, err);
    if (BURROW_FAILED(cerr))
        testing_t_fatal_v(t, cerr);
}

static void gc_request_body(void *env, TestingT *t) {
    test_transport_gc_request(t, *(const TestMode *)env, true);
}

static void gc_request_no_body(void *env, TestingT *t) {
    test_transport_gc_request(t, *(const TestMode *)env, false);
}

static void transport_gc_request(TestingT *t, TestMode mode, void *env) {
    (void)env;
    TestMode *m = (TestMode *)test_alloc(t, sizeof *m);
    *m = mode;
    (void)testing_t_run(t, BURROW_S("Body"),
                        BURROW_FN(TestingTFunc, gc_request_body, m));
    (void)testing_t_run(t, BURROW_S("NoBody"),
                        BURROW_FN(TestingTFunc, gc_request_no_body, m));
}

static void TestTransportGCRequest(TestingT *t) {
    run(t, transport_gc_request, NULL, NULL);
}

static void handler_saw_headers(void *env, HttpResponseWriter w, HttpRequest *r) {
    (void)env;
    (void)fmt_fprintf_v(http_response_writer_as_io_writer(w), "Handler saw headers: %q",
                        header_string(arena_allocator(&r->arena), r->header));
}

static NetConn rejects_invalid_dial(void *env, Str netw, Str addr, Error *err) {
    bool v = true;
    chan_send((Chan *)env, &v);
    return net_dial(heap_allocator(), netw, addr, err);
}

enum { REJECTS_INVALID_N = 9 };

/* Not in Go: each test's dialedc, kept until the transport is done with
 * them. */
typedef struct RejectsInvalid {
    Chan *dialedc[REJECTS_INVALID_N];
} RejectsInvalid;

static void rejects_invalid_free(void *env) {
    RejectsInvalid *ri = (RejectsInvalid *)env;
    for (int i = 0; i < REJECTS_INVALID_N; i++)
        chan_free(ri->dialedc[i]);
}

static void test_transport_rejects_invalid_headers(TestingT *t, TestMode mode,
                                                   void *env) {
    (void)env;
    Alloc *a = test_arena(t);
    RejectsInvalid *ri = (RejectsInvalid *)test_alloc(t, sizeof *ri);
    testing_t_cleanup(t, BURROW_FN(Func, rejects_invalid_free, ri));
    CSTOpts opts = {.error_log = cst_quiet_log(t, a)};
    ClientServerTest *cst = new_client_server_test_func(
        t, mode, BURROW_FN(HttpHandlerFunc, handler_saw_headers, NULL), &opts);
    cst->tr->disable_keep_alives = true;

    struct {
        Str key, val;
        bool ok;
    } tests[REJECTS_INVALID_N] = {
        {BURROW_S("Foo"), BURROW_S("capital-key"),
         true}, /* verify h2 allows capital keys */
        {BURROW_S("Foo"),
         BURROW_S("foo\x00"
                  "bar"),
         false}, /* \x00 byte in value not allowed */
        {BURROW_S("Foo"), BURROW_S("two\nlines"),
         false}, /* \n byte in value not allowed */
        {BURROW_S("bogus\nkey"), BURROW_S("v"),
         false}, /* \n byte also not allowed in key */
        {BURROW_S("A space"), BURROW_S("v"), false}, /* spaces in keys not allowed */
        {BURROW_S("имя"), BURROW_S("v"), false},     /* key must be ascii */
        {BURROW_S("name"), BURROW_S("валю"), true},  /* value may be non-ascii */
        {BURROW_S(""), BURROW_S("v"), false},        /* key must be non-empty */
        {BURROW_S("k"), BURROW_S(""), true},         /* value may be empty */
    };
    for (int i = 0; i < REJECTS_INVALID_N; i++) {
        Chan *dialedc = chan_make(heap_allocator(), TYPE_BOOL, 1);
        if (dialedc == NULL)
            testing_t_fatal_v(t, "out of memory");
        ri->dialedc[i] = dialedc;
        cst->tr->dial = BURROW_FN(HttpDialFunc, rejects_invalid_dial, dialedc);
        Error err = BURROW_NO_ERROR;
        HttpRequest *req =
            http_new_request(a, BURROW_S("GET"), cst->ts->url, (IoReader){0}, &err);
        if (req == NULL)
            testing_t_fatal_v(t, err);
        Str *v = (Str *)mem_alloc(a, sizeof *v, _Alignof(Str));
        if (v == NULL)
            testing_t_fatal_v(t, "out of memory");
        *v = tests[i].val;
        Slice vs = slice_from(v, 1, 1, TYPE_STRING);
        if (!map_set(req->header, &tests[i].key, &vs))
            testing_t_fatal_v(t, "out of memory");
        HttpResponse *res = http_client_do(cst->c, req, &err);
        Slice body = {0};
        if (res != NULL) {
            Error rerr = BURROW_NO_ERROR;
            body = io_read_all(a, cst_body(res), &rerr);
            (void)res->body.vt->closer.close(res->body.data);
        }
        bool dialed = false;
        bool got;
        bool ok;
        if (chan_try_recv(dialedc, &got, &ok))
            dialed = true;

        if (!tests[i].ok && dialed) {
            /* Not in Go: the response's status stands for the response. */
            testing_t_errorf_v(
                t,
                "For key %q, value %q, transport dialed. Expected local "
                "failure. Response was: (%v, %v)\nServer replied with: %s",
                tests[i].key, tests[i].val,
                res != NULL ? res->status : BURROW_S("<nil>"), err,
                str_from_bytes(body.p, body.len));
        } else if ((res != NULL) != tests[i].ok) {
            testing_t_errorf_v(t, "For key %q, value %q; got err = %v; want ok=%v",
                               tests[i].key, tests[i].val, err, tests[i].ok);
        }
        if (res != NULL)
            http_response_free(res);
        http_request_free(req);
    }
}

static void TestTransportRejectsInvalidHeaders(TestingT *t) {
    RunOpts o = {http3_skipped_mode, 2, false};
    run(t, test_transport_rejects_invalid_headers, NULL, &o);
}

typedef struct LockedBytesBuffer {
    SyncMutex mu;
    BytesBuffer buf;
} LockedBytesBuffer;

static Int locked_bytes_buffer_write(void *self, Slice p, Error *err) {
    LockedBytesBuffer *b = (LockedBytesBuffer *)self;
    sync_mutex_lock(&b->mu);
    Int n = bytes_buffer_write(&b->buf, p, err);
    sync_mutex_unlock(&b->mu);
    return n;
}

static const IoWriterVT locked_bytes_buffer_vt = {NULL, locked_bytes_buffer_write};

/* Not in Go: the buffer's contents, copied into a under the lock. */
static Str locked_bytes_buffer_string(LockedBytesBuffer *b, Alloc *a) {
    sync_mutex_lock(&b->mu);
    Slice p = bytes_buffer_bytes(&b->buf);
    Str s = strings_clone(a, str_from_bytes(p.p, p.len));
    sync_mutex_unlock(&b->mu);
    return s;
}

/* waitCondition waits for fn to return true,
 * checking immediately and then at exponentially increasing intervals.
 *
 * Not in Go: it is from main_test.go, and fn takes env. */
static void wait_condition(TestingT *t, Duration delay,
                           bool (*fn)(TestingT *t, void *env, Duration d), void *env) {
    testing_t_helper(t);
    Time start = time_now();
    Duration since = 0;
    while (!fn(t, env, since)) {
        time_sleep(delay);
        delay = 2 * delay - (delay / 2); /* 1.5x, rounded up */
        since = time_since(start);
    }
}

/* Not in Go: what the handler and the test share. */
typedef struct InterruptPanic {
    Chan *test_done;
    Chan *got_headers;
    Any panic_value;
    LockedBytesBuffer error_log;
    LogLogger *log;
    Alloc *a;
    bool want_stack_logged;
} InterruptPanic;

static void interrupt_panic_free(void *env) {
    InterruptPanic *ip = (InterruptPanic *)env;
    chan_free(ip->test_done);
    chan_free(ip->got_headers);
    if (ip->log != NULL)
        log_logger_free(heap_allocator(), ip->log);
    bytes_buffer_free(&ip->error_log.buf);
}

static void interrupt_panic_done(void *env) {
    chan_close(((InterruptPanic *)env)->test_done);
}

static void interrupt_panic_handler(void *env, HttpResponseWriter w, HttpRequest *r) {
    (void)r;
    InterruptPanic *ip = (InterruptPanic *)env;
    write_string(w, BURROW_S("hello"));
    (void)w.vt->flush(w.data);

    SelectCase cases[] = {
        BURROW_RECV(ip->got_headers, NULL),
        BURROW_RECV(ip->test_done, NULL),
    };
    (void)chan_select(cases, 2);
    panic(ip->panic_value);
}

static void interrupt_panic_server(HttptestServer *ts, void *env) {
    ts->config.error_log = ((InterruptPanic *)env)->log;
}

static bool interrupt_panic_logged(TestingT *t, void *env, Duration d) {
    InterruptPanic *ip = (InterruptPanic *)env;
    Str got_log = locked_bytes_buffer_string(&ip->error_log, ip->a);
    if (!ip->want_stack_logged) {
        if (got_log.len == 0)
            return true;
        testing_t_fatalf_v(t, "want no log output; got: %s", got_log);
    }
    if (got_log.len == 0) {
        if (d > 0)
            testing_t_logf_v(t, "wanted a stack trace logged; got nothing after %v",
                             duration_string(d, ip->a));
        return false;
    }
    if (!strings_contains(got_log, BURROW_S("created by ")) &&
        strings_count(got_log, BURROW_S("\n")) < 6) {
        if (d > 0)
            testing_t_logf_v(
                t, "output doesn't look like a panic stack trace after %v. Got: %s",
                duration_string(d, ip->a), got_log);
        return false;
    }
    return true;
}

static void test_interrupt_with_panic(TestingT *t, TestMode mode, Any panic_value,
                                      bool abort) {
    Str msg = BURROW_S("hello");

    InterruptPanic *ip = (InterruptPanic *)test_alloc(t, sizeof *ip);
    testing_t_cleanup(t, BURROW_FN(Func, interrupt_panic_free, ip));
    ip->a = test_arena(t);
    ip->panic_value = panic_value;
    ip->test_done = chan_make(heap_allocator(), TYPE_BOOL, 0);
    ip->got_headers = chan_make(heap_allocator(), TYPE_BOOL, 1);
    ip->error_log.buf = BYTES_BUFFER(heap_allocator());
    IoWriter lw = {&locked_bytes_buffer_vt, &ip->error_log};
    ip->log = log_new(heap_allocator(), lw, BURROW_S(""), 0);
    if (ip->test_done == NULL || ip->got_headers == NULL || ip->log == NULL)
        testing_t_fatal_v(t, "out of memory");

    CSTOpts opts = {.server = interrupt_panic_server, .env = ip};
    ClientServerTest *cst = new_client_server_test_func(
        t, mode, BURROW_FN(HttpHandlerFunc, interrupt_panic_handler, ip), &opts);
    /* Not in Go: a cleanup registered after the server's runs before it, as
     * Go's defer does. */
    testing_t_cleanup(t, BURROW_FN(Func, interrupt_panic_done, ip));

    Error err = BURROW_NO_ERROR;
    HttpResponse *res = http_client_get(cst->c, cst->ts->url, &err);
    if (res == NULL)
        testing_t_fatal_v(t, err);
    bool v = true;
    chan_send(ip->got_headers, &v);
    Slice slurp = io_read_all(ip->a, cst_body(res), &err);
    cst_close_response(res);
    Str got = str_from_bytes(slurp.p, slurp.len);
    if (!str_eq(got, msg))
        testing_t_errorf_v(t, "client read %q; want %q", got, msg);
    if (BURROW_OK(err))
        testing_t_errorf_v(t, "client read all successfully; want some error");
    ip->want_stack_logged = panic_value.t != NULL && !abort;

    wait_condition(t, 10 * TIME_MILLISECOND, interrupt_panic_logged, ip);
}

static void interrupt_with_panic_boom(void *env, TestingT *t) {
    Str *boom = (Str *)test_alloc(t, sizeof *boom);
    *boom = BURROW_S("boom");
    test_interrupt_with_panic(t, *(const TestMode *)env, BURROW_ANY(TYPE_STRING, boom),
                              false);
}

static void interrupt_with_panic_abort(void *env, TestingT *t) {
    test_interrupt_with_panic(
        t, *(const TestMode *)env,
        BURROW_ANY(TYPE_ERROR, (void *)(uintptr_t)&http_err_abort_handler), true);
}

/* Not in Go: there is no "nil" subtest. It sets GODEBUG=panicnil=1, and
 * burrow has no such setting, so panic(nil) always panics with a value, which
 * is what Go does without it. */
static void interrupt_with_panic(TestingT *t, TestMode mode, void *env) {
    (void)env;
    TestMode *m = (TestMode *)test_alloc(t, sizeof *m);
    *m = mode;
    (void)testing_t_run(t, BURROW_S("boom"),
                        BURROW_FN(TestingTFunc, interrupt_with_panic_boom, m));
    (void)testing_t_run(t, BURROW_S("ErrAbortHandler"),
                        BURROW_FN(TestingTFunc, interrupt_with_panic_abort, m));
}

static void TestInterruptWithPanic(TestingT *t) {
    RunOpts o = {http3_skipped_mode, 2, true};
    run(t, interrupt_with_panic, NULL, &o);
}

/* Issue 15366 */
static void handler_gzip_foo(void *env, HttpResponseWriter w, HttpRequest *r) {
    (void)env;
    (void)r;
    HttpHeader h = w.vt->header(w.data);
    (void)http_header_set(h, BURROW_S("Content-Encoding"), BURROW_S("gzip"));
    (void)http_header_set(h, BURROW_S("Content-Length"), BURROW_S("23"));
    write_string(w,
                 BURROW_S("\x1f\x8b\b\x00\x00\x00\x00\x00\x00\x00s\xf3\xf7\a\x00\xab'"
                          "\xd4\x1a\x03\x00\x00\x00"));
}

static void gzip_dump_early_check(TestingT *t, void *env, Str proto, H12Res *r) {
    (void)env;
    HttpResponse *res = r->res;
    if (!res->uncompressed)
        testing_t_errorf_v(t, "%s: expected Uncompressed to be set", proto);
    Error err = BURROW_NO_ERROR;
    Slice dump = httputil_dump_response(test_arena(t), res, true, &err);
    if (BURROW_FAILED(err)) {
        testing_t_errorf_v(t, "%s: DumpResponse: %v", proto, err);
        return;
    }
    Str d = str_from_bytes(dump.p, dump.len);
    if (strings_contains(d, BURROW_S("Connection: close")))
        testing_t_errorf_v(
            t, "%s: should not see \"Connection: close\" in dump; got:\n%s", proto, d);
    if (!strings_contains(d, BURROW_S("FOO")))
        testing_t_errorf_v(t, "%s: should see \"FOO\" in response; got:\n%s", proto, d);
}

static void TestH12_AutoGzipWithDumpResponse(TestingT *t) {
    H12Compare tt = {.handler = handler_gzip_foo,
                     .early_check_response = gzip_dump_early_check};
    h12_run(t, &tt);
}

/* Issue 14607 */
static void x_addr_handler(void *env, HttpResponseWriter w, HttpRequest *r) {
    (void)env;
    (void)http_header_set(w.vt->header(w.data), BURROW_S("X-Addr"), r->remote_addr);
}

static Str close_idle_get(TestingT *t, ClientServerTest *cst, Alloc *a) {
    Error err = BURROW_NO_ERROR;
    HttpResponse *res = http_client_get(cst->c, cst->ts->url, &err);
    if (res == NULL)
        testing_t_fatal_v(t, err);
    (void)res->body.vt->closer.close(res->body.data);
    Str v = strings_clone(a, http_header_get(res->header, BURROW_S("X-Addr")));
    http_response_free(res);
    if (v.len == 0)
        testing_t_fatal_v(t, "didn't get X-Addr");
    return v;
}

static void test_close_idle_connections(TestingT *t, TestMode mode, void *env) {
    (void)env;
    ClientServerTest *cst = new_client_server_test_func(
        t, mode, BURROW_FN(HttpHandlerFunc, x_addr_handler, NULL), NULL);
    Alloc *a = test_arena(t);
    Str a1 = close_idle_get(t, cst, a);
    http_transport_close_idle_connections(cst->tr);
    Str a2 = close_idle_get(t, cst, a);
    if (str_eq(a1, a2))
        testing_t_errorf_v(t, "didn't close connection");
}

static void TestCloseIdleConnections(TestingT *t) {
    run(t, test_close_idle_connections, NULL, NULL);
}

static Int test_error_reader_read(void *self, Slice p, Error *err) {
    (void)p;
    testing_t_error_v((TestingT *)self, "unexpected Read call");
    if (err != NULL)
        *err = io_eof;
    return 0;
}

static const IoReaderVT test_error_reader_vt = {NULL, test_error_reader_read};

static void unauthorized_handler(void *env, HttpResponseWriter w, HttpRequest *r) {
    (void)env;
    (void)r;
    w.vt->write_header(w.data, HTTP_STATUS_UNAUTHORIZED);
}

static void test_no_sniff_expect_request_body(TestingT *t, TestMode mode, void *env) {
    (void)env;
    ClientServerTest *cst = new_client_server_test_func(
        t, mode, BURROW_FN(HttpHandlerFunc, unauthorized_handler, NULL), NULL);

    /* Set ExpectContinueTimeout non-zero so RoundTrip won't try to write it. */
    cst->tr->expect_continue_timeout = 10 * TIME_SECOND;

    Alloc *a = test_arena(t);
    Error err = BURROW_NO_ERROR;
    HttpRequest *req = http_new_request(a, BURROW_S("POST"), cst->ts->url,
                                        (IoReader){&test_error_reader_vt, t}, &err);
    if (req == NULL)
        testing_t_fatal_v(t, err);
    testing_t_cleanup(t, BURROW_FN(Func, cst_free_request, req));
    req->content_length = 0; /* so transport is tempted to sniff it */
    (void)http_header_set(req->header, BURROW_S("Expect"), BURROW_S("100-continue"));
    HttpResponse *res = http_transport_round_trip(cst->tr, req, &err);
    if (res == NULL)
        testing_t_fatal_v(t, err);
    if (res->status_code != HTTP_STATUS_UNAUTHORIZED)
        testing_t_errorf_v(t, "status code = %v; want %v", res->status_code,
                           (Int)HTTP_STATUS_UNAUTHORIZED);
    cst_close_response(res);
}

static void TestNoSniffExpectRequestBody(TestingT *t) {
    run(t, test_no_sniff_expect_request_body, NULL, NULL);
}

static void undeclared_trailers_handler(void *env, HttpResponseWriter w,
                                        HttpRequest *r) {
    (void)env;
    (void)r;
    (void)http_header_set(w.vt->header(w.data), BURROW_S("Foo"), BURROW_S("Bar"));
    (void)http_header_set(w.vt->header(w.data), BURROW_S("Trailer:Foo"),
                          BURROW_S("Baz"));
    (void)w.vt->flush(w.data);
    (void)http_header_add(w.vt->header(w.data), BURROW_S("Trailer:Foo"),
                          BURROW_S("Baz2"));
    (void)http_header_set(w.vt->header(w.data), BURROW_S("Trailer:Bar"),
                          BURROW_S("Quux"));
}

static void test_server_undeclared_trailers(TestingT *t, TestMode mode, void *env) {
    (void)env;
    ClientServerTest *cst = new_client_server_test_func(
        t, mode, BURROW_FN(HttpHandlerFunc, undeclared_trailers_handler, NULL), NULL);
    Error err = BURROW_NO_ERROR;
    HttpResponse *res = http_client_get(cst->c, cst->ts->url, &err);
    if (res == NULL)
        testing_t_fatal_v(t, err);
    Alloc *a = test_arena(t);
    (void)io_copy(a, io_discard, cst_body(res), &err);
    if (BURROW_FAILED(err)) {
        cst_close_response(res);
        testing_t_fatal_v(t, err);
    }
    (void)res->body.vt->closer.close(res->body.data);
    http_header_del(res->header, BURROW_S("Date"));
    http_header_del(res->header, BURROW_S("Content-Type"));

    static const char *const bar[] = {"Bar"};
    HttpHeader want = http_header_make(a);
    if (want == NULL)
        testing_t_fatal_v(t, "out of memory");
    header_set_values(t, want, a, BURROW_S("Foo"), bar, 1);
    if (!header_equal(res->header, want))
        testing_t_errorf_v(t, "Header = %v; want %v", header_string(a, res->header),
                           header_string(a, want));

    static const char *const baz[] = {"Baz", "Baz2"};
    static const char *const quux[] = {"Quux"};
    want = http_header_make(a);
    if (want == NULL)
        testing_t_fatal_v(t, "out of memory");
    header_set_values(t, want, a, BURROW_S("Foo"), baz, 2);
    header_set_values(t, want, a, BURROW_S("Bar"), quux, 1);
    if (!header_equal(res->trailer, want))
        testing_t_errorf_v(t, "Trailer = %v; want %v", header_string(a, res->trailer),
                           header_string(a, want));
    http_response_free(res);
}

static void TestServerUndeclaredTrailers(TestingT *t) {
    RunOpts o = {http3_skipped_mode, 2, false};
    run(t, test_server_undeclared_trailers, NULL, &o);
}

static void bad_response_handler(void *env, HttpResponseWriter w, HttpRequest *r) {
    TestingT *t = (TestingT *)env;
    Error err = BURROW_NO_ERROR;
    (void)io_copy(arena_allocator(&r->arena), io_discard,
                  (IoReader){&r->body.vt->reader, r->body.data}, &err);
    if (BURROW_FAILED(err))
        testing_t_fatal_v(t, err);
    HttpResponseController rc = http_new_response_controller(w);
    BufioReadWriter buf;
    NetConn c = http_response_controller_hijack(&rc, &buf, &err);
    if (BURROW_FAILED(err))
        testing_t_fatal_v(t, err);
    IoWriter cw = {&c.vt->writer, c.data};
    (void)fmt_fprintln_v(cw, "some bogus crap");
    /* Not in Go: what the hijack handed over is given back, and net_conn_free
     * closes the connection. */
    bufio_reader_free(buf.reader);
    bufio_writer_free(buf.writer);
    net_conn_free(c);
}

/* countCloseReader, from transport_test.go. */
typedef struct CountCloseReader {
    SyncAtomicInt32 *n;
    IoReader r;
} CountCloseReader;

static Int count_close_read(void *self, Slice p, Error *err) {
    IoReader r = ((CountCloseReader *)self)->r;
    return r.vt->read(r.data, p, err);
}

static Error count_close_close(void *self) {
    (void)sync_atomic_int32_add(((CountCloseReader *)self)->n, 1);
    return BURROW_NO_ERROR;
}

static const IoReadCloserVT count_close_reader_vt = {{NULL, count_close_read},
                                                     {NULL, count_close_close}};

static void test_bad_response_after_reading_body(TestingT *t, TestMode mode,
                                                 void *env) {
    (void)env;
    ClientServerTest *cst = new_client_server_test_func(
        t, mode, BURROW_FN(HttpHandlerFunc, bad_response_handler, t), NULL);

    /* Not in Go: Post takes an IoReader, which cannot say that it is an
     * io.ReadCloser, so the request is the one Post makes, with the closer set
     * as its body by hand. closes is atomic, since the transport may close the
     * body on a goroutine of its own. */
    SyncAtomicInt32 *closes = (SyncAtomicInt32 *)test_alloc(t, sizeof *closes);
    Alloc *a = test_arena(t);
    CountCloseReader *cr = (CountCloseReader *)test_alloc(t, sizeof *cr);
    *cr = (CountCloseReader){
        closes, strings_reader_as_io_reader(strings_new_reader(a, BURROW_S("hello")))};
    Error err = BURROW_NO_ERROR;
    IoReader body = {&count_close_reader_vt.reader, cr};
    HttpRequest *req = http_new_request(a, BURROW_S("POST"), cst->ts->url, body, &err);
    if (req == NULL)
        testing_t_fatal_v(t, err);
    testing_t_cleanup(t, BURROW_FN(Func, cst_free_request, req));
    req->body = (IoReadCloser){&count_close_reader_vt, cr};
    (void)http_header_set(req->header, BURROW_S("Content-Type"),
                          BURROW_S("text/plain"));
    HttpResponse *res = http_client_do(cst->c, req, &err);
    if (res != NULL) {
        cst_close_response(res);
        testing_t_fatal_v(t, "expected an error to be returned from Post");
    }
    int32_t n = sync_atomic_int32_load(closes);
    if (n != 1)
        testing_t_errorf_v(t, "closes = %d; want 1", n);
}

static void TestBadResponseAfterReadingBody(TestingT *t) {
    static const TestMode modes[1] = {HTTP1_MODE};
    RunOpts o = {modes, 1, false};
    run(t, test_bad_response_after_reading_body, NULL, &o);
}

#define TESTS(X)                                                                       \
    X(TestNewClientServerTest)                                                         \
    X(TestChunkedResponseHeaders)                                                      \
    X(TestH12_HeadContentLengthNoBody)                                                 \
    X(TestH12_HeadContentLengthSmallBody)                                              \
    X(TestH12_HeadContentLengthLargeBody)                                              \
    X(TestH12_200NoBody)                                                               \
    X(TestH2_204NoBody)                                                                \
    X(TestH2_304NoBody)                                                                \
    X(TestH2_404NoBody)                                                                \
    X(TestH12_SmallBody)                                                               \
    X(TestH12_ExplicitContentLength)                                                   \
    X(TestH12_FlushBeforeBody)                                                         \
    X(TestH12_FlushMidBody)                                                            \
    X(TestH12_Head_ExplicitLen)                                                        \
    X(TestH12_Head_ImplicitLen)                                                        \
    X(TestH12_HandlerWritesTooLittle)                                                  \
    X(TestHandlerWritesTooMuch)                                                        \
    X(TestH12_AutoGzip)                                                                \
    X(TestH12_AutoGzip_Disabled)                                                       \
    X(Test304Responses)                                                                \
    X(TestH12_ServerEmptyContentLength)                                                \
    X(TestH12_RequestContentLength_Known_NonZero)                                      \
    X(TestH12_RequestContentLength_Known_Zero)                                         \
    X(TestH12_RequestContentLength_Unknown)                                            \
    X(TestCancelRequestMidBody)                                                        \
    X(TestTrailersClientToServer)                                                      \
    X(TestTrailersServerToClient)                                                      \
    X(TestTrailersServerToClientFlush)                                                 \
    X(TestResponseBodyReadAfterClose)                                                  \
    X(TestConcurrentReadWriteReqBody)                                                  \
    X(TestConnectRequest)                                                              \
    X(TestTransportUserAgent)                                                          \
    X(TestStarRequestMethod)                                                           \
    X(TestTransportDiscardsUnneededConns)                                              \
    X(TestTransportGCRequest)                                                          \
    X(TestTransportRejectsInvalidHeaders)                                              \
    X(TestInterruptWithPanic)                                                          \
    X(TestH12_AutoGzipWithDumpResponse)                                                \
    X(TestCloseIdleConnections)                                                        \
    X(TestNoSniffExpectRequestBody)                                                    \
    X(TestServerUndeclaredTrailers)                                                    \
    X(TestBadResponseAfterReadingBody)
TESTING_MAIN(TESTS)
