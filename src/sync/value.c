/* Derived from Go's src/sync/atomic/value.go.
 * Go source: go1.27.1.
 *
 * Copyright 2009 The Go Authors. All rights reserved.
 * Copyright 2026 The burrow Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style licence that can be found
 * in the LICENSE file. */

#include "burrow/sync/atomic.h"

#include "burrow/atomic.h"
#include "burrow/core.h"
#include "burrow/iface.h"
#include "burrow/panic.h"
#include "burrow/thread.h"
#include "burrow/type.h"

#include <stdbool.h>

/* A Value is two words, and no machine here stores two words at once.
 *
 * Go's answer, which this is a port of, is that the two words are not equal
 * partners. The type descriptor is the one that says whether there is anything
 * to read, so it is written last on the way in and read first on the way out,
 * and a reader that sees a type knows the data beside it was already there.
 * That covers every store after the first one, because from then on the type
 * never changes and a store is a single word.
 *
 * The first store is the one that has to change both, and it is serialised with
 * a sentinel: the type word goes from nothing to an address that is not a type,
 * then the data is written, then the real type replaces the sentinel. A reader
 * that sees the sentinel is a reader that arrived in the middle and answers nil,
 * which is the same answer it would have given a moment earlier. A second
 * storer that sees it waits, because there is nothing else it can safely do:
 * it does not yet know what type this Value is going to be.
 *
 * The window is two stores wide and nobody can be in it twice, so the wait is a
 * spin rather than anything that sleeps. Go spins with preemption disabled so
 * that the thread in the window cannot be taken off its processor. burrow has
 * no such switch yet, so the spin gives up and asks the system for a turn after
 * a while, which costs a syscall in a case that essentially never happens and
 * is the difference between a slow first store and a livelock on one core. */

/* Not a type, never equal to one, and the address is all that is ever used. It
 * is const because nothing writes to it, which is also what keeps it out of
 * tools/globals.txt: a global nobody can write is not state. */
static const Byte first_store_in_progress = 0;

#define STORING ((Uintptr)(&first_store_in_progress))

/* How many turns of the loop happen before asking the system for a new
 * timeslice. Small, because the thread being waited for has two stores left to
 * do, and anything longer than that means it was descheduled and no amount of
 * spinning will bring it back sooner. */
#define SPINS 64

static void back_off(Int *rounds) {
    if (*rounds < SPINS) {
        (*rounds)++;
        burrow__atomic_spin_hint();
        return;
    }
    burrow__thread_yield();
}

static Any pack(Uintptr typ, Uintptr data) {
    Any v;

    v.t = (const Type *)typ;
    v.data = (void *)data;
    return v;
}

/* Claims an empty Value and fills it in. False means somebody else claimed it
 * between the load and here, and the caller has to look at what they left. */
static bool try_first_store(SyncAtomicValue *v, Any val) {
    Uintptr empty = 0;

    if (!burrow__atomic_cas_uptr(&v->typ, &empty, STORING))
        return false;

    burrow__atomic_store_uptr(&v->data, (Uintptr)val.data);
    burrow__atomic_store_uptr(&v->typ, (Uintptr)val.t);
    return true;
}

Any sync_atomic_value_load(const SyncAtomicValue *v) {
    Uintptr typ = burrow__atomic_load_uptr(&v->typ);

    if (typ == 0 || typ == STORING)
        return pack(0, 0);

    return pack(typ, burrow__atomic_load_uptr(&v->data));
}

void sync_atomic_value_store(SyncAtomicValue *v, Any val) {
    Int rounds = 0;

    if (BURROW_ANY_IS_NIL(val))
        panic_str(BURROW_S("sync/atomic: store of nil value into Value"));

    for (;;) {
        Uintptr typ = burrow__atomic_load_uptr(&v->typ);

        if (typ == 0) {
            if (try_first_store(v, val))
                return;
            continue;
        }
        if (typ == STORING) {
            back_off(&rounds);
            continue;
        }
        if (typ != (Uintptr)val.t)
            panic_str(BURROW_S(
                "sync/atomic: store of inconsistently typed value into Value"));

        burrow__atomic_store_uptr(&v->data, (Uintptr)val.data);
        return;
    }
}

Any sync_atomic_value_swap(SyncAtomicValue *v, Any val) {
    Int rounds = 0;

    if (BURROW_ANY_IS_NIL(val))
        panic_str(BURROW_S("sync/atomic: swap of nil value into Value"));

    for (;;) {
        Uintptr typ = burrow__atomic_load_uptr(&v->typ);

        if (typ == 0) {
            if (try_first_store(v, val))
                return pack(0, 0);
            continue;
        }
        if (typ == STORING) {
            back_off(&rounds);
            continue;
        }
        if (typ != (Uintptr)val.t)
            panic_str(
                BURROW_S("sync/atomic: swap of inconsistently typed value into Value"));

        return pack(typ, burrow__atomic_swap_uptr(&v->data, (Uintptr)val.data));
    }
}

bool sync_atomic_value_compare_and_swap(SyncAtomicValue *v, Any old, Any val) {
    Int rounds = 0;

    if (BURROW_ANY_IS_NIL(val))
        panic_str(BURROW_S("sync/atomic: compare and swap of nil value into Value"));
    if (!BURROW_ANY_IS_NIL(old) && old.t != val.t)
        panic_str(BURROW_S("sync/atomic: compare and swap of inconsistently typed "
                           "values"));

    for (;;) {
        Uintptr typ = burrow__atomic_load_uptr(&v->typ);
        Uintptr data;
        Uintptr seen;

        if (typ == 0) {
            if (!BURROW_ANY_IS_NIL(old))
                return false;
            if (try_first_store(v, val))
                return true;
            continue;
        }
        if (typ == STORING) {
            back_off(&rounds);
            continue;
        }
        if (typ != (Uintptr)val.t)
            panic_str(BURROW_S("sync/atomic: compare and swap of inconsistently typed "
                               "value into Value"));

        /* The comparison goes through the descriptor rather than comparing the
         * two data words, which is what lets this work on a value type: two
         * copies of the same struct are equal here and are two different
         * addresses. Go does the same thing for the same reason, and it is the
         * one place where Value offers something the plain functions cannot.
         *
         * The swap below still compares the address, because that is what makes
         * it a compare and swap: an equal value that arrived after this load is
         * a value this call did not see, and storing over it would lose it. */
        data = burrow__atomic_load_uptr(&v->data);
        if (!any_equal(pack(typ, data), old))
            return false;

        seen = data;
        return burrow__atomic_cas_uptr(&v->data, &seen, (Uintptr)val.data);
    }
}
