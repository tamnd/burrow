/* The runtime's own lock, which is not what a program means by one.
 *
 * A sync.Mutex parks the goroutine and hands the thread to somebody else. This
 * cannot do that, because it is one of the things parking a goroutine is built
 * out of, and a lock that needs a scheduler cannot be the lock the scheduler
 * takes. So it blocks the thread, and the rule that makes that acceptable is
 * that every critical section under it is a handful of pointer writes with no
 * call out of the runtime inside it.
 *
 * Spin and then yield, for now. The spin is what makes the uncontended and
 * lightly contended cases cost nothing, and the yield is what stops a thread
 * burning a core waiting for a lock whose holder has been descheduled. The
 * futex version, which is a spin and then a note, arrives with sync, since that
 * is where the rest of the same machinery is going. Swapping it in changes one
 * file and nothing above it.
 *
 * This is Go's runtime mutex from runtime/lock_futex.go, and it is in a header
 * of its own because both the scheduler and the timers need it and the timers
 * do not otherwise need the scheduler.
 *
 * All zeroes is unlocked, which is the same rule as everywhere else in burrow
 * and means a lock in a static or a calloc'd struct is ready to use.
 *
 * Copyright 2026 The burrow Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style licence that can be found
 * in the LICENSE file. */

#ifndef BURROW_LOCK_H
#define BURROW_LOCK_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct burrow__Lock {
    uint32_t state;
} burrow__Lock;

/* Takes the lock, blocking the thread until it has it. Not recursive: a thread
 * that takes a lock it is already holding hangs, and the fix is to stop doing
 * that rather than to count. */
void burrow__lock(burrow__Lock *l);

/* Takes the lock if it is free. Answers whether it did. */
bool burrow__trylock(burrow__Lock *l);

/* Gives it back. Only the thread that took it may do this. */
void burrow__unlock(burrow__Lock *l);

#ifdef __cplusplus
}
#endif

#endif /* BURROW_LOCK_H */
