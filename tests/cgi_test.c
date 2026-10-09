/* Derived from Go's src/net/http/cgi/child_test.go, host_test.go,
 * integration_test.go and cgi_main.go, the CGI tests from both sides.
 *
 * As in Go, the test binary is also the CGI program. A CgiHandler runs it with
 * SERVER_SOFTWARE set, and then it answers the request instead of running the
 * tests: as Go's Perl-like testCGI for the host tests, and through cgi_serve,
 * as a CGI-in-CGI, for the integration tests.
 *
 * There is no TLS field on a request yet, so TestRequestWithTLS checks only
 * the URL's scheme. TestCopyError counts running handlers itself, where Go
 * looks for ServeHTTP in a dump of every goroutine's stack.
 * Go source: go1.27.1.
 *
 * Copyright 2011 The Go Authors. All rights reserved.
 * Copyright 2026 The burrow Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style licence that can be found
 * in the LICENSE file. */

#include "check.h"

#include "../src/net/cgi_internal.h"

#include "burrow/bufio.h"
#include "burrow/bytes.h"
#include "burrow/core.h"
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
#include "burrow/net/http/cgi.h"
#include "burrow/net/http/httptest.h"
#include "burrow/net/url.h"
#include "burrow/netpoll.h"
#include "burrow/os.h"
#include "burrow/panic.h"
#include "burrow/path.h"
#include "burrow/path/filepath.h"
#include "burrow/slice.h"
#include "burrow/sort.h"
#include "burrow/strings.h"
#include "burrow/sync/atomic.h"
#include "burrow/time.h"

#include <stdint.h>
#include <string.h>

#define S BURROW_S

#if defined(BURROW_NETPOLL_READINESS) && !defined(BURROW_OS_WASI)
#define HAVE_TCP 1
#endif

static void need_tcp(TestingT *t) {
#if !defined(HAVE_TCP)
    testing_t_skip_v(t, "TCP here needs the readiness poll FD");
#else
    (void)t;
#endif
}

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

static Str cs(const char *s) {
    return str_from_cstr(s);
}

/* m's keys, sorted, for a Map with Str keys. */
static Slice sorted_keys(Alloc *a, Map *m) {
    Slice keys = slice_make(a, TYPE_STRING, 0, map_len(m));
    const void *kp;
    void *vp;
    for (MapIter it = map_iter(m); map_next(&it, &kp, &vp);)
        keys = BURROW_APPEND(Str, a, keys, *(const Str *)kp);
    sort_strings(keys);
    return keys;
}

/* envMap, from cgi's child.go. */
static Map *env_map(Alloc *a, Slice env) {
    Map *m = map_make(a, TYPE_STRING, TYPE_STRING, env.len);
    for (Int i = 0; i < env.len; i++) {
        Str v = {0};
        bool ok = false;
        Str k = strings_cut(BURROW_AT(Str, env, i), S("="), &v, &ok);
        if (ok)
            (void)map_set(m, &k, &v);
    }
    return m;
}

/* ----------------------------------------------------- the CGI program */

/* Go's fmt.Printf goes straight to the descriptor, and the program leaves
 * through os_exit, which flushes nothing, so the program writes to os_stdout
 * itself rather than through stdio. */
#define out_printf(...)                                                                \
    (void)fmt_fprintf_v(os_file_as_io_writer(os_stdout), __VA_ARGS__)

/* testCGI is a CGI program translated from a Perl program to complete
 * host_test. test cases in host_test should be provided by testCGI. */
static void test_cgi(Alloc *a) {
    Error err = BURROW_NO_ERROR;
    HttpRequest *req = cgi_request(a, &err);
    if (req == NULL)
        panic(BURROW_ANY(TYPE_ERROR, &err));

    err = http_request_parse_form(req);
    if (BURROW_FAILED(err))
        panic(BURROW_ANY(TYPE_ERROR, &err));

    UrlValues params = req->form;
    Str loc = url_values_get(params, S("loc"));
    if (loc.len > 0) {
        out_printf("Location: %s\r\n\r\n", loc);
        return;
    }

    out_printf("Content-Type: text/html\r\n");
    out_printf("X-CGI-Pid: %d\r\n", os_getpid());
    out_printf("X-Test-Header: X-Test-Value\r\n");
    out_printf("\r\n");

    if (url_values_get(params, S("writestderr")).len > 0)
        (void)fmt_fprintf_v(os_file_as_io_writer(os_stderr), "Hello, stderr!\n");

    if (url_values_get(params, S("bigresponse")).len > 0) {
        /* 17 MB, for OS X: golang.org/issue/4958 */
        Str line = strings_repeat(a, S("A"), 1024);
        for (Int i = 0; i < (Int)17 * 1024; i++)
            out_printf("%s\r\n", line);
        return;
    }

    out_printf("test=Hello CGI\r\n");

    Slice keys = sorted_keys(a, params);
    for (Int i = 0; i < keys.len; i++) {
        Str key = BURROW_AT(Str, keys, i);
        out_printf("param-%s=%s\r\n", key, url_values_get(params, key));
    }

    Map *envs = env_map(a, os_environ(a));
    keys = sorted_keys(a, envs);
    for (Int i = 0; i < keys.len; i++) {
        Str key = BURROW_AT(Str, keys, i);
        out_printf("env-%s=%s\r\n", key, *(Str *)map_get(envs, &key));
    }

    Str cwd = os_getwd(a, &err);
    out_printf("cwd=%s\r\n", cwd);
    http_request_free(req);
}

static Int never_ending_read(void *self, Slice p, Error *err) {
    (void)err;
    memset(p.p, *(const Byte *)self, (size_t)p.len);
    return p.len;
}

static const IoReaderVT never_ending_vt = {NULL, never_ending_read};

static void child_serve(void *env, HttpResponseWriter rw, HttpRequest *req) {
    Alloc *a = (Alloc *)env;
    IoWriter w = http_response_writer_as_io_writer(rw);
    if (str_eq(http_request_form_value(req, S("nil-request-body")), S("1"))) {
        bool nil_body = req->body.vt == NULL;
        (void)fmt_fprintf_v(w, "nil-request-body=%v\n", nil_body);
        return;
    }
    (void)http_header_set(http_response_writer_header(rw), S("X-Test-Header"),
                          S("X-Test-Value"));
    (void)http_request_parse_form(req);
    if (str_eq(http_request_form_value(req, S("no-body")), S("1")))
        return;
    Str exact = S("exact-body");
    Slice *eb = (Slice *)map_get(req->form, &exact);
    if (eb != NULL) {
        (void)io_write_string(w, BURROW_AT(Str, *eb, 0), NULL);
        return;
    }
    if (str_eq(http_request_form_value(req, S("write-forever")), S("1"))) {
        static const Byte a_byte = 'a';
        IoReader never = {&never_ending_vt, (void *)(uintptr_t)&a_byte};
        (void)io_copy(a, w, never, NULL);
        for (;;)
            time_sleep(5 * TIME_SECOND); /* hang forever, until killed */
    }
    (void)fmt_fprintf_v(w, "test=Hello CGI-in-CGI\n");
    const void *kp;
    void *vp;
    for (MapIter it = map_iter(req->form); map_next(&it, &kp, &vp);) {
        const Slice *vv = (const Slice *)vp;
        for (Int i = 0; i < vv->len; i++)
            (void)fmt_fprintf_v(w, "param-%s=%s\n", *(const Str *)kp,
                                BURROW_AT(Str, *vv, i));
    }
    Slice environ = os_environ(a);
    for (Int i = 0; i < environ.len; i++)
        (void)fmt_fprintf_v(w, "env-%s\n", BURROW_AT(Str, environ, i));
}

/* childCGIProcess is used by integration_test to complete unit tests. */
static void child_cgi_process(Alloc *a) {
    if (os_getenv(a, S("REQUEST_METHOD")).len == 0) {
        /* Not in a CGI environment; skipping test. */
        return;
    }
    Str uri = os_getenv(a, S("REQUEST_URI"));
    if (str_eq(uri, S("/immediate-disconnect"))) {
        os_exit(0);
    } else if (str_eq(uri, S("/no-content-type"))) {
        out_printf("Content-Length: 6\n\nHello\n");
        os_exit(0);
    } else if (str_eq(uri, S("/empty-headers"))) {
        out_printf("\nHello");
        os_exit(0);
    }
    HttpHandlerFunc f = BURROW_FN(HttpHandlerFunc, child_serve, a);
    (void)cgi_serve(http_handler_func_as_handler(&f));
    os_exit(0);
}

static void cgi_main(void) {
    ARENA_BEGIN;
    Str p =
        path_join_v(a, 2, os_getenv(a, S("SCRIPT_NAME")), os_getenv(a, S("PATH_INFO")));
    if (str_eq(p, S("/bar")) || str_eq(p, S("/test.cgi")) ||
        str_eq(p, S("/myscript/bar")) || str_eq(p, S("/test.cgi/extrapath"))) {
        test_cgi(a);
        ARENA_END;
        return;
    }
    child_cgi_process(a);
    ARENA_END;
}

/* ------------------------------------------------------------ the host */

/* os.Args[0], which Go's test runner makes absolute. The make rule runs the
 * test as ./build/tests/cgi_test, and a handler runs the program from its own
 * directory, where that path names nothing, so this makes it absolute here. */
static Str self_path(Alloc *a) {
    Slice args = os_args();
    Error err = BURROW_NO_ERROR;
    Str abs = filepath_abs(a, BURROW_AT(Str, args, 0), &err);
    return BURROW_OK(err) ? abs : BURROW_AT(Str, args, 0);
}

static HttpRequest *new_request(Alloc *a, Str httpreq) {
    BufioReader *buf = bufio_new_reader(
        a, strings_reader_as_io_reader(strings_new_reader(a, httpreq)));
    Error err = BURROW_NO_ERROR;
    HttpRequest *req = http_read_request(a, buf, &err);
    if (req == NULL)
        panic_str(fmt_sprintf_v(a, "cgi: bogus http request in test: %s", httpreq));
    req->remote_addr = S("1.2.3.4:1234");
    return req;
}

typedef struct Want {
    Str k, v;
} Want;

/* runResponseChecks. The lines of the body that are "key=value", as a Map,
 * with "_body" the whole body. NULL after a failure. */
static Map *run_response_checks(TestingT *t, Alloc *a, HttptestResponseRecorder *rw,
                                const Want *want, size_t nwant) {
    /* Make a map to hold the test map that the CGI returns. */
    Map *m = map_make(a, TYPE_STRING, TYPE_STRING, 0);
    Slice b = bytes_buffer_bytes(rw->body);
    Str body = str_from_bytes(b.p, b.len);
    Str key = S("_body");
    (void)map_set(m, &key, &body);
    Int lines_read = 0;
    for (Int nl; (nl = strings_index_byte(body, '\n')) >= 0;) {
        Str line = str_from_bytes(body.p, nl + 1);
        body = str_from_bytes(body.p + nl + 1, body.len - nl - 1);
        lines_read++;
        Str trimmed_line = strings_trim_right(line, S("\r\n"));
        Str v = {0};
        bool ok = false;
        Str k = strings_cut(trimmed_line, S("="), &v, &ok);
        if (!ok) {
            testing_t_fatalf_v(t, "Unexpected response from invalid line number %v: %q",
                               lines_read, line);
            return NULL;
        }
        (void)map_set(m, &k, &v);
    }

    for (size_t i = 0; i < nwant; i++) {
        Str *gp = (Str *)map_get(m, &want[i].k);
        Str got = gp != NULL ? *gp : BURROW_STR_EMPTY;
        if (str_eq(want[i].k, S("cwd"))) {
            /* For Windows. golang.org/issue/4645. */
            Error e1 = BURROW_NO_ERROR, e2 = BURROW_NO_ERROR;
            OsFileInfo fi1 = os_stat(a, got, &e1);
            OsFileInfo fi2 = os_stat(a, want[i].v, &e2);
            if (BURROW_OK(e1) && BURROW_OK(e2) && os_same_file(fi1, fi2))
                got = want[i].v;
        }
        if (!str_eq(got, want[i].v))
            testing_t_errorf_v(t, "for key %q got %q; expected %q", want[i].k, got,
                               want[i].v);
    }
    return m;
}

/* runCgiTest. The recorder, and in *info, when it isn't NULL, what
 * run_response_checks found. */
static HttptestResponseRecorder *run_cgi_test(TestingT *t, Alloc *a, CgiHandler *h,
                                              Str httpreq, const Want *want,
                                              size_t nwant, Map **info) {
    HttptestResponseRecorder *rw = httptest_new_recorder(a);
    HttpRequest *req = new_request(a, httpreq);
    cgi_handler_serve_http(h, httptest_response_recorder_as_response_writer(rw), req);
    Map *m = run_response_checks(t, a, rw, want, nwant);
    if (info != NULL)
        *info = m;
    http_request_free(req);
    return rw;
}

#define NWANT(w) (sizeof(w) / sizeof(w)[0])

static Str header_get(HttptestResponseRecorder *rw, const char *key) {
    return http_header_get(httptest_response_recorder_header(rw), cs(key));
}

/* ------------------------------------------------------------ child_test.go */

/* A Map of Str to Str from pairs of C strings. */
static Map *params_of(Alloc *a, const char *const (*kv)[2], size_t n) {
    Map *m = map_make(a, TYPE_STRING, TYPE_STRING, 0);
    for (size_t i = 0; i < n; i++) {
        Str k = cs(kv[i][0]), v = cs(kv[i][1]);
        (void)map_set(m, &k, &v);
    }
    return m;
}

/* The "expected %s %q; got %q" checks of Go's TestRequest. */
static void expect_str(TestingT *t, const char *what, Str e, Str g) {
    if (!str_eq(e, g))
        testing_t_errorf_v(t, "expected %s %q; got %q", cs(what), e, g);
}

static void TestRequest(TestingT *t) {
    static const char *const env[][2] = {
        {"SERVER_PROTOCOL", "HTTP/1.1"}, {"REQUEST_METHOD", "GET"},
        {"HTTP_HOST", "example.com"},    {"HTTP_REFERER", "elsewhere"},
        {"HTTP_USER_AGENT", "goclient"}, {"HTTP_FOO_BAR", "baz"},
        {"REQUEST_URI", "/path?a=b"},    {"CONTENT_LENGTH", "123"},
        {"CONTENT_TYPE", "text/xml"},    {"REMOTE_ADDR", "5.6.7.8"},
        {"REMOTE_PORT", "54321"},
    };
    ARENA_BEGIN;
    Error err = BURROW_NO_ERROR;
    HttpRequest *req =
        cgi_request_from_map(a, params_of(a, env, sizeof env / sizeof env[0]), &err);
    if (req == NULL) {
        ARENA_END;
        FATALF("RequestFromMap: %v", err);
    }
    if (req->header == NULL) {
        http_request_free(req);
        ARENA_END;
        FATALF("unexpected nil Header");
    }
    expect_str(t, "UserAgent", S("goclient"), http_request_user_agent(req));
    expect_str(t, "Method", S("GET"), req->method);
    expect_str(t, "Content-Type", S("text/xml"),
               http_header_get(req->header, S("Content-Type")));
    if (req->content_length != 123)
        testing_t_errorf_v(t, "expected ContentLength %d; got %d", (int64_t)123,
                           req->content_length);
    expect_str(t, "Referer", S("elsewhere"), http_request_referer(req));
    expect_str(t, "Foo-Bar", S("baz"), http_header_get(req->header, S("Foo-Bar")));
    expect_str(t, "URL", S("http://example.com/path?a=b"), url_string(req->url, a));
    expect_str(t, "FormValue(a)", S("b"), http_request_form_value(req, S("a")));
    if (req->trailer == NULL)
        testing_t_errorf_v(t, "unexpected nil Trailer");
    /* Go checks that req.TLS is nil here. There is no TLS field yet. */
    Str e = S("5.6.7.8:54321"), g = req->remote_addr;
    if (!str_eq(e, g))
        testing_t_errorf_v(t, "RemoteAddr: got %q; want %q", g, e);
    http_request_free(req);
    ARENA_END;
}

static void TestRequestWithTLS(TestingT *t) {
    static const char *const env[][2] = {
        {"SERVER_PROTOCOL", "HTTP/1.1"},
        {"REQUEST_METHOD", "GET"},
        {"HTTP_HOST", "example.com"},
        {"HTTP_REFERER", "elsewhere"},
        {"REQUEST_URI", "/path?a=b"},
        {"CONTENT_TYPE", "text/xml"},
        {"HTTPS", "1"},
        {"REMOTE_ADDR", "5.6.7.8"},
    };
    ARENA_BEGIN;
    Error err = BURROW_NO_ERROR;
    HttpRequest *req =
        cgi_request_from_map(a, params_of(a, env, sizeof env / sizeof env[0]), &err);
    if (req == NULL) {
        ARENA_END;
        FATALF("RequestFromMap: %v", err);
    }
    Str g = url_string(req->url, a), e = S("https://example.com/path?a=b");
    if (!str_eq(e, g))
        testing_t_errorf_v(t, "expected URL %q; got %q", e, g);
    /* Go checks that req.TLS is set here. There is no TLS field yet. */
    http_request_free(req);
    ARENA_END;
}

static void TestRequestWithoutHost(TestingT *t) {
    static const char *const env[][2] = {
        {"SERVER_PROTOCOL", "HTTP/1.1"}, {"HTTP_HOST", ""},
        {"REQUEST_METHOD", "GET"},       {"REQUEST_URI", "/path?a=b"},
        {"CONTENT_LENGTH", "123"},
    };
    ARENA_BEGIN;
    Error err = BURROW_NO_ERROR;
    HttpRequest *req =
        cgi_request_from_map(a, params_of(a, env, sizeof env / sizeof env[0]), &err);
    if (req == NULL) {
        ARENA_END;
        FATALF("RequestFromMap: %v", err);
    }
    if (req->url == NULL) {
        http_request_free(req);
        ARENA_END;
        FATALF("unexpected nil URL");
    }
    Str g = url_string(req->url, a), e = S("/path?a=b");
    if (!str_eq(e, g))
        testing_t_errorf_v(t, "URL = %q; want %q", g, e);
    http_request_free(req);
    ARENA_END;
}

static void TestRequestWithoutRequestURI(TestingT *t) {
    static const char *const env[][2] = {
        {"SERVER_PROTOCOL", "HTTP/1.1"}, {"HTTP_HOST", "example.com"},
        {"REQUEST_METHOD", "GET"},       {"SCRIPT_NAME", "/dir/scriptname"},
        {"PATH_INFO", "/p1/p2"},         {"QUERY_STRING", "a=1&b=2"},
        {"CONTENT_LENGTH", "123"},
    };
    ARENA_BEGIN;
    Error err = BURROW_NO_ERROR;
    HttpRequest *req =
        cgi_request_from_map(a, params_of(a, env, sizeof env / sizeof env[0]), &err);
    if (req == NULL) {
        ARENA_END;
        FATALF("RequestFromMap: %v", err);
    }
    if (req->url == NULL) {
        http_request_free(req);
        ARENA_END;
        FATALF("unexpected nil URL");
    }
    Str g = url_string(req->url, a),
        e = S("http://example.com/dir/scriptname/p1/p2?a=1&b=2");
    if (!str_eq(e, g))
        testing_t_errorf_v(t, "URL = %q; want %q", g, e);
    http_request_free(req);
    ARENA_END;
}

static void TestRequestWithoutRemotePort(TestingT *t) {
    static const char *const env[][2] = {
        {"SERVER_PROTOCOL", "HTTP/1.1"}, {"HTTP_HOST", "example.com"},
        {"REQUEST_METHOD", "GET"},       {"REQUEST_URI", "/path?a=b"},
        {"CONTENT_LENGTH", "123"},       {"REMOTE_ADDR", "5.6.7.8"},
    };
    ARENA_BEGIN;
    Error err = BURROW_NO_ERROR;
    HttpRequest *req =
        cgi_request_from_map(a, params_of(a, env, sizeof env / sizeof env[0]), &err);
    if (req == NULL) {
        ARENA_END;
        FATALF("RequestFromMap: %v", err);
    }
    Str e = S("5.6.7.8:0"), g = req->remote_addr;
    if (!str_eq(e, g))
        testing_t_errorf_v(t, "RemoteAddr: got %q; want %q", g, e);
    http_request_free(req);
    ARENA_END;
}

/* CGI Specification RFC 3875 - section 4.1.16
 * INCLUDED value for SERVER_PROTOCOL must be treated as an HTTP/1.0 request */
static void TestIncludedServerProtocol(TestingT *t) {
    static const char *const env[][2] = {
        {"REQUEST_METHOD", "GET"},
        {"SERVER_PROTOCOL", "INCLUDED"},
    };
    ARENA_BEGIN;
    Error err = BURROW_NO_ERROR;
    HttpRequest *req =
        cgi_request_from_map(a, params_of(a, env, sizeof env / sizeof env[0]), &err);
    if (req == NULL) {
        ARENA_END;
        FATALF("expected INCLUDED to be treated as HTTP/1.0 request");
    }
    if (!str_eq(req->proto, S("INCLUDED")))
        testing_t_errorf_v(t, "unexpected change to SERVER_PROTOCOL");
    if (req->proto_major != 1)
        testing_t_errorf_v(t, "ProtoMajor: got %d, want %d", req->proto_major, 1);
    if (req->proto_minor != 0)
        testing_t_errorf_v(t, "ProtoMinor: got %d, want %d", req->proto_minor, 0);
    http_request_free(req);
    ARENA_END;
}

typedef struct ContentTypeTest {
    const char *name;
    Str body;
    const char *want_ct;
} ContentTypeTest;

static ContentTypeTest content_type_tests[4];

static void content_type_tests_init(Alloc *a) {
    static const Byte jpg_head[] = {0xFF, 0xD8, 0xFF};
    content_type_tests[0] =
        (ContentTypeTest){"no body", {0}, "text/plain; charset=utf-8"};
    content_type_tests[1] =
        (ContentTypeTest){"html",
                          S("<html><head><title>test page</title></head><body>This is "
                            "a body</body></html>"),
                          "text/html; charset=utf-8"};
    content_type_tests[2] = (ContentTypeTest){
        "text", strings_repeat(a, S("gopher"), 86), "text/plain; charset=utf-8"};
    content_type_tests[3] =
        (ContentTypeTest){"jpg",
                          fmt_sprintf_v(a, "%s%s", str_from_bytes(jpg_head, 3),
                                        strings_repeat(a, S("B"), 1024)),
                          "image/jpeg"};
}

static void response_case(void *env, TestingT *t) {
    const ContentTypeTest *tt = (const ContentTypeTest *)env;
    ARENA_BEGIN;
    BytesBuffer *buf = bytes_new_buffer(a, (Slice){0});
    HttpRequest *req =
        httptest_new_request(a, S("GET"), S("/"), (IoReader){NULL, NULL});
    burrow__CgiResponse *resp =
        burrow__cgi_new_response(a, req, bytes_buffer_as_io_writer(buf));
    HttpResponseWriter w = burrow__cgi_response_writer(resp);
    Error err = BURROW_NO_ERROR;
    Int n = http_response_writer_write(w, slice_from_str(a, tt->body), &err);
    if (BURROW_FAILED(err))
        testing_t_errorf_v(t, "Write: unexpected %v", err);
    if (n != tt->body.len)
        testing_t_errorf_v(t, "reported short Write: got %v want %v", n, tt->body.len);
    burrow__cgi_write_cgi_header(resp, (Slice){0});
    (void)w.vt->flush(w.data);
    Str got = http_header_get(http_response_writer_header(w), S("Content-Type"));
    if (!str_eq(got, cs(tt->want_ct)))
        testing_t_errorf_v(t, "wrong content-type: got %q, want %q", got,
                           cs(tt->want_ct));
    Slice b = bytes_buffer_bytes(buf);
    if (!strings_has_suffix(str_from_bytes(b.p, b.len), tt->body))
        testing_t_errorf_v(t, "body was not correctly written");
    http_request_free(req);
    ARENA_END;
}

static void TestResponse(TestingT *t) {
    ARENA_BEGIN;
    content_type_tests_init(a);
    for (size_t i = 0; i < sizeof content_type_tests / sizeof content_type_tests[0];
         i++)
        testing_t_run(t, cs(content_type_tests[i].name),
                      BURROW_FN(TestingTFunc, response_case, &content_type_tests[i]));
    ARENA_END;
}

/* ------------------------------------------------------------- host_test.go */

static void TestCGIBasicGet(TestingT *t) {
    SKIP_WITHOUT_EXEC(t);
    ARENA_BEGIN;
    CgiHandler h = {0};
    h.path = self_path(a);
    h.root = S("/test.cgi");
    Want want[] = {
        {S("test"), S("Hello CGI")},
        {S("param-a"), S("b")},
        {S("param-foo"), S("bar")},
        {S("env-GATEWAY_INTERFACE"), S("CGI/1.1")},
        {S("env-HTTP_HOST"), S("example.com:80")},
        {S("env-PATH_INFO"), S("")},
        {S("env-QUERY_STRING"), S("foo=bar&a=b")},
        {S("env-REMOTE_ADDR"), S("1.2.3.4")},
        {S("env-REMOTE_HOST"), S("1.2.3.4")},
        {S("env-REMOTE_PORT"), S("1234")},
        {S("env-REQUEST_METHOD"), S("GET")},
        {S("env-REQUEST_URI"), S("/test.cgi?foo=bar&a=b")},
        {S("env-SCRIPT_FILENAME"), self_path(a)},
        {S("env-SCRIPT_NAME"), S("/test.cgi")},
        {S("env-SERVER_NAME"), S("example.com")},
        {S("env-SERVER_PORT"), S("80")},
        {S("env-SERVER_SOFTWARE"), S("go")},
    };
    HttptestResponseRecorder *replay = run_cgi_test(
        t, a, &h, S("GET /test.cgi?foo=bar&a=b HTTP/1.0\nHost: example.com:80\n\n"),
        want, NWANT(want), NULL);

    Str expected = S("text/html"), got = header_get(replay, "Content-Type");
    if (!str_eq(got, expected))
        testing_t_errorf_v(t, "got a Content-Type of %q; expected %q", got, expected);
    expected = S("X-Test-Value");
    got = header_get(replay, "X-Test-Header");
    if (!str_eq(got, expected))
        testing_t_errorf_v(t, "got a X-Test-Header of %q; expected %q", got, expected);
    httptest_response_recorder_free(replay);
    ARENA_END;
}

static void TestCGIEnvIPv6(TestingT *t) {
    SKIP_WITHOUT_EXEC(t);
    ARENA_BEGIN;
    CgiHandler h = {0};
    h.path = self_path(a);
    h.root = S("/test.cgi");
    Want want[] = {
        {S("test"), S("Hello CGI")},
        {S("param-a"), S("b")},
        {S("param-foo"), S("bar")},
        {S("env-GATEWAY_INTERFACE"), S("CGI/1.1")},
        {S("env-HTTP_HOST"), S("example.com")},
        {S("env-PATH_INFO"), S("")},
        {S("env-QUERY_STRING"), S("foo=bar&a=b")},
        {S("env-REMOTE_ADDR"), S("2000::3000")},
        {S("env-REMOTE_HOST"), S("2000::3000")},
        {S("env-REMOTE_PORT"), S("12345")},
        {S("env-REQUEST_METHOD"), S("GET")},
        {S("env-REQUEST_URI"), S("/test.cgi?foo=bar&a=b")},
        {S("env-SCRIPT_FILENAME"), self_path(a)},
        {S("env-SCRIPT_NAME"), S("/test.cgi")},
        {S("env-SERVER_NAME"), S("example.com")},
        {S("env-SERVER_PORT"), S("80")},
        {S("env-SERVER_SOFTWARE"), S("go")},
    };

    HttptestResponseRecorder *rw = httptest_new_recorder(a);
    HttpRequest *req =
        new_request(a, S("GET /test.cgi?foo=bar&a=b HTTP/1.0\nHost: example.com\n\n"));
    req->remote_addr = S("[2000::3000]:12345");
    cgi_handler_serve_http(&h, httptest_response_recorder_as_response_writer(rw), req);
    (void)run_response_checks(t, a, rw, want, NWANT(want));
    http_request_free(req);
    httptest_response_recorder_free(rw);
    ARENA_END;
}

static void TestCGIBasicGetAbsPath(TestingT *t) {
    ARENA_BEGIN;
    Error err = BURROW_NO_ERROR;
    Str abs_path = filepath_abs(a, self_path(a), &err);
    if (BURROW_FAILED(err)) {
        ARENA_END;
        FATALF("%v", err);
    }
    SKIP_WITHOUT_EXEC(t);
    CgiHandler h = {0};
    h.path = abs_path;
    h.root = S("/test.cgi");
    Want want[] = {
        {S("env-REQUEST_URI"), S("/test.cgi?foo=bar&a=b")},
        {S("env-SCRIPT_FILENAME"), abs_path},
        {S("env-SCRIPT_NAME"), S("/test.cgi")},
    };
    httptest_response_recorder_free(run_cgi_test(
        t, a, &h, S("GET /test.cgi?foo=bar&a=b HTTP/1.0\nHost: example.com\n\n"), want,
        NWANT(want), NULL));
    ARENA_END;
}

static void TestPathInfo(TestingT *t) {
    SKIP_WITHOUT_EXEC(t);
    ARENA_BEGIN;
    CgiHandler h = {0};
    h.path = self_path(a);
    h.root = S("/test.cgi");
    Want want[] = {
        {S("param-a"), S("b")},
        {S("env-PATH_INFO"), S("/extrapath")},
        {S("env-QUERY_STRING"), S("a=b")},
        {S("env-REQUEST_URI"), S("/test.cgi/extrapath?a=b")},
        {S("env-SCRIPT_FILENAME"), self_path(a)},
        {S("env-SCRIPT_NAME"), S("/test.cgi")},
    };
    httptest_response_recorder_free(run_cgi_test(
        t, a, &h, S("GET /test.cgi/extrapath?a=b HTTP/1.0\nHost: example.com\n\n"),
        want, NWANT(want), NULL));
    ARENA_END;
}

static void TestPathInfoDirRoot(TestingT *t) {
    SKIP_WITHOUT_EXEC(t);
    ARENA_BEGIN;
    CgiHandler h = {0};
    h.path = self_path(a);
    h.root = S("/myscript//");
    Want want[] = {
        {S("env-PATH_INFO"), S("/bar")},
        {S("env-QUERY_STRING"), S("a=b")},
        {S("env-REQUEST_URI"), S("/myscript/bar?a=b")},
        {S("env-SCRIPT_FILENAME"), self_path(a)},
        {S("env-SCRIPT_NAME"), S("/myscript")},
    };
    httptest_response_recorder_free(run_cgi_test(
        t, a, &h, S("GET /myscript/bar?a=b HTTP/1.0\nHost: example.com\n\n"), want,
        NWANT(want), NULL));
    ARENA_END;
}

static void TestDupHeaders(TestingT *t) {
    SKIP_WITHOUT_EXEC(t);
    ARENA_BEGIN;
    CgiHandler h = {0};
    h.path = self_path(a);
    Want want[] = {
        {S("env-REQUEST_URI"), S("/myscript/bar?a=b")},
        {S("env-SCRIPT_FILENAME"), self_path(a)},
        {S("env-HTTP_COOKIE"), S("nom=NOM; yum=YUM")},
        {S("env-HTTP_X_FOO"), S("val1, val2")},
    };
    httptest_response_recorder_free(run_cgi_test(t, a, &h,
                                                 S("GET /myscript/bar?a=b HTTP/1.0\n"
                                                   "Cookie: nom=NOM\n"
                                                   "Cookie: yum=YUM\n"
                                                   "X-Foo: val1\n"
                                                   "X-Foo: val2\n"
                                                   "Host: example.com\n\n"),
                                                 want, NWANT(want), NULL));
    ARENA_END;
}

/* Issue 16405: CGI+http.Transport differing uses of HTTP_PROXY.
 * Verify we don't set the HTTP_PROXY environment variable.
 * Hope nobody was depending on it. It's not a known header, though. */
static void TestDropProxyHeader(TestingT *t) {
    SKIP_WITHOUT_EXEC(t);
    ARENA_BEGIN;
    CgiHandler h = {0};
    h.path = self_path(a);
    Want want[] = {
        {S("env-REQUEST_URI"), S("/myscript/bar?a=b")},
        {S("env-SCRIPT_FILENAME"), self_path(a)},
        {S("env-HTTP_X_FOO"), S("a")},
    };
    Map *req_info = NULL;
    httptest_response_recorder_free(run_cgi_test(t, a, &h,
                                                 S("GET /myscript/bar?a=b HTTP/1.0\n"
                                                   "X-Foo: a\n"
                                                   "Proxy: should_be_stripped\n"
                                                   "Host: example.com\n\n"),
                                                 want, NWANT(want), &req_info));
    Str key = S("env-HTTP_PROXY");
    Str *v = req_info != NULL ? (Str *)map_get(req_info, &key) : NULL;
    if (v != NULL)
        testing_t_errorf_v(t, "HTTP_PROXY = %q; should be absent", *v);
    ARENA_END;
}

static void TestPathInfoNoRoot(TestingT *t) {
    SKIP_WITHOUT_EXEC(t);
    ARENA_BEGIN;
    CgiHandler h = {0};
    h.path = self_path(a);
    h.root = S("");
    Want want[] = {
        {S("env-PATH_INFO"), S("/bar")},       {S("env-QUERY_STRING"), S("a=b")},
        {S("env-REQUEST_URI"), S("/bar?a=b")}, {S("env-SCRIPT_FILENAME"), self_path(a)},
        {S("env-SCRIPT_NAME"), S("")},
    };
    httptest_response_recorder_free(
        run_cgi_test(t, a, &h, S("GET /bar?a=b HTTP/1.0\nHost: example.com\n\n"), want,
                     NWANT(want), NULL));
    ARENA_END;
}

static void TestCGIBasicPost(TestingT *t) {
    SKIP_WITHOUT_EXEC(t);
    ARENA_BEGIN;
    Str post_req = S("POST /test.cgi?a=b HTTP/1.0\n"
                     "Host: example.com\n"
                     "Content-Type: application/x-www-form-urlencoded\n"
                     "Content-Length: 15\n"
                     "\n"
                     "postfoo=postbar");
    CgiHandler h = {0};
    h.path = self_path(a);
    h.root = S("/test.cgi");
    Want want[] = {
        {S("test"), S("Hello CGI")},
        {S("param-postfoo"), S("postbar")},
        {S("env-REQUEST_METHOD"), S("POST")},
        {S("env-CONTENT_LENGTH"), S("15")},
        {S("env-REQUEST_URI"), S("/test.cgi?a=b")},
    };
    httptest_response_recorder_free(
        run_cgi_test(t, a, &h, post_req, want, NWANT(want), NULL));
    ARENA_END;
}

static Str chunk(Alloc *a, Str s) {
    return fmt_sprintf_v(a, "%x\r\n%s\r\n", s.len, s);
}

/* The CGI spec doesn't allow chunked requests. */
static void TestCGIPostChunked(TestingT *t) {
    SKIP_WITHOUT_EXEC(t);
    ARENA_BEGIN;
    Str post_req = fmt_sprintf_v(a, "%s%s%s%s%s",
                                 S("POST /test.cgi?a=b HTTP/1.1\n"
                                   "Host: example.com\n"
                                   "Content-Type: application/x-www-form-urlencoded\n"
                                   "Transfer-Encoding: chunked\n"
                                   "\n"),
                                 chunk(a, S("postfoo")), chunk(a, S("=")),
                                 chunk(a, S("postbar")), chunk(a, S("")));
    CgiHandler h = {0};
    h.path = self_path(a);
    h.root = S("/test.cgi");
    HttptestResponseRecorder *resp = run_cgi_test(t, a, &h, post_req, NULL, 0, NULL);
    Int got = resp->code, expected = HTTP_STATUS_BAD_REQUEST;
    httptest_response_recorder_free(resp);
    ARENA_END;
    if (got != expected)
        FATALF("Expected %v response code from chunked request body; got %d", expected,
               got);
}

static void TestRedirect(TestingT *t) {
    SKIP_WITHOUT_EXEC(t);
    ARENA_BEGIN;
    CgiHandler h = {0};
    h.path = self_path(a);
    h.root = S("/test.cgi");
    HttptestResponseRecorder *rec = run_cgi_test(
        t, a, &h,
        S("GET /test.cgi?loc=http://foo.com/ HTTP/1.0\nHost: example.com\n\n"), NULL, 0,
        NULL);
    if (rec->code != 302)
        testing_t_errorf_v(t, "expected status code %d; got %d", 302, rec->code);
    Str e = S("http://foo.com/"), g = header_get(rec, "Location");
    if (!str_eq(e, g))
        testing_t_errorf_v(t, "expected Location header of %q; got %q", e, g);
    httptest_response_recorder_free(rec);
    ARENA_END;
}

static void base_handler(void *env, HttpResponseWriter rw, HttpRequest *req) {
    (void)env;
    IoWriter w = http_response_writer_as_io_writer(rw);
    (void)fmt_fprintf_v(w, "basepath=%s\n", req->url->path);
    (void)fmt_fprintf_v(w, "remoteaddr=%s\n", req->remote_addr);
}

static void TestInternalRedirect(TestingT *t) {
    SKIP_WITHOUT_EXEC(t);
    ARENA_BEGIN;
    HttpHandlerFunc base = BURROW_FN(HttpHandlerFunc, base_handler, NULL);
    CgiHandler h = {0};
    h.path = self_path(a);
    h.root = S("/test.cgi");
    h.path_location_handler = http_handler_func_as_handler(&base);
    Want want[] = {
        {S("basepath"), S("/foo")},
        {S("remoteaddr"), S("1.2.3.4:1234")},
    };
    httptest_response_recorder_free(run_cgi_test(
        t, a, &h, S("GET /test.cgi?loc=/foo HTTP/1.0\nHost: example.com\n\n"), want,
        NWANT(want), NULL));
    ARENA_END;
}

/* How many ServeHTTP calls of TestCopyError's handler are running. */
static SyncAtomicInt64 copy_error_running;

static void copy_error_serve(void *env, HttpResponseWriter rw, HttpRequest *req) {
    (void)sync_atomic_int64_add(&copy_error_running, 1);
    cgi_handler_serve_http((CgiHandler *)env, rw, req);
    (void)sync_atomic_int64_add(&copy_error_running, -1);
}

/* handlerRunning reports whether any goroutine is currently running
 * [Handler.ServeHTTP]. */
static bool handler_running(void) {
    return sync_atomic_int64_load(&copy_error_running) > 0;
}

/* TestCopyError tests that we kill the process if there's an error copying
 * its output. (for example, from the client having gone away)
 *
 * If we fail to do so, the test will time out with a call to
 * cgi_handler_serve_http blocked on its wait for the program. */
static void TestCopyError(TestingT *t) {
    SKIP_WITHOUT_EXEC(t);
    need_tcp(t);
    ARENA_BEGIN;
    CgiHandler h = {0};
    h.path = self_path(a);
    h.root = S("/test.cgi");
    HttpHandlerFunc f = BURROW_FN(HttpHandlerFunc, copy_error_serve, &h);
    HttptestServer *ts = httptest_new_server(NULL, http_handler_func_as_handler(&f));

    NetAddr la = ts->listener.vt->addr(ts->listener.data);
    Error err = BURROW_NO_ERROR;
    NetConn conn =
        net_dial(heap_allocator(), S("tcp"), la.vt->string(la.data, a), &err);
    if (conn.vt == NULL) {
        httptest_server_close(ts);
        httptest_server_free(ts);
        ARENA_END;
        FATALF("%v", err);
    }
    HttpRequest *req =
        http_new_request(a, S("GET"), S("http://example.com/test.cgi?bigresponse=1"),
                         (IoReader){NULL, NULL}, &err);
    HttpResponse *res = NULL;
    bool conn_closed = false;
    err = http_request_write(req, net_conn_as_io_writer(conn));
    if (BURROW_FAILED(err)) {
        testing_t_errorf_v(t, "Write: %v", err);
        goto done;
    }
    res = http_read_response(a, bufio_new_reader(a, net_conn_as_io_reader(conn)), req,
                             &err);
    if (res == NULL) {
        testing_t_errorf_v(t, "ReadResponse: %v", err);
        goto done;
    }
    static Byte buf[5000];
    Int n = io_read_full(io_read_closer_as_io_reader(res->body),
                         (Slice){buf, 5000, 5000, TYPE_BYTE}, &err);
    if (BURROW_FAILED(err)) {
        testing_t_errorf_v(t, "ReadFull: %d bytes, %v", n, err);
        goto done;
    }

    if (!handler_running()) {
        testing_t_errorf_v(t, "pre-conn.Close, expected handler to still be running");
        goto done;
    }
    (void)conn.vt->closer.close(conn.data);
    conn_closed = true;
    Time closed = time_now();

    Duration next_sleep = TIME_MILLISECOND;
    for (;;) {
        time_sleep(next_sleep);
        next_sleep *= 2;
        if (!handler_running())
            break;
        testing_t_logf_v(t, "handler still running %v after conn.Close",
                         duration_string(time_since(closed), a));
    }

done:
    if (!conn_closed)
        (void)conn.vt->closer.close(conn.data);
    http_response_free(res);
    net_conn_free(conn);
    http_request_free(req);
    httptest_server_close(ts);
    httptest_server_free(ts);
    ARENA_END;
}

static void TestDir(TestingT *t) {
    SKIP_WITHOUT_EXEC(t);
    ARENA_BEGIN;
    Error err = BURROW_NO_ERROR;
    Str cwd = os_getwd(a, &err);
    CgiHandler h = {0};
    h.path = self_path(a);
    h.root = S("/test.cgi");
    h.dir = cwd;
    Want want[] = {{S("cwd"), cwd}};
    httptest_response_recorder_free(
        run_cgi_test(t, a, &h, S("GET /test.cgi HTTP/1.0\nHost: example.com\n\n"), want,
                     NWANT(want), NULL));

    Str file = {0};
    cwd = filepath_split(self_path(a), &file);
    h = (CgiHandler){0};
    h.path = self_path(a);
    h.root = S("/test.cgi");
    want[0].v = cwd;
    httptest_response_recorder_free(
        run_cgi_test(t, a, &h, S("GET /test.cgi HTTP/1.0\nHost: example.com\n\n"), want,
                     NWANT(want), NULL));
    ARENA_END;
}

static void TestEnvOverride(TestingT *t) {
    SKIP_WITHOUT_EXEC(t);
    ARENA_BEGIN;
    Error err = BURROW_NO_ERROR;
    Str cgifile = filepath_abs(a, S("testdata/test.cgi"), &err);

    Str cwd = os_getwd(a, &err);
    Str env[] = {
        fmt_sprintf_v(a, "SCRIPT_FILENAME=%s", cgifile),
        S("REQUEST_URI=/foo/bar"),
        S("PATH=/wibble"),
    };
    CgiHandler h = {0};
    h.path = self_path(a);
    h.root = S("/test.cgi");
    h.dir = cwd;
    h.env = slice_from(env, 3, 3, TYPE_STRING);
    Want want[] = {
        {S("cwd"), cwd},
        {S("env-SCRIPT_FILENAME"), cgifile},
        {S("env-REQUEST_URI"), S("/foo/bar")},
        {S("env-PATH"), S("/wibble")},
    };
    httptest_response_recorder_free(
        run_cgi_test(t, a, &h, S("GET /test.cgi HTTP/1.0\nHost: example.com\n\n"), want,
                     NWANT(want), NULL));
    ARENA_END;
}

static void TestHandlerStderr(TestingT *t) {
    SKIP_WITHOUT_EXEC(t);
    ARENA_BEGIN;
    /* The program's stderr is copied in on another goroutine while this one
     * allocates from a, and an arena is not safe for that, so the builder
     * has an arena of its own. */
    Arena ea;
    arena_init(&ea, heap_allocator(), 0);
    StringsBuilder stderr_ = STRINGS_BUILDER(arena_allocator(&ea));
    CgiHandler h = {0};
    h.path = self_path(a);
    h.root = S("/test.cgi");
    h.stderr_ = strings_builder_as_io_writer(&stderr_);

    HttptestResponseRecorder *rw = httptest_new_recorder(a);
    HttpRequest *req = new_request(
        a, S("GET /test.cgi?writestderr=1 HTTP/1.0\nHost: example.com\n\n"));
    cgi_handler_serve_http(&h, httptest_response_recorder_as_response_writer(rw), req);
    Str got = strings_builder_string(&stderr_), want = S("Hello, stderr!\n");
    if (!str_eq(got, want))
        testing_t_errorf_v(t, "Stderr = %q; want %q", got, want);
    arena_free(&ea);
    http_request_free(req);
    httptest_response_recorder_free(rw);
    ARENA_END;
}

static Str join_env(Alloc *a, const Str *env, Int n) {
    return strings_join(a, slice_from((void *)(uintptr_t)env, n, n, TYPE_STRING),
                        S(" "));
}

static void TestRemoveLeadingDuplicates(TestingT *t) {
    static const struct {
        const char *env[4];
        Int nenv;
        const char *want[4];
        Int nwant;
    } tests[] = {
        {{"a=b", "b=c", "a=b2"}, 3, {"b=c", "a=b2"}, 2},
        {{"a=b", "b=c", "d", "e=f"}, 4, {"a=b", "b=c", "d", "e=f"}, 4},
    };
    ARENA_BEGIN;
    for (size_t i = 0; i < sizeof tests / sizeof tests[0]; i++) {
        Str env[4], got[4], want[4];
        for (Int j = 0; j < tests[i].nenv; j++)
            env[j] = got[j] = cs(tests[i].env[j]);
        for (Int j = 0; j < tests[i].nwant; j++)
            want[j] = cs(tests[i].want[j]);
        Int n = burrow__cgi_remove_leading_duplicates(got, tests[i].nenv);
        bool equal = n == tests[i].nwant;
        for (Int j = 0; equal && j < n; j++)
            equal = str_eq(got[j], want[j]);
        if (!equal)
            testing_t_errorf_v(t, "removeLeadingDuplicates(%q) = %q; want %q",
                               join_env(a, env, tests[i].nenv), join_env(a, got, n),
                               join_env(a, want, tests[i].nwant));
    }
    ARENA_END;
}

/* ------------------------------------------------------ integration_test.go */

/* This test is a CGI host (testing host.go) that runs its own binary
 * as a child process testing the other half of CGI (child.go). */
static void TestHostingOurselves(TestingT *t) {
    SKIP_WITHOUT_EXEC(t);
    ARENA_BEGIN;
    CgiHandler h = {0};
    h.path = self_path(a);
    h.root = S("/test.go");
    Want want[] = {
        {S("test"), S("Hello CGI-in-CGI")},
        {S("param-a"), S("b")},
        {S("param-foo"), S("bar")},
        {S("env-GATEWAY_INTERFACE"), S("CGI/1.1")},
        {S("env-HTTP_HOST"), S("example.com")},
        {S("env-PATH_INFO"), S("")},
        {S("env-QUERY_STRING"), S("foo=bar&a=b")},
        {S("env-REMOTE_ADDR"), S("1.2.3.4")},
        {S("env-REMOTE_HOST"), S("1.2.3.4")},
        {S("env-REMOTE_PORT"), S("1234")},
        {S("env-REQUEST_METHOD"), S("GET")},
        {S("env-REQUEST_URI"), S("/test.go?foo=bar&a=b")},
        {S("env-SCRIPT_FILENAME"), self_path(a)},
        {S("env-SCRIPT_NAME"), S("/test.go")},
        {S("env-SERVER_NAME"), S("example.com")},
        {S("env-SERVER_PORT"), S("80")},
        {S("env-SERVER_SOFTWARE"), S("go")},
    };
    HttptestResponseRecorder *replay = run_cgi_test(
        t, a, &h, S("GET /test.go?foo=bar&a=b HTTP/1.0\nHost: example.com\n\n"), want,
        NWANT(want), NULL);

    Str expected = S("text/plain; charset=utf-8"),
        got = header_get(replay, "Content-Type");
    if (!str_eq(got, expected))
        testing_t_errorf_v(t, "got a Content-Type of %q; expected %q", got, expected);
    expected = S("X-Test-Value");
    got = header_get(replay, "X-Test-Header");
    if (!str_eq(got, expected))
        testing_t_errorf_v(t, "got a X-Test-Header of %q; expected %q", got, expected);
    httptest_response_recorder_free(replay);
    ARENA_END;
}

BURROW_SENTINEL_ERROR(err_past_write_limit, "past write limit");

/* customWriterRecorder, a recorder whose Write goes to a limitWriter. */
typedef struct CustomWriterRecorder {
    BytesBuffer *w;
    Int n;
    HttptestResponseRecorder *rec;
} CustomWriterRecorder;

static Int cwr_write(void *self, Slice p, Error *err) {
    CustomWriterRecorder *w = (CustomWriterRecorder *)self;
    if (p.len > w->n)
        p.len = w->n;
    Int n = 0;
    Error e = BURROW_NO_ERROR;
    if (p.len > 0) {
        n = bytes_buffer_write(w->w, p, &e);
        w->n -= n;
    }
    if (w->n == 0)
        e = err_past_write_limit;
    BURROW_OUT(err, e);
    return n;
}

static HttpHeader cwr_header(void *self) {
    return httptest_response_recorder_header(((CustomWriterRecorder *)self)->rec);
}

static void cwr_write_header(void *self, Int code) {
    httptest_response_recorder_write_header(((CustomWriterRecorder *)self)->rec, code);
}

static Error cwr_flush(void *self) {
    httptest_response_recorder_flush(((CustomWriterRecorder *)self)->rec);
    return BURROW_NO_ERROR;
}

static const HttpResponseWriterVT cwr_vt = {
    {NULL, cwr_write},
    cwr_header,
    cwr_write_header,
    cwr_flush,
    NULL,
    NULL,
    NULL,
    NULL,
    NULL,
    NULL,
};

/* If there's an error copying the child's output to the parent, test
 * that we kill the child. */
static void TestKillChildAfterCopyError(TestingT *t) {
    SKIP_WITHOUT_EXEC(t);
    ARENA_BEGIN;
    CgiHandler h = {0};
    h.path = self_path(a);
    h.root = S("/test.go");
    Error err = BURROW_NO_ERROR;
    HttpRequest *req =
        http_new_request(a, S("GET"), S("http://example.com/test.go?write-forever=1"),
                         (IoReader){NULL, NULL}, &err);
    HttptestResponseRecorder *rec = httptest_new_recorder(a);
    BytesBuffer *out = bytes_new_buffer(a, (Slice){0});
    enum { write_len = 50 << 10 };
    CustomWriterRecorder rw = {out, write_len, rec};

    cgi_handler_serve_http(&h, (HttpResponseWriter){&cwr_vt, &rw}, req);
    Slice b = bytes_buffer_bytes(out);
    if (b.len != write_len || ((const Byte *)b.p)[0] != 'a')
        testing_t_errorf_v(t, "unexpected output: %q", str_from_bytes(b.p, b.len));
    http_request_free(req);
    httptest_response_recorder_free(rec);
    ARENA_END;
}

/* Test that a child handler writing only headers works.
 * golang.org/issue/7196 */
static void TestChildOnlyHeaders(TestingT *t) {
    SKIP_WITHOUT_EXEC(t);
    ARENA_BEGIN;
    CgiHandler h = {0};
    h.path = self_path(a);
    h.root = S("/test.go");
    Want want[] = {{S("_body"), S("")}};
    HttptestResponseRecorder *replay = run_cgi_test(
        t, a, &h, S("GET /test.go?no-body=1 HTTP/1.0\nHost: example.com\n\n"), want,
        NWANT(want), NULL);
    Str expected = S("X-Test-Value"), got = header_get(replay, "X-Test-Header");
    if (!str_eq(got, expected))
        testing_t_errorf_v(t, "got a X-Test-Header of %q; expected %q", got, expected);
    httptest_response_recorder_free(replay);
    ARENA_END;
}

/* Test that a child handler does not receive a nil Request Body.
 * golang.org/issue/39190 */
static void TestNilRequestBody(TestingT *t) {
    SKIP_WITHOUT_EXEC(t);
    ARENA_BEGIN;
    CgiHandler h = {0};
    h.path = self_path(a);
    h.root = S("/test.go");
    Want want[] = {{S("nil-request-body"), S("false")}};
    httptest_response_recorder_free(run_cgi_test(
        t, a, &h, S("POST /test.go?nil-request-body=1 HTTP/1.0\nHost: example.com\n\n"),
        want, NWANT(want), NULL));
    httptest_response_recorder_free(
        run_cgi_test(t, a, &h,
                     S("POST /test.go?nil-request-body=1 HTTP/1.0\nHost: "
                       "example.com\nContent-Length: "
                       "0\n\n"),
                     want, NWANT(want), NULL));
    ARENA_END;
}

static void child_content_type_case(void *env, TestingT *t) {
    const ContentTypeTest *tt = (const ContentTypeTest *)env;
    ARENA_BEGIN;
    CgiHandler h = {0};
    h.path = self_path(a);
    h.root = S("/test.go");
    Want want[] = {{S("_body"), tt->body}};
    Str req =
        fmt_sprintf_v(a, "GET /test.go?exact-body=%s HTTP/1.0\nHost: example.com\n\n",
                      url_query_escape(a, tt->body));
    HttptestResponseRecorder *replay =
        run_cgi_test(t, a, &h, req, want, NWANT(want), NULL);
    Str got = header_get(replay, "Content-Type");
    if (!str_eq(got, cs(tt->want_ct)))
        testing_t_errorf_v(t, "got a Content-Type of %q; expected it to start with %q",
                           got, cs(tt->want_ct));
    httptest_response_recorder_free(replay);
    ARENA_END;
}

static void TestChildContentType(TestingT *t) {
    SKIP_WITHOUT_EXEC(t);
    ARENA_BEGIN;
    content_type_tests_init(a);
    for (size_t i = 0; i < sizeof content_type_tests / sizeof content_type_tests[0];
         i++)
        testing_t_run(
            t, cs(content_type_tests[i].name),
            BURROW_FN(TestingTFunc, child_content_type_case, &content_type_tests[i]));
    ARENA_END;
}

static void want500_test(TestingT *t, const char *path) {
    ARENA_BEGIN;
    CgiHandler h = {0};
    h.path = self_path(a);
    h.root = S("/test.go");
    Want want[] = {{S("_body"), S("")}};
    Str req = fmt_sprintf_v(a, "GET %s HTTP/1.0\nHost: example.com\n\n", cs(path));
    HttptestResponseRecorder *replay =
        run_cgi_test(t, a, &h, req, want, NWANT(want), NULL);
    if (replay->code != 500)
        testing_t_errorf_v(t, "Got code %d; want 500", replay->code);
    httptest_response_recorder_free(replay);
    ARENA_END;
}

/* golang.org/issue/7198 */
static void Test500WithNoHeaders(TestingT *t) {
    SKIP_WITHOUT_EXEC(t);
    want500_test(t, "/immediate-disconnect");
}

static void Test500WithNoContentType(TestingT *t) {
    SKIP_WITHOUT_EXEC(t);
    want500_test(t, "/no-content-type");
}

static void Test500WithEmptyHeaders(TestingT *t) {
    SKIP_WITHOUT_EXEC(t);
    want500_test(t, "/empty-headers");
}

/* TestMain executes the test binary as the cgi server if
 * SERVER_SOFTWARE is set, and runs the tests otherwise. */
static int cgi_test_main(TestingM *m) {
    Arena ar;
    arena_init(&ar, NULL, 0);
    /* SERVER_SOFTWARE swap variable is set when starting the cgi server. */
    bool child = os_getenv(arena_allocator(&ar), S("SERVER_SOFTWARE")).len > 0;
    arena_free(&ar);
    if (child) {
        cgi_main();
        os_exit(0);
    }
    return testing_m_run(m);
}

#define TESTS(X)                                                                       \
    X(TestRequest)                                                                     \
    X(TestRequestWithTLS)                                                              \
    X(TestRequestWithoutHost)                                                          \
    X(TestRequestWithoutRequestURI)                                                    \
    X(TestRequestWithoutRemotePort)                                                    \
    X(TestIncludedServerProtocol)                                                      \
    X(TestResponse)                                                                    \
    X(TestCGIBasicGet)                                                                 \
    X(TestCGIEnvIPv6)                                                                  \
    X(TestCGIBasicGetAbsPath)                                                          \
    X(TestPathInfo)                                                                    \
    X(TestPathInfoDirRoot)                                                             \
    X(TestDupHeaders)                                                                  \
    X(TestDropProxyHeader)                                                             \
    X(TestPathInfoNoRoot)                                                              \
    X(TestCGIBasicPost)                                                                \
    X(TestCGIPostChunked)                                                              \
    X(TestRedirect)                                                                    \
    X(TestInternalRedirect)                                                            \
    X(TestCopyError)                                                                   \
    X(TestDir)                                                                         \
    X(TestEnvOverride)                                                                 \
    X(TestHandlerStderr)                                                               \
    X(TestRemoveLeadingDuplicates)                                                     \
    X(TestHostingOurselves)                                                            \
    X(TestKillChildAfterCopyError)                                                     \
    X(TestChildOnlyHeaders)                                                            \
    X(TestNilRequestBody)                                                              \
    X(TestChildContentType)                                                            \
    X(Test500WithNoHeaders)                                                            \
    X(Test500WithNoContentType)                                                        \
    X(Test500WithEmptyHeaders)

TESTING_MAIN_WITH(cgi_test_main, TESTS)
