/* Derived from Go's src/iter/iter.go.
 * Go source: go1.27.1.
 *
 * Pull and Pull2, on the runtime's coroutines. The shape is Go's line for line:
 * the coroutine runs the sequence with a yield that hands each value back and
 * switches, and a deferred call on the coroutine's side notes how the sequence
 * ended so that next and stop can do the same thing on the caller's side.
 *
 * Copyright 2009 The Go Authors. All rights reserved.
 * Copyright 2026 The burrow Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style licence that can be found
 * in the LICENSE file. */

#include "burrow/iter.h"

#include "burrow/core.h"
#include "burrow/defer.h"
#include "burrow/panic.h"
#include "burrow/proc.h"
#include "burrow/runtime.h"
#include "burrow/sched.h"

#include <string.h>

/* Keeps a panic value that was caught on the coroutine. A value of thirty two
 * bytes or less lives in the catching frame, which is gone once the coroutine
 * exits, so it moves into the pull state, which the caller owns. */
static void iter_keep_panic(Any *dst, burrow__PanicValue *storage, Any v,
                            const burrow__PanicValue *frame) {
    *dst = v;
    if (v.data == (const void *)frame) {
        *storage = *frame;
        dst->data = storage;
    }
}

/* What next and stop do once the coroutine has switched back: carry on the way
 * the sequence ended, if it ended badly. */
static void iter_rethrow(Any v, bool goexit) {
    if (goexit)
        runtime_goexit();
    if (!BURROW_ANY_IS_NIL(v))
        panic(v);
}

/* ------------------------------------------------------------------- Pull */

static bool iter_pull_yield(void *env, const void *v) {
    IterPull *p = (IterPull *)env;
    if (p->done)
        return false;
    if (!p->yield_next)
        panic_str(BURROW_S("iter.Pull: yield called again before next"));
    p->yield_next = false;
    p->v = v;
    p->ok = true;
    burrow__coroswitch(&p->coro);
    return !p->done;
}

static void iter_pull_finish(void *env) {
    IterPull *p = (IterPull *)env;
    if (BURROW_ANY_IS_NIL(p->panic_value) && !p->seq_done)
        p->goexit = true;
    p->done = true; /* Invalidate iterator */
}

/* Runs the sequence. This is its own function so the yield value is built
 * outside the frame that holds the setjmp, where gcc warns it might be
 * clobbered. */
static void iter_pull_run(IterPull *p) {
    BURROW_CALLF(p->seq, BURROW_FN(IterYield, iter_pull_yield, p));
}

static void iter_pull_body(burrow__Coro *c, void *env) {
    (void)c;
    IterPull *p = (IterPull *)env;
    if (p->done)
        return;
    BURROW_SCOPE {
        BURROW_DEFER(iter_pull_finish, p);
        BURROW_TRY {
            iter_pull_run(p);
            p->v = NULL;
            p->ok = false;
            p->seq_done = true;
        }
        BURROW_CATCH(v) {
            iter_keep_panic(&p->panic_value, &p->storage, v, &burrow__rec.storage);
        }
        BURROW_TRY_END;
    }
    BURROW_SCOPE_END;
}

bool iter_pull(IterPull *p, IterSeq seq) {
    memset(p, 0, sizeof *p);
    p->seq = seq;
    if (!burrow__newcoro(&p->coro, iter_pull_body, p)) {
        p->done = true;
        return false;
    }
    return true;
}

bool iter_pull_next(IterPull *p, const void **v) {
    *v = NULL;
    if (p->done)
        return false;
    if (p->yield_next)
        panic_str(BURROW_S("iter.Pull: next called again before yield"));
    p->yield_next = true;
    burrow__coroswitch(&p->coro);
    iter_rethrow(p->panic_value, p->goexit);
    *v = p->v;
    return p->ok;
}

void iter_pull_stop(IterPull *p) {
    if (!p->done) {
        p->done = true;
        burrow__coroswitch(&p->coro);
        iter_rethrow(p->panic_value, p->goexit);
    }
}

/* ------------------------------------------------------------------ Pull2 */

static bool iter_pull2_yield(void *env, const void *k, const void *v) {
    IterPull2 *p = (IterPull2 *)env;
    if (p->done)
        return false;
    if (!p->yield_next)
        panic_str(BURROW_S("iter.Pull2: yield called again before next"));
    p->yield_next = false;
    p->k = k;
    p->v = v;
    p->ok = true;
    burrow__coroswitch(&p->coro);
    return !p->done;
}

static void iter_pull2_finish(void *env) {
    IterPull2 *p = (IterPull2 *)env;
    if (BURROW_ANY_IS_NIL(p->panic_value) && !p->seq_done)
        p->goexit = true;
    p->done = true; /* Invalidate iterator. */
}

/* Runs the sequence. This is its own function so the yield value is built
 * outside the frame that holds the setjmp, where gcc warns it might be
 * clobbered. */
static void iter_pull2_run(IterPull2 *p) {
    BURROW_CALLF(p->seq, BURROW_FN(IterYield2, iter_pull2_yield, p));
}

static void iter_pull2_body(burrow__Coro *c, void *env) {
    (void)c;
    IterPull2 *p = (IterPull2 *)env;
    if (p->done)
        return;
    BURROW_SCOPE {
        BURROW_DEFER(iter_pull2_finish, p);
        BURROW_TRY {
            iter_pull2_run(p);
            p->k = NULL;
            p->v = NULL;
            p->ok = false;
            p->seq_done = true;
        }
        BURROW_CATCH(v) {
            iter_keep_panic(&p->panic_value, &p->storage, v, &burrow__rec.storage);
        }
        BURROW_TRY_END;
    }
    BURROW_SCOPE_END;
}

bool iter_pull2(IterPull2 *p, IterSeq2 seq) {
    memset(p, 0, sizeof *p);
    p->seq = seq;
    if (!burrow__newcoro(&p->coro, iter_pull2_body, p)) {
        p->done = true;
        return false;
    }
    return true;
}

bool iter_pull2_next(IterPull2 *p, const void **k, const void **v) {
    *k = NULL;
    *v = NULL;
    if (p->done)
        return false;
    if (p->yield_next)
        panic_str(BURROW_S("iter.Pull2: next called again before yield"));
    p->yield_next = true;
    burrow__coroswitch(&p->coro);
    iter_rethrow(p->panic_value, p->goexit);
    *k = p->k;
    *v = p->v;
    return p->ok;
}

void iter_pull2_stop(IterPull2 *p) {
    if (!p->done) {
        p->done = true;
        burrow__coroswitch(&p->coro);
        iter_rethrow(p->panic_value, p->goexit);
    }
}

/* ------------------------------------------------------------ BURROW_RANGE */

bool burrow__iter_range_next(burrow__IterRange *r, void *out, size_t size) {
    if (!r->started) {
        r->started = true;
        if (!iter_pull(&r->p, r->seq))
            panic_str(BURROW_S("iter: out of memory starting a range loop"));
    }
    const void *v;
    if (!iter_pull_next(&r->p, &v))
        return false;
    memcpy(out, v, size);
    return true;
}

void burrow__iter_range_end(burrow__IterRange *r) {
    r->live = false;
    if (r->started)
        iter_pull_stop(&r->p);
}

bool burrow__iter_range2_next(burrow__IterRange2 *r, void *kout, size_t ksize,
                              void *vout, size_t vsize) {
    if (!r->started) {
        r->started = true;
        if (!iter_pull2(&r->p, r->seq))
            panic_str(BURROW_S("iter: out of memory starting a range loop"));
    }
    const void *k;
    const void *v;
    if (!iter_pull2_next(&r->p, &k, &v))
        return false;
    memcpy(kout, k, ksize);
    memcpy(vout, v, vsize);
    return true;
}

void burrow__iter_range2_end(burrow__IterRange2 *r) {
    r->live = false;
    if (r->started)
        iter_pull2_stop(&r->p);
}
