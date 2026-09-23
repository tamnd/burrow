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

/* burrow:package sync */

#ifndef BURROW_SYNC_H
#define BURROW_SYNC_H

#include "burrow/core.h"
#include "burrow/func.h"
#include "burrow/iface.h"
#include "burrow/mem.h"
#include "burrow/own.h"
#include "burrow/panic.h"
#include "burrow/sema.h"
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

/* ----------------------------------------------------------- sync.WaitGroup
 *
 * A counter of things still to finish, and a way to wait for it to reach zero.
 *
 *     static SyncWaitGroup wg;
 *
 *     for (int i = 0; i < 4; i++)
 *         sync_wait_group_go(&wg, BURROW_FN(Func, worker, &jobs[i]));
 *     sync_wait_group_wait(&wg);
 *
 * The zero value is a group with nothing in it, so there is nothing to
 * initialise, and the same rule about not copying applies. */
typedef struct SyncWaitGroup {
    /* The counter in the high 32 bits and the waiter count in the low ones, so
     * that adding to the counter and reading how many are waiting is one
     * atomic. Bit 31, the top of the low half, says this group belongs to a
     * synctest bubble, which is why the waiter count is read with that bit
     * masked out. */
    uint64_t state;

    /* What the waiters queue on. Zero until somebody actually waits. */
    uint32_t sema;

    /* Which bubble, when the bit above is set, and NULL otherwise. Only ever
     * compared, never followed, and set and cleared by Add.
     *
     * Go keeps this in a table in the runtime keyed on the address of the group,
     * because the size of a sync.WaitGroup is something programs depend on.
     * Nothing depends on the size of this one yet, and a word here is a
     * comparison where a table would be a hash and a lock on every Add made
     * inside a bubble. */
    BURROW_BORROWS(1) void *bubble;
} SyncWaitGroup;

extern const Type *const TYPE_SYNC_WAIT_GROUP;

/* Adds delta, which may be negative, to the counter. Releases everybody in Wait
 * when the counter reaches zero, and stops the program if it goes below zero.
 *
 * The ordering rule is Go's and it is the part people get wrong. A call that
 * takes the counter up from zero has to happen before the Wait it is meant to
 * hold, which in practice means adding before starting the goroutine rather
 * than inside it. Adding while the counter is already above zero, or
 * subtracting, may happen whenever. Reusing a group for a second round of work
 * means waiting for the first round to finish first.
 *
 * Prefer sync_wait_group_go, which cannot get any of that wrong. */
void sync_wait_group_add(SyncWaitGroup *wg, Int delta);

/* Takes one off the counter. The same as adding minus one, and the name worth
 * using because it is what the call site means. */
void sync_wait_group_done(SyncWaitGroup *wg);

/* Waits until the counter is zero. */
void sync_wait_group_wait(SyncWaitGroup *wg);

/* Adds one, starts f in a new goroutine, and takes one off when f returns.
 *
 * Go's WaitGroup.Go, and the call to reach for, since the counter and the
 * goroutine cannot get out of step with each other. Answers false when the
 * goroutine could not be started, and takes the one back off the counter first,
 * so a group that fails to start anything is a group with nothing in it.
 *
 * f must not panic. A panic that nobody recovers inside a goroutine ends the
 * program, and it ends it without taking one off the counter, so a Wait
 * somewhere else cannot return and race the shutdown. That is Go's behaviour
 * and it is deliberate. */
bool sync_wait_group_go(SyncWaitGroup *wg, Func f);

/* ---------------------------------------------------------------- sync.Once
 *
 * Runs one function one time, however many callers ask and however many of them
 * ask at once.
 *
 *     static SyncOnce started;
 *
 *     void ensure_started(void) {
 *         sync_once_do(&started, BURROW_FN(Func, start, NULL));
 *     }
 *
 * The zero value has not run yet, so again there is nothing to initialise.
 *
 * What it promises is stronger than "f is called once", and the difference is
 * the reason this is not just a compare and swap. When any call returns, f has
 * finished. The caller that lost the race waits for the caller that won rather
 * than carrying on past a half built thing. */
typedef struct SyncOnce {
    /* First, because it is the field the fast path reads and the only one an
     * already initialised Once ever touches. */
    SyncAtomicBool done;

    SyncMutex m;
} SyncOnce;

extern const Type *const TYPE_SYNC_ONCE;

void burrow__sync_once_do_slow(SyncOnce *o, Func f);

/* Runs f if no call on this Once has run one yet, and otherwise waits for the
 * call that is running one.
 *
 * A different f on a later call is not run. One Once means one action, and a
 * second action wants a second Once.
 *
 * Calling this from inside f, on the same Once, deadlocks. So does calling it
 * on a Once whose f is waiting for this goroutine.
 *
 * If f panics, the Once counts as done and later calls return without running
 * anything. That is Go's rule: f had its turn. */
static inline void sync_once_do(SyncOnce *o, Func f) {
    if (!sync_atomic_bool_load(&o->done))
        burrow__sync_once_do_slow(o, f);
}

/* ------------------------------------------------------------ sync.OnceFunc
 *
 * Go returns a closure from OnceFunc and lets the collector clean it up. C has
 * no closures and no collector, so the state is a struct you declare and the
 * function is a call on it. Nothing is allocated and there is nothing to free.
 *
 *     static SyncOnceFunc setup = SYNC_ONCE_FUNC(BURROW_FN(Func, load, NULL));
 *
 *     sync_once_func_call(&setup);
 *
 * The difference from a bare Once is what happens to a panic. A Once lets the
 * panic out once and then reports the action as done, so every later caller
 * carries on as if the thing had been built. These remember the panic and raise
 * it again on every later call, so nobody gets a half built thing quietly. That
 * is Go's behaviour for all three of these. */
typedef struct SyncOnceFunc {
    SyncOnce once;

    /* Cleared once it has run, so that whatever the environment pointer keeps
     * alive is not kept alive by this for the rest of the program. */
    Func f;

    /* Whether f returned rather than panicked. */
    bool ok;

    /* What it panicked with, when it did, and room to keep it.
     *
     * The value a catch block is holding lives in the frame that caught it, so
     * it has to be copied somewhere that outlives that frame before it can be
     * raised again later. Thirty two bytes covers every builtin and most small
     * structs, which is the same bargain and the same number burrow/panic.h
     * makes for a catch block. A panic value bigger than that keeps pointing
     * where it pointed and has to outlive this struct, which in practice means
     * do not panic with a large value you built on the stack. */
    Any p;
    burrow__PanicValue storage;
} SyncOnceFunc;

/* The initialiser, since the function to run has to be known before the first
 * call and a zero value cannot hold it. Works at file scope and in a block. */
#define SYNC_ONCE_FUNC(fn) ((SyncOnceFunc){.f = (fn)})

/* Runs f once, and re-raises its panic on this and every later call if it
 * panicked. */
void sync_once_func_call(SyncOnceFunc *of);

/* The same thing as a Func, for handing to something that takes one. Borrows
 * of and is valid for exactly as long as it is. */
BURROW_BORROWS(ret, of) Func sync_once_func_fn(SyncOnceFunc *of);

/* ----------------------------------------------------------- sync.OnceValue
 *
 * The same, for a function that produces a value.
 *
 *     static Config cfg;
 *
 *     static Any load(void *env) {
 *         (void)env;
 *         cfg = read_config();
 *         return BURROW_ANY(TYPE_CONFIG, &cfg);
 *     }
 *
 *     static SyncOnceValue config = SYNC_ONCE_VALUE(BURROW_FN(AnyFunc, load, NULL));
 *
 *     Config *c = any_assert(sync_once_value_get(&config), TYPE_CONFIG);
 *
 * Go's OnceValue is generic in the result type. This one is an Any, for the
 * reason every other place in the library that holds one value of any type is
 * an Any: C has no type parameters, the descriptor is what carries the type,
 * and an assertion on the way out is what the compiler would otherwise have
 * checked. An Any points rather than holds, so what f returns has to outlive
 * the SyncOnceValue, which is the ordinary rule for Any and is usually free
 * here since the thing being computed once is usually a static. */
typedef struct SyncOnceValue {
    SyncOnce once;
    AnyFunc f;
    bool ok;
    Any p;
    burrow__PanicValue storage;
    Any result;
} SyncOnceValue;

#define SYNC_ONCE_VALUE(fn) ((SyncOnceValue){.f = (fn)})

/* Runs f once and gives back what it returned, on this and every later call.
 * Re-raises f's panic instead if it panicked.
 *
 * The Any is the one f produced, kept in ov, so it is borrowed twice over: it
 * lives as long as ov does, and what it points at has to outlive both. */
BURROW_BORROWS(ret, ov) Any sync_once_value_get(SyncOnceValue *ov);

/* ---------------------------------------------------------- sync.OnceValues
 *
 * Two results rather than one, because Go's second result is usually an error
 * and dropping it would make this useless for the case it exists for.
 *
 * The function writes both through pointers rather than returning a pair, which
 * is how the rest of the library spells two results. */
typedef struct SyncOnceValues SyncOnceValues;

/* What such a function looks like. Both out pointers are non-NULL and both are
 * zeroed before the call. */
BURROW_FUNC(SyncOnceValuesFn, void, Any *a, Any *b);

struct SyncOnceValues {
    SyncOnce once;
    SyncOnceValuesFn f;
    bool ok;
    Any p;
    burrow__PanicValue storage;
    Any first;
    Any second;
};

#define SYNC_ONCE_VALUES(fn) ((SyncOnceValues){.f = (fn)})

/* Runs f once and writes both of its results through a and b, on this and every
 * later call. Either pointer may be NULL to ignore that result. Re-raises f's
 * panic instead if it panicked. */
void sync_once_values_get(SyncOnceValues *ov, Any *a, Any *b);

/* ---------------------------------------------------------------- sync.Cond
 *
 * A place for goroutines to wait until something they care about changes.
 *
 * A Cond is a queue attached to a lock. The lock is yours and it is the one
 * that guards whatever the condition is about. Waiting drops the lock, sleeps,
 * and takes the lock again before returning, so a waiter always comes back
 * holding what it was holding when it went to sleep.
 *
 *     static SyncMutex mu;
 *     static SyncCond ready;
 *     static bool has_work;
 *
 *     void consume(void) {
 *         sync_mutex_lock(&mu);
 *         while (!has_work)
 *             sync_cond_wait(&ready);
 *         take_the_work();
 *         sync_mutex_unlock(&mu);
 *     }
 *
 *     void produce(void) {
 *         sync_mutex_lock(&mu);
 *         has_work = true;
 *         sync_cond_signal(&ready);
 *         sync_mutex_unlock(&mu);
 *     }
 *
 * The `while` is not a style preference. A Cond makes exactly one promise, that
 * a waiter which was waiting when the signal went out will wake up, and it
 * promises nothing about what is true when it does. Somebody else may have
 * taken the work in between. Go says the same thing, and a Cond used with an
 * `if` is the most common way to misuse one.
 *
 * Unlike everything else in this file, the zero value is not ready to use: a
 * Cond has to know which lock it belongs to. That is what the initialiser
 * below is for, and it is Go's NewCond by another spelling.
 *
 * Go's Cond has a note saying most uses are better served by a channel, and it
 * is right. A Cond is for the case where the thing being waited on is a
 * condition over shared state rather than a value being handed over. If what
 * you have is a value being handed over, use a channel.
 *
 * Derived from Go's src/sync/cond.go. */
typedef struct SyncCond {
    /* The lock held while the condition is observed or changed. Go's `L`. Set
     * it once, before the first wait, and do not change it afterwards. */
    SyncLocker l;

    /* The queue. The implementation's, and not a thing to read or write. */
    burrow__NotifyList notify;

    /* Where this Cond was the first time it was used, so that a copy of one can
     * be caught. Also the implementation's. */
    Uintptr checker;
} SyncCond;

/* The initialiser. Works at file scope and in a block.
 *
 *     SyncCond ready = SYNC_COND(sync_mutex_locker(&mu));
 *
 * A Cond in a static cannot be written this way, because sync_mutex_locker is
 * a call and a static initialiser is a constant. Assign the whole struct at
 * start up instead:
 *
 *     static SyncCond ready;
 *     ...
 *     ready = SYNC_COND(sync_mutex_locker(&mu));
 *
 * which is fine for as long as it happens before the first wait. */
#define SYNC_COND(locker) ((SyncCond){.l = (locker)})

/* The descriptor, so that an Any holding a Cond can be asserted back. */
extern const Type *const TYPE_SYNC_COND;

/* Waits for a signal or a broadcast.
 *
 * The lock must be held on the way in. It is dropped for the duration of the
 * wait and taken again before this returns, so a wakeup is not a place where
 * the caller loses the lock.
 *
 * It returns when it has been woken, which is not the same as the condition
 * being true. Call it in a loop that re-tests the condition. */
void sync_cond_wait(SyncCond *c);

/* Wakes one waiter, if there is one. Waking nobody is not an error, and is what
 * happens when the signal arrives before anybody is waiting, which is why the
 * condition has to be re-tested rather than trusted.
 *
 * The lock does not have to be held. Holding it is usually clearer, because
 * then the change and the signal cannot be seen out of order by a reader. */
void sync_cond_signal(SyncCond *c);

/* Wakes every waiter that is waiting now, and none that arrive afterwards. The
 * lock does not have to be held here either. */
void sync_cond_broadcast(SyncCond *c);

/* ----------------------------------------------------------------- sync.Map
 *
 * A map many goroutines can read and write at once without a lock around it.
 *
 *     static SyncMap cache;
 *     cache = SYNC_MAP(heap_allocator(), TYPE_STRING, TYPE_INT);
 *
 *     Int n = 1;
 *     Str k = BURROW_S("hits");
 *     sync_map_store(&cache, &k, &n);
 *
 *     Int got;
 *     if (sync_map_load(&cache, &k, &got))
 *         use(got);
 *
 * The first question to ask is whether you want one. A plain Map behind a
 * SyncMutex is simpler, it is faster for most workloads, and it is what Go's
 * own documentation tells you to reach for first. This is for the two cases
 * where it wins, and they are the two Go names: a cache that is written once
 * per key and read many times, and a map that many goroutines touch but with
 * little overlap in the keys they touch. In both, the point is that readers do
 * not write to a shared cache line and so do not fight each other.
 *
 * Underneath is Go's own implementation, a hash trie of sixteen way nodes taken
 * four hash bits at a time. A read follows pointers and takes no lock at all. A
 * write locks one node, which is the node holding the slot being changed, so
 * two writes to different parts of the map do not meet.
 *
 * Keys and values go in and come out by pointer, for the same reason they do in
 * burrow/map.h: the map holds values of a type it learns at runtime. The macros
 * at the end put the static typing back.
 *
 * Unlike the rest of this file the zero value is not ready to use, because a
 * map has to be told its key type, its value type and where its memory comes
 * from. Go gets all three from the type system and burrow has to be handed
 * them. Use SYNC_MAP below.
 *
 * Two differences from Go that are worth knowing before you start.
 *
 * The first is that a range callback must not block. The reclamation this map
 * stands on is tied to the thread, so a goroutine that parks in the middle of a
 * read is a bug the runtime will report. Read below at sync_map_range.
 *
 * The second is that everything which can allocate says so. Go's Store cannot
 * fail because Go stops the world when it runs out of memory; a C library has
 * to hand that decision back, so the allocating calls return whether they could
 * do it and give you Go's result through a pointer.
 *
 * Derived from Go's src/internal/sync/hashtriemap.go and src/sync/map.go. */
typedef struct SyncMap {
    /* Where the nodes come from, and what is in them. Set by SYNC_MAP and not
     * changed afterwards. */
    Alloc *a;
    const Type *key;
    const Type *val;

    /* The trie, the hash seed, and where the key and the value sit inside a
     * node. The implementation's, all of it. */
    void *root;
    uint64_t seed;
    uint32_t key_off;
    uint32_t val_off;
    uint32_t node_size;
    uint32_t node_align;
    SyncMutex init_mu;
    SyncAtomicUint32 inited;
} SyncMap;

/* The initialiser. Works in a block and at file scope, and a map in a static
 * has to be assigned at start up the same way a Cond does, because
 * heap_allocator is a call.
 *
 *     SyncMap cache = SYNC_MAP(heap_allocator(), TYPE_STRING, TYPE_INT);
 *
 * Nothing is allocated here. The first store builds the trie, so a map that is
 * declared and never written costs its own struct and no more.
 *
 * Stops the program if the key type is not comparable, at the first store
 * rather than here, since a macro cannot check anything. Same rule as
 * map_make: a slice, a map and a function cannot be keys. */
#define SYNC_MAP(alloc, key_type, val_type)                                            \
    ((SyncMap){.a = (alloc), .key = (key_type), .val = (val_type)})

/* The descriptor, so that an Any holding a Map can be asserted back. */
extern const Type *const TYPE_SYNC_MAP;

/* m.Load(key). Copies the value out and answers whether the key was there.
 *
 * out_val may be NULL to ask only whether the key is present. When the key is
 * absent nothing is written, which differs from map_get2 and is deliberate:
 * this map is read concurrently and a caller that ignores the answer and reads
 * the buffer anyway should get its own uninitialised value rather than a zero
 * that looks like a stored one.
 *
 * Takes no lock and allocates nothing. */
bool sync_map_load(SyncMap *m, const void *key, void *out_val);

/* m.Store(key, value). Copies both in, replacing whatever was there.
 *
 * val may be NULL, which stores the value type's zero value.
 *
 * Returns false when a node was needed and the allocator said no, in which case
 * the map is unchanged. */
bool sync_map_store(SyncMap *m, const void *key, const void *val);

/* m.Swap(key, value). Stores, and hands back what was there before.
 *
 * out_prev gets the old value and out_loaded gets whether there was one. Either
 * may be NULL. Nothing is written to out_prev when the key was absent.
 *
 * Returns false on allocation failure, with the map unchanged, which is why
 * Go's `loaded` comes out through a pointer rather than as the result. */
bool sync_map_swap(SyncMap *m, const void *key, const void *val, void *out_prev,
                   bool *out_loaded);

/* m.LoadOrStore(key, value). Stores only if the key is absent.
 *
 * out_actual gets the value that is in the map afterwards, which is the one
 * that was already there or the one just stored, and out_loaded gets which of
 * those happened. Either may be NULL.
 *
 * Returns false on allocation failure, with the map unchanged. A key that was
 * already present never allocates, so the read mostly path through this cannot
 * fail. */
bool sync_map_load_or_store(SyncMap *m, const void *key, const void *val,
                            void *out_actual, bool *out_loaded);

/* m.CompareAndSwap(key, old, new). Replaces the value only if what is there
 * equals old.
 *
 * out_swapped gets whether it happened, and may be NULL. Returns false on
 * allocation failure, with the map unchanged.
 *
 * Stops the program if the value type is not comparable, which is Go's panic
 * for the same call. */
bool sync_map_compare_and_swap(SyncMap *m, const void *key, const void *old,
                               const void *val, bool *out_swapped);

/* m.LoadAndDelete(key). Removes the key and copies out what it held.
 *
 * out_val may be NULL. Answers whether the key was there. Allocates nothing,
 * so there is nothing to fail. */
bool sync_map_load_and_delete(SyncMap *m, const void *key, void *out_val);

/* m.Delete(key). Does nothing if the key is not there. */
void sync_map_delete(SyncMap *m, const void *key);

/* m.CompareAndDelete(key, old). Removes the key only if what it holds equals
 * old, and answers whether it happened.
 *
 * Stops the program if the value type is not comparable. */
bool sync_map_compare_and_delete(SyncMap *m, const void *key, const void *old);

/* m.Clear(). Empties the map and hands every node back to the allocator once no
 * reader can still be inside one.
 *
 * Returns false if the one node it needs could not be allocated, in which case
 * the map is untouched. Go's Clear cannot fail for the usual reason. */
bool sync_map_clear(SyncMap *m);

/* What sync_map_range calls, once per entry. Return false to stop the walk,
 * which is Go's `yield` returning false. key and value point into the map and
 * are only valid until you return, so copy anything you want to keep. */
typedef bool (*SyncMapRangeFunc)(const void *key, const void *val, void *arg);

/* m.Range(f). Calls f for every key and value, in no particular order.
 *
 * There is no snapshot. No key is visited more than once, but a key stored or
 * deleted while the walk is running may or may not be visited, and a value
 * changed during the walk may be seen before or after the change. Go says the
 * same thing about its Range and means it the same way.
 *
 * f may call back into the same map, including to store and delete.
 *
 * f must not block. Not on a channel, not on a mutex, not on anything that can
 * park the goroutine. A walk holds a reclamation pin for its whole length, a
 * pin belongs to the thread rather than to the goroutine, and a goroutine that
 * parks inside one is a fatal error the runtime reports rather than a quiet
 * wrong answer. If what you want to do per entry can block, copy the keys out
 * in f and do the work after the walk has finished. This restriction is the one
 * place this map is not Go's, and it is there because Go's version of this
 * problem is solved by the garbage collector. */
void sync_map_range(SyncMap *m, SyncMapRangeFunc f, void *arg);

/* Hands every node back to the allocator, and leaves the map empty and still
 * usable, the way a freshly initialised one is.
 *
 * Go has no such thing, because Go has a collector. This is here for the same
 * reason map_free is: the nodes are the map's and nothing outside can name
 * them, so nobody else can give them back.
 *
 * Unlike every other call here this one is not safe to make concurrently with
 * the others. Nothing may be reading or writing the map while it runs. A map in
 * an arena can ignore it, since arena_free covers everything. */
void sync_map_free(SyncMap *m);

/* Typed access, for the call sites that know the types. Same shape as the map.h
 * macros: types first, then the map, then the values. */
#define BURROW_SYNC_MAP_STORE(KT, VT, m, k, v)                                         \
    sync_map_store((m), (const KT[]){(k)}, (const VT[]){(v)})

#define BURROW_SYNC_MAP_LOAD(KT, m, k, out_val)                                        \
    sync_map_load((m), (const KT[]){(k)}, (out_val))

#define BURROW_SYNC_MAP_HAS(KT, m, k) sync_map_load((m), (const KT[]){(k)}, NULL)

#define BURROW_SYNC_MAP_DELETE(KT, m, k) sync_map_delete((m), (const KT[]){(k)})

#if defined(BURROW_SHORT) && BURROW_SHORT
#define SYNC_MAP_STORE(KT, VT, m, k, v) BURROW_SYNC_MAP_STORE(KT, VT, m, k, v)
#define SYNC_MAP_LOAD(KT, m, k, out_val) BURROW_SYNC_MAP_LOAD(KT, m, k, out_val)
#define SYNC_MAP_HAS(KT, m, k) BURROW_SYNC_MAP_HAS(KT, m, k)
#define SYNC_MAP_DELETE(KT, m, k) BURROW_SYNC_MAP_DELETE(KT, m, k)
#endif

/* ---------------------------------------------------------------- sync.Pool
 *
 * A set of spare objects that goroutines hand back when they are done with one
 * and take again when they need one, without going to the allocator and without
 * a lock.
 *
 *     static Any make_buf(void *env) {
 *         (void)env;
 *         Buf *b = BURROW_NEW(heap_allocator(), Buf);
 *         return BURROW_ANY(TYPE_BUF, b);
 *     }
 *
 *     static void drop_buf(void *env, Any v) {
 *         (void)env;
 *         mem_free(heap_allocator(), v.data, sizeof(Buf), _Alignof(Buf));
 *     }
 *
 *     static SyncPool bufs;
 *     bufs = SYNC_POOL(heap_allocator(), BURROW_FN(SyncPoolNewFunc, make_buf, NULL),
 *                      BURROW_FN(SyncPoolFreeFunc, drop_buf, NULL));
 *
 *     Any v = sync_pool_get(&bufs);
 *     Buf *b = any_assert(v, TYPE_BUF);
 *     use(b);
 *     sync_pool_put(&bufs, v);
 *
 * What it is for is the case where the same short lived object is made and
 * thrown away over and over by many goroutines at once, a scratch buffer being
 * the usual one. It is not a free list and it is not a cache. Anything put in
 * may be gone the next time you look, so it can hold nothing you would miss.
 *
 * Underneath is Go's design. Every P has a slot of its own and a queue of its
 * own, so a Get and a Put that stay on one P touch no shared memory at all. A P
 * that finds its own queue empty steals from the far end of another P's, which
 * is the only time two of them meet.
 *
 * Two things are burrow's rather than Go's, and both come from there being no
 * collector.
 *
 * The first is the free function. Go drops an object and the collector takes it
 * from there. Here the pool has to be told how to give one back, because the
 * pool is the only thing holding it when the time comes.
 *
 * The second is when that time is. Go throws a pool's contents away at a
 * garbage collection, which means the objects live about as long as a
 * collection cycle. burrow has the system monitor do it on a timer, with Go's
 * two generation rule kept intact: what you put in survives at least one sweep
 * in the live set and one more in the victim set behind it, so an object put
 * back and taken again within a second is the same object. A program whose
 * runtime never started the monitor, and one that has gone completely idle,
 * does not sweep at all. That costs memory held and not correctness, and
 * sync_pool_free is the way to get it back on demand.
 *
 * Derived from Go's src/sync/pool.go and src/sync/poolqueue.go. */

/* What the pool calls when it has nothing to hand out. Returns a nil Any to say
 * it could not make one, which is what a Get that fails returns too. */
BURROW_FUNC0(SyncPoolNewFunc, Any);

/* What the pool calls when it is throwing one away, which is on a sweep and in
 * sync_pool_free. It must not block and it must not call back into the same
 * pool. Leave it nil to have the pool drop objects without freeing them, which
 * is right when the objects come from an arena that will be reset anyway. */
BURROW_FUNC(SyncPoolFreeFunc, void, Any v);

typedef struct SyncPool {
    /* Yours. Set by SYNC_POOL and not read anywhere else. */
    Alloc *a;
    SyncPoolNewFunc new_fn;
    SyncPoolFreeFunc free_fn;

    /* The implementation's. The two per P arrays, which one is live and which
     * one is the victim behind it, the registry link, and the lock that covers
     * setting the arrays up and the one extra slot a thread with no P uses. */
    void *shard[2];
    void *live;
    void *victim;
    struct SyncPool *allnext;
    SyncMutex mu;
    SyncAtomicUint32 inited;
} SyncPool;

/* The initialiser. Works in a block and at file scope, and a pool in a static
 * has to be assigned at start up the same way a Cond does, because
 * heap_allocator is a call.
 *
 * Nothing is allocated here. The first Get or Put builds the per P arrays, so a
 * pool that is declared and never used costs its own struct and no more. */
#define SYNC_POOL(alloc, new_func, free_func)                                          \
    ((SyncPool){.a = (alloc), .new_fn = (new_func), .free_fn = (free_func)})

/* Go's Get. Takes an object out of the pool, or makes one with new_fn when the
 * pool is empty, or returns a nil Any when the pool is empty and new_fn is nil
 * or could not make one.
 *
 * Which object you get back is not defined and may be one another goroutine put
 * in. Go says the same. An object that comes out of here is yours until you put
 * it back, and it still holds whatever the last user left in it, so reset it. */
BURROW_OWNS(ret) Any sync_pool_get(SyncPool *p);

/* Go's Put. Hands an object back, and may drop it on the floor instead, which
 * is what happens when this P's queue is full.
 *
 * A nil Any is ignored, which is what makes putting back the result of a failed
 * Get harmless. Do not put an object back twice and do not keep using it after
 * you have, for the same reason you would not free it twice. */
void sync_pool_put(SyncPool *p, Any v);

/* Hands everything in the pool to free_fn, gives the per P arrays back to the
 * allocator, and leaves the pool empty and still usable.
 *
 * Go has no such thing, because Go has a collector. Unlike Get and Put this one
 * is not safe to call concurrently with anything else on the same pool. */
void sync_pool_free(SyncPool *p);

/* Sweep every pool now, which is what the system monitor calls on its timer.
 *
 * Here so that a test can make the thing happen instead of waiting a second for
 * it, and for a program that knows it has just finished with something large
 * and would rather not wait either. Safe to call from anywhere and at any time,
 * including with every other call here running. */
void burrow__pool_sweep(void);

#ifdef __cplusplus
}
#endif

#endif /* BURROW_SYNC_H */
