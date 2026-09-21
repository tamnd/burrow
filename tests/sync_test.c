/* Tests for sync.Mutex and sync.RWMutex.
 *
 * In three parts, for the same reason tests/chan_test.c is in two.
 *
 * The first part never starts the runtime. A lock that nobody is contending is
 * a compare and swap on a word, and checking the rules about what a zero value
 * is, what try answers and what an unlock of nothing does needs no scheduler
 * underneath, so a failure there points at the lock rather than at the lock or
 * the scheduler or the timing.
 *
 * The second part starts the runtime and contends for real, because the queue
 * is the interesting half and a queue with nobody in it is not a queue. Those
 * tests follow the rule tests/sched_test.c sets out: only the main goroutine
 * calls CHECK, and anything a child goroutine finds out it says through an
 * atomic, since the harness counts its checks in two plain ints.
 *
 * The third part uses host threads that are not goroutines, which Go has no
 * equivalent of and burrow has to support: burrow is a library inside somebody
 * else's program and that program's threads are allowed to take a lock. Such a
 * thread sleeps on a note instead of parking, which is a different path through
 * the semaphore and therefore worth its own tests.
 *
 * Every assertion about contention is a count that was known in advance. A test
 * that says "and it did not crash" is a test that passes on a lock which lets
 * two goroutines into the critical section, so each one here either counts
 * something whose total is arithmetic or watches for an overlap that must never
 * happen and reports how many times it did.
 *
 * Copyright 2026 The burrow Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style licence that can be found
 * in the LICENSE file. */

#include "burrow/sync.h"

#include "burrow/atomic.h"
#include "burrow/clock.h"
#include "burrow/func.h"
#include "burrow/proc.h"
#include "burrow/sync/atomic.h"
#include "burrow/thread.h"

#include "burrow/panic.h"
#include "burrow/runtime.h"

#include "fatal.h"
#include "harness.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

/* ----------------------------------------------------- without a scheduler */

TEST(a_zero_mutex_is_an_unlocked_one) {
    SyncMutex m;
    memset(&m, 0, sizeof(m));

    CHECK(sync_mutex_try_lock(&m));
    CHECK(!sync_mutex_try_lock(&m));
    sync_mutex_unlock(&m);

    CHECK(sync_mutex_try_lock(&m));
    sync_mutex_unlock(&m);

    /* And the plain calls agree with try about what locked means. */
    sync_mutex_lock(&m);
    CHECK(!sync_mutex_try_lock(&m));
    sync_mutex_unlock(&m);
}

TEST(a_zero_rw_mutex_is_an_unlocked_one) {
    SyncRWMutex rw;
    memset(&rw, 0, sizeof(rw));

    CHECK(sync_rw_mutex_try_lock(&rw));
    CHECK(!sync_rw_mutex_try_lock(&rw));
    CHECK(!sync_rw_mutex_try_r_lock(&rw));
    sync_rw_mutex_unlock(&rw);

    CHECK(sync_rw_mutex_try_r_lock(&rw));
    sync_rw_mutex_r_unlock(&rw);
}

TEST(readers_do_not_keep_each_other_out) {
    SyncRWMutex rw;
    memset(&rw, 0, sizeof(rw));

    sync_rw_mutex_r_lock(&rw);
    sync_rw_mutex_r_lock(&rw);
    CHECK(sync_rw_mutex_try_r_lock(&rw));

    /* Three readers are in, so a writer cannot be. */
    CHECK(!sync_rw_mutex_try_lock(&rw));

    sync_rw_mutex_r_unlock(&rw);
    sync_rw_mutex_r_unlock(&rw);
    CHECK(!sync_rw_mutex_try_lock(&rw));

    /* The last one out is what lets the writer in. */
    sync_rw_mutex_r_unlock(&rw);
    CHECK(sync_rw_mutex_try_lock(&rw));
    sync_rw_mutex_unlock(&rw);
}

TEST(a_locker_is_whichever_lock_it_came_from) {
    SyncMutex m;
    SyncRWMutex rw;
    memset(&m, 0, sizeof(m));
    memset(&rw, 0, sizeof(rw));

    SyncLocker l = sync_mutex_locker(&m);
    CHECK(l.data == &m);
    CHECK(l.vt->self_type == TYPE_SYNC_MUTEX);

    sync_locker_lock(l);
    CHECK(!sync_mutex_try_lock(&m));
    sync_locker_unlock(l);
    CHECK(sync_mutex_try_lock(&m));
    sync_mutex_unlock(&m);

    /* The write side keeps everybody out. */
    SyncLocker w = sync_rw_mutex_locker(&rw);
    CHECK(w.vt->self_type == TYPE_SYNC_RW_MUTEX);
    sync_locker_lock(w);
    CHECK(!sync_rw_mutex_try_r_lock(&rw));
    sync_locker_unlock(w);

    /* The read side keeps only writers out. */
    SyncLocker r = sync_rw_mutex_r_locker(&rw);
    sync_locker_lock(r);
    CHECK(sync_rw_mutex_try_r_lock(&rw));
    sync_rw_mutex_r_unlock(&rw);
    CHECK(!sync_rw_mutex_try_lock(&rw));
    sync_locker_unlock(r);
    CHECK(sync_rw_mutex_try_lock(&rw));
    sync_rw_mutex_unlock(&rw);
}

TEST(unlocking_something_that_is_not_locked_stops_the_program) {
    SyncMutex m;
    SyncRWMutex rw1;
    SyncRWMutex rw2;
    memset(&m, 0, sizeof(m));
    memset(&rw1, 0, sizeof(rw1));
    memset(&rw2, 0, sizeof(rw2));

    CHECK_FATAL(sync_mutex_unlock(&m), "sync: unlock of unlocked mutex");
    CHECK_FATAL(sync_rw_mutex_unlock(&rw1), "sync: unlock of unlocked RWMutex");
    CHECK_FATAL(sync_rw_mutex_r_unlock(&rw2), "sync: r_unlock of unlocked RWMutex");
}

TEST(a_nil_locker_panics_rather_than_stopping_the_program) {
    SyncLocker nothing;
    memset(&nothing, 0, sizeof(nothing));

    CHECK_RUNTIME_ERROR(sync_locker_lock(nothing), "sync: lock of a nil Locker");
    CHECK_RUNTIME_ERROR(sync_locker_unlock(nothing), "sync: unlock of a nil Locker");
}

/* ------------------------------------------------------------ with goroutines
 *
 * Eight workers and two thousand rounds each is enough that every worker queues
 * many times on every machine this runs on, and small enough that the whole
 * file stays under a second. */

#define WORKERS 8
#define ROUNDS 2000

static SyncMutex mu;
static SyncRWMutex rw;

/* Under mu, so a plain int is the right kind of int for it. If the mutex is
 * broken this is a data race, which is exactly what the sanitizer build is
 * there to notice. */
static int64_t counter;
static int32_t inside;

/* Everything a worker reports, which has to be atomic because the harness's own
 * counters are not. */
static SyncAtomicUint32 finished;
static SyncAtomicUint32 overlaps;
static SyncAtomicInt32 readers_now;
static SyncAtomicUint32 writer_in;

static void reset(void) {
    memset(&mu, 0, sizeof(mu));
    memset(&rw, 0, sizeof(rw));
    counter = 0;
    inside = 0;
    sync_atomic_uint32_store(&finished, 0);
    sync_atomic_uint32_store(&overlaps, 0);
    sync_atomic_int32_store(&readers_now, 0);
    sync_atomic_uint32_store(&writer_in, 0);
}

/* The main goroutine's way of waiting for the others, since WaitGroup is the
 * next change and this one cannot use it yet. */
static void wait_for(uint32_t n) {
    while (sync_atomic_uint32_load(&finished) < n)
        runtime_gosched();
}

static void count_body(void *env) {
    (void)env;

    for (int i = 0; i < ROUNDS; i++) {
        sync_mutex_lock(&mu);

        /* Both of these are plain, and that is the assertion: if two goroutines
         * are ever in here at once the increment loses counts and the flag is
         * already set. */
        if (inside != 0)
            sync_atomic_uint32_add(&overlaps, 1);
        inside = 1;
        counter++;
        inside = 0;

        sync_mutex_unlock(&mu);
    }
    sync_atomic_uint32_add(&finished, 1);
}

static void count_main(void *env) {
    (void)env;

    for (int i = 0; i < WORKERS; i++) {
        if (!go(BURROW_FN(Func, count_body, NULL)))
            return;
    }
    wait_for(WORKERS);
}

TEST(every_increment_under_the_mutex_is_kept) {
    reset();
    runtime_main(BURROW_FN(Func, count_main, NULL));

    CHECK_INT_EQ(counter, (int64_t)WORKERS * ROUNDS);
    CHECK_INT_EQ(sync_atomic_uint32_load(&overlaps), 0);
    CHECK_INT_EQ(sync_atomic_uint32_load(&finished), WORKERS);
}

TEST(the_same_is_true_with_one_processor) {
    reset();
    (void)runtime_gomaxprocs(1);
    runtime_main(BURROW_FN(Func, count_main, NULL));
    (void)runtime_gomaxprocs(0);

    CHECK_INT_EQ(counter, (int64_t)WORKERS * ROUNDS);
    CHECK_INT_EQ(sync_atomic_uint32_load(&overlaps), 0);
}

/* ------------------------------------------------------------- starvation
 *
 * Holding the lock for longer than the threshold is what puts the mutex into
 * the mode where it hands the lock to the front of the queue instead of letting
 * whoever is running take it. Two milliseconds is comfortably over the one
 * millisecond threshold on every machine, including the ones where the clock is
 * coarse.
 *
 * The count is still the assertion. What this adds is that it is a count taken
 * through the handoff path, which the tests above never reach. */

#define HOLD_NS 2000000
#define STARVE_ROUNDS 2

static void spin_for(int64_t ns) {
    int64_t until = burrow__nanotime() + ns;

    while (burrow__nanotime() < until)
        burrow__atomic_spin_hint();
}

static void starve_body(void *env) {
    (void)env;

    for (int i = 0; i < STARVE_ROUNDS; i++) {
        sync_mutex_lock(&mu);

        if (inside != 0)
            sync_atomic_uint32_add(&overlaps, 1);
        inside = 1;
        counter++;
        spin_for(HOLD_NS);
        inside = 0;

        sync_mutex_unlock(&mu);
    }
    sync_atomic_uint32_add(&finished, 1);
}

static void starve_main(void *env) {
    (void)env;

    for (int i = 0; i < 4; i++) {
        if (!go(BURROW_FN(Func, starve_body, NULL)))
            return;
    }
    wait_for(4);
}

TEST(a_lock_held_past_the_threshold_still_lets_everybody_through) {
    reset();
    runtime_main(BURROW_FN(Func, starve_main, NULL));

    CHECK_INT_EQ(counter, (int64_t)4 * STARVE_ROUNDS);
    CHECK_INT_EQ(sync_atomic_uint32_load(&overlaps), 0);
    CHECK_INT_EQ(sync_atomic_uint32_load(&finished), 4);
}

/* ----------------------------------------------------------------- rw mutex */

#define RW_ROUNDS 400

static void reader_body(void *env) {
    (void)env;

    for (int i = 0; i < RW_ROUNDS; i++) {
        sync_rw_mutex_r_lock(&rw);

        sync_atomic_int32_add(&readers_now, 1);
        if (sync_atomic_uint32_load(&writer_in) != 0)
            sync_atomic_uint32_add(&overlaps, 1);
        sync_atomic_int32_add(&readers_now, -1);

        sync_rw_mutex_r_unlock(&rw);
    }
    sync_atomic_uint32_add(&finished, 1);
}

static void writer_body(void *env) {
    (void)env;

    for (int i = 0; i < RW_ROUNDS; i++) {
        sync_rw_mutex_lock(&rw);

        /* Atomic because the readers write them without the write lock. Under
         * this lock nobody should be doing either, and the swap is what says so:
         * finding the flag already set means a second writer got in. */
        if (sync_atomic_uint32_swap(&writer_in, 1) != 0)
            sync_atomic_uint32_add(&overlaps, 1);
        if (sync_atomic_int32_load(&readers_now) != 0)
            sync_atomic_uint32_add(&overlaps, 1);
        counter++;
        sync_atomic_uint32_store(&writer_in, 0);

        sync_rw_mutex_unlock(&rw);
    }
    sync_atomic_uint32_add(&finished, 1);
}

static void rw_main(void *env) {
    (void)env;

    for (int i = 0; i < 6; i++) {
        if (!go(BURROW_FN(Func, reader_body, NULL)))
            return;
    }
    for (int i = 0; i < 2; i++) {
        if (!go(BURROW_FN(Func, writer_body, NULL)))
            return;
    }
    wait_for(8);
}

TEST(a_writer_is_never_inside_with_anybody_else) {
    reset();
    runtime_main(BURROW_FN(Func, rw_main, NULL));

    CHECK_INT_EQ(counter, (int64_t)2 * RW_ROUNDS);
    CHECK_INT_EQ(sync_atomic_uint32_load(&overlaps), 0);
    CHECK_INT_EQ(sync_atomic_uint32_load(&finished), 8);
    CHECK_INT_EQ(sync_atomic_int32_load(&readers_now), 0);

    /* And it is unlocked again at the end, both ways. */
    CHECK(sync_rw_mutex_try_lock(&rw));
    sync_rw_mutex_unlock(&rw);
}

/* -------------------------------------------------------- with host threads
 *
 * No scheduler at all here. Every one of these is a thread that is not running
 * a goroutine, so every wait goes through the note rather than through park,
 * which is the path Go does not have and therefore the one nothing else tests. */

static burrow__Thread threads[WORKERS];

static void thread_count_body(void *env) {
    (void)env;

    for (int i = 0; i < ROUNDS; i++) {
        sync_mutex_lock(&mu);

        if (inside != 0)
            sync_atomic_uint32_add(&overlaps, 1);
        inside = 1;
        counter++;
        inside = 0;

        sync_mutex_unlock(&mu);
    }
    sync_atomic_uint32_add(&finished, 1);
}

TEST(threads_that_are_not_goroutines_can_share_a_mutex) {
    reset();

    for (size_t i = 0; i < WORKERS; i++)
        CHECK(burrow__thread_start(&threads[i], thread_count_body, NULL, 0));
    for (size_t i = 0; i < WORKERS; i++)
        CHECK(burrow__thread_join(&threads[i]));

    CHECK_INT_EQ(counter, (int64_t)WORKERS * ROUNDS);
    CHECK_INT_EQ(sync_atomic_uint32_load(&overlaps), 0);
    CHECK_INT_EQ(sync_atomic_uint32_load(&finished), WORKERS);
}

static void thread_reader_body(void *env) {
    (void)env;

    for (int i = 0; i < RW_ROUNDS; i++) {
        sync_rw_mutex_r_lock(&rw);

        sync_atomic_int32_add(&readers_now, 1);
        if (sync_atomic_uint32_load(&writer_in) != 0)
            sync_atomic_uint32_add(&overlaps, 1);
        sync_atomic_int32_add(&readers_now, -1);

        sync_rw_mutex_r_unlock(&rw);
    }
    sync_atomic_uint32_add(&finished, 1);
}

static void thread_writer_body(void *env) {
    (void)env;

    for (int i = 0; i < RW_ROUNDS; i++) {
        sync_rw_mutex_lock(&rw);

        if (sync_atomic_uint32_swap(&writer_in, 1) != 0)
            sync_atomic_uint32_add(&overlaps, 1);
        if (sync_atomic_int32_load(&readers_now) != 0)
            sync_atomic_uint32_add(&overlaps, 1);
        counter++;
        sync_atomic_uint32_store(&writer_in, 0);

        sync_rw_mutex_unlock(&rw);
    }
    sync_atomic_uint32_add(&finished, 1);
}

TEST(threads_that_are_not_goroutines_can_share_an_rw_mutex) {
    reset();

    for (size_t i = 0; i < 6; i++)
        CHECK(burrow__thread_start(&threads[i], thread_reader_body, NULL, 0));
    for (size_t i = 6; i < 8; i++)
        CHECK(burrow__thread_start(&threads[i], thread_writer_body, NULL, 0));
    for (size_t i = 0; i < 8; i++)
        CHECK(burrow__thread_join(&threads[i]));

    CHECK_INT_EQ(counter, (int64_t)2 * RW_ROUNDS);
    CHECK_INT_EQ(sync_atomic_uint32_load(&overlaps), 0);
    CHECK_INT_EQ(sync_atomic_uint32_load(&finished), 8);
}

/* A goroutine and a thread on the same mutex, which is the case that has one
 * waiter parked and one asleep on a note in the same queue at the same time. */

static void mixed_thread(void *env) {
    (void)env;
    thread_count_body(NULL);
}

static void mixed_main(void *env) {
    (void)env;

    for (int i = 0; i < 4; i++) {
        if (!go(BURROW_FN(Func, count_body, NULL)))
            return;
    }
    wait_for(4 + 4);
}

TEST(a_goroutine_and_a_thread_can_queue_on_the_same_mutex) {
    reset();

    for (size_t i = 0; i < 4; i++)
        CHECK(burrow__thread_start(&threads[i], mixed_thread, NULL, 0));

    runtime_main(BURROW_FN(Func, mixed_main, NULL));

    for (size_t i = 0; i < 4; i++)
        CHECK(burrow__thread_join(&threads[i]));

    CHECK_INT_EQ(counter, (int64_t)8 * ROUNDS);
    CHECK_INT_EQ(sync_atomic_uint32_load(&overlaps), 0);
    CHECK_INT_EQ(sync_atomic_uint32_load(&finished), 8);
}

int main(void) {
    RUN(a_zero_mutex_is_an_unlocked_one);
    RUN(a_zero_rw_mutex_is_an_unlocked_one);
    RUN(readers_do_not_keep_each_other_out);
    RUN(a_locker_is_whichever_lock_it_came_from);
    RUN(unlocking_something_that_is_not_locked_stops_the_program);
    RUN(a_nil_locker_panics_rather_than_stopping_the_program);

    RUN(every_increment_under_the_mutex_is_kept);
    RUN(the_same_is_true_with_one_processor);
    RUN(a_lock_held_past_the_threshold_still_lets_everybody_through);
    RUN(a_writer_is_never_inside_with_anybody_else);

    RUN(threads_that_are_not_goroutines_can_share_a_mutex);
    RUN(threads_that_are_not_goroutines_can_share_an_rw_mutex);
    RUN(a_goroutine_and_a_thread_can_queue_on_the_same_mutex);

    return harness_report("sync");
}
