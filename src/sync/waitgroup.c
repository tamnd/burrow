/* sync.WaitGroup. See burrow/sync.h for what it is and how to use it.
 *
 * One uint64 holds both halves: the counter of outstanding work in the high 32
 * bits and the number of goroutines waiting for it in the low ones. That is
 * what lets Add take the counter down and find out whether anybody is waiting
 * in a single atomic, which is the whole trick, because the two questions have
 * to be answered together or a wakeup goes missing.
 *
 * The misuse checks are Go's and they are worth keeping rather than trimming.
 * A WaitGroup that is added to while somebody is already waiting is a program
 * whose bug will not show up on the machine it was written on, and the checks
 * here turn it into something that stops immediately and says what happened.
 *
 * Derived from Go's src/sync/waitgroup.go.
 * Go source: go1.27.1.
 *
 * Copyright 2011 The Go Authors. All rights reserved.
 * Copyright 2026 The burrow Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style licence that can be found
 * in the LICENSE file. */

#include "burrow/sync.h"

#include "burrow/atomic.h"
#include "burrow/core.h"
#include "burrow/func.h"
#include "burrow/mem.h"
#include "burrow/mem/heap.h"
#include "burrow/proc.h"
#include "burrow/runtime.h"
#include "burrow/sched.h"
#include "burrow/sema.h"
#include "burrow/sync/atomic.h"
#include "burrow/type.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* Bit 31 of the state word marks a group that belongs to a synctest bubble, so
 * the waiter count is the low half with that bit taken out. Which bubble is in
 * the field next to the state. */
#define WAIT_GROUP_WAITER_MASK ((uint32_t)0x7fffffffU)
#define WAIT_GROUP_BUBBLE_FLAG ((uint64_t)0x80000000U)

static const Type wait_group_desc = {
    {(const Byte *)"WaitGroup", 9},
    {(const Byte *)"sync", 4},
    KIND_STRUCT,
    (uint32_t)sizeof(SyncWaitGroup),
    (uint16_t)_Alignof(SyncWaitGroup),
    0,
    0,
    NULL,
    NULL,
    NULL,
    NULL,
    0,
    0x73776774U, /* "swgt", distinct from every builtin's */
    NULL,
};

const Type *const TYPE_SYNC_WAIT_GROUP = &wait_group_desc;

/* The counter, which is the signed high half. */
static int32_t counter_of(uint64_t state) {
    return (int32_t)(uint32_t)(state >> 32);
}

/* How many are waiting, which is the low half with the bubble bit removed. */
static uint32_t waiters_of(uint64_t state) {
    return (uint32_t)state & WAIT_GROUP_WAITER_MASK;
}

/* ------------------------------------------------------------------ bubbles
 *
 * A Wait is durable when every Add that put work into this group came from the
 * bubble the waiter is in, because then the only thing that can take the
 * counter back down is a goroutine in that bubble. Both halves of that have to
 * be checked, and mixing them is a misuse that stops the program, the same as
 * the other WaitGroup misuses here: a group that half a bubble is using is a
 * program whose bug shows up as a test that hangs on somebody else's machine.
 *
 * Go keeps the association in a table in its runtime. Here it is a field, which
 * is the same information without the hash and the lock. */

/* True when this group belongs to the bubble the caller is in. NULL is not a
 * bubble, so a caller outside every bubble is never associated with anything. */
static bool associated(const SyncWaitGroup *wg, const void *bubble) {
    return bubble != NULL &&
           burrow__atomic_load_acquire_ptr((void *const *)&wg->bubble) == bubble;
}

/* Puts the group in the caller's bubble if it is not in one already. Stops the
 * program if it is already in a different one.
 *
 * The compare and swap can only be lost to another goroutine doing this at the
 * same moment, and two goroutines in the same bubble racing to associate the
 * same group is ordinary, so losing it is only a problem when the winner is
 * from somewhere else. */
static void associate(SyncWaitGroup *wg, void *bubble) {
    void *have = NULL;
    if (burrow__atomic_cas_ptr(&wg->bubble, &have, bubble))
        return;
    if (have != bubble)
        runtime_throw(BURROW_S("sync: WaitGroup.Add called from two synctest bubbles"));
}

void sync_wait_group_add(SyncWaitGroup *wg, int delta) {
    /* Claiming the group for this bubble happens before the counter moves, and
     * the flag rather than the field is what answers the misuse question,
     * because the flag and the counter are one word and so cannot disagree. A
     * previous state that was not zero and had no flag is a group that somebody
     * outside the bubble has already put work into. */
    void *bubble = burrow__curbubble();
    if (bubble != NULL) {
        associate(wg, bubble);

        uint64_t prev = sync_atomic_or_uint64(&wg->state, WAIT_GROUP_BUBBLE_FLAG);
        if (prev != 0 && (prev & WAIT_GROUP_BUBBLE_FLAG) == 0)
            runtime_throw(BURROW_S("sync: WaitGroup.Add called from inside and outside "
                                   "a synctest bubble"));
    }

    /* Delta goes into the high half. Shifting it as unsigned is what makes a
     * negative delta a borrow out of the high half, which is the arithmetic
     * this wants and which shifting a signed value does not define. */
    uint64_t state =
        sync_atomic_add_uint64(&wg->state, (uint64_t)(uint32_t)(int32_t)delta << 32);

    /* The other way round: the group is in a bubble and this caller is not in
     * it. Read from the same word the add returned, so there is no second look
     * for the flag to change under. */
    if ((state & WAIT_GROUP_BUBBLE_FLAG) != 0 && bubble == NULL)
        runtime_throw(BURROW_S(
            "sync: WaitGroup.Add called from inside and outside a synctest bubble"));

    int32_t v = counter_of(state);
    uint32_t w = waiters_of(state);

    if (v < 0)
        runtime_panic(BURROW_S("sync: negative WaitGroup counter"));

    /* Somebody is waiting and this call took the counter up from zero, so the
     * Add that was supposed to happen before the Wait did not. */
    if (w != 0 && delta > 0 && v == delta)
        runtime_panic(
            BURROW_S("sync: WaitGroup misuse: Add called concurrently with Wait"));

    if (v > 0 || w == 0)
        return;

    /* This call took the counter to zero with waiters queued, so from here
     * nothing else may be changing the state: an Add cannot race a Wait, and a
     * Wait that sees a zero counter does not queue. A cheap check that the
     * state really did stand still is worth it, since the cases it catches are
     * the ones that would otherwise be a hang somewhere else entirely. */
    if (sync_atomic_load_uint64(&wg->state) != state)
        runtime_panic(
            BURROW_S("sync: WaitGroup misuse: Add called concurrently with Wait"));

    /* Clear both halves before waking anybody, so that the group is ready for
     * a second round the moment the last waiter returns. The bubble flag goes
     * with them, and the field it points at goes next, because this is the one
     * moment where nothing can be adding and nothing can be waiting. A group
     * that kept the association would be one that a later bubble could not
     * use. */
    sync_atomic_store_uint64(&wg->state, 0);
    if (bubble != NULL)
        burrow__atomic_store_release_ptr(&wg->bubble, NULL);

    for (; w != 0; w--)
        burrow__sema_release(&wg->sema, false);
}

void sync_wait_group_done(SyncWaitGroup *wg) {
    sync_wait_group_add(wg, -1);
}

void sync_wait_group_wait(SyncWaitGroup *wg) {
    void *bubble = burrow__curbubble();

    for (;;) {
        uint64_t state = sync_atomic_load_uint64(&wg->state);

        if (counter_of(state) == 0) {
            /* Nothing to wait for, and nobody else waiting either, so this is
             * the other moment where the association can be let go. Add does
             * the same thing when it takes the counter to zero, and between
             * them a group that has gone quiet does not hold on to a bubble
             * that is about to end. */
            if (waiters_of(state) == 0 && (state & WAIT_GROUP_BUBBLE_FLAG) != 0 &&
                associated(wg, bubble) &&
                sync_atomic_compare_and_swap_uint64(&wg->state, state, 0))
                burrow__atomic_store_release_ptr(&wg->bubble, NULL);
            return;
        }

        /* One more waiter, which is one on the low half. The compare and swap
         * is what makes this safe against an Add landing in between: if it
         * fails the counter may now be zero and the loop looks again. */
        if (sync_atomic_compare_and_swap_uint64(&wg->state, state, state + 1)) {
            /* Durable when every Add came from the bubble this goroutine is in,
             * because then the Done that ends this wait has to come from in
             * there too. The flag is read from the state this waiter just
             * counted itself into, so an Add from outside cannot have slipped
             * in unnoticed: it would have stopped the program. */
            bool durable =
                (state & WAIT_GROUP_BUBBLE_FLAG) != 0 && associated(wg, bubble);

            burrow__sema_acquire(&wg->sema, false, durable);

            /* Add zeroes the state before releasing anybody, so a state that
             * is not zero here means somebody started a second round of work
             * before this waiter got out of the first one. */
            if (sync_atomic_load_uint64(&wg->state) != 0)
                runtime_panic(BURROW_S(
                    "sync: WaitGroup is reused before previous Wait has returned"));

            return;
        }
    }
}

/* ------------------------------------------------------------------ Go
 *
 * The goroutine needs somewhere to keep the group and the function, and a
 * goroutine takes one pointer, so the pair goes in a small allocation that the
 * goroutine frees on its way out.
 *
 * Go's version needs no such thing, because a closure there is a heap
 * allocation the collector deals with. This is the same allocation written
 * down. */
typedef struct Task {
    SyncWaitGroup *wg;
    Func f;
} Task;

static void run_task(void *env) {
    Task *t = (Task *)env;
    SyncWaitGroup *wg = t->wg;
    Func f = t->f;

    mem_free(heap_allocator(), t, sizeof *t, _Alignof(Task));

    /* Not deferred, and that is on purpose. A panic nobody recovers inside a
     * goroutine ends the program, and it has to end it with the counter still
     * up: taking one off here would let a Wait somewhere else return and race
     * the shutdown, which can mean a process that exits zero while it is in
     * the middle of reporting a crash. Go's WaitGroup.Go says the same thing
     * in a recover that re-panics. */
    BURROW_CALLF0(f);

    sync_wait_group_done(wg);
}

bool sync_wait_group_go(SyncWaitGroup *wg, Func f) {
    Task *t = (Task *)mem_alloc(heap_allocator(), sizeof *t, _Alignof(Task));
    if (t == NULL)
        return false;

    t->wg = wg;
    t->f = f;

    sync_wait_group_add(wg, 1);

    if (!go(BURROW_FN(Func, run_task, t))) {
        /* Nothing was started, so the group has to end up where it began
         * rather than holding a Wait open for work that does not exist. */
        sync_wait_group_add(wg, -1);
        mem_free(heap_allocator(), t, sizeof *t, _Alignof(Task));
        return false;
    }

    return true;
}
