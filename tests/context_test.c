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
 * The second half starts the runtime, because three things need it. A goroutine
 * parked on a done channel is the whole point of the package, the fallback path
 * for a parent this package did not make is a goroutine watching two channels,
 * and a deadline is a timer in the heap of a P. Those tests follow the rule
 * tests/sched_test.c sets out: what a child goroutine finds out it says through
 * an atomic.
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

/* Two reasons to cancel something, spelled out rather than built with
 * BURROW_SENTINEL_ERROR because that macro makes the Error extern and a test
 * file has no header to declare it in. The shape is the macro's shape. */
static const Str too_slow_text = {(const Byte *)"too slow", 8};
static const Error err_too_slow = {&burrow_sentinel_error_vt, &too_slow_text};

static const Str gave_up_text = {(const Byte *)"gave up", 7};
static const Error err_gave_up = {&burrow_sentinel_error_vt, &gave_up_text};

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
    context_release(c);
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
    ContextCancelFunc cancel;
    Context c = context_with_cancel(a, context_background(), &cancel);

    CHECK(!BURROW_CONTEXT_IS_NIL(c));
    CHECK(context_done(c) != NULL);
    CHECK(BURROW_OK(context_err(c)));
    CHECK(!is_done(c));

    BURROW_CALLF0(cancel);

    CHECK(is_done(c));
    CHECK(is(context_err(c), context_canceled));
    CHECK_STR_EQ((const char *)error_text(context_err(c)).p, "context canceled");

    /* A cancel says nothing about a deadline, so the parent's answer stands. */
    int64_t when = 0;
    CHECK(!context_deadline(c, &when));

    context_release(c);
}

TEST(cancelling_twice_changes_nothing) {
    Alloc *a = heap_allocator();
    ContextCancelFunc cancel;
    Context c = context_with_cancel(a, context_background(), &cancel);

    BURROW_CALLF0(cancel);
    BURROW_CALLF0(cancel);
    BURROW_CALLF0(cancel);

    CHECK(is_done(c));
    CHECK(is(context_err(c), context_canceled));

    /* And a free after all that is still one free. */
    context_release(c);
}

TEST(cancelling_a_parent_cancels_every_child) {
    Alloc *a = heap_allocator();
    ContextCancelFunc cancel_top;
    ContextCancelFunc cancel_left;
    ContextCancelFunc cancel_right;

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

    context_release(deep);
    context_release(right);
    context_release(left);
    context_release(top);
}

TEST(cancelling_a_child_leaves_the_parent_alone) {
    Alloc *a = heap_allocator();
    ContextCancelFunc cancel_top;
    ContextCancelFunc cancel_child;

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

    context_release(child);
    context_release(top);
}

TEST(a_child_of_something_already_cancelled_starts_cancelled) {
    Alloc *a = heap_allocator();
    ContextCancelFunc cancel_top;
    ContextCancelFunc cancel_child;

    Context top = context_with_cancel(a, context_background(), &cancel_top);
    BURROW_CALLF0(cancel_top);

    Context child = context_with_cancel(a, top, &cancel_child);

    CHECK(!BURROW_CONTEXT_IS_NIL(child));
    CHECK(is_done(child));
    CHECK(is(context_err(child), context_canceled));

    context_release(child);
    context_release(top);
}

/* ------------------------------------------------------- WithCancelCause, Cause */

TEST(a_cause_says_why_where_the_error_only_says_that) {
    Alloc *a = heap_allocator();
    ContextCancelCauseFunc cancel;
    Context c = context_with_cancel_cause(a, context_background(), &cancel);

    CHECK(!BURROW_CONTEXT_IS_NIL(c));
    CHECK(BURROW_OK(context_cause(c)));

    BURROW_CALLF(cancel, err_too_slow);

    CHECK(is_done(c));

    /* The error is the same one a plain cancel gives, because every caller
     * that only wants to know whether to stop should not have to learn a new
     * error to compare against. The reason is the extra. */
    CHECK(is(context_err(c), context_canceled));
    CHECK(is(context_cause(c), err_too_slow));
    CHECK_STR_EQ((const char *)error_text(context_cause(c)).p, "too slow");

    context_release(c);
}

TEST(a_cancel_with_no_reason_leaves_the_cause_equal_to_the_error) {
    Alloc *a = heap_allocator();
    ContextCancelCauseFunc cancel;
    Context c = context_with_cancel_cause(a, context_background(), &cancel);

    BURROW_CALLF(cancel, BURROW_NO_ERROR);

    /* Nothing in particular went wrong, so the cause is the cancellation
     * itself and context_cause still has an answer. Asking both questions is
     * never necessary. */
    CHECK(is(context_err(c), context_canceled));
    CHECK(is(context_cause(c), context_canceled));

    context_release(c);
}

TEST(the_first_reason_is_the_one_that_sticks) {
    Alloc *a = heap_allocator();
    ContextCancelCauseFunc cancel;
    Context c = context_with_cancel_cause(a, context_background(), &cancel);

    BURROW_CALLF(cancel, err_too_slow);
    BURROW_CALLF(cancel, err_gave_up);
    BURROW_CALLF(cancel, BURROW_NO_ERROR);

    CHECK(is(context_cause(c), err_too_slow));

    context_release(c);
}

TEST(a_live_context_has_no_cause) {
    Alloc *a = heap_allocator();
    ContextCancelCauseFunc cancel;
    Context c = context_with_cancel_cause(a, context_background(), &cancel);

    CHECK(BURROW_OK(context_cause(c)));
    CHECK(BURROW_OK(context_cause(context_background())));
    CHECK(BURROW_OK(context_cause(context_todo())));

    BURROW_CALLF(cancel, err_too_slow);
    context_release(c);
}

TEST(a_reason_given_at_the_top_is_the_answer_at_the_bottom) {
    Alloc *a = heap_allocator();
    ContextCancelCauseFunc cancel_top;
    ContextCancelFunc cancel_leaf;
    Int id = 4;

    Context top = context_with_cancel_cause(a, context_background(), &cancel_top);
    Context with =
        context_with_value(a, top, REQUEST_ID_KEY, BURROW_ANY(TYPE_INT, &id));
    Context leaf = context_with_cancel(a, with, &cancel_leaf);

    BURROW_CALLF(cancel_top, err_too_slow);

    /* The reason travels down with the cancellation, so a function three
     * levels away from the call that gave up learns why without anyone
     * passing it along by hand. */
    CHECK(is(context_err(leaf), context_canceled));
    CHECK(is(context_cause(leaf), err_too_slow));
    CHECK(is(context_cause(top), err_too_slow));

    context_release(leaf);
    context_release(with);
    context_release(top);
}

TEST(a_cause_given_below_stays_below) {
    Alloc *a = heap_allocator();
    ContextCancelCauseFunc cancel_top;
    ContextCancelCauseFunc cancel_leaf;

    Context top = context_with_cancel_cause(a, context_background(), &cancel_top);
    Context leaf = context_with_cancel_cause(a, top, &cancel_leaf);

    BURROW_CALLF(cancel_leaf, err_gave_up);

    CHECK(is(context_cause(leaf), err_gave_up));
    CHECK(BURROW_OK(context_cause(top)));

    /* And the parent's own reason afterwards does not reach down and rewrite
     * what the child already settled on. */
    BURROW_CALLF(cancel_top, err_too_slow);
    CHECK(is(context_cause(leaf), err_gave_up));
    CHECK(is(context_cause(top), err_too_slow));

    context_release(leaf);
    context_release(top);
}

TEST(a_child_of_something_cancelled_with_a_reason_starts_with_it) {
    Alloc *a = heap_allocator();
    ContextCancelCauseFunc cancel_top;
    ContextCancelFunc cancel_child;

    Context top = context_with_cancel_cause(a, context_background(), &cancel_top);
    BURROW_CALLF(cancel_top, err_too_slow);

    Context child = context_with_cancel(a, top, &cancel_child);

    CHECK(is_done(child));
    CHECK(is(context_cause(child), err_too_slow));

    context_release(child);
    context_release(top);
}

TEST(a_plain_cancel_context_still_has_a_cause) {
    Alloc *a = heap_allocator();
    ContextCancelFunc cancel;
    Context c = context_with_cancel(a, context_background(), &cancel);

    BURROW_CALLF0(cancel);

    /* Nobody asked for a reason here, so the reason is the cancellation.
     * Code that reads context_cause works against contexts built by code that
     * has never heard of it. */
    CHECK(is(context_cause(c), context_canceled));

    context_release(c);
}

TEST(a_context_that_cannot_be_cancelled_has_no_cause) {
    Alloc *a = heap_allocator();
    Fake f = {0};
    Int id = 8;

    Context with = context_with_value(a, context_background(), REQUEST_ID_KEY,
                                      BURROW_ANY(TYPE_INT, &id));

    CHECK(BURROW_OK(context_cause(with)));
    CHECK(BURROW_OK(context_cause(fake_context(&f))));

    context_release(with);
}

/* ------------------------------------------------------------- WithoutCancel */

TEST(work_that_outlives_its_request_keeps_the_values) {
    Alloc *a = heap_allocator();
    ContextCancelFunc cancel;
    Int id = 11;

    Context req = context_with_cancel(a, context_background(), &cancel);
    Context with =
        context_with_value(a, req, REQUEST_ID_KEY, BURROW_ANY(TYPE_INT, &id));
    Context detached = context_without_cancel(a, with);

    CHECK(!BURROW_CONTEXT_IS_NIL(detached));
    CHECK_INT_EQ(*(Int *)context_value(detached, REQUEST_ID_KEY).data, 11);

    BURROW_CALLF0(cancel);

    /* The request is over and the logging or the cleanup that hangs off it is
     * not. Everything the handler put in the context is still readable, and
     * nothing about the context says to stop. */
    CHECK(is_done(req));
    CHECK_INT_EQ(*(Int *)context_value(detached, REQUEST_ID_KEY).data, 11);
    CHECK(context_done(detached) == NULL);
    CHECK(BURROW_OK(context_err(detached)));

    context_release(detached);
    context_release(with);
    context_release(req);
}

TEST(nothing_under_a_without_cancel_is_reached_by_the_parent) {
    Alloc *a = heap_allocator();
    ContextCancelFunc cancel_req;
    ContextCancelFunc cancel_task;

    Context req = context_with_cancel(a, context_background(), &cancel_req);
    Context detached = context_without_cancel(a, req);
    Context task = context_with_cancel(a, detached, &cancel_task);

    BURROW_CALLF0(cancel_req);

    /* The break is the point. A cancellable context under a detached one is
     * still cancellable, it just is not cancelled by what it came from. */
    CHECK(is_done(req));
    CHECK(!is_done(task));

    BURROW_CALLF0(cancel_task);
    CHECK(is_done(task));

    context_release(task);
    context_release(detached);
    context_release(req);
}

TEST(a_without_cancel_has_no_deadline_and_no_cause) {
    Alloc *a = heap_allocator();
    Fake f = {0};
    ContextCancelCauseFunc cancel;

    f.has_deadline = true;
    f.when = burrow_nanotime() + TIME_SECOND;

    Context req = context_with_cancel_cause(a, fake_context(&f), &cancel);
    Context detached = context_without_cancel(a, req);

    int64_t when = 1234;
    CHECK(!context_deadline(detached, &when));
    CHECK_INT_EQ(when, 1234);

    BURROW_CALLF(cancel, err_too_slow);

    CHECK(BURROW_OK(context_err(detached)));
    CHECK(BURROW_OK(context_cause(detached)));

    context_release(detached);
    context_release(req);
}

TEST(a_detached_context_is_given_back_to_the_allocator) {
    Track tr;
    track_init(&tr, heap_allocator());
    track_set_quarantine(&tr, 0);
    Alloc *a = track_allocator(&tr);

    ContextCancelFunc cancel;
    Context req = context_with_cancel(a, context_background(), &cancel);
    Context detached = context_without_cancel(a, req);
    Context task = context_with_cancel(a, detached, &cancel);

    CHECK(track_live(&tr) > 0);

    context_release(task);
    context_release(detached);
    context_release(req);

    CHECK_INT_EQ((Int)track_live(&tr), 0);
    CHECK_INT_EQ((Int)track_check(&tr), 0);
    track_free(&tr);
}

/* ----------------------------------------------------------------- AfterFunc
 *
 * The half that does not need a scheduler. Stopping never starts a goroutine,
 * and a registration under a parent this package made never gets a watcher, so
 * everything here except actually running the function is testable flat. */

static uint32_t after_ran;

static void count_a_run(void *env) {
    (void)env;

    (void)burrow__atomic_add_u32(&after_ran, 1);
}

TEST(a_stopped_after_func_never_runs) {
    Alloc *a = heap_allocator();
    ContextCancelFunc cancel;
    StopFunc stop;

    after_ran = 0;

    Context req = context_with_cancel(a, context_background(), &cancel);
    Context reg = context_after_func(a, req, BURROW_FN(Func, count_a_run, NULL), &stop);

    CHECK(!BURROW_CONTEXT_IS_NIL(reg));
    CHECK(BURROW_CALLF0(stop));

    /* Stopping cancels the registration, because one left in the parent's child
     * list would sit there for as long as the parent does. */
    CHECK(is_done(reg));

    BURROW_CALLF0(cancel);

    CHECK_INT_EQ((Int)burrow__atomic_load_u32(&after_ran), 0);

    context_release(reg);
    context_release(req);
}

TEST(only_one_stop_ever_answers_true) {
    Alloc *a = heap_allocator();
    ContextCancelFunc cancel;
    StopFunc stop;

    after_ran = 0;

    Context req = context_with_cancel(a, context_background(), &cancel);
    Context reg = context_after_func(a, req, BURROW_FN(Func, count_a_run, NULL), &stop);

    CHECK(BURROW_CALLF0(stop));
    CHECK(!BURROW_CALLF0(stop));
    CHECK(!BURROW_CALLF0(stop));

    BURROW_CALLF0(cancel);
    CHECK_INT_EQ((Int)burrow__atomic_load_u32(&after_ran), 0);

    context_release(reg);
    context_release(req);
}

TEST(freeing_a_registration_is_not_a_reason_to_run_it) {
    Alloc *a = heap_allocator();
    ContextCancelFunc cancel;
    StopFunc stop;

    after_ran = 0;

    Context req = context_with_cancel(a, context_background(), &cancel);
    Context reg = context_after_func(a, req, BURROW_FN(Func, count_a_run, NULL), &stop);

    /* Given back without ever being stopped. Handing something back is not a
     * reason for its cleanup to run, and a caller who wanted the function to
     * run has a stop function to not call. */
    context_release(reg);

    BURROW_CALLF0(cancel);
    CHECK_INT_EQ((Int)burrow__atomic_load_u32(&after_ran), 0);

    context_release(req);
}

TEST(a_registration_answers_the_four_questions_like_anything_else) {
    Alloc *a = heap_allocator();
    Fake f = {0};
    ContextCancelFunc cancel;
    StopFunc stop;
    Int id = 12;

    f.has_deadline = true;
    f.when = burrow_nanotime() + TIME_SECOND;

    Context req = context_with_cancel(a, fake_context(&f), &cancel);
    Context with =
        context_with_value(a, req, REQUEST_ID_KEY, BURROW_ANY(TYPE_INT, &id));
    Context reg =
        context_after_func(a, with, BURROW_FN(Func, count_a_run, NULL), &stop);

    /* Go hands back only the stop function and keeps the node to itself. This
     * hands the node back too, because something has to free it, and then it
     * may as well be usable. */
    int64_t when = 0;
    CHECK(context_deadline(reg, &when));
    CHECK_INT_EQ(when, f.when);
    CHECK(context_done(reg) != NULL);
    CHECK(BURROW_OK(context_err(reg)));
    CHECK_INT_EQ(*(Int *)context_value(reg, REQUEST_ID_KEY).data, 12);

    /* Stopped rather than cancelled from above, because a cancellation that
     * reaches this node starts a goroutine and there is no scheduler here. The
     * parent's cancellation getting through is the runtime half's subject. */
    CHECK(BURROW_CALLF0(stop));
    CHECK(is_done(reg));
    CHECK(is(context_err(reg), context_canceled));

    BURROW_CALLF0(cancel);

    context_release(reg);
    context_release(with);
    context_release(req);
}

TEST(a_registration_is_given_back_to_the_allocator) {
    Track tr;
    track_init(&tr, heap_allocator());
    track_set_quarantine(&tr, 0);
    Alloc *a = track_allocator(&tr);

    ContextCancelFunc cancel;
    StopFunc stop;

    Context req = context_with_cancel(a, context_background(), &cancel);
    Context reg = context_after_func(a, req, BURROW_FN(Func, count_a_run, NULL), &stop);

    CHECK(track_live(&tr) > 0);
    CHECK(BURROW_CALLF0(stop));

    context_release(reg);
    context_release(req);

    CHECK_INT_EQ((Int)track_live(&tr), 0);
    CHECK_INT_EQ((Int)track_check(&tr), 0);
    track_free(&tr);
}

/* ----------------------------------------------------------------- WithValue */

TEST(a_value_is_found_through_everything_above_it) {
    Alloc *a = heap_allocator();
    Int id = 99;
    Str name = BURROW_S("gopher");
    ContextCancelFunc cancel;

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

    context_release(another);
    context_release(cancellable);
    context_release(with);
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

    context_release(with);
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

    context_release(inner);
    context_release(outer);
}

/* -------------------------------------------------------------- the stranger */

TEST(a_deadline_is_whatever_the_parent_says) {
    Alloc *a = heap_allocator();
    Fake f = {0};
    Int id = 7;
    ContextCancelFunc cancel;

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

    context_release(cancellable);
    context_release(with);
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

    context_release(top);
    context_release(bottom);
}

TEST(a_stranger_that_is_never_cancelled_needs_no_watching) {
    Alloc *a = heap_allocator();
    Fake f = {0};
    ContextCancelFunc cancel;

    /* No done channel, so there is nothing to wait on and no goroutine is
     * started. That is what makes this test runnable with no runtime at all,
     * which is the assertion: a stranger with no cancellation costs nothing. */
    Context c = context_with_cancel(a, fake_context(&f), &cancel);

    CHECK(!BURROW_CONTEXT_IS_NIL(c));
    CHECK(!is_done(c));

    BURROW_CALLF0(cancel);
    CHECK(is_done(c));

    context_release(c);
}

/* --------------------------------------------------------------- the memory */

TEST(everything_is_given_back_to_the_allocator) {
    Track tr;
    track_init(&tr, heap_allocator());
    track_set_quarantine(&tr, 0);
    Alloc *a = track_allocator(&tr);

    Int id = 3;
    ContextCancelFunc cancel_top;
    ContextCancelFunc cancel_leaf;

    Context top = context_with_cancel(a, context_background(), &cancel_top);
    Context with =
        context_with_value(a, top, REQUEST_ID_KEY, BURROW_ANY(TYPE_INT, &id));
    Context leaf = context_with_cancel(a, with, &cancel_leaf);

    CHECK(track_live(&tr) > 0);

    BURROW_CALLF0(cancel_leaf);
    BURROW_CALLF0(cancel_top);

    context_release(leaf);
    context_release(with);
    context_release(top);

    CHECK_INT_EQ((Int)track_live(&tr), 0);
    CHECK_INT_EQ((Int)track_check(&tr), 0);
    track_free(&tr);
}

TEST(a_context_nobody_cancelled_is_still_freed) {
    Track tr;
    track_init(&tr, heap_allocator());
    track_set_quarantine(&tr, 0);
    Alloc *a = track_allocator(&tr);

    ContextCancelFunc cancel;
    Context top = context_with_cancel(a, context_background(), &cancel);
    Context child = context_with_cancel(a, top, &cancel);

    /* No cancel call anywhere. The free has to do it, or the child is still in
     * the parent's list when the parent's memory goes. */
    context_release(child);
    context_release(top);

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

/* Something to hand context_after_func in the test below. It never runs, since
 * the call it is passed to panics before the registration exists. */
static void panic_never_runs(void *env) {
    (void)env;
}

TEST(a_nil_parent_panics) {
    Context none = {NULL, NULL};
    ContextCancelFunc cancel;
    Int id = 1;

    panic_alloc = heap_allocator();
    panic_parent = none;

    CHECK_PANIC((void)context_with_cancel(panic_alloc, panic_parent, &cancel),
                "cannot create context from nil parent");
    CHECK_PANIC((void)context_with_value(panic_alloc, panic_parent, REQUEST_ID_KEY,
                                         BURROW_ANY(TYPE_INT, &id)),
                "cannot create context from nil parent");

    /* The two deadline calls check the parent before they check anything else,
     * which is why these are here and not down with the runtime tests: a nil
     * parent is caught before the missing goroutine is. */
    CHECK_PANIC((void)context_with_deadline(panic_alloc, panic_parent, 0, NULL),
                "cannot create context from nil parent");
    CHECK_PANIC(
        (void)context_with_timeout(panic_alloc, panic_parent, TIME_SECOND, NULL),
        "cannot create context from nil parent");

    StopFunc stop;
    CHECK_PANIC((void)context_after_func(panic_alloc, panic_parent,
                                         BURROW_FN(Func, panic_never_runs, NULL),
                                         &stop),
                "cannot create context from nil parent");

    ContextCancelCauseFunc cancel_cause;
    CHECK_PANIC(
        (void)context_with_cancel_cause(panic_alloc, panic_parent, &cancel_cause),
        "cannot create context from nil parent");
    CHECK_PANIC((void)context_without_cancel(panic_alloc, panic_parent),
                "cannot create context from nil parent");
    CHECK_PANIC((void)context_with_deadline_cause(panic_alloc, panic_parent, 0,
                                                  err_too_slow, NULL),
                "cannot create context from nil parent");
    CHECK_PANIC((void)context_with_timeout_cause(panic_alloc, panic_parent, TIME_SECOND,
                                                 err_too_slow, NULL),
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
    CHECK_RUNTIME_ERROR((void)context_cause(panic_parent),
                        "runtime error: invalid memory address or nil pointer "
                        "dereference");
}

/* Here rather than with the other deadline tests, because the whole point of it
 * is that there is no runtime running. A P is where the timer heap lives, and a
 * thread the runtime did not start has no P to put one on. */
TEST(a_deadline_off_a_goroutine_stops_the_program) {
    panic_alloc = heap_allocator();
    panic_parent = context_background();

    CHECK_FATAL(
        (void)context_with_timeout(panic_alloc, panic_parent, TIME_SECOND, NULL),
        "context: a deadline needs a goroutine to put the timer on");
    CHECK_FATAL((void)context_with_deadline(panic_alloc, panic_parent, 0, NULL),
                "context: a deadline needs a goroutine to put the timer on");
    CHECK_FATAL((void)context_with_timeout_cause(panic_alloc, panic_parent, TIME_SECOND,
                                                 err_too_slow, NULL),
                "context: a deadline needs a goroutine to put the timer on");
    CHECK_FATAL((void)context_with_deadline_cause(panic_alloc, panic_parent, 0,
                                                  err_too_slow, NULL),
                "context: a deadline needs a goroutine to put the timer on");
}

static Fake foreign;

TEST(freeing_a_context_this_package_did_not_make_stops_the_program) {
    panic_parent = fake_context(&foreign);

    CHECK_FATAL(context_release(panic_parent),
                "context: freeing a context this package did not make");
}

/* ------------------------------------------------------------- with a runtime
 *
 * Everything below here needs goroutines. The results come back through
 * atomics, because the harness counts checks in two plain ints and a second
 * thread touching those is a race in the test rather than in the package. */

static Alloc *rt_alloc;
static Context rt_ctx;
static ContextCancelFunc rt_cancel;
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

    context_release(rt_ctx);
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

    context_release(rt_ctx);
    chan_free(rt_stranger_done);
}

static Context rt_wrapped;
static ContextCancelFunc rt_wrapped_cancel;

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

    context_release(rt_ctx);
    context_release(rt_wrapped);
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
    context_release(c);

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

/* ------------------------------------------------------------ with a deadline
 *
 * All of these need the runtime, including the ones that never arm a timer,
 * because context_with_deadline wants a P to put the timer on and says so on
 * every path rather than only on the ones that use it.
 *
 * So each one is a body that does the work on the goroutine runtime_main
 * starts and leaves what it found in dl below, and a test that starts the
 * runtime and then does the checking. Plain fields and no atomics, because one
 * goroutine writes them and runtime_main has returned before anything reads
 * them.
 *
 * The waiting is polling with a limit rather than a receive on the done
 * channel. A timer that never fires should fail one test rather than hang the
 * whole run, and a limit of several seconds against a deadline of twenty
 * milliseconds is a machine that has stopped rather than a machine that is
 * busy. The one test that does park on the channel is the one whose whole
 * subject is the parking. */

#define DL_SOON (20 * TIME_MILLISECOND)
#define DL_NEVER TIME_HOUR
#define DL_LIMIT (5 * TIME_SECOND)

typedef struct DeadlineResult {
    bool made;
    bool made_child;
    bool has_deadline;
    bool done_at_once;
    bool done_in_the_end;
    bool parent_done;
    bool child_done;
    int64_t started;
    int64_t elapsed;
    int64_t when;
    int64_t parent_when;
    Error err;
    Error err_after;
    Error parent_err;
    Error child_err;
} DeadlineResult;

static DeadlineResult dl;

static void dl_reset(void) {
    DeadlineResult zero = {0};

    dl = zero;
}

static bool wait_done(Context c) {
    int64_t give_up = burrow_nanotime() + DL_LIMIT;

    while (burrow_nanotime() < give_up) {
        if (is_done(c))
            return true;
        time_sleep(TIME_MILLISECOND);
    }
    return false;
}

static void future_body(void *env) {
    (void)env;

    ContextCancelFunc cancel;

    dl.started = burrow_nanotime();

    Context c = context_with_timeout(rt_alloc, context_background(), DL_NEVER, &cancel);
    if (BURROW_CONTEXT_IS_NIL(c))
        return;

    dl.made = true;
    dl.has_deadline = context_deadline(c, &dl.when);
    dl.done_at_once = is_done(c);
    dl.err = context_err(c);

    /* An hour away, so the cancel wins every time and this is the path where
     * the stop takes the timer's reference back. */
    BURROW_CALLF0(cancel);
    dl.done_in_the_end = is_done(c);
    dl.err_after = context_err(c);

    context_release(c);
}

TEST(a_deadline_an_hour_away_leaves_the_cancel_to_win) {
    dl_reset();
    rt_alloc = heap_allocator();

    runtime_main(BURROW_FN(Func, future_body, NULL));

    CHECK(dl.made);
    CHECK(dl.has_deadline);

    /* An hour from the clock reading inside the call, which is at or after the
     * one taken outside it, so the lower bound is exact and the upper bound is
     * however long the call took plus room for a machine having a bad day. */
    CHECK(dl.when >= dl.started + DL_NEVER);
    CHECK(dl.when <= dl.started + DL_NEVER + TIME_SECOND);

    CHECK(!dl.done_at_once);
    CHECK(BURROW_OK(dl.err));
    CHECK(dl.done_in_the_end);
    CHECK(is(dl.err_after, context_canceled));
}

static void past_body(void *env) {
    (void)env;

    ContextCancelFunc cancel;
    Context c =
        context_with_timeout(rt_alloc, context_background(), -TIME_SECOND, &cancel);
    if (BURROW_CONTEXT_IS_NIL(c))
        return;

    dl.made = true;
    dl.done_at_once = is_done(c);
    dl.err = context_err(c);
    dl.has_deadline = context_deadline(c, &dl.when);

    /* A cancel after the fact changes nothing, which is Go's rule everywhere in
     * this package: the first answer is the only answer. */
    BURROW_CALLF0(cancel);
    dl.err_after = context_err(c);
    context_release(c);

    /* The same thing said as an instant rather than as a duration, and with no
     * cancel function asked for at all. */
    Context d = context_with_deadline(rt_alloc, context_background(),
                                      burrow_nanotime() - TIME_SECOND, NULL);
    if (BURROW_CONTEXT_IS_NIL(d))
        return;

    dl.made_child = true;
    dl.child_done = is_done(d);
    dl.child_err = context_err(d);
    context_release(d);
}

TEST(a_deadline_that_has_gone_by_comes_back_already_cancelled) {
    dl_reset();
    rt_alloc = heap_allocator();

    runtime_main(BURROW_FN(Func, past_body, NULL));

    CHECK(dl.made);
    CHECK(dl.done_at_once);
    CHECK(is(dl.err, context_deadline_exceeded));
    CHECK(is(dl.err_after, context_deadline_exceeded));

    /* It still has the deadline it was given, in the past though it is. */
    CHECK(dl.has_deadline);
    CHECK(dl.when < burrow_nanotime());

    CHECK(dl.made_child);
    CHECK(dl.child_done);
    CHECK(is(dl.child_err, context_deadline_exceeded));
}

static void fires_body(void *env) {
    (void)env;

    ContextCancelFunc cancel;

    dl.started = burrow_nanotime();

    Context c = context_with_timeout(rt_alloc, context_background(), DL_SOON, &cancel);
    if (BURROW_CONTEXT_IS_NIL(c))
        return;

    /* Done at once only counts if the look finished before the deadline could
     * have passed. A loaded machine can take this goroutine off its thread for
     * longer than DL_SOON between the two lines, and then a done context is
     * the right answer. */
    dl.made = true;
    bool done = is_done(c);
    dl.done_at_once = done && burrow_nanotime() - dl.started < DL_SOON;
    dl.done_in_the_end = wait_done(c);
    dl.elapsed = burrow_nanotime() - dl.started;
    dl.err = context_err(c);

    /* The cancel that arrives after the timer has fired, which is the path
     * where the stop loses and the callback owns the reference. */
    BURROW_CALLF0(cancel);
    dl.err_after = context_err(c);

    context_release(c);
}

TEST(a_timeout_fires_and_says_the_deadline_went_by) {
    dl_reset();
    rt_alloc = heap_allocator();

    runtime_main(BURROW_FN(Func, fires_body, NULL));

    CHECK(dl.made);
    CHECK(!dl.done_at_once);
    CHECK(dl.done_in_the_end);
    CHECK(is(dl.err, context_deadline_exceeded));
    CHECK(is(dl.err_after, context_deadline_exceeded));

    /* Not early. Late is the machine's business and there is no upper bound
     * worth asserting, which is what burrow/time.h says about every timer. */
    CHECK(dl.elapsed >= DL_SOON);
}

static void parent_sooner_body(void *env) {
    (void)env;

    ContextCancelFunc parent_cancel;
    ContextCancelFunc cancel;

    Context p =
        context_with_timeout(rt_alloc, context_background(), DL_SOON, &parent_cancel);
    if (BURROW_CONTEXT_IS_NIL(p))
        return;

    dl.made = context_deadline(p, &dl.parent_when);

    Context c = context_with_deadline(rt_alloc, p, dl.parent_when + TIME_HOUR, &cancel);
    if (BURROW_CONTEXT_IS_NIL(c)) {
        BURROW_CALLF0(parent_cancel);
        context_release(p);
        return;
    }

    dl.made_child = true;
    dl.has_deadline = context_deadline(c, &dl.when);

    /* No timer of its own, so the only thing that can cancel it is the parent,
     * and the parent is twenty milliseconds from giving up. */
    /* A wait on the child too, and not a look. The parent's channel closes
     * before the cancel reaches the children, in Go as well as here, so a
     * goroutine woken by the first can get to the second before it closes. */
    dl.parent_done = wait_done(p);
    dl.child_done = wait_done(c);
    dl.child_err = context_err(c);

    BURROW_CALLF0(cancel);
    BURROW_CALLF0(parent_cancel);
    context_release(c);
    context_release(p);
}

TEST(a_parent_that_gives_up_sooner_keeps_the_deadline) {
    dl_reset();
    rt_alloc = heap_allocator();

    runtime_main(BURROW_FN(Func, parent_sooner_body, NULL));

    CHECK(dl.made);
    CHECK(dl.made_child);
    CHECK(dl.has_deadline);
    CHECK_INT_EQ((Int)dl.when, (Int)dl.parent_when);
    CHECK(dl.parent_done);
    CHECK(dl.child_done);
    CHECK(is(dl.child_err, context_deadline_exceeded));
}

static void child_sooner_body(void *env) {
    (void)env;

    ContextCancelFunc parent_cancel;
    ContextCancelFunc cancel;

    Context p =
        context_with_timeout(rt_alloc, context_background(), DL_NEVER, &parent_cancel);
    if (BURROW_CONTEXT_IS_NIL(p))
        return;

    Context c = context_with_timeout(rt_alloc, p, DL_SOON, &cancel);
    if (BURROW_CONTEXT_IS_NIL(c)) {
        BURROW_CALLF0(parent_cancel);
        context_release(p);
        return;
    }

    dl.made = true;
    dl.child_done = wait_done(c);
    dl.parent_done = is_done(p);
    dl.child_err = context_err(c);
    dl.parent_err = context_err(p);

    BURROW_CALLF0(cancel);
    BURROW_CALLF0(parent_cancel);
    context_release(c);
    context_release(p);
}

TEST(a_child_with_a_sooner_deadline_fires_on_its_own) {
    dl_reset();
    rt_alloc = heap_allocator();

    runtime_main(BURROW_FN(Func, child_sooner_body, NULL));

    CHECK(dl.made);
    CHECK(dl.child_done);
    CHECK(is(dl.child_err, context_deadline_exceeded));

    /* Nothing goes upwards. The parent has fifty nine minutes left on it. */
    CHECK(!dl.parent_done);
    CHECK(BURROW_OK(dl.parent_err));
}

static void deadline_downwards_body(void *env) {
    (void)env;

    ContextCancelFunc parent_cancel;
    Int id = 7;

    Context p =
        context_with_timeout(rt_alloc, context_background(), DL_SOON, &parent_cancel);
    if (BURROW_CONTEXT_IS_NIL(p))
        return;

    dl.made = context_deadline(p, &dl.parent_when);

    Context kid = context_with_cancel(rt_alloc, p, NULL);
    if (BURROW_CONTEXT_IS_NIL(kid)) {
        BURROW_CALLF0(parent_cancel);
        context_release(p);
        return;
    }

    Context grandkid =
        context_with_value(rt_alloc, kid, REQUEST_ID_KEY, BURROW_ANY(TYPE_INT, &id));
    if (BURROW_CONTEXT_IS_NIL(grandkid)) {
        BURROW_CALLF0(parent_cancel);
        context_release(kid);
        context_release(p);
        return;
    }

    dl.made_child = true;

    /* The deadline is visible from the bottom, through a cancel node and a
     * value node, because neither of them has one of its own. */
    dl.has_deadline = context_deadline(grandkid, &dl.when);

    /* Waiting on each for the reason given in the test above. */
    dl.parent_done = wait_done(p);
    dl.child_done = wait_done(kid);
    dl.child_err = context_err(kid);
    dl.err = context_err(grandkid);

    BURROW_CALLF0(parent_cancel);
    context_release(grandkid);
    context_release(kid);
    context_release(p);
}

TEST(a_deadline_reaches_everything_underneath_it) {
    dl_reset();
    rt_alloc = heap_allocator();

    runtime_main(BURROW_FN(Func, deadline_downwards_body, NULL));

    CHECK(dl.made);
    CHECK(dl.made_child);
    CHECK(dl.has_deadline);
    CHECK_INT_EQ((Int)dl.when, (Int)dl.parent_when);
    CHECK(dl.parent_done);
    CHECK(dl.child_done);
    CHECK(is(dl.child_err, context_deadline_exceeded));
    CHECK(is(dl.err, context_deadline_exceeded));
}

/* The tracked allocator sees everything come back whichever of the two wins,
 * and the two are different paths. The cancel stops the timer and takes its
 * reference back on the spot. The deadline fires, and then the reference goes
 * down on a goroutine the caller never saw and whichever of the two is last
 * does the freeing. */
static void dl_memory(bool let_it_fire) {
    ContextCancelFunc cancel;
    Context p = context_with_timeout(rt_alloc, context_background(),
                                     let_it_fire ? DL_SOON : DL_NEVER, &cancel);
    if (BURROW_CONTEXT_IS_NIL(p))
        return;

    Context kid = context_with_cancel(rt_alloc, p, NULL);
    if (BURROW_CONTEXT_IS_NIL(kid)) {
        BURROW_CALLF0(cancel);
        context_release(p);
        return;
    }

    dl.made = true;
    if (let_it_fire)
        dl.done_in_the_end = wait_done(p);
    else
        BURROW_CALLF0(cancel);

    context_release(kid);
    context_release(p);

    /* Long enough for the callback's goroutine to have finished with the node
     * if it was still inside it. Nothing is asked of the tracker from in here,
     * for the reason the test above this one gives. */
    time_sleep(100 * TIME_MILLISECOND);
}

static void deadline_won_body(void *env) {
    (void)env;
    dl_memory(true);
}

static void cancel_won_body(void *env) {
    (void)env;
    dl_memory(false);
}

TEST(everything_is_given_back_when_the_deadline_wins) {
    dl_reset();
    track_init(&rt_track, heap_allocator());
    track_set_quarantine(&rt_track, 0);
    rt_alloc = track_allocator(&rt_track);

    runtime_main(BURROW_FN(Func, deadline_won_body, NULL));

    CHECK(dl.made);
    CHECK(dl.done_in_the_end);
    CHECK_INT_EQ((Int)track_live(&rt_track), 0);
    CHECK_INT_EQ((Int)track_check(&rt_track), 0);

    track_free(&rt_track);
}

TEST(everything_is_given_back_when_the_cancel_wins) {
    dl_reset();
    track_init(&rt_track, heap_allocator());
    track_set_quarantine(&rt_track, 0);
    rt_alloc = track_allocator(&rt_track);

    runtime_main(BURROW_FN(Func, cancel_won_body, NULL));

    CHECK(dl.made);
    CHECK_INT_EQ((Int)track_live(&rt_track), 0);
    CHECK_INT_EQ((Int)track_check(&rt_track), 0);

    track_free(&rt_track);
}

static void early_deadline_free_body(void *env) {
    (void)env;

    Context c = context_with_timeout(rt_alloc, context_background(), DL_SOON, NULL);
    if (BURROW_CONTEXT_IS_NIL(c))
        return;

    dl.made = true;

    /* Freed with the timer armed and twenty milliseconds still to run. The free
     * cancels, the cancel stops the timer and takes its reference back, and
     * there is nothing left for the callback to walk into. */
    context_release(c);

    time_sleep(DL_SOON + 100 * TIME_MILLISECOND);
}

TEST(a_context_freed_before_its_deadline_takes_the_timer_with_it) {
    dl_reset();
    track_init(&rt_track, heap_allocator());
    track_set_quarantine(&rt_track, 0);
    rt_alloc = track_allocator(&rt_track);

    runtime_main(BURROW_FN(Func, early_deadline_free_body, NULL));

    CHECK(dl.made);
    CHECK_INT_EQ((Int)track_live(&rt_track), 0);
    CHECK_INT_EQ((Int)track_check(&rt_track), 0);

    track_free(&rt_track);
}

static uint32_t rt_err_was_deadline;

static void deadline_waiter(void *env) {
    (void)env;

    chan_close(rt_ready);
    (void)chan_recv(context_done(rt_ctx), NULL);

    if (is(context_err(rt_ctx), context_deadline_exceeded))
        burrow__atomic_store_release_u32(&rt_err_was_deadline, 1);
    burrow__atomic_store_release_u32(&rt_woke, 1);
}

static void deadline_waiter_body(void *env) {
    (void)env;

    rt_ready = chan_make(rt_alloc, TYPE_UINT8, 0);
    if (rt_ready == NULL)
        return;

    rt_ctx = context_with_timeout(rt_alloc, context_background(), DL_SOON, &rt_cancel);
    if (BURROW_CONTEXT_IS_NIL(rt_ctx))
        return;

    if (!go(BURROW_FN(Func, deadline_waiter, NULL)))
        return;

    /* Inside the receive before the deadline can arrive, so that this is a test
     * of a timer waking a parked goroutine rather than of a goroutine finding a
     * channel that was closed while it was being started. */
    (void)chan_recv(rt_ready, NULL);

    while (burrow__atomic_load_acquire_u32(&rt_woke) == 0)
        time_sleep(TIME_MILLISECOND);

    dl.made = true;
}

TEST(a_goroutine_parked_on_done_wakes_up_when_the_deadline_goes_by) {
    dl_reset();
    reset();
    rt_err_was_deadline = 0;
    rt_alloc = heap_allocator();

    runtime_main(BURROW_FN(Func, deadline_waiter_body, NULL));

    CHECK(dl.made);
    CHECK_INT_EQ(burrow__atomic_load_acquire_u32(&rt_woke), 1);
    CHECK_INT_EQ(burrow__atomic_load_acquire_u32(&rt_err_was_deadline), 1);
    CHECK(is_done(rt_ctx));

    BURROW_CALLF0(rt_cancel);
    context_release(rt_ctx);
    chan_free(rt_ready);
}

static void far_future_body(void *env) {
    (void)env;

    /* The largest duration there is, which is the one that would run off the
     * end of the clock if the addition were written the obvious way. */
    Context c = context_with_timeout(rt_alloc, context_background(), INT64_MAX, NULL);
    if (BURROW_CONTEXT_IS_NIL(c))
        return;

    dl.made = true;
    dl.has_deadline = context_deadline(c, &dl.when);
    dl.done_at_once = is_done(c);
    dl.err = context_err(c);

    context_release(c);
}

TEST(a_timeout_too_big_to_add_lands_at_the_end_of_the_clock) {
    dl_reset();
    rt_alloc = heap_allocator();

    runtime_main(BURROW_FN(Func, far_future_body, NULL));

    CHECK(dl.made);
    CHECK(dl.has_deadline);
    CHECK_INT_EQ((Int)dl.when, (Int)INT64_MAX);
    CHECK(!dl.done_at_once);
    CHECK(BURROW_OK(dl.err));
}

/* ---------------------------------------------------- with a deadline and a cause
 *
 * A cause carried by a deadline cannot live in a closure the way Go's does,
 * so it sits on the node and the timer reads it when it fires. That is worth
 * testing from all three directions: the timer wins, the cancel wins, and the
 * deadline was already behind us when the context was made. */

static void deadline_cause_body(void *env) {
    (void)env;

    ContextCancelFunc cancel;
    Context c = context_with_timeout_cause(rt_alloc, context_background(), DL_SOON,
                                           err_too_slow, &cancel);
    if (BURROW_CONTEXT_IS_NIL(c))
        return;

    dl.made = true;
    dl.err = context_cause(c);
    dl.done_in_the_end = wait_done(c);
    dl.err_after = context_err(c);
    dl.child_err = context_cause(c);

    BURROW_CALLF0(cancel);
    context_release(c);
}

TEST(a_deadline_that_fires_says_what_it_was_waiting_for) {
    dl_reset();
    rt_alloc = heap_allocator();

    runtime_main(BURROW_FN(Func, deadline_cause_body, NULL));

    CHECK(dl.made);
    CHECK(BURROW_OK(dl.err)); /* nothing has happened yet */
    CHECK(dl.done_in_the_end);

    /* The error is the usual one, so a caller that only cares whether the
     * clock ran out does not have to change. The reason is what the caller
     * handed over when it set the timeout. */
    CHECK(is(dl.err_after, context_deadline_exceeded));
    CHECK(is(dl.child_err, err_too_slow));
}

static void cancel_beats_cause_body(void *env) {
    (void)env;

    ContextCancelFunc cancel;
    Context c = context_with_deadline_cause(rt_alloc, context_background(),
                                            burrow_nanotime() + DL_NEVER, err_too_slow,
                                            &cancel);
    if (BURROW_CONTEXT_IS_NIL(c))
        return;

    dl.made = true;

    /* An hour away, so this is the stop path and the deadline's reason should
     * never be reached. */
    BURROW_CALLF0(cancel);
    dl.err_after = context_err(c);
    dl.child_err = context_cause(c);

    context_release(c);
}

TEST(a_cancel_before_the_deadline_leaves_the_deadline_cause_alone) {
    dl_reset();
    rt_alloc = heap_allocator();

    runtime_main(BURROW_FN(Func, cancel_beats_cause_body, NULL));

    CHECK(dl.made);
    CHECK(is(dl.err_after, context_canceled));
    CHECK(is(dl.child_err, context_canceled));
}

static void past_cause_body(void *env) {
    (void)env;

    Context c = context_with_timeout_cause(rt_alloc, context_background(), -TIME_SECOND,
                                           err_gave_up, NULL);
    if (BURROW_CONTEXT_IS_NIL(c))
        return;

    dl.made = true;
    dl.done_at_once = is_done(c);
    dl.err = context_err(c);
    dl.child_err = context_cause(c);

    context_release(c);
}

TEST(a_deadline_already_gone_by_still_carries_its_reason) {
    dl_reset();
    rt_alloc = heap_allocator();

    runtime_main(BURROW_FN(Func, past_cause_body, NULL));

    CHECK(dl.made);
    CHECK(dl.done_at_once);
    CHECK(is(dl.err, context_deadline_exceeded));

    /* The early path never arms a timer, so it is a separate piece of code
     * from the one above and it has to remember the reason too. */
    CHECK(is(dl.child_err, err_gave_up));
}

/* ------------------------------------------------------- AfterFunc, running it
 *
 * The other half. A cancellation that reaches one of these starts a goroutine,
 * so everything below needs a scheduler, and what the function found out it
 * says through an atomic like every other child goroutine in this file.
 *
 * The waiting is polling with a limit, for the reason written over the deadline
 * tests: a function that never runs should fail one test rather than hang the
 * run. */

static uint32_t af_ran;
static bool af_stopped;
static bool af_made;

static void af_reset(void) {
    burrow__atomic_store_u32(&af_ran, 0);
    af_stopped = false;
    af_made = false;
}

static void af_run(void *env) {
    (void)env;

    (void)burrow__atomic_add_u32(&af_ran, 1);
}

static bool wait_ran(void) {
    int64_t give_up = burrow_nanotime() + DL_LIMIT;

    while (burrow_nanotime() < give_up) {
        if (burrow__atomic_load_acquire_u32(&af_ran) != 0)
            return true;
        time_sleep(TIME_MILLISECOND);
    }
    return false;
}

static void af_cancel_body(void *env) {
    (void)env;

    ContextCancelFunc cancel;
    StopFunc stop;

    Context req = context_with_cancel(rt_alloc, context_background(), &cancel);
    Context reg =
        context_after_func(rt_alloc, req, BURROW_FN(Func, af_run, NULL), &stop);
    if (BURROW_CONTEXT_IS_NIL(reg))
        return;

    af_made = true;
    BURROW_CALLF0(cancel);

    af_stopped = wait_ran();

    /* Too late, and saying so is the whole of what a false answer means. */
    af_stopped = af_stopped && !BURROW_CALLF0(stop);

    context_release(reg);
    context_release(req);
}

TEST(a_cancel_runs_the_function_on_a_goroutine_of_its_own) {
    af_reset();
    rt_alloc = heap_allocator();

    runtime_main(BURROW_FN(Func, af_cancel_body, NULL));

    CHECK(af_made);
    CHECK(af_stopped);
    CHECK_INT_EQ((Int)burrow__atomic_load_u32(&af_ran), 1);
}

static void af_already_body(void *env) {
    (void)env;

    ContextCancelFunc cancel;
    StopFunc stop;

    Context req = context_with_cancel(rt_alloc, context_background(), &cancel);
    BURROW_CALLF0(cancel);

    /* Registered against something that is already over. Go starts the function
     * straight away rather than never, which is the only reading of "after this
     * is cancelled" that is any use. */
    Context reg =
        context_after_func(rt_alloc, req, BURROW_FN(Func, af_run, NULL), &stop);
    if (BURROW_CONTEXT_IS_NIL(reg))
        return;

    af_made = true;
    af_stopped = wait_ran();

    context_release(reg);
    context_release(req);
}

TEST(a_context_that_is_already_over_starts_the_function_at_once) {
    af_reset();
    rt_alloc = heap_allocator();

    runtime_main(BURROW_FN(Func, af_already_body, NULL));

    CHECK(af_made);
    CHECK(af_stopped);
    CHECK_INT_EQ((Int)burrow__atomic_load_u32(&af_ran), 1);
}

static void af_deadline_body(void *env) {
    (void)env;

    ContextCancelFunc cancel;
    StopFunc stop;

    Context req =
        context_with_timeout(rt_alloc, context_background(), DL_SOON, &cancel);
    if (BURROW_CONTEXT_IS_NIL(req))
        return;

    Context reg =
        context_after_func(rt_alloc, req, BURROW_FN(Func, af_run, NULL), &stop);
    if (BURROW_CONTEXT_IS_NIL(reg))
        return;

    af_made = true;
    af_stopped = wait_ran();

    BURROW_CALLF0(cancel);
    context_release(reg);
    context_release(req);
}

TEST(a_deadline_running_out_runs_the_function_too) {
    af_reset();
    rt_alloc = heap_allocator();

    runtime_main(BURROW_FN(Func, af_deadline_body, NULL));

    CHECK(af_made);
    CHECK(af_stopped);
    CHECK_INT_EQ((Int)burrow__atomic_load_u32(&af_ran), 1);
}

static void af_once_body(void *env) {
    (void)env;

    ContextCancelFunc cancel;
    StopFunc stop;

    Context req = context_with_cancel(rt_alloc, context_background(), &cancel);
    Context reg =
        context_after_func(rt_alloc, req, BURROW_FN(Func, af_run, NULL), &stop);
    if (BURROW_CONTEXT_IS_NIL(reg))
        return;

    af_made = true;

    /* Cancelled, freed and cancelled again. None of the three after the first
     * gets past the once, so the count below is the assertion. */
    BURROW_CALLF0(cancel);
    af_stopped = wait_ran();

    context_release(reg);
    BURROW_CALLF0(cancel);
    context_release(req);

    time_sleep(20 * TIME_MILLISECOND);
}

TEST(the_function_runs_once_however_many_things_cancel) {
    af_reset();
    rt_alloc = heap_allocator();

    runtime_main(BURROW_FN(Func, af_once_body, NULL));

    CHECK(af_made);
    CHECK(af_stopped);
    CHECK_INT_EQ((Int)burrow__atomic_load_u32(&af_ran), 1);
}

static Track af_track;

static void af_memory_body(void *env) {
    (void)env;

    ContextCancelFunc cancel;
    StopFunc stop;

    Context req = context_with_cancel(rt_alloc, context_background(), &cancel);
    Context reg =
        context_after_func(rt_alloc, req, BURROW_FN(Func, af_run, NULL), &stop);
    if (BURROW_CONTEXT_IS_NIL(reg))
        return;

    af_made = true;

    BURROW_CALLF0(cancel);
    af_stopped = wait_ran();

    /* Freed after the function has been started, which is the case that has a
     * goroutine in flight while the node goes. Nothing on that goroutine reads
     * the node, because what it runs is the caller's function with the caller's
     * environment, and this is where that is checked. */
    context_release(reg);
    context_release(req);

    time_sleep(20 * TIME_MILLISECOND);
}

TEST(a_registration_whose_function_has_run_is_still_given_back) {
    af_reset();
    track_init(&af_track, heap_allocator());
    track_set_quarantine(&af_track, 0);
    rt_alloc = track_allocator(&af_track);

    runtime_main(BURROW_FN(Func, af_memory_body, NULL));

    CHECK(af_made);
    CHECK(af_stopped);
    CHECK_INT_EQ((Int)track_live(&af_track), 0);
    CHECK_INT_EQ((Int)track_check(&af_track), 0);
    track_free(&af_track);
}

int main(void) {
    RUN(the_root_is_never_cancelled_and_carries_nothing);
    RUN(background_and_todo_are_not_the_same_context);

    RUN(a_cancel_closes_the_done_channel_and_sets_the_error);
    RUN(cancelling_twice_changes_nothing);
    RUN(cancelling_a_parent_cancels_every_child);
    RUN(cancelling_a_child_leaves_the_parent_alone);
    RUN(a_child_of_something_already_cancelled_starts_cancelled);

    RUN(a_cause_says_why_where_the_error_only_says_that);
    RUN(a_cancel_with_no_reason_leaves_the_cause_equal_to_the_error);
    RUN(the_first_reason_is_the_one_that_sticks);
    RUN(a_live_context_has_no_cause);
    RUN(a_reason_given_at_the_top_is_the_answer_at_the_bottom);
    RUN(a_cause_given_below_stays_below);
    RUN(a_child_of_something_cancelled_with_a_reason_starts_with_it);
    RUN(a_plain_cancel_context_still_has_a_cause);
    RUN(a_context_that_cannot_be_cancelled_has_no_cause);

    RUN(a_stopped_after_func_never_runs);
    RUN(only_one_stop_ever_answers_true);
    RUN(freeing_a_registration_is_not_a_reason_to_run_it);
    RUN(a_registration_answers_the_four_questions_like_anything_else);
    RUN(a_registration_is_given_back_to_the_allocator);

    RUN(work_that_outlives_its_request_keeps_the_values);
    RUN(nothing_under_a_without_cancel_is_reached_by_the_parent);
    RUN(a_without_cancel_has_no_deadline_and_no_cause);
    RUN(a_detached_context_is_given_back_to_the_allocator);

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
    RUN(a_deadline_off_a_goroutine_stops_the_program);
    RUN(freeing_a_context_this_package_did_not_make_stops_the_program);

    RUN(a_goroutine_parked_on_done_wakes_up_when_somebody_cancels);
    RUN(a_parent_from_outside_the_package_still_cancels_what_is_under_it);
    RUN(a_wrapper_that_replaces_done_is_not_mistaken_for_what_it_wraps);
    RUN(a_context_with_a_watcher_can_be_freed_before_the_watcher_wakes);

    RUN(a_deadline_an_hour_away_leaves_the_cancel_to_win);
    RUN(a_deadline_that_has_gone_by_comes_back_already_cancelled);
    RUN(a_timeout_fires_and_says_the_deadline_went_by);
    RUN(a_parent_that_gives_up_sooner_keeps_the_deadline);
    RUN(a_child_with_a_sooner_deadline_fires_on_its_own);
    RUN(a_deadline_reaches_everything_underneath_it);
    RUN(everything_is_given_back_when_the_deadline_wins);
    RUN(everything_is_given_back_when_the_cancel_wins);
    RUN(a_context_freed_before_its_deadline_takes_the_timer_with_it);
    RUN(a_goroutine_parked_on_done_wakes_up_when_the_deadline_goes_by);
    RUN(a_timeout_too_big_to_add_lands_at_the_end_of_the_clock);

    RUN(a_deadline_that_fires_says_what_it_was_waiting_for);
    RUN(a_cancel_before_the_deadline_leaves_the_deadline_cause_alone);
    RUN(a_deadline_already_gone_by_still_carries_its_reason);

    RUN(a_cancel_runs_the_function_on_a_goroutine_of_its_own);
    RUN(a_context_that_is_already_over_starts_the_function_at_once);
    RUN(a_deadline_running_out_runs_the_function_too);
    RUN(the_function_runs_once_however_many_things_cancel);
    RUN(a_registration_whose_function_has_run_is_still_given_back);

    return harness_report("context");
}
