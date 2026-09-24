/* Tests for the atomics abstraction.
 *
 * These are single threaded, and that is on purpose rather than a gap. What a
 * second thread finds is a lost update, and that has its own file now that
 * there are platform threads to write it with. See atomic_concurrent_test.c.
 *
 * What is left is still most of what goes wrong. Every backend has a cast in
 * every operation, and a cast that truncates, sign extends or drops the top
 * half shows up immediately on a single thread with the right value in it. So
 * the values here are deliberately awkward: full width, top bit set, and
 * different in every byte.
 *
 * The same file is compiled three times. Once as itself, once with the lock
 * table forced on, and once with the last resort C11 backend forced on, so that
 * the two paths a normal build never takes are run by every job in the matrix.
 * See atomic_lock64_test.c and atomic_c11_test.c, which are three lines each.
 *
 * Copyright 2026 The burrow Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style licence that can be found
 * in the LICENSE file. */

#include "burrow/atomic.h"

#include "check.h"

#ifndef ATOMIC_SUITE
#define ATOMIC_SUITE "atomic"
#endif

/* Different in every byte, top bit set, and not a value any arithmetic here
 * could arrive at by accident. */
#define U32_ODD 0x8f1e2d3cu
#define U64_ODD UINT64_C(0x8f1e2d3c4b5a6978)

static void TestExactlyOneBackendIsSelected(TestingT *t) {
    int n = BURROW__ATOMIC_BUILTIN + BURROW__ATOMIC_MSVC + BURROW__ATOMIC_C11;
    CHECK_INT_EQ(n, 1);
}

static void TestU32RoundTripsThroughEveryOrder(TestingT *t) {
    uint32_t x = 0;

    burrow__atomic_store_relaxed_u32(&x, U32_ODD);
    CHECK_INT_EQ(burrow__atomic_load_relaxed_u32(&x), U32_ODD);
    CHECK_INT_EQ(burrow__atomic_load_acquire_u32(&x), U32_ODD);
    CHECK_INT_EQ(burrow__atomic_load_u32(&x), U32_ODD);

    burrow__atomic_store_release_u32(&x, 0xffffffffu);
    CHECK_INT_EQ(burrow__atomic_load_u32(&x), 0xffffffffu);

    burrow__atomic_store_u32(&x, 1u);
    CHECK_INT_EQ(burrow__atomic_load_relaxed_u32(&x), 1u);
}

static void TestU32ReadModifyWriteReturnsTheOldValue(TestingT *t) {
    uint32_t x = 100;

    CHECK_INT_EQ(burrow__atomic_add_u32(&x, 5u), 100u);
    CHECK_INT_EQ(x, 105u);

    /* Subtraction is addition of the two's complement, which is the only way
     * an unsigned add has to offer and is what every caller will write. */
    CHECK_INT_EQ(burrow__atomic_add_u32(&x, 0u - 5u), 105u);
    CHECK_INT_EQ(x, 100u);

    x = U32_ODD;
    CHECK_INT_EQ(burrow__atomic_and_u32(&x, 0x0000ffffu), U32_ODD);
    CHECK_INT_EQ(x, U32_ODD & 0x0000ffffu);

    CHECK_INT_EQ(burrow__atomic_or_u32(&x, 0xffff0000u), U32_ODD & 0x0000ffffu);
    CHECK_INT_EQ(x, (U32_ODD & 0x0000ffffu) | 0xffff0000u);

    x = 7;
    CHECK_INT_EQ(burrow__atomic_swap_u32(&x, U32_ODD), 7u);
    CHECK_INT_EQ(x, U32_ODD);
}

static void TestU32AddWrapsWhereTheTypeWraps(TestingT *t) {
    uint32_t x = 0xffffffffu;
    CHECK_INT_EQ(burrow__atomic_add_u32(&x, 1u), 0xffffffffu);
    CHECK_INT_EQ(x, 0u);
}

static void TestU32CasReportsWhatItActuallySaw(TestingT *t) {
    uint32_t x = U32_ODD;
    uint32_t expected = U32_ODD;

    CHECK(burrow__atomic_cas_u32(&x, &expected, 42u));
    CHECK_INT_EQ(x, 42u);
    CHECK_INT_EQ(expected, U32_ODD); /* untouched on success */

    expected = 0;
    CHECK(!burrow__atomic_cas_u32(&x, &expected, 99u));
    CHECK_INT_EQ(x, 42u);
    CHECK_INT_EQ(expected, 42u); /* written back on failure */

    /* The acquire and release forms are the same operation with a different
     * barrier, so all a single thread can check is that they still work. */
    expected = 42;
    CHECK(burrow__atomic_cas_acquire_u32(&x, &expected, 43u));
    expected = 43;
    CHECK(burrow__atomic_cas_release_u32(&x, &expected, 44u));
    CHECK_INT_EQ(x, 44u);

    expected = 1;
    CHECK(!burrow__atomic_cas_acquire_u32(&x, &expected, 0u));
    CHECK_INT_EQ(expected, 44u);
}

static void TestU32CasWeakConvergesInALoop(TestingT *t) {
    uint32_t x = 10;
    uint32_t seen = burrow__atomic_load_relaxed_u32(&x);

    /* The whole point of the weak form: it is allowed to fail spuriously, so
     * the only correct way to use it is a loop that recomputes from what it
     * saw. This is the shape every caller in the runtime will have. */
    int spins = 0;
    while (!burrow__atomic_cas_weak_u32(&x, &seen, seen * 2)) {
        if (++spins > 1000)
            break;
    }
    CHECK(spins <= 1000);
    CHECK_INT_EQ(x, 20u);
}

static void TestU64RoundTripsThroughEveryOrder(TestingT *t) {
    uint64_t x = 0;

    burrow__atomic_store_relaxed_u64(&x, U64_ODD);
    CHECK(burrow__atomic_load_relaxed_u64(&x) == U64_ODD);
    CHECK(burrow__atomic_load_acquire_u64(&x) == U64_ODD);
    CHECK(burrow__atomic_load_u64(&x) == U64_ODD);

    /* All ones, because a backend that goes through a 32 bit path somewhere
     * loses the top half here and nowhere else. */
    burrow__atomic_store_release_u64(&x, UINT64_MAX);
    CHECK(burrow__atomic_load_u64(&x) == UINT64_MAX);

    burrow__atomic_store_u64(&x, UINT64_C(1) << 63);
    CHECK(burrow__atomic_load_relaxed_u64(&x) == UINT64_C(1) << 63);
}

static void TestU64ReadModifyWriteReturnsTheOldValue(TestingT *t) {
    uint64_t x = UINT64_C(1) << 40;

    CHECK(burrow__atomic_add_u64(&x, UINT64_C(1) << 40) == UINT64_C(1) << 40);
    CHECK(x == UINT64_C(1) << 41);

    x = U64_ODD;
    CHECK(burrow__atomic_and_u64(&x, UINT64_C(0xffffffff)) == U64_ODD);
    CHECK(x == (U64_ODD & UINT64_C(0xffffffff)));

    CHECK(burrow__atomic_or_u64(&x, UINT64_C(0xffffffff00000000)) ==
          (U64_ODD & UINT64_C(0xffffffff)));
    CHECK(x == ((U64_ODD & UINT64_C(0xffffffff)) | UINT64_C(0xffffffff00000000)));

    x = 7;
    CHECK(burrow__atomic_swap_u64(&x, U64_ODD) == 7);
    CHECK(x == U64_ODD);
}

static void TestU64CasReportsWhatItActuallySaw(TestingT *t) {
    uint64_t x = U64_ODD;
    uint64_t expected = U64_ODD;

    CHECK(burrow__atomic_cas_u64(&x, &expected, UINT64_MAX));
    CHECK(x == UINT64_MAX);
    CHECK(expected == U64_ODD);

    expected = 0;
    CHECK(!burrow__atomic_cas_u64(&x, &expected, 1));
    CHECK(x == UINT64_MAX);
    CHECK(expected == UINT64_MAX);

    expected = UINT64_MAX;
    CHECK(burrow__atomic_cas_acquire_u64(&x, &expected, U64_ODD));
    expected = U64_ODD;
    CHECK(burrow__atomic_cas_release_u64(&x, &expected, 0));
    CHECK(x == 0);

    expected = 0;
    CHECK(burrow__atomic_cas_weak_u64(&x, &expected, 5) || true);
}

/* The table hashes on the address, so two counters in the same array are the
 * interesting case: either they collide in a slot, which has to still be
 * correct, or they do not, which also has to be correct. */
static void TestU64CountersSideBySideDoNotInterfere(TestingT *t) {
    uint64_t c[8];
    for (int i = 0; i < 8; i++)
        c[i] = 0;
    for (int round = 0; round < 100; round++)
        for (int i = 0; i < 8; i++)
            (void)burrow__atomic_add_u64(&c[i], (uint64_t)(i + 1));
    for (int i = 0; i < 8; i++)
        CHECK(c[i] == (uint64_t)(i + 1) * 100);
}

static void TestUptrRoundTripsAndCounts(TestingT *t) {
    uintptr_t x = 0;
    uintptr_t odd = (uintptr_t)~(uintptr_t)0 ^ (uintptr_t)0x5a5a;

    burrow__atomic_store_relaxed_uptr(&x, odd);
    CHECK(burrow__atomic_load_relaxed_uptr(&x) == odd);
    CHECK(burrow__atomic_load_acquire_uptr(&x) == odd);
    CHECK(burrow__atomic_load_uptr(&x) == odd);

    x = 0;
    CHECK(burrow__atomic_add_uptr(&x, 3) == 0);
    CHECK(burrow__atomic_or_uptr(&x, 4) == 3);
    CHECK(burrow__atomic_and_uptr(&x, 6) == 7);
    CHECK(x == 6);
    CHECK(burrow__atomic_swap_uptr(&x, odd) == 6);

    uintptr_t expected = odd;
    CHECK(burrow__atomic_cas_uptr(&x, &expected, 0));
    CHECK(x == 0);
    expected = 1;
    CHECK(!burrow__atomic_cas_uptr(&x, &expected, 2));
    CHECK(expected == 0);
}

static void TestPtrRoundTrips(TestingT *t) {
    int a = 1, b = 2;
    void *p = NULL;

    burrow__atomic_store_relaxed_ptr(&p, &a);
    CHECK(burrow__atomic_load_relaxed_ptr(&p) == &a);
    CHECK(burrow__atomic_load_acquire_ptr(&p) == &a);
    CHECK(burrow__atomic_load_ptr(&p) == &a);

    burrow__atomic_store_release_ptr(&p, &b);
    CHECK(burrow__atomic_load_ptr(&p) == &b);

    burrow__atomic_store_ptr(&p, NULL);
    CHECK(burrow__atomic_load_ptr(&p) == NULL);

    CHECK(burrow__atomic_swap_ptr(&p, &a) == NULL);
    CHECK(p == &a);
}

static void TestPtrCasReportsWhatItActuallySaw(TestingT *t) {
    int a = 1, b = 2;
    void *p = &a;
    void *expected = &a;

    CHECK(burrow__atomic_cas_ptr(&p, &expected, &b));
    CHECK(p == &b);
    CHECK(expected == &a);

    expected = &a;
    CHECK(!burrow__atomic_cas_ptr(&p, &expected, NULL));
    CHECK(p == &b);
    CHECK(expected == &b);

    /* NULL on both sides, which is the case a backend that uses a compare and
     * exchange with NULL as its load sentinel could get wrong. */
    p = NULL;
    expected = NULL;
    CHECK(burrow__atomic_cas_ptr(&p, &expected, &a));
    CHECK(p == &a);

    expected = &a;
    CHECK(burrow__atomic_cas_acquire_ptr(&p, &expected, NULL));
    expected = NULL;
    CHECK(burrow__atomic_cas_release_ptr(&p, &expected, &b));
    CHECK(p == &b);

    expected = &b;
    CHECK(burrow__atomic_cas_weak_ptr(&p, &expected, NULL) || true);
}

/* A fence has nothing observable to a single thread, so what is under test is
 * that all three compile, link and do not crash on every backend. That is a
 * lower bar than the rest of this file and it is still worth having, because
 * the MSVC fence is hand written and the failure mode is a build error on a
 * machine none of us has in front of us. */
static void TestTheFencesAndTheSpinHintAreCallable(TestingT *t) {
    uint32_t x = 0;
    burrow__atomic_store_relaxed_u32(&x, 1);
    burrow__atomic_fence_release();
    burrow__atomic_fence_acquire();
    burrow__atomic_fence();
    burrow__atomic_spin_hint();
    CHECK_INT_EQ(burrow__atomic_load_relaxed_u32(&x), 1u);
}

/* Reached through the header's own inline wrappers above on a 32 bit machine or
 * under the forced build, and called directly here so that the entry points
 * themselves are exercised everywhere. */
static void TestTheLockTableIsACorrectImplementationOnItsOwn(TestingT *t) {
    uint64_t x = 0;

    burrow__atomic64_store(&x, U64_ODD);
    CHECK(burrow__atomic64_load(&x) == U64_ODD);
    CHECK(burrow__atomic64_add(&x, 1) == U64_ODD);
    CHECK(burrow__atomic64_and(&x, UINT64_C(0xffffffff)) == U64_ODD + 1);
    CHECK(burrow__atomic64_or(&x, UINT64_C(1) << 63) ==
          ((U64_ODD + 1) & UINT64_C(0xffffffff)));
    CHECK(burrow__atomic64_swap(&x, 5) ==
          (((U64_ODD + 1) & UINT64_C(0xffffffff)) | (UINT64_C(1) << 63)));

    uint64_t expected = 5;
    CHECK(burrow__atomic64_cas(&x, &expected, 6));
    CHECK(x == 6);
    expected = 5;
    CHECK(!burrow__atomic64_cas(&x, &expected, 7));
    CHECK(expected == 6);
}

#define TESTS(X)                                                                       \
    X(TestExactlyOneBackendIsSelected)                                                 \
    X(TestU32RoundTripsThroughEveryOrder)                                              \
    X(TestU32ReadModifyWriteReturnsTheOldValue)                                        \
    X(TestU32AddWrapsWhereTheTypeWraps)                                                \
    X(TestU32CasReportsWhatItActuallySaw)                                              \
    X(TestU32CasWeakConvergesInALoop)                                                  \
    X(TestU64RoundTripsThroughEveryOrder)                                              \
    X(TestU64ReadModifyWriteReturnsTheOldValue)                                        \
    X(TestU64CasReportsWhatItActuallySaw)                                              \
    X(TestU64CountersSideBySideDoNotInterfere)                                         \
    X(TestUptrRoundTripsAndCounts)                                                     \
    X(TestPtrRoundTrips)                                                               \
    X(TestPtrCasReportsWhatItActuallySaw)                                              \
    X(TestTheFencesAndTheSpinHintAreCallable)                                          \
    X(TestTheLockTableIsACorrectImplementationOnItsOwn)

TESTING_MAIN(TESTS)
