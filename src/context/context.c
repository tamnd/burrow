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
 *     a watcher goroutine exists. That is what makes it safe to free a context
 *     whose watcher has not woken up yet: whoever puts the node down last frees
 *     it, and the loser of that race has already stopped touching it.
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

/* ------------------------------------------------------------- the two nodes */

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
     * when there is one. Whoever drops the last reference frees. */
    uint32_t refs;

    SyncMutex mu;

    /* Set once, under mu, at the same moment done is closed. */
    Error err;

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

typedef struct ValueCtx {
    Context parent;
    Alloc *a;
    Any key;
    Any val;
} ValueCtx;

/* ------------------------------------------------------------- the vtables */

static bool empty_deadline(void *self, int64_t *when);
static Chan *empty_done(void *self);
static Error empty_err(void *self);
static Any empty_value(void *self, Any key);

static bool cancel_deadline(void *self, int64_t *when);
static Chan *cancel_done(void *self);
static Error cancel_err(void *self);
static Any cancel_value(void *self, Any key);

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

        if (c.vt == &cancel_vt) {
            CancelCtx *cc = (CancelCtx *)c.data;
            if (any_equal(key, cancel_key))
                return BURROW_ANY(&cancel_ctx_type, &cc->self);
            c = cc->parent;
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
static void cancel_node(CancelCtx *c, bool remove_from_parent, Error err) {
    if (BURROW_OK(err))
        panic_str(BURROW_S("context: internal error: missing cancel error"));

    sync_mutex_lock(&c->mu);
    if (BURROW_FAILED(c->err)) {
        sync_mutex_unlock(&c->mu);
        return;
    }

    c->err = err;
    chan_close(c->done);

    CancelCtx *child = c->children;
    while (child != NULL) {
        CancelCtx *after = child->next;

        child->prev = NULL;
        child->next = NULL;
        cancel_node(child, false, err);
        child = after;
    }
    c->children = NULL;
    sync_mutex_unlock(&c->mu);

    if (remove_from_parent)
        remove_child(c);
}

/* Puts one reference down, and frees on the last one.
 *
 * The count is one for the whole life of almost every context, so this is an
 * atomic subtract and a branch. It is two only while a watcher goroutine is
 * alive, and it exists for that case alone: the watcher may be on the point of
 * waking when the program frees the context, and without the count there is no
 * moment at which freeing is safe. */
static void release(CancelCtx *c) {
    if (burrow__atomic_add_u32(&c->refs, (uint32_t)-1) != 1)
        return;

    Alloc *a = c->a;

    chan_free(c->done);
    mem_free(a, c, sizeof(CancelCtx), _Alignof(CancelCtx));
}

static void cancel_func(void *env) {
    cancel_node((CancelCtx *)env, true, context_canceled);
}

/* What *cancel is set to when the context could not be made. Calling a cancel
 * for a context that does not exist has nothing to undo, and a caller who
 * deferred the cancel before looking at the context is then still right. */
static void cancel_nothing(void *env) {
    (void)env;
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
        cancel_node(c, false, context_err(c->parent));

    release(c);
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
        cancel_node(c, false, context_err(parent));
        return true;
    }

    CancelCtx *p = parent_cancel_ctx(parent);
    if (p != NULL) {
        sync_mutex_lock(&p->mu);
        if (BURROW_FAILED(p->err)) {
            /* The parent was cancelled between the check above and this lock,
             * which is the race the whole lock is here for. */
            cancel_node(c, false, p->err);
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

Context context_with_cancel(Alloc *a, Context parent, CancelFunc *cancel) {
    Context none = {NULL, NULL};

    if (BURROW_CONTEXT_IS_NIL(parent))
        panic_str(BURROW_S("cannot create context from nil parent"));

    if (cancel != NULL)
        *cancel = BURROW_FN(CancelFunc, cancel_nothing, NULL);

    CancelCtx *c = BURROW_NEW(a, CancelCtx);
    if (c == NULL)
        return none;

    Chan *done = chan_make(a, TYPE_UINT8, 0);
    if (done == NULL) {
        mem_free(a, c, sizeof(CancelCtx), _Alignof(CancelCtx));
        return none;
    }

    c->parent = parent;
    c->a = a;
    c->self = c;
    c->done = done;
    c->refs = 1;

    if (!propagate_cancel(c, parent)) {
        chan_free(done);
        mem_free(a, c, sizeof(CancelCtx), _Alignof(CancelCtx));
        return none;
    }

    if (cancel != NULL)
        *cancel = BURROW_FN(CancelFunc, cancel_func, c);

    Context out = {&cancel_vt, c};
    return out;
}

/* --------------------------------------------------------------------- free */

void context_free(Context c) {
    if (c.vt == NULL || c.vt == &background_vt || c.vt == &todo_vt)
        return;

    if (c.vt == &cancel_vt) {
        CancelCtx *cc = (CancelCtx *)c.data;

        /* Cancel first, so that the done channel is closed and anything parked
         * on it has been let go before the channel stops existing. */
        cancel_node(cc, true, context_canceled);
        release(cc);
        return;
    }

    if (c.vt == &value_vt) {
        ValueCtx *v = (ValueCtx *)c.data;

        mem_free(v->a, v, sizeof(ValueCtx), _Alignof(ValueCtx));
        return;
    }

    runtime_throw(BURROW_S("context: freeing a context this package did not make"));
}
