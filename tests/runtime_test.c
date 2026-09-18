/* Copyright 2026 The burrow Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style licence that can be found
 * in the LICENSE file. */

#include "burrow/core.h"
#include "burrow/runtime.h"

#include "fatal.h"
#include "harness.h"

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
    CHECK(fatal_did_catch);
    CHECK_STR_EQ(fatal_caught, "stop");
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
    CHECK(fatal_did_catch);
    CHECK(strncmp(fatal_caught, "runtime error: index out of range [", 35) == 0);
    CHECK(strlen(fatal_caught) < 128);
}

int main(void) {
    RUN(throw_carries_the_message_through);
    RUN(the_messages_are_the_ones_go_prints);
    RUN(str_at_checks_both_ends);
    RUN(an_absurd_number_truncates_rather_than_overflowing);
    return harness_report("runtime");
}
