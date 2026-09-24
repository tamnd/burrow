/* Tests for defer.
 *
 * Almost everything here works the same way: deferred calls append a character
 * to a buffer, and the test checks the string afterwards. An order is the only
 * thing worth asserting about defer, and a string is the shortest way to write
 * one down that still says what actually happened when it goes wrong.
 *
 * The first half needs no scheduler, because a scope is a scope whether or not
 * there is a runtime under it, and that is a property worth having a test for
 * rather than a sentence about. The second half is the part that needs a
 * goroutine: a chain that belongs to the goroutine rather than the thread, a
 * goroutine that parks in the middle of a scope and finishes it after waking up
 * on some other thread, and runtime_goexit running the calls of the frames it
 * is about to throw away.
 *
 * Copyright 2026 The burrow Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style licence that can be found
 * in the LICENSE file. */

#include "burrow/defer.h"

#include "burrow/chan.h"
#include "burrow/func.h"
#include "burrow/mem/heap.h"
#include "burrow/proc.h"
#include "burrow/thread.h"

#include "check.h"

#include <stdbool.h>
#include <stddef.h>
#include <string.h>

/* Where the deferred calls write. One buffer and an index into it, reset by
 * every test that uses it, which is all of them. */
static char trace[64];
static size_t traced;

/* What was on the chain when the test began: the testing package's own scope
 * on the thread a test runs on, and nothing on a goroutine. A test that is over
 * has put it back to that. */
static burrow__DeferScope *outer;

static void reset(void) {
    memset(trace, 0, sizeof(trace));
    traced = 0;
    outer = *burrow__defer_chain();
}

/* The deferred function for most of this file. The argument is the character to
 * append, passed as the pointer itself rather than through one, since what is
 * being tested is the order and not the argument. */
static void mark(void *env) {
    char c = (char)(size_t)env;
    if (traced + 1 < sizeof(trace))
        trace[traced++] = c;
}

#define MARK(c) BURROW_DEFER(mark, (size_t)(c))

/* --------------------------------------------------- without a scheduler */

static void TestAScopeWithNothingInItLeavesTheChainAsItFoundIt(TestingT *t) {
    burrow__DeferScope **chain = burrow__defer_chain();
    burrow__DeferScope *before = *chain;

    BURROW_SCOPE {
        CHECK(*chain != before);
    }
    BURROW_SCOPE_END;

    CHECK(*chain == before);
}

static void TestADeferredCallRunsAtTheEndOfTheScopeAndNotBefore(TestingT *t) {
    reset();

    BURROW_SCOPE {
        MARK('a');
        CHECK_STR_EQ(trace, "");
    }
    BURROW_SCOPE_END;

    CHECK_STR_EQ(trace, "a");
}

static void TestDeferredCallsRunLastInFirstOut(TestingT *t) {
    reset();

    BURROW_SCOPE {
        MARK('a');
        MARK('b');
        MARK('c');
    }
    BURROW_SCOPE_END;

    CHECK_STR_EQ(trace, "cba");
}

/* The one that matters most, since a defer that only runs when control falls
 * off the end of the block is a defer nobody can rely on. */
static void returns_from_inside_a_scope(void) {
    BURROW_SCOPE {
        MARK('a');
        if (traced == 0)
            return;
        MARK('b');
    }
    BURROW_SCOPE_END;
}

static void TestAReturnFromInsideAScopeStillRunsTheCalls(TestingT *t) {
    reset();
    returns_from_inside_a_scope();
    CHECK_STR_EQ(trace, "a");
}

static void TestABreakOutOfTheLoopTheScopeIsInStillRunsTheCalls(TestingT *t) {
    reset();

    for (int i = 0; i < 4; i++) {
        BURROW_SCOPE {
            MARK((char)('0' + i));
            if (i == 1)
                break;
        }
        BURROW_SCOPE_END;
    }

    CHECK_STR_EQ(trace, "01");
}

static void TestAGotoOutOfAScopeStillRunsTheCalls(TestingT *t) {
    reset();

    BURROW_SCOPE {
        MARK('a');
        goto out;
    }
    BURROW_SCOPE_END;

out:
    CHECK_STR_EQ(trace, "a");
}

static void TestAnInnerScopeFinishesBeforeTheOuterOne(TestingT *t) {
    reset();

    BURROW_SCOPE {
        MARK('a');

        BURROW_SCOPE {
            MARK('b');
            MARK('c');
        }
        BURROW_SCOPE_END;

        CHECK_STR_EQ(trace, "cb");
        MARK('d');
    }
    BURROW_SCOPE_END;

    CHECK_STR_EQ(trace, "cbda");
}

/* Go's defer would hold all four to the end of the function. This one runs on
 * every turn, which is the difference burrow/defer.h is explicit about and the
 * reason a file handle opened in a loop is safe here. */
static void TestAScopeInsideALoopRunsItsCallsEveryTurn(TestingT *t) {
    reset();

    for (int i = 0; i < 4; i++) {
        BURROW_SCOPE {
            MARK((char)('0' + i));
        }
        BURROW_SCOPE_END;
    }

    CHECK_STR_EQ(trace, "0123");
}

/* Go evaluates a deferred call's arguments where the defer is written, and so
 * does this, because the record holds the value rather than a way of getting it
 * again later. */
static void TestTheArgumentIsTheOneTheDeferWasWrittenWith(TestingT *t) {
    reset();

    char c = 'a';
    BURROW_SCOPE {
        MARK(c);
        c = 'z';
    }
    BURROW_SCOPE_END;

    CHECK_STR_EQ(trace, "a");
    CHECK_INT_EQ(c, 'z');
}

static void defers_something_of_its_own(void *env) {
    (void)env;

    BURROW_SCOPE {
        MARK('c');
    }
    BURROW_SCOPE_END;

    mark((void *)(size_t)'b');
}

static void TestADeferredCallCanOpenScopesAndDeferThingsOfItsOwn(TestingT *t) {
    reset();

    BURROW_SCOPE {
        MARK('a');
        BURROW_DEFER(defers_something_of_its_own, NULL);
    }
    BURROW_SCOPE_END;

    CHECK_STR_EQ(trace, "cba");
    CHECK(*burrow__defer_chain() == outer);
}

/* Go's rule for a defer in a loop, which burrow has too whenever the scope is
 * outside the loop: the calls pile up and run at the end of the scope. Worth a
 * test because the calls past the fourth live somewhere else, and because this
 * is the shape that used to hang. */
static void TestADeferInALoopPilesUpUntilTheEndOfTheScope(TestingT *t) {
    reset();

    BURROW_SCOPE {
        for (int i = 0; i < 3; i++)
            MARK((char)('0' + i));

        CHECK_STR_EQ(trace, "");
    }
    BURROW_SCOPE_END;

    CHECK_STR_EQ(trace, "210");
}

/* Enough to fill the scope's own room for calls and grow what holds the rest
 * three times, since the growth is the part with arithmetic in it. */
static void TestAScopeHoldsAsManyCallsAsItIsGiven(TestingT *t) {
    reset();

    BURROW_SCOPE {
        for (int i = 0; i < 26; i++)
            MARK((char)('a' + i));
    }
    BURROW_SCOPE_END;

    CHECK_STR_EQ(trace, "zyxwvutsrqponmlkjihgfedcba");
    CHECK(*burrow__defer_chain() == outer);
}

/* A BURROW_DEFER is one statement and nothing else, so it goes where a
 * statement goes. It was a declaration once, which meant a loop body or an if
 * without braces did not compile, and that was a worse trade than it looked. */
static void TestADeferIsAnOrdinaryStatement(TestingT *t) {
    reset();

    BURROW_SCOPE {
        if (traced == 0)
            MARK('a');

        for (int i = 0; i < 2; i++)
            MARK((char)('0' + i));

        MARK('b');

        /* Two on one line, which used to be a redefinition error back when a
         * defer was a declaration named after the line it was written on. */
        MARK('c'), MARK('d');
    }
    BURROW_SCOPE_END;

    CHECK_STR_EQ(trace, "dcb10a");
}

/* ------------------------------------------------------ with a scheduler */

/* Every goroutine test below ends with the child telling the main goroutine it
 * has finished, on this channel, because a Go program ends when its main
 * function returns whether or not anything else was in the middle of something.
 * A test that read the trace without waiting would be reading it while the
 * other goroutine was still writing to it. */
static Chan *done;

static void start_run(void) {
    reset();
    done = chan_make(heap_allocator(), TYPE_INT, 0);
}

static void finish_run(void) {
    chan_free(done);
    done = NULL;
}

static void say_done(void) {
    Int v = 1;
    chan_send(done, &v);
}

static void wait_for_done(void) {
    Int v = 0;
    chan_recv(done, &v);
}

static void child_defers(void *env) {
    (void)env;

    BURROW_SCOPE {
        MARK('c');
        runtime_gosched();
        MARK('d');
    }
    BURROW_SCOPE_END;

    say_done();
}

static void main_with_a_goroutine(void *env) {
    (void)env;

    BURROW_SCOPE {
        MARK('a');
        go(BURROW_FN(Func, child_defers, NULL));
        wait_for_done();
        MARK('b');
    }
    BURROW_SCOPE_END;
}

static void TestAGoroutineRunsItsOwnDefersAndNotAnybodyElses(TestingT *t) {
    start_run();
    runtime_gomaxprocs(1);
    runtime_main(BURROW_FN(Func, main_with_a_goroutine, NULL));
    finish_run();

    /* The child's two calls are on the child's chain and run when the child
     * leaves its scope, which is before it says it is done. The main
     * goroutine's two are on its own chain and run after that. Neither pair
     * ever interleaves with the other, which is the whole point: the chain
     * belongs to the goroutine. */
    CHECK_STR_EQ(trace, "dcba");
}

/* Deferred rather than called, because a goroutine that ends with goexit never
 * gets back to the line after the scope. This is what tells the main goroutine
 * the child is finished, and it is deferred first so that it runs last. */
static void say_done_deferred(void *env) {
    (void)env;
    say_done();
}

static void goexits_from_inside_a_scope(void *env) {
    (void)env;

    BURROW_SCOPE {
        BURROW_DEFER(say_done_deferred, NULL);
        MARK('a');

        BURROW_SCOPE {
            MARK('b');
            runtime_goexit();
        }
        BURROW_SCOPE_END;

        MARK('z');
    }
    BURROW_SCOPE_END;
}

static void main_that_goexits(void *env) {
    (void)env;
    go(BURROW_FN(Func, goexits_from_inside_a_scope, NULL));
    wait_for_done();
}

static void TestGoexitRunsTheCallsOfEveryScopeTheGoroutineIsInside(TestingT *t) {
    start_run();
    runtime_gomaxprocs(1);
    runtime_main(BURROW_FN(Func, main_that_goexits, NULL));
    finish_run();

    /* Both scopes run, innermost first, and nothing after the goexit does. */
    CHECK_STR_EQ(trace, "ba");
}

static Chan *handoff;

static void parks_in_the_middle_of_a_scope(void *env) {
    (void)env;

    BURROW_SCOPE {
        MARK('a');

        Int v = 0;
        chan_recv(handoff, &v);

        MARK('b');
    }
    BURROW_SCOPE_END;

    say_done();
}

static void main_that_hands_over(void *env) {
    (void)env;

    go(BURROW_FN(Func, parks_in_the_middle_of_a_scope, NULL));

    Int v = 1;
    chan_send(handoff, &v);

    wait_for_done();
}

static void TestAGoroutineThatParksInsideAScopeStillHasItsCallsAfterwards(TestingT *t) {
    start_run();
    runtime_gomaxprocs(4);

    handoff = chan_make(heap_allocator(), TYPE_INT, 0);
    CHECK(handoff != NULL);

    runtime_main(BURROW_FN(Func, main_that_hands_over, NULL));

    chan_free(handoff);
    handoff = NULL;
    finish_run();

    /* With four Ps the goroutine that opened the scope is unlikely to be on the
     * thread it started on by the time the scope closes, which is the thing
     * being checked. The order is the plain one: two calls, last in first
     * out. */
    CHECK_STR_EQ(trace, "ba");
}

/* ------------------------------------------- on a thread that is not a G */

static uint32_t thread_ran;

static void on_a_bare_thread(void *env) {
    (void)env;

    burrow__DeferScope **chain = burrow__defer_chain();
    bool empty_before = *chain == NULL;

    BURROW_SCOPE {
        MARK('t');
    }
    BURROW_SCOPE_END;

    if (empty_before && *chain == NULL)
        thread_ran = 1;
}

static void TestAThreadThatIsNotAGoroutineHasAChainOfItsOwn(TestingT *t) {
    reset();
    thread_ran = 0;

    burrow__Thread th;
    CHECK(burrow__thread_start(&th, on_a_bare_thread, NULL, 0));
    burrow__thread_join(&th);

    CHECK_INT_EQ(thread_ran, 1);
    CHECK_STR_EQ(trace, "t");

    /* This thread's chain is its own and the one that just ran did not touch
     * it. */
    CHECK(*burrow__defer_chain() == outer);
}

#define TESTS(X)                                                                       \
    X(TestAScopeWithNothingInItLeavesTheChainAsItFoundIt)                              \
    X(TestADeferredCallRunsAtTheEndOfTheScopeAndNotBefore)                             \
    X(TestDeferredCallsRunLastInFirstOut)                                              \
    X(TestAReturnFromInsideAScopeStillRunsTheCalls)                                    \
    X(TestABreakOutOfTheLoopTheScopeIsInStillRunsTheCalls)                             \
    X(TestAGotoOutOfAScopeStillRunsTheCalls)                                           \
    X(TestAnInnerScopeFinishesBeforeTheOuterOne)                                       \
    X(TestAScopeInsideALoopRunsItsCallsEveryTurn)                                      \
    X(TestTheArgumentIsTheOneTheDeferWasWrittenWith)                                   \
    X(TestADeferredCallCanOpenScopesAndDeferThingsOfItsOwn)                            \
    X(TestADeferInALoopPilesUpUntilTheEndOfTheScope)                                   \
    X(TestAScopeHoldsAsManyCallsAsItIsGiven)                                           \
    X(TestADeferIsAnOrdinaryStatement)                                                 \
    X(TestAGoroutineRunsItsOwnDefersAndNotAnybodyElses)                                \
    X(TestGoexitRunsTheCallsOfEveryScopeTheGoroutineIsInside)                          \
    X(TestAGoroutineThatParksInsideAScopeStillHasItsCallsAfterwards)                   \
    X(TestAThreadThatIsNotAGoroutineHasAChainOfItsOwn)

TESTING_MAIN_BARE(TESTS)
