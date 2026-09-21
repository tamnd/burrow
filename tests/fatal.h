/* Running something that is supposed to end the process, and coming back.
 *
 * Testing a fatal error needs a way to not be fatal, and the fatal handler is
 * that way. It is not a hack for the tests, it is the hook a kernel module or a
 * wasm host needs for the same reason: somewhere else to put the last message.
 *
 * The handler here does the one thing runtime.h says a handler must not do,
 * which is fail to return by leaving sideways instead. That is fine. A panic
 * out of it is still not returning, so the contract holds, and the process
 * survives. What a real program would do here is write the message somewhere
 * and then stop.
 *
 * This used to be the only setjmp in the tree and it is now a BURROW_TRY like
 * everything else, which is what burrow/panic.h is for. tools/check-banned.sh
 * keeps it that way.
 *
 * Both things a test can be checking for end up in the same place. A throw goes
 * through the handler and is turned into a panic here. A panic arrives on its
 * own. Either way the catch block below has the text, so CHECK_FATAL reads the
 * same whichever one the code under test reaches for.
 *
 * Reading the same is exactly what you do not want when the question is which
 * of the two happened, and that is what CHECK_PANIC and CHECK_RUNTIME_ERROR are
 * for. They install no handler, so a statement that throws under one of them
 * ends the process instead of being caught. Use CHECK_FATAL for a condition
 * that is meant to stop the program and CHECK_PANIC for one that is meant to be
 * survivable, and the test then says which it is rather than accepting either.
 *
 * The throw's message needs copying before the panic and not after, which is
 * the one thing here that is easy to get wrong. runtime_index_out_of_range and
 * the rest of them format their message into a buffer in their own frame, and
 * the handler runs while that frame is still there but the catch block does
 * not, because the panic jumped past it. So the handler copies the bytes into a
 * buffer at file scope and panics with a Str over that instead.
 *
 * Copyright 2026 The burrow Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style licence that can be found
 * in the LICENSE file. */

#ifndef BURROW_TESTS_FATAL_H
#define BURROW_TESTS_FATAL_H

#include "burrow/core.h"
#include "burrow/panic.h"
#include "burrow/runtime.h"

#include "harness.h"

/* gcc's -Wclobbered fires on every local a test builds before EXPECT_FATAL and
 * then hands to the statement inside it, because the local is live across the
 * setjmp inside BURROW_TRY and gcc cannot prove it is never written to
 * afterwards. The rule it is guarding is about locals that *change* between the
 * setjmp and the jump back, and none of these do: a test sets its values up,
 * calls the thing that stops, and never touches them again.
 *
 * It only fires on 32 bit x86, where there are not enough registers to keep
 * them all in memory, which is what makes it a false positive rather than a
 * portability warning worth restructuring for. Turned off here rather than in
 * the Makefile so that it stays off in exactly the translation units that catch
 * panics, which is the ones that include this header and nothing else. */
#if defined(__GNUC__) && !defined(__clang__)
#pragma GCC diagnostic ignored "-Wclobbered"
#endif

static char fatal_caught[256];
static char fatal_thrown[256];
static bool fatal_did_catch;

/* BURROW_UNUSED for the reason fatal_handler has it: a test file that only
 * reaches for CHECK_FATAL never touches this one, and gcc treats an untouched
 * static as an error under this build's warnings. */
static BURROW_UNUSED bool fatal_was_runtime_error;

/* Truncating rather than growing, because a test that wants more than this out
 * of a fatal message is testing the wrong thing. Returns what it wrote. */
static Int fatal_copy(char *dst, size_t cap, Str msg) {
    size_t n = (size_t)(msg.len < (Int)cap - 1 ? msg.len : (Int)cap - 1);

    if (msg.p != NULL && n > 0)
        memcpy(dst, msg.p, n);
    dst[n] = '\0';
    return (Int)n;
}

/* The throw's way in, and the place the message stops being the thrower's.
 *
 * BURROW_UNUSED because a test file that only reaches for CHECK_PANIC never
 * names this, and a static function nobody calls is an error under this
 * build's warnings. */
static BURROW_UNUSED void fatal_handler(Str msg) {
    Int n = fatal_copy(fatal_thrown, sizeof(fatal_thrown), msg);

    panic_str(str_from_bytes(fatal_thrown, n));
}

/* Everything it touches is at file scope, for the reason the pragma above
 * gives. */
#define EXPECT_FATAL(stmt)                                                             \
    do {                                                                               \
        memset(fatal_caught, 0, sizeof(fatal_caught));                                 \
        memset(fatal_thrown, 0, sizeof(fatal_thrown));                                 \
        fatal_did_catch = false;                                                       \
        runtime_set_fatal_handler(fatal_handler);                                      \
        BURROW_TRY {                                                                   \
            stmt;                                                                      \
        }                                                                              \
        BURROW_CATCH(fatal_p) {                                                        \
            fatal_copy(fatal_caught, sizeof(fatal_caught), panic_text(fatal_p));       \
            fatal_did_catch = true;                                                    \
        }                                                                              \
        BURROW_TRY_END;                                                                \
        runtime_set_fatal_handler(NULL);                                               \
    } while (0)

/* The common case: it stopped, and it said this. Not stopping at all is a
 * different failure from stopping with the wrong text, so they report
 * differently. */
#define CHECK_FATAL(stmt, want)                                                        \
    do {                                                                               \
        EXPECT_FATAL(stmt);                                                            \
        CHECK(fatal_did_catch);                                                        \
        if (fatal_did_catch)                                                           \
            CHECK_STR_EQ(fatal_caught, want);                                          \
    } while (0)

/* The recoverable half, and the difference from the pair above is one line that
 * is not there: no fatal handler is installed.
 *
 * That absence is the assertion. A statement that throws rather than panicking
 * has no net under it here, so it ends the process and takes the run with it,
 * which is a loud failure rather than a quiet pass. A condition Go lets you
 * recover from has to be one burrow lets you recover from, and from inside a
 * test the only way to check that is to remove the alternative. */
#define EXPECT_PANIC(stmt)                                                             \
    do {                                                                               \
        memset(fatal_caught, 0, sizeof(fatal_caught));                                 \
        fatal_did_catch = false;                                                       \
        fatal_was_runtime_error = false;                                               \
        BURROW_TRY {                                                                   \
            stmt;                                                                      \
        }                                                                              \
        BURROW_CATCH(fatal_p) {                                                        \
            fatal_copy(fatal_caught, sizeof(fatal_caught), panic_text(fatal_p));       \
            fatal_was_runtime_error = runtime_error_from(fatal_p) != NULL;             \
            fatal_did_catch = true;                                                    \
        }                                                                              \
        BURROW_TRY_END;                                                                \
    } while (0)

#define CHECK_PANIC(stmt, want)                                                        \
    do {                                                                               \
        EXPECT_PANIC(stmt);                                                            \
        CHECK(fatal_did_catch);                                                        \
        if (fatal_did_catch)                                                           \
            CHECK_STR_EQ(fatal_caught, want);                                          \
    } while (0)

/* A panic carrying one of the runtime's own errors, which is what every check
 * in burrow/runtime.h raises and what Go hands back from a recover as a
 * runtime.Error. Checking the type as well as the text matters because the text
 * would still match if somebody panicked with a bare string that happened to
 * say the same thing, and a caller sorting its own mistakes from the runtime's
 * asks the type. */
#define CHECK_RUNTIME_ERROR(stmt, want)                                                \
    do {                                                                               \
        CHECK_PANIC(stmt, want);                                                       \
        CHECK(fatal_was_runtime_error);                                                \
    } while (0)

#endif /* BURROW_TESTS_FATAL_H */
