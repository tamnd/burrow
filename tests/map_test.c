/* Copyright 2026 The burrow Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style licence that can be found
 * in the LICENSE file. */

#include "burrow/core.h"
#include "burrow/map.h"
#include "burrow/mem/arena.h"
#include "burrow/mem/fixed.h"
#include "burrow/mem/heap.h"
#include "burrow/slice.h"
#include "burrow/type.h"

#include "fatal.h"
#include "harness.h"

static Arena ar;
static Alloc *a;

static void setup(void) {
    arena_init(&ar, NULL, 0);
    a = arena_allocator(&ar);
}

static void teardown(void) {
    arena_free(&ar);
}

/* struct{}, which is the value type of every set in Go's standard library and
 * the one type whose size is zero. Written out here rather than being asked for
 * from the type registry, because the registry does not exist yet. */
static const Type empty_struct = {
    {(const Byte *)"struct {}", 9},
    {NULL, 0},
    KIND_STRUCT,
    0,
    1,
    0,
    0,
    NULL,
    NULL,
    NULL,
    NULL,
    0,
    9001,
    NULL,
};

/* []int, which is not a valid map key. elem is left NULL because a static
 * initialiser cannot name TYPE_INT, and because nothing below looks past the
 * kind to decide that a slice is uncomparable. */
static const Type slice_of_int = {
    {(const Byte *)"[]int", 5},
    {NULL, 0},
    KIND_SLICE,
    (uint32_t)sizeof(Slice),
    (uint16_t)_Alignof(Slice),
    0,
    0,
    NULL,
    NULL,
    NULL,
    NULL,
    0,
    9002,
    NULL,
};

/* A quiet NaN out of its bits, rather than 0.0/0.0, which some compilers fold
 * and some warn about. */
static double make_nan(void) {
    uint64_t bits = 0x7ff8000000000000U;
    double d;
    memcpy(&d, &bits, sizeof d);
    return d;
}

static bool is_negative_zero(double d) {
    uint64_t bits;
    memcpy(&bits, &d, sizeof bits);
    return bits == 0x8000000000000000U;
}

/* ------------------------------------------------------------------- basics */

TEST(a_new_map_is_empty) {
    Map *m = map_make(a, TYPE_STRING, TYPE_INT, 0);
    CHECK(m != NULL);
    CHECK_INT_EQ(map_len(m), 0);
    CHECK(map_key_type(m) == TYPE_STRING);
    CHECK(map_val_type(m) == TYPE_INT);
    CHECK(BURROW_MAP_GET(Str, Int, m, BURROW_S("nothing")) == NULL);
}

/* A nil map in Go is readable and empty, and only writing to one is a problem.
 * Code ported from Go leans on this, since the zero value of a map field is a
 * map you are allowed to read. */
TEST(a_nil_map_reads_as_empty) {
    const void *k = NULL;
    void *v = NULL;
    MapIter it;

    CHECK_INT_EQ(map_len(NULL), 0);
    CHECK(map_key_type(NULL) == NULL);
    CHECK(map_val_type(NULL) == NULL);
    CHECK(map_get(NULL, "x") == NULL);
    CHECK(!map_get2(NULL, "x", NULL));

    /* Deleting from one and clearing one are no-ops rather than crashes, which
     * is what delete(m, k) and clear(m) do to a nil map in Go. */
    map_del(NULL, "x");
    map_clear(NULL);
    map_free(NULL);

    it = map_iter(NULL);
    CHECK(!map_next(&it, &k, &v));
}

TEST(set_then_get) {
    Map *m = map_make(a, TYPE_INT, TYPE_INT, 0);
    Int *v;

    CHECK(BURROW_MAP_SET(Int, Int, m, 42, 7));
    CHECK_INT_EQ(map_len(m), 1);

    v = BURROW_MAP_GET(Int, Int, m, 42);
    CHECK(v != NULL);
    if (v != NULL)
        CHECK_INT_EQ(*v, 7);

    CHECK(BURROW_MAP_GET(Int, Int, m, 43) == NULL);
    CHECK(BURROW_MAP_HAS(Int, m, 42));
    CHECK(!BURROW_MAP_HAS(Int, m, 43));
}

/* m[k] = v twice is one entry, and the pointer that comes back is into the
 * table, so writing through it is m[k] = v as well. */
TEST(setting_the_same_key_twice_replaces_the_value) {
    Map *m = map_make(a, TYPE_INT, TYPE_INT, 0);
    Int *v;

    BURROW_MAP_SET(Int, Int, m, 1, 10);
    BURROW_MAP_SET(Int, Int, m, 1, 20);
    CHECK_INT_EQ(map_len(m), 1);

    v = BURROW_MAP_GET(Int, Int, m, 1);
    CHECK(v != NULL);
    if (v != NULL) {
        CHECK_INT_EQ(*v, 20);
        (*v)++;
    }
    v = BURROW_MAP_GET(Int, Int, m, 1);
    CHECK(v != NULL);
    if (v != NULL)
        CHECK_INT_EQ(*v, 21);
}

TEST(get2_reports_presence_and_zeroes_on_a_miss) {
    Map *m = map_make(a, TYPE_INT, TYPE_INT, 0);
    Int key = 5;
    Int got = 999;

    CHECK(!map_get2(m, &key, &got));
    CHECK_INT_EQ(got, 0); /* v, ok := m[k] leaves v as the zero value */

    BURROW_MAP_SET(Int, Int, m, 5, 50);
    CHECK(map_get2(m, &key, &got));
    CHECK_INT_EQ(got, 50);

    /* And asking without wanting the value works. */
    CHECK(map_get2(m, &key, NULL));
}

/* The reason TypeOps exists. Two Str values with different pointers and the
 * same bytes are one string in Go, so they have to be one key. */
TEST(string_keys_compare_by_their_bytes) {
    Map *m = map_make(a, TYPE_STRING, TYPE_INT, 0);
    char buf[] = {'k', 'e', 'y'};
    Str same = str_from_bytes(buf, 3);
    Int *v;

    BURROW_MAP_SET(Str, Int, m, BURROW_S("key"), 1);
    CHECK_INT_EQ(map_len(m), 1);

    CHECK(same.p != BURROW_S("key").p);
    v = map_get(m, &same);
    CHECK(v != NULL);
    if (v != NULL)
        CHECK_INT_EQ(*v, 1);

    /* An empty key is a key, and it is not the same as a missing one. */
    BURROW_MAP_SET(Str, Int, m, BURROW_STR_EMPTY, 2);
    CHECK_INT_EQ(map_len(m), 2);
    CHECK(BURROW_MAP_HAS(Str, m, BURROW_STR_EMPTY));
}

/* --------------------------------------------------------------- the growing */

TEST(a_thousand_keys_all_come_back) {
    Map *m = map_make(a, TYPE_INT, TYPE_INT, 0);
    Int i;

    for (i = 0; i < 1000; i++)
        CHECK(BURROW_MAP_SET(Int, Int, m, i, i * 3));
    CHECK_INT_EQ(map_len(m), 1000);

    for (i = 0; i < 1000; i++) {
        Int *v = BURROW_MAP_GET(Int, Int, m, i);
        if (v == NULL) {
            CHECK(v != NULL);
            break;
        }
        CHECK_INT_EQ(*v, i * 3);
    }

    /* Nothing that was never put in is in. A table that loses track of its
     * tombstones tends to fail this one rather than the loop above. */
    for (i = 1000; i < 1100; i++)
        CHECK(BURROW_MAP_GET(Int, Int, m, i) == NULL);
}

/* A hint is a promise that filling the map to that many entries will not
 * rehash, and a rehash is observable from outside: the arena never hands out
 * the same address twice, so a table that moved is a pointer that changed. */
TEST(a_hint_means_no_rehash_while_filling_to_it) {
    Map *m = map_make(a, TYPE_INT, TYPE_INT, 100);
    Int *p;
    Int i;

    BURROW_MAP_SET(Int, Int, m, 0, 0);
    p = BURROW_MAP_GET(Int, Int, m, 0);
    CHECK(p != NULL);

    for (i = 1; i < 100; i++)
        BURROW_MAP_SET(Int, Int, m, i, i);

    CHECK(BURROW_MAP_GET(Int, Int, m, 0) == p);
    CHECK_INT_EQ(map_len(m), 100);
}

/* -------------------------------------------------------------- the deleting */

TEST(len_tracks_inserts_and_deletes) {
    Map *m = map_make(a, TYPE_INT, TYPE_INT, 0);
    Int i;

    for (i = 0; i < 50; i++)
        BURROW_MAP_SET(Int, Int, m, i, i);
    CHECK_INT_EQ(map_len(m), 50);

    for (i = 0; i < 50; i += 2)
        BURROW_MAP_DEL(Int, m, i);
    CHECK_INT_EQ(map_len(m), 25);

    for (i = 0; i < 50; i++) {
        bool want = (i % 2) == 1;
        CHECK(BURROW_MAP_HAS(Int, m, i) == want);
    }

    /* Putting a deleted key back is an insert again, not a resurrection. */
    BURROW_MAP_SET(Int, Int, m, 0, 100);
    CHECK_INT_EQ(map_len(m), 26);
    CHECK(BURROW_MAP_HAS(Int, m, 0));
}

TEST(deleting_a_key_that_is_not_there_does_nothing) {
    Map *m = map_make(a, TYPE_INT, TYPE_INT, 0);

    BURROW_MAP_DEL(Int, m, 1); /* on an empty map, which has no table yet */
    BURROW_MAP_SET(Int, Int, m, 1, 1);
    BURROW_MAP_DEL(Int, m, 2);
    CHECK_INT_EQ(map_len(m), 1);
    CHECK(BURROW_MAP_HAS(Int, m, 1));
}

/* The tombstone test, and the reason map_set rebuilds at the same size instead
 * of always doubling.
 *
 * Filling and emptying a map over and over is what a cache does, and a table
 * that leaks a tombstone per delete grows without bound while holding nothing.
 * The arena never reuses an address, so every byte it hands out is counted for
 * ever, which makes bytes_total the exact number of bytes this map has ever
 * asked for. Once the churn reaches a steady state that number has to stop
 * moving. */
TEST(filling_and_emptying_a_map_reaches_a_steady_state) {
    Arena churn;
    Alloc *ca;
    Map *m;
    uint64_t after_warmup;
    Int round, i;

    arena_init(&churn, NULL, 0);
    ca = arena_allocator(&churn);
    m = map_make(ca, TYPE_INT, TYPE_INT, 0);

    for (round = 0; round < 3; round++) {
        for (i = 0; i < 100; i++)
            CHECK(BURROW_MAP_SET(Int, Int, m, i, i));
        for (i = 0; i < 100; i++)
            BURROW_MAP_DEL(Int, m, i);
    }
    after_warmup = mem_stats(ca).bytes_total;

    for (round = 0; round < 50; round++) {
        for (i = 0; i < 100; i++)
            CHECK(BURROW_MAP_SET(Int, Int, m, i, i));
        CHECK_INT_EQ(map_len(m), 100);
        for (i = 0; i < 100; i++)
            BURROW_MAP_DEL(Int, m, i);
        CHECK_INT_EQ(map_len(m), 0);
    }

    CHECK_INT_EQ(mem_stats(ca).bytes_total, after_warmup);
    arena_free(&churn);
}

TEST(clear_empties_the_map_and_keeps_it_usable) {
    Map *m = map_make(a, TYPE_INT, TYPE_INT, 0);
    Int i;

    for (i = 0; i < 40; i++)
        BURROW_MAP_SET(Int, Int, m, i, i);

    map_clear(m);
    CHECK_INT_EQ(map_len(m), 0);
    for (i = 0; i < 40; i++)
        CHECK(BURROW_MAP_GET(Int, Int, m, i) == NULL);

    /* And the memory is still there, so refilling to the same size does not
     * allocate. Same trick as the hint test: a moved table is a moved
     * pointer. */
    BURROW_MAP_SET(Int, Int, m, 0, 0);
    {
        Int *p = BURROW_MAP_GET(Int, Int, m, 0);
        for (i = 1; i < 40; i++)
            BURROW_MAP_SET(Int, Int, m, i, i);
        CHECK(BURROW_MAP_GET(Int, Int, m, 0) == p);
    }
    CHECK_INT_EQ(map_len(m), 40);
}

/* ------------------------------------------------------------- the iteration */

TEST(iteration_visits_every_entry_exactly_once) {
    Map *m = map_make(a, TYPE_INT, TYPE_INT, 0);
    bool seen[200];
    const void *k;
    void *v;
    MapIter it;
    Int i, count = 0;

    for (i = 0; i < 200; i++) {
        BURROW_MAP_SET(Int, Int, m, i, i * 2);
        seen[i] = false;
    }

    for (it = map_iter(m); map_next(&it, &k, &v);) {
        Int key = *(const Int *)k;
        CHECK(key >= 0 && key < 200);
        if (key < 0 || key >= 200)
            continue;
        CHECK(!seen[key]);
        seen[key] = true;
        CHECK_INT_EQ(*(Int *)v, key * 2);
        count++;
    }
    CHECK_INT_EQ(count, 200);
    for (i = 0; i < 200; i++)
        CHECK(seen[i]);

    /* And it stays finished rather than starting over. */
    CHECK(!map_next(&it, &k, &v));
}

TEST(iteration_over_an_empty_map_produces_nothing) {
    Map *fresh = map_make(a, TYPE_INT, TYPE_INT, 0);
    Map *emptied = map_make(a, TYPE_INT, TYPE_INT, 8);
    MapIter it;

    it = map_iter(fresh); /* never had a table at all */
    CHECK(!map_next(&it, NULL, NULL));

    BURROW_MAP_SET(Int, Int, emptied, 1, 1);
    BURROW_MAP_DEL(Int, emptied, 1);
    it = map_iter(emptied); /* has a table, all of it tombstones or empty */
    CHECK(!map_next(&it, NULL, NULL));
}

/* The order is different every time, on purpose. This is the property that
 * stops somebody writing code that depends on it, so it is worth a test even
 * though the test is a statistical one. Sixteen iterators over a map of 64
 * entries agreeing on the first key by chance is somewhere around one in a
 * hundred billion. */
TEST(two_iterators_disagree_about_the_order) {
    Map *m = map_make(a, TYPE_INT, TYPE_INT, 0);
    Int firsts[16];
    bool all_same = true;
    Int i;

    for (i = 0; i < 64; i++)
        BURROW_MAP_SET(Int, Int, m, i, i);

    for (i = 0; i < 16; i++) {
        const void *k = NULL;
        MapIter it = map_iter(m);
        firsts[i] = map_next(&it, &k, NULL) ? *(const Int *)k : -1;
    }
    for (i = 1; i < 16; i++) {
        if (firsts[i] != firsts[0])
            all_same = false;
    }
    CHECK(!all_same);
}

TEST(deleting_during_iteration_is_allowed) {
    Map *m = map_make(a, TYPE_INT, TYPE_INT, 0);
    const void *k;
    MapIter it;
    Int i, count = 0;

    for (i = 0; i < 100; i++)
        BURROW_MAP_SET(Int, Int, m, i, i);

    /* Deleting the entry that was just produced is the common shape of this,
     * and every entry still turns up. */
    for (it = map_iter(m); map_next(&it, &k, NULL);) {
        Int key = *(const Int *)k;
        map_del(m, &key);
        count++;
    }
    CHECK_INT_EQ(count, 100);
    CHECK_INT_EQ(map_len(m), 0);

    /* An entry deleted before the walk reaches it is not produced. Emptying the
     * map on the first step therefore ends the walk after one entry. */
    for (i = 0; i < 100; i++)
        BURROW_MAP_SET(Int, Int, m, i, i);
    count = 0;
    for (it = map_iter(m); map_next(&it, &k, NULL);) {
        count++;
        for (i = 0; i < 100; i++)
            map_del(m, &i);
    }
    CHECK_INT_EQ(count, 1);
}

/* ------------------------------------------------- floats, and their corners */

/* IEEE 754 says a negative zero and a positive zero are equal, so Go's map
 * says they are one key. Go also keeps the key that arrived first, which is
 * observable here because the two have different bits. */
TEST(a_negative_zero_and_a_positive_zero_are_one_key) {
    Map *m = map_make(a, TYPE_FLOAT64, TYPE_INT, 0);
    const void *k = NULL;
    MapIter it;
    Int *v;

    BURROW_MAP_SET(double, Int, m, -0.0, 7);
    CHECK_INT_EQ(map_len(m), 1);

    v = BURROW_MAP_GET(double, Int, m, 0.0);
    CHECK(v != NULL);
    if (v != NULL)
        CHECK_INT_EQ(*v, 7);

    BURROW_MAP_SET(double, Int, m, 0.0, 8);
    CHECK_INT_EQ(map_len(m), 1);

    it = map_iter(m);
    CHECK(map_next(&it, &k, NULL));
    if (k != NULL)
        CHECK(is_negative_zero(*(const double *)k));
}

/* A NaN is not equal to itself, so a NaN key goes in and can never be found
 * again, and two of them are two entries. Go behaves exactly this way and it
 * catches everybody out once. */
TEST(every_nan_key_is_a_different_key) {
    Map *m = map_make(a, TYPE_FLOAT64, TYPE_INT, 0);
    double nan = make_nan();
    Int one = 1;

    CHECK(map_set(m, &nan, &one));
    CHECK(map_set(m, &nan, &one));
    CHECK(map_set(m, &nan, &one));
    CHECK_INT_EQ(map_len(m), 3);

    CHECK(map_get(m, &nan) == NULL);

    /* And it cannot be deleted either, which is why Go's advice is not to use
     * one as a key. Clearing the map is the only way out. */
    map_del(m, &nan);
    CHECK_INT_EQ(map_len(m), 3);
    map_clear(m);
    CHECK_INT_EQ(map_len(m), 0);

    /* Ordinary floats are ordinary. */
    BURROW_MAP_SET(double, Int, m, 1.5, 15);
    BURROW_MAP_SET(double, Int, m, -1.5, 16);
    CHECK_INT_EQ(map_len(m), 2);
    CHECK(BURROW_MAP_HAS(double, m, 1.5));
    CHECK(BURROW_MAP_HAS(double, m, -1.5));
}

TEST(float32_keys_follow_the_same_two_rules) {
    Map *m = map_make(a, TYPE_FLOAT32, TYPE_INT, 0);
    float nan = (float)make_nan();
    Int one = 1;

    BURROW_MAP_SET(float, Int, m, -0.0f, 7);
    CHECK(BURROW_MAP_HAS(float, m, 0.0f));
    CHECK_INT_EQ(map_len(m), 1);

    CHECK(map_set(m, &nan, &one));
    CHECK(map_set(m, &nan, &one));
    CHECK_INT_EQ(map_len(m), 3);
    CHECK(map_get(m, &nan) == NULL);
}

/* Go defines complex equality componentwise, so both float rules apply to both
 * halves. A complex with a NaN in it is never equal to anything. */
TEST(complex_keys_compare_componentwise) {
    Map *m = map_make(a, TYPE_COMPLEX128, TYPE_INT, 0);
    Complex128 signed_zeros = {0.0, -0.0};
    Complex128 plain_zeros = {0.0, 0.0};
    Complex128 has_nan = {1.0, 0.0};
    Int one = 1;

    has_nan.im = make_nan();

    CHECK(map_set(m, &signed_zeros, &one));
    CHECK(map_get(m, &plain_zeros) != NULL);
    CHECK_INT_EQ(map_len(m), 1);

    CHECK(map_set(m, &has_nan, &one));
    CHECK(map_set(m, &has_nan, &one));
    CHECK_INT_EQ(map_len(m), 3);
    CHECK(map_get(m, &has_nan) == NULL);
}

/* ---------------------------------------------------------- the odd shapes */

/* map[K]struct{} is how Go spells a set, so the value type has size zero and
 * has to keep working. A value pointer that came back NULL would read as a
 * missing key, so it is a real address into the slot instead. */
TEST(a_zero_sized_value_makes_a_set) {
    Map *m = map_make(a, TYPE_INT, &empty_struct, 0);
    Int i;

    for (i = 0; i < 40; i++)
        CHECK(map_set(m, &i, NULL)); /* NULL val means the zero value */
    CHECK_INT_EQ(map_len(m), 40);

    for (i = 0; i < 40; i++)
        CHECK(BURROW_MAP_GET(Int, char, m, i) != NULL);
    CHECK(BURROW_MAP_GET(Int, char, m, 41) == NULL);
    CHECK(BURROW_MAP_HAS(Int, m, 5));
}

/* And a zero sized key, which Go allows and which holds exactly one entry,
 * since every key of that type is equal to every other. */
TEST(a_zero_sized_key_holds_one_entry) {
    Map *m = map_make(a, &empty_struct, TYPE_INT, 0);
    char nothing = 0;
    Int v = 1;
    Int *got;

    CHECK(map_set(m, &nothing, &v));
    v = 2;
    CHECK(map_set(m, &nothing, &v));
    CHECK_INT_EQ(map_len(m), 1);

    got = map_get(m, &nothing);
    CHECK(got != NULL);
    if (got != NULL)
        CHECK_INT_EQ(*got, 2);
}

/* A key type with an alignment bigger than the control bytes, to make sure the
 * slots inside a group stay aligned. UndefinedBehaviorSanitizer is what
 * actually checks this, and it runs over this test in CI. */
TEST(a_wide_key_stays_aligned) {
    Map *m = map_make(a, TYPE_COMPLEX128, TYPE_COMPLEX128, 0);
    Int i;

    for (i = 0; i < 100; i++) {
        Complex128 k = {(double)i, (double)-i};
        Complex128 v = {(double)i * 2, 0.0};
        CHECK(map_set(m, &k, &v));
    }
    CHECK_INT_EQ(map_len(m), 100);

    for (i = 0; i < 100; i++) {
        Complex128 k = {(double)i, (double)-i};
        Complex128 *v = map_get(m, &k);
        CHECK(v != NULL);
        if (v != NULL)
            CHECK(v->re == (double)i * 2);
    }
}

/* ------------------------------------------------------- running out of room */

/* Go's assignment cannot fail because Go stops the world instead. A library in
 * C has to hand the decision back, so map_set returns a bool and the map is
 * untouched when it says false. */
TEST(an_insert_that_cannot_allocate_says_so) {
    unsigned char buf[2048];
    Fixed fx;
    Alloc *fa;
    Map *m;
    bool ok = true;
    Int i = 0;

    fixed_init(&fx, buf, sizeof buf);
    fa = fixed_allocator(&fx);

    m = map_make(fa, TYPE_INT, TYPE_INT, 0);
    CHECK(m != NULL);

    while (ok && i < 10000) {
        ok = BURROW_MAP_SET(Int, Int, m, i, i);
        i++;
    }

    CHECK(!ok);                       /* it ran out */
    CHECK(i > 2);                     /* after getting somewhere first */
    CHECK_INT_EQ(map_len(m), i - 1);  /* and the failed insert added nothing */
    CHECK(BURROW_MAP_HAS(Int, m, 0)); /* and lost nothing */
    CHECK(BURROW_MAP_HAS(Int, m, i - 2));
}

TEST(a_map_that_cannot_be_made_returns_null) {
    unsigned char buf[64];
    Fixed fx;
    Alloc *fa;

    fixed_init(&fx, buf, sizeof buf);
    fa = fixed_allocator(&fx);

    /* The header fits and the table for this hint does not. */
    CHECK(map_make(fa, TYPE_INT, TYPE_INT, 1000) == NULL);
}

/* The heap allocator frees what it is given, so this is where map_free gets
 * checked. AddressSanitizer and LeakSanitizer run this test in CI and they are
 * the ones holding the assertion. */
TEST(a_map_on_the_heap_can_be_freed) {
    Alloc *h = heap_allocator();
    Map *m = map_make(h, TYPE_INT, TYPE_INT, 0);
    Int i;

    for (i = 0; i < 500; i++)
        CHECK(BURROW_MAP_SET(Int, Int, m, i, i));
    CHECK_INT_EQ(map_len(m), 500);
    map_free(m);
}

/* ---------------------------------------------------------- the fatal errors */

TEST(assigning_to_a_nil_map_panics) {
    Int k = 1, v = 1;
    CHECK_RUNTIME_ERROR((void)map_set(NULL, &k, &v), "assignment to entry in nil map");
}

TEST(a_map_keyed_by_an_uncomparable_type_stops_the_program) {
    CHECK_FATAL((void)map_make(a, &slice_of_int, TYPE_INT, 0),
                "runtime error: makemap: invalid map key type");

    /* A slice as the value type is fine, since values are never compared. */
    CHECK(map_make(a, TYPE_INT, &slice_of_int, 0) != NULL);
}

TEST(an_absurd_hint_panics) {
    CHECK_RUNTIME_ERROR((void)map_make(a, TYPE_INT, TYPE_INT, -1),
                        "runtime error: makemap: size out of range");
}

/* Growing the table while an iterator is live moves every entry, so the
 * iterator would be handing out pointers into memory that has been freed. Go
 * keeps the old table alive for the iterator and can afford to because it has a
 * collector. This stops instead, which is the honest version of the same
 * unspecified program. */
TEST(growing_during_iteration_stops_the_program) {
    static Map *m;
    static MapIter it;
    Int i;

    m = map_make(a, TYPE_INT, TYPE_INT, 0);
    for (i = 0; i < 4; i++)
        BURROW_MAP_SET(Int, Int, m, i, i);

    it = map_iter(m);
    CHECK(map_next(&it, NULL, NULL));

    for (i = 100; i < 140; i++)
        BURROW_MAP_SET(Int, Int, m, i, i);

    CHECK_FATAL((void)map_next(&it, NULL, NULL), "map grew during iteration");
}

int main(void) {
    setup();
    RUN(a_new_map_is_empty);
    RUN(a_nil_map_reads_as_empty);
    RUN(set_then_get);
    RUN(setting_the_same_key_twice_replaces_the_value);
    RUN(get2_reports_presence_and_zeroes_on_a_miss);
    RUN(string_keys_compare_by_their_bytes);
    RUN(a_thousand_keys_all_come_back);
    RUN(a_hint_means_no_rehash_while_filling_to_it);
    RUN(len_tracks_inserts_and_deletes);
    RUN(deleting_a_key_that_is_not_there_does_nothing);
    RUN(filling_and_emptying_a_map_reaches_a_steady_state);
    RUN(clear_empties_the_map_and_keeps_it_usable);
    RUN(iteration_visits_every_entry_exactly_once);
    RUN(iteration_over_an_empty_map_produces_nothing);
    RUN(two_iterators_disagree_about_the_order);
    RUN(deleting_during_iteration_is_allowed);
    RUN(a_negative_zero_and_a_positive_zero_are_one_key);
    RUN(every_nan_key_is_a_different_key);
    RUN(float32_keys_follow_the_same_two_rules);
    RUN(complex_keys_compare_componentwise);
    RUN(a_zero_sized_value_makes_a_set);
    RUN(a_zero_sized_key_holds_one_entry);
    RUN(a_wide_key_stays_aligned);
    RUN(an_insert_that_cannot_allocate_says_so);
    RUN(a_map_that_cannot_be_made_returns_null);
    RUN(a_map_on_the_heap_can_be_freed);
    RUN(assigning_to_a_nil_map_panics);
    RUN(a_map_keyed_by_an_uncomparable_type_stops_the_program);
    RUN(an_absurd_hint_panics);
    RUN(growing_during_iteration_stops_the_program);
    teardown();
    return harness_report("map");
}
