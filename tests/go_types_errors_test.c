/* Derived from Go's src/go/types/errors_test.go. Go source: go1.27.1.
 *
 * Copyright 2020 The Go Authors. All rights reserved.
 * Copyright 2026 The burrow Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style licence that can be found
 * in the LICENSE file. */

#include "check.h"

#include "../src/go/types_check.h"
#include "burrow/burrow.h"
#include "burrow/go/token.h"
#include "burrow/go/types.h"
#include "burrow/mem/arena.h"

#define S(lit) BURROW_S(lit)

static void TestError(TestingT *t) {
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    /* Not in Go: an error_ makes its messages with its checker's allocator, so
     * this one has a checker where Go's zero error_ has none. */
    TypesPackage *pkg = types_new_package(a, S("p"), S("p"));
    TypesChecker *check = types_new_checker(a, NULL, token_new_file_set(a), pkg, NULL);
    GtError err = {.check = check};
    GtPosn noposn = burrow__tc_at_pos(TOKEN_NO_POS);

    Str want = S("no error");
    Str got = burrow__tc_error_msg(&err);
    if (!str_eq(got, want))
        testing_t_errorf_v(t, "empty error: got %q, want %q", got, want);

    want = S("foo 42");
    GT_ADDF(&err, noposn, "foo %d", (Int)42);
    got = burrow__tc_error_msg(&err);
    if (!str_eq(got, want))
        testing_t_errorf_v(t, "simple error: got %q, want %q", got, want);

    want = S("foo 42\n\tbar 43");
    GT_ADDF(&err, noposn, "bar %d", (Int)43);
    got = burrow__tc_error_msg(&err);
    if (!str_eq(got, want))
        testing_t_errorf_v(t, "simple error: got %q, want %q", got, want);
    arena_free(&ar);
}

static void TestStripAnnotations(TestingT *t) {
    static const struct {
        Str in, want;
    } tests[] = {
        {BURROW_S_INIT(""), BURROW_S_INIT("")},
        {BURROW_S_INIT("   "), BURROW_S_INIT("   ")},
        {BURROW_S_INIT("foo"), BURROW_S_INIT("foo")},
        {BURROW_S_INIT("foo\342\202\200"), BURROW_S_INIT("foo")},
        {BURROW_S_INIT("foo(T\342\202\200)"), BURROW_S_INIT("foo(T)")},
    };
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    for (Int i = 0; i < (Int)(sizeof(tests) / sizeof(tests[0])); i++) {
        Str got = burrow__tc_strip_annotations(a, tests[i].in);
        if (!str_eq(got, tests[i].want))
            testing_t_errorf_v(t, "%q: got %q; want %q", tests[i].in, got,
                               tests[i].want);
    }
    arena_free(&ar);
}

#define TESTS(X)                                                                       \
    X(TestError)                                                                       \
    X(TestStripAnnotations)

TESTING_MAIN(TESTS)
