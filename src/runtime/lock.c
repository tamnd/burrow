/* The runtime's own lock. See burrow/lock.h for what it is and what it is not.
 *
 * Copyright 2026 The burrow Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style licence that can be found
 * in the LICENSE file. */

#include "burrow/lock.h"

#include "burrow/atomic.h"
#include "burrow/thread.h"

#include <stdbool.h>
#include <stdint.h>

/* How many times burrow__lock spins before it stops burning the processor.
 *
 * Every critical section under this lock is a handful of pointer writes, so the
 * holder is almost always gone within a few tens of cycles and spinning wins.
 * When it does not, the holder has usually been descheduled by the operating
 * system, and then no amount of spinning helps and yielding is the only thing
 * that does. */
#define LOCK_SPINS 60

void burrow__lock(burrow__Lock *l) {
    for (;;) {
        for (int i = 0; i < LOCK_SPINS; i++) {
            /* Read before trying. A compare and swap on a lock somebody else is
             * holding takes the cache line exclusively and takes it away from
             * the holder, which makes the holder slower and therefore makes the
             * wait longer. A plain load leaves the line shared. */
            if (burrow__atomic_load_relaxed_u32(&l->state) == 0) {
                uint32_t free = 0;
                if (burrow__atomic_cas_acquire_u32(&l->state, &free, 1))
                    return;
            }
            burrow__atomic_spin_hint();
        }
        burrow__thread_yield();
    }
}

bool burrow__trylock(burrow__Lock *l) {
    uint32_t free = 0;
    return burrow__atomic_cas_acquire_u32(&l->state, &free, 1);
}

void burrow__unlock(burrow__Lock *l) {
    burrow__atomic_store_release_u32(&l->state, 0);
}
