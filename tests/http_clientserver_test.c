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

#include "burrow/burrow.h"
#include "burrow/bytes.h"
#include "burrow/compress/gzip.h"
#include "burrow/error.h"
#include "burrow/fmt.h"
#include "burrow/io.h"
#include "burrow/log.h"
#include "burrow/map.h"
#include "burrow/net/http.h"
#include "burrow/net/http/httptest.h"
#include "burrow/os.h"
#include "burrow/strings.h"
#include "burrow/sync.h"
#include "burrow/testing.h"

#include <stdint.h>
#include <string.h>

/* Not in Go: s as the []byte Go would convert it to. */
static Slice bytes_of(Str s) {
    return slice_from((void *)(uintptr_t)s.p, s.len, s.len, TYPE_BYTE);
}

static void write_string(HttpResponseWriter w, Str s) {
    (void)io_write_string(http_response_writer_as_io_writer(w), s, NULL);
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

typedef HttpResponse *(*ReqFunc)(HttpClient *c, Str url, Error *err);

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
        return http_client_get;
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
    H12Res res1 = {h12_req_func(tt)(cst1->c, cst1->ts->url, &err), {0}, {0}};
    if (res1.res == NULL) {
        testing_t_errorf_v(t, "HTTP/1 request: %v", err);
        cst_close(cst2);
        cst_close(cst1);
        return;
    }
    H12Res res2 = {h12_req_func(tt)(cst2->c, cst2->ts->url, &err), {0}, {0}};
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
    H12Compare tt = {.req_func = http_client_head, .handler = handler_nothing};
    h12_run(t, &tt);
}

static void handler_small(void *env, HttpResponseWriter w, HttpRequest *r) {
    (void)env;
    (void)r;
    write_string(w, BURROW_S("small"));
}

static void TestH12_HeadContentLengthSmallBody(TestingT *t) {
    H12Compare tt = {.req_func = http_client_head, .handler = handler_small};
    h12_run(t, &tt);
}

static void handler_large(void *env, HttpResponseWriter w, HttpRequest *r) {
    (void)env;
    Str chunk = strings_repeat(r->a, BURROW_S("x"), 512 * 1024);
    for (int i = 0; i < 10; i++)
        write_string(w, chunk);
}

static void TestH12_HeadContentLengthLargeBody(TestingT *t) {
    H12Compare tt = {.req_func = http_client_head, .handler = handler_large};
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
        .req_func = http_client_head, .handler = handler_head_explicit_len, .env = t};
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
        .req_func = http_client_head, .handler = handler_head_implicit_len, .env = t};
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
    (void)http_header_set(w.vt->header(w.data), BURROW_S("Content-Length"),
                          fmt_sprintf_v(r->a, "%v", want_body.len));
    (void)http_response_controller_flush(&rc);
    IoWriter ww = http_response_writer_as_io_writer(w);
    (void)ww.vt->write(ww.data, bytes_of(want_body), NULL);
    (void)http_response_controller_flush(&rc);
    Error err = BURROW_NO_ERROR;
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
    Slice got_body = io_read_all(a, cst_body(res), NULL);
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
    GzipWriter *gz = gzip_new_writer(r->a, http_response_writer_as_io_writer(w));
    if (gz == NULL) {
        testing_t_errorf_v(t, "out of memory");
        return;
    }
    (void)io_write_string(
        gzip_writer_as_io_writer(gz),
        BURROW_S("I am some gzipped content. Go go go go go go go go go "
                 "go go go should compress well."),
        NULL);
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
                          fmt_sprint_v(r->a, r->content_length));
    (void)fmt_fprintf_v(http_response_writer_as_io_writer(w), "Req.ContentLength=%v",
                        r->content_length);
}

/* Not in Go: the test's body function, which the ReqFunc can't take. */
static const H12ReqCL *h12_req_cl;
static Alloc *h12_req_cl_alloc;

static HttpResponse *post_body(HttpClient *c, Str url, Error *err) {
    return http_client_post(c, url, BURROW_S("text/plain"),
                            h12_req_cl->bodyfn(h12_req_cl_alloc), err);
}

static void check_request_content_length(TestingT *t, void *env, Str proto,
                                         H12Res *res) {
    const H12ReqCL *cl = (const H12ReqCL *)env;
    Str got = http_header_get(res->res->header, BURROW_S("Got-Length"));
    Str want = fmt_sprint_v(h12_req_cl_alloc, cl->want_len);
    if (!str_eq(got, want))
        testing_t_errorf_v(t, "Proto %q got length %q; want %q", proto, got, want);
}

static void h12_request_content_length(TestingT *t, IoReader (*bodyfn)(Alloc *a),
                                       int64_t want_len) {
    H12ReqCL *cl = (H12ReqCL *)test_alloc(t, sizeof *cl);
    cl->bodyfn = bodyfn;
    cl->want_len = want_len;
    h12_req_cl = cl;
    h12_req_cl_alloc = test_arena(t);
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
    X(TestH12_RequestContentLength_Unknown)
TESTING_MAIN(TESTS)
