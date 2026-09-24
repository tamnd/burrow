/* Derived from Go's src/iter/pull_test.go.
 * Go source: go1.27.1.
 *
 * Go's closures become an env struct each. The package level slots the double
 * next and double yield tests use stay package level, as statics.
 *
 * Two tests are not from Go. TestRange and TestRange2 check BURROW_RANGE, which
 * stands in for Go's range over func, including break, continue and nesting.
 *
 * Copyright 2023 The Go Authors. All rights reserved.
 * Copyright 2026 The burrow Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style licence that can be found
 * in the LICENSE file. */

#include "burrow/iter.h"

#include "burrow/burrow.h"
#include "burrow/chan.h"
#include "burrow/mem/heap.h"
#include "burrow/panic.h"
#include "burrow/proc.h"

#include "check.h"

/* ------------------------------------------------------------ sequences */

static void count_seq(void *env, IterYield yield) {
    Int n = *(const Int *)env;
    for (Int i = 0; i < n; i++) {
        if (!BURROW_CALLF(yield, &i))
            break;
    }
}

static IterSeq count(Int *n) {
    return BURROW_FN(IterSeq, count_seq, n);
}

static void squares_seq(void *env, IterYield2 yield) {
    Int n = *(const Int *)env;
    for (Int i = 0; i < n; i++) {
        int64_t sq = (int64_t)i * (int64_t)i;
        if (!BURROW_CALLF(yield, &i, &sq))
            break;
    }
}

static IterSeq2 squares(Int *n) {
    return BURROW_FN(IterSeq2, squares_seq, n);
}

/* stableNumGoroutine is like NumGoroutine but tries to ensure stability of
 * the value by letting any exiting goroutines finish exiting. */
static int stable_num_goroutine(void) {
    int procs = runtime_gomaxprocs(1);
    int c = 0;
    int ng = runtime_numgoroutine();
    for (int i = 0; i < 1000; i++) {
        int nng = runtime_numgoroutine();
        if (nng == ng) {
            c++;
        } else {
            c = 0;
            ng = nng;
        }
        if (c >= 100) {
            /* The same value 100 times in a row is good enough. */
            runtime_gomaxprocs(procs);
            return ng;
        }
        runtime_gosched();
    }
    panic_str(BURROW_S("failed to stabilize NumGoroutine after 1000 iterations"));
}

#define WANT_NG(t, ng, want)                                                           \
    do {                                                                               \
        int xg_ = runtime_numgoroutine() - (ng);                                       \
        if (xg_ != (want))                                                             \
            testing_t_errorf_v((t), "have %d extra goroutines, want %d", xg_, (want)); \
    } while (0)

/* ------------------------------------------------------------ TestPull */

static void pull_end(void *env, TestingT *t) {
    Int end = *(const Int *)env;
    int ng = stable_num_goroutine();
    WANT_NG(t, ng, 0);
    Int three = 3;
    IterPull p;
    CHECK(iter_pull(&p, count(&three)));
    WANT_NG(t, ng, 1);
    for (Int i = 0; i < end; i++) {
        const void *v;
        bool ok = iter_pull_next(&p, &v);
        if (!ok || *(const Int *)v != i)
            testing_t_fatalf_v(t, "next() = %d, %v, want %d, %v",
                               ok ? *(const Int *)v : 0, ok, i, true);
        WANT_NG(t, ng, 1);
    }
    WANT_NG(t, ng, 1);
    if (end < 3) {
        iter_pull_stop(&p);
        WANT_NG(t, ng, 0);
    }
    for (int k = 0; k < 2; k++) {
        const void *v;
        bool ok = iter_pull_next(&p, &v);
        if (v != NULL || ok != false)
            testing_t_fatalf_v(t, "next() = %p, %v, want nil, %v", v, ok, false);
        WANT_NG(t, ng, 0);
    }
    WANT_NG(t, ng, 0);

    iter_pull_stop(&p);
    iter_pull_stop(&p);
    iter_pull_stop(&p);
    WANT_NG(t, ng, 0);
}

static const char *const end_names[] = {"0", "1", "2", "3"};

static void TestPull(TestingT *t) {
    for (Int end = 0; end <= 3; end++)
        testing_t_run(t, str_from_cstr(end_names[end]),
                      BURROW_FN(TestingTFunc, pull_end, &end));
}

static void pull2_end(void *env, TestingT *t) {
    Int end = *(const Int *)env;
    int ng = stable_num_goroutine();
    WANT_NG(t, ng, 0);
    Int three = 3;
    IterPull2 p;
    CHECK(iter_pull2(&p, squares(&three)));
    WANT_NG(t, ng, 1);
    for (Int i = 0; i < end; i++) {
        const void *k;
        const void *v;
        bool ok = iter_pull2_next(&p, &k, &v);
        if (!ok || *(const Int *)k != i || *(const int64_t *)v != (int64_t)(i * i))
            testing_t_fatalf_v(t, "next() = %d, %d, %v, want %d, %d, %v",
                               ok ? *(const Int *)k : 0, ok ? *(const int64_t *)v : 0,
                               ok, i, i * i, true);
        WANT_NG(t, ng, 1);
    }
    WANT_NG(t, ng, 1);
    if (end < 3) {
        iter_pull2_stop(&p);
        WANT_NG(t, ng, 0);
    }
    for (int j = 0; j < 2; j++) {
        const void *k;
        const void *v;
        bool ok = iter_pull2_next(&p, &k, &v);
        if (v != NULL || ok != false)
            testing_t_fatalf_v(t, "next() = %p, %p, %v, want nil, nil, %v", k, v, ok,
                               false);
        WANT_NG(t, ng, 0);
    }
    WANT_NG(t, ng, 0);

    iter_pull2_stop(&p);
    iter_pull2_stop(&p);
    iter_pull2_stop(&p);
    WANT_NG(t, ng, 0);
}

static void TestPull2(TestingT *t) {
    for (Int end = 0; end <= 3; end++)
        testing_t_run(t, str_from_cstr(end_names[end]),
                      BURROW_FN(TestingTFunc, pull2_end, &end));
}

/* ------------------------------------------------------ double next and yield */

static IterPull *next_slot;

static void do_double_next(void *env, IterYield yield) {
    (void)env;
    (void)yield;
    BURROW_TRY {
        const void *v;
        iter_pull_next(next_slot, &v);
    }
    BURROW_CATCH(p) {
        (void)p;
        next_slot = NULL;
    }
    BURROW_TRY_END;
}

static void TestPullDoubleNext(TestingT *t) {
    IterPull p;
    CHECK(iter_pull(&p, BURROW_FN(IterSeq, do_double_next, NULL)));
    next_slot = &p;
    const void *v;
    iter_pull_next(&p, &v);
    if (next_slot != NULL)
        testing_t_fatal_v(t, "double next did not fail");
}

static IterPull2 *next_slot2;

static void do_double_next2(void *env, IterYield2 yield) {
    (void)env;
    (void)yield;
    BURROW_TRY {
        const void *k;
        const void *v;
        iter_pull2_next(next_slot2, &k, &v);
    }
    BURROW_CATCH(p) {
        (void)p;
        next_slot2 = NULL;
    }
    BURROW_TRY_END;
}

static void TestPullDoubleNext2(TestingT *t) {
    IterPull2 p;
    CHECK(iter_pull2(&p, BURROW_FN(IterSeq2, do_double_next2, NULL)));
    next_slot2 = &p;
    const void *k;
    const void *v;
    iter_pull2_next(&p, &k, &v);
    if (next_slot2 != NULL)
        testing_t_fatal_v(t, "double next did not fail");
}

static IterYield yield_slot;
static bool yield_slot_set;

static void store_yield(void *env, IterYield yield) {
    (void)env;
    yield_slot = yield;
    yield_slot_set = true;
    Int five = 5;
    if (!BURROW_CALLF(yield, &five))
        return;
}

static void TestPullDoubleYield(TestingT *t) {
    IterPull p;
    CHECK(iter_pull(&p, BURROW_FN(IterSeq, store_yield, NULL)));
    const void *v;
    iter_pull_next(&p, &v);
    if (!yield_slot_set)
        testing_t_fatal_v(t, "yield failed");
    BURROW_TRY {
        Int five = 5;
        BURROW_CALLF(yield_slot, &five);
    }
    BURROW_CATCH(r) {
        (void)r;
        yield_slot_set = false;
    }
    BURROW_TRY_END;
    iter_pull_stop(&p);
    if (yield_slot_set)
        testing_t_fatal_v(t, "double yield did not fail");
}

static IterYield2 yield_slot2;
static bool yield_slot2_set;

static void store_yield2(void *env, IterYield2 yield) {
    (void)env;
    yield_slot2 = yield;
    yield_slot2_set = true;
    Int k = 23, v = 77;
    if (!BURROW_CALLF(yield, &k, &v))
        return;
}

static void TestPullDoubleYield2(TestingT *t) {
    IterPull2 p;
    CHECK(iter_pull2(&p, BURROW_FN(IterSeq2, store_yield2, NULL)));
    const void *k;
    const void *v;
    iter_pull2_next(&p, &k, &v);
    if (!yield_slot2_set)
        testing_t_fatal_v(t, "yield failed");
    BURROW_TRY {
        Int k2 = 23, v2 = 77;
        BURROW_CALLF(yield_slot2, &k2, &v2);
    }
    BURROW_CATCH(r) {
        (void)r;
        yield_slot2_set = false;
    }
    BURROW_TRY_END;
    iter_pull2_stop(&p);
    if (yield_slot2_set)
        testing_t_fatal_v(t, "double yield did not fail");
}

/* ------------------------------------------------------------ panics */

static void panic_seq(void *env, IterYield yield) {
    (void)env;
    (void)yield;
    panic_str(BURROW_S("boom"));
}

static void panic_cleanup_seq(void *env, IterYield yield) {
    (void)env;
    for (;;) {
        Int v = 55;
        if (!BURROW_CALLF(yield, &v))
            panic_str(BURROW_S("boom"));
    }
}

static void panic_seq2(void *env, IterYield2 yield) {
    (void)env;
    (void)yield;
    panic_str(BURROW_S("boom"));
}

static void panic_cleanup_seq2(void *env, IterYield2 yield) {
    (void)env;
    for (;;) {
        Int k = 55, v = 100;
        if (!BURROW_CALLF(yield, &k, &v))
            panic_str(BURROW_S("boom"));
    }
}

/* panicsWith, for a string value, which is what every caller here passes.
 * Anything else is panicked again, as in Go. */
static bool panics_with(Str want, Func f) {
    volatile bool panicked = false;
    BURROW_TRY {
        BURROW_CALLF0(f);
    }
    BURROW_CATCH(r) {
        if (r.t != TYPE_STRING || !str_eq(*(const Str *)r.data, want))
            panic(r);
        panicked = true;
    }
    BURROW_TRY_END;
    return panicked;
}

static void call_next(void *env) {
    const void *v;
    iter_pull_next((IterPull *)env, &v);
}

static void call_stop(void *env) {
    iter_pull_stop((IterPull *)env);
}

static void call_next2(void *env) {
    const void *k;
    const void *v;
    iter_pull2_next((IterPull2 *)env, &k, &v);
}

static void call_stop2(void *env) {
    iter_pull2_stop((IterPull2 *)env);
}

static void pull_panic_next(void *env, TestingT *t) {
    (void)env;
    IterPull p;
    CHECK(iter_pull(&p, BURROW_FN(IterSeq, panic_seq, NULL)));
    if (!panics_with(BURROW_S("boom"), BURROW_FN(Func, call_next, &p)))
        testing_t_fatal_v(t, "failed to propagate panic on first next");
    /* Make sure we don't panic again if we try to call next or stop. */
    const void *v;
    if (iter_pull_next(&p, &v))
        testing_t_fatal_v(t, "next returned true after iterator panicked");
    /* Calling stop again should be a no-op. */
    iter_pull_stop(&p);
}

static void pull_panic_stop(void *env, TestingT *t) {
    (void)env;
    IterPull p;
    CHECK(iter_pull(&p, BURROW_FN(IterSeq, panic_cleanup_seq, NULL)));
    const void *x;
    bool ok = iter_pull_next(&p, &x);
    if (!ok || *(const Int *)x != 55)
        testing_t_fatalf_v(t, "expected (55, true) from next, got (%d, %t)",
                           ok ? *(const Int *)x : 0, ok);
    if (!panics_with(BURROW_S("boom"), BURROW_FN(Func, call_stop, &p)))
        testing_t_fatal_v(t, "failed to propagate panic on stop");
    /* Make sure we don't panic again if we try to call next or stop. */
    if (iter_pull_next(&p, &x))
        testing_t_fatal_v(t, "next returned true after iterator panicked");
    /* Calling stop again should be a no-op. */
    iter_pull_stop(&p);
}

static void TestPullPanic(TestingT *t) {
    testing_t_run(t, BURROW_S("next"), BURROW_FN(TestingTFunc, pull_panic_next, NULL));
    testing_t_run(t, BURROW_S("stop"), BURROW_FN(TestingTFunc, pull_panic_stop, NULL));
}

static void pull2_panic_next(void *env, TestingT *t) {
    (void)env;
    IterPull2 p;
    CHECK(iter_pull2(&p, BURROW_FN(IterSeq2, panic_seq2, NULL)));
    if (!panics_with(BURROW_S("boom"), BURROW_FN(Func, call_next2, &p)))
        testing_t_fatal_v(t, "failed to propagate panic on first next");
    /* Make sure we don't panic again if we try to call next or stop. */
    const void *k;
    const void *v;
    if (iter_pull2_next(&p, &k, &v))
        testing_t_fatal_v(t, "next returned true after iterator panicked");
    /* Calling stop again should be a no-op. */
    iter_pull2_stop(&p);
}

static void pull2_panic_stop(void *env, TestingT *t) {
    (void)env;
    IterPull2 p;
    CHECK(iter_pull2(&p, BURROW_FN(IterSeq2, panic_cleanup_seq2, NULL)));
    const void *x;
    const void *y;
    bool ok = iter_pull2_next(&p, &x, &y);
    if (!ok || *(const Int *)x != 55 || *(const Int *)y != 100)
        testing_t_fatalf_v(t, "expected (55, 100, true) from next, got (%d, %d, %t)",
                           ok ? *(const Int *)x : 0, ok ? *(const Int *)y : 0, ok);
    if (!panics_with(BURROW_S("boom"), BURROW_FN(Func, call_stop2, &p)))
        testing_t_fatal_v(t, "failed to propagate panic on stop");
    /* Make sure we don't panic again if we try to call next or stop. */
    if (iter_pull2_next(&p, &x, &y))
        testing_t_fatal_v(t, "next returned true after iterator panicked");
    /* Calling stop again should be a no-op. */
    iter_pull2_stop(&p);
}

static void TestPull2Panic(TestingT *t) {
    testing_t_run(t, BURROW_S("next"), BURROW_FN(TestingTFunc, pull2_panic_next, NULL));
    testing_t_run(t, BURROW_S("stop"), BURROW_FN(TestingTFunc, pull2_panic_stop, NULL));
}

/* ------------------------------------------------------------ Goexit */

static void goexit_seq(void *env, IterYield yield) {
    (void)env;
    (void)yield;
    runtime_goexit();
}

static void goexit_cleanup_seq(void *env, IterYield yield) {
    (void)env;
    for (;;) {
        Int v = 55;
        if (!BURROW_CALLF(yield, &v))
            runtime_goexit();
    }
}

static void goexit_seq2(void *env, IterYield2 yield) {
    (void)env;
    (void)yield;
    runtime_goexit();
}

static void goexit_cleanup_seq2(void *env, IterYield2 yield) {
    (void)env;
    for (;;) {
        Int k = 55, v = 100;
        if (!BURROW_CALLF(yield, &k, &v))
            runtime_goexit();
    }
}

typedef struct GoexitsEnv {
    Chan *exit;
    Func f;
    volatile bool clean_exit;
    volatile bool panicked;
} GoexitsEnv;

static void goexits_report(void *env) {
    GoexitsEnv *e = (GoexitsEnv *)env;
    bool v = !e->panicked && !e->clean_exit;
    chan_send(e->exit, &v);
}

static void goexits_body(void *env) {
    GoexitsEnv *e = (GoexitsEnv *)env;
    BURROW_SCOPE {
        BURROW_DEFER(goexits_report, e);
        BURROW_TRY {
            BURROW_CALLF0(e->f);
            e->clean_exit = true;
        }
        BURROW_CATCH(r) {
            (void)r;
            e->panicked = true;
        }
        BURROW_TRY_END;
    }
    BURROW_SCOPE_END;
}

static bool goexits(TestingT *t, Func f) {
    testing_t_helper(t);
    GoexitsEnv e = {chan_make(heap_allocator(), TYPE_BOOL, 0), f, false, false};
    CHECK(e.exit != NULL);
    CHECK(go(BURROW_FN(Func, goexits_body, &e)));
    bool v = false;
    chan_recv(e.exit, &v);
    chan_free(e.exit);
    return v;
}

typedef struct GoexitNext {
    IterPull p;
    IterPull2 p2;
} GoexitNext;

static void goexit_start_next(void *env) {
    GoexitNext *g = (GoexitNext *)env;
    if (!iter_pull(&g->p, BURROW_FN(IterSeq, goexit_seq, NULL)))
        panic_str(BURROW_S("out of memory"));
    const void *v;
    iter_pull_next(&g->p, &v);
}

static void goexit_start_next2(void *env) {
    GoexitNext *g = (GoexitNext *)env;
    if (!iter_pull2(&g->p2, BURROW_FN(IterSeq2, goexit_seq2, NULL)))
        panic_str(BURROW_S("out of memory"));
    const void *k;
    const void *v;
    iter_pull2_next(&g->p2, &k, &v);
}

static void pull_goexit_next(void *env, TestingT *t) {
    (void)env;
    GoexitNext g;
    if (!goexits(t, BURROW_FN(Func, goexit_start_next, &g)))
        testing_t_fatal_v(t, "failed to Goexit from next");
    const void *x;
    if (iter_pull_next(&g.p, &x) || x != NULL)
        testing_t_fatal_v(t, "iterator returned valid value after iterator Goexited");
    iter_pull_stop(&g.p);
}

static void pull_goexit_stop(void *env, TestingT *t) {
    (void)env;
    IterPull p;
    CHECK(iter_pull(&p, BURROW_FN(IterSeq, goexit_cleanup_seq, NULL)));
    const void *x;
    bool ok = iter_pull_next(&p, &x);
    if (!ok || *(const Int *)x != 55)
        testing_t_fatalf_v(t, "expected (55, true) from next, got (%d, %t)",
                           ok ? *(const Int *)x : 0, ok);
    if (!goexits(t, BURROW_FN(Func, call_stop, &p)))
        testing_t_fatal_v(t, "failed to Goexit from stop");
    /* Make sure we don't panic again if we try to call next or stop. */
    if (iter_pull_next(&p, &x) || x != NULL)
        testing_t_fatal_v(
            t, "next returned true or non-zero value after iterator Goexited");
    /* Calling stop again should be a no-op. */
    iter_pull_stop(&p);
}

static void TestPullGoexit(TestingT *t) {
    testing_t_run(t, BURROW_S("next"), BURROW_FN(TestingTFunc, pull_goexit_next, NULL));
    testing_t_run(t, BURROW_S("stop"), BURROW_FN(TestingTFunc, pull_goexit_stop, NULL));
}

static void pull2_goexit_next(void *env, TestingT *t) {
    (void)env;
    GoexitNext g;
    if (!goexits(t, BURROW_FN(Func, goexit_start_next2, &g)))
        testing_t_fatal_v(t, "failed to Goexit from next");
    const void *x;
    const void *y;
    if (iter_pull2_next(&g.p2, &x, &y) || x != NULL || y != NULL)
        testing_t_fatal_v(t, "iterator returned valid value after iterator Goexited");
    iter_pull2_stop(&g.p2);
}

static void pull2_goexit_stop(void *env, TestingT *t) {
    (void)env;
    IterPull2 p;
    CHECK(iter_pull2(&p, BURROW_FN(IterSeq2, goexit_cleanup_seq2, NULL)));
    const void *x;
    const void *y;
    bool ok = iter_pull2_next(&p, &x, &y);
    if (!ok || *(const Int *)x != 55 || *(const Int *)y != 100)
        testing_t_fatalf_v(t, "expected (55, 100, true) from next, got (%d, %d, %t)",
                           ok ? *(const Int *)x : 0, ok ? *(const Int *)y : 0, ok);
    if (!goexits(t, BURROW_FN(Func, call_stop2, &p)))
        testing_t_fatal_v(t, "failed to Goexit from stop");
    /* Make sure we don't panic again if we try to call next or stop. */
    if (iter_pull2_next(&p, &x, &y) || x != NULL || y != NULL)
        testing_t_fatal_v(t, "next returned true or non-zero after iterator Goexited");
    /* Calling stop again should be a no-op. */
    iter_pull2_stop(&p);
}

static void TestPull2Goexit(TestingT *t) {
    testing_t_run(t, BURROW_S("next"),
                  BURROW_FN(TestingTFunc, pull2_goexit_next, NULL));
    testing_t_run(t, BURROW_S("stop"),
                  BURROW_FN(TestingTFunc, pull2_goexit_stop, NULL));
}

/* ------------------------------------------------------------ immediate stop */

static void TestPullImmediateStop(TestingT *t) {
    IterPull p;
    CHECK(iter_pull(&p, BURROW_FN(IterSeq, panic_seq, NULL)));
    iter_pull_stop(&p);
    /* Make sure we don't panic if we try to call next or stop. */
    const void *v;
    if (iter_pull_next(&p, &v))
        testing_t_fatal_v(t, "next returned true after iterator was stopped");
}

static void TestPull2ImmediateStop(TestingT *t) {
    IterPull2 p;
    CHECK(iter_pull2(&p, BURROW_FN(IterSeq2, panic_seq2, NULL)));
    iter_pull2_stop(&p);
    /* Make sure we don't panic if we try to call next or stop. */
    const void *k;
    const void *v;
    if (iter_pull2_next(&p, &k, &v))
        testing_t_fatal_v(t, "next returned true after iterator was stopped");
}

/* ------------------------------------------------------------ BURROW_RANGE */

static void TestRange(TestingT *t) {
    int ng = stable_num_goroutine();
    Int five = 5;
    Int sum = 0;
    BURROW_RANGE(Int, v, count(&five)) {
        if (v == 1)
            continue;
        if (v == 4)
            break;
        sum += v;
    }
    if (sum != 0 + 2 + 3)
        testing_t_errorf_v(t, "sum = %d, want 5", sum);
    WANT_NG(t, ng, 0);

    Int three = 3;
    Int pairs = 0;
    BURROW_RANGE(Int, i, count(&three)) {
        BURROW_RANGE(Int, j, count(&three)) {
            if (j > i)
                break;
            pairs++;
        }
    }
    if (pairs != 6)
        testing_t_errorf_v(t, "pairs = %d, want 6", pairs);
    WANT_NG(t, ng, 0);

    Int zero = 0;
    BURROW_RANGE(Int, v, count(&zero)) {
        testing_t_errorf_v(t, "range over an empty sequence ran with %d", v);
    }
    WANT_NG(t, ng, 0);
}

static void TestRange2(TestingT *t) {
    int ng = stable_num_goroutine();
    Int four = 4;
    Int n = 0;
    BURROW_RANGE2(Int, i, int64_t, sq, squares(&four)) {
        if (sq != (int64_t)(i * i))
            testing_t_errorf_v(t, "got %d, %d", i, sq);
        n++;
    }
    if (n != 4)
        testing_t_errorf_v(t, "n = %d, want 4", n);
    WANT_NG(t, ng, 0);
}

/* ------------------------------------------------------------ benchmarks */

static void BenchmarkPull(TestingB *b) {
    Int one = 1;
    IterSeq seq = count(&one);
    for (Int i = 0; i < testing_b_n(b); i++) {
        IterPull p;
        iter_pull(&p, seq);
        iter_pull_stop(&p);
    }
}

static void BenchmarkPull2(TestingB *b) {
    Int one = 1;
    IterSeq2 seq = squares(&one);
    for (Int i = 0; i < testing_b_n(b); i++) {
        IterPull2 p;
        iter_pull2(&p, seq);
        iter_pull2_stop(&p);
    }
}

/* Not from Go: what a value costs through a pull, which is what BURROW_RANGE
 * pays per turn of the loop. */
static void BenchmarkPullNext(TestingB *b) {
    Int n = testing_b_n(b);
    IterPull p;
    iter_pull(&p, count(&n));
    const void *v;
    while (iter_pull_next(&p, &v)) {
    }
    iter_pull_stop(&p);
}

#define TESTS(X)                                                                       \
    X(TestPull)                                                                        \
    X(TestPull2)                                                                       \
    X(TestPullDoubleNext)                                                              \
    X(TestPullDoubleNext2)                                                             \
    X(TestPullDoubleYield)                                                             \
    X(TestPullDoubleYield2)                                                            \
    X(TestPullPanic)                                                                   \
    X(TestPull2Panic)                                                                  \
    X(TestPullGoexit)                                                                  \
    X(TestPull2Goexit)                                                                 \
    X(TestPullImmediateStop)                                                           \
    X(TestPull2ImmediateStop)                                                          \
    X(TestRange)                                                                       \
    X(TestRange2)                                                                      \
    X(BenchmarkPull)                                                                   \
    X(BenchmarkPull2)                                                                  \
    X(BenchmarkPullNext)

TESTING_MAIN(TESTS)
