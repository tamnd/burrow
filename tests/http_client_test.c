/* Derived from Go's src/net/http/client_test.go, the parts that HTTP/1 can
 * run: what the client sends, its redirects, its cookies, its errors and its
 * time limit. The tests that need a server use httptest's, on the loopback
 * address.
 * Go source: go1.27.1.
 *
 * Copyright 2009 The Go Authors. All rights reserved.
 * Copyright 2026 The burrow Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style licence that can be found
 * in the LICENSE file. */

#include "check.h"

#include "../src/net/http_internal.h"

#include "burrow/burrow.h"
#include "burrow/context.h"
#include "burrow/encoding/base64.h"
#include "burrow/error.h"
#include "burrow/fmt.h"
#include "burrow/io.h"
#include "burrow/mem/arena.h"
#include "burrow/mem/heap.h"
#include "burrow/net.h"
#include "burrow/net/http.h"
#include "burrow/net/http/httptest.h"
#include "burrow/net/url.h"
#include "burrow/strings.h"
#include "burrow/sync.h"
#include "burrow/time.h"

#include <stdint.h>
#include <string.h>

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

static Str cs(const char *s) {
    return str_from_cstr(s);
}

static IoReader body_reader(IoReadCloser rc) {
    return (IoReader){&rc.vt->reader, rc.data};
}

/* The body of res, read to the end into a, and closed. */
static Str read_body(Alloc *a, HttpResponse *res, Error *err) {
    Slice b = io_read_all(a, body_reader(res->body), err);
    (void)res->body.vt->closer.close(res->body.data);
    return str_from_bytes(b.p, b.len);
}

static const char *handler_failed;

#define HCHECK(cond)                                                                   \
    do {                                                                               \
        if (!(cond) && handler_failed == NULL)                                         \
            handler_failed = #cond;                                                    \
    } while (0)

static void check_handlers(TestingT *t) {
    if (handler_failed != NULL)
        testing_t_errorf_v(t, "in a handler: %s", handler_failed);
    handler_failed = NULL;
}

static void write_str(HttpResponseWriter w, Str s) {
    Error err;
    (void)http_response_writer_write(
        w, slice_from((void *)(uintptr_t)s.p, s.len, s.len, TYPE_BYTE), &err);
}

BURROW_SENTINEL_ERROR(dummy_impl, "dummy impl");

/* ------------------------------------------------------- recordingTransport */

/* What a request had when the transport saw it, copied, since the client
 * frees the request when the round trip fails. */
typedef struct Recorded {
    Arena ar;
    int calls;
    Str method;
    Str url;
    bool header_nil;
    bool close;
    int64_t content_length;
    Str content_type;
    Str authorization;
    Str body;
    Slice cookies; /* of HttpCookie */
} Recorded;

static HttpResponse *rec_round_trip(void *self, HttpRequest *req, Error *err) {
    Recorded *r = (Recorded *)self;
    Alloc *a = arena_allocator(&r->ar);
    r->calls++;
    r->method = str_clone(a, req->method);
    r->url = url_string(req->url, a);
    r->header_nil = req->header == NULL;
    r->close = req->close;
    r->content_length = req->content_length;
    r->content_type = BURROW_STR_EMPTY;
    r->authorization = BURROW_STR_EMPTY;
    r->cookies = (Slice){0};
    if (req->header != NULL) {
        r->content_type =
            str_clone(a, http_header_get(req->header, cs("Content-Type")));
        r->authorization =
            str_clone(a, http_header_get(req->header, cs("Authorization")));
        r->cookies = http_request_cookies(req, a);
    }
    r->body = BURROW_STR_EMPTY;
    if (req->body.vt != NULL) {
        Error e = BURROW_NO_ERROR;
        Slice b = io_read_all(a, body_reader(req->body), &e);
        r->body = str_from_bytes(b.p, b.len);
    }
    *err = dummy_impl;
    return NULL;
}

static const HttpRoundTripperVT rec_vt = {NULL, rec_round_trip};

static void rec_init(Recorded *r) {
    memset(r, 0, sizeof *r);
    arena_init(&r->ar, heap_allocator(), 0);
}

static HttpClient rec_client(Recorded *r) {
    HttpClient c;
    memset(&c, 0, sizeof c);
    c.transport = (HttpRoundTripper){&rec_vt, r};
    return c;
}

static void TestGetRequestFormat(TestingT *t) {
    Recorded tr;
    rec_init(&tr);
    HttpClient client = rec_client(&tr);
    Str url = cs("http://dummy.faketld/");
    Error err;
    HttpResponse *res = http_client_get(&client, url, &err); /* doesn't hit network */
    CHECK(res == NULL);
    CHECK(str_eq(tr.method, cs("GET")));
    CHECK(str_eq(tr.url, url));
    CHECK(!tr.header_nil);
    arena_free(&tr.ar);
}

static void TestPostRequestFormat(TestingT *t) {
    Recorded tr;
    rec_init(&tr);
    HttpClient client = rec_client(&tr);
    Str url = cs("http://dummy.faketld/");
    Str json = cs("{\"key\":\"value\"}");
    StringsReader b;
    strings_reader_reset(&b, json);
    Error err;
    HttpResponse *res = http_client_post(&client, url, cs("application/json"),
                                         strings_reader_as_io_reader(&b), &err);
    CHECK(res == NULL);
    CHECK(str_eq(tr.method, cs("POST")));
    CHECK(str_eq(tr.url, url));
    CHECK(!tr.header_nil);
    CHECK(!tr.close);
    CHECK_INT_EQ(tr.content_length, json.len);
    arena_free(&tr.ar);
}

static void TestPostFormRequestFormat(TestingT *t) {
    Recorded tr;
    rec_init(&tr);
    HttpClient client = rec_client(&tr);
    Arena ar;
    arena_init(&ar, heap_allocator(), 0);
    Alloc *a = arena_allocator(&ar);
    Str url = cs("http://dummy.faketld/");
    UrlValues form = url_values_make(a);
    CHECK(url_values_set(form, cs("foo"), cs("bar")));
    CHECK(url_values_add(form, cs("foo"), cs("bar2")));
    CHECK(url_values_set(form, cs("bar"), cs("baz")));
    Error err;
    HttpResponse *res = http_client_post_form(&client, url, form, &err);
    CHECK(res == NULL);
    CHECK(str_eq(tr.method, cs("POST")));
    CHECK(str_eq(tr.url, url));
    CHECK(!tr.header_nil);
    CHECK(str_eq(tr.content_type, cs("application/x-www-form-urlencoded")));
    CHECK(!tr.close);
    /* Go's map order lets it be either. Encode sorts by key, so it is the
     * second. */
    Str want = cs("foo=bar&foo=bar2&bar=baz");
    Str want1 = cs("bar=baz&foo=bar&foo=bar2");
    CHECK_INT_EQ(tr.content_length, want.len);
    if (!str_eq(tr.body, want) && !str_eq(tr.body, want1))
        testing_t_errorf_v(t, "got body %q, want %q or %q", tr.body, want, want1);
    arena_free(&ar);
    arena_free(&tr.ar);
}

static void TestBasicAuth(TestingT *t) {
    Recorded tr;
    rec_init(&tr);
    HttpClient client = rec_client(&tr);
    Str url = cs("http://My%20User:My%20Pass@dummy.faketld/");
    Error err;
    HttpResponse *res = http_client_get(&client, url, &err);
    CHECK(res == NULL);
    CHECK(str_eq(tr.method, cs("GET")));
    CHECK(str_eq(tr.url, url));
    CHECK(!tr.header_nil);
    Str auth = tr.authorization;
    if (strings_has_prefix(auth, cs("Basic "))) {
        Error derr = BURROW_NO_ERROR;
        Slice d = base64_encoding_decode_string(
            base64_std_encoding, arena_allocator(&tr.ar),
            str_from_bytes(auth.p + 6, auth.len - 6), &derr);
        CHECK(BURROW_OK(derr));
        Str s = str_from_bytes(d.p, d.len);
        if (!str_eq(s, cs("My User:My Pass")))
            testing_t_errorf_v(t, "Invalid Authorization header. Got %q, wanted %q", s,
                               cs("My User:My Pass"));
    } else {
        testing_t_errorf_v(t, "Invalid auth %q", auth);
    }
    arena_free(&tr.ar);
}

static void TestStripPasswordFromError(TestingT *t) {
    static const struct {
        const char *in, *out;
    } cases[] = {
        /* Strip password from error message. */
        {"http://user:password@dummy.faketld/",
         "Get \"http://user:***@dummy.faketld/\": dummy impl"},
        /* Don't strip password from domain name. */
        {"http://user:password@password.faketld/",
         "Get \"http://user:***@password.faketld/\": dummy impl"},
        /* Don't strip password from path. */
        {"http://user:password@dummy.faketld/password",
         "Get \"http://user:***@dummy.faketld/password\": dummy impl"},
        /* Strip escaped password. */
        {"http://user:pa%2Fssword@dummy.faketld/",
         "Get \"http://user:***@dummy.faketld/\": dummy impl"},
    };
    Recorded tr;
    rec_init(&tr);
    HttpClient client = rec_client(&tr);
    for (size_t i = 0; i < sizeof cases / sizeof cases[0]; i++) {
        Error err = BURROW_NO_ERROR;
        HttpResponse *res = http_client_get(&client, cs(cases[i].in), &err);
        CHECK(res == NULL);
        Str got = error_text(err);
        if (!str_eq(got, cs(cases[i].out)))
            testing_t_errorf_v(t, "Unexpected output for %q: expected %q, actual %q",
                               cs(cases[i].in), cs(cases[i].out), got);
    }
    arena_free(&tr.ar);
}

static void TestClientErrorWithRequestURI(TestingT *t) {
    Error err = BURROW_NO_ERROR;
    IoReader none = {NULL, NULL};
    HttpRequest *req = http_new_request(heap_allocator(), cs("GET"),
                                        cs("http://localhost:1234/"), none, &err);
    CHECK(req != NULL);
    if (req == NULL)
        return;
    req->request_uri = cs("/this/field/is/illegal/and/should/error/");
    HttpResponse *res = http_client_do(http_default_client, req, &err);
    if (res != NULL) {
        http_response_free(res);
        testing_t_errorf_v(t, "expected an error");
    } else if (!strings_contains(error_text(err), cs("RequestURI"))) {
        testing_t_errorf_v(t, "wanted error mentioning RequestURI; got error: %v", err);
    }
    http_request_free(req);
}

/* A response made the way a round tripper outside this package would, which
 * http_response_free can free. */
static HttpResponse *new_response(Int code) {
    Alloc *a = heap_allocator();
    HttpResponse *r = (HttpResponse *)mem_alloc(a, sizeof *r, _Alignof(HttpResponse));
    if (r == NULL)
        return NULL;
    r->a = a;
    arena_init(&r->arena, a, 0);
    r->status_code = code;
    r->status = http_status_text(code);
    r->proto = cs("HTTP/1.1");
    r->proto_major = 1;
    r->proto_minor = 1;
    r->header = http_header_make(arena_allocator(&r->arena));
    return r;
}

/* nilBodyRoundTripper. */
static HttpResponse *nil_body_round_trip(void *self, HttpRequest *req, Error *err) {
    (void)self;
    HttpResponse *r = new_response(HTTP_STATUS_OK);
    if (r != NULL)
        r->request = req;
    *err = r == NULL ? burrow_err_out_of_memory : BURROW_NO_ERROR;
    return r;
}

static const HttpRoundTripperVT nil_body_vt = {NULL, nil_body_round_trip};

static void TestClientPopulatesNilResponseBody(TestingT *t) {
    HttpClient c;
    memset(&c, 0, sizeof c);
    c.transport = (HttpRoundTripper){&nil_body_vt, NULL};
    Error err = BURROW_NO_ERROR;
    HttpResponse *resp = http_client_get(&c, cs("http://localhost/anything"), &err);
    if (resp == NULL) {
        testing_t_errorf_v(t, "Client.Get rejected Response with nil Body: %v", err);
        return;
    }
    if (resp->body.vt == NULL) {
        testing_t_errorf_v(t, "Client failed to provide a non-nil Body as documented");
        http_response_free(resp);
        return;
    }
    Slice b = io_read_all(heap_allocator(), body_reader(resp->body), &err);
    if (BURROW_FAILED(err))
        testing_t_errorf_v(t, "read error from substitute Response.Body: %v", err);
    else if (b.len != 0)
        testing_t_errorf_v(t, "substitute Response.Body was unexpectedly non-empty");
    if (b.p != NULL)
        mem_free(heap_allocator(), b.p, (size_t)b.cap, 1);
    Error cerr = resp->body.vt->closer.close(resp->body.data);
    if (BURROW_FAILED(cerr))
        testing_t_errorf_v(t, "error from Close on substitute Response.Body: %v", cerr);
    http_response_free(resp);
}

/* issue15577Tripper: a redirect with no request in the response. */
static HttpResponse *issue15577_round_trip(void *self, HttpRequest *req, Error *err) {
    (void)self;
    (void)req;
    HttpResponse *r = new_response(303);
    if (r == NULL || r->header == NULL ||
        !http_header_set(r->header, cs("Location"), cs("http://www.example.com/"))) {
        http_response_free(r);
        *err = burrow_err_out_of_memory;
        return NULL;
    }
    r->body = http_no_body;
    *err = BURROW_NO_ERROR;
    return r;
}

static const HttpRoundTripperVT issue15577_vt = {NULL, issue15577_round_trip};

BURROW_SENTINEL_ERROR(no_redirects, "no redirects!");

static Error refuse_redirect(void *env, HttpRequest *req, Slice via) {
    (void)env;
    (void)req;
    (void)via;
    return no_redirects;
}

static void TestClientRedirectResponseWithoutRequest(TestingT *t) {
    HttpClient c;
    memset(&c, 0, sizeof c);
    c.check_redirect = BURROW_FN(HttpCheckRedirectFunc, refuse_redirect, NULL);
    c.transport = (HttpRoundTripper){&issue15577_vt, NULL};
    /* That this doesn't crash is the test. */
    Error err = BURROW_NO_ERROR;
    HttpResponse *res = http_client_get(&c, cs("http://dummy.tld"), &err);
    CHECK(errors_is(err, no_redirects));
    http_response_free(res);
}

static void TestReferer(TestingT *t) {
    static const struct {
        const char *last_req, *new_req, *explicit_ref, *want;
    } tests[] = {
        /* Don't send user. */
        {"http://gopher@test.com", "http://link.com", "", "http://test.com"},
        {"https://gopher@test.com", "https://link.com", "", "https://test.com"},
        /* Don't send a user and password. */
        {"http://gopher:go@test.com", "http://link.com", "", "http://test.com"},
        {"https://gopher:go@test.com", "https://link.com", "", "https://test.com"},
        /* Nothing to do. */
        {"http://test.com", "http://link.com", "", "http://test.com"},
        {"https://test.com", "https://link.com", "", "https://test.com"},
        /* https to http doesn't send a referer. */
        {"https://test.com", "http://link.com", "", ""},
        {"https://gopher:go@test.com", "http://link.com", "", ""},
        /* https to http should remove an existing referer. */
        {"https://test.com", "http://link.com", "https://foo.com", ""},
        {"https://gopher:go@test.com", "http://link.com", "https://foo.com", ""},
        /* Don't override an existing referer. */
        {"https://test.com", "https://link.com", "https://foo.com", "https://foo.com"},
        {"https://gopher:go@test.com", "https://link.com", "https://foo.com",
         "https://foo.com"},
    };
    Arena ar;
    arena_init(&ar, heap_allocator(), 0);
    Alloc *a = arena_allocator(&ar);
    for (size_t i = 0; i < sizeof tests / sizeof tests[0]; i++) {
        Error err = BURROW_NO_ERROR;
        Url *l = url_parse(a, cs(tests[i].last_req), &err);
        Url *n = url_parse(a, cs(tests[i].new_req), &err);
        if (l == NULL || n == NULL) {
            testing_t_errorf_v(t, "parse: %v", err);
            continue;
        }
        Str r = burrow__http_referer_for_url(a, l, n, cs(tests[i].explicit_ref));
        if (!str_eq(r, cs(tests[i].want)))
            testing_t_errorf_v(t, "refererForURL(%q, %q) = %q; want %q",
                               cs(tests[i].last_req), cs(tests[i].new_req), r,
                               cs(tests[i].want));
    }
    arena_free(&ar);
}

static void TestShouldCopyHeaderOnRedirect(TestingT *t) {
    static const struct {
        const char *initial_url, *dest_url;
        bool want;
    } tests[] = {
        /* Sensitive headers. */
        {"http://foo.com/", "http://bar.com/", false},
        {"http://foo.com/", "http://bar.com/", false},
        {"http://foo.com/", "http://bar.com/", false},
        {"http://foo.com/", "https://foo.com/", true},
        {"http://foo.com:1234/", "http://foo.com:4321/", true},
        {"http://foo.com/", "http://bar.com/", false},
        {"http://foo.com/", "http://[::1%25.foo.com]/", false},

        /* But subdomains should work. */
        {"http://foo.com/", "http://foo.com/", true},
        {"http://foo.com/", "http://sub.foo.com/", true},
        {"http://foo.com/", "http://notfoo.com/", false},
        {"http://foo.com/", "https://foo.com/", true},
        {"http://foo.com:80/", "http://foo.com/", true},
        {"http://foo.com:80/", "http://sub.foo.com/", true},
        {"http://foo.com:443/", "https://foo.com/", true},
        {"http://foo.com:443/", "https://sub.foo.com/", true},
        {"http://foo.com:1234/", "http://foo.com/", true},

        {"http://foo.com/", "http://foo.com/", true},
        {"http://foo.com/", "http://sub.foo.com/", true},
        {"http://foo.com/", "http://notfoo.com/", false},
        {"http://foo.com/", "https://foo.com/", true},
        {"http://foo.com:80/", "http://foo.com/", true},
        {"http://foo.com:80/", "http://sub.foo.com/", true},
        {"http://foo.com:443/", "https://foo.com/", true},
        {"http://foo.com:443/", "https://sub.foo.com/", true},
        {"http://foo.com:1234/", "http://foo.com/", true},

        {"http://foobar.com/", "http://fooBAR.com/", true},

        {"http://example.com/",
         "http://evil\xe3\x80\x82"
         "example.com/",
         false},
        {"http://example.com/", "http://\xef\xbd\x85xample.com/", false},
        {"http://s\xc3\xbc"
         "b.example.com/",
         "http://s\xc3\x9c"
         "b.example.com/",
         false},
    };
    Arena ar;
    arena_init(&ar, heap_allocator(), 0);
    Alloc *a = arena_allocator(&ar);
    for (size_t i = 0; i < sizeof tests / sizeof tests[0]; i++) {
        Error err = BURROW_NO_ERROR;
        Url *u0 = url_parse(a, cs(tests[i].initial_url), &err);
        if (u0 == NULL) {
            testing_t_errorf_v(t, "%d. initial URL %q parse error: %v", (Int)i,
                               cs(tests[i].initial_url), err);
            continue;
        }
        Url *u1 = url_parse(a, cs(tests[i].dest_url), &err);
        if (u1 == NULL) {
            testing_t_errorf_v(t, "%d. dest URL %q parse error: %v", (Int)i,
                               cs(tests[i].dest_url), err);
            continue;
        }
        bool got = burrow__http_should_copy_header_on_redirect(a, u0, u1);
        if (got != tests[i].want)
            testing_t_errorf_v(
                t, "%d. shouldCopyHeaderOnRedirect(%q => %q) = %t; want %t", (Int)i,
                cs(tests[i].initial_url), cs(tests[i].dest_url), got, tests[i].want);
    }
    arena_free(&ar);
}

/* ---------------------------------------------------------------- TestJar */

/* Just enough of a jar for the redirect tests. The URL's host is the scope of
 * all the cookies. */
typedef struct TestJar {
    SyncMutex mu;
    Arena ar;
    Str hosts[8];
    Slice cookies[8]; /* of HttpCookie */
    Int n;
} TestJar;

static void test_jar_set_cookies(void *self, const Url *u, Slice cookies) {
    TestJar *j = (TestJar *)self;
    sync_mutex_lock(&j->mu);
    Alloc *a = arena_allocator(&j->ar);
    Int i = 0;
    while (i < j->n && !str_eq(j->hosts[i], u->host))
        i++;
    if (i == j->n && j->n < (Int)(sizeof j->hosts / sizeof j->hosts[0])) {
        j->hosts[i] = str_clone(a, u->host);
        j->n++;
    }
    if (i < j->n) {
        HttpCookie *cs_ = (HttpCookie *)mem_alloc(
            a, (size_t)(cookies.len + 1) * sizeof *cs_, _Alignof(HttpCookie));
        for (Int k = 0; cs_ != NULL && k < cookies.len; k++) {
            const HttpCookie *c = &((const HttpCookie *)cookies.p)[k];
            cs_[k].name = str_clone(a, c->name);
            cs_[k].value = str_clone(a, c->value);
        }
        j->cookies[i] = (Slice){cs_, cs_ != NULL ? cookies.len : 0,
                                cs_ != NULL ? cookies.len : 0, NULL};
    }
    sync_mutex_unlock(&j->mu);
}

static Slice test_jar_cookies(void *self, Alloc *a, const Url *u) {
    TestJar *j = (TestJar *)self;
    Slice out = {0};
    sync_mutex_lock(&j->mu);
    for (Int i = 0; i < j->n; i++) {
        if (!str_eq(j->hosts[i], u->host))
            continue;
        Int n = j->cookies[i].len;
        HttpCookie *cs_ = (HttpCookie *)mem_alloc(a, (size_t)(n + 1) * sizeof *cs_,
                                                  _Alignof(HttpCookie));
        if (cs_ != NULL && n > 0)
            memcpy(cs_, j->cookies[i].p, (size_t)n * sizeof *cs_);
        out = (Slice){cs_, cs_ != NULL ? n : 0, cs_ != NULL ? n : 0, NULL};
    }
    sync_mutex_unlock(&j->mu);
    return out;
}

static const HttpCookieJarVT test_jar_vt = {NULL, test_jar_set_cookies,
                                            test_jar_cookies};

static void test_jar_init(TestJar *j) {
    memset(j, 0, sizeof *j);
    arena_init(&j->ar, heap_allocator(), 0);
}

static HttpCookieJar test_jar_as_jar(TestJar *j) {
    return (HttpCookieJar){&test_jar_vt, j};
}

static const HttpCookie expected_cookies[3] = {
    {.name = BURROW_S_INIT("ChocolateChip"), .value = BURROW_S_INIT("tasty")},
    {.name = BURROW_S_INIT("First"), .value = BURROW_S_INIT("Hit")},
    {.name = BURROW_S_INIT("Second"), .value = BURROW_S_INIT("Hit")},
};

static Slice expected_slice(Int n) {
    return (Slice){(void *)(uintptr_t)expected_cookies, n, n, NULL};
}

/* matchReturnedCookies. */
static void match_returned_cookies(TestingT *t, Slice expected, Slice given) {
    const HttpCookie *e = (const HttpCookie *)expected.p;
    const HttpCookie *g = (const HttpCookie *)given.p;
    if (given.len != expected.len)
        testing_t_errorf_v(t, "Expected %d cookies, got %d", expected.len, given.len);
    for (Int i = 0; i < expected.len; i++) {
        bool found = false;
        for (Int k = 0; k < given.len && !found; k++)
            found = str_eq(e[i].name, g[k].name) && str_eq(e[i].value, g[k].value);
        if (!found)
            testing_t_errorf_v(t, "Missing cookie %s=%s", e[i].name, e[i].value);
    }
}

static void TestClientSendsCookieFromJar(TestingT *t) {
    Recorded tr;
    rec_init(&tr);
    HttpClient client = rec_client(&tr);
    TestJar jar;
    test_jar_init(&jar);
    client.jar = test_jar_as_jar(&jar);
    Arena ar;
    arena_init(&ar, heap_allocator(), 0);
    Alloc *a = arena_allocator(&ar);
    Str us = cs("http://dummy.faketld/");
    Error err = BURROW_NO_ERROR;
    Url *u = url_parse(a, us, &err);
    CHECK(u != NULL);
    if (u == NULL)
        return;
    http_cookie_jar_set_cookies(client.jar, u, expected_slice(3));
    IoReader none = {NULL, NULL};

    CHECK(http_client_get(&client, us, &err) == NULL); /* doesn't hit network */
    match_returned_cookies(t, expected_slice(3), tr.cookies);

    CHECK(http_client_head(&client, us, &err) == NULL);
    match_returned_cookies(t, expected_slice(3), tr.cookies);

    StringsReader body;
    strings_reader_reset(&body, cs("body"));
    CHECK(http_client_post(&client, us, cs("text/plain"),
                           strings_reader_as_io_reader(&body), &err) == NULL);
    match_returned_cookies(t, expected_slice(3), tr.cookies);

    CHECK(http_client_post_form(&client, us, url_values_make(a), &err) == NULL);
    match_returned_cookies(t, expected_slice(3), tr.cookies);

    HttpRequest *req = http_new_request(heap_allocator(), cs("GET"), us, none, &err);
    CHECK(req != NULL && http_client_do(&client, req, &err) == NULL);
    match_returned_cookies(t, expected_slice(3), tr.cookies);
    http_request_free(req);

    req = http_new_request(heap_allocator(), cs("POST"), us, none, &err);
    CHECK(req != NULL && http_client_do(&client, req, &err) == NULL);
    match_returned_cookies(t, expected_slice(3), tr.cookies);
    http_request_free(req);

    arena_free(&ar);
    arena_free(&jar.ar);
    arena_free(&tr.ar);
}

/* ------------------------------------------------------- against a server */

/* robotsTxtHandler. */
static void serve_robots(void *env, HttpResponseWriter w, HttpRequest *r) {
    (void)env;
    (void)r;
    HCHECK(http_header_set(http_response_writer_header(w), cs("Last-Modified"),
                           cs("sometime")));
    write_str(w, cs("User-agent: go\nDisallow: /something/"));
}

/* pedanticReadAll: io_read_all, and a check that the body keeps to the
 * io.Reader contract. */
static Str pedantic_read_all(Alloc *a, IoReader r, Error *err) {
    Byte bufa[64];
    Slice buf = slice_from(bufa, 64, 64, TYPE_BYTE);
    Slice b = slice_make(a, TYPE_BYTE, 0, 64);
    for (;;) {
        Error e = BURROW_NO_ERROR;
        Int n = r.vt->read(r.data, buf, &e);
        if (n == 0 && BURROW_OK(e)) {
            *err = fmt_errorf_v("Read: n=0 with err=nil");
            return BURROW_STR_EMPTY;
        }
        b = slice_append(a, b, bufa, n);
        if (errors_is(e, io_eof)) {
            n = r.vt->read(r.data, buf, &e);
            if (n != 0 || !errors_is(e, io_eof)) {
                *err = fmt_errorf_v("Read: n=%d err=%v after EOF", n, e);
                return BURROW_STR_EMPTY;
            }
            *err = BURROW_NO_ERROR;
            return str_from_bytes(b.p, b.len);
        }
        if (BURROW_FAILED(e)) {
            *err = e;
            return str_from_bytes(b.p, b.len);
        }
    }
}

static void TestClient(TestingT *t) {
    need_tcp(t);
    HttpHandlerFunc f = BURROW_FN(HttpHandlerFunc, serve_robots, NULL);
    HttptestServer *ts = httptest_new_server(NULL, http_handler_func_as_handler(&f));
    Arena ar;
    arena_init(&ar, heap_allocator(), 0);
    HttpClient *c = httptest_server_client(ts);
    Error err = BURROW_NO_ERROR;
    HttpResponse *r = http_client_get(c, ts->url, &err);
    Str b = BURROW_STR_EMPTY;
    if (r != NULL) {
        b = pedantic_read_all(arena_allocator(&ar), body_reader(r->body), &err);
        (void)r->body.vt->closer.close(r->body.data);
    }
    if (BURROW_FAILED(err))
        testing_t_errorf_v(t, "%v", err);
    else if (!strings_has_prefix(b, cs("User-agent:")))
        testing_t_errorf_v(t, "Incorrect page body (did not begin with User-agent): %q",
                           b);
    http_response_free(r);
    arena_free(&ar);
    httptest_server_free(ts);
    check_handlers(t);
}

static void TestClientHead(TestingT *t) {
    need_tcp(t);
    HttpHandlerFunc f = BURROW_FN(HttpHandlerFunc, serve_robots, NULL);
    HttptestServer *ts = httptest_new_server(NULL, http_handler_func_as_handler(&f));
    Error err = BURROW_NO_ERROR;
    HttpResponse *r = http_client_head(httptest_server_client(ts), ts->url, &err);
    if (r == NULL) {
        testing_t_errorf_v(t, "%v", err);
    } else {
        if (http_header_values(r->header, cs("Last-Modified")).len == 0)
            testing_t_errorf_v(t, "Last-Modified header not found.");
        (void)r->body.vt->closer.close(r->body.data);
        http_response_free(r);
    }
    httptest_server_free(ts);
    check_handlers(t);
}

/* The handler of TestClientRedirects, which redirects /?n=k to k+1 until 15. */
static HttptestServer *redirects_ts;

static void serve_redirects(void *env, HttpResponseWriter w, HttpRequest *r) {
    (void)env;
    Arena ar;
    arena_init(&ar, heap_allocator(), 0);
    Alloc *a = arena_allocator(&ar);
    Str ns = url_values_get(url_query(r->url, a), cs("n"));
    Int n = 0;
    for (Int i = 0; i < ns.len && ns.p[i] >= '0' && ns.p[i] <= '9'; i++)
        n = n * 10 + (ns.p[i] - '0');
    /* The Referer, at 7, which is as good as anywhere. */
    if (n == 7) {
        Str want = fmt_sprintf_v(a, "%s/?n=6", redirects_ts->url);
        HCHECK(str_eq(http_request_referer(r), want));
    }
    if (n < 15)
        http_redirect(w, r, fmt_sprintf_v(a, "/?n=%d", n + 1),
                      HTTP_STATUS_TEMPORARY_REDIRECT);
    else
        write_str(w, fmt_sprintf_v(a, "n=%d", n));
    arena_free(&ar);
}

typedef struct LastVia {
    Int len;
    Error check_err;
} LastVia;

static Error record_via(void *env, HttpRequest *req, Slice via) {
    LastVia *lv = (LastVia *)env;
    (void)req;
    lv->len = via.len;
    return lv->check_err;
}

BURROW_SENTINEL_ERROR(no_redirects_allowed, "no redirects allowed");

static void check_err_text(TestingT *t, const char *what, Error err, const char *want) {
    if (!str_eq(error_text(err), cs(want)))
        testing_t_errorf_v(t, "%s, expected error %q, got %q", cs(what), cs(want),
                           error_text(err));
}

static void TestClientRedirects(TestingT *t) {
    need_tcp(t);
    HttpHandlerFunc f = BURROW_FN(HttpHandlerFunc, serve_redirects, NULL);
    HttptestServer *ts = httptest_new_server(NULL, http_handler_func_as_handler(&f));
    redirects_ts = ts;
    HttpClient *c = httptest_server_client(ts);
    IoReader none = {NULL, NULL};

    Error err = BURROW_NO_ERROR;
    http_response_free(http_client_get(c, ts->url, &err));
    check_err_text(t, "with default client Get", err,
                   "Get \"/?n=10\": stopped after 10 redirects");

    /* HEAD follows redirects too. */
    http_response_free(http_client_head(c, ts->url, &err));
    check_err_text(t, "with default client Head", err,
                   "Head \"/?n=10\": stopped after 10 redirects");

    /* And so does Do. */
    HttpRequest *greq =
        http_new_request(heap_allocator(), cs("GET"), ts->url, none, &err);
    CHECK(greq != NULL);
    if (greq != NULL) {
        http_response_free(http_client_do(c, greq, &err));
        check_err_text(t, "with default client Do", err,
                       "Get \"/?n=10\": stopped after 10 redirects");

        /* An empty method redirects as well, issue 12705. */
        greq->method = BURROW_STR_EMPTY;
        http_response_free(http_client_do(c, greq, &err));
        check_err_text(t, "with default client Do and empty Method", err,
                       "Get \"/?n=10\": stopped after 10 redirects");
        http_request_free(greq);
    }

    LastVia lv = {0, BURROW_NO_ERROR};
    c->check_redirect = BURROW_FN(HttpCheckRedirectFunc, record_via, &lv);
    HttpResponse *res = http_client_get(c, ts->url, &err);
    if (res == NULL) {
        testing_t_errorf_v(t, "Get error: %v", err);
    } else {
        (void)res->body.vt->closer.close(res->body.data);
        Arena ar;
        arena_init(&ar, heap_allocator(), 0);
        Str final_url = url_string(res->request->url, arena_allocator(&ar));
        if (!strings_has_suffix(final_url, cs("/?n=15")))
            testing_t_errorf_v(t, "expected final url to end in /?n=15; got url %q",
                               final_url);
        arena_free(&ar);
        CHECK_INT_EQ(lv.len, 15);
        http_response_free(res);
    }

    lv.check_err = no_redirects_allowed;
    res = http_client_get(c, ts->url, &err);
    const UrlError *ue = (const UrlError *)errors_as(err, TYPE_URL_ERROR);
    if (ue == NULL || !errors_is(ue->err, no_redirects_allowed))
        testing_t_errorf_v(t,
                           "with redirects forbidden, expected a *url.Error with our "
                           "'no redirects allowed' error inside; got %q",
                           error_text(err));
    if (res == NULL) {
        testing_t_errorf_v(t, "Expected a non-nil Response on CheckRedirect failure "
                              "(https://golang.org/issue/3795)");
    } else {
        (void)res->body.vt->closer.close(res->body.data);
        if (http_header_get(res->header, cs("Location")).len == 0)
            testing_t_errorf_v(t, "no Location header in Response");
        http_response_free(res);
    }
    httptest_server_free(ts);
    check_handlers(t);
}

static void serve_use_response(void *env, HttpResponseWriter w, HttpRequest *r) {
    (void)env;
    if (strings_contains(r->url->path, cs("/other"))) {
        write_str(w, cs("wrong body"));
        return;
    }
    /* The header keeps the value it is given, so it goes in the header's
     * allocator. */
    HttpHeader h = http_response_writer_header(w);
    Str loc = fmt_sprintf_v(burrow__map_allocator(h), "http://%s/other", r->host);
    HCHECK(http_header_set(h, cs("Location"), loc));
    http_response_writer_write_header(w, HTTP_STATUS_FOUND);
    write_str(w, cs("Hello, world."));
}

static Error use_last_response(void *env, HttpRequest *req, Slice via) {
    (void)env;
    (void)via;
    HCHECK(req->response != NULL);
    return http_err_use_last_response;
}

static void TestClientRedirectUseResponse(TestingT *t) {
    need_tcp(t);
    HttpHandlerFunc f = BURROW_FN(HttpHandlerFunc, serve_use_response, NULL);
    HttptestServer *ts = httptest_new_server(NULL, http_handler_func_as_handler(&f));
    HttpClient *c = httptest_server_client(ts);
    c->check_redirect = BURROW_FN(HttpCheckRedirectFunc, use_last_response, NULL);
    Error err = BURROW_NO_ERROR;
    HttpResponse *res = http_client_get(c, ts->url, &err);
    if (res == NULL) {
        testing_t_errorf_v(t, "%v", err);
    } else {
        CHECK_INT_EQ(res->status_code, HTTP_STATUS_FOUND);
        Arena ar;
        arena_init(&ar, heap_allocator(), 0);
        Str slurp = read_body(arena_allocator(&ar), res, &err);
        CHECK(BURROW_OK(err));
        if (!str_eq(slurp, cs("Hello, world.")))
            testing_t_errorf_v(t, "body = %q; want %q", slurp, cs("Hello, world."));
        arena_free(&ar);
        http_response_free(res);
    }
    httptest_server_free(ts);
    check_handlers(t);
}

static void serve_no_location(void *env, HttpResponseWriter w, HttpRequest *r) {
    (void)r;
    HCHECK(http_header_set(http_response_writer_header(w), cs("Foo"), cs("Bar")));
    http_response_writer_write_header(w, *(const Int *)env);
}

static void no_location(void *env, TestingT *t) {
    need_tcp(t);
    HttpHandlerFunc f = BURROW_FN(HttpHandlerFunc, serve_no_location, env);
    HttptestServer *ts = httptest_new_server(NULL, http_handler_func_as_handler(&f));
    Error err = BURROW_NO_ERROR;
    HttpResponse *res = http_client_get(httptest_server_client(ts), ts->url, &err);
    if (res == NULL) {
        testing_t_errorf_v(t, "%v", err);
    } else {
        (void)res->body.vt->closer.close(res->body.data);
        CHECK_INT_EQ(res->status_code, *(const Int *)env);
        if (!str_eq(http_header_get(res->header, cs("Foo")), cs("Bar")))
            testing_t_errorf_v(t, "Foo header = %q; want Bar",
                               http_header_get(res->header, cs("Foo")));
        http_response_free(res);
    }
    httptest_server_free(ts);
    check_handlers(t);
}

static void TestClientRedirectNoLocation(TestingT *t) {
    static const Int codes[] = {301, 308};
    (void)testing_t_run(
        t, cs("301"),
        BURROW_FN(TestingTFunc, no_location, (void *)(uintptr_t)&codes[0]));
    (void)testing_t_run(
        t, cs("308"),
        BURROW_FN(TestingTFunc, no_location, (void *)(uintptr_t)&codes[1]));
}

/* echoCookiesRedirectHandler. */
static void serve_echo_cookies_redirect(void *env, HttpResponseWriter w,
                                        HttpRequest *r) {
    (void)env;
    Arena ar;
    arena_init(&ar, heap_allocator(), 0);
    Slice cookies = http_request_cookies(r, arena_allocator(&ar));
    for (Int i = 0; i < cookies.len; i++)
        HCHECK(http_set_cookie(w, &((const HttpCookie *)cookies.p)[i]));
    if (str_eq(r->url->path, cs("/"))) {
        HCHECK(http_set_cookie(w, &expected_cookies[1]));
        http_redirect(w, r, cs("/second"), HTTP_STATUS_MOVED_PERMANENTLY);
    } else {
        HCHECK(http_set_cookie(w, &expected_cookies[2]));
        write_str(w, cs("hello"));
    }
    arena_free(&ar);
}

static void TestRedirectCookiesJar(TestingT *t) {
    need_tcp(t);
    HttpHandlerFunc f = BURROW_FN(HttpHandlerFunc, serve_echo_cookies_redirect, NULL);
    HttptestServer *ts = httptest_new_server(NULL, http_handler_func_as_handler(&f));
    HttpClient *c = httptest_server_client(ts);
    TestJar jar;
    test_jar_init(&jar);
    c->jar = test_jar_as_jar(&jar);
    Arena ar;
    arena_init(&ar, heap_allocator(), 0);
    Alloc *a = arena_allocator(&ar);
    Error err = BURROW_NO_ERROR;
    Url *u = url_parse(a, ts->url, &err);
    CHECK(u != NULL);
    if (u != NULL) {
        http_cookie_jar_set_cookies(c->jar, u, expected_slice(1));
        HttpResponse *resp = http_client_get(c, ts->url, &err);
        if (resp == NULL) {
            testing_t_errorf_v(t, "Get: %v", err);
        } else {
            (void)resp->body.vt->closer.close(resp->body.data);
            match_returned_cookies(t, expected_slice(3),
                                   http_response_cookies(resp, a));
            http_response_free(resp);
        }
    }
    arena_free(&ar);
    httptest_server_free(ts);
    arena_free(&jar.ar);
    check_handlers(t);
}

static SyncWaitGroup timeout_gate;

static void serve_blocked(void *env, HttpResponseWriter w, HttpRequest *r) {
    (void)env;
    (void)w;
    (void)r;
    sync_wait_group_wait(&timeout_gate);
}

static void TestClientTimeout_Headers(TestingT *t) {
    need_tcp(t);
    sync_wait_group_add(&timeout_gate, 1);
    HttpHandlerFunc f = BURROW_FN(HttpHandlerFunc, serve_blocked, NULL);
    HttptestServer *ts = httptest_new_server(NULL, http_handler_func_as_handler(&f));
    HttpClient *c = httptest_server_client(ts);
    c->timeout = 5 * TIME_MILLISECOND;
    Error err = BURROW_NO_ERROR;
    HttpResponse *res = http_client_get(c, ts->url, &err);
    if (res != NULL) {
        (void)res->body.vt->closer.close(res->body.data);
        http_response_free(res);
        testing_t_errorf_v(t, "got response from Get; expected error");
    } else {
        if (errors_as(err, TYPE_URL_ERROR) == NULL)
            testing_t_errorf_v(t, "Got error %v; want *url.Error", err);
        if (!net_is_error(err))
            testing_t_errorf_v(t, "Got error %v; want some net.Error", err);
        else if (!net_error_timeout(err))
            testing_t_errorf_v(t, "net.Error.Timeout = false; want true");
        if (!errors_is(err, context_deadline_exceeded))
            testing_t_errorf_v(t, "error = %q; expected some context.DeadlineExceeded",
                               error_text(err));
        if (!strings_contains(error_text(err), cs("Client.Timeout exceeded")))
            testing_t_errorf_v(t, "error string = %q; missing timeout substring",
                               error_text(err));
    }
    sync_wait_group_done(&timeout_gate);
    httptest_server_free(ts);
    check_handlers(t);
}

static void serve_hello(void *env, HttpResponseWriter w, HttpRequest *r) {
    (void)env;
    (void)r;
    write_str(w, cs("hello"));
}

/* The server's own: a second request on the same client goes on the same
 * connection, which the server sees as new once. */
static void count_new(void *env, NetConn c, HttpConnState s) {
    (void)c;
    if (s == HTTP_STATE_NEW)
        sync_atomic_int64_add((SyncAtomicInt64 *)env, 1);
}

static void TestClientReusesTheConnection(TestingT *t) {
    need_tcp(t);
    HttpHandlerFunc f = BURROW_FN(HttpHandlerFunc, serve_hello, NULL);
    HttptestServer *ts =
        httptest_new_unstarted_server(NULL, http_handler_func_as_handler(&f));
    SyncAtomicInt64 conns;
    memset(&conns, 0, sizeof conns);
    ts->config.conn_state = BURROW_FN(HttpConnStateFunc, count_new, &conns);
    httptest_server_start(ts);
    Arena ar;
    arena_init(&ar, heap_allocator(), 0);
    for (int i = 0; i < 3; i++) {
        Error err = BURROW_NO_ERROR;
        HttpResponse *res = http_client_get(httptest_server_client(ts), ts->url, &err);
        if (res == NULL) {
            testing_t_errorf_v(t, "Get %d: %v", (Int)i, err);
            break;
        }
        Str b = read_body(arena_allocator(&ar), res, &err);
        CHECK(str_eq(b, cs("hello")));
        http_response_free(res);
    }
    CHECK_INT_EQ(sync_atomic_int64_load(&conns), 1);
    arena_free(&ar);
    httptest_server_free(ts);
    check_handlers(t);
}

#define TESTS(X)                                                                       \
    X(TestGetRequestFormat)                                                            \
    X(TestPostRequestFormat)                                                           \
    X(TestPostFormRequestFormat)                                                       \
    X(TestBasicAuth)                                                                   \
    X(TestStripPasswordFromError)                                                      \
    X(TestClientErrorWithRequestURI)                                                   \
    X(TestClientPopulatesNilResponseBody)                                              \
    X(TestClientRedirectResponseWithoutRequest)                                        \
    X(TestReferer)                                                                     \
    X(TestShouldCopyHeaderOnRedirect)                                                  \
    X(TestClientSendsCookieFromJar)                                                    \
    X(TestClient)                                                                      \
    X(TestClientHead)                                                                  \
    X(TestClientRedirects)                                                             \
    X(TestClientRedirectUseResponse)                                                   \
    X(TestClientRedirectNoLocation)                                                    \
    X(TestRedirectCookiesJar)                                                          \
    X(TestClientTimeout_Headers)                                                       \
    X(TestClientReusesTheConnection)
TESTING_MAIN(TESTS)
