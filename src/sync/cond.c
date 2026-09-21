/* sync.Cond. See burrow/sync.h.
 *
 * Almost nothing, because the hard part is the notify list in the runtime and
 * this is the four lines on top of it. The four lines are still worth reading:
 * the order of the ticket, the unlock and the wait is the whole correctness of
 * a condition variable, and getting it wrong gives a program that works for
 * months and then hangs.
 *
 * Derived from Go's src/sync/cond.go.
 * Go source: go1.27.1.
 *
 * Copyright 2009 The Go Authors. All rights reserved.
 * Copyright 2026 The burrow Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style licence that can be found
 * in the LICENSE file. */

#include "burrow/sync.h"

#include "burrow/core.h"
#include "burrow/runtime.h"
#include "burrow/sema.h"
#include "burrow/sync/atomic.h"
#include "burrow/type.h"

#include <stddef.h>
#include <stdint.h>

static const Type cond_desc = {
    {(const Byte *)"Cond", 4},
    {(const Byte *)"sync", 4},
    KIND_STRUCT,
    (uint32_t)sizeof(SyncCond),
    (uint16_t)_Alignof(SyncCond),
    0,
    0,
    NULL,
    NULL,
    NULL,
    NULL,
    0,
    0x73636e64U, /* "scnd", distinct from every builtin's */
    NULL,
};

const Type *const TYPE_SYNC_COND = &cond_desc;

/* Stops a Cond that has been copied from being used.
 *
 * A Cond holds a queue, and a copy holds the same queue with a different
 * address, so a waiter sleeping on one of them can be signalled through the
 * other and never wake up. Go has go vet to catch this at build time. C does
 * not, so it is caught here, at the cost of one atomic load on a path that is
 * already about to take a lock.
 *
 * The first use writes this Cond's own address into the checker. Every later
 * use compares against it, and a mismatch means the struct was copied after
 * that first use. The compare and swap in the middle is for two goroutines
 * arriving at an untouched Cond at the same time: one of them wins and the
 * other reads the winner's value, which is the right one, so the third check
 * has to happen before anybody concludes anything.
 *
 * A copy taken before the first use is not caught, and cannot be: nothing has
 * happened yet that could tell the two apart. Go has the same hole. */
static void check_copy(SyncCond *c) {
    Uintptr self = (Uintptr)(void *)c;

    if (sync_atomic_load_uintptr(&c->checker) == self)
        return;
    if (sync_atomic_compare_and_swap_uintptr(&c->checker, 0, self))
        return;
    if (sync_atomic_load_uintptr(&c->checker) == self)
        return;

    runtime_panic(BURROW_S("sync: Cond is copied"));
}

void sync_cond_wait(SyncCond *c) {
    check_copy(c);

    /* Three steps, in this order, and the order is the point.
     *
     * The ticket is taken while the caller still holds the lock, so it is
     * ordered against whatever the caller has just seen about the condition.
     * Then the lock goes, which is the window where a signal can arrive. Then
     * the wait, which sees from the ticket that it has already been signalled
     * and returns rather than sleeping through it.
     *
     * Take the ticket after dropping the lock instead and that window is a lost
     * wakeup: the signal goes out to nobody, and the waiter goes to sleep a
     * moment later with nothing left to wake it. */
    uint32_t t = burrow__notify_list_add(&c->notify);
    sync_locker_unlock(c->l);
    burrow__notify_list_wait(&c->notify, t);
    sync_locker_lock(c->l);
}

void sync_cond_signal(SyncCond *c) {
    check_copy(c);
    burrow__notify_list_notify_one(&c->notify);
}

void sync_cond_broadcast(SyncCond *c) {
    check_copy(c);
    burrow__notify_list_notify_all(&c->notify);
}
