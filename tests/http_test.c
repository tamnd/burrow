/* Derived from Go's src/net/http/http_test.go.
 * Go source: go1.27.1.
 *
 * TestProtocols, TestRemovePort and TestForeachHeaderElement are Go's. The
 * rest are burrow's own, and what they expect is what Go gives for the same
 * input.
 *
 * Copyright 2014 The Go Authors. All rights reserved.
 * Copyright 2026 The burrow Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style licence that can be found
 * in the LICENSE file. */

#include "check.h"

#include "../src/net/http_internal.h"

#include "burrow/burrow.h"
#include "burrow/mem/arena.h"
#include "burrow/net/http.h"

#include <string.h>

static void TestProtocols(TestingT *t) {
    HttpProtocols p = {0};
    if (http_protocols_http1(p))
        testing_t_errorf_v(t, "zero-value protocols: p.HTTP1() = true, want false");
    http_protocols_set_http1(&p, true);
    http_protocols_set_http2(&p, true);
    if (!http_protocols_http1(p))
        testing_t_errorf_v(t, "initialized protocols: p.HTTP1() = false, want true");
    if (!http_protocols_http2(p))
        testing_t_errorf_v(t, "initialized protocols: p.HTTP2() = false, want true");
    http_protocols_set_http1(&p, false);
    if (http_protocols_http1(p))
        testing_t_errorf_v(t, "after unsetting HTTP1: p.HTTP1() = true, want false");
    if (!http_protocols_http2(p))
        testing_t_errorf_v(t, "after unsetting HTTP1: p.HTTP2() = false, want true");
}

static void TestRemovePort(TestingT *t) {
    static const struct {
        const char *in, *want;
    } tests[] = {
        {"example.com:8080", "example.com"},    {"example.com", "example.com"},
        {"[2001:db8::1]:443", "[2001:db8::1]"}, {"[2001:db8::1]", "[2001:db8::1]"},
        {"192.0.2.1:8080", "192.0.2.1"},        {"192.0.2.1", "192.0.2.1"},
    };
    for (size_t i = 0; i < sizeof tests / sizeof tests[0]; i++) {
        Str got = burrow__http_remove_port(str_from_cstr(tests[i].in));
        if (!str_eq(got, str_from_cstr(tests[i].want)))
            testing_t_errorf_v(t, "removePort(%q) = %q; want %q", tests[i].in, got,
                               tests[i].want);
    }
}

/* Protocols.String for every set of the four bits, HTTP/3 included, though
 * only net/http itself can set that one. */
static void TestProtocolsString(TestingT *t) {
    static const struct {
        uint8_t bits;
        const char *want;
    } tests[] = {
        {0, "{}"},
        {1, "{HTTP1}"},
        {2, "{HTTP2}"},
        {3, "{HTTP1,HTTP2}"},
        {4, "{UnencryptedHTTP2}"},
        {5, "{HTTP1,UnencryptedHTTP2}"},
        {6, "{HTTP2,UnencryptedHTTP2}"},
        {7, "{HTTP1,HTTP2,UnencryptedHTTP2}"},
        {8, "{HTTP3}"},
        {9, "{HTTP1,HTTP3}"},
        {10, "{HTTP2,HTTP3}"},
        {11, "{HTTP1,HTTP2,HTTP3}"},
        {12, "{UnencryptedHTTP2,HTTP3}"},
        {13, "{HTTP1,UnencryptedHTTP2,HTTP3}"},
        {14, "{HTTP2,UnencryptedHTTP2,HTTP3}"},
        {15, "{HTTP1,HTTP2,UnencryptedHTTP2,HTTP3}"},
    };
    for (size_t i = 0; i < sizeof tests / sizeof tests[0]; i++) {
        HttpProtocols p = {tests[i].bits};
        Str got = http_protocols_string(p);
        if (!str_eq(got, str_from_cstr(tests[i].want)))
            testing_t_errorf_v(t, "Protocols{%d}.String() = %q; want %q",
                               (Int)tests[i].bits, got, tests[i].want);
    }
    HttpProtocols p = {0};
    burrow__http_protocols_set_http3(&p, true);
    if (!burrow__http_protocols_http3(p) || p.bits != 8)
        testing_t_errorf_v(t, "setHTTP3(true): bits = %d; want 8", (Int)p.bits);
    if (burrow__http_protocols_empty(p))
        testing_t_errorf_v(t, "empty() = true with HTTP3 set");
    burrow__http_protocols_set_http3(&p, false);
    if (!burrow__http_protocols_empty(p))
        testing_t_errorf_v(t, "empty() = false after unsetting HTTP3");
}

/* Every code from -1 to 999. The 62 with a text are what Go's StatusText
 * gives; the rest are "". */
static void TestStatusText(TestingT *t) {
    static const struct {
        int code;
        const char *want;
    } known[] = {
        {100, "Continue"},
        {101, "Switching Protocols"},
        {102, "Processing"},
        {103, "Early Hints"},
        {200, "OK"},
        {201, "Created"},
        {202, "Accepted"},
        {203, "Non-Authoritative Information"},
        {204, "No Content"},
        {205, "Reset Content"},
        {206, "Partial Content"},
        {207, "Multi-Status"},
        {208, "Already Reported"},
        {226, "IM Used"},
        {300, "Multiple Choices"},
        {301, "Moved Permanently"},
        {302, "Found"},
        {303, "See Other"},
        {304, "Not Modified"},
        {305, "Use Proxy"},
        {307, "Temporary Redirect"},
        {308, "Permanent Redirect"},
        {400, "Bad Request"},
        {401, "Unauthorized"},
        {402, "Payment Required"},
        {403, "Forbidden"},
        {404, "Not Found"},
        {405, "Method Not Allowed"},
        {406, "Not Acceptable"},
        {407, "Proxy Authentication Required"},
        {408, "Request Timeout"},
        {409, "Conflict"},
        {410, "Gone"},
        {411, "Length Required"},
        {412, "Precondition Failed"},
        {413, "Request Entity Too Large"},
        {414, "Request URI Too Long"},
        {415, "Unsupported Media Type"},
        {416, "Requested Range Not Satisfiable"},
        {417, "Expectation Failed"},
        {418, "I'm a teapot"},
        {421, "Misdirected Request"},
        {422, "Unprocessable Entity"},
        {423, "Locked"},
        {424, "Failed Dependency"},
        {425, "Too Early"},
        {426, "Upgrade Required"},
        {428, "Precondition Required"},
        {429, "Too Many Requests"},
        {431, "Request Header Fields Too Large"},
        {451, "Unavailable For Legal Reasons"},
        {500, "Internal Server Error"},
        {501, "Not Implemented"},
        {502, "Bad Gateway"},
        {503, "Service Unavailable"},
        {504, "Gateway Timeout"},
        {505, "HTTP Version Not Supported"},
        {506, "Variant Also Negotiates"},
        {507, "Insufficient Storage"},
        {508, "Loop Detected"},
        {510, "Not Extended"},
        {511, "Network Authentication Required"},
    };
    size_t k = 0;
    for (int code = -1; code < 1000; code++) {
        const char *want = "";
        if (k < sizeof known / sizeof known[0] && known[k].code == code)
            want = known[k++].want;
        Str got = http_status_text(code);
        if (!str_eq(got, str_from_cstr(want)))
            testing_t_errorf_v(t, "StatusText(%d) = %q; want %q", (Int)code, got, want);
    }
    if (k != sizeof known / sizeof known[0])
        testing_t_errorf_v(t, "matched %d codes; want %d", (Int)k,
                           (Int)(sizeof known / sizeof known[0]));
}

static void TestHexEscapeNonASCII(TestingT *t) {
    static const struct {
        const char *in, *want;
    } tests[] = {
        {"", ""},
        {"abc", "abc"},
        {"/\xC3\xBC", "/%c3%bc"},
        {"\xFF", "%ff"},
        {"a\x80"
         "b\xFF"
         "c",
         "a%80b%ffc"},
        {"\xE6\x97\xA5\xE6\x9C\xAC", "%e6%97%a5%e6%9c%ac"},
        {"%41", "%41"},
        {"/path?q=\xC3\xA9#f", "/path?q=%c3%a9#f"},
    };
    Arena ar;
    arena_init(&ar, NULL, 0);
    for (size_t i = 0; i < sizeof tests / sizeof tests[0]; i++) {
        Str in = str_from_cstr(tests[i].in);
        Str got = burrow__http_hex_escape_non_ascii(arena_allocator(&ar), in);
        if (!str_eq(got, str_from_cstr(tests[i].want)))
            testing_t_errorf_v(t, "hexEscapeNonASCII(%q) = %q; want %q", tests[i].in,
                               got, tests[i].want);
    }
    arena_free(&ar);
}

static void TestStringContainsCTLByte(TestingT *t) {
    static const struct {
        const char *in;
        size_t len;
        bool want;
    } tests[] = {
        {"", 0, false},    {"abc", 3, false},  {"a\tb", 3, true},
        {"\x00", 1, true}, {"a\x7F", 2, true}, {"\x80", 1, false},
        {"\x1F", 1, true}, {" ~", 2, false},   {"line\r\n", 6, true},
    };
    for (size_t i = 0; i < sizeof tests / sizeof tests[0]; i++) {
        Str in = {(const Byte *)tests[i].in, (Int)tests[i].len};
        bool got = burrow__http_string_contains_ctl_byte(in);
        if (got != tests[i].want)
            testing_t_errorf_v(t, "stringContainsCTLByte(%q) = %t; want %t", in, got,
                               tests[i].want);
    }
}

static void TestIsToken(TestingT *t) {
    static const struct {
        const char *in;
        bool want;
    } tests[] = {
        {"", false},     {"Content-Type", true}, {"X-Foo_Bar", true},
        {"a b", false},  {"a:b", false},         {"!#$%&'*+-.^_`|~", true},
        {"\x80", false}, {"\xC3\xBC", false},    {"a\"b", false},
        {"(x)", false},  {"a/b", false},         {"0123456789", true},
    };
    for (size_t i = 0; i < sizeof tests / sizeof tests[0]; i++) {
        bool got = burrow__http_is_token(str_from_cstr(tests[i].in));
        if (got != tests[i].want)
            testing_t_errorf_v(t, "isToken(%q) = %t; want %t", tests[i].in, got,
                               tests[i].want);
    }
}

typedef struct ForeachGot {
    Str v[8];
    Int n;
} ForeachGot;

static void foreach_append(void *env, Str v) {
    ForeachGot *got = (ForeachGot *)env;
    if (got->n < (Int)(sizeof got->v / sizeof got->v[0]))
        got->v[got->n] = v;
    got->n++;
}

static void TestForeachHeaderElement(TestingT *t) {
    static const struct {
        const char *in;
        const char *want[5];
    } tests[] = {
        {"Foo", {"Foo"}},
        {" Foo", {"Foo"}},
        {"Foo ", {"Foo"}},
        {" Foo ", {"Foo"}},

        {"foo", {"foo"}},
        {"anY-cAsE", {"anY-cAsE"}},

        {"", {0}},
        {",,,,  ,  ,,   ,,, ,", {0}},

        {" Foo,Bar, Baz,lower,,Quux ", {"Foo", "Bar", "Baz", "lower", "Quux"}},
    };
    for (size_t i = 0; i < sizeof tests / sizeof tests[0]; i++) {
        ForeachGot got = {0};
        burrow__http_foreach_header_element(str_from_cstr(tests[i].in), foreach_append,
                                            &got);
        Str want[5];
        Int nwant = 0;
        while (nwant < 5 && tests[i].want[nwant] != NULL) {
            want[nwant] = str_from_cstr(tests[i].want[nwant]);
            nwant++;
        }
        bool equal = got.n == nwant;
        for (Int j = 0; equal && j < nwant; j++)
            equal = str_eq(got.v[j], want[j]);
        if (!equal) {
            Int n = got.n < 8 ? got.n : 8;
            testing_t_errorf_v(t, "foreachHeaderElement(%q) = %q; want %q", tests[i].in,
                               slice_from(got.v, n, n, TYPE_STRING),
                               slice_from(want, nwant, nwant, TYPE_STRING));
        }
    }
}

#define TESTS(X)                                                                       \
    X(TestProtocols)                                                                   \
    X(TestRemovePort)                                                                  \
    X(TestProtocolsString)                                                             \
    X(TestStatusText)                                                                  \
    X(TestHexEscapeNonASCII)                                                           \
    X(TestStringContainsCTLByte)                                                       \
    X(TestIsToken)                                                                     \
    X(TestForeachHeaderElement)

TESTING_MAIN(TESTS)
