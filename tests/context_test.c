/* Tests for the context package.
 *
 * In two halves, the same way tests/chan_test.c is, and for the same reason.
 *
 * Almost everything a context does is an ordinary function call on an ordinary
 * data structure. Making one, cancelling one, looking a value up and walking
 * the chain need no scheduler, and testing them without one means a failure
 * points at the context and not at the context or the scheduler or the timing.
 * A cancel closes a channel nobody is waiting on, which is a lock and a flag.
 *
 * The second half starts the runtime, because two things need it. A goroutine
 * parked on a done channel is the whole point of the package, and the fallback
 * path for a parent this package did not make is a goroutine watching two
 * channels. Those tests follow the rule tests/sched_test.c sets out: what a
 * child goroutine finds out it says through an atomic.
 *
 * The fake context near the top is not padding. Go's context is an interface
 * and so is this one, and the two hardest paths in the package only run when
 * the parent came from outside it: the value walk has to go through a stranger
 * and come back, and a stranger that replaces Done has to not be mistaken for
 * what it wraps. Neither is reachable without writing one.
 *
 * Copyright 2026 The burrow Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style licence that can be found
 * in the LICENSE file. */

#include "burrow/context.h"

#include "burrow/atomic.h"
#include "burrow/chan.h"
#include "burrow/func.h"
#include "burrow/mem/heap.h"
#include "burrow/mem/track.h"
#include "burrow/proc.h"
#include "burrow/slice.h"
#include "burrow/time.h"
#include "burrow/type.h"

#include "fatal.h"
#include "harness.h"

/* ------------------------------------------------------------------- keys
 *
 * A key type private to this file, which is the pattern burrow/context.h
 * describes: the descriptor is static here, so no other translation unit can
 * build a key that matches it. Two keys of the same type are told apart by
 * their value, exactly as Go's `type ctxKey int` with two constants of it. */
static const Type ctx_key_type = {
    {(const Byte *)"ctxKey", 6},
    {(const Byte *)"context_test", 12},
    KIND_INT,
    (uint32_t)sizeof(Int),
    (uint16_t)_Alignof(Int),
    0,
    0,
    NULL,
    NULL,
    NULL,
    NULL,
    0,
    0x746b6579U,
    NULL,
};

static Int request_id_key = 0;
static Int user_key = 1;

#define REQUEST_ID_KEY BURROW_ANY(&ctx_key_type, &request_id_key)
#define USER_KEY BURROW_ANY(&ctx_key_type, &user_key)

/* []int, for the one key that cannot be compared. The kind is all anything
 * looks at to decide that, so elem is left NULL. */
static const Type slice_of_int = {
    {(const Byte *)"[]int", 5},
    {NULL, 0},
    KIND_SLICE,
    (uint32_t)sizeof(Slice),
    (uint16_t)_Alignof(Slice),
    0,
    0,
    NULL,
    NULL,
    NULL,
    NULL,
    0,
    0x736c6369U,
    NULL,
};

/* --------------------------------------------------------------- the fake
 *
 * A Context written the way somebody outside the package would write one. Its
 * value does not forward anywhere, because it is a root, and everything about
 * it is settable so that one type covers every case the tests need. */
typedef struct Fake {
    Chan *done;
    Error err;
    bool has_deadline;
    int64_t when;
    Any key;
    Any val;

    /* For the wrapper case: a fake that answers value out of another context
     * while answering done out of its own channel. */
    Context inner;
    bool wrap;
} Fake;

static bool fake_deadline(void *self, int64_t *when) {
    Fake *f = (Fake *)self;

    if (!f->has_deadline)
        return false;
    *when = f->when;
    return true;
}

static Chan *fake_done(void *self) {
    return ((Fake *)self)->done;
}

static Error fake_err(void *self) {
    return ((Fake *)self)->err;
}

static Any fake_value(void *self, Any key) {
    Fake *f = (Fake *)self;
    Any none = {NULL, NULL};

    if (!BURROW_ANY_IS_NIL(f->key) && any_equal(key, f->key))
        return f->val;
    if (f->wrap)
        return context_value(f->inner, key);
    return none;
}

static const ContextVT fake_vt = {
    NULL, fake_deadline, fake_done, fake_err, fake_value,
};

static Context fake_context(Fake *f) {
    Context c = {&fake_vt, f};
    return c;
}

/* ----------------------------------------------------------------- helpers */

/* Whether the done channel says the work should stop. A done channel never
 * carries a value, so the only way a receive can succeed on one is a close. */
static bool is_done(Context c) {
    bool had_value = false;

    return chan_try_recv(context_done(c), NULL, &had_value) && !had_value;
}

static bool is(Error e, Error want) {
    return e.vt == want.vt && e.data == want.data;
}

/* ------------------------------------------------------------------ the root */

TEST(the_root_is_never_cancelled_and_carries_nothing) {
    Context c = context_background();
    int64_t when = 1234;

    CHECK(!BURROW_CONTEXT_IS_NIL(c));
    CHECK(!context_deadline(c, &when));
    CHECK_INT_EQ(when, 1234); /* untouched, because there is no deadline */
    CHECK(context_done(c) == NULL);
    CHECK(BURROW_OK(context_err(c)));
    CHECK(BURROW_ANY_IS_NIL(context_value(c, REQUEST_ID_KEY)));

    /* Nothing was allocated, so nothing has to be freed, and saying so is
     * allowed rather than a mistake. */
    context_free(c);
}

TEST(background_and_todo_are_not_the_same_context) {
    Context b = context_background();
    Context t = context_todo();

    CHECK(b.vt != t.vt);
    CHECK(any_equal(BURROW_ANY(TYPE_CONTEXT, &b), BURROW_ANY(TYPE_CONTEXT, &b)));
    CHECK(!any_equal(BURROW_ANY(TYPE_CONTEXT, &b), BURROW_ANY(TYPE_CONTEXT, &t)));

    /* TODO behaves exactly like Background, which is the point of it. */
    CHECK(context_done(t) == NULL);
    CHECK(BURROW_OK(context_err(t)));
}

/* ---------------------------------------------------------------- WithCancel */

TEST(a_cancel_closes_the_done_channel_and_sets_the_error) {
    Alloc *a = heap_allocator();
    CancelFunc cancel;
    Context c = context_with_cancel(a, context_background(), &cancel);

    CHECK(!BURROW_CONTEXT_IS_NIL(c));
    CHECK(context_done(c) != NULL);
    CHECK(BURROW_OK(context_err(c)));
    CHECK(!is_done(c));

    BURROW_CALLF0(cancel);

    CHECK(is_done(c));
    CHECK(is(context_err(c), context_canceled));
    CHECK_STR_EQ((const char *)error_message(context_err(c)).p, "context canceled");

    /* A cancel says nothing about a deadline, so the parent's answer stands. */
    int64_t when = 0;
    CHECK(!context_deadline(c, &when));

    context_free(c);
}

TEST(cancelling_twice_changes_nothing) {
    Alloc *a = heap_allocator();
    CancelFunc cancel;
    Context c = context_with_cancel(a, context_background(), &cancel);

    BURROW_CALLF0(cancel);
    BURROW_CALLF0(cancel);
    BURROW_CALLF0(cancel);

    CHECK(is_done(c));
    CHECK(is(context_err(c), context_canceled));

    /* And a free after all that is still one free. */
    context_free(c);
}

TEST(cancelling_a_parent_cancels_every_child) {
    Alloc *a = heap_allocator();
    CancelFunc cancel_top;
    CancelFunc cancel_left;
    CancelFunc cancel_right;

    Context top = context_with_cancel(a, context_background(), &cancel_top);
    Context left = context_with_cancel(a, top, &cancel_left);
    Context right = context_with_cancel(a, top, &cancel_right);
    Context deep = context_with_cancel(a, left, &cancel_left);

    CHECK(!BURROW_CONTEXT_IS_NIL(deep));
    CHECK(!is_done(left));
    CHECK(!is_done(right));
    CHECK(!is_done(deep));

    BURROW_CALLF0(cancel_top);

    CHECK(is_done(top));
    CHECK(is_done(left));
    CHECK(is_done(right));
    CHECK(is_done(deep));
    CHECK(is(context_err(deep), context_canceled));

    context_free(deep);
    context_free(right);
    context_free(left);
    context_free(top);
}

TEST(cancelling_a_child_leaves_the_parent_alone) {
    Alloc *a = heap_allocator();
    CancelFunc cancel_top;
    CancelFunc cancel_child;

    Context top = context_with_cancel(a, context_background(), &cancel_top);
    Context child = context_with_cancel(a, top, &cancel_child);

    BURROW_CALLF0(cancel_child);

    CHECK(is_done(child));
    CHECK(!is_done(top));
    CHECK(BURROW_OK(context_err(top)));

    /* The child took itself out of the parent's list when it was cancelled, so
     * cancelling the parent afterwards walks an empty list. There is no way to
     * see that from out here beyond it not crashing, which is what the
     * sanitizer builds are for. */
    BURROW_CALLF0(cancel_top);
    CHECK(is_done(top));

    context_free(child);
    context_free(top);
}

TEST(a_child_of_something_already_cancelled_starts_cancelled) {
    Alloc *a = heap_allocator();
    CancelFunc cancel_top;
    CancelFunc cancel_child;

    Context top = context_with_cancel(a, context_background(), &cancel_top);
    BURROW_CALLF0(cancel_top);

    Context child = context_with_cancel(a, top, &cancel_child);

    CHECK(!BURROW_CONTEXT_IS_NIL(child));
    CHECK(is_done(child));
    CHECK(is(context_err(child), context_canceled));

    context_free(child);
    context_free(top);
}

/* ----------------------------------------------------------------- WithValue */

TEST(a_value_is_found_through_everything_above_it) {
    Alloc *a = heap_allocator();
    Int id = 99;
    Str name = BURROW_S("gopher");
    CancelFunc cancel;

    Context with = context_with_value(a, context_background(), REQUEST_ID_KEY,
                                      BURROW_ANY(TYPE_INT, &id));
    CHECK(!BURROW_CONTEXT_IS_NIL(with));

    Context cancellable = context_with_cancel(a, with, &cancel);
    Context another =
        context_with_value(a, cancellable, USER_KEY, BURROW_ANY(TYPE_STRING, &name));

    Any got = context_value(another, REQUEST_ID_KEY);
    CHECK(got.t == TYPE_INT);
    CHECK(got.data == &id);
    CHECK_INT_EQ(*(Int *)got.data, 99);

    /* And the value context answers for itself, and for nothing it does not
     * know about. */
    CHECK(!BURROW_ANY_IS_NIL(context_value(another, USER_KEY)));
    CHECK(BURROW_ANY_IS_NIL(context_value(with, USER_KEY)));

    context_free(another);
    context_free(cancellable);
    context_free(with);
}

TEST(an_unknown_key_answers_nothing) {
    Alloc *a = heap_allocator();
    Int id = 1;
    Context with = context_with_value(a, context_background(), REQUEST_ID_KEY,
                                      BURROW_ANY(TYPE_INT, &id));

    /* Same value, different type, which is a different key. Go's interface
     * comparison says so and so does this. */
    Int same = 0;
    CHECK(BURROW_ANY_IS_NIL(context_value(with, BURROW_ANY(TYPE_INT, &same))));
    CHECK(BURROW_ANY_IS_NIL(context_value(with, USER_KEY)));

    context_free(with);
}

TEST(a_later_value_shadows_an_earlier_one_with_the_same_key) {
    Alloc *a = heap_allocator();
    Int first = 1;
    Int second = 2;

    Context outer = context_with_value(a, context_background(), REQUEST_ID_KEY,
                                       BURROW_ANY(TYPE_INT, &first));
    Context inner =
        context_with_value(a, outer, REQUEST_ID_KEY, BURROW_ANY(TYPE_INT, &second));

    CHECK_INT_EQ(*(Int *)context_value(inner, REQUEST_ID_KEY).data, 2);
    CHECK_INT_EQ(*(Int *)context_value(outer, REQUEST_ID_KEY).data, 1);

    context_free(inner);
    context_free(outer);
}

/* -------------------------------------------------------------- the stranger */

TEST(a_deadline_is_whatever_the_parent_says) {
    Alloc *a = heap_allocator();
    Fake f = {0};
    Int id = 7;
    CancelFunc cancel;

    f.has_deadline = true;
    f.when = burrow_nanotime() + TIME_SECOND;

    Context root = fake_context(&f);
    Context with =
        context_with_value(a, root, REQUEST_ID_KEY, BURROW_ANY(TYPE_INT, &id));
    Context cancellable = context_with_cancel(a, with, &cancel);

    int64_t when = 0;
    CHECK(context_deadline(cancellable, &when));
    CHECK_INT_EQ(when, f.when);

    when = 0;
    CHECK(context_deadline(with, &when));
    CHECK_INT_EQ(when, f.when);

    context_free(cancellable);
    context_free(with);
}

TEST(a_value_walk_goes_through_a_stranger_and_comes_back) {
    Alloc *a = heap_allocator();
    Fake f = {0};
    Int outer_val = 5;
    Int inner_val = 6;

    /* Ours, then theirs, then ours again. The middle one only forwards value,
     * which is the one thing every context in the chain has to forward. */
    Context bottom = context_with_value(a, context_background(), REQUEST_ID_KEY,
                                        BURROW_ANY(TYPE_INT, &outer_val));
    f.wrap = true;
    f.inner = bottom;

    Context middle = fake_context(&f);
    Context top =
        context_with_value(a, middle, USER_KEY, BURROW_ANY(TYPE_INT, &inner_val));

    CHECK_INT_EQ(*(Int *)context_value(top, USER_KEY).data, 6);
    CHECK_INT_EQ(*(Int *)context_value(top, REQUEST_ID_KEY).data, 5);

    context_free(top);
    context_free(bottom);
}

TEST(a_stranger_that_is_never_cancelled_needs_no_watching) {
    Alloc *a = heap_allocator();
    Fake f = {0};
    CancelFunc cancel;

    /* No done channel, so there is nothing to wait on and no goroutine is
     * started. That is what makes this test runnable with no runtime at all,
     * which is the assertion: a stranger with no cancellation costs nothing. */
    Context c = context_with_cancel(a, fake_context(&f), &cancel);

    CHECK(!BURROW_CONTEXT_IS_NIL(c));
    CHECK(!is_done(c));

    BURROW_CALLF0(cancel);
    CHECK(is_done(c));

    context_free(c);
}

/* --------------------------------------------------------------- the memory */

TEST(everything_is_given_back_to_the_allocator) {
    Track tr;
    track_init(&tr, heap_allocator());
    track_set_quarantine(&tr, 0);
    Alloc *a = track_allocator(&tr);

    Int id = 3;
    CancelFunc cancel_top;
    CancelFunc cancel_leaf;

    Context top = context_with_cancel(a, context_background(), &cancel_top);
    Context with =
        context_with_value(a, top, REQUEST_ID_KEY, BURROW_ANY(TYPE_INT, &id));
    Context leaf = context_with_cancel(a, with, &cancel_leaf);

    CHECK(track_live(&tr) > 0);

    BURROW_CALLF0(cancel_leaf);
    BURROW_CALLF0(cancel_top);

    context_free(leaf);
    context_free(with);
    context_free(top);

    CHECK_INT_EQ((Int)track_live(&tr), 0);
    CHECK_INT_EQ((Int)track_check(&tr), 0);
    track_free(&tr);
}

TEST(a_context_nobody_cancelled_is_still_freed) {
    Track tr;
    track_init(&tr, heap_allocator());
    track_set_quarantine(&tr, 0);
    Alloc *a = track_allocator(&tr);

    CancelFunc cancel;
    Context top = context_with_cancel(a, context_background(), &cancel);
    Context child = context_with_cancel(a, top, &cancel);

    /* No cancel call anywhere. The free has to do it, or the child is still in
     * the parent's list when the parent's memory goes. */
    context_free(child);
    context_free(top);

    CHECK_INT_EQ((Int)track_live(&tr), 0);
    CHECK_INT_EQ((Int)track_check(&tr), 0);
    track_free(&tr);
}

/* ----------------------------------------------------------------- the clock */

TEST(the_monotonic_clock_only_goes_forwards) {
    int64_t first = burrow_nanotime();
    int64_t last = first;

    for (int i = 0; i < 1000; i++) {
        int64_t now = burrow_nanotime();
        CHECK(now >= last);
        last = now;
    }

    /* A thousand readings take some time, and a clock that answers the same
     * number a thousand times running is not a clock. */
    CHECK(last > first);
}

/* --------------------------------------------------------------- the mistakes
 *
 * Statics, because a local touched by a BURROW_TRY block is a local live across
 * a setjmp, which is the one thing tests/fatal.h asks callers not to write. */

static Alloc *panic_alloc;
static Context panic_parent;

TEST(a_nil_parent_panics) {
    Context none = {NULL, NULL};
    CancelFunc cancel;
    Int id = 1;

    panic_alloc = heap_allocator();
    panic_parent = none;

    CHECK_PANIC((void)context_with_cancel(panic_alloc, panic_parent, &cancel),
                "cannot create context from nil parent");
    CHECK_PANIC((void)context_with_value(panic_alloc, panic_parent, REQUEST_ID_KEY,
                                         BURROW_ANY(TYPE_INT, &id)),
                "cannot create context from nil parent");
}

TEST(a_bad_key_panics) {
    Any none = {NULL, NULL};
    Slice s = {NULL, 0, 0, NULL};
    Int id = 1;

    panic_alloc = heap_allocator();
    panic_parent = context_background();

    CHECK_PANIC((void)context_with_value(panic_alloc, panic_parent, none,
                                         BURROW_ANY(TYPE_INT, &id)),
                "nil key");
    CHECK_PANIC((void)context_with_value(panic_alloc, panic_parent,
                                         BURROW_ANY(&slice_of_int, &s),
                                         BURROW_ANY(TYPE_INT, &id)),
                "key is not comparable");
}

TEST(a_nil_context_has_no_methods) {
    Context none = {NULL, NULL};

    panic_parent = none;

    CHECK_RUNTIME_ERROR((void)context_err(panic_parent),
                        "runtime error: invalid memory address or nil pointer "
                        "dereference");
    CHECK_RUNTIME_ERROR((void)context_done(panic_parent),
                        "runtime error: invalid memory address or nil pointer "
                        "dereference");
}

static Fake foreign;

TEST(freeing_a_context_this_package_did_not_make_stops_the_program) {
    panic_parent = fake_context(&foreign);

    CHECK_FATAL(context_free(panic_parent),
                "context: freeing a context this package did not make");
}

/* ------------------------------------------------------------- with a runtime
 *
 * Everything below here needs goroutines. The results come back through
 * atomics, because the harness counts checks in two plain ints and a second
 * thread touching those is a race in the test rather than in the package. */

static Alloc *rt_alloc;
static Context rt_ctx;
static CancelFunc rt_cancel;
static Chan *rt_ready;
static Chan *rt_stranger_done;
static Fake rt_fake;
static uint32_t rt_woke;
static uint32_t rt_err_was_canceled;

static void reset(void) {
    rt_woke = 0;
    rt_err_was_canceled = 0;
}

static void waiter(void *env) {
    (void)env;

    /* Say we are here, then block until somebody cancels. */
    chan_close(rt_ready);
    (void)chan_recv(context_done(rt_ctx), NULL);

    if (is(context_err(rt_ctx), context_canceled))
        burrow__atomic_store_release_u32(&rt_err_was_canceled, 1);
    burrow__atomic_store_release_u32(&rt_woke, 1);
}

static void waiter_body(void *env) {
    (void)env;

    rt_ready = chan_make(rt_alloc, TYPE_UINT8, 0);
    if (rt_ready == NULL)
        return;

    rt_ctx = context_with_cancel(rt_alloc, context_background(), &rt_cancel);
    if (BURROW_CONTEXT_IS_NIL(rt_ctx))
        return;

    if (!go(BURROW_FN(Func, waiter, NULL)))
        return;

    /* Wait for the goroutine to be inside the receive rather than merely
     * started, so that this is a test of waking a parked goroutine and not of
     * a goroutine finding the channel already closed. */
    (void)chan_recv(rt_ready, NULL);
    time_sleep(TIME_MILLISECOND);

    BURROW_CALLF0(rt_cancel);

    /* A cancel does not wait for anybody, so this does. */
    while (burrow__atomic_load_acquire_u32(&rt_woke) == 0)
        time_sleep(TIME_MILLISECOND);
}

TEST(a_goroutine_parked_on_done_wakes_up_when_somebody_cancels) {
    reset();
    rt_alloc = heap_allocator();

    runtime_main(BURROW_FN(Func, waiter_body, NULL));

    CHECK_INT_EQ(burrow__atomic_load_acquire_u32(&rt_woke), 1);
    CHECK_INT_EQ(burrow__atomic_load_acquire_u32(&rt_err_was_canceled), 1);
    CHECK(is_done(rt_ctx));

    context_free(rt_ctx);
    chan_free(rt_ready);
}

static void stranger_body(void *env) {
    (void)env;

    rt_stranger_done = chan_make(rt_alloc, TYPE_UINT8, 0);
    if (rt_stranger_done == NULL)
        return;

    rt_fake = (Fake){0};
    rt_fake.done = rt_stranger_done;

    rt_ctx = context_with_cancel(rt_alloc, fake_context(&rt_fake), &rt_cancel);
    if (BURROW_CONTEXT_IS_NIL(rt_ctx))
        return;

    if (is_done(rt_ctx))
        return;

    /* The stranger is cancelled the only way a stranger can be, which is by
     * doing whatever it does. The watcher goroutine has to notice. */
    rt_fake.err = context_deadline_exceeded;
    chan_close(rt_stranger_done);

    for (int i = 0; i < 1000 && !is_done(rt_ctx); i++)
        time_sleep(TIME_MILLISECOND);

    if (is_done(rt_ctx))
        burrow__atomic_store_release_u32(&rt_woke, 1);
    if (is(context_err(rt_ctx), context_deadline_exceeded))
        burrow__atomic_store_release_u32(&rt_err_was_canceled, 1);
}

TEST(a_parent_from_outside_the_package_still_cancels_what_is_under_it) {
    reset();
    rt_alloc = heap_allocator();

    runtime_main(BURROW_FN(Func, stranger_body, NULL));

    CHECK_INT_EQ(burrow__atomic_load_acquire_u32(&rt_woke), 1);

    /* And the error it reports is the stranger's own, not this package's, which
     * is what makes a custom context worth writing. */
    CHECK_INT_EQ(burrow__atomic_load_acquire_u32(&rt_err_was_canceled), 1);

    context_free(rt_ctx);
    chan_free(rt_stranger_done);
}

static Context rt_wrapped;
static CancelFunc rt_wrapped_cancel;

static void wrapper_body(void *env) {
    (void)env;

    rt_stranger_done = chan_make(rt_alloc, TYPE_UINT8, 0);
    if (rt_stranger_done == NULL)
        return;

    /* One of ours, wrapped by a stranger that forwards value and answers done
     * out of a channel of its own. Joining the inner node's child list would
     * mean the wrapper's cancellation never arrived, and cancelling the inner
     * one would wrongly cancel what is above the wrapper. */
    rt_wrapped =
        context_with_cancel(rt_alloc, context_background(), &rt_wrapped_cancel);
    if (BURROW_CONTEXT_IS_NIL(rt_wrapped))
        return;

    rt_fake = (Fake){0};
    rt_fake.done = rt_stranger_done;
    rt_fake.wrap = true;
    rt_fake.inner = rt_wrapped;

    rt_ctx = context_with_cancel(rt_alloc, fake_context(&rt_fake), &rt_cancel);
    if (BURROW_CONTEXT_IS_NIL(rt_ctx))
        return;

    BURROW_CALLF0(rt_wrapped_cancel);
    time_sleep(10 * TIME_MILLISECOND);

    if (!is_done(rt_ctx))
        burrow__atomic_store_release_u32(&rt_woke, 1);

    /* And the wrapper's own cancellation does arrive. */
    rt_fake.err = context_canceled;
    chan_close(rt_stranger_done);

    for (int i = 0; i < 1000 && !is_done(rt_ctx); i++)
        time_sleep(TIME_MILLISECOND);

    if (is_done(rt_ctx))
        burrow__atomic_store_release_u32(&rt_err_was_canceled, 1);
}

TEST(a_wrapper_that_replaces_done_is_not_mistaken_for_what_it_wraps) {
    reset();
    rt_alloc = heap_allocator();

    runtime_main(BURROW_FN(Func, wrapper_body, NULL));

    CHECK_INT_EQ(burrow__atomic_load_acquire_u32(&rt_woke), 1);
    CHECK_INT_EQ(burrow__atomic_load_acquire_u32(&rt_err_was_canceled), 1);

    context_free(rt_ctx);
    context_free(rt_wrapped);
    chan_free(rt_stranger_done);
}

static Track rt_track;

static void early_free_body(void *env) {
    (void)env;

    /* From the heap and not from the tracked allocator, because the test below
     * watches the tracked allocator go back to empty and this channel outlives
     * the run. The only thing the tracker should have in it is the context. */
    rt_stranger_done = chan_make(heap_allocator(), TYPE_UINT8, 0);
    if (rt_stranger_done == NULL)
        return;

    rt_fake = (Fake){0};
    rt_fake.done = rt_stranger_done;

    Context c = context_with_cancel(rt_alloc, fake_context(&rt_fake), &rt_cancel);
    if (BURROW_CONTEXT_IS_NIL(c))
        return;

    /* Freed while the watcher goroutine is still parked on two channels. The
     * free cancels, which closes the channel the watcher is waiting on, and
     * then puts down one of the two references. The watcher puts down the other
     * one and does the freeing, whenever it gets round to it. */
    context_free(c);

    /* Long enough for the watcher to wake on a channel that is already closed
     * and run three lines. The tracker is deliberately not asked anything here:
     * it keeps a plain count and a plain table with no lock on either, so
     * reading it from this goroutine while the watcher frees through it is a
     * race in the test rather than in the package. The counting happens after
     * runtime_main has returned and there is one thread left. */
    time_sleep(100 * TIME_MILLISECOND);
}

TEST(a_context_with_a_watcher_can_be_freed_before_the_watcher_wakes) {
    reset();
    track_init(&rt_track, heap_allocator());
    track_set_quarantine(&rt_track, 0);
    rt_alloc = track_allocator(&rt_track);

    runtime_main(BURROW_FN(Func, early_free_body, NULL));

    /* Nothing left: the context node and the done channel that went with it
     * both came back, one of them from a goroutine the caller never saw. */
    CHECK_INT_EQ((Int)track_live(&rt_track), 0);
    CHECK_INT_EQ((Int)track_check(&rt_track), 0);

    chan_free(rt_stranger_done);
    track_free(&rt_track);
}

int main(void) {
    RUN(the_root_is_never_cancelled_and_carries_nothing);
    RUN(background_and_todo_are_not_the_same_context);

    RUN(a_cancel_closes_the_done_channel_and_sets_the_error);
    RUN(cancelling_twice_changes_nothing);
    RUN(cancelling_a_parent_cancels_every_child);
    RUN(cancelling_a_child_leaves_the_parent_alone);
    RUN(a_child_of_something_already_cancelled_starts_cancelled);

    RUN(a_value_is_found_through_everything_above_it);
    RUN(an_unknown_key_answers_nothing);
    RUN(a_later_value_shadows_an_earlier_one_with_the_same_key);

    RUN(a_deadline_is_whatever_the_parent_says);
    RUN(a_value_walk_goes_through_a_stranger_and_comes_back);
    RUN(a_stranger_that_is_never_cancelled_needs_no_watching);

    RUN(everything_is_given_back_to_the_allocator);
    RUN(a_context_nobody_cancelled_is_still_freed);

    RUN(the_monotonic_clock_only_goes_forwards);

    RUN(a_nil_parent_panics);
    RUN(a_bad_key_panics);
    RUN(a_nil_context_has_no_methods);
    RUN(freeing_a_context_this_package_did_not_make_stops_the_program);

    RUN(a_goroutine_parked_on_done_wakes_up_when_somebody_cancels);
    RUN(a_parent_from_outside_the_package_still_cancels_what_is_under_it);
    RUN(a_wrapper_that_replaces_done_is_not_mistaken_for_what_it_wraps);
    RUN(a_context_with_a_watcher_can_be_freed_before_the_watcher_wakes);

    return harness_report("context");
}
