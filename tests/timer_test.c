/* Tests for timers.
 *
 * Most of these drive a set of timers with no scheduler anywhere near it, which
 * is what burrow__timer_reset_on is for. That buys two things. The clock is the
 * test's: every `when` below is a small number and every reading handed to
 * burrow__timers_check is another small number, so a test can say that a timer
 * due at 5000 does not fire at 1000 and mean it exactly, rather than sleeping
 * and hoping. And the set is the test's, so the checks can look at the heap
 * directly and say that a stop left the timer in it, which is the property the
 * whole design is built around and which nothing visible from outside reveals.
 *
 * The last two tests do it the other way and go through the real scheduler with
 * the real clock, because the arithmetic being right is not the same as a
 * sleeping thread actually waking up.
 *
 * Copyright 2026 The burrow Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style licence that can be found
 * in the LICENSE file. */

#include "burrow/timer.h"

#include "burrow/atomic.h"
#include "burrow/clock.h"
#include "burrow/func.h"
#include "burrow/lock.h"
#include "burrow/proc.h"
#include "burrow/sched.h"

#include "check.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* ------------------------------------------------------------- the bookkeeping */

#define MAX_FIRED 256

static void *fired_arg[MAX_FIRED];
static int64_t fired_delay[MAX_FIRED];
static uint32_t nfired;

/* What nearly every timer here runs. It writes down what it was handed and in
 * what order, because the order is most of what these tests are about. */
static void note_fire(void *arg, int64_t delay) {
    if (nfired < MAX_FIRED) {
        fired_arg[nfired] = arg;
        fired_delay[nfired] = delay;
    }
    nfired++;
}

static void forget_fires(void) {
    nfired = 0;
}

/* Deliberately not a good generator. It only has to produce the same jumbled
 * order on every machine, so that a heap bug shows up in the same place twice. */
static uint64_t rng_state = 1;

static int64_t rng_when(void) {
    rng_state = rng_state * 6364136223846793005ULL + 1442695040888963407ULL;
    return (int64_t)((rng_state >> 33) % 10000U) + 1;
}

/* ------------------------------------------------------------------ the basics */

static void TestAFreshSetHasNothingInIt(TestingT *t) {
    burrow__Timers ts;
    burrow__timers_init(&ts);

    CHECK_INT_EQ(burrow__timers_wake_time(&ts), 0);
    CHECK_INT_EQ(burrow__timers_len(&ts), 0);

    int64_t next = -1;
    bool ran = true;
    CHECK_INT_EQ(burrow__timers_check(&ts, 1000, &next, &ran), 1000);
    CHECK_INT_EQ(next, 0);
    CHECK(!ran);

    burrow__timers_free(&ts);
}

static void TestATimerThatIsDueRuns(TestingT *t) {
    burrow__Timers ts;
    burrow__Timer tm;
    burrow__timers_init(&ts);
    burrow__timer_init(&tm, note_fire, &tm);
    forget_fires();

    bool pending = true;
    CHECK(burrow__timer_reset_on(&ts, &tm, 1000, 0, NULL, NULL, &pending));
    CHECK(!pending);
    CHECK_INT_EQ(burrow__timers_len(&ts), 1);
    CHECK_INT_EQ(burrow__timers_wake_time(&ts), 1000);

    int64_t next = -1;
    bool ran = false;
    CHECK_INT_EQ(burrow__timers_check(&ts, 2500, &next, &ran), 2500);
    CHECK(ran);
    CHECK_INT_EQ(nfired, 1);
    CHECK(fired_arg[0] == &tm);

    /* How late it was, which is what the layer above subtracts back out. */
    CHECK_INT_EQ(fired_delay[0], 1500);

    /* A timer that fires once is out of the heap afterwards and disarmed. */
    CHECK_INT_EQ(next, 0);
    CHECK_INT_EQ(ts.len, 0);
    CHECK_INT_EQ(burrow__timers_wake_time(&ts), 0);
    CHECK_INT_EQ(tm.state, 0);
    CHECK(tm.ts == NULL);
    CHECK(!burrow__timer_stop(&tm));

    burrow__timers_free(&ts);
}

static void TestATimerThatIsNotDueYetSaysWhenItWillBe(TestingT *t) {
    burrow__Timers ts;
    burrow__Timer tm;
    burrow__timers_init(&ts);
    burrow__timer_init(&tm, note_fire, &tm);
    forget_fires();

    CHECK(burrow__timer_reset_on(&ts, &tm, 5000, 0, NULL, NULL, NULL));

    int64_t next = -1;
    bool ran = true;
    CHECK_INT_EQ(burrow__timers_check(&ts, 1000, &next, &ran), 1000);
    CHECK(!ran);
    CHECK_INT_EQ(nfired, 0);
    CHECK_INT_EQ(next, 5000);
    CHECK_INT_EQ(ts.len, 1);

    /* And it is still armed, so stopping it reports that. */
    CHECK(burrow__timer_stop(&tm));
    burrow__timers_free(&ts);
}

static void TestTakingAReadingIsLeftToTheCaller(TestingT *t) {
    burrow__Timers ts;
    burrow__Timer tm;
    burrow__timers_init(&ts);
    burrow__timer_init(&tm, note_fire, &tm);
    forget_fires();

    /* Zero means take one, and the reading used comes back, which is how a
     * thread walking several Ps asks the clock once rather than once per P. */
    int64_t before = burrow__nanotime();
    CHECK(burrow__timer_reset_on(&ts, &tm, before + 1, 0, NULL, NULL, NULL));
    int64_t used = burrow__timers_check(&ts, 0, NULL, NULL);
    CHECK(used >= before);
    CHECK(used <= burrow__nanotime());

    burrow__timers_free(&ts);
}

/* ------------------------------------------------------------------- the heap */

static void TestTimersRunInTheOrderTheyAreDue(TestingT *t) {
    static burrow__Timer timers[200];
    static int64_t whens[200];
    burrow__Timers ts;

    burrow__timers_init(&ts);
    forget_fires();
    rng_state = 1;

    int64_t least = 0;
    for (uint32_t i = 0; i < 200; i++) {
        whens[i] = rng_when();
        if (least == 0 || whens[i] < least)
            least = whens[i];
        burrow__timer_init(&timers[i], note_fire, &whens[i]);
        CHECK(burrow__timer_reset_on(&ts, &timers[i], whens[i], 0, NULL, NULL, NULL));
    }

    /* Two hundred of them means the heap has grown five times, so this is the
     * growth path as much as it is the ordering one. */
    CHECK_INT_EQ(ts.len, 200);
    CHECK(ts.cap >= 200);
    CHECK_INT_EQ(burrow__timers_wake_time(&ts), least);

    int64_t next = -1;
    bool ran = false;
    CHECK_INT_EQ(burrow__timers_check(&ts, 20000, &next, &ran), 20000);
    CHECK(ran);
    CHECK_INT_EQ(next, 0);
    CHECK_INT_EQ(ts.len, 0);
    CHECK_INT_EQ(nfired, 200);

    bool ordered = true;
    for (uint32_t i = 1; i < nfired && i < MAX_FIRED; i++) {
        if (*(int64_t *)fired_arg[i] < *(int64_t *)fired_arg[i - 1])
            ordered = false;
    }
    CHECK(ordered);

    burrow__timers_free(&ts);
}

static void TestATimerMovedEarlierMovesTheWakeTimeWithIt(TestingT *t) {
    burrow__Timers ts;
    burrow__Timer early, late;
    burrow__timers_init(&ts);
    burrow__timer_init(&early, note_fire, &early);
    burrow__timer_init(&late, note_fire, &late);
    forget_fires();

    CHECK(burrow__timer_reset_on(&ts, &early, 5000, 0, NULL, NULL, NULL));
    CHECK(burrow__timer_reset_on(&ts, &late, 9000, 0, NULL, NULL, NULL));
    CHECK_INT_EQ(burrow__timers_wake_time(&ts), 5000);

    /* Moving the one at the back of the heap to the front is the case the heap
     * cannot answer on its own, because the entry it holds still says 9000. The
     * published lower bound is what carries it until somebody fixes the heap. */
    bool pending = false;
    CHECK(burrow__timer_reset_on(&ts, &late, 1000, 0, NULL, NULL, &pending));
    CHECK(pending);
    CHECK_INT_EQ(burrow__timers_wake_time(&ts), 1000);
    CHECK_INT_EQ(late.state, BURROW__TIMER_HEAPED | BURROW__TIMER_MODIFIED);

    int64_t next = -1;
    bool ran = false;
    CHECK_INT_EQ(burrow__timers_check(&ts, 2000, &next, &ran), 2000);
    CHECK(ran);
    CHECK_INT_EQ(nfired, 1);
    CHECK(fired_arg[0] == &late);
    CHECK_INT_EQ(next, 5000);
    CHECK_INT_EQ(ts.len, 1);
    CHECK_INT_EQ(burrow__timers_wake_time(&ts), 5000);

    burrow__timers_free(&ts);
}

static void TestATimerMovedLaterStaysWhereItWasUntilSomebodyLooks(TestingT *t) {
    burrow__Timers ts;
    burrow__Timer a, b;
    burrow__timers_init(&ts);
    burrow__timer_init(&a, note_fire, &a);
    burrow__timer_init(&b, note_fire, &b);
    forget_fires();

    CHECK(burrow__timer_reset_on(&ts, &a, 1000, 0, NULL, NULL, NULL));
    CHECK(burrow__timer_reset_on(&ts, &b, 2000, 0, NULL, NULL, NULL));
    CHECK(burrow__timer_reset_on(&ts, &a, 8000, 0, NULL, NULL, NULL));

    /* The published answer is now earlier than the truth, which is the direction
     * that costs a wakeup and never a missed firing. */
    CHECK_INT_EQ(burrow__timers_wake_time(&ts), 1000);

    int64_t next = -1;
    bool ran = false;
    CHECK_INT_EQ(burrow__timers_check(&ts, 3000, &next, &ran), 3000);
    CHECK(ran);
    CHECK_INT_EQ(nfired, 1);
    CHECK(fired_arg[0] == &b);
    CHECK_INT_EQ(next, 8000);

    burrow__timers_free(&ts);
}

/* -------------------------------------------------------------------- stopping */

static void TestAStoppedTimerStaysInTheHeapAndDoesNotFire(TestingT *t) {
    burrow__Timers ts;
    burrow__Timer stopped, live;
    burrow__timers_init(&ts);
    burrow__timer_init(&stopped, note_fire, &stopped);
    burrow__timer_init(&live, note_fire, &live);
    forget_fires();

    CHECK(burrow__timer_reset_on(&ts, &stopped, 1000, 0, NULL, NULL, NULL));
    CHECK(burrow__timer_reset_on(&ts, &live, 2000, 0, NULL, NULL, NULL));

    CHECK(burrow__timer_stop(&stopped));
    CHECK_INT_EQ(ts.len, 2);
    CHECK_INT_EQ(ts.zombies, 1);
    CHECK_INT_EQ(stopped.state,
                 BURROW__TIMER_HEAPED | BURROW__TIMER_MODIFIED | BURROW__TIMER_ZOMBIE);

    /* Stopping it twice reports that the second one did nothing. */
    CHECK(!burrow__timer_stop(&stopped));
    CHECK_INT_EQ(ts.zombies, 1);

    int64_t next = -1;
    bool ran = false;
    CHECK_INT_EQ(burrow__timers_check(&ts, 5000, &next, &ran), 5000);
    CHECK(ran);
    CHECK_INT_EQ(nfired, 1);
    CHECK(fired_arg[0] == &live);
    CHECK_INT_EQ(ts.len, 0);
    CHECK_INT_EQ(ts.zombies, 0);
    CHECK_INT_EQ(stopped.state, 0);
    CHECK(stopped.ts == NULL);

    burrow__timers_free(&ts);
}

static void TestStartingAStoppedTimerAgainNeverTouchesTheHeap(TestingT *t) {
    burrow__Timers ts;
    burrow__Timer tm;
    burrow__timers_init(&ts);
    burrow__timer_init(&tm, note_fire, &tm);
    forget_fires();

    CHECK(burrow__timer_reset_on(&ts, &tm, 1000, 0, NULL, NULL, NULL));
    CHECK(burrow__timer_stop(&tm));
    CHECK_INT_EQ(ts.zombies, 1);

    /* This is the read deadline on a busy connection: stopped and started again
     * before the P that owns the heap got round to throwing it out. The mark
     * comes off, the heap is not touched, and the entry it already holds is the
     * one that gets corrected later. */
    CHECK(burrow__timer_reset_on(&ts, &tm, 4000, 0, NULL, NULL, NULL));
    CHECK_INT_EQ(ts.len, 1);
    CHECK_INT_EQ(ts.zombies, 0);
    CHECK_INT_EQ(tm.state, BURROW__TIMER_HEAPED | BURROW__TIMER_MODIFIED);

    int64_t next = -1;
    bool ran = true;
    CHECK_INT_EQ(burrow__timers_check(&ts, 2000, &next, &ran), 2000);
    CHECK(!ran);
    CHECK_INT_EQ(nfired, 0);
    CHECK_INT_EQ(next, 4000);

    CHECK_INT_EQ(burrow__timers_check(&ts, 4000, &next, &ran), 4000);
    CHECK(ran);
    CHECK_INT_EQ(nfired, 1);
    CHECK_INT_EQ(fired_delay[0], 0);

    burrow__timers_free(&ts);
}

static void TestASetThatIsMostlyStoppedTimersGetsCleanedOut(TestingT *t) {
    static burrow__Timer timers[64];
    burrow__Timers ts;

    burrow__timers_init(&ts);
    forget_fires();

    for (uint32_t i = 0; i < 64; i++) {
        burrow__timer_init(&timers[i], note_fire, &timers[i]);
        CHECK(burrow__timer_reset_on(&ts, &timers[i], (int64_t)i * 100 + 100, 0, NULL,
                                     NULL, NULL));
    }
    for (uint32_t i = 0; i < 63; i++)
        CHECK(burrow__timer_stop(&timers[i]));

    CHECK_INT_EQ(ts.len, 64);
    CHECK_INT_EQ(ts.zombies, 63);

    /* The earliest thing in the heap is stopped, so this pass has to dig through
     * sixty three of those to find out that the one timer still alive is not due
     * for a while yet. Nothing runs and the heap comes out with one entry. */
    int64_t next = -1;
    bool ran = true;
    CHECK_INT_EQ(burrow__timers_check(&ts, 100, &next, &ran), 100);
    CHECK(!ran);
    CHECK_INT_EQ(nfired, 0);
    CHECK_INT_EQ(ts.len, 1);
    CHECK_INT_EQ(ts.zombies, 0);
    CHECK_INT_EQ(next, 6400);
    CHECK_INT_EQ(burrow__timers_wake_time(&ts), 6400);

    CHECK_INT_EQ(burrow__timers_check(&ts, 6400, &next, &ran), 6400);
    CHECK(ran);
    CHECK_INT_EQ(nfired, 1);
    CHECK(fired_arg[0] == &timers[63]);

    burrow__timers_free(&ts);
}

/* ------------------------------------------------------------------- repeating */

static void TestARepeatingTimerArmsItselfAgain(TestingT *t) {
    burrow__Timers ts;
    burrow__Timer tm;
    burrow__timers_init(&ts);
    burrow__timer_init(&tm, note_fire, &tm);
    forget_fires();

    CHECK(burrow__timer_reset_on(&ts, &tm, 1000, 1000, NULL, NULL, NULL));

    int64_t next = -1;
    bool ran = false;
    CHECK_INT_EQ(burrow__timers_check(&ts, 1000, &next, &ran), 1000);
    CHECK(ran);
    CHECK_INT_EQ(nfired, 1);
    CHECK_INT_EQ(next, 2000);
    CHECK_INT_EQ(ts.len, 1);

    /* Late by half a period, so the next one is still on the original grid
     * rather than half a period behind it forever. */
    CHECK_INT_EQ(burrow__timers_check(&ts, 2500, &next, &ran), 2500);
    CHECK(ran);
    CHECK_INT_EQ(nfired, 2);
    CHECK_INT_EQ(fired_delay[1], 500);
    CHECK_INT_EQ(next, 3000);

    /* And a whole minute late is one firing, not sixty thousand. */
    CHECK_INT_EQ(burrow__timers_check(&ts, 60000, &next, &ran), 60000);
    CHECK(ran);
    CHECK_INT_EQ(nfired, 3);
    CHECK_INT_EQ(next, 61000);

    CHECK(burrow__timer_stop(&tm));
    CHECK_INT_EQ(burrow__timers_check(&ts, 70000, &next, &ran), 70000);
    CHECK(!ran);
    CHECK_INT_EQ(nfired, 3);
    CHECK_INT_EQ(next, 0);
    CHECK_INT_EQ(ts.len, 0);

    burrow__timers_free(&ts);
}

/* -------------------------------------------------------------- whole sets */

static void TestOneSetCanTakeOverAnother(TestingT *t) {
    burrow__Timers dst, src;
    burrow__Timer keep, moved, also, dead;

    burrow__timers_init(&dst);
    burrow__timers_init(&src);
    burrow__timer_init(&keep, note_fire, &keep);
    burrow__timer_init(&moved, note_fire, &moved);
    burrow__timer_init(&also, note_fire, &also);
    burrow__timer_init(&dead, note_fire, &dead);
    forget_fires();

    CHECK(burrow__timer_reset_on(&dst, &keep, 3000, 0, NULL, NULL, NULL));
    CHECK(burrow__timer_reset_on(&src, &moved, 1000, 0, NULL, NULL, NULL));
    CHECK(burrow__timer_reset_on(&src, &also, 5000, 0, NULL, NULL, NULL));
    CHECK(burrow__timer_reset_on(&src, &dead, 2000, 0, NULL, NULL, NULL));
    CHECK(burrow__timer_stop(&dead));

    CHECK(burrow__timers_take(&dst, &src));
    CHECK_INT_EQ(src.len, 0);
    CHECK(src.heap == NULL);

    /* The stopped one is dropped on the way over rather than carried. */
    CHECK_INT_EQ(dst.len, 3);
    CHECK_INT_EQ(dst.zombies, 0);
    CHECK_INT_EQ(dead.state, 0);
    CHECK(dead.ts == NULL);
    CHECK(moved.ts == &dst);
    CHECK_INT_EQ(burrow__timers_len(&dst), 3);
    CHECK_INT_EQ(burrow__timers_wake_time(&dst), 1000);

    bool ran = false;
    CHECK_INT_EQ(burrow__timers_check(&dst, 9000, NULL, &ran), 9000);
    CHECK(ran);
    CHECK_INT_EQ(nfired, 3);
    CHECK(fired_arg[0] == &moved);
    CHECK(fired_arg[1] == &keep);
    CHECK(fired_arg[2] == &also);

    burrow__timers_free(&dst);
}

static void TestFreeingASetDisarmsWhatIsLeftInIt(TestingT *t) {
    burrow__Timers first, second;
    burrow__Timer tm;

    burrow__timers_init(&first);
    burrow__timers_init(&second);
    burrow__timer_init(&tm, note_fire, &tm);
    forget_fires();

    CHECK(burrow__timer_reset_on(&first, &tm, 1000, 0, NULL, NULL, NULL));
    burrow__timers_free(&first);

    /* A timer that outlives the set it was in is disarmed rather than left
     * pointing at memory that has gone. */
    CHECK_INT_EQ(tm.state, 0);
    CHECK(tm.ts == NULL);

    /* And it works again somewhere else. */
    CHECK(burrow__timer_reset_on(&second, &tm, 1000, 0, NULL, NULL, NULL));
    bool ran = false;
    CHECK_INT_EQ(burrow__timers_check(&second, 1000, NULL, &ran), 1000);
    CHECK(ran);
    CHECK_INT_EQ(nfired, 1);

    burrow__timers_free(&second);
}

/* ------------------------------------------------------ through the scheduler
 *
 * A one goroutine gate, copied from tests/sched_test.c for the same reason it is
 * there: it is the smallest thing that parks a goroutine and readies it again,
 * and what these two tests need is for the readying to be done by a timer. */

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

static void gate_signal_from_timer(void *arg, int64_t delay) {
    (void)delay;
    gate_signal((Gate *)arg);
}

/* Twenty milliseconds, which is long enough that a thread really has to go to
 * sleep for it and short enough that the suite does not notice. */
#define WAIT_NS (20 * 1000 * 1000)

/* How many times a goroutine that cannot park spins before it gives up. Only
 * costs anything when the test is already failing. */
#define SPIN_LIMIT (500 * 1000 * 1000)

static Gate wait_gate;
static burrow__Timer waker;
static bool armed;
static int64_t waited;

static void park_until_the_timer(void *env) {
    (void)env;
    int64_t start = burrow__nanotime();
    burrow__timer_init(&waker, gate_signal_from_timer, &wait_gate);
    armed = burrow__timer_reset(&waker, start + WAIT_NS, 0, NULL, NULL, NULL);
    gate_wait(&wait_gate);
    waited = burrow__nanotime() - start;
}

static void TestATimerWakesAThreadThatHasNothingElseToDo(TestingT *t) {
    wait_gate = (Gate){0};
    armed = false;
    waited = 0;
    (void)runtime_gomaxprocs(1);

    /* One P, one goroutine, and the goroutine is asleep. So the thread has run
     * out of everything, and the only reason it ever wakes up again is that it
     * worked out a deadline from the timer before it parked. */
    runtime_main(BURROW_FN(Func, park_until_the_timer, NULL));

    CHECK(armed);
    CHECK(waited >= WAIT_NS);

    /* Nothing checks how late it was. A bound on that is a test that fails on a
     * machine somebody else is using, and a wake that never comes is already a
     * test that never finishes. */
}

static uint32_t fired_flag;

static void raise_flag(void *arg, int64_t delay) {
    (void)delay;
    burrow__atomic_store_u32((uint32_t *)arg, 1);
}

static void spin_until_the_timer(void *env) {
    (void)env;
    int64_t start = burrow__nanotime();
    burrow__timer_init(&waker, raise_flag, &fired_flag);
    armed = burrow__timer_reset(&waker, start + WAIT_NS, 0, NULL, NULL, NULL);

    /* Never gives the processor away, so the P this timer went on to never
     * reaches the scheduler again. Somebody else has to run it. */
    for (long i = 0; i < SPIN_LIMIT; i++) {
        if (burrow__atomic_load_acquire_u32(&fired_flag) != 0)
            break;
        burrow__atomic_spin_hint();
    }
    waited = burrow__nanotime() - start;
}

static void TestATimerOnABusyPIsRunByAnotherThread(TestingT *t) {
    armed = false;
    waited = 0;
    fired_flag = 0;
    (void)runtime_gomaxprocs(2);

    runtime_main(BURROW_FN(Func, spin_until_the_timer, NULL));

    CHECK(armed);
    CHECK_INT_EQ(fired_flag, 1);
    CHECK(waited >= WAIT_NS);
}

#define TESTS(X)                                                                       \
    X(TestAFreshSetHasNothingInIt)                                                     \
    X(TestATimerThatIsDueRuns)                                                         \
    X(TestATimerThatIsNotDueYetSaysWhenItWillBe)                                       \
    X(TestTakingAReadingIsLeftToTheCaller)                                             \
    X(TestTimersRunInTheOrderTheyAreDue)                                               \
    X(TestATimerMovedEarlierMovesTheWakeTimeWithIt)                                    \
    X(TestATimerMovedLaterStaysWhereItWasUntilSomebodyLooks)                           \
    X(TestAStoppedTimerStaysInTheHeapAndDoesNotFire)                                   \
    X(TestStartingAStoppedTimerAgainNeverTouchesTheHeap)                               \
    X(TestASetThatIsMostlyStoppedTimersGetsCleanedOut)                                 \
    X(TestARepeatingTimerArmsItselfAgain)                                              \
    X(TestOneSetCanTakeOverAnother)                                                    \
    X(TestFreeingASetDisarmsWhatIsLeftInIt)                                            \
    X(TestATimerWakesAThreadThatHasNothingElseToDo)                                    \
    X(TestATimerOnABusyPIsRunByAnotherThread)

TESTING_MAIN_BARE(TESTS)
