/* The one file in burrow that is allowed to end the host's process.
 *
 * tools/check-banned.sh enforces that, and the reason it is worth enforcing is
 * that a library which calls exit has taken a decision away from the program
 * that linked it. Everything in burrow that can fail returns an Error. What is
 * left over is the class of condition where there is nothing to return to,
 * because the caller's belief about its own state was wrong, and that is what
 * this file handles.
 *
 * Copyright 2026 The burrow Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style licence that can be found
 * in the LICENSE file. */

#include "burrow/runtime.h"

#include <stdio.h>
#include <stdlib.h>

/* Read on the way out and written once during startup. It is a plain pointer
 * rather than an atomic because the atomics layer does not exist yet and
 * because the documented contract is to install it before there is a second
 * thread. When sync/atomic lands this becomes an atomic load and the contract
 * gets weaker, which is a change nobody has to notice. */
static RuntimeFatalFunc fatal_handler;

void runtime_set_fatal_handler(RuntimeFatalFunc fn) {
    fatal_handler = fn;
}

/* Go's format, down to the leading "fatal error: " and the newline, because
 * that prefix is what somebody pastes into a search box. */
static void write_message(Str msg) {
    fputs("fatal error: ", stderr);
    if (msg.p != NULL && msg.len > 0)
        fwrite(msg.p, 1, (size_t)msg.len, stderr);
    fputc('\n', stderr);
    fflush(stderr);
}

void runtime_throw(Str msg) {
    RuntimeFatalFunc fn = fatal_handler;

    /* A handler that returns is a handler that has misunderstood the deal, so
     * the default path runs afterwards and the process still ends. Letting it
     * return would mean resuming code that has already been told its
     * assumptions do not hold, which is how a caught bug becomes a corrupted
     * file. */
    if (fn != NULL)
        fn(msg);

    write_message(msg);

    /* Status 2 is what a Go program exits with when a panic goes unrecovered,
     * and scripts around burrow should not have to care which of the two
     * produced the failure.
     *
     * _Exit rather than exit, because exit runs atexit handlers and flushes
     * every stream, and we have just finished saying that the program's state
     * is not what the program thinks it is. stderr is flushed above, by hand,
     * because that one message is worth more than the rest of the buffers put
     * together. */
    _Exit(2);
}

/* Both of the specific ones format into a stack buffer. Nothing here allocates,
 * on purpose: running out of memory is one of the things that will eventually
 * get here, and a reporting path that needs an allocator is a reporting path
 * that stops working exactly when it is needed.
 *
 * 128 bytes holds the longest either of these can produce, since the text is
 * fixed and the three numbers are at most twenty digits each. snprintf
 * truncates rather than overflowing if that arithmetic is ever wrong. */
#define MSG_MAX 128

void runtime_index_out_of_range(Int i, Int len) {
    char buf[MSG_MAX];
    int n = snprintf(buf, sizeof(buf),
                     "runtime error: index out of range [%lld] with length %lld",
                     (long long)i, (long long)len);
    if (n < 0)
        runtime_throw(BURROW_S("runtime error: index out of range"));
    runtime_throw(
        str_from_bytes(buf, (Int)(n < (int)sizeof(buf) ? n : (int)sizeof(buf) - 1)));
}

/* These two carry no numbers, so they are a throw with a constant string and
 * nothing else. They exist as functions rather than as the string written out
 * at each call site because burrow/num.h has forty of those call sites and
 * because the text has to stay identical across all of them. */
void runtime_integer_divide_by_zero(void) {
    runtime_throw(BURROW_S("runtime error: integer divide by zero"));
}

void runtime_negative_shift(void) {
    runtime_throw(BURROW_S("runtime error: negative shift amount"));
}

void runtime_slice_bounds_out_of_range(Int lo, Int hi, Int cap) {
    char buf[MSG_MAX];
    int n = snprintf(
        buf, sizeof(buf),
        "runtime error: slice bounds out of range [%lld:%lld] with capacity %lld",
        (long long)lo, (long long)hi, (long long)cap);
    if (n < 0)
        runtime_throw(BURROW_S("runtime error: slice bounds out of range"));
    runtime_throw(
        str_from_bytes(buf, (Int)(n < (int)sizeof(buf) ? n : (int)sizeof(buf) - 1)));
}
