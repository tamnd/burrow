/* Tests for panic and the catch block.
 *
 * Two things are worth asserting about a panic and both of them are orders.
 * Which deferred calls ran, in which order, which is the same trace buffer
 * trick tests/defer_test.c uses. And which catch block got it, which is what
 * the recorded text is for. Everything here is one of those two.
 *
 * What is not here is the unrecovered path, where a panic with no BURROW_TRY
 * above it prints and ends the process. Testing that in process is not possible
 * by construction: the moment there is a block that can catch it, it is no
 * longer that path. It needs a child process to run the panic in, which needs
 * os/exec, and that is a test that gets written when os/exec exists.
 *
 * The state checks after each test are not decoration either. A panic that
 * lands in the right place having left a scope on the chain or a recovery point
 * pointing at a dead frame is a bug that reports itself much later and
 * somewhere else, so the invariant is checked where it is cheap to check.
 *
 * Copyright 2026 The burrow Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style licence that can be found
 * in the LICENSE file. */

#include "burrow/panic.h"

#include "burrow/chan.h"
#include "burrow/defer.h"
#include "burrow/error.h"
#include "burrow/func.h"
#include "burrow/mem/heap.h"
#include "burrow/proc.h"
#include "burrow/sched.h"
#include "burrow/type.h"

#include "check.h"

#include <stdbool.h>
#include <stddef.h>
#include <string.h>

/* gcc's -Wclobbered fires on locals that are live across a setjmp, which in a
 * file full of BURROW_TRY is most of them, and it fires whether or not the
 * local is ever written to in between. Everything this file reads after a catch
 * is at file scope for that reason, which is also the advice burrow/panic.h
 * gives, so the warning has nothing left to be right about. */
#if defined(__GNUC__) && !defined(__clang__)
#pragma GCC diagnostic ignored "-Wclobbered"
#endif

/* Where the deferred calls write, oldest first. */
static char trace[64];
static size_t traced;

/* The text of the last panic a catch block saw, and how many of them there
 * have been since the last reset. */
static char caught[64];
static int catches;

/* What the testing package has open around the test on the thread it runs on,
 * which is where that thread's chains have to be back to when it is over. A
 * goroutine starts with nothing, so for one of those it is nothing. */
static burrow__DeferScope *outer_scopes;
static burrow__Recover *outer_recovers;

static void reset(void) {
    memset(trace, 0, sizeof(trace));
    traced = 0;
    memset(caught, 0, sizeof(caught));
    catches = 0;
    if (burrow__curg() == NULL) {
        outer_scopes = *burrow__defer_chain();
        outer_recovers = burrow__panic_state()->recovers;
    }
}

static void mark(void *env) {
    char c = (char)(size_t)env;

    if (traced + 1 < sizeof(trace))
        trace[traced++] = c;
}

#define MARK(c) BURROW_DEFER(mark, (size_t)(c))

static void record(Any p) {
    Str s = panic_text(p);
    size_t n =
        (size_t)(s.len < (Int)sizeof(caught) - 1 ? s.len : (Int)sizeof(caught) - 1);

    if (s.p != NULL && n > 0)
        memcpy(caught, s.p, n);
    caught[n] = '\0';
    catches++;
}

/* Nothing on either chain but what was there before, and no panic in flight,
 * which is what every one of these tests should be able to say about itself
 * when it is over. */
static void check_clean(TestingT *t) {
    burrow__PanicState *st = burrow__panic_state();
    bool on_g = burrow__curg() != NULL;

    CHECK(*burrow__defer_chain() == (on_g ? NULL : outer_scopes));
    CHECK(st->recovers == (on_g ? NULL : outer_recovers));
    CHECK(st->panics == NULL);
}

/* ------------------------------------------------------------ the block */

static void TestABlockThatDoesNotPanicNeverReachesTheCatch(TestingT *t) {
    reset();

    BURROW_TRY {
        mark((void *)(size_t)'a');
    }
    BURROW_CATCH(p) {
        record(p);
    }
    BURROW_TRY_END;

    CHECK_STR_EQ(trace, "a");
    CHECK_INT_EQ(catches, 0);
    check_clean(t);
}

static void TestAPanicLandsInTheCatchBlock(TestingT *t) {
    reset();

    BURROW_TRY {
        panic_str(BURROW_S("boom"));
    }
    BURROW_CATCH(p) {
        record(p);
    }
    BURROW_TRY_END;

    CHECK_INT_EQ(catches, 1);
    CHECK_STR_EQ(caught, "boom");
    check_clean(t);
}

static void TestTheDeferredCallsRunOnTheWayOut(TestingT *t) {
    reset();

    BURROW_TRY {
        BURROW_SCOPE {
            MARK('a');
            MARK('b');
            panic_str(BURROW_S("boom"));
        }
        BURROW_SCOPE_END;
    }
    BURROW_CATCH(p) {
        record(p);

        /* Both of them, newest first, and before the catch block runs rather
         * than after it. */
        CHECK_STR_EQ(trace, "ba");
    }
    BURROW_TRY_END;

    CHECK_INT_EQ(catches, 1);
    check_clean(t);
}

static void TestTheInnermostScopeRunsItsCallsFirst(TestingT *t) {
    reset();

    BURROW_TRY {
        BURROW_SCOPE {
            MARK('a');
            BURROW_SCOPE {
                MARK('b');
                panic_str(BURROW_S("boom"));
            }
            BURROW_SCOPE_END;
        }
        BURROW_SCOPE_END;
    }
    BURROW_CATCH(p) {
        record(p);
    }
    BURROW_TRY_END;

    CHECK_STR_EQ(trace, "ba");
    check_clean(t);
}

static void TestAScopeOutsideTheTryKeepsItsCallsForLater(TestingT *t) {
    reset();

    BURROW_SCOPE {
        MARK('o');

        BURROW_TRY {
            BURROW_SCOPE {
                MARK('i');
                panic_str(BURROW_S("boom"));
            }
            BURROW_SCOPE_END;
        }
        BURROW_CATCH(p) {
            record(p);

            /* The scope this block is inside is not part of the unwinding, so
             * its call has not run and the program can still use whatever it is
             * holding open. */
            CHECK_STR_EQ(trace, "i");
        }
        BURROW_TRY_END;
    }
    BURROW_SCOPE_END;

    CHECK_STR_EQ(trace, "io");
    check_clean(t);
}

/* ------------------------------------------------------------- the value */

static void panics_with_an_int(void) {
    Int n = 42;

    panic(BURROW_ANY(TYPE_INT, &n));
}

static void TestTheValueOutlivesTheFrameItWasMadeIn(TestingT *t) {
    reset();

    BURROW_TRY {
        panics_with_an_int();
    }
    BURROW_CATCH(p) {
        CHECK(p.t == TYPE_INT);
        CHECK_INT_EQ(*(const Int *)p.data, 42);
        record(p);
    }
    BURROW_TRY_END;

    CHECK_STR_EQ(caught, "42");
    check_clean(t);
}

static void TestPanickingWithNothingStillGivesTheCatchBlockSomething(TestingT *t) {
    reset();

    BURROW_TRY {
        Any nothing = {NULL, NULL};

        panic(nothing);
    }
    BURROW_CATCH(p) {
        CHECK(!BURROW_ANY_IS_NIL(p));
        record(p);
    }
    BURROW_TRY_END;

    CHECK_STR_EQ(caught, "panic called with nil argument");
    check_clean(t);
}

BURROW_SENTINEL_ERROR(a_test_error, "a test error");

static bool text_is(Any v, const char *want) {
    Str s = panic_text(v);
    size_t n = strlen(want);

    return s.len == (Int)n && (n == 0 || memcmp(s.p, want, n) == 0);
}

static void TestPanicTextReadsTheKindsItKnows(TestingT *t) {
    Str s = BURROW_S("some text");
    Error e = a_test_error;
    Int i = -7;
    Uint u = 9;
    bool b = true;
    double f = 1.5;
    Any nothing = {NULL, NULL};

    CHECK(text_is(BURROW_ANY(TYPE_STRING, &s), "some text"));
    CHECK(text_is(BURROW_ANY(TYPE_ERROR, &e), "a test error"));
    CHECK(text_is(BURROW_ANY(TYPE_INT, &i), "-7"));
    CHECK(text_is(BURROW_ANY(TYPE_UINT, &u), "9"));
    CHECK(text_is(BURROW_ANY(TYPE_BOOL, &b), "true"));
    CHECK(text_is(BURROW_ANY(TYPE_FLOAT64, &f), "1.5"));
    CHECK(text_is(nothing, "nil"));
}

/* --------------------------------------------------- from a deferred call */

static void note_whether_a_panic_is_running(void *env) {
    Any v = panic_value();

    (void)env;
    mark((void *)(size_t)(BURROW_ANY_IS_NIL(v) ? 'n' : 'p'));
}

static void TestADeferredCallCanAskWhetherItIsRunningBecauseOfAPanic(TestingT *t) {
    reset();

    BURROW_SCOPE {
        BURROW_DEFER(note_whether_a_panic_is_running, NULL);
    }
    BURROW_SCOPE_END;

    CHECK_STR_EQ(trace, "n");

    reset();

    BURROW_TRY {
        BURROW_SCOPE {
            BURROW_DEFER(note_whether_a_panic_is_running, NULL);
            panic_str(BURROW_S("boom"));
        }
        BURROW_SCOPE_END;
    }
    BURROW_CATCH(p) {
        record(p);
    }
    BURROW_TRY_END;

    CHECK_STR_EQ(trace, "p");

    /* And nothing is unwinding once the catch block has it. */
    CHECK(BURROW_ANY_IS_NIL(panic_value()));
    check_clean(t);
}

static void panics_again(void *env) {
    (void)env;

    panic_str(BURROW_S("second"));
}

static void TestAPanicFromADeferredCallReplacesTheOneThatWasUnwinding(TestingT *t) {
    reset();

    BURROW_TRY {
        BURROW_SCOPE {
            MARK('a');
            BURROW_DEFER(panics_again, NULL);
            panic_str(BURROW_S("first"));
        }
        BURROW_SCOPE_END;
    }
    BURROW_CATCH(p) {
        record(p);
    }
    BURROW_TRY_END;

    /* The catch block gets the newest panic, which is Go's rule for what
     * recover returns, and the deferred call beside the one that panicked still
     * ran, which is also Go's rule. */
    CHECK_STR_EQ(caught, "second");
    CHECK_STR_EQ(trace, "a");
    check_clean(t);
}

/* -------------------------------------------------------- blocks in blocks */

static void TestTheInnermostBlockCatches(TestingT *t) {
    reset();

    BURROW_TRY {
        BURROW_TRY {
            panic_str(BURROW_S("inner"));
        }
        BURROW_CATCH(p) {
            record(p);
        }
        BURROW_TRY_END;

        mark((void *)(size_t)'a');
    }
    BURROW_CATCH(p) {
        record(p);
    }
    BURROW_TRY_END;

    CHECK_INT_EQ(catches, 1);
    CHECK_STR_EQ(caught, "inner");

    /* The block carried on where it left off rather than ending with the panic
     * it caught. */
    CHECK_STR_EQ(trace, "a");
    check_clean(t);
}

static void TestAPanicFromACatchBlockGoesOutward(TestingT *t) {
    reset();

    BURROW_TRY {
        BURROW_TRY {
            panic_str(BURROW_S("first"));
        }
        BURROW_CATCH(p) {
            record(p);
            panic_str(BURROW_S("second"));
        }
        BURROW_TRY_END;
    }
    BURROW_CATCH(p) {
        record(p);
    }
    BURROW_TRY_END;

    CHECK_INT_EQ(catches, 2);
    CHECK_STR_EQ(caught, "second");
    check_clean(t);
}

/* The same thing again, with the blocks in frames of their own.
 *
 * The version above has both blocks in one frame, so the jump out of the catch
 * never leaves a frame. These three do, and that is a different path: MSVC
 * spells the end of a block as a __finally, and a longjmp past the frame runs
 * it, after panic has already taken the block off the chain and moved the head
 * further out. Windows is where this showed up and it is not a Windows test,
 * because what it asserts is true everywhere. */
static void innermost(void) {
    BURROW_TRY {
        panic_str(BURROW_S("first"));
    }
    BURROW_CATCH(p) {
        record(p);
        panic_str(BURROW_S("second"));
    }
    BURROW_TRY_END;
}

static void middle(void) {
    BURROW_TRY {
        innermost();
    }
    BURROW_CATCH(p) {
        record(p);
        panic_str(BURROW_S("third"));
    }
    BURROW_TRY_END;
}

static void TestAPanicFromACatchBlockInACalledFunctionGoesOutward(TestingT *t) {
    reset();

    BURROW_TRY {
        middle();
    }
    BURROW_CATCH(p) {
        record(p);
    }
    BURROW_TRY_END;

    /* Three catches, each one further out than the last. A block that caught
     * twice, or a jump that went back into a frame it had already left, shows
     * up here as the wrong count or the wrong text. */
    CHECK_INT_EQ(catches, 3);
    CHECK_STR_EQ(caught, "third");
    check_clean(t);
}

static void returns_from_inside_a_try(void) {
    BURROW_TRY {
        return;
    }
    BURROW_CATCH(p) {
        record(p);
    }
    BURROW_TRY_END;
}

static void TestAReturnOutOfATryBlockLeavesNothingBehind(TestingT *t) {
    reset();

    returns_from_inside_a_try();
    check_clean(t);

    /* The real check is that the next panic finds the block it should rather
     * than the one that has gone home. */
    BURROW_TRY {
        panic_str(BURROW_S("after"));
    }
    BURROW_CATCH(p) {
        record(p);
    }
    BURROW_TRY_END;

    CHECK_INT_EQ(catches, 1);
    CHECK_STR_EQ(caught, "after");
    check_clean(t);
}

/* ------------------------------------------------------------ goroutines */

static Chan *done;
static Chan *handoff;

static void say_done(void) {
    Int v = 1;

    chan_send(done, &v);
}

static void wait_for_done(void) {
    Int v = 0;

    chan_recv(done, &v);
}

static void child_panics(void *env) {
    (void)env;

    BURROW_TRY {
        BURROW_SCOPE {
            MARK('c');
            panic_str(BURROW_S("child"));
        }
        BURROW_SCOPE_END;
    }
    BURROW_CATCH(p) {
        record(p);
    }
    BURROW_TRY_END;

    say_done();
}

static void main_with_a_panicking_goroutine(void *env) {
    (void)env;

    BURROW_TRY {
        go(BURROW_FN(Func, child_panics, NULL));
        wait_for_done();
        mark((void *)(size_t)'m');
    }
    BURROW_CATCH(p) {
        record(p);
    }
    BURROW_TRY_END;
}

static void TestAPanicStaysOnTheGoroutineThatRaisedIt(TestingT *t) {
    reset();
    done = chan_make(heap_allocator(), TYPE_INT, 0);
    runtime_gomaxprocs(1);
    runtime_main(BURROW_FN(Func, main_with_a_panicking_goroutine, NULL));
    chan_free(done);
    done = NULL;

    /* One catch, the child's own, and the main goroutine carried on. */
    CHECK_INT_EQ(catches, 1);
    CHECK_STR_EQ(caught, "child");
    CHECK_STR_EQ(trace, "cm");
    check_clean(t);
}

static void parks_inside_a_try(void *env) {
    Int v = 0;

    (void)env;

    BURROW_TRY {
        BURROW_SCOPE {
            MARK('c');
            chan_recv(handoff, &v);
            panic_str(BURROW_S("woke up"));
        }
        BURROW_SCOPE_END;
    }
    BURROW_CATCH(p) {
        record(p);
    }
    BURROW_TRY_END;

    say_done();
}

static void main_that_hands_over(void *env) {
    Int v = 1;

    (void)env;

    go(BURROW_FN(Func, parks_inside_a_try, NULL));
    chan_send(handoff, &v);
    wait_for_done();
}

static void TestAGoroutineThatParksInsideABlockStillHasItAfterwards(TestingT *t) {
    reset();
    done = chan_make(heap_allocator(), TYPE_INT, 0);
    handoff = chan_make(heap_allocator(), TYPE_INT, 0);

    /* More than one P, so that the goroutine can come back on a thread that is
     * not the one it parked on. That is the arrangement the chain has to
     * survive, and it is why the state is a field of the goroutine. */
    runtime_gomaxprocs(4);
    runtime_main(BURROW_FN(Func, main_that_hands_over, NULL));

    chan_free(handoff);
    chan_free(done);
    handoff = NULL;
    done = NULL;

    CHECK_INT_EQ(catches, 1);
    CHECK_STR_EQ(caught, "woke up");
    CHECK_STR_EQ(trace, "c");
    check_clean(t);
}

#define TESTS(X)                                                                       \
    X(TestABlockThatDoesNotPanicNeverReachesTheCatch)                                  \
    X(TestAPanicLandsInTheCatchBlock)                                                  \
    X(TestTheDeferredCallsRunOnTheWayOut)                                              \
    X(TestTheInnermostScopeRunsItsCallsFirst)                                          \
    X(TestAScopeOutsideTheTryKeepsItsCallsForLater)                                    \
    X(TestTheValueOutlivesTheFrameItWasMadeIn)                                         \
    X(TestPanickingWithNothingStillGivesTheCatchBlockSomething)                        \
    X(TestPanicTextReadsTheKindsItKnows)                                               \
    X(TestADeferredCallCanAskWhetherItIsRunningBecauseOfAPanic)                        \
    X(TestAPanicFromADeferredCallReplacesTheOneThatWasUnwinding)                       \
    X(TestTheInnermostBlockCatches)                                                    \
    X(TestAPanicFromACatchBlockGoesOutward)                                            \
    X(TestAPanicFromACatchBlockInACalledFunctionGoesOutward)                           \
    X(TestAReturnOutOfATryBlockLeavesNothingBehind)                                    \
    X(TestAPanicStaysOnTheGoroutineThatRaisedIt)                                       \
    X(TestAGoroutineThatParksInsideABlockStillHasItAfterwards)

TESTING_MAIN_BARE(TESTS)
