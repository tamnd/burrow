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
    if (k->same_site != 0)
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
        {{.name = T("cookie-1"), .value = T("v$1")}, T("cookie-1=v$1")},
        {{.name = T("cookie-2"), .value = T("two"), .max_age = 3600},
         T("cookie-2=two; Max-Age=3600")},
        {{.name = T("cookie-3"), .value = T("three"), .domain = T(".example.com")},
         T("cookie-3=three; Domain=example.com")},
        {{.name = T("cookie-4"), .value = T("four"), .path = T("/restricted/")},
         T("cookie-4=four; Path=/restricted/")},
        {{.name = T("cookie-5"), .value = T("five"), .domain = T("wrong;bad.abc")},
         T("cookie-5=five")},
        {{.name = T("cookie-6"), .value = T("six"), .domain = T("bad-.abc")},
         T("cookie-6=six")},
        {{.name = T("cookie-7"), .value = T("seven"), .domain = T("127.0.0.1")},
         T("cookie-7=seven; Domain=127.0.0.1")},
        {{.name = T("cookie-8"), .value = T("eight"), .domain = T("::1")},
         T("cookie-8=eight")},
        {{.name = T("cookie-9"),
          .value = T("expiring"),
          .exp = 1,
          .exp_sec = 1257894000,
          .exp_nsec = 0},
         T("cookie-9=expiring; Expires=Tue, 10 Nov 2009 23:00:00 GMT")},
        {{.name = T("cookie-10"),
          .value = T("expiring-1601"),
          .exp = 1,
          .exp_sec = -11644469939,
          .exp_nsec = 1},
         T("cookie-10=expiring-1601; Expires=Mon, 01 Jan 1601 01:01:01 GMT")},
        {{.name = T("cookie-11"),
          .value = T("invalid-expiry"),
          .exp = 1,
          .exp_sec = -11676092339,
          .exp_nsec = 1},
         T("cookie-11=invalid-expiry")},
        {{.name = T("cookie-12"), .value = T("samesite-default"), .same_site = 1},
         T("cookie-12=samesite-default")},
        {{.name = T("cookie-13"), .value = T("samesite-lax"), .same_site = 2},
         T("cookie-13=samesite-lax; SameSite=Lax")},
        {{.name = T("cookie-14"), .value = T("samesite-strict"), .same_site = 3},
         T("cookie-14=samesite-strict; SameSite=Strict")},
        {{.name = T("cookie-15"), .value = T("samesite-none"), .same_site = 4},
         T("cookie-15=samesite-none; SameSite=None")},
        {{.name = T("cookie-16"),
          .value = T("partitioned"),
          .path = T("/"),
          .secure = true,
          .same_site = 4,
          .partitioned = true},
         T("cookie-16=partitioned; Path=/; Secure; SameSite=None; Partitioned")},
        {{.name = T("special-1"), .value = T("a z")}, T("special-1=\"a z\"")},
        {{.name = T("special-2"), .value = T(" z")}, T("special-2=\" z\"")},
        {{.name = T("special-3"), .value = T("a ")}, T("special-3=\"a \"")},
        {{.name = T("special-4"), .value = T(" ")}, T("special-4=\" \"")},
        {{.name = T("special-5"), .value = T("a,z")}, T("special-5=\"a,z\"")},
        {{.name = T("special-6"), .value = T(",z")}, T("special-6=\",z\"")},
        {{.name = T("special-7"), .value = T("a,")}, T("special-7=\"a,\"")},
        {{.name = T("special-8"), .value = T(",")}, T("special-8=\",\"")},
        {{.name = T("empty-value")}, T("empty-value=")},
        {.raw = T("")},
        {.raw = T("")},
        {{.name = T("\011")}, T("")},
        {{.name = T("\015")}, T("")},
        {{.name = T("a\012b"), .value = T("v")}, T("")},
        {{.name = T("a\012b"), .value = T("v")}, T("")},
        {{.name = T("a\015b"), .value = T("v")}, T("")},
        {{.name = T("cookie"), .value = T("quoted"), .quoted = true},
         T("cookie=\"quoted\"")},
        {{.name = T("cookie"), .value = T("quoted with spaces"), .quoted = true},
         T("cookie=\"quoted with spaces\"")},
        {{.name = T("cookie"), .value = T("quoted,with,commas"), .quoted = true},
         T("cookie=\"quoted,with,commas\"")},
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
        {.nh = 1,
         .h = {T("Set-Cookie"), T("Cookie-1=v$1")},
         .n = 1,
         .want = {{.name = T("Cookie-1"), .value = T("v$1"), .raw = T("Cookie-1=v$1")}},
         .godebug = ""},
        {.nh = 1,
         .h = {T("Set-Cookie"), T("NID=99=YsDT5i3E-CXax-; expires=Wed, 23-Nov-2011 "
                                  "01:05:03 GMT; path=/; domain=.google.ch; HttpOnly")},
         .n = 1,
         .want = {{.name = T("NID"),
                   .value = T("99=YsDT5i3E-CXax-"),
                   .path = T("/"),
                   .domain = T(".google.ch"),
                   .exp = 1,
                   .exp_sec = 1322010303,
                   .exp_nsec = 0,
                   .raw_expires = T("Wed, 23-Nov-2011 01:05:03 GMT"),
                   .http_only = true,
                   .raw = T("NID=99=YsDT5i3E-CXax-; expires=Wed, 23-Nov-2011 01:05:03 "
                            "GMT; path=/; domain=.google.ch; HttpOnly")}},
         .godebug = ""},
        {.nh = 1,
         .h = {T("Set-Cookie"), T(".ASPXAUTH=7E3AA; expires=Wed, 07-Mar-2012 14:25:06 "
                                  "GMT; path=/; HttpOnly")},
         .n = 1,
         .want = {{.name = T(".ASPXAUTH"),
                   .value = T("7E3AA"),
                   .path = T("/"),
                   .exp = 1,
                   .exp_sec = 1331130306,
                   .exp_nsec = 0,
                   .raw_expires = T("Wed, 07-Mar-2012 14:25:06 GMT"),
                   .http_only = true,
                   .raw = T(".ASPXAUTH=7E3AA; expires=Wed, 07-Mar-2012 14:25:06 GMT; "
                            "path=/; HttpOnly")}},
         .godebug = ""},
        {.nh = 1,
         .h = {T("Set-Cookie"), T("ASP.NET_SessionId=foo; path=/; HttpOnly")},
         .n = 1,
         .want = {{.name = T("ASP.NET_SessionId"),
                   .value = T("foo"),
                   .path = T("/"),
                   .http_only = true,
                   .raw = T("ASP.NET_SessionId=foo; path=/; HttpOnly")}},
         .godebug = ""},
        {.nh = 1,
         .h = {T("Set-Cookie"), T("samesitedefault=foo; SameSite")},
         .n = 1,
         .want = {{.name = T("samesitedefault"),
                   .value = T("foo"),
                   .same_site = 1,
                   .raw = T("samesitedefault=foo; SameSite")}},
         .godebug = ""},
        {.nh = 1,
         .h = {T("Set-Cookie"), T("samesiteinvalidisdefault=foo; SameSite=invalid")},
         .n = 1,
         .want = {{.name = T("samesiteinvalidisdefault"),
                   .value = T("foo"),
                   .same_site = 1,
                   .raw = T("samesiteinvalidisdefault=foo; SameSite=invalid")}},
         .godebug = ""},
        {.nh = 1,
         .h = {T("Set-Cookie"), T("samesitelax=foo; SameSite=Lax")},
         .n = 1,
         .want = {{.name = T("samesitelax"),
                   .value = T("foo"),
                   .same_site = 2,
                   .raw = T("samesitelax=foo; SameSite=Lax")}},
         .godebug = ""},
        {.nh = 1,
         .h = {T("Set-Cookie"), T("samesitestrict=foo; SameSite=Strict")},
         .n = 1,
         .want = {{.name = T("samesitestrict"),
                   .value = T("foo"),
                   .same_site = 3,
                   .raw = T("samesitestrict=foo; SameSite=Strict")}},
         .godebug = ""},
        {.nh = 1,
         .h = {T("Set-Cookie"), T("samesitenone=foo; SameSite=None")},
         .n = 1,
         .want = {{.name = T("samesitenone"),
                   .value = T("foo"),
                   .same_site = 4,
                   .raw = T("samesitenone=foo; SameSite=None")}},
         .godebug = ""},
        {.nh = 1,
         .h = {T("Set-Cookie"), T("special-1=a z")},
         .n = 1,
         .want = {{.name = T("special-1"),
                   .value = T("a z"),
                   .raw = T("special-1=a z")}},
         .godebug = ""},
        {.nh = 1,
         .h = {T("Set-Cookie"), T("special-2=\" z\"")},
         .n = 1,
         .want = {{.name = T("special-2"),
                   .value = T(" z"),
                   .quoted = true,
                   .raw = T("special-2=\" z\"")}},
         .godebug = ""},
        {.nh = 1,
         .h = {T("Set-Cookie"), T("special-3=\"a \"")},
         .n = 1,
         .want = {{.name = T("special-3"),
                   .value = T("a "),
                   .quoted = true,
                   .raw = T("special-3=\"a \"")}},
         .godebug = ""},
        {.nh = 1,
         .h = {T("Set-Cookie"), T("special-4=\" \"")},
         .n = 1,
         .want = {{.name = T("special-4"),
                   .value = T(" "),
                   .quoted = true,
                   .raw = T("special-4=\" \"")}},
         .godebug = ""},
        {.nh = 1,
         .h = {T("Set-Cookie"), T("special-5=a,z")},
         .n = 1,
         .want = {{.name = T("special-5"),
                   .value = T("a,z"),
                   .raw = T("special-5=a,z")}},
         .godebug = ""},
        {.nh = 1,
         .h = {T("Set-Cookie"), T("special-6=\",z\"")},
         .n = 1,
         .want = {{.name = T("special-6"),
                   .value = T(",z"),
                   .quoted = true,
                   .raw = T("special-6=\",z\"")}},
         .godebug = ""},
        {.nh = 1,
         .h = {T("Set-Cookie"), T("special-7=a,")},
         .n = 1,
         .want = {{.name = T("special-7"), .value = T("a,"), .raw = T("special-7=a,")}},
         .godebug = ""},
        {.nh = 1,
         .h = {T("Set-Cookie"), T("special-8=\",\"")},
         .n = 1,
         .want = {{.name = T("special-8"),
                   .value = T(","),
                   .quoted = true,
                   .raw = T("special-8=\",\"")}},
         .godebug = ""},
        {.nh = 1,
         .h = {T("Set-Cookie"), T("special-9 =\",\"")},
         .n = 1,
         .want = {{.name = T("special-9"),
                   .value = T(","),
                   .quoted = true,
                   .raw = T("special-9 =\",\"")}},
         .godebug = ""},
        {.nh = 1,
         .h = {T("Set-Cookie"), T("cookie=\"quoted\"")},
         .n = 1,
         .want = {{.name = T("cookie"),
                   .value = T("quoted"),
                   .quoted = true,
                   .raw = T("cookie=\"quoted\"")}},
         .godebug = ""},
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
        {.nh = 2,
         .h = {T("Cookie"), T("Cookie-1=v$1"), T("c2=v2")},
         .filter = T(""),
         .n = 2,
         .want = {{.name = T("Cookie-1"), .value = T("v$1")},
                  {.name = T("c2"), .value = T("v2")}},
         .godebug = ""},
        {.nh = 2,
         .h = {T("Cookie"), T("Cookie-1=v$1"), T("c2=v2")},
         .filter = T("c2"),
         .n = 1,
         .want = {{.name = T("c2"), .value = T("v2")}},
         .godebug = ""},
        {.nh = 1,
         .h = {T("Cookie"), T("Cookie-1=v$1; c2=v2")},
         .filter = T(""),
         .n = 2,
         .want = {{.name = T("Cookie-1"), .value = T("v$1")},
                  {.name = T("c2"), .value = T("v2")}},
         .godebug = ""},
        {.nh = 1,
         .h = {T("Cookie"), T("Cookie-1=v$1; c2=v2")},
         .filter = T("c2"),
         .n = 1,
         .want = {{.name = T("c2"), .value = T("v2")}},
         .godebug = ""},
        {.nh = 1,
         .h = {T("Cookie"), T("Cookie-1=\"v$1\"; c2=\"v2\"")},
         .filter = T(""),
         .n = 2,
         .want = {{.name = T("Cookie-1"), .value = T("v$1"), .quoted = true},
                  {.name = T("c2"), .value = T("v2"), .quoted = true}},
         .godebug = ""},
        {.nh = 1,
         .h = {T("Cookie"), T("Cookie-1=\"v$1\"; c2=v2;")},
         .filter = T(""),
         .n = 2,
         .want = {{.name = T("Cookie-1"), .value = T("v$1"), .quoted = true},
                  {.name = T("c2"), .value = T("v2")}},
         .godebug = ""},
        {.nh = 1, .h = {T("Cookie"), T("")}, .filter = T(""), .n = 0, .godebug = ""},
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
        {T("foo"), false, T("foo")},
        {T("foo;bar"), false, T("foobar")},
        {T("foo\\bar"), false, T("foobar")},
        {T("foo\"bar"), false, T("foobar")},
        {T("\000~\177\200"), false, T("~")},
        {T("withquotes"), true, T("\"withquotes\"")},
        {T("\"withquotes\""), true, T("\"withquotes\"")},
        {T("a z"), false, T("\"a z\"")},
        {T(" z"), false, T("\" z\"")},
        {T("a "), false, T("\"a \"")},
        {T("a,z"), false, T("\"a,z\"")},
        {T(",z"), false, T("\",z\"")},
        {T("a,"), false, T("\"a,\"")},
        {T(""), true, T("\"\"")},
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
        {T("/path"), T("/path")},
        {T("/path with space/"), T("/path with space/")},
        {T("/just;no;semicolon\000orstuff/"), T("/justnosemicolonorstuff/")},
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
        {.nil = true},
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
        CK want[2];
        int err;
        int n;
    } tests[] = {
        {T("Cookie-1=v$1"), .err = none, .n = 1,
         .want = {{.name = T("Cookie-1"), .value = T("v$1")}}},
        {T("Cookie-1=v$1;c2=v2"), .err = none, .n = 2,
         .want = {{.name = T("Cookie-1"), .value = T("v$1")},
                  {.name = T("c2"), .value = T("v2")}}},
        {T("Cookie-1=\"v$1\";c2=\"v2\""), .err = none, .n = 2,
         .want = {{.name = T("Cookie-1"), .value = T("v$1"), .quoted = true},
                  {.name = T("c2"), .value = T("v2"), .quoted = true}}},
        {T("k1="), .err = none, .n = 1, .want = {{.name = T("k1")}}},
        {T(""), .err = blank, .n = 0},
        {T("equal-not-found"), .err = equal_not_found, .n = 0},
        {T("=v1"), .err = invalid_name, .n = 0},
        {T("k1=\\"), .err = invalid_value, .n = 0},
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
    /* The rest of Go's table: more cookies than the limit, a lower limit, and
     * the limit taken away or raised. */
    struct {
        const char *godebug;
        Int n, want;
        int err;
    } limits[] = {
        {"", MAX_NUM + 1, 0, num_limit},
        {"httpcookiemaxnum=5", 10, 0, num_limit},
        {"httpcookiemaxnum=0", MAX_NUM + 1, MAX_NUM + 1, none},
        {"httpcookiemaxnum=3001", MAX_NUM + 1, MAX_NUM + 1, none},
    };
    for (size_t i = 0; i < sizeof limits / sizeof limits[0]; i++) {
        burrow__http_godebug_set(limits[i].godebug);
        Error err;
        Slice got = http_parse_cookie(a, ck_repeat_line(a, limits[i].n), &err);
        bool err_ok = limits[i].err == none ? BURROW_OK(err)
                                            : errors_is(err, ck_err(limits[i].err));
        if (!err_ok || !ck_all_a(got, limits[i].want))
            testing_t_errorf_v(t,
                               "GODEBUG=%s: ParseCookie of %d cookies: error %v, %d "
                               "cookies, want %d",
                               limits[i].godebug, (int)limits[i].n, err, (int)got.len,
                               (int)limits[i].want);
    }
    burrow__http_godebug_set(NULL);
    ARENA_END;
}

static void TestParseSetCookie(TestingT *t) {
    static const struct {
        Str line;
        int err;
        CK want;
    } tests[] = {
        {T("Cookie-1=v$1"), .err = none,
         .want = {.name = T("Cookie-1"), .value = T("v$1"), .raw = T("Cookie-1=v$1")}},
        {T("NID=99=YsDT5i3E-CXax-; expires=Wed, 23-Nov-2011 01:05:03 GMT; path=/; "
           "domain=.google.ch; HttpOnly"),
         .err = none,
         .want = {.name = T("NID"),
                  .value = T("99=YsDT5i3E-CXax-"),
                  .path = T("/"),
                  .domain = T(".google.ch"),
                  .exp = 1,
                  .exp_sec = 1322010303,
                  .exp_nsec = 0,
                  .raw_expires = T("Wed, 23-Nov-2011 01:05:03 GMT"),
                  .http_only = true,
                  .raw = T("NID=99=YsDT5i3E-CXax-; expires=Wed, 23-Nov-2011 01:05:03 "
                           "GMT; path=/; domain=.google.ch; HttpOnly")}},
        {T(".ASPXAUTH=7E3AA; expires=Wed, 07-Mar-2012 14:25:06 GMT; path=/; HttpOnly"),
         .err = none,
         .want = {.name = T(".ASPXAUTH"),
                  .value = T("7E3AA"),
                  .path = T("/"),
                  .exp = 1,
                  .exp_sec = 1331130306,
                  .exp_nsec = 0,
                  .raw_expires = T("Wed, 07-Mar-2012 14:25:06 GMT"),
                  .http_only = true,
                  .raw = T(".ASPXAUTH=7E3AA; expires=Wed, 07-Mar-2012 14:25:06 GMT; "
                           "path=/; HttpOnly")}},
        {T("ASP.NET_SessionId=foo; path=/; HttpOnly"), .err = none,
         .want = {.name = T("ASP.NET_SessionId"),
                  .value = T("foo"),
                  .path = T("/"),
                  .http_only = true,
                  .raw = T("ASP.NET_SessionId=foo; path=/; HttpOnly")}},
        {T("samesitedefault=foo; SameSite"), .err = none,
         .want = {.name = T("samesitedefault"),
                  .value = T("foo"),
                  .same_site = 1,
                  .raw = T("samesitedefault=foo; SameSite")}},
        {T("samesiteinvalidisdefault=foo; SameSite=invalid"), .err = none,
         .want = {.name = T("samesiteinvalidisdefault"),
                  .value = T("foo"),
                  .same_site = 1,
                  .raw = T("samesiteinvalidisdefault=foo; SameSite=invalid")}},
        {T("samesitelax=foo; SameSite=Lax"), .err = none,
         .want = {.name = T("samesitelax"),
                  .value = T("foo"),
                  .same_site = 2,
                  .raw = T("samesitelax=foo; SameSite=Lax")}},
        {T("samesitestrict=foo; SameSite=Strict"), .err = none,
         .want = {.name = T("samesitestrict"),
                  .value = T("foo"),
                  .same_site = 3,
                  .raw = T("samesitestrict=foo; SameSite=Strict")}},
        {T("samesitenone=foo; SameSite=None"), .err = none,
         .want = {.name = T("samesitenone"),
                  .value = T("foo"),
                  .same_site = 4,
                  .raw = T("samesitenone=foo; SameSite=None")}},
        {T("special-1=a z"), .err = none,
         .want = {.name = T("special-1"),
                  .value = T("a z"),
                  .raw = T("special-1=a z")}},
        {T("special-2=\" z\""), .err = none,
         .want = {.name = T("special-2"),
                  .value = T(" z"),
                  .quoted = true,
                  .raw = T("special-2=\" z\"")}},
        {T("special-3=\"a \""), .err = none,
         .want = {.name = T("special-3"),
                  .value = T("a "),
                  .quoted = true,
                  .raw = T("special-3=\"a \"")}},
        {T("special-4=\" \""), .err = none,
         .want = {.name = T("special-4"),
                  .value = T(" "),
                  .quoted = true,
                  .raw = T("special-4=\" \"")}},
        {T("special-5=a,z"), .err = none,
         .want = {.name = T("special-5"),
                  .value = T("a,z"),
                  .raw = T("special-5=a,z")}},
        {T("special-6=\",z\""), .err = none,
         .want = {.name = T("special-6"),
                  .value = T(",z"),
                  .quoted = true,
                  .raw = T("special-6=\",z\"")}},
        {T("special-7=a,"), .err = none,
         .want = {.name = T("special-7"), .value = T("a,"), .raw = T("special-7=a,")}},
        {T("special-8=\",\""), .err = none,
         .want = {.name = T("special-8"),
                  .value = T(","),
                  .quoted = true,
                  .raw = T("special-8=\",\"")}},
        {T("special-9 =\",\""), .err = none,
         .want = {.name = T("special-9"),
                  .value = T(","),
                  .quoted = true,
                  .raw = T("special-9 =\",\"")}},
        {T(""), .err = blank},
        {T("equal-not-found"), .err = equal_not_found},
        {T("=v1"), .err = invalid_name},
        {T("k1=\\"), .err = invalid_value},
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
