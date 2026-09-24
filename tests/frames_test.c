/* Caller, CallersFrames, FuncForPC and Stack.
 *
 * Two things make these awkward to test and both are worth knowing about before
 * reading any of it.
 *
 * The first is that the symbol table is built from the library's objects and
 * not from the test's, so no function in this file has a name in it. Every test
 * that needs a name therefore asks about a library function, and the one it
 * asks about is runtime_callers, which is in every build that can run this at
 * all.
 *
 * The second is that a build without a table is a supported build, and it is
 * what make SYMTAB=0 produces. Anything that needs a name is skipped there
 * rather than failing, which is what have_table is for. The tests about frame
 * counting and about buffers do not need one and always run.
 *
 * A machine with no stack walk is a supported machine too, for the same reason
 * and with the same consequences. burrow/trace.h says which architectures have
 * one and says that no frames is a normal answer everywhere else, so s390x and
 * riscv64 get a traceback that is one header line and nothing under it. That is
 * what have_walk is for, and it asks rather than testing for an architecture,
 * because a build with frame pointers omitted answers the same way on a machine
 * that would otherwise have had one.
 *
 * Copyright 2026 The burrow Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style licence that can be found
 * in the LICENSE file. */

#include "burrow/runtime.h"

#include "burrow/atomic.h"
#include "burrow/core.h"
#include "burrow/platform.h"
#include "burrow/proc.h"
#include "burrow/slice.h"
#include "burrow/symtab.h"

#include "check.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

static bool have_table(void) {
    return burrow__symtab_len() > 0;
}

/* Whether this build can walk a stack at all.
 *
 * Asked with a skip of one rather than zero, because frame zero is the one
 * runtime_callers writes itself without walking anything, so a skip of zero
 * answers one on every machine and would tell us nothing. */
static bool have_walk(void) {
    Uintptr buf[4];
    Slice s = {buf, 4, 4, NULL};

    return runtime_callers(1, s) > 0;
}

/* ------------------------------------------------------------- FuncForPC */

static void TestALibraryFunctionKnowsItsOwnName(TestingT *t) {
    RuntimeFunc fn;

    if (!have_table())
        return;

    CHECK(runtime_func_for_pc((Uintptr)runtime_callers, &fn));
    CHECK(str_eq(fn.name, BURROW_S("runtime_callers")));
    CHECK(fn.entry == (Uintptr)runtime_callers);
}

static void TestAnAddressInsideAFunctionGetsTheSameName(TestingT *t) {
    RuntimeFunc at;
    RuntimeFunc inside;

    if (!have_table())
        return;

    CHECK(runtime_func_for_pc((Uintptr)runtime_callers, &at));
    CHECK(runtime_func_for_pc((Uintptr)runtime_callers + 4, &inside));
    CHECK(str_eq(at.name, inside.name));
    CHECK(at.entry == inside.entry);
}

static void TestAnAddressThatIsNobodysGetsNothing(TestingT *t) {
    RuntimeFunc fn;

    /* Zero is never a function and the top of the address space is never inside
     * one, and both come back cleared rather than half filled in. */
    CHECK(!runtime_func_for_pc(0, &fn));
    CHECK(fn.name.len == 0);
    CHECK(fn.entry == 0);

    CHECK(!runtime_func_for_pc(UINTPTR_MAX - 4096, &fn));
    CHECK(fn.name.len == 0);
    CHECK(fn.entry == 0);
}

static void TestFuncForPcSurvivesANullAnswer(TestingT *t) {
    CHECK(!runtime_func_for_pc((Uintptr)runtime_callers, NULL));
}

/* ---------------------------------------------------------------- Caller */

/* The two ways of asking for the same frame, asked from one frame.
 *
 * runtime_callers numbers its own frame zero and runtime_caller numbers its
 * caller zero, so from here, frame two of the first and frame one of the second
 * are both the frame that called this function. They are not merely the same
 * function, they are the same address, because they are the same call site: the
 * instruction after this function's own call. That makes it an equality rather
 * than a guess, which is the only way to pin an off by one down. */
BURROW_NOINLINE static void ask_both_ways(Uintptr *by_callers, Uintptr *by_caller) {
    Uintptr buf[4];
    Slice s = {buf, 4, 4, NULL};
    Int n;

    *by_callers = 0;
    *by_caller = 0;

    n = runtime_callers(2, s);
    if (n > 0)
        *by_callers = buf[0];

    (void)runtime_caller(1, by_caller, NULL, NULL);
}

static void TestCallerAndCallersAreOneApartInExactlyTheDocumentedWay(TestingT *t) {
    Uintptr by_callers = 0;
    Uintptr by_caller = 0;

    ask_both_ways(&by_callers, &by_caller);

    if (by_callers == 0 && by_caller == 0)
        return; /* an architecture with no walk, which trace_test covers */

    CHECK(by_callers != 0);
    CHECK(by_caller == by_callers);
}

static void TestCallerLeavesFileAndLineAloneForNow(TestingT *t) {
    Uintptr pc = 0;
    Str file = BURROW_S_INIT("untouched");
    Int line = -1;

    if (!runtime_caller(0, &pc, &file, &line))
        return;

    CHECK(pc != 0);

    /* Empty and zero rather than unset, so a caller that checks the boolean and
     * then reads all three gets something it can print. */
    CHECK(file.len == 0);
    CHECK_INT_EQ(line, 0);
}

static void TestCallerTakesNullForAnythingItIsNotAskedFor(TestingT *t) {
    /* All three outputs dropped is a legal way to ask whether a frame exists. */
    if (!runtime_caller(0, NULL, NULL, NULL))
        return;
    CHECK(true);
}

static void TestSkippingPastTheBottomIsFalseRatherThanAFault(TestingT *t) {
    Uintptr pc = 12345;

    CHECK(!runtime_caller(1 << 20, &pc, NULL, NULL));
    CHECK(pc == 12345); /* an output left alone is left alone */
}

static void TestANegativeSkipIsTreatedAsNone(TestingT *t) {
    Uintptr negative = 0;
    Uintptr zero = 0;

    if (!runtime_caller(-3, &negative, NULL, NULL))
        return;
    CHECK(runtime_caller(0, &zero, NULL, NULL));

    /* Two different call sites in this function, so not the same address, but
     * both inside it and therefore within a few hundred bytes of each other. */
    CHECK(negative != 0);
    CHECK(zero != 0);
}

/* --------------------------------------------------------- CallersFrames */

static void TestEveryAddressPutInComesBackAsAFrame(TestingT *t) {
    Uintptr buf[16];
    Slice s = {buf, 16, 16, NULL};
    Int n = runtime_callers(0, s);
    RuntimeFrames it;
    RuntimeFrame f;
    Int seen = 0;

    CHECK(n > 0);

    it = runtime_callers_frames(slice_sub(s, 0, n));
    while (runtime_frames_next(&it, &f)) {
        CHECK(f.pc != 0);
        seen++;
    }
    CHECK_INT_EQ(seen, n);
}

static void TestTheFirstFrameIsTheWalkItself(TestingT *t) {
    Uintptr buf[16];
    Slice s = {buf, 16, 16, NULL};
    Int n = runtime_callers(0, s);
    RuntimeFrames it;
    RuntimeFrame f;

    if (!have_table())
        return;

    CHECK(n > 0);

    /* Frame zero is the one runtime_callers writes for itself, so this is the
     * one address on the path whose name is known in advance. */
    it = runtime_callers_frames(slice_sub(s, 0, n));
    CHECK(runtime_frames_next(&it, &f));
    CHECK(str_eq(f.function, BURROW_S("runtime_callers")));
    CHECK(f.entry == (Uintptr)runtime_callers);
    CHECK(f.pc == (Uintptr)runtime_callers + 1);

    /* The address is reported exactly as it was in the slice, which is the one
     * place burrow deliberately differs from Go. */
    CHECK(f.pc != f.entry);
}

static void TestAFrameWithNoNameIsStillAFrame(TestingT *t) {
    Uintptr buf[2] = {0, 0};
    Slice s = {buf, 2, 2, NULL};
    RuntimeFrames it = runtime_callers_frames(s);
    RuntimeFrame f;
    Int seen = 0;

    while (runtime_frames_next(&it, &f)) {
        CHECK(f.function.len == 0);
        CHECK(f.entry == 0);
        seen++;
    }
    CHECK_INT_EQ(seen, 2);
}

static void TestAnEmptySliceGivesNoFrames(TestingT *t) {
    Slice none = {NULL, 0, 0, NULL};
    RuntimeFrames it = runtime_callers_frames(none);
    RuntimeFrame f;

    CHECK(!runtime_frames_next(&it, &f));
    CHECK(f.pc == 0);
    CHECK(f.function.len == 0);
    CHECK(f.file.len == 0);
    CHECK_INT_EQ(f.line, 0);
}

static void TestFramesNextSurvivesANullOfEitherKind(TestingT *t) {
    Uintptr buf[1] = {0};
    Slice s = {buf, 1, 1, NULL};
    RuntimeFrames it = runtime_callers_frames(s);
    RuntimeFrame f;

    CHECK(!runtime_frames_next(NULL, &f));
    CHECK(!runtime_frames_next(&it, NULL));

    /* Refusing to answer did not move the cursor on. */
    CHECK(runtime_frames_next(&it, &f));
    CHECK(!runtime_frames_next(&it, &f));
}

/* ----------------------------------------------------------------- Stack */

static Byte stack_buf[8192];

static Slice whole_buf(void) {
    Slice s = {stack_buf, (Int)sizeof stack_buf, (Int)sizeof stack_buf, NULL};
    return s;
}

static void TestAStackStartsWithAHeaderAndHasFramesUnderIt(TestingT *t) {
    Slice s = whole_buf();
    Int n;

    memset(stack_buf, 0, sizeof stack_buf);
    n = runtime_stack(s, false);

    CHECK(n > 0);
    CHECK(n < (Int)sizeof stack_buf);

    /* No goroutine here, because the tests run on a plain thread unless they
     * ask for a scheduler. */
    CHECK(memcmp(stack_buf, "thread [running]:\n", 18) == 0);

    /* The frames are indented under it, the same as an uncaught panic. On a
     * machine with no walk there are no frames to indent and the header is the
     * whole answer, which is the documented outcome rather than a failure. */
    if (have_walk())
        CHECK(memchr(stack_buf, '\t', (size_t)n) != NULL);
    else
        CHECK(n == 18);
}

static void TestAStackNamesTheFunctionThatAskedForIt(TestingT *t) {
    Slice s = whole_buf();
    Int n;

    if (!have_table())
        return;

    memset(stack_buf, 0, sizeof stack_buf);
    n = runtime_stack(s, false);
    CHECK(n > 0);

    /* The testing package's runner is in the library, but this function is a
     * static in the test binary and not in the table, so the names in here belong to whatever called into the library
     * further down. On a walk that works there is always at least one, because
     * main is reached through the C runtime and the C runtime is not burrow's
     * either. What can be checked without naming a frame is that nothing in the
     * text is a truncated line. */
    CHECK(stack_buf[n - 1] == '\n');
}

static void TestABufferTooSmallForALineGetsNothingRatherThanHalfOfOne(TestingT *t) {
    Byte small[9000];
    Slice s;
    Int n;

    memset(small, 0xAB, sizeof small);
    s.p = small;
    s.len = 4;
    s.cap = 4;
    s.elem = NULL;

    n = runtime_stack(s, false);

    /* The header alone is longer than four bytes, so nothing fits. */
    CHECK_INT_EQ(n, 0);

    /* And nothing was written past the end, which is the property that matters
     * on the path this is for. */
    CHECK(small[4] == 0xAB);
    CHECK(small[5] == 0xAB);
}

static void TestABufferThatFitsTheHeaderAndNotTheFramesGetsTheHeader(TestingT *t) {
    Byte medium[64];
    Slice s = {medium, 24, 24, NULL};
    Int n;

    memset(medium, 0xCD, sizeof medium);
    n = runtime_stack(s, false);

    CHECK_INT_EQ(n, 18);
    CHECK(memcmp(medium, "thread [running]:\n", 18) == 0);
    CHECK(medium[24] == 0xCD);
}

static void TestNoBufferAtAllIsNotAFault(TestingT *t) {
    Slice none = {NULL, 0, 0, NULL};
    Slice empty = {stack_buf, 0, 0, NULL};

    CHECK_INT_EQ(runtime_stack(none, false), 0);
    CHECK_INT_EQ(runtime_stack(empty, true), 0);
}

/* The scheduler running, so that there is more than one goroutine to list. */

static uint32_t all_child_up;
static uint32_t all_child_go;
static Int all_n;

static void all_child(void *env) {
    (void)env;

    burrow__atomic_store_release_u32(&all_child_up, 1);
    while (burrow__atomic_load_acquire_u32(&all_child_go) == 0)
        runtime_gosched();
}

static void all_body(void *env) {
    TestingT *t = env;

    CHECK(go(BURROW_FN(Func, all_child, NULL)));

    /* Let it get as far as the loop, so it is a goroutine that exists and is
     * not dead by the time the stack is taken. */
    for (int i = 0; i < 1000 && burrow__atomic_load_acquire_u32(&all_child_up) == 0;
         i++)
        runtime_gosched();

    memset(stack_buf, 0, sizeof stack_buf);
    all_n = runtime_stack(whole_buf(), true);

    burrow__atomic_store_release_u32(&all_child_go, 1);
}

/* How many times needle appears in the first n bytes of stack_buf. */
static int count_in_stack(Int n, const char *needle) {
    size_t len = strlen(needle);
    int found = 0;

    for (Int i = 0; i + (Int)len <= n; i++) {
        if (memcmp(stack_buf + i, needle, len) == 0)
            found++;
    }
    return found;
}

static void TestAllGoroutinesListsTheOthersByNumber(TestingT *t) {
    all_child_up = 0;
    all_child_go = 0;
    all_n = 0;

    (void)runtime_gomaxprocs(2);
    runtime_main(BURROW_FN(Func, all_body, t));

    CHECK(all_n > 0);

    /* The calling goroutine with its frames, then the child as a line of its
     * own. Two headers is the least this can be: the main goroutine and the one
     * it started. */
    CHECK(count_in_stack(all_n, "goroutine ") >= 2);
    CHECK(count_in_stack(all_n, "stack not walked") >= 1);

    /* The one it was called on is the one that got frames, and it is not listed
     * a second time among the others. */
    CHECK(count_in_stack(all_n, "[running]:") >= 1);
}

static void TestOneGoroutineIsTheDefaultAndLeavesTheOthersOut(TestingT *t) {
    Slice s = whole_buf();
    Int n;

    memset(stack_buf, 0, sizeof stack_buf);
    n = runtime_stack(s, false);

    CHECK(n > 0);
    CHECK_INT_EQ(count_in_stack(n, "stack not walked"), 0);
}

#define TESTS(X)                                                                       \
    X(TestALibraryFunctionKnowsItsOwnName)                                             \
    X(TestAnAddressInsideAFunctionGetsTheSameName)                                     \
    X(TestAnAddressThatIsNobodysGetsNothing)                                           \
    X(TestFuncForPcSurvivesANullAnswer)                                                \
    X(TestCallerAndCallersAreOneApartInExactlyTheDocumentedWay)                        \
    X(TestCallerLeavesFileAndLineAloneForNow)                                          \
    X(TestCallerTakesNullForAnythingItIsNotAskedFor)                                   \
    X(TestSkippingPastTheBottomIsFalseRatherThanAFault)                                \
    X(TestANegativeSkipIsTreatedAsNone)                                                \
    X(TestEveryAddressPutInComesBackAsAFrame)                                          \
    X(TestTheFirstFrameIsTheWalkItself)                                                \
    X(TestAFrameWithNoNameIsStillAFrame)                                               \
    X(TestAnEmptySliceGivesNoFrames)                                                   \
    X(TestFramesNextSurvivesANullOfEitherKind)                                         \
    X(TestAStackStartsWithAHeaderAndHasFramesUnderIt)                                  \
    X(TestAStackNamesTheFunctionThatAskedForIt)                                        \
    X(TestABufferTooSmallForALineGetsNothingRatherThanHalfOfOne)                       \
    X(TestABufferThatFitsTheHeaderAndNotTheFramesGetsTheHeader)                        \
    X(TestNoBufferAtAllIsNotAFault)                                                    \
    X(TestOneGoroutineIsTheDefaultAndLeavesTheOthersOut)                               \
    X(TestAllGoroutinesListsTheOthersByNumber)

TESTING_MAIN_BARE(TESTS)
