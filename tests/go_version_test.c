/* Derived from Go's src/go/version/version_test.go.
 * Go source: go1.27.1.
 *
 * Copyright 2023 The Go Authors. All rights reserved.
 * Copyright 2026 The burrow Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style licence that can be found
 * in the LICENSE file. */

#include "check.h"

#include "burrow/burrow.h"
#include "burrow/go/version.h"
#include "burrow/mem/arena.h"

#define S_(lit) BURROW_S_INIT(lit)
#define NELEM(x) ((Int)(sizeof(x) / sizeof((x)[0])))

static const struct {
    Str in1, in2;
    Int out;
} compare_tests[] = {
    {S_(""), S_(""), 0},
    {S_("x"), S_("x"), 0},
    {S_(""), S_("x"), 0},
    {S_("1"), S_("1.1"), 0},
    {S_("go1"), S_("go1.1"), -1},
    {S_("go1.5"), S_("go1.6"), -1},
    {S_("go1.5"), S_("go1.10"), -1},
    {S_("go1.6"), S_("go1.6.1"), -1},
    {S_("go1.19"), S_("go1.19.0"), 0},
    {S_("go1.19rc1"), S_("go1.19"), -1},
    {S_("go1.20"), S_("go1.20.0"), 0},
    {S_("go1.20"), S_("go1.20.0-bigcorp"), 0},
    {S_("go1.20rc1"), S_("go1.20"), -1},
    {S_("go1.21"), S_("go1.21.0"), -1},
    {S_("go1.21"), S_("go1.21.0-bigcorp"), -1},
    {S_("go1.21"), S_("go1.21rc1"), -1},
    {S_("go1.21rc1"), S_("go1.21.0"), -1},
    {S_("go1.6"), S_("go1.19"), -1},
    {S_("go1.19"), S_("go1.19.1"), -1},
    {S_("go1.19rc1"), S_("go1.19"), -1},
    {S_("go1.19rc1"), S_("go1.19"), -1},
    {S_("go1.19rc1"), S_("go1.19.1"), -1},
    {S_("go1.19rc1"), S_("go1.19rc2"), -1},
    {S_("go1.19.0"), S_("go1.19.1"), -1},
    {S_("go1.19rc1"), S_("go1.19.0"), -1},
    {S_("go1.19alpha3"), S_("go1.19beta2"), -1},
    {S_("go1.19beta2"), S_("go1.19rc1"), -1},
    {S_("go1.1"), S_("go1.99999999999999998"), -1},
    {S_("go1.99999999999999998"), S_("go1.99999999999999999"), -1},
};

static void TestCompare(TestingT *t) {
    for (Int i = 0; i < NELEM(compare_tests); i++) {
        Int out = version_compare(compare_tests[i].in1, compare_tests[i].in2);
        if (out != compare_tests[i].out)
            testing_t_errorf_v(t, "Compare(%+v, %+v) = %+v, want %+v",
                               compare_tests[i].in1, compare_tests[i].in2, out,
                               compare_tests[i].out);
    }
}

static const struct {
    Str in, out;
} lang_tests[] = {
    {S_("bad"), S_("")},
    {S_("go1.2rc3"), S_("go1.2")},
    {S_("go1.2.3"), S_("go1.2")},
    {S_("go1.2"), S_("go1.2")},
    {S_("go1"), S_("go1")},
    {S_("go222"), S_("go222.0")},
    {S_("go1.999testmod"), S_("go1.999")},
};

static void TestLang(TestingT *t) {
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    for (Int i = 0; i < NELEM(lang_tests); i++) {
        Str out = version_lang(a, lang_tests[i].in);
        if (!str_eq(out, lang_tests[i].out))
            testing_t_errorf_v(t, "Lang(%v) = %v, want %v", lang_tests[i].in, out,
                               lang_tests[i].out);
    }
    arena_free(&ar);
}

static const struct {
    Str in;
    bool out;
} is_valid_tests[] = {
    {S_(""), false},       {S_("1.2.3"), false},         {S_("go1.2rc3"), true},
    {S_("go1.2.3"), true}, {S_("go1.999testmod"), true}, {S_("go1.600+auto"), false},
    {S_("go1.22"), true},  {S_("go1.21.0"), true},       {S_("go1.21rc2"), true},
    {S_("go1.21"), true},  {S_("go1.20.0"), true},       {S_("go1.20"), true},
    {S_("go1.19"), true},  {S_("go1.3"), true},          {S_("go1.2"), true},
    {S_("go1"), true},
};

static void TestIsValid(TestingT *t) {
    for (Int i = 0; i < NELEM(is_valid_tests); i++) {
        bool out = version_is_valid(is_valid_tests[i].in);
        if (out != is_valid_tests[i].out)
            testing_t_errorf_v(t, "IsValid(%v) = %v, want %v", is_valid_tests[i].in,
                               out, is_valid_tests[i].out);
    }
}

/* Lang hands back the front of x whenever Go's would be x[:2+len(v)], and
 * only builds a new string for a bare major. */
static void TestLangView(TestingT *t) {
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    Str x = BURROW_S("go1.21.2-custom");
    Str out = version_lang(a, x);
    if (out.p != x.p || out.len != 6)
        testing_t_errorf_v(t, "Lang(%v) = %v, not a view of the input", x, out);
    x = BURROW_S("go1.0.3");
    out = version_lang(a, x);
    if (out.p != x.p || !str_eq(out, BURROW_S("go1")))
        testing_t_errorf_v(t, "Lang(%v) = %v, want a view holding go1", x, out);
    x = BURROW_S("go7");
    out = version_lang(a, x);
    if (!str_eq(out, BURROW_S("go7.0")))
        testing_t_errorf_v(t, "Lang(%v) = %v, want go7.0", x, out);
    arena_free(&ar);
}

#define TESTS(X)                                                                       \
    X(TestCompare)                                                                     \
    X(TestLang)                                                                        \
    X(TestIsValid)                                                                     \
    X(TestLangView)

TESTING_MAIN(TESTS)
