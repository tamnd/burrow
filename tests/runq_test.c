/* Tests for the run queues.
 *
 * The goroutines here never run. They are 512 zeroed structures with an id in
 * each one, and everything below moves pointers to them around and checks which
 * ones came out where. That is the whole reason the queues are a separate file
 * from the scheduler: a queue that loses a goroutine one time in ten thousand
 * is almost impossible to see when the goroutines are also running, and it is
 * easy to see when they are standing still and the only thing that moves is the
 * queue.
 *
 * Two kinds of test in here. The single threaded ones check that the queues do
 * what the header says, including the parts that are easy to get backwards, like
 * which goroutine a full put hands back and where a displaced runnext ends up.
 * The last two run several threads at a lock free queue at once and check that
 * every goroutine came out exactly once, which is the only property that
 * matters and the only one a single threaded test cannot check at all.
 *
 * The Ps are file scope because a burrow__P is two kilobytes of ring with three
 * cache line aligned fields in it, and because a thief thread reads the one it
 * was given. Nothing here is reentrant and nothing needs to be.
 *
 * Copyright 2026 The burrow Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style licence that can be found
 * in the LICENSE file. */

#include "burrow/sched.h"

#include "burrow/atomic.h"
#include "burrow/clock.h"
#include "burrow/thread.h"

#include "harness.h"

#include <stdint.h>
#include <string.h>

/* Room for two full rings, which is the most any test below needs. */
#define POOL 512

static burrow__G pool[POOL];

/* Hands out goroutines with ids 1, 2, 3 and so on, so that a zeroed G is never
 * mistaken for a real one and so that the order things come out in is readable
 * in a failure message. */
static burrow__G *fresh(uint64_t id) {
    burrow__G *g = &pool[id - 1];
    memset(g, 0, sizeof(*g));
    g->id = id;
    g->status = BURROW_GRUNNABLE;
    return g;
}

static void reset_p(burrow__P *p, int32_t id) {
    memset(p, 0, sizeof(*p));
    p->id = id;
    p->status = BURROW_PRUNNING;
}

/* Every put in the single threaded tests goes through this, because a put that
 * silently overflowed would otherwise look like a put that worked and the test
 * after it would be checking the wrong queue. */
static bool put(burrow__P *p, burrow__G *g, bool next) {
    burrow__G *overflow = NULL;
    bool ok = burrow__runq_put(p, g, next, &overflow);
    if (!ok)
        CHECK(overflow != NULL);
    return ok;
}

static uint64_t id_of(const burrow__G *g) {
    return g == NULL ? 0 : g->id;
}

static burrow__P p1;
static burrow__P p2;

/* ------------------------------------------------------------------- the ring */

TEST(an_empty_queue_has_nothing_in_it) {
    reset_p(&p1, 1);

    CHECK(burrow__runq_get(&p1) == NULL);
    CHECK(burrow__runq_len(&p1) == 0);

    /* Twice, because the first get on an empty queue is the one that has to
     * leave the indices where it found them. */
    CHECK(burrow__runq_get(&p1) == NULL);
    CHECK(burrow__runq_len(&p1) == 0);
}

TEST(goroutines_come_off_the_ring_in_the_order_they_went_on) {
    reset_p(&p1, 1);

    for (uint64_t i = 1; i <= 8; i++)
        CHECK(put(&p1, fresh(i), false));

    CHECK(burrow__runq_len(&p1) == 8);

    for (uint64_t i = 1; i <= 8; i++)
        CHECK_INT_EQ(id_of(burrow__runq_get(&p1)), i);

    CHECK(burrow__runq_len(&p1) == 0);
    CHECK(burrow__runq_get(&p1) == NULL);
}

TEST(runnext_jumps_the_queue) {
    reset_p(&p1, 1);

    for (uint64_t i = 1; i <= 3; i++)
        CHECK(put(&p1, fresh(i), false));

    CHECK(put(&p1, fresh(99), true));
    CHECK(burrow__runq_len(&p1) == 4);

    /* The one that asked to go next goes next, and the three that were already
     * waiting keep their order behind it. */
    CHECK_INT_EQ(id_of(burrow__runq_get(&p1)), 99);
    CHECK_INT_EQ(id_of(burrow__runq_get(&p1)), 1);
    CHECK_INT_EQ(id_of(burrow__runq_get(&p1)), 2);
    CHECK_INT_EQ(id_of(burrow__runq_get(&p1)), 3);
    CHECK(burrow__runq_get(&p1) == NULL);
}

TEST(a_second_runnext_pushes_the_first_one_to_the_back) {
    reset_p(&p1, 1);

    CHECK(put(&p1, fresh(1), false));
    CHECK(put(&p1, fresh(50), true));
    CHECK(put(&p1, fresh(51), true));

    CHECK(burrow__runq_len(&p1) == 3);

    /* 51 took the slot, so 50 went to the tail of the ring, which puts it
     * behind 1. The slot is one deep and the goroutine that loses it is not
     * dropped and does not keep its place. */
    CHECK_INT_EQ(id_of(burrow__runq_get(&p1)), 51);
    CHECK_INT_EQ(id_of(burrow__runq_get(&p1)), 1);
    CHECK_INT_EQ(id_of(burrow__runq_get(&p1)), 50);
    CHECK(burrow__runq_get(&p1) == NULL);
}

TEST(runnext_on_its_own_is_found_and_counted) {
    reset_p(&p1, 1);

    CHECK(burrow__runq_len(&p1) == 0);
    CHECK(put(&p1, fresh(7), true));
    CHECK(burrow__runq_len(&p1) == 1);

    CHECK_INT_EQ(id_of(burrow__runq_get(&p1)), 7);
    CHECK(burrow__runq_len(&p1) == 0);
    CHECK(burrow__runq_get(&p1) == NULL);
}

TEST(the_ring_holds_exactly_its_size_and_then_says_so) {
    reset_p(&p1, 1);

    for (uint64_t i = 1; i <= BURROW_RUNQ_SIZE; i++)
        CHECK(put(&p1, fresh(i), false));

    CHECK(burrow__runq_len(&p1) == BURROW_RUNQ_SIZE);

    burrow__G *overflow = NULL;
    burrow__G *extra = fresh(BURROW_RUNQ_SIZE + 1);
    CHECK(!burrow__runq_put(&p1, extra, false, &overflow));
    CHECK(overflow == extra);

    /* The failed put changed nothing. A caller that goes on to the slow path
     * and then retries has to find the queue exactly as it left it. */
    CHECK(burrow__runq_len(&p1) == BURROW_RUNQ_SIZE);
    CHECK_INT_EQ(id_of(burrow__runq_get(&p1)), 1);
}

TEST(a_full_ring_still_takes_a_runnext_and_hands_back_the_old_one) {
    reset_p(&p1, 1);

    for (uint64_t i = 1; i <= BURROW_RUNQ_SIZE; i++)
        CHECK(put(&p1, fresh(i), false));

    /* The slot is empty, so this one lands in it and the full ring never comes
     * into it. */
    burrow__G *old_next = fresh(300);
    burrow__G *overflow = NULL;
    CHECK(burrow__runq_put(&p1, old_next, true, &overflow));

    /* Now the slot is taken and the ring is full, which is the only state where
     * the two goroutines a put is juggling are different from each other. */
    burrow__G *new_next = fresh(301);
    overflow = NULL;
    CHECK(!burrow__runq_put(&p1, new_next, true, &overflow));

    /* The one that needs a home is the goroutine that got knocked out of the
     * slot, not the one that was just handed in. Getting this backwards is the
     * bug the out parameter exists to prevent: 301 is safe in the slot and 300
     * is the one that would be lost. */
    CHECK(overflow == old_next);
    CHECK_INT_EQ(id_of(overflow), 300);

    CHECK_INT_EQ(id_of(burrow__runq_get(&p1)), 301);
}

TEST(the_indices_keep_working_after_the_ring_has_wrapped) {
    reset_p(&p1, 1);

    /* Four times round, one goroutine at a time, so that head and tail walk
     * past the end of the array over and over and the masking is the only thing
     * keeping them honest. */
    for (uint64_t round = 0; round < 4; round++) {
        for (uint64_t i = 1; i <= BURROW_RUNQ_SIZE; i++) {
            CHECK(put(&p1, fresh(i), false));
            CHECK_INT_EQ(id_of(burrow__runq_get(&p1)), i);
        }
    }

    CHECK(burrow__runq_len(&p1) == 0);
}

/* ------------------------------------------------------------ the global queue */

TEST(a_global_queue_is_first_in_first_out) {
    burrow__GQueue q;
    memset(&q, 0, sizeof(q));

    CHECK(burrow__gqueue_pop(&q) == NULL);
    CHECK(q.len == 0);

    for (uint64_t i = 1; i <= 5; i++)
        burrow__gqueue_push(&q, fresh(i));

    CHECK(q.len == 5);

    for (uint64_t i = 1; i <= 5; i++)
        CHECK_INT_EQ(id_of(burrow__gqueue_pop(&q)), i);

    CHECK(q.len == 0);
    CHECK(burrow__gqueue_pop(&q) == NULL);

    /* Emptied and used again, which is what catches a tail pointer that was
     * left pointing at the last goroutine to leave. */
    burrow__gqueue_push(&q, fresh(9));
    CHECK_INT_EQ(id_of(burrow__gqueue_pop(&q)), 9);
    CHECK(q.len == 0);
}

TEST(pushing_to_the_head_puts_a_goroutine_back_where_it_was) {
    burrow__GQueue q;
    memset(&q, 0, sizeof(q));

    /* On an empty queue, which is the case where the head and the tail both
     * have to end up pointing at the same goroutine. */
    burrow__gqueue_push_head(&q, fresh(1));
    CHECK(q.len == 1);

    burrow__gqueue_push(&q, fresh(2));
    burrow__gqueue_push_head(&q, fresh(3));

    CHECK(q.len == 3);
    CHECK_INT_EQ(id_of(burrow__gqueue_pop(&q)), 3);
    CHECK_INT_EQ(id_of(burrow__gqueue_pop(&q)), 1);
    CHECK_INT_EQ(id_of(burrow__gqueue_pop(&q)), 2);
    CHECK(burrow__gqueue_pop(&q) == NULL);
}

TEST(one_queue_can_be_poured_onto_the_end_of_another) {
    burrow__GQueue dst;
    burrow__GQueue src;
    memset(&dst, 0, sizeof(dst));
    memset(&src, 0, sizeof(src));

    /* Empty onto empty, which has to leave both of them usable. */
    burrow__gqueue_push_all(&dst, &src);
    CHECK(dst.len == 0);
    CHECK(src.len == 0);

    for (uint64_t i = 1; i <= 3; i++)
        burrow__gqueue_push(&src, fresh(i));

    /* Empty destination, so dst takes src's head as well as its tail. */
    burrow__gqueue_push_all(&dst, &src);
    CHECK(dst.len == 3);
    CHECK(src.len == 0);
    CHECK(src.head == NULL);
    CHECK(src.tail == NULL);

    for (uint64_t i = 4; i <= 6; i++)
        burrow__gqueue_push(&src, fresh(i));

    burrow__gqueue_push_all(&dst, &src);
    CHECK(dst.len == 6);
    CHECK(src.len == 0);

    for (uint64_t i = 1; i <= 6; i++)
        CHECK_INT_EQ(id_of(burrow__gqueue_pop(&dst)), i);

    CHECK(burrow__gqueue_pop(&dst) == NULL);
}

TEST(a_full_ring_gives_up_half_of_itself_and_the_new_goroutine) {
    reset_p(&p1, 1);

    burrow__GQueue batch;
    memset(&batch, 0, sizeof(batch));

    for (uint64_t i = 1; i <= BURROW_RUNQ_SIZE; i++)
        CHECK(put(&p1, fresh(i), false));

    burrow__G *extra = fresh(BURROW_RUNQ_SIZE + 1);
    CHECK(burrow__runq_put_slow(&p1, extra, &batch));

    /* Half the ring plus the one that did not fit. */
    CHECK_INT_EQ(batch.len, BURROW_RUNQ_SIZE / 2 + 1);
    CHECK_INT_EQ(burrow__runq_len(&p1), BURROW_RUNQ_SIZE / 2);

    /* The oldest half went, in order, and the new one is on the end of it. The
     * order matters: these goroutines have been waiting longest and putting
     * them on the global queue backwards would starve them for a second time. */
    for (uint64_t i = 1; i <= BURROW_RUNQ_SIZE / 2; i++)
        CHECK_INT_EQ(id_of(burrow__gqueue_pop(&batch)), i);

    CHECK_INT_EQ(id_of(burrow__gqueue_pop(&batch)), BURROW_RUNQ_SIZE + 1);
    CHECK(burrow__gqueue_pop(&batch) == NULL);

    /* The newer half is still where it was, and the P carries on from the
     * middle of the ring. */
    CHECK_INT_EQ(id_of(burrow__runq_get(&p1)), BURROW_RUNQ_SIZE / 2 + 1);

    /* And the put that was retried after all this now fits. */
    CHECK(put(&p1, fresh(BURROW_RUNQ_SIZE + 2), false));
}

TEST(the_slow_path_says_no_when_there_is_nothing_to_move) {
    reset_p(&p1, 1);

    burrow__GQueue batch;
    memset(&batch, 0, sizeof(batch));

    /* An empty ring has no half to give, and one goroutine has half of one,
     * which rounds down to none. Either way the caller has to go back and put
     * the goroutine on the ring the ordinary way. */
    CHECK(!burrow__runq_put_slow(&p1, fresh(1), &batch));
    CHECK(batch.len == 0);

    CHECK(put(&p1, fresh(2), false));
    CHECK(!burrow__runq_put_slow(&p1, fresh(3), &batch));
    CHECK(batch.len == 0);
    CHECK_INT_EQ(burrow__runq_len(&p1), 1);
}

/* ---------------------------------------------------------------- the stealing */

TEST(stealing_from_an_empty_p_takes_nothing) {
    reset_p(&p1, 1);
    reset_p(&p2, 2);

    CHECK(burrow__runq_steal(&p1, &p2, true) == NULL);
    CHECK(burrow__runq_len(&p1) == 0);
    CHECK(burrow__runq_len(&p2) == 0);
}

TEST(a_thief_takes_half_of_the_queue) {
    reset_p(&p1, 1);
    reset_p(&p2, 2);

    for (uint64_t i = 1; i <= 8; i++)
        CHECK(put(&p2, fresh(i), false));

    burrow__G *g = burrow__runq_steal(&p1, &p2, false);

    /* Four of the eight moved. One of those four is the answer and is not in
     * the thief's ring, which is why the thief has three and not four. */
    CHECK_INT_EQ(id_of(g), 4);
    CHECK_INT_EQ(burrow__runq_len(&p1), 3);
    CHECK_INT_EQ(burrow__runq_len(&p2), 4);

    /* The oldest half went, and the victim keeps the rest in order. */
    CHECK_INT_EQ(id_of(burrow__runq_get(&p1)), 1);
    CHECK_INT_EQ(id_of(burrow__runq_get(&p1)), 2);
    CHECK_INT_EQ(id_of(burrow__runq_get(&p1)), 3);
    CHECK(burrow__runq_get(&p1) == NULL);

    for (uint64_t i = 5; i <= 8; i++)
        CHECK_INT_EQ(id_of(burrow__runq_get(&p2)), i);
}

TEST(a_queue_of_one_is_worth_stealing) {
    reset_p(&p1, 1);
    reset_p(&p2, 2);

    CHECK(put(&p2, fresh(1), false));

    /* Half of one rounded down is nothing, which would make work stealing do
     * nothing at all on a program with one goroutine per P to spare. Rounding
     * up is the difference. */
    CHECK_INT_EQ(id_of(burrow__runq_steal(&p1, &p2, false)), 1);
    CHECK(burrow__runq_len(&p1) == 0);
    CHECK(burrow__runq_len(&p2) == 0);
}

TEST(runnext_is_only_taken_when_the_thief_asks_for_it) {
    reset_p(&p1, 1);
    reset_p(&p2, 2);

    CHECK(put(&p2, fresh(5), true));

    /* A thief that has other places left to look leaves the slot alone, because
     * that goroutine is the one the victim is about to run and moving it to
     * another core is the exact cache miss the slot exists to avoid. */
    CHECK(burrow__runq_steal(&p1, &p2, false) == NULL);
    CHECK_INT_EQ(burrow__runq_len(&p2), 1);

    /* On the last pass, with nowhere else to look, it takes it. */
    CHECK_INT_EQ(id_of(burrow__runq_steal(&p1, &p2, true)), 5);
    CHECK(burrow__runq_len(&p2) == 0);
    CHECK(burrow__runq_len(&p1) == 0);

    /* And the victim does not hand out the same goroutine twice. */
    CHECK(burrow__runq_get(&p2) == NULL);
}

TEST(the_ring_is_emptied_before_the_slot_is_touched) {
    reset_p(&p1, 1);
    reset_p(&p2, 2);

    CHECK(put(&p2, fresh(1), false));
    CHECK(put(&p2, fresh(2), false));
    CHECK(put(&p2, fresh(9), true));

    /* Two on the ring and one in the slot. Asking for the slot does not mean
     * taking the slot: it is the last resort and the ring comes first. */
    CHECK_INT_EQ(id_of(burrow__runq_steal(&p1, &p2, true)), 1);
    CHECK_INT_EQ(burrow__runq_len(&p2), 2);
    CHECK_INT_EQ(id_of(burrow__runq_get(&p2)), 9);
}

TEST(a_thief_can_steal_a_full_ring_without_overflowing_its_own) {
    reset_p(&p1, 1);
    reset_p(&p2, 2);

    for (uint64_t i = 1; i <= BURROW_RUNQ_SIZE; i++)
        CHECK(put(&p2, fresh(i), false));

    /* The largest steal there is: half of a full ring is 128, which is the most
     * that can land in a thief's ring in one go. The check in the steal that
     * throws on overflow is the one this is aimed at. */
    burrow__G *g = burrow__runq_steal(&p1, &p2, false);
    CHECK_INT_EQ(id_of(g), BURROW_RUNQ_SIZE / 2);
    CHECK_INT_EQ(burrow__runq_len(&p1), BURROW_RUNQ_SIZE / 2 - 1);
    CHECK_INT_EQ(burrow__runq_len(&p2), BURROW_RUNQ_SIZE / 2);

    for (uint64_t i = 1; i < BURROW_RUNQ_SIZE / 2; i++)
        CHECK_INT_EQ(id_of(burrow__runq_get(&p1)), i);

    CHECK(burrow__runq_get(&p1) == NULL);
}

/* ------------------------------------------------------------- several threads
 *
 * Everything above runs on one thread, which is enough to check that the queues
 * do what they say and no help at all with the question these were written for.
 * A lock free queue is wrong in the window between two instructions, so the
 * tests that matter are the ones with a real thread in that window.
 *
 * The property checked is the only one worth checking: every goroutine put in
 * comes out exactly once. Counting is not enough on its own, because a queue
 * that hands the same goroutine to two thieves and drops another one keeps the
 * count exactly right, so each goroutine gets a seen counter and all of them
 * have to end on one.
 *
 * These are timing tests, so a pass is weaker evidence than a failure. They run
 * enough goroutines through enough rounds to lose on any machine with more than
 * one core, and under ThreadSanitizer they are the thing that reads the atomics
 * rather than the comments about them. */

#define THIEVES 3
#define ROUNDS 200

struct Thief {
    burrow__Thread thread;
    burrow__P p;
    burrow__P *victim;
    uint32_t taken[POOL];
    uint32_t total;
};

static struct Thief thieves[THIEVES];
static uint32_t seen[POOL];
static uint32_t stress_stop;
static uint32_t thieves_running;

/* Steals until the flag goes up, draining whatever lands in its own ring as it
 * goes. Every goroutine it ends up holding is counted in its own array, so that
 * the counting itself never needs a lock and never becomes the thing being
 * measured. */
static void steal_loop(void *arg) {
    struct Thief *t = (struct Thief *)arg;

    (void)burrow__atomic_add_u32(&thieves_running, 1);

    while (burrow__atomic_load_acquire_u32(&stress_stop) == 0) {
        burrow__G *g = burrow__runq_steal(&t->p, t->victim, true);
        if (g == NULL) {
            /* Nothing to take, so get off the core rather than sitting in a
             * tight loop on somebody else's cache line. */
            burrow__thread_yield();
            continue;
        }

        do {
            t->taken[g->id - 1]++;
            t->total++;
        } while ((g = burrow__runq_get(&t->p)) != NULL);
    }

    /* One more pass after the flag, because the owner's last few puts can land
     * between the load above and the store that set it. */
    for (;;) {
        burrow__G *g = burrow__runq_steal(&t->p, t->victim, true);
        if (g == NULL)
            break;
        t->taken[g->id - 1]++;
        t->total++;
    }

    burrow__G *g;
    while ((g = burrow__runq_get(&t->p)) != NULL) {
        t->taken[g->id - 1]++;
        t->total++;
    }
}

/* Starts the thieves and does not come back until every one of them is in its
 * steal loop.
 *
 * The waiting is the point. Starting a thread is a request, and on a machine
 * with as many busy threads as cores the request can take longer to be granted
 * than the owner's whole loop takes to run. A thief that is still inside
 * thread_start when the stop flag goes up reads the flag once, finds it set, and
 * steals nothing, which looks from the outside exactly like a steal that is
 * broken. That is what the thief_total checks at the end of both tests are for,
 * so it is worth the few microseconds here to make sure they only ever fail for
 * the reason they are written to catch. It cost a red run on a four core virtual
 * machine to find that out.
 *
 * Answering false means a thread would not start, and the caller gives up rather
 * than waiting for a thief that is never going to arrive. */
static bool start_thieves(void) {
    stress_stop = 0;
    thieves_running = 0;

    for (int i = 0; i < THIEVES; i++) {
        memset(&thieves[i], 0, sizeof(thieves[i]));
        reset_p(&thieves[i].p, (int32_t)(i + 2));
        thieves[i].victim = &p1;
    }

    bool ok = true;
    for (int i = 0; i < THIEVES; i++)
        ok = burrow__thread_start(&thieves[i].thread, steal_loop, &thieves[i], 0) && ok;

    CHECK(ok);
    if (!ok)
        return false;

    while (burrow__atomic_load_acquire_u32(&thieves_running) < (uint32_t)THIEVES)
        burrow__thread_yield();

    return true;
}

TEST(nothing_is_lost_or_duplicated_while_thieves_are_running) {
    reset_p(&p1, 1);
    memset(seen, 0, sizeof(seen));

    for (uint64_t i = 1; i <= POOL; i++)
        (void)fresh(i);

    if (!start_thieves())
        return;

    /* The owner fills and drains its own queue over and over while the thieves
     * work at the other end of it.
     *
     * Four puts for every take, on purpose. A loop that takes one back out for
     * every one it puts in keeps the queue one goroutine deep whatever the
     * machine is doing, and a thief then has to win a race on a single
     * goroutine to get anything at all, which on a quiet machine it never does.
     * The first version of this test did that and passed everywhere while
     * proving nothing, so the owner here runs ahead of itself and leaves a real
     * backlog for the thieves to take half of. */
    uint32_t owner_total = 0;
    for (int round = 0; round < ROUNDS; round++) {
        for (uint64_t i = 1; i <= POOL; i++) {
            burrow__G *overflow = NULL;

            /* Every fourth one asks for the slot, so the thieves' last resort
             * path is exercised too. */
            bool next = (i % 4) == 0;

            if (!burrow__runq_put(&p1, &pool[i - 1], next, &overflow)) {
                /* The ring filled, which it will, since the thieves cannot keep
                 * up with a loop that does nothing else. The goroutine that did
                 * not fit is counted as run here, which is what the scheduler
                 * will do with it once there is a global queue to put it on
                 * instead. */
                seen[overflow->id - 1]++;
                owner_total++;
            }

            if ((i % 4) != 0)
                continue;

            burrow__G *g = burrow__runq_get(&p1);
            if (g != NULL) {
                seen[g->id - 1]++;
                owner_total++;
            }
        }
    }

    burrow__atomic_store_release_u32(&stress_stop, 1);

    for (int i = 0; i < THIEVES; i++)
        CHECK(burrow__thread_join(&thieves[i].thread));

    /* Whatever is left on the owner's queue after everybody has stopped. */
    for (;;) {
        burrow__G *g = burrow__runq_get(&p1);
        if (g == NULL)
            break;
        seen[g->id - 1]++;
        owner_total++;
    }

    uint32_t thief_total = 0;
    for (int i = 0; i < THIEVES; i++) {
        thief_total += thieves[i].total;
        for (int j = 0; j < POOL; j++)
            seen[j] += thieves[i].taken[j];
    }

    /* The same goroutines go round and round, so the expected count is the
     * number of puts and not the size of the pool. */
    uint32_t want = (uint32_t)ROUNDS * (uint32_t)POOL;
    CHECK_INT_EQ(owner_total + thief_total, want);

    uint32_t counted = 0;
    for (int j = 0; j < POOL; j++)
        counted += seen[j];

    CHECK_INT_EQ(counted, want);

    /* Each goroutine went in ROUNDS times and so has to have come out ROUNDS
     * times. A queue that duplicates one and drops another keeps the totals
     * above correct and fails here. */
    int wrong = 0;
    for (int j = 0; j < POOL; j++) {
        if (seen[j] != (uint32_t)ROUNDS)
            wrong++;
    }
    CHECK_INT_EQ(wrong, 0);

    /* At least one thief got something. Without this the whole test passes on a
     * steal that always returns NULL. */
    CHECK(thief_total > 0);
}

/* Holds the owner off long enough that a thief can win.
 *
 * A thief backs off before it touches a running victim's runnext, on purpose,
 * because the goroutine in that slot is the one the victim is about to run and
 * moving it to another core is work for nothing. So on a round where the owner
 * does no more than yield, the owner is supposed to win, and on a quiet machine
 * it wins every single time. Asking afterwards whether any steal succeeded is
 * then asking the runtime to be worse at its job, and it failed on an idle six
 * core box for exactly that reason.
 *
 * Fifty microseconds is a long time next to a yield and a compare and swap, so a
 * thief that is working takes the slot on every one of these rounds, and a run
 * where none of them do is a broken steal rather than a quiet machine. */
static void let_a_thief_in(void) {
    int64_t end = burrow__nanotime() + 50000;

    while (burrow__nanotime() < end)
        burrow__thread_yield();
}

/* The same idea with the queue kept nearly empty instead of nearly full, which
 * is a different set of windows. A steal from a queue with one goroutine on it
 * races with the owner taking that same goroutine, and the runnext slot is
 * contended by two threads doing a compare and swap on one pointer rather than
 * being quietly ignored because the ring always has something better in it. */
TEST(one_goroutine_at_a_time_is_never_handed_to_two_threads) {
    reset_p(&p1, 1);
    memset(seen, 0, sizeof(seen));

    for (uint64_t i = 1; i <= POOL; i++)
        (void)fresh(i);

    if (!start_thieves())
        return;

    uint32_t owner_total = 0;
    uint32_t rounds = 0;

    /* How many rounds ended with the owner's own get coming back empty, which
     * on this queue can only mean a thief took the goroutine out of the slot.
     *
     * This is the same fact as the thief counters at the end, read from the
     * owner's side, and it is read this way for two reasons. The thief counters
     * are plain words written by running threads and cannot be read until those
     * threads are joined, and by then it is too late to do anything about an
     * answer of zero. This one can be watched as it goes. */
    uint32_t taken_from_me = 0;

    /* When to stop waiting for a steal that is not coming, in the case below
     * where the fixed rounds produced none.
     *
     * Two seconds is a very long time for three threads that are already awake
     * and already spinning on the slot, and the number is that large on purpose.
     * The machines this runs on include a four core box that sits at a load
     * average of sixty, where a thread that is perfectly healthy still waits
     * tens of milliseconds for a core. The thing being ruled out is a steal that
     * never works, not a machine that is busy, and two seconds tells those two
     * apart with room to spare while costing nothing at all on a run where the
     * fixed rounds already saw a steal. */
    int64_t give_up = burrow__nanotime() + 2000000000;

    for (int round = 0;
         round < ROUNDS * 20 || (taken_from_me == 0 && burrow__nanotime() < give_up);
         round++) {
        burrow__G *overflow = NULL;
        burrow__G *g = &pool[round % POOL];

        /* Into the slot every time, so that the thieves and the owner are all
         * reaching for the same pointer. */
        if (!burrow__runq_put(&p1, g, true, &overflow)) {
            seen[overflow->id - 1]++;
            owner_total++;
        }

        rounds++;

        /* Off the core between the put and the get, which is what makes this
         * test do anything. Without it the owner takes back what it just put
         * before a thief has finished reading the pointer, the thieves lose
         * every race, and a steal that was broken in exactly the way this is
         * looking for would never get the chance to prove it. With it, the
         * queue is sitting there holding one goroutine while three other
         * threads reach for it, and the get below has to come back empty
         * whenever one of them got there first.
         *
         * Most rounds are a bare yield, because a narrow window is the one that
         * catches a broken steal. Every eighth round is wide enough that a
         * thief which gets a core at all will win it, and so is every round
         * past the fixed count, which are the rounds that only happen when
         * nothing has been stolen yet. */
        if (round >= ROUNDS * 20 || (round % 8) == 0)
            let_a_thief_in();
        else
            burrow__thread_yield();

        burrow__G *got = burrow__runq_get(&p1);
        if (got == NULL) {
            taken_from_me++;
        } else {
            seen[got->id - 1]++;
            owner_total++;
        }
    }

    burrow__atomic_store_release_u32(&stress_stop, 1);

    for (int i = 0; i < THIEVES; i++)
        CHECK(burrow__thread_join(&thieves[i].thread));

    for (;;) {
        burrow__G *g = burrow__runq_get(&p1);
        if (g == NULL)
            break;
        seen[g->id - 1]++;
        owner_total++;
    }

    uint32_t thief_total = 0;
    for (int i = 0; i < THIEVES; i++) {
        thief_total += thieves[i].total;
        for (int j = 0; j < POOL; j++)
            seen[j] += thieves[i].taken[j];
    }

    CHECK_INT_EQ(owner_total + thief_total, rounds);

    /* At least one steal happened, without which everything above passes on a
     * steal that always answers NULL.
     *
     * Both halves of this are worth having. The first is what the owner watched
     * happen while it was running and is what the loop above waits for. The
     * second is the thieves agreeing that they were the ones who took them,
     * which is not the same statement: a queue that handed a goroutine to
     * nobody at all would satisfy the first and fail the second. */
    CHECK(taken_from_me > 0);
    CHECK(thief_total >= taken_from_me);
}

int main(void) {
    RUN(an_empty_queue_has_nothing_in_it);
    RUN(goroutines_come_off_the_ring_in_the_order_they_went_on);
    RUN(runnext_jumps_the_queue);
    RUN(a_second_runnext_pushes_the_first_one_to_the_back);
    RUN(runnext_on_its_own_is_found_and_counted);
    RUN(the_ring_holds_exactly_its_size_and_then_says_so);
    RUN(a_full_ring_still_takes_a_runnext_and_hands_back_the_old_one);
    RUN(the_indices_keep_working_after_the_ring_has_wrapped);

    RUN(a_global_queue_is_first_in_first_out);
    RUN(pushing_to_the_head_puts_a_goroutine_back_where_it_was);
    RUN(one_queue_can_be_poured_onto_the_end_of_another);
    RUN(a_full_ring_gives_up_half_of_itself_and_the_new_goroutine);
    RUN(the_slow_path_says_no_when_there_is_nothing_to_move);

    RUN(stealing_from_an_empty_p_takes_nothing);
    RUN(a_thief_takes_half_of_the_queue);
    RUN(a_queue_of_one_is_worth_stealing);
    RUN(runnext_is_only_taken_when_the_thief_asks_for_it);
    RUN(the_ring_is_emptied_before_the_slot_is_touched);
    RUN(a_thief_can_steal_a_full_ring_without_overflowing_its_own);

    RUN(nothing_is_lost_or_duplicated_while_thieves_are_running);
    RUN(one_goroutine_at_a_time_is_never_handed_to_two_threads);
    return harness_report("runq");
}
