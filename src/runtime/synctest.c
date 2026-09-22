/* The synctest bubble. See burrow/synctest.h for what it is for.
 *
 * The whole of this file is two numbers and the places they change.
 *
 * `active` is how many goroutines in the bubble could still get somewhere on
 * their own. It goes up when a goroutine is born into the bubble and when one
 * is woken, and down when one exits and when one parks on something only
 * another goroutine in the bubble can end. When it reaches zero, nothing in
 * here is going to move again without help, and that is the one interesting
 * event a bubble has. Either somebody is sitting in synctest_wait, in which
 * case that is exactly what they asked to be told, or nobody is, in which case
 * the program has deadlocked and saying so is better than hanging.
 *
 * `total` is the same count without the durability, and it is how synctest_run
 * knows the bubble is over. It is not the same question: a bubble with four
 * goroutines blocked on each other has total four and active zero, and one that
 * has finished has both at zero.
 *
 * Raising either count is done outside the lock, which is worth an argument
 * because a count read outside a lock that decides anything is usually wrong. It
 * is sound because once active reads zero nothing can raise it again until
 * somebody acts on it: raising it takes a new goroutine or a wake, both of which
 * take a goroutine in the bubble that is running, and if one were running the
 * count would not have been zero. Dropping a count is under the lock, because a
 * drop can be the one that ends the bubble, and burrow__bubble_leave says why.
 *
 * The bubble lives on synctest_run's own stack, which is not a trick. Nothing
 * outlives the run by construction, so there is no allocation here, no
 * allocator to choose, and no way for making a bubble to fail. What it does cost
 * is that "who might still be looking at this bubble" is a real question rather
 * than one the garbage collector answers for Go, and burrow__bubble_hold is
 * where the answer turns out to be wider than it looks.
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

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

struct burrow__Bubble {
    /* Over the two handoffs and nothing else: which thread gets to wake the
     * goroutine in synctest_wait, and which one gets to wake the goroutine in
     * synctest_run. Those have to happen once each and a count cannot say that,
     * which is the only thing the counts above cannot do on their own. */
    burrow__Lock lock;

    uint32_t total;
    uint32_t active;

    /* The goroutine parked in synctest_wait, or NULL. At most one, because two
     * goroutines each waiting for the other to stop is a test whose meaning
     * nobody can work out, and Go refuses it for the same reason. */
    burrow__G *waiter;

    /* The goroutine that called synctest_run, once it has committed to parking
     * until the bubble is empty. NULL before that, which is how the last
     * goroutine out knows there is nobody to wake yet. */
    burrow__G *parent;
};

static uint32_t dec(uint32_t *p) {
    return burrow__atomic_add_u32(p, (uint32_t)-1) - 1;
}

static uint32_t inc(uint32_t *p) {
    return burrow__atomic_add_u32(p, 1) + 1;
}

/* ------------------------------------------------------------------ settling
 *
 * What to do about a bubble whose counts have just changed. Answers a goroutine
 * to start, which the caller does once it has let the lock go.
 *
 * The order of the three questions is the order of how final they are. An empty
 * bubble is over whatever else is true of it. A bubble with something still
 * running has nothing to decide. What is left is a bubble where everything is
 * blocked, and that is either the thing synctest_wait asked for or a
 * deadlock. */
static burrow__G *settle_locked(burrow__Bubble *b) {
    if (burrow__atomic_load_acquire_u32(&b->total) == 0) {
        burrow__G *p = b->parent;
        b->parent = NULL;
        return p;
    }

    if (burrow__atomic_load_acquire_u32(&b->active) > 0)
        return NULL;

    if (b->waiter != NULL) {
        burrow__G *w = b->waiter;
        b->waiter = NULL;

        /* Counted again here rather than left to the wake, because the lock is
         * about to be released and a second thread that found active at zero
         * would decide this bubble had deadlocked. Taking the flag is what
         * makes the increment happen exactly once even though sched_ready is
         * about to ask the same question. */
        if (burrow__atomic_swap_u32(&w->bubbleblocked, 0) == 1)
            (void)inc(&b->active);
        return w;
    }

    /* Every goroutine in the bubble is waiting for another goroutine in the
     * bubble, and none of them is going to run again. Go stops the program here
     * too, with the same reasoning: a deadlock inside a bubble is knowable,
     * unlike one in a program at large, so there is no reason to hang. */
    runtime_throw(BURROW_S("synctest: all goroutines in the bubble are blocked"));
}

/* ------------------------------------------ what the scheduler reports to us
 *
 * The six calls burrow/sched.h declares, in the order a goroutine meets
 * them. */

void burrow__bubble_join(burrow__Bubble *b) {
    (void)inc(&b->total);
    (void)inc(&b->active);
}

/* Leaving takes the counts down under the lock, and the reason is that dropping
 * a count can make the bubble's memory go away underneath the thread that
 * dropped it.
 *
 * Two goroutines exiting at once, with the decrements outside the lock: the
 * first takes total from two to one, the second takes it to zero, wakes the
 * goroutine in synctest_run, and that one returns and takes the stack frame the
 * bubble lives in with it. The first is by then somewhere inside this function,
 * holding a pointer to memory that has been reused. Doing the decrement under
 * the lock makes the two orders the only two there are, and whichever of them
 * holds the lock last is the one that sees zero. */
void burrow__bubble_leave(burrow__Bubble *b) {
    burrow__lock(&b->lock);
    (void)dec(&b->total);
    (void)dec(&b->active);
    burrow__G *w = settle_locked(b);
    burrow__unlock(&b->lock);

    /* Nothing of b is touched after this, for the reason above. */
    if (w != NULL)
        sched_ready(w);
}

/* A park in flight takes a place in the bubble of its own, exactly the place a
 * goroutine takes, for the stretch between the start of the park and the moment
 * the park's answer is known. That is why these two are join and leave again
 * rather than arithmetic of their own, and there are two separate reasons for
 * it.
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
 * exit and take total to zero, all while this thread is still in the rest of
 * park0 with the bubble pointer in hand. The goroutine's own place does not
 * cover that, because the goroutine is gone before the park is. So a bubble is
 * over when the last park in it has finished and not merely when the last
 * goroutine has. */
void burrow__bubble_hold(burrow__Bubble *b) {
    burrow__bubble_join(b);
}

void burrow__bubble_release(burrow__Bubble *b) {
    burrow__bubble_leave(b);
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

static bool unlock_bubble(burrow__G *g, void *p) {
    (void)g;
    burrow__unlock(&((burrow__Bubble *)p)->lock);
    return true;
}

bool synctest_run(Func f) {
    burrow__G *g = burrow__curg();
    if (g == NULL)
        runtime_throw(BURROW_S("synctest_run: not on a goroutine"));
    if (g->bubble != NULL)
        runtime_throw(BURROW_S("synctest_run: bubbles do not nest"));

    burrow__Bubble b;
    memset(&b, 0, sizeof(b));

    /* The counts start at zero and the goroutine below puts itself in, the same
     * way every other goroutine in the bubble does. Counting it here instead
     * would mean a bubble that go could not start was a bubble with a goroutine
     * in it that does not exist. */
    if (!burrow__go_bubble(f, &b))
        return false;

    burrow__lock(&b.lock);

    /* The bubble may already be over, which is a body that did nothing and
     * finished before this line. Nobody had written the parent down, so nobody
     * tried to wake it, and there is nothing left to wait for. */
    if (burrow__atomic_load_acquire_u32(&b.total) == 0) {
        burrow__unlock(&b.lock);
        return true;
    }

    /* Written under the lock and read under the lock, which is what keeps the
     * last goroutine out of the bubble from trying to start a goroutine that is
     * still running. Until this line there is no parent to find.
     *
     * The park is not durable. This goroutine is not in the bubble, so the
     * bubble has no opinion about it, and a park that said otherwise would be
     * leaving out the one goroutine that is certainly not going to help. */
    b.parent = g;
    burrow__park(unlock_bubble, &b, false);
    return true;
}

/* -------------------------------------------------------------------- wait */

void synctest_wait(void) {
    burrow__G *g = burrow__curg();
    if (g == NULL || g->bubble == NULL)
        runtime_throw(BURROW_S("synctest_wait: not inside a bubble"));

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
