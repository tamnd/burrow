/* Derived from Go's src/net/http/internal/ascii/print_test.go.
 * Go source: go1.27.1.
 *
 * TestToLower and TestIs are burrow's own, with what Go gives for the same
 * strings.
 *
 * Copyright 2021 The Go Authors. All rights reserved.
 * Copyright 2026 The burrow Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style licence that can be found
 * in the LICENSE file. */

#include "check.h"

#include "../src/net/http_ascii.h"

#include "burrow/burrow.h"
#include "burrow/mem/arena.h"

static void TestEqualFold(TestingT *t) {
    static const struct {
        const char *name;
        const char *a, *b;
        bool want;
    } tests[] = {
        {"empty", "", "", true},
        {"simple match", "CHUNKED", "chunked", true},
        {"same string", "chunked", "chunked", true},
        /* The "K" is KELVIN SIGN, U+212A. */
        {"Unicode Kelvin symbol",
         "chun\xE2\x84\xAA"
         "ed",
         "chunked", false},
    };
    for (size_t i = 0; i < sizeof tests / sizeof tests[0]; i++) {
        bool got = burrow__http_ascii_equal_fold(str_from_cstr(tests[i].a),
                                                 str_from_cstr(tests[i].b));
        if (got != tests[i].want)
            testing_t_errorf_v(t, "%s: AsciiEqualFold(%q,%q): got %t want %t",
                               tests[i].name, tests[i].a, tests[i].b, got,
                               tests[i].want);
    }
}

static void TestIsPrint(TestingT *t) {
    static const struct {
        const char *name;
        const char *in;
        bool want;
    } tests[] = {
        {"empty", "", true},
        {"ASCII low", "This is a space: ' '", true},
        {"ASCII high", "This is a tilde: '~'", true},
        {"ASCII low non-print", "This is a unit separator: \x1F", false},
        {"Ascii high non-print", "This is a Delete: \x7F", false},
        /* The "K" is KELVIN SIGN, U+212A. */
        {"Unicode letter", "Today it's 280\xE2\x84\xAA outside: it's freezing!", false},
        {"Unicode emoji", "Gophers like \xF0\x9F\xA7\x80", false},
    };
    for (size_t i = 0; i < sizeof tests / sizeof tests[0]; i++) {
        bool got = burrow__http_ascii_is_print(str_from_cstr(tests[i].in));
        if (got != tests[i].want)
            testing_t_errorf_v(t, "%s: IsASCIIPrint(%q): got %t want %t", tests[i].name,
                               tests[i].in, got, tests[i].want);
    }
}

static void TestToLower(TestingT *t) {
    static const struct {
        const char *in, *want;
        bool ok;
    } tests[] = {
        {"", "", true},
        {"abc", "abc", true},
        {"ABC", "abc", true},
        {"MiXeD-Case 123", "mixed-case 123", true},
        {"\x7F", "", false},
        {"\xC3\xBC", "", false},
        {"\tA", "", false},
        /* The "K" is KELVIN SIGN, U+212A. */
        {"K\xE2\x84\xAA", "", false},
    };
    Arena ar;
    arena_init(&ar, NULL, 0);
    for (size_t i = 0; i < sizeof tests / sizeof tests[0]; i++) {
        bool ok = false;
        Str got = burrow__http_ascii_to_lower(arena_allocator(&ar),
                                              str_from_cstr(tests[i].in), &ok);
        if (!str_eq(got, str_from_cstr(tests[i].want)) || ok != tests[i].ok)
            testing_t_errorf_v(t, "ToLower(%q) = %q, %t; want %q, %t", tests[i].in, got,
                               ok, tests[i].want, tests[i].ok);
    }
    arena_free(&ar);
}

static void TestIs(TestingT *t) {
    static const struct {
        const char *in;
        size_t len;
        bool want;
    } tests[] = {
        {"", 0, true},      {"abc", 3, true},  {"\x7F", 1, true},
        {"\x80", 1, false}, {"\x00", 1, true}, {"\xE6\x97\xA5\xE6\x9C\xAC", 6, false},
        {"~ \t", 3, true},
    };
    for (size_t i = 0; i < sizeof tests / sizeof tests[0]; i++) {
        Str in = {(const Byte *)tests[i].in, (Int)tests[i].len};
        bool got = burrow__http_ascii_is(in);
        if (got != tests[i].want)
            testing_t_errorf_v(t, "Is(%q) = %t; want %t", in, got, tests[i].want);
    }
}

#define TESTS(X)                                                                       \
    X(TestEqualFold)                                                                   \
    X(TestIsPrint)                                                                     \
    X(TestToLower)                                                                     \
    X(TestIs)

TESTING_MAIN(TESTS)
