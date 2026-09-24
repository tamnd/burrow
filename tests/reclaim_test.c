/* Tests for epoch based reclamation.
 *
 * There are two things to get wrong here and they fail in opposite directions.
 * Freeing too late is a leak, which a counter can see. Freeing too early is a
 * read of memory somebody gave back, which a counter cannot see and which the
 * address sanitizer can, so the stress test below allocates real memory, writes
 * a pattern into it, and reads the pattern back while another goroutine is
 * swapping the object out from under it. A build with the sanitizer on is where
 * that test earns its keep.
 *
 * The rest are timing tests written without timing. Every one of them drives
 * the epoch by hand with burrow__reclaim_flush, so a run either sees the object
 * survive the flushes it is supposed to survive or it does not, and there is no
 * sleep anywhere to make the answer depend on how loaded the machine is.
 *
 * The usual rule applies. Only the main goroutine calls CHECK, and everybody
 * else reports through an atomic.
 *
 * Copyright 2026 The burrow Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style licence that can be found
 * in the LICENSE file. */

#include "burrow/reclaim.h"

#include "burrow/atomic.h"
#include "burrow/core.h"
#include "burrow/func.h"
#include "burrow/mem.h"
#include "burrow/mem/heap.h"
#include "burrow/proc.h"
#include "burrow/sync/atomic.h"
#include "burrow/thread.h"

#include "check.h"
#include "fatal.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* Enough flushes to walk the epoch all the way round, with room to spare. An
 * object retired now is freed three epochs later and one flush moves the epoch
 * at most once, so four would do and ten is there so that a change to the
 * number of bags does not quietly turn this file into a test of nothing. */
#define FLUSHES 10

#define MAGIC 0x5ec0ffee5ec0ffeeULL
#define DEAD 0xdeadbeefdeadbeefULL

/* What gets allocated, retired and freed. The bookkeeping goes first, which is
 * not required and does make the debugger output easier to read. */
typedef struct Box {
    burrow__Retired retired;
    uint64_t magic;
} Box;

static SyncAtomicInt64 frees;
static SyncAtomicInt64 bad_reads;

static Box *box_new(void) {
    Box *b = BURROW_NEW(heap_allocator(), Box);
    if (b != NULL)
        b->magic = MAGIC;
    return b;
}

static void box_free(void *p) {
    Box *b = (Box *)p;
    b->magic = DEAD;
    sync_atomic_int64_add(&frees, 1);
    mem_free(heap_allocator(), b, sizeof(Box), _Alignof(Box));
}

static void flush_a_few(void) {
    for (int i = 0; i < FLUSHES; i++)
        burrow__reclaim_flush();
}

static void reset(void) {
    burrow__reclaim_drain();
    sync_atomic_int64_store(&frees, 0);
    sync_atomic_int64_store(&bad_reads, 0);
}

/* --------------------------------------------------- nobody is reading at all */

static void TestAnObjectNobodyIsReadingIsFreed(TestingT *t) {
    reset();

    Box *b = box_new();
    CHECK(b != NULL);

    burrow__retire(&b->retired, box_free, b);
    CHECK_INT_EQ(burrow__reclaim_pending(), 1);

    /* Not freed yet. It is in this thread's pocket, and the point of the pocket
     * is that a burst of deletes pays for one walk of the slots rather than
     * one each. */
    CHECK_INT_EQ(sync_atomic_int64_load(&frees), 0);

    flush_a_few();

    CHECK_INT_EQ(sync_atomic_int64_load(&frees), 1);
    CHECK_INT_EQ(burrow__reclaim_pending(), 0);
}

static void TestABatchIsFreedWithoutAnybodyAsking(TestingT *t) {
    reset();

    /* One more than the batch size, so the retire that crosses it pushes and
     * collects by itself. Nothing here calls flush. */
    const int n = 65;
    for (int i = 0; i < n; i++) {
        Box *b = box_new();
        CHECK(b != NULL);
        burrow__retire(&b->retired, box_free, b);
    }

    /* The push happened, so the objects are on the shared list rather than in
     * this thread's pocket, and the epoch has moved. They are not free yet
     * because freeing them needs the epoch to move three times, and one retire
     * only moves it once. What this checks is that a program that never calls
     * flush still makes progress. */
    for (int i = 0; i < n; i++) {
        Box *b = box_new();
        CHECK(b != NULL);
        burrow__retire(&b->retired, box_free, b);
    }
    for (int i = 0; i < n; i++) {
        Box *b = box_new();
        CHECK(b != NULL);
        burrow__retire(&b->retired, box_free, b);
    }

    CHECK(sync_atomic_int64_load(&frees) > 0);

    flush_a_few();
    CHECK_INT_EQ(burrow__reclaim_pending(), 0);
    CHECK_INT_EQ(sync_atomic_int64_load(&frees), 3 * n);
}

/* ------------------------------------------------------- somebody is reading */

static void TestAPinnedThreadHoldsTheObjectBack(TestingT *t) {
    reset();

    Box *b = box_new();
    CHECK(b != NULL);

    burrow__pin();
    burrow__retire(&b->retired, box_free, b);
    flush_a_few();

    /* This thread is pinned, so the epoch got one step and then stopped. Three
     * steps is what the object needs, so it is still here. */
    CHECK_INT_EQ(sync_atomic_int64_load(&frees), 0);
    CHECK(b->magic == MAGIC);

    burrow__unpin();
    flush_a_few();

    CHECK_INT_EQ(sync_atomic_int64_load(&frees), 1);
}

static void TestANestedPinIsOnlyReleasedByTheOuterUnpin(TestingT *t) {
    reset();

    Box *b = box_new();
    CHECK(b != NULL);

    burrow__pin();
    burrow__pin();
    burrow__unpin();

    burrow__retire(&b->retired, box_free, b);
    flush_a_few();

    /* Still pinned, because the inner unpin above matched the inner pin and
     * not the outer one. A depth counter that got this wrong would let the
     * epoch run and free the object here. */
    CHECK_INT_EQ(sync_atomic_int64_load(&frees), 0);

    burrow__unpin();
    flush_a_few();

    CHECK_INT_EQ(sync_atomic_int64_load(&frees), 1);
}

static void TestAnUnpinWithoutAPinIsCaught(TestingT *t) {
    reset();

    CHECK_FATAL(burrow__unpin(), "reclaim: unpin without a pin");
}

/* ---------------------------------------------- a reader on a foreign thread */

static SyncAtomicUint32 reader_pinned;
static SyncAtomicUint32 reader_release;
static SyncAtomicUint32 reader_done;

static void foreign_reader(void *arg) {
    (void)arg;

    burrow__pin();
    sync_atomic_uint32_store(&reader_pinned, 1);

    /* Spinning rather than sleeping, because a pin belongs to the thread and a
     * thread that parks inside one holds the epoch still for as long as it is
     * parked. This is the rule the header states and this loop is what obeying
     * it looks like. */
    while (sync_atomic_uint32_load(&reader_release) == 0)
        burrow__thread_yield();

    burrow__unpin();
    sync_atomic_uint32_store(&reader_done, 1);
}

static void TestAThreadThatIsNotAGoroutineHoldsTheEpochToo(TestingT *t) {
    reset();
    sync_atomic_uint32_store(&reader_pinned, 0);
    sync_atomic_uint32_store(&reader_release, 0);
    sync_atomic_uint32_store(&reader_done, 0);

    burrow__Thread th;
    CHECK(burrow__thread_start(&th, foreign_reader, NULL, 0));

    while (sync_atomic_uint32_load(&reader_pinned) == 0)
        burrow__thread_yield();

    Box *b = box_new();
    CHECK(b != NULL);
    burrow__retire(&b->retired, box_free, b);
    flush_a_few();

    CHECK_INT_EQ(sync_atomic_int64_load(&frees), 0);

    sync_atomic_uint32_store(&reader_release, 1);
    while (sync_atomic_uint32_load(&reader_done) == 0)
        burrow__thread_yield();

    flush_a_few();
    CHECK_INT_EQ(sync_atomic_int64_load(&frees), 1);

    CHECK(burrow__thread_join(&th));
}

/* ------------------------------------------------------------- under real load
 *
 * One cell, one goroutine replacing what is in it, and the rest reading it. The
 * readers check that what they found still has its pattern in it, which is the
 * only way a use after free shows up without a sanitizer, and with a sanitizer
 * the read itself is the report. */

#define READERS 4
#define SWAPS 4000
#define READS 20000

static Box *cell;
static SyncAtomicUint32 stop;
static SyncAtomicInt64 running;

static void stress_reader(void *arg) {
    (void)arg;

    for (int64_t i = 0; i < READS; i++) {
        burrow__pin();
        Box *b = (Box *)burrow__atomic_load_ptr((void *const *)&cell);
        if (b != NULL && b->magic != MAGIC)
            sync_atomic_int64_add(&bad_reads, 1);
        burrow__unpin();

        if (sync_atomic_uint32_load(&stop) != 0)
            break;
    }

    sync_atomic_int64_add(&running, -1);
}

static void stress_main(void *arg) {
    (void)arg;

    runtime_gomaxprocs(READERS + 1);

    sync_atomic_int64_store(&running, READERS);
    for (int i = 0; i < READERS; i++) {
        if (!go(BURROW_FN(Func, stress_reader, NULL)))
            sync_atomic_int64_add(&running, -1);
    }

    for (int64_t i = 0; i < SWAPS; i++) {
        Box *fresh = box_new();
        if (fresh == NULL)
            break;
        /* Only this goroutine ever writes the cell, so reading the old value
         * without an atomic is reading a variable nobody else can have
         * changed. The store has to be one, because the readers are loading
         * it and what they get has to be either the old object or the new one
         * and never half of a pointer. */
        Box *old = cell;
        burrow__atomic_store_ptr((void **)&cell, fresh);
        if (old != NULL)
            burrow__retire(&old->retired, box_free, old);
    }

    sync_atomic_uint32_store(&stop, 1);
    while (sync_atomic_int64_load(&running) > 0)
        runtime_gosched();
}

static void TestReadersNeverSeeAnObjectThatWasFreed(TestingT *t) {
    reset();
    cell = NULL;
    sync_atomic_uint32_store(&stop, 0);

    runtime_main(BURROW_FN(Func, stress_main, NULL));

    CHECK_INT_EQ(sync_atomic_int64_load(&bad_reads), 0);

    /* Everything that was swapped out has to have been handed over, and the
     * last one is still in the cell rather than retired. */
    CHECK(cell != NULL);
    CHECK(cell->magic == MAGIC);

    /* A drain rather than a flush, because a flush only empties the pocket of
     * the M the caller is on and the swapper ran on whichever M the scheduler
     * gave it. The runtime has stopped by now, so a drain is the right tool and
     * the accounting below is exact. */
    burrow__reclaim_drain();
    CHECK_INT_EQ(burrow__reclaim_pending(), 0);
    CHECK_INT_EQ(sync_atomic_int64_load(&frees), SWAPS - 1);

    box_free(cell);
    cell = NULL;
}

/* ---------------------------------------------------------------- shutting down */

static void TestADrainFreesWhatTheEpochHasNotGotToYet(TestingT *t) {
    reset();

    for (int i = 0; i < 5; i++) {
        Box *b = box_new();
        CHECK(b != NULL);
        burrow__retire(&b->retired, box_free, b);
    }

    /* One flush, which pushes them onto the shared list and moves the epoch one
     * step. Three steps is what they need, so nothing is free. */
    burrow__reclaim_flush();
    CHECK_INT_EQ(sync_atomic_int64_load(&frees), 0);

    burrow__reclaim_drain();

    CHECK_INT_EQ(sync_atomic_int64_load(&frees), 5);
    CHECK_INT_EQ(burrow__reclaim_pending(), 0);
}

#define TESTS(X)                                                                       \
    X(TestAnObjectNobodyIsReadingIsFreed)                                              \
    X(TestABatchIsFreedWithoutAnybodyAsking)                                           \
    X(TestAPinnedThreadHoldsTheObjectBack)                                             \
    X(TestANestedPinIsOnlyReleasedByTheOuterUnpin)                                     \
    X(TestAnUnpinWithoutAPinIsCaught)                                                  \
    X(TestAThreadThatIsNotAGoroutineHoldsTheEpochToo)                                  \
    X(TestReadersNeverSeeAnObjectThatWasFreed)                                         \
    X(TestADrainFreesWhatTheEpochHasNotGotToYet)

TESTING_MAIN_BARE(TESTS)
