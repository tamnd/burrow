/* The synctest bubble. See burrow/synctest.h for what it is for.
 *
 * Two numbers and a clock.
 *
 * `active` is how many goroutines in the bubble could still get somewhere on
 * their own. It goes up when one is born into the bubble and when one is woken,
 * and down when one exits and when one parks on something only another goroutine
 * in the bubble can end. Zero is the one interesting event a bubble has. `total`
 * is the same count without the durability, and it is how the end of a bubble is
 * told apart from a deadlock in one: four goroutines blocked on each other is
 * total four and active zero, and a finished bubble has both at one.
 *
 * Both counts go up outside the lock and down under it. Up is sound because once
 * active reads zero nothing can raise it again until somebody acts on it: that
 * takes a new goroutine or a wake, and both of those take a goroutine in the
 * bubble that is running. Down is under the lock because a drop can be the one
 * that ends the bubble, and drop_locked says why.
 *
 * The clock is what makes a test of something with a timeout in it finish
 * instantly. A bubble has a reading of its own and timers of its own, and the
 * goroutine that called synctest_run is the only one that touches either. It
 * loops: run whatever is due, park until nothing in the bubble can move, wind
 * the reading forward to the next timer, go round again. Time in a bubble does
 * not pass, it jumps, and only when nothing would notice the difference.
 *
 * That goroutine is a member of its own bubble, which is Go's shape. It is what
 * stops the bubble being called idle while the clock is wound, and it is why a
 * finished bubble counts one rather than zero.
 *
 * The bubble lives on synctest_run's own stack, so there is no allocation here
 * and no way for making one to fail. What it costs is that "who might still be
 * looking at this" is a real question rather than one a collector answers, and
 * burrow__bubble_hold is where the answer is wider than it looks.
 *
 * Derived from Go's src/runtime/synctest.go.
 * Go source: go1.27.1.
 *
 * Copyright 2009 The Go Authors. All rights reserved.
 * Copyright 2026 The burrow Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style licence that can be found
 * in the LICENSE file. */

#include "burrow/synctest.h"

#include "burrow/atomic.h"
#include "burrow/core.h"
#include "burrow/func.h"
#include "burrow/lock.h"
#include "burrow/proc.h"
#include "burrow/runtime.h"
#include "burrow/sched.h"
#include "burrow/time.h"
#include "burrow/timer.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

/* Midnight UTC on the first of January 2000, in nanoseconds, which is where
 * every bubble's clock starts. Go's number.
 *
 * burrow has no wall clock yet, so nothing here can tell the difference between
 * this and any other starting point. It is Go's so that the day a Time type
 * arrives the two agree without anybody having to go and change it, and because
 * a reading that is obviously not the machine's is one less thing to work out
 * when a test prints one. */
#define BUBBLE_BASE_TIME ((int64_t)946684800000000000)

struct burrow__Bubble {
    /* Over the two handoffs and nothing else: which thread gets to wake the
     * goroutine in synctest_wait, and which one gets to wake the goroutine in
     * synctest_run. Those have to happen once each and a count cannot say that,
     * which is the only thing the counts above cannot do on their own. */
    burrow__Lock lock;

    uint32_t total;
    uint32_t active;

    /* What time it is in here. Atomic, because it is read by any goroutine in
     * the bubble that asks the clock and written by the root.
     *
     * The write can only happen while every other goroutine in the bubble is
     * durably blocked, so there is no reader that could get half of it and no
     * question about which value a reader should see. The atomic is for the
     * ordering, and for saying out loud that this is read from other threads. */
    uint64_t now;

    /* The timers armed inside the bubble, on the clock above. Nothing outside
     * the bubble ever looks at this set, which is what `fake` in burrow/timer.h
     * is about. */
    burrow__Timers timers;

    /* Whether the goroutine below has exited. Atomic, read by the root.
     *
     * Time stops when it does. A bubble whose body has returned and which still
     * has goroutines waiting on timers is a test that leaked one, and winding
     * the clock on for it would turn that into a test that passes slowly rather
     * than one that says what happened. Go stops for the same reason. */
    uint32_t done;

    /* The goroutine parked in synctest_wait, or NULL. At most one, because two
     * goroutines each waiting for the other to stop is a test whose meaning
     * nobody can work out, and Go refuses it for the same reason. */
    burrow__G *waiter;

    /* The goroutine that called synctest_run. In the bubble for the whole of
     * the run, and the only one that winds the clock. */
    burrow__G *root;

    /* The goroutine synctest_run started, the one the bubble exists for. Only
     * ever compared, never followed, which is what lets the scheduler hand it
     * over after it has gone on a free list. */
    burrow__G *main;
};

static uint32_t dec(uint32_t *p) {
    return burrow__atomic_add_u32(p, (uint32_t)-1) - 1;
}

static uint32_t inc(uint32_t *p) {
    return burrow__atomic_add_u32(p, 1) + 1;
}

int64_t burrow__bubble_now(burrow__Bubble *b) {
    return (int64_t)burrow__atomic_load_acquire_u64(&b->now);
}

burrow__Timers *burrow__bubble_timers(burrow__Bubble *b) {
    return &b->timers;
}

bool burrow__bubble_is_root(burrow__Bubble *b, burrow__G *g) {
    return b->root == g;
}

/* ------------------------------------------------------------------ settling
 *
 * What to do about a bubble whose counts have just changed. Answers a goroutine
 * to start, which the caller does once it has let the lock go.
 *
 * Three questions in the order of how final they are, and every one of them is
 * only asked once the bubble has nothing left that can move on its own. A timer
 * that is already due goes first, because the goroutine it is about to start
 * counts as running and anything that answered before it would be saying the
 * opposite. Then the goroutine in synctest_wait, which asked to be told exactly
 * this. Then the root, which is left holding either a clock to wind or a
 * deadlock to report, and which is the one that can tell those apart. */

/* Hands a goroutine its place in the bubble back and answers it.
 *
 * Counted here rather than left to the wake, because the lock is about to be
 * released and a second thread that found active at zero would decide this
 * bubble was idle all over again. Taking the flag is what makes the increment
 * happen exactly once even though sched_ready is about to ask the same
 * question.
 *
 * Everything this is called on is durably parked, because active is zero and a
 * goroutine that was running would be counted in it. */
static burrow__G *wake_locked(burrow__Bubble *b, burrow__G *g) {
    if (burrow__atomic_swap_u32(&g->bubbleblocked, 0) == 1)
        (void)inc(&b->active);
    return g;
}

static burrow__G *settle_locked(burrow__Bubble *b) {
    if (burrow__atomic_load_acquire_u32(&b->active) > 0)
        return NULL;

    int64_t next = burrow__timers_wake_time(&b->timers);
    if (next > 0 && next <= burrow__bubble_now(b))
        return wake_locked(b, b->root);

    if (b->waiter != NULL) {
        burrow__G *w = b->waiter;
        b->waiter = NULL;
        return wake_locked(b, w);
    }

    return wake_locked(b, b->root);
}

/* ------------------------------------------ what the scheduler reports to us
 *
 * The calls burrow/sched.h declares, in the order a goroutine meets them. */

void burrow__bubble_main(burrow__Bubble *b, burrow__G *g) {
    b->main = g;
}

void burrow__bubble_join(burrow__Bubble *b) {
    (void)inc(&b->total);
    (void)inc(&b->active);
}

/* Taking the counts down is under the lock, and the reason is that dropping a
 * count can make the bubble's memory go away underneath the thread that dropped
 * it.
 *
 * Two goroutines exiting at once, with the decrements outside the lock: the
 * first takes active from two to one, the second takes it to zero, wakes the
 * root, and that one finds nothing left, returns, and takes the stack frame the
 * bubble lives in with it. The first is by then somewhere inside this function,
 * holding a pointer to memory that has been reused. Doing the decrement under
 * the lock makes the two orders the only two there are, and whichever of them
 * holds the lock last is the one that sees zero. */
static void drop(burrow__Bubble *b, burrow__G *exiting) {
    burrow__lock(&b->lock);
    if (exiting != NULL && exiting == b->main)
        burrow__atomic_store_release_u32(&b->done, 1);
    (void)dec(&b->total);
    (void)dec(&b->active);
    burrow__G *w = settle_locked(b);
    burrow__unlock(&b->lock);

    /* Nothing of b is touched after this, for the reason above. */
    if (w != NULL)
        sched_ready(w);
}

void burrow__bubble_exit(burrow__Bubble *b, burrow__G *g) {
    drop(b, g);
}

/* A park in flight takes a place in the bubble of its own, exactly the place a
 * goroutine takes, for the stretch between the start of the park and the moment
 * the park's answer is known. That is why these two are the same arithmetic a
 * goroutine does rather than arithmetic of their own, and there are two separate
 * reasons for it.
 *
 * The active half is the one Go has too. The park drops the goroutine's active
 * count on the way in, before the lock the goroutine is parking under is
 * released, because a wake arriving after that release has to find a count it
 * can put back. But the park is not over at that point: the unlock function gets
 * one more look and may say the goroutine is not parking after all. Between
 * those two moments the count is short by one for a goroutine that is still
 * running, and another thread reading it would call a deadlock in a program that
 * is fine. The park's own place cancels that.
 *
 * The total half is not in Go, where a bubble is an object on the heap and a
 * stale pointer to one is harmless. Here the bubble is a stack frame. Once the
 * unlock has happened the parked goroutine can be woken by another thread, run,
 * exit and take total to its floor, all while this thread is still in the rest
 * of park0 with the bubble pointer in hand. The goroutine's own place does not
 * cover that, because the goroutine is gone before the park is. So a bubble is
 * over when the last park in it has finished and not merely when the last
 * goroutine has. */
void burrow__bubble_hold(burrow__Bubble *b) {
    burrow__bubble_join(b);
}

void burrow__bubble_release(burrow__Bubble *b) {
    drop(b, NULL);
}

void burrow__bubble_blocking(burrow__G *g) {
    /* The flag before the count, so that a wake arriving the instant the park's
     * own lock is released finds the flag set and gives the count back. A wake
     * cannot arrive any earlier than that, which is the reason this is called
     * with that lock still held. */
    burrow__atomic_store_release_u32(&g->bubbleblocked, 1);
    (void)dec(&g->bubble->active);
}

void burrow__bubble_unblock(burrow__G *g) {
    /* The load first, because this is on the path of every wake in the program
     * and almost every one of them is a goroutine that was never in a bubble at
     * all. A swap would be a write to a line another thread may be reading. */
    if (burrow__atomic_load_acquire_u32(&g->bubbleblocked) == 0)
        return;
    if (burrow__atomic_swap_u32(&g->bubbleblocked, 0) == 0)
        return;

    (void)inc(&g->bubble->active);
}

/* --------------------------------------------------------------------- run */

/* The root's park.
 *
 * With no unlock function of its own, which is worth saying because the obvious
 * shape here is one that asks whether the bubble is already idle and carries on
 * without parking if it is. That shape is wrong, and the reason is the whole of
 * why the decision about what to wake lives in settle_locked. A bubble that has
 * gone idle with a goroutine sitting in synctest_wait wants to wake that one,
 * not the root, and a root that never parked would take the answer for itself
 * and wind the clock on past a goroutine that was waiting to be told.
 *
 * So the root parks like everybody else and lets the count decide. If the bubble
 * was already idle, the release at the end of its own park takes the count to
 * zero and starts it again straight away, which costs one trip through the
 * scheduler and gets the answer right. Go parks unconditionally here too. */
bool synctest_run(Func f) {
    burrow__G *g = burrow__curg();
    if (g == NULL)
        runtime_throw(BURROW_S("synctest_run: not on a goroutine"));
    if (g->bubble != NULL)
        runtime_throw(BURROW_S("synctest_run: bubbles do not nest"));

    burrow__Bubble b;
    memset(&b, 0, sizeof(b));
    b.root = g;
    b.now = (uint64_t)BUBBLE_BASE_TIME;
    burrow__timers_init(&b.timers);
    b.timers.fake = true;

    /* This goroutine joins its own bubble before anything else is in it, which
     * closes the window where a body that blocked immediately would find a
     * bubble with nothing active in it and no root written down yet. */
    g->bubble = &b;
    burrow__bubble_join(&b);

    if (!burrow__go_bubble(f, &b)) {
        /* Nothing was ever in the bubble but this goroutine, so there is no
         * count to put back and nobody who could be reading one. The bubble
         * goes away with the frame. */
        g->bubble = NULL;
        burrow__timers_free(&b.timers);
        return false;
    }

    for (;;) {
        /* On this goroutine's stack rather than the scheduler's, which is where
         * a P's timers run. A bubbled timer belongs to the bubble and the
         * bubble is here, and what these do is start goroutines in it. */
        (void)burrow__timers_check(&b.timers, burrow__bubble_now(&b), NULL, NULL);

        burrow__park(NULL, NULL, true);

        /* Past the park, so every other goroutine in the bubble is durably
         * blocked and none of them can arm a timer or exit underneath this.
         * That is what makes reading the set without its lock the right thing
         * rather than a shortcut. */
        int64_t next = burrow__timers_wake_time(&b.timers);
        if (next == 0)
            break;
        if (next < burrow__bubble_now(&b))
            runtime_throw(
                BURROW_S("synctest: a timer is due before the bubble's clock"));
        if (burrow__atomic_load_acquire_u32(&b.done) != 0)
            break;

        burrow__atomic_store_release_u64(&b.now, (uint64_t)next);
    }

    uint32_t total = burrow__atomic_load_acquire_u32(&b.total);
    bool done = burrow__atomic_load_acquire_u32(&b.done) != 0;

    g->bubble = NULL;
    burrow__timers_free(&b.timers);

    /* One is this goroutine and nobody else, which is a bubble that finished.
     * Anything more is goroutines that are never going to run again, and the
     * two messages are the two ways a test gets there: one that leaked a
     * goroutine past the end of its body, and one where everything in the
     * bubble ended up waiting for everything else. */
    if (total != 1) {
        if (done)
            runtime_throw(BURROW_S("synctest: the bubble's body has finished and "
                                   "goroutines it started are still blocked"));
        runtime_throw(BURROW_S("synctest: all goroutines in the bubble are blocked"));
    }
    return true;
}

/* -------------------------------------------------------------------- wait */

static bool unlock_bubble(burrow__G *g, void *p) {
    (void)g;
    burrow__unlock(&((burrow__Bubble *)p)->lock);
    return true;
}

void synctest_wait(void) {
    burrow__G *g = burrow__curg();
    if (g == NULL || g->bubble == NULL)
        runtime_throw(BURROW_S("synctest_wait: not inside a bubble"));
    if (burrow__bubble_is_root(g->bubble, g))
        runtime_throw(BURROW_S("synctest_wait: called from synctest_run"));

    burrow__Bubble *b = g->bubble;

    burrow__lock(&b->lock);
    if (b->waiter != NULL) {
        burrow__unlock(&b->lock);
        runtime_throw(BURROW_S(
            "synctest_wait: another goroutine in the bubble is already waiting"));
    }

    /* One active goroutine is this one, since this one is running. So there is
     * nothing to wait for and no reason to stop. Checked here rather than left
     * for the park to discover, because a bubble of one is the ordinary shape
     * of a test that has not started its workers yet. */
    if (burrow__atomic_load_acquire_u32(&b->active) == 1) {
        burrow__unlock(&b->lock);
        return;
    }

    /* The waiter is written down before the count is given back, so that
     * whoever takes the count to zero finds somebody to tell. The park does the
     * giving back, once the lock above is released, which is the ordering every
     * durable wait in the runtime uses. */
    b->waiter = g;
    burrow__park(unlock_bubble, b, true);
}

void synctest_sleep(Duration d) {
    time_sleep(d);
    synctest_wait();
}
