/* Errors, which is how every fallible function in this library answers.
 *
 * Go's error is an interface with one method, and the whole convention is built
 * out of that one method plus three functions in the errors package. burrow
 * keeps the shape:
 *
 *     Error err = os_write_file(path, data, 0644);
 *     if (BURROW_FAILED(err))
 *         printf("nope: " BURROW_STR_FMT "\n", BURROW_STR_ARG(error_text(err)));
 *
 * An Error is a vtable pointer and a data pointer, the same two words an
 * interface value is anywhere else, and the zero value means no error. So a
 * function that returns Error costs nothing to succeed, and a struct with an
 * Error field in it starts out with no error in it without anybody saying so.
 *
 * Unwrap, Is and As are slots in the vtable rather than method sets discovered
 * by type assertion. Go looks them up dynamically every time; here they are
 * either there or NULL, which is a load and a branch instead of an interface
 * conversion. From the outside errors_is, errors_as and errors_unwrap behave the
 * way Go's do, including the Unwrap() []error trees that errors.Join builds.
 *
 * Error paths must never be able to fail, and constructing an error message
 * needs memory, so everything here is arranged so that the important cases need
 * none. A sentinel is a static const object in read only memory: io_eof costs
 * nothing to create, nothing to compare and nothing to clean up. Only
 * errors_new and errors_join allocate, and both of them fall back to a static
 * error if the allocator says no, so a failed allocation on an error path is
 * never a crash and never a silently empty error.
 *
 * This is its own header for the same reason slice.h is: an Error's vtable
 * names a Type, errors_join takes a Slice, and a header cannot come before the
 * one it needs. Include burrow/burrow.h and the order is somebody else's
 * problem.
 *
 * Copyright 2026 The burrow Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style licence that can be found
 * in the LICENSE file. */

#ifndef BURROW_ERROR_H
#define BURROW_ERROR_H

#include "burrow/core.h"
#include "burrow/mem.h"
#include "burrow/mem/arena.h"
#include "burrow/slice.h"
#include "burrow/type.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Error itself, and BURROW_NO_ERROR, BURROW_FAILED and BURROW_OK with it, are
 * in burrow/core.h. The value is two pointers and depends on nothing, an
 * ErrorVT names a Type, and type.h has functions that report an error, so the
 * value had to be below all three of them. The comment there says the same
 * thing from the other side. */

struct ErrorVT {
    /* The concrete type behind data, which is what errors_as matches on. It may
     * be NULL for an error type that does not want to be extracted, which is
     * the same thing an unexported type gets you in Go. */
    const Type *self_type;

    /* Go's Error() string. Required, and the only required slot.
     *
     * It does not take an allocator, so it cannot build its message on demand
     * the way Go's can. Anything with a message worth computing computes it at
     * construction time, where there is an allocator in hand, and this returns
     * what it stored. That costs an allocation Go might not have paid if nobody
     * printed the error, and it buys a printing path that cannot fail. */
    Str (*message)(const void *self);

    /* Go's Unwrap() error. NULL means this error does not wrap anything, which
     * is how most of them are. */
    Error (*unwrap)(const void *self);

    /* Go's Unwrap() []error, the multi error form that errors.Join returns.
     * NULL for everything that is not a tree. The result is a Slice of Error
     * and it borrows, so it lives as long as the error does.
     *
     * Set one of unwrap and unwrap_multi, not both. A Go type cannot satisfy
     * both because it has one method of that name, so there is no Go behaviour
     * to copy if you do it anyway. What you get is the chain form, because that
     * is the first case in Go's type switch, and unwrap_multi is then never
     * called at all. */
    Slice (*unwrap_multi)(const void *self);

    /* Go's optional Is(error) bool, for an error that wants to match something
     * it is not identical to. NULL is the common case and means identity. */
    bool (*is)(const void *self, Error target);

    /* Go's optional As(any) bool, for an error that wants to be extracted as a
     * type it is not. Return the pointer to hand back, or NULL to decline.
     * NULL in the slot means self_type comparison, which is the common case. */
    const void *(*as)(const void *self, const Type *target);

    /* A copy of this error in a, deep enough that nothing in it points back at
     * the original's memory, for error_retain. Copy what the error wraps with
     * error_retain as well. NULL is fine for an error whose memory never goes
     * away, which is every sentinel, and error_retain falls back to copying the
     * message and the chain for everything else, which keeps errors_is working
     * and loses errors_as. Give an error with a self_type a clone and it keeps
     * both. */
    Error (*clone)(const void *self, Alloc *a);
};

/* The descriptor for Go's error, which is a builtin interface type.
 *
 * It belongs with the other builtins in burrow/type.h and it cannot go there,
 * because the descriptor needs sizeof(Error) and type.h comes before Error
 * exists. So it is declared here beside the type it describes.
 *
 * You need it to build the Slice that errors_join takes, and reflect needs it
 * to describe a struct with an error field in it, which is most of them. */
extern const Type *const TYPE_ERROR;

/* Success, and the test for it.
 *
 * Write BURROW_FAILED rather than comparing against BURROW_NO_ERROR, because C
 * has no == on structs and the version people write by hand compares data too,
 * which is wrong for any error whose data pointer happens to be NULL. */
#define BURROW_NO_ERROR ((Error){NULL, NULL})
#define BURROW_FAILED(e) ((e).vt != NULL)
#define BURROW_OK(e) ((e).vt == NULL)

/* err.Error().
 *
 * Named message rather than error_error, which is the only place in the library
 * where a method's name is its receiver type's name. It borrows: the result
 * points into the error and lives as long as the error does.
 *
 * No error gives you the empty string rather than stopping the program, because
 * this gets called from log lines and a logging call that can take the process
 * down is worse than a blank message. Test with BURROW_FAILED first if the
 * difference matters. */
BURROW_BORROWS(ret, err) Str error_text(Error err);

/* errors.New.
 *
 * The text is copied, so the result does not depend on where the text came
 * from. Two calls with the same text give two different errors that are not
 * errors_is each other, which is Go's behaviour and is the reason sentinels are
 * package level variables rather than constructed at the point of use.
 *
 * A failed allocation gives you burrow_err_out_of_memory, so this never returns
 * a success by accident. */
BURROW_OWNS(ret) Error errors_new(Alloc *a, Str text);

/* errors.Unwrap. No error and an error that does not wrap both give you no
 * error, which is Go returning nil in both cases.
 *
 * A multi error unwraps to nothing here, exactly as in Go: errors.Unwrap is
 * defined over Unwrap() error and says nothing about the tree form. Walk it
 * with errors_is or errors_as, or call the vtable slot yourself. */
BURROW_BORROWS(ret, err) Error errors_unwrap(Error err);

/* errors.Is. Walks the chain and the tree, asking each error whether it matches
 * and then asking it what it wraps.
 *
 * Identity here is both words equal, which is what Go's == on two interface
 * values does. Since a sentinel's data points at its own static object, every
 * sentinel is distinct from every other one without anybody assigning ids.
 *
 * errors_is(err, BURROW_NO_ERROR) is true only when err is also no error, which
 * is Go's errors.Is(nil, nil). */
bool errors_is(Error err, Error target);

/* errors.As, spelled the way C wants it.
 *
 * Go takes a pointer to a variable and returns a bool. This returns the
 * pointer, because in C the pointer is the bool and there is nothing to
 * assign through:
 *
 *     const OsPathError *pe = errors_as(err, TYPE_OS_PATH_ERROR);
 *     if (pe != NULL)
 *         printf("failed on " BURROW_STR_FMT "\n", BURROW_STR_ARG(pe->path));
 *
 * void * converts to any object pointer in C, so there is no cast at the call
 * site and no way to ask for one type and get another. It also drops both of
 * Go's panics, since there is no nil target and no non pointer target to
 * complain about.
 *
 * That makes it Go 1.26's errors.AsType as well, which exists because Go's As
 * has the awkward shape and this one never did. There is no second function
 * for it, since it would be this one again under a longer name. */
BURROW_BORROWS(ret, err) const void *errors_as(Error err, const Type *target);

/* errors.Join.
 *
 * errs is a Slice of Error. The ones that are no error are dropped, and if
 * every one of them is, the result is no error, which is Go returning nil for
 * Join(nil, nil). One survivor still gets wrapped in a tree of one, because
 * Go's does and because errors_is has to keep giving the same answer whether
 * the caller filtered first.
 *
 * The message is the surviving messages joined with a newline, built now rather
 * than when somebody prints it, for the reason written over the message slot.
 *
 * A failed allocation gives you burrow_err_out_of_memory. */
BURROW_OWNS(ret) Error errors_join(Alloc *a, Slice errs);

/* Join without building a Slice first, which is what most call sites want:
 *
 *     Error err = errors_join_v(a, 2, first, second);
 *
 * n is the count because C variadics cannot be counted at runtime. Getting it
 * wrong reads past the arguments, the same way a wrong printf format does. */
BURROW_OWNS(ret) Error errors_join_v(Alloc *a, int n, ...);

/* ------------------------------------------------------ where errors live
 *
 * A function that fails in Go returns an error it made on the spot, and the
 * garbage collector deals with it later. strconv_atoi has no allocator to make
 * one from, and should not need one, so the runtime keeps an arena for errors
 * and hands it out here. There is one per goroutine, and one per thread for
 * code that is not running on a goroutine.
 *
 *     Error err = errors_new(error_allocator(), BURROW_S("no such thing"));
 *
 * An error made from it lives until one of two things happens: the goroutine
 * that made it ends, or a mark taken before it was made is released. That is
 * what a long running loop uses to stop its errors from piling up:
 *
 *     for (;;) {
 *         ArenaMark m = error_mark();
 *         Error err = handle(next());
 *         if (BURROW_FAILED(err))
 *             log_error(err);
 *         error_release(m);
 *     }
 *
 * A release that is skipped, because of an early return or a panic, only means
 * the memory is held until an outer release or the end of the goroutine. It
 * never leaves anything pointing at freed memory that was not already going to.
 *
 * An error that has to outlive both, because it is kept in a struct or sent to
 * another goroutine that may outlive this one, goes through error_retain on the
 * way out. */

/* The allocator for the calling goroutine's errors, or the calling thread's
 * when it is not running one. Never NULL. Use it from the goroutine it belongs
 * to and nowhere else, since it takes no lock. */
BURROW_BORROWS(ret) Alloc *error_allocator(void);

/* Where the calling goroutine's error arena is now, and back to there. Take and
 * release a mark on the same goroutine. Marks nest like arena marks do, which
 * is what they are. */
ArenaMark error_mark(void);
void error_release(ArenaMark m);

/* A copy of err in a that does not depend on the error arena or on anything
 * else err pointed at. Sentinels come back as they are, since they are already
 * immortal and errors_is compares them by address. An error whose vtable has a
 * clone slot is copied by it. Anything else becomes an error with the same
 * message that wraps a retained copy of what the original wrapped, so errors_is
 * still finds the sentinels in the chain, and errors_as no longer finds the
 * original's type.
 *
 * A failed allocation gives you burrow_err_out_of_memory. */
BURROW_OWNS(ret) Error error_retain(Alloc *a, Error err);

/* errors.ErrUnsupported, which Go added so that a caller can ask whether an
 * operation is unavailable rather than whether it failed. */
extern const Error errors_err_unsupported;

/* Not a Go symbol, which is why it says burrow rather than errors.
 *
 * Returned by errors_new and errors_join when the allocator cannot give them
 * the few bytes they need. It is static, so it is always available, which is
 * the whole point: the failure mode of an error constructor cannot itself be a
 * failure to construct an error.
 *
 * The plan in docs/design/05-memory.md is for every allocator to keep a
 * reserved block so that this is unreachable in practice. That reserve is not
 * built yet, so today this is reachable and a caller under memory pressure will
 * see it. */
extern const Error burrow_err_out_of_memory;

/* Declares a sentinel error: a static, immortal, allocation free error whose
 * message is a string literal. Put this in a .c file:
 *
 *     BURROW_SENTINEL_ERROR(io_eof, "EOF");
 *
 * and the matching line in the header:
 *
 *     extern const Error io_eof;
 *
 * There are several hundred of these across Go's library and every one of them
 * is compared far more often than it is printed, so the comparison has to be
 * two pointer loads and nothing else. It is, because the expansion is a const
 * struct the linker fills in and errors_is on it does no work beyond comparing
 * the words.
 *
 * All the sentinels share one vtable, so each one costs a Str and two words of
 * rodata and no code at all.
 *
 * The Str is spelled out in braces rather than with BURROW_S, because BURROW_S
 * is a compound literal and C11 does not accept one of those as the initialiser
 * for an object with static storage. gcc says so under -Wpedantic and it is
 * right. Only ever hand this a string literal, the same as BURROW_S. */
#define BURROW_SENTINEL_ERROR(name, text)                                              \
    static const Str name##__text = {(const Byte *)("" text),                          \
                                     (Int)(sizeof(text) - 1)};                         \
    const Error name = {&burrow_sentinel_error_vt, &name##__text}

/* What the macro points at. Public because the macro expands in your file and
 * has to be able to name it, not because you should be filling it in yourself.
 * Its data is a const Str * and its message returns that Str. */
extern const ErrorVT burrow_sentinel_error_vt;

#if defined(BURROW_SHORT) && BURROW_SHORT
#define NO_ERROR BURROW_NO_ERROR
#define FAILED(e) BURROW_FAILED(e)
#define OK(e) BURROW_OK(e)
#endif

#ifdef __cplusplus
}
#endif

#endif /* BURROW_ERROR_H */
