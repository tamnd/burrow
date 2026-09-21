/* Tests for sync.Pool.
 *
 * A pool is allowed to lose things, which makes it awkward to test: almost
 * nothing here can assert that a Get returns what a Put put in, because a pool
 * that answered every Get with a fresh object would also be correct. So the
 * tests are about the two things a pool is never allowed to do, which are to
 * hand the same object to two callers and to lose track of one.
 *
 * Both are checked by counting. Every object the New function makes is counted,
 * every object the free function takes back is counted, and each object carries
 * a marker that says whether it is currently out with a caller. A Get that
 * returns an object already marked as out is the first failure. A count that
 * does not add up at the end is the second.
 *
 * The sweep is driven by hand rather than waited for. burrow__pool_sweep is the
 * same call the system monitor makes on its timer, so a test that makes it
 * happen is testing the same code without a second of sleeping per case.
 *
 * The usual rule applies. Only the main goroutine calls CHECK, and everybody
 * else reports through an atomic.
 *
 * Copyright 2026 The burrow Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style licence that can be found
 * in the LICENSE file. */

#include "burrow/sync.h"

#include "burrow/core.h"
#include "burrow/func.h"
#include "burrow/iface.h"
#include "burrow/mem.h"
#include "burrow/mem/heap.h"
#include "burrow/proc.h"
#include "burrow/sync/atomic.h"
#include "burrow/thread.h"
#include "burrow/type.h"

#include "harness.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* What the pool holds. The serial number is so that a failure can say which
 * object went wrong, and `out` is what catches an object handed to two callers
 * at once. */
typedef struct Item {
    int64_t serial;
    SyncAtomicUint32 out;
} Item;

/* Written out by hand, because the macro that generates one of these is on the
 * reflect milestone and is not here yet. The pool never looks past the pointer
 * being set, so a descriptor with no operations on it is enough. */
static const Type item_desc = {
    {(const Byte *)"Item", 4},
    {(const Byte *)"test", 4},
    KIND_STRUCT,
    (uint32_t)sizeof(Item),
    (uint16_t)_Alignof(Item),
    0,
    0,
    NULL,
    NULL,
    NULL,
    NULL,
    0,
    0x6974656dU, /* "item" */
    NULL,
};
static const Type *const TYPE_ITEM = &item_desc;

static SyncAtomicInt64 made;
static SyncAtomicInt64 freed;
static SyncAtomicInt64 doubled;
static SyncAtomicInt64 lost;

static Any make_item(void *env) {
    (void)env;

    Item *it = BURROW_NEW(heap_allocator(), Item);
    if (it == NULL)
        return (Any){NULL, NULL};

    it->serial = sync_atomic_int64_add(&made, 1);
    return BURROW_ANY(TYPE_ITEM, it);
}

static void drop_item(void *env, Any v) {
    (void)env;

    Item *it = (Item *)v.data;
    if (sync_atomic_uint32_load(&it->out) != 0)
        sync_atomic_int64_add(&lost, 1);

    sync_atomic_int64_add(&freed, 1);
    mem_free(heap_allocator(), it, sizeof(Item), _Alignof(Item));
}

static SyncPool new_pool(void) {
    return SYNC_POOL(heap_allocator(), BURROW_FN(SyncPoolNewFunc, make_item, NULL),
                     BURROW_FN(SyncPoolFreeFunc, drop_item, NULL));
}

static void reset_counts(void) {
    sync_atomic_int64_store(&made, 0);
    sync_atomic_int64_store(&freed, 0);
    sync_atomic_int64_store(&doubled, 0);
    sync_atomic_int64_store(&lost, 0);
}

/* Take one out, marking it, and report a pool that handed it out twice. */
static Item *take(SyncPool *p) {
    Any v = sync_pool_get(p);
    if (BURROW_ANY_IS_NIL(v))
        return NULL;

    Item *it = (Item *)v.data;
    if (sync_atomic_uint32_swap(&it->out, 1) != 0)
        sync_atomic_int64_add(&doubled, 1);
    return it;
}

static void give(SyncPool *p, Item *it) {
    sync_atomic_uint32_store(&it->out, 0);
    sync_pool_put(p, BURROW_ANY(TYPE_ITEM, it));
}

/* ------------------------------------------------------- the plain behaviour
 */

TEST(a_get_from_an_empty_pool_calls_new) {
    reset_counts();
    SyncPool p = new_pool();

    Item *it = take(&p);
    CHECK(it != NULL);
    CHECK_INT_EQ(sync_atomic_int64_load(&made), 1);

    give(&p, it);
    sync_pool_free(&p);
    CHECK_INT_EQ(sync_atomic_int64_load(&freed), 1);
}

TEST(a_get_from_an_empty_pool_with_no_new_returns_nothing) {
    SyncPoolNewFunc no_new = {NULL, NULL};
    SyncPoolFreeFunc no_free = {NULL, NULL};
    SyncPool p = SYNC_POOL(heap_allocator(), no_new, no_free);

    Any v = sync_pool_get(&p);
    CHECK(BURROW_ANY_IS_NIL(v));

    sync_pool_free(&p);
}

TEST(what_went_in_comes_back_out) {
    reset_counts();
    SyncPool p = new_pool();

    Item *first = take(&p);
    CHECK(first != NULL);
    int64_t serial = first->serial;
    give(&p, first);

    Item *again = take(&p);
    CHECK(again != NULL);
    CHECK_INT_EQ(again->serial, serial);
    CHECK_INT_EQ(sync_atomic_int64_load(&made), 1);

    give(&p, again);
    sync_pool_free(&p);
}

TEST(putting_nothing_back_is_ignored) {
    reset_counts();
    SyncPool p = new_pool();

    sync_pool_put(&p, (Any){NULL, NULL});
    CHECK_INT_EQ(sync_atomic_int64_load(&freed), 0);

    Item *it = take(&p);
    CHECK(it != NULL);
    CHECK_INT_EQ(sync_atomic_int64_load(&made), 1);

    give(&p, it);
    sync_pool_free(&p);
}

/* More than one ring holds, so the chain has to grow and the grown chain has to
 * hand everything back. The first ring is eight slots and the private slot is
 * one more, so anything past nine is a second ring. */
#define MANY 200

TEST(more_than_one_ring_holds_all_come_back) {
    reset_counts();
    SyncPool p = new_pool();

    Item *held[MANY];
    for (int i = 0; i < MANY; i++) {
        held[i] = take(&p);
        CHECK(held[i] != NULL);
    }
    CHECK_INT_EQ(sync_atomic_int64_load(&made), MANY);

    for (int i = 0; i < MANY; i++)
        give(&p, held[i]);

    int got = 0;
    for (int i = 0; i < MANY; i++) {
        Item *it = take(&p);
        if (it == NULL)
            break;
        if (it->serial > MANY)
            break;
        held[got++] = it;
    }
    CHECK_INT_EQ(got, MANY);
    CHECK_INT_EQ(sync_atomic_int64_load(&made), MANY);
    CHECK_INT_EQ(sync_atomic_int64_load(&doubled), 0);

    for (int i = 0; i < got; i++)
        give(&p, held[i]);

    sync_pool_free(&p);
    CHECK_INT_EQ(sync_atomic_int64_load(&freed), MANY);
    CHECK_INT_EQ(sync_atomic_int64_load(&lost), 0);
}

TEST(free_hands_everything_back_and_leaves_the_pool_usable) {
    reset_counts();
    SyncPool p = new_pool();

    Item *held[16];
    for (int i = 0; i < 16; i++)
        held[i] = take(&p);
    for (int i = 0; i < 16; i++)
        give(&p, held[i]);

    sync_pool_free(&p);
    CHECK_INT_EQ(sync_atomic_int64_load(&freed), 16);

    Item *it = take(&p);
    CHECK(it != NULL);
    CHECK_INT_EQ(sync_atomic_int64_load(&made), 17);

    give(&p, it);
    sync_pool_free(&p);
    CHECK_INT_EQ(sync_atomic_int64_load(&freed), 17);
}

TEST(free_on_a_pool_nobody_used_does_nothing) {
    reset_counts();
    SyncPool p = new_pool();

    sync_pool_free(&p);
    CHECK_INT_EQ(sync_atomic_int64_load(&freed), 0);
}

/* ------------------------------------------------------------- the two sweeps
 */

/* The rule this checks is Go's, which is that a pooled object survives one
 * sweep and not two. The objects go into the chains rather than the private
 * slot, so more than one of them goes in, and the chains are what a sweep
 * empties. */
TEST(one_sweep_keeps_what_was_put_and_two_do_not) {
    reset_counts();
    SyncPool p = new_pool();

    Item *held[8];
    for (int i = 0; i < 8; i++)
        held[i] = take(&p);
    for (int i = 0; i < 8; i++)
        give(&p, held[i]);
    CHECK_INT_EQ(sync_atomic_int64_load(&made), 8);

    burrow__pool_sweep();
    CHECK_INT_EQ(sync_atomic_int64_load(&freed), 0);

    /* Still there, so this takes one of the eight rather than making a ninth. */
    Item *it = take(&p);
    CHECK(it != NULL);
    CHECK(it->serial <= 8);
    CHECK_INT_EQ(sync_atomic_int64_load(&made), 8);
    give(&p, it);

    burrow__pool_sweep();
    burrow__pool_sweep();
    CHECK(sync_atomic_int64_load(&freed) >= 7);

    sync_pool_free(&p);
    CHECK_INT_EQ(sync_atomic_int64_load(&freed), 8);
    CHECK_INT_EQ(sync_atomic_int64_load(&lost), 0);
}

TEST(a_sweep_leaves_the_pool_usable) {
    reset_counts();
    SyncPool p = new_pool();

    for (int round = 0; round < 4; round++) {
        Item *held[4];
        for (int i = 0; i < 4; i++) {
            held[i] = take(&p);
            CHECK(held[i] != NULL);
        }
        for (int i = 0; i < 4; i++)
            give(&p, held[i]);
        burrow__pool_sweep();
    }

    sync_pool_free(&p);
    CHECK_INT_EQ(sync_atomic_int64_load(&freed), sync_atomic_int64_load(&made));
    CHECK_INT_EQ(sync_atomic_int64_load(&doubled), 0);
    CHECK_INT_EQ(sync_atomic_int64_load(&lost), 0);
}

TEST(a_sweep_with_no_pools_at_all_is_harmless) {
    burrow__pool_sweep();
    CHECK(true);
}

/* --------------------------------------------------------- under goroutines
 */

#define WORKERS 4
#define ROUNDS 4000

static SyncPool shared;
static SyncAtomicInt64 running;
static SyncAtomicUint32 stop;

/* Take one, hold it for a moment, put it back. The hold is what gives another
 * goroutine a chance to be handed the same object, which is the thing `out`
 * catches. */
static void worker(void *arg) {
    (void)arg;

    for (int i = 0; i < ROUNDS; i++) {
        Item *it = take(&shared);
        if (it == NULL) {
            sync_atomic_int64_add(&lost, 1);
            continue;
        }

        /* Touch it, so that a sanitizer has something to complain about if the
         * object was freed while this goroutine was holding it. */
        it->serial += 0;

        give(&shared, it);

        if ((i & 0x3f) == 0)
            runtime_gosched();
    }

    sync_atomic_int64_add(&running, -1);
}

/* Sweeps while the workers are working, which is the case the design had to be
 * built around: the monitor's thread is not an M, holds no P, and runs with
 * everything else still going.
 *
 * It yields every pass. There is no preemption yet, so a goroutine that spins
 * without yielding or parking can hold its processor against the workers and
 * wait forever for a flag only they can set. Take the yield out once preemption
 * lands. */
static void sweeper(void *arg) {
    (void)arg;

    while (sync_atomic_uint32_load(&stop) == 0) {
        burrow__pool_sweep();
        runtime_gosched();
    }

    sync_atomic_int64_add(&running, -1);
}

static void load_main(void *arg) {
    (void)arg;

    runtime_gomaxprocs(WORKERS + 1);
    sync_atomic_int64_store(&running, WORKERS + 1);

    if (!go(BURROW_FN(Func, sweeper, NULL)))
        sync_atomic_int64_add(&running, -1);
    for (int i = 0; i < WORKERS; i++) {
        if (!go(BURROW_FN(Func, worker, NULL)))
            sync_atomic_int64_add(&running, -1);
    }

    while (sync_atomic_int64_load(&running) > 1)
        runtime_gosched();
    sync_atomic_uint32_store(&stop, 1);
    while (sync_atomic_int64_load(&running) > 0)
        runtime_gosched();
}

TEST(many_goroutines_and_a_sweeper_never_share_an_object) {
    reset_counts();
    shared = new_pool();
    sync_atomic_uint32_store(&stop, 0);

    runtime_main(BURROW_FN(Func, load_main, NULL));

    CHECK_INT_EQ(sync_atomic_int64_load(&doubled), 0);
    CHECK_INT_EQ(sync_atomic_int64_load(&lost), 0);

    sync_pool_free(&shared);
    CHECK_INT_EQ(sync_atomic_int64_load(&freed), sync_atomic_int64_load(&made));
}

/* One goroutine fills the pool and a different one empties it, which is the
 * only path that reaches the steal. With one P they take turns on the same
 * slot, so the test asks for several. */
static SyncAtomicInt64 taken;

static void filler(void *arg) {
    (void)arg;

    Item *held[64];
    for (int i = 0; i < 64; i++)
        held[i] = take(&shared);
    for (int i = 0; i < 64; i++) {
        if (held[i] != NULL)
            give(&shared, held[i]);
    }

    sync_atomic_int64_add(&running, -1);
}

static void emptier(void *arg) {
    (void)arg;

    /* Wait for the filler to have finished, so that every object this takes had
     * to come off another P's queue. */
    while (sync_atomic_int64_load(&running) > 1)
        runtime_gosched();

    for (int i = 0; i < 64; i++) {
        Any v = sync_pool_get(&shared);
        if (BURROW_ANY_IS_NIL(v))
            break;
        if (((Item *)v.data)->serial <= 64)
            sync_atomic_int64_add(&taken, 1);
        sync_pool_put(&shared, v);
    }

    sync_atomic_int64_add(&running, -1);
}

static void steal_main(void *arg) {
    (void)arg;

    runtime_gomaxprocs(4);
    sync_atomic_int64_store(&running, 2);

    if (!go(BURROW_FN(Func, emptier, NULL)))
        sync_atomic_int64_add(&running, -1);
    if (!go(BURROW_FN(Func, filler, NULL)))
        sync_atomic_int64_add(&running, -1);

    while (sync_atomic_int64_load(&running) > 0)
        runtime_gosched();
}

TEST(a_goroutine_takes_what_another_one_left) {
    reset_counts();
    shared = new_pool();
    sync_atomic_int64_store(&taken, 0);

    runtime_main(BURROW_FN(Func, steal_main, NULL));

    CHECK_INT_EQ(sync_atomic_int64_load(&made), 64);
    CHECK(sync_atomic_int64_load(&taken) > 0);
    CHECK_INT_EQ(sync_atomic_int64_load(&doubled), 0);

    sync_pool_free(&shared);
    CHECK_INT_EQ(sync_atomic_int64_load(&freed), 64);
}

/* ------------------------------------------------------- from outside a goroutine
 */

static void foreign(void *arg) {
    SyncPool *p = (SyncPool *)arg;

    for (int i = 0; i < 500; i++) {
        Item *it = take(p);
        if (it == NULL) {
            sync_atomic_int64_add(&lost, 1);
            continue;
        }
        give(p, it);
    }
}

TEST(a_thread_that_is_not_a_goroutine_can_use_a_pool) {
    reset_counts();
    SyncPool p = new_pool();

    burrow__Thread t[3];
    int started = 0;
    for (int i = 0; i < 3; i++) {
        if (burrow__thread_start(&t[i], foreign, &p, 0))
            started++;
    }
    for (int i = 0; i < started; i++)
        burrow__thread_join(&t[i]);

    CHECK_INT_EQ(started, 3);
    CHECK_INT_EQ(sync_atomic_int64_load(&doubled), 0);
    CHECK_INT_EQ(sync_atomic_int64_load(&lost), 0);

    sync_pool_free(&p);
    CHECK_INT_EQ(sync_atomic_int64_load(&freed), sync_atomic_int64_load(&made));
}

int main(void) {
    RUN(a_get_from_an_empty_pool_calls_new);
    RUN(a_get_from_an_empty_pool_with_no_new_returns_nothing);
    RUN(what_went_in_comes_back_out);
    RUN(putting_nothing_back_is_ignored);
    RUN(more_than_one_ring_holds_all_come_back);
    RUN(free_hands_everything_back_and_leaves_the_pool_usable);
    RUN(free_on_a_pool_nobody_used_does_nothing);
    RUN(one_sweep_keeps_what_was_put_and_two_do_not);
    RUN(a_sweep_leaves_the_pool_usable);
    RUN(a_sweep_with_no_pools_at_all_is_harmless);
    RUN(many_goroutines_and_a_sweeper_never_share_an_object);
    RUN(a_goroutine_takes_what_another_one_left);
    RUN(a_thread_that_is_not_a_goroutine_can_use_a_pool);

    return harness_report("sync_pool");
}
