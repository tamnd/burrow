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

#include "burrow/core.h"
#include "burrow/func.h"
#include "burrow/mem.h"
#include "burrow/mem/heap.h"
#include "burrow/proc.h"
#include "burrow/runtime.h"
#include "burrow/sema.h"
#include "burrow/sync/atomic.h"
#include "burrow/type.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* Bit 31 of the state word, which Go uses to mark a group that belongs to a
 * synctest bubble. Nothing here sets it, because there are no bubbles yet, and
 * the waiter count is masked with it left out anyway so that the layout does
 * not have to move when testing/synctest arrives. */
#define WAIT_GROUP_WAITER_MASK ((uint32_t)0x7fffffffU)

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

void sync_wait_group_add(SyncWaitGroup *wg, int delta) {
    /* Delta goes into the high half. Shifting it as unsigned is what makes a
     * negative delta a borrow out of the high half, which is the arithmetic
     * this wants and which shifting a signed value does not define. */
    uint64_t state =
        sync_atomic_add_uint64(&wg->state, (uint64_t)(uint32_t)(int32_t)delta << 32);

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
     * a second round the moment the last waiter returns. */
    sync_atomic_store_uint64(&wg->state, 0);

    for (; w != 0; w--)
        burrow__sema_release(&wg->sema, false);
}

void sync_wait_group_done(SyncWaitGroup *wg) {
    sync_wait_group_add(wg, -1);
}

void sync_wait_group_wait(SyncWaitGroup *wg) {
    for (;;) {
        uint64_t state = sync_atomic_load_uint64(&wg->state);

        if (counter_of(state) == 0)
            return;

        /* One more waiter, which is one on the low half. The compare and swap
         * is what makes this safe against an Add landing in between: if it
         * fails the counter may now be zero and the loop looks again. */
        if (sync_atomic_compare_and_swap_uint64(&wg->state, state, state + 1)) {
            burrow__sema_acquire(&wg->sema, false);

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
