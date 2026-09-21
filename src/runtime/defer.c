/* The defer chain.
 *
 * A few short functions and a thread local, and the header next door has the
 * long explanation. What is worth saying here is where the calls live and where
 * the chain lives, which are two different questions with two answers.
 *
 * The calls live in the scope, four of them in the scope itself and the rest in
 * one allocation hanging off it. The obvious alternative is one record per
 * deferred call declared where the defer is written, which is free and which is
 * wrong: those records are between the caller's braces and the last of the
 * calls runs after that block has ended, so by the time the scope reads them
 * their lifetime is over and the compiler is within its rights to have put
 * something else there. An address sanitizer says so out loud, which is how
 * this was found rather than shipped.
 *
 * A goroutine that parks in the middle of a scope can wake up on a different
 * thread, so a thread local chain would lose track of it: the defers would be
 * on the thread that ran the first half of the function. The chain is a field
 * of the goroutine for that reason, and following it costs one load of the
 * current goroutine per scope rather than per deferred call.
 *
 * A thread that is not a goroutine has nowhere to put one, and these macros
 * have to work before runtime_main is called and after it returns, because a
 * program that uses burrow's allocators and nothing else is a program burrow
 * supports. So there is a thread local for that case and it is only ever
 * reached by a thread the scheduler does not know about.
 *
 * Copyright 2026 The burrow Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style licence that can be found
 * in the LICENSE file. */

#include "burrow/defer.h"

#include "burrow/mem.h"
#include "burrow/mem/heap.h"
#include "burrow/runtime.h"
#include "burrow/sched.h"

#include <stddef.h>

/* The chain for a thread that is not running a goroutine. Thread local, so
 * there is no synchronisation to get wrong: the only thread that can reach it
 * is the one it belongs to, and that thread is inside the scope. */
static BURROW_THREAD_LOCAL burrow__DeferScope *thread_scopes;

/* The first size the overflow is asked for, and it is eight rather than one
 * because a scope that has gone past four calls is a scope collecting them in a
 * loop, and a loop rarely goes round once. */
#define OVER_FIRST 8

burrow__DeferScope **burrow__defer_chain(void) {
    burrow__G *g = burrow__curg();

    return g != NULL ? &g->scopes : &thread_scopes;
}

void burrow__scope_open(burrow__DeferScope *s) {
    burrow__DeferScope **chain = burrow__defer_chain();

    s->over = NULL;
    s->n = 0;
    s->over_cap = 0;
    s->outer = *chain;
    s->chain = chain;

    /* On the chain now rather than at the first push. Waiting would cost one
     * store in a scope that turns out to have no defers in it, and it would put
     * a scope on the chain in front of the one nested inside it, which is the
     * order a panic walks. */
    *chain = s;

    /* The array of calls is deliberately not touched. A scope that defers one
     * thing writes one of them, and a scope that defers nothing writes none. */
}

static void over_grow(burrow__DeferScope *s) {
    Int cap = s->over_cap;

    if (cap > BURROW_INT_MAX / 2)
        runtime_throw(BURROW_S("too many deferred calls in one scope"));

    Int want = cap == 0 ? OVER_FIRST : cap * 2;
    Func *over = mem_realloc(heap_allocator(), s->over, (size_t)cap * sizeof(Func),
                             (size_t)want * sizeof(Func), _Alignof(Func));
    if (over == NULL)
        runtime_throw(BURROW_S("defer: out of memory"));

    s->over = over;
    s->over_cap = want;
}

void burrow__defer_push(burrow__DeferScope *s, Func fn) {
    if (s->n < BURROW_DEFER_INLINE) {
        s->calls[s->n] = fn;
        s->n++;
        return;
    }

    Int i = s->n - BURROW_DEFER_INLINE;
    if (i == s->over_cap)
        over_grow(s);

    s->over[i] = fn;
    s->n++;
}

void burrow__scope_close(burrow__DeferScope *s) {
    if (s->chain == NULL)
        return;

    /* Off the chain before the calls run, so that a deferred call opening a
     * scope of its own nests inside the one that contains this scope rather
     * than inside a scope that is halfway through being taken apart. It is also
     * what makes closing a scope twice safe, which the MSVC path needs: a
     * longjmp there runs the __finally blocks of frames a panic has already
     * unwound by hand. */
    *s->chain = s->outer;
    s->chain = NULL;

    while (s->n > 0) {
        s->n--;

        Func fn = s->n < BURROW_DEFER_INLINE ? s->calls[s->n]
                                             : s->over[s->n - BURROW_DEFER_INLINE];
        BURROW_CALLF0(fn);
    }

    if (s->over != NULL) {
        mem_free(heap_allocator(), s->over, (size_t)s->over_cap * sizeof(Func),
                 _Alignof(Func));
        s->over = NULL;
        s->over_cap = 0;
    }
}

void burrow__defer_unwind_all(void) {
    burrow__DeferScope **chain = burrow__defer_chain();

    /* Innermost first, and re-reading the head every turn because a deferred
     * call is allowed to open and close scopes of its own while this runs. */
    while (*chain != NULL)
        burrow__scope_close(*chain);
}
