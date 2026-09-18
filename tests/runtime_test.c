/* Copyright 2026 The burrow Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style licence that can be found
 * in the LICENSE file. */

#include "harness.h"

#include "burrow/core.h"
#include "burrow/runtime.h"

#include <setjmp.h>

/* Testing something that ends the process needs a way to not end the process,
 * and the fatal handler is that way. It is not a hack for the tests, it is the
 * hook a kernel module or a wasm host needs for the same reason: somewhere else
 * to put the last message.
 *
 * The handler here does the one thing the documentation says a handler must not
 * do, which is fail to return by leaving sideways instead. That is fine.
 * longjmp out of it is still not returning, so the contract holds, and the
 * process survives. What a real program would do here is write the message
 * somewhere and then stop.
 *
 * This is also the only setjmp in the tree. When defer and panic land they own
 * setjmp, there will be a checker that says so, and this file gets rewritten
 * against BURROW_TRY along with everything else. */

static jmp_buf escape;
static char caught[256];
static bool did_catch;

static void catch_fatal(Str msg) {
    size_t n =
        (size_t)(msg.len < (Int)sizeof(caught) - 1 ? msg.len : (Int)sizeof(caught) - 1);
    if (msg.p != NULL && n > 0)
        memcpy(caught, msg.p, n);
    caught[n] = '\0';
    did_catch = true;
    longjmp(escape, 1);
}

/* Run something that is supposed to stop, and come back.
 *
 * setjmp has to be the whole controlling expression of an if for this to be
 * defined, which is why this is a statement macro rather than something that
 * returns the message. Everything it touches is at file scope, since a local
 * that changes between setjmp and longjmp has an indeterminate value afterwards
 * unless it is volatile. */
#define EXPECT_FATAL(stmt)                                                             \
    do {                                                                               \
        memset(caught, 0, sizeof(caught));                                             \
        did_catch = false;                                                             \
        runtime_set_fatal_handler(catch_fatal);                                        \
        if (setjmp(escape) == 0) {                                                     \
            stmt;                                                                      \
        }                                                                              \
        runtime_set_fatal_handler(NULL);                                               \
    } while (0)

/* The common case: it stopped, and it said this. Not stopping at all is a
 * different failure from stopping with the wrong text, so they report
 * differently. */
#define CHECK_FATAL(stmt, want)                                                        \
    do {                                                                               \
        EXPECT_FATAL(stmt);                                                            \
        CHECK(did_catch);                                                              \
        if (did_catch)                                                                 \
            CHECK_STR_EQ(caught, want);                                                \
    } while (0)

TEST(throw_carries_the_message_through) {
    CHECK_FATAL(runtime_throw(BURROW_S("something is wrong")), "something is wrong");

    /* An empty message still stops. The alternative is a program that carries
     * on because the reason was hard to phrase. */
    CHECK_FATAL(runtime_throw(BURROW_STR_EMPTY), "");

    /* And a message with a NUL in it arrives whole, since it is a Str. The
     * handler here copies it into a C buffer and so only sees the first part,
     * which is the handler's problem rather than the runtime's, but the length
     * that got handed over is the real one. */
    EXPECT_FATAL(runtime_throw(BURROW_S("stop\0here")));
    CHECK(did_catch);
    CHECK_STR_EQ(caught, "stop");
}

TEST(the_messages_are_the_ones_go_prints) {
    /* Byte for byte. These strings are in Go's own tests and they are the first
     * thing somebody pastes into a search box, so a paraphrase here would be a
     * small lie that costs somebody an afternoon. */
    CHECK_FATAL(runtime_index_out_of_range(5, 3),
                "runtime error: index out of range [5] with length 3");

    CHECK_FATAL(runtime_index_out_of_range(-1, 0),
                "runtime error: index out of range [-1] with length 0");

    CHECK_FATAL(runtime_slice_bounds_out_of_range(0, 5, 3),
                "runtime error: slice bounds out of range [0:5] with capacity 3");
}

TEST(str_at_checks_both_ends) {
    Str s = BURROW_S("hello");

    CHECK_INT_EQ(str_at(s, 0), 'h');
    CHECK_INT_EQ(str_at(s, 4), 'o');

    /* A NUL in the middle is a byte like any other, which is the whole point of
     * Str, and indexing has to agree. */
    Str nul = BURROW_S("a\0b");
    CHECK_INT_EQ(str_at(nul, 1), 0);
    CHECK_INT_EQ(str_at(nul, 2), 'b');

    CHECK_FATAL(str_at(s, 5), "runtime error: index out of range [5] with length 5");

    /* Negative, which the unsigned compare trick would have let through on a
     * Str somebody filled in by hand. */
    CHECK_FATAL(str_at(s, -1), "runtime error: index out of range [-1] with length 5");

    /* The empty string has no valid index at all, including zero. */
    CHECK_FATAL(str_at(BURROW_STR_EMPTY, 0),
                "runtime error: index out of range [0] with length 0");
}

TEST(an_absurd_number_truncates_rather_than_overflowing) {
    /* The formatting buffer is fixed and nothing on this path allocates, since
     * running out of memory is one of the things that will eventually arrive
     * here. So the only question is what the widest possible numbers do. */
    EXPECT_FATAL(runtime_index_out_of_range(BURROW_INT_MAX, BURROW_INT_MIN));
    CHECK(did_catch);
    CHECK(strncmp(caught, "runtime error: index out of range [", 35) == 0);
    CHECK(strlen(caught) < 128);
}

int main(void) {
    RUN(throw_carries_the_message_through);
    RUN(the_messages_are_the_ones_go_prints);
    RUN(str_at_checks_both_ends);
    RUN(an_absurd_number_truncates_rather_than_overflowing);
    return harness_report("runtime");
}
