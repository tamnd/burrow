/* Tests for turning an address back into a name.
 *
 * The table is built out of the objects this test is linked against, so the
 * names in it are burrow's own and the test can ask about functions it can name
 * itself. That is the whole trick here: runtime_callers is a real function in
 * the library, &runtime_callers is its address, and a lookup that does not come
 * back with "runtime_callers" is wrong in a way no relationship test would
 * catch.
 *
 * A test binary built with SYMTAB=0 has an empty table, and every test below
 * that needs a name says so and steps aside rather than failing. An empty table
 * is a build somebody asked for, not a bug.
 *
 * Copyright 2026 The burrow Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style licence that can be found
 * in the LICENSE file. */

#include "burrow/symtab.h"

#include "burrow/core.h"
#include "burrow/runtime.h"
#include "burrow/slice.h"
#include "burrow/trace.h"

#include "check.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* Whether there is a table to ask about at all. */
static bool have_table(void) {
    return burrow__symtab_len() > 0;
}

/* The address of a function this file can name and the table certainly has,
 * because it is a global function in the library the test just linked. */
static Uintptr known_entry(void) {
    return (Uintptr)runtime_callers;
}

static void TestABuildWithATableHasNamesInIt(TestingT *t) {
    /* Not a fixed number, because the count is however many global functions
     * burrow has today and that goes up every week. What is worth asserting is
     * that a default build has a table rather than the empty one. */
    Int n = burrow__symtab_len();
    CHECK(n >= 0);
    if (!have_table())
        return;
    CHECK(n > 100);
}

static void TestAFunctionResolvesToItsOwnName(TestingT *t) {
    if (!have_table())
        return;

    burrow__Frame f;
    CHECK(burrow__symbolise(known_entry(), &f));
    CHECK(str_eq(f.name, BURROW_S("runtime_callers")));
    CHECK(f.entry == known_entry());
}

static void TestAnAddressInsideAFunctionResolvesToThatFunction(TestingT *t) {
    if (!have_table())
        return;

    /* Four bytes in, which is inside the first instruction on arm64 and inside
     * the first two on amd64, so it is a real address in the function on every
     * machine this runs on. */
    burrow__Frame f;
    CHECK(burrow__symbolise(known_entry() + 4, &f));
    CHECK(str_eq(f.name, BURROW_S("runtime_callers")));
    CHECK(f.entry == known_entry());
}

static void TestTheEntryIsNeverAboveTheAddressAskedAbout(TestingT *t) {
    if (!have_table())
        return;

    for (Int off = 0; off < 32; off += 4) {
        burrow__Frame f;
        if (!burrow__symbolise(known_entry() + (Uintptr)off, &f))
            continue;
        CHECK(f.entry <= known_entry() + (Uintptr)off);
        CHECK(f.name.len > 0);
        CHECK(f.name.p != NULL);
    }
}

static void TestAnAddressNowhereNearTheLibraryHasNoName(TestingT *t) {
    burrow__Frame f;

    /* A quarter of a megabyte is the furthest a hit is allowed to be from the
     * name it got, so this is well past the end of anything the table covers
     * and is not a valid address on any of the platforms this builds for. */
    CHECK(!burrow__symbolise(UINTPTR_MAX - 4096, &f));
    CHECK(f.name.len == 0);
    CHECK(f.entry == 0);
}

static void TestALowAddressHasNoName(TestingT *t) {
    burrow__Frame f;

    /* Below every mapping on every system burrow runs on, so there is nothing
     * at or under it for the search to land on. */
    CHECK(!burrow__symbolise(4096, &f));
    CHECK(f.name.len == 0);
}

static void TestZeroIsNotAnAddress(TestingT *t) {
    burrow__Frame f;

    CHECK(!burrow__symbolise(0, &f));
    CHECK(f.name.len == 0);
    CHECK(f.entry == 0);
}

static void TestAskingWithNowhereToPutTheAnswerSaysNo(TestingT *t) {
    /* The panic path calls this, so refusing beats writing through a null
     * pointer while the program is already on its way out. */
    CHECK(!burrow__symbolise(known_entry(), NULL));
}

static void TestTwoDifferentFunctionsGetTwoDifferentNames(TestingT *t) {
    if (!have_table())
        return;

    burrow__Frame a;
    burrow__Frame b;

    CHECK(burrow__symbolise((Uintptr)runtime_callers, &a));
    CHECK(burrow__symbolise((Uintptr)burrow__symtab_len, &b));
    CHECK(str_eq(a.name, BURROW_S("runtime_callers")));
    CHECK(str_eq(b.name, BURROW_S("burrow__symtab_len")));
    CHECK(a.entry != b.entry);
}

static void TestTheSameQuestionTwiceGetsTheSameAnswer(TestingT *t) {
    if (!have_table())
        return;

    /* The first call sorts the table and the second one does not, and the two
     * paths through the lookup have to agree. */
    burrow__Frame a;
    burrow__Frame b;

    CHECK(burrow__symbolise(known_entry(), &a));
    CHECK(burrow__symbolise(known_entry(), &b));
    CHECK(str_eq(a.name, b.name));
    CHECK(a.entry == b.entry);
}

/* Deep enough that the walk has something to find and shallow enough that every
 * frame is one of this file's. */
static Uintptr collected[8];

BURROW_NOINLINE static Int collect(void) {
    Slice s = {collected, 8, 8, NULL};
    return runtime_callers(0, s);
}

static void TestTheFramesAWalkCollectsHaveNames(TestingT *t) {
    if (!have_table())
        return;

    Int n = collect();

    /* Zero is a real answer on an architecture with no walk, and there is a
     * test for that in trace_test.c. Here it means there is nothing to
     * symbolise, which is not this file's business. */
    if (n <= 0)
        return;

    /* Frame zero is runtime_callers itself, which it writes in by hand, and it
     * writes one past the entry so that the subtraction below lands inside.
     * That makes it the one frame in any trace whose name is known in advance,
     * which is what makes it worth checking. */
    burrow__Frame f;
    CHECK(burrow__symbolise(collected[0] - 1, &f));
    CHECK(str_eq(f.name, BURROW_S("runtime_callers")));

    /* The rest are return addresses into this file and into the harness, and
     * what matters is that a walk of real frames produces names rather than a
     * column of nothing. Not all of them: a static function is not in the table
     * and this file is full of them. */
    Int named = 0;
    for (Int i = 0; i < n; i++) {
        if (collected[i] != 0 && burrow__symbolise(collected[i] - 1, &f))
            named++;
    }
    CHECK(named > 0);
}

#define TESTS(X)                                                                       \
    X(TestABuildWithATableHasNamesInIt)                                                \
    X(TestAFunctionResolvesToItsOwnName)                                               \
    X(TestAnAddressInsideAFunctionResolvesToThatFunction)                              \
    X(TestTheEntryIsNeverAboveTheAddressAskedAbout)                                    \
    X(TestAnAddressNowhereNearTheLibraryHasNoName)                                     \
    X(TestALowAddressHasNoName)                                                        \
    X(TestZeroIsNotAnAddress)                                                          \
    X(TestAskingWithNowhereToPutTheAnswerSaysNo)                                       \
    X(TestTwoDifferentFunctionsGetTwoDifferentNames)                                   \
    X(TestTheSameQuestionTwiceGetsTheSameAnswer)                                       \
    X(TestTheFramesAWalkCollectsHaveNames)

TESTING_MAIN(TESTS)
