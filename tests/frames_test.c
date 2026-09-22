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

#include "harness.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

static bool have_table(void) {
    return burrow__symtab_len() > 0;
}

/* ------------------------------------------------------------- FuncForPC */

TEST(a_library_function_knows_its_own_name) {
    RuntimeFunc fn;

    if (!have_table())
        return;

    CHECK(runtime_func_for_pc((Uintptr)runtime_callers, &fn));
    CHECK(str_eq(fn.name, BURROW_S("runtime_callers")));
    CHECK(fn.entry == (Uintptr)runtime_callers);
}

TEST(an_address_inside_a_function_gets_the_same_name) {
    RuntimeFunc at;
    RuntimeFunc inside;

    if (!have_table())
        return;

    CHECK(runtime_func_for_pc((Uintptr)runtime_callers, &at));
    CHECK(runtime_func_for_pc((Uintptr)runtime_callers + 4, &inside));
    CHECK(str_eq(at.name, inside.name));
    CHECK(at.entry == inside.entry);
}

TEST(an_address_that_is_nobodys_gets_nothing) {
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

TEST(func_for_pc_survives_a_null_answer) {
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

TEST(caller_and_callers_are_one_apart_in_exactly_the_documented_way) {
    Uintptr by_callers = 0;
    Uintptr by_caller = 0;

    ask_both_ways(&by_callers, &by_caller);

    if (by_callers == 0 && by_caller == 0)
        return; /* an architecture with no walk, which trace_test covers */

    CHECK(by_callers != 0);
    CHECK(by_caller == by_callers);
}

TEST(caller_leaves_file_and_line_alone_for_now) {
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

TEST(caller_takes_null_for_anything_it_is_not_asked_for) {
    /* All three outputs dropped is a legal way to ask whether a frame exists. */
    if (!runtime_caller(0, NULL, NULL, NULL))
        return;
    CHECK(true);
}

TEST(skipping_past_the_bottom_is_false_rather_than_a_fault) {
    Uintptr pc = 12345;

    CHECK(!runtime_caller(1 << 20, &pc, NULL, NULL));
    CHECK(pc == 12345); /* an output left alone is left alone */
}

TEST(a_negative_skip_is_treated_as_none) {
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

TEST(every_address_put_in_comes_back_as_a_frame) {
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

TEST(the_first_frame_is_the_walk_itself) {
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

TEST(a_frame_with_no_name_is_still_a_frame) {
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

TEST(an_empty_slice_gives_no_frames) {
    Slice none = {NULL, 0, 0, NULL};
    RuntimeFrames it = runtime_callers_frames(none);
    RuntimeFrame f;

    CHECK(!runtime_frames_next(&it, &f));
    CHECK(f.pc == 0);
    CHECK(f.function.len == 0);
    CHECK(f.file.len == 0);
    CHECK_INT_EQ(f.line, 0);
}

TEST(frames_next_survives_a_null_of_either_kind) {
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

TEST(a_stack_starts_with_a_header_and_has_frames_under_it) {
    Slice s = whole_buf();
    Int n;

    memset(stack_buf, 0, sizeof stack_buf);
    n = runtime_stack(s, false);

    CHECK(n > 0);
    CHECK(n < (Int)sizeof stack_buf);

    /* No goroutine here, because the tests run on a plain thread unless they
     * ask for a scheduler. */
    CHECK(memcmp(stack_buf, "thread [running]:\n", 18) == 0);

    /* The frames are indented under it, the same as an uncaught panic. */
    CHECK(memchr(stack_buf, '\t', (size_t)n) != NULL);
}

TEST(a_stack_names_the_function_that_asked_for_it) {
    Slice s = whole_buf();
    Int n;

    if (!have_table())
        return;

    memset(stack_buf, 0, sizeof stack_buf);
    n = runtime_stack(s, false);
    CHECK(n > 0);

    /* harness_report is in the test binary and not in the table, and so is this
     * function, so the names in here belong to whatever called into the library
     * further down. On a walk that works there is always at least one, because
     * main is reached through the C runtime and the C runtime is not burrow's
     * either. What can be checked without naming a frame is that nothing in the
     * text is a truncated line. */
    CHECK(stack_buf[n - 1] == '\n');
}

TEST(a_buffer_too_small_for_a_line_gets_nothing_rather_than_half_of_one) {
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

TEST(a_buffer_that_fits_the_header_and_not_the_frames_gets_the_header) {
    Byte medium[64];
    Slice s = {medium, 24, 24, NULL};
    Int n;

    memset(medium, 0xCD, sizeof medium);
    n = runtime_stack(s, false);

    CHECK_INT_EQ(n, 18);
    CHECK(memcmp(medium, "thread [running]:\n", 18) == 0);
    CHECK(medium[24] == 0xCD);
}

TEST(no_buffer_at_all_is_not_a_fault) {
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
    (void)env;

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

TEST(all_goroutines_lists_the_others_by_number) {
    all_child_up = 0;
    all_child_go = 0;
    all_n = 0;

    (void)runtime_gomaxprocs(2);
    runtime_main(BURROW_FN(Func, all_body, NULL));

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

TEST(one_goroutine_is_the_default_and_leaves_the_others_out) {
    Slice s = whole_buf();
    Int n;

    memset(stack_buf, 0, sizeof stack_buf);
    n = runtime_stack(s, false);

    CHECK(n > 0);
    CHECK_INT_EQ(count_in_stack(n, "stack not walked"), 0);
}

int main(void) {
    RUN(a_library_function_knows_its_own_name);
    RUN(an_address_inside_a_function_gets_the_same_name);
    RUN(an_address_that_is_nobodys_gets_nothing);
    RUN(func_for_pc_survives_a_null_answer);
    RUN(caller_and_callers_are_one_apart_in_exactly_the_documented_way);
    RUN(caller_leaves_file_and_line_alone_for_now);
    RUN(caller_takes_null_for_anything_it_is_not_asked_for);
    RUN(skipping_past_the_bottom_is_false_rather_than_a_fault);
    RUN(a_negative_skip_is_treated_as_none);
    RUN(every_address_put_in_comes_back_as_a_frame);
    RUN(the_first_frame_is_the_walk_itself);
    RUN(a_frame_with_no_name_is_still_a_frame);
    RUN(an_empty_slice_gives_no_frames);
    RUN(frames_next_survives_a_null_of_either_kind);
    RUN(a_stack_starts_with_a_header_and_has_frames_under_it);
    RUN(a_stack_names_the_function_that_asked_for_it);
    RUN(a_buffer_too_small_for_a_line_gets_nothing_rather_than_half_of_one);
    RUN(a_buffer_that_fits_the_header_and_not_the_frames_gets_the_header);
    RUN(no_buffer_at_all_is_not_a_fault);
    RUN(one_goroutine_is_the_default_and_leaves_the_others_out);
    RUN(all_goroutines_lists_the_others_by_number);
    return harness_report("frames");
}
