/* Derived from Go's src/go/types/typeterm_test.go, termlist_test.go,
 * typeset_test.go, object_test.go and api_test.go. Go source: go1.27.1.
 *
 * Copyright 2021 The Go Authors. All rights reserved.
 * Copyright 2026 The burrow Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style licence that can be found
 * in the LICENSE file. */

#include "check.h"

#include "../src/go/types_internal.h"
#include "burrow/burrow.h"
#include "burrow/go/types.h"
#include "burrow/mem/arena.h"
#include "burrow/strings.h"

#define S(lit) BURROW_S(lit)
#define S_(lit) BURROW_S_INIT(lit)
#define NELEM(x) ((Int)(sizeof(x) / sizeof((x)[0])))

/* testTerms, made afresh in each test's arena since the terms point at
 * myInt, which is made there too. */
typedef struct {
    Str name;
    TypesTerm *term;
} TestTerm;

enum { N_TEST_TERMS = 7 };

typedef struct {
    TestTerm terms[N_TEST_TERMS];
} TestTerms;

static TypesType my_int(Alloc *a) {
    TypesTypeName *tname = types_new_type_name(a, TOKEN_NO_POS, NULL, S("myInt"), NULL);
    return types_type(
        types_new_named(a, tname, GT_TYP(TYPES_INT), slice_nil(TYPE_UNSAFE_POINTER)));
}

static void make_test_terms(Alloc *a, TestTerms *tt) {
    TypesType my = my_int(a);
    TestTerm terms[N_TEST_TERMS] = {
        {S("∅"), NULL},
        {S("𝓤"), types_new_term(a, false, NULL)},
        {S("int"), types_new_term(a, false, GT_TYP(TYPES_INT))},
        {S("~int"), types_new_term(a, true, GT_TYP(TYPES_INT))},
        {S("string"), types_new_term(a, false, GT_TYP(TYPES_STRING))},
        {S("~string"), types_new_term(a, true, GT_TYP(TYPES_STRING))},
        {S("myInt"), types_new_term(a, false, my)},
    };
    for (Int i = 0; i < N_TEST_TERMS; i++)
        tt->terms[i] = terms[i];
}

static Slice split(Alloc *a, Str s, Int n) {
    Slice r = strings_split(a, s, S(" "));
    if (r.len != n)
        panic_str(BURROW_S("invalid test case"));
    return r;
}

static TypesTerm *test_term(const TestTerms *tt, Str name) {
    for (Int i = 0; i < N_TEST_TERMS; i++)
        if (str_eq(tt->terms[i].name, name))
            return tt->terms[i].term;
    panic_str(BURROW_S("invalid test argument"));
}

static Str term_string(Alloc *a, TypesTerm *x) {
    return types_term_string(a, x);
}

static Str arg(Slice args, Int i) {
    return GT_AT(args, i, Str);
}

static void TestTermString(TestingT *t) {
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    TestTerms tt;
    make_test_terms(a, &tt);
    for (Int i = 0; i < N_TEST_TERMS; i++) {
        Str want = tt.terms[i].name;
        Str got = term_string(a, tt.terms[i].term);
        if (!str_eq(got, want))
            testing_t_errorf_v(t, "%s.String() == %s; want %s", want, got, want);
    }
    arena_free(&ar);
}

static const Str term_equal_tests[] = {
    S_("∅ ∅ T"),         S_("𝓤 𝓤 T"),        S_("int int T"), S_("~int ~int T"),
    S_("myInt myInt T"), S_("∅ 𝓤 F"),        S_("∅ int F"),   S_("∅ ~int F"),
    S_("𝓤 int F"),       S_("𝓤 ~int F"),     S_("𝓤 myInt F"), S_("int ~int F"),
    S_("int myInt F"),   S_("~int myInt F"),
};

static void TestTermEqual(TestingT *t) {
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    TestTerms tt;
    make_test_terms(a, &tt);
    for (Int i = 0; i < NELEM(term_equal_tests); i++) {
        Slice args = split(a, term_equal_tests[i], 3);
        TypesTerm *x = test_term(&tt, arg(args, 0));
        TypesTerm *y = test_term(&tt, arg(args, 1));
        bool want = str_eq(arg(args, 2), S("T"));
        if (burrow__types_term_equal(x, y) != want)
            testing_t_errorf_v(t, "%s.equal(%s) = %t; want %t", arg(args, 0),
                               arg(args, 1), !want, want);
        /* equal is symmetric */
        if (burrow__types_term_equal(y, x) != want)
            testing_t_errorf_v(t, "%s.equal(%s) = %t; want %t", arg(args, 1),
                               arg(args, 0), !want, want);
    }
    arena_free(&ar);
}

static const Str term_union_tests[] = {
    S_("∅ ∅ ∅ ∅"),
    S_("∅ 𝓤 𝓤 ∅"),
    S_("∅ int int ∅"),
    S_("∅ ~int ~int ∅"),
    S_("∅ myInt myInt ∅"),
    S_("𝓤 𝓤 𝓤 ∅"),
    S_("𝓤 int 𝓤 ∅"),
    S_("𝓤 ~int 𝓤 ∅"),
    S_("𝓤 myInt 𝓤 ∅"),
    S_("int int int ∅"),
    S_("int ~int ~int ∅"),
    S_("int string int string"),
    S_("int ~string int ~string"),
    S_("int myInt int myInt"),
    S_("~int ~string ~int ~string"),
    S_("~int myInt ~int ∅"),

    /* union is symmetric, but the result order isn't - repeat symmetric
     * cases explicitly */
    S_("𝓤 ∅ 𝓤 ∅"),
    S_("int ∅ int ∅"),
    S_("~int ∅ ~int ∅"),
    S_("myInt ∅ myInt ∅"),
    S_("int 𝓤 𝓤 ∅"),
    S_("~int 𝓤 𝓤 ∅"),
    S_("myInt 𝓤 𝓤 ∅"),
    S_("~int int ~int ∅"),
    S_("string int string int"),
    S_("~string int ~string int"),
    S_("myInt int myInt int"),
    S_("~string ~int ~string ~int"),
    S_("myInt ~int ~int ∅"),
};

static void TestTermUnion(TestingT *t) {
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    TestTerms tt;
    make_test_terms(a, &tt);
    for (Int i = 0; i < NELEM(term_union_tests); i++) {
        Slice args = split(a, term_union_tests[i], 4);
        TypesTerm *x = test_term(&tt, arg(args, 0));
        TypesTerm *y = test_term(&tt, arg(args, 1));
        TypesTerm *want1 = test_term(&tt, arg(args, 2));
        TypesTerm *want2 = test_term(&tt, arg(args, 3));
        TypesTerm *got1, *got2;
        burrow__types_term_union(x, y, &got1, &got2);
        if (!burrow__types_term_equal(got1, want1) ||
            !burrow__types_term_equal(got2, want2))
            testing_t_errorf_v(t, "%s.union(%s) = %s, %s; want %s, %s", arg(args, 0),
                               arg(args, 1), term_string(a, got1), term_string(a, got2),
                               arg(args, 2), arg(args, 3));
    }
    arena_free(&ar);
}

static const Str term_intersection_tests[] = {
    S_("∅ ∅ ∅"),          S_("∅ 𝓤 ∅"),
    S_("∅ int ∅"),        S_("∅ ~int ∅"),
    S_("∅ myInt ∅"),      S_("𝓤 𝓤 𝓤"),
    S_("𝓤 int int"),      S_("𝓤 ~int ~int"),
    S_("𝓤 myInt myInt"),  S_("int int int"),
    S_("int ~int int"),   S_("int string ∅"),
    S_("int ~string ∅"),  S_("int string ∅"),
    S_("~int ~string ∅"), S_("~int myInt myInt"),
};

static void TestTermIntersection(TestingT *t) {
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    TestTerms tt;
    make_test_terms(a, &tt);
    for (Int i = 0; i < NELEM(term_intersection_tests); i++) {
        Slice args = split(a, term_intersection_tests[i], 3);
        TypesTerm *x = test_term(&tt, arg(args, 0));
        TypesTerm *y = test_term(&tt, arg(args, 1));
        TypesTerm *want = test_term(&tt, arg(args, 2));
        TypesTerm *got = burrow__types_term_intersect(x, y);
        if (!burrow__types_term_equal(got, want))
            testing_t_errorf_v(t, "%s.intersect(%s) = %s; want %s", arg(args, 0),
                               arg(args, 1), term_string(a, got), arg(args, 2));
        /* intersect is symmetric */
        got = burrow__types_term_intersect(y, x);
        if (!burrow__types_term_equal(got, want))
            testing_t_errorf_v(t, "%s.intersect(%s) = %s; want %s", arg(args, 1),
                               arg(args, 0), term_string(a, got), arg(args, 2));
    }
    arena_free(&ar);
}

static const Str term_includes_tests[] = {
    S_("∅ int F"),      S_("𝓤 int T"),      S_("int int T"),     S_("~int int T"),
    S_("~int myInt T"), S_("string int F"), S_("~string int F"), S_("myInt int F"),
};

static void TestTermIncludes(TestingT *t) {
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    TestTerms tt;
    make_test_terms(a, &tt);
    for (Int i = 0; i < NELEM(term_includes_tests); i++) {
        Slice args = split(a, term_includes_tests[i], 3);
        TypesTerm *x = test_term(&tt, arg(args, 0));
        TypesType y = types_term_type(test_term(&tt, arg(args, 1)));
        bool want = str_eq(arg(args, 2), S("T"));
        if (burrow__types_term_includes(x, y) != want)
            testing_t_errorf_v(t, "%s.includes(%s) = %t; want %t", arg(args, 0),
                               arg(args, 1), !want, want);
    }
    arena_free(&ar);
}

static const Str term_subset_of_tests[] = {
    S_("∅ ∅ T"),         S_("𝓤 𝓤 T"),       S_("int int T"),    S_("~int ~int T"),
    S_("myInt myInt T"), S_("∅ 𝓤 T"),       S_("∅ int T"),      S_("∅ ~int T"),
    S_("∅ myInt T"),     S_("𝓤 int F"),     S_("𝓤 ~int F"),     S_("𝓤 myInt F"),
    S_("int ~int T"),    S_("int myInt F"), S_("~int myInt F"), S_("myInt int F"),
    S_("myInt ~int T"),
};

static void TestTermSubsetOf(TestingT *t) {
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    TestTerms tt;
    make_test_terms(a, &tt);
    for (Int i = 0; i < NELEM(term_subset_of_tests); i++) {
        Slice args = split(a, term_subset_of_tests[i], 3);
        TypesTerm *x = test_term(&tt, arg(args, 0));
        TypesTerm *y = test_term(&tt, arg(args, 1));
        bool want = str_eq(arg(args, 2), S("T"));
        if (burrow__types_term_subset_of(x, y) != want)
            testing_t_errorf_v(t, "%s.subsetOf(%s) = %t; want %t", arg(args, 0),
                               arg(args, 1), !want, want);
    }
    arena_free(&ar);
}

static const Str term_disjoint_tests[] = {
    S_("int int F"),       S_("~int ~int F"),   S_("int ~int F"),
    S_("int string T"),    S_("int ~string T"), S_("int myInt T"),
    S_("~int ~string T"),  S_("~int myInt F"),  S_("string myInt T"),
    S_("~string myInt T"),
};

static void TestTermDisjoint(TestingT *t) {
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    TestTerms tt;
    make_test_terms(a, &tt);
    for (Int i = 0; i < NELEM(term_disjoint_tests); i++) {
        Slice args = split(a, term_disjoint_tests[i], 3);
        TypesTerm *x = test_term(&tt, arg(args, 0));
        TypesTerm *y = test_term(&tt, arg(args, 1));
        bool want = str_eq(arg(args, 2), S("T"));
        if (burrow__types_term_disjoint(x, y) != want)
            testing_t_errorf_v(t, "%s.disjoint(%s) = %t; want %t", arg(args, 0),
                               arg(args, 1), !want, want);
        /* disjoint is symmetric */
        if (burrow__types_term_disjoint(y, x) != want)
            testing_t_errorf_v(t, "%s.disjoint(%s) = %t; want %t", arg(args, 1),
                               arg(args, 0), !want, want);
    }
    arena_free(&ar);
}

/* maketl makes a term list from a string of the term list. */
static Slice maketl(Alloc *a, const TestTerms *tt, Str s) {
    s = strings_replace_all(a, s, S(" "), S(""));
    Slice names = strings_split(a, s, S("|"));
    Slice r = burrow__types_ptrs_make(a, names.len);
    for (Int i = 0; i < names.len; i++)
        GT_AT(r, i, TypesTerm *) = test_term(tt, arg(names, i));
    return r;
}

static void TestTermlistAll(TestingT *t) {
    if (!burrow__types_termlist_is_all(burrow__types_all_termlist()))
        testing_t_errorf_v(t, "allTermlist is not the set of all types");
}

static const Str termlist_string_tests[] = {
    S_("∅"),     S_("𝓤"),           S_("int"),
    S_("~int"),  S_("myInt"),       S_("∅ | ∅"),
    S_("𝓤 | 𝓤"), S_("∅ | 𝓤 | int"), S_("∅ | 𝓤 | int | myInt"),
};

static void TestTermlistString(TestingT *t) {
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    TestTerms tt;
    make_test_terms(a, &tt);
    for (Int i = 0; i < NELEM(termlist_string_tests); i++) {
        Str want = termlist_string_tests[i];
        Str got = burrow__types_termlist_string(a, maketl(a, &tt, want));
        if (!str_eq(got, want))
            testing_t_errorf_v(t, "(%s).String() == %s", want, got);
    }
    arena_free(&ar);
}

typedef struct {
    Str xl;
    bool want;
} TermlistBoolTest;

static void TestTermlistIsEmpty(TestingT *t) {
    static const TermlistBoolTest tests[] = {
        {S_("∅"), true},
        {S_("∅ | ∅"), true},
        {S_("∅ | ∅ | 𝓤"), false},
        {S_("∅ | ∅ | myInt"), false},
        {S_("𝓤"), false},
        {S_("𝓤 | int"), false},
        {S_("𝓤 | myInt | ∅"), false},
    };
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    TestTerms tt;
    make_test_terms(a, &tt);
    for (Int i = 0; i < NELEM(tests); i++) {
        Slice xl = maketl(a, &tt, tests[i].xl);
        bool got = burrow__types_termlist_is_empty(xl);
        if (got != tests[i].want)
            testing_t_errorf_v(t, "(%s).isEmpty() == %t; want %t", tests[i].xl, got,
                               tests[i].want);
    }
    arena_free(&ar);
}

static void TestTermlistIsAll(TestingT *t) {
    static const TermlistBoolTest tests[] = {
        {S_("∅"), false},
        {S_("∅ | ∅"), false},
        {S_("int | ~string"), false},
        {S_("~int | myInt"), false},
        {S_("∅ | ∅ | 𝓤"), true},
        {S_("𝓤"), true},
        {S_("𝓤 | int"), true},
        {S_("myInt | 𝓤"), true},
    };
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    TestTerms tt;
    make_test_terms(a, &tt);
    for (Int i = 0; i < NELEM(tests); i++) {
        Slice xl = maketl(a, &tt, tests[i].xl);
        bool got = burrow__types_termlist_is_all(xl);
        if (got != tests[i].want)
            testing_t_errorf_v(t, "(%s).isAll() == %t; want %t", tests[i].xl, got,
                               tests[i].want);
    }
    arena_free(&ar);
}

static void TestTermlistNorm(TestingT *t) {
    static const struct {
        Str xl, want;
    } tests[] = {
        {S_("∅"), S_("∅")},
        {S_("∅ | ∅"), S_("∅")},
        {S_("∅ | int"), S_("int")},
        {S_("∅ | myInt"), S_("myInt")},
        {S_("𝓤 | int"), S_("𝓤")},
        {S_("𝓤 | myInt"), S_("𝓤")},
        {S_("int | myInt"), S_("int | myInt")},
        {S_("~int | int"), S_("~int")},
        {S_("~int | myInt"), S_("~int")},
        {S_("int | ~string | int"), S_("int | ~string")},
        {S_("~int | string | 𝓤 | ~string | int"), S_("𝓤")},
        {S_("~int | string | myInt | ~string | int"), S_("~int | ~string")},
    };
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    TestTerms tt;
    make_test_terms(a, &tt);
    for (Int i = 0; i < NELEM(tests); i++) {
        Slice got = burrow__types_termlist_norm(a, maketl(a, &tt, tests[i].xl));
        Str gs = burrow__types_termlist_string(a, got);
        if (!str_eq(gs, tests[i].want))
            testing_t_errorf_v(t, "(%s).norm() = %s; want %s", tests[i].xl, gs,
                               tests[i].want);
    }
    arena_free(&ar);
}

typedef struct {
    Str xl, yl, want;
} TermlistOpTest;

static void TestTermlistUnion(TestingT *t) {
    static const TermlistOpTest tests[] = {
        {S_("∅"), S_("∅"), S_("∅")},
        {S_("∅"), S_("𝓤"), S_("𝓤")},
        {S_("∅"), S_("int"), S_("int")},
        {S_("𝓤"), S_("~int"), S_("𝓤")},
        {S_("int"), S_("~int"), S_("~int")},
        {S_("int"), S_("string"), S_("int | string")},
        {S_("int"), S_("myInt"), S_("int | myInt")},
        {S_("~int"), S_("myInt"), S_("~int")},
        {S_("int | string"), S_("~string"), S_("int | ~string")},
        {S_("~int | string"), S_("~string | int"), S_("~int | ~string")},
        {S_("~int | string | ∅"), S_("~string | int"), S_("~int | ~string")},
        {S_("~int | myInt | ∅"), S_("~string | int"), S_("~int | ~string")},
        {S_("~int | string | 𝓤"), S_("~string | int"), S_("𝓤")},
        {S_("~int | string | myInt"), S_("~string | int"), S_("~int | ~string")},
    };
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    TestTerms tt;
    make_test_terms(a, &tt);
    for (Int i = 0; i < NELEM(tests); i++) {
        Slice xl = maketl(a, &tt, tests[i].xl);
        Slice yl = maketl(a, &tt, tests[i].yl);
        Str got =
            burrow__types_termlist_string(a, burrow__types_termlist_union(a, xl, yl));
        if (!str_eq(got, tests[i].want))
            testing_t_errorf_v(t, "(%s).union(%s) = %s; want %s", tests[i].xl,
                               tests[i].yl, got, tests[i].want);
    }
    arena_free(&ar);
}

static void TestTermlistIntersect(TestingT *t) {
    static const TermlistOpTest tests[] = {
        {S_("∅"), S_("∅"), S_("∅")},
        {S_("∅"), S_("𝓤"), S_("∅")},
        {S_("∅"), S_("int"), S_("∅")},
        {S_("∅"), S_("myInt"), S_("∅")},
        {S_("𝓤"), S_("~int"), S_("~int")},
        {S_("𝓤"), S_("myInt"), S_("myInt")},
        {S_("int"), S_("~int"), S_("int")},
        {S_("int"), S_("string"), S_("∅")},
        {S_("int"), S_("myInt"), S_("∅")},
        {S_("~int"), S_("myInt"), S_("myInt")},
        {S_("int | string"), S_("~string"), S_("string")},
        {S_("~int | string"), S_("~string | int"), S_("int | string")},
        {S_("~int | string | ∅"), S_("~string | int"), S_("int | string")},
        {S_("~int | myInt | ∅"), S_("~string | int"), S_("int")},
        {S_("~int | string | 𝓤"), S_("~string | int"), S_("int | ~string")},
        {S_("~int | string | myInt"), S_("~string | int"), S_("int | string")},
    };
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    TestTerms tt;
    make_test_terms(a, &tt);
    for (Int i = 0; i < NELEM(tests); i++) {
        Slice xl = maketl(a, &tt, tests[i].xl);
        Slice yl = maketl(a, &tt, tests[i].yl);
        Str got = burrow__types_termlist_string(
            a, burrow__types_termlist_intersect(a, xl, yl));
        if (!str_eq(got, tests[i].want))
            testing_t_errorf_v(t, "(%s).intersect(%s) = %s; want %s", tests[i].xl,
                               tests[i].yl, got, tests[i].want);
    }
    arena_free(&ar);
}

typedef struct {
    Str xl, yl;
    bool want;
} TermlistPredTest;

static void TestTermlistEqual(TestingT *t) {
    static const TermlistPredTest tests[] = {
        {S_("∅"), S_("∅"), true},
        {S_("∅"), S_("𝓤"), false},
        {S_("𝓤"), S_("𝓤"), true},
        {S_("𝓤 | int"), S_("𝓤"), true},
        {S_("𝓤 | int"), S_("string | 𝓤"), true},
        {S_("𝓤 | myInt"), S_("string | 𝓤"), true},
        {S_("int | ~string"), S_("string | int"), false},
        {S_("~int | string"), S_("string | myInt"), false},
        {S_("int | ~string | ∅"), S_("string | int | ~string"), true},
    };
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    TestTerms tt;
    make_test_terms(a, &tt);
    for (Int i = 0; i < NELEM(tests); i++) {
        Slice xl = maketl(a, &tt, tests[i].xl);
        Slice yl = maketl(a, &tt, tests[i].yl);
        bool got = burrow__types_termlist_equal(xl, yl);
        if (got != tests[i].want)
            testing_t_errorf_v(t, "(%s).equal(%s) = %t; want %t", tests[i].xl,
                               tests[i].yl, got, tests[i].want);
    }
    arena_free(&ar);
}

static void TestTermlistIncludes(TestingT *t) {
    static const TermlistPredTest tests[] = {
        {S_("∅"), S_("int"), false},
        {S_("𝓤"), S_("int"), true},
        {S_("~int"), S_("int"), true},
        {S_("int"), S_("string"), false},
        {S_("~int"), S_("string"), false},
        {S_("~int"), S_("myInt"), true},
        {S_("int | string"), S_("string"), true},
        {S_("~int | string"), S_("int"), true},
        {S_("~int | string"), S_("myInt"), true},
        {S_("~int | myInt | ∅"), S_("myInt"), true},
        {S_("myInt | ∅ | 𝓤"), S_("int"), true},
    };
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    TestTerms tt;
    make_test_terms(a, &tt);
    for (Int i = 0; i < NELEM(tests); i++) {
        Slice xl = maketl(a, &tt, tests[i].xl);
        TypesType yl = types_term_type(test_term(&tt, tests[i].yl));
        bool got = burrow__types_termlist_includes(xl, yl);
        if (got != tests[i].want)
            testing_t_errorf_v(t, "(%s).includes(%s) = %t; want %t", tests[i].xl,
                               tests[i].yl, got, tests[i].want);
    }
    arena_free(&ar);
}

static void TestTermlistSupersetOf(TestingT *t) {
    static const TermlistPredTest tests[] = {
        {S_("∅"), S_("∅"), true},
        {S_("∅"), S_("𝓤"), false},
        {S_("∅"), S_("int"), false},
        {S_("𝓤"), S_("∅"), true},
        {S_("𝓤"), S_("𝓤"), true},
        {S_("𝓤"), S_("int"), true},
        {S_("𝓤"), S_("~int"), true},
        {S_("𝓤"), S_("myInt"), true},
        {S_("~int"), S_("int"), true},
        {S_("~int"), S_("~int"), true},
        {S_("~int"), S_("myInt"), true},
        {S_("int"), S_("~int"), false},
        {S_("myInt"), S_("~int"), false},
        {S_("int"), S_("string"), false},
        {S_("~int"), S_("string"), false},
        {S_("int | string"), S_("string"), true},
        {S_("int | string"), S_("~string"), false},
        {S_("~int | string"), S_("int"), true},
        {S_("~int | string"), S_("myInt"), true},
        {S_("~int | string | ∅"), S_("string"), true},
        {S_("~string | ∅ | 𝓤"), S_("myInt"), true},
    };
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    TestTerms tt;
    make_test_terms(a, &tt);
    for (Int i = 0; i < NELEM(tests); i++) {
        Slice xl = maketl(a, &tt, tests[i].xl);
        TypesTerm *y = test_term(&tt, tests[i].yl);
        bool got = burrow__types_termlist_superset_of(xl, y);
        if (got != tests[i].want)
            testing_t_errorf_v(t, "(%s).supersetOf(%s) = %t; want %t", tests[i].xl,
                               tests[i].yl, got, tests[i].want);
    }
    arena_free(&ar);
}

static void TestTermlistSubsetOf(TestingT *t) {
    static const TermlistPredTest tests[] = {
        {S_("∅"), S_("∅"), true},
        {S_("∅"), S_("𝓤"), true},
        {S_("𝓤"), S_("∅"), false},
        {S_("𝓤"), S_("𝓤"), true},
        {S_("int"), S_("int | string"), true},
        {S_("~int"), S_("int | string"), false},
        {S_("~int"), S_("myInt | string"), false},
        {S_("myInt"), S_("~int | string"), true},
        {S_("~int"), S_("string | string | int | ~int"), true},
        {S_("myInt"), S_("string | string | ~int"), true},
        {S_("int | string"), S_("string"), false},
        {S_("int | string"), S_("string | int"), true},
        {S_("int | ~string"), S_("string | int"), false},
        {S_("myInt | ~string"), S_("string | int | 𝓤"), true},
        {S_("int | ~string"), S_("string | int | ∅ | string"), false},
        {S_("int | myInt"), S_("string | ~int | ∅ | string"), true},
    };
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    TestTerms tt;
    make_test_terms(a, &tt);
    for (Int i = 0; i < NELEM(tests); i++) {
        Slice xl = maketl(a, &tt, tests[i].xl);
        Slice yl = maketl(a, &tt, tests[i].yl);
        bool got = burrow__types_termlist_subset_of(xl, yl);
        if (got != tests[i].want)
            testing_t_errorf_v(t, "(%s).subsetOf(%s) = %t; want %t", tests[i].xl,
                               tests[i].yl, got, tests[i].want);
    }
    arena_free(&ar);
}

static void TestInvalidTypeSet(TestingT *t) {
    if (!burrow__types_type_set_is_empty(burrow__types_invalid_type_set()))
        testing_t_errorf_v(t, "invalidTypeSet is not empty");
}

/* Not in Go: TestTypeSetString type checks "package p; type T interface"
 * and a body, and there is no checker yet, so these are the cases it has
 * that need none of the declarations after the body, with T built the way
 * the checker builds it. */

typedef struct {
    Alloc *a;
    TypesPackage *pkg;
    TypesNamed *named; /* T */
    Slice methods;
    Slice embeddeds;
} IfaceBuilder;

static void ib_init(IfaceBuilder *b, Alloc *a) {
    b->a = a;
    b->pkg = types_new_package(a, S("p"), S("p"));
    TypesTypeName *obj = types_new_type_name(a, TOKEN_NO_POS, b->pkg, S("T"), NULL);
    b->named = types_new_named(a, obj, NULL, slice_nil(TYPE_UNSAFE_POINTER));
    b->methods = slice_nil(TYPE_UNSAFE_POINTER);
    b->embeddeds = slice_nil(TYPE_UNSAFE_POINTER);
}

/* m(), or m() int with a result */
static void ib_method(IfaceBuilder *b, Str name, bool result) {
    Alloc *a = b->a;
    TypesVar *recv =
        types_new_param(a, TOKEN_NO_POS, b->pkg, S(""), types_type(b->named));
    TypesTuple *results = NULL;
    if (result) {
        Slice r = burrow__types_ptrs_make(a, 1);
        GT_AT(r, 0, TypesVar *) =
            types_new_param(a, TOKEN_NO_POS, b->pkg, S(""), GT_TYP(TYPES_INT));
        results = types_new_tuple(a, r);
    }
    TypesSignature *sig = types_new_signature(a, recv, NULL, results, false);
    burrow__types_ptrs_append(a, &b->methods,
                              types_new_func(a, TOKEN_NO_POS, b->pkg, name, sig));
}

static void ib_embed(IfaceBuilder *b, TypesType typ) {
    burrow__types_ptrs_append(b->a, &b->embeddeds, (void *)typ);
}

/* an embedded union of the basic types kinds */
static void ib_union(IfaceBuilder *b, const TypesBasicKind *kinds, Int n) {
    Slice terms = burrow__types_ptrs_make(b->a, n);
    for (Int i = 0; i < n; i++)
        GT_AT(terms, i, TypesTerm *) = types_new_term(b->a, false, GT_TYP(kinds[i]));
    ib_embed(b, types_type(types_new_union(b->a, terms)));
}

static TypesType universe_type(Str name) {
    return types_object_type(types_scope_lookup(types_universe(), name));
}

static Str ib_type_set(IfaceBuilder *b) {
    TypesInterface *iface = types_new_interface_type(b->a, b->methods, b->embeddeds);
    types_named_set_underlying(b->named, types_type(iface));
    return burrow__types_type_set_string(b->a, burrow__types_iface_type_set(iface));
}

static void TestTypeSetString(TestingT *t) {
    static const TypesBasicKind int_string[] = {TYPES_INT, TYPES_STRING};
    static const TypesBasicKind int_float32_string[] = {TYPES_INT, TYPES_FLOAT32,
                                                        TYPES_STRING};
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    TypesType comparable = universe_type(S("comparable"));
    TypesType error = universe_type(S("error"));
    TypesType tint = GT_TYP(TYPES_INT);
    TypesType tstring = GT_TYP(TYPES_STRING);
    Slice tilde_int = burrow__types_ptrs_make(a, 1);
    GT_AT(tilde_int, 0, TypesTerm *) = types_new_term(a, true, tint);
    IfaceBuilder b;
    Str got;

#define WANT(body, want)                                                               \
    do {                                                                               \
        got = ib_type_set(&b);                                                         \
        if (!str_eq(got, S(want)))                                                     \
            testing_t_errorf_v(t, "%s: got %s; want %s", body, got, want);             \
    } while (0)

    ib_init(&b, a);
    WANT("{}", "𝓤");

    ib_init(&b, a);
    ib_embed(&b, tint);
    WANT("{int}", "{int}");

    ib_init(&b, a);
    ib_embed(&b, types_type(types_new_union(a, tilde_int)));
    WANT("{~int}", "{~int}");

    ib_init(&b, a);
    ib_union(&b, int_string, 2);
    WANT("{int|string}", "{int | string}");

    ib_init(&b, a);
    ib_embed(&b, tint);
    ib_embed(&b, tstring);
    WANT("{int; string}", "∅");

    ib_init(&b, a);
    ib_embed(&b, comparable);
    WANT("{comparable}", "{comparable}");

    ib_init(&b, a);
    ib_embed(&b, comparable);
    ib_embed(&b, tint);
    WANT("{comparable; int}", "{int}");

    ib_init(&b, a);
    ib_embed(&b, types_type(types_new_union(a, tilde_int)));
    ib_embed(&b, comparable);
    WANT("{~int; comparable}", "{~int}");

    ib_init(&b, a);
    ib_union(&b, int_string, 2);
    ib_embed(&b, comparable);
    WANT("{int|string; comparable}", "{int | string}");

    ib_init(&b, a);
    ib_embed(&b, comparable);
    ib_embed(&b, tint);
    ib_embed(&b, tstring);
    WANT("{comparable; int; string}", "∅");

    ib_init(&b, a);
    ib_method(&b, S("m"), false);
    WANT("{m()}", "{func (p.T).m()}");

    ib_init(&b, a);
    ib_method(&b, S("m1"), false);
    ib_method(&b, S("m2"), true);
    WANT("{m1(); m2() int }", "{func (p.T).m1(); func (p.T).m2() int}");

    ib_init(&b, a);
    ib_embed(&b, error);
    WANT("{error}", "{func (error).Error() string}");

    ib_init(&b, a);
    ib_method(&b, S("m"), false);
    ib_embed(&b, comparable);
    WANT("{m(); comparable}", "{comparable; func (p.T).m()}");

    ib_init(&b, a);
    ib_method(&b, S("m1"), false);
    ib_embed(&b, comparable);
    ib_method(&b, S("m2"), true);
    WANT("{m1(); comparable; m2() int }",
         "{comparable; func (p.T).m1(); func (p.T).m2() int}");

    ib_init(&b, a);
    ib_embed(&b, comparable);
    ib_embed(&b, error);
    WANT("{comparable; error}", "{comparable; func (error).Error() string}");

    ib_init(&b, a);
    ib_method(&b, S("m"), false);
    ib_embed(&b, comparable);
    ib_union(&b, int_float32_string, 3);
    WANT("{m(); comparable; int|float32|string}",
         "{func (p.T).m(); int | float32 | string}");

    ib_init(&b, a);
    ib_method(&b, S("m1"), false);
    ib_embed(&b, tint);
    ib_method(&b, S("m2"), false);
    ib_embed(&b, comparable);
    WANT("{m1(); int; m2(); comparable }", "{func (p.T).m1(); func (p.T).m2(); int}");

#undef WANT
    arena_free(&ar);
}

static void TestIsAlias(TestingT *t) {
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);

#define CHECK_ALIAS(obj, want)                                                         \
    do {                                                                               \
        TypesTypeName *o_ = (obj);                                                     \
        bool w_ = (want);                                                              \
        if (types_type_name_is_alias(o_) != w_) {                                      \
            Str s_ = types_object_string(a, types_object(o_), TYPES_NO_QUALIFIER);     \
            testing_t_errorf_v(t, "%s: got IsAlias = %t; want %t", s_, !w_, w_);       \
        }                                                                              \
    } while (0)

    /* predeclared types */
    CHECK_ALIAS(types_as_type_name(types_scope_lookup(
                    types_package_scope(types_unsafe()), S("Pointer"))),
                false);
    Slice names = types_scope_names(a, types_universe());
    for (Int i = 0; i < names.len; i++) {
        Str name = arg(names, i);
        TypesTypeName *obj =
            types_as_type_name(types_scope_lookup(types_universe(), name));
        if (obj != NULL)
            CHECK_ALIAS(obj, str_eq(name, S("any")) || str_eq(name, S("byte")) ||
                                 str_eq(name, S("rune")));
    }

    /* various other types */
    TypesPackage *pkg = types_new_package(a, S("p"), S("p"));
    TypesTypeName *t1 = types_new_type_name(a, TOKEN_NO_POS, pkg, S("t1"), NULL);
    TypesNamed *n1 =
        types_new_named(a, t1,
                        types_type(types_new_struct(a, slice_nil(TYPE_UNSAFE_POINTER),
                                                    slice_nil(TYPE_STRING))),
                        slice_nil(TYPE_UNSAFE_POINTER));
    TypesTypeName *t5 = types_new_type_name(a, TOKEN_NO_POS, pkg, S("t5"), NULL);
    types_new_type_param(a, t5, NULL);
    struct {
        TypesTypeName *name;
        bool alias;
    } tests[] = {
        /* no type yet */
        {types_new_type_name(a, TOKEN_NO_POS, NULL, S("t0"), NULL), false},
        /* no type yet */
        {types_new_type_name(a, TOKEN_NO_POS, pkg, S("t0"), NULL), false},
        /* type name refers to named type and vice versa */
        {t1, false},
        /* type name refers to unnamed type */
        {types_new_type_name(
             a, TOKEN_NO_POS, NULL, S("t2"),
             types_type(types_new_interface_type(a, slice_nil(TYPE_UNSAFE_POINTER),
                                                 slice_nil(TYPE_UNSAFE_POINTER)))),
         true},
        /* type name refers to named type with different type name */
        {types_new_type_name(a, TOKEN_NO_POS, pkg, S("t3"), types_type(n1)), true},
        /* type name refers to basic type with different name */
        {types_new_type_name(a, TOKEN_NO_POS, NULL, S("t4"), GT_TYP(TYPES_INT32)),
         true},
        /* type name refers to basic type with same name */
        {types_new_type_name(a, TOKEN_NO_POS, NULL, S("int32"), GT_TYP(TYPES_INT32)),
         false},
        /* type name is declared in user-defined package (outside Universe) */
        {types_new_type_name(a, TOKEN_NO_POS, pkg, S("int32"), GT_TYP(TYPES_INT32)),
         true},
        /* type name refers to basic type rune which is an alias already */
        {types_new_type_name(a, TOKEN_NO_POS, NULL, S("rune"), GT_TYP(TYPES_RUNE)),
         true},
        /* type name refers to type parameter and vice versa */
        {t5, false},
    };
    for (Int i = 0; i < NELEM(tests); i++)
        CHECK_ALIAS(tests[i].name, tests[i].alias);

#undef CHECK_ALIAS
    arena_free(&ar);
}

static void TestIdenticalUnions(TestingT *t) {
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    TypesType my = my_int(a);
    struct {
        Str name;
        TypesTerm *term;
    } tmap[] = {
        {S("int"), types_new_term(a, false, GT_TYP(TYPES_INT))},
        {S("~int"), types_new_term(a, true, GT_TYP(TYPES_INT))},
        {S("string"), types_new_term(a, false, GT_TYP(TYPES_STRING))},
        {S("~string"), types_new_term(a, true, GT_TYP(TYPES_STRING))},
        {S("myInt"), types_new_term(a, false, my)},
    };
    static const struct {
        Str x, y;
        bool want;
    } tests[] = {
        /* These tests are just sanity checks. The tests for type sets and
         * interfaces provide much more test coverage. */
        {S_("int|~int"), S_("~int"), true},
        {S_("myInt|~int"), S_("~int"), true},
        {S_("int|string"), S_("string|int"), true},
        {S_("int|int|string"), S_("string|int"), true},
        {S_("myInt|string"), S_("int|string"), false},
    };
    for (Int i = 0; i < NELEM(tests); i++) {
        TypesType u[2];
        for (Int k = 0; k < 2; k++) {
            Slice parts = strings_split(a, k == 0 ? tests[i].x : tests[i].y, S("|"));
            Slice terms = burrow__types_ptrs_make(a, parts.len);
            for (Int j = 0; j < parts.len; j++) {
                TypesTerm *term = NULL;
                for (Int m = 0; m < NELEM(tmap); m++)
                    if (str_eq(tmap[m].name, arg(parts, j)))
                        term = tmap[m].term;
                if (term == NULL)
                    testing_t_fatalf_v(t, "missing term %q", arg(parts, j));
                GT_AT(terms, j, TypesTerm *) = term;
            }
            u[k] = types_type(types_new_union(a, terms));
        }
        bool got = types_identical(u[0], u[1]);
        if (got != tests[i].want)
            testing_t_errorf_v(t, "Identical(%s, %s) = %t", tests[i].x, tests[i].y,
                               got);
    }
    arena_free(&ar);
}

#define TESTS(X)                                                                       \
    X(TestTermString)                                                                  \
    X(TestTermEqual)                                                                   \
    X(TestTermUnion)                                                                   \
    X(TestTermIntersection)                                                            \
    X(TestTermIncludes)                                                                \
    X(TestTermSubsetOf)                                                                \
    X(TestTermDisjoint)                                                                \
    X(TestTermlistAll)                                                                 \
    X(TestTermlistString)                                                              \
    X(TestTermlistIsEmpty)                                                             \
    X(TestTermlistIsAll)                                                               \
    X(TestTermlistNorm)                                                                \
    X(TestTermlistUnion)                                                               \
    X(TestTermlistIntersect)                                                           \
    X(TestTermlistEqual)                                                               \
    X(TestTermlistIncludes)                                                            \
    X(TestTermlistSupersetOf)                                                          \
    X(TestTermlistSubsetOf)                                                            \
    X(TestInvalidTypeSet)                                                              \
    X(TestTypeSetString)                                                               \
    X(TestIsAlias)                                                                     \
    X(TestIdenticalUnions)

TESTING_MAIN(TESTS)
