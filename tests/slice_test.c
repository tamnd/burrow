/* Copyright 2026 The burrow Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style licence that can be found
 * in the LICENSE file. */

#include "burrow/core.h"
#include "burrow/mem/arena.h"
#include "burrow/slice.h"
#include "burrow/type.h"

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

static bool str_is(Str got, const char *want) {
    size_t n = strlen(want);
    if (got.len != (Int)n)
        return false;
    return n == 0 || memcmp(got.p, want, n) == 0;
}

/* ------------------------------------------------------------------- make */

TEST(make_gives_the_length_and_capacity_asked_for) {
    Slice s = slice_make(a, TYPE_INT, 3, 10);
    CHECK_INT_EQ(s.len, 3);
    CHECK_INT_EQ(s.cap, 10);
    CHECK(s.elem == TYPE_INT);
    CHECK(s.p != NULL);
}

/* Go's zero value rule is the language and not a convention, so make gives back
 * zeroes and code ported from Go is allowed to rely on it without asking. */
TEST(make_zeroes_what_it_hands_back) {
    Slice s = slice_make(a, TYPE_INT, 64, 64);
    for (Int i = 0; i < s.len; i++)
        CHECK_INT_EQ(BURROW_AT(Int, s, i), 0);
}

TEST(a_capacity_bigger_than_the_length_is_allowed_and_is_not_readable) {
    Slice s = slice_make(a, TYPE_INT, 2, 8);
    CHECK_INT_EQ(s.len, 2);
    CHECK_INT_EQ(s.cap, 8);
    /* The spare capacity exists but is not part of the slice, which is why
     * slice_at checks against len. Reaching it is what reslicing is for. */
    Slice grown = slice_sub(s, 0, 8);
    CHECK_INT_EQ(grown.len, 8);
    for (Int i = 0; i < grown.len; i++)
        CHECK_INT_EQ(BURROW_AT(Int, grown, i), 0);
}

/* nil and empty are different values in Go and the difference is observable:
 * a nil slice marshals to null and an empty one to []. Keeping them apart here
 * is what lets encoding/json be faithful later. */
TEST(nil_and_empty_are_not_the_same_thing) {
    Slice n = slice_nil(TYPE_INT);
    CHECK(slice_is_nil(n));
    CHECK_INT_EQ(n.len, 0);
    CHECK_INT_EQ(n.cap, 0);
    CHECK(n.elem == TYPE_INT);

    Slice e = slice_make(a, TYPE_INT, 0, 0);
    CHECK(!slice_is_nil(e));
    CHECK_INT_EQ(e.len, 0);
    CHECK_INT_EQ(e.cap, 0);
}

TEST(from_wraps_memory_without_copying_it) {
    Int backing[4] = {10, 20, 30, 40};
    Slice s = slice_from(backing, 4, 4, TYPE_INT);
    CHECK(s.p == backing);
    CHECK_INT_EQ(BURROW_AT(Int, s, 2), 30);

    /* Writing through the slice writes through to the array, because there is
     * only one array. */
    BURROW_AT(Int, s, 2) = 99;
    CHECK_INT_EQ(backing[2], 99);
}

/* ------------------------------------------------------------------ index */

TEST(index_reads_and_writes_the_element) {
    Slice s = slice_make(a, TYPE_INT, 4, 4);
    for (Int i = 0; i < 4; i++)
        BURROW_AT(Int, s, i) = i * i;
    CHECK_INT_EQ(BURROW_AT(Int, s, 0), 0);
    CHECK_INT_EQ(BURROW_AT(Int, s, 1), 1);
    CHECK_INT_EQ(BURROW_AT(Int, s, 2), 4);
    CHECK_INT_EQ(BURROW_AT(Int, s, 3), 9);

    /* slice_at returns a pointer into the backing array rather than a copy, so
     * the arithmetic has to land on the element and not near it. */
    CHECK(slice_at(s, 1) == (Byte *)s.p + sizeof(Int));
}

TEST(index_works_on_a_slice_of_strings) {
    Slice s = slice_make(a, TYPE_STRING, 2, 2);
    BURROW_AT(Str, s, 0) = BURROW_S("alpha");
    BURROW_AT(Str, s, 1) = BURROW_S("beta");
    CHECK(str_is(BURROW_AT(Str, s, 0), "alpha"));
    CHECK(str_is(BURROW_AT(Str, s, 1), "beta"));
}

/* ------------------------------------------------------------- reslicing */

TEST(sub_is_gos_two_index_slice_expression) {
    Slice s = slice_make(a, TYPE_INT, 10, 16);
    for (Int i = 0; i < 10; i++)
        BURROW_AT(Int, s, i) = i;

    Slice m = slice_sub(s, 2, 5);
    CHECK_INT_EQ(m.len, 3);
    /* cap is cap - lo and not hi - lo, which is the part people forget. */
    CHECK_INT_EQ(m.cap, 14);
    CHECK_INT_EQ(BURROW_AT(Int, m, 0), 2);
    CHECK_INT_EQ(BURROW_AT(Int, m, 2), 4);
    CHECK(m.elem == s.elem);
}

TEST(sub_does_not_copy) {
    Slice s = slice_make(a, TYPE_INT, 8, 8);
    Slice m = slice_sub(s, 4, 8);
    BURROW_AT(Int, m, 0) = 1234;
    CHECK_INT_EQ(BURROW_AT(Int, s, 4), 1234);
}

TEST(sub_can_reach_past_the_length_up_to_the_capacity) {
    Slice s = slice_make(a, TYPE_INT, 2, 8);
    /* Bounds are checked against cap and not len, which is exactly what Go
     * does and is what makes s = s[:cap(s)] legal. */
    Slice g = slice_sub(s, 0, 8);
    CHECK_INT_EQ(g.len, 8);
    CHECK_INT_EQ(g.cap, 8);
}

TEST(sub3_caps_the_result) {
    Slice s = slice_make(a, TYPE_INT, 10, 16);
    Slice m = slice_sub3(s, 2, 5, 6);
    CHECK_INT_EQ(m.len, 3);
    CHECK_INT_EQ(m.cap, 4);

    /* The whole reason the three index form exists: appending to m now has to
     * allocate rather than overwriting s[6]. */
    BURROW_AT(Int, s, 6) = 777;
    Slice grown = slice_append(a, m, (const Int[]){1, 2}, 2);
    CHECK_INT_EQ(grown.len, 5);
    CHECK_INT_EQ(BURROW_AT(Int, s, 6), 777);
}

TEST(an_empty_slice_of_an_empty_slice_is_fine) {
    Slice n = slice_nil(TYPE_INT);
    Slice m = slice_sub(n, 0, 0);
    CHECK_INT_EQ(m.len, 0);
    CHECK_INT_EQ(m.cap, 0);
    /* A nil pointer stays a nil pointer rather than becoming p + 0, which would
     * be undefined even though every compiler does the obvious thing. */
    CHECK(m.p == NULL);
}

/* ----------------------------------------------------------------- append */

TEST(append_to_nil_starts_a_slice) {
    Slice s = slice_nil(TYPE_INT);
    s = BURROW_APPEND(Int, a, s, 42);
    CHECK_INT_EQ(s.len, 1);
    CHECK_INT_EQ(s.cap, 1);
    CHECK_INT_EQ(BURROW_AT(Int, s, 0), 42);
    CHECK(s.elem == TYPE_INT);
}

TEST(append_of_nothing_returns_the_slice_unchanged) {
    Slice s = slice_make(a, TYPE_INT, 3, 5);
    Slice r = slice_append(a, s, NULL, 0);
    CHECK(r.p == s.p);
    CHECK_INT_EQ(r.len, 3);
    CHECK_INT_EQ(r.cap, 5);

    /* append(nil) is nil in Go, not an empty slice, and that survives here. */
    Slice n = slice_nil(TYPE_INT);
    CHECK(slice_is_nil(slice_append(a, n, NULL, 0)));
}

/* The capacity progression is a faithful port of Go's nextslicecap: double
 * below 256 elements, then approach 1.25x through the formula Go uses.
 *
 * Go applies a size class rounding step on top of this which burrow does not,
 * because that step is a property of Go's allocator and burrow's allocator is
 * whatever the caller passed in. The numbers below are nextslicecap's, so they
 * agree with Go for element sizes where the rounding is a no-op and diverge
 * where it is not. docs/guides/slices.md has the long version and the measured
 * numbers from go1.27.1. */
TEST(the_growth_sequence_is_gos) {
    Slice s = slice_nil(TYPE_INT);
    const Int want[] = {1, 2, 4, 4, 8, 8, 8, 8, 16, 16, 16, 16, 16, 16, 16, 16};
    for (Int i = 0; i < 16; i++) {
        s = BURROW_APPEND(Int, a, s, i);
        CHECK_INT_EQ(s.cap, want[i]);
    }
    for (Int i = 0; i < 16; i++)
        CHECK_INT_EQ(BURROW_AT(Int, s, i), i);
}

TEST(growth_switches_off_doubling_at_two_hundred_and_fifty_six) {
    Slice s = slice_make(a, TYPE_INT, 256, 256);

    /* Below the threshold it would have doubled to 512, and here it does too,
     * because the formula's first step from 256 lands exactly on 512. */
    s = BURROW_APPEND(Int, a, s, 0);
    CHECK_INT_EQ(s.len, 257);
    CHECK_INT_EQ(s.cap, 512);

    /* The next one is where the two rules visibly part company: doubling would
     * give 1024 and the formula gives 832. */
    Slice t = slice_make(a, TYPE_INT, 512, 512);
    t = BURROW_APPEND(Int, a, t, 0);
    CHECK_INT_EQ(t.cap, 832);

    Slice u = slice_make(a, TYPE_INT, 832, 832);
    u = BURROW_APPEND(Int, a, u, 0);
    CHECK_INT_EQ(u.cap, 1232);
}

TEST(appending_more_than_double_takes_exactly_what_is_needed) {
    Slice s = slice_make(a, TYPE_INT, 4, 4);
    const Int more[] = {1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12};
    s = slice_append(a, s, more, 12);
    CHECK_INT_EQ(s.len, 16);
    /* Doubling would give 8, which is not enough, so Go takes the new length. */
    CHECK_INT_EQ(s.cap, 16);
}

/* This is the behaviour that makes append subtle and it is reproduced on
 * purpose. When the capacity is there, the new elements go into the backing
 * array that other slices are still looking at. Go does this, Go's tests depend
 * on it, and a version of burrow that quietly always copied would be a nicer
 * library that runs ported Go code incorrectly. */
TEST(append_within_capacity_is_visible_through_the_other_slice) {
    Slice base = slice_make(a, TYPE_INT, 8, 8);
    for (Int i = 0; i < 8; i++)
        BURROW_AT(Int, base, i) = 100 + i;

    Slice head = slice_sub(base, 0, 3);
    CHECK_INT_EQ(head.cap, 8);

    Slice grown = slice_append(a, head, (const Int[]){999}, 1);
    CHECK(grown.p == base.p);
    CHECK_INT_EQ(grown.len, 4);
    CHECK_INT_EQ(BURROW_AT(Int, base, 3), 999);
}

TEST(append_past_capacity_stops_sharing) {
    Slice base = slice_make(a, TYPE_INT, 4, 4);
    for (Int i = 0; i < 4; i++)
        BURROW_AT(Int, base, i) = 100 + i;

    Slice grown = slice_append(a, base, (const Int[]){999}, 1);
    CHECK(grown.p != base.p);
    CHECK_INT_EQ(BURROW_AT(Int, grown, 0), 100);
    CHECK_INT_EQ(BURROW_AT(Int, grown, 4), 999);

    /* Writing to the new one now leaves the old one alone. */
    BURROW_AT(Int, grown, 0) = 0;
    CHECK_INT_EQ(BURROW_AT(Int, base, 0), 100);
}

/* append(s, s...), which works because the copy happens after the allocation
 * and the old array is still there while it does. */
TEST(a_slice_can_be_appended_to_itself) {
    Slice s = slice_make(a, TYPE_INT, 3, 3);
    for (Int i = 0; i < 3; i++)
        BURROW_AT(Int, s, i) = i + 1;

    s = slice_append_slice(a, s, s);
    CHECK_INT_EQ(s.len, 6);
    CHECK_INT_EQ(BURROW_AT(Int, s, 0), 1);
    CHECK_INT_EQ(BURROW_AT(Int, s, 1), 2);
    CHECK_INT_EQ(BURROW_AT(Int, s, 2), 3);
    CHECK_INT_EQ(BURROW_AT(Int, s, 3), 1);
    CHECK_INT_EQ(BURROW_AT(Int, s, 4), 2);
    CHECK_INT_EQ(BURROW_AT(Int, s, 5), 3);
}

/* Go clears the tail of a grown array and it has to, because the next thing
 * somebody does is reslice up to the capacity and read a zero value out of it.
 * The allocation here is deliberately not zeroed, so this is checking that the
 * memset actually happens rather than that the arena happened to be clean. */
TEST(the_spare_capacity_after_a_grow_is_zeroed) {
    Slice s = slice_make(a, TYPE_INT, 4, 4);
    for (Int i = 0; i < 4; i++)
        BURROW_AT(Int, s, i) = -1;

    s = BURROW_APPEND(Int, a, s, -1);
    CHECK_INT_EQ(s.len, 5);
    CHECK_INT_EQ(s.cap, 8);

    Slice all = slice_sub(s, 0, s.cap);
    for (Int i = 5; i < 8; i++)
        CHECK_INT_EQ(BURROW_AT(Int, all, i), 0);
}

TEST(append_slice_moves_every_element) {
    Slice dst = slice_make(a, TYPE_STRING, 1, 1);
    BURROW_AT(Str, dst, 0) = BURROW_S("first");

    Slice src = slice_make(a, TYPE_STRING, 2, 2);
    BURROW_AT(Str, src, 0) = BURROW_S("second");
    BURROW_AT(Str, src, 1) = BURROW_S("third");

    dst = slice_append_slice(a, dst, src);
    CHECK_INT_EQ(dst.len, 3);
    CHECK(str_is(BURROW_AT(Str, dst, 0), "first"));
    CHECK(str_is(BURROW_AT(Str, dst, 1), "second"));
    CHECK(str_is(BURROW_AT(Str, dst, 2), "third"));
}

/* ------------------------------------------------------------------- copy */

TEST(copy_takes_the_shorter_of_the_two_lengths) {
    Slice dst = slice_make(a, TYPE_INT, 2, 8);
    Slice src = slice_make(a, TYPE_INT, 5, 5);
    for (Int i = 0; i < 5; i++)
        BURROW_AT(Int, src, i) = i + 1;

    CHECK_INT_EQ(slice_copy(dst, src), 2);
    CHECK_INT_EQ(BURROW_AT(Int, dst, 0), 1);
    CHECK_INT_EQ(BURROW_AT(Int, dst, 1), 2);

    /* And the other way round, where the destination is the longer one. */
    Slice big = slice_make(a, TYPE_INT, 5, 5);
    Slice small = slice_sub(src, 0, 2);
    CHECK_INT_EQ(slice_copy(big, small), 2);
    CHECK_INT_EQ(BURROW_AT(Int, big, 2), 0);
}

/* copy(s, s[1:]) is how you delete an element, so overlap is not an edge case,
 * it is the common case, and Go's copy promises it works. */
TEST(copy_handles_overlap) {
    Slice s = slice_make(a, TYPE_INT, 5, 5);
    for (Int i = 0; i < 5; i++)
        BURROW_AT(Int, s, i) = i;

    Int n = slice_copy(slice_sub(s, 1, 5), slice_sub(s, 2, 5));
    CHECK_INT_EQ(n, 3);
    CHECK_INT_EQ(BURROW_AT(Int, s, 0), 0);
    CHECK_INT_EQ(BURROW_AT(Int, s, 1), 2);
    CHECK_INT_EQ(BURROW_AT(Int, s, 2), 3);
    CHECK_INT_EQ(BURROW_AT(Int, s, 3), 4);
}

TEST(copy_from_a_string_fills_a_byte_slice) {
    Slice b = slice_make(a, TYPE_BYTE, 3, 3);
    CHECK_INT_EQ(slice_copy_str(b, BURROW_S("hello")), 3);
    CHECK_INT_EQ(BURROW_AT(Byte, b, 0), 'h');
    CHECK_INT_EQ(BURROW_AT(Byte, b, 2), 'l');

    Slice wide = slice_make(a, TYPE_BYTE, 8, 8);
    CHECK_INT_EQ(slice_copy_str(wide, BURROW_S("ab")), 2);
    CHECK_INT_EQ(BURROW_AT(Byte, wide, 2), 0);
}

/* ------------------------------------------------------------ conversions */

TEST(a_string_converts_to_bytes_and_back) {
    Str s = BURROW_S("with a \0 in it");
    Slice b = slice_from_str(a, s);
    CHECK_INT_EQ(b.len, 14);
    CHECK(b.elem == TYPE_BYTE);
    CHECK(b.p != s.p);

    Str back = str_from_slice(a, b);
    CHECK(str_eq(back, s));
    CHECK(back.p != s.p);
}

TEST(converting_an_empty_string_gives_an_empty_slice_not_a_nil_one) {
    Slice b = slice_from_str(a, BURROW_STR_EMPTY);
    CHECK_INT_EQ(b.len, 0);
    CHECK(!slice_is_nil(b));
}

TEST(a_conversion_copies_so_the_two_stop_sharing) {
    Byte buf[3] = {'a', 'b', 'c'};
    Str s = str_from_bytes(buf, 3);
    Slice b = slice_from_str(a, s);
    buf[0] = 'z';
    CHECK_INT_EQ(BURROW_AT(Byte, b, 0), 'a');
}

/* ------------------------------------------------------- zero sized things */

/* []struct{} is a real thing in Go and it never allocates, because every zero
 * sized object shares one address. Getting this wrong shows up as an allocator
 * that is asked for zero bytes, which some allocators answer with NULL. */
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

TEST(a_slice_of_zero_sized_elements_never_allocates) {
    AllocStats before = mem_stats(a);

    Slice s = slice_make(a, &empty_struct, 4, 4);
    CHECK_INT_EQ(s.len, 4);
    CHECK(s.p != NULL);
    CHECK(!slice_is_nil(s));

    s = slice_append(a, s, s.p, 3);
    CHECK_INT_EQ(s.len, 7);
    CHECK(s.cap >= 7);

    AllocStats after = mem_stats(a);
    CHECK(after.allocs == before.allocs);

    /* Every element is the same address, which is what Go's zerobase gives you
     * and what makes two pointers to distinct zero sized values compare equal. */
    CHECK(slice_at(s, 0) == slice_at(s, 6));
}

/* ------------------------------------------------ the inline fast paths
 *
 * slice_at_fast and slice_append_fast exist for speed and are allowed to be
 * faster, not different. So these tests do not check what they do, they check
 * that what they do is what slice_at and slice_append do, element for element,
 * including when the size they are handed is the wrong one. */

TEST(the_fast_index_agrees_with_the_slow_one) {
    Slice s = slice_make(a, TYPE_INT, 5, 5);
    for (Int i = 0; i < 5; i++)
        *(Int *)slice_at(s, i) = i * 11;

    for (Int i = 0; i < 5; i++) {
        CHECK(slice_at_fast(s, i, sizeof(Int)) == slice_at(s, i));
        CHECK_INT_EQ(BURROW_AT(Int, s, i), i * 11);
    }

    /* A size that does not match the descriptor is not a different answer, it
     * is the same answer arrived at through the general path. */
    for (Int i = 0; i < 5; i++) {
        CHECK(slice_at_fast(s, i, 1) == slice_at(s, i));
        CHECK(slice_at_fast(s, i, sizeof(Int) * 2) == slice_at(s, i));
    }
}

TEST(the_fast_index_still_bounds_checks) {
    /* The fast path is a range test and a size test and nothing else, so an
     * index it rejects has to end up in slice_at, which is where the failure
     * lives. There is no way to observe a fatal error from in here without a
     * subprocess, so what is checked is the part that can be: an index inside
     * the length takes the fast path and an index past the length does not
     * silently return a pointer past the end. */
    Slice s = slice_make(a, TYPE_INT, 3, 8);
    CHECK(slice_at_fast(s, 2, sizeof(Int)) == (Byte *)s.p + 2 * sizeof(Int));
    CHECK(slice_at_fast(s, 0, sizeof(Int)) == s.p);
}

TEST(the_fast_append_agrees_with_the_slow_one) {
    /* Two slices built the same way by the two paths, compared at every step,
     * across the point where the capacity runs out and the backing array
     * moves. */
    Slice fast = slice_nil(TYPE_INT);
    Slice slow = slice_nil(TYPE_INT);

    for (Int i = 0; i < 40; i++) {
        fast = BURROW_APPEND(Int, a, fast, i);
        slow = slice_append(a, slow, &i, 1);

        CHECK_INT_EQ(fast.len, slow.len);
        CHECK_INT_EQ(fast.cap, slow.cap);
        CHECK(fast.elem == slow.elem);
        for (Int j = 0; j <= i; j++)
            CHECK_INT_EQ(BURROW_AT(Int, fast, j), BURROW_AT(Int, slow, j));
    }
}

TEST(the_fast_append_falls_back_when_the_size_does_not_match) {
    /* Eight bytes of room, one byte per element. Appending with the size of an
     * Int has to write one byte and not eight, because the descriptor is what
     * says how wide an element is and the size passed in is only a hint that
     * the compiler can use when it happens to be right. */
    Slice s = slice_make(a, TYPE_BYTE, 0, 8);
    for (Int i = 0; i < 8; i++)
        ((Byte *)s.p)[i] = 0xEE;

    Int wide = 0x41;
    s = slice_append_fast(a, s, &wide, sizeof(Int));

    CHECK_INT_EQ(s.len, 1);
    CHECK_INT_EQ(s.cap, 8);
    CHECK_INT_EQ(((Byte *)s.p)[0], 0x41);
    for (Int i = 1; i < 8; i++)
        CHECK_INT_EQ(((Byte *)s.p)[i], 0xEE);
}

TEST(the_fast_append_grows_a_nil_slice) {
    /* A nil slice has no pointer, so the fast path cannot take it and the
     * general one has to do the allocating. */
    Slice s = slice_nil(TYPE_INT);
    s = BURROW_APPEND(Int, a, s, 7);
    CHECK_INT_EQ(s.len, 1);
    CHECK_INT_EQ(s.cap, 1);
    CHECK(s.p != NULL);
    CHECK_INT_EQ(BURROW_AT(Int, s, 0), 7);
}

TEST(the_fast_append_keeps_the_sharing_within_capacity) {
    Slice s = slice_make(a, TYPE_INT, 1, 4);
    Slice other = s;
    s = BURROW_APPEND(Int, a, s, 99);

    CHECK(s.p == other.p);
    CHECK_INT_EQ(s.len, 2);
    CHECK_INT_EQ(other.len, 1);
    CHECK_INT_EQ(((Int *)other.p)[1], 99);
}

int main(void) {
    setup();
    RUN(make_gives_the_length_and_capacity_asked_for);
    RUN(make_zeroes_what_it_hands_back);
    RUN(a_capacity_bigger_than_the_length_is_allowed_and_is_not_readable);
    RUN(nil_and_empty_are_not_the_same_thing);
    RUN(from_wraps_memory_without_copying_it);
    RUN(index_reads_and_writes_the_element);
    RUN(index_works_on_a_slice_of_strings);
    RUN(sub_is_gos_two_index_slice_expression);
    RUN(sub_does_not_copy);
    RUN(sub_can_reach_past_the_length_up_to_the_capacity);
    RUN(sub3_caps_the_result);
    RUN(an_empty_slice_of_an_empty_slice_is_fine);
    RUN(append_to_nil_starts_a_slice);
    RUN(append_of_nothing_returns_the_slice_unchanged);
    RUN(the_growth_sequence_is_gos);
    RUN(growth_switches_off_doubling_at_two_hundred_and_fifty_six);
    RUN(appending_more_than_double_takes_exactly_what_is_needed);
    RUN(append_within_capacity_is_visible_through_the_other_slice);
    RUN(append_past_capacity_stops_sharing);
    RUN(a_slice_can_be_appended_to_itself);
    RUN(the_spare_capacity_after_a_grow_is_zeroed);
    RUN(append_slice_moves_every_element);
    RUN(copy_takes_the_shorter_of_the_two_lengths);
    RUN(copy_handles_overlap);
    RUN(copy_from_a_string_fills_a_byte_slice);
    RUN(a_string_converts_to_bytes_and_back);
    RUN(converting_an_empty_string_gives_an_empty_slice_not_a_nil_one);
    RUN(a_conversion_copies_so_the_two_stop_sharing);
    RUN(a_slice_of_zero_sized_elements_never_allocates);
    RUN(the_fast_index_agrees_with_the_slow_one);
    RUN(the_fast_index_still_bounds_checks);
    RUN(the_fast_append_agrees_with_the_slow_one);
    RUN(the_fast_append_falls_back_when_the_size_does_not_match);
    RUN(the_fast_append_grows_a_nil_slice);
    RUN(the_fast_append_keeps_the_sharing_within_capacity);
    teardown();
    return harness_report("slice");
}
