/* Go's sync, in C. The locks that a program written against this library
 * reaches for, as opposed to the one the runtime uses on itself.
 *
 *     static SyncMutex mu;
 *     static int balance;
 *
 *     void deposit(int n) {
 *         sync_mutex_lock(&mu);
 *         balance += n;
 *         sync_mutex_unlock(&mu);
 *     }
 *
 * The zero value is an unlocked mutex, so there is nothing to initialise and
 * nothing to destroy. A mutex in a static, in a struct you calloc'd, or in an
 * arena is ready the moment its memory is zero, which is Go's rule and is the
 * reason there is no sync_mutex_init here to forget to call.
 *
 * What makes these different from burrow/lock.h, which looks like the same
 * thing, is that these park. A goroutine waiting here gives its thread back to
 * the scheduler and costs nothing but its stack, so ten thousand goroutines
 * queued on one mutex are ten thousand parked goroutines and not ten thousand
 * threads. The runtime's own lock cannot do that, because parking a goroutine
 * is one of the things it is built out of.
 *
 * Threads that are not goroutines may use these too. burrow is a library inside
 * somebody else's program and that program's own threads are allowed to take a
 * lock; such a thread sleeps rather than parking, and costs itself. Go is never
 * in that situation and so has only the first half of this.
 *
 * Derived from Go's src/sync/rwmutex.go.
 * Go source: go1.27.1.
 *
 * Copyright 2009 The Go Authors. All rights reserved.
 * Copyright 2026 The burrow Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style licence that can be found
 * in the LICENSE file. */

#ifndef BURROW_SYNC_H
#define BURROW_SYNC_H

#include "burrow/own.h"
#include "burrow/sync/atomic.h"
#include "burrow/type.h"

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* --------------------------------------------------------------- sync.Mutex
 *
 * Two words, and both of them are the implementation's. Read them in a
 * debugger if you like, but do not write them and do not copy a Mutex once it
 * has been used: a copy has the same waiter counts as the original and neither
 * of them is right afterwards. Go says the same thing and has go vet to say it
 * at compile time. C has no vet, so it is said here. */
typedef struct SyncMutex {
    /* The lock bit, the woken bit, the starving bit and the waiter count, in
     * that order from the bottom. One word so that locking an uncontended mutex
     * is one compare and swap. */
    int32_t state;

    /* The counter the waiters queue on, from the runtime's semaphore. Zero
     * until somebody actually has to wait, and no queue exists anywhere for a
     * mutex nobody is waiting on. */
    uint32_t sema;
} SyncMutex;

/* The descriptor, so that a Mutex can be the receiver behind a SyncLocker and
 * so that an Any holding one can be asserted back. */
extern const Type *const TYPE_SYNC_MUTEX;

/* Two of the bits of state, named because the fast paths below are inline and
 * have to test them. The other two fields of the word belong to sync/mutex.c
 * and are not here, since nothing outside it ever looks at them. */
enum {
    /* Somebody holds the lock. */
    BURROW_MUTEX_LOCKED = 1,

    /* The queue is being served in order, so the lock is owed to whoever is at
     * the front of it and is not available to whoever happens to ask. */
    BURROW_MUTEX_STARVING = 4
};

/* The slow halves. These are the implementation and not the interface, and are
 * declared here only because the inline fast paths below have to be able to
 * call them. Call sync_mutex_lock and sync_mutex_unlock instead. */
void burrow__sync_mutex_lock_slow(SyncMutex *m);
void burrow__sync_mutex_unlock_slow(SyncMutex *m, int32_t next);

/* Takes the lock, waiting until it is free.
 *
 * A mutex is not reentrant. A goroutine that locks one it already holds waits
 * forever for itself, exactly as in Go, and the answer is to stop doing that
 * rather than to count. It is also not owned: any goroutine may unlock a mutex
 * any other goroutine locked, which is what makes a mutex usable as a signal
 * between two of them and is again Go's rule.
 *
 * The wait is fair enough to matter and not so fair that it is slow, which is
 * the whole design of Go's mutex and is ported here rather than simplified. A
 * waiter that has been queued for more than a millisecond switches the mutex
 * into starvation mode, and then unlocking hands the lock straight to the
 * goroutine at the front of the queue instead of letting whoever happens to be
 * running take it first. That is what stops a thread that locks and unlocks in
 * a tight loop from starving a queue forever. As soon as the queue is empty, or
 * a waiter gets the lock inside the threshold, the mutex goes back to the fast
 * mode where a lock is one compare and swap.
 *
 * Inline, and so are the two below it, for the same reason Go's are inlined by
 * its compiler. A lock nobody holds is one instruction, and wrapping one
 * instruction in a function call across a library boundary roughly doubles what
 * it costs. Everything past the first compare and swap is out of line, which is
 * the part that is worth a call. */
static inline void sync_mutex_lock(SyncMutex *m) {
    /* Zero is a mutex nobody holds, nobody wants and nobody is queued on, and
     * it is the state almost every lock in a running program finds. */
    if (sync_atomic_compare_and_swap_int32(&m->state, 0, BURROW_MUTEX_LOCKED))
        return;
    burrow__sync_mutex_lock_slow(m);
}

/* Takes the lock if it is free and answers whether it did. Never waits.
 *
 * Go's TryLock, along with Go's warning about it: a correct use of this is
 * rare, and code that loops on it is usually code that wanted Lock. It is here
 * because the cases it is right for, such as a status page that would rather
 * report "busy" than block, are real.
 *
 * Not a cheaper Lock, and measurably a dearer one. Lock goes straight at a
 * compare and swap against zero, while this has to read the state first to find
 * out whether the mutex is starving, since taking the lock out from under a
 * queue being served in order is the one thing it must not do. */
static inline bool sync_mutex_try_lock(SyncMutex *m) {
    int32_t old = sync_atomic_load_int32(&m->state);

    if ((old & (BURROW_MUTEX_LOCKED | BURROW_MUTEX_STARVING)) != 0)
        return false;

    /* There may be waiters queued. Taking the lock out from under them is
     * allowed here because this goroutine is already running and they are not,
     * which is the same bargain the ordinary fast path makes. */
    return sync_atomic_compare_and_swap_int32(&m->state, old,
                                              old | BURROW_MUTEX_LOCKED);
}

/* Gives the lock back.
 *
 * Unlocking a mutex that is not locked is a bug in the program and stops it,
 * the same as in Go. There is no way to make it survivable: the state that
 * would have to be repaired is the state that says who is allowed to be in the
 * critical section. */
static inline void sync_mutex_unlock(SyncMutex *m) {
    /* Clearing the lock bit of a mutex with nobody queued leaves zero, and zero
     * means there is nobody to wake and nothing else to do. */
    int32_t next = sync_atomic_add_int32(&m->state, -BURROW_MUTEX_LOCKED);
    if (next != 0)
        burrow__sync_mutex_unlock_slow(m, next);
}

/* ---------------------------------------------------------- sync.RWMutex
 *
 * A lock that either one writer or any number of readers may hold.
 *
 *     static SyncRWMutex mu;
 *
 *     Entry *lookup(Str k) {
 *         sync_rw_mutex_r_lock(&mu);
 *         Entry *e = table_get(k);
 *         sync_rw_mutex_r_unlock(&mu);
 *         return e;
 *     }
 *
 * The zero value is unlocked, and the same rules about copying apply.
 *
 * Read locks do not nest with writers waiting. A goroutine holding a read lock
 * that takes the same read lock a second time deadlocks if a writer arrived in
 * between, because the second read lock queues behind that writer and the
 * writer queues behind the first read lock. Go documents exactly this, and it
 * is not an implementation detail that could be fixed: the alternative is
 * letting readers starve writers indefinitely.
 *
 * Worth one sentence on when to use it. A read lock is a read modify write on a
 * shared counter, so two readers on two cores contend just as much as two
 * writers would. It wins when the critical section is long enough for the
 * parallelism to pay for that, and a plain Mutex wins when it is a pointer load
 * and a compare. Measure it rather than assuming, which is Go's advice too. */
typedef struct SyncRWMutex {
    /* Held by whichever writer is next, so that writers queue against each
     * other on the mutex and against readers on the counts below. */
    SyncMutex w;

    /* A writer waits here for the readers that were already inside. */
    uint32_t writer_sem;

    /* Readers wait here for the writer that is inside or pending. */
    uint32_t reader_sem;

    /* How many readers hold or want the lock. A writer subtracts a large
     * constant from it to make it negative, which is one atomic that both
     * announces the writer and tells every arriving reader to wait. */
    int32_t reader_count;

    /* How many of those readers the pending writer is still waiting for. The
     * last one to leave is the one that wakes the writer. */
    int32_t reader_wait;
} SyncRWMutex;

extern const Type *const TYPE_SYNC_RW_MUTEX;

/* The slow halves of the two reader calls, out of line for the same reason the
 * mutex's are. Not the interface. */
void burrow__sync_rw_mutex_r_lock_slow(SyncRWMutex *rw);
void burrow__sync_rw_mutex_r_unlock_slow(SyncRWMutex *rw, int32_t r);

/* Takes the lock for writing, waiting for the current readers and writers. */
void sync_rw_mutex_lock(SyncRWMutex *rw);

/* Takes the lock for writing if nothing holds or wants it. Never waits. */
bool sync_rw_mutex_try_lock(SyncRWMutex *rw);

/* Gives the write lock back, releasing every reader that queued behind it.
 *
 * Unlocking one that is not locked for writing stops the program, as in Go. */
void sync_rw_mutex_unlock(SyncRWMutex *rw);

/* Takes the lock for reading, waiting only if a writer holds or wants it.
 *
 * Inline, because with no writer in sight it is one atomic add and a branch on
 * the sign of the result. */
static inline void sync_rw_mutex_r_lock(SyncRWMutex *rw) {
    if (sync_atomic_add_int32(&rw->reader_count, 1) < 0)
        burrow__sync_rw_mutex_r_lock_slow(rw);
}

/* Takes the lock for reading if no writer holds or wants it. Never waits. */
bool sync_rw_mutex_try_r_lock(SyncRWMutex *rw);

/* Gives one read lock back. Does not unlock other readers. */
static inline void sync_rw_mutex_r_unlock(SyncRWMutex *rw) {
    int32_t r = sync_atomic_add_int32(&rw->reader_count, -1);
    if (r < 0)
        burrow__sync_rw_mutex_r_unlock_slow(rw, r);
}

/* -------------------------------------------------------------- sync.Locker
 *
 * Something with a lock and an unlock, which is what a function that wants to
 * hold a lock without caring which one takes.
 *
 * It is a vtable pointer and a data pointer, every vtable starts with
 * self_type, and a zeroed value is nil. Those rules are explained once in
 * burrow/iface.h and are not repeated here. */
typedef struct SyncLockerVT {
    const Type *self_type;
    void (*lock)(void *self);
    void (*unlock)(void *self);
} SyncLockerVT;

typedef struct SyncLocker {
    const SyncLockerVT *vt;
    void *data;
} SyncLocker;

/* A Mutex as a Locker.
 *
 * Go needs no such function, because a *Mutex satisfies Locker by having the
 * methods and the compiler does the rest. C has no structural typing, so the
 * conversion has to be written down, and this is where it is written. The
 * result borrows m and is valid for exactly as long as m is. */
BURROW_BORROWS(ret, m) SyncLocker sync_mutex_locker(SyncMutex *m);

/* An RWMutex's write lock as a Locker, for the same reason. */
BURROW_BORROWS(ret, rw) SyncLocker sync_rw_mutex_locker(SyncRWMutex *rw);

/* An RWMutex's read lock as a Locker.
 *
 * Go's RLocker. Locking the result takes a read lock and unlocking it gives
 * that read lock back, which is how a Cond can wait on the read side of an
 * RWMutex. */
BURROW_BORROWS(ret, rw) SyncLocker sync_rw_mutex_r_locker(SyncRWMutex *rw);

/* Takes the lock, whichever lock it is. Calling either of these on a nil Locker
 * stops the program, the same as calling a method on a nil interface in Go. */
void sync_locker_lock(SyncLocker l);
void sync_locker_unlock(SyncLocker l);

#ifdef __cplusplus
}
#endif

#endif /* BURROW_SYNC_H */
