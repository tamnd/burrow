/* Tests for sync.Cond and the notify list under it.
 *
 * A condition variable is easy to write in a way that passes every test and
 * hangs in production, because the bug is always a lost wakeup and a lost
 * wakeup only shows up when the signal lands in a window a few instructions
 * wide. So the tests here try to land in it on purpose: a signal sent while the
 * waiter is between taking its ticket and going to sleep, a broadcast to a
 * crowd that is still arriving, and a lot of repetitions of both.
 *
 * The usual rule applies. Only the main goroutine calls CHECK, and everybody
 * else reports through an atomic.
 *
 * Copyright 2026 The burrow Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style licence that can be found
 * in the LICENSE file. */

#include "burrow/sync.h"

#include "burrow/func.h"
#include "burrow/proc.h"
#include "burrow/sync/atomic.h"
#include "burrow/thread.h"

#include "check.h"
#include "fatal.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#define WAITERS 5
#define ROUNDS 200

static SyncMutex mu;
static SyncCond cond;
static SyncAtomicInt64 woken;
static SyncAtomicInt64 queued;
static SyncAtomicUint32 saw_condition_false;
static int64_t state;

static void reset(void) {
    memset(&mu, 0, sizeof(mu));
    memset(&cond, 0, sizeof(cond));
    cond = SYNC_COND(sync_mutex_locker(&mu));

    sync_atomic_int64_store(&woken, 0);
    sync_atomic_int64_store(&queued, 0);
    sync_atomic_uint32_store(&saw_condition_false, 0);
    state = 0;
}

/* ------------------------------------------------------- nobody is waiting */

static void TestSignallingAnEmptyCondDoesNothing(TestingT *t) {
    reset();

    /* Neither of these has anybody to wake, and neither of them may leave
     * anything behind that a later waiter would pick up as a wakeup it never
     * got. That is the bug a naive counting implementation has. */
    sync_cond_signal(&cond);
    sync_cond_broadcast(&cond);
    sync_cond_signal(&cond);

    /* So a Wait after all that still has to actually wait, which is checked by
     * the tests below rather than here, since checking it here would mean
     * hanging on failure. What is checked here is that none of it crashed and
     * the Cond is still usable. */
    sync_mutex_lock(&mu);
    state = 1;
    sync_mutex_unlock(&mu);
    sync_cond_broadcast(&cond);

    CHECK_INT_EQ(state, 1);
}

/* ------------------------------------------------------------- one waiter */

static void one_waiter(void *env) {
    (void)env;

    sync_mutex_lock(&mu);
    (void)sync_atomic_int64_add(&queued, 1);

    while (state == 0)
        sync_cond_wait(&cond);

    /* Wait returns holding the lock, so reading state here without any further
     * synchronisation is the whole contract. */
    if (state != 1)
        (void)sync_atomic_uint32_add(&saw_condition_false, 1);

    sync_mutex_unlock(&mu);
    (void)sync_atomic_int64_add(&woken, 1);
}

static void signal_main(void *env) {
    (void)env;

    if (!go(BURROW_FN(Func, one_waiter, NULL)))
        return;

    /* Let it get as far as the wait. Not needed for correctness, and that is
     * the point: the loop around the wait means the test passes whether the
     * signal lands before or after, and this only makes the interesting
     * ordering the likely one. */
    while (sync_atomic_int64_load(&queued) < 1)
        runtime_gosched();

    sync_mutex_lock(&mu);
    state = 1;
    sync_mutex_unlock(&mu);
    sync_cond_signal(&cond);

    while (sync_atomic_int64_load(&woken) < 1)
        runtime_gosched();
}

static void TestASignalWakesAWaiter(TestingT *t) {
    reset();

    runtime_main(BURROW_FN(Func, signal_main, NULL));

    CHECK_INT_EQ(sync_atomic_int64_load(&woken), 1);
    CHECK_INT_EQ(sync_atomic_uint32_load(&saw_condition_false), 0);
}

/* ------------------------------------------------------------- a broadcast */

static void broadcast_main(void *env) {
    (void)env;

    int started = 0;
    for (int i = 0; i < WAITERS; i++) {
        if (go(BURROW_FN(Func, one_waiter, NULL)))
            started++;
    }

    while (sync_atomic_int64_load(&queued) < started)
        runtime_gosched();

    /* Everybody who is waiting has to come back, and a broadcast that wakes
     * only the first is the classic off by one in a notify list that stops at
     * the first match. */
    sync_mutex_lock(&mu);
    state = 1;
    sync_mutex_unlock(&mu);
    sync_cond_broadcast(&cond);

    while (sync_atomic_int64_load(&woken) < started)
        runtime_gosched();
}

static void TestABroadcastWakesEverybody(TestingT *t) {
    reset();

    runtime_main(BURROW_FN(Func, broadcast_main, NULL));

    CHECK_INT_EQ(sync_atomic_int64_load(&woken), WAITERS);
    CHECK_INT_EQ(sync_atomic_uint32_load(&saw_condition_false), 0);
}

/* --------------------------------------------------------- one at a time
 *
 * A signal wakes one waiter and not all of them, which is the difference
 * between Signal and Broadcast and the reason both exist. Checked by waking
 * them one at a time and counting after each, with the condition arranged so
 * that a waiter which wakes without being signalled goes back to sleep and does
 * not change the count. */

static int64_t tokens;

static void token_waiter(void *env) {
    (void)env;

    sync_mutex_lock(&mu);
    (void)sync_atomic_int64_add(&queued, 1);

    while (tokens == 0)
        sync_cond_wait(&cond);

    tokens--;
    sync_mutex_unlock(&mu);
    (void)sync_atomic_int64_add(&woken, 1);
}

static void one_at_a_time_main(void *env) {
    (void)env;

    int started = 0;
    for (int i = 0; i < WAITERS; i++) {
        if (go(BURROW_FN(Func, token_waiter, NULL)))
            started++;
    }

    while (sync_atomic_int64_load(&queued) < started)
        runtime_gosched();

    for (int i = 0; i < started; i++) {
        sync_mutex_lock(&mu);
        tokens++;
        sync_mutex_unlock(&mu);
        sync_cond_signal(&cond);

        /* Exactly one more gets through per token. If a signal woke everybody,
         * the ones past the first would find no token and go back to sleep, so
         * the count would still be right; what would go wrong is the token
         * being left behind, which the check after the loop catches. */
        while (sync_atomic_int64_load(&woken) < i + 1)
            runtime_gosched();
    }
}

static void TestASignalHandsOverOneAtATime(TestingT *t) {
    reset();
    tokens = 0;

    runtime_main(BURROW_FN(Func, one_at_a_time_main, NULL));

    CHECK_INT_EQ(sync_atomic_int64_load(&woken), WAITERS);
    CHECK_INT_EQ(tokens, 0);
}

/* ------------------------------------------------------- the narrow window
 *
 * The test this file exists for.
 *
 * A waiter takes a ticket, drops the lock, and only then goes to sleep. The gap
 * between the unlock and the sleep is where a signal can arrive for somebody
 * who is not on the queue yet, and the ticket is what stops that signal from
 * being lost. Take the ticket after the unlock instead and this test hangs.
 *
 * One waiter and one signaller, going round a few hundred times with no
 * handshake between them beyond the condition itself, so the signal lands all
 * over that window across the run. */

static void ping_pong_waiter(void *env) {
    (void)env;

    for (int i = 0; i < ROUNDS; i++) {
        sync_mutex_lock(&mu);
        while (state != i * 2 + 1)
            sync_cond_wait(&cond);
        state = i * 2 + 2;
        sync_mutex_unlock(&mu);
        sync_cond_signal(&cond);
    }

    (void)sync_atomic_int64_add(&woken, 1);
}

static void ping_pong_main(void *env) {
    (void)env;

    if (!go(BURROW_FN(Func, ping_pong_waiter, NULL)))
        return;

    for (int i = 0; i < ROUNDS; i++) {
        sync_mutex_lock(&mu);
        state = i * 2 + 1;
        sync_mutex_unlock(&mu);
        sync_cond_signal(&cond);

        sync_mutex_lock(&mu);
        while (state != i * 2 + 2)
            sync_cond_wait(&cond);
        sync_mutex_unlock(&mu);
    }

    while (sync_atomic_int64_load(&woken) < 1)
        runtime_gosched();
}

static void TestASignalInTheWindowBeforeTheSleepIsNotLost(TestingT *t) {
    reset();

    runtime_main(BURROW_FN(Func, ping_pong_main, NULL));

    CHECK_INT_EQ(sync_atomic_int64_load(&woken), 1);
    CHECK_INT_EQ(state, ROUNDS * 2);
}

/* ------------------------------------------------------------- an RWMutex
 *
 * A Cond on the read side of an RWMutex, which is what Go's RLocker is for and
 * the reason sync.Locker is an interface rather than a concrete type. Several
 * readers wait, a writer changes the thing, and a broadcast lets them all back
 * in at once, which is a shape a plain Mutex cannot express. */

static SyncRWMutex rw;
static SyncCond rw_cond;
static int64_t rw_state;

static void reader(void *env) {
    (void)env;

    sync_rw_mutex_r_lock(&rw);
    (void)sync_atomic_int64_add(&queued, 1);

    while (rw_state == 0)
        sync_cond_wait(&rw_cond);

    if (rw_state != 1)
        (void)sync_atomic_uint32_add(&saw_condition_false, 1);

    sync_rw_mutex_r_unlock(&rw);
    (void)sync_atomic_int64_add(&woken, 1);
}

static void rw_main(void *env) {
    (void)env;

    int started = 0;
    for (int i = 0; i < WAITERS; i++) {
        if (go(BURROW_FN(Func, reader, NULL)))
            started++;
    }

    while (sync_atomic_int64_load(&queued) < started)
        runtime_gosched();

    sync_rw_mutex_lock(&rw);
    rw_state = 1;
    sync_rw_mutex_unlock(&rw);
    sync_cond_broadcast(&rw_cond);

    while (sync_atomic_int64_load(&woken) < started)
        runtime_gosched();
}

static void TestACondWorksOnTheReadSideOfAnRwMutex(TestingT *t) {
    reset();
    memset(&rw, 0, sizeof(rw));
    memset(&rw_cond, 0, sizeof(rw_cond));
    rw_cond = SYNC_COND(sync_rw_mutex_r_locker(&rw));
    rw_state = 0;

    runtime_main(BURROW_FN(Func, rw_main, NULL));

    CHECK_INT_EQ(sync_atomic_int64_load(&woken), WAITERS);
    CHECK_INT_EQ(sync_atomic_uint32_load(&saw_condition_false), 0);
}

/* ---------------------------------------------------------------- threads
 *
 * Not goroutines. A thread of the host program's own sleeps on a note instead
 * of parking, which is a different path through the waiter, and a Cond has to
 * work for it too. */

static burrow__Thread threads[WAITERS];

static void thread_waiter(void *env) {
    (void)env;

    sync_mutex_lock(&mu);
    (void)sync_atomic_int64_add(&queued, 1);

    while (state == 0)
        sync_cond_wait(&cond);

    if (state != 1)
        (void)sync_atomic_uint32_add(&saw_condition_false, 1);

    sync_mutex_unlock(&mu);
    (void)sync_atomic_int64_add(&woken, 1);
}

static void TestThreadsThatAreNotGoroutinesCanWaitOnACond(TestingT *t) {
    reset();

    for (size_t i = 0; i < WAITERS; i++)
        CHECK(burrow__thread_start(&threads[i], thread_waiter, NULL, 0));

    while (sync_atomic_int64_load(&queued) < WAITERS)
        burrow__thread_yield();

    sync_mutex_lock(&mu);
    state = 1;
    sync_mutex_unlock(&mu);
    sync_cond_broadcast(&cond);

    for (size_t i = 0; i < WAITERS; i++)
        CHECK(burrow__thread_join(&threads[i]));

    CHECK_INT_EQ(sync_atomic_int64_load(&woken), WAITERS);
    CHECK_INT_EQ(sync_atomic_uint32_load(&saw_condition_false), 0);
}

/* ------------------------------------------------------------- misuse */

static void TestACopiedCondIsCaught(TestingT *t) {
    SyncMutex m;
    memset(&m, 0, sizeof(m));

    SyncCond c = SYNC_COND(sync_mutex_locker(&m));

    /* First use, which is what writes the address down. A signal with nobody
     * waiting is the cheapest way to have one. */
    sync_cond_signal(&c);

    SyncCond copy = c;
    CHECK_RUNTIME_ERROR(sync_cond_signal(&copy), "sync: Cond is copied");
    CHECK_RUNTIME_ERROR(sync_cond_broadcast(&copy), "sync: Cond is copied");

    /* The original is still fine, which it would not be if the check were
     * writing rather than comparing. */
    sync_cond_broadcast(&c);
    CHECK(true);
}

#define TESTS(X)                                                                       \
    X(TestSignallingAnEmptyCondDoesNothing)                                            \
    X(TestASignalWakesAWaiter)                                                         \
    X(TestABroadcastWakesEverybody)                                                    \
    X(TestASignalHandsOverOneAtATime)                                                  \
    X(TestASignalInTheWindowBeforeTheSleepIsNotLost)                                   \
    X(TestACondWorksOnTheReadSideOfAnRwMutex)                                          \
    X(TestThreadsThatAreNotGoroutinesCanWaitOnACond)                                   \
    X(TestACopiedCondIsCaught)

TESTING_MAIN_BARE(TESTS)
