/* Tests for the monotonic clock.
 *
 * A clock is awkward to test, because the only thing it promises is a relation
 * between two readings and there is no oracle to check a reading against. What
 * can be checked is the relation itself, and these do: it goes forwards, it
 * never goes backwards no matter how hard it is read or from how many threads,
 * and a measured interval is close enough to a busy loop of a known length that
 * the units are nanoseconds and not microseconds or ticks.
 *
 * Nothing here checks a wall clock duration against a sleep, because that test
 * fails on a loaded machine and a test that fails on a loaded machine gets
 * disabled and then tests nothing. The upper bounds below are all loose enough
 * to survive a build running on every core at once.
 *
 * Copyright 2026 The burrow Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style licence that can be found
 * in the LICENSE file. */

#include "burrow/clock.h"

#include "burrow/atomic.h"
#include "burrow/thread.h"

#include "harness.h"

#include <stdbool.h>
#include <stdint.h>

/* ------------------------------------------------------------ it runs at all */

TEST(the_clock_returns_something_other_than_zero) {
    /* Zero is what every failure path in clock.c returns, and a machine whose
     * monotonic clock genuinely reads zero has been up for under a nanosecond.
     * So this is the test for the clock being wired up at all. */
    CHECK(burrow__nanotime() != 0);
}

TEST(two_readings_in_a_row_do_not_go_backwards) {
    int64_t a = burrow__nanotime();
    int64_t b = burrow__nanotime();

    CHECK(b >= a);
}

/* -------------------------------------------------------------- it goes up */

#define SAMPLES 100000

TEST(a_hundred_thousand_readings_never_go_backwards) {
    int64_t prev = burrow__nanotime();
    int64_t first = prev;

    for (int i = 0; i < SAMPLES; i++) {
        int64_t now = burrow__nanotime();
        CHECK(now >= prev);
        prev = now;
    }

    /* And it did move at some point over all of that. A clock that returns a
     * constant passes every check above and is useless. */
    CHECK(prev > first);
}

TEST(the_clock_moves_forward_while_a_loop_spins) {
    int64_t start = burrow__nanotime();

    /* Spun rather than slept, so this does not depend on a timer existing yet
     * and does not hand the core to anything else. */
    while (burrow__nanotime() - start < 1000000) {
        /* A millisecond of asking what time it is. */
    }

    CHECK(burrow__nanotime() - start >= 1000000);
}

/* -------------------------------------------------------------- the units */

TEST(a_measured_millisecond_is_a_millisecond_and_not_a_tick) {
    /* The point of this one is the scale and not the accuracy. If the
     * conversion factor were missing, or applied upside down, or the clock
     * counted microseconds, then a loop that runs until a million of these have
     * passed would take a thousand times too long or a thousand times too
     * little, and either way the second reading below lands outside a range
     * this wide. Twenty milliseconds of slack for one millisecond of work,
     * because the scheduler can take the core away in the middle. */
    int64_t start = burrow__nanotime();

    while (burrow__nanotime() - start < 1000000) {
    }

    int64_t taken = burrow__nanotime() - start;

    CHECK(taken >= 1000000);
    CHECK(taken < 20000000);
}

TEST(successive_readings_are_finer_than_a_microsecond_apart) {
    /* Resolution, loosely. Every platform this builds for reads the clock in
     * tens of nanoseconds, so out of a thousand tries at least one pair has to
     * differ by less than a microsecond. One pair and not all of them, since an
     * interrupt can land between any two of them. */
    bool fine = false;

    for (int i = 0; i < 1000 && !fine; i++) {
        int64_t a = burrow__nanotime();
        int64_t b = burrow__nanotime();

        if (b > a && b - a < 1000)
            fine = true;
    }

    CHECK(fine);
}

/* ------------------------------------------------------- the same everywhere */

/* Four threads reading at once, which is the case the cached conversion factor
 * on Windows and macOS has to survive, and which is also the only way to check
 * that all four are reading one clock rather than four differently scaled ones.
 *
 * The check is per reading and the order of the two operations in it is the
 * whole trick. Each thread loads the highest reading anybody has published
 * before it takes its own, so whatever it loaded was taken at an earlier moment
 * in real time than the reading it is about to take. A clock every thread agrees
 * about therefore cannot hand back anything smaller. Taking the two in the other
 * order proves nothing, because the value published in between could have been
 * read after this thread's own.
 *
 * Doing it this way also removes the timing assumption that the obvious version
 * of this test smuggles in. Comparing the first reading of one thread against
 * the last reading of another only works if the threads overlap, and on a
 * machine where this loop is quicker than starting a thread they do not. That
 * version failed on Linux, where the clock reads through the vdso and twenty
 * thousand readings are done before the fourth thread exists. */
#define READERS 4
#define READINGS 20000

static burrow__Thread readers[READERS];

/* The highest reading published so far, as a uint64_t because that is what the
 * atomics take. Every reading is positive, so nothing is lost by the cast. */
static uint64_t highest;
static uint32_t backwards;

static void read_the_clock(void *arg) {
    (void)arg;

    for (int i = 0; i < READINGS; i++) {
        uint64_t seen = burrow__atomic_load_acquire_u64(&highest);
        uint64_t now = (uint64_t)burrow__nanotime();

        if (now < seen)
            (void)burrow__atomic_add_u32(&backwards, 1);

        /* Published for the threads behind this one. The compare and swap can
         * lose to a higher reading, and losing to a higher one is the answer
         * already being correct, so there is nothing to retry. */
        while (seen < now && !burrow__atomic_cas_u64(&highest, &seen, now)) {
        }
    }
}

TEST(four_threads_read_one_clock_and_agree_about_it) {
    backwards = 0;
    highest = 0;

    for (size_t i = 0; i < READERS; i++)
        CHECK(burrow__thread_start(&readers[i], read_the_clock, NULL, 0));

    for (size_t i = 0; i < READERS; i++)
        CHECK(burrow__thread_join(&readers[i]));

    CHECK(burrow__atomic_load_u32(&backwards) == 0);

    /* And the threads did between them move the clock along, so the loop above
     * was not eighty thousand readings of a constant. */
    CHECK(burrow__atomic_load_u64(&highest) > 0);
}

int main(void) {
    RUN(the_clock_returns_something_other_than_zero);
    RUN(two_readings_in_a_row_do_not_go_backwards);
    RUN(a_hundred_thousand_readings_never_go_backwards);
    RUN(the_clock_moves_forward_while_a_loop_spins);
    RUN(a_measured_millisecond_is_a_millisecond_and_not_a_tick);
    RUN(successive_readings_are_finer_than_a_microsecond_apart);
    RUN(four_threads_read_one_clock_and_agree_about_it);
    return harness_report("clock");
}
