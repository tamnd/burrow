/* runtime/cgo's tests, from Go's handle_test.go.
 *
 * Copyright 2021 The Go Authors. All rights reserved.
 * Copyright 2026 The burrow Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style licence that can be found
 * in the LICENSE file. */

#include "burrow/runtime/cgo.h"

#include "burrow/declare.h"
#include "burrow/func.h"
#include "burrow/panic.h"
#include "burrow/reflect.h"
#include "burrow/testing.h"

#include <string.h>

BURROW_PTR_TYPE(IntPtr, Int);

/* Go's test walks the package's map, and this is the same walk from inside
 * src/runtime/cgo.c. */
Int burrow__cgo_handle_count(void);

/* The panic text of f(env), or an empty string when it did not panic. */
static char panic_buf[256];

static bool panics(void (*f)(void *), void *env, Str *msg) {
    volatile bool got = false;
    volatile Int n = 0;
    BURROW_TRY {
        f(env);
    }
    BURROW_CATCH(r) {
        Str s = panic_text(r);
        n = s.len < (Int)sizeof panic_buf ? s.len : (Int)sizeof panic_buf;
        memcpy(panic_buf, s.p, (size_t)n);
        got = true;
    }
    BURROW_TRY_END;
    if (msg != NULL)
        *msg = str_from_bytes((const Byte *)panic_buf, n);
    return got;
}

static void TestHandle(TestingT *t) {
    Int v = 42;
    Int *pv = &v;

    struct {
        Any v1;
        Any v2;
    } tests[] = {
        {BURROW_ANY(TYPE_INT, &v), BURROW_ANY(TYPE_INT, &v)},
        {BURROW_ANY(TYPE_OF(IntPtr), &pv), BURROW_ANY(TYPE_OF(IntPtr), &pv)},
        {BURROW_ANY(NULL, NULL), BURROW_ANY(NULL, NULL)},
    };

    for (size_t i = 0; i < sizeof tests / sizeof tests[0]; i++) {
        CgoHandle h1 = cgo_new_handle(tests[i].v1);
        CgoHandle h2 = cgo_new_handle(tests[i].v2);

        if ((Uintptr)h1 == 0 || (Uintptr)h2 == 0)
            testing_t_fatalf_v(t, "NewHandle returns zero");

        if ((Uintptr)h1 == (Uintptr)h2)
            testing_t_fatalf_v(
                t, "Duplicated Go values should have different handles, but "
                   "got equal");

        Any h1v = cgo_handle_value(h1);
        Any h2v = cgo_handle_value(h2);
        if (!reflect_deep_equal(h1v, h2v) || !reflect_deep_equal(h1v, tests[i].v1)) {
            Any want = tests[i].v1;
            testing_t_fatalf_v(t, "Value of a Handle got wrong, got %+v %+v, want %+v",
                               h1v, h2v, want);
        }

        cgo_handle_delete(h1);
        cgo_handle_delete(h2);
    }

    Int siz = burrow__cgo_handle_count();
    if (siz != 0)
        testing_t_fatalf_v(t, "handles are not cleared, got %d, want %d", siz, 0);
}

static void delete_handle(void *env) {
    cgo_handle_delete(*(CgoHandle *)env);
}

static void invalid_zero(void *env, TestingT *t) {
    (void)env;
    CgoHandle h = 0;

    Str msg;
    if (!panics(delete_handle, &h, &msg))
        testing_t_fatalf_v(t, "Delete of zero handle did not trigger a panic");
    /* Not in Go: the panic says what Go's says. */
    if (!str_eq(msg, BURROW_S("runtime/cgo: misuse of an invalid Handle")))
        testing_t_errorf_v(t, "panic %q, want Go's", msg);
}

static void invalid_invalid(void *env, TestingT *t) {
    (void)env;
    static Int forty_two = 42;
    CgoHandle h = cgo_new_handle(BURROW_ANY(TYPE_INT, &forty_two));

    CgoHandle bad = h + 1;
    if (panics(delete_handle, &bad, NULL)) {
        cgo_handle_delete(h);
        return;
    }
    testing_t_fatalf_v(t, "Invalid handle did not trigger a panic");
}

static void TestInvalidHandle(TestingT *t) {
    testing_t_run(t, BURROW_S("zero"), BURROW_FN(TestingTFunc, invalid_zero, NULL));
    testing_t_run(t, BURROW_S("invalid"),
                  BURROW_FN(TestingTFunc, invalid_invalid, NULL));
}

static void bench_non_concurrent(void *env, TestingB *b) {
    (void)env;
    for (Int i = 0; i < testing_b_n(b); i++) {
        CgoHandle h = cgo_new_handle(BURROW_ANY(TYPE_INT, &i));
        (void)cgo_handle_value(h);
        cgo_handle_delete(h);
    }
}

static void bench_concurrent_body(void *env, TestingPB *pb) {
    (void)env;
    Int v = 0;
    while (testing_pb_next(pb)) {
        CgoHandle h = cgo_new_handle(BURROW_ANY(TYPE_INT, &v));
        (void)cgo_handle_value(h);
        cgo_handle_delete(h);
    }
}

static void bench_concurrent(void *env, TestingB *b) {
    (void)env;
    testing_b_run_parallel(b, BURROW_FN(TestingPBFunc, bench_concurrent_body, NULL));
}

static void BenchmarkHandle(TestingB *b) {
    testing_b_run(b, BURROW_S("non-concurrent"),
                  BURROW_FN(TestingBFunc, bench_non_concurrent, NULL));
    testing_b_run(b, BURROW_S("concurrent"),
                  BURROW_FN(TestingBFunc, bench_concurrent, NULL));
}

#define TESTS(X)                                                                       \
    X(TestHandle)                                                                      \
    X(TestInvalidHandle)                                                               \
    X(BenchmarkHandle)

TESTING_MAIN(TESTS)
