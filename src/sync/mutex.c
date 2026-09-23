/* sync.Mutex. See burrow/sync.h for what it is and when to reach for it.
 *
 * One int32 holds everything: the lock bit, a woken bit, a starving bit and the
 * number of waiters in the bits above. That is what makes locking an
 * uncontended mutex a single compare and swap on a word the caller already has
 * in cache, and it is why the slow path below is written as one loop over a
 * compare and swap rather than as a sequence of steps.
 *
 * The part worth understanding is the starving bit, because it is the answer to
 * the question every fair lock has to answer. A mutex that always hands the
 * lock to whoever is running is fast and can starve a queue forever. A mutex
 * that always hands it to the front of the queue is fair and pays a scheduling
 * round trip on every single unlock, which on a hot lock is most of the cost of
 * the program. Go's answer, and therefore this one, is to be the first kind
 * until somebody has been waiting more than a millisecond and the second kind
 * until the queue drains. The threshold is the one number in here that was
 * tuned rather than derived, and it is Go's number.
 *
 * Derived from Go's src/internal/sync/mutex.go.
 * Go source: go1.27.1.
 *
 * Copyright 2024 The Go Authors. All rights reserved.
 * Copyright 2026 The burrow Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style licence that can be found
 * in the LICENSE file. */

#include "burrow/sync.h"

#include "burrow/clock.h"
#include "burrow/core.h"
#include "burrow/runtime.h"
#include "burrow/sema.h"
#include "burrow/sync/atomic.h"
#include "burrow/type.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* The four fields of the state word. The lock bit and the starving bit are
 * declared in burrow/sync.h, because the inline fast paths there test them, and
 * the other two are only ever looked at in here.
 *
 * All four are macros rather than an enum so that they are int32_t and nothing
 * else. An enum constant has an implementation defined type, and MSVC refuses
 * to combine constants from two different unnamed enums with a bitwise or,
 * which is exactly what testing the lock bit and the woken bit together does. */
#define MUTEX_LOCKED ((int32_t)BURROW_MUTEX_LOCKED)
#define MUTEX_STARVING ((int32_t)BURROW_MUTEX_STARVING)

/* Somebody has been woken or is spinning and is about to try for the lock, so
 * an unlock does not need to wake anybody else. Without it every unlock on a
 * contended mutex wakes a waiter that then finds the lock already taken by a
 * spinner and goes back to sleep. */
#define MUTEX_WOKEN ((int32_t)2)

/* Everything above the three flag bits is the number of waiters. */
#define MUTEX_WAITER_SHIFT 3

/* How long a waiter has to have been queued before it decides the mutex is
 * unfair and switches it into starvation mode. One millisecond, which is Go's
 * number: long enough that an ordinary contended handoff never reaches it, and
 * short enough that a goroutine which does reach it has already lost. */
#define STARVATION_THRESHOLD_NS 1000000

static const Type mutex_desc = {
    {(const Byte *)"Mutex", 5},
    {(const Byte *)"sync", 4},
    KIND_STRUCT,
    (uint32_t)sizeof(SyncMutex),
    (uint16_t)_Alignof(SyncMutex),
    0,
    0,
    NULL,
    NULL,
    NULL,
    NULL,
    0,
    0x736d7578U, /* "smux", distinct from every builtin's */
    NULL,
};

const Type *const TYPE_SYNC_MUTEX = &mutex_desc;

/* How many goroutines are queued, read out of a state word. Unsigned, because a
 * shift of a negative number is not something C defines and the waiter count
 * reaching the top bit is not something this has to rule out by hand. */
static int32_t mutex_waiters_of(int32_t state) {
    return (int32_t)((uint32_t)state >> MUTEX_WAITER_SHIFT);
}

void burrow__sync_mutex_lock_slow(SyncMutex *m) {
    int64_t wait_start = 0;
    bool starving = false;
    bool awoke = false;
    int32_t iter = 0;
    int32_t old = sync_atomic_load_int32(&m->state);

    for (;;) {
        /* Spin only when the lock is held and the mutex is running normally.
         * In starvation mode the lock is handed to the front of the queue, so
         * spinning for it cannot succeed and is pure waste. */
        if ((old & (MUTEX_LOCKED | MUTEX_STARVING)) == MUTEX_LOCKED &&
            burrow__sync_can_spin(iter)) {
            /* Tell the unlocker not to wake anybody, since this spinner is
             * about to take the lock and a woken waiter would only find it
             * gone. Worth doing only when there is somebody to not wake. */
            if (!awoke && (old & MUTEX_WOKEN) == 0 && mutex_waiters_of(old) != 0 &&
                sync_atomic_compare_and_swap_int32(&m->state, old, old | MUTEX_WOKEN))
                awoke = true;

            burrow__sync_do_spin();
            iter++;
            old = sync_atomic_load_int32(&m->state);
            continue;
        }

        int32_t next = old;

        /* A starving mutex belongs to the queue, so a goroutine arriving now
         * joins the queue rather than trying for the lock. */
        if ((old & MUTEX_STARVING) == 0)
            next |= MUTEX_LOCKED;
        if ((old & (MUTEX_LOCKED | MUTEX_STARVING)) != 0)
            next += 1 << MUTEX_WAITER_SHIFT;

        /* Switch the mutex into starvation mode, but not if it happens to be
         * unlocked right now: unlock assumes a starving mutex has waiters, and
         * this goroutine is about to become the holder rather than a waiter. */
        if (starving && (old & MUTEX_LOCKED) != 0)
            next |= MUTEX_STARVING;

        if (awoke) {
            if ((next & MUTEX_WOKEN) == 0)
                runtime_throw(BURROW_S("sync: inconsistent mutex state"));
            next &= ~MUTEX_WOKEN;
        }

        if (!sync_atomic_compare_and_swap_int32(&m->state, old, next)) {
            old = sync_atomic_load_int32(&m->state);
            continue;
        }

        if ((old & (MUTEX_LOCKED | MUTEX_STARVING)) == 0)
            break; /* the compare and swap took the lock */

        /* Somebody who has waited before goes back to the front of the queue
         * rather than the back, because being woken and losing the race is not
         * a reason to start queueing again from scratch. */
        bool lifo = wait_start != 0;
        if (wait_start == 0)
            wait_start = burrow__nanotime();

        /* Never durable. A goroutine waiting for a mutex is waiting for the
         * goroutine holding it, and that one is running, so a synctest bubble
         * full of these has not gone idle. */
        burrow__sema_acquire(&m->sema, lifo, false);

        starving =
            starving || burrow__nanotime() - wait_start > STARVATION_THRESHOLD_NS;
        old = sync_atomic_load_int32(&m->state);

        if ((old & MUTEX_STARVING) != 0) {
            /* Woken in starvation mode, which means the lock was handed over
             * rather than raced for. The state has not been fixed up yet: the
             * locked bit is not set and this goroutine is still counted as a
             * waiter, and setting both right is this goroutine's job. */
            if ((old & (MUTEX_LOCKED | MUTEX_WOKEN)) != 0 || mutex_waiters_of(old) == 0)
                runtime_throw(BURROW_S("sync: inconsistent mutex state"));

            int32_t delta = MUTEX_LOCKED - (1 << MUTEX_WAITER_SHIFT);

            /* Leave starvation mode when this waiter did not have to wait long
             * or when it was the last one in the queue. Doing it here rather
             * than on the next unlock is what stops two goroutines handing the
             * lock back and forth in starvation mode forever. */
            if (!starving || mutex_waiters_of(old) == 1)
                delta -= MUTEX_STARVING;

            sync_atomic_add_int32(&m->state, delta);
            break;
        }

        awoke = true;
        iter = 0;
    }
}

void burrow__sync_mutex_unlock_slow(SyncMutex *m, int32_t next) {
    if (((next + MUTEX_LOCKED) & MUTEX_LOCKED) == 0)
        runtime_throw(BURROW_S("sync: unlock of unlocked mutex"));

    if ((next & MUTEX_STARVING) != 0) {
        /* Starving, so hand the lock straight to the front of the queue and
         * give this thread's turn up so that the waiter actually runs. The
         * locked bit stays clear and the waiter sets it when it wakes; until
         * then the starving bit is what keeps new arrivals out. */
        burrow__sema_release(&m->sema, true);
        return;
    }

    int32_t old = next;
    for (;;) {
        /* Nobody queued, or somebody is already awake and coming for the lock,
         * or the mutex went into starvation mode after this unlock started, in
         * which case the handoff chain is somebody else's and this call is not
         * part of it. */
        if (mutex_waiters_of(old) == 0 ||
            (old & (MUTEX_LOCKED | MUTEX_WOKEN | MUTEX_STARVING)) != 0)
            return;

        int32_t want = (old - (1 << MUTEX_WAITER_SHIFT)) | MUTEX_WOKEN;
        if (sync_atomic_compare_and_swap_int32(&m->state, old, want)) {
            burrow__sema_release(&m->sema, false);
            return;
        }
        old = sync_atomic_load_int32(&m->state);
    }
}

/* ------------------------------------------------------------ as a Locker */

static void mutex_locker_lock(void *self) {
    sync_mutex_lock((SyncMutex *)self);
}

static void mutex_locker_unlock(void *self) {
    sync_mutex_unlock((SyncMutex *)self);
}

static const SyncLockerVT mutex_locker_vt = {
    &mutex_desc,
    mutex_locker_lock,
    mutex_locker_unlock,
};

SyncLocker sync_mutex_locker(SyncMutex *m) {
    return (SyncLocker){&mutex_locker_vt, m};
}

void sync_locker_lock(SyncLocker l) {
    if (l.vt == NULL)
        runtime_panic(BURROW_S("sync: lock of a nil Locker"));
    l.vt->lock(l.data);
}

void sync_locker_unlock(SyncLocker l) {
    if (l.vt == NULL)
        runtime_panic(BURROW_S("sync: unlock of a nil Locker"));
    l.vt->unlock(l.data);
}
