/* Cancellation, deadlines and request scoped values.
 *
 * Go's context package is three hundred lines and almost all of the difficulty
 * is in one function. Making a context is trivial, cancelling one is trivial,
 * and attaching a new one to a parent so that the parent's cancellation reaches
 * it is where everything interesting lives. That is propagate_cancel below, and
 * it is worth reading before anything else here.
 *
 * The shape of the problem: a new cancellable context has to be told when its
 * parent is cancelled. The cheap way is for the parent to keep a list of
 * children and walk it, and that works whenever the parent is one of ours. It
 * does not work when the parent came from somebody else's code, because there
 * is no list to join. Go's answer, and this file's, is to find the nearest
 * ancestor that is one of ours by asking for a value under a key no other
 * package can build, and to fall back to a goroutine watching two channels when
 * even that fails. Everything else in the file is bookkeeping around those two
 * cases.
 *
 * What is different from Go, all of it because there is no collector:
 *
 *   - Every node remembers the allocator it came from and is freed explicitly.
 *   - The children are an intrusive doubly linked list rather than a map, so
 *     attaching a child allocates nothing. Go pays a map insert per WithCancel
 *     and cannot avoid it, since it has nowhere to put the links.
 *   - The done channel is made when the node is, because context_done has no
 *     allocator and no way to report a failure.
 *   - The node is reference counted, and the count is only ever above one while
 *     a watcher goroutine or an armed deadline timer exists. That is what makes
 *     it safe to free a context whose watcher has not woken up yet, or whose
 *     deadline is a nanosecond away: whoever puts the node down last frees it,
 *     and the loser of that race has already stopped touching it.
 *
 * Copyright 2026 The burrow Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style licence that can be found
 * in the LICENSE file. */

#include "burrow/context.h"

#include "burrow/atomic.h"
#include "burrow/chan.h"
#include "burrow/core.h"
#include "burrow/error.h"
#include "burrow/iface.h"
#include "burrow/mem.h"
#include "burrow/panic.h"
#include "burrow/proc.h"
#include "burrow/runtime.h"
#include "burrow/sync.h"
#include "burrow/time.h"
#include "burrow/timer.h"
#include "burrow/type.h"

#include <stdbool.h>
#include <stdint.h>

/* ---------------------------------------------------------------- sentinels */

BURROW_SENTINEL_ERROR(context_canceled, "context canceled");
BURROW_SENTINEL_ERROR(context_deadline_exceeded, "context deadline exceeded");

/* -------------------------------------------------------------- descriptors */

/* Interface equality, which for a Context is the same thing it is for an Error:
 * the same vtable and the same receiver. Two calls to context_background are
 * equal because both are a vtable and a NULL, which is what == on two
 * context.Background() values answers in Go. */
static bool context_ops_equal(const void *a, const void *b) {
    const Context *x = (const Context *)a;
    const Context *y = (const Context *)b;
    return x->vt == y->vt && x->data == y->data;
}

static uint64_t context_ops_hash(const void *p, uint64_t seed) {
    const Context *c = (const Context *)p;
    uint64_t h = seed ^ 0x9e3779b97f4a7c15U;
    h = (h ^ (uint64_t)(Uintptr)c->vt) * 0x100000001b3U;
    h = (h ^ (uint64_t)(Uintptr)c->data) * 0x100000001b3U;
    return h;
}

static const TypeOps context_ops = {
    context_ops_equal,
    context_ops_hash,
    NULL,
    NULL,
};

static const Type context_type = {
    {(const Byte *)"Context", 7},
    {(const Byte *)"context", 7},
    KIND_INTERFACE,
    (uint32_t)sizeof(Context),
    (uint16_t)_Alignof(Context),
    0,
    0,
    NULL,
    NULL,
    NULL,
    NULL,
    0,
    0x63747874U, /* "ctxt", distinct from every builtin's */
    &context_ops,
};

const Type *const TYPE_CONTEXT = &context_type;

/* ---------------------------------------------------------- the cancel key
 *
 * How a new cancellable context finds the nearest cancellable one above it.
 *
 * The obvious way is to look at the parent and see whether its vtable is ours.
 * That is wrong, and Go gets it right by not doing it either. A context in
 * between may be a WithValue, which is not cancellable and has to be looked
 * through, and it may be somebody else's wrapper that forwards Value to what it
 * wraps, which also has to be looked through and which no amount of vtable
 * comparison will see past. So the question is asked as a value lookup, because
 * a value lookup is the one operation every context in the chain forwards.
 *
 * The key is a value of a type declared in this file and nowhere else. A key
 * comparison compares type descriptors before it compares anything else, and
 * this descriptor is static, so no code outside this file can build a key that
 * matches. That is what Go's unexported cancelCtxKey buys, in the one way C has
 * of buying it.
 *
 * The key type is Go's struct{}: there is exactly one value of it and it has no
 * bytes. So the Any's data is NULL and never read, which is why the two
 * operations below ignore their arguments. A zero size descriptor is what the
 * BURROW_STRUCT macro will produce for struct{} when it exists, and this is the
 * first place that needed one. */

static bool cancel_key_equal(const void *a, const void *b) {
    (void)a;
    (void)b;
    return true;
}

static uint64_t cancel_key_hash(const void *p, uint64_t seed) {
    (void)p;
    return seed ^ 0x636b6579U;
}

static const TypeOps cancel_key_ops = {
    cancel_key_equal,
    cancel_key_hash,
    NULL,
    NULL,
};

static const Type cancel_key_type = {
    {(const Byte *)"cancelCtxKey", 12},
    {(const Byte *)"context", 7},
    KIND_STRUCT,
    0,
    1,
    0,
    0,
    NULL,
    NULL,
    NULL,
    NULL,
    0,
    0x636b6579U, /* "ckey" */
    &cancel_key_ops,
};

static const Any cancel_key = {&cancel_key_type, NULL};

/* And the descriptor for what the lookup hands back, which is a pointer to one
 * of the nodes below. Also private, so that the only code able to unwrap the
 * answer is the code that produced it. */
static const Type cancel_ctx_type = {
    {(const Byte *)"*cancelCtx", 10},
    {(const Byte *)"context", 7},
    KIND_POINTER,
    (uint32_t)sizeof(void *),
    (uint16_t)_Alignof(void *),
    0,
    0,
    NULL,
    NULL,
    NULL,
    NULL,
    0,
    0x63637478U, /* "cctx" */
    NULL,
};

/* --------------------------------------------------------------- the nodes */

typedef struct CancelCtx CancelCtx;

struct CancelCtx {
    /* Go's embedded Context field, which is the parent. Written once before
     * anything else can see the node, so it is read without the lock. */
    Context parent;

    Alloc *a;

    /* This node's own address, because value hands a pointer to this node back
     * inside an Any and an Any has to point at something. */
    CancelCtx *self;

    /* Closed exactly once, by whichever cancel gets there first. Never
     * replaced, so it is read without the lock, which is the whole reason it is
     * made eagerly. */
    Chan *done;

    /* One for whoever made the context, and one more for the watcher goroutine
     * when there is one and for the deadline timer while it is armed. Whoever
     * drops the last reference frees. */
    uint32_t refs;

    /* Whether this node is the first member of a TimerCtx. Sitting in the
     * padding after refs, so it costs a plain WithCancel nothing.
     *
     * Go has no such field: a timerCtx is a different type and every call that
     * cares goes through an interface. Here the two share a vtable slot for
     * three of the four methods and share every line of the cancelling code, so
     * the code that needs to know asks. */
    bool timed;

    /* Whether this node is the first member of an AfterFuncCtx. The same
     * argument as timed, and in the same padding, so neither of them costs a
     * plain WithCancel anything. Unlike timed this one changes no method, only
     * what cancelling does at the end and what freeing has to size. */
    bool after;

    SyncMutex mu;

    /* Set once, under mu, at the same moment done is closed. */
    Error err;

    /* Why, as opposed to err, which says only that. Set under mu at the same
     * moment as err and never changed again, so the first reason is the one
     * that sticks. Equal to err when nobody gave a reason, which is what Go
     * does and is what makes context_cause safe to read instead of
     * context_err rather than as well as it. */
    Error cause;

    /* Head of the child list, under mu. */
    CancelCtx *children;

    /* The nearest cancellable ancestor, or NULL when there is none and when a
     * watcher goroutine is doing the propagating instead. Written once during
     * construction.
     *
     * Go looks this up again when it needs it, by walking the value chain a
     * second time. The answer cannot change, so remembering it is the same
     * answer for less work. */
    CancelCtx *plink;

    /* This node's place in plink's list. These two belong to plink's lock and
     * not to this node's, which is the same rule as the map entry they replace:
     * in Go the entry is in the parent's map and the parent's mutex covers it. */
    CancelCtx *prev;
    CancelCtx *next;
};

/* A cancel node with a deadline attached, which is Go's timerCtx. Go embeds a
 * cancelCtx in it and so does this, and because the embedded one is the first
 * member the two pointers are the same address. That is what lets cancel_node,
 * remove_child, release and three of the four methods work on either kind
 * without knowing which they have, the same way Go's promoted methods do. */
typedef struct TimerCtx {
    CancelCtx c;

    /* Set once, before the node is visible to anybody, and read without the
     * lock. Go keeps the deadline in the node the same way. */
    int64_t when;

    /* Under c.mu. NULL when the deadline had already gone by, and NULL again
     * once whoever cancelled has disarmed it. */
    TimeTimer *timer;

    /* What to blame if the deadline is what stops this context. Set once
     * before the node is visible and read without the lock, because Go keeps
     * it in a closure the timer holds and this has no closures. Nothing reads
     * it on a path where the deadline did not win. */
    Error deadline_cause;
} TimerCtx;

/* Go's afterFuncCtx, a cancel node with a function to run instead of anybody
 * waiting on the channel. Embedded the same way TimerCtx embeds one, so the two
 * pointers are the same address and everything that works on a CancelCtx works
 * on this.
 *
 * The once is what makes exactly one of the two outcomes happen. Either the
 * stop function takes it and f never runs, or the cancellation takes it and f
 * is started, and whichever arrives second finds it taken and does nothing. Go
 * uses a sync.Once here for the same reason and in the same two places. */
typedef struct AfterFuncCtx {
    CancelCtx c;

    SyncOnce once;

    /* Set once, before the node is visible to anybody. Read on the cancelling
     * goroutine when it hands the function to go, and not after that: what runs
     * is the caller's function with the caller's environment, so the goroutine
     * never touches this node and the node can be freed underneath it. */
    Func f;
} AfterFuncCtx;

typedef struct ValueCtx {
    Context parent;
    Alloc *a;
    Any key;
    Any val;
} ValueCtx;

/* Go's withoutCancelCtx, which is a parent and nothing else. No lock, no
 * channel and no place in any child list, because it is never cancelled. */
typedef struct WithoutCancelCtx {
    Context parent;
    Alloc *a;
} WithoutCancelCtx;

/* ------------------------------------------------------------- the vtables */

static bool empty_deadline(void *self, int64_t *when);
static Chan *empty_done(void *self);
static Error empty_err(void *self);
static Any empty_value(void *self, Any key);

static bool cancel_deadline(void *self, int64_t *when);
static Chan *cancel_done(void *self);
static Error cancel_err(void *self);
static Any cancel_value(void *self, Any key);

static bool timer_deadline(void *self, int64_t *when);

static bool without_cancel_deadline(void *self, int64_t *when);
static Chan *without_cancel_done(void *self);
static Error without_cancel_err(void *self);
static Any without_cancel_value(void *self, Any key);

static bool value_deadline(void *self, int64_t *when);
static Chan *value_done(void *self);
static Error value_err(void *self);
static Any value_value(void *self, Any key);

/* Two vtables with the same four functions in them, because Background and TODO
 * behave identically and are still not the same thing. They are told apart by
 * address, which C guarantees to differ for two distinct objects, and that is
 * the same trick Go uses: its background and todo are two values of two named
 * types whose only difference is what String prints. Nothing in this file
 * branches on which one it got, and a linter that wants to find the TODOs can.
 *
 * self_type is NULL on both. The concrete type is Go's emptyCtx, which is
 * unexported and which nobody has any business extracting, and an unexported
 * type is exactly the case the header says NULL is for. */
static const ContextVT background_vt = {
    NULL, empty_deadline, empty_done, empty_err, empty_value,
};

static const ContextVT todo_vt = {
    NULL, empty_deadline, empty_done, empty_err, empty_value,
};

static const ContextVT cancel_vt = {
    NULL, cancel_deadline, cancel_done, cancel_err, cancel_value,
};

/* Three slots of cancel_vt and one of its own, which is Go's timerCtx: it
 * overrides Deadline and promotes the rest from the cancelCtx inside it. */
static const ContextVT timer_vt = {
    NULL, timer_deadline, cancel_done, cancel_err, cancel_value,
};

/* Four slots of its own rather than three shared with background_vt, because
 * the value slot has a parent to walk into and because the vtable pointer is
 * what context_free and the value loop tell the node types apart by. */
static const ContextVT without_cancel_vt = {
    NULL,
    without_cancel_deadline,
    without_cancel_done,
    without_cancel_err,
    without_cancel_value,
};

static const ContextVT value_vt = {
    NULL, value_deadline, value_done, value_err, value_value,
};

/* --------------------------------------------------------------- the methods
 *
 * Four functions that do the indirect call, and a nil check in front of each.
 *
 * A nil Context is a bug in the caller, and Go's version of that bug is a nil
 * interface value whose method call faults. The message here is Go's message
 * for that fault rather than something friendlier, because it is what somebody
 * puts into a search engine when they hit it. */

static void nonnil(Context c) {
    if (c.vt == NULL)
        runtime_panic(BURROW_S(
            "runtime error: invalid memory address or nil pointer dereference"));
}

/* The check is its own statement rather than an argument, because C does not say
 * which of the two happens first and every compiler tried loads the vtable
 * slot before making the call. On a nil context that is a load through a null
 * pointer, which is undefined whether or not the check would have panicked
 * first, and the sanitizer says so. */

bool context_deadline(Context c, int64_t *when) {
    nonnil(c);
    return c.vt->deadline(c.data, when);
}

Chan *context_done(Context c) {
    nonnil(c);
    return c.vt->done(c.data);
}

Error context_err(Context c) {
    nonnil(c);
    return c.vt->err(c.data);
}

/* The one method that is not a plain forward, for the reason Go's free standing
 * value function is not one either.
 *
 * Written as a loop over the nodes this package knows, so that a lookup down a
 * chain of a hundred WithValues is a hundred iterations rather than a hundred
 * stack frames. A context from outside this package ends the loop by being
 * asked properly, and whatever it forwards to comes back through here. */
Any context_value(Context c, Any key) {
    Any none = {NULL, NULL};

    nonnil(c);

    for (;;) {
        if (c.vt == &value_vt) {
            ValueCtx *v = (ValueCtx *)c.data;
            if (any_equal(key, v->key))
                return v->val;
            c = v->parent;
            continue;
        }

        if (c.vt == &cancel_vt || c.vt == &timer_vt) {
            CancelCtx *cc = (CancelCtx *)c.data;
            if (any_equal(key, cancel_key))
                return BURROW_ANY(&cancel_ctx_type, &cc->self);
            c = cc->parent;
            continue;
        }

        if (c.vt == &without_cancel_vt) {
            WithoutCancelCtx *w = (WithoutCancelCtx *)c.data;
            if (any_equal(key, cancel_key))
                return none;
            c = w->parent;
            continue;
        }

        if (c.vt == &background_vt || c.vt == &todo_vt)
            return none;

        return c.vt->value(c.data, key);
    }
}

/* --------------------------------------------------------- Background and TODO */

static bool empty_deadline(void *self, int64_t *when) {
    (void)self;
    (void)when;
    return false;
}

static Chan *empty_done(void *self) {
    (void)self;
    return NULL;
}

static Error empty_err(void *self) {
    (void)self;
    return BURROW_NO_ERROR;
}

static Any empty_value(void *self, Any key) {
    Any none = {NULL, NULL};
    (void)self;
    (void)key;
    return none;
}

Context context_background(void) {
    Context c = {&background_vt, NULL};
    return c;
}

Context context_todo(void) {
    Context c = {&todo_vt, NULL};
    return c;
}

/* ---------------------------------------------------------------- WithValue */

static bool value_deadline(void *self, int64_t *when) {
    return context_deadline(((ValueCtx *)self)->parent, when);
}

static Chan *value_done(void *self) {
    return context_done(((ValueCtx *)self)->parent);
}

static Error value_err(void *self) {
    return context_err(((ValueCtx *)self)->parent);
}

static Any value_value(void *self, Any key) {
    ValueCtx *v = (ValueCtx *)self;

    if (any_equal(key, v->key))
        return v->val;
    return context_value(v->parent, key);
}

Context context_with_value(Alloc *a, Context parent, Any key, Any val) {
    Context none = {NULL, NULL};

    if (BURROW_CONTEXT_IS_NIL(parent))
        panic_str(BURROW_S("cannot create context from nil parent"));
    if (BURROW_ANY_IS_NIL(key))
        panic_str(BURROW_S("nil key"));
    if (!type_is_comparable(key.t))
        panic_str(BURROW_S("key is not comparable"));

    ValueCtx *v = BURROW_NEW(a, ValueCtx);
    if (v == NULL)
        return none;

    v->parent = parent;
    v->a = a;
    v->key = key;
    v->val = val;

    Context c = {&value_vt, v};
    return c;
}

/* --------------------------------------------------------------- WithCancel */

static bool cancel_deadline(void *self, int64_t *when) {
    return context_deadline(((CancelCtx *)self)->parent, when);
}

static Chan *cancel_done(void *self) {
    return ((CancelCtx *)self)->done;
}

static Error cancel_err(void *self) {
    CancelCtx *c = (CancelCtx *)self;

    sync_mutex_lock(&c->mu);
    Error e = c->err;
    sync_mutex_unlock(&c->mu);
    return e;
}

static Any cancel_value(void *self, Any key) {
    CancelCtx *c = (CancelCtx *)self;

    if (any_equal(key, cancel_key))
        return BURROW_ANY(&cancel_ctx_type, &c->self);
    return context_value(c->parent, key);
}

/* Takes this node out of its parent's list.
 *
 * Runs with no lock held and takes the parent's, which is the one order this
 * file uses. The caller has already let go of the child's lock, exactly as
 * Go's cancel does before it calls removeChild, and that is what keeps a parent
 * cancelling downwards and a child detaching upwards from meeting in the
 * middle.
 *
 * Doing nothing is the common case rather than an error: a node whose parent
 * cancelled it has already been taken out, in one go, by the loop below. */
static void remove_child(CancelCtx *c) {
    CancelCtx *p = c->plink;

    if (p == NULL)
        return;

    sync_mutex_lock(&p->mu);
    if (c->prev != NULL)
        c->prev->next = c->next;
    else if (p->children == c)
        p->children = c->next;
    if (c->next != NULL)
        c->next->prev = c->prev;
    c->prev = NULL;
    c->next = NULL;
    sync_mutex_unlock(&p->mu);
}

/* Declared up here because cancelling can be what puts the deadline timer's
 * reference down, and the two are easier to read in this order. */
static void context_release(CancelCtx *c);

/* Declared up here for the same reason: cancelling an AfterFunc node is what
 * starts the function, and the section that builds one is a long way below. */
static void after_func_start(AfterFuncCtx *af);

/* Cancels this node and everything under it, once.
 *
 * The child's lock is taken while this node's is held, which looks like the
 * start of a deadlock and is not: every lock in this file is taken parent
 * first, and the only upward move is remove_child, which holds nothing when it
 * starts. Go's comment at the same spot says the same thing.
 *
 * remove_from_parent is false for everything reached through the loop, because
 * the loop empties the list wholesale and a child unlinking itself from a list
 * that is being thrown away is work at best and a walk into the parent's lock
 * from underneath it at worst. */
static void cancel_node(CancelCtx *c, bool remove_from_parent, Error err, Error cause) {
    if (BURROW_OK(err))
        panic_str(BURROW_S("context: internal error: missing cancel error"));

    /* No reason given means the reason is the error, so that context_cause
     * always has an answer for a cancelled context and a caller never has to
     * ask both questions. Go does this in the same line of its cancel. */
    if (BURROW_OK(cause))
        cause = err;

    sync_mutex_lock(&c->mu);
    if (BURROW_FAILED(c->err)) {
        sync_mutex_unlock(&c->mu);
        return;
    }

    /* Exactly one call gets past the check above, so exactly one disarms the
     * deadline timer, and a stop that says true means the callback will never
     * run and its reference on the node is this call's to put down. The put down
     * is at the bottom, after the last read of c, because it can be the last
     * reference. The memory behind the timer itself goes back in release, which
     * is the only place that knows nothing else can be looking at it. */
    bool timer_stopped = false;
    if (c->timed) {
        TimerCtx *t = (TimerCtx *)c;
        if (t->timer != NULL)
            timer_stopped = time_timer_stop(t->timer);
    }

    c->err = err;
    c->cause = cause;
    chan_close(c->done);

    CancelCtx *child = c->children;
    while (child != NULL) {
        CancelCtx *after = child->next;

        child->prev = NULL;
        child->next = NULL;
        cancel_node(child, false, err, cause);
        child = after;
    }
    c->children = NULL;
    sync_mutex_unlock(&c->mu);

    if (remove_from_parent)
        remove_child(c);

    /* Outside the lock, because this starts a goroutine and because the
     * function it starts is the caller's. Go puts the same call at the same
     * point, in the override afterFuncCtx has for cancel. Nothing gets here
     * twice: the check at the top lets exactly one cancel past. */
    if (c->after)
        after_func_start((AfterFuncCtx *)c);

    if (timer_stopped)
        context_release(c);
}

/* Puts one reference down, and frees on the last one.
 *
 * The count is one for the whole life of almost every context, so this is an
 * atomic subtract and a branch. It is two only while a watcher goroutine is
 * alive, and it exists for that case alone: the watcher may be on the point of
 * waking when the program frees the context, and without the count there is no
 * moment at which freeing is safe. */
static void context_release(CancelCtx *c) {
    if (burrow__atomic_add_u32(&c->refs, (uint32_t)-1) != 1)
        return;

    Alloc *a = c->a;
    Chan *done = c->done;
    size_t size = sizeof(CancelCtx);
    size_t align = _Alignof(CancelCtx);

    if (c->timed) {
        /* The timer is freed rather than stopped here because nothing can be
         * holding it: an armed timer is a reference of its own, so this being
         * the last one means the timer has either fired or been disarmed.
         * time_timer_free takes it out of whatever P's heap it is still sitting
         * in, which a plain mem_free would not, and NULL is fine. */
        time_timer_free(((TimerCtx *)c)->timer);
        size = sizeof(TimerCtx);
        align = _Alignof(TimerCtx);
    } else if (c->after) {
        size = sizeof(AfterFuncCtx);
        align = _Alignof(AfterFuncCtx);
    }

    chan_free(done);
    mem_free(a, c, size, align);
}

static void cancel_func(void *env) {
    cancel_node((CancelCtx *)env, true, context_canceled, BURROW_NO_ERROR);
}

static void cancel_cause_func(void *env, Error cause) {
    cancel_node((CancelCtx *)env, true, context_canceled, cause);
}

/* What *cancel is set to when the context could not be made. Calling a cancel
 * for a context that does not exist has nothing to undo, and a caller who
 * deferred the cancel before looking at the context is then still right. */
static void cancel_nothing(void *env) {
    (void)env;
}

static void cancel_cause_nothing(void *env, Error cause) {
    (void)env;
    (void)cause;
}

/* The nearest cancellable ancestor, or NULL if there is not one to be had.
 *
 * Go's parentCancelCtx, including the last check, which is the one that is easy
 * to leave out and hard to debug. A context that wraps one of ours and returns
 * a Done channel of its own is a different thing to wait on, and joining the
 * inner node's child list would mean the wrapper's cancellation never arrived.
 * Comparing the channels catches it, because a wrapper that did not override
 * Done hands back the same channel it wraps. */
static CancelCtx *parent_cancel_ctx(Context parent) {
    Chan *done = context_done(parent);

    if (done == NULL)
        return NULL;

    Any v = context_value(parent, cancel_key);
    CancelCtx **p = (CancelCtx **)any_assert(v, &cancel_ctx_type);
    if (p == NULL || *p == NULL)
        return NULL;
    if ((*p)->done != done)
        return NULL;

    return *p;
}

/* The fallback, for a parent this package did not make.
 *
 * One goroutine, waiting on the parent's done channel and on its own. The
 * second case is not optional: without it the goroutine outlives the context it
 * was watching, and a server that makes one of these per request leaks a
 * goroutine per request. Go's version is the same two cases for the same
 * reason.
 *
 * The reference this goroutine holds is what lets the program free the context
 * without waiting for the goroutine to notice. */
static void watch(void *env) {
    CancelCtx *c = (CancelCtx *)env;

    SelectCase cases[2];
    cases[0] = BURROW_RECV(context_done(c->parent), NULL);
    cases[1] = BURROW_RECV(c->done, NULL);

    if (chan_select(cases, 2) == 0)
        cancel_node(c, false, context_err(c->parent), context_cause(c->parent));

    context_release(c);
}

/* Arranges for the parent's cancellation to reach c, and answers whether it
 * could. False means a goroutine would not start, which is the one failure
 * here that is not a refused allocation.
 *
 * Four cases, in the order Go tries them: a parent that can never be cancelled
 * and needs nothing, a parent that is cancelled already, a parent with a child
 * list to join, and a parent that needs watching. */
static bool propagate_cancel(CancelCtx *c, Context parent) {
    Chan *done = context_done(parent);

    if (done == NULL)
        return true;

    /* Already cancelled. A closed and drained channel answers a receive at
     * once and says false, which is the only way this can be true for a done
     * channel, since nothing ever sends on one. */
    bool had_value = false;
    if (chan_try_recv(done, NULL, &had_value) && !had_value) {
        cancel_node(c, false, context_err(parent), context_cause(parent));
        return true;
    }

    CancelCtx *p = parent_cancel_ctx(parent);
    if (p != NULL) {
        sync_mutex_lock(&p->mu);
        if (BURROW_FAILED(p->err)) {
            /* The parent was cancelled between the check above and this lock,
             * which is the race the whole lock is here for. */
            cancel_node(c, false, p->err, p->cause);
        } else {
            c->plink = p;
            c->next = p->children;
            if (p->children != NULL)
                p->children->prev = c;
            p->children = c;
        }
        sync_mutex_unlock(&p->mu);
        return true;
    }

    /* Set before the goroutine exists, so the goroutine can never see the old
     * value and no atomic is needed for the store. */
    c->refs = 2;
    if (!go(BURROW_FN(Func, watch, c))) {
        c->refs = 1;
        return false;
    }
    return true;
}

/* The node, the channel and the attaching, which is everything the two
 * WithCancel constructors have in common. NULL means the allocator said no or a
 * watcher goroutine would not start, and nothing has been kept in either
 * case. */
static CancelCtx *new_cancel_ctx(Alloc *a, Context parent) {
    CancelCtx *c = BURROW_NEW(a, CancelCtx);
    if (c == NULL)
        return NULL;

    Chan *done = chan_make(a, TYPE_UINT8, 0);
    if (done == NULL) {
        mem_free(a, c, sizeof(CancelCtx), _Alignof(CancelCtx));
        return NULL;
    }

    c->parent = parent;
    c->a = a;
    c->self = c;
    c->done = done;
    c->refs = 1;

    if (!propagate_cancel(c, parent)) {
        chan_free(done);
        mem_free(a, c, sizeof(CancelCtx), _Alignof(CancelCtx));
        return NULL;
    }

    return c;
}

Context context_with_cancel(Alloc *a, Context parent, CancelFunc *cancel) {
    Context none = {NULL, NULL};

    if (BURROW_CONTEXT_IS_NIL(parent))
        panic_str(BURROW_S("cannot create context from nil parent"));

    if (cancel != NULL)
        *cancel = BURROW_FN(CancelFunc, cancel_nothing, NULL);

    CancelCtx *c = new_cancel_ctx(a, parent);
    if (c == NULL)
        return none;

    if (cancel != NULL)
        *cancel = BURROW_FN(CancelFunc, cancel_func, c);

    Context out = {&cancel_vt, c};
    return out;
}

/* ---------------------------------------------------- WithCancelCause, Cause
 *
 * The same node. A cause is a second error written beside the first one at the
 * moment of cancellation, and everything that cancels was already carrying it
 * down the subtree before this section existed.
 *
 * What is new is the reading. Cause looks up the nearest cancellable context
 * the way the value walk does, rather than asking the context in front of it,
 * and that is the feature: a reason given at the top is the answer five layers
 * down, where the layers in between are WithValues that cancel nothing. */

Context context_with_cancel_cause(Alloc *a, Context parent, CancelCauseFunc *cancel) {
    Context none = {NULL, NULL};

    if (BURROW_CONTEXT_IS_NIL(parent))
        panic_str(BURROW_S("cannot create context from nil parent"));

    if (cancel != NULL)
        *cancel = BURROW_FN(CancelCauseFunc, cancel_cause_nothing, NULL);

    CancelCtx *c = new_cancel_ctx(a, parent);
    if (c == NULL)
        return none;

    if (cancel != NULL)
        *cancel = BURROW_FN(CancelCauseFunc, cancel_cause_func, c);

    Context out = {&cancel_vt, c};
    return out;
}

Error context_cause(Context c) {
    nonnil(c);

    /* The same lookup parent_cancel_ctx makes, without the done channel check
     * it does. Go's Cause leaves that check out too, and it has to: the check
     * is there to stop a node joining the child list of a context it does not
     * really sit under, and reading a reason is not joining anything. */
    Any v = context_value(c, cancel_key);
    CancelCtx **p = (CancelCtx **)any_assert(v, &cancel_ctx_type);

    /* Nothing cancellable above this, so there is nowhere for a reason to have
     * been written and the honest answer is whatever this context says about
     * itself. That covers Background, a tree of nothing but WithValues, a
     * context somebody else's package wrote, and the WithoutCancel node, which
     * answers nothing to this key on purpose. */
    if (p == NULL || *p == NULL)
        return context_err(c);

    CancelCtx *cc = *p;

    sync_mutex_lock(&cc->mu);
    Error cause = cc->cause;
    sync_mutex_unlock(&cc->mu);
    return cause;
}

/* --------------------------------------------------------------- WithoutCancel
 *
 * A context that keeps the values and drops everything else, for work that has
 * to outlive the request it came from: the access log line, the metric, the
 * transaction that would otherwise be rolled back.
 *
 * It is a two word node with no lock, no channel and no place in any child
 * list, which is the whole of Go's withoutCancelCtx as well. The only part
 * worth a comment is that it answers nothing to the cancel key, which is two
 * things at once: a cancellable context built on this one finds no ancestor to
 * join, so cancellation really does stop here, and context_cause answers
 * nothing rather than reaching past it for the parent's reason. */

static bool without_cancel_deadline(void *self, int64_t *when) {
    (void)self;
    (void)when;
    return false;
}

static Chan *without_cancel_done(void *self) {
    (void)self;
    return NULL;
}

static Error without_cancel_err(void *self) {
    (void)self;
    return BURROW_NO_ERROR;
}

static Any without_cancel_value(void *self, Any key) {
    WithoutCancelCtx *w = (WithoutCancelCtx *)self;
    Any none = {NULL, NULL};

    if (any_equal(key, cancel_key))
        return none;
    return context_value(w->parent, key);
}

Context context_without_cancel(Alloc *a, Context parent) {
    Context none = {NULL, NULL};

    if (BURROW_CONTEXT_IS_NIL(parent))
        panic_str(BURROW_S("cannot create context from nil parent"));

    WithoutCancelCtx *w = BURROW_NEW(a, WithoutCancelCtx);
    if (w == NULL)
        return none;

    w->parent = parent;
    w->a = a;

    Context out = {&without_cancel_vt, w};
    return out;
}

/* ------------------------------------------------------------------- AfterFunc
 *
 * A cancel node with a function hanging off it. Nobody waits on the channel;
 * the cancellation itself is what starts the work.
 *
 * The whole of it is one Once with two callers. The stop function takes it and
 * the function never runs, or cancel_node takes it and the function is started.
 * Whichever is second finds it taken and does nothing, and that is also what
 * makes a second call to stop answer false rather than do it again.
 *
 * Go returns only the stop function and lets the collector have the node. There
 * is no collector here, so the node comes back as a Context and context_free is
 * how it is given back. Making it a Context rather than a handle of its own is
 * not a stretch: it is a cancel node, it has a parent and a done channel and an
 * error, and it answers all four questions correctly already.
 *
 * Go also has an afterFuncer escape hatch, where a parent that implements
 * AfterFunc itself gets to do the arranging. That exists for testing/synctest,
 * whose contexts have to know about every goroutine waiting on them, and it is
 * a fifth method on an interface Go can type assert for and this cannot. It
 * comes back with synctest if synctest turns out to need it. */

/* Under the once, so this and the stop function cannot both win.
 *
 * The function is handed to go by value, so once the goroutine exists nothing
 * on it reads this node again and the node may be freed underneath it. That is
 * why f is copied out before anything else here.
 *
 * A go that fails is a program with no memory left, which Go treats as fatal
 * and so does this. The alternative is running the function on the goroutine
 * that did the cancelling, and that one is holding no locks but is also not
 * expecting to block, which is a worse failure than stopping. */
static void after_func_go(void *env) {
    AfterFuncCtx *af = (AfterFuncCtx *)env;
    Func f = af->f;

    if (!go(f))
        runtime_throw(BURROW_S("context: out of memory starting an AfterFunc"));
}

static void after_func_start(AfterFuncCtx *af) {
    sync_once_do(&af->once, BURROW_FN(Func, after_func_go, af));
}

/* Under the once, to find out whether this call is the one that took it. Go
 * writes the same thing as a closure over a local. */
static void after_func_claim(void *env) {
    *(bool *)env = true;
}

static bool after_func_stop(void *env) {
    AfterFuncCtx *af = (AfterFuncCtx *)env;
    bool stopped = false;

    sync_once_do(&af->once, BURROW_FN(Func, after_func_claim, &stopped));
    if (!stopped)
        return false;

    /* The once is taken, so the cancel below cannot start the function and this
     * is only detaching the node from its parent and closing its channel. Go
     * cancels here for the same reason: a stopped registration that stayed in
     * the parent's child list would be a leak for as long as the parent lives. */
    cancel_node(&af->c, true, context_canceled, BURROW_NO_ERROR);
    return true;
}

static bool after_func_stop_nothing(void *env) {
    (void)env;
    return false;
}

Context context_after_func(Alloc *a, Context parent, Func f, StopFunc *stop) {
    Context none = {NULL, NULL};

    if (BURROW_CONTEXT_IS_NIL(parent))
        panic_str(BURROW_S("cannot create context from nil parent"));

    if (stop != NULL)
        *stop = BURROW_FN(StopFunc, after_func_stop_nothing, NULL);

    AfterFuncCtx *af = BURROW_NEW(a, AfterFuncCtx);
    if (af == NULL)
        return none;

    Chan *done = chan_make(a, TYPE_UINT8, 0);
    if (done == NULL) {
        mem_free(a, af, sizeof(AfterFuncCtx), _Alignof(AfterFuncCtx));
        return none;
    }

    CancelCtx *c = &af->c;

    c->parent = parent;
    c->a = a;
    c->self = c;
    c->done = done;
    c->refs = 1;
    c->after = true;
    af->f = f;

    /* Everything above is in place first, because this is where a parent that
     * is already cancelled cancels this node, and that reads the flag and the
     * function. Go orders it the same way. */
    if (!propagate_cancel(c, parent)) {
        chan_free(done);
        mem_free(a, af, sizeof(AfterFuncCtx), _Alignof(AfterFuncCtx));
        return none;
    }

    if (stop != NULL)
        *stop = BURROW_FN(StopFunc, after_func_stop, af);

    Context out = {&cancel_vt, c};
    return out;
}

/* ------------------------------------------------------- WithDeadline, WithTimeout
 *
 * A deadline is a cancel node with a timer pointed at it, which is all Go's
 * timerCtx is. The only thing worth thinking about is who owns the node while
 * the timer is armed.
 *
 * The timer holds a reference. It has to: the callback runs on a goroutine of
 * its own and touches the node, and a program is allowed to free its context a
 * nanosecond before the deadline. Whoever disarms the timer first puts that
 * reference down, which is the callback when the deadline wins and cancel_node
 * when the cancel wins, and time_timer_stop answering true is what says which
 * of the two happened.
 *
 * There is one hole and it is time_after_func's. The callback is started with
 * go, a goroutine that will not start is a program with no memory left, and
 * then the reference the timer held is never put down and the node is never
 * freed. Nothing better is available from here: refusing to arm the timer is
 * the same failure earlier, and taking the reference back from inside the
 * runtime is a race. A leaked node on the way out of memory is the least of
 * what is going wrong by then. */

static bool timer_deadline(void *self, int64_t *when) {
    *when = ((TimerCtx *)self)->when;
    return true;
}

/* What the timer runs when the deadline arrives.
 *
 * On a goroutine of its own, because that is what a time_after_func callback
 * gets and because this one wants it: cancelling walks a subtree taking a lock
 * and closing a channel per node, and none of that belongs on a scheduler
 * thread in the middle of looking for work. Go runs timerCtx's cancellation on
 * a goroutine for the same reason.
 *
 * The release at the end is the timer's own reference. The stop inside
 * cancel_node cannot have taken it as well, because a timer that has already
 * fired answers false to a stop. */
static void deadline_reached(void *env) {
    CancelCtx *c = (CancelCtx *)env;

    cancel_node(c, true, context_deadline_exceeded, ((TimerCtx *)c)->deadline_cause);
    context_release(c);
}

Context context_with_deadline_cause(Alloc *a, Context parent, int64_t when, Error cause,
                                    CancelFunc *cancel) {
    Context none = {NULL, NULL};

    if (BURROW_CONTEXT_IS_NIL(parent))
        panic_str(BURROW_S("cannot create context from nil parent"));

    /* Checked here rather than left to time_after_func, so that the message
     * says what the program got wrong rather than naming a function it has
     * never heard of. Checked on every path and not only on the one that arms a
     * timer, because a call that works or throws depending on what the clock
     * says is worse than one that always throws. */
    if (burrow__timers_local() == NULL)
        runtime_throw(
            BURROW_S("context: a deadline needs a goroutine to put the timer on"));

    /* A parent that gives up sooner makes the timer pointless: its cancellation
     * arrives first every time, and a plain cancel node answers its parent's
     * deadline when it has none of its own, so nothing is lost by not having
     * one. Go hands the whole job to WithCancel here and uses the same strict
     * comparison, so two contexts with the same instant on them both get a
     * timer. */
    int64_t parent_when = 0;
    if (context_deadline(parent, &parent_when) && parent_when < when)
        return context_with_cancel(a, parent, cancel);

    if (cancel != NULL)
        *cancel = BURROW_FN(CancelFunc, cancel_nothing, NULL);

    TimerCtx *t = BURROW_NEW(a, TimerCtx);
    if (t == NULL)
        return none;

    Chan *done = chan_make(a, TYPE_UINT8, 0);
    if (done == NULL) {
        mem_free(a, t, sizeof(TimerCtx), _Alignof(TimerCtx));
        return none;
    }

    CancelCtx *c = &t->c;

    c->parent = parent;
    c->a = a;
    c->self = c;
    c->done = done;
    c->refs = 1;
    c->timed = true;
    t->when = when;
    t->deadline_cause = cause;

    if (!propagate_cancel(c, parent)) {
        chan_free(done);
        mem_free(a, t, sizeof(TimerCtx), _Alignof(TimerCtx));
        return none;
    }

    if (cancel != NULL)
        *cancel = BURROW_FN(CancelFunc, cancel_func, c);

    Context out = {&timer_vt, c};

    /* Already gone by, so there is nothing to arm. Go cancels here too and
     * hands back a context whose done channel is closed, which is more useful
     * than a failure: the caller's select wakes at once and takes the same path
     * it would have taken a second later. */
    Duration left = when - burrow_nanotime();
    if (left <= 0) {
        cancel_node(c, false, context_deadline_exceeded, t->deadline_cause);
        return out;
    }

    bool no_timer = false;

    /* Armed under the lock, and only if nothing has cancelled this already,
     * which is what Go does at the same point. A parent that cancelled during
     * propagate_cancel has closed the channel and set the error, and a timer
     * armed after that is one nobody will ever stop. */
    sync_mutex_lock(&c->mu);
    if (BURROW_OK(c->err)) {
        (void)burrow__atomic_add_u32(&c->refs, 1);

        TimeTimer *timer =
            time_after_func(a, left, BURROW_FN(Func, deadline_reached, c));
        if (timer == NULL) {
            (void)burrow__atomic_add_u32(&c->refs, (uint32_t)-1);
            no_timer = true;
        }
        t->timer = timer;
    }
    sync_mutex_unlock(&c->mu);

    /* No memory for the timer. Undo what propagate_cancel did, which is exactly
     * what context_free does, and answer the nil context. The release here does
     * not always free: a watcher goroutine may still hold the node, and then it
     * frees when it wakes. */
    if (no_timer) {
        cancel_node(c, true, context_canceled, BURROW_NO_ERROR);
        context_release(c);
        if (cancel != NULL)
            *cancel = BURROW_FN(CancelFunc, cancel_nothing, NULL);
        return none;
    }

    return out;
}

Context context_with_deadline(Alloc *a, Context parent, int64_t when,
                              CancelFunc *cancel) {
    return context_with_deadline_cause(a, parent, when, BURROW_NO_ERROR, cancel);
}

/* now + d, with the two constructors that take a duration sharing the one
 * conversion to an instant rather than each doing it slightly differently. */
static int64_t context_deadline_from(Duration d) {
    int64_t now = burrow_nanotime();

    /* Without the signed overflow, which is undefined rather than negative. A
     * reading of this clock is never below zero, so the only end that can be
     * run off is the far one, and a timeout that lands 292 years out is one
     * nobody meant. */
    if (d > 0 && d > INT64_MAX - now)
        return INT64_MAX;
    return now + d;
}

Context context_with_timeout_cause(Alloc *a, Context parent, Duration d, Error cause,
                                   CancelFunc *cancel) {
    return context_with_deadline_cause(a, parent, context_deadline_from(d), cause,
                                       cancel);
}

Context context_with_timeout(Alloc *a, Context parent, Duration d, CancelFunc *cancel) {
    return context_with_deadline_cause(a, parent, context_deadline_from(d),
                                       BURROW_NO_ERROR, cancel);
}

/* --------------------------------------------------------------------- free */

void context_free(Context c) {
    if (c.vt == NULL || c.vt == &background_vt || c.vt == &todo_vt)
        return;

    if (c.vt == &cancel_vt || c.vt == &timer_vt) {
        CancelCtx *cc = (CancelCtx *)c.data;

        /* An AfterFunc registration is stopped before it is cancelled, or the
         * free would be what starts the function. Giving something back is not
         * a reason for its cleanup to run, and a caller that wanted the
         * function to run has a stop function to not call. This does nothing if
         * the cancellation already got there. */
        if (cc->after)
            (void)after_func_stop(cc);

        /* Cancel first, so that the done channel is closed and anything parked
         * on it has been let go before the channel stops existing. */
        cancel_node(cc, true, context_canceled, BURROW_NO_ERROR);
        context_release(cc);
        return;
    }

    if (c.vt == &value_vt) {
        ValueCtx *v = (ValueCtx *)c.data;

        mem_free(v->a, v, sizeof(ValueCtx), _Alignof(ValueCtx));
        return;
    }

    if (c.vt == &without_cancel_vt) {
        WithoutCancelCtx *w = (WithoutCancelCtx *)c.data;

        mem_free(w->a, w, sizeof(WithoutCancelCtx), _Alignof(WithoutCancelCtx));
        return;
    }

    runtime_throw(BURROW_S("context: freeing a context this package did not make"));
}
