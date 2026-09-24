/* The error arena and error_retain, from burrow/error.h.
 *
 * Copyright 2026 The burrow Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style licence that can be found
 * in the LICENSE file. */

#include "burrow/burrow.h"

#include "check.h"

#include <stdint.h>

BURROW_SENTINEL_ERROR(test_err_deep, "deep down");

/* An error type with a self_type and an unwrap and no clone slot, which is
 * what error_retain has to make the best of. */
typedef struct Wrapper {
    Str text;
    Error inner;
} Wrapper;

static const Type wrapper_type = {
    {(const Byte *)"Wrapper", 7},
    {(const Byte *)"errarena_test", 13},
    KIND_STRUCT,
    (uint32_t)sizeof(Wrapper),
    (uint16_t)_Alignof(Wrapper),
    0,
    0,
    NULL,
    NULL,
    NULL,
    NULL,
    0,
    0x77726170U,
    NULL,
};

static Str wrapper_message(const void *self) {
    return ((const Wrapper *)self)->text;
}

static Error wrapper_unwrap(const void *self) {
    return ((const Wrapper *)self)->inner;
}

static const ErrorVT wrapper_vt = {
    &wrapper_type, wrapper_message, wrapper_unwrap, NULL, NULL, NULL, NULL,
};

static Error wrap(Alloc *a, const char *text, Error inner) {
    Wrapper *w = BURROW_NEW(a, Wrapper);
    w->text = str_from_cstr(text);
    w->inner = inner;
    return (Error){&wrapper_vt, w};
}

/* -------------------------------------------------- without the runtime */

static void TestAThreadHasAnErrorAllocatorBeforeTheRuntimeStarts(TestingT *t) {
    Alloc *a = error_allocator();
    CHECK(a != NULL);
    CHECK(a == error_allocator());

    Error err = errors_new(a, BURROW_S("made on a plain thread"));
    CHECK(str_eq(error_text(err), BURROW_S("made on a plain thread")));
}

static void TestReleasingAMarkGivesTheMemoryBack(TestingT *t) {
    Alloc *a = error_allocator();
    ArenaMark m = error_mark();
    uint64_t live = mem_stats(a).bytes_live;

    for (int i = 0; i < 10000; i++) {
        ArenaMark inner = error_mark();
        Error err = errors_new(a, BURROW_S("one of many"));
        CHECK(BURROW_FAILED(err));
        error_release(inner);
    }

    /* Ten thousand errors in a loop that releases, and the arena is where it
     * started and never needed a second chunk. */
    CHECK_INT_EQ(mem_stats(a).bytes_live, live);
    CHECK(mem_stats(a).blocks <= 1);
    error_release(m);
}

static void TestRetainingASentinelGivesBackTheSentinel(TestingT *t) {
    Error e = error_retain(heap_allocator(), test_err_deep);
    CHECK(e.vt == test_err_deep.vt && e.data == test_err_deep.data);
    CHECK(BURROW_OK(error_retain(heap_allocator(), BURROW_NO_ERROR)));
}

static void TestRetainingCopiesTheTextOutOfTheArena(TestingT *t) {
    Arena keep;
    arena_init(&keep, NULL, 0);

    ArenaMark m = error_mark();
    Error made = errors_new(error_allocator(), BURROW_S("will outlive the mark"));
    Error kept = error_retain(arena_allocator(&keep), made);
    error_release(m);

    /* Something else takes the released memory, and the kept copy does not
     * notice. */
    Error other =
        errors_new(error_allocator(), BURROW_S("XXXXXXXXXXXXXXXXXXXXXXXXXXXX"));
    CHECK(BURROW_FAILED(other));
    CHECK(str_eq(error_text(kept), BURROW_S("will outlive the mark")));
    arena_free(&keep);
}

static void TestRetainingKeepsErrorsIsThroughAChainAndAJoin(TestingT *t) {
    Arena keep;
    arena_init(&keep, NULL, 0);
    Alloc *ea = error_allocator();

    Error chain = wrap(ea, "outer", wrap(ea, "middle", test_err_deep));
    Error joined = errors_join_v(ea, 2, errors_new(ea, BURROW_S("first")), chain);

    Error kc = error_retain(arena_allocator(&keep), chain);
    Error kj = error_retain(arena_allocator(&keep), joined);

    CHECK(errors_is(kc, test_err_deep));
    CHECK(errors_is(kj, test_err_deep));
    CHECK(str_eq(error_text(kc), BURROW_S("outer")));
    CHECK(str_eq(error_text(kj), BURROW_S("first\nouter")));

    /* The chain was rebuilt from messages, so the wrapper type is gone, which
     * is what the header says and why a type that wants errors_as after a
     * retain gives itself a clone slot. */
    CHECK(errors_as(chain, &wrapper_type) != NULL);
    CHECK(errors_as(kc, &wrapper_type) == NULL);
    arena_free(&keep);
}

/* Static so that the longjmp out of the panic cannot clobber them. */
static Error caught, kept;

static void TestRetainingARuntimeErrorKeepsItsType(TestingT *t) {
    volatile Int zero = 0;

    BURROW_TRY {
        (void)int_div(1, zero);
    }
    BURROW_CATCH(p) {
        const RuntimeError *re = runtime_error_from(p);
        CHECK(re != NULL);
        caught = *(const Error *)p.data;
        kept = error_retain(heap_allocator(), caught);
    }
    BURROW_TRY_END;

    const RuntimeError *re = errors_as(kept, TYPE_RUNTIME_ERROR);
    CHECK(re != NULL);
    if (re != NULL)
        CHECK(str_eq(re->message, BURROW_S("runtime error: integer divide by zero")));
}

/* ----------------------------------------------------- with goroutines */

static Alloc *main_alloc;
static Alloc *child_alloc;
static Error handed_over;
static Arena parent_keep;

static void child(void *arg) {
    SyncWaitGroup *wg = arg;

    child_alloc = error_allocator();
    Error err = wrap(child_alloc, "child failed", test_err_deep);

    /* What errgroup and every worker pool does, and why error_retain exists:
     * the error goes to a goroutine that outlives this one, and this one's
     * arena goes when it returns. */
    handed_over = error_retain(arena_allocator(&parent_keep), err);
    sync_wait_group_done(wg);
}

static void goroutine_body(void *arg) {
    (void)arg;
    SyncWaitGroup wg = {0};

    main_alloc = error_allocator();
    arena_init(&parent_keep, NULL, 0);

    sync_wait_group_add(&wg, 1);
    if (!go(BURROW_FN(Func, child, &wg)))
        return;
    sync_wait_group_wait(&wg);
}

static void TestEachGoroutineHasItsOwnArenaAndARetainedErrorOutlivesIt(TestingT *t) {
    Alloc *thread = error_allocator();

    runtime_main(BURROW_FN(Func, goroutine_body, NULL));

    CHECK(main_alloc != NULL && child_alloc != NULL);
    CHECK(main_alloc != child_alloc);
    CHECK(main_alloc != thread);

    /* By now the child is long gone and its arena with it. */
    CHECK(str_eq(error_text(handed_over), BURROW_S("child failed")));
    CHECK(errors_is(handed_over, test_err_deep));
    arena_free(&parent_keep);
}

#define TESTS(X)                                                                       \
    X(TestAThreadHasAnErrorAllocatorBeforeTheRuntimeStarts)                            \
    X(TestReleasingAMarkGivesTheMemoryBack)                                            \
    X(TestRetainingASentinelGivesBackTheSentinel)                                      \
    X(TestRetainingCopiesTheTextOutOfTheArena)                                         \
    X(TestRetainingKeepsErrorsIsThroughAChainAndAJoin)                                 \
    X(TestRetainingARuntimeErrorKeepsItsType)                                          \
    X(TestEachGoroutineHasItsOwnArenaAndARetainedErrorOutlivesIt)

TESTING_MAIN_BARE(TESTS)
