/* Copyright 2026 The burrow Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style licence that can be found
 * in the LICENSE file. */

/* defer: run this when you leave, whichever way you leave.
 *
 *     BURROW_SCOPE {
 *         OsFile *f = os_open(a, path, &err);
 *         if (BURROW_FAILED(err))
 *             return err;
 *         BURROW_DEFER(os_file_close, f);
 *
 *         ...
 *     }
 *     BURROW_SCOPE_END;
 *
 * The close runs when control leaves the block, and it does not matter how
 * control leaves: falling off the end, `return`, `break`, `continue`, a `goto`
 * out, or a panic once panic lands. That is the whole feature, and it is worth
 * having in C for the same reason it is worth having in Go: the line that opens
 * a thing and the line that closes it are next to each other, so the reader can
 * see both at once and a new early return added later cannot forget the second
 * one.
 *
 * ---------------------------------------------------------------- the block
 *
 * The two halves are not decoration and they are not the same block. BURROW_SCOPE
 * opens a block of its own, your braces open another inside it, and
 * BURROW_SCOPE_END closes the outer one. The reason there is an outer one is
 * that the bookkeeping has to be declared before your first statement and a
 * macro cannot reach inside braces that come after it.
 *
 * BURROW_SCOPE_END goes on its own line because that is where clang-format puts
 * it, and the tree is formatted by clang-format rather than by argument.
 *
 * BURROW_DEFER only compiles inside a BURROW_SCOPE, on every compiler, and that
 * is deliberate rather than incidental. The alternative was a bare BURROW_DEFER
 * that works on GCC and Clang, which have the cleanup attribute, and quietly
 * never runs on MSVC, which does not. A feature that silently does nothing on
 * one of three supported platforms is worse than one that asks for an extra
 * line on all three.
 *
 * Underneath it is the cleanup attribute on GCC and Clang and __try/__finally
 * on MSVC, both of which handle `return` out of the middle correctly, which is
 * the only property that matters here.
 *
 * ------------------------------------------------- where this differs from Go
 *
 * Go's defer runs at function return. This one runs at scope exit, and there is
 * no way to have the first in C without wrapping every function body in
 * something. If you want Go's rule, put the scope around the whole body:
 *
 *     static Error handle(Conn *c) {
 *         BURROW_SCOPE {
 *             ...
 *         }
 *         BURROW_SCOPE_END;
 *
 *         return err;
 *     }
 *
 * The difference shows up in a loop, and when it does, this rule is the one
 * people wanted. A defer inside a Go `for` body does not run until the function
 * returns, which is how a Go program ends up holding ten thousand file
 * descriptors and is a bug people write often enough that `go vet` has a check
 * for the shape of it. A defer inside a burrow scope inside a loop runs on
 * every turn.
 *
 * Everything else is Go's. Deferred calls run last in first out. The argument
 * is read when the defer is written and not when it runs, so deferring a call
 * on a pointer variable that is reassigned afterwards still uses the value it
 * had at the defer. A defer in a loop whose scope is outside the loop piles up
 * and runs at the end of the scope, which is Go's behaviour for a defer in a
 * loop and is almost never what anybody wanted. There is no way to undo a
 * defer, in either language.
 *
 * ---------------------------------------------------------------- what it costs
 *
 * A scope is one struct in your frame and the deferred calls live in it, eight
 * of them without asking anything of anybody, which is the same number Go's
 * compiler open-codes into a frame. The ninth and everything after it go in one
 * allocation from the heap allocator that doubles as it fills and is freed
 * before the scope returns, which is the same trade Go makes when it cannot
 * open-code a frame's defers.
 *
 * The calls live in the scope rather than each in its own local so that the
 * storage outlives your braces, since that is where the last of it runs. A
 * record declared between your braces is dead by then, which is a rule C has
 * always had and an address sanitizer will tell you about.
 *
 * ------------------------------------------------------------ what goes in one
 *
 * A deferred call is a Func, which is a function taking void * and the pointer
 * it was made with, so the thing being cleaned up is usually the argument:
 *
 *     BURROW_DEFER(os_file_close, f);
 *     BURROW_DEFER(mem_free_slice, &s);
 *     BURROW_DEFER_FUNC(cleanup);
 *
 * A deferred function returning something other than void does not fit and does
 * not compile. Wrap it in a small static function that ignores the result, and
 * if the result is an Error, that wrapper is the place where a reader can see
 * you decided to ignore it, which is where the decision belongs.
 *
 * A deferred call may open scopes and defer things of its own. A deferred call
 * that ends the goroutine with runtime_goexit runs the rest of the chain, the
 * same as Go.
 *
 * ---------------------------------------------------------------- goroutines
 *
 * The chain belongs to the goroutine and not to the thread, so a goroutine that
 * parks in the middle of a scope and wakes up on another thread still has its
 * defers. A thread that is not a goroutine has a chain of its own, which is
 * what makes these macros usable in code that has not started the runtime.
 */

#ifndef BURROW_DEFER_H
#define BURROW_DEFER_H

#include "burrow/core.h"
#include "burrow/func.h"
#include "burrow/own.h"
#include "burrow/platform.h"

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* How many deferred calls a scope holds before it has to allocate.
 *
 * Eight, which is also where Go's compiler stops open-coding defers into the
 * frame, and it is eight because burrow-bench measured the alternative. It was
 * four, on the argument that a scope with five cleanups in it is rare and
 * should be the one that pays. The row that came back said the fifth call cost
 * a hundred nanoseconds, because paying means a malloc and a free, while the
 * four empty slots cost sixty four bytes of a stack frame and no time at all:
 * this array is deliberately never initialised, so a slot nobody uses is
 * address space and nothing more.
 *
 * It is not a knob. The number is part of the shape of the struct below, so a
 * translation unit that changed it would disagree with the library about where
 * the fields are. */
#define BURROW_DEFER_INLINE 8

/* One scope, living in the frame that opened it.
 *
 * `calls` and `over` hold this scope's deferred calls in the order they were
 * written, oldest first, which is the order they run backwards. `n` is how many
 * there are in total and the first BURROW_DEFER_INLINE of them are in `calls`.
 * `outer` is the scope this one is nested inside on this goroutine, which is
 * what a panic walks. `chain` is where the head of that walk lives, which is a
 * field of the goroutine or a thread local when there is no goroutine, and it
 * is also the closed flag: a scope that has run its calls has NULL here and
 * running it again does nothing.
 *
 * Public because the macro has to declare one, not because there is anything to
 * do with it. Nothing outside this header reads these fields. */
typedef struct burrow__DeferScope {
    Func calls[BURROW_DEFER_INLINE];
    BURROW_OWNS(1) Func *over;
    Int n;
    Int over_cap;
    struct burrow__DeferScope *outer;
    struct burrow__DeferScope **chain;
} burrow__DeferScope;

/* Opens one, on the scope variable the macro declared, which it fills in. */
void burrow__scope_open(BURROW_RETAINS(1) burrow__DeferScope *s);

/* Runs a scope's deferred calls, newest first, and takes it off the chain.
 * Doing this twice to the same scope runs them once, which is what makes the
 * MSVC path safe: a longjmp there runs __finally blocks for frames a panic has
 * already unwound by hand. */
void burrow__scope_close(burrow__DeferScope *s);

/* Adds a call to a scope. */
void burrow__defer_push(BURROW_RETAINS(1) burrow__DeferScope *s, Func fn);

/* Runs every open scope on this goroutine, innermost first. This is what
 * runtime_goexit calls, and it is the reason Go's Goexit is not the same thing
 * as falling off the end of a goroutine in the wrong place. */
void burrow__defer_unwind_all(void);

/* Where the innermost open scope of the caller lives. The runtime uses it and
 * the tests use it to check that a scope left nothing behind. */
BURROW_BORROWS(ret) burrow__DeferScope **burrow__defer_chain(void);

/* The two halves of the block.
 *
 * The pragmas are about the scope variable having a fixed name, which it needs
 * because BURROW_DEFER names it, and which means a scope inside a scope shadows
 * one. That shadow is the correct reading and not a mistake: a defer between
 * the inner braces belongs to the inner scope. The warning has no way to know
 * that, so it is turned off for the one declaration.
 *
 * The variable is left uninitialised and filled in by the call on the next
 * line, which is the one place in this library where that is the right thing:
 * zeroing it first would write the whole array of calls, and the point of the
 * array is that a scope with one defer in it pays for one. */
#if BURROW_CC_MSVC

#define BURROW_SCOPE                                                                   \
    {                                                                                  \
        __pragma(warning(push)) __pragma(warning(disable : 4456))                      \
            burrow__DeferScope burrow__scope;                                          \
        __pragma(warning(pop)) burrow__scope_open(&burrow__scope);                     \
        __try {

#define BURROW_SCOPE_END                                                               \
    }                                                                                  \
    __finally {                                                                        \
        burrow__scope_close(&burrow__scope);                                           \
    }                                                                                  \
    }                                                                                  \
    (void)0

#else

#define BURROW_SCOPE                                                                   \
    {                                                                                  \
        _Pragma("GCC diagnostic push") _Pragma("GCC diagnostic ignored \"-Wshadow\"")  \
            __attribute__((                                                            \
                cleanup(burrow__scope_close))) burrow__DeferScope burrow__scope;       \
        _Pragma("GCC diagnostic pop") burrow__scope_open(&burrow__scope);

#define BURROW_SCOPE_END                                                               \
    }                                                                                  \
    (void)0

#endif

/* Defers a Func you already have, which is what a caller that was handed a
 * cleanup function has. */
#define BURROW_DEFER_FUNC(f) burrow__defer_push(&burrow__scope, (f))

/* Defers a call. fn takes a void * and returns nothing, arg is what it gets. */
#define BURROW_DEFER(fn, arg) BURROW_DEFER_FUNC(BURROW_FN(Func, fn, arg))

#ifdef __cplusplus
}
#endif

#endif /* BURROW_DEFER_H */
