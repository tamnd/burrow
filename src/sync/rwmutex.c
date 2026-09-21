/* sync.RWMutex. See burrow/sync.h for what it is and when it is worth having.
 *
 * Writers queue against each other on an ordinary Mutex, which is the easy
 * half. The interesting half is how a writer and the readers get out of each
 * other's way, and it is one number doing two jobs.
 *
 * reader_count is how many readers hold or want the lock. A writer subtracts a
 * billion from it, which does not change how many readers there are but does
 * make the number negative, and every reader that arrives afterwards sees a
 * negative count and waits. So one atomic both announces the writer and closes
 * the door behind it, and a read lock with no writer in sight stays a single
 * add with no branch worth mentioning.
 *
 * What the writer got back from that add tells it how many readers were already
 * inside, and it waits for exactly that many to leave. reader_wait counts them
 * down and the last one out wakes the writer.
 *
 * Derived from Go's src/sync/rwmutex.go.
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

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* More readers than this at once is a bug in the caller rather than a state
 * worth handling, and the number has to leave room for the count to go negative
 * without overflowing. Go's constant. */
#define RWMUTEX_MAX_READERS (1 << 30)

static const Type rwmutex_desc = {
    {(const Byte *)"RWMutex", 7},
    {(const Byte *)"sync", 4},
    KIND_STRUCT,
    (uint32_t)sizeof(SyncRWMutex),
    (uint16_t)_Alignof(SyncRWMutex),
    0,
    0,
    NULL,
    NULL,
    NULL,
    NULL,
    0,
    0x7372776dU, /* "srwm", distinct from every builtin's and from Mutex's */
    NULL,
};

const Type *const TYPE_SYNC_RW_MUTEX = &rwmutex_desc;

/* ------------------------------------------------------------------ readers */

void burrow__sync_rw_mutex_r_lock_slow(SyncRWMutex *rw) {
    burrow__sema_acquire(&rw->reader_sem, false);
}

bool sync_rw_mutex_try_r_lock(SyncRWMutex *rw) {
    for (;;) {
        int32_t c = sync_atomic_load_int32(&rw->reader_count);
        if (c < 0)
            return false;
        if (sync_atomic_compare_and_swap_int32(&rw->reader_count, c, c + 1))
            return true;
    }
}

void burrow__sync_rw_mutex_r_unlock_slow(SyncRWMutex *rw, int32_t r) {
    /* Two ways to get here without a writer pending, and both of them are an
     * unlock of something that was not locked: the count was zero, or it was
     * the negated maximum, which is a writer holding the lock with no readers
     * inside it. */
    if (r + 1 == 0 || r + 1 == -RWMUTEX_MAX_READERS)
        runtime_throw(BURROW_S("sync: r_unlock of unlocked RWMutex"));

    if (sync_atomic_add_int32(&rw->reader_wait, -1) == 0)
        burrow__sema_release(&rw->writer_sem, false);
}

/* ------------------------------------------------------------------ writers */

void sync_rw_mutex_lock(SyncRWMutex *rw) {
    /* Against the other writers first, so that only one writer is ever in the
     * rest of this. */
    sync_mutex_lock(&rw->w);

    /* Tell the readers, and find out how many were already inside. */
    int32_t r = sync_atomic_add_int32(&rw->reader_count, -RWMUTEX_MAX_READERS) +
                RWMUTEX_MAX_READERS;

    /* Wait for those, if any are left by the time this add lands. A reader that
     * leaves in between takes reader_wait back to zero itself, which is why the
     * result of the add is what decides rather than r. */
    if (r != 0 && sync_atomic_add_int32(&rw->reader_wait, r) != 0)
        burrow__sema_acquire(&rw->writer_sem, false);
}

bool sync_rw_mutex_try_lock(SyncRWMutex *rw) {
    if (!sync_mutex_try_lock(&rw->w))
        return false;

    /* Only when there is not a single reader, since there is nothing to wait on
     * here and waiting is what this call promised not to do. */
    if (!sync_atomic_compare_and_swap_int32(&rw->reader_count, 0,
                                            -RWMUTEX_MAX_READERS)) {
        sync_mutex_unlock(&rw->w);
        return false;
    }
    return true;
}

void sync_rw_mutex_unlock(SyncRWMutex *rw) {
    /* Put the billion back, which makes the count positive again and reopens
     * the door. What comes back is the number of readers that queued while the
     * writer held it. */
    int32_t r = sync_atomic_add_int32(&rw->reader_count, RWMUTEX_MAX_READERS);
    if (r >= RWMUTEX_MAX_READERS)
        runtime_throw(BURROW_S("sync: unlock of unlocked RWMutex"));

    for (int32_t i = 0; i < r; i++)
        burrow__sema_release(&rw->reader_sem, false);

    /* Last, so that the readers released above are ahead of the next writer. */
    sync_mutex_unlock(&rw->w);
}

/* ---------------------------------------------------------------- as Lockers */

static void rw_locker_lock(void *self) {
    sync_rw_mutex_lock((SyncRWMutex *)self);
}

static void rw_locker_unlock(void *self) {
    sync_rw_mutex_unlock((SyncRWMutex *)self);
}

static void rw_r_locker_lock(void *self) {
    sync_rw_mutex_r_lock((SyncRWMutex *)self);
}

static void rw_r_locker_unlock(void *self) {
    sync_rw_mutex_r_unlock((SyncRWMutex *)self);
}

static const SyncLockerVT rw_locker_vt = {
    &rwmutex_desc,
    rw_locker_lock,
    rw_locker_unlock,
};

/* Two vtables over the same receiver, which is how one type ends up with two
 * different Lockers. Go writes the read side as a separate named type with the
 * same layout and does the same thing by a different route. */
static const SyncLockerVT rw_r_locker_vt = {
    &rwmutex_desc,
    rw_r_locker_lock,
    rw_r_locker_unlock,
};

SyncLocker sync_rw_mutex_locker(SyncRWMutex *rw) {
    return (SyncLocker){&rw_locker_vt, rw};
}

SyncLocker sync_rw_mutex_r_locker(SyncRWMutex *rw) {
    return (SyncLocker){&rw_r_locker_vt, rw};
}
