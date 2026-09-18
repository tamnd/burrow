/* Stopping, for the cases where carrying on would be a lie.
 *
 * This is the smallest useful piece of the runtime and it is here early because
 * every indexing operation in the library needs it. Go's s[i] panics when i is
 * out of range. A port that returns NULL instead has turned a caught bug into an
 * uncaught one, and a port that leaves the check out has turned it into a
 * silently wrong answer, so the check has to exist before anything with an index
 * can be written.
 *
 * What is here today is Go's fatal error rather than Go's panic. The difference
 * is that a fatal error cannot be recovered from, and it cannot be recovered
 * from because recover needs defer, defer needs the goroutine's defer chain, and
 * that needs the scheduler. So the mechanism is temporary and it is the only
 * temporary thing about this file. The message text is not temporary. Those
 * strings appear in Go's own tests, byte for byte, which is why they are written
 * out in full here rather than being approximated now and fixed later.
 *
 * When defer and recover land, these functions start panicking with the matching
 * runtime.Error value and every caller stays as it is.
 *
 * Copyright 2026 The burrow Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style licence that can be found
 * in the LICENSE file. */

#ifndef BURROW_RUNTIME_H
#define BURROW_RUNTIME_H

#include "burrow/core.h"
#include "burrow/platform.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Stop the program, the way Go's runtime.throw does: print
 *
 *     fatal error: <msg>
 *
 * to standard error and leave. Not recoverable, not catchable, and deliberately
 * not an Error return, because every caller of this has already established that
 * the program's state is not what the program believes it to be.
 *
 * Callers inside burrow are the bounds checks below and nothing else yet. You
 * are welcome to call it from your own code if you have a condition of the same
 * kind, but if you are reaching for it to report a bad argument, return an Error
 * instead. */
BURROW_NORETURN void runtime_throw(Str msg);

/* The specific ones. Each prints exactly what Go prints, because the text is
 * what people search for when they hit it, and because Go's tests compare it. */

/* runtime error: index out of range [i] with length len */
BURROW_NORETURN void runtime_index_out_of_range(Int i, Int len);

/* runtime error: slice bounds out of range [lo:hi] with capacity cap */
BURROW_NORETURN void runtime_slice_bounds_out_of_range(Int lo, Int hi, Int cap);

/* Where the message goes on the way out.
 *
 * The default writes to standard error and ends the process. That is right for
 * a program and wrong for several things burrow is meant to run inside: a
 * kernel module has no stderr, a wasm host wants the string rather than a trap,
 * and an embedded target may have exactly one place to put a last message and it
 * is not a file descriptor.
 *
 * A handler must not return. If one does, burrow ends the process anyway, since
 * the alternative is returning into code that has already been told its
 * assumptions do not hold. Installing a handler is not a way to make a fatal
 * error survivable, it is a way to decide where the note is left.
 *
 * Not safe to call concurrently with itself or with a fatal error. Install it
 * once, during startup, before there is a second thread. */
typedef void (*RuntimeFatalFunc)(Str msg);
void runtime_set_fatal_handler(RuntimeFatalFunc fn);

/* ------------------------------------------------------------------ random
 *
 * Sixty four random bits, from a generator seeded once per thread out of
 * whatever the operating system hands out.
 *
 * This is Go's runtime.rand and it is here for the same reason Go has it in the
 * runtime rather than in math/rand: a map needs a hash seed before any user
 * code has run, and the seed has to be unpredictable rather than merely
 * arbitrary. A map with a fixed seed is a map an attacker can fill with keys
 * that all land in one bucket, and the request that does it costs them nothing.
 *
 * It is not a cryptographic generator and crypto/rand will not be built on it.
 * It is fast, it is seeded from the system, and its output is fine for hash
 * seeds, for shuffling an iteration order, and for tests.
 *
 * The state is per thread, so there is no lock on the fast path and no data
 * race for ThreadSanitizer to find. Two threads therefore get two independent
 * streams, which is what Go's per-m generator gives as well. */
uint64_t runtime_rand64(void);

#ifdef __cplusplus
}
#endif

#endif /* BURROW_RUNTIME_H */
