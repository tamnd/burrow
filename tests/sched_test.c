/* Tests for the scheduler.
 *
 * Unlike tests/runq_test.c, where the goroutines are zeroed structures that
 * never run, everything in here really runs. That makes the tests slower and it
 * makes a broken change show up as a hang rather than as a failed check, so the
 * waits below are all bounded: a test that is waiting for something that is
 * never going to happen gives up after a while and fails, which is a great deal
 * easier to read in CI than a job that was killed after six hours.
 *
 * Two kinds of test again. The ones that set GOMAXPROCS to one are checking
 * exact behaviour, and they can, because with one P and one thread there is
 * nothing else to interleave with and the order is decided entirely by the
 * queues. The ones that set it higher are checking the properties that only show
 * up with several threads: that every goroutine ran exactly once, that work
 * started on one P is picked up by another, and that a park and a ready hand a
 * goroutine between threads without losing it.
 *
 * The rule for the multi threaded ones is that only the main goroutine calls
 * CHECK. The harness counts checks in two plain ints, and a goroutine on another
 * thread touching those is a data race in the test rather than in the thing
 * being tested, which is the most annoying kind to chase. So the goroutines
 * write into atomics and arrays, and the checking happens after runtime_main has
 * returned and joined every thread.
 *
 * runtime_main gets called many times here, once per test, which Go cannot do
 * and which is worth having: it means every test starts from a scheduler that
 * has just been built, and it puts the teardown path under test for free.
 *
 * Copyright 2026 The burrow Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style licence that can be found
 * in the LICENSE file. */

#include "burrow/proc.h"

#include "burrow/atomic.h"
#include "burrow/func.h"
#include "burrow/sched.h"
#include "burrow/thread.h"

#include "harness.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

/* How many times a waiting goroutine gives the processor away before it decides
 * that whatever it is waiting for is not coming. Every wait in here finishes in
 * a handful of turns when the scheduler is working, so this only costs anything
 * when a test is already failing. */
#define YIELD_LIMIT (2 * 1000 * 1000)

/* The same idea for a goroutine that has to spin rather than yield, because it
 * is waiting for another goroutine that has to be running at the same time. */
#define SPIN_LIMIT (100 * 1000 * 1000)

/* Waits for a counter to reach a value by giving the processor away, and answers
 * whether it got there. */
static bool wait_for(uint32_t *counter, uint32_t want) {
    for (long i = 0; i < YIELD_LIMIT; i++) {
        if (burrow__atomic_load_acquire_u32(counter) >= want)
            return true;
        runtime_gosched();
    }
    return false;
}

/* ------------------------------------------------------------------ the gate
 *
 * A one goroutine gate, which is the smallest thing that uses sched_park and
 * sched_ready the way a channel will. It is here rather than in the library
 * because it is only enough for one waiter, and the point of it is to exercise
 * the park and ready pair under the lock discipline the header describes. */

typedef struct Gate {
    burrow__Lock lock;
    Goroutine *waiter;
    uint32_t signalled;
} Gate;

static bool gate_unlock(Goroutine *g, void *arg) {
    (void)g;
    burrow__unlock(&((Gate *)arg)->lock);
    return true;
}

static void gate_wait(Gate *gt) {
    burrow__lock(&gt->lock);
    if (gt->signalled != 0) {
        gt->signalled = 0;
        burrow__unlock(&gt->lock);
        return;
    }
    gt->waiter = sched_current();
    sched_park(gate_unlock, gt);
}

static void gate_signal(Gate *gt) {
    burrow__lock(&gt->lock);
    Goroutine *w = gt->waiter;
    if (w == NULL) {
        gt->signalled = 1;
        burrow__unlock(&gt->lock);
        return;
    }
    gt->waiter = NULL;
    burrow__unlock(&gt->lock);
    sched_ready(w);
}

/* ------------------------------------------------------------- the basic ones */

static uint32_t main_ran;
static Goroutine *main_g;
static int count_at_start;

static void main_body(void *env) {
    (void)env;
    main_ran = 1;
    main_g = sched_current();
    count_at_start = runtime_numgoroutine();
}

TEST(the_function_handed_to_runtime_main_runs) {
    main_ran = 0;
    main_g = NULL;
    count_at_start = -1;

    CHECK(sched_current() == NULL);
    CHECK_INT_EQ(runtime_numgoroutine(), 0);

    runtime_main(BURROW_FN(Func, main_body, NULL));

    CHECK_INT_EQ(main_ran, 1);
    CHECK(main_g != NULL);
    CHECK_INT_EQ(count_at_start, 1);

    /* And the world is gone again once it returns. */
    CHECK(sched_current() == NULL);
    CHECK_INT_EQ(runtime_numgoroutine(), 0);
}

TEST(runtime_main_can_be_called_twice) {
    main_ran = 0;
    runtime_main(BURROW_FN(Func, main_body, NULL));
    CHECK_INT_EQ(main_ran, 1);

    main_ran = 0;
    runtime_main(BURROW_FN(Func, main_body, NULL));
    CHECK_INT_EQ(main_ran, 1);
}

static uint32_t one_ran;
static Goroutine *one_g;
static Goroutine *one_main_g;
static bool one_arrived;

static void one_child(void *env) {
    (void)env;
    one_g = sched_current();
    burrow__atomic_add_u32(&one_ran, 1);
}

static void one_body(void *env) {
    (void)env;
    one_main_g = sched_current();
    CHECK(go(BURROW_FN(Func, one_child, NULL)));
    one_arrived = wait_for(&one_ran, 1);
}

TEST(a_goroutine_runs) {
    one_ran = 0;
    one_g = NULL;
    one_main_g = NULL;
    one_arrived = false;

    (void)runtime_gomaxprocs(1);
    runtime_main(BURROW_FN(Func, one_body, NULL));

    CHECK(one_arrived);
    CHECK_INT_EQ(one_ran, 1);
    CHECK(one_g != NULL);

    /* Within one run. Comparing against a goroutine from an earlier run would
     * prove nothing, because a dead goroutine's structure is kept and handed out
     * again and the addresses repeat. */
    CHECK(one_g != one_main_g);
}

/* ---------------------------------------------------------------- the ordering
 *
 * With one P and one thread the order goroutines run in is decided entirely by
 * the queues, so it can be checked exactly. `go` puts the new goroutine in the
 * runnext slot and pushes whatever was there to the back of the ring, so the
 * last one started runs first and the rest run oldest first behind it. That is
 * Go's behaviour and it looks strange written down, which is the reason to pin
 * it here rather than rediscover it later. */

#define ORDERED 4
static int order_log[ORDERED + 1];
static int order_len;
static bool order_done;

static void order_child(void *env) {
    order_log[order_len++] = (int)(intptr_t)env;
}

static void order_body(void *env) {
    (void)env;
    for (int i = 0; i < ORDERED; i++)
        CHECK(go(BURROW_FN(Func, order_child, (void *)(intptr_t)(i + 1))));

    /* Everything above is still sitting in the queues, because nothing has given
     * the thread up yet. */
    CHECK_INT_EQ(order_len, 0);
    CHECK_INT_EQ(runtime_numgoroutine(), ORDERED + 1);

    runtime_gosched();
    order_done = true;
}

TEST(the_newest_goroutine_runs_first_and_the_rest_run_oldest_first) {
    memset(order_log, 0, sizeof order_log);
    order_len = 0;
    order_done = false;

    (void)runtime_gomaxprocs(1);
    runtime_main(BURROW_FN(Func, order_body, NULL));

    CHECK(order_done);
    CHECK_INT_EQ(order_len, ORDERED);
    CHECK_INT_EQ(order_log[0], ORDERED);
    CHECK_INT_EQ(order_log[1], 1);
    CHECK_INT_EQ(order_log[2], 2);
    CHECK_INT_EQ(order_log[3], 3);
}

static uint32_t yield_flag;
static int yield_turns;
static bool yield_saw_it;

static void yield_child(void *env) {
    (void)env;
    burrow__atomic_store_release_u32(&yield_flag, 1);
}

static void yield_body(void *env) {
    (void)env;
    CHECK(go(BURROW_FN(Func, yield_child, NULL)));
    while (burrow__atomic_load_acquire_u32(&yield_flag) == 0 &&
           yield_turns < YIELD_LIMIT) {
        yield_turns++;
        runtime_gosched();
    }
    yield_saw_it = burrow__atomic_load_acquire_u32(&yield_flag) != 0;
}

TEST(gosched_lets_another_goroutine_have_the_thread) {
    yield_flag = 0;
    yield_turns = 0;
    yield_saw_it = false;

    (void)runtime_gomaxprocs(1);
    runtime_main(BURROW_FN(Func, yield_body, NULL));

    CHECK(yield_saw_it);

    /* One turn is all it should take. The child is in the runnext slot, so the
     * first gosched runs it and the main goroutine comes back to a set flag. */
    CHECK_INT_EQ(yield_turns, 1);
}

static uint32_t exit_before;
static uint32_t exit_after;

static void exit_child(void *env) {
    (void)env;
    burrow__atomic_add_u32(&exit_before, 1);
    runtime_goexit();
    burrow__atomic_add_u32(&exit_after, 1); /* unreachable, and the point */
}

static void exit_body(void *env) {
    (void)env;
    CHECK(go(BURROW_FN(Func, exit_child, NULL)));
    CHECK(wait_for(&exit_before, 1));

    /* Another turn or two, so that anything the dead goroutine was going to do
     * after runtime_goexit would have had the chance to do it. */
    for (int i = 0; i < 8; i++)
        runtime_gosched();
}

TEST(goexit_ends_the_goroutine_where_it_stands) {
    exit_before = 0;
    exit_after = 0;

    (void)runtime_gomaxprocs(1);
    runtime_main(BURROW_FN(Func, exit_body, NULL));

    CHECK_INT_EQ(exit_before, 1);
    CHECK_INT_EQ(exit_after, 0);
}

/* ------------------------------------------------------------------ the counts */

static Gate hold_gate;
static uint32_t hold_parked;
static int count_with_parked;
static int count_after;

static void hold_child(void *env) {
    (void)env;
    burrow__atomic_add_u32(&hold_parked, 1);
    gate_wait(&hold_gate);
}

static void count_body(void *env) {
    (void)env;
    for (int i = 0; i < 3; i++)
        CHECK(go(BURROW_FN(Func, hold_child, NULL)));

    CHECK(wait_for(&hold_parked, 3));
    count_with_parked = runtime_numgoroutine();

    /* Only one of the three is holding the gate. The other two are parked
     * forever, which is the case runtime_main has to survive. */
    gate_signal(&hold_gate);
    runtime_gosched();
    count_after = runtime_numgoroutine();
}

TEST(numgoroutine_counts_parked_goroutines_too) {
    memset(&hold_gate, 0, sizeof hold_gate);
    hold_parked = 0;
    count_with_parked = -1;
    count_after = -1;

    (void)runtime_gomaxprocs(1);
    runtime_main(BURROW_FN(Func, count_body, NULL));

    CHECK_INT_EQ(count_with_parked, 4);
    CHECK_INT_EQ(count_after, 3);
}

TEST(gomaxprocs_reports_and_sets_before_the_scheduler_starts) {
    int cpus = runtime_numcpu();
    CHECK(cpus >= 1);

    /* Nothing has set it, so it answers with the machine. */
    CHECK_INT_EQ(runtime_gomaxprocs(0), cpus);

    CHECK_INT_EQ(runtime_gomaxprocs(3), cpus);
    CHECK_INT_EQ(runtime_gomaxprocs(0), 3);
    CHECK_INT_EQ(runtime_gomaxprocs(-1), 3);

    /* And it goes back to the machine's number once a run is over, because the
     * scheduler that was set up is gone. */
    main_ran = 0;
    runtime_main(BURROW_FN(Func, main_body, NULL));
    CHECK_INT_EQ(main_ran, 1);
    CHECK_INT_EQ(runtime_gomaxprocs(0), cpus);
}

static int procs_inside;
static int procs_changed;

static void procs_body(void *env) {
    (void)env;
    procs_inside = runtime_gomaxprocs(0);
    procs_changed = runtime_gomaxprocs(2);
    procs_inside = runtime_gomaxprocs(0);
}

TEST(gomaxprocs_does_not_change_while_the_scheduler_is_running) {
    (void)runtime_gomaxprocs(3);
    procs_inside = -1;
    procs_changed = -1;

    runtime_main(BURROW_FN(Func, procs_body, NULL));

    CHECK_INT_EQ(procs_changed, 3);
    CHECK_INT_EQ(procs_inside, 3);
}

/* ----------------------------------------------------------------- the stacks */

static volatile unsigned deep_sink;
static unsigned deep_result;
static bool deep_started;

/* Uses about four kilobytes of stack per level, and the volatile array is what
 * stops the compiler from noticing that none of it matters and giving the
 * function no frame at all. */
static unsigned deep(unsigned n) {
    volatile unsigned char buf[4096];
    for (size_t i = 0; i < sizeof buf; i += 512)
        buf[i] = (unsigned char)n;
    if (n == 0)
        return buf[0];
    return deep(n - 1) + buf[0];
}

static void deep_child(void *env) {
    (void)env;
    deep_result = deep(120);
    deep_sink = deep_result;
}

static void deep_body(void *env) {
    (void)env;
    deep_started = go_stack(BURROW_FN(Func, deep_child, NULL), 1024 * 1024);
    CHECK(deep_started);
    for (int i = 0; i < 8; i++)
        runtime_gosched();
}

TEST(a_goroutine_can_ask_for_a_bigger_stack_and_use_it) {
    deep_result = 0;
    deep_started = false;

    (void)runtime_gomaxprocs(1);
    runtime_main(BURROW_FN(Func, deep_body, NULL));

    CHECK(deep_started);

    /* Every level writes its own number and adds the one below, so the answer is
     * the sum from 0 to 120 truncated to a byte at each level. Checking that it
     * is not zero is enough: the point is that 120 frames of four kilobytes did
     * not run off the end of the stack. */
    CHECK(deep_result != 0);
}

static int gfree_after_death;
static int gfree_after_reuse;
static uint32_t reuse_ran;

static void reuse_child(void *env) {
    (void)env;
    burrow__atomic_add_u32(&reuse_ran, 1);
}

static void reuse_body(void *env) {
    (void)env;
    CHECK(go(BURROW_FN(Func, reuse_child, NULL)));
    CHECK(wait_for(&reuse_ran, 1));

    /* With one P there is only one place a dead goroutine can have gone. */
    gfree_after_death = burrow__allp(0)->gfree_count;

    CHECK(go(BURROW_FN(Func, reuse_child, NULL)));
    gfree_after_reuse = burrow__allp(0)->gfree_count;
    CHECK(wait_for(&reuse_ran, 2));
}

TEST(a_dead_goroutine_is_kept_and_handed_out_again) {
    gfree_after_death = -1;
    gfree_after_reuse = -1;
    reuse_ran = 0;

    (void)runtime_gomaxprocs(1);
    runtime_main(BURROW_FN(Func, reuse_body, NULL));

    CHECK_INT_EQ(gfree_after_death, 1);
    CHECK_INT_EQ(gfree_after_reuse, 0);
    CHECK_INT_EQ(reuse_ran, 2);
}

/* ------------------------------------------------------------- park and ready */

static Gate handoff_there;
static Gate handoff_back;
static int handoff_value;
static uint32_t handoff_done;
static bool handoff_finished;

static void handoff_child(void *env) {
    (void)env;
    gate_wait(&handoff_there);
    handoff_value = handoff_value * 2 + 1;
    gate_signal(&handoff_back);
    burrow__atomic_add_u32(&handoff_done, 1);
}

static void handoff_body(void *env) {
    (void)env;
    handoff_value = 20;
    CHECK(go(BURROW_FN(Func, handoff_child, NULL)));
    gate_signal(&handoff_there);
    gate_wait(&handoff_back);

    /* The child set it before it signalled, so this goroutine cannot be looking
     * at the old value unless park and ready lost the ordering. */
    handoff_finished = handoff_value == 41;
    CHECK(wait_for(&handoff_done, 1));
}

TEST(a_value_can_be_handed_between_two_goroutines_through_a_park) {
    memset(&handoff_there, 0, sizeof handoff_there);
    memset(&handoff_back, 0, sizeof handoff_back);
    handoff_value = 0;
    handoff_done = 0;
    handoff_finished = false;

    (void)runtime_gomaxprocs(2);
    runtime_main(BURROW_FN(Func, handoff_body, NULL));

    CHECK(handoff_finished);
    CHECK_INT_EQ(handoff_value, 41);
}

static Gate early_gate;
static uint32_t early_done;
static bool early_waited;

static void early_child(void *env) {
    (void)env;
    /* The signal has already happened, so this must not park at all. */
    gate_wait(&early_gate);
    early_waited = true;
    burrow__atomic_add_u32(&early_done, 1);
}

static void early_body(void *env) {
    (void)env;
    gate_signal(&early_gate);
    CHECK(go(BURROW_FN(Func, early_child, NULL)));
    CHECK(wait_for(&early_done, 1));
}

TEST(a_signal_that_arrives_before_the_wait_is_not_lost) {
    memset(&early_gate, 0, sizeof early_gate);
    early_done = 0;
    early_waited = false;

    (void)runtime_gomaxprocs(1);
    runtime_main(BURROW_FN(Func, early_body, NULL));

    CHECK(early_waited);
}

/* ------------------------------------------------------------ the crowded ones */

#define MANY 1024
static uint32_t many_seen[MANY];
static uint32_t many_done;
static int many_started;
static bool many_all_ran;

static void many_child(void *env) {
    burrow__atomic_add_u32((uint32_t *)env, 1);
    burrow__atomic_add_u32(&many_done, 1);
}

static void many_body(void *env) {
    (void)env;
    for (int i = 0; i < MANY; i++) {
        if (go(BURROW_FN(Func, many_child, &many_seen[i])))
            many_started++;
    }
    many_all_ran = wait_for(&many_done, MANY);
}

TEST(every_goroutine_runs_exactly_once_however_many_there_are) {
    memset(many_seen, 0, sizeof many_seen);
    many_done = 0;
    many_started = 0;
    many_all_ran = false;

    /* Four times the size of a P's ring, so most of these go to the global queue
     * on the way in and come back off it in batches. */
    (void)runtime_gomaxprocs(4);
    runtime_main(BURROW_FN(Func, many_body, NULL));

    CHECK_INT_EQ(many_started, MANY);
    CHECK(many_all_ran);
    CHECK_INT_EQ(many_done, MANY);

    int wrong = 0;
    for (int i = 0; i < MANY; i++) {
        if (many_seen[i] != 1)
            wrong++;
    }
    CHECK_INT_EQ(wrong, 0);
}

static uint32_t nested_done;
static bool nested_all_ran;

static void nested_leaf(void *env) {
    (void)env;
    burrow__atomic_add_u32(&nested_done, 1);
}

static void nested_child(void *env) {
    (void)env;
    for (int i = 0; i < 8; i++)
        (void)go(BURROW_FN(Func, nested_leaf, NULL));
    burrow__atomic_add_u32(&nested_done, 1);
}

static void nested_body(void *env) {
    (void)env;
    for (int i = 0; i < 8; i++)
        CHECK(go(BURROW_FN(Func, nested_child, NULL)));
    nested_all_ran = wait_for(&nested_done, 8 + 8 * 8);
}

TEST(a_goroutine_can_start_goroutines) {
    nested_done = 0;
    nested_all_ran = false;

    (void)runtime_gomaxprocs(4);
    runtime_main(BURROW_FN(Func, nested_body, NULL));

    CHECK(nested_all_ran);
    CHECK_INT_EQ(nested_done, 8 + 8 * 8);
}

/* Every goroutine here arrives and then waits for all the others to arrive,
 * without ever giving its thread up. That can only finish if the scheduler is
 * really running them at the same time on different threads, which means the
 * work one P was handed has to have been stolen by the others. If stealing is
 * broken this fails rather than hangs, which is the reason for the spin bound.
 *
 * The main goroutine parks instead of yielding in a loop, because a goroutine
 * that yields is still a goroutine that wants to run, and with one P per worker
 * it would take a turn that one of the workers needs. There is no preemption
 * yet, so a worker that does not get a thread does not get one later either. */
#define SPREAD 4
static Gate spread_gate;
static uint32_t spread_arrived;
static uint32_t spread_finished;
static uint32_t spread_stuck;
static int64_t spread_m[SPREAD];

static void spread_child(void *env) {
    int64_t *slot = env;
    burrow__M *m = burrow__curm();
    *slot = m != NULL ? m->id : -1;

    burrow__atomic_add_u32(&spread_arrived, 1);
    for (long i = 0; i < SPIN_LIMIT; i++) {
        if (burrow__atomic_load_acquire_u32(&spread_arrived) >= SPREAD)
            break;
        burrow__atomic_spin_hint();
    }
    if (burrow__atomic_load_acquire_u32(&spread_arrived) < SPREAD)
        burrow__atomic_add_u32(&spread_stuck, 1);

    if (burrow__atomic_add_u32(&spread_finished, 1) == SPREAD - 1)
        gate_signal(&spread_gate);
}

static void spread_body(void *env) {
    (void)env;
    for (int i = 0; i < SPREAD; i++)
        CHECK(go(BURROW_FN(Func, spread_child, &spread_m[i])));
    gate_wait(&spread_gate);
}

TEST(work_started_on_one_processor_is_picked_up_by_the_others) {
    memset(&spread_gate, 0, sizeof spread_gate);
    spread_arrived = 0;
    spread_finished = 0;
    spread_stuck = 0;
    for (int i = 0; i < SPREAD; i++)
        spread_m[i] = -2;

    (void)runtime_gomaxprocs(SPREAD);
    runtime_main(BURROW_FN(Func, spread_body, NULL));

    CHECK_INT_EQ(spread_arrived, SPREAD);
    CHECK_INT_EQ(spread_stuck, 0);

    /* And they really were different threads. */
    int distinct = 0;
    for (int i = 0; i < SPREAD; i++) {
        int seen_before = 0;
        for (int j = 0; j < i; j++) {
            if (spread_m[j] == spread_m[i])
                seen_before = 1;
        }
        if (!seen_before)
            distinct++;
    }
    CHECK(distinct >= 2);
}

/* The stress test, and the one that matters. Pairs of goroutines pass a token
 * back and forth through a park and a ready, on as many threads as the machine
 * will give. Every handoff is a goroutine stopping on one thread and starting
 * again on whichever thread picked it up, so a scheduler that loses a wakeup,
 * runs a goroutine twice or readies one that has not finished parking yet shows
 * up here as a count that is wrong or as a run that does not finish. */
#define PAIRS 8
#define ROUNDS 200

typedef struct Pair {
    Gate there;
    Gate back;
    int token;
    uint32_t finished;
} Pair;

static Pair pairs[PAIRS];
static uint32_t pairs_done;
static bool pairs_all_ran;

static void pair_ping(void *env) {
    Pair *p = env;
    for (int r = 0; r < ROUNDS; r++) {
        p->token++;
        gate_signal(&p->back);
        gate_wait(&p->there);
    }
    burrow__atomic_add_u32(&p->finished, 1);
    burrow__atomic_add_u32(&pairs_done, 1);
}

static void pair_pong(void *env) {
    Pair *p = env;
    for (int r = 0; r < ROUNDS; r++) {
        gate_wait(&p->back);
        p->token++;
        gate_signal(&p->there);
    }
    burrow__atomic_add_u32(&p->finished, 1);
    burrow__atomic_add_u32(&pairs_done, 1);
}

static void pairs_body(void *env) {
    (void)env;
    for (int i = 0; i < PAIRS; i++) {
        CHECK(go(BURROW_FN(Func, pair_pong, &pairs[i])));
        CHECK(go(BURROW_FN(Func, pair_ping, &pairs[i])));
    }
    pairs_all_ran = wait_for(&pairs_done, 2 * PAIRS);
}

TEST(nothing_is_lost_while_goroutines_park_and_ready_each_other) {
    memset(pairs, 0, sizeof pairs);
    pairs_done = 0;
    pairs_all_ran = false;

    int cpus = runtime_numcpu();
    (void)runtime_gomaxprocs(cpus < 4 ? 4 : cpus);
    runtime_main(BURROW_FN(Func, pairs_body, NULL));

    CHECK(pairs_all_ran);
    CHECK_INT_EQ(pairs_done, 2 * PAIRS);

    int wrong = 0;
    for (int i = 0; i < PAIRS; i++) {
        if (pairs[i].token != 2 * ROUNDS)
            wrong++;
        if (pairs[i].finished != 2)
            wrong++;
    }
    CHECK_INT_EQ(wrong, 0);
}

int main(void) {
    RUN(the_function_handed_to_runtime_main_runs);
    RUN(runtime_main_can_be_called_twice);
    RUN(a_goroutine_runs);

    RUN(the_newest_goroutine_runs_first_and_the_rest_run_oldest_first);
    RUN(gosched_lets_another_goroutine_have_the_thread);
    RUN(goexit_ends_the_goroutine_where_it_stands);

    RUN(numgoroutine_counts_parked_goroutines_too);
    RUN(gomaxprocs_reports_and_sets_before_the_scheduler_starts);
    RUN(gomaxprocs_does_not_change_while_the_scheduler_is_running);

    RUN(a_goroutine_can_ask_for_a_bigger_stack_and_use_it);
    RUN(a_dead_goroutine_is_kept_and_handed_out_again);

    RUN(a_value_can_be_handed_between_two_goroutines_through_a_park);
    RUN(a_signal_that_arrives_before_the_wait_is_not_lost);

    RUN(every_goroutine_runs_exactly_once_however_many_there_are);
    RUN(a_goroutine_can_start_goroutines);
    RUN(work_started_on_one_processor_is_picked_up_by_the_others);
    RUN(nothing_is_lost_while_goroutines_park_and_ready_each_other);
    return harness_report("sched");
}
