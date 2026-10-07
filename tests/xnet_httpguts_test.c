/* Derived from Go's src/vendor/golang.org/x/net/http/httpguts/httplex_test.go.
 * Go source: go1.27.1, golang.org/x/net v0.55.1-0.20260731170536-c1d18010be90.
 *
 * Go vendors the package without its test, so this is the test from the x/net
 * commit Go vendors. The benchmarks are left out.
 *
 * TestValidTrailerHeader, TestValidHostHeader and TestValidHeaderFieldValue
 * are burrow's own. The first checks the trailer test, which does without
 * textproto.CanonicalMIMEHeaderKey, against one that uses it.
 *
 * Copyright 2009 The Go Authors. All rights reserved.
 * Copyright 2026 The burrow Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style licence that can be found
 * in the LICENSE file. */

#include "check.h"

#include "../src/xnet/httpguts.h"

#include "burrow/burrow.h"
#include "burrow/math/rand.h"
#include "burrow/mem/arena.h"
#include "burrow/net/textproto.h"
#include "burrow/strings.h"

#include <stdint.h>
#include <string.h>

static bool is_char(int32_t c) {
    return c <= 127;
}

static bool is_ctl(int32_t c) {
    return c <= 31 || c == 127;
}

static bool is_separator(int32_t c) {
    switch (c) {
    case '(':
    case ')':
    case '<':
    case '>':
    case '@':
    case ',':
    case ';':
    case ':':
    case '\\':
    case '"':
    case '/':
    case '[':
    case ']':
    case '?':
    case '=':
    case '{':
    case '}':
    case ' ':
    case '\t':
        return true;
    default:
        return false;
    }
}

static void TestIsTokenRune(TestingT *t) {
    for (int32_t r = 0; r <= 130; r++) {
        bool expected = is_char(r) && !is_ctl(r) && !is_separator(r);
        if (burrow__httpguts_is_token_rune(r) != expected)
            testing_t_errorf_v(t, "isToken(0x%x) = %t", (Int)r, !expected);
    }
    if (burrow__httpguts_is_token_rune(-1) || burrow__httpguts_is_token_rune(0x10FFFF))
        testing_t_errorf_v(t, "a rune outside ASCII is a token rune");
}

static void TestHeaderValuesContainsToken(TestingT *t) {
    static const struct {
        const char *vals[3];
        Int n;
        const char *token;
        bool want;
    } tests[] = {
        {{"foo"}, 1, "foo", true},         {{"bar", "foo"}, 2, "foo", true},
        {{"foo"}, 1, "FOO", true},         {{"foo"}, 1, "bar", false},
        {{" foo "}, 1, "FOO", true},       {{"foo,bar"}, 1, "FOO", true},
        {{"bar,foo,bar"}, 1, "FOO", true}, {{"bar , foo"}, 1, "FOO", true},
        {{"foo ,bar "}, 1, "FOO", true},   {{"bar, foo ,bar"}, 1, "FOO", true},
        {{"bar , foo"}, 1, "FOO", true},
    };
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    for (size_t i = 0; i < sizeof tests / sizeof tests[0]; i++) {
        Str vals[3];
        Str shown = BURROW_S("[");
        for (Int k = 0; k < tests[i].n; k++) {
            vals[k] = str_from_cstr(tests[i].vals[k]);
            shown = fmt_sprintf_v(a, "%s%s%q", shown, k > 0 ? " " : "", vals[k]);
        }
        shown = fmt_sprintf_v(a, "%s]", shown);
        Str token = str_from_cstr(tests[i].token);
        bool got =
            burrow__httpguts_header_values_contains_token(vals, tests[i].n, token);
        if (got != tests[i].want)
            testing_t_errorf_v(t, "headerValuesContainsToken(%s, %q) = %t; want %t",
                               shown, token, got, tests[i].want);
    }
    arena_free(&ar);
}

static void TestValidHeaderFieldName(TestingT *t) {
    static const struct {
        const char *in;
        bool want;
    } tests[] = {
        {"", false},
        {"Accept Charset", false},
        {"Accept-Charset", true},
        {"AccepT-EncodinG", true},
        {"CONNECTION", true},
        {"r\xc3\xa9sum\xc3\xa9", false},
    };
    for (size_t i = 0; i < sizeof tests / sizeof tests[0]; i++) {
        Str in = str_from_cstr(tests[i].in);
        bool got = burrow__httpguts_valid_header_field_name(in);
        if (tests[i].want != got)
            testing_t_errorf_v(t, "ValidHeaderFieldName(%q) = %t; want %t", in, got,
                               tests[i].want);
    }
}

static void TestPunycodeHostPort(TestingT *t) {
    static const struct {
        const char *in, *want;
    } tests[] = {
        {"www.google.com", "www.google.com"},
        {"\xd0\xb3\xd0\xbe\xd1\x84\xd0\xb5\xd1\x80.\xd1\x80\xd1\x84",
         "xn--c1ae0ajs.xn--p1ai"},
        {"b\xc3\xbc"
         "cher.de",
         "xn--bcher-kva.de"},
        {"b\xc3\xbc"
         "cher.de:8080",
         "xn--bcher-kva.de:8080"},
        {"[1::6]:8080", "[1::6]:8080"},
    };
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    for (size_t i = 0; i < sizeof tests / sizeof tests[0]; i++) {
        Str in = str_from_cstr(tests[i].in);
        Str want = str_from_cstr(tests[i].want);
        Error err = BURROW_NO_ERROR;
        Str got = burrow__httpguts_punycode_host_port(a, in, &err);
        if (!str_eq(want, got) || BURROW_FAILED(err))
            testing_t_errorf_v(
                t, "PunycodeHostPort(%q) = %q, %s, want %q, nil", in, got,
                BURROW_OK(err) ? BURROW_S("<nil>") : error_text(err), want);
    }
    arena_free(&ar);
}

/* What guts.go does, with the canonical key made by textproto. */
static bool trailer_by_canonical(Alloc *a, Str name) {
    static const char *const bad[] = {
        "Authorization",
        "Cache-Control",
        "Connection",
        "Content-Encoding",
        "Content-Length",
        "Content-Range",
        "Content-Type",
        "Expect",
        "Host",
        "Keep-Alive",
        "Max-Forwards",
        "Pragma",
        "Proxy-Authenticate",
        "Proxy-Authorization",
        "Proxy-Connection",
        "Range",
        "Realm",
        "Te",
        "Trailer",
        "Transfer-Encoding",
        "Www-Authenticate",
    };
    name = textproto_canonical_mime_header_key(a, name);
    if (strings_has_prefix(name, BURROW_S("If-")))
        return false;
    for (size_t i = 0; i < sizeof bad / sizeof bad[0]; i++)
        if (str_eq(name, str_from_cstr(bad[i])))
            return false;
    return true;
}

static void TestValidTrailerHeader(TestingT *t) {
    static const struct {
        const char *in;
        bool want;
    } tests[] = {
        {"X-Checksum", true},
        {"x-checksum", true},
        {"Content-Length", false},
        {"content-length", false},
        {"CONTENT-LENGTH", false},
        {"te", false},
        {"If-Match", false},
        {"if-none-match", false},
        {"If", true},
        {"Iff-Match", true},
        /* Not a token, so textproto leaves it alone and it matches nothing. */
        {"content length", true},
        {"if match", true},
        {"If-Match ", false},
        {"if-match ", true},
        {"", true},
    };
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    for (size_t i = 0; i < sizeof tests / sizeof tests[0]; i++) {
        Str in = str_from_cstr(tests[i].in);
        bool got = burrow__httpguts_valid_trailer_header(in);
        if (got != tests[i].want)
            testing_t_errorf_v(t, "ValidTrailerHeader(%q) = %t; want %t", in, got,
                               tests[i].want);
        if (trailer_by_canonical(a, in) != tests[i].want)
            testing_t_errorf_v(t, "by the canonical key, ValidTrailerHeader(%q) = %t",
                               in, !tests[i].want);
    }

    /* Every bad trailer in random case, with a random byte swapped in now and
     * then, both ways. */
    static const char alphabet[] = "aAeEiInNoOrRtT-fF ;\xc3"
                                   "cCgGhHkKlLpPsSwWxXyY";
    MathRandRand *r = math_rand_new(a, math_rand_new_source(a, 1));
    if (r == NULL) {
        testing_t_fatalf_v(t, "rand.New failed");
        return;
    }
    static const char *const seeds[] = {
        "authorization",
        "cache-control",
        "connection",
        "content-encoding",
        "content-length",
        "content-range",
        "content-type",
        "expect",
        "host",
        "keep-alive",
        "max-forwards",
        "pragma",
        "proxy-authenticate",
        "proxy-authorization",
        "proxy-connection",
        "range",
        "realm",
        "te",
        "trailer",
        "transfer-encoding",
        "www-authenticate",
        "if-",
        "if-x",
        "x-if-",
    };
    Byte buf[32];
    for (int k = 0; k < 20000; k++) {
        ArenaMark m = arena_mark(&ar);
        const char *s =
            seeds[math_rand_rand_intn(r, (Int)(sizeof seeds / sizeof seeds[0]))];
        Int n = (Int)strlen(s);
        for (Int i = 0; i < n; i++) {
            Byte c = (Byte)s[i];
            if (c >= 'a' && c <= 'z' && math_rand_rand_intn(r, 2) == 0)
                c = (Byte)(c - 'a' + 'A');
            if (math_rand_rand_intn(r, 40) == 0)
                c = (Byte)alphabet[math_rand_rand_intn(r, (Int)sizeof alphabet - 1)];
            buf[i] = c;
        }
        Str in = str_from_bytes(buf, n);
        bool got = burrow__httpguts_valid_trailer_header(in);
        if (got != trailer_by_canonical(a, in))
            testing_t_errorf_v(t,
                               "ValidTrailerHeader(%q) = %t; by the canonical key %t",
                               in, got, !got);
        arena_release(&ar, m);
    }
    arena_free(&ar);
}

static void TestValidHostHeader(TestingT *t) {
    static const struct {
        const char *in;
        bool want;
    } tests[] = {
        {"", true},
        {"example.com", true},
        {"example.com:8080", true},
        {"[::1]:443", true},
        {"[fe80::1%25en0]", true},
        {"a_b~c!$&'()*+,;=", true},
        {"exa mple.com", false},
        {"example.com/", false},
        {"user@example.com", false},
        {"example.com\r\n", false},
        {"b\xc3\xbc"
         "cher.de",
         false},
    };
    for (size_t i = 0; i < sizeof tests / sizeof tests[0]; i++) {
        Str in = str_from_cstr(tests[i].in);
        bool got = burrow__httpguts_valid_host_header(in);
        if (got != tests[i].want)
            testing_t_errorf_v(t, "ValidHostHeader(%q) = %t; want %t", in, got,
                               tests[i].want);
    }
}

static void TestValidHeaderFieldValue(TestingT *t) {
    for (int b = 0; b < 256; b++) {
        Byte c = (Byte)b;
        bool want = !(b < ' ' || b == 0x7F) || b == ' ' || b == '\t';
        bool got = burrow__httpguts_valid_header_field_value(str_from_bytes(&c, 1));
        if (got != want)
            testing_t_errorf_v(t, "ValidHeaderFieldValue(%q) = %t; want %t",
                               str_from_bytes(&c, 1), got, want);
    }
    if (!burrow__httpguts_valid_header_field_value(BURROW_S(" text/html; q=0.9\t")))
        testing_t_errorf_v(t, "a value with spaces and a tab is invalid");
    if (burrow__httpguts_valid_header_field_value(BURROW_S("a\r\nX-Injected: 1")))
        testing_t_errorf_v(t, "a value with CRLF in it is valid");
}

#define TESTS(X)                                                                       \
    X(TestIsTokenRune)                                                                 \
    X(TestHeaderValuesContainsToken)                                                   \
    X(TestValidHeaderFieldName)                                                        \
    X(TestPunycodeHostPort)                                                            \
    X(TestValidTrailerHeader)                                                          \
    X(TestValidHostHeader)                                                             \
    X(TestValidHeaderFieldValue)

TESTING_MAIN(TESTS)
