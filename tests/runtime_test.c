/* Copyright 2026 The burrow Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style licence that can be found
 * in the LICENSE file. */

#include "burrow/core.h"
#include "burrow/runtime.h"

#include "burrow/defer.h"
#include "burrow/error.h"
#include "burrow/panic.h"

#include "check.h"
#include "fatal.h"

static void TestThrowCarriesTheMessageThrough(TestingT *t) {
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

static void TestTheMessagesAreTheOnesGoPrints(TestingT *t) {
    /* Byte for byte. These strings are in Go's own tests and they are the first
     * thing somebody pastes into a search box, so a paraphrase here would be a
     * small lie that costs somebody an afternoon. */
    CHECK_RUNTIME_ERROR(runtime_index_out_of_range(5, 3),
                        "runtime error: index out of range [5] with length 3");

    CHECK_RUNTIME_ERROR(runtime_index_out_of_range(-1, 0),
                        "runtime error: index out of range [-1] with length 0");

    CHECK_RUNTIME_ERROR(
        runtime_slice_bounds_out_of_range(0, 5, 3),
        "runtime error: slice bounds out of range [0:5] with capacity 3");
}

static void TestTheNumbersInThemSurviveTheEdges(TestingT *t) {
    /* The message is built by hand rather than by snprintf, so the three cases
     * a hand written decimal conversion gets wrong are worth a test each. Zero
     * is the one a loop that divides until nothing is left prints as nothing at
     * all. The largest Int is the longest it ever gets. The most negative Int
     * is the one with no positive of the same size, which is why the conversion
     * negates in unsigned and prints the sign separately. */
    CHECK_RUNTIME_ERROR(runtime_index_out_of_range(0, 0),
                        "runtime error: index out of range [0] with length 0");

#if BURROW_INT_MAX > 2147483647
    CHECK_RUNTIME_ERROR(runtime_index_out_of_range(BURROW_INT_MIN, BURROW_INT_MAX),
                        "runtime error: index out of range [-9223372036854775808] "
                        "with length 9223372036854775807");
#else
    CHECK_RUNTIME_ERROR(runtime_index_out_of_range(BURROW_INT_MIN, BURROW_INT_MAX),
                        "runtime error: index out of range [-2147483648] "
                        "with length 2147483647");
#endif
}

static void TestStrAtChecksBothEnds(TestingT *t) {
    Str s = BURROW_S("hello");

    CHECK_INT_EQ(str_at(s, 0), 'h');
    CHECK_INT_EQ(str_at(s, 4), 'o');

    /* A NUL in the middle is a byte like any other, which is the whole point of
     * Str, and indexing has to agree. */
    Str nul = BURROW_S("a\0b");
    CHECK_INT_EQ(str_at(nul, 1), 0);
    CHECK_INT_EQ(str_at(nul, 2), 'b');

    CHECK_RUNTIME_ERROR(str_at(s, 5),
                        "runtime error: index out of range [5] with length 5");

    /* Negative, which the unsigned compare trick would have let through on a
     * Str somebody filled in by hand. */
    CHECK_RUNTIME_ERROR(str_at(s, -1),
                        "runtime error: index out of range [-1] with length 5");

    /* The empty string has no valid index at all, including zero. */
    CHECK_RUNTIME_ERROR(str_at(BURROW_STR_EMPTY, 0),
                        "runtime error: index out of range [0] with length 0");
}

static void TestAnAbsurdNumberTruncatesRatherThanOverflowing(TestingT *t) {
    /* The formatting buffer is fixed and nothing on this path allocates, since
     * running out of memory is one of the things that will eventually arrive
     * here. So the only question is what the widest possible numbers do. */
    EXPECT_PANIC(runtime_index_out_of_range(BURROW_INT_MAX, BURROW_INT_MIN));
    CHECK(fatal_did_catch);
    CHECK(strncmp(fatal_caught, "runtime error: index out of range [", 35) == 0);
    CHECK(strlen(fatal_caught) < 128);
}

/* Everything below is at file scope for the reason fatal.h's pragma gives: a
 * local written inside a BURROW_TRY and read after it is a local gcc warns
 * about and C says nothing useful about. */
static bool cleaned_up;
static bool caught;
static char kept[BURROW_RUNTIME_ERROR_MAX];

static void mark_cleaned_up(void *unused) {
    (void)unused;
    cleaned_up = true;
}

static bool says(Str got, const char *want) {
    return str_eq(got, str_from_cstr(want));
}

static void TestABoundsCheckIsAPanicTheProgramCanCatch(TestingT *t) {
    Str s = BURROW_S("hello");

    caught = false;
    cleaned_up = false;

    BURROW_TRY {
        BURROW_SCOPE {
            BURROW_DEFER(mark_cleaned_up, NULL);
            (void)str_at(s, 99);
        }
        BURROW_SCOPE_END;
    }
    BURROW_CATCH(p) {
        const RuntimeError *re = runtime_error_from(p);

        caught = true;
        CHECK(re != NULL);
        if (re != NULL)
            CHECK(says(re->message,
                       "runtime error: index out of range [99] with length 5"));

        /* The same thing the other way round, since the value is a plain Error
         * and errors_as is how anybody else would ask. */
        CHECK(p.t == TYPE_ERROR);
        if (p.t == TYPE_ERROR) {
            Error err = *(const Error *)p.data;

            CHECK(errors_as(err, TYPE_RUNTIME_ERROR) != NULL);
            CHECK(says(error_text(err),
                       "runtime error: index out of range [99] with length 5"));
        }

        /* And what the default printer would have said, which is the message
         * and not the type's name. */
        CHECK(says(panic_text(p),
                   "runtime error: index out of range [99] with length 5"));
    }
    BURROW_TRY_END;

    CHECK(caught);

    /* The point of it being a panic rather than a fatal error. The scope
     * between the failure and the catch block closed on the way past. */
    CHECK(cleaned_up);
}

static void TestAPanicThatIsNotTheRuntimesSaysSo(TestingT *t) {
    caught = false;

    BURROW_TRY {
        panic_str(BURROW_S("mine, not the runtime's"));
    }
    BURROW_CATCH(p) {
        caught = true;
        CHECK(runtime_error_from(p) == NULL);
    }
    BURROW_TRY_END;
    CHECK(caught);

    /* An ordinary Error is the interesting near miss, because the panicked
     * value has the same type descriptor on it and only the error's own type
     * tells the two apart. */
    caught = false;
    BURROW_TRY {
        Error err = errors_err_unsupported;

        panic(BURROW_ANY(TYPE_ERROR, &err));
    }
    BURROW_CATCH(p) {
        caught = true;
        CHECK(runtime_error_from(p) == NULL);
    }
    BURROW_TRY_END;
    CHECK(caught);

    /* A nil Any is what panic_value hands back when nothing is unwinding, so
     * it has to be a question this can be asked. */
    CHECK(runtime_error_from((Any){NULL, NULL}) == NULL);
}

static void TestTheMessageIsBorrowedAndACopyIsYours(TestingT *t) {
    caught = false;
    memset(kept, 0, sizeof(kept));

    BURROW_TRY {
        runtime_index_out_of_range(1, 0);
    }
    BURROW_CATCH(p) {
        const RuntimeError *re = runtime_error_from(p);

        caught = true;
        if (re != NULL) {
            memcpy(kept, re->message.p, (size_t)re->message.len);
            kept[re->message.len] = '\0';
        }
    }
    BURROW_TRY_END;

    CHECK(caught);
    CHECK_STR_EQ(kept, "runtime error: index out of range [1] with length 0");

    /* The slot the message lived in belongs to the goroutine and the next
     * runtime error on it writes over the bytes. The copy above does not care,
     * which is the whole reason the header says to make one. */
    BURROW_TRY {
        runtime_negative_shift();
    }
    BURROW_CATCH(p) {
        CHECK(says(panic_text(p), "runtime error: negative shift amount"));
    }
    BURROW_TRY_END;

    CHECK_STR_EQ(kept, "runtime error: index out of range [1] with length 0");
}

static void TestTheTypeSaysWhichPackageItCameFrom(TestingT *t) {
    /* It prints as runtime.Error, which is the name of the thing in Go, and it
     * is a distinct descriptor from error itself so that errors_as can tell a
     * runtime error from any other one. */
    CHECK(says(TYPE_RUNTIME_ERROR->name, "Error"));
    CHECK(says(TYPE_RUNTIME_ERROR->pkg_path, "runtime"));
    CHECK(TYPE_RUNTIME_ERROR != TYPE_ERROR);
    CHECK(TYPE_RUNTIME_ERROR->size == (uint32_t)sizeof(RuntimeError));
}

static void TestRuntimePanicIsOpenForBusiness(TestingT *t) {
    /* Anybody writing their own container writes their own bounds check, and
     * this is how it says so in the same voice the library uses. */
    caught = false;

    BURROW_TRY {
        runtime_panic(BURROW_S("runtime error: ring buffer index out of range"));
    }
    BURROW_CATCH(p) {
        const RuntimeError *re = runtime_error_from(p);

        caught = true;
        CHECK(re != NULL);
        if (re != NULL)
            CHECK(says(re->message, "runtime error: ring buffer index out of range"));
    }
    BURROW_TRY_END;
    CHECK(caught);

    /* An empty message still panics, the same as an empty throw still stops. */
    caught = false;
    BURROW_TRY {
        runtime_panic(BURROW_STR_EMPTY);
    }
    BURROW_CATCH(p) {
        const RuntimeError *re = runtime_error_from(p);

        caught = true;
        CHECK(re != NULL);
        if (re != NULL)
            CHECK(re->message.len == 0);
    }
    BURROW_TRY_END;
    CHECK(caught);
}

#define TESTS(X)                                                                       \
    X(TestThrowCarriesTheMessageThrough)                                               \
    X(TestTheMessagesAreTheOnesGoPrints)                                               \
    X(TestTheNumbersInThemSurviveTheEdges)                                             \
    X(TestStrAtChecksBothEnds)                                                         \
    X(TestAnAbsurdNumberTruncatesRatherThanOverflowing)                                \
    X(TestABoundsCheckIsAPanicTheProgramCanCatch)                                      \
    X(TestAPanicThatIsNotTheRuntimesSaysSo)                                            \
    X(TestTheMessageIsBorrowedAndACopyIsYours)                                         \
    X(TestTheTypeSaysWhichPackageItCameFrom)                                           \
    X(TestRuntimePanicIsOpenForBusiness)

TESTING_MAIN(TESTS)
