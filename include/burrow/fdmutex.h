/* The lock and reference count that keep a descriptor alive while it is in use.
 *
 * A Read racing a Close is the problem. Without this, Close closes the
 * descriptor, another goroutine opens a file and the kernel hands it the same
 * number, and the Read that was about to start reads somebody else's file. Go
 * solves it in internal/poll with fdMutex, and this is that, word for word: one
 * 64 bit state word and two semaphores.
 *
 *     bit 0       closed, after which every lock fails
 *     bit 1       the read lock
 *     bit 2       the write lock
 *     bits 3-22   references, from reads, writes and everything else
 *     bits 23-42  goroutines waiting for the read lock
 *     bits 43-62  goroutines waiting for the write lock
 *
 * A read takes rwlock(true) and gives it back with rwunlock(true), a write does
 * the same with false, and anything else that uses the descriptor without
 * reading or writing, a stat or a chmod, takes incref and gives decref. Close
 * calls incref_and_close and then decref. Whichever of those calls drops the
 * last reference after the close returns true, and that caller is the one that
 * really closes the descriptor. So the number is never closed under somebody
 * still using it, and it is always closed by somebody.
 *
 * Reads are serialised against reads and writes against writes, because two
 * reads at once on one file would see the offset move under them. A read and a
 * write can run together.
 *
 * Going past 1,048,575 of anything panics with Go's message, and unlocking what
 * is not locked panics with "inconsistent poll.fdMutex". Both are bugs in the
 * caller, but Go panics rather than throwing, so a recover catches them.
 *
 * Internal. os and net use it, and nothing outside burrow should.
 *
 * Derived from Go's src/internal/poll/fd_mutex.go.
 * Go source: go1.27.1.
 *
 * Copyright 2013 The Go Authors. All rights reserved.
 * Copyright 2026 The burrow Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style licence that can be found
 * in the LICENSE file. */

#ifndef BURROW_FDMUTEX_H
#define BURROW_FDMUTEX_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* All zero is open, unlocked and unreferenced. */
typedef struct burrow__FdMutex {
    uint64_t state;
    uint32_t rsema;
    uint32_t wsema;
} burrow__FdMutex;

/* Adds a reference. False when the descriptor is closed. */
bool burrow__fdmu_incref(burrow__FdMutex *mu);

/* Marks it closed and adds a reference, waking everybody waiting for a lock so
 * they see the close and fail. False when it was closed already. */
bool burrow__fdmu_incref_and_close(burrow__FdMutex *mu);

/* Drops a reference. True when it was the last one after a close, which means
 * the caller is the one that has to close the descriptor. */
bool burrow__fdmu_decref(burrow__FdMutex *mu);

/* Takes the read lock when read is true and the write lock when it is false,
 * with a reference. With wait false it gives up rather than waiting for a lock
 * somebody else has. False when it is closed or, without wait, taken. */
bool burrow__fdmu_rwlock(burrow__FdMutex *mu, bool read, bool wait);

/* Gives back the lock and the reference rwlock took, and wakes one waiter.
 * True as decref says. */
bool burrow__fdmu_rwunlock(burrow__FdMutex *mu, bool read);

/* Whether incref_and_close has happened. */
bool burrow__fdmu_closing(const burrow__FdMutex *mu);

#ifdef __cplusplus
}
#endif

#endif /* BURROW_FDMUTEX_H */
