/* Cancellation, deadlines and request scoped values.
 *
 * This is Go's context package. It is the thing a server hands down through
 * every layer so that when the client hangs up, the database query, the two
 * outbound requests and the retry loop underneath it all stop instead of
 * finishing work nobody is waiting for. Go's own library takes one of these as
 * the first argument to almost everything that can block, and so does burrow's.
 *
 *     static void work(void *env) {
 *         Context ctx = *(Context *)env;
 *         bool ok;
 *         while (!chan_try_recv(context_done(ctx), NULL, &ok))
 *             step();
 *     }
 *
 *     CancelFunc cancel;
 *     Context ctx = context_with_cancel(a, context_background(), &cancel);
 *     go(BURROW_FN(Func, work, &ctx));
 *     ...
 *     BURROW_CALLF0(cancel);
 *     context_free(ctx);
 *
 * A Context is two words and is passed by value, the same shape an Error or any
 * other interface value has here. The zero value is Go's nil context, and
 * nothing accepts one: a function that needs a context and has none should say
 * context_todo, so that the next person reading it knows the difference between
 * "no cancellation wanted" and "nobody has threaded one through yet".
 *
 * Three things differ from Go, all of them because there is no collector. Every
 * constructor takes an allocator and what it returns has to be given back with
 * context_free. A context has to be freed before the ones derived from it, since
 * a child holds a pointer to its parent for the value lookup. And the done
 * channel is made when the context is rather than when somebody first asks for
 * it, because context_done has no allocator and no way to report a failure, so a
 * WithCancel costs a channel whether or not anybody waits on it and in exchange
 * context_done never fails. An arena user can ignore the first two.
 *
 * Copyright 2026 The burrow Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style licence that can be found
 * in the LICENSE file. */

#ifndef BURROW_CONTEXT_H
#define BURROW_CONTEXT_H

#include "burrow/chan.h"
#include "burrow/core.h"
#include "burrow/error.h"
#include "burrow/func.h"
#include "burrow/iface.h"
#include "burrow/mem.h"
#include "burrow/own.h"
#include "burrow/time.h"
#include "burrow/type.h"

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct ContextVT ContextVT;

/* The interface value. A vtable and a pointer, passed and returned by value,
 * and a zeroed one is Go's nil context.
 *
 * data is not const, unlike an Error's, because a context is a thing with state
 * in it: cancelling one takes its lock, writes its error and closes its
 * channel. An Error is read and never written, and this is the other case. */
typedef struct Context {
    const ContextVT *vt;
    void *data;
} Context;

/* Go's four methods, in Go's order.
 *
 * Every slot is required. Go's Context is an interface with four methods and a
 * type that implements three of them does not implement Context, so there is no
 * optional slot here to leave NULL. That is different from ErrorVT, where Go
 * discovers Unwrap, Is and As by type assertion and their absence is normal.
 *
 * self is not const for the reason Context.data is not. */
struct ContextVT {
    /* The concrete type behind data, for a caller that wants its own context
     * back out of one. May be NULL for a type that does not want to be
     * extracted, which is what an unexported type gets you in Go. */
    const Type *self_type;

    /* Go's Deadline() (deadline time.Time, ok bool).
     *
     * False means there is no deadline and *when is not written. True means
     * *when is the instant the work should be given up on, as a reading of the
     * monotonic clock in nanoseconds, comparable with burrow_nanotime.
     *
     * Go's is a time.Time and this will be one too once burrow has the calendar
     * half of the time package. Until then it is the monotonic reading, which is
     * the part every caller actually uses: a deadline is compared against now
     * and subtracted from now, and both of those want the clock that cannot go
     * backwards. */
    bool (*deadline)(void *self, int64_t *when);

    /* Go's Done() <-chan struct{}.
     *
     * The channel is closed when the work should stop. NULL means this context
     * is never cancelled, which is what Background is, and a NULL channel
     * blocks forever and is never ready, exactly as Go's nil channel does. So
     * a select on context_done handles both cases without asking which one it
     * got.
     *
     * Returns the same channel every time and must not fail. Never send on it
     * and never close it: it belongs to the context. */
    Chan *(*done)(void *self);

    /* Go's Err() error.
     *
     * No error while the done channel is open. Afterwards it is
     * context_canceled if somebody cancelled, or context_deadline_exceeded if
     * the deadline went by, and it never changes again. */
    Error (*err)(void *self);

    /* Go's Value(key any) any.
     *
     * A nil Any means no value for that key. Walks up to the parent, so a
     * lookup costs one comparison per context between here and wherever the
     * value was put in. */
    Any (*value)(void *self, Any key);
};

/* Is this the nil context. Write this rather than comparing against anything,
 * for the reason BURROW_FAILED exists: C has no == on structs. */
#define BURROW_CONTEXT_IS_NIL(c) ((c).vt == NULL)

/* The descriptor for Go's context.Context, so that an Any can hold one and a
 * struct with a context field in it can be described.
 *
 * It is here rather than with the builtins in burrow/type.h because the
 * descriptor needs sizeof(Context), and type.h comes before Context exists. */
extern const Type *const TYPE_CONTEXT;

/* context.Canceled, returned by Err after a cancel function was called. */
extern const Error context_canceled;

/* context.DeadlineExceeded, returned by Err after a deadline went by.
 *
 * In Go this satisfies net.Error with Timeout() true, so that code written
 * against the network package treats it as a timeout rather than as a hard
 * failure. burrow has no net package yet and this is a plain sentinel until it
 * does. */
extern const Error context_deadline_exceeded;

/* Go's context.CancelFunc.
 *
 * An alias for Func rather than a type of its own, so that the usual things
 * work on it without a conversion: BURROW_DEFER takes a Func, and a cancel that
 * has to be handed to something else does not need a wrapper.
 *
 * Calling it more than once is fine and does nothing after the first. Calling
 * it is not optional: until it runs, the context is still attached to its
 * parent and the parent still has a pointer to it. Go says the same thing and
 * has a vet check for the case where nobody calls it. */
typedef Func CancelFunc;

/* Go's context.CancelCauseFunc, the cancel that takes a reason with it.
 *
 *     BURROW_CALLF(cancel, err_too_slow);
 *
 * Not an alias for anything, because it takes an argument. Pass BURROW_NO_ERROR
 * to say nothing in particular went wrong, which is what a plain CancelFunc
 * does, and then context_cause answers context_canceled like context_err.
 *
 * The same rules as a CancelFunc otherwise: calling it more than once does
 * nothing after the first, the first cause is the one that sticks, and calling
 * it at all is not optional. */
BURROW_FUNC(CancelCauseFunc, void, Error cause);

/* context.Background(). The root, which is never cancelled, has no deadline and
 * carries no values.
 *
 * Costs nothing to make, needs no allocator and needs no free. It is a vtable
 * pointer and a NULL, and every call returns the same two words. */
Context context_background(void);

/* context.TODO(). Background with a note attached to it.
 *
 * Behaves identically and exists to be different from Background when somebody
 * reads the code: this is the call that says a context should be threaded
 * through here and has not been yet. Go's static analysis tells them apart and
 * a future burrow linter can too, because the vtables differ. */
Context context_todo(void);

/* The four methods, as functions, which is how a caller uses them.
 *
 * Each one is a load and an indirect call, the same as calling a method on a Go
 * interface value. They stop the program on a nil Context rather than crashing
 * in the indirect call, because a nil context is a bug in the caller and the
 * message is worth more than the segfault. */
bool context_deadline(Context c, int64_t *when);
BURROW_BORROWS(ret, c) Chan *context_done(Context c);
BURROW_BORROWS(ret, c) Error context_err(Context c);
BURROW_BORROWS(ret, c) Any context_value(Context c, Any key);

/* context.WithCancel(parent).
 *
 * Returns a copy of parent that is also cancelled when the returned function is
 * called, and writes that function to *cancel.
 *
 * A nil Context is returned when the allocator cannot give out the context and
 * its done channel, and *cancel is then a function that does nothing, so that a
 * caller who defers the cancel before checking the context is still correct.
 * Check with BURROW_CONTEXT_IS_NIL.
 *
 * Cancelling the parent cancels this and everything below it, and the cancel
 * function detaches this from the parent so that a long lived parent does not
 * accumulate children forever. That is the whole reason the cancel has to be
 * called even on a path where the work finished normally.
 *
 * Panics on a nil parent, with Go's message. */
BURROW_OWNS(ret) Context context_with_cancel(Alloc *a, Context parent,
                                             CancelFunc *cancel);

/* context.WithCancelCause(parent).
 *
 * WithCancel with a reason attached to the cancel:
 *
 *     CancelCauseFunc cancel;
 *     Context ctx = context_with_cancel_cause(a, parent, &cancel);
 *     ...
 *     BURROW_CALLF(cancel, err_upstream_gone);
 *
 * and then context_err(ctx) is context_canceled, the way it is for every
 * cancelled context, while context_cause(ctx) is err_upstream_gone. The point
 * is that context_err has to stay one of two values for code that switches on
 * it, so the interesting error goes somewhere else.
 *
 * The cause reaches everything below this context, so a handler five layers
 * down finds out why it was stopped without anybody threading the reason
 * through. That is the whole feature.
 *
 * Cancelling with BURROW_NO_ERROR is the same as a plain cancel and leaves the
 * cause equal to context_canceled. A second call changes nothing, including the
 * cause: the first reason is the real one.
 *
 * Everything else is context_with_cancel, including the nil context on a
 * refused allocation, the do nothing cancel that goes with it, and the panic on
 * a nil parent. */
BURROW_OWNS(ret) Context context_with_cancel_cause(Alloc *a, Context parent,
                                                   CancelCauseFunc *cancel);

/* context.Cause(c). Why this context was cancelled, as opposed to context_err,
 * which says only that it was.
 *
 * The answer is BURROW_NO_ERROR while the context is live. Afterwards it is
 * whatever was handed to a CancelCauseFunc or to one of the WithCause
 * constructors, and when nobody supplied a reason it is the same thing
 * context_err says: context_canceled or context_deadline_exceeded.
 *
 * It reads the nearest cancellable context at or above c, which is what makes a
 * reason set at the top visible at the bottom. A context with no cancellable
 * ancestor answers context_err(c), and one made by context_without_cancel
 * answers nothing at all, since that is the point of it.
 *
 * Stops the program on a nil Context, like the four methods. */
BURROW_BORROWS(ret, c) Error context_cause(Context c);

/* context.WithoutCancel(parent).
 *
 * A copy of parent that keeps its values and is never cancelled, for the work
 * that has to finish after the request it belongs to has gone: writing the
 * access log line, flushing a metric, finishing a database transaction that
 * would otherwise be rolled back.
 *
 *     Context bg = context_without_cancel(a, ctx);
 *     go(BURROW_FN(Func, write_the_log_line, &bg));
 *
 * context_done answers NULL, context_err answers nothing, context_deadline says
 * there is none, and a value lookup walks through into the parent as usual.
 * Cancelling the parent does not reach here, and neither does its deadline.
 *
 * context_cause answers nothing here too, rather than the reason the parent
 * stopped, which is Go's behaviour and is the consistent one: a context that is
 * not cancelled has no reason for being cancelled.
 *
 * Nothing about the parent's lifetime changes. The node holds a pointer to it
 * for the value walk, so the parent still has to outlive this, and the usual
 * rule of freeing children first still applies.
 *
 * A nil Context is returned when the allocator says no. Panics on a nil
 * parent. */
BURROW_OWNS(ret) Context context_without_cancel(Alloc *a, Context parent);

/* context.WithDeadline(parent, when).
 *
 * Returns a copy of parent that is cancelled when the returned function is
 * called, when the parent is cancelled, or when the clock reaches when,
 * whichever happens first. Err is context_deadline_exceeded in the last case
 * and context_canceled in the other two.
 *
 * when is a reading of the monotonic clock in nanoseconds, the same thing
 * context_deadline hands back and the same thing burrow_nanotime returns, so a
 * deadline three seconds out is burrow_nanotime() + 3 * TIME_SECOND. Go takes a
 * time.Time and this takes one too once burrow has the calendar half of the time
 * package. Most callers want context_with_timeout below and never write this.
 *
 * A deadline that has already gone by cancels the context before this returns,
 * which is Go's rule: what comes back is a real context whose done channel is
 * already closed rather than nothing at all. A parent that gives up sooner is
 * left to do the job, and then this is context_with_cancel with no timer in it.
 *
 * Call it from a goroutine. The timer goes into the heap of the P the caller is
 * on and a thread the runtime did not start has no P, which is the same rule
 * time_after_func has. Calling it from anywhere else stops the program, on
 * every path and not only on the one that arms a timer.
 *
 * Calling the cancel function is not optional here either, and it is the thing
 * that stops the timer. A context left to reach its deadline is cheaper than a
 * timer per request that nobody ever disarms.
 *
 * A nil Context is returned when the allocator will not give out the context,
 * its done channel or the timer, and *cancel is then a function that does
 * nothing. Panics on a nil parent, with Go's message. */
BURROW_OWNS(ret) Context context_with_deadline(Alloc *a, Context parent, int64_t when,
                                               CancelFunc *cancel);

/* context.WithTimeout(parent, d). The deadline measured from now, which is what
 * almost every caller has:
 *
 *     CancelFunc cancel;
 *     Context ctx = context_with_timeout(a, parent, 5 * TIME_SECOND, &cancel);
 *     if (BURROW_CONTEXT_IS_NIL(ctx))
 *         return err_no_memory;
 *     ...
 *     BURROW_CALLF0(cancel);
 *     context_free(ctx);
 *
 * A duration of zero or less is a deadline in the past, so the context comes
 * back already cancelled rather than never firing. A duration long enough to run
 * off the end of an int64 is clamped to the last instant there is, about 292
 * years out, which is nobody's timeout and is still not a wrong answer.
 *
 * Everything else is context_with_deadline. */
BURROW_OWNS(ret) Context context_with_timeout(Alloc *a, Context parent, Duration d,
                                              CancelFunc *cancel);

/* context.WithDeadlineCause(parent, when, cause) and
 * context.WithTimeoutCause(parent, d, cause).
 *
 * The same two constructors with a reason attached to the deadline, so that
 * code underneath can tell one timeout from another:
 *
 *     Context ctx = context_with_timeout_cause(a, parent, 2 * TIME_SECOND,
 *                                              err_database_too_slow, &cancel);
 *
 * and when those two seconds go by, context_err(ctx) is
 * context_deadline_exceeded and context_cause(ctx) is err_database_too_slow.
 *
 * The cause is only used when the deadline is what stops the context. A cancel
 * that gets there first leaves the cause at context_canceled, and so does a
 * parent cancelling from above, because in neither case was the deadline the
 * reason. Pass BURROW_NO_ERROR to get exactly context_with_deadline.
 *
 * Note that the cancel function here is a plain CancelFunc rather than a
 * CancelCauseFunc. That is Go's shape: the cause is fixed when the context is
 * made, and the caller who wants both a reason for the deadline and a reason
 * for the cancel builds this on top of context_with_cancel_cause.
 *
 * Everything else is context_with_deadline and context_with_timeout, including
 * the requirement to call these from a goroutine. */
BURROW_OWNS(ret) Context context_with_deadline_cause(Alloc *a, Context parent,
                                                     int64_t when, Error cause,
                                                     CancelFunc *cancel);

BURROW_OWNS(ret) Context context_with_timeout_cause(Alloc *a, Context parent,
                                                    Duration d, Error cause,
                                                    CancelFunc *cancel);

/* context.WithValue(parent, key, val).
 *
 * Returns a copy of parent that answers key with val. Use it for values that
 * cross an API boundary and belong to the request rather than to the call: a
 * request id, an authenticated user, a trace span. Not for passing arguments,
 * which is what arguments are for.
 *
 * The key has to be comparable, since looking a value up compares keys, and
 * nothing here copies what the two Any values point at. Both of them have to
 * outlive the context, which for the usual case of a static key and a value
 * owned by the request is already true. any_box is how to make a value that
 * would not be.
 *
 * A key should be of a type private to whoever puts the value in, so that no
 * two packages can collide on one. In Go that is an unexported named type; here
 * it is a Type descriptor defined static in your own .c file, and the effect is
 * the same because a key comparison compares descriptors first.
 *
 * A nil Context is returned when the allocator says no. Panics on a nil parent
 * and on a nil or uncomparable key, all three with Go's messages. */
BURROW_OWNS(ret) Context context_with_value(Alloc *a, Context parent, Any key, Any val);

/* Gives a context made by one of the With functions back to its allocator, and
 * leaves the Context dangling, so it is the last thing done with one.
 *
 * Go has no such call. The rule is burrow's usual one: whoever made it frees
 * it, and an arena user can skip this entirely.
 *
 * Cancels first, so this is safe on a context nobody cancelled and so that
 * anything still waiting on the done channel is released rather than left
 * parked on freed memory. That means it is not a substitute for calling the
 * cancel function at the right moment, only a guarantee that forgetting to does
 * not corrupt anything.
 *
 * Free a context before its children. A child holds a pointer to its parent,
 * and while cancelling detaches a child from a cancellable parent, a value
 * context is not cancellable and has nothing to detach from.
 *
 * Background, TODO and the nil context are fine to pass and do nothing. A
 * context this package did not make stops the program, because the alternative
 * is freeing a pointer to something whose shape is unknown. */
void context_free(Context c);

#ifdef __cplusplus
}
#endif

#endif /* BURROW_CONTEXT_H */
