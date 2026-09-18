/* The atomics, under real contention.
 *
 * atomic_test.c is single threaded and says so: it exists to catch a cast that
 * truncates or sign extends, which shows up on one thread with the right value
 * in it. This file is the other half, and it could not be written until there
 * were platform threads to write it with.
 *
 * What it can find that the single threaded file cannot is a lost update. Every
 * test here is arranged so that the answer is known in advance and is only
 * reachable if nothing was lost: counters that are added to a fixed number of
 * times, a bitmask where each thread owns one bit, a compare and swap loop that
 * has to converge, and a pointer that is only ever swapped between values the
 * checker knows. A read modify write that is not atomic fails these within a
 * few milliseconds on any machine with more than one processor.
 *
 * The same file is compiled twice, the second time with the lock table forced
 * on, which is the only way the spin locks are run under contention on a 64 bit
 * machine. See atomic_concurrent_lock64_test.c, which is three lines.
 *
 * Copyright 2026 The burrow Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style licence that can be found
 * in the LICENSE file. */

#include "burrow/atomic.h"

#include "burrow/thread.h"

#include "harness.h"

#include <stdint.h>

#ifndef CONCURRENT_SUITE
#define CONCURRENT_SUITE "atomic/concurrent"
#endif

/* Eight threads is more than the processor count of a CI runner, which is the
 * point: threads that are preempted in the middle of a read modify write are
 * the ones that find a missing lock. The counts are small enough that the whole
 * file runs in well under a second under ThreadSanitizer, which is the slowest
 * thing it has to finish inside. */
#define THREADS 8
#define ROUNDS 20000

static burrow__Thread threads[THREADS];

static void run_all(burrow__ThreadFn fn) {
    for (size_t i = 0; i < THREADS; i++)
        CHECK(burrow__thread_start(&threads[i], fn, (void *)(uintptr_t)i, 0));
    for (size_t i = 0; i < THREADS; i++)
        CHECK(burrow__thread_join(&threads[i]));
}

/* ----------------------------------------------------------------- counting */

static uint32_t count32;
static uint64_t count64;
static uintptr_t countptr;

static void add_everything(void *arg) {
    (void)arg;
    for (int i = 0; i < ROUNDS; i++) {
        (void)burrow__atomic_add_u32(&count32, 1);
        (void)burrow__atomic_add_u64(&count64, 1);
        (void)burrow__atomic_add_uptr(&countptr, 1);
    }
}

TEST(nothing_is_lost_when_every_width_is_counted_at_once) {
    count32 = 0;
    count64 = 0;
    countptr = 0;

    run_all(add_everything);

    CHECK(burrow__atomic_load_u32(&count32) == (uint32_t)(THREADS * ROUNDS));
    CHECK(burrow__atomic_load_u64(&count64) == (uint64_t)(THREADS * ROUNDS));
    CHECK(burrow__atomic_load_uptr(&countptr) == (uintptr_t)(THREADS * ROUNDS));
}

/* The old value a read modify write returns is what a ticket lock and a wait
 * group are built on, so every thread keeps its own and the test checks that no
 * two threads were ever handed the same one. Summing them would not: a sum is
 * the same whether the numbers were 0 1 2 3 or 1 1 2 3. */
static uint32_t tickets;
static uint32_t seen[THREADS][64];

static void take_tickets(void *arg) {
    size_t me = (size_t)(uintptr_t)arg;
    for (size_t i = 0; i < 64; i++)
        seen[me][i] = burrow__atomic_add_u32(&tickets, 1);
}

TEST(no_two_threads_are_handed_the_same_old_value) {
    tickets = 0;
    for (size_t i = 0; i < THREADS; i++)
        for (size_t j = 0; j < 64; j++)
            seen[i][j] = UINT32_MAX;

    run_all(take_tickets);

    CHECK(burrow__atomic_load_u32(&tickets) == THREADS * 64);

    /* Every ticket from 0 to THREADS*64-1 was handed out exactly once. */
    static uint8_t handed[THREADS * 64];
    for (size_t i = 0; i < sizeof handed; i++)
        handed[i] = 0;

    for (size_t i = 0; i < THREADS; i++) {
        for (size_t j = 0; j < 64; j++) {
            uint32_t t = seen[i][j];
            CHECK(t < THREADS * 64);
            if (t < THREADS * 64)
                handed[t]++;
        }
    }
    for (size_t i = 0; i < sizeof handed; i++)
        CHECK(handed[i] == 1);
}

/* ---------------------------------------------------------------- bit masks */

/* Each thread owns one bit and flips it on and off, so or and and are being
 * tested rather than the memory ordering around them. An or that reads, writes
 * and misses a neighbour's flip clears a bit nobody asked it to clear, and the
 * check at the end is that every bit is back where it started. */
static uint64_t mask;

static void flip_my_bit(void *arg) {
    uint64_t bit = UINT64_C(1) << (unsigned)(uintptr_t)arg;
    for (int i = 0; i < ROUNDS; i++) {
        (void)burrow__atomic_or_u64(&mask, bit);
        (void)burrow__atomic_and_u64(&mask, ~bit);
    }
    (void)burrow__atomic_or_u64(&mask, bit);
}

TEST(or_and_and_do_not_lose_a_neighbours_bit) {
    mask = 0;

    run_all(flip_my_bit);

    /* Every thread's last act was to set its own bit, and none of them may have
     * cleared anybody else's on the way. */
    CHECK(burrow__atomic_load_u64(&mask) == (UINT64_C(1) << THREADS) - 1);
}

/* ------------------------------------------------------- compare and swap */

/* The loop every lock free structure is made of. Each thread adds its own
 * number through a compare and swap rather than through add, so the test is of
 * the loop converging rather than of the adder. */
static uint64_t cas_total;

static void add_through_cas(void *arg) {
    (void)arg;
    for (int i = 0; i < ROUNDS; i++) {
        uint64_t seen_value = burrow__atomic_load_u64(&cas_total);
        while (!burrow__atomic_cas_u64(&cas_total, &seen_value, seen_value + 1))
            ; /* seen_value now holds what was really there, so try again. */
    }
}

TEST(a_compare_and_swap_loop_converges_under_contention) {
    cas_total = 0;

    run_all(add_through_cas);

    CHECK(burrow__atomic_load_u64(&cas_total) == (uint64_t)(THREADS * ROUNDS));
}

/* The weak form is allowed to fail when nothing changed, so a loop around it
 * has to keep going rather than treat a failure as a lost race. On the
 * platforms where weak is a load linked and store conditional, this is where a
 * loop that stops after one failure spins forever or gets the wrong answer. */
static uint32_t weak_total;

static void add_through_weak_cas(void *arg) {
    (void)arg;
    for (int i = 0; i < ROUNDS; i++) {
        uint32_t seen_value = burrow__atomic_load_u32(&weak_total);
        while (!burrow__atomic_cas_weak_u32(&weak_total, &seen_value, seen_value + 1))
            ;
    }
}

TEST(the_weak_compare_and_swap_gets_there_too) {
    weak_total = 0;

    run_all(add_through_weak_cas);

    CHECK(burrow__atomic_load_u32(&weak_total) == (uint32_t)(THREADS * ROUNDS));
}

/* ----------------------------------------------------------------- pointers */

/* A publish and consume, which is what the pointer width is for. One side
 * swaps between two fully initialised structures and the other side reads
 * whichever is current and checks it is one of them and is intact. A torn
 * pointer read lands on neither and the check says so. */
typedef struct Payload {
    uint64_t a;
    uint64_t b;
} Payload;

static Payload left = {UINT64_C(0x1111111111111111), UINT64_C(0x2222222222222222)};
static Payload right = {UINT64_C(0x3333333333333333), UINT64_C(0x4444444444444444)};
static void *published;
static uint32_t torn;

static void publish_and_consume(void *arg) {
    bool writer = ((uintptr_t)arg % 2) == 0;

    for (int i = 0; i < ROUNDS; i++) {
        if (writer) {
            void *next = (i % 2 == 0) ? (void *)&left : (void *)&right;
            (void)burrow__atomic_swap_ptr(&published, next);
            continue;
        }

        const Payload *p = (const Payload *)burrow__atomic_load_acquire_ptr(&published);
        if (p == NULL)
            continue;
        if (p == &left && p->a == UINT64_C(0x1111111111111111))
            continue;
        if (p == &right && p->a == UINT64_C(0x3333333333333333))
            continue;
        (void)burrow__atomic_add_u32(&torn, 1);
    }
}

TEST(a_published_pointer_is_never_seen_half_written) {
    published = NULL;
    torn = 0;

    run_all(publish_and_consume);

    CHECK(burrow__atomic_load_u32(&torn) == 0);

    void *last = burrow__atomic_load_ptr(&published);
    CHECK(last == (void *)&left || last == (void *)&right);
}

int main(void) {
    RUN(nothing_is_lost_when_every_width_is_counted_at_once);
    RUN(no_two_threads_are_handed_the_same_old_value);
    RUN(or_and_and_do_not_lose_a_neighbours_bit);
    RUN(a_compare_and_swap_loop_converges_under_contention);
    RUN(the_weak_compare_and_swap_gets_there_too);
    RUN(a_published_pointer_is_never_seen_half_written);
    return harness_report(CONCURRENT_SUITE);
}
