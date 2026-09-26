/* unique's tests, from Go's handle_test.go and clone_test.go. The ones that
 * watch the collector reclaim values have nothing to watch here, since the
 * canonical copies are never reclaimed, and are left out.
 *
 * Copyright 2024 The Go Authors. All rights reserved.
 * Copyright 2026 The burrow Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style licence that can be found
 * in the LICENSE file. */

#include "burrow/unique.h"

#include "burrow/declare.h"
#include "burrow/func.h"
#include "burrow/mem/arena.h"
#include "burrow/mem/heap.h"
#include "burrow/proc.h"
#include "burrow/strconv.h"
#include "burrow/sync.h"
#include "burrow/testing.h"

#include "check.h"

#include <string.h>

BURROW_ARRAY_TYPE(TestIntArray, Int, 4);
BURROW_ARRAY_TYPE(TestStringArray, Str, 3);

#define TEST_STRING_STRUCT_FIELDS(F, T) F(T, Str, a, "")
BURROW_STRUCT(TestStringStruct, TEST_STRING_STRUCT_FIELDS);

BURROW_ARRAY_TYPE(TestStringStructArray, TestStringStruct, 2);

#define TEST_STRING_STRUCT_ARRAY_STRUCT_FIELDS(F, T) F(T, TestStringStructArray, s, "")
BURROW_STRUCT(TestStringStructArrayStruct, TEST_STRING_STRUCT_ARRAY_STRUCT_FIELDS);

#define TEST_STRUCT_FIELDS(F, T)                                                       \
    F(T, double, z, "")                                                                \
    F(T, Str, b, "")
BURROW_STRUCT(TestStruct, TEST_STRUCT_FIELDS);

BURROW_ARRAY_TYPE(Intx6, Int, 6);

#define TEST_NESTED_HANDLE_FIELDS(F, T)                                                \
    F(T, UniqueHandle, next, "")                                                       \
    F(T, Intx6, arr, "")
BURROW_STRUCT(TestNestedHandle, TEST_NESTED_HANDLE_FIELDS);

/* Go's testHandle: two handles for the same value are the same handle, and
 * the value comes back equal to what went in. */
static void check_handle(TestingT *t, const Type *typ, const void *value) {
    UniqueHandle v0 = unique_make(typ, value);
    UniqueHandle v1 = unique_make(typ, value);
    if (!type_equal(typ, unique_handle_value(v0), unique_handle_value(v1)))
        testing_t_errorf_v(t, "%s: v0.Value != v1.Value", typ->name);
    if (!type_equal(typ, unique_handle_value(v0), value))
        testing_t_errorf_v(t, "%s: v0.Value not the value", typ->name);
    if (!unique_handle_eq(v0, v1))
        testing_t_errorf_v(t, "%s: v0 != v1", typ->name);
}

static void TestHandle(TestingT *t) {
    Str foo = BURROW_S("foo"), bar = BURROW_S("bar"), empty = BURROW_S("");
    check_handle(t, TYPE_STRING, &foo);
    check_handle(t, TYPE_STRING, &bar);
    check_handle(t, TYPE_STRING, &empty);
    TestIntArray ia = {{7, 77, 777, 7777}};
    check_handle(t, TYPE_OF(TestIntArray), &ia);
    TestStringArray sa = {{BURROW_S("a"), BURROW_S("b"), BURROW_S("c")}};
    check_handle(t, TYPE_OF(TestStringArray), &sa);
    TestStringStruct ss = {BURROW_S("x")};
    check_handle(t, TYPE_OF(TestStringStruct), &ss);
    TestStringStructArrayStruct sas = {{{{BURROW_S("y")}, {BURROW_S("z")}}}};
    check_handle(t, TYPE_OF(TestStringStructArrayStruct), &sas);
    TestStruct ts = {0.5, BURROW_S("184")};
    check_handle(t, TYPE_OF(TestStruct), &ts);
}

/* Different values get different handles. */
static void TestHandleDistinct(TestingT *t) {
    Str a = BURROW_S("a"), b = BURROW_S("b");
    CHECK(!unique_handle_eq(UNIQUE_MAKE(Str, &a), UNIQUE_MAKE(Str, &b)));
    TestStruct x = {0.5, BURROW_S("184")}, y = {0.5, BURROW_S("185")};
    CHECK(!unique_handle_eq(UNIQUE_MAKE(TestStruct, &x), UNIQUE_MAKE(TestStruct, &y)));
    /* The same bytes as a different type are a different value. */
    TestStringStruct s = {BURROW_S("a")};
    CHECK(UNIQUE_MAKE(TestStringStruct, &s).value != UNIQUE_MAKE(Str, &a).value);
    /* Go's == on floats: 0.0 and -0.0 are equal, so they share a handle. */
    double pz = 0.0, nz = -0.0;
    CHECK(unique_handle_eq(UNIQUE_MAKE(double, &pz), UNIQUE_MAKE(double, &nz)));
    UniqueHandle zero = {0};
    CHECK(!unique_handle_eq(zero, UNIQUE_MAKE(Str, &a)));
}

/* A type of size zero, which Go gives one shared address. */
static const Type zero_size_type = {{(const Byte *)"testZeroSize", 12},
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
                                    NULL};

static void TestHandleZeroSize(TestingT *t) {
    Byte b = 0;
    UniqueHandle h = unique_make(&zero_size_type, &b);
    CHECK(h.value != NULL);
    CHECK(unique_handle_eq(h, unique_make(&zero_size_type, &b)));
}

/* Go's TestMakeClonesStrings, the other way round: the handle must not keep
 * the caller's bytes, so changing them afterwards changes nothing. */
static void TestMakeClonesStrings(TestingT *t) {
    char buf[] = "abcdefghijklmnopqrstuvwxyz";
    Str s = str_from_bytes((const Byte *)buf, 26);
    UniqueHandle h = UNIQUE_MAKE(Str, &s);
    CHECK(UNIQUE_VALUE(Str, h).p != s.p);
    memset(buf, 'x', 26);
    CHECK(str_eq(UNIQUE_VALUE(Str, h), BURROW_S("abcdefghijklmnopqrstuvwxyz")));

    char sbuf[] = "one";
    char abuf[] = "two";
    TestStringArray sa = {{str_from_bytes((const Byte *)sbuf, 3), BURROW_S("mid"),
                           str_from_bytes((const Byte *)abuf, 3)}};
    UniqueHandle ha = UNIQUE_MAKE(TestStringArray, &sa);
    sbuf[0] = 'O';
    abuf[0] = 'T';
    const TestStringArray *got = unique_handle_value(ha);
    CHECK(str_eq(got->v[0], BURROW_S("one")));
    CHECK(str_eq(got->v[2], BURROW_S("two")));
}

static void TestHandleUnsafeString(TestingT *t) {
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    UniqueHandle handles[1024];
    Byte buf[8];
    for (Int i = 0; i < 1024; i++) {
        Str s = strconv_itoa(a, i);
        memcpy(buf, s.p, (size_t)s.len);
        Str sbuf = str_from_bytes(buf, s.len);
        handles[i] = UNIQUE_MAKE(Str, &sbuf);
    }
    for (Int i = 0; i < 1024; i++) {
        Str s = strconv_itoa(a, i);
        UniqueHandle h = UNIQUE_MAKE(Str, &s);
        if (!str_eq(UNIQUE_VALUE(Str, handles[i]), UNIQUE_VALUE(Str, h)))
            testing_t_fatalf_v(t, "unsafe string improperly retained internally");
        CHECK(unique_handle_eq(handles[i], h));
    }
    arena_free(&ar);
}

static TestNestedHandle nest_handle(TestNestedHandle n) {
    TestNestedHandle r = {UNIQUE_MAKE(TestNestedHandle, &n), n.arr};
    return r;
}

static bool nested_eq(TestNestedHandle a, TestNestedHandle b) {
    return type_equal(TYPE_OF(TestNestedHandle), &a, &b);
}

static void TestNestedHandleChain(TestingT *t) {
    TestNestedHandle n0 = {{0}, {{1, 2, 3, 4, 5, 6}}};
    TestNestedHandle n1 = nest_handle(n0);
    TestNestedHandle n2 = nest_handle(n1);
    TestNestedHandle n3 = nest_handle(n2);
    CHECK(nested_eq(UNIQUE_VALUE(TestNestedHandle, n3.next), n2));
    CHECK(nested_eq(UNIQUE_VALUE(TestNestedHandle, n2.next), n1));
    CHECK(nested_eq(UNIQUE_VALUE(TestNestedHandle, n1.next), n0));
    CHECK(unique_handle_eq(nest_handle(n2).next, n3.next));
}

/* A handle for a value that already has one allocates nothing. The first
 * part of Go's TestMakeAllocs that has a meaning here. */
static void TestMakeAllocs(TestingT *t) {
    Str s = BURROW_S("this string is statically allocated");
    UniqueHandle want = UNIQUE_MAKE(Str, &s);
    uint64_t before = 0, after = 0, bytes = 0;
    burrow__heap_count(true);
    burrow__heap_counts(&before, &bytes);
    for (int i = 0; i < 100; i++) {
        Byte b[16] = {0};
        b[8] = 'a';
        Str st = str_from_bytes(b, 16);
        (void)UNIQUE_MAKE(Str, &st);
        CHECK(unique_handle_eq(UNIQUE_MAKE(Str, &s), want));
    }
    burrow__heap_counts(&after, &bytes);
    burrow__heap_count(false);
    /* One allocation at most, for the first time the stack string is seen. */
    if (after - before > 1)
        testing_t_errorf_v(t, "got %v allocs, want at most 1", after - before);
}

enum { CONC_WORKERS = 8, CONC_KEYS = 256 };

typedef struct ConcEnv {
    SyncWaitGroup *wg;
    UniqueHandle got[CONC_KEYS];
} ConcEnv;

static void conc_worker(void *arg) {
    ConcEnv *e = arg;
    Byte buf[16];
    for (int i = 0; i < CONC_KEYS; i++) {
        int n = 0;
        buf[n++] = 'k';
        for (int v = i;; v /= 10) {
            buf[n++] = (Byte)('0' + v % 10);
            if (v < 10)
                break;
        }
        Str s = str_from_bytes(buf, n);
        e->got[i] = UNIQUE_MAKE(Str, &s);
    }
    sync_wait_group_done(e->wg);
}

static void TestMakeConcurrent(TestingT *t) {
    static ConcEnv envs[CONC_WORKERS];
    SyncWaitGroup wg = {0};
    for (int w = 0; w < CONC_WORKERS; w++) {
        envs[w].wg = &wg;
        sync_wait_group_add(&wg, 1);
        if (!go(BURROW_FN(Func, conc_worker, &envs[w])))
            testing_t_fatalf_v(t, "go failed");
    }
    sync_wait_group_wait(&wg);
    for (int i = 0; i < CONC_KEYS; i++)
        for (int w = 1; w < CONC_WORKERS; w++)
            if (!unique_handle_eq(envs[w].got[i], envs[0].got[i]))
                testing_t_errorf_v(t, "key %v: worker %v got a different handle", i, w);
}

static void BenchmarkMake(TestingB *b) {
    Str s = BURROW_S("fe80::1%eth0 zone name");
    for (Int i = 0; i < testing_b_n(b); i++)
        (void)UNIQUE_MAKE(Str, &s);
}

static void make_parallel(void *env, TestingPB *pb) {
    (void)env;
    Str s = BURROW_S("fe80::1%eth0 zone name");
    while (testing_pb_next(pb))
        (void)UNIQUE_MAKE(Str, &s);
}

static void BenchmarkMakeParallel(TestingB *b) {
    testing_b_run_parallel(b, BURROW_FN(TestingPBFunc, make_parallel, NULL));
}

#define TESTS(X)                                                                       \
    X(TestHandle)                                                                      \
    X(TestHandleDistinct)                                                              \
    X(TestHandleZeroSize)                                                              \
    X(TestMakeClonesStrings)                                                           \
    X(TestHandleUnsafeString)                                                          \
    X(TestNestedHandleChain)                                                           \
    X(TestMakeAllocs)                                                                  \
    X(TestMakeConcurrent)                                                              \
    X(BenchmarkMake)                                                                   \
    X(BenchmarkMakeParallel)

TESTING_MAIN(TESTS)
