/* Tests for sync.Map.
 *
 * The single threaded tests are about the trie: a key that goes in comes out,
 * an overwrite replaces, a delete removes, and the shape of the tree afterwards
 * is still something the other operations can walk. They are dull on purpose,
 * because the interesting failures are not in the logic.
 *
 * The interesting failures are memory. Every replaced and every deleted node is
 * handed to the reclamation layer rather than freed, so the two ways to get it
 * wrong are to free a node a reader is still inside, which is a use after free
 * that only the address sanitizer will see, and to never free it, which is a
 * leak that only the sanitizer's own accounting will see. Both need a build
 * with the sanitizer on and both need concurrency, which is what the last third
 * of this file is.
 *
 * One test uses a key type whose hash function always returns the same number.
 * A real hash collision in sixty four bits cannot be arranged with a random
 * seed, and the overflow chain is the code path that only a collision reaches,
 * so the type is built to collide every time.
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
#include "burrow/mem.h"
#include "burrow/mem/heap.h"
#include "burrow/proc.h"
#include "burrow/sync/atomic.h"
#include "burrow/type.h"

#include "harness.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* Enough keys to push the trie several levels down. Sixteen children per level
 * means the first level fills at about sixteen keys and collisions in the top
 * four bits start immediately, so a few thousand exercises expand, the overflow
 * free path, and the pruning on the way back up. */
#define KEYS 4000

static SyncMap new_map(void) {
    return SYNC_MAP(heap_allocator(), TYPE_INT64, TYPE_INT64);
}

static bool store(SyncMap *m, int64_t k, int64_t v) {
    return sync_map_store(m, &k, &v);
}

static bool load(SyncMap *m, int64_t k, int64_t *out) {
    return sync_map_load(m, &k, out);
}

/* ------------------------------------------------------------ one at a time */

TEST(a_key_that_was_stored_comes_back) {
    SyncMap m = new_map();

    int64_t got = -1;
    CHECK(!load(&m, 1, &got));

    CHECK(store(&m, 1, 100));
    CHECK(load(&m, 1, &got));
    CHECK_INT_EQ(got, 100);

    /* The value is copied out, so the buffer is untouched when the key is
     * absent and the caller can tell the two apart without a sentinel. */
    got = -7;
    CHECK(!load(&m, 2, &got));
    CHECK_INT_EQ(got, -7);

    sync_map_free(&m);
}

TEST(a_second_store_replaces_the_first) {
    SyncMap m = new_map();

    CHECK(store(&m, 1, 100));
    CHECK(store(&m, 1, 200));

    int64_t got = 0;
    CHECK(load(&m, 1, &got));
    CHECK_INT_EQ(got, 200);

    sync_map_free(&m);
}

TEST(swap_hands_back_what_was_there) {
    SyncMap m = new_map();

    int64_t prev = -1;
    bool loaded = true;
    int64_t v = 100;
    CHECK(sync_map_swap(&m, &(int64_t){1}, &v, &prev, &loaded));
    CHECK(!loaded);
    CHECK_INT_EQ(prev, -1); /* untouched, because there was nothing there */

    v = 200;
    CHECK(sync_map_swap(&m, &(int64_t){1}, &v, &prev, &loaded));
    CHECK(loaded);
    CHECK_INT_EQ(prev, 100);

    int64_t got = 0;
    CHECK(load(&m, 1, &got));
    CHECK_INT_EQ(got, 200);

    sync_map_free(&m);
}

TEST(load_or_store_only_stores_when_the_key_is_missing) {
    SyncMap m = new_map();

    int64_t actual = 0;
    bool loaded = true;
    CHECK(sync_map_load_or_store(&m, &(int64_t){1}, &(int64_t){100}, &actual, &loaded));
    CHECK(!loaded);
    CHECK_INT_EQ(actual, 100);

    CHECK(sync_map_load_or_store(&m, &(int64_t){1}, &(int64_t){999}, &actual, &loaded));
    CHECK(loaded);
    CHECK_INT_EQ(actual, 100);

    int64_t got = 0;
    CHECK(load(&m, 1, &got));
    CHECK_INT_EQ(got, 100);

    sync_map_free(&m);
}

TEST(compare_and_swap_only_swaps_when_the_value_matches) {
    SyncMap m = new_map();

    bool swapped = true;

    /* Nothing there at all. Not an error, just a loss. */
    CHECK(sync_map_compare_and_swap(&m, &(int64_t){1}, &(int64_t){100}, &(int64_t){200},
                                    &swapped));
    CHECK(!swapped);

    CHECK(store(&m, 1, 100));

    CHECK(sync_map_compare_and_swap(&m, &(int64_t){1}, &(int64_t){999}, &(int64_t){200},
                                    &swapped));
    CHECK(!swapped);

    int64_t got = 0;
    CHECK(load(&m, 1, &got));
    CHECK_INT_EQ(got, 100);

    CHECK(sync_map_compare_and_swap(&m, &(int64_t){1}, &(int64_t){100}, &(int64_t){200},
                                    &swapped));
    CHECK(swapped);
    CHECK(load(&m, 1, &got));
    CHECK_INT_EQ(got, 200);

    sync_map_free(&m);
}

TEST(delete_removes_and_hands_the_value_back) {
    SyncMap m = new_map();

    CHECK(store(&m, 1, 100));

    int64_t got = 0;
    CHECK(sync_map_load_and_delete(&m, &(int64_t){1}, &got));
    CHECK_INT_EQ(got, 100);
    CHECK(!load(&m, 1, &got));

    /* Deleting what is not there is not an error and does not disturb the
     * tree, which the store afterwards proves. */
    CHECK(!sync_map_load_and_delete(&m, &(int64_t){1}, &got));
    sync_map_delete(&m, &(int64_t){2});
    CHECK(store(&m, 3, 300));
    CHECK(load(&m, 3, &got));
    CHECK_INT_EQ(got, 300);

    sync_map_free(&m);
}

TEST(compare_and_delete_only_deletes_when_the_value_matches) {
    SyncMap m = new_map();

    CHECK(store(&m, 1, 100));

    CHECK(!sync_map_compare_and_delete(&m, &(int64_t){1}, &(int64_t){999}));
    int64_t got = 0;
    CHECK(load(&m, 1, &got));
    CHECK_INT_EQ(got, 100);

    CHECK(sync_map_compare_and_delete(&m, &(int64_t){1}, &(int64_t){100}));
    CHECK(!load(&m, 1, &got));

    sync_map_free(&m);
}

/* ------------------------------------------------------------- a real trie */

TEST(a_few_thousand_keys_all_come_back) {
    SyncMap m = new_map();

    for (int64_t i = 0; i < KEYS; i++)
        CHECK(store(&m, i, i * 2));

    int bad = 0;
    for (int64_t i = 0; i < KEYS; i++) {
        int64_t got = 0;
        if (!load(&m, i, &got) || got != i * 2)
            bad++;
    }
    CHECK_INT_EQ(bad, 0);

    /* Deleting every other key exercises the unlinking of nodes that have just
     * lost their last child, which is the code path that hands an indirect node
     * over rather than an entry. */
    for (int64_t i = 0; i < KEYS; i += 2)
        sync_map_delete(&m, &i);

    bad = 0;
    for (int64_t i = 0; i < KEYS; i++) {
        int64_t got = 0;
        bool want = (i % 2) != 0;
        if (load(&m, i, &got) != want)
            bad++;
        else if (want && got != i * 2)
            bad++;
    }
    CHECK_INT_EQ(bad, 0);

    sync_map_free(&m);
}

/* ------------------------------------------------- keys whose hashes collide
 *
 * A key type that hashes everything to the same number, so that every insert
 * lands in the same overflow chain. Nothing else in the map can reach that code
 * and a chain is where the subtle bugs are, because it is the only structure
 * here that is edited in place rather than replaced. */

static bool clash_equal(const void *a, const void *b) {
    return *(const int64_t *)a == *(const int64_t *)b;
}

static uint64_t clash_hash(const void *p, uint64_t seed) {
    (void)p;
    (void)seed;
    return 0x5ec0ffeeU;
}

static const TypeOps clash_ops = {clash_equal, clash_hash, NULL, NULL};

static const Type clash_desc = {
    {(const Byte *)"clash", 5},
    {(const Byte *)"test", 4},
    KIND_INT64,
    (uint32_t)sizeof(int64_t),
    (uint16_t)_Alignof(int64_t),
    0,
    0,
    NULL,
    NULL,
    NULL,
    NULL,
    0,
    0x636c7368U, /* "clsh" */
    &clash_ops,
};

TEST(keys_with_the_same_hash_all_fit) {
    const int n = 64;
    SyncMap m = SYNC_MAP(heap_allocator(), &clash_desc, TYPE_INT64);

    for (int64_t i = 0; i < n; i++)
        CHECK(store(&m, i, i * 3));

    int bad = 0;
    for (int64_t i = 0; i < n; i++) {
        int64_t got = 0;
        if (!load(&m, i, &got) || got != i * 3)
            bad++;
    }
    CHECK_INT_EQ(bad, 0);

    /* Replacing in the middle of a chain, which edits a published node's
     * overflow pointer rather than storing into a slot. */
    CHECK(store(&m, n / 2, 7));
    int64_t got = 0;
    CHECK(load(&m, n / 2, &got));
    CHECK_INT_EQ(got, 7);

    /* Dropping the head of the chain and then one from the middle. */
    CHECK(sync_map_load_and_delete(&m, &(int64_t){n - 1}, &got));
    CHECK_INT_EQ(got, (n - 1) * 3);
    CHECK(sync_map_load_and_delete(&m, &(int64_t){n / 2}, &got));
    CHECK_INT_EQ(got, 7);

    bad = 0;
    for (int64_t i = 0; i < n; i++) {
        bool want = i != n - 1 && i != n / 2;
        if (load(&m, i, &got) != want)
            bad++;
    }
    CHECK_INT_EQ(bad, 0);

    sync_map_free(&m);
}

/* ------------------------------------------------------------------- range */

typedef struct Seen {
    int64_t sum;
    int n;
    int stop_after;
    bool bad;
} Seen;

static bool see(const void *key, const void *val, void *arg) {
    Seen *s = (Seen *)arg;
    int64_t k = *(const int64_t *)key;
    int64_t v = *(const int64_t *)val;

    if (v != k * 2)
        s->bad = true;
    s->sum += k;
    s->n++;
    return s->stop_after == 0 || s->n < s->stop_after;
}

TEST(range_sees_every_key_once) {
    SyncMap m = new_map();

    const int64_t n = 200;
    int64_t want = 0;
    for (int64_t i = 0; i < n; i++) {
        CHECK(store(&m, i, i * 2));
        want += i;
    }

    Seen s = {0, 0, 0, false};
    sync_map_range(&m, see, &s);

    CHECK(!s.bad);
    CHECK_INT_EQ(s.n, n);
    CHECK_INT_EQ(s.sum, want);

    sync_map_free(&m);
}

TEST(range_stops_when_the_callback_says_so) {
    SyncMap m = new_map();

    for (int64_t i = 0; i < 200; i++)
        CHECK(store(&m, i, i * 2));

    Seen s = {0, 0, 5, false};
    sync_map_range(&m, see, &s);

    CHECK(!s.bad);
    CHECK_INT_EQ(s.n, 5);

    sync_map_free(&m);
}

TEST(range_over_an_untouched_map_does_nothing) {
    SyncMap m = new_map();

    Seen s = {0, 0, 0, false};
    sync_map_range(&m, see, &s);
    CHECK_INT_EQ(s.n, 0);

    sync_map_free(&m);
}

/* ------------------------------------------------------------------- clear */

TEST(clear_empties_the_map_and_leaves_it_usable) {
    SyncMap m = new_map();

    for (int64_t i = 0; i < 500; i++)
        CHECK(store(&m, i, i * 2));

    CHECK(sync_map_clear(&m));

    Seen s = {0, 0, 0, false};
    sync_map_range(&m, see, &s);
    CHECK_INT_EQ(s.n, 0);

    int64_t got = 0;
    CHECK(!load(&m, 1, &got));

    CHECK(store(&m, 1, 2));
    CHECK(load(&m, 1, &got));
    CHECK_INT_EQ(got, 2);

    sync_map_free(&m);
}

/* ------------------------------------------------------------- under load
 *
 * Four writers with their own ranges of keys, four readers checking that
 * whatever they find is consistent with what a writer would have written, and
 * one goroutine walking the whole map while the others work.
 *
 * A reader that sees a key at all must see the right value for it, because the
 * only value ever stored under key k is k*2 and a torn or freed node would
 * almost certainly show something else. Under the address sanitizer the read
 * itself is the report, which is the real reason this test exists.
 *
 * The readers and the walker yield at the end of every pass. They run until the
 * writers are finished and nothing here ever parks, and the scheduler has no
 * preemption yet, so a reader that never yields can hold its processor against
 * a writer that has not started. That is a hang and not a slow test: the reader
 * is waiting for a writer that is waiting for the reader. Take the yields out
 * once preemption lands. */

#define WORKERS 4
#define ROUNDS 2000
#define SPAN 500

static SyncMap shared;
static SyncAtomicInt64 running;
static SyncAtomicInt64 wrong;
static SyncAtomicInt64 refused;
static SyncAtomicUint32 stop;

static void writer(void *arg) {
    int64_t base = (int64_t)(Uintptr)arg * SPAN;

    for (int64_t r = 0; r < ROUNDS; r++) {
        int64_t k = base + (r % SPAN);
        if (!sync_map_store(&shared, &k, &(int64_t){0}))
            sync_atomic_int64_add(&refused, 1);
        int64_t v = k * 2;
        if (!sync_map_store(&shared, &k, &v))
            sync_atomic_int64_add(&refused, 1);
        if ((r % 3) == 0)
            sync_map_delete(&shared, &k);
    }

    sync_atomic_int64_add(&running, -1);
}

static void reader(void *arg) {
    (void)arg;

    while (sync_atomic_uint32_load(&stop) == 0) {
        for (int64_t k = 0; k < WORKERS * SPAN; k++) {
            int64_t got = 0;
            if (sync_map_load(&shared, &k, &got) && got != 0 && got != k * 2)
                sync_atomic_int64_add(&wrong, 1);
        }
        runtime_gosched();
    }

    sync_atomic_int64_add(&running, -1);
}

static bool check_pair(const void *key, const void *val, void *arg) {
    (void)arg;
    int64_t k = *(const int64_t *)key;
    int64_t v = *(const int64_t *)val;
    if (v != 0 && v != k * 2)
        sync_atomic_int64_add(&wrong, 1);
    return true;
}

static void walker(void *arg) {
    (void)arg;

    while (sync_atomic_uint32_load(&stop) == 0) {
        sync_map_range(&shared, check_pair, NULL);
        runtime_gosched();
    }

    sync_atomic_int64_add(&running, -1);
}

static void stress_main(void *arg) {
    (void)arg;

    runtime_gomaxprocs(WORKERS * 2 + 1);
    sync_atomic_int64_store(&running, WORKERS * 2 + 1);

    for (int64_t i = 0; i < WORKERS; i++) {
        if (!go(BURROW_FN(Func, writer, (void *)(Uintptr)i)))
            sync_atomic_int64_add(&running, -1);
    }
    for (int i = 0; i < WORKERS; i++) {
        if (!go(BURROW_FN(Func, reader, NULL)))
            sync_atomic_int64_add(&running, -1);
    }
    if (!go(BURROW_FN(Func, walker, NULL)))
        sync_atomic_int64_add(&running, -1);

    /* The writers finish on their own. The readers and the walker run until
     * they are told to stop, which is once the writers are the only ones
     * left. */
    while (sync_atomic_int64_load(&running) > WORKERS + 1)
        runtime_gosched();
    sync_atomic_uint32_store(&stop, 1);
    while (sync_atomic_int64_load(&running) > 0)
        runtime_gosched();
}

TEST(readers_never_see_a_value_that_was_never_stored) {
    shared = new_map();
    sync_atomic_int64_store(&wrong, 0);
    sync_atomic_int64_store(&refused, 0);
    sync_atomic_uint32_store(&stop, 0);

    runtime_main(BURROW_FN(Func, stress_main, NULL));

    CHECK_INT_EQ(sync_atomic_int64_load(&wrong), 0);
    CHECK_INT_EQ(sync_atomic_int64_load(&refused), 0);

    /* Every key a writer touched and did not delete on its last pass has to be
     * there with the right value, which is the check that all that unlinking
     * left a tree the reads can still walk. */
    int bad = 0;
    for (int64_t k = 0; k < WORKERS * SPAN; k++) {
        int64_t got = 0;
        if (sync_map_load(&shared, &k, &got) && got != k * 2)
            bad++;
    }
    CHECK_INT_EQ(bad, 0);

    sync_map_free(&shared);
}

/* Clear while everybody is reading, which drops a whole tree on the floor while
 * readers are inside it. Nothing here can be checked except that it does not
 * crash and does not leak, so this one is entirely for the sanitizers. */

static void clearing_writer(void *arg) {
    (void)arg;

    for (int64_t r = 0; r < 200; r++) {
        for (int64_t k = 0; k < 100; k++) {
            int64_t v = k * 2;
            if (!sync_map_store(&shared, &k, &v))
                sync_atomic_int64_add(&refused, 1);
        }
        if (!sync_map_clear(&shared))
            sync_atomic_int64_add(&refused, 1);
    }

    sync_atomic_int64_add(&running, -1);
}

static void clear_main(void *arg) {
    (void)arg;

    runtime_gomaxprocs(WORKERS + 1);
    sync_atomic_int64_store(&running, WORKERS + 1);

    if (!go(BURROW_FN(Func, clearing_writer, NULL)))
        sync_atomic_int64_add(&running, -1);
    for (int i = 0; i < WORKERS; i++) {
        if (!go(BURROW_FN(Func, reader, NULL)))
            sync_atomic_int64_add(&running, -1);
    }

    while (sync_atomic_int64_load(&running) > WORKERS)
        runtime_gosched();
    sync_atomic_uint32_store(&stop, 1);
    while (sync_atomic_int64_load(&running) > 0)
        runtime_gosched();
}

TEST(a_clear_under_readers_frees_the_old_tree_and_nothing_else) {
    shared = new_map();
    sync_atomic_int64_store(&wrong, 0);
    sync_atomic_int64_store(&refused, 0);
    sync_atomic_uint32_store(&stop, 0);

    runtime_main(BURROW_FN(Func, clear_main, NULL));

    CHECK_INT_EQ(sync_atomic_int64_load(&wrong), 0);
    CHECK_INT_EQ(sync_atomic_int64_load(&refused), 0);

    sync_map_free(&shared);
}

int main(void) {
    RUN(a_key_that_was_stored_comes_back);
    RUN(a_second_store_replaces_the_first);
    RUN(swap_hands_back_what_was_there);
    RUN(load_or_store_only_stores_when_the_key_is_missing);
    RUN(compare_and_swap_only_swaps_when_the_value_matches);
    RUN(delete_removes_and_hands_the_value_back);
    RUN(compare_and_delete_only_deletes_when_the_value_matches);
    RUN(a_few_thousand_keys_all_come_back);
    RUN(keys_with_the_same_hash_all_fit);
    RUN(range_sees_every_key_once);
    RUN(range_stops_when_the_callback_says_so);
    RUN(range_over_an_untouched_map_does_nothing);
    RUN(clear_empties_the_map_and_leaves_it_usable);
    RUN(readers_never_see_a_value_that_was_never_stored);
    RUN(a_clear_under_readers_frees_the_old_tree_and_nothing_else);

    return harness_report("sync_map");
}
