/* Derived from Go's src/net/http/server_test.go, serve_test.go,
 * request_test.go and pattern_test.go, the tests of ServeMux and of the
 * handlers that go with it.
 * Go source: go1.27.1.
 *
 * There is no httptest yet, so a small recorder here stands in for
 * httptest.ResponseRecorder, and new_test_request for httptest.NewRequest.
 * There is no Server or Client either, so TestStripPrefix, TestPathValueAndPattern,
 * TestSetPathValue and TestStatus read each request from its wire form and
 * call ServeHTTP on it, which is what the server would do, and TestStatus
 * follows the redirects itself, as the client would.
 *
 * Copyright 2009 The Go Authors. All rights reserved.
 * Copyright 2026 The burrow Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style licence that can be found
 * in the LICENSE file. */

#include "check.h"

#include "../src/net/http_internal.h"
#include "../src/net/http_routing.h"

#include "burrow/bufio.h"
#include "burrow/burrow.h"
#include "burrow/fmt.h"
#include "burrow/io.h"
#include "burrow/map.h"
#include "burrow/mem/arena.h"
#include "burrow/net/http.h"
#include "burrow/net/url.h"
#include "burrow/panic.h"
#include "burrow/regexp.h"
#include "burrow/strings.h"

#include <stdint.h>
#include <string.h>

#define S BURROW_S

#define ARENA_BEGIN                                                                    \
    Arena ar;                                                                          \
    arena_init(&ar, NULL, 0);                                                          \
    Alloc *a = arena_allocator(&ar)
#define ARENA_END arena_free(&ar)

static Str cs(const char *s) {
    return s != NULL ? str_from_cstr(s) : (Str){0};
}

static void *must(TestingT *t, void *p) {
    if (p == NULL)
        testing_t_fatalf_v(t, "out of memory");
    return p;
}

/* ---------------------------------------------------------------- recorder */

/* httptest.ResponseRecorder, as much of it as these tests look at. */
typedef struct Recorder {
    HttpHeader header;
    StringsBuilder body;
    Int code;
    bool wrote;
} Recorder;

static void rec_write_header(void *self, Int code) {
    Recorder *rr = (Recorder *)self;
    if (rr->wrote)
        return;
    rr->code = code;
    rr->wrote = true;
}

static Int rec_write(void *self, Slice p, Error *err) {
    Recorder *rr = (Recorder *)self;
    if (!rr->wrote)
        rec_write_header(self, HTTP_STATUS_OK);
    return strings_builder_write(&rr->body, p, err);
}

static HttpHeader rec_header(void *self) {
    return ((Recorder *)self)->header;
}

static const HttpResponseWriterVT rec_vt = {
    {NULL, rec_write}, rec_header, rec_write_header};

/* httptest.NewRecorder, with 200 for the code until something writes one. */
static void rec_init(TestingT *t, Alloc *a, Recorder *rr) {
    memset(rr, 0, sizeof *rr);
    rr->header = (HttpHeader)must(t, http_header_make(a));
    rr->body = STRINGS_BUILDER(a);
    rr->code = HTTP_STATUS_OK;
}

static HttpResponseWriter rec_writer(Recorder *rr) {
    return (HttpResponseWriter){&rec_vt, rr};
}

/* ---------------------------------------------------------------- requests */

static HttpRequest *read_req(TestingT *t, Alloc *a, Str raw) {
    StringsReader *sr =
        (StringsReader *)must(t, mem_alloc(a, sizeof *sr, _Alignof(StringsReader)));
    strings_reader_reset(sr, raw);
    BufioReader *br =
        (BufioReader *)must(t, bufio_new_reader(a, strings_reader_as_io_reader(sr)));
    Error err = BURROW_NO_ERROR;
    HttpRequest *r = http_read_request(a, br, &err);
    if (r == NULL)
        testing_t_fatalf_v(t, "ReadRequest(%q): %s", raw, error_text(err));
    return r;
}

/* httptest.NewRequest. target is read as the target of an HTTP/1.0 request,
 * which is then made HTTP/1.1, and the host is example.com when the target
 * does not give one. */
static HttpRequest *new_test_request(TestingT *t, Alloc *a, const char *method,
                                     Str target) {
    Str raw = fmt_sprintf_v(a, "%s %s HTTP/1.0\r\n\r\n", cs(method), target);
    HttpRequest *r = read_req(t, a, raw);
    r->proto = S("HTTP/1.1");
    r->proto_minor = 1;
    r->close = false;
    if (r->host.len == 0)
        r->host = S("example.com");
    return r;
}

/* http.NewRequest with no body. */
static HttpRequest *new_request(TestingT *t, Alloc *a, const char *method, Str url) {
    Error err = BURROW_NO_ERROR;
    HttpRequest *r = http_new_request(a, cs(method), url, (IoReader){0}, &err);
    if (r == NULL)
        testing_t_fatalf_v(t, "NewRequest(%s, %q): %s", cs(method), url,
                           error_text(err));
    return r;
}

/* A request for a method, host and URL, as a struct literal makes one in
 * Go, with nothing of its own. */
static void zero_request(HttpRequest *r, Url *u, Str method, Str host, Str path) {
    memset(r, 0, sizeof *r);
    memset(u, 0, sizeof *u);
    u->path = path;
    r->method = method;
    r->host = host;
    r->url = u;
}

/* ---------------------------------------------------------------- handlers */

/* server_test.go's handler, which does nothing and is told apart by i. */
typedef struct TestHandler {
    Int i;
} TestHandler;

static void test_handler_serve(void *self, HttpResponseWriter w, HttpRequest *r) {
    (void)self;
    (void)w;
    (void)r;
}

static const HttpHandlerVT test_handler_vt = {NULL, test_handler_serve};

/* stringHandler, which sets Result to its string. */
static void string_handler_serve(void *self, HttpResponseWriter w, HttpRequest *r) {
    (void)r;
    (void)http_header_set(http_response_writer_header(w), S("Result"),
                          *(const Str *)self);
}

static const HttpHandlerVT string_handler_vt = {NULL, string_handler_serve};

static HttpHandler string_handler(TestingT *t, Alloc *a, const char *s) {
    Str *p = (Str *)must(t, mem_alloc(a, sizeof *p, _Alignof(Str)));
    *p = cs(s);
    return (HttpHandler){&string_handler_vt, p};
}

/* serve(code), which writes code. */
static void serve_code(void *env, HttpResponseWriter w, HttpRequest *r) {
    (void)r;
    http_response_writer_write_header(w, *(const Int *)env);
}

static void serve_nothing(void *env, HttpResponseWriter w, HttpRequest *r) {
    (void)env;
    (void)w;
    (void)r;
}

static void serve_ok(void *env, HttpResponseWriter w, HttpRequest *r) {
    (void)env;
    (void)r;
    (void)fmt_fprintln_v(http_response_writer_as_io_writer(w), S("ok"));
}

/* checkQueryStringHandler. 200 when the query is the URL without its scheme
 * and query, and 500 when it is not. */
static void check_query_string(void *env, HttpResponseWriter w, HttpRequest *r) {
    (void)env;
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    Url u = *r->url;
    u.scheme = S("http");
    u.host = r->host;
    u.raw_query = (Str){0};
    Str want = fmt_sprintf_v(a, "http://%s", r->url->raw_query);
    bool ok = str_eq(want, url_string(&u, a));
    arena_free(&ar);
    http_response_writer_write_header(w, ok ? 200 : 500);
}

/* What %#v shows for the handlers TestFindHandler looks for. */
static Str describe_handler(Alloc *a, HttpHandler h) {
    if (h.vt == &test_handler_vt)
        return fmt_sprintf_v(a, "&http.handler{i:%d}",
                             ((const TestHandler *)h.data)->i);
    if (h.vt == &burrow__http_redirect_handler_vt) {
        const burrow__HttpRedirectHandler *rh =
            (const burrow__HttpRedirectHandler *)h.data;
        return fmt_sprintf_v(a, "&http.redirectHandler{url:%q, code:%d}", rh->url,
                             rh->code);
    }
    if (h.vt->self_type == TYPE_HTTP_HANDLER_FUNC)
        return S("(http.HandlerFunc)(func)");
    return S("unknown handler");
}

/* ------------------------------------------------------------ server_test.go */

static void TestFindHandler(TestingT *t) {
    ARENA_BEGIN;
    HttpServeMux *mux = (HttpServeMux *)must(t, http_new_serve_mux(a));
    static const char *const pats[] = {"/", "/foo/", "/foo", "/bar/", "//foo"};
    TestHandler hs[sizeof pats / sizeof pats[0]];
    for (size_t i = 0; i < sizeof pats / sizeof pats[0]; i++) {
        hs[i].i = (Int)i + 1;
        http_serve_mux_handle(mux, cs(pats[i]),
                              ((HttpHandler){&test_handler_vt, &hs[i]}));
    }

    static const struct {
        const char *method;
        const char *path;
        const char *want_handler;
    } tests[] = {
        {"GET", "/", "&http.handler{i:1}"},
        {"GET", "//", "&http.redirectHandler{url:\"/\", code:307}"},
        {"GET", "/foo/../bar/./..//baz",
         "&http.redirectHandler{url:\"/baz\", code:307}"},
        {"GET", "/foo", "&http.handler{i:3}"},
        {"GET", "/foo/x", "&http.handler{i:2}"},
        {"GET", "/bar/x", "&http.handler{i:4}"},
        {"GET", "/bar", "&http.redirectHandler{url:\"/bar/\", code:307}"},
        {"CONNECT", "", "(http.HandlerFunc)(.*)"},
        {"CONNECT", "/", "&http.handler{i:1}"},
        {"CONNECT", "//", "&http.handler{i:1}"},
        {"CONNECT", "//foo", "&http.handler{i:5}"},
        {"CONNECT", "/foo/../bar/./..//baz", "&http.handler{i:2}"},
        {"CONNECT", "/foo", "&http.handler{i:3}"},
        {"CONNECT", "/foo/x", "&http.handler{i:2}"},
        {"CONNECT", "/bar/x", "&http.handler{i:4}"},
        {"CONNECT", "/bar", "&http.redirectHandler{url:\"/bar/\", code:307}"},
    };
    for (size_t i = 0; i < sizeof tests / sizeof tests[0]; i++) {
        HttpRequest r;
        Url u;
        zero_request(&r, &u, cs(tests[i].method), S("example.com"), cs(tests[i].path));
        Str pattern;
        const burrow__HttpPattern *pat;
        Slice matches;
        HttpHandler h =
            burrow__http_serve_mux_find_handler(mux, &r, a, &pattern, &pat, &matches);
        Str got = describe_handler(a, h);
        Regexp *re = regexp_must_compile(a, cs(tests[i].want_handler));
        if (!regexp_match_string(re, got))
            testing_t_errorf_v(t, "%s %q: got %q, want %q", cs(tests[i].method),
                               cs(tests[i].path), got, cs(tests[i].want_handler));
        regexp_free(re);
    }
    http_serve_mux_free(mux);
    ARENA_END;
}

/* A ServeMux with nothing registered does not panic. */
static void TestEmptyServeMux(TestingT *t) {
    ARENA_BEGIN;
    HttpServeMux *mux = (HttpServeMux *)must(t, http_new_serve_mux(a));
    HttpRequest r;
    Url u;
    zero_request(&r, &u, S("GET"), S("example.com"), S("/"));
    Str p;
    (void)http_serve_mux_handler(mux, &r, a, &p);
    if (p.len != 0)
        testing_t_errorf_v(t, "got %q, want \"\"", p);
    http_serve_mux_free(mux);
    ARENA_END;
}

static void TestRegisterErr(TestingT *t) {
    ARENA_BEGIN;
    HttpServeMux *mux = (HttpServeMux *)must(t, http_new_serve_mux(a));
    TestHandler th = {0};
    HttpHandler h = {&test_handler_vt, &th};
    http_serve_mux_handle(mux, S("/a"), h);

    HttpHandlerFunc nil_func = {0};
    HttpHandler nil_handler = {0};
    HttpHandler nil_func_handler = http_handler_func_as_handler(&nil_func);
    const struct {
        const char *pattern;
        const HttpHandler *handler;
        const char *want_regexp;
    } tests[] = {
        {"", &h, "invalid pattern"},
        {"/", &nil_handler, "nil handler"},
        {"/", &nil_func_handler, "nil handler"},
        {"/{x", &h, "parsing \"/\\{x\": at offset 1: bad wildcard segment"},
        {"/a", &h, "conflicts with pattern.* \\(registered at .*http_mux_test.c:\\d+"},
    };
    for (size_t i = 0; i < sizeof tests / sizeof tests[0]; i++) {
        Error err = burrow__http_serve_mux_register_err(
            mux, cs(tests[i].pattern), *tests[i].handler, __FILE__, __LINE__);
        if (BURROW_OK(err)) {
            testing_t_errorf_v(t, "%q: got nil error", cs(tests[i].pattern));
            continue;
        }
        Regexp *re = regexp_must_compile(a, cs(tests[i].want_regexp));
        Str g = error_text(err);
        if (!regexp_match_string(re, g))
            testing_t_errorf_v(t, "\ngot %q\nwant string matching %q", g,
                               cs(tests[i].want_regexp));
        regexp_free(re);
    }
    http_serve_mux_free(mux);
    ARENA_END;
}

static void TestExactMatch(TestingT *t) {
    ARENA_BEGIN;
    static const struct {
        const char *pattern;
        const char *path;
        bool want;
    } tests[] = {
        {"", "/a", false},           {"/", "/a", false},
        {"/a", "/a", true},          {"/a/{x...}", "/a/b", false},
        {"/a/{x}", "/a/b", true},    {"/a/b/", "/a/b/", true},
        {"/a/b/{$}", "/a/b/", true}, {"/a/", "/a/b/", false},
    };
    for (size_t i = 0; i < sizeof tests / sizeof tests[0]; i++) {
        burrow__HttpRoutingNode node;
        const burrow__HttpRoutingNode *n = NULL;
        if (tests[i].pattern[0] != '\0') {
            Error err = BURROW_NO_ERROR;
            burrow__HttpPattern *pat =
                burrow__http_parse_pattern(a, cs(tests[i].pattern), &err);
            if (pat == NULL)
                testing_t_fatalf_v(t, "parsePattern(%q): %s", cs(tests[i].pattern),
                                   error_text(err));
            memset(&node, 0, sizeof node);
            node.pattern = pat;
            n = &node;
        }
        bool got = burrow__http_exact_match(n, cs(tests[i].path));
        if (got != tests[i].want)
            testing_t_errorf_v(t, "%q, %s: got %t, want %t", cs(tests[i].pattern),
                               cs(tests[i].path), got, tests[i].want);
    }
    ARENA_END;
}

typedef struct EscapedCase {
    const char *pattern;
    const char *paths[5];    /* paths that match the pattern */
    const char *paths121[2]; /* paths that matched the pattern in Go 1.21 */
} EscapedCase;

static const EscapedCase escaped_cases[] = {
    /* This pattern matches a path that unescapes to "/a". */
    {"/a", {"/a", "/%61"}, {"/a", "/%61"}},
    /* Patterns are unescaped by segment, so this matches paths that unescape
     * to "/b". */
    {"/%62",
     {"/b", "/%62"},
     {"/%2562"}}, /* 1.21 did not unescape patterns, but did paths */
    /* The only way to write a pattern that matches '{' or '}'. */
    {"/%7B/%7D", {"/{/}", "/%7b/}", "/{/%7d", "/%7B/%7D"}, {"/%257B/%257D"}},
    /* Patterns that do not unescape are left unchanged. */
    {"/%x", {"/%25x"}, {"/%25x"}},
};

static void escaped_run(TestingT *t, bool test121) {
    ARENA_BEGIN;
    burrow__http_godebug_set(test121 ? "httpmuxgo121=1" : "");
    HttpServeMux *mux = (HttpServeMux *)must(t, http_new_serve_mux(a));
    for (size_t i = 0; i < sizeof escaped_cases / sizeof escaped_cases[0]; i++)
        http_serve_mux_handle_func(mux, cs(escaped_cases[i].pattern),
                                   BURROW_FN(HttpHandlerFunc, serve_nothing, NULL));

    for (size_t i = 0; i < sizeof escaped_cases / sizeof escaped_cases[0]; i++) {
        const EscapedCase *m = &escaped_cases[i];
        const char *const *paths = test121 ? m->paths121 : m->paths;
        size_t n = test121 ? sizeof m->paths121 / sizeof m->paths121[0]
                           : sizeof m->paths / sizeof m->paths[0];
        for (size_t j = 0; j < n && paths[j] != NULL; j++) {
            Error err = BURROW_NO_ERROR;
            Url *u = url_parse_request_uri(a, cs(paths[j]), &err);
            if (u == NULL)
                testing_t_fatalf_v(t, "%s", error_text(err));
            HttpRequest req;
            memset(&req, 0, sizeof req);
            req.url = u;
            Str got;
            (void)http_serve_mux_handler(mux, &req, a, &got);
            if (!str_eq(got, cs(m->pattern)))
                testing_t_errorf_v(t, "%s: pattern: got %q, want %q", cs(paths[j]), got,
                                   cs(m->pattern));
        }
    }
    http_serve_mux_free(mux);
    burrow__http_godebug_set(NULL);
    ARENA_END;
}

static void escaped_latest(void *env, TestingT *t) {
    (void)env;
    escaped_run(t, false);
}

static void escaped_121(void *env, TestingT *t) {
    (void)env;
    escaped_run(t, true);
}

static void TestEscapedPathsAndPatterns(TestingT *t) {
    testing_t_run(t, S("latest"), BURROW_FN(TestingTFunc, escaped_latest, NULL));
    testing_t_run(t, S("1.21"), BURROW_FN(TestingTFunc, escaped_121, NULL));
}

static void TestCleanPath(TestingT *t) {
    ARENA_BEGIN;
    static const struct {
        const char *in;
        const char *want;
    } tests[] = {
        {"//", "/"},
        {"/x", "/x"},
        {"//x", "/x"},
        {"x//", "/x/"},
        {"a//b/////c", "/a/b/c"},
        {"/foo/../bar/./..//baz", "/baz"},
    };
    for (size_t i = 0; i < sizeof tests / sizeof tests[0]; i++) {
        Str got = burrow__http_clean_path(a, cs(tests[i].in));
        if (!str_eq(got, cs(tests[i].want)))
            testing_t_errorf_v(t, "%s: got %q, want %q", cs(tests[i].in), got,
                               cs(tests[i].want));
    }
    ARENA_END;
}

/* ----------------------------------------------------------- pattern_test.go */

static void TestRegisterConflict(TestingT *t) {
    ARENA_BEGIN;
    HttpServeMux *mux = (HttpServeMux *)must(t, http_new_serve_mux(a));
    Str pat1 = S("/a/{x}/");
    Error err = burrow__http_serve_mux_register_err(mux, pat1, http_not_found_handler(),
                                                    __FILE__, __LINE__);
    if (BURROW_FAILED(err))
        testing_t_fatalf_v(t, "%s", error_text(err));
    Str pat2 = S("/a/{y}/{z...}");
    err = burrow__http_serve_mux_register_err(mux, pat2, http_not_found_handler(),
                                              __FILE__, __LINE__);
    Str got = BURROW_OK(err) ? S("<nil>") : error_text(err);
    Str want = S("matches the same requests as");
    if (!strings_contains(got, want))
        testing_t_errorf_v(t, "got\n%s\nwant\n%s", got, want);
    http_serve_mux_free(mux);
    ARENA_END;
}

/* ------------------------------------------------------------- serve_test.go */

/* serveMuxRegister. A code of -1 is checkQueryStringHandler. */
static const struct {
    const char *pattern;
    Int code;
} serve_mux_register[] = {
    {"/dir/", 200},
    {"/search", 201},
    {"codesearch.google.com/search", 202},
    {"codesearch.google.com/", 203},
    {"example.com/", -1},
    {"/pkg/bar/extra%2fpath", 200},
};

static void register_serve_mux(HttpServeMux *mux) {
    for (size_t i = 0; i < sizeof serve_mux_register / sizeof serve_mux_register[0];
         i++) {
        HttpHandlerFunc f =
            serve_mux_register[i].code < 0
                ? BURROW_FN(HttpHandlerFunc, check_query_string, NULL)
                : BURROW_FN(HttpHandlerFunc, serve_code,
                            (void *)(uintptr_t)&serve_mux_register[i].code);
        http_serve_mux_handle_func(mux, cs(serve_mux_register[i].pattern), f);
    }
}

static void TestServeMuxHandler(TestingT *t) {
    ARENA_BEGIN;
    HttpServeMux *mux = (HttpServeMux *)must(t, http_new_serve_mux(a));
    register_serve_mux(mux);

    static const struct {
        const char *method;
        const char *host;
        const char *path;
        const char *pattern;
        Int code;
    } tests[] = {
        {"GET", "google.com", "/", "", 404},
        {"GET", "google.com", "/dir", "/dir/", 307},
        {"GET", "google.com", "/dir/", "/dir/", 200},
        {"GET", "google.com", "/dir/file", "/dir/", 200},
        {"GET", "google.com", "/search", "/search", 201},
        {"GET", "google.com", "/search/", "", 404},
        {"GET", "google.com", "/search/foo", "", 404},
        {"GET", "codesearch.google.com", "/search", "codesearch.google.com/search",
         202},
        {"GET", "codesearch.google.com", "/search/", "codesearch.google.com/", 203},
        {"GET", "codesearch.google.com", "/search/foo", "codesearch.google.com/", 203},
        {"GET", "codesearch.google.com", "/", "codesearch.google.com/", 203},
        {"GET", "codesearch.google.com:443", "/", "codesearch.google.com/", 203},
        {"GET", "images.google.com", "/search", "/search", 201},
        {"GET", "images.google.com", "/search/", "", 404},
        {"GET", "images.google.com", "/search/foo", "", 404},
        {"GET", "google.com", "/../search", "/search", 307},
        {"GET", "google.com", "/dir/..", "", 307},
        {"GET", "google.com", "/dir/..", "", 307},
        {"GET", "google.com", "/dir/./file", "/dir/", 307},

        /* The /foo -> /foo/ redirect applies to CONNECT requests but the
         * path canonicalization does not. */
        {"CONNECT", "google.com", "/dir", "/dir/", 307},
        {"CONNECT", "google.com", "/../search", "", 404},
        {"CONNECT", "google.com", "/dir/..", "/dir/", 200},
        {"CONNECT", "google.com", "/dir/..", "/dir/", 200},
        {"CONNECT", "google.com", "/dir/./file", "/dir/", 200},
    };
    for (size_t i = 0; i < sizeof tests / sizeof tests[0]; i++) {
        HttpRequest r;
        Url u;
        zero_request(&r, &u, cs(tests[i].method), cs(tests[i].host), cs(tests[i].path));
        Str pattern;
        HttpHandler h = http_serve_mux_handler(mux, &r, a, &pattern);
        Recorder rr;
        rec_init(t, a, &rr);
        http_handler_serve_http(h, rec_writer(&rr), &r);
        if (!str_eq(pattern, cs(tests[i].pattern)) || rr.code != tests[i].code)
            testing_t_errorf_v(t, "%s %s %s = %d, %q, want %d, %q", cs(tests[i].method),
                               cs(tests[i].host), cs(tests[i].path), rr.code, pattern,
                               tests[i].code, cs(tests[i].pattern));
    }
    http_serve_mux_free(mux);
    ARENA_END;
}

/* Issue 73688. */
static void TestServeMuxHandlerTrailingSlash(TestingT *t) {
    ARENA_BEGIN;
    HttpServeMux *mux = (HttpServeMux *)must(t, http_new_serve_mux(a));
    Str original = S("/{x}/");
    http_serve_mux_handle(mux, original, http_not_found_handler());
    HttpRequest *r = new_request(t, a, "POST", S("/foo"));
    Str p;
    (void)http_serve_mux_handler(mux, r, a, &p);
    if (!str_eq(p, original))
        testing_t_errorf_v(t, "got %q, want %q", p, original);
    http_request_free(r);
    http_serve_mux_free(mux);
    ARENA_END;
}

/* Issue 24297. */
static void TestServeMuxHandleFuncWithNilHandler(TestingT *t) {
    ARENA_BEGIN;
    HttpServeMux *mux = (HttpServeMux *)must(t, http_new_serve_mux(a));
    volatile bool panicked = false;
    BURROW_TRY {
        http_serve_mux_handle_func(mux, S("/"), (HttpHandlerFunc){0});
    }
    BURROW_CATCH(p) {
        (void)p;
        panicked = true;
    }
    BURROW_TRY_END;
    if (!panicked)
        testing_t_errorf_v(t, "expected call to mux.HandleFunc to panic");
    http_serve_mux_free(mux);
    ARENA_END;
}

/* TestServeMuxHandlerRedirects. The redirects mux.Handler makes keep the
 * request's query string. */
static void TestServeMuxHandlerRedirects(TestingT *t) {
    ARENA_BEGIN;
    HttpServeMux *mux = (HttpServeMux *)must(t, http_new_serve_mux(a));
    register_serve_mux(mux);

    static const struct {
        const char *method;
        const char *host;
        const char *url;
        Int code;
        bool redir_ok;
    } tests[] = {
        {"GET", "google.com", "/", 404, false},
        {"GET", "example.com", "/test/?example.com/test/", 200, false},
        {"GET", "example.com", "test/?example.com/test/", 200, true},
        {"GET", "google.com", "/pkg/bar//extra%2fpath", 200, true},
        {"GET", "google.com", "/dir/b%2fc/..", 200, true},
        {"GET", "google.com", "/doesnotexist/b%2fc/..", 404, true},
    };
    for (size_t i = 0; i < sizeof tests / sizeof tests[0]; i++) {
        int tries = 1; /* at most 1 redirect when redir_ok is true */
        Str turl = cs(tests[i].url);
        for (;;) {
            Error err = BURROW_NO_ERROR;
            Url *u = url_parse(a, turl, &err);
            if (u == NULL)
                testing_t_fatalf_v(t, "%s", error_text(err));
            HttpRequest r;
            memset(&r, 0, sizeof r);
            r.method = cs(tests[i].method);
            r.host = cs(tests[i].host);
            r.url = u;
            HttpHandler h = http_serve_mux_handler(mux, &r, a, NULL);
            Recorder rr;
            rec_init(t, a, &rr);
            http_handler_serve_http(h, rec_writer(&rr), &r);
            if (rr.code != 307) {
                if (rr.code != tests[i].code)
                    testing_t_errorf_v(t, "%s %s %s = %d, want %d", cs(tests[i].method),
                                       cs(tests[i].host), cs(tests[i].url), rr.code,
                                       tests[i].code);
                break;
            }
            if (!tests[i].redir_ok) {
                testing_t_errorf_v(t, "%s %s %s, unexpected redirect",
                                   cs(tests[i].method), cs(tests[i].host),
                                   cs(tests[i].url));
                break;
            }
            turl = http_header_get(rr.header, S("Location"));
            tries--;
        }
        if (tries < 0)
            testing_t_errorf_v(t, "%s %s %s, too many redirects", cs(tests[i].method),
                               cs(tests[i].host), cs(tests[i].url));
    }
    http_serve_mux_free(mux);
    ARENA_END;
}

static void TestServeMuxHandlerRedirectPost(TestingT *t) {
    ARENA_BEGIN;
    HttpServeMux *mux = (HttpServeMux *)must(t, http_new_serve_mux(a));
    static const Int ok = 200;
    http_serve_mux_handle_func(
        mux, S("POST /test/"),
        BURROW_FN(HttpHandlerFunc, serve_code, (void *)(uintptr_t)&ok));

    Int code = 0;
    int retries;
    Str start_url = S("http://example.com/test");
    Str req_url = start_url;
    for (retries = 0; retries <= 1; retries++) {
        HttpRequest *r = new_test_request(t, a, "POST", req_url);
        HttpHandler h = http_serve_mux_handler(mux, r, a, NULL);
        Recorder rr;
        rec_init(t, a, &rr);
        http_handler_serve_http(h, rec_writer(&rr), r);
        http_request_free(r);
        code = rr.code;
        if (rr.code == 307) {
            req_url = http_header_get(rr.header, S("Location"));
            continue;
        }
        if (rr.code != 200)
            testing_t_errorf_v(t, "unhandled response code: %d", rr.code);
    }
    if (code != 200)
        testing_t_errorf_v(t, "POST %s = %d after %d retries, want = 200", start_url,
                           code, (Int)retries);
    http_serve_mux_free(mux);
    ARENA_END;
}

/* Issue 900. */
static void TestMuxRedirectLeadingSlashes(TestingT *t) {
    ARENA_BEGIN;
    static const char *const paths[] = {"//foo.txt", "///foo.txt", "/../../foo.txt"};
    for (size_t i = 0; i < sizeof paths / sizeof paths[0]; i++) {
        Str raw =
            fmt_sprintf_v(a, "GET %s HTTP/1.1\r\nHost: test\r\n\r\n", cs(paths[i]));
        HttpRequest *req = read_req(t, a, raw);
        HttpServeMux *mux = (HttpServeMux *)must(t, http_new_serve_mux(a));
        Recorder resp;
        rec_init(t, a, &resp);

        http_serve_mux_serve_http(mux, rec_writer(&resp), req);
        http_request_free(req);
        http_serve_mux_free(mux);

        Str loc = http_header_get(resp.header, S("Location"));
        if (!str_eq(loc, S("/foo.txt"))) {
            testing_t_errorf_v(t, "Expected Location header set to %q; got %q",
                               S("/foo.txt"), loc);
            break;
        }
        if (resp.code != HTTP_STATUS_TEMPORARY_REDIRECT) {
            testing_t_errorf_v(
                t, "Expected response code of StatusPermanentRedirect; got %d",
                resp.code);
            break;
        }
    }
    ARENA_END;
}

static void TestServeWithSlashRedirectForHostPatterns(TestingT *t) {
    ARENA_BEGIN;
    HttpServeMux *mux = (HttpServeMux *)must(t, http_new_serve_mux(a));
    static const char *const pats[] = {
        "example.com/pkg/foo/", "example.com/pkg/bar",
        "example.com/pkg/bar/", "example.com:3000/pkg/connect/",
        "example.com:9000/",    "/pkg/baz/",
        "example.com/a%2fb/",
    };
    for (size_t i = 0; i < sizeof pats / sizeof pats[0]; i++)
        http_serve_mux_handle(mux, cs(pats[i]), string_handler(t, a, pats[i]));

    static const struct {
        const char *method;
        const char *url;
        const char *loc;
        const char *want;
        Int code;
    } tests[] = {
        {"GET", "http://example.com/", "", "", 404},
        {"GET", "http://example.com/pkg/foo", "/pkg/foo/", "", 307},
        {"GET", "http://example.com/pkg/bar", "", "example.com/pkg/bar", 200},
        {"GET", "http://example.com/pkg/bar/", "", "example.com/pkg/bar/", 200},
        {"GET", "http://example.com/pkg/baz", "/pkg/baz/", "", 307},
        {"GET", "http://example.com:3000/pkg/foo", "/pkg/foo/", "", 307},
        {"CONNECT", "http://example.com/", "", "", 404},
        {"CONNECT", "http://example.com:3000/", "", "", 404},
        {"CONNECT", "http://example.com:9000/", "", "example.com:9000/", 200},
        {"CONNECT", "http://example.com/pkg/foo", "/pkg/foo/", "", 307},
        {"CONNECT", "http://example.com:3000/pkg/foo", "", "", 404},
        {"CONNECT", "http://example.com:3000/pkg/baz", "/pkg/baz/", "", 307},
        {"CONNECT", "http://example.com:3000/pkg/connect", "/pkg/connect/", "", 307},
        {"GET", "http://example.com/a%2fb", "/a%2fb/", "", 307},
    };
    for (size_t i = 0; i < sizeof tests / sizeof tests[0]; i++) {
        HttpRequest *req = new_request(t, a, tests[i].method, cs(tests[i].url));
        Recorder w;
        rec_init(t, a, &w);
        http_serve_mux_serve_http(mux, rec_writer(&w), req);
        http_request_free(req);

        if (w.code != tests[i].code)
            testing_t_errorf_v(t, "#%d: Status = %d; want = %d", (Int)i, w.code,
                               tests[i].code);
        if (tests[i].code == 307) {
            Str got = http_header_get(w.header, S("Location"));
            if (!str_eq(got, cs(tests[i].loc)))
                testing_t_errorf_v(t, "#%d: Location = %q; want = %q", (Int)i, got,
                                   cs(tests[i].loc));
        } else {
            Str got = http_header_get(w.header, S("Result"));
            if (!str_eq(got, cs(tests[i].want)))
                testing_t_errorf_v(t, "#%d: Result = %q; want = %q", (Int)i, got,
                                   cs(tests[i].want));
        }
    }
    http_serve_mux_free(mux);
    ARENA_END;
}

/* No trailing-slash redirect for a path that already has a trailing slash.
 * Issue 65624. */
static void TestMuxNoSlashRedirectWithTrailingSlash(TestingT *t) {
    ARENA_BEGIN;
    HttpServeMux *mux = (HttpServeMux *)must(t, http_new_serve_mux(a));
    http_serve_mux_handle_func(mux, S("/{x}/"),
                               BURROW_FN(HttpHandlerFunc, serve_ok, NULL));
    Recorder w;
    rec_init(t, a, &w);
    HttpRequest *req = new_request(t, a, "GET", S("/"));
    http_serve_mux_serve_http(mux, rec_writer(&w), req);
    if (w.code != 404)
        testing_t_errorf_v(t, "got %d, want %d", w.code, (Int)404);
    http_request_free(req);
    http_serve_mux_free(mux);
    ARENA_END;
}

/* No trailing-slash 405 for a path that already has a trailing slash.
 * Issue 67657. */
static void TestMuxNoSlash405WithTrailingSlash(TestingT *t) {
    ARENA_BEGIN;
    HttpServeMux *mux = (HttpServeMux *)must(t, http_new_serve_mux(a));
    http_serve_mux_handle_func(mux, S("GET /{x}/"),
                               BURROW_FN(HttpHandlerFunc, serve_ok, NULL));
    Recorder w;
    rec_init(t, a, &w);
    HttpRequest *req = new_request(t, a, "GET", S("/"));
    http_serve_mux_serve_http(mux, rec_writer(&w), req);
    if (w.code != 404)
        testing_t_errorf_v(t, "got %d, want %d", w.code, (Int)404);
    http_request_free(req);
    http_serve_mux_free(mux);
    ARENA_END;
}

/* This used to crash. A path with no leading slash is not valid, but it
 * should not crash. */
static void TestRedirectBadPath(TestingT *t) {
    ARENA_BEGIN;
    Recorder rr;
    rec_init(t, a, &rr);
    HttpRequest req;
    Url u;
    zero_request(&req, &u, S("GET"), (Str){0}, S("not-empty-but-no-leading-slash"));
    u.scheme = S("http");
    http_redirect(rec_writer(&rr), &req, (Str){0}, 304);
    if (rr.code != 304)
        testing_t_errorf_v(t, "Code = %d; want 304", rr.code);
    ARENA_END;
}

static void TestRedirectEscapedPath(TestingT *t) {
    ARENA_BEGIN;
    Str base_url = S("http://example.com/foo%2Fbar/");
    Str redirect_url = S("qux%2Fbaz");
    HttpRequest *req = new_test_request(t, a, "GET", base_url);

    Recorder rr;
    rec_init(t, a, &rr);
    http_redirect(rec_writer(&rr), req, redirect_url, HTTP_STATUS_MOVED_PERMANENTLY);

    Str want_url = S("/foo%2Fbar/qux%2Fbaz");
    Str got = http_header_get(rr.header, S("Location"));
    if (!str_eq(got, want_url))
        testing_t_errorf_v(t, "Redirect(%s, %s) = %s, want = %s", base_url,
                           redirect_url, got, want_url);
    http_request_free(req);
    ARENA_END;
}

/* Different URL forms and schemes. */
static void TestRedirect(TestingT *t) {
    ARENA_BEGIN;
    HttpRequest *req = new_request(t, a, "GET", S("http://example.com/qux/"));

    static const struct {
        const char *in;
        const char *want;
    } tests[] = {
        /* normal http */
        {"http://foobar.com/baz", "http://foobar.com/baz"},
        /* normal https */
        {"https://foobar.com/baz", "https://foobar.com/baz"},
        /* custom scheme */
        {"test://foobar.com/baz", "test://foobar.com/baz"},
        /* schemeless */
        {"//foobar.com/baz", "//foobar.com/baz"},
        /* relative to the root */
        {"/foobar.com/baz", "/foobar.com/baz"},
        /* relative to the current path */
        {"foobar.com/baz", "/qux/foobar.com/baz"},
        /* relative to the current path, going upwards */
        {"../quux/foobar.com/baz", "/quux/foobar.com/baz"},
        /* incorrect number of slashes */
        {"///foobar.com/baz", "/foobar.com/baz"},

        /* No path_clean on the wrong parts in redirects. */
        {"/foo?next=http://bar.com/", "/foo?next=http://bar.com/"},
        {"http://localhost:8080/_ah/login?continue=http://localhost:8080/",
         "http://localhost:8080/_ah/login?continue=http://localhost:8080/"},

        /* "/фубар" */
        {"/\xd1\x84\xd1\x83\xd0\xb1\xd0\xb0\xd1\x80",
         "/%d1%84%d1%83%d0%b1%d0%b0%d1%80"},
        {"http://foo.com/\xd1\x84\xd1\x83\xd0\xb1\xd0\xb0\xd1\x80",
         "http://foo.com/%d1%84%d1%83%d0%b1%d0%b0%d1%80"},
    };
    for (size_t i = 0; i < sizeof tests / sizeof tests[0]; i++) {
        Recorder rec;
        rec_init(t, a, &rec);
        http_redirect(rec_writer(&rec), req, cs(tests[i].in), 302);
        if (rec.code != 302)
            testing_t_errorf_v(t, "Redirect(%q) generated status code %d; want %d",
                               cs(tests[i].in), rec.code, (Int)302);
        Str got = http_header_get(rec.header, S("Location"));
        if (!str_eq(got, cs(tests[i].want)))
            testing_t_errorf_v(t, "Redirect(%q) generated Location header %q; want %q",
                               cs(tests[i].in), got, cs(tests[i].want));
    }
    http_request_free(req);
    ARENA_END;
}

/* Redirect sets Content-Type for GET and HEAD and writes a short HTML body,
 * unless the response already has a Content-Type. Go tells a nil slice of
 * values from an empty one, and both are a key with no values here. */
static void TestRedirectContentTypeAndBody(TestingT *t) {
    ARENA_BEGIN;
    enum { CT_NONE, CT_VALUE, CT_EMPTY };
    static const struct {
        const char *method;
        const char *want_ct;
        const char *want_body;
        int ct;
    } tests[] = {
        {"GET", "text/html; charset=utf-8", "<a href=\"/foo\">Found</a>.\n\n", CT_NONE},
        {"HEAD", "text/html; charset=utf-8", "", CT_NONE},
        {"POST", "", "", CT_NONE},
        {"DELETE", "", "", CT_NONE},
        {"foo", "", "", CT_NONE},
        {"GET", "application/test", "", CT_VALUE},
        {"GET", "", "", CT_EMPTY},
        {"GET", "", "", CT_EMPTY},
    };
    for (size_t i = 0; i < sizeof tests / sizeof tests[0]; i++) {
        HttpRequest *req =
            new_test_request(t, a, tests[i].method, S("http://example.com/qux/"));
        Recorder rec;
        rec_init(t, a, &rec);
        if (tests[i].ct == CT_VALUE) {
            (void)http_header_set(rec.header, S("Content-Type"), S("application/test"));
        } else if (tests[i].ct == CT_EMPTY) {
            Str key = S("Content-Type");
            Slice none = slice_from(NULL, 0, 0, TYPE_STRING);
            if (!map_set(rec.header, &key, &none))
                testing_t_fatalf_v(t, "out of memory");
        }
        http_redirect(rec_writer(&rec), req, S("/foo"), 302);
        if (rec.code != 302)
            testing_t_errorf_v(t, "#%d: Redirect(%q) generated status code %d; want %d",
                               (Int)i, cs(tests[i].method), rec.code, (Int)302);
        Str ct = http_header_get(rec.header, S("Content-Type"));
        if (!str_eq(ct, cs(tests[i].want_ct)))
            testing_t_errorf_v(
                t, "#%d: Redirect(%q) generated Content-Type header %q; want %q",
                (Int)i, cs(tests[i].method), ct, cs(tests[i].want_ct));
        Str body = strings_builder_string(&rec.body);
        if (!str_eq(body, cs(tests[i].want_body)))
            testing_t_errorf_v(t, "#%d: Redirect(%q) generated Body %q; want %q",
                               (Int)i, cs(tests[i].method), body,
                               cs(tests[i].want_body));
        http_request_free(req);
    }
    ARENA_END;
}

static void strip_prefix_record(void *env, HttpResponseWriter w, HttpRequest *r) {
    (void)env;
    HttpHeader h = http_response_writer_header(w);
    (void)http_header_set(h, S("X-Path"), r->url->path);
    (void)http_header_set(h, S("X-RawPath"), r->url->raw_path);
}

static void TestStripPrefix(TestingT *t) {
    ARENA_BEGIN;
    HttpHandlerFunc f = BURROW_FN(HttpHandlerFunc, strip_prefix_record, NULL);
    HttpHandler h =
        http_strip_prefix(a, S("/foo/bar"), http_handler_func_as_handler(&f));
    if (h.data == NULL)
        testing_t_fatalf_v(t, "out of memory");

    static const struct {
        const char *req_path;
        const char *path; /* NULL for a 404 */
        const char *raw_path;
    } cases[] = {
        {"/foo/bar/qux", "/qux", ""},
        {"/foo/bar%2Fqux", "/qux", "%2Fqux"},
        {"/foo%2Fbar/qux", NULL, ""}, /* An escaped prefix does not match. */
        {"/bar", NULL, ""},           /* No prefix match. */
    };
    for (size_t i = 0; i < sizeof cases / sizeof cases[0]; i++) {
        Str raw = fmt_sprintf_v(a, "GET %s HTTP/1.1\r\nHost: test\r\n\r\n",
                                cs(cases[i].req_path));
        HttpRequest *req = read_req(t, a, raw);
        Recorder rec;
        rec_init(t, a, &rec);
        http_handler_serve_http(h, rec_writer(&rec), req);
        if (cases[i].path == NULL) {
            if (rec.code != HTTP_STATUS_NOT_FOUND)
                testing_t_errorf_v(t, "%s: got %d, want 404 Not Found",
                                   cs(cases[i].req_path), rec.code);
        } else if (rec.code != HTTP_STATUS_OK) {
            testing_t_errorf_v(t, "%s: got %d, want 200 OK", cs(cases[i].req_path),
                               rec.code);
        } else {
            Str g = http_header_get(rec.header, S("X-Path"));
            if (!str_eq(g, cs(cases[i].path)))
                testing_t_errorf_v(t, "%s: got Path %q, want %q", cs(cases[i].req_path),
                                   g, cs(cases[i].path));
            g = http_header_get(rec.header, S("X-RawPath"));
            if (!str_eq(g, cs(cases[i].raw_path)))
                testing_t_errorf_v(t, "%s: got RawPath %q, want %q",
                                   cs(cases[i].req_path), g, cs(cases[i].raw_path));
        }
        http_request_free(req);
    }
    ARENA_END;
}

/* Issue 18952. */
static void TestStripPrefixNotModifyRequest(TestingT *t) {
    ARENA_BEGIN;
    HttpHandler h = http_strip_prefix(a, S("/foo"), http_not_found_handler());
    if (h.data == NULL)
        testing_t_fatalf_v(t, "out of memory");
    HttpRequest *req = new_test_request(t, a, "GET", S("/foo/bar"));
    Recorder rec;
    rec_init(t, a, &rec);
    http_handler_serve_http(h, rec_writer(&rec), req);
    if (!str_eq(req->url->path, S("/foo/bar")))
        testing_t_errorf_v(
            t, "StripPrefix should not modify the provided Request, but it did");
    http_request_free(req);
    ARENA_END;
}

static void TestMuxRedirectRelative(TestingT *t) {
    ARENA_BEGIN;
    HttpRequest *req =
        read_req(t, a, S("GET http://example.com HTTP/1.1\r\nHost: test\r\n\r\n"));
    HttpServeMux *mux = (HttpServeMux *)must(t, http_new_serve_mux(a));
    Recorder resp;
    rec_init(t, a, &resp);
    http_serve_mux_serve_http(mux, rec_writer(&resp), req);
    Str got = http_header_get(resp.header, S("Location"));
    if (!str_eq(got, S("/")))
        testing_t_errorf_v(t, "Location header expected %q; got %q", S("/"), got);
    if (resp.code != HTTP_STATUS_TEMPORARY_REDIRECT)
        testing_t_errorf_v(t, "Expected response code %d; got %d",
                           (Int)HTTP_STATUS_TEMPORARY_REDIRECT, resp.code);
    http_request_free(req);
    http_serve_mux_free(mux);
    ARENA_END;
}

static void TestError(TestingT *t) {
    ARENA_BEGIN;
    Recorder w;
    rec_init(t, a, &w);
    (void)http_header_set(w.header, S("Content-Length"), S("1"));
    (void)http_header_set(w.header, S("X-Content-Type-Options"),
                          S("scratch and sniff"));
    (void)http_header_set(w.header, S("Other"), S("foo"));
    http_error(rec_writer(&w), S("oops"), 432);

    if (burrow__http_header_has(w.header, S("Content-Length")))
        testing_t_errorf_v(t, "Content-Length: %q, want not present",
                           http_header_get(w.header, S("Content-Length")));
    Str v = http_header_get(w.header, S("Content-Type"));
    if (!str_eq(v, S("text/plain; charset=utf-8")))
        testing_t_errorf_v(t, "Content-Type: %q, want %q", v,
                           S("text/plain; charset=utf-8"));
    v = http_header_get(w.header, S("X-Content-Type-Options"));
    if (!str_eq(v, S("nosniff")))
        testing_t_errorf_v(t, "X-Content-Type-Options: %q, want %q", v, S("nosniff"));
    ARENA_END;
}

/* ----------------------------------------------------------- request_test.go */

/* PathValue and SetPathValue on a request that was never matched. */
static void TestPathValueNoMatch(TestingT *t) {
    ARENA_BEGIN;
    HttpRequest r;
    memset(&r, 0, sizeof r);
    Str g = http_request_path_value(&r, S("x"));
    if (g.len != 0)
        testing_t_errorf_v(t, "got %q, want %q", g, S(""));
    if (!http_request_set_path_value(&r, a, S("x"), S("a")))
        testing_t_fatalf_v(t, "out of memory");
    g = http_request_path_value(&r, S("x"));
    if (!str_eq(g, S("a")))
        testing_t_errorf_v(t, "got %q, want %q", g, S("a"));
    ARENA_END;
}

typedef struct PathValue {
    const char *name;
    const char *want;
} PathValue;

typedef struct PathValueCase {
    const char *pattern;
    const char *url;
    PathValue want[5]; /* up to the first with a NULL name */
} PathValueCase;

typedef struct PathValueRun {
    TestingT *t;
    const PathValueCase *tc;
    bool called;
} PathValueRun;

static void path_value_check(void *env, HttpResponseWriter w, HttpRequest *r) {
    (void)w;
    PathValueRun *run = (PathValueRun *)env;
    run->called = true;
    const PathValueCase *tc = run->tc;
    for (size_t i = 0;
         i < sizeof tc->want / sizeof tc->want[0] && tc->want[i].name != NULL; i++) {
        Str got = http_request_path_value(r, cs(tc->want[i].name));
        if (!str_eq(got, cs(tc->want[i].want)))
            testing_t_errorf_v(run->t, "%q, %q: got %q, want %q", cs(tc->pattern),
                               cs(tc->want[i].name), got, cs(tc->want[i].want));
    }
    if (!str_eq(r->pattern, cs(tc->pattern)))
        testing_t_errorf_v(run->t, "pattern: got %s, want %s", r->pattern,
                           cs(tc->pattern));
}

static void TestPathValueAndPattern(TestingT *t) {
    ARENA_BEGIN;
    static const PathValueCase tests[] = {
        {"/{a}/is/{b}/{c...}",
         "/now/is/the/time/for/all",
         {{"a", "now"}, {"b", "the"}, {"c", "time/for/all"}, {"d", ""}}},
        {"/names/{name}/{other...}",
         "/names/%2fjohn/address",
         {{"name", "/john"}, {"other", "address"}}},
        {"/names/{name}/{other...}",
         "/names/john%2Fdoe/there/is%2F/more",
         {{"name", "john/doe"}, {"other", "there/is//more"}}},
        {"/names/{name}/{other...}", "/names/n/*", {{"name", "n"}, {"other", "*"}}},
    };
    for (size_t i = 0; i < sizeof tests / sizeof tests[0]; i++) {
        HttpServeMux *mux = (HttpServeMux *)must(t, http_new_serve_mux(a));
        PathValueRun run = {t, &tests[i], false};
        http_serve_mux_handle_func(mux, cs(tests[i].pattern),
                                   BURROW_FN(HttpHandlerFunc, path_value_check, &run));
        Str raw =
            fmt_sprintf_v(a, "GET %s HTTP/1.1\r\nHost: test\r\n\r\n", cs(tests[i].url));
        HttpRequest *req = read_req(t, a, raw);
        Recorder rec;
        rec_init(t, a, &rec);
        http_serve_mux_serve_http(mux, rec_writer(&rec), req);
        if (!run.called)
            testing_t_errorf_v(t, "%q: the handler was not called, got %d",
                               cs(tests[i].url), rec.code);
        http_request_free(req);
        http_serve_mux_free(mux);
    }
    ARENA_END;
}

typedef struct SetPathValueRun {
    TestingT *t;
    Alloc *a;
    bool called;
} SetPathValueRun;

static void set_path_value_check(void *env, HttpResponseWriter w, HttpRequest *r) {
    (void)w;
    SetPathValueRun *run = (SetPathValueRun *)env;
    run->called = true;
    static const PathValue kvs[] = {{"b", "X"}, {"d", "Y"}, {"a", "Z"}};
    for (size_t i = 0; i < sizeof kvs / sizeof kvs[0]; i++)
        if (!http_request_set_path_value(r, run->a, cs(kvs[i].name), cs(kvs[i].want)))
            testing_t_fatalf_v(run->t, "out of memory");
    for (size_t i = 0; i < sizeof kvs / sizeof kvs[0]; i++) {
        Str g = http_request_path_value(r, cs(kvs[i].name));
        if (!str_eq(g, cs(kvs[i].want)))
            testing_t_errorf_v(run->t, "got %q, want %q", g, cs(kvs[i].want));
    }
}

static void TestSetPathValue(TestingT *t) {
    ARENA_BEGIN;
    HttpServeMux *mux = (HttpServeMux *)must(t, http_new_serve_mux(a));
    SetPathValueRun run = {t, a, false};
    http_serve_mux_handle_func(mux, S("/a/{b}/c/{d...}"),
                               BURROW_FN(HttpHandlerFunc, set_path_value_check, &run));
    HttpRequest *req =
        read_req(t, a, S("GET /a/b/c/d/e HTTP/1.1\r\nHost: test\r\n\r\n"));
    Recorder rec;
    rec_init(t, a, &rec);
    http_serve_mux_serve_http(mux, rec_writer(&rec), req);
    if (!run.called)
        testing_t_errorf_v(t, "the handler was not called, got %d", rec.code);
    http_request_free(req);
    http_serve_mux_free(mux);
    ARENA_END;
}

/* The 405 responses and the Allow field, mostly. */
static void TestStatus(TestingT *t) {
    ARENA_BEGIN;
    HttpHandlerFunc f = BURROW_FN(HttpHandlerFunc, serve_nothing, NULL);
    HttpHandler h = http_handler_func_as_handler(&f);
    HttpServeMux *mux = (HttpServeMux *)must(t, http_new_serve_mux(a));
    http_serve_mux_handle(mux, S("GET /g"), h);
    http_serve_mux_handle(mux, S("POST /p"), h);
    http_serve_mux_handle(mux, S("PATCH /p"), h);
    http_serve_mux_handle(mux, S("PUT /r"), h);
    http_serve_mux_handle(mux, S("GET /r/"), h);

    static const struct {
        const char *method;
        const char *path;
        const char *want_allow;
        Int want_status;
    } tests[] = {
        {"GET", "/g", "", 200},
        {"HEAD", "/g", "", 200},
        {"POST", "/g", "GET, HEAD", 405},
        {"GET", "/x", "", 404},
        {"GET", "/p", "PATCH, POST", 405},
        {"GET", "/./p", "PATCH, POST", 405},
        {"GET", "/r/", "", 200},
        {"GET", "/r", "", 200}, /* redirected */
        {"HEAD", "/r/", "", 200},
        {"HEAD", "/r", "", 200}, /* redirected */
        {"PUT", "/r/", "GET, HEAD", 405},
        {"PUT", "/r", "", 200},
    };
    for (size_t i = 0; i < sizeof tests / sizeof tests[0]; i++) {
        Str path = cs(tests[i].path);
        Recorder rec;
        /* The client follows a 307 with the same method, and gives up
         * after 10. */
        for (int n = 0;; n++) {
            Str raw = fmt_sprintf_v(a, "%s %s HTTP/1.1\r\nHost: test\r\n\r\n",
                                    cs(tests[i].method), path);
            HttpRequest *req = read_req(t, a, raw);
            rec_init(t, a, &rec);
            http_serve_mux_serve_http(mux, rec_writer(&rec), req);
            http_request_free(req);
            if (rec.code != HTTP_STATUS_TEMPORARY_REDIRECT || n == 10)
                break;
            path = http_header_get(rec.header, S("Location"));
        }
        if (rec.code != tests[i].want_status)
            testing_t_errorf_v(t, "%s %s: got %d, want %d", cs(tests[i].method),
                               cs(tests[i].path), rec.code, tests[i].want_status);
        Str g = http_header_get(rec.header, S("Allow"));
        if (!str_eq(g, cs(tests[i].want_allow)))
            testing_t_errorf_v(t, "%s %s, Allow: got %q, want %q", cs(tests[i].method),
                               cs(tests[i].path), g, cs(tests[i].want_allow));
    }
    http_serve_mux_free(mux);
    ARENA_END;
}

#define TESTS(X)                                                                       \
    X(TestFindHandler)                                                                 \
    X(TestEmptyServeMux)                                                               \
    X(TestRegisterErr)                                                                 \
    X(TestExactMatch)                                                                  \
    X(TestEscapedPathsAndPatterns)                                                     \
    X(TestCleanPath)                                                                   \
    X(TestRegisterConflict)                                                            \
    X(TestServeMuxHandler)                                                             \
    X(TestServeMuxHandlerTrailingSlash)                                                \
    X(TestServeMuxHandleFuncWithNilHandler)                                            \
    X(TestServeMuxHandlerRedirects)                                                    \
    X(TestServeMuxHandlerRedirectPost)                                                 \
    X(TestMuxRedirectLeadingSlashes)                                                   \
    X(TestServeWithSlashRedirectForHostPatterns)                                       \
    X(TestMuxNoSlashRedirectWithTrailingSlash)                                         \
    X(TestMuxNoSlash405WithTrailingSlash)                                              \
    X(TestRedirectBadPath)                                                             \
    X(TestRedirectEscapedPath)                                                         \
    X(TestRedirect)                                                                    \
    X(TestRedirectContentTypeAndBody)                                                  \
    X(TestStripPrefix)                                                                 \
    X(TestStripPrefixNotModifyRequest)                                                 \
    X(TestMuxRedirectRelative)                                                         \
    X(TestError)                                                                       \
    X(TestPathValueNoMatch)                                                            \
    X(TestPathValueAndPattern)                                                         \
    X(TestSetPathValue)                                                                \
    X(TestStatus)

TESTING_MAIN(TESTS)
