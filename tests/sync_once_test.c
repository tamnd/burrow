/* Tests for sync.WaitGroup, sync.Once and the three Once wrappers.
 *
 * Split from tests/sync_test.c because these are the things built on the locks
 * rather than the locks themselves, and a failure in one file should not have
 * to be read against the other.
 *
 * The same rule about goroutines applies here as everywhere else: only the main
 * goroutine calls CHECK, and a child says what it found out through an atomic.
 *
 * Every count is one that was known before the test ran. A WaitGroup test that
 * only asserts it did not hang is a test that passes on a Wait which returns
 * straight away, so the ones here check the work was actually finished by the
 * time Wait came back, which is the whole claim.
 *
 * Copyright 2026 The burrow Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style licence that can be found
 * in the LICENSE file. */

#include "burrow/sync.h"

#include "burrow/func.h"
#include "burrow/iface.h"
#include "burrow/panic.h"
#include "burrow/proc.h"
#include "burrow/sync/atomic.h"
#include "burrow/thread.h"
#include "burrow/type.h"

#include "fatal.h"
#include "harness.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

/* ---------------------------------------------------------------- WaitGroup */

#define WORKERS 8
#define ROUNDS 500

static SyncWaitGroup wg;
static SyncAtomicInt64 done_work;
static SyncAtomicUint32 wait_returned;
static SyncAtomicUint32 saw_unfinished;

static void reset(void) {
    memset(&wg, 0, sizeof(wg));
    sync_atomic_int64_store(&done_work, 0);
    sync_atomic_uint32_store(&wait_returned, 0);
    sync_atomic_uint32_store(&saw_unfinished, 0);
}

static void add_some(void *env) {
    (void)env;

    for (int i = 0; i < ROUNDS; i++)
        (void)sync_atomic_int64_add(&done_work, 1);
}

/* Started before the group has anything in it, which is the ordering Go warns
 * about and the reason Go grew WaitGroup.Go. Every start here is a Go. */
static void go_main(void *env) {
    (void)env;

    for (int i = 0; i < WORKERS; i++) {
        if (!sync_wait_group_go(&wg, BURROW_FN(Func, add_some, NULL)))
            return;
    }

    sync_wait_group_wait(&wg);

    /* The claim is not that Wait returned, it is that every worker had
     * finished when it did. */
    if (sync_atomic_int64_load(&done_work) != (int64_t)WORKERS * ROUNDS)
        (void)sync_atomic_uint32_add(&saw_unfinished, 1);

    (void)sync_atomic_uint32_add(&wait_returned, 1);
}

TEST(wait_returns_only_after_every_goroutine_has_finished) {
    reset();

    runtime_main(BURROW_FN(Func, go_main, NULL));

    CHECK_INT_EQ(sync_atomic_int64_load(&done_work), (int64_t)WORKERS * ROUNDS);
    CHECK_INT_EQ(sync_atomic_uint32_load(&wait_returned), 1);
    CHECK_INT_EQ(sync_atomic_uint32_load(&saw_unfinished), 0);
}

/* The same thing spelled out with Add and Done, which is the shape of every Go
 * program written before WaitGroup.Go existed and has to keep working. */
static void add_done_body(void *env) {
    (void)env;

    add_some(NULL);
    sync_wait_group_done(&wg);
}

static void add_done_main(void *env) {
    (void)env;

    sync_wait_group_add(&wg, WORKERS);

    for (int i = 0; i < WORKERS; i++) {
        if (!go(BURROW_FN(Func, add_done_body, NULL)))
            sync_wait_group_done(&wg);
    }

    sync_wait_group_wait(&wg);

    if (sync_atomic_int64_load(&done_work) != (int64_t)WORKERS * ROUNDS)
        (void)sync_atomic_uint32_add(&saw_unfinished, 1);

    (void)sync_atomic_uint32_add(&wait_returned, 1);
}

TEST(add_and_done_work_the_way_go_calls_them) {
    reset();

    runtime_main(BURROW_FN(Func, add_done_main, NULL));

    CHECK_INT_EQ(sync_atomic_int64_load(&done_work), (int64_t)WORKERS * ROUNDS);
    CHECK_INT_EQ(sync_atomic_uint32_load(&wait_returned), 1);
    CHECK_INT_EQ(sync_atomic_uint32_load(&saw_unfinished), 0);
}

/* Several waiters, all of which have to come back, since Add wakes them by
 * releasing the semaphore once per waiter and an off by one there would leave
 * one of them asleep forever. */
#define WAITERS 5

static void waiter_body(void *env) {
    (void)env;

    sync_wait_group_wait(&wg);

    if (sync_atomic_int64_load(&done_work) != ROUNDS)
        (void)sync_atomic_uint32_add(&saw_unfinished, 1);

    (void)sync_atomic_uint32_add(&wait_returned, 1);
}

static void many_waiters_main(void *env) {
    (void)env;

    sync_wait_group_add(&wg, 1);

    for (int i = 0; i < WAITERS; i++) {
        if (!go(BURROW_FN(Func, waiter_body, NULL)))
            (void)sync_atomic_uint32_add(&wait_returned, 1);
    }

    /* Give them a chance to queue, so that the release path with waiters on it
     * is the one under test rather than the one where the counter was already
     * zero. Not required for correctness either way. */
    for (int i = 0; i < 100; i++)
        runtime_gosched();

    add_some(NULL);
    sync_wait_group_done(&wg);

    while (sync_atomic_uint32_load(&wait_returned) < WAITERS)
        runtime_gosched();
}

TEST(every_waiter_is_woken_and_not_just_the_first) {
    reset();

    runtime_main(BURROW_FN(Func, many_waiters_main, NULL));

    CHECK_INT_EQ(sync_atomic_uint32_load(&wait_returned), WAITERS);
    CHECK_INT_EQ(sync_atomic_uint32_load(&saw_unfinished), 0);
}

TEST(a_group_with_nothing_in_it_does_not_wait) {
    SyncWaitGroup empty;
    memset(&empty, 0, sizeof(empty));

    /* No runtime, no goroutines, nothing to wait for. Twice, because a Wait
     * that consumed something would only show up on the second. */
    sync_wait_group_wait(&empty);
    sync_wait_group_wait(&empty);

    sync_wait_group_add(&empty, 2);
    sync_wait_group_done(&empty);
    sync_wait_group_done(&empty);
    sync_wait_group_wait(&empty);

    /* And it is reusable once it has drained. */
    sync_wait_group_add(&empty, 1);
    sync_wait_group_done(&empty);
    sync_wait_group_wait(&empty);

    CHECK(true);
}

TEST(a_counter_below_zero_panics) {
    SyncWaitGroup bad;
    memset(&bad, 0, sizeof(bad));

    CHECK_RUNTIME_ERROR(sync_wait_group_done(&bad), "sync: negative WaitGroup counter");

    memset(&bad, 0, sizeof(bad));
    sync_wait_group_add(&bad, 1);
    sync_wait_group_done(&bad);
    CHECK_RUNTIME_ERROR(sync_wait_group_add(&bad, -1),
                        "sync: negative WaitGroup counter");
}

/* ---------------------------------------------------------------- Once */

static SyncOnce once;
static SyncAtomicInt64 ran;
static SyncAtomicInt64 callers_through;
static SyncAtomicUint32 saw_half_built;
static int64_t built;

static void build(void *env) {
    (void)env;

    /* Long enough that a caller which did not wait would be through before
     * this finishes, which is the difference between "called once" and "when
     * you return, it has finished". */
    for (int i = 0; i < 100000; i++)
        built = i + 1;

    (void)sync_atomic_int64_add(&ran, 1);
}

static void use_it(void *env) {
    (void)env;

    sync_once_do(&once, BURROW_FN(Func, build, NULL));

    if (built != 100000)
        (void)sync_atomic_uint32_add(&saw_half_built, 1);

    (void)sync_atomic_int64_add(&callers_through, 1);
}

static void once_main(void *env) {
    (void)env;

    for (int i = 0; i < WORKERS; i++) {
        if (!go(BURROW_FN(Func, use_it, NULL)))
            (void)sync_atomic_int64_add(&callers_through, 1);
    }

    while (sync_atomic_int64_load(&callers_through) < WORKERS)
        runtime_gosched();
}

TEST(once_runs_one_thing_one_time_however_many_ask_at_once) {
    memset(&once, 0, sizeof(once));
    sync_atomic_int64_store(&ran, 0);
    sync_atomic_int64_store(&callers_through, 0);
    sync_atomic_uint32_store(&saw_half_built, 0);
    built = 0;

    runtime_main(BURROW_FN(Func, once_main, NULL));

    CHECK_INT_EQ(sync_atomic_int64_load(&ran), 1);
    CHECK_INT_EQ(sync_atomic_int64_load(&callers_through), WORKERS);

    /* The real claim: nobody came back from Do before build had finished. */
    CHECK_INT_EQ(sync_atomic_uint32_load(&saw_half_built), 0);
}

static int64_t first_count;
static int64_t second_count;

static void bump_first(void *env) {
    (void)env;
    first_count++;
}

static void bump_second(void *env) {
    (void)env;
    second_count++;
}

TEST(a_different_function_on_a_later_call_is_not_run) {
    SyncOnce o;
    memset(&o, 0, sizeof(o));
    first_count = 0;
    second_count = 0;

    sync_once_do(&o, BURROW_FN(Func, bump_first, NULL));
    sync_once_do(&o, BURROW_FN(Func, bump_second, NULL));
    sync_once_do(&o, BURROW_FN(Func, bump_second, NULL));

    CHECK_INT_EQ(first_count, 1);
    CHECK_INT_EQ(second_count, 0);
}

static void blow_up(void *env) {
    (void)env;
    panic_str(BURROW_S("boom"));
}

TEST(a_once_whose_function_panics_counts_as_done) {
    SyncOnce o;
    memset(&o, 0, sizeof(o));
    first_count = 0;

    CHECK_PANIC(sync_once_do(&o, BURROW_FN(Func, blow_up, NULL)), "boom");

    /* Go's rule: f had its turn, so a later call runs nothing and returns. The
     * lock has to have been given back on the way out too, or this hangs. */
    sync_once_do(&o, BURROW_FN(Func, bump_first, NULL));
    CHECK_INT_EQ(first_count, 0);
}

/* ------------------------------------------------------------- OnceFunc */

TEST(a_once_func_runs_one_time) {
    first_count = 0;
    SyncOnceFunc of = SYNC_ONCE_FUNC(BURROW_FN(Func, bump_first, NULL));

    sync_once_func_call(&of);
    sync_once_func_call(&of);
    sync_once_func_call(&of);

    CHECK_INT_EQ(first_count, 1);

    /* And as a Func, which is the form for handing it to something else. */
    Func f = sync_once_func_fn(&of);
    BURROW_CALLF0(f);
    CHECK_INT_EQ(first_count, 1);
}

TEST(a_once_func_that_panics_panics_again_every_time) {
    SyncOnceFunc of = SYNC_ONCE_FUNC(BURROW_FN(Func, blow_up, NULL));

    /* This is the whole difference from a bare Once. There, the second caller
     * carries on past a thing that was never built. */
    CHECK_PANIC(sync_once_func_call(&of), "boom");
    CHECK_PANIC(sync_once_func_call(&of), "boom");
    CHECK_PANIC(sync_once_func_call(&of), "boom");
}

/* ------------------------------------------------------------ OnceValue */

static int64_t computed;
static int64_t computations;

static Any compute(void *env) {
    (void)env;

    computations++;
    computed = 42;

    return BURROW_ANY(TYPE_INT64, &computed);
}

static Any compute_blows_up(void *env) {
    (void)env;

    panic_str(BURROW_S("boom"));
}

TEST(a_once_value_computes_one_time_and_keeps_the_answer) {
    computations = 0;
    SyncOnceValue ov = SYNC_ONCE_VALUE(BURROW_FN(AnyFunc, compute, NULL));

    Any first = sync_once_value_get(&ov);
    Any again = sync_once_value_get(&ov);

    CHECK_INT_EQ(computations, 1);
    CHECK(first.t == TYPE_INT64);
    CHECK(again.t == TYPE_INT64);
    CHECK(first.data == again.data);

    int64_t *n = any_assert(first, TYPE_INT64);
    CHECK(n != NULL);
    CHECK_INT_EQ(*n, 42);
}

TEST(a_once_value_that_panics_panics_again_every_time) {
    SyncOnceValue ov = SYNC_ONCE_VALUE(BURROW_FN(AnyFunc, compute_blows_up, NULL));

    CHECK_PANIC((void)sync_once_value_get(&ov), "boom");
    CHECK_PANIC((void)sync_once_value_get(&ov), "boom");
}

/* ----------------------------------------------------------- OnceValues */

static int64_t pair_first;
static int64_t pair_second;

static void compute_pair(void *env, Any *a, Any *b) {
    (void)env;

    computations++;
    pair_first = 7;
    pair_second = 9;

    *a = BURROW_ANY(TYPE_INT64, &pair_first);
    *b = BURROW_ANY(TYPE_INT64, &pair_second);
}

static void compute_pair_blows_up(void *env, Any *a, Any *b) {
    (void)env;
    (void)a;
    (void)b;

    panic_str(BURROW_S("boom"));
}

TEST(a_once_values_gives_back_both_results_every_time) {
    computations = 0;
    SyncOnceValues ov =
        SYNC_ONCE_VALUES(BURROW_FN(SyncOnceValuesFn, compute_pair, NULL));

    Any a;
    Any b;
    memset(&a, 0, sizeof(a));
    memset(&b, 0, sizeof(b));

    sync_once_values_get(&ov, &a, &b);
    CHECK_INT_EQ(computations, 1);
    CHECK_INT_EQ(*(int64_t *)any_assert(a, TYPE_INT64), 7);
    CHECK_INT_EQ(*(int64_t *)any_assert(b, TYPE_INT64), 9);

    memset(&a, 0, sizeof(a));
    memset(&b, 0, sizeof(b));

    sync_once_values_get(&ov, &a, &b);
    CHECK_INT_EQ(computations, 1);
    CHECK_INT_EQ(*(int64_t *)any_assert(a, TYPE_INT64), 7);
    CHECK_INT_EQ(*(int64_t *)any_assert(b, TYPE_INT64), 9);

    /* Either result may be dropped, which is what a caller that only wants the
     * error, or only the value, writes. */
    sync_once_values_get(&ov, NULL, NULL);
    CHECK_INT_EQ(computations, 1);
}

TEST(a_once_values_that_panics_panics_again_every_time) {
    SyncOnceValues ov =
        SYNC_ONCE_VALUES(BURROW_FN(SyncOnceValuesFn, compute_pair_blows_up, NULL));

    CHECK_PANIC(sync_once_values_get(&ov, NULL, NULL), "boom");
    CHECK_PANIC(sync_once_values_get(&ov, NULL, NULL), "boom");
}

/* ------------------------------------------------------------- threads
 *
 * Not goroutines, because a Once and a WaitGroup have to work for the host
 * program's own threads too, and that is a different path through the
 * semaphore: a thread sleeps on a note instead of parking. */

static burrow__Thread threads[WORKERS];
static SyncWaitGroup thread_wg;

static void thread_use_it(void *env) {
    (void)env;

    sync_once_do(&once, BURROW_FN(Func, build, NULL));

    if (built != 100000)
        (void)sync_atomic_uint32_add(&saw_half_built, 1);

    (void)sync_atomic_int64_add(&callers_through, 1);
    sync_wait_group_done(&thread_wg);
}

TEST(threads_that_are_not_goroutines_share_a_once_and_a_wait_group) {
    memset(&once, 0, sizeof(once));
    memset(&thread_wg, 0, sizeof(thread_wg));
    sync_atomic_int64_store(&ran, 0);
    sync_atomic_int64_store(&callers_through, 0);
    sync_atomic_uint32_store(&saw_half_built, 0);
    built = 0;

    sync_wait_group_add(&thread_wg, WORKERS);

    for (size_t i = 0; i < WORKERS; i++)
        CHECK(burrow__thread_start(&threads[i], thread_use_it, NULL, 0));

    sync_wait_group_wait(&thread_wg);

    /* Wait came back, so every thread is past its Done, which means every one
     * of them is past the Once. */
    CHECK_INT_EQ(sync_atomic_int64_load(&ran), 1);
    CHECK_INT_EQ(sync_atomic_int64_load(&callers_through), WORKERS);
    CHECK_INT_EQ(sync_atomic_uint32_load(&saw_half_built), 0);

    for (size_t i = 0; i < WORKERS; i++)
        CHECK(burrow__thread_join(&threads[i]));
}

int main(void) {
    RUN(a_group_with_nothing_in_it_does_not_wait);
    RUN(a_counter_below_zero_panics);
    RUN(wait_returns_only_after_every_goroutine_has_finished);
    RUN(add_and_done_work_the_way_go_calls_them);
    RUN(every_waiter_is_woken_and_not_just_the_first);

    RUN(a_different_function_on_a_later_call_is_not_run);
    RUN(a_once_whose_function_panics_counts_as_done);
    RUN(once_runs_one_thing_one_time_however_many_ask_at_once);

    RUN(a_once_func_runs_one_time);
    RUN(a_once_func_that_panics_panics_again_every_time);
    RUN(a_once_value_computes_one_time_and_keeps_the_answer);
    RUN(a_once_value_that_panics_panics_again_every_time);
    RUN(a_once_values_gives_back_both_results_every_time);
    RUN(a_once_values_that_panics_panics_again_every_time);

    RUN(threads_that_are_not_goroutines_share_a_once_and_a_wait_group);

    return harness_report("sync_once");
}
