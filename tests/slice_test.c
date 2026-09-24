/* Copyright 2026 The burrow Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style licence that can be found
 * in the LICENSE file. */

#include "burrow/core.h"
#include "burrow/mem/arena.h"
#include "burrow/slice.h"
#include "burrow/type.h"

#include "burrow/panic.h"
#include "burrow/runtime.h"

#include "check.h"
#include "fatal.h"

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

static void TestMakeGivesTheLengthAndCapacityAskedFor(TestingT *t) {
    Slice s = slice_make(a, TYPE_INT, 3, 10);
    CHECK_INT_EQ(s.len, 3);
    CHECK_INT_EQ(s.cap, 10);
    CHECK(s.elem == TYPE_INT);
    CHECK(s.p != NULL);
}

/* Go's zero value rule is the language and not a convention, so make gives back
 * zeroes and code ported from Go is allowed to rely on it without asking. */
static void TestMakeZeroesWhatItHandsBack(TestingT *t) {
    Slice s = slice_make(a, TYPE_INT, 64, 64);
    for (Int i = 0; i < s.len; i++)
        CHECK_INT_EQ(BURROW_AT(Int, s, i), 0);
}

static void TestACapacityBiggerThanTheLengthIsAllowedAndIsNotReadable(TestingT *t) {
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
static void TestNilAndEmptyAreNotTheSameThing(TestingT *t) {
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

static void TestFromWrapsMemoryWithoutCopyingIt(TestingT *t) {
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

static void TestIndexReadsAndWritesTheElement(TestingT *t) {
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

static void TestIndexWorksOnASliceOfStrings(TestingT *t) {
    Slice s = slice_make(a, TYPE_STRING, 2, 2);
    BURROW_AT(Str, s, 0) = BURROW_S("alpha");
    BURROW_AT(Str, s, 1) = BURROW_S("beta");
    CHECK(str_is(BURROW_AT(Str, s, 0), "alpha"));
    CHECK(str_is(BURROW_AT(Str, s, 1), "beta"));
}

/* ------------------------------------------------------------- reslicing */

static void TestSubIsGosTwoIndexSliceExpression(TestingT *t) {
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

static void TestSubDoesNotCopy(TestingT *t) {
    Slice s = slice_make(a, TYPE_INT, 8, 8);
    Slice m = slice_sub(s, 4, 8);
    BURROW_AT(Int, m, 0) = 1234;
    CHECK_INT_EQ(BURROW_AT(Int, s, 4), 1234);
}

static void TestSubCanReachPastTheLengthUpToTheCapacity(TestingT *t) {
    Slice s = slice_make(a, TYPE_INT, 2, 8);
    /* Bounds are checked against cap and not len, which is exactly what Go
     * does and is what makes s = s[:cap(s)] legal. */
    Slice g = slice_sub(s, 0, 8);
    CHECK_INT_EQ(g.len, 8);
    CHECK_INT_EQ(g.cap, 8);
}

static void TestSub3CapsTheResult(TestingT *t) {
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

static void TestAnEmptySliceOfAnEmptySliceIsFine(TestingT *t) {
    Slice n = slice_nil(TYPE_INT);
    Slice m = slice_sub(n, 0, 0);
    CHECK_INT_EQ(m.len, 0);
    CHECK_INT_EQ(m.cap, 0);
    /* A nil pointer stays a nil pointer rather than becoming p + 0, which would
     * be undefined even though every compiler does the obvious thing. */
    CHECK(m.p == NULL);
}

/* ----------------------------------------------------------------- append */

static void TestAppendToNilStartsASlice(TestingT *t) {
    Slice s = slice_nil(TYPE_INT);
    s = BURROW_APPEND(Int, a, s, 42);
    CHECK_INT_EQ(s.len, 1);
    CHECK_INT_EQ(s.cap, 1);
    CHECK_INT_EQ(BURROW_AT(Int, s, 0), 42);
    CHECK(s.elem == TYPE_INT);
}

static void TestAppendOfNothingReturnsTheSliceUnchanged(TestingT *t) {
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
static void TestTheGrowthSequenceIsGos(TestingT *t) {
    Slice s = slice_nil(TYPE_INT);
    const Int want[] = {1, 2, 4, 4, 8, 8, 8, 8, 16, 16, 16, 16, 16, 16, 16, 16};
    for (Int i = 0; i < 16; i++) {
        s = BURROW_APPEND(Int, a, s, i);
        CHECK_INT_EQ(s.cap, want[i]);
    }
    for (Int i = 0; i < 16; i++)
        CHECK_INT_EQ(BURROW_AT(Int, s, i), i);
}

static void TestGrowthSwitchesOffDoublingAtTwoHundredAndFiftySix(TestingT *t) {
    Slice s = slice_make(a, TYPE_INT, 256, 256);

    /* Below the threshold it would have doubled to 512, and here it does too,
     * because the formula's first step from 256 lands exactly on 512. */
    s = BURROW_APPEND(Int, a, s, 0);
    CHECK_INT_EQ(s.len, 257);
    CHECK_INT_EQ(s.cap, 512);

    /* The next one is where the two rules visibly part company: doubling would
     * give 1024 and the formula gives 832. */
    Slice sl = slice_make(a, TYPE_INT, 512, 512);
    sl = BURROW_APPEND(Int, a, sl, 0);
    CHECK_INT_EQ(sl.cap, 832);

    Slice u = slice_make(a, TYPE_INT, 832, 832);
    u = BURROW_APPEND(Int, a, u, 0);
    CHECK_INT_EQ(u.cap, 1232);
}

static void TestAppendingMoreThanDoubleTakesExactlyWhatIsNeeded(TestingT *t) {
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
static void TestAppendWithinCapacityIsVisibleThroughTheOtherSlice(TestingT *t) {
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

static void TestAppendPastCapacityStopsSharing(TestingT *t) {
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
static void TestASliceCanBeAppendedToItself(TestingT *t) {
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
static void TestTheSpareCapacityAfterAGrowIsZeroed(TestingT *t) {
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

static void TestAppendSliceMovesEveryElement(TestingT *t) {
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

static void TestCopyTakesTheShorterOfTheTwoLengths(TestingT *t) {
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
static void TestCopyHandlesOverlap(TestingT *t) {
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

static void TestCopyFromAStringFillsAByteSlice(TestingT *t) {
    Slice b = slice_make(a, TYPE_BYTE, 3, 3);
    CHECK_INT_EQ(slice_copy_str(b, BURROW_S("hello")), 3);
    CHECK_INT_EQ(BURROW_AT(Byte, b, 0), 'h');
    CHECK_INT_EQ(BURROW_AT(Byte, b, 2), 'l');

    Slice wide = slice_make(a, TYPE_BYTE, 8, 8);
    CHECK_INT_EQ(slice_copy_str(wide, BURROW_S("ab")), 2);
    CHECK_INT_EQ(BURROW_AT(Byte, wide, 2), 0);
}

/* ------------------------------------------------------------ conversions */

static void TestAStringConvertsToBytesAndBack(TestingT *t) {
    Str s = BURROW_S("with a \0 in it");
    Slice b = slice_from_str(a, s);
    CHECK_INT_EQ(b.len, 14);
    CHECK(b.elem == TYPE_BYTE);
    CHECK(b.p != s.p);

    Str back = str_from_slice(a, b);
    CHECK(str_eq(back, s));
    CHECK(back.p != s.p);
}

static void TestConvertingAnEmptyStringGivesAnEmptySliceNotANilOne(TestingT *t) {
    Slice b = slice_from_str(a, BURROW_STR_EMPTY);
    CHECK_INT_EQ(b.len, 0);
    CHECK(!slice_is_nil(b));
}

static void TestAConversionCopiesSoTheTwoStopSharing(TestingT *t) {
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

static void TestASliceOfZeroSizedElementsNeverAllocates(TestingT *t) {
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

static void TestTheFastIndexAgreesWithTheSlowOne(TestingT *t) {
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

/* ------------------------------------------------------------ the checks
 *
 * Every one of these is a panic rather than a fatal error, because Go's version
 * of each is something recover can catch. File static because a local live
 * across the setjmp inside these macros is a local gcc warns about. */

static Slice checked;

static void TestIndexingPastTheEndPanics(TestingT *t) {
    checked = slice_make(a, TYPE_INT, 3, 8);

    CHECK_RUNTIME_ERROR((void)slice_at(checked, 3),
                        "runtime error: index out of range [3] with length 3");

    /* Against the length and not the capacity, which is what s[i] checks in
     * Go. The spare capacity is reslicing's business. */
    CHECK_RUNTIME_ERROR((void)slice_at(checked, 7),
                        "runtime error: index out of range [7] with length 3");

    CHECK_RUNTIME_ERROR((void)slice_at(checked, -1),
                        "runtime error: index out of range [-1] with length 3");
}

static void TestASliceExpressionPastTheCapacityPanics(TestingT *t) {
    checked = slice_make(a, TYPE_INT, 3, 8);

    CHECK_RUNTIME_ERROR(
        (void)slice_sub(checked, 0, 9),
        "runtime error: slice bounds out of range [0:9] with capacity 8");

    CHECK_RUNTIME_ERROR(
        (void)slice_sub(checked, 5, 2),
        "runtime error: slice bounds out of range [5:2] with capacity 8");

    CHECK_RUNTIME_ERROR(
        (void)slice_sub(checked, -1, 2),
        "runtime error: slice bounds out of range [-1:2] with capacity 8");
}

static void TestAnImpossibleLengthPanics(TestingT *t) {
    CHECK_RUNTIME_ERROR((void)slice_make(a, TYPE_INT, -1, 0),
                        "runtime error: makeslice: len out of range");

    CHECK_RUNTIME_ERROR((void)slice_make(a, TYPE_INT, 4, 2),
                        "runtime error: makeslice: cap out of range");

    /* Go reports the length first when the length itself is impossible and the
     * capacity otherwise, and the two messages are different because people
     * search for them. */
    CHECK_RUNTIME_ERROR((void)slice_from(NULL, -1, 0, TYPE_INT),
                        "runtime error: slice_from: len out of range");
}

static void TestTheFastIndexStillBoundsChecks(TestingT *t) {
    /* The fast path is a range test and a size test and nothing else, so an
     * index it rejects has to end up in slice_at, which is where the failure
     * lives. Both halves are checked: an index inside the length takes the fast
     * path, and one past it comes out of slice_at with Go's message rather than
     * a pointer past the end. */
    checked = slice_make(a, TYPE_INT, 3, 8);
    CHECK(slice_at_fast(checked, 2, sizeof(Int)) ==
          (Byte *)checked.p + 2 * sizeof(Int));
    CHECK(slice_at_fast(checked, 0, sizeof(Int)) == checked.p);

    CHECK_RUNTIME_ERROR((void)slice_at_fast(checked, 3, sizeof(Int)),
                        "runtime error: index out of range [3] with length 3");

    /* And an element size that does not match the descriptor falls off the fast
     * path into the same check. */
    CHECK_RUNTIME_ERROR((void)slice_at_fast(checked, 3, 1),
                        "runtime error: index out of range [3] with length 3");
}

static void TestTheFastAppendAgreesWithTheSlowOne(TestingT *t) {
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

static void TestTheFastAppendFallsBackWhenTheSizeDoesNotMatch(TestingT *t) {
    /* Eight bytes of room, one byte per element. Appending with the size of an
     * Int has to write one byte and not eight, because the descriptor is what
     * says how wide an element is and the size passed in is only a hint that
     * the compiler can use when it happens to be right. */
    Slice s = slice_make(a, TYPE_BYTE, 0, 8);
    for (Int i = 0; i < 8; i++)
        ((Byte *)s.p)[i] = 0xEE;

    /* The source is a byte array rather than an Int, so that the first byte of
     * it is the same byte on every machine. An Int holding 0x41 begins with
     * 0x41 on a little endian machine and with 0x00 on a big endian one, and a
     * test that reads the first byte of one is asserting on the byte order
     * instead of on the append. It is still sizeof(Int) bytes wide, so a
     * version of slice_append_fast that believed the hint would copy the 0xAA
     * filler into the slice and the loop below would catch it. */
    Byte wide[sizeof(Int)];
    for (size_t i = 0; i < sizeof wide; i++)
        wide[i] = 0xAA;
    wide[0] = 0x41;

    s = slice_append_fast(a, s, wide, sizeof(Int));

    CHECK_INT_EQ(s.len, 1);
    CHECK_INT_EQ(s.cap, 8);
    CHECK_INT_EQ(((Byte *)s.p)[0], 0x41);
    for (Int i = 1; i < 8; i++)
        CHECK_INT_EQ(((Byte *)s.p)[i], 0xEE);
}

static void TestTheFastAppendGrowsANilSlice(TestingT *t) {
    /* A nil slice has no pointer, so the fast path cannot take it and the
     * general one has to do the allocating. */
    Slice s = slice_nil(TYPE_INT);
    s = BURROW_APPEND(Int, a, s, 7);
    CHECK_INT_EQ(s.len, 1);
    CHECK_INT_EQ(s.cap, 1);
    CHECK(s.p != NULL);
    CHECK_INT_EQ(BURROW_AT(Int, s, 0), 7);
}

static void TestTheFastAppendKeepsTheSharingWithinCapacity(TestingT *t) {
    Slice s = slice_make(a, TYPE_INT, 1, 4);
    Slice other = s;
    s = BURROW_APPEND(Int, a, s, 99);

    CHECK(s.p == other.p);
    CHECK_INT_EQ(s.len, 2);
    CHECK_INT_EQ(other.len, 1);
    CHECK_INT_EQ(((Int *)other.p)[1], 99);
}

#define TESTS(X)                                                                       \
    X(TestMakeGivesTheLengthAndCapacityAskedFor)                                       \
    X(TestMakeZeroesWhatItHandsBack)                                                   \
    X(TestACapacityBiggerThanTheLengthIsAllowedAndIsNotReadable)                       \
    X(TestNilAndEmptyAreNotTheSameThing)                                               \
    X(TestFromWrapsMemoryWithoutCopyingIt)                                             \
    X(TestIndexReadsAndWritesTheElement)                                               \
    X(TestIndexWorksOnASliceOfStrings)                                                 \
    X(TestSubIsGosTwoIndexSliceExpression)                                             \
    X(TestSubDoesNotCopy)                                                              \
    X(TestSubCanReachPastTheLengthUpToTheCapacity)                                     \
    X(TestSub3CapsTheResult)                                                           \
    X(TestAnEmptySliceOfAnEmptySliceIsFine)                                            \
    X(TestAppendToNilStartsASlice)                                                     \
    X(TestAppendOfNothingReturnsTheSliceUnchanged)                                     \
    X(TestTheGrowthSequenceIsGos)                                                      \
    X(TestGrowthSwitchesOffDoublingAtTwoHundredAndFiftySix)                            \
    X(TestAppendingMoreThanDoubleTakesExactlyWhatIsNeeded)                             \
    X(TestAppendWithinCapacityIsVisibleThroughTheOtherSlice)                           \
    X(TestAppendPastCapacityStopsSharing)                                              \
    X(TestASliceCanBeAppendedToItself)                                                 \
    X(TestTheSpareCapacityAfterAGrowIsZeroed)                                          \
    X(TestAppendSliceMovesEveryElement)                                                \
    X(TestCopyTakesTheShorterOfTheTwoLengths)                                          \
    X(TestCopyHandlesOverlap)                                                          \
    X(TestCopyFromAStringFillsAByteSlice)                                              \
    X(TestAStringConvertsToBytesAndBack)                                               \
    X(TestConvertingAnEmptyStringGivesAnEmptySliceNotANilOne)                          \
    X(TestAConversionCopiesSoTheTwoStopSharing)                                        \
    X(TestASliceOfZeroSizedElementsNeverAllocates)                                     \
    X(TestTheFastIndexAgreesWithTheSlowOne)                                            \
    X(TestIndexingPastTheEndPanics)                                                    \
    X(TestASliceExpressionPastTheCapacityPanics)                                       \
    X(TestAnImpossibleLengthPanics)                                                    \
    X(TestTheFastIndexStillBoundsChecks)                                               \
    X(TestTheFastAppendAgreesWithTheSlowOne)                                           \
    X(TestTheFastAppendFallsBackWhenTheSizeDoesNotMatch)                               \
    X(TestTheFastAppendGrowsANilSlice)                                                 \
    X(TestTheFastAppendKeepsTheSharingWithinCapacity)

static int TestMain(TestingM *m) {
    setup();
    int code = testing_m_run(m);
    teardown();
    return code;
}

TESTING_MAIN_WITH(TestMain, TESTS)
