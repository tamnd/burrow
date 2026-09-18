/* Running something that is supposed to end the process, and coming back.
 *
 * Testing a fatal error needs a way to not be fatal, and the fatal handler is
 * that way. It is not a hack for the tests, it is the hook a kernel module or a
 * wasm host needs for the same reason: somewhere else to put the last message.
 *
 * The handler here does the one thing runtime.h says a handler must not do,
 * which is fail to return by leaving sideways instead. That is fine. longjmp
 * out of it is still not returning, so the contract holds, and the process
 * survives. What a real program would do here is write the message somewhere
 * and then stop.
 *
 * This is the only setjmp in the tree. When defer and panic land they own
 * setjmp, there will be a checker that says so, and everything here gets
 * rewritten against BURROW_TRY.
 *
 * Copyright 2026 The burrow Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style licence that can be found
 * in the LICENSE file. */

#ifndef BURROW_TESTS_FATAL_H
#define BURROW_TESTS_FATAL_H

#include "burrow/core.h"
#include "burrow/runtime.h"

#include "harness.h"

#include <setjmp.h>

static jmp_buf fatal_escape;
static char fatal_caught[256];
static bool fatal_did_catch;

static void fatal_handler(Str msg) {
    size_t n = (size_t)(msg.len < (Int)sizeof(fatal_caught) - 1
                            ? msg.len
                            : (Int)sizeof(fatal_caught) - 1);
    if (msg.p != NULL && n > 0)
        memcpy(fatal_caught, msg.p, n);
    fatal_caught[n] = '\0';
    fatal_did_catch = true;
    longjmp(fatal_escape, 1);
}

/* setjmp has to be the whole controlling expression of an if for this to be
 * defined, which is why this is a statement macro rather than something that
 * returns the message. Everything it touches is at file scope, since a local
 * that changes between setjmp and longjmp has an indeterminate value afterwards
 * unless it is volatile. */
#define EXPECT_FATAL(stmt)                                                             \
    do {                                                                               \
        memset(fatal_caught, 0, sizeof(fatal_caught));                                 \
        fatal_did_catch = false;                                                       \
        runtime_set_fatal_handler(fatal_handler);                                      \
        if (setjmp(fatal_escape) == 0) {                                               \
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
        CHECK(fatal_did_catch);                                                        \
        if (fatal_did_catch)                                                           \
            CHECK_STR_EQ(fatal_caught, want);                                          \
    } while (0)

#endif /* BURROW_TESTS_FATAL_H */
