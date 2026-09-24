/* sync/atomic, under real contention.
 *
 * The plain functions and the typed structs are a thin layer over
 * burrow/atomic.h, which has a contention test of its own, so what is worth
 * running here is the small amount of this package that is not thin: Value.
 *
 * Value is two words, and its first store is the one moment where both of them
 * change and a reader could see half of it. Every test below is arranged so
 * that the answer is known in advance and is only reachable if that window held
 * together. A fresh Value per round and every thread walking the same array is
 * what puts the threads inside the window at the same time, and it is a better
 * race than any barrier because it happens thousands of times a second without
 * anybody arranging it.
 *
 * Copyright 2026 The burrow Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style licence that can be found
 * in the LICENSE file. */

#include "burrow/sync/atomic.h"

#include "burrow/core.h"
#include "burrow/iface.h"
#include "burrow/thread.h"
#include "burrow/type.h"

#include "check.h"

#include <stdint.h>
#include <string.h>

/* More threads than a CI runner has processors, so that some of them are
 * preempted in the middle of the two stores rather than all of them running to
 * the end of it. That is the case the sentinel exists for. */
#define THREADS 8
#define ROUNDS 20000

/* One Value per round, all of them untouched when the round starts. A shared
 * array rather than a shared single Value because a Value can only have a first
 * store once, and the first store is the whole subject. */
#define VALUES 4000

static burrow__Thread threads[THREADS];

static void run_all(TestingT *t, burrow__ThreadFn fn) {
    for (size_t i = 0; i < THREADS; i++)
        CHECK(burrow__thread_start(&threads[i], fn, (void *)(uintptr_t)i, 0));
    for (size_t i = 0; i < THREADS; i++)
        CHECK(burrow__thread_join(&threads[i]));
}

/* ----------------------------------------------------------------- counting */

static int64_t plain;
static SyncAtomicInt64 typed;
static SyncAtomicUint32 bits;

static void count_everything(void *arg) {
    size_t me = (size_t)(uintptr_t)arg;

    for (int i = 0; i < ROUNDS; i++) {
        (void)sync_atomic_add_int64(&plain, 1);
        (void)sync_atomic_int64_add(&typed, 1);
    }
    /* One bit each, so a read modify write that is not atomic loses a bit that
     * belongs to somebody else and the total at the end says which. */
    (void)sync_atomic_uint32_or(&bits, 1u << me);
}

static void TestNothingIsLostWhenEveryShapeCountsAtOnce(TestingT *t) {
    plain = 0;
    typed = (SyncAtomicInt64){0};
    bits = (SyncAtomicUint32){0};

    run_all(t, count_everything);

    CHECK(sync_atomic_load_int64(&plain) == (int64_t)THREADS * ROUNDS);
    CHECK(sync_atomic_int64_load(&typed) == (int64_t)THREADS * ROUNDS);
    CHECK_INT_EQ(sync_atomic_uint32_load(&bits), (1u << THREADS) - 1u);
}

/* -------------------------------------------------------------- the value */

static SyncAtomicValue values[VALUES];
static Int cells[THREADS];
static SyncAtomicUint32 wrong;
static int32_t nils[THREADS];
static int32_t wins[THREADS];

static void reset_values(void) {
    memset(values, 0, sizeof values);
    memset(nils, 0, sizeof nils);
    memset(wins, 0, sizeof wins);
    wrong = (SyncAtomicUint32){0};
}

static void note_wrong(void) {
    (void)sync_atomic_uint32_add(&wrong, 1);
}

/* A load either sees nothing yet or sees a whole value. What it must never see
 * is the sentinel, a type with somebody else's data beside it, or a data word
 * that was never stored. */
static void check_seen(Any got) {
    if (BURROW_ANY_IS_NIL(got))
        return;
    if (got.t != TYPE_INT)
        note_wrong();
    else if ((Int *)got.data < cells || (Int *)got.data >= cells + THREADS)
        note_wrong();
}

static void store_and_read(void *arg) {
    size_t me = (size_t)(uintptr_t)arg;

    for (int i = 0; i < VALUES; i++) {
        check_seen(sync_atomic_value_load(&values[i]));
        sync_atomic_value_store(&values[i], BURROW_ANY(TYPE_INT, &cells[me]));

        /* After our own store there is definitely something there, so a nil
         * here would mean the store returned before it had published. */
        if (BURROW_ANY_IS_NIL(sync_atomic_value_load(&values[i])))
            note_wrong();
        check_seen(sync_atomic_value_load(&values[i]));
    }
}

static void TestAValueIsNeverSeenHalfStored(TestingT *t) {
    reset_values();

    run_all(t, store_and_read);

    CHECK_INT_EQ(sync_atomic_uint32_load(&wrong), 0);
    for (int i = 0; i < VALUES; i++)
        CHECK(!BURROW_ANY_IS_NIL(sync_atomic_value_load(&values[i])));
}

/* Exactly one thread does the first store of any Value, so exactly one swap per
 * Value comes back nil. Two would mean two threads got through the window, and
 * none would mean a swap invented a value nobody stored. */
static void swap_and_count(void *arg) {
    size_t me = (size_t)(uintptr_t)arg;

    for (int i = 0; i < VALUES; i++) {
        Any old = sync_atomic_value_swap(&values[i], BURROW_ANY(TYPE_INT, &cells[me]));

        if (BURROW_ANY_IS_NIL(old))
            nils[me]++;
        else
            check_seen(old);
    }
}

static void TestExactlyOneSwapPerValueFindsItEmpty(TestingT *t) {
    int32_t total = 0;

    reset_values();

    run_all(t, swap_and_count);

    for (size_t i = 0; i < THREADS; i++)
        total += nils[i];
    CHECK_INT_EQ(total, VALUES);
    CHECK_INT_EQ(sync_atomic_uint32_load(&wrong), 0);
}

/* The same question asked of compare and swap, which has the extra rule that a
 * nil old only matches a Value nothing has been stored to. One winner per
 * Value, and every loser has to be told no rather than quietly overwriting. */
static void race_to_claim(void *arg) {
    size_t me = (size_t)(uintptr_t)arg;

    for (int i = 0; i < VALUES; i++) {
        Any nothing = {NULL, NULL};

        if (sync_atomic_value_compare_and_swap(&values[i], nothing,
                                               BURROW_ANY(TYPE_INT, &cells[me])))
            wins[me]++;
        check_seen(sync_atomic_value_load(&values[i]));
    }
}

static void TestExactlyOneCompareAndSwapPerValueClaimsIt(TestingT *t) {
    int32_t total = 0;

    reset_values();

    run_all(t, race_to_claim);

    for (size_t i = 0; i < THREADS; i++)
        total += wins[i];
    CHECK_INT_EQ(total, VALUES);
    CHECK_INT_EQ(sync_atomic_uint32_load(&wrong), 0);
}

#define TESTS(X)                                                                       \
    X(TestNothingIsLostWhenEveryShapeCountsAtOnce)                                     \
    X(TestAValueIsNeverSeenHalfStored)                                                 \
    X(TestExactlyOneSwapPerValueFindsItEmpty)                                          \
    X(TestExactlyOneCompareAndSwapPerValueClaimsIt)

static int TestMain(TestingM *m) {
    for (size_t i = 0; i < THREADS; i++)
        cells[i] = (Int)i;
    int code = testing_m_run(m);
    return code;
}

TESTING_MAIN_WITH(TestMain, TESTS)
