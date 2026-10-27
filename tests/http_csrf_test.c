/* Derived from Go's src/net/http/csrf_test.go.
 * Go source: go1.27.3.
 *
 * Copyright 2025 The Go Authors. All rights reserved.
 * Copyright 2026 The burrow Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style licence that can be found
 * in the LICENSE file. */

#include "check.h"

#include "burrow/burrow.h"
#include "burrow/bytes.h"
#include "burrow/chan.h"
#include "burrow/func.h"
#include "burrow/io.h"
#include "burrow/mem/arena.h"
#include "burrow/mem/heap.h"
#include "burrow/net/http.h"
#include "burrow/net/http/httptest.h"
#include "burrow/net/url.h"
#include "burrow/proc.h"
#include "burrow/slice.h"
#include "burrow/strings.h"
#include "burrow/testing.h"

#include <stdint.h>
#include <string.h>

#define S BURROW_S

static Str cs(const char *s) {
    return s == NULL ? BURROW_STR_EMPTY : str_from_cstr(s);
}

/* a + b, in a. */
static Str cat(Alloc *a, Str x, Str y) {
    Str parts[2] = {x, y};
    return strings_join(a, slice_from(parts, 2, 2, TYPE_STRING), BURROW_STR_EMPTY);
}

/* httptestNewRequest works around https://go.dev/issue/73151. */
static HttpRequest *httptest_new_request_no_host(Alloc *a, Str method, Str target) {
    HttpRequest *req = httptest_new_request(a, method, target, (IoReader){0});
    req->url->scheme = BURROW_STR_EMPTY;
    req->url->host = BURROW_STR_EMPTY;
    return req;
}

static void ok_handler_serve(void *env, HttpResponseWriter w, HttpRequest *r) {
    (void)env;
    (void)r;
    http_response_writer_write_header(w, HTTP_STATUS_OK);
}

static HttpHandlerFunc ok_handler_func = {ok_handler_serve, NULL};

static HttpHandler ok_handler(void) {
    return http_handler_func_as_handler(&ok_handler_func);
}

/* The status h gives req, on a new recorder. */
static Int serve_code(HttpHandler h, HttpRequest *req) {
    HttptestResponseRecorder *w = httptest_new_recorder(heap_allocator());
    http_handler_serve_http(h, httptest_response_recorder_as_response_writer(w), req);
    Int code = w->code;
    httptest_response_recorder_free(w);
    return code;
}

/* ------------------------------------------------------------- SecFetchSite */

typedef struct SecFetchSiteTest {
    const char *name;
    const char *method;
    const char *sec_fetch_site;
    const char *origin;
    Int expected_status;
} SecFetchSiteTest;

static const SecFetchSiteTest sec_fetch_site_tests[] = {
    {"same-origin allowed", "POST", "same-origin", "", HTTP_STATUS_OK},
    {"none allowed", "POST", "none", "", HTTP_STATUS_OK},
    {"cross-site blocked", "POST", "cross-site", "", HTTP_STATUS_FORBIDDEN},
    {"same-site blocked", "POST", "same-site", "", HTTP_STATUS_FORBIDDEN},

    {"no header with no origin", "POST", "", "", HTTP_STATUS_OK},
    {"no header with matching origin", "POST", "", "https://example.com",
     HTTP_STATUS_OK},
    {"no header with mismatched origin", "POST", "", "https://attacker.example",
     HTTP_STATUS_FORBIDDEN},
    {"no header with null origin", "POST", "", "null", HTTP_STATUS_FORBIDDEN},

    {"GET allowed", "GET", "cross-site", "", HTTP_STATUS_OK},
    {"HEAD allowed", "HEAD", "cross-site", "", HTTP_STATUS_OK},
    {"OPTIONS allowed", "OPTIONS", "cross-site", "", HTTP_STATUS_OK},
    {"PUT blocked", "PUT", "cross-site", "", HTTP_STATUS_FORBIDDEN},
};

typedef struct SecFetchSiteEnv {
    const SecFetchSiteTest *tc;
    HttpHandler handler;
} SecFetchSiteEnv;

static void sec_fetch_site_case(void *env, TestingT *t) {
    const SecFetchSiteEnv *e = (const SecFetchSiteEnv *)env;
    const SecFetchSiteTest *tc = e->tc;
    HttpRequest *req = httptest_new_request_no_host(heap_allocator(), cs(tc->method),
                                                    S("https://example.com/"));
    if (tc->sec_fetch_site[0] != '\0')
        http_header_set(req->header, S("Sec-Fetch-Site"), cs(tc->sec_fetch_site));
    if (tc->origin[0] != '\0')
        http_header_set(req->header, S("Origin"), cs(tc->origin));

    Int code = serve_code(e->handler, req);

    if (code != tc->expected_status)
        testing_t_errorf_v(t, "got status %d, want %d", code, tc->expected_status);
    http_request_free(req);
}

static void TestCrossOriginProtectionSecFetchSite(TestingT *t) {
    HttpCrossOriginProtection *protection =
        http_new_cross_origin_protection(heap_allocator());
    HttpHandler handler =
        http_cross_origin_protection_handler(protection, ok_handler());

    for (size_t i = 0; i < sizeof sec_fetch_site_tests / sizeof sec_fetch_site_tests[0];
         i++) {
        SecFetchSiteEnv env = {&sec_fetch_site_tests[i], handler};
        (void)testing_t_run(t, cs(sec_fetch_site_tests[i].name),
                            BURROW_FN(TestingTFunc, sec_fetch_site_case, &env));
    }
    http_cross_origin_protection_free(protection);
}

/* ------------------------------------------------------- TrustedOriginBypass */

typedef struct TrustedOriginTest {
    const char *name;
    const char *origin;
    const char *sec_fetch_site;
    Int expected_status;
} TrustedOriginTest;

static const TrustedOriginTest trusted_origin_tests[] = {
    {"trusted origin without sec-fetch-site", "https://trusted.example", "",
     HTTP_STATUS_OK},
    {"trusted origin with cross-site", "https://trusted.example", "cross-site",
     HTTP_STATUS_OK},
    {"untrusted origin without sec-fetch-site", "https://attacker.example", "",
     HTTP_STATUS_FORBIDDEN},
    {"untrusted origin with cross-site", "https://attacker.example", "cross-site",
     HTTP_STATUS_FORBIDDEN},
};

typedef struct TrustedOriginEnv {
    const TrustedOriginTest *tc;
    HttpHandler handler;
} TrustedOriginEnv;

static void trusted_origin_case(void *env, TestingT *t) {
    const TrustedOriginEnv *e = (const TrustedOriginEnv *)env;
    const TrustedOriginTest *tc = e->tc;
    HttpRequest *req = httptest_new_request_no_host(heap_allocator(), S("POST"),
                                                    S("https://example.com/"));
    http_header_set(req->header, S("Origin"), cs(tc->origin));
    if (tc->sec_fetch_site[0] != '\0')
        http_header_set(req->header, S("Sec-Fetch-Site"), cs(tc->sec_fetch_site));

    Int code = serve_code(e->handler, req);

    if (code != tc->expected_status)
        testing_t_errorf_v(t, "got status %d, want %d", code, tc->expected_status);
    http_request_free(req);
}

static void TestCrossOriginProtectionTrustedOriginBypass(TestingT *t) {
    HttpCrossOriginProtection *protection =
        http_new_cross_origin_protection(heap_allocator());
    Error err = http_cross_origin_protection_add_trusted_origin(
        protection, S("https://trusted.example"));
    if (BURROW_FAILED(err)) {
        testing_t_fatalf_v(t, "AddTrustedOrigin: %v", err);
        http_cross_origin_protection_free(protection);
        return;
    }
    HttpHandler handler =
        http_cross_origin_protection_handler(protection, ok_handler());

    for (size_t i = 0; i < sizeof trusted_origin_tests / sizeof trusted_origin_tests[0];
         i++) {
        TrustedOriginEnv env = {&trusted_origin_tests[i], handler};
        (void)testing_t_run(t, cs(trusted_origin_tests[i].name),
                            BURROW_FN(TestingTFunc, trusted_origin_case, &env));
    }
    http_cross_origin_protection_free(protection);
}

/* ------------------------------------------------------------- PatternBypass */

typedef struct PatternBypassTest {
    const char *name;
    const char *path;
    const char *sec_fetch_site;
    Int expected_status;
} PatternBypassTest;

static const PatternBypassTest pattern_bypass_tests[] = {
    {"bypass path without sec-fetch-site", "/bypass/", "", HTTP_STATUS_OK},
    {"bypass path with cross-site", "/bypass/", "cross-site", HTTP_STATUS_OK},
    {"non-bypass path without sec-fetch-site", "/api/", "", HTTP_STATUS_FORBIDDEN},
    {"non-bypass path with cross-site", "/api/", "cross-site", HTTP_STATUS_FORBIDDEN},

    {"redirect to bypass path without ..", "/foo/../bypass/bar", "",
     HTTP_STATUS_FORBIDDEN},
    {"redirect to bypass path with trailing slash", "/bypass", "",
     HTTP_STATUS_FORBIDDEN},
    {"redirect to non-bypass path with ..", "/foo/../api/bar", "",
     HTTP_STATUS_FORBIDDEN},
    {"redirect to non-bypass path with trailing slash", "/api", "",
     HTTP_STATUS_FORBIDDEN},

    {"wildcard bypass", "/only/123", "", HTTP_STATUS_OK},
    {"non-wildcard", "/only/123/foo", "", HTTP_STATUS_FORBIDDEN},

    /* https://go.dev/issue/75054 */
    {"no trailing slash exact match", "/no-trailing", "", HTTP_STATUS_OK},
    {"no trailing slash with slash", "/no-trailing/", "", HTTP_STATUS_FORBIDDEN},
    {"yes trailing slash exact match", "/yes-trailing/", "", HTTP_STATUS_OK},
    {"yes trailing slash without slash", "/yes-trailing", "", HTTP_STATUS_FORBIDDEN},

    {"method-specific hit", "/post-only/", "", HTTP_STATUS_OK},
    {"method-specific miss (PUT)", "/put-only/", "", HTTP_STATUS_FORBIDDEN},
    {"method-specific miss (GET)", "/get-only/", "", HTTP_STATUS_FORBIDDEN},
};

typedef struct PatternBypassEnv {
    const PatternBypassTest *tc;
    HttpHandler handler;
} PatternBypassEnv;

static void pattern_bypass_case(void *env, TestingT *t) {
    const PatternBypassEnv *e = (const PatternBypassEnv *)env;
    const PatternBypassTest *tc = e->tc;
    Arena ar;
    arena_init(&ar, NULL, 0);
    Str target = cat(arena_allocator(&ar), S("https://example.com"), cs(tc->path));
    HttpRequest *req =
        httptest_new_request_no_host(heap_allocator(), S("POST"), target);
    http_header_set(req->header, S("Origin"), S("https://attacker.example"));
    if (tc->sec_fetch_site[0] != '\0')
        http_header_set(req->header, S("Sec-Fetch-Site"), cs(tc->sec_fetch_site));

    Int code = serve_code(e->handler, req);

    if (code != tc->expected_status)
        testing_t_errorf_v(t, "got status %d, want %d", code, tc->expected_status);
    http_request_free(req);
    arena_free(&ar);
}

static void TestCrossOriginProtectionPatternBypass(TestingT *t) {
    HttpCrossOriginProtection *protection =
        http_new_cross_origin_protection(heap_allocator());
    http_cross_origin_protection_add_insecure_bypass_pattern(protection, S("/bypass/"));
    http_cross_origin_protection_add_insecure_bypass_pattern(protection,
                                                             S("/only/{foo}"));
    http_cross_origin_protection_add_insecure_bypass_pattern(protection,
                                                             S("/no-trailing"));
    http_cross_origin_protection_add_insecure_bypass_pattern(protection,
                                                             S("/yes-trailing/"));
    http_cross_origin_protection_add_insecure_bypass_pattern(protection,
                                                             S("PUT /put-only/"));
    http_cross_origin_protection_add_insecure_bypass_pattern(protection,
                                                             S("GET /get-only/"));
    http_cross_origin_protection_add_insecure_bypass_pattern(protection,
                                                             S("POST /post-only/"));
    HttpHandler handler =
        http_cross_origin_protection_handler(protection, ok_handler());

    for (size_t i = 0; i < sizeof pattern_bypass_tests / sizeof pattern_bypass_tests[0];
         i++) {
        PatternBypassEnv env = {&pattern_bypass_tests[i], handler};
        (void)testing_t_run(t, cs(pattern_bypass_tests[i].name),
                            BURROW_FN(TestingTFunc, pattern_bypass_case, &env));
    }
    http_cross_origin_protection_free(protection);
}

/* ------------------------------------------------------------ SetDenyHandler */

static void custom_err_handler_serve(void *env, HttpResponseWriter w, HttpRequest *r) {
    (void)env;
    (void)r;
    http_response_writer_write_header(w, HTTP_STATUS_TEAPOT);
    (void)io_write_string(http_response_writer_as_io_writer(w), S("custom error"),
                          NULL);
}

static HttpHandlerFunc custom_err_handler_func = {custom_err_handler_serve, NULL};

static void TestCrossOriginProtectionSetDenyHandler(TestingT *t) {
    Alloc *a = heap_allocator();
    HttpCrossOriginProtection *protection = http_new_cross_origin_protection(a);

    HttpHandler handler =
        http_cross_origin_protection_handler(protection, ok_handler());

    HttpRequest *req =
        httptest_new_request_no_host(a, S("POST"), S("https://example.com/"));
    http_header_set(req->header, S("Sec-Fetch-Site"), S("cross-site"));

    HttptestResponseRecorder *w = httptest_new_recorder(a);
    http_handler_serve_http(handler, httptest_response_recorder_as_response_writer(w),
                            req);

    if (w->code != HTTP_STATUS_FORBIDDEN)
        testing_t_errorf_v(t, "got status %d, want %d", w->code, HTTP_STATUS_FORBIDDEN);
    httptest_response_recorder_free(w);

    HttpHandler custom_err_handler =
        http_handler_func_as_handler(&custom_err_handler_func);
    (void)http_cross_origin_protection_set_deny_handler(protection, custom_err_handler);

    w = httptest_new_recorder(a);
    http_handler_serve_http(handler, httptest_response_recorder_as_response_writer(w),
                            req);

    if (w->code != HTTP_STATUS_TEAPOT)
        testing_t_errorf_v(t, "got status %d, want %d", w->code, HTTP_STATUS_TEAPOT);

    Arena ar;
    arena_init(&ar, NULL, 0);
    Str body = bytes_buffer_string(w->body, arena_allocator(&ar));
    if (!strings_contains(body, S("custom error")))
        testing_t_errorf_v(t, "expected custom error message, got: %q", body);
    arena_free(&ar);
    httptest_response_recorder_free(w);
    http_request_free(req);

    req = httptest_new_request_no_host(a, S("GET"), S("https://example.com/"));

    w = httptest_new_recorder(a);
    http_handler_serve_http(handler, httptest_response_recorder_as_response_writer(w),
                            req);

    if (w->code != HTTP_STATUS_OK)
        testing_t_errorf_v(t, "got status %d, want %d", w->code, HTTP_STATUS_OK);
    httptest_response_recorder_free(w);
    http_request_free(req);

    (void)http_cross_origin_protection_set_deny_handler(protection, (HttpHandler){0});

    req = httptest_new_request_no_host(a, S("POST"), S("https://example.com/"));
    http_header_set(req->header, S("Sec-Fetch-Site"), S("cross-site"));

    w = httptest_new_recorder(a);
    http_handler_serve_http(handler, httptest_response_recorder_as_response_writer(w),
                            req);

    if (w->code != HTTP_STATUS_FORBIDDEN)
        testing_t_errorf_v(t, "got status %d, want %d", w->code, HTTP_STATUS_FORBIDDEN);
    httptest_response_recorder_free(w);
    http_request_free(req);
    http_cross_origin_protection_free(protection);
}

/* ---------------------------------------------------- AddTrustedOriginErrors */

typedef struct AddTrustedOriginTest {
    const char *name;
    const char *origin;
    bool want_err;
} AddTrustedOriginTest;

static const AddTrustedOriginTest add_trusted_origin_tests[] = {
    {"valid origin", "https://example.com", false},
    {"valid origin with port", "https://example.com:8080", false},
    {"http origin", "http://example.com", false},
    {"missing scheme", "example.com", true},
    {"missing host", "https://", true},
    {"trailing slash", "https://example.com/", true},
    {"with path", "https://example.com/path", true},
    {"with query", "https://example.com?query=value", true},
    {"with fragment", "https://example.com#fragment", true},
    {"invalid url", "https://ex ample.com", true},
    {"empty string", "", true},
    {"null", "null", true},
};

typedef struct AddTrustedOriginEnv {
    const AddTrustedOriginTest *tc;
    HttpCrossOriginProtection *protection;
} AddTrustedOriginEnv;

static void add_trusted_origin_case(void *env, TestingT *t) {
    const AddTrustedOriginEnv *e = (const AddTrustedOriginEnv *)env;
    const AddTrustedOriginTest *tc = e->tc;
    Error err =
        http_cross_origin_protection_add_trusted_origin(e->protection, cs(tc->origin));
    if (BURROW_FAILED(err) != tc->want_err)
        testing_t_errorf_v(t, "AddTrustedOrigin(%q) error = %v, wantErr %v",
                           cs(tc->origin), err, tc->want_err);
}

static void TestCrossOriginProtectionAddTrustedOriginErrors(TestingT *t) {
    HttpCrossOriginProtection *protection =
        http_new_cross_origin_protection(heap_allocator());

    for (size_t i = 0;
         i < sizeof add_trusted_origin_tests / sizeof add_trusted_origin_tests[0];
         i++) {
        AddTrustedOriginEnv env = {&add_trusted_origin_tests[i], protection};
        (void)testing_t_run(t, cs(add_trusted_origin_tests[i].name),
                            BURROW_FN(TestingTFunc, add_trusted_origin_case, &env));
    }
    http_cross_origin_protection_free(protection);
}

/* ---------------------------------------------- AddingBypassesConcurrently */

typedef struct ConcurrentEnv {
    HttpHandler handler;
    HttpRequest *req;
    Chan *start;
    Chan *done;
} ConcurrentEnv;

static void concurrent_requests(void *env) {
    ConcurrentEnv *e = (ConcurrentEnv *)env;
    chan_close(e->start);
    for (int i = 0; i < 10; i++)
        (void)serve_code(e->handler, e->req);
    chan_close(e->done);
}

static void TestCrossOriginProtectionAddingBypassesConcurrently(TestingT *t) {
    Alloc *a = heap_allocator();
    HttpCrossOriginProtection *protection = http_new_cross_origin_protection(a);
    HttpHandler handler =
        http_cross_origin_protection_handler(protection, ok_handler());

    HttpRequest *req =
        httptest_new_request_no_host(a, S("POST"), S("https://example.com/"));
    http_header_set(req->header, S("Origin"), S("https://concurrent.example"));
    http_header_set(req->header, S("Sec-Fetch-Site"), S("cross-site"));

    Int code = serve_code(handler, req);

    if (code != HTTP_STATUS_FORBIDDEN)
        testing_t_errorf_v(t, "got status %d, want %d", code, HTTP_STATUS_FORBIDDEN);

    ConcurrentEnv env = {handler, req, chan_make(a, TYPE_BOOL, 0),
                         chan_make(a, TYPE_BOOL, 0)};
    if (!go(BURROW_FN(Func, concurrent_requests, &env))) {
        testing_t_fatalf_v(t, "go: out of memory");
        return;
    }

    /* Add bypasses while the requests are in flight. */
    bool v;
    (void)chan_recv(env.start, &v);
    (void)http_cross_origin_protection_add_trusted_origin(
        protection, S("https://concurrent.example"));
    http_cross_origin_protection_add_insecure_bypass_pattern(protection, S("/foo/"));
    (void)chan_recv(env.done, &v);

    code = serve_code(handler, req);

    if (code != HTTP_STATUS_OK)
        testing_t_errorf_v(t,
                           "After concurrent bypass addition, got status %d, want %d",
                           code, HTTP_STATUS_OK);
    chan_free(env.start);
    chan_free(env.done);
    http_request_free(req);
    http_cross_origin_protection_free(protection);
}

/* -------------------------------------------------------------------- Server */

typedef struct ServerTest {
    const char *name;
    const char *method;
    const char *path;   /* after ts.URL */
    bool origin_is_url; /* origin is ts.URL */
    const char *origin;
    const char *sec_fetch_site;
    Int expected_status;
} ServerTest;

static const ServerTest server_tests[] = {
    {"cross-site", "POST", "", false, "https://attacker.example", "cross-site",
     HTTP_STATUS_FORBIDDEN},
    {"same-origin", "POST", "", false, "", "same-origin", HTTP_STATUS_OK},
    {"origin matches host", "POST", "", true, "", "", HTTP_STATUS_OK},
    {"trusted origin", "POST", "", false, "https://trusted.example", "",
     HTTP_STATUS_OK},
    {"untrusted origin", "POST", "", false, "https://attacker.example", "",
     HTTP_STATUS_FORBIDDEN},
    {"bypass path", "POST", "/bypass/", false, "https://attacker.example", "",
     HTTP_STATUS_OK},
};

typedef struct ServerEnv {
    const ServerTest *tc;
    HttptestServer *ts;
} ServerEnv;

static void server_case(void *env, TestingT *t) {
    const ServerEnv *e = (const ServerEnv *)env;
    const ServerTest *tc = e->tc;
    Alloc *a = heap_allocator();
    Arena ar;
    arena_init(&ar, NULL, 0);
    Str url = cat(arena_allocator(&ar), e->ts->url, cs(tc->path));
    Error err = BURROW_NO_ERROR;
    HttpRequest *req = http_new_request(a, cs(tc->method), url, (IoReader){0}, &err);
    if (BURROW_FAILED(err)) {
        testing_t_fatalf_v(t, "NewRequest: %v", err);
        arena_free(&ar);
        return;
    }
    Str origin = tc->origin_is_url ? e->ts->url : cs(tc->origin);
    if (origin.len != 0)
        http_header_set(req->header, S("Origin"), origin);
    if (tc->sec_fetch_site[0] != '\0')
        http_header_set(req->header, S("Sec-Fetch-Site"), cs(tc->sec_fetch_site));
    HttpClient client = {0};
    HttpResponse *resp = http_client_do(&client, req, &err);
    if (BURROW_FAILED(err)) {
        testing_t_fatalf_v(t, "Do: %v", err);
        http_request_free(req);
        arena_free(&ar);
        return;
    }
    if (resp->status_code != tc->expected_status)
        testing_t_errorf_v(t, "got status %d, want %d", resp->status_code,
                           tc->expected_status);
    (void)resp->body.vt->closer.close(resp->body.data);
    http_response_free(resp);
    http_request_free(req);
    arena_free(&ar);
}

static void TestCrossOriginProtectionServer(TestingT *t) {
    HttpCrossOriginProtection *protection =
        http_new_cross_origin_protection(heap_allocator());
    (void)http_cross_origin_protection_add_trusted_origin(protection,
                                                          S("https://trusted.example"));
    http_cross_origin_protection_add_insecure_bypass_pattern(protection, S("/bypass/"));
    HttpHandler handler =
        http_cross_origin_protection_handler(protection, ok_handler());

    HttptestServer *ts = httptest_new_server(heap_allocator(), handler);

    for (size_t i = 0; i < sizeof server_tests / sizeof server_tests[0]; i++) {
        ServerEnv env = {&server_tests[i], ts};
        (void)testing_t_run(t, cs(server_tests[i].name),
                            BURROW_FN(TestingTFunc, server_case, &env));
    }
    httptest_server_free(ts);
    http_cross_origin_protection_free(protection);
}

#define TESTS(X)                                                                       \
    X(TestCrossOriginProtectionSecFetchSite)                                           \
    X(TestCrossOriginProtectionTrustedOriginBypass)                                    \
    X(TestCrossOriginProtectionPatternBypass)                                          \
    X(TestCrossOriginProtectionSetDenyHandler)                                         \
    X(TestCrossOriginProtectionAddTrustedOriginErrors)                                 \
    X(TestCrossOriginProtectionAddingBypassesConcurrently)                             \
    X(TestCrossOriginProtectionServer)

TESTING_MAIN(TESTS)
