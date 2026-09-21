/* sync.Once and the three wrappers around it. See burrow/sync.h.
 *
 * The promise is not "f runs once", it is "when any call returns, f has
 * finished". A compare and swap on a flag gives the first and not the second:
 * the caller that loses the race would return straight away, past a thing that
 * is still being built. So the slow path is a mutex, the flag is set after f
 * returns rather than before it runs, and the loser waits on the mutex for the
 * winner. The fast path is still one atomic load, which is all a Once that has
 * already run ever costs.
 *
 * Derived from Go's src/sync/once.go.
 * The three wrappers below it come from Go's src/sync/oncefunc.go.
 * Go source: go1.27.1.
 *
 * Copyright 2009 The Go Authors. All rights reserved.
 * Copyright 2026 The burrow Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style licence that can be found
 * in the LICENSE file. */

#include "burrow/sync.h"

#include "burrow/core.h"
#include "burrow/func.h"
#include "burrow/iface.h"
#include "burrow/panic.h"
#include "burrow/sync/atomic.h"
#include "burrow/type.h"

#include <stdbool.h>
#include <stddef.h>

static const Type once_desc = {
    {(const Byte *)"Once", 4},
    {(const Byte *)"sync", 4},
    KIND_STRUCT,
    (uint32_t)sizeof(SyncOnce),
    (uint16_t)_Alignof(SyncOnce),
    0,
    0,
    NULL,
    NULL,
    NULL,
    NULL,
    0,
    0x736f6e63U, /* "sonc", distinct from every builtin's */
    NULL,
};

const Type *const TYPE_SYNC_ONCE = &once_desc;

void burrow__sync_once_do_slow(SyncOnce *o, Func f) {
    sync_mutex_lock(&o->m);

    /* Somebody else may have run it between the load in the fast path and this
     * lock, and the lock is what made that visible. */
    if (!sync_atomic_bool_load(&o->done)) {
        /* Marked done whatever happens, including a panic, because f had its
         * turn and running it again on the next call would be running it
         * twice. The flag is a release store, so a caller that sees it also
         * sees everything f wrote. */
        BURROW_TRY {
            BURROW_CALLF0(f);
            sync_atomic_bool_store(&o->done, true);
        }
        BURROW_CATCH(p) {
            sync_atomic_bool_store(&o->done, true);
            sync_mutex_unlock(&o->m);
            panic(p);
        }
        BURROW_TRY_END;
    }

    sync_mutex_unlock(&o->m);
}

/* Copies a caught panic value somewhere that outlives the frame that caught it.
 *
 * The catch block's value lives in the catching frame, which is the frame of
 * the run function below, so raising it again from a later call means copying
 * it out first. This is the same thirty two byte bargain the panic machinery
 * makes for a catch block, written here against the caller's own storage.
 *
 * A value too big to fit keeps pointing where it pointed, which is the case
 * burrow/sync.h tells the caller about. */
static Any keep(burrow__PanicValue *storage, Any v) {
    if (v.t == NULL || v.data == NULL)
        return v;
    if (v.t->size > sizeof storage->bytes)
        return v;

    type_copy(v.t, storage->bytes, v.data);
    v.data = storage->bytes;
    return v;
}

/* ------------------------------------------------------------- OnceFunc
 *
 * A Once that remembers the panic instead of letting it out once and then
 * reporting the action as done. Every later caller gets the same panic, so a
 * failed initialisation cannot be mistaken for a finished one.
 *
 * The three of these are the same six lines with a different result type, and
 * they are written out three times rather than shared through a macro, because
 * a macro that expands to a panic and a longjmp is harder to read than the
 * thing it replaces. */

static void once_func_run(void *env) {
    SyncOnceFunc *of = (SyncOnceFunc *)env;
    Func f = of->f;

    /* Dropped before the call, so that whatever the environment points at is
     * not kept alive by this struct for the rest of the program. Go does the
     * same, in a defer, for the same reason. */
    of->f = (Func){NULL, NULL};

    BURROW_TRY {
        BURROW_CALLF0(f);
        of->ok = true;
    }
    BURROW_CATCH(p) {
        of->p = keep(&of->storage, p);

        /* Let the first caller have the panic with its stack still under it,
         * rather than a copy raised from here. Later callers get the copy,
         * which is the best that can be done for them. */
        panic(p);
    }
    BURROW_TRY_END;
}

void sync_once_func_call(SyncOnceFunc *of) {
    sync_once_do(&of->once, BURROW_FN(Func, once_func_run, of));

    if (!of->ok)
        panic(of->p);
}

static void once_func_thunk(void *env) {
    sync_once_func_call((SyncOnceFunc *)env);
}

Func sync_once_func_fn(SyncOnceFunc *of) {
    return BURROW_FN(Func, once_func_thunk, of);
}

/* ------------------------------------------------------------ OnceValue */

static void once_value_run(void *env) {
    SyncOnceValue *ov = (SyncOnceValue *)env;
    AnyFunc f = ov->f;

    ov->f = (AnyFunc){NULL, NULL};

    BURROW_TRY {
        ov->result = BURROW_CALLF0(f);
        ov->ok = true;
    }
    BURROW_CATCH(p) {
        ov->p = keep(&ov->storage, p);
        panic(p);
    }
    BURROW_TRY_END;
}

Any sync_once_value_get(SyncOnceValue *ov) {
    sync_once_do(&ov->once, BURROW_FN(Func, once_value_run, ov));

    if (!ov->ok)
        panic(ov->p);

    return ov->result;
}

/* ----------------------------------------------------------- OnceValues */

static void once_values_run(void *env) {
    SyncOnceValues *ov = (SyncOnceValues *)env;
    SyncOnceValuesFn f = ov->f;

    ov->f = (SyncOnceValuesFn){NULL, NULL};

    BURROW_TRY {
        BURROW_CALLF(f, &ov->first, &ov->second);
        ov->ok = true;
    }
    BURROW_CATCH(p) {
        ov->p = keep(&ov->storage, p);
        panic(p);
    }
    BURROW_TRY_END;
}

void sync_once_values_get(SyncOnceValues *ov, Any *a, Any *b) {
    sync_once_do(&ov->once, BURROW_FN(Func, once_values_run, ov));

    if (!ov->ok)
        panic(ov->p);

    if (a != NULL)
        *a = ov->first;
    if (b != NULL)
        *b = ov->second;
}
