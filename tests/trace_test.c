/* Tests for the stack walker.
 *
 * A walker is awkward to test because the thing it reports is the test itself,
 * so most of what is here checks relationships rather than values: that adding
 * one to skip removes exactly one frame from the front and leaves the rest
 * untouched, that asking from three functions down reports three functions,
 * that a short buffer gets a short answer. Those hold whatever the compiler did
 * with the code around them, which a check against a particular address would
 * not.
 *
 * The helpers are marked as functions the compiler may not inline, and each one
 * does something with the result after the call, which is what stops the call
 * from becoming a jump that reuses the caller's frame. Both of those are about
 * the test rather than about burrow: the walker reports the frames that are
 * there, and a test that wants three frames has to make sure three frames
 * exist.
 *
 * Copyright 2026 The burrow Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style licence that can be found
 * in the LICENSE file. */

#include "burrow/trace.h"

#include "burrow/core.h"
#include "burrow/func.h"
#include "burrow/platform.h"
#include "burrow/proc.h"
#include "burrow/runtime.h"
#include "burrow/slice.h"
#include "burrow/thread.h"
#include "burrow/type.h"

#include "harness.h"

#include <stdbool.h>
#include <stddef.h>
#include <string.h>

/* Deeper than anything in this file and a long way short of the buffer a real
 * traceback uses, so that nothing here is measuring what happens when the
 * buffer fills up except the one test that means to. */
#define PCS_MAX 32

/* What the helpers write their result into, so that the value is used and the
 * call to the level below cannot turn into a tail call. */
static volatile Int sink;

static Slice pcs_slice(Uintptr *buf, Int n) {
    return slice_from(buf, n, n, TYPE_UINTPTR);
}

static BURROW_NOINLINE Int level_three(Int skip, Slice pcs) {
    Int n = runtime_callers(skip, pcs);

    sink = n;
    return n;
}

static BURROW_NOINLINE Int level_two(Int skip, Slice pcs) {
    Int n = level_three(skip, pcs);

    sink = n;
    return n;
}

static BURROW_NOINLINE Int level_one(Int skip, Slice pcs) {
    Int n = level_two(skip, pcs);

    sink = n;
    return n;
}

/* Every walk in this file goes through here, and the skip goes through a
 * volatile on the way.
 *
 * That is not about the walker, it is about what the compiler is allowed to do
 * to the helpers above. A skip that is a constant at the call site lets it make
 * a specialised copy of the three of them for each constant it sees, and gcc on
 * Windows does exactly that. Two copies are two sets of addresses, and half the
 * tests here compare the addresses from one walk against the addresses from
 * another. Through a volatile there is one copy and one set. */
static Int walk(Int skip, Uintptr *buf, Int max) {
    volatile Int s = skip;

    return level_one(s, pcs_slice(buf, max));
}

/* Whether this build can walk at all, which is the first thing every other test
 * here has to know. The architectures burrow claims are listed in
 * src/runtime/trace.c and the check below is the same list, so a build that
 * claims one of them and then reports nothing is a failure rather than a skip. */
static bool walks(void) {
    Uintptr buf[PCS_MAX];

    return walk(1, buf, PCS_MAX) > 0;
}

TEST(the_architectures_burrow_claims_can_walk) {
#if defined(BURROW_ARCH_AMD64) || defined(BURROW_ARCH_ARM64) || defined(BURROW_ARCH_386)
    CHECK(walks());
#else
    /* Nothing to check. The header says zero frames is a real answer and this
     * is a machine that gives it. */
    CHECK(true);
#endif
}

TEST(three_functions_deep_reports_three_functions) {
    Uintptr buf[PCS_MAX];
    Int n;

    if (!walks())
        return;

    n = walk(1, buf, PCS_MAX);

    /* The three helpers, the test function and whatever the harness and the C
     * runtime put under it. Four is the part this file is responsible for. */
    CHECK(n >= 4);

    /* Three different call sites, so three different addresses. A walker that
     * reported the same frame repeatedly would pass a count check. */
    CHECK(buf[0] != buf[1]);
    CHECK(buf[1] != buf[2]);
    CHECK(buf[0] != buf[2]);

    for (Int i = 0; i < n; i++)
        CHECK(buf[i] != 0);
}

TEST(skip_takes_one_frame_off_the_front) {
    Uintptr one[PCS_MAX];
    Uintptr two[PCS_MAX];
    Int n1;
    Int n2;

    if (!walks())
        return;

    n1 = walk(1, one, PCS_MAX);
    n2 = walk(2, two, PCS_MAX);

    CHECK_INT_EQ(n2, n1 - 1);

    /* The two calls are on different lines, so the frame for this function
     * differs between them. Everything between there and the top is the same
     * pair of helpers called from the same places, shifted by one. */
    CHECK(one[1] == two[0]);
    CHECK(one[2] == two[1]);
}

TEST(skip_zero_is_the_frame_for_callers_itself) {
    Uintptr zero[PCS_MAX];
    Uintptr one[PCS_MAX];
    Int n0;
    Int n1;

    if (!walks())
        return;

    n0 = walk(0, zero, PCS_MAX);
    n1 = walk(1, one, PCS_MAX);

    /* Go numbers runtime.Callers itself zero and burrow numbers the same way,
     * so asking with zero gives one more entry than asking with one, and the
     * rest of the two traces line up. */
    CHECK_INT_EQ(n0, n1 + 1);
    CHECK(zero[0] != 0);
    CHECK(zero[1] == one[0]);
    CHECK(zero[2] == one[1]);

    /* A negative skip is not a way to ask for more than everything. */
    CHECK_INT_EQ(walk(-4, zero, PCS_MAX), n0);
}

TEST(a_short_buffer_gets_a_short_answer) {
    Uintptr one[1];
    Uintptr none[1];

    if (!walks())
        return;

    CHECK_INT_EQ(walk(1, one, 1), 1);
    CHECK(one[0] != 0);

    /* And the two ways of asking for nothing. Neither is an error and neither
     * writes anywhere. */
    CHECK_INT_EQ(walk(1, none, 0), 0);
    CHECK_INT_EQ(level_one(1, slice_nil(TYPE_UINTPTR)), 0);
}

TEST(skipping_past_the_bottom_of_the_stack_is_not_a_fault) {
    Uintptr buf[PCS_MAX];

    CHECK_INT_EQ(walk(1000000, buf, PCS_MAX), 0);
}

/* The same walk through the internal entry point, which is what the panic
 * printer uses and what burrow__traceback prints.
 *
 * The two are asked to start in the same place, which takes different numbers
 * because they count differently. The internal one reports the callers of
 * whoever passed the frame address, so skip zero is this function's caller. The
 * public one numbers itself zero the way Go does, so one is public_walk and two
 * is public_walk's caller. */
static BURROW_NOINLINE Int internal_walk(Uintptr *buf, Int max) {
    Int n = burrow__callers(BURROW_WALK_FROM, 0, buf, max);

    sink = n;
    return n;
}

static BURROW_NOINLINE Int public_walk(Uintptr *buf, Int max) {
    Int n = runtime_callers(2, pcs_slice(buf, max));

    sink = n;
    return n;
}

TEST(the_internal_walk_and_the_public_one_see_the_same_stack) {
    Uintptr inner[PCS_MAX];
    Uintptr outer[PCS_MAX];
    Int ni;
    Int np;

    if (!walks())
        return;

    ni = internal_walk(inner, PCS_MAX);
    np = public_walk(outer, PCS_MAX);

    CHECK_INT_EQ(ni, np);

    /* Both traces start in this function and differ only in the line the call
     * was on, so everything under that first frame is the same. */
    CHECK(ni >= 2 && np >= 2);
    if (ni >= 2 && np >= 2)
        CHECK(memcmp(inner + 1, outer + 1, (size_t)(ni - 1) * sizeof inner[0]) == 0);
}

/* A walk on a goroutine, which is the case the bounds checking exists for: the
 * frames are on a stack burrow mapped rather than on the one the operating
 * system gave the thread, and the walker has to notice. */
static Int goroutine_frames;

static void goroutine_body(void *unused) {
    Uintptr buf[PCS_MAX];

    (void)unused;
    goroutine_frames = walk(1, buf, PCS_MAX);
}

TEST(a_goroutine_stack_walks_too) {
    if (!walks())
        return;

    goroutine_frames = -1;
    runtime_main(BURROW_FN(Func, goroutine_body, NULL));

    /* The three helpers and the body, and then the bottom of the goroutine
     * stack, which is where the walk stops because there is nothing under it. */
    CHECK(goroutine_frames >= 4);
}

/* And a walk on a plain operating system thread that the scheduler has never
 * heard of, where the bounds come from the thread rather than from burrow. */
static Int thread_frames;

static void thread_body(void *unused) {
    Uintptr buf[PCS_MAX];

    (void)unused;
    thread_frames = walk(1, buf, PCS_MAX);
}

TEST(a_plain_thread_walks_too) {
    burrow__Thread t;

    if (!walks())
        return;

    thread_frames = -1;
    CHECK(burrow__thread_start(&t, thread_body, NULL, 0));
    CHECK(burrow__thread_join(&t));
    CHECK(thread_frames >= 4);
}

TEST(a_thread_knows_where_its_own_stack_is) {
    void *lo = NULL;
    void *hi = NULL;
    char here;

    if (!burrow__thread_stack_bounds(&lo, &hi)) {
        /* Allowed, and it is what the walker checks for. Nothing else in this
         * test means anything if the system will not say. */
        CHECK(lo == NULL);
        return;
    }

    CHECK(lo != NULL);
    CHECK(hi != NULL);
    CHECK((char *)lo < (char *)hi);

    /* This frame is on that stack, which is the whole property. */
    CHECK(&here >= (char *)lo);
    CHECK(&here < (char *)hi);
}

int main(void) {
    RUN(the_architectures_burrow_claims_can_walk);
    RUN(three_functions_deep_reports_three_functions);
    RUN(skip_takes_one_frame_off_the_front);
    RUN(skip_zero_is_the_frame_for_callers_itself);
    RUN(a_short_buffer_gets_a_short_answer);
    RUN(skipping_past_the_bottom_of_the_stack_is_not_a_fault);
    RUN(the_internal_walk_and_the_public_one_see_the_same_stack);
    RUN(a_goroutine_stack_walks_too);
    RUN(a_plain_thread_walks_too);
    RUN(a_thread_knows_where_its_own_stack_is);
    return harness_report("trace");
}
