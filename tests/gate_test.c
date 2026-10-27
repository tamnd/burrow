/* Derived from Go's src/internal/gate/gate_test.go.
 * Go source: go1.27.3.
 *
 * Copyright 2024 The Go Authors. All rights reserved.
 * Copyright 2026 The burrow Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style licence that can be found
 * in the LICENSE file. */

#include "../src/sync/gate.h"

#include "burrow/burrow.h"
#include "burrow/chan.h"
#include "burrow/context.h"
#include "burrow/mem/heap.h"
#include "burrow/time.h"

#include "check.h"

#include <stdbool.h>

static Alloc *heap(void) {
    return heap_allocator();
}

typedef struct LockEnv {
    TestingT *t;
    Gate *g;
    Chan *unlockedc;
    Chan *donec;
} LockEnv;

static void lock_and_unlock_child(void *env) {
    LockEnv *e = env;
    TestingT *t = e->t;
    bool set = burrow__gate_lock(e->g);
    if (!set)
        testing_t_errorf_v(t, "g.Lock of set gate: false, want true");
    if (!chan_try_recv(e->unlockedc, NULL, NULL))
        testing_t_errorf_v(t, "g.Lock succeeded while gate was held");
    burrow__gate_unlock(e->g, false);
    chan_close(e->donec);
}

static void TestGateLockAndUnlock(TestingT *t) {
    Gate g;
    if (!burrow__gate_init(&g, heap(), false))
        testing_t_fatal_v(t, "gate.New: out of memory");
    bool set = burrow__gate_lock(&g);
    if (set)
        testing_t_errorf_v(t, "g.Lock of never-locked gate: true, want false");
    LockEnv e = {t, &g, chan_make(heap(), TYPE_BOOL, 0),
                 chan_make(heap(), TYPE_BOOL, 0)};
    go(BURROW_FN(Func, lock_and_unlock_child, &e));
    time_sleep(1 * TIME_MILLISECOND);
    chan_close(e.unlockedc);
    burrow__gate_unlock(&g, true);
    chan_recv(e.donec, NULL);
    set = burrow__gate_lock(&g);
    if (set)
        testing_t_errorf_v(t, "g.Lock of unset gate: true, want false");
    burrow__gate_unlock(&g, set); /* Not in Go: leaves the gate free to destroy. */
    chan_free(e.unlockedc);
    chan_free(e.donec);
    burrow__gate_destroy(&g);
}

typedef struct WaitEnv {
    Gate *g;
    bool set;
    Chan *donec; /* Not in Go: the child says when it is finished with g */
} WaitEnv;

static void wait_and_lock_child(void *env) {
    WaitEnv *e = env;
    time_sleep(1 * TIME_MILLISECOND);
    burrow__gate_lock(e->g);
    e->set = true;
    burrow__gate_unlock(e->g, true);
    chan_close(e->donec);
}

static void TestGateWaitAndLock(TestingT *t) {
    Gate g;
    if (!burrow__gate_init(&g, heap(), false))
        testing_t_fatal_v(t, "gate.New: out of memory");
    /* WaitAndLock is canceled. */
    ContextCancelFunc cancel;
    Context ctx = context_with_timeout(heap(), context_background(),
                                       1 * TIME_MILLISECOND, &cancel);
    Error err = burrow__gate_wait_and_lock(&g, ctx);
    if (!errors_is(err, context_deadline_exceeded)) {
        testing_t_fatalf_v(t, "g.WaitAndLock = %v, want context.DeadlineExceeded", err);
    }
    /* WaitAndLock succeeds. */
    WaitEnv e = {&g, false, chan_make(heap(), TYPE_BOOL, 0)};
    go(BURROW_FN(Func, wait_and_lock_child, &e));
    err = burrow__gate_wait_and_lock(&g, context_background());
    if (BURROW_FAILED(err))
        testing_t_fatalf_v(t, "g.WaitAndLock = %v, want nil", err);
    if (!e.set)
        testing_t_fatalf_v(t, "g.WaitAndLock returned before gate was set");
    burrow__gate_unlock(&g, true);
    /* WaitAndLock succeeds when the gate is set and the context is canceled. */
    err = burrow__gate_wait_and_lock(&g, ctx);
    if (BURROW_FAILED(err))
        testing_t_fatalf_v(t, "g.WaitAndLock = %v, want nil", err);
    burrow__gate_unlock(&g, true); /* Not in Go: leaves the gate free to destroy. */
    chan_recv(e.donec, NULL);
    chan_free(e.donec);
    BURROW_CALLF0(cancel);
    context_release(ctx);
    burrow__gate_destroy(&g);
}

static void TestGateLockIfSet(TestingT *t) {
    Gate g;
    if (!burrow__gate_init(&g, heap(), false))
        testing_t_fatal_v(t, "gate.New: out of memory");
    bool locked = burrow__gate_lock_if_set(&g);
    if (locked)
        testing_t_fatalf_v(t, "g.LockIfSet of unset gate = %v, want false", locked);
    burrow__gate_lock(&g);
    locked = burrow__gate_lock_if_set(&g);
    if (locked)
        testing_t_fatalf_v(t, "g.LockIfSet of locked gate = %v, want false", locked);
    burrow__gate_unlock(&g, true);
    locked = burrow__gate_lock_if_set(&g);
    if (!locked)
        testing_t_fatalf_v(t, "g.LockIfSet of set gate = %v, want true", locked);
    burrow__gate_unlock(&g, true); /* Not in Go: leaves the gate free to destroy. */
    burrow__gate_destroy(&g);
}

#define TESTS(X)                                                                       \
    X(TestGateLockAndUnlock)                                                           \
    X(TestGateWaitAndLock)                                                             \
    X(TestGateLockIfSet)

TESTING_MAIN(TESTS)
