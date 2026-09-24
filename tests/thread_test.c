/* Tests for the platform threads.
 *
 * A thread test that only checks a flag got set proves the thread ran and
 * nothing else, so these check the parts that are actually different between
 * pthreads and Windows: that the argument arrives, that a join waits rather
 * than returning early, that a detached thread still runs, that an identity is
 * an identity, and that a stack size is a request the system accepts.
 *
 * The handles are file scope rather than on the stack of the test, because the
 * running thread reads the function and the argument out of its own handle and
 * a handle that goes away first is a use after free. The header says so and
 * these tests are written the way the header says to write them.
 *
 * Copyright 2026 The burrow Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style licence that can be found
 * in the LICENSE file. */

/* For sched_getaffinity and sched_setaffinity, which the processor count test
 * uses to narrow the mask and put it back. Before every include. */
#if defined(__linux__) && !defined(_GNU_SOURCE)
#define _GNU_SOURCE
#endif

#include "burrow/thread.h"

#include "burrow/atomic.h"
#include "burrow/platform.h"

#include "check.h"

#include <stdint.h>
#include <string.h>

#if defined(__linux__)
#include <sched.h>
#endif

/* ------------------------------------------------------------ one at a time */

static uint32_t ran;
static void *got_arg;

static void set_ran(void *arg) {
    got_arg = arg;
    burrow__atomic_store_release_u32(&ran, 1);
}

static burrow__Thread one;

static void TestAThreadRunsAndGetsItsArgument(TestingT *t) {
    int marker = 42;

    ran = 0;
    got_arg = NULL;

    CHECK(burrow__thread_start(&one, set_ran, &marker, 0));
    CHECK(burrow__thread_join(&one));

    /* After a join the thread is finished, so this needs no atomic to be
     * correct. It is read with one anyway, because a test that would still pass
     * if the join did nothing is not testing the join. */
    CHECK(burrow__atomic_load_acquire_u32(&ran) == 1);
    CHECK(got_arg == &marker);
    CHECK(marker == 42);

    /* Joining twice is a caller bug and says so rather than crashing. */
    CHECK(!burrow__thread_join(&one));
}

static void TestAStackSizeIsARequestTheSystemTakes(TestingT *t) {
    /* Two megabytes, which every system accepts, then the sizes that are the
     * reason the clamp exists at all.
     *
     * 1024 is below every minimum there is. 16 kilobytes is at or below the
     * minimum on most systems and well below it on glibc arm64, which wants
     * 128, and that difference is what this test is really for: a build that
     * guesses a floor instead of asking for one passes here on amd64 and fails
     * on arm64. 100000 is not a multiple of any page size, which macOS refuses
     * unless it is rounded up. Every one of them has to start a thread. */
    static burrow__Thread big;
    static burrow__Thread tiny;
    static burrow__Thread small;
    static burrow__Thread odd;

    ran = 0;
    CHECK(burrow__thread_start(&big, set_ran, NULL, 2u * 1024u * 1024u));
    CHECK(burrow__thread_join(&big));
    CHECK(burrow__atomic_load_acquire_u32(&ran) == 1);

    ran = 0;
    CHECK(burrow__thread_start(&tiny, set_ran, NULL, 1024u));
    CHECK(burrow__thread_join(&tiny));
    CHECK(burrow__atomic_load_acquire_u32(&ran) == 1);

    ran = 0;
    CHECK(burrow__thread_start(&small, set_ran, NULL, 16u * 1024u));
    CHECK(burrow__thread_join(&small));
    CHECK(burrow__atomic_load_acquire_u32(&ran) == 1);

    ran = 0;
    CHECK(burrow__thread_start(&odd, set_ran, NULL, 100000u));
    CHECK(burrow__thread_join(&odd));
    CHECK(burrow__atomic_load_acquire_u32(&ran) == 1);
}

static void TestAHandleThatWasNeverStartedIsNotJoinable(TestingT *t) {
    burrow__Thread never;
    memset(&never, 0, sizeof never);

    CHECK(!burrow__thread_join(&never));
    CHECK(!burrow__thread_detach(&never));
    CHECK(!burrow__thread_start(&never, NULL, NULL, 0));
    CHECK(!burrow__thread_start(NULL, set_ran, NULL, 0));
}

/* ----------------------------------------------------------------- identity */

#define IDS 8

static uint64_t ids[IDS];
static burrow__Thread id_threads[IDS];

static void record_id(void *arg) {
    size_t i = (size_t)(uintptr_t)arg;

    uint64_t first = burrow__thread_self();
    uint64_t second = burrow__thread_self();

    /* Asking twice has to give the same answer, which is the whole of what
     * "stable for as long as that thread runs" means. */
    ids[i] = first == second ? first : 0;
}

static void TestEveryRunningThreadHasItsOwnIdentity(TestingT *t) {
    uint64_t mine = burrow__thread_self();
    CHECK(mine == burrow__thread_self());

    for (size_t i = 0; i < IDS; i++)
        CHECK(burrow__thread_start(&id_threads[i], record_id, (void *)(uintptr_t)i, 0));
    for (size_t i = 0; i < IDS; i++)
        CHECK(burrow__thread_join(&id_threads[i]));

    for (size_t i = 0; i < IDS; i++) {
        CHECK(ids[i] != 0);
        CHECK(ids[i] != mine);
        for (size_t j = i + 1; j < IDS; j++)
            CHECK(ids[i] != ids[j]);
    }
}

/* -------------------------------------------------------------- contention */

#define WORKERS 8
#define PER_WORKER 20000

static uint32_t counter32;
static uint64_t counter64;
static burrow__Thread workers[WORKERS];

static void count_up(void *arg) {
    (void)arg;
    for (int i = 0; i < PER_WORKER; i++) {
        (void)burrow__atomic_add_u32(&counter32, 1);
        (void)burrow__atomic_add_u64(&counter64, 1);
    }
}

static void TestEightThreadsAddingToOneCounterLoseNothing(TestingT *t) {
    counter32 = 0;
    counter64 = 0;

    for (size_t i = 0; i < WORKERS; i++)
        CHECK(burrow__thread_start(&workers[i], count_up, NULL, 0));
    for (size_t i = 0; i < WORKERS; i++)
        CHECK(burrow__thread_join(&workers[i]));

    CHECK(burrow__atomic_load_u32(&counter32) == (uint32_t)(WORKERS * PER_WORKER));
    CHECK(burrow__atomic_load_u64(&counter64) == (uint64_t)(WORKERS * PER_WORKER));
}

/* ---------------------------------------------------------- detach and yield */

static uint32_t detached_done;
static burrow__Thread detached;

static void finish(void *arg) {
    (void)arg;
    burrow__atomic_store_release_u32(&detached_done, 1);
}

static void TestADetachedThreadStillRuns(TestingT *t) {
    detached_done = 0;

    CHECK(burrow__thread_start(&detached, finish, NULL, 0));
    CHECK(burrow__thread_detach(&detached));

    /* There is nothing to wait on, which is the point of detaching, so this
     * waits the only way a caller without a join can: it looks, and it gives
     * the processor up while it is not its turn. That is also the only test
     * here that yield is doing something rather than merely being callable,
     * since on one processor this loop never finishes without it. */
    while (burrow__atomic_load_acquire_u32(&detached_done) == 0)
        burrow__thread_yield();

    CHECK(burrow__atomic_load_acquire_u32(&detached_done) == 1);
    CHECK(!burrow__thread_detach(&detached));
}

static void TestThereIsAtLeastOneProcessor(TestingT *t) {
    int n = burrow__thread_ncpu();

    CHECK(n >= 1);

    /* Not a real upper bound, a sanity check. A number in the millions means
     * something was read as the wrong width or the wrong sign. */
    CHECK(n < 1000000);
    CHECK(burrow__thread_ncpu() == n);
}

#if defined(BURROW_OS_LINUX)

/* The count has to be the processors this process may use rather than the
 * processors the machine has, because those are different numbers inside a
 * container with a cpuset and under taskset, and everything downstream of the
 * count gets the wrong answer when it picks the larger one. GOMAXPROCS becomes
 * a thread per core on a box where two cores are allowed, and a thread decides
 * to spin waiting for a lock on the strength of cores it cannot run on.
 *
 * Narrowing the mask to one processor here rather than trusting the caller to
 * have run the test under taskset, so that it proves something when somebody
 * runs the binary by hand. */
static void TestTheProcessorCountIsTheOnesThisProcessMayUse(TestingT *t) {
    cpu_set_t before;
    if (sched_getaffinity(0, sizeof(before), &before) != 0)
        return; /* Blocked by a sandbox or a seccomp filter. Nothing to prove. */

    if (CPU_COUNT(&before) < 2)
        return; /* Already pinned, so there is no narrowing left to do. */

    /* The lowest processor in the current mask, because a processor outside it
     * is one this process is not allowed to ask for. */
    size_t only = (size_t)CPU_SETSIZE;
    for (size_t i = 0; i < (size_t)CPU_SETSIZE && only == (size_t)CPU_SETSIZE; i++) {
        if (CPU_ISSET(i, &before))
            only = i;
    }
    CHECK(only < (size_t)CPU_SETSIZE);

    cpu_set_t narrow;
    CPU_ZERO(&narrow);
    CPU_SET(only, &narrow);
    if (sched_setaffinity(0, sizeof(narrow), &narrow) != 0)
        return;

    int narrowed = burrow__thread_ncpu();
    CHECK(sched_setaffinity(0, sizeof(before), &before) == 0);

    CHECK(narrowed == 1);
    CHECK(burrow__thread_ncpu() == CPU_COUNT(&before));
}

#endif

#if defined(BURROW_OS_LINUX)
#define TESTS_1(X) X(TestTheProcessorCountIsTheOnesThisProcessMayUse)
#else
#define TESTS_1(X)
#endif

#define TESTS(X)                                                                       \
    X(TestAThreadRunsAndGetsItsArgument)                                               \
    X(TestAStackSizeIsARequestTheSystemTakes)                                          \
    X(TestAHandleThatWasNeverStartedIsNotJoinable)                                     \
    X(TestEveryRunningThreadHasItsOwnIdentity)                                         \
    X(TestEightThreadsAddingToOneCounterLoseNothing)                                   \
    X(TestADetachedThreadStillRuns)                                                    \
    X(TestThereIsAtLeastOneProcessor)                                                  \
    TESTS_1(X)

TESTING_MAIN(TESTS)
