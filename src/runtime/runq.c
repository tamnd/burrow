/* The run queues. See burrow/sched.h for what they are and why there are three
 * of them.
 *
 * Every algorithm in this file is Go's, from runtime/proc.go, and the places
 * where it looks like there is a simpler way to write something are the places
 * to read most carefully. The ring is a single producer queue at one end and a
 * multiple consumer queue at the other, which is a shape with a well known set
 * of ways to get it subtly wrong, and the memory ordering below is the part that
 * makes it right:
 *
 *   the owner adds        relaxed store to runq[t], release store to runqtail
 *   the owner takes       acquire load of runqhead, release compare and swap
 *   a thief takes         acquire loads of both, release compare and swap
 *
 * The release on the tail is what publishes the slot: a thief that sees the new
 * tail is guaranteed to see the pointer written into the array before it, which
 * is why the store into the array itself can be relaxed. The acquire on the head
 * is the other half, and it is what stops the owner writing into a slot a thief
 * has not finished reading. Neither end needs a read modify write to add work,
 * which is the whole reason for the shape.
 *
 * The indices are free running 32 bit counters that are masked when they index
 * the array, so the queue is empty when head equals tail and full when tail
 * minus head is the size of it. Unsigned subtraction is what makes that keep
 * working when the counters wrap, and they are allowed to wrap: 2^32 is four
 * billion goroutines through one P, and the difference between the two is
 * always less than 256, so the subtraction is correct across the wrap and every
 * comparison in this file is on the difference rather than on the counters.
 *
 * Copyright 2026 The burrow Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style licence that can be found
 * in the LICENSE file. */

#include "burrow/sched.h"

#include "burrow/atomic.h"
#include "burrow/core.h"
#include "burrow/runtime.h"
#include "burrow/thread.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* The ring is a power of two, so the wrap is a mask rather than a division. */
#define RUNQ_MASK ((uint32_t)BURROW_RUNQ_SIZE - 1U)

/* Half the ring plus the goroutine that did not fit, which is the largest batch
 * an overflowing put can move at once. */
#define BATCH_MAX (BURROW_RUNQ_SIZE / 2 + 1)

/* The runnext slot is a pointer that other Ps compare and swap, so every touch
 * of it goes through the atomics. These two are here to keep the casts in one
 * place: the atomic pointer operations are spelled in void *, which is the only
 * spelling that works for every pointer type, and a cast at each of the six call
 * sites below would be six chances to cast the wrong thing. */
static burrow__G *load_runnext(const burrow__P *p) {
    return (burrow__G *)burrow__atomic_load_acquire_ptr((void *const *)&p->runnext);
}

static bool cas_runnext(burrow__P *p, burrow__G **expected, burrow__G *want) {
    return burrow__atomic_cas_ptr((void **)&p->runnext, (void **)expected,
                                  (void *)want);
}

/* The ring slots, masked on the way in so that no caller has to remember to.
 *
 * Relaxed and not plain, which is worth being clear about because it looks like
 * pedantry and is not. The indices guarantee that the slot the owner is writing
 * is never the slot a thief is reading at that instant, so on the face of it a
 * plain write and a plain read would do. They would not. A thief loads the head
 * and the tail, works out which slots it wants, and then can be descheduled for
 * as long as the scheduler likes before it reads them, and in that time the
 * owner can go all the way round the ring and write over the exact slot the
 * thief is about to read. The thief's compare and swap on the head then fails
 * and the value it read is thrown away, so nothing goes wrong, but the read and
 * the write really did overlap and in C that is a data race with undefined
 * behaviour rather than a stale value. Go has the same overlap and gets away
 * with it because its race detector does not look at its own runtime.
 *
 * Relaxed is enough because none of the ordering lives here. The release store
 * to runqtail is what publishes a slot and the acquire load of it is what makes
 * the contents visible, so all these two have to do is be indivisible and stay
 * where they are written. On every architecture burrow targets that compiles to
 * the same load and store a plain access would have. */
static void store_slot(burrow__P *p, uint32_t i, burrow__G *g) {
    burrow__atomic_store_relaxed_ptr((void **)&p->runq[i & RUNQ_MASK], (void *)g);
}

static burrow__G *load_slot(const burrow__P *p, uint32_t i) {
    return (burrow__G *)burrow__atomic_load_relaxed_ptr(
        (void *const *)&p->runq[i & RUNQ_MASK]);
}

/* ------------------------------------------------------------------ the ring */

bool burrow__runq_put(burrow__P *p, burrow__G *g, bool next, burrow__G **overflow) {
    if (next) {
        /* A loop rather than a single compare and swap because a thief can take
         * runnext out from under this, and losing that race means trying again
         * rather than giving up: the caller asked for this goroutine to go next
         * and the slot is empty now, which is a better outcome than before. */
        burrow__G *old = load_runnext(p);
        while (!cas_runnext(p, &old, g)) {
            /* The compare and swap writes back what it found, so the next
             * attempt is against the current value. */
        }

        if (old == NULL)
            return true;

        /* Whatever was in the slot is still runnable and now has nowhere to be,
         * so it goes on the ring in place of the one that displaced it. */
        g = old;
    }

    uint32_t h = burrow__atomic_load_acquire_u32(&p->runqhead);

    /* A plain read, because this is the only thread that writes it. A thief
     * reads it atomically, and two reads never race with each other. */
    uint32_t t = p->runqtail;

    if (t - h < (uint32_t)BURROW_RUNQ_SIZE) {
        store_slot(p, t, g);
        burrow__atomic_store_release_u32(&p->runqtail, t + 1U);
        return true;
    }

    /* `g` and not the argument, because of the swap above. */
    *overflow = g;
    return false;
}

burrow__G *burrow__runq_get(burrow__P *p) {
    burrow__G *next = load_runnext(p);
    if (next != NULL) {
        burrow__G *expected = next;
        if (cas_runnext(p, &expected, NULL))
            return next;

        /* A thief got there first, which is unusual and is not worth retrying
         * for: the slot is empty now and the ring is the next place to look. */
    }

    for (;;) {
        uint32_t h = burrow__atomic_load_acquire_u32(&p->runqhead);
        uint32_t t = p->runqtail;

        if (t == h)
            return NULL;

        burrow__G *g = load_slot(p, h);

        /* The compare and swap is here even though this is the P's own queue,
         * because thieves take from this end too. Reading the slot before the
         * swap is safe for the same reason it is in Go: a thief that wins takes
         * the same goroutine and this side notices by failing and looking
         * again. */
        uint32_t expected = h;
        if (burrow__atomic_cas_release_u32(&p->runqhead, &expected, h + 1U))
            return g;
    }
}

uint32_t burrow__runq_len(const burrow__P *p) {
    for (;;) {
        uint32_t h = burrow__atomic_load_acquire_u32(&p->runqhead);
        uint32_t t = burrow__atomic_load_acquire_u32(&p->runqtail);
        burrow__G *next = load_runnext(p);

        /* Reading the tail again is what makes this a snapshot rather than three
         * unrelated numbers. Without it, an owner that adds a goroutine to the
         * ring and then moves it into runnext between the first two loads would
         * be counted twice, and a queue with one goroutine on it would report
         * two. Go's runqempty has the same re-read for the same reason. */
        if (t == burrow__atomic_load_acquire_u32(&p->runqtail))
            return (t - h) + (next != NULL ? 1U : 0U);
    }
}

bool burrow__runq_put_slow(burrow__P *p, burrow__G *g, burrow__GQueue *batch) {
    burrow__G *taken[BATCH_MAX];

    uint32_t h = burrow__atomic_load_acquire_u32(&p->runqhead);
    uint32_t t = p->runqtail;
    uint32_t n = (t - h) / 2U;

    if (n == 0)
        return false;

    /* Copied out before the head moves, and linked together only after it has.
     * The other order looks equivalent and is not: until the compare and swap
     * below succeeds these goroutines are still in the ring, a thief can still
     * take them, and writing `next` on one of them would be writing into a list
     * that thief is building. */
    for (uint32_t i = 0; i < n; i++)
        taken[i] = load_slot(p, h + i);

    uint32_t expected = h;
    if (!burrow__atomic_cas_release_u32(&p->runqhead, &expected, h + n))
        return false;

    /* The new one goes last, so the goroutines that have been waiting longest
     * are the ones that end up on the global queue and the freshest one stays
     * nearest the P that made it. */
    taken[n] = g;

    for (uint32_t i = 0; i <= n; i++)
        burrow__gqueue_push(batch, taken[i]);

    return true;
}

/* --------------------------------------------------------------- the stealing
 *
 * Takes half of the victim's queue and writes it straight into the thief's ring
 * starting at `at`, which the caller passes as the thief's own tail. Going
 * directly into the ring rather than through a temporary is what makes a steal
 * one pass over the goroutines instead of two.
 *
 * Nothing is published until the thief moves its own tail, which the caller does
 * after this returns, so the goroutines sitting in those slots are invisible to
 * anybody stealing from the thief in the meantime. */
static uint32_t runq_grab(burrow__P *victim, burrow__P *thief, uint32_t at,
                          bool steal_runnext) {
    for (;;) {
        uint32_t h = burrow__atomic_load_acquire_u32(&victim->runqhead);
        uint32_t t = burrow__atomic_load_acquire_u32(&victim->runqtail);

        /* Round up rather than down, so that a queue with one goroutine on it
         * gives that one up instead of nothing. Half of one is zero and half of
         * one rounded up is one, and the difference between those two is whether
         * work stealing does anything at all on a lightly loaded program. */
        uint32_t n = t - h;
        n = n - n / 2U;

        if (n == 0) {
            if (!steal_runnext)
                return 0;

            burrow__G *next = load_runnext(victim);
            if (next == NULL)
                return 0;

            /* Backing off before taking the one goroutine the victim is about to
             * run. The case this is for is a goroutine that readies another and
             * then immediately blocks, which is what every channel send looks
             * like: in the window between those two the victim's queue is empty
             * and its runnext is exactly the goroutine that should stay there.
             * Taking it moves the work to a different core for no reason and
             * then the victim steals it back.
             *
             * Go sleeps three microseconds here, which is roughly fifty times a
             * channel handoff. There is no monotonic sleep in this runtime yet,
             * so this yields instead, which is also what Go does on the
             * platforms whose timers are too coarse to sleep that briefly. It
             * becomes the sleep when timers land. */
            if (burrow__atomic_load_acquire_u32(&victim->status) == BURROW_PRUNNING)
                burrow__thread_yield();

            burrow__G *expected = next;
            if (!cas_runnext(victim, &expected, NULL))
                continue;

            store_slot(thief, at, next);
            return 1;
        }

        /* More than half the ring means the two loads above straddled the
         * victim's own progress and the numbers do not describe any moment that
         * existed. Read them again. */
        if (n > (uint32_t)(BURROW_RUNQ_SIZE / 2))
            continue;

        for (uint32_t i = 0; i < n; i++)
            store_slot(thief, at + i, load_slot(victim, h + i));

        uint32_t expected = h;
        if (burrow__atomic_cas_release_u32(&victim->runqhead, &expected, h + n))
            return n;
    }
}

burrow__G *burrow__runq_steal(burrow__P *thief, burrow__P *victim, bool steal_runnext) {
    uint32_t t = thief->runqtail;
    uint32_t n = runq_grab(victim, thief, t, steal_runnext);

    if (n == 0)
        return NULL;

    /* The last one of the batch is the answer and never becomes visible in the
     * ring, because the tail below stops one short of it. A goroutine that is
     * about to run does not need to be put somewhere first and taken out
     * again. */
    n--;
    burrow__G *g = load_slot(thief, t + n);

    if (n == 0)
        return g;

    uint32_t h = burrow__atomic_load_acquire_u32(&thief->runqhead);
    if (t - h + n >= (uint32_t)BURROW_RUNQ_SIZE)
        runtime_throw(BURROW_S("runqsteal: runq overflow"));

    burrow__atomic_store_release_u32(&thief->runqtail, t + n);
    return g;
}

/* ------------------------------------------------------------ the global queue
 *
 * A singly linked list with a tail pointer, so that adding a batch of a hundred
 * and twenty nine goroutines is two pointer writes rather than a hundred and
 * twenty nine. No atomics anywhere in here: every one of these runs under the
 * scheduler's lock, and a lock that is already being held is not made safer by
 * atomics underneath it. */

void burrow__gqueue_push(burrow__GQueue *q, burrow__G *g) {
    g->next = NULL;

    if (q->tail == NULL)
        q->head = g;
    else
        q->tail->next = g;

    q->tail = g;
    q->len++;
}

void burrow__gqueue_push_head(burrow__GQueue *q, burrow__G *g) {
    g->next = q->head;
    q->head = g;

    if (q->tail == NULL)
        q->tail = g;

    q->len++;
}

burrow__G *burrow__gqueue_pop(burrow__GQueue *q) {
    burrow__G *g = q->head;
    if (g == NULL)
        return NULL;

    q->head = g->next;
    if (q->head == NULL)
        q->tail = NULL;

    /* Cleared on the way out, so that a goroutine which is not on a list never
     * carries a pointer to one it used to be on. That is worth a store here
     * because the alternative is a stale link that looks live in a debugger and
     * in every leak check written later. */
    g->next = NULL;
    q->len--;
    return g;
}

void burrow__gqueue_push_all(burrow__GQueue *dst, burrow__GQueue *src) {
    if (src->head == NULL)
        return;

    if (dst->tail == NULL)
        dst->head = src->head;
    else
        dst->tail->next = src->head;

    dst->tail = src->tail;
    dst->len += src->len;

    src->head = NULL;
    src->tail = NULL;
    src->len = 0;
}
