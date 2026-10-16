/* go/types: the common underlying type of a type set.
 *
 * Go's typeError is a format and its arguments, formatted when the error is
 * reported. Here the cause is formatted when the error is made, in check->a,
 * which gives the same text because the qualifier does not change in between.
 * Go's empty typeError is an empty cause.
 *
 * Derived from Go's src/go/types/under.go. Go source: go1.27.1.
 *
 * Copyright 2011 The Go Authors. All rights reserved.
 * Copyright 2026 The burrow Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style licence that can be found
 * in the LICENSE file. */

#include "burrow/go/types.h"

#include "burrow/core.h"

#include "types_check.h"
#include "types_internal.h"

#include <stdbool.h>

/* What the walk of commonUnder over the type set closes over. */
typedef struct TudCommon {
    TypesChecker *check;
    GtUnderCond cond;
    TypesType ct, cu; /* type and respective common underlying type */
    bool failed;
    Str cause;
} TudCommon;

static bool tud_fail(TudCommon *c, Str cause) {
    c->failed = true;
    c->cause = cause;
    return false;
}

static bool tud_common(void *env, TypesType t, TypesType u) {
    TudCommon *c = env;
    TypesChecker *check = c->check;
    if (c->cond.f != NULL) {
        Str cause = {0};
        if (!BURROW_CALLF(c->cond, t, u, &cause))
            return tud_fail(c, cause);
    }

    if (u == NULL)
        return tud_fail(c, BURROW_S("no specific type"));

    /* If this is the first type we're seeing, we're done. */
    if (c->cu == NULL) {
        c->ct = t;
        c->cu = u;
        return true;
    }

    /* If we've seen a channel before, and we have a channel now, they must be
     * compatible. */
    TypesChan *chu = types_as_chan(c->cu);
    TypesChan *ch = types_as_chan(u);
    if (chu != NULL && ch != NULL) {
        if (!types_identical(chu->elem, ch->elem)) {
            if (check == NULL)
                return tud_fail(c, BURROW_S(""));
            Str cts = burrow__tc_ts(check, c->ct);
            Str ts = burrow__tc_ts(check, t);
            return tud_fail(
                c, GT_SPRINTF(check, "channels %s and %s have different element types",
                              cts, ts));
        }
        /* If we have different channel directions, keep the restricted one and
         * complain if they conflict. */
        if (chu->dir == ch->dir) {
            /* nothing to do */
        } else if (chu->dir == TYPES_SEND_RECV) {
            /* switch to restricted channel */
            c->ct = t;
            c->cu = u;
        } else if (ch->dir != TYPES_SEND_RECV) {
            if (check == NULL)
                return tud_fail(c, BURROW_S(""));
            Str cts = burrow__tc_ts(check, c->ct);
            Str ts = burrow__tc_ts(check, t);
            return tud_fail(
                c, GT_SPRINTF(check, "channels %s and %s have conflicting directions",
                              cts, ts));
        }
        return true;
    }

    /* Otherwise, the current type must have the same underlying type as all
     * previous types. */
    if (!types_identical(c->cu, u)) {
        if (check == NULL)
            return tud_fail(c, BURROW_S(""));
        Str cts = burrow__tc_ts(check, c->ct);
        Str ts = burrow__tc_ts(check, t);
        return tud_fail(
            c, GT_SPRINTF(check, "%s and %s have different underlying types", cts, ts));
    }
    return true;
}

/* commonUnder: if t is a type parameter, cond is nil, and t's type set
 * contains no channel types, the common underlying type of all types in t's
 * type set if it exists, or NULL and a type error otherwise.
 *
 * If t is a type parameter, cond is nil, and there are channel types, t's type
 * set must only contain channel types, they must all have the same element
 * types, channel directions must not conflict, and the result is one of the
 * most restricted channels. Otherwise, the function returns NULL and a type
 * error.
 *
 * If cond is not nil, each pair (t, u) of type and underlying type in t's type
 * set must satisfy the condition expressed by cond. If cond fails, the result
 * is NULL and the type error reported by cond. Note that cond is called before
 * any other conditions are checked; specifically cond may be called with
 * (NULL, NULL) if the type set contains no specific types.
 *
 * If t is not a type parameter, commonUnder behaves as if t was a type
 * parameter with the single type t in its type set.
 *
 * Not in Go: check may be NULL when cause is, and then the causes are not
 * formatted. */
TypesType burrow__tc_common_under(TypesChecker *check, TypesType t, GtUnderCond cond,
                                  Str *cause) {
    burrow__types_assert(check != NULL || cause == NULL);
    TudCommon c = {check, cond, NULL, NULL, false, {0}};
    (void)burrow__types_all(t, BURROW_FN(GtTypePairFunc, tud_common, &c));
    if (cause != NULL)
        *cause = c.cause;
    if (c.failed)
        return NULL;
    return c.cu;
}
