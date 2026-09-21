/* Stopping, for the cases where carrying on would be a lie.
 *
 * This is the smallest useful piece of the runtime and it is here early because
 * every indexing operation in the library needs it. Go's s[i] panics when i is
 * out of range. A port that returns NULL instead has turned a caught bug into an
 * uncaught one, and a port that leaves the check out has turned it into a
 * silently wrong answer, so the check has to exist before anything with an index
 * can be written.
 *
 * There are two ways out of this file and the difference between them is the
 * whole subject. A throw is the runtime saying its own invariants are broken,
 * and the process ends, which is Go's runtime.throw. A panic is the runtime
 * saying a call was wrong, and a program that wants to can catch it, which is
 * Go's panic. runtime_throw is the first. The four checks under it are the
 * second: they panic with a RuntimeError, which is Go's runtime.Error.
 *
 * Which condition gets which is not a judgement call, it is a lookup. If Go's
 * version of the same condition is a recoverable panic then burrow's is a
 * panic, and if Go throws, or if the condition only exists because burrow is
 * written in C, burrow throws. So an index past the end panics, a nil map
 * written to panics, a send on a closed channel panics, and a map that grew
 * under an iterator ends the process, because that is the line Go draws in each
 * of those four places.
 *
 * The message text is Go's, byte for byte. Those strings appear in Go's own
 * tests and they are what somebody pastes into a search box when they hit one,
 * which is why they are written out in full here rather than approximated.
 *
 * Copyright 2026 The burrow Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style licence that can be found
 * in the LICENSE file. */

#ifndef BURROW_RUNTIME_H
#define BURROW_RUNTIME_H

#include "burrow/core.h"
#include "burrow/error.h"
#include "burrow/iface.h"
#include "burrow/platform.h"
#include "burrow/slice.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Go's runtime.Error: the error a panic raised by the runtime carries.
 *
 * Go's is an interface with a marker method on it, because the concrete types
 * behind it are unexported and the only thing a program is allowed to ask is
 * whether this failure came from the runtime or from somewhere else. Here it is
 * a struct with the message in it and TYPE_RUNTIME_ERROR is how errors_as finds
 * it, which is the same question with the same answer.
 *
 * You get one out of a catch block:
 *
 *     BURROW_TRY {
 *         handle(request);
 *     }
 *     BURROW_CATCH(p) {
 *         const RuntimeError *re = runtime_error_from(p);
 *
 *         if (re != NULL)
 *             log_bug(re->message);
 *         else
 *             log_panic(panic_text(p));
 *     }
 *     BURROW_TRY_END;
 *
 * or out of an Error that came from somewhere else with errors_as, since the
 * value panicked with is a plain Error and behaves like one:
 *
 *     const RuntimeError *re = errors_as(err, TYPE_RUNTIME_ERROR); */
typedef struct RuntimeError {
    /* What went wrong, in Go's words. It borrows, and the paragraph over
     * BURROW_RUNTIME_ERROR_MAX says for how long. */
    Str message;
} RuntimeError;

/* The descriptor, which is what errors_as matches on. Its name is "Error" and
 * its package is "runtime", so it prints as runtime.Error. */
extern const Type *const TYPE_RUNTIME_ERROR;

/* How long a runtime error's message can be, and where it lives.
 *
 * Both halves of that sentence are the same fact. The message is built out of
 * the numbers that caused the failure, so it cannot be a literal, and the frame
 * that would hold it is a frame the panic jumps past. So it is built in a slot
 * on the goroutine instead, which is still there when the catch block runs, and
 * the slot is a fixed size because nothing on this path is allowed to allocate:
 * running out of memory is one of the conditions that eventually arrives here
 * and a reporting path that needs an allocator stops working exactly when it is
 * needed. A message longer than this is truncated rather than growing.
 *
 * One slot per goroutine, and the next runtime error on the same goroutine
 * writes over it. So a message you intend to keep past the catch block is a
 * message to copy, which is the rule every borrowed Str in burrow follows and
 * the same thing panic_text says about its own scratch. The window is wide
 * enough for anything normal: it closes on the next index out of range on this
 * goroutine and not before. */
#define BURROW_RUNTIME_ERROR_MAX 128

/* Go's recover().(runtime.Error), which is the question "did the runtime stop
 * this, or did the program".
 *
 * Hand it what a catch block caught. It gives you the runtime error inside, or
 * NULL for a panic that is anything else, including a panic with an ordinary
 * Error. It borrows from v, and v's message borrows from the goroutine. */
BURROW_BORROWS(ret, v) const RuntimeError *runtime_error_from(Any v);

/* Stop the program, the way Go's runtime.throw does: print
 *
 *     fatal error: <msg>
 *
 * to standard error and leave. Not recoverable, not catchable, and deliberately
 * not an Error return, because every caller of this has already established that
 * the program's state is not what the program believes it to be.
 *
 * You are welcome to call it from your own code if you have a condition of the
 * same kind, but if you are reaching for it to report a bad argument, return an
 * Error instead, and if the caller could reasonably want to carry on, the
 * function under this one is the one you want. */
BURROW_NORETURN void runtime_throw(Str msg);

/* Panic with a RuntimeError carrying msg, which is what every check below does
 * and what Go's runtime does for the conditions it lets you recover from.
 *
 * The bytes are copied, so msg may point into the calling frame, and that is
 * the reason this exists as a function rather than as panic with an error each
 * caller built for itself. A panic does not come back, so the frame that
 * formatted the message is gone by the time a catch block reads it, and a
 * message built out of the numbers that caused the failure is the only kind
 * worth having. The copy goes somewhere that outlives the jump. See
 * BURROW_RUNTIME_ERROR_MAX for how long it may be and how long it lasts.
 *
 * Call it for the same class of condition burrow calls it for: the caller asked
 * for something that cannot be done because the caller's own belief about its
 * state was wrong. A bad argument from a user is an Error return, not this. */
BURROW_NORETURN void runtime_panic(Str msg);

/* The specific ones. Each says exactly what Go says, because the text is what
 * people search for when they hit it, and because Go's tests compare it. All
 * four panic, so all four can be caught with BURROW_TRY, and an uncaught one
 * prints
 *
 *     panic: runtime error: index out of range [5] with length 3
 *
 * and ends the process with status 2, which is Go down to the prefix. */

/* runtime error: index out of range [i] with length len */
BURROW_NORETURN void runtime_index_out_of_range(Int i, Int len);

/* runtime error: slice bounds out of range [lo:hi] with capacity cap */
BURROW_NORETURN void runtime_slice_bounds_out_of_range(Int lo, Int hi, Int cap);

/* runtime error: integer divide by zero
 *
 * Every division in burrow/num.h checks its divisor first, because the bare
 * instruction faults on x86 and a fault arrives as a signal with no message in
 * it. Go prints this line and so does this. */
BURROW_NORETURN void runtime_integer_divide_by_zero(void);

/* runtime error: negative shift amount
 *
 * Go's shift count is a count rather than a direction, so a negative one is a
 * bug in the caller rather than a shift the other way. The message does not
 * carry the number, because Go's does not either. */
BURROW_NORETURN void runtime_negative_shift(void);

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

/* ---------------------------------------------------------- stack walking
 *
 * Go's runtime.Callers: fill pcs with the return addresses of the frames above
 * this call, innermost first, and answer how many went in.
 *
 * skip counts the same frames Go counts. Zero is the frame for runtime_callers
 * itself, one is whoever called it, two is that function's caller, and a trace
 * meant for a person to read normally starts at one. A skip past the bottom of
 * the stack writes nothing and answers zero rather than failing.
 *
 *     Uintptr buf[32];
 *     Slice pcs = slice_from(buf, 32, 32, TYPE_UINTPTR);
 *     Int n = runtime_callers(1, pcs);
 *
 * pcs has to be a slice of Uintptr and the element type is not checked, for the
 * same reason nothing else on this path allocates or validates: one of the
 * callers is a program that is already failing.
 *
 * What you get back is addresses and not names. Turning one into a function, a
 * file and a line is symbolisation, and the table that does it is generated
 * when the amalgamation is built, which has not happened yet. So Caller,
 * CallersFrames and FuncForPC are not here alongside this yet either. An
 * address on its own is still worth having: it is what addr2line and atos take,
 * and it is what an uncaught panic prints under the goroutine line.
 *
 * Zero frames is a real answer rather than an error. It is what an architecture
 * burrow has no frame layout for gives, and what a build with frame pointers
 * omitted gives on the architectures that need them. burrow's own Makefile
 * passes -fno-omit-frame-pointer, and a project that wants tracebacks out of
 * the amalgamation wants that flag too. */
Int runtime_callers(Int skip, Slice pcs);

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
