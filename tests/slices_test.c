/* Derived from Go's src/slices/slices_test.go, sort_test.go and iter_test.go.
 * Go source: go1.27.1.
 *
 * Copyright 2021 The Go Authors. All rights reserved.
 * Copyright 2026 The burrow Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style licence that can be found
 * in the LICENSE file. */

#include "check.h"

#include "burrow/burrow.h"
#include "burrow/mem/arena.h"
#include "burrow/slices.h"
#include "burrow/strings.h"

#include <math.h>
#include <string.h>

/* Every test takes its memory from one arena and frees it all at the end. */
static Arena arena;
static Alloc *A;

static void arena_start(void) {
    arena_init(&arena, NULL, 0);
    A = arena_allocator(&arena);
}

static void arena_done(void) {
    arena_free(&arena);
}

/* A splitmix64 generator standing in for math/rand until that is ported. */
static uint64_t rng_state = 0x9e3779b97f4a7c15u;

static Int rng_intn(Int n) {
    uint64_t z = (rng_state += 0x9e3779b97f4a7c15u);
    z = (z ^ (z >> 30)) * 0xbf58476d1ce4e5b9u;
    z = (z ^ (z >> 27)) * 0x94d049bb133111ebu;
    return (Int)((z ^ (z >> 31)) % (uint64_t)n);
}

#define NELEM(a) ((Int)(sizeof(a) / sizeof((a)[0])))

/* A copy of n Ints in the arena, so a test can write to it. n == 0 gives an
 * empty slice that is not nil, which is Go's []int{}. */
static Slice ints_of(const Int *p, Int n) {
    Slice s = slice_make(A, TYPE_INT, n, n);
    if (n > 0)
        memcpy(s.p, p, (size_t)n * sizeof(Int));
    return s;
}

#define IS(...) ints_of((const Int[]){__VA_ARGS__}, NELEM(((const Int[]){__VA_ARGS__})))
#define INONE() ints_of(NULL, 0)
#define INIL() slice_nil(TYPE_INT)

static Slice floats_of(const double *p, Int n) {
    Slice s = slice_make(A, TYPE_FLOAT64, n, n);
    if (n > 0)
        memcpy(s.p, p, (size_t)n * sizeof(double));
    return s;
}

#define FS(...)                                                                        \
    floats_of((const double[]){__VA_ARGS__}, NELEM(((const double[]){__VA_ARGS__})))

static Slice strs_of(const char *const *p, Int n) {
    Slice s = slice_make(A, TYPE_STRING, n, n);
    Str *d = s.p;
    for (Int i = 0; i < n; i++)
        d[i] = str_from_cstr(p[i]);
    return s;
}

#define SS(...)                                                                        \
    strs_of((const char *const[]){__VA_ARGS__},                                        \
            NELEM(((const char *const[]){__VA_ARGS__})))

static Int *ip(Slice s) {
    return s.p;
}

/* Whether f panics. */
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

/* Go's equal[int], equal[float64] and equalNaN[float64]. */
static bool eq_int(void *env, const void *a, const void *b) {
    (void)env;
    return *(const Int *)a == *(const Int *)b;
}

static bool eq_float(void *env, const void *a, const void *b) {
    (void)env;
    return *(const double *)a == *(const double *)b;
}

static bool eq_float_nan(void *env, const void *a, const void *b) {
    (void)env;
    double x = *(const double *)a, y = *(const double *)b;
    return x == y || (x != x && y != y);
}

static bool eq_str(void *env, const void *a, const void *b) {
    (void)env;
    return str_eq(*(const Str *)a, *(const Str *)b);
}

static bool eq_fold(void *env, const void *a, const void *b) {
    (void)env;
    return strings_equal_fold(*(const Str *)a, *(const Str *)b);
}

static bool off_by_one(void *env, const void *a, const void *b) {
    (void)env;
    Int v1 = *(const Int *)a, v2 = *(const Int *)b;
    return v1 == v2 + 1 || v1 == v2 - 1;
}

/* string(rune(v1)-1+'a') == v2. */
static bool eq_int_string(void *env, const void *a, const void *b) {
    (void)env;
    Byte c = (Byte)(*(const Int *)a - 1 + 'a');
    return str_eq(str_from_bytes(&c, 1), *(const Str *)b);
}

static int cmp_int(void *env, const void *a, const void *b) {
    (void)env;
    return cmp_compare(*(const Int *)a, *(const Int *)b);
}

static int sub_int(void *env, const void *a, const void *b) {
    (void)env;
    return (int)(*(const Int *)a - *(const Int *)b);
}

static int cmp_float(void *env, const void *a, const void *b) {
    (void)env;
    return cmp_compare(*(const double *)a, *(const double *)b);
}

static int cmp_str(void *env, const void *a, const void *b) {
    (void)env;
    return cmp_compare(*(const Str *)a, *(const Str *)b);
}

static int cmp_lower(void *env, const void *a, const void *b) {
    (void)env;
    Str x = strings_to_lower(A, *(const Str *)a);
    Str y = strings_to_lower(A, *(const Str *)b);
    return str_cmp(x, y);
}

static int cmp_int_string(void *env, const void *a, const void *b) {
    (void)env;
    Byte c = (Byte)(*(const Int *)a - 1 + 'a');
    return str_cmp(str_from_bytes(&c, 1), *(const Str *)b);
}

/* Go's equalToCmp: 0 when eq says equal, 1 otherwise. */
static int eq_to_cmp(void *env, const void *a, const void *b) {
    const SlicesEqualFunc *eq = env;
    return BURROW_CALLF(*eq, a, b) ? 0 : 1;
}

static SlicesEqualFunc eqf(bool (*f)(void *, const void *, const void *)) {
    return BURROW_FN(SlicesEqualFunc, f, NULL);
}

static SlicesCmpFunc cmpf(int (*f)(void *, const void *, const void *)) {
    return BURROW_FN(SlicesCmpFunc, f, NULL);
}

typedef struct EqualIntTest {
    Slice s1, s2;
    bool want;
} EqualIntTest;

typedef struct EqualFloatTest {
    Slice s1, s2;
    bool want_equal, want_equal_nan;
} EqualFloatTest;

static Int equal_int_tests(EqualIntTest *t) {
    t[0] = (EqualIntTest){IS(1), INIL(), false};
    t[1] = (EqualIntTest){INONE(), INIL(), true};
    t[2] = (EqualIntTest){IS(1, 2, 3), IS(1, 2, 3), true};
    t[3] = (EqualIntTest){IS(1, 2, 3), IS(1, 2, 3, 4), false};
    return 4;
}

static Int equal_float_tests(EqualFloatTest *t) {
    double nan = (double)NAN;
    t[0] = (EqualFloatTest){FS(1, 2), FS(1, 2), true, true};
    t[1] = (EqualFloatTest){FS(1, 2, nan), FS(1, 2, nan), false, true};
    return 2;
}

static void TestEqual(TestingT *t) {
    arena_start();
    EqualIntTest it[4];
    EqualFloatTest ft[2];
    for (Int i = 0, n = equal_int_tests(it); i < n; i++)
        if (slices_equal(it[i].s1, it[i].s2) != it[i].want)
            testing_t_errorf_v(t, "Equal int case %d wrong", i);
    for (Int i = 0, n = equal_float_tests(ft); i < n; i++)
        if (slices_equal(ft[i].s1, ft[i].s2) != ft[i].want_equal)
            testing_t_errorf_v(t, "Equal float case %d wrong", i);
    /* Strings compare by their bytes, not by where the bytes are. */
    char buf[] = "abc";
    Str dup = str_from_bytes((const Byte *)buf, 3);
    Slice s1 = SS("abc", "d");
    Slice s2 = SS("x", "d");
    ((Str *)s2.p)[0] = dup;
    CHECK(slices_equal(s1, s2));
    arena_done();
}

static void TestEqualFunc(TestingT *t) {
    arena_start();
    EqualIntTest it[4];
    EqualFloatTest ft[2];
    for (Int i = 0, n = equal_int_tests(it); i < n; i++)
        if (slices_equal_func(it[i].s1, it[i].s2, eqf(eq_int)) != it[i].want)
            testing_t_errorf_v(t, "EqualFunc int case %d wrong", i);
    for (Int i = 0, n = equal_float_tests(ft); i < n; i++) {
        if (slices_equal_func(ft[i].s1, ft[i].s2, eqf(eq_float)) != ft[i].want_equal)
            testing_t_errorf_v(t, "EqualFunc equal[float64] case %d wrong", i);
        if (slices_equal_func(ft[i].s1, ft[i].s2, eqf(eq_float_nan)) !=
            ft[i].want_equal_nan)
            testing_t_errorf_v(t, "EqualFunc equalNaN case %d wrong", i);
    }
    Slice s1 = IS(1, 2, 3);
    Slice s2 = IS(2, 3, 4);
    CHECK(!slices_equal_func(s1, s1, eqf(off_by_one)));
    CHECK(slices_equal_func(s1, s2, eqf(off_by_one)));
    Slice s3 = SS("a", "b", "c");
    Slice s4 = SS("A", "B", "C");
    CHECK(slices_equal_func(s3, s4, eqf(eq_fold)));
    CHECK(slices_equal_func(s1, s3, eqf(eq_int_string)));
    arena_done();
}

typedef struct CompareTest {
    Slice s1, s2;
    int want;
} CompareTest;

static Int compare_int_tests(CompareTest *t) {
    t[0] = (CompareTest){IS(1), IS(1), 0};
    t[1] = (CompareTest){IS(1), INONE(), 1};
    t[2] = (CompareTest){INONE(), IS(1), -1};
    t[3] = (CompareTest){INONE(), INONE(), 0};
    t[4] = (CompareTest){IS(1, 2, 3), IS(1, 2, 3), 0};
    t[5] = (CompareTest){IS(1, 2, 3), IS(1, 2, 3, 4), -1};
    t[6] = (CompareTest){IS(1, 2, 3, 4), IS(1, 2, 3), +1};
    t[7] = (CompareTest){IS(1, 2, 3), IS(1, 4, 3), -1};
    t[8] = (CompareTest){IS(1, 4, 3), IS(1, 2, 3), +1};
    t[9] = (CompareTest){IS(1, 4, 3), IS(1, 2, 3, 8, 9), +1};
    return 10;
}

static Int compare_float_tests(CompareTest *t) {
    double nan = (double)NAN;
    Slice empty = floats_of(NULL, 0);
    t[0] = (CompareTest){empty, empty, 0};
    t[1] = (CompareTest){FS(1), FS(1), 0};
    t[2] = (CompareTest){FS(nan), FS(nan), 0};
    t[3] = (CompareTest){FS(1, 2, nan), FS(1, 2, nan), 0};
    t[4] = (CompareTest){FS(1, nan, 3), FS(1, nan, 4), -1};
    t[5] = (CompareTest){FS(1, nan, 3), FS(1, 2, 4), -1};
    t[6] = (CompareTest){FS(1, nan, 3), FS(1, 2, nan), -1};
    t[7] = (CompareTest){FS(1, 2, 3), FS(1, 2, nan), +1};
    t[8] = (CompareTest){FS(1, 2, 3), FS(1, nan, 3), +1};
    t[9] = (CompareTest){FS(1, nan, 3, 4), FS(1, 2, nan), -1};
    return 10;
}

static void TestCompare(TestingT *t) {
    arena_start();
    EqualIntTest it[4];
    EqualFloatTest ft[2];
    CompareTest ct[10];
    for (Int i = 0, n = equal_int_tests(it); i < n; i++)
        if ((slices_compare(it[i].s1, it[i].s2) == 0) != it[i].want)
            testing_t_errorf_v(t, "Compare equal int case %d wrong", i);
    for (Int i = 0, n = equal_float_tests(ft); i < n; i++)
        if ((slices_compare(ft[i].s1, ft[i].s2) == 0) != ft[i].want_equal_nan)
            testing_t_errorf_v(t, "Compare equal float case %d wrong", i);
    for (Int i = 0, n = compare_int_tests(ct); i < n; i++)
        CHECK_INT_EQ(slices_compare(ct[i].s1, ct[i].s2), ct[i].want);
    for (Int i = 0, n = compare_float_tests(ct); i < n; i++)
        CHECK_INT_EQ(slices_compare(ct[i].s1, ct[i].s2), ct[i].want);
    CHECK_INT_EQ(slices_compare(SS("a", "b"), SS("a", "c")), -1);
    arena_done();
}

static void TestCompareFunc(TestingT *t) {
    arena_start();
    EqualIntTest it[4];
    EqualFloatTest ft[2];
    CompareTest ct[10];
    SlicesEqualFunc ei = eqf(eq_int), ef = eqf(eq_float), eo = eqf(off_by_one);
    for (Int i = 0, n = equal_int_tests(it); i < n; i++) {
        int got = slices_compare_func(it[i].s1, it[i].s2,
                                      BURROW_FN(SlicesCmpFunc, eq_to_cmp, &ei));
        if ((got == 0) != it[i].want)
            testing_t_errorf_v(t, "CompareFunc equal int case %d wrong", i);
    }
    for (Int i = 0, n = equal_float_tests(ft); i < n; i++) {
        int got = slices_compare_func(ft[i].s1, ft[i].s2,
                                      BURROW_FN(SlicesCmpFunc, eq_to_cmp, &ef));
        if ((got == 0) != ft[i].want_equal)
            testing_t_errorf_v(t, "CompareFunc equal float case %d wrong", i);
    }
    for (Int i = 0, n = compare_int_tests(ct); i < n; i++)
        CHECK_INT_EQ(slices_compare_func(ct[i].s1, ct[i].s2, cmpf(cmp_int)),
                     ct[i].want);
    for (Int i = 0, n = compare_float_tests(ct); i < n; i++)
        CHECK_INT_EQ(slices_compare_func(ct[i].s1, ct[i].s2, cmpf(cmp_float)),
                     ct[i].want);

    Slice s1 = IS(1, 2, 3);
    Slice s2 = IS(2, 3, 4);
    CHECK_INT_EQ(slices_compare_func(s1, s2, BURROW_FN(SlicesCmpFunc, eq_to_cmp, &eo)),
                 0);
    Slice s3 = SS("a", "b", "c");
    Slice s4 = SS("A", "B", "C");
    CHECK_INT_EQ(slices_compare_func(s3, s4, cmpf(cmp_str)), 1);
    CHECK_INT_EQ(slices_compare_func(s3, s4, cmpf(cmp_lower)), 0);
    CHECK_INT_EQ(slices_compare_func(s1, s3, cmpf(cmp_int_string)), 0);
    arena_done();
}

typedef struct IndexTest {
    Slice s;
    Int v, want;
} IndexTest;

static Int index_tests(IndexTest *t) {
    t[0] = (IndexTest){INIL(), 0, -1};
    t[1] = (IndexTest){INONE(), 0, -1};
    t[2] = (IndexTest){IS(1, 2, 3), 2, 1};
    t[3] = (IndexTest){IS(1, 2, 2, 3), 2, 1};
    t[4] = (IndexTest){IS(1, 2, 3, 2), 2, 1};
    return 5;
}

/* Go's equalToIndex(f, v1): a test that is f(v1, element). */
typedef struct EqualTo {
    SlicesEqualFunc f;
    const void *v;
} EqualTo;

static bool equal_to(void *env, const void *v) {
    const EqualTo *e = env;
    return BURROW_CALLF(e->f, e->v, v);
}

static SlicesPredFunc equal_to_index(EqualTo *e) {
    return BURROW_FN(SlicesPredFunc, equal_to, e);
}

static void TestIndex(TestingT *t) {
    arena_start();
    IndexTest it[5];
    for (Int i = 0, n = index_tests(it); i < n; i++) {
        CHECK_INT_EQ(slices_index(it[i].s, &it[i].v), it[i].want);
        CHECK(slices_contains(it[i].s, &it[i].v) == (it[i].want != -1));
    }
    /* Every width the fast paths cover. */
    Byte b[] = {1, 2, 3, 2};
    uint16_t h[] = {1, 2, 3, 2};
    uint32_t w[] = {1, 2, 3, 2};
    Byte b3 = 3;
    uint16_t h3 = 3;
    uint32_t w3 = 3;
    CHECK_INT_EQ(slices_index(slice_from(b, 4, 4, TYPE_BYTE), &b3), 2);
    CHECK_INT_EQ(slices_index(slice_from(h, 4, 4, TYPE_UINT16), &h3), 2);
    CHECK_INT_EQ(slices_index(slice_from(w, 4, 4, TYPE_UINT32), &w3), 2);
    Str hi = BURROW_S("HI");
    CHECK_INT_EQ(slices_index(SS("hi", "HI"), &hi), 1);
    /* A NaN is not equal to itself, so it is never found. */
    double nan = (double)NAN;
    CHECK_INT_EQ(slices_index(FS(1, nan), &nan), -1);
    arena_done();
}

static void TestIndexFunc(TestingT *t) {
    arena_start();
    IndexTest it[5];
    for (Int i = 0, n = index_tests(it); i < n; i++) {
        EqualTo e = {eqf(eq_int), &it[i].v};
        CHECK_INT_EQ(slices_index_func(it[i].s, equal_to_index(&e)), it[i].want);
        CHECK(slices_contains_func(it[i].s, equal_to_index(&e)) == (it[i].want != -1));
    }
    Slice s1 = SS("hi", "HI");
    Str hi = BURROW_S("HI"), lo = BURROW_S("hI");
    EqualTo exact = {eqf(eq_str), &hi};
    EqualTo fold = {eqf(eq_fold), &hi};
    CHECK_INT_EQ(slices_index_func(s1, equal_to_index(&exact)), 1);
    CHECK_INT_EQ(slices_index_func(s1, equal_to_index(&fold)), 0);
    CHECK(slices_contains_func(s1, equal_to_index(&exact)));
    EqualTo exact_lo = {eqf(eq_str), &lo};
    EqualTo fold_lo = {eqf(eq_fold), &lo};
    CHECK(!slices_contains_func(s1, equal_to_index(&exact_lo)));
    CHECK(slices_contains_func(s1, equal_to_index(&fold_lo)));
    arena_done();
}

typedef struct InsertTest {
    Slice s;
    Int i;
    Slice add, want;
} InsertTest;

static void TestInsert(TestingT *t) {
    arena_start();
    Slice s = IS(1, 2, 3);
    CHECK(slices_equal(slices_insert(A, s, 0, NULL, 0), s));
    InsertTest tests[] = {
        {IS(1, 2, 3), 0, IS(4), IS(4, 1, 2, 3)},
        {IS(1, 2, 3), 1, IS(4), IS(1, 4, 2, 3)},
        {IS(1, 2, 3), 3, IS(4), IS(1, 2, 3, 4)},
        {IS(1, 2, 3), 2, IS(4, 5), IS(1, 2, 4, 5, 3)},
    };
    for (Int i = 0; i < NELEM(tests); i++) {
        Slice c = slices_clone(A, tests[i].s);
        Slice got = slices_insert(A, c, tests[i].i, tests[i].add.p, tests[i].add.len);
        if (!slices_equal(got, tests[i].want))
            testing_t_errorf_v(t, "Insert case %d wrong", i);
    }
    /* Growth is amortized: fifty inserts at the front move to a new array
     * far fewer than fifty times. */
    s = IS(1, 2, 3);
    Int one = 1, moves = 0;
    for (Int i = 0; i < 50; i++) {
        Slice next = slices_insert(A, s, 0, &one, 1);
        moves += next.p != s.p;
        s = next;
    }
    CHECK_INT_EQ(s.len, 53);
    if (moves > 25)
        testing_t_errorf_v(t, "too many allocations inserting 50 elements: got %d",
                           moves);
    arena_done();
}

static void TestInsertOverlap(TestingT *t) {
    arena_start();
    enum { N = 10 };
    Int a[N];
    Int wantbuf[2 * N];
    for (Int n = 0; n <= N; n++) {
        for (Int i = 0; i <= n; i++) {
            for (Int x = 0; x <= N; x++) {
                for (Int y = x; y <= N; y++) {
                    for (Int k = 0; k < N; k++)
                        a[k] = k;
                    Int w = 0;
                    for (Int k = 0; k < i; k++)
                        wantbuf[w++] = a[k];
                    for (Int k = x; k < y; k++)
                        wantbuf[w++] = a[k];
                    for (Int k = i; k < n; k++)
                        wantbuf[w++] = a[k];
                    Slice got = slices_insert(A, slice_from(a, n, N, TYPE_INT), i,
                                              &a[x], y - x);
                    if (!slices_equal(got, slice_from(wantbuf, w, w, TYPE_INT)))
                        testing_t_errorf_v(
                            t, "Insert with overlap failed n=%d i=%d x=%d y=%d", n, i,
                            x, y);
                }
            }
        }
    }
    arena_done();
}

/* The arguments for one call that ought to panic. */
typedef struct PanicCall {
    Slice s;
    Int i, j;
    Slice v;
} PanicCall;

static void call_insert(void *env) {
    PanicCall *c = env;
    (void)slices_insert(A, c->s, c->i, c->v.p, c->v.len);
}

static void call_delete(void *env) {
    PanicCall *c = env;
    (void)slices_delete(c->s, c->i, c->j);
}

static void call_replace(void *env) {
    PanicCall *c = env;
    (void)slices_replace(A, c->s, c->i, c->j, c->v.p, c->v.len);
}

static void call_grow(void *env) {
    PanicCall *c = env;
    (void)slices_grow(A, c->s, c->i);
}

static void call_repeat(void *env) {
    PanicCall *c = env;
    (void)slices_repeat(A, c->s, c->i);
}

static void call_chunk(void *env) {
    PanicCall *c = env;
    (void)slices_chunk(A, c->s, c->i);
}

static void call_min(void *env) {
    (void)slices_min(((PanicCall *)env)->s);
}

static void call_max(void *env) {
    (void)slices_max(((PanicCall *)env)->s);
}

static void call_min_func(void *env) {
    (void)slices_min_func(((PanicCall *)env)->s, cmpf(sub_int));
}

static void call_max_func(void *env) {
    (void)slices_max_func(((PanicCall *)env)->s, cmpf(sub_int));
}

static void call_sort(void *env) {
    slices_sort(((PanicCall *)env)->s);
}

static bool call_panics(void (*f)(void *), PanicCall c) {
    return panics(BURROW_FN(Func, f, &c));
}

static void TestInsertPanics(TestingT *t) {
    arena_start();
    Int a[3] = {0}, b[1] = {0};
    Slice none = INIL(), one = slice_from(b, 1, 1, TYPE_INT);
    Slice vs[] = {none, one};
    for (Int k = 0; k < 2; k++) {
        CHECK(call_panics(call_insert,
                          (PanicCall){slice_from(a, 1, 1, TYPE_INT), -1, 0, vs[k]}));
        CHECK(call_panics(call_insert,
                          (PanicCall){slice_from(a, 1, 1, TYPE_INT), 2, 0, vs[k]}));
        CHECK(call_panics(call_insert,
                          (PanicCall){slice_from(a, 1, 2, TYPE_INT), 2, 0, vs[k]}));
        CHECK(call_panics(call_insert,
                          (PanicCall){slice_from(a, 1, 3, TYPE_INT), 2, 0, vs[k]}));
    }
    arena_done();
}

static void TestDelete(TestingT *t) {
    arena_start();
    struct {
        Slice s;
        Int i, j;
        Slice want;
    } tests[] = {
        {IS(1, 2, 3), 0, 0, IS(1, 2, 3)}, {IS(1, 2, 3), 0, 1, IS(2, 3)},
        {IS(1, 2, 3), 3, 3, IS(1, 2, 3)}, {IS(1, 2, 3), 0, 2, IS(3)},
        {IS(1, 2, 3), 0, 3, INONE()},
    };
    for (Int i = 0; i < NELEM(tests); i++) {
        Slice c = slices_clone(A, tests[i].s);
        if (!slices_equal(slices_delete(c, tests[i].i, tests[i].j), tests[i].want))
            testing_t_errorf_v(t, "Delete case %d wrong", i);
    }
    arena_done();
}

static bool pred_true(void *env, const void *v) {
    (void)env;
    (void)v;
    return true;
}

static bool pred_false(void *env, const void *v) {
    (void)env;
    (void)v;
    return false;
}

static bool pred_gt2(void *env, const void *v) {
    (void)env;
    return *(const Int *)v > 2;
}

static bool pred_lt2(void *env, const void *v) {
    (void)env;
    return *(const Int *)v < 2;
}

static bool pred_ge10(void *env, const void *v) {
    (void)env;
    return *(const Int *)v >= 10;
}

static SlicesPredFunc predf(bool (*f)(void *, const void *)) {
    return BURROW_FN(SlicesPredFunc, f, NULL);
}

static void TestDeleteFunc(TestingT *t) {
    arena_start();
    struct {
        Slice s;
        SlicesPredFunc fn;
        Slice want;
    } tests[] = {
        {INIL(), predf(pred_true), INIL()},
        {IS(1, 2, 3), predf(pred_true), INIL()},
        {IS(1, 2, 3), predf(pred_false), IS(1, 2, 3)},
        {IS(1, 2, 3), predf(pred_gt2), IS(1, 2)},
        {IS(1, 2, 3), predf(pred_lt2), IS(2, 3)},
        {IS(10, 2, 30), predf(pred_ge10), IS(2)},
    };
    for (Int i = 0; i < NELEM(tests); i++) {
        Slice c = slices_clone(A, tests[i].s);
        if (!slices_equal(slices_delete_func(c, tests[i].fn), tests[i].want))
            testing_t_errorf_v(t, "DeleteFunc case %d wrong", i);
    }
    arena_done();
}

static void TestDeletePanics(TestingT *t) {
    arena_start();
    Int five[] = {0, 1, 2, 3, 4};
    Slice s = slice_from(five, 2, 5, TYPE_INT);
    struct {
        Slice s;
        Int i, j;
    } tests[] = {
        {IS(42), -2, 1}, {IS(42), 1, -1}, {IS(42), 2, 3}, {IS(42), 0, 2},
        {IS(42), 2, 2},  {IS(42), 1, 0},  {s, 0, 4},      {s, 3, 3},
    };
    for (Int i = 0; i < NELEM(tests); i++)
        if (!call_panics(call_delete,
                         (PanicCall){tests[i].s, tests[i].i, tests[i].j, INIL()}))
            testing_t_errorf_v(t, "Delete case %d: got no panic, want panic", i);
    arena_done();
}

/* A pointer element type, for the tests that watch discarded elements being
 * set to nil. */
typedef Int *IntPtr;

static const Type int_ptr_type = {
    {(const Byte *)"*int", 4},
    {NULL, 0},
    KIND_POINTER,
    (uint32_t)sizeof(IntPtr),
    (uint16_t)_Alignof(IntPtr),
    0,
    0,
    NULL,
    NULL,
    &burrow_type_Int,
    NULL,
    0,
    0,
    NULL,
};

static Slice ptrs(IntPtr *p, Int len, Int cap) {
    return slice_from(p, len, cap, &int_ptr_type);
}

static void TestDeleteClearTail(TestingT *t) {
    Int v[6] = {0};
    IntPtr mem[6] = {&v[0], &v[1], &v[2], &v[3], &v[4], &v[5]};
    Slice s = slices_delete(ptrs(mem, 5, 6), 2, 4);
    CHECK_INT_EQ(s.len, 3);
    CHECK(mem[3] == NULL && mem[4] == NULL);
    CHECK(mem[5] != NULL);
}

static bool is_42(void *env, const void *v) {
    (void)env;
    IntPtr p = *(const IntPtr *)v;
    return p != NULL && *p == 42;
}

static void TestDeleteFuncClearTail(TestingT *t) {
    Int v[6] = {0, 0, 42, 42, 0, 0};
    IntPtr mem[6] = {&v[0], &v[1], &v[2], &v[3], &v[4], &v[5]};
    Slice s = slices_delete_func(ptrs(mem, 5, 6), predf(is_42));
    CHECK_INT_EQ(s.len, 3);
    CHECK(mem[3] == NULL && mem[4] == NULL);
    CHECK(mem[5] != NULL);
}

static void TestClone(TestingT *t) {
    arena_start();
    Slice s1 = IS(1, 2, 3);
    Slice s2 = slices_clone(A, s1);
    CHECK(slices_equal(s1, s2));
    ip(s1)[0] = 4;
    CHECK(slices_equal(s2, IS(1, 2, 3)));
    CHECK(slice_is_nil(slices_clone(A, INIL())));
    Slice e = slices_clone(A, slice_sub(s1, 0, 0));
    CHECK(!slice_is_nil(e) && e.len == 0);
    arena_done();
}

typedef struct CompactTest {
    Slice s, want;
} CompactTest;

static Int compact_tests(CompactTest *t) {
    t[0] = (CompactTest){INIL(), INIL()};
    t[1] = (CompactTest){IS(1), IS(1)};
    t[2] = (CompactTest){IS(1, 2, 3), IS(1, 2, 3)};
    t[3] = (CompactTest){IS(1, 1, 2), IS(1, 2)};
    t[4] = (CompactTest){IS(1, 2, 1), IS(1, 2, 1)};
    t[5] = (CompactTest){IS(1, 2, 2, 3, 3, 4), IS(1, 2, 3, 4)};
    return 6;
}

static void TestCompact(TestingT *t) {
    arena_start();
    CompactTest ct[6];
    for (Int i = 0, n = compact_tests(ct); i < n; i++) {
        Slice c = slices_clone(A, ct[i].s);
        if (!slices_equal(slices_compact(c), ct[i].want))
            testing_t_errorf_v(t, "Compact case %d wrong", i);
    }
    arena_done();
}

static void TestCompactFunc(TestingT *t) {
    arena_start();
    CompactTest ct[6];
    for (Int i = 0, n = compact_tests(ct); i < n; i++) {
        Slice c = slices_clone(A, ct[i].s);
        if (!slices_equal(slices_compact_func(c, eqf(eq_int)), ct[i].want))
            testing_t_errorf_v(t, "CompactFunc case %d wrong", i);
    }
    Slice s1 = SS("a", "a", "A", "B", "b");
    CHECK(slices_equal(slices_compact_func(slices_clone(A, s1), eqf(eq_fold)),
                       SS("a", "B")));
    arena_done();
}

static void TestCompactClearTail(TestingT *t) {
    Int one = 1, two = 2, three = 3, four = 4;
    IntPtr mem[6] = {&one, &one, &two, &two, &three, &four};
    Slice s = slices_compact(ptrs(mem, 5, 6));
    IntPtr want[] = {&one, &two, &three};
    CHECK(slices_equal(s, ptrs(want, 3, 3)));
    CHECK(mem[3] == NULL && mem[4] == NULL);
    CHECK(mem[5] == &four);
}

static bool eq_deref(void *env, const void *a, const void *b) {
    (void)env;
    IntPtr x = *(const IntPtr *)a, y = *(const IntPtr *)b;
    if (x == NULL || y == NULL)
        return x == y;
    return *x == *y;
}

static void TestCompactFuncClearTail(TestingT *t) {
    Int a = 1, b = 1, c = 2, d = 2, e = 3, f = 4;
    IntPtr mem[6] = {&a, &b, &c, &d, &e, &f};
    Slice s = slices_compact_func(ptrs(mem, 5, 6), eqf(eq_deref));
    IntPtr want[] = {&a, &c, &e};
    CHECK(slices_equal(s, ptrs(want, 3, 3)));
    CHECK(mem[3] == NULL && mem[4] == NULL);
    CHECK(mem[5] == &f);
}

static void TestGrow(TestingT *t) {
    arena_start();
    Slice s1 = IS(1, 2, 3);
    Slice s2 = slices_grow(A, slices_clone(A, s1), 1000);
    CHECK(slices_equal(s1, s2));
    CHECK(s2.cap >= 1000 + s1.len);

    /* The elements between length and capacity are left alone. */
    Slice c = slices_clone(A, s1);
    Slice s3 = slice_sub(slices_grow(A, slice_sub(c, 0, 1), 2), 0, 3);
    CHECK(slices_equal(s1, s3));
    s3 = slice_sub(slices_grow(A, slice_sub(c, 0, 1), 1000), 0, 3);
    CHECK(slices_equal(s1, s3));

    /* Enough room already means the same array back. */
    Slice s4 = slices_grow(A, s2, s2.cap - s2.len);
    CHECK(s4.p == s2.p && s4.cap == s2.cap);
    Slice s5 = slices_grow(A, s2, s2.cap - s2.len + 1);
    CHECK(s5.p != s2.p && s5.cap > s2.cap);

    CHECK(call_panics(call_grow, (PanicCall){s1, -1, 0, INIL()}));
    arena_done();
}

static void TestClip(TestingT *t) {
    arena_start();
    Slice s1 = slice_sub(IS(1, 2, 3, 4, 5, 6), 0, 3);
    CHECK_INT_EQ(s1.len, 3);
    CHECK(s1.cap >= 6);
    Slice s2 = slices_clip(s1);
    CHECK(slices_equal(s1, s2));
    CHECK_INT_EQ(s2.cap, 3);
    arena_done();
}

static void TestReverse(TestingT *t) {
    arena_start();
    Slice even = IS(3, 1, 4, 1, 5, 9);
    slices_reverse(even);
    CHECK(slices_equal(even, IS(9, 5, 1, 4, 1, 3)));
    Slice odd = IS(3, 1, 4, 1, 5, 9, 2);
    slices_reverse(odd);
    CHECK(slices_equal(odd, IS(2, 9, 5, 1, 4, 1, 3)));
    Slice words = strings_fields(A, BURROW_S("one two three"));
    slices_reverse(words);
    CHECK(slices_equal(words, strings_fields(A, BURROW_S("three two one"))));
    Slice singleton = SS("one");
    slices_reverse(singleton);
    CHECK(slices_equal(singleton, SS("one")));
    slices_reverse(slice_nil(TYPE_STRING));
    arena_done();
}

/* Go's naiveReplace, the baseline Replace is checked against. */
static Slice naive_replace(Slice s, Int i, Int j, Slice v) {
    s = slices_delete(s, i, j);
    return slices_insert(A, s, i, v.p, v.len);
}

static void TestReplace(TestingT *t) {
    arena_start();
    Slice big = slice_make(A, TYPE_INT, 3, 20);
    ip(big)[0] = 0;
    ip(big)[1] = 1;
    ip(big)[2] = 2;
    struct {
        Slice s, v;
        Int i, j;
    } tests[] = {
        {INIL(), INIL(), 0, 0},
        {IS(1, 2, 3, 4), IS(5), 1, 2},
        {IS(1, 2, 3, 4), IS(5, 6, 7, 8), 1, 2},
        {big, IS(3, 4, 5, 6, 7), 0, 1},
    };
    for (Int k = 0; k < NELEM(tests); k++) {
        Slice ss = slices_clone(A, tests[k].s), vv = slices_clone(A, tests[k].v);
        Slice want = naive_replace(ss, tests[k].i, tests[k].j, vv);
        Slice got = slices_replace(A, tests[k].s, tests[k].i, tests[k].j, tests[k].v.p,
                                   tests[k].v.len);
        if (!slices_equal(got, want))
            testing_t_errorf_v(t, "Replace case %d wrong", k);
    }
    arena_done();
}

static void TestReplacePanics(TestingT *t) {
    arena_start();
    Int five[] = {0, 1, 2, 3, 4};
    Slice s = slice_from(five, 2, 5, TYPE_INT);
    struct {
        Slice s, v;
        Int i, j;
    } tests[] = {
        {IS(1, 2), IS(3), 2, 1},
        {IS(1, 2), IS(3), 1, 10},
        {IS(1, 2), IS(3), -1, 2},
        {s, INIL(), 0, 4},
    };
    for (Int k = 0; k < NELEM(tests); k++) {
        PanicCall c = {tests[k].s, tests[k].i, tests[k].j, tests[k].v};
        if (!call_panics(call_replace, c))
            testing_t_errorf_v(t, "Replace case %d: should have panicked", k);
    }
    arena_done();
}

static void TestReplaceGrow(TestingT *t) {
    arena_start();
    Int a = 1, b = 2, c = 3, d = 4, e = 5, f = 6;
    IntPtr mem[6] = {&a, &b, &c, &d, &e, &f};
    IntPtr memcopy[6];
    memcpy(memcopy, mem, sizeof mem);
    Slice s = ptrs(mem, 5, 6);
    Slice copy = slices_clone(A, s);
    Slice original = s;

    /* The new elements do not fit within cap(s), so Replace allocates. */
    Int z = 99;
    IntPtr zs[] = {&z, &z, &z, &z};
    s = slices_replace(A, s, 1, 3, zs, 4);

    IntPtr want[] = {&a, &z, &z, &z, &z, &d, &e};
    CHECK(slices_equal(s, ptrs(want, 7, 7)));
    CHECK(slices_equal(original, copy));
    CHECK(slices_equal(ptrs(mem, 6, 6), ptrs(memcopy, 6, 6)));
    arena_done();
}

static void TestReplaceClearTail(TestingT *t) {
    arena_start();
    Int a = 1, b = 2, c = 3, d = 4, e = 5, f = 6;
    IntPtr mem[6] = {&a, &b, &c, &d, &e, &f};
    Int y = 8, z = 9;
    IntPtr yz[] = {&y, &z};
    Slice s = slices_replace(A, ptrs(mem, 5, 6), 1, 4, yz, 2);
    IntPtr want[] = {&a, &y, &z, &e};
    CHECK(slices_equal(s, ptrs(want, 4, 4)));
    CHECK(mem[4] == NULL);
    CHECK(mem[5] == &f);
    arena_done();
}

static void TestReplaceOverlap(TestingT *t) {
    arena_start();
    enum { N = 10 };
    Int a[N];
    Int wantbuf[2 * N];
    for (Int n = 0; n <= N; n++) {
        for (Int i = 0; i <= n; i++) {
            for (Int j = i; j <= n; j++) {
                for (Int x = 0; x <= N; x++) {
                    for (Int y = x; y <= N; y++) {
                        for (Int k = 0; k < N; k++)
                            a[k] = k;
                        Int w = 0;
                        for (Int k = 0; k < i; k++)
                            wantbuf[w++] = a[k];
                        for (Int k = x; k < y; k++)
                            wantbuf[w++] = a[k];
                        for (Int k = j; k < n; k++)
                            wantbuf[w++] = a[k];
                        Slice got = slices_replace(A, slice_from(a, n, N, TYPE_INT), i,
                                                   j, &a[x], y - x);
                        if (!slices_equal(got, slice_from(wantbuf, w, w, TYPE_INT)))
                            testing_t_errorf_v(
                                t,
                                "Replace with overlap failed n=%d i=%d j=%d x=%d y=%d",
                                n, i, j, x, y);
                    }
                }
            }
        }
    }
    arena_done();
}

static void TestReplaceEndClearTail(TestingT *t) {
    arena_start();
    Slice s = IS(11, 22, 33);
    Int v = 99;
    s = slices_replace(A, s, 1, 3, &v, 1);
    CHECK_INT_EQ(ip(slice_sub(s, 0, 3))[2], 0);
    arena_done();
}

/* How many times a million inserts or replaces at the end grow the array. Go
 * grows by at least 1.25 once a slice is large, which bounds the count. */
static void TestInsertGrowthRate(TestingT *t) {
    arena_start();
    Slice b = slice_make(A, TYPE_BYTE, 1, 1);
    Int max_cap = b.cap, n_grow = 0;
    Byte zero = 0;
    for (Int i = 0; i < 1000000; i++) {
        b = slices_insert(A, b, b.len - 1, &zero, 1);
        if (b.cap > max_cap) {
            max_cap = b.cap;
            n_grow++;
        }
    }
    Int want = (Int)(log(1e6) / log(1.25));
    if (n_grow > want)
        testing_t_errorf_v(t, "too many grows. got:%d want:%d", n_grow, want);
    arena_done();
}

static void TestReplaceGrowthRate(TestingT *t) {
    arena_start();
    Slice b = slice_make(A, TYPE_BYTE, 2, 2);
    Int max_cap = b.cap, n_grow = 0;
    Byte zeros[2] = {0};
    for (Int i = 0; i < 1000000; i++) {
        b = slices_replace(A, b, b.len - 2, b.len - 1, zeros, 2);
        if (b.cap > max_cap) {
            max_cap = b.cap;
            n_grow++;
        }
    }
    Int want = (Int)(log(1e6) / log(1.25));
    if (n_grow > want)
        testing_t_errorf_v(t, "too many grows. got:%d want:%d", n_grow, want);
    arena_done();
}

static void TestConcat(TestingT *t) {
    arena_start();
    Slice c1[] = {INIL()};
    Slice c2[] = {IS(1)};
    Slice c3[] = {IS(1), IS(2)};
    Slice c4[] = {IS(1), INIL(), IS(2)};
    CHECK(slice_is_nil(slices_concat(A, c1, 1)));
    CHECK(slices_equal(slices_concat(A, c2, 1), IS(1)));
    CHECK(slices_equal(slices_concat(A, c3, 2), IS(1, 2)));
    CHECK(slices_equal(slices_concat(A, c4, 3), IS(1, 2)));
    /* One allocation, sized exactly. */
    Slice got = slices_concat(A, c4, 3);
    CHECK_INT_EQ(got.cap, 2);
    arena_done();
}

/* struct{}, which takes no memory, so a slice of it can be as long as an Int
 * allows. */
static const Type void_type = {
    {(const Byte *)"struct {}", 9},
    {NULL, 0},
    KIND_STRUCT,
    0,
    1,
    0,
    0,
    NULL,
    NULL,
    NULL,
    NULL,
    0,
    0,
    NULL,
};

static Byte void_array;

static Slice voids(Int n) {
    return slice_from(&void_array, n, n, &void_type);
}

static void call_concat(void *env) {
    const Slice *ss = env;
    Int n = 0;
    while (ss[n].elem != NULL)
        n++;
    (void)slices_concat(A, ss, n);
}

static void TestConcatTooLarge(TestingT *t) {
    arena_start();
    struct {
        Int lengths[3];
        Int n;
        bool should_panic;
    } tests[] = {
        {{0, 0}, 2, false},
        {{BURROW_INT_MAX, 0}, 2, false},
        {{0, BURROW_INT_MAX}, 2, false},
        {{BURROW_INT_MAX - 1, 1}, 2, false},
        {{BURROW_INT_MAX - 1, 1, 1}, 3, true},
        {{BURROW_INT_MAX, 1}, 2, true},
        {{BURROW_INT_MAX, BURROW_INT_MAX}, 2, true},
    };
    for (Int k = 0; k < NELEM(tests); k++) {
        Slice ss[4] = {{0}};
        for (Int i = 0; i < tests[k].n; i++)
            ss[i] = voids(tests[k].lengths[i]);
        bool did = panics(BURROW_FN(Func, call_concat, ss));
        if (did != tests[k].should_panic)
            testing_t_errorf_v(t, "Concat case %d got panic == %t", k, did);
    }
    arena_done();
}

static void TestRepeat(TestingT *t) {
    arena_start();
    struct {
        Slice x;
        Int count;
        Slice want;
    } tests[] = {
        {INIL(), 0, INONE()},
        {INIL(), 1, INONE()},
        {INIL(), BURROW_INT_MAX, INONE()},
        {INONE(), 0, INONE()},
        {INONE(), 1, INONE()},
        {INONE(), BURROW_INT_MAX, INONE()},
        {IS(0), 0, INONE()},
        {IS(0), 1, IS(0)},
        {IS(0), 2, IS(0, 0)},
        {IS(0), 3, IS(0, 0, 0)},
        {IS(0), 4, IS(0, 0, 0, 0)},
        {IS(0, 1), 0, INONE()},
        {IS(0, 1), 1, IS(0, 1)},
        {IS(0, 1), 2, IS(0, 1, 0, 1)},
        {IS(0, 1), 3, IS(0, 1, 0, 1, 0, 1)},
        {IS(0, 1), 4, IS(0, 1, 0, 1, 0, 1, 0, 1)},
        {IS(0, 1, 2), 0, INONE()},
        {IS(0, 1, 2), 1, IS(0, 1, 2)},
        {IS(0, 1, 2), 2, IS(0, 1, 2, 0, 1, 2)},
        {IS(0, 1, 2), 3, IS(0, 1, 2, 0, 1, 2, 0, 1, 2)},
        {IS(0, 1, 2), 4, IS(0, 1, 2, 0, 1, 2, 0, 1, 2, 0, 1, 2)},
    };
    for (Int k = 0; k < NELEM(tests); k++) {
        Slice got = slices_repeat(A, tests[k].x, tests[k].count);
        if (slice_is_nil(got) || got.cap != tests[k].want.cap ||
            !slices_equal(got, tests[k].want))
            testing_t_errorf_v(t, "Repeat case %d wrong", k);
    }
    /* Big slices of struct{}. */
    for (Int count = 1; count <= 9; count++) {
        Int n = BURROW_INT_MAX / count - (count - 1);
        Slice got = slices_repeat(A, voids(n), count);
        if (slice_is_nil(got) || got.len != count * n || got.cap != count * n)
            testing_t_errorf_v(t, "Repeat(make([]struct{}, %d), %d) wrong", n, count);
    }
    arena_done();
}

static void TestRepeatPanics(TestingT *t) {
    arena_start();
    CHECK(call_panics(call_repeat, (PanicCall){voids(0), -1, 0, INIL()}));
    CHECK(call_panics(call_repeat, (PanicCall){voids(3), BURROW_INT_MAX, 0, INIL()}));
    CHECK(call_panics(call_repeat,
                      (PanicCall){voids(2), 1 + BURROW_INT_MAX / 2, 0, INIL()}));
    arena_done();
}

/* A clone of an empty slice of s does not point into s. */
static void TestIssue68488(TestingT *t) {
    arena_start();
    Slice s = slice_make(A, TYPE_INT, 3, 3);
    Slice clone = slices_clone(A, slice_sub(s, 1, 1));
    const Int *p = clone.p;
    CHECK(p != &ip(s)[0] && p != &ip(s)[1] && p != &ip(s)[2]);
    arena_done();
}

static void empty_seq_run(void *env, IterYield yield) {
    (void)env;
    (void)yield;
}

static bool eq_unreachable(void *env, const void *a, const void *b) {
    (void)env;
    (void)a;
    (void)b;
    panic_str(BURROW_S("unreachable"));
    return false;
}

static void TestNilness(TestingT *t) {
    arena_start();
    Slice empty = INONE(), nil = INIL();
    IterSeq empty_seq = BURROW_FN(IterSeq, empty_seq_run, NULL);
    SlicesPredFunc truth = predf(pred_true);
    SlicesEqualFunc eq = eqf(eq_unreachable);

    CHECK(slice_is_nil(slices_append_seq(A, nil, empty_seq)));
    CHECK(!slice_is_nil(slices_append_seq(A, empty, empty_seq)));

    CHECK(slice_is_nil(slices_insert(A, nil, 0, NULL, 0)));
    CHECK(!slice_is_nil(slices_insert(A, empty, 0, NULL, 0)));

    CHECK(slice_is_nil(slices_delete(nil, 0, 0)));
    CHECK(!slice_is_nil(slices_delete(empty, 0, 0)));
    CHECK(!slice_is_nil(slices_delete(IS(1), 0, 1)));

    CHECK(slice_is_nil(slices_delete_func(nil, truth)));
    CHECK(!slice_is_nil(slices_delete_func(empty, truth)));
    CHECK(!slice_is_nil(slices_delete_func(IS(1), truth)));

    CHECK(slice_is_nil(slices_replace(A, nil, 0, 0, NULL, 0)));
    CHECK(!slice_is_nil(slices_replace(A, empty, 0, 0, NULL, 0)));
    CHECK(!slice_is_nil(slices_replace(A, IS(1), 0, 1, NULL, 0)));

    CHECK(slice_is_nil(slices_clone(A, nil)));
    CHECK(!slice_is_nil(slices_clone(A, empty)));

    CHECK(slice_is_nil(slices_compact(nil)));
    CHECK(!slice_is_nil(slices_compact(empty)));

    CHECK(slice_is_nil(slices_compact_func(nil, eq)));
    CHECK(!slice_is_nil(slices_compact_func(empty, eq)));

    CHECK(slice_is_nil(slices_grow(A, nil, 0)));
    CHECK(!slice_is_nil(slices_grow(A, empty, 0)));

    CHECK(slice_is_nil(slices_clip(nil)));
    CHECK(!slice_is_nil(slices_clip(empty)));
    CHECK(!slice_is_nil(slices_clip(slice_sub3(IS(1), 0, 0, 0))));

    /* Concat answers nil exactly when the result is empty. */
    Slice ss[] = {nil, empty, nil, empty};
    Slice ss2[] = {empty, empty, nil, empty};
    CHECK(slice_is_nil(slices_concat(A, ss, 4)));
    CHECK(slice_is_nil(slices_concat(A, ss2, 4)));
    CHECK(slice_is_nil(slices_concat(A, NULL, 0)));

    /* Repeat never answers nil. */
    CHECK(!slice_is_nil(slices_repeat(A, nil, 0)));
    CHECK(!slice_is_nil(slices_repeat(A, empty, 0)));
    CHECK(!slice_is_nil(slices_repeat(A, nil, 2)));
    CHECK(!slice_is_nil(slices_repeat(A, empty, 2)));

    CHECK(slice_is_nil(slices_collect(A, TYPE_INT, empty_seq)));
    CHECK(slice_is_nil(slices_sorted(A, TYPE_INT, empty_seq)));
    CHECK(slice_is_nil(slices_sorted_func(A, TYPE_INT, empty_seq, cmpf(cmp_int))));
    CHECK(
        slice_is_nil(slices_sorted_stable_func(A, TYPE_INT, empty_seq, cmpf(cmp_int))));
    arena_done();
}

/* sort_test.go */

static const Int sort_ints_data[] = {74, 59, 238, -784, 9845,     959, 905,
                                     0,  0,  42,  7586, -5467984, 7586};
static const char *const sort_strs[] = {"",    "Hello", "foo",      "bar",
                                        "foo", "f00",   "%*&^*&^&", "***"};

static Slice float64s(void) {
    double inf = (double)INFINITY;
    return FS(74.3, 59.0, inf, 238.2, -784.0, 2.3, -inf, 9845.768, -959.7485, 905, 7.8,
              7.8, 74.3, 59.0, inf, 238.2, -784.0, 2.3);
}

static void TestSortIntSlice(TestingT *t) {
    arena_start();
    Slice data = ints_of(sort_ints_data, NELEM(sort_ints_data));
    slices_sort(data);
    CHECK(slices_is_sorted(data));
    arena_done();
}

static void TestSortFuncIntSlice(TestingT *t) {
    arena_start();
    Slice data = ints_of(sort_ints_data, NELEM(sort_ints_data));
    slices_sort_func(data, cmpf(sub_int));
    CHECK(slices_is_sorted(data));
    arena_done();
}

static void TestSortFloat64Slice(TestingT *t) {
    arena_start();
    Slice data = float64s();
    slices_sort(data);
    CHECK(slices_is_sorted(data));
    arena_done();
}

static void TestSortStringSlice(TestingT *t) {
    arena_start();
    Slice data = strs_of(sort_strs, NELEM(sort_strs));
    slices_sort(data);
    CHECK(slices_is_sorted(data));
    arena_done();
}

/* Every ordered width, and a NaN, which sorts first. */
static void TestSortWidths(TestingT *t) {
    int8_t i8[] = {3, -1, 2};
    uint16_t u16[] = {300, 1, 65535};
    int32_t i32[] = {-7, 7, 0};
    uint64_t u64[] = {UINT64_MAX, 0, 1};
    float f32[] = {1.5f, (float)NAN, -2.0f};
    slices_sort(slice_from(i8, 3, 3, TYPE_INT8));
    slices_sort(slice_from(u16, 3, 3, TYPE_UINT16));
    slices_sort(slice_from(i32, 3, 3, TYPE_INT32));
    slices_sort(slice_from(u64, 3, 3, TYPE_UINT64));
    slices_sort(slice_from(f32, 3, 3, TYPE_FLOAT32));
    CHECK(i8[0] == -1 && i8[2] == 3);
    CHECK(u16[0] == 1 && u16[2] == 65535);
    CHECK(i32[0] == -7 && i32[2] == 7);
    CHECK(u64[0] == 0 && u64[2] == UINT64_MAX);
    CHECK(f32[0] != f32[0] && f32[1] == -2.0f && f32[2] == 1.5f);
    /* A type with no order panics, where Go would not compile. */
    IntPtr p[1] = {NULL};
    CHECK(call_panics(call_sort, (PanicCall){ptrs(p, 1, 1), 0, 0, INIL()}));
}

static void TestSortLargeRandom(TestingT *t) {
    arena_start();
    Int n = 1000000;
    if (testing_short())
        n /= 100;
    Slice data = slice_make(A, TYPE_INT, n, n);
    for (Int i = 0; i < n; i++)
        ip(data)[i] = rng_intn(100);
    CHECK(!slices_is_sorted(data));
    slices_sort(data);
    CHECK(slices_is_sorted(data));
    arena_done();
}

typedef struct IntPair {
    Int a, b;
} IntPair;

static const Type int_pair_type = {
    {(const Byte *)"intPair", 7},
    {NULL, 0},
    KIND_STRUCT,
    (uint32_t)sizeof(IntPair),
    (uint16_t)_Alignof(IntPair),
    0,
    0,
    NULL,
    NULL,
    NULL,
    NULL,
    0,
    0,
    NULL,
};

/* Pairs compare on a only. */
static int int_pair_cmp(void *env, const void *x, const void *y) {
    (void)env;
    return (int)(((const IntPair *)x)->a - ((const IntPair *)y)->a);
}

static void init_b(Slice d) {
    IntPair *p = d.p;
    for (Int i = 0; i < d.len; i++)
        p[i].b = i;
}

/* Whether elements with equal a kept the order b records, or the reverse of
 * it. */
static bool in_order(Slice d, bool reversed) {
    const IntPair *p = d.p;
    Int last_a = -1, last_b = 0;
    for (Int i = 0; i < d.len; i++) {
        if (last_a != p[i].a) {
            last_a = p[i].a;
            last_b = p[i].b;
            continue;
        }
        if (!reversed ? p[i].b <= last_b : p[i].b >= last_b)
            return false;
        last_b = p[i].b;
    }
    return true;
}

static void TestStability(TestingT *t) {
    arena_start();
    Int n = 100000, m = 1000;
    if (testing_short()) {
        n = 1000;
        m = 100;
    }
    Slice data = slice_make(A, &int_pair_type, n, n);
    IntPair *p = data.p;
    for (Int i = 0; i < n; i++)
        p[i].a = rng_intn(m);
    CHECK(!slices_is_sorted_func(data, cmpf(int_pair_cmp)));
    init_b(data);
    slices_sort_stable_func(data, cmpf(int_pair_cmp));
    CHECK(slices_is_sorted_func(data, cmpf(int_pair_cmp)));
    CHECK(in_order(data, false));

    /* Already sorted. */
    init_b(data);
    slices_sort_stable_func(data, cmpf(int_pair_cmp));
    CHECK(slices_is_sorted_func(data, cmpf(int_pair_cmp)));
    CHECK(in_order(data, false));

    /* Sorted reversed. */
    for (Int i = 0; i < n; i++)
        p[i].a = n - i;
    init_b(data);
    slices_sort_stable_func(data, cmpf(int_pair_cmp));
    CHECK(slices_is_sorted_func(data, cmpf(int_pair_cmp)));
    CHECK(in_order(data, false));
    arena_done();
}

typedef struct SVal {
    Int a;
    const char *b;
} SVal;

static int cmp_s(void *env, const void *x, const void *y) {
    (void)env;
    return cmp_compare(((const SVal *)x)->a, ((const SVal *)y)->a);
}

static const Type s_type = {
    {(const Byte *)"S", 1},
    {NULL, 0},
    KIND_STRUCT,
    (uint32_t)sizeof(SVal),
    (uint16_t)_Alignof(SVal),
    0,
    0,
    NULL,
    NULL,
    NULL,
    NULL,
    0,
    0,
    NULL,
};

static void TestMinMax(TestingT *t) {
    arena_start();
    struct {
        Slice data;
        Int want_min, want_max;
    } tests[] = {
        {IS(7), 7, 7},       {IS(1, 2), 1, 2},    {IS(2, 1), 1, 2},
        {IS(1, 2, 3), 1, 3}, {IS(3, 2, 1), 1, 3}, {IS(2, 1, 3), 1, 3},
        {IS(2, 2, 3), 2, 3}, {IS(3, 2, 3), 2, 3}, {IS(0, 2, -9), -9, 2},
    };
    for (Int k = 0; k < NELEM(tests); k++) {
        Slice d = tests[k].data;
        CHECK_INT_EQ(*(const Int *)slices_min(d), tests[k].want_min);
        CHECK_INT_EQ(*(const Int *)slices_min_func(d, cmpf(sub_int)),
                     tests[k].want_min);
        CHECK_INT_EQ(*(const Int *)slices_max(d), tests[k].want_max);
        CHECK_INT_EQ(*(const Int *)slices_max_func(d, cmpf(sub_int)),
                     tests[k].want_max);
    }
    SVal svals[] = {{1, "a"}, {2, "a"}, {1, "b"}, {2, "b"}};
    Slice sv = slice_from(svals, 4, 4, &s_type);
    const SVal *got_min = slices_min_func(sv, cmpf(cmp_s));
    const SVal *got_max = slices_max_func(sv, cmpf(cmp_s));
    CHECK(got_min->a == 1 && strcmp(got_min->b, "a") == 0);
    CHECK(got_max->a == 2 && strcmp(got_max->b, "a") == 0);
    arena_done();
}

static void TestMinMaxNaNs(TestingT *t) {
    arena_start();
    Slice fs = FS(1.0, 999.9, 3.14, -400.4, -5.14);
    CHECK(*(const double *)slices_min(fs) == -400.4);
    CHECK(*(const double *)slices_max(fs) == 999.9);
    /* Whichever element is a NaN, both answer a NaN. */
    for (Int i = 0; i < fs.len; i++) {
        Slice tf = slices_clone(A, fs);
        ((double *)tf.p)[i] = (double)NAN;
        double fmin = *(const double *)slices_min(tf);
        double fmax = *(const double *)slices_max(tf);
        CHECK(fmin != fmin);
        CHECK(fmax != fmax);
    }
    /* -0.0 is smaller than 0.0, as with Go's min and max. */
    double z[] = {0.0, copysign(0, -1)};
    CHECK(signbit(*(const double *)slices_min(slice_from(z, 2, 2, TYPE_FLOAT64))));
    CHECK(!signbit(*(const double *)slices_max(slice_from(z, 2, 2, TYPE_FLOAT64))));
    arena_done();
}

static void TestMinMaxPanics(TestingT *t) {
    arena_start();
    PanicCall c = {INONE(), 0, 0, INIL()};
    CHECK(call_panics(call_min, c));
    CHECK(call_panics(call_max, c));
    CHECK(call_panics(call_min_func, c));
    CHECK(call_panics(call_max_func, c));
    arena_done();
}

static void TestBinarySearch(TestingT *t) {
    arena_start();
    Slice str1 = SS("foo");
    Slice str2 = SS("ab", "ca");
    Slice str3 = SS("mo", "qo", "vo");
    Slice str4 = SS("ab", "ad", "ca", "xy");
    Slice repeats = SS("ba", "ca", "da", "da", "da", "ka", "ma", "ma", "ta");
    Slice same = SS("xx", "xx", "xx");
    Slice none = strs_of(NULL, 0);
    struct {
        Slice data;
        const char *target;
        Int want_pos;
        bool want_found;
    } tests[] = {
        {none, "foo", 0, false},   {none, "", 0, false},

        {str1, "foo", 0, true},    {str1, "bar", 0, false},   {str1, "zx", 1, false},

        {str2, "aa", 0, false},    {str2, "ab", 0, true},     {str2, "ad", 1, false},
        {str2, "ca", 1, true},     {str2, "ra", 2, false},

        {str3, "bb", 0, false},    {str3, "mo", 0, true},     {str3, "nb", 1, false},
        {str3, "qo", 1, true},     {str3, "tr", 2, false},    {str3, "vo", 2, true},
        {str3, "xr", 3, false},

        {str4, "aa", 0, false},    {str4, "ab", 0, true},     {str4, "ac", 1, false},
        {str4, "ad", 1, true},     {str4, "ax", 2, false},    {str4, "ca", 2, true},
        {str4, "cc", 3, false},    {str4, "dd", 3, false},    {str4, "xy", 3, true},
        {str4, "zz", 4, false},

        {repeats, "da", 2, true},  {repeats, "db", 5, false}, {repeats, "ma", 6, true},
        {repeats, "mb", 8, false},

        {same, "xx", 0, true},     {same, "ab", 0, false},    {same, "zz", 3, false},
    };
    for (Int k = 0; k < NELEM(tests); k++) {
        Str target = str_from_cstr(tests[k].target);
        bool found;
        Int pos = slices_binary_search(tests[k].data, &target, &found);
        if (pos != tests[k].want_pos || found != tests[k].want_found)
            testing_t_errorf_v(t, "BinarySearch %q got (%d, %t)", tests[k].target, pos,
                               found);
        pos = slices_binary_search_func(tests[k].data, &target, cmpf(cmp_str), &found);
        if (pos != tests[k].want_pos || found != tests[k].want_found)
            testing_t_errorf_v(t, "BinarySearchFunc %q got (%d, %t)", tests[k].target,
                               pos, found);
    }
    arena_done();
}

static void TestBinarySearchInts(TestingT *t) {
    arena_start();
    Slice data = IS(20, 30, 40, 50, 60, 70, 80, 90);
    struct {
        Int target, want_pos;
        bool want_found;
    } tests[] = {{20, 0, true}, {23, 1, false}, {43, 3, false}, {80, 6, true}};
    for (Int k = 0; k < NELEM(tests); k++) {
        bool found;
        Int pos = slices_binary_search(data, &tests[k].target, &found);
        CHECK(pos == tests[k].want_pos && found == tests[k].want_found);
        pos = slices_binary_search_func(data, &tests[k].target, cmpf(sub_int), &found);
        CHECK(pos == tests[k].want_pos && found == tests[k].want_found);
    }
    /* found may be left out. */
    CHECK_INT_EQ(slices_binary_search(data, &tests[1].target, NULL), 1);
    arena_done();
}

static void TestBinarySearchFloats(TestingT *t) {
    arena_start();
    double nan = (double)NAN, inf = (double)INFINITY;
    Slice data = FS(nan, -0.25, 0.0, 1.4);
    struct {
        double target;
        Int want_pos;
        bool want_found;
    } tests[] = {{nan, 0, true}, {-inf, 1, false}, {-0.25, 1, true},
                 {0.0, 2, true}, {1.4, 3, true},   {1.5, 4, false}};
    for (Int k = 0; k < NELEM(tests); k++) {
        bool found;
        Int pos = slices_binary_search(data, &tests[k].target, &found);
        if (pos != tests[k].want_pos || found != tests[k].want_found)
            testing_t_errorf_v(t, "BinarySearch case %d got (%d, %t)", k, pos, found);
    }
    arena_done();
}

/* cmp(a int, b string) is strings.Compare(strconv.Itoa(a), b). */
static int cmp_itoa(void *env, const void *a, const void *b) {
    (void)env;
    char buf[32];
    int n = snprintf(buf, sizeof buf, "%lld", (long long)*(const Int *)a);
    return str_cmp(str_from_bytes((const Byte *)buf, n), *(const Str *)b);
}

static void TestBinarySearchFunc(TestingT *t) {
    arena_start();
    Slice data = IS(1, 10, 11, 2); /* sorted lexicographically */
    Str two = BURROW_S("2");
    bool found;
    Int pos = slices_binary_search_func(data, &two, cmpf(cmp_itoa), &found);
    CHECK(pos == 3 && found);
    arena_done();
}

/* iter_test.go */

typedef struct Walk {
    Int ei, ev, step, cnt;
    bool bad;
} Walk;

static bool walk2(void *env, const void *k, const void *v) {
    Walk *w = env;
    if (*(const Int *)k != w->ei || *(const Int *)v != w->ev)
        w->bad = true;
    w->ei += w->step;
    w->ev += w->step;
    w->cnt++;
    return true;
}

static bool walk1(void *env, const void *v) {
    Walk *w = env;
    if (*(const Int *)v != w->ev)
        w->bad = true;
    w->ev++;
    w->cnt++;
    return true;
}

static Slice iota_ints(Int size) {
    Slice s = INIL();
    for (Int i = 0; i < size; i++)
        s = slice_append(A, s, &i, 1);
    return s;
}

static void TestAll(TestingT *t) {
    arena_start();
    for (Int size = 0; size < 10; size++) {
        Slice s = iota_ints(size);
        Walk w = {0, 0, 1, 0, false};
        IterSeq2 seq = slices_all(&s);
        seq.f(seq.env, BURROW_FN(IterYield2, walk2, &w));
        CHECK(!w.bad);
        CHECK_INT_EQ(w.cnt, size);
    }
    arena_done();
}

static void TestBackward(TestingT *t) {
    arena_start();
    for (Int size = 0; size < 10; size++) {
        Slice s = iota_ints(size);
        Walk w = {size - 1, size - 1, -1, 0, false};
        IterSeq2 seq = slices_backward(&s);
        seq.f(seq.env, BURROW_FN(IterYield2, walk2, &w));
        CHECK(!w.bad);
        CHECK_INT_EQ(w.cnt, size);
    }
    arena_done();
}

static void TestValues(TestingT *t) {
    arena_start();
    for (Int size = 0; size < 10; size++) {
        Slice s = iota_ints(size);
        Walk w = {0, 0, 1, 0, false};
        IterSeq seq = slices_values(&s);
        seq.f(seq.env, BURROW_FN(IterYield, walk1, &w));
        CHECK(!w.bad);
        CHECK_INT_EQ(w.cnt, size);
    }
    arena_done();
}

static void test_seq_run(void *env, IterYield yield) {
    (void)env;
    for (Int i = 0; i < 10; i += 2) {
        if (!yield.f(yield.env, &i))
            return;
    }
}

static const IterSeq test_seq = {test_seq_run, NULL};

static void TestAppendSeq(TestingT *t) {
    arena_start();
    Slice s = slices_append_seq(A, IS(1, 2), test_seq);
    CHECK(slices_equal(s, IS(1, 2, 0, 2, 4, 6, 8)));
    arena_done();
}

static void TestCollect(TestingT *t) {
    arena_start();
    CHECK(slices_equal(slices_collect(A, TYPE_INT, test_seq), IS(0, 2, 4, 6, 8)));
    arena_done();
}

static Int iter_tests(Slice *t) {
    t[0] = slice_nil(TYPE_STRING);
    t[1] = SS("a");
    t[2] = SS("a", "b");
    t[3] = SS("b", "a");
    t[4] = strs_of(sort_strs, NELEM(sort_strs));
    return 5;
}

static void TestValuesAppendSeq(TestingT *t) {
    arena_start();
    Slice tests[5];
    Int n = iter_tests(tests);
    for (Int i = 0; i < n; i++) {
        for (Int j = 0; j < n; j++) {
            Slice prefix = slices_clone(A, tests[i]);
            Slice got = slices_append_seq(A, prefix, slices_values(&tests[j]));
            Slice want =
                slice_append(A, slices_clone(A, tests[i]), tests[j].p, tests[j].len);
            if (!slices_equal(got, want))
                testing_t_errorf_v(t, "AppendSeq case %d, %d wrong", i, j);
        }
    }
    arena_done();
}

static void TestValuesCollect(TestingT *t) {
    arena_start();
    Slice tests[5];
    for (Int i = 0, n = iter_tests(tests); i < n; i++)
        CHECK(slices_equal(slices_collect(A, TYPE_STRING, slices_values(&tests[i])),
                           tests[i]));
    arena_done();
}

static void TestSorted(TestingT *t) {
    arena_start();
    Slice in = ints_of(sort_ints_data, NELEM(sort_ints_data));
    CHECK(slices_is_sorted(slices_sorted(A, TYPE_INT, slices_values(&in))));
    arena_done();
}

static void TestSortedFunc(TestingT *t) {
    arena_start();
    Slice in = ints_of(sort_ints_data, NELEM(sort_ints_data));
    CHECK(slices_is_sorted(
        slices_sorted_func(A, TYPE_INT, slices_values(&in), cmpf(sub_int))));
    arena_done();
}

/* Go's iterVal: the values of a Seq2 as a Seq. */
typedef struct IterVal {
    IterSeq2 seq;
    IterYield yield;
} IterVal;

static bool iter_val_yield(void *env, const void *k, const void *v) {
    (void)k;
    IterVal *iv = env;
    return iv->yield.f(iv->yield.env, v);
}

static void iter_val_run(void *env, IterYield yield) {
    IterVal *iv = env;
    iv->yield = yield;
    iv->seq.f(iv->seq.env, BURROW_FN(IterYield2, iter_val_yield, iv));
}

static void TestSortedStableFunc(TestingT *t) {
    arena_start();
    Int n = 1000, m = 100;
    Slice data = slice_make(A, &int_pair_type, n, n);
    IntPair *p = data.p;
    for (Int i = 0; i < n; i++)
        p[i].a = rng_intn(m);
    init_b(data);

    Slice s = slices_sorted_stable_func(A, &int_pair_type, slices_values(&data),
                                        cmpf(int_pair_cmp));
    CHECK(slices_is_sorted_func(s, cmpf(int_pair_cmp)));
    CHECK(in_order(s, false));

    IterVal iv = {slices_backward(&data), {NULL, NULL}};
    s = slices_sorted_stable_func(
        A, &int_pair_type, BURROW_FN(IterSeq, iter_val_run, &iv), cmpf(int_pair_cmp));
    CHECK(slices_is_sorted_func(s, cmpf(int_pair_cmp)));
    CHECK(in_order(s, true));
    arena_done();
}

/* Collects what a Chunk sequence yields, up to limit chunks. */
typedef struct Chunks {
    Slice got[8];
    Int n, limit;
} Chunks;

static bool chunks_yield(void *env, const void *v) {
    Chunks *c = env;
    if (c->n == c->limit)
        return false;
    c->got[c->n++] = *(const Slice *)v;
    return true;
}

static void TestChunk(TestingT *t) {
    arena_start();
    struct {
        Slice s;
        Int n;
        Slice chunks[3];
        Int nchunks;
    } tests[] = {
        {INIL(), 1, {{0}}, 0},
        {INONE(), 1, {{0}}, 0},
        {IS(1, 2), 3, {IS(1, 2)}, 1},
        {IS(1, 2), 2, {IS(1, 2)}, 1},
        {IS(1, 2, 3, 4), 2, {IS(1, 2), IS(3, 4)}, 2},
        {IS(1, 2, 3, 4, 5), 2, {IS(1, 2), IS(3, 4), IS(5)}, 3},
    };
    for (Int k = 0; k < NELEM(tests); k++) {
        Chunks c = {{{0}}, 0, 8};
        IterSeq seq = slices_chunk(A, tests[k].s, tests[k].n);
        seq.f(seq.env, BURROW_FN(IterYield, chunks_yield, &c));
        slices_seq_free(A, seq);
        bool ok = c.n == tests[k].nchunks;
        for (Int i = 0; ok && i < c.n; i++)
            ok = slices_equal(c.got[i], tests[k].chunks[i]);
        if (!ok)
            testing_t_errorf_v(t, "Chunk case %d wrong", k);
        if (c.n == 0)
            continue;
        /* Appending to the first chunk does not clobber the next one. */
        Slice s = slices_clone(A, tests[k].s);
        Int minus = -1;
        c.got[0] = slice_append(A, c.got[0], &minus, 1);
        CHECK(slices_equal(s, tests[k].s));
    }
    arena_done();
}

static void TestChunkPanics(TestingT *t) {
    arena_start();
    CHECK(call_panics(call_chunk, (PanicCall){voids(0), 0, 0, INIL()}));
    arena_done();
}

static void TestChunkRange(TestingT *t) {
    arena_start();
    Chunks c = {{{0}}, 0, 2};
    IterSeq seq = slices_chunk(A, IS(1, 2, 3, 4, -100), 2);
    seq.f(seq.env, BURROW_FN(IterYield, chunks_yield, &c));
    slices_seq_free(A, seq);
    CHECK_INT_EQ(c.n, 2);
    CHECK(slices_equal(c.got[0], IS(1, 2)));
    CHECK(slices_equal(c.got[1], IS(3, 4)));
    arena_done();
}

#define TESTS(X)                                                                       \
    X(TestEqual)                                                                       \
    X(TestEqualFunc)                                                                   \
    X(TestCompare)                                                                     \
    X(TestCompareFunc)                                                                 \
    X(TestIndex)                                                                       \
    X(TestIndexFunc)                                                                   \
    X(TestInsert)                                                                      \
    X(TestInsertOverlap)                                                               \
    X(TestInsertPanics)                                                                \
    X(TestDelete)                                                                      \
    X(TestDeleteFunc)                                                                  \
    X(TestDeletePanics)                                                                \
    X(TestDeleteClearTail)                                                             \
    X(TestDeleteFuncClearTail)                                                         \
    X(TestClone)                                                                       \
    X(TestCompact)                                                                     \
    X(TestCompactFunc)                                                                 \
    X(TestCompactClearTail)                                                            \
    X(TestCompactFuncClearTail)                                                        \
    X(TestGrow)                                                                        \
    X(TestClip)                                                                        \
    X(TestReverse)                                                                     \
    X(TestReplace)                                                                     \
    X(TestReplacePanics)                                                               \
    X(TestReplaceGrow)                                                                 \
    X(TestReplaceClearTail)                                                            \
    X(TestReplaceOverlap)                                                              \
    X(TestReplaceEndClearTail)                                                         \
    X(TestInsertGrowthRate)                                                            \
    X(TestReplaceGrowthRate)                                                           \
    X(TestConcat)                                                                      \
    X(TestConcatTooLarge)                                                              \
    X(TestRepeat)                                                                      \
    X(TestRepeatPanics)                                                                \
    X(TestIssue68488)                                                                  \
    X(TestNilness)                                                                     \
    X(TestSortIntSlice)                                                                \
    X(TestSortFuncIntSlice)                                                            \
    X(TestSortFloat64Slice)                                                            \
    X(TestSortStringSlice)                                                             \
    X(TestSortWidths)                                                                  \
    X(TestSortLargeRandom)                                                             \
    X(TestStability)                                                                   \
    X(TestMinMax)                                                                      \
    X(TestMinMaxNaNs)                                                                  \
    X(TestMinMaxPanics)                                                                \
    X(TestBinarySearch)                                                                \
    X(TestBinarySearchInts)                                                            \
    X(TestBinarySearchFloats)                                                          \
    X(TestBinarySearchFunc)                                                            \
    X(TestAll)                                                                         \
    X(TestBackward)                                                                    \
    X(TestValues)                                                                      \
    X(TestAppendSeq)                                                                   \
    X(TestCollect)                                                                     \
    X(TestValuesAppendSeq)                                                             \
    X(TestValuesCollect)                                                               \
    X(TestSorted)                                                                      \
    X(TestSortedFunc)                                                                  \
    X(TestSortedStableFunc)                                                            \
    X(TestChunk)                                                                       \
    X(TestChunkPanics)                                                                 \
    X(TestChunkRange)

TESTING_MAIN(TESTS)
