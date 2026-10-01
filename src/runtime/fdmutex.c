/* fdMutex. See burrow/fdmutex.h.
 *
 * Derived from Go's src/internal/poll/fd_mutex.go.
 * Go source: go1.27.1.
 *
 * Copyright 2013 The Go Authors. All rights reserved.
 * Copyright 2026 The burrow Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style licence that can be found
 * in the LICENSE file. */

#include "burrow/fdmutex.h"

#include "burrow/atomic.h"
#include "burrow/core.h"
#include "burrow/panic.h"
#include "burrow/sema.h"

#define FDMU_CLOSED ((uint64_t)1 << 0)
#define FDMU_RLOCK ((uint64_t)1 << 1)
#define FDMU_WLOCK ((uint64_t)1 << 2)
#define FDMU_REF ((uint64_t)1 << 3)
#define FDMU_REF_MASK ((((uint64_t)1 << 20) - 1) << 3)
#define FDMU_RWAIT ((uint64_t)1 << 23)
#define FDMU_RMASK ((((uint64_t)1 << 20) - 1) << 23)
#define FDMU_WWAIT ((uint64_t)1 << 43)
#define FDMU_WMASK ((((uint64_t)1 << 20) - 1) << 43)

BURROW_NORETURN static void fdmu_overflow(void) {
    panic_str(BURROW_S(
        "too many concurrent operations on a single file or socket (max 1048575)"));
}

BURROW_NORETURN static void fdmu_inconsistent(void) {
    panic_str(BURROW_S("inconsistent poll.fdMutex"));
}

bool burrow__fdmu_incref(burrow__FdMutex *mu) {
    uint64_t old = burrow__atomic_load_u64(&mu->state);
    for (;;) {
        if ((old & FDMU_CLOSED) != 0)
            return false;
        uint64_t next = old + FDMU_REF;
        if ((next & FDMU_REF_MASK) == 0)
            fdmu_overflow();
        if (burrow__atomic_cas_u64(&mu->state, &old, next))
            return true;
    }
}

bool burrow__fdmu_incref_and_close(burrow__FdMutex *mu) {
    uint64_t old = burrow__atomic_load_u64(&mu->state);
    for (;;) {
        if ((old & FDMU_CLOSED) != 0)
            return false;
        uint64_t next = (old | FDMU_CLOSED) + FDMU_REF;
        if ((next & FDMU_REF_MASK) == 0)
            fdmu_overflow();
        /* Nobody waits any more. They are woken below and see the close. */
        next &= ~(FDMU_RMASK | FDMU_WMASK);
        if (burrow__atomic_cas_u64(&mu->state, &old, next)) {
            for (; (old & FDMU_RMASK) != 0; old -= FDMU_RWAIT)
                burrow__sema_release(&mu->rsema, false);
            for (; (old & FDMU_WMASK) != 0; old -= FDMU_WWAIT)
                burrow__sema_release(&mu->wsema, false);
            return true;
        }
    }
}

bool burrow__fdmu_decref(burrow__FdMutex *mu) {
    uint64_t old = burrow__atomic_load_u64(&mu->state);
    for (;;) {
        if ((old & FDMU_REF_MASK) == 0)
            fdmu_inconsistent();
        uint64_t next = old - FDMU_REF;
        if (burrow__atomic_cas_u64(&mu->state, &old, next))
            return (next & (FDMU_CLOSED | FDMU_REF_MASK)) == FDMU_CLOSED;
    }
}

bool burrow__fdmu_rwlock(burrow__FdMutex *mu, bool read, bool wait) {
    uint64_t bit = read ? FDMU_RLOCK : FDMU_WLOCK;
    uint64_t one_waiter = read ? FDMU_RWAIT : FDMU_WWAIT;
    uint64_t waiters = read ? FDMU_RMASK : FDMU_WMASK;
    uint32_t *sema = read ? &mu->rsema : &mu->wsema;
    for (;;) {
        uint64_t old = burrow__atomic_load_u64(&mu->state);
        if ((old & FDMU_CLOSED) != 0)
            return false;
        uint64_t next;
        if ((old & bit) == 0) {
            next = (old | bit) + FDMU_REF;
            if ((next & FDMU_REF_MASK) == 0)
                fdmu_overflow();
        } else {
            if (!wait)
                return false;
            next = old + one_waiter;
            if ((next & waiters) == 0)
                fdmu_overflow();
        }
        if (burrow__atomic_cas_u64(&mu->state, &old, next)) {
            if ((old & bit) == 0)
                return true;
            /* Whoever wakes us has taken us off the count already. */
            burrow__sema_acquire(sema, false, false);
        }
    }
}

bool burrow__fdmu_rwunlock(burrow__FdMutex *mu, bool read) {
    uint64_t bit = read ? FDMU_RLOCK : FDMU_WLOCK;
    uint64_t one_waiter = read ? FDMU_RWAIT : FDMU_WWAIT;
    uint64_t waiters = read ? FDMU_RMASK : FDMU_WMASK;
    uint32_t *sema = read ? &mu->rsema : &mu->wsema;
    uint64_t old = burrow__atomic_load_u64(&mu->state);
    for (;;) {
        if ((old & bit) == 0 || (old & FDMU_REF_MASK) == 0)
            fdmu_inconsistent();
        uint64_t next = (old & ~bit) - FDMU_REF;
        if ((old & waiters) != 0)
            next -= one_waiter;
        if (burrow__atomic_cas_u64(&mu->state, &old, next)) {
            if ((old & waiters) != 0)
                burrow__sema_release(sema, false);
            return (next & (FDMU_CLOSED | FDMU_REF_MASK)) == FDMU_CLOSED;
        }
    }
}

bool burrow__fdmu_closing(const burrow__FdMutex *mu) {
    return (burrow__atomic_load_u64(&mu->state) & FDMU_CLOSED) != 0;
}
