/* Derived from Go's src/net/http/cookie_test.go.
 * Go source: go1.27.1.
 *
 * The tables were printed by Go 1.27.1 from Go's own: the cookies Go's tests
 * write as literals, and for TestParseCookie and TestParseSetCookie what Go's
 * ParseCookie and ParseSetCookie give for each line of their tables, which
 * Go's tests check are those literals. The cases that repeat a cookie 3001
 * times are written out as loops. TestSetCookie, TestAddCookie and
 * TestSetCookieDoubleQuotes need ResponseWriter, Request and Response, and
 * come with them.
 *
 * Copyright 2010 The Go Authors. All rights reserved.
 * Copyright 2026 The burrow Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style licence that can be found
 * in the LICENSE file. */

#include "check.h"

#include "../src/net/http_internal.h"

#include "burrow/burrow.h"
#include "burrow/bytes.h"
#include "burrow/log.h"
#include "burrow/map.h"
#include "burrow/mem/arena.h"
#include "burrow/net/http.h"
#include "burrow/panic.h"
#include "burrow/strings.h"
#include "burrow/time.h"

#include <stdint.h>
#include <string.h>

#define S BURROW_S
#define T BURROW_S_INIT

#define ARENA_BEGIN                                                                    \
    Arena ar;                                                                          \
    arena_init(&ar, NULL, 0);                                                          \
    Alloc *a = arena_allocator(&ar)
#define ARENA_END arena_free(&ar)

#define MAX_NUM BURROW__HTTP_DEFAULT_COOKIE_MAX_NUM

/* A cookie as Go's tests write one. exp says expires is set, to exp_sec and
 * exp_nsec after the Unix epoch in UTC. */
typedef struct CK {
    Str name, value, path, domain, raw_expires, raw;
    Str unparsed[4];
    int64_t exp_sec;
    Int max_age;
    int nunparsed;
    int exp_nsec;
    int same_site;
    bool exp, quoted, secure, http_only, partitioned;
} CK;

static HttpCookie ck_cookie(const CK *k) {
    HttpCookie c = {0};
    c.name = k->name;
    c.value = k->value;
    c.quoted = k->quoted;
    c.path = k->path;
    c.domain = k->domain;
    if (k->exp)
        c.expires = time_utc(time_from_unix(k->exp_sec, k->exp_nsec));
    c.raw_expires = k->raw_expires;
    c.max_age = k->max_age;
    c.secure = k->secure;
    c.http_only = k->http_only;
    c.same_site = (HttpSameSite)k->same_site;
    c.partitioned = k->partitioned;
    c.raw = k->raw;
    return c;
}

/* reflect.DeepEqual for a cookie. */
static bool ck_equal(const HttpCookie *c, const CK *k) {
    if (!str_eq(c->name, k->name) || !str_eq(c->value, k->value) ||
        c->quoted != k->quoted || !str_eq(c->path, k->path) ||
        !str_eq(c->domain, k->domain) || !str_eq(c->raw_expires, k->raw_expires) ||
        c->max_age != k->max_age || c->secure != k->secure ||
        c->http_only != k->http_only || (int)c->same_site != k->same_site ||
        c->partitioned != k->partitioned || !str_eq(c->raw, k->raw))
        return false;
    if (k->exp ? !time_equal(c->expires, time_from_unix(k->exp_sec, k->exp_nsec))
               : !time_is_zero(c->expires))
        return false;
    if (c->unparsed.len != k->nunparsed)
        return false;
    for (int i = 0; i < k->nunparsed; i++) {
        if (!str_eq(BURROW_AT(Str, c->unparsed, i), k->unparsed[i]))
            return false;
    }
    return true;
}

/* A header with the values in h, h[0] being the key, as it is with no
 * canonical form, the way a Go map literal has it. */
static HttpHeader ck_header(Alloc *a, const Str *h, int nh) {
    HttpHeader hdr = http_header_make(a);
    if (hdr == NULL)
        panic_str(S("out of memory"));
    Slice vs = slice_make(a, TYPE_STRING, nh, nh);
    if (nh > 0 && vs.p == NULL)
        panic_str(S("out of memory"));
    for (int i = 0; i < nh; i++)
        ((Str *)vs.p)[i] = h[i + 1];
    if (!map_set(hdr, &h[0], &vs))
        panic_str(S("out of memory"));
    return hdr;
}

/* A header with key set to n copies of value. */
static HttpHeader ck_header_repeat(Alloc *a, Str key, Str value, Int n) {
    HttpHeader hdr = http_header_make(a);
    Slice vs = slice_make(a, TYPE_STRING, n, n);
    if (hdr == NULL || vs.p == NULL)
        panic_str(S("out of memory"));
    for (Int i = 0; i < n; i++)
        ((Str *)vs.p)[i] = value;
    if (!map_set(hdr, &key, &vs))
        panic_str(S("out of memory"));
    return hdr;
}

/* strings.Repeat(";a=", n)[1:]. */
static Str ck_repeat_line(Alloc *a, Int n) {
    Str s = strings_repeat(a, S(";a="), n);
    if (s.p == NULL)
        panic_str(S("out of memory"));
    return str_from_bytes(s.p + 1, s.len - 1);
}

/* Whether got is n cookies named "a" with empty values. */
static bool ck_all_a(Slice got, Int n) {
    if (got.len != n)
        return false;
    for (Int i = 0; i < n; i++) {
        HttpCookie c = BURROW_AT(HttpCookie, got, i);
        if (!str_eq(c.name, S("a")) || c.value.len != 0 || c.quoted)
            return false;
    }
    return true;
}

/* The standard logger's output goes to b until ck_log_end. */
static void ck_log_begin(BytesBuffer *b) {
    log_set_output(bytes_buffer_as_io_writer(b));
}

static Str ck_log_end(BytesBuffer *b) {
    log_set_output((IoWriter){0});
    Slice s = bytes_buffer_bytes(b);
    return str_from_bytes(s.p, s.len);
}

/* ---------------------------------------------------------------- the tests */

static void TestWriteSetCookies(TestingT *t) {
    static const struct {
        CK cookie;
        Str raw;
    } tests[] = {
        /* PROBE: writeSetCookiesTests */
    };
    ARENA_BEGIN;
    BytesBuffer logbuf = BYTES_BUFFER(a);
    ck_log_begin(&logbuf);
    for (size_t i = 0; i < sizeof tests / sizeof tests[0]; i++) {
        HttpCookie c = ck_cookie(&tests[i].cookie);
        Str g = http_cookie_string(a, &c);
        if (!str_eq(g, tests[i].raw))
            testing_t_errorf_v(t, "Test %d, expecting:\n%s\nGot:\n%s\n", (int)i,
                               tests[i].raw, g);
    }
    Str got = ck_log_end(&logbuf);
    Str sub = S("dropping domain attribute");
    if (!strings_contains(got, sub))
        testing_t_errorf_v(t, "Expected substring %q in log output. Got:\n%s", sub,
                           got);
    ARENA_END;
}

/* Header{"Set-Cookie": {...}} or Header{"Cookie": {...}}, and the cookies. */
typedef struct CKRead {
    Str h[4];
    Str filter;
    CK want[6];
    const char *godebug;
    int nh;
    int n;
} CKRead;

static void TestReadSetCookies(TestingT *t) {
    static const CKRead tests[] = {
        /* PROBE: readSetCookiesTests */
    };
    ARENA_BEGIN;
    for (size_t i = 0; i < sizeof tests / sizeof tests[0]; i++) {
        burrow__http_godebug_set(tests[i].godebug);
        HttpHeader h = ck_header(a, tests[i].h, tests[i].nh);
        /* Twice, to see that readSetCookies leaves its input alone. */
        for (int n = 0; n < 2; n++) {
            Slice c = burrow__http_read_set_cookies(a, h);
            bool ok = c.len == tests[i].n;
            for (int j = 0; ok && j < tests[i].n; j++)
                ok = ck_equal(&BURROW_AT(HttpCookie, c, j), &tests[i].want[j]);
            if (!ok)
                testing_t_errorf_v(t,
                                   "#%d readSetCookies: have %d cookies, want %d, or "
                                   "one is not the same",
                                   (int)i, (int)c.len, tests[i].n);
        }
    }
    /* The rest of Go's table: more Set-Cookie values than the limit, and the
     * limit taken away. */
    struct {
        const char *godebug;
        Int n, want;
    } limits[] = {
        {"", MAX_NUM + 1, 0},
        {"httpcookiemaxnum=5", 10, 0},
        {"httpcookiemaxnum=0", MAX_NUM + 1, MAX_NUM + 1},
        {"httpcookiemaxnum=3001", MAX_NUM + 1, MAX_NUM + 1},
    };
    for (size_t i = 0; i < sizeof limits / sizeof limits[0]; i++) {
        burrow__http_godebug_set(limits[i].godebug);
        HttpHeader h = ck_header_repeat(a, S("Set-Cookie"), S("a="), limits[i].n);
        Slice c = burrow__http_read_set_cookies(a, h);
        if (c.len != limits[i].want) {
            testing_t_errorf_v(t, "GODEBUG=%s: %d cookies, want %d", limits[i].godebug,
                               (int)c.len, (int)limits[i].want);
            continue;
        }
        bool ok = true;
        for (Int j = 0; ok && j < c.len; j++) {
            HttpCookie k = BURROW_AT(HttpCookie, c, j);
            ok = str_eq(k.name, S("a")) && k.value.len == 0 && str_eq(k.raw, S("a="));
        }
        if (!ok)
            testing_t_errorf_v(t, "GODEBUG=%s: a cookie is not a=", limits[i].godebug);
    }
    burrow__http_godebug_set(NULL);
    ARENA_END;
}

static void TestReadCookies(TestingT *t) {
    static const CKRead tests[] = {
        /* PROBE: readCookiesTests */
    };
    ARENA_BEGIN;
    for (size_t i = 0; i < sizeof tests / sizeof tests[0]; i++) {
        burrow__http_godebug_set(tests[i].godebug);
        HttpHeader h = ck_header(a, tests[i].h, tests[i].nh);
        for (int n = 0; n < 2; n++) {
            Slice c = burrow__http_read_cookies(a, h, tests[i].filter);
            bool ok = c.len == tests[i].n;
            for (int j = 0; ok && j < tests[i].n; j++)
                ok = ck_equal(&BURROW_AT(HttpCookie, c, j), &tests[i].want[j]);
            if (!ok)
                testing_t_errorf_v(t,
                                   "#%d readCookies: have %d cookies, want %d, or one "
                                   "is not the same",
                                   (int)i, (int)c.len, tests[i].n);
        }
    }
    /* GODEBUG=httpcookiemaxnum works whether the cookies come in one Cookie
     * field or in many. */
    Str one = ck_repeat_line(a, MAX_NUM + 1);
    struct {
        const char *godebug;
        bool many;
        Int n, want;
    } limits[] = {
        {"", false, MAX_NUM + 1, 0},
        {"httpcookiemaxnum=5", true, 10, 0},
        {"httpcookiemaxnum=0", false, MAX_NUM + 1, MAX_NUM + 1},
        {"httpcookiemaxnum=3001", true, MAX_NUM + 1, MAX_NUM + 1},
    };
    for (size_t i = 0; i < sizeof limits / sizeof limits[0]; i++) {
        burrow__http_godebug_set(limits[i].godebug);
        HttpHeader h = limits[i].many
                           ? ck_header_repeat(a, S("Cookie"), S("a="), limits[i].n)
                           : ck_header_repeat(a, S("Cookie"), one, 1);
        for (int n = 0; n < 2; n++) {
            Slice c = burrow__http_read_cookies(a, h, BURROW_STR_EMPTY);
            if (!ck_all_a(c, limits[i].want))
                testing_t_errorf_v(t, "GODEBUG=%s: %d cookies, want %d a=",
                                   limits[i].godebug, (int)c.len, (int)limits[i].want);
        }
    }
    burrow__http_godebug_set(NULL);
    ARENA_END;
}

static void TestCookieSanitizeValue(TestingT *t) {
    static const struct {
        Str in;
        bool quoted;
        Str want;
    } tests[] = {
        /* PROBE: sanitizeCookieValue */
    };
    ARENA_BEGIN;
    BytesBuffer logbuf = BYTES_BUFFER(a);
    ck_log_begin(&logbuf);
    for (size_t i = 0; i < sizeof tests / sizeof tests[0]; i++) {
        Str got = burrow__http_sanitize_cookie_value(a, tests[i].in, tests[i].quoted);
        if (!str_eq(got, tests[i].want))
            testing_t_errorf_v(t, "sanitizeCookieValue(%q) = %q; want %q", tests[i].in,
                               got, tests[i].want);
    }
    Str got = ck_log_end(&logbuf);
    Str sub = S("dropping invalid bytes");
    if (!strings_contains(got, sub))
        testing_t_errorf_v(t, "Expected substring %q in log output. Got:\n%s", sub,
                           got);
    ARENA_END;
}

static void TestCookieSanitizePath(TestingT *t) {
    static const struct {
        Str in, want;
    } tests[] = {
        /* PROBE: sanitizeCookiePath */
    };
    ARENA_BEGIN;
    BytesBuffer logbuf = BYTES_BUFFER(a);
    ck_log_begin(&logbuf);
    for (size_t i = 0; i < sizeof tests / sizeof tests[0]; i++) {
        Str got = burrow__http_sanitize_cookie_path(a, tests[i].in);
        if (!str_eq(got, tests[i].want))
            testing_t_errorf_v(t, "sanitizeCookiePath(%q) = %q; want %q", tests[i].in,
                               got, tests[i].want);
    }
    Str got = ck_log_end(&logbuf);
    Str sub = S("dropping invalid bytes");
    if (!strings_contains(got, sub))
        testing_t_errorf_v(t, "Expected substring %q in log output. Got:\n%s", sub,
                           got);
    ARENA_END;
}

static void TestCookieValid(TestingT *t) {
    Time t1600 = time_date(1600, TIME_JANUARY, 1, 1, 1, 1, 1, time_utc_loc);
    Time epoch = time_from_unix(0, 0);
    struct {
        HttpCookie cookie;
        bool nil;
        bool valid;
    } tests[] = {
        {{0}, true, false},
        {{.name = T("")}, false, false},
        {{.name = T("invalid-value"), .value = T("foo\"bar")}, false, false},
        {{.name = T("invalid-path"), .path = T("/foo;bar/")}, false, false},
        {{.name = T("invalid-secure-for-partitioned"),
          .value = T("foo"),
          .path = T("/"),
          .secure = false,
          .partitioned = true},
         false,
         false},
        {{.name = T("invalid-domain"), .domain = T("example.com:80")}, false, false},
        {{.name = T("invalid-expiry"), .value = T(""), .expires = t1600}, false, false},
        {{.name = T("valid-empty")}, false, true},
        {{.name = T("valid-expires"),
          .value = T("foo"),
          .path = T("/bar"),
          .domain = T("example.com"),
          .expires = epoch},
         false,
         true},
        {{.name = T("valid-max-age"),
          .value = T("foo"),
          .path = T("/bar"),
          .domain = T("example.com"),
          .max_age = 60},
         false,
         true},
        {{.name = T("valid-all-fields"),
          .value = T("foo"),
          .path = T("/bar"),
          .domain = T("example.com"),
          .expires = epoch,
          .max_age = 0},
         false,
         true},
        {{.name = T("valid-partitioned"),
          .value = T("foo"),
          .path = T("/"),
          .secure = true,
          .partitioned = true},
         false,
         true},
    };
    for (size_t i = 0; i < sizeof tests / sizeof tests[0]; i++) {
        Error err = http_cookie_valid(tests[i].nil ? NULL : &tests[i].cookie);
        if (BURROW_FAILED(err) && tests[i].valid)
            testing_t_errorf_v(t, "%s.Valid() returned error %v; want nil",
                               tests[i].cookie.name, err);
        if (BURROW_OK(err) && !tests[i].valid)
            testing_t_errorf_v(t, "%s.Valid() returned nil; want error",
                               tests[i].cookie.name);
    }
}

/* The name of a cookie error, for the tables. */
enum {
    none,
    blank,
    equal_not_found,
    invalid_name,
    invalid_value,
    num_limit,
};

static Error ck_err(int e) {
    switch (e) {
    case blank:
        return burrow__http_err_blank_cookie;
    case equal_not_found:
        return burrow__http_err_equal_not_found_in_cookie;
    case invalid_name:
        return burrow__http_err_invalid_cookie_name;
    case invalid_value:
        return burrow__http_err_invalid_cookie_value;
    case num_limit:
        return burrow__http_err_cookie_num_limit_exceeded;
    default:
        return BURROW_NO_ERROR;
    }
}

static void TestParseCookie(TestingT *t) {
    static const struct {
        Str line;
        int err;
        CK want[2];
        int n;
    } tests[] = {
        /* PROBE: TestParseCookie */
    };
    ARENA_BEGIN;
    for (size_t i = 0; i < sizeof tests / sizeof tests[0]; i++) {
        Error err;
        Slice got = http_parse_cookie(a, tests[i].line, &err);
        if (!errors_is(err, ck_err(tests[i].err)) ||
            (tests[i].err == none && BURROW_FAILED(err)))
            testing_t_errorf_v(t, "#%d ParseCookie got error %v, want error %v", (int)i,
                               err, ck_err(tests[i].err));
        bool ok = got.len == tests[i].n;
        for (int j = 0; ok && j < tests[i].n; j++)
            ok = ck_equal(&BURROW_AT(HttpCookie, got, j), &tests[i].want[j]);
        if (!ok)
            testing_t_errorf_v(t,
                               "#%d ParseCookie: got %d cookies, want %d, or one is "
                               "not the same",
                               (int)i, (int)got.len, tests[i].n);
    }
    /* The last case of Go's table: 3001 cookies with the limit raised. */
    burrow__http_godebug_set("httpcookiemaxnum=3001");
    Error err;
    Slice got = http_parse_cookie(a, ck_repeat_line(a, MAX_NUM + 1), &err);
    if (BURROW_FAILED(err) || !ck_all_a(got, MAX_NUM + 1))
        testing_t_errorf_v(t, "ParseCookie of %d cookies: error %v, %d cookies",
                           MAX_NUM + 1, err, (int)got.len);
    /* And without it raised, which is the error. */
    burrow__http_godebug_set("");
    got = http_parse_cookie(a, ck_repeat_line(a, MAX_NUM + 1), &err);
    if (!errors_is(err, burrow__http_err_cookie_num_limit_exceeded) || got.len != 0)
        testing_t_errorf_v(t, "ParseCookie of %d cookies: error %v, %d cookies",
                           MAX_NUM + 1, err, (int)got.len);
    burrow__http_godebug_set(NULL);
    ARENA_END;
}

static void TestParseSetCookie(TestingT *t) {
    static const struct {
        Str line;
        int err;
        CK want;
    } tests[] = {
        /* PROBE: TestParseSetCookie */
    };
    ARENA_BEGIN;
    for (size_t i = 0; i < sizeof tests / sizeof tests[0]; i++) {
        Error err;
        HttpCookie got = http_parse_set_cookie(a, tests[i].line, &err);
        if (!errors_is(err, ck_err(tests[i].err)) ||
            (tests[i].err == none && BURROW_FAILED(err))) {
            testing_t_errorf_v(t, "#%d ParseSetCookie got error %v, want error %v",
                               (int)i, err, ck_err(tests[i].err));
            continue;
        }
        if (!ck_equal(&got, &tests[i].want))
            testing_t_errorf_v(t, "#%d ParseSetCookie: got %s, want %s", (int)i,
                               http_cookie_string(a, &got), tests[i].want.raw);
    }
    ARENA_END;
}

/* burrow's own. On the heap, a parsed cookie gives back what it took, and
 * String gives back what sanitizing took. */
static void TestCookieHeap(TestingT *t) {
    Alloc *a = heap_allocator();
    Error err;
    HttpCookie c = http_parse_set_cookie(
        a, S("id=a3fWa; Expires=Wed, 21 Oct 2015 07:28:00 GMT; Lang=en; Bad=\\"), &err);
    CHECK(BURROW_OK(err));
    CHECK(c.unparsed.len == 2);
    Str s = http_cookie_string(a, &c);
    CHECK(str_eq(s, S("id=a3fWa; Expires=Wed, 21 Oct 2015 07:28:00 GMT")));
    mem_free(a, (void *)(uintptr_t)s.p, (size_t)s.len, 1);
    http_cookie_free(a, &c);

    BytesBuffer logbuf = BYTES_BUFFER(a);
    ck_log_begin(&logbuf);
    HttpCookie d = {.name = T("n"), .value = T("a b;c"), .path = T("/x;y")};
    s = http_cookie_string(a, &d);
    CHECK(str_eq(s, S("n=\"a bc\"; Path=/xy")));
    (void)ck_log_end(&logbuf);
    bytes_buffer_free(&logbuf);
    if (s.p != NULL)
        mem_free(a, (void *)(uintptr_t)s.p, (size_t)s.len, 1);

    Slice cs = http_parse_cookie(a, S("a=1; b=\"2\""), &err);
    CHECK(BURROW_OK(err) && cs.len == 2);
    mem_free(a, cs.p, (size_t)cs.cap * sizeof(HttpCookie), _Alignof(HttpCookie));
    (void)t;
}

#define TESTS(X)                                                                       \
    X(TestWriteSetCookies)                                                             \
    X(TestReadSetCookies)                                                              \
    X(TestReadCookies)                                                                 \
    X(TestCookieSanitizeValue)                                                         \
    X(TestCookieSanitizePath)                                                          \
    X(TestCookieValid)                                                                 \
    X(TestParseCookie)                                                                 \
    X(TestParseSetCookie)                                                              \
    X(TestCookieHeap)

TESTING_MAIN(TESTS)
