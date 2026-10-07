/* Derived from Go's src/net/http/httptest/httptest_test.go, recorder_test.go
 * and server_test.go. The Server tests are the HTTP ones, since there is no
 * TLS yet, and NewTestServer is not here either.
 * Go source: go1.27.1.
 *
 * Go's request in the https case of TestNewRequestWithContext also has TLS
 * state, which needs crypto/tls, so that one field is not checked here.
 *
 * Copyright 2016 The Go Authors. All rights reserved.
 * Copyright 2026 The burrow Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style licence that can be found
 * in the LICENSE file. */

#include "check.h"

#include "../src/net/http_internal.h"

#include "burrow/burrow.h"
#include "burrow/bytes.h"
#include "burrow/context.h"
#include "burrow/fmt.h"
#include "burrow/iface.h"
#include "burrow/io.h"
#include "burrow/map.h"
#include "burrow/mem/arena.h"
#include "burrow/mem/heap.h"
#include "burrow/net/http.h"
#include "burrow/net/http/httptest.h"
#include "burrow/net/url.h"
#include "burrow/panic.h"
#include "burrow/strings.h"

#if defined(BURROW_NETPOLL_READINESS) && !defined(BURROW_OS_WASI)
#define HAVE_TCP 1
#endif

#define S BURROW_S

static Str cs(const char *s) {
    return s != NULL ? str_from_cstr(s) : (Str){0};
}

/* ------------------------------------------------------------ TestRecorder */

typedef void (*Handler)(HttpResponseWriter w, HttpRequest *r);
typedef void (*Check)(TestingT *t, HttptestResponseRecorder *rec);

static HttptestResponseRecorder *as_recorder(HttpResponseWriter w) {
    return (HttptestResponseRecorder *)iface_assert(BURROW_IFACE(w),
                                                    TYPE_HTTPTEST_RESPONSE_RECORDER);
}

static void write_bytes(HttpResponseWriter w, const char *s) {
    Str str = cs(s);
    (void)http_response_writer_write(
        w, slice_from((void *)(uintptr_t)str.p, str.len, str.len, TYPE_BYTE), NULL);
}

static void write_string(HttpResponseWriter w, const char *s) {
    (void)io_write_string(http_response_writer_as_io_writer(w), cs(s), NULL);
}

static void set_header(HttpResponseWriter w, const char *k, const char *v) {
    (void)http_header_set(http_response_writer_header(w), cs(k), cs(v));
}

static HttpResponse *result(TestingT *t, HttptestResponseRecorder *rec) {
    HttpResponse *res = httptest_response_recorder_result(rec);
    if (res == NULL) {
        testing_t_errorf_v(t, "Result() = nil");
        testing_t_fail_now(t);
    }
    return res;
}

static void has_status(TestingT *t, HttptestResponseRecorder *rec, Int want) {
    if (rec->code != want)
        testing_t_errorf_v(t, "Status = %d; want %d", rec->code, want);
}

static void has_result_status(TestingT *t, HttptestResponseRecorder *rec,
                              const char *want) {
    Str got = result(t, rec)->status;
    if (!str_eq(got, cs(want)))
        testing_t_errorf_v(t, "Result().Status = %q; want %q", got, cs(want));
}

static void has_result_status_code(TestingT *t, HttptestResponseRecorder *rec,
                                   Int want) {
    Int got = result(t, rec)->status_code;
    if (got != want)
        testing_t_errorf_v(t, "Result().StatusCode = %d; want %d", got, want);
}

static void has_result_contents(TestingT *t, HttptestResponseRecorder *rec,
                                const char *want) {
    Arena ar;
    arena_init(&ar, NULL, 0);
    Error err = BURROW_NO_ERROR;
    Slice b = io_read_all(arena_allocator(&ar),
                          io_read_closer_as_io_reader(result(t, rec)->body), &err);
    if (!BURROW_OK(err)) {
        testing_t_errorf_v(t, "%s", error_text(err));
    } else {
        Str got = {(const Byte *)b.p, b.len};
        if (!str_eq(got, cs(want)))
            testing_t_errorf_v(t, "Result().Body = %s; want %s", got, cs(want));
    }
    arena_free(&ar);
}

static void has_contents(TestingT *t, HttptestResponseRecorder *rec, const char *want) {
    Slice b = bytes_buffer_bytes(rec->body);
    Str got = {(const Byte *)b.p, b.len};
    if (!str_eq(got, cs(want)))
        testing_t_errorf_v(t, "wrote = %q; want %q", got, cs(want));
}

static void has_flush(TestingT *t, HttptestResponseRecorder *rec, bool want) {
    if (rec->flushed != want)
        testing_t_errorf_v(t, "Flushed = %v; want %v", rec->flushed, want);
}

static void has_old_header(TestingT *t, HttptestResponseRecorder *rec, const char *key,
                           const char *want) {
    Str got = http_header_get(rec->header_map, cs(key));
    if (!str_eq(got, cs(want)))
        testing_t_errorf_v(t, "HeaderMap header %s = %q; want %q", cs(key), got,
                           cs(want));
}

static void has_header(TestingT *t, HttptestResponseRecorder *rec, const char *key,
                       const char *want) {
    Str got = http_header_get(result(t, rec)->header, cs(key));
    if (!str_eq(got, cs(want)))
        testing_t_errorf_v(t, "final header %s = %q; want %q", cs(key), got, cs(want));
}

/* Whether h has the canonical form of key, as Go's h[CanonicalHeaderKey(k)]. */
static bool has_key(HttpHeader h, const char *key) {
    if (h == NULL)
        return false;
    Arena ar;
    arena_init(&ar, NULL, 0);
    Str k = http_canonical_header_key(arena_allocator(&ar), cs(key));
    bool ok = map_get(h, &k) != NULL;
    arena_free(&ar);
    return ok;
}

static void has_not_headers(TestingT *t, HttptestResponseRecorder *rec,
                            const char *const *keys, Int n) {
    HttpHeader h = result(t, rec)->header;
    for (Int i = 0; i < n; i++) {
        if (has_key(h, keys[i]))
            testing_t_errorf_v(t, "unexpected header %s with value %q", cs(keys[i]),
                               http_header_get(h, cs(keys[i])));
    }
}

static void has_trailer(TestingT *t, HttptestResponseRecorder *rec, const char *key,
                        const char *want) {
    Str got = http_header_get(result(t, rec)->trailer, cs(key));
    if (!str_eq(got, cs(want)))
        testing_t_errorf_v(t, "trailer %s = %q; want %q", cs(key), got, cs(want));
}

static void has_not_trailers(TestingT *t, HttptestResponseRecorder *rec,
                             const char *const *keys, Int n) {
    HttpHeader h = result(t, rec)->trailer;
    for (Int i = 0; i < n; i++) {
        if (has_key(h, keys[i]))
            testing_t_errorf_v(t, "unexpected trailer %s", cs(keys[i]));
    }
}

static void has_content_length(TestingT *t, HttptestResponseRecorder *rec,
                               int64_t want) {
    int64_t got = result(t, rec)->content_length;
    if (got != want)
        testing_t_errorf_v(t, "ContentLength = %d; want %d", got, want);
}

#define NKEYS(k) (Int)(sizeof(k) / sizeof((k)[0]))

/* 200 default */
static void h_default(HttpResponseWriter w, HttpRequest *r) {
    (void)w;
    (void)r;
}
static void c_default(TestingT *t, HttptestResponseRecorder *rec) {
    has_status(t, rec, 200);
    has_contents(t, rec, "");
}

/* first code only */
static void h_first_code(HttpResponseWriter w, HttpRequest *r) {
    (void)r;
    http_response_writer_write_header(w, 201);
    http_response_writer_write_header(w, 202);
    write_bytes(w, "hi");
}
static void c_first_code(TestingT *t, HttptestResponseRecorder *rec) {
    has_status(t, rec, 201);
    has_contents(t, rec, "hi");
}

/* write sends 200 */
static void h_write_200(HttpResponseWriter w, HttpRequest *r) {
    (void)r;
    write_bytes(w, "hi first");
    http_response_writer_write_header(w, 201);
    http_response_writer_write_header(w, 202);
}
static void c_write_200(TestingT *t, HttptestResponseRecorder *rec) {
    has_status(t, rec, 200);
    has_contents(t, rec, "hi first");
    has_flush(t, rec, false);
}

/* write string */
static void h_write_string(HttpResponseWriter w, HttpRequest *r) {
    (void)r;
    write_string(w, "hi first");
}
static void c_write_string(TestingT *t, HttptestResponseRecorder *rec) {
    has_status(t, rec, 200);
    has_contents(t, rec, "hi first");
    has_flush(t, rec, false);
    has_header(t, rec, "Content-Type", "text/plain; charset=utf-8");
}

/* flush */
static void h_flush(HttpResponseWriter w, HttpRequest *r) {
    (void)r;
    httptest_response_recorder_flush(as_recorder(w)); /* also sends a 200 */
    http_response_writer_write_header(w, 201);
}
static void c_flush(TestingT *t, HttptestResponseRecorder *rec) {
    has_status(t, rec, 200);
    has_flush(t, rec, true);
    has_content_length(t, rec, -1);
}

/* Content-Type detection */
static void h_detect(HttpResponseWriter w, HttpRequest *r) {
    (void)r;
    write_string(w, "<html>");
}
static void c_detect(TestingT *t, HttptestResponseRecorder *rec) {
    has_header(t, rec, "Content-Type", "text/html; charset=utf-8");
}

/* no Content-Type detection with Transfer-Encoding */
static void h_te(HttpResponseWriter w, HttpRequest *r) {
    (void)r;
    set_header(w, "Transfer-Encoding", "some encoding");
    write_string(w, "<html>");
}
static void c_te(TestingT *t, HttptestResponseRecorder *rec) {
    has_header(t, rec, "Content-Type", ""); /* no header */
}

/* no Content-Type detection if set explicitly */
static void h_explicit(HttpResponseWriter w, HttpRequest *r) {
    (void)r;
    set_header(w, "Content-Type", "some/type");
    write_string(w, "<html>");
}
static void c_explicit(TestingT *t, HttptestResponseRecorder *rec) {
    has_header(t, rec, "Content-Type", "some/type");
}

/* Content-Type detection doesn't crash if HeaderMap is nil */
static void h_nil_map(HttpResponseWriter w, HttpRequest *r) {
    (void)r;
    as_recorder(w)->header_map = NULL;
    write_string(w, "<html>");
}

/* Header is not changed after write */
static void h_snapshot(HttpResponseWriter w, HttpRequest *r) {
    (void)r;
    HttpHeader hdr = http_response_writer_header(w);
    (void)http_header_set(hdr, S("Key"), S("correct"));
    http_response_writer_write_header(w, 200);
    (void)http_header_set(hdr, S("Key"), S("incorrect"));
}
static void c_snapshot(TestingT *t, HttptestResponseRecorder *rec) {
    has_header(t, rec, "Key", "correct");
}

/* Trailer headers are correctly recorded */
static void h_trailers(HttpResponseWriter w, HttpRequest *r) {
    (void)r;
    set_header(w, "Non-Trailer", "correct");
    set_header(w, "Trailer", "Trailer-A, Trailer-B");
    (void)http_header_add(http_response_writer_header(w), S("Trailer"), S("Trailer-C"));
    write_string(w, "<html>");
    set_header(w, "Non-Trailer", "incorrect");
    set_header(w, "Trailer-A", "valuea");
    set_header(w, "Trailer-C", "valuec");
    set_header(w, "Trailer-NotDeclared", "should be omitted");
    set_header(w, "Trailer:Trailer-D", "with prefix");
}
static void c_trailers(TestingT *t, HttptestResponseRecorder *rec) {
    static const char *const not_headers[] = {"Trailer-A", "Trailer-B", "Trailer-C",
                                              "Trailer-NotDeclared"};
    static const char *const not_trailers[] = {"Non-Trailer", "Trailer-B",
                                               "Trailer-NotDeclared"};
    has_status(t, rec, 200);
    has_header(t, rec, "Content-Type", "text/html; charset=utf-8");
    has_header(t, rec, "Non-Trailer", "correct");
    has_not_headers(t, rec, not_headers, NKEYS(not_headers));
    has_trailer(t, rec, "Trailer-A", "valuea");
    has_trailer(t, rec, "Trailer-C", "valuec");
    has_not_trailers(t, rec, not_trailers, NKEYS(not_trailers));
    has_trailer(t, rec, "Trailer-D", "with prefix");
}

/* Header set without any write, issue 15560 */
static void h_no_write(HttpResponseWriter w, HttpRequest *r) {
    (void)r;
    set_header(w, "X-Foo", "1");
    as_recorder(w)->code = 0;
}
static void c_no_write(TestingT *t, HttptestResponseRecorder *rec) {
    has_old_header(t, rec, "X-Foo", "1");
    has_status(t, rec, 0);
    has_header(t, rec, "X-Foo", "1");
    has_result_status(t, rec, "200 OK");
    has_result_status_code(t, rec, 200);
}

/* HeaderMap vs FinalHeaders, more for issue 15560 */
static void h_final(HttpResponseWriter w, HttpRequest *r) {
    (void)r;
    HttpHeader h = http_response_writer_header(w);
    (void)http_header_set(h, S("X-Foo"), S("1"));
    write_bytes(w, "hi");
    (void)http_header_set(h, S("X-Foo"), S("2"));
    (void)http_header_set(h, S("X-Bar"), S("2"));
}
static void c_final(TestingT *t, HttptestResponseRecorder *rec) {
    static const char *const not_headers[] = {"X-Bar"};
    has_old_header(t, rec, "X-Foo", "2");
    has_old_header(t, rec, "X-Bar", "2");
    has_header(t, rec, "X-Foo", "1");
    has_not_headers(t, rec, not_headers, NKEYS(not_headers));
}

/* setting Content-Length header */
static void h_content_length(HttpResponseWriter w, HttpRequest *r) {
    (void)r;
    /* The value has to last as long as the recorder, so it is a literal here
     * where Go formats it. */
    (void)http_header_set(http_response_writer_header(w), S("Content-Length"), S("9"));
    write_string(w, "Some body");
}
static void c_content_length(TestingT *t, HttptestResponseRecorder *rec) {
    has_status(t, rec, 200);
    has_contents(t, rec, "Some body");
    has_content_length(t, rec, 9);
}

/* nil ResponseRecorder.Body, issue 26642 */
static void h_nil_body(HttpResponseWriter w, HttpRequest *r) {
    (void)r;
    as_recorder(w)->body = NULL;
    write_string(w, "hi");
}
static void c_nil_body(TestingT *t, HttptestResponseRecorder *rec) {
    has_result_contents(t, rec, ""); /* reading the body does not crash */
}

typedef struct RecorderTest {
    const char *name;
    Handler h;
    Check check;
} RecorderTest;

static const RecorderTest recorder_tests[] = {
    {"200 default", h_default, c_default},
    {"first code only", h_first_code, c_first_code},
    {"write sends 200", h_write_200, c_write_200},
    {"write string", h_write_string, c_write_string},
    {"flush", h_flush, c_flush},
    {"Content-Type detection", h_detect, c_detect},
    {"no Content-Type detection with Transfer-Encoding", h_te, c_te},
    {"no Content-Type detection if set explicitly", h_explicit, c_explicit},
    {"Content-Type detection doesn't crash if HeaderMap is nil", h_nil_map, c_detect},
    {"Header is not changed after write", h_snapshot, c_snapshot},
    {"Trailer headers are correctly recorded", h_trailers, c_trailers},
    {"Header set without any write", h_no_write, c_no_write},
    {"HeaderMap vs FinalHeaders", h_final, c_final},
    {"setting Content-Length header", h_content_length, c_content_length},
    {"nil ResponseRecorder.Body", h_nil_body, c_nil_body},
};

static void serve(void *env, HttpResponseWriter w, HttpRequest *r) {
    ((const RecorderTest *)env)->h(w, r);
}

static void recorder_case(void *env, TestingT *t) {
    const RecorderTest *tt = (const RecorderTest *)env;
    Alloc *a = heap_allocator();
    Error err = BURROW_NO_ERROR;
    HttpRequest *r =
        http_new_request(a, S("GET"), S("http://foo.com/"), (IoReader){0}, &err);
    if (r == NULL) {
        testing_t_fatalf_v(t, "NewRequest: %s", error_text(err));
        return;
    }
    HttptestResponseRecorder *rec = httptest_new_recorder(a);
    if (rec == NULL) {
        testing_t_fatalf_v(t, "out of memory");
        return;
    }
    HttpHandlerFunc h = BURROW_FN(HttpHandlerFunc, serve, (void *)(uintptr_t)tt);
    http_handler_func_serve_http(h, httptest_response_recorder_as_response_writer(rec),
                                 r);
    tt->check(t, rec);
    httptest_response_recorder_free(rec);
    http_request_free(r);
}

static void TestRecorder(TestingT *t) {
    for (size_t i = 0; i < sizeof recorder_tests / sizeof recorder_tests[0]; i++) {
        const RecorderTest *tt = &recorder_tests[i];
        testing_t_run(t, cs(tt->name),
                      BURROW_FN(TestingTFunc, recorder_case, (void *)(uintptr_t)tt));
    }
}

static void TestBodyNotAllowed(TestingT *t) {
    HttptestResponseRecorder *rw = httptest_new_recorder(heap_allocator());
    if (rw == NULL) {
        testing_t_fatalf_v(t, "out of memory");
        return;
    }
    httptest_response_recorder_write_header(rw, 204);

    Error err = BURROW_NO_ERROR;
    Str hello = S("hello ");
    (void)httptest_response_recorder_write(
        rw, slice_from((void *)(uintptr_t)hello.p, hello.len, hello.len, TYPE_BYTE),
        &err);
    if (!errors_is(err, http_err_body_not_allowed))
        testing_t_errorf_v(t, "expected BodyNotAllowed for Write after 204, got: %v",
                           err);

    (void)httptest_response_recorder_write_string(rw, S("world"), &err);
    if (!errors_is(err, http_err_body_not_allowed))
        testing_t_errorf_v(
            t, "expected BodyNotAllowed for WriteString after 204, got: %v", err);

    Slice b = bytes_buffer_bytes(rw->body);
    Str got = {(const Byte *)b.p, b.len};
    if (!str_eq(got, S("hello world")))
        testing_t_errorf_v(t, "got Body=%q, want %q", got, S("hello world"));
    httptest_response_recorder_free(rw);
}

static void TestParseContentLength(TestingT *t) {
    static const struct {
        const char *cl;
        int64_t want;
    } tests[] = {
        {"3", 3},
        {"+3", -1},
        {"-3", -1},
        {"9223372036854775807", INT64_MAX},
        {"9223372036854775808", -1},
    };
    for (size_t i = 0; i < sizeof tests / sizeof tests[0]; i++) {
        int64_t got = burrow__httptest_parse_content_length(cs(tests[i].cl));
        if (got != tests[i].want)
            testing_t_errorf_v(t, "%q:\n\tgot=%d\n\twant=%d", cs(tests[i].cl), got,
                               tests[i].want);
    }
}

static void bad_code_case(void *env, TestingT *t) {
    Int code = (Int)(intptr_t)env;
    HttptestResponseRecorder *rw = httptest_new_recorder(heap_allocator());
    if (rw == NULL) {
        testing_t_fatalf_v(t, "out of memory");
        return;
    }
    volatile bool panicked = false;
    BURROW_TRY {
        httptest_response_recorder_write_header(rw, code);
    }
    BURROW_CATCH(p) {
        Arena ar;
        arena_init(&ar, NULL, 0);
        Str want =
            fmt_sprintf_v(arena_allocator(&ar), "invalid WriteHeader code %d", code);
        if (!str_eq(panic_text(p), want))
            testing_t_errorf_v(t, "panic = %q; want %q", panic_text(p), want);
        arena_free(&ar);
        panicked = true;
    }
    BURROW_TRY_END;
    httptest_response_recorder_free(rw);
    if (!panicked)
        testing_t_fatalf_v(t, "Expected a panic");
}

static void TestRecorderPanicsOnNonXXXStatusCode(TestingT *t) {
    static const Int bad_codes[] = {-100, 0, 99, 1000, 20000};
    for (size_t i = 0; i < sizeof bad_codes / sizeof bad_codes[0]; i++) {
        Arena ar;
        arena_init(&ar, NULL, 0);
        Str name = fmt_sprintf_v(arena_allocator(&ar), "Code=%d", bad_codes[i]);
        testing_t_run(
            t, name,
            BURROW_FN(TestingTFunc, bad_code_case, (void *)(intptr_t)bad_codes[i]));
        arena_free(&ar);
    }
}

/* ------------------------------------------------------------- NewRequest */

/* struct{ io.Reader }{strings.NewReader("foo")}: a reader whose type says
 * nothing about its length. */
static Int plain_read(void *self, Slice p, Error *err) {
    return strings_reader_read((StringsReader *)self, p, err);
}

static const IoReaderVT plain_vt = {NULL, plain_read};

typedef struct WantRequest {
    const char *method;
    const char *host;
    const char *url_scheme;
    const char *url_host;
    const char *url_path;
    const char *url_raw_path;
    int64_t content_length;
    const char *request_uri;
} WantRequest;

enum { BODY_NIL, BODY_STRINGS_READER, BODY_PLAIN, BODY_NO_BODY };

typedef struct NewRequestTest {
    const char *name;
    const char *method;
    const char *uri;
    int body;
    WantRequest want;
    const char *want_body;
} NewRequestTest;

static const NewRequestTest new_request_tests[] = {
    {"Empty method means GET",
     "",
     "/",
     BODY_NIL,
     {"GET", "example.com", "", "", "/", "", 0, "/"},
     ""},
    {"GET with full URL",
     "GET",
     "http://foo.com/path/%2f/bar/",
     BODY_NIL,
     {"GET", "foo.com", "http", "foo.com", "/path///bar/", "/path/%2f/bar/", 0,
      "http://foo.com/path/%2f/bar/"},
     ""},
    {"GET with full https URL",
     "GET",
     "https://foo.com/path/",
     BODY_NIL,
     {"GET", "foo.com", "https", "foo.com", "/path/", "", 0, "https://foo.com/path/"},
     ""},
    {"Post with known length",
     "POST",
     "/",
     BODY_STRINGS_READER,
     {"POST", "example.com", "", "", "/", "", 3, "/"},
     "foo"},
    {"Post with unknown length",
     "POST",
     "/",
     BODY_PLAIN,
     {"POST", "example.com", "", "", "/", "", -1, "/"},
     "foo"},
    {"Post with NoBody",
     "POST",
     "/",
     BODY_NO_BODY,
     {"POST", "example.com", "", "", "/", "", 0, "/"},
     ""},
    {"OPTIONS *",
     "OPTIONS",
     "*",
     BODY_NIL,
     {"OPTIONS", "example.com", "", "", "*", "", 0, "*"},
     ""},
};

static void check_str(TestingT *t, const char *field, Str got, const char *want) {
    if (!str_eq(got, cs(want)))
        testing_t_errorf_v(t, "Request.%s = %q; want %q", cs(field), got, cs(want));
}

/* The fields Go's reflect.DeepEqual compares, one at a time. */
static void check_request(TestingT *t, const HttpRequest *got, const WantRequest *w) {
    check_str(t, "Method", got->method, w->method);
    check_str(t, "Host", got->host, w->host);
    const Url *u = got->url;
    if (u == NULL) {
        testing_t_errorf_v(t, "Request.URL = nil");
    } else {
        check_str(t, "URL.Scheme", u->scheme, w->url_scheme);
        check_str(t, "URL.Host", u->host, w->url_host);
        check_str(t, "URL.Path", u->path, w->url_path);
        check_str(t, "URL.RawPath", u->raw_path, w->url_raw_path);
        if (u->user != NULL || u->opaque.len > 0 || u->raw_query.len > 0 ||
            u->fragment.len > 0 || u->raw_fragment.len > 0 || u->force_query ||
            u->omit_host)
            testing_t_errorf_v(t, "Request.URL has more than Scheme, Host and Path");
    }
    if (map_len(got->header) != 0)
        testing_t_errorf_v(t, "Request.Header has %d keys; want none",
                           map_len(got->header));
    check_str(t, "Proto", got->proto, "HTTP/1.1");
    if (got->proto_major != 1 || got->proto_minor != 1)
        testing_t_errorf_v(t, "Request.ProtoMajor, ProtoMinor = %d, %d; want 1, 1",
                           got->proto_major, got->proto_minor);
    if (got->content_length != w->content_length)
        testing_t_errorf_v(t, "Request.ContentLength = %d; want %d",
                           got->content_length, w->content_length);
    if (got->transfer_encoding.len != 0)
        testing_t_errorf_v(t, "Request.TransferEncoding has %d values; want nil",
                           got->transfer_encoding.len);
    if (got->close)
        testing_t_errorf_v(t, "Request.Close = true; want false");
    if (map_len(got->trailer) != 0)
        testing_t_errorf_v(t, "Request.Trailer has keys; want none");
    check_str(t, "RemoteAddr", got->remote_addr, "192.0.2.1:1234");
    check_str(t, "RequestURI", got->request_uri, w->request_uri);
    if (got->ctx.vt != context_background().vt ||
        got->ctx.data != context_background().data)
        testing_t_errorf_v(t, "Request.Context() is not context.Background()");
}

static void TestNewRequest(TestingT *t) {
    HttpRequest *got =
        httptest_new_request(heap_allocator(), S("GET"), S("/"), (IoReader){0});
    static const WantRequest want = {"GET", "example.com", "", "", "/", "", 0, "/"};
    check_request(t, got, &want);
    http_request_free(got);
}

static void new_request_case(void *env, TestingT *t) {
    const NewRequestTest *tt = (const NewRequestTest *)env;
    Alloc *a = heap_allocator();
    StringsReader sr;
    strings_reader_reset(&sr, S("foo"));
    IoReader body = {0};
    switch (tt->body) {
    case BODY_STRINGS_READER:
        body = strings_reader_as_io_reader(&sr);
        break;
    case BODY_PLAIN:
        body = (IoReader){&plain_vt, &sr};
        break;
    case BODY_NO_BODY:
        body = io_read_closer_as_io_reader(http_no_body);
        break;
    default:
        break;
    }
    HttpRequest *got = httptest_new_request_with_context(
        a, context_background(), cs(tt->method), cs(tt->uri), body);
    Arena ar;
    arena_init(&ar, NULL, 0);
    Error err = BURROW_NO_ERROR;
    Slice slurp =
        io_read_all(arena_allocator(&ar), io_read_closer_as_io_reader(got->body), &err);
    if (!BURROW_OK(err))
        testing_t_errorf_v(t, "ReadAll: %v", err);
    Str s = {(const Byte *)slurp.p, slurp.len};
    if (!str_eq(s, cs(tt->want_body)))
        testing_t_errorf_v(t, "Body = %q; want %q", s, cs(tt->want_body));
    arena_free(&ar);
    check_request(t, got, &tt->want);
    http_request_free(got);
}

static void TestNewRequestWithContext(TestingT *t) {
    for (size_t i = 0; i < sizeof new_request_tests / sizeof new_request_tests[0];
         i++) {
        const NewRequestTest *tt = &new_request_tests[i];
        testing_t_run(t, cs(tt->name),
                      BURROW_FN(TestingTFunc, new_request_case, (void *)(uintptr_t)tt));
    }
}

/* Out of the BURROW_TRY so the Str literals are not locals that a longjmp
 * could clobber. */
static void new_request_bad_target(Alloc *a) {
    (void)httptest_new_request(a, S("GET"), S("/a b"), (IoReader){0});
}

static bool is_new_request_panic(Str text) {
    return strings_has_prefix(text, S("invalid NewRequest arguments; "));
}

/* Not in Go's tests: the panic for arguments that make no request line. */
static void TestNewRequestPanics(TestingT *t) {
    Arena ar;
    arena_init(&ar, NULL, 0);
    volatile bool panicked = false;
    BURROW_TRY {
        new_request_bad_target(arena_allocator(&ar));
    }
    BURROW_CATCH(p) {
        if (!is_new_request_panic(panic_text(p)))
            testing_t_errorf_v(t, "panic = %q", panic_text(p));
        panicked = true;
    }
    BURROW_TRY_END;
    arena_free(&ar);
    if (!panicked)
        testing_t_errorf_v(t, "NewRequest with a space in the target did not panic");
}

/* ------------------------------------------------------------------ Server */

static void need_tcp(TestingT *t) {
#if !defined(HAVE_TCP)
    testing_t_skip_v(t, "TCP here needs the readiness poll FD");
#else
    (void)t;
#endif
}

static void serve_hello(void *env, HttpResponseWriter w, HttpRequest *r) {
    (void)env;
    (void)r;
    write_bytes(w, "hello");
}

static void serve_nothing(void *env, HttpResponseWriter w, HttpRequest *r) {
    (void)env;
    (void)w;
    (void)r;
}

/* newServers, the two that need no TLS. */
typedef HttptestServer *(*NewServerFunc)(HttpHandler h);

static HttptestServer *new_server(HttpHandler h) {
    return httptest_new_server(NULL, h);
}

static HttptestServer *new_server_unstarted(HttpHandler h) {
    HttptestServer *ts = httptest_new_unstarted_server(NULL, h);
    httptest_server_start(ts);
    return ts;
}

/* The body of a Get of url with c, or of the default client when c is NULL,
 * into a. */
static Str get_body(TestingT *t, Alloc *a, HttpClient *c, Str url) {
    Error err = BURROW_NO_ERROR;
    HttpResponse *res = c != NULL ? http_client_get(c, url, &err) : http_get(url, &err);
    if (res == NULL) {
        testing_t_errorf_v(t, "Get: %v", err);
        return BURROW_STR_EMPTY;
    }
    Slice b = io_read_all(a, io_read_closer_as_io_reader(res->body), &err);
    (void)res->body.vt->closer.close(res->body.data);
    http_response_free(res);
    if (BURROW_FAILED(err))
        testing_t_errorf_v(t, "ReadAll: %v", err);
    return str_from_bytes(b.p, b.len);
}

static void server_hello(void *env, TestingT *t) {
    need_tcp(t);
    HttpHandlerFunc f = BURROW_FN(HttpHandlerFunc, serve_hello, NULL);
    HttptestServer *ts = (*(NewServerFunc *)env)(http_handler_func_as_handler(&f));
    Arena ar;
    arena_init(&ar, NULL, 0);
    Str got = get_body(t, arena_allocator(&ar), NULL, ts->url);
    if (!str_eq(got, S("hello")))
        testing_t_errorf_v(t, "got %q, want hello", got);
    arena_free(&ar);
    httptest_server_free(ts);
}

/* Issue 12781. */
static void get_after_close(void *env, TestingT *t) {
    need_tcp(t);
    HttpHandlerFunc f = BURROW_FN(HttpHandlerFunc, serve_hello, NULL);
    HttptestServer *ts = (*(NewServerFunc *)env)(http_handler_func_as_handler(&f));
    Arena ar;
    arena_init(&ar, NULL, 0);
    Str got = get_body(t, arena_allocator(&ar), NULL, ts->url);
    if (!str_eq(got, S("hello")))
        testing_t_errorf_v(t, "got %q, want hello", got);

    httptest_server_close(ts);

    Error err = BURROW_NO_ERROR;
    HttpResponse *res = http_get(ts->url, &err);
    if (res != NULL) {
        testing_t_errorf_v(t, "Unexpected response after close: %s", res->status);
        (void)res->body.vt->closer.close(res->body.data);
        http_response_free(res);
    }
    arena_free(&ar);
    httptest_server_free(ts);
}

static void server_close_blocking(void *env, TestingT *t) {
    need_tcp(t);
    HttpHandlerFunc f = BURROW_FN(HttpHandlerFunc, serve_hello, NULL);
    HttptestServer *ts = (*(NewServerFunc *)env)(http_handler_func_as_handler(&f));
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    NetAddr la = ts->listener.vt->addr(ts->listener.data);
    Str addr = la.vt->string(la.data, a);
    Error err = BURROW_NO_ERROR;

    /* Keep one connection in StateNew (connected, but not sending anything). */
    NetConn cnew = net_dial(heap_allocator(), S("tcp"), addr, &err);
    CHECK(cnew.vt != NULL);

    /* Keep one connection in StateIdle (idle after a request). */
    NetConn cidle = net_dial(heap_allocator(), S("tcp"), addr, &err);
    CHECK(cidle.vt != NULL);
    if (cidle.vt != NULL) {
        io_write_string(net_conn_as_io_writer(cidle),
                        S("HEAD / HTTP/1.1\r\nHost: foo\r\n\r\n"), &err);
        BufioReader *br = bufio_new_reader(a, net_conn_as_io_reader(cidle));
        HttpResponse *res = http_read_response(a, br, NULL, &err);
        if (res == NULL)
            testing_t_errorf_v(t, "%v", err);
        http_response_free(res);
    }

    httptest_server_close(ts); /* test we don't hang here forever. */
    if (cnew.vt != NULL)
        net_conn_free(cnew);
    if (cidle.vt != NULL)
        net_conn_free(cidle);
    arena_free(&ar);
    httptest_server_free(ts);
}

static void serve_close_client_connections(void *env, HttpResponseWriter w,
                                           HttpRequest *r) {
    (void)w;
    (void)r;
    httptest_server_close_client_connections(*(HttptestServer **)env);
}

/* Issue 14290. */
static void server_close_client_connections(void *env, TestingT *t) {
    need_tcp(t);
    HttptestServer *s = NULL;
    HttpHandlerFunc f = BURROW_FN(HttpHandlerFunc, serve_close_client_connections, &s);
    HttptestServer *ts =
        httptest_new_unstarted_server(NULL, http_handler_func_as_handler(&f));
    s = ts;
    httptest_server_start(ts);
    (void)env;
    Error err = BURROW_NO_ERROR;
    HttpResponse *res = http_get(ts->url, &err);
    if (res != NULL) {
        testing_t_errorf_v(t, "Unexpected response: %s", res->status);
        (void)res->body.vt->closer.close(res->body.data);
        http_response_free(res);
    }
    httptest_server_free(ts);
}

/* That the client's transport is a transport. */
static void server_client_transport_type(void *env, TestingT *t) {
    need_tcp(t);
    HttpHandlerFunc f = BURROW_FN(HttpHandlerFunc, serve_nothing, NULL);
    HttptestServer *ts = (*(NewServerFunc *)env)(http_handler_func_as_handler(&f));
    HttpClient *client = httptest_server_client(ts);
    if (burrow__http_as_transport(client->transport) == NULL)
        testing_t_errorf_v(t, "got a round tripper that is not an HttpTransport");
    httptest_server_free(ts);
}

static void server_variant(void *env, TestingT *t) {
    (void)testing_t_run(t, S("Server"), BURROW_FN(TestingTFunc, server_hello, env));
    (void)testing_t_run(t, S("GetAfterClose"),
                        BURROW_FN(TestingTFunc, get_after_close, env));
    (void)testing_t_run(t, S("ServerCloseBlocking"),
                        BURROW_FN(TestingTFunc, server_close_blocking, env));
    (void)testing_t_run(t, S("ServerCloseClientConnections"),
                        BURROW_FN(TestingTFunc, server_close_client_connections, env));
    (void)testing_t_run(t, S("ServerClientTransportType"),
                        BURROW_FN(TestingTFunc, server_client_transport_type, env));
}

static void TestServer(TestingT *t) {
    static NewServerFunc news[2] = {new_server, new_server_unstarted};
    (void)testing_t_run(t, S("NewServer"),
                        BURROW_FN(TestingTFunc, server_variant, &news[0]));
    (void)testing_t_run(t, S("NewUnstartedServer"),
                        BURROW_FN(TestingTFunc, server_variant, &news[1]));
}

static void serve_requested_hostname(void *env, HttpResponseWriter w, HttpRequest *r) {
    (void)env;
    HttpHeader h = http_response_writer_header(w);
    (void)http_header_set(h, S("requested-hostname"),
                          str_clone(burrow__map_allocator(h), r->host));
}

static void client_example_com(void *env, TestingT *t) {
    need_tcp(t);
    Str host = *(const Str *)env;
    HttpHandlerFunc f = BURROW_FN(HttpHandlerFunc, serve_requested_hostname, NULL);
    HttptestServer *cst =
        httptest_new_unstarted_server(NULL, http_handler_func_as_handler(&f));
    httptest_server_start(cst);
    Arena ar;
    arena_init(&ar, NULL, 0);
    Error err = BURROW_NO_ERROR;
    HttpResponse *res =
        http_client_get(httptest_server_client(cst),
                        fmt_sprintf_v(arena_allocator(&ar), "http://%s", host), &err);
    if (res == NULL) {
        testing_t_errorf_v(t, "Failed to make request: %v", err);
    } else {
        Str got = http_header_get(res->header, S("requested-hostname"));
        if (!str_eq(got, host))
            testing_t_errorf_v(t, "Requested hostname mismatch\ngot: %q\nwant: %q", got,
                               host);
        (void)res->body.vt->closer.close(res->body.data);
        http_response_free(res);
    }
    arena_free(&ar);
    httptest_server_free(cst);
}

static void TestClientExampleCom(TestingT *t) {
    static Str example_hosts[2] = {BURROW_S_INIT("example.com"),
                                   BURROW_S_INIT("foo.example.com")};
    (void)testing_t_run(t, S("http example.com"),
                        BURROW_FN(TestingTFunc, client_example_com, &example_hosts[0]));
    (void)testing_t_run(t, S("http foo.example.com"),
                        BURROW_FN(TestingTFunc, client_example_com, &example_hosts[1]));
}

/* A listener that can only be closed, for a Server that was never started. */
static Error only_close(void *self) {
    (void)self;
    return BURROW_NO_ERROR;
}

static NetConn only_close_accept(void *self, Error *err) {
    (void)self;
    (void)err;
    panic_str(S("not implemented"));
    NetConn none = {NULL, NULL};
    return none;
}

static NetAddr only_close_addr(void *self) {
    (void)self;
    panic_str(S("not implemented"));
    NetAddr none = {NULL, NULL};
    return none;
}

static const NetListenerVT only_close_listener_vt = {
    .closer = {.self_type = NULL, .close = only_close},
    .accept = only_close_accept,
    .addr = only_close_addr,
};

static void TestServerZeroValueClose(TestingT *t) {
    (void)t;
    HttptestServer ts;
    memset(&ts, 0, sizeof ts);
    ts.listener.vt = &only_close_listener_vt;

    httptest_server_close(&ts); /* tests that it doesn't panic */
}

typedef struct Hijack {
    HttptestServer *ts;
    SyncWaitGroup hijacked; /* done once conn is set, or the hijack failed */
    NetConn conn;
    Error err;
} Hijack;

static void serve_hijack(void *env, HttpResponseWriter w, HttpRequest *r) {
    (void)r;
    Hijack *h = env;
    HttpResponseController rc = http_new_response_controller(w);
    BufioReadWriter buf;
    memset(&buf, 0, sizeof buf);
    h->conn = http_response_controller_hijack(&rc, &buf, &h->err);
    if (h->conn.vt != NULL) {
        bufio_reader_free(buf.reader);
        bufio_writer_free(buf.writer);
    }
    sync_wait_group_done(&h->hijacked);
}

/* Uses a client not associated with the Server. */
static void hijack_get(void *env) {
    Hijack *h = env;
    HttpClient c;
    memset(&c, 0, sizeof c);
    Error err = BURROW_NO_ERROR;
    HttpRequest *req = http_new_request(heap_allocator(), S("GET"), h->ts->url,
                                        (IoReader){NULL, NULL}, &err);
    if (req == NULL)
        return;
    HttpResponse *resp = http_client_do(&c, req, &err);
    if (resp != NULL) {
        (void)resp->body.vt->closer.close(resp->body.data);
        http_response_free(resp);
    }
    http_request_free(req);
}

/* Closes the connection and then tells the Server that it is closed. */
static void hijack_close_conn(void *env) {
    Hijack *h = env;
    (void)h->conn.vt->closer.close(h->conn.data);
    BURROW_CALLF(h->ts->config.conn_state, h->conn, HTTP_STATE_CLOSED);
}

static void hijack_close_server(void *env) {
    httptest_server_close(((Hijack *)env)->ts);
}

/* Issue 51799: test hijacking a connection and then closing it concurrently
 * with closing the server. */
static void TestCloseHijackedConnection(TestingT *t) {
    need_tcp(t);
    Hijack h;
    memset(&h, 0, sizeof h);
    sync_wait_group_add(&h.hijacked, 1);
    HttpHandlerFunc f = BURROW_FN(HttpHandlerFunc, serve_hijack, &h);
    h.ts = httptest_new_server(NULL, http_handler_func_as_handler(&f));

    SyncWaitGroup wg;
    memset(&wg, 0, sizeof wg);
    sync_wait_group_go(&wg, BURROW_FN(Func, hijack_get, &h));

    sync_wait_group_wait(&h.hijacked);
    if (h.conn.vt == NULL) {
        testing_t_errorf_v(t, "failed to hijack: %v", h.err);
    } else {
        sync_wait_group_go(&wg, BURROW_FN(Func, hijack_close_conn, &h));
    }
    sync_wait_group_go(&wg, BURROW_FN(Func, hijack_close_server, &h));
    sync_wait_group_wait(&wg);
    net_conn_free(h.conn);
    httptest_server_free(h.ts);
}

#define TESTS(X)                                                                       \
    X(TestRecorder)                                                                    \
    X(TestBodyNotAllowed)                                                              \
    X(TestParseContentLength)                                                          \
    X(TestRecorderPanicsOnNonXXXStatusCode)                                            \
    X(TestNewRequest)                                                                  \
    X(TestNewRequestWithContext)                                                       \
    X(TestNewRequestPanics)                                                            \
    X(TestServer)                                                                      \
    X(TestClientExampleCom) X(TestServerZeroValueClose) X(TestCloseHijackedConnection)

TESTING_MAIN(TESTS)
