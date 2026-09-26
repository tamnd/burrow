/* Derived from Go's src/maps/maps_test.go and src/maps/iter_test.go.
 * Go source: go1.27.1.
 *
 * Copyright 2021 The Go Authors. All rights reserved.
 * Copyright 2026 The burrow Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style licence that can be found
 * in the LICENSE file. */

#include "check.h"

#include "burrow/burrow.h"
#include "burrow/maps.h"
#include "burrow/math.h"
#include "burrow/mem/arena.h"
#include "burrow/mem/heap.h"

#include <math.h>

static Arena ar;
static Alloc *A;

static void arena_start(void) {
    arena_init(&ar, NULL, 0);
    A = arena_allocator(&ar);
}

static void arena_done(void) {
    arena_free(&ar);
    A = NULL;
}

/* A map[int]int from n pairs. */
static Map *ints(const Int *kv, Int n) {
    Map *m = map_make(A, TYPE_INT, TYPE_INT, 0);
    for (Int i = 0; i < n; i++)
        BURROW_MAP_SET(Int, Int, m, kv[2 * i], kv[2 * i + 1]);
    return m;
}

#define INTS(...)                                                                      \
    ints((const Int[]){__VA_ARGS__},                                                   \
         (Int)(sizeof((const Int[]){__VA_ARGS__}) / sizeof(Int) / 2))

/* var m1 = map[int]int{1: 2, 2: 4, 4: 8, 8: 16} */
static Map *m1(void) {
    return INTS(1, 2, 2, 4, 4, 8, 8, 16);
}

/* var m2 = map[int]string{1: "2", 2: "4", 4: "8", 8: "16"} */
static Map *m2(void) {
    Map *m = map_make(A, TYPE_INT, TYPE_STRING, 0);
    BURROW_MAP_SET(Int, Str, m, 1, BURROW_S("2"));
    BURROW_MAP_SET(Int, Str, m, 2, BURROW_S("4"));
    BURROW_MAP_SET(Int, Str, m, 4, BURROW_S("8"));
    BURROW_MAP_SET(Int, Str, m, 8, BURROW_S("16"));
    return m;
}

/* mf := map[int]float64{1: 0, 2: math.NaN()} */
static Map *mf(void) {
    Map *m = map_make(A, TYPE_INT, TYPE_FLOAT64, 0);
    BURROW_MAP_SET(Int, double, m, 1, 0.0);
    BURROW_MAP_SET(Int, double, m, 2, (double)NAN);
    return m;
}

static bool panics(Func f) {
    volatile bool got = false;
    BURROW_TRY {
        BURROW_CALLF0(f);
    }
    BURROW_CATCH(r) {
        (void)r;
        got = true;
    }
    BURROW_TRY_END;
    return got;
}

static void TestEqual(TestingT *t) {
    arena_start();
    Map *m = m1();
    CHECK(maps_equal(m, m));
    CHECK(!maps_equal(m, NULL));
    CHECK(!maps_equal(NULL, m));
    CHECK(maps_equal(NULL, NULL));
    CHECK(!maps_equal(m, INTS(1, 2)));

    /* Comparing NaN for equality is expected to fail. */
    Map *f = mf();
    CHECK(!maps_equal(f, f));
    arena_done();
}

/* equal[int], equal[float64], equalNaN[float64] and equalIntStr. */
static bool eq_int(void *env, const void *a, const void *b) {
    (void)env;
    return *(const Int *)a == *(const Int *)b;
}

static bool eq_float(void *env, const void *a, const void *b) {
    (void)env;
    return *(const double *)a == *(const double *)b;
}

static bool eq_nan(void *env, const void *a, const void *b) {
    double x = *(const double *)a, y = *(const double *)b;
    (void)env;
    return x == y || (math_is_nan(x) && math_is_nan(y));
}

static bool eq_int_str(void *env, const void *a, const void *b) {
    (void)env;
    return str_eq(strconv_itoa(A, *(const Int *)a), *(const Str *)b);
}

static void TestEqualFunc(TestingT *t) {
    arena_start();
    MapsEqualFunc eqi = BURROW_FN(MapsEqualFunc, eq_int, NULL);
    Map *m = m1();
    CHECK(maps_equal_func(m, m, eqi));
    CHECK(!maps_equal_func(m, NULL, eqi));
    CHECK(!maps_equal_func(NULL, m, eqi));
    CHECK(maps_equal_func(NULL, NULL, eqi));
    CHECK(!maps_equal_func(m, INTS(1, 2), eqi));

    /* Comparing NaN for equality is expected to fail, but it should succeed
     * using equalNaN. */
    Map *f = mf();
    CHECK(!maps_equal_func(f, f, BURROW_FN(MapsEqualFunc, eq_float, NULL)));
    CHECK(maps_equal_func(f, f, BURROW_FN(MapsEqualFunc, eq_nan, NULL)));

    CHECK(maps_equal_func(m, m2(), BURROW_FN(MapsEqualFunc, eq_int_str, NULL)));
    arena_done();
}

static void TestClone(TestingT *t) {
    arena_start();
    Map *m = m1();
    Map *mc = maps_clone(A, m);
    CHECK(maps_equal(mc, m));
    BURROW_MAP_SET(Int, Int, mc, 16, 32);
    CHECK(!maps_equal(mc, m));
    CHECK_INT_EQ(map_len(m), 4);
    arena_done();
}

static void TestCloneNil(TestingT *t) {
    arena_start();
    CHECK(maps_clone(A, NULL) == NULL);

    /* An empty map is not nil, and neither is its clone. */
    Map *e = map_make(A, TYPE_STRING, TYPE_INT, 0);
    Map *c = maps_clone(A, e);
    CHECK(c != NULL);
    CHECK_INT_EQ(map_len(c), 0);
    BURROW_MAP_SET(Str, Int, c, BURROW_S("x"), 1);
    CHECK_INT_EQ(map_len(c), 1);
    CHECK_INT_EQ(map_len(e), 0);
    arena_done();
}

static void TestCopy(TestingT *t) {
    arena_start();
    Map *mc = maps_clone(A, m1());
    CHECK(maps_copy(mc, mc));
    CHECK(maps_equal(mc, m1()));
    CHECK(maps_copy(mc, INTS(16, 32)));
    CHECK(maps_equal(mc, INTS(1, 2, 2, 4, 4, 8, 8, 16, 16, 32)));

    CHECK(maps_copy(map_make(A, TYPE_INT, TYPE_BOOL, 0),
                    map_make(A, TYPE_INT, TYPE_BOOL, 0)));
    arena_done();
}

static void copy_to_nil(void *env) {
    maps_copy(NULL, env);
}

/* Copy into a nil map is an assignment to an entry in a nil map, unless there
 * is nothing to copy. */
static void TestCopyNil(TestingT *t) {
    arena_start();
    CHECK(maps_copy(NULL, NULL));
    CHECK(maps_copy(NULL, map_make(A, TYPE_INT, TYPE_INT, 0)));
    CHECK(panics(BURROW_FN(Func, copy_to_nil, m1())));
    arena_done();
}

static bool del_none(void *env, const void *k, const void *v) {
    (void)env, (void)k, (void)v;
    return false;
}

static bool del_big(void *env, const void *k, const void *v) {
    (void)env, (void)v;
    return *(const Int *)k > 3;
}

static void TestDeleteFunc(TestingT *t) {
    arena_start();
    Map *mc = maps_clone(A, m1());
    maps_delete_func(mc, BURROW_FN(MapsPredFunc, del_none, NULL));
    CHECK(maps_equal(mc, m1()));
    maps_delete_func(mc, BURROW_FN(MapsPredFunc, del_big, NULL));
    CHECK(maps_equal(mc, INTS(1, 2, 2, 4)));
    maps_delete_func(NULL, BURROW_FN(MapsPredFunc, del_big, NULL));
    arena_done();
}

static void TestCloneWithDelete(TestingT *t) {
    arena_start();
    Map *m = map_make(A, TYPE_INT, TYPE_INT, 0);
    for (Int i = 0; i < 32; i++)
        BURROW_MAP_SET(Int, Int, m, i, i);
    for (Int i = 8; i < 32; i++)
        BURROW_MAP_DEL(Int, m, i);
    Map *c = maps_clone(A, m);
    CHECK_INT_EQ(map_len(c), 8);
    for (Int i = 0; i < 8; i++) {
        Int *got = BURROW_MAP_GET(Int, Int, c, i);
        CHECK(got != NULL && *got == i);
    }
    for (Int i = 8; i < 32; i++)
        CHECK(!BURROW_MAP_HAS(Int, c, i));
    arena_done();
}

static void TestCloneWithMapAssign(TestingT *t) {
    arena_start();
    enum { N = 25 };
    Map *m = map_make(A, TYPE_INT, TYPE_INT, 0);
    for (Int i = 0; i < N; i++)
        BURROW_MAP_SET(Int, Int, m, i, i);
    Map *c = maps_clone(A, m);
    CHECK_INT_EQ(map_len(c), N);
    for (Int i = 0; i < N; i++) {
        Int *got = BURROW_MAP_GET(Int, Int, c, i);
        CHECK(got != NULL && *got == i);
    }
    arena_done();
}

/* type K [17]float64, and V the same. */
typedef struct Big {
    double f[17];
} Big;

static const Type big_type = {
    {NULL, 0},    {NULL, 0}, KIND_ARRAY, sizeof(Big), _Alignof(Big), 0, 0, NULL, NULL,
    TYPE_FLOAT64, NULL,      17,         0,           NULL,
};

/* See Go issue 64474. The key's first element goes from 0 to -0, which is the
 * same key, so the original's entry is replaced and the clone's must not be. */
static void TestCloneLarge(TestingT *t) {
    arena_start();
    double zero = 0;
    double neg_zero = -zero;
    for (int tst = 0; tst < 3; tst++) {
        Map *m = map_make(A, &big_type, &big_type, 0);
        Big k1 = {{0}}, v1 = {{0}};
        map_set(m, &k1, &v1);

        /* 0 is a one entry map, 1 grows it past one group and 2 past four. */
        int more = tst == 0 ? 0 : tst == 1 ? 7 + 1 : 7 + 5 + 13 + 1;
        for (int i = 0; i < more; i++) {
            Big k = {{(double)i + 1}};
            map_set(m, &k, NULL);
        }

        Map *c = maps_clone(A, m);

        Big k2 = k1, v2 = v1;
        k2.f[0] = neg_zero;
        v2.f[0] = 1.0;
        map_set(m, &k2, &v2);

        const void *k;
        void *v;
        for (MapIter it = map_iter(c); map_next(&it, &k, &v);) {
            if (math_signbit(((const Big *)k)->f[0]))
                testing_t_errorf_v(t, "tst%d: sign bit of key changed", tst);
            if (!type_equal(&big_type, v, &v1))
                testing_t_errorf_v(t, "tst%d: value changed", tst);
        }
        CHECK_INT_EQ(map_len(c), 1 + more);
    }
    arena_done();
}

/* The copy has to own its table: freeing the original leaves the clone
 * readable, and both free cleanly, which the sanitizer builds check. A Str
 * key has ops, which takes the slot by slot path through map_clone. */
static void TestCloneHeap(TestingT *t) {
    Alloc *h = heap_allocator();
    Map *m = map_make(h, TYPE_STRING, TYPE_INT, 0);
    static const char *const words[] = {"a", "b", "c", "d", "e",
                                        "f", "g", "h", "i", "j"};
    for (Int i = 0; i < 10; i++)
        BURROW_MAP_SET(Str, Int, m, str_from_cstr(words[i]), i);
    BURROW_MAP_DEL(Str, m, BURROW_S("c"));
    Map *c = maps_clone(h, m);
    map_free(m);
    CHECK_INT_EQ(map_len(c), 9);
    for (Int i = 0; i < 10; i++) {
        Int *got = BURROW_MAP_GET(Str, Int, c, str_from_cstr(words[i]));
        if (i == 2)
            CHECK(got == NULL);
        else
            CHECK(got != NULL && *got == i);
    }
    BURROW_MAP_SET(Str, Int, c, BURROW_S("z"), 26);
    CHECK_INT_EQ(map_len(c), 10);
    map_free(c);
}

/* What the range loops over a sequence collect. */
typedef struct Seen {
    TestingT *t;
    Map *m;
    Int cnt;
    Slice got;
} Seen;

static bool all_yield(void *env, const void *k, const void *v) {
    Seen *s = env;
    TestingT *t = s->t;
    Int *v1 = BURROW_MAP_GET(Int, Int, s->m, *(const Int *)k);
    if (v1 == NULL || *v1 != *(const Int *)v)
        testing_t_errorf_v(t, "at iteration %d got %d, %d", (int)s->cnt,
                           (int)*(const Int *)k, (int)*(const Int *)v);
    s->cnt++;
    return true;
}

static void TestAll(TestingT *t) {
    arena_start();
    for (Int size = 0; size < 10; size++) {
        Map *m = map_make(A, TYPE_INT, TYPE_INT, 0);
        for (Int i = 0; i < size; i++)
            BURROW_MAP_SET(Int, Int, m, i, i);
        Seen s = {t, m, 0, {0}};
        IterSeq2 seq = maps_all(m);
        seq.f(seq.env, BURROW_FN(IterYield2, all_yield, &s));
        CHECK_INT_EQ(s.cnt, size);
    }
    arena_done();
}

static bool collect_yield(void *env, const void *v) {
    Seen *s = env;
    s->got = slice_append(A, s->got, v, 1);
    return true;
}

static void check_sorted_range(TestingT *t, IterSeq seq, Int size) {
    Seen s = {t, NULL, 0, slice_nil(TYPE_INT)};
    seq.f(seq.env, BURROW_FN(IterYield, collect_yield, &s));
    slices_sort(s.got);
    CHECK_INT_EQ(s.got.len, size);
    for (Int i = 0; i < s.got.len; i++)
        CHECK_INT_EQ(BURROW_AT(Int, s.got, i), i);
}

static void TestKeys(TestingT *t) {
    arena_start();
    for (Int size = 0; size < 10; size++) {
        Map *m = map_make(A, TYPE_INT, TYPE_INT, 0);
        for (Int i = 0; i < size; i++)
            BURROW_MAP_SET(Int, Int, m, i, i * 10);
        check_sorted_range(t, maps_keys(m), size);
    }
    arena_done();
}

static void TestValues(TestingT *t) {
    arena_start();
    for (Int size = 0; size < 10; size++) {
        Map *m = map_make(A, TYPE_INT, TYPE_INT, 0);
        for (Int i = 0; i < size; i++)
            BURROW_MAP_SET(Int, Int, m, i * 10, i);
        check_sorted_range(t, maps_values(m), size);
    }
    arena_done();
}

/* Breaking out of the range stops the walk. */
static bool stop_yield(void *env, const void *v) {
    Int *n = env;
    (void)v;
    return ++*n < 3;
}

static bool stop_yield2(void *env, const void *k, const void *v) {
    (void)k;
    return stop_yield(env, v);
}

static void TestBreak(TestingT *t) {
    arena_start();
    Map *m = INTS(1, 1, 2, 2, 3, 3, 4, 4, 5, 5);
    Int n = 0;
    IterSeq seq = maps_keys(m);
    seq.f(seq.env, BURROW_FN(IterYield, stop_yield, &n));
    CHECK_INT_EQ(n, 3);
    n = 0;
    seq = maps_values(m);
    seq.f(seq.env, BURROW_FN(IterYield, stop_yield, &n));
    CHECK_INT_EQ(n, 3);
    n = 0;
    IterSeq2 seq2 = maps_all(m);
    seq2.f(seq2.env, BURROW_FN(IterYield2, stop_yield2, &n));
    CHECK_INT_EQ(n, 3);
    arena_done();
}

/* for i := 0; i < 10; i += 2 { yield(i, i+1) } */
static void evens_run(void *env, IterYield2 yield) {
    (void)env;
    for (Int i = 0; i < 10; i += 2) {
        Int v = i + 1;
        if (!yield.f(yield.env, &i, &v))
            return;
    }
}

static void TestInsert(TestingT *t) {
    arena_start();
    Map *got = INTS(1, 1, 2, 1);
    CHECK(maps_insert(got, BURROW_FN(IterSeq2, evens_run, NULL)));
    Map *want = INTS(1, 1, 2, 1);
    CHECK(maps_copy(want, INTS(0, 1, 2, 3, 4, 5, 6, 7, 8, 9)));
    CHECK(maps_equal(got, want));
    arena_done();
}

static void TestCollect(TestingT *t) {
    arena_start();
    Map *m = INTS(0, 1, 2, 3, 4, 5, 6, 7, 8, 9);
    Map *got = maps_collect(A, TYPE_INT, TYPE_INT, maps_all(m));
    CHECK(maps_equal(got, m));
    arena_done();
}

#define TESTS(X)                                                                       \
    X(TestEqual)                                                                       \
    X(TestEqualFunc)                                                                   \
    X(TestClone)                                                                       \
    X(TestCloneNil)                                                                    \
    X(TestCopy)                                                                        \
    X(TestCopyNil)                                                                     \
    X(TestDeleteFunc)                                                                  \
    X(TestCloneWithDelete)                                                             \
    X(TestCloneWithMapAssign)                                                          \
    X(TestCloneLarge)                                                                  \
    X(TestCloneHeap)                                                                   \
    X(TestAll)                                                                         \
    X(TestKeys)                                                                        \
    X(TestValues)                                                                      \
    X(TestBreak)                                                                       \
    X(TestInsert)                                                                      \
    X(TestCollect)

TESTING_MAIN(TESTS)
