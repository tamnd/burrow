/* Copyright 2026 The burrow Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style licence that can be found
 * in the LICENSE file. */

#include "burrow/error.h"
#include "burrow/core.h"
#include "burrow/mem.h"
#include "burrow/slice.h"
#include "burrow/type.h"

#include <stdarg.h>
#include <string.h>

/* Errors, which is four functions and a convention.
 *
 * errors_is and errors_as are ports of Go's, walk for walk, including the part
 * where a multi error turns the walk from a chain into a depth first search of
 * a tree. That is not an edge case: errors.Join produces the tree form, every
 * wrapped error in the library produces the chain form, and code ported from Go
 * asks errors.Is questions about both. */

/* ------------------------------------------------------------- descriptors */

/* error itself. Go's error is a language builtin, not a library type, so it has
 * no package path, and it is an interface so it has no fields to describe.
 *
 * Not comparable through type_equal by the default memcmp, because two Error
 * values are equal when both words match and the padding between them, if a
 * platform ever has any, is not part of the value. The ops table says so
 * explicitly rather than relying on Error having no padding, which it does not
 * today on any target burrow builds for and which is not a thing to bet on. */
static bool error_ops_equal(const void *a, const void *b) {
    const Error *x = (const Error *)a;
    const Error *y = (const Error *)b;
    return x->vt == y->vt && x->data == y->data;
}

static uint64_t error_ops_hash(const void *p, uint64_t seed) {
    const Error *e = (const Error *)p;
    uint64_t h = seed ^ 0x9e3779b97f4a7c15U;
    h = (h ^ (uint64_t)(Uintptr)e->vt) * 0x100000001b3U;
    h = (h ^ (uint64_t)(Uintptr)e->data) * 0x100000001b3U;
    return h;
}

static const TypeOps error_ops = {
    error_ops_equal,
    error_ops_hash,
    NULL,
    NULL,
};

const Type burrow_type_Error = {
    {(const Byte *)"error", 5},
    {NULL, 0},
    KIND_INTERFACE,
    (uint32_t)sizeof(Error),
    (uint16_t)_Alignof(Error),
    0,
    0,
    NULL,
    NULL,
    NULL,
    NULL,
    0,
    0x65727200U, /* "err\0", distinct from every builtin's */
    &error_ops,
};

const Type *const TYPE_ERROR = &burrow_type_Error;

/* ---------------------------------------------------------------- sentinels */

/* Every sentinel in the library shares this one vtable, so a sentinel costs a
 * Str of rodata and nothing else. data is the const Str * that
 * BURROW_SENTINEL_ERROR put beside it.
 *
 * self_type is NULL on purpose. Go's sentinels are *errors.errorString values
 * and errors.As on them from outside the errors package cannot name the type,
 * so it never matches. Leaving it NULL gives the same answer without inventing
 * a descriptor for a type nobody can name. */
static Str sentinel_message(const void *self) {
    return *(const Str *)self;
}

const ErrorVT burrow_sentinel_error_vt = {
    NULL, sentinel_message, NULL, NULL, NULL, NULL, NULL,
};

BURROW_SENTINEL_ERROR(errors_err_unsupported, "unsupported operation");
BURROW_SENTINEL_ERROR(burrow_err_out_of_memory, "out of memory");

/* ------------------------------------------------------------- errors_new */

/* The struct and its text in one allocation, because an error is two loads from
 * whoever is printing it and splitting it across two allocations would put them
 * in different cache lines for no reason. The text follows the struct. */
typedef struct ErrorString {
    Str text;
} ErrorString;

static Str error_string_message(const void *self) {
    return ((const ErrorString *)self)->text;
}

static Error error_string_clone(const void *self, Alloc *a) {
    return errors_new(a, ((const ErrorString *)self)->text);
}

/* No self_type, for the reason written over burrow_sentinel_error_vt: Go's
 * errorString is unexported and errors.As can never match it. */
static const ErrorVT error_string_vt = {
    NULL, error_string_message, NULL, NULL, NULL, NULL, error_string_clone,
};

Error errors_new(Alloc *a, Str text) {
    size_t n;
    ErrorString *e;
    Byte *bytes;

    n = (text.len > 0) ? (size_t)text.len : 0;

    e = (ErrorString *)mem_alloc(a, sizeof(ErrorString) + n, _Alignof(ErrorString));
    if (e == NULL)
        return burrow_err_out_of_memory;

    bytes = (Byte *)e + sizeof(ErrorString);
    if (n > 0)
        memcpy(bytes, text.p, n);

    e->text.p = bytes;
    e->text.len = (Int)n;

    return (Error){&error_string_vt, e};
}

/* ------------------------------------------------------------------ message */

Str error_text(Error err) {
    if (err.vt == NULL || err.vt->message == NULL)
        return BURROW_STR_EMPTY;
    return err.vt->message(err.data);
}

/* ------------------------------------------------------------------- unwrap */

/* Go's errors.Unwrap is defined over Unwrap() error only. An error that
 * implements the tree form is not unwrappable by this function and returns nil,
 * and the same is true here, because errors.Is and errors.As are where the tree
 * is walked. */
Error errors_unwrap(Error err) {
    if (err.vt == NULL || err.vt->unwrap == NULL)
        return BURROW_NO_ERROR;
    return err.vt->unwrap(err.data);
}

/* ----------------------------------------------------------------------- is
 *
 * Both walks below ask about the two wrap forms in the order Go's type switch
 * asks: Unwrap() error first, Unwrap() []error second. A Go type can only ever
 * satisfy one of them, because it has one method of that name. Here they are
 * two vtable slots and a caller can fill in both, which is a mistake with no Go
 * equivalent and therefore no Go behaviour to be faithful to, so the chain form
 * wins. That also keeps errors_unwrap and errors_is answering the same question
 * the same way instead of needing a rule of their own.
 *
 * The null check on the vtable is written out in the loop rather than folded
 * into a helper. It was in a helper, and the static analyser lost track of it
 * once the recursion got deep enough to stop inlining, which produced a null
 * dereference report on a path that cannot happen. A check the analyser can see
 * is worth more than a check that reads slightly better. */

static bool error_identical(Error a, Error b) {
    return a.vt == b.vt && a.data == b.data;
}

bool errors_is(Error err, Error target) {
    /* Go's Is returns err == target when target is nil, which for nil err is
     * true and for anything else is false. Identity gets that right already,
     * and the loop below would too, but saying it here keeps the walk from
     * asking a custom Is slot about a nil target. */
    if (target.vt == NULL)
        return err.vt == NULL;

    for (;;) {
        if (error_identical(err, target))
            return true;

        if (err.vt == NULL)
            return false;

        if (err.vt->is != NULL && err.vt->is(err.data, target))
            return true;

        if (err.vt->unwrap != NULL) {
            err = err.vt->unwrap(err.data);
            continue;
        }

        if (err.vt->unwrap_multi != NULL) {
            Slice kids = err.vt->unwrap_multi(err.data);
            Int i;
            for (i = 0; i < kids.len; i++) {
                if (errors_is(BURROW_AT(Error, kids, i), target))
                    return true;
            }
            return false;
        }

        return false;
    }
}

/* ----------------------------------------------------------------------- as */

const void *errors_as(Error err, const Type *target) {
    if (target == NULL)
        return NULL;

    for (;;) {
        if (err.vt == NULL)
            return NULL;

        if (err.vt->self_type == target)
            return err.data;

        if (err.vt->as != NULL) {
            const void *got = err.vt->as(err.data, target);
            if (got != NULL)
                return got;
        }

        if (err.vt->unwrap != NULL) {
            err = err.vt->unwrap(err.data);
            continue;
        }

        if (err.vt->unwrap_multi != NULL) {
            Slice kids = err.vt->unwrap_multi(err.data);
            Int i;
            for (i = 0; i < kids.len; i++) {
                const void *got = errors_as(BURROW_AT(Error, kids, i), target);
                if (got != NULL)
                    return got;
            }
            return NULL;
        }

        return NULL;
    }
}

/* --------------------------------------------------------------------- join */

/* The children and the joined message, built at construction time. Go builds
 * the message in Error() every time it is called; doing it here means printing
 * an error cannot allocate and therefore cannot fail, which is the property
 * that matters more on an error path than the allocation Go saves when nobody
 * prints. */
typedef struct ErrorJoin {
    Slice errs; /* of Error, owned, borrowed by unwrap_multi */
    Str text;
} ErrorJoin;

static Str error_join_message(const void *self) {
    return ((const ErrorJoin *)self)->text;
}

static Slice error_join_unwrap_multi(const void *self) {
    return ((const ErrorJoin *)self)->errs;
}

static Error join_wrap(Alloc *a, Slice kids);

static Error error_join_clone(const void *self, Alloc *a) {
    const ErrorJoin *j = (const ErrorJoin *)self;
    Slice kids = slice_make(a, TYPE_ERROR, j->errs.len, j->errs.len);
    if (kids.p == NULL)
        return burrow_err_out_of_memory;
    for (Int i = 0; i < j->errs.len; i++)
        BURROW_AT(Error, kids, i) = error_retain(a, BURROW_AT(Error, j->errs, i));
    return join_wrap(a, kids);
}

static const ErrorVT error_join_vt = {
    NULL, error_join_message, NULL, error_join_unwrap_multi, NULL,
    NULL, error_join_clone,
};

/* Takes the children already filtered and already in a Slice, and wraps them.
 *
 * It takes the Slice rather than an array so that neither caller needs a
 * temporary: an intermediate buffer here would be allocated from the caller's
 * allocator and never handed back, which an arena does not care about and heap
 * does. The Slice becomes part of the result, so nothing is wasted.
 *
 * Go's joinError.Error joins with a single newline and no trailing one, so two
 * errors produce "first\nsecond". */
static Error join_wrap(Alloc *a, Slice kids) {
    ErrorJoin *j;
    Byte *text;
    size_t total = 0;
    size_t off = 0;
    Int i;

    if (kids.len <= 0)
        return BURROW_NO_ERROR;

    for (i = 0; i < kids.len; i++) {
        Str m = error_text(BURROW_AT(Error, kids, i));
        if (i > 0)
            total += 1;
        if (m.len > 0)
            total += (size_t)m.len;
    }

    j = (ErrorJoin *)mem_alloc(a, sizeof(ErrorJoin) + total, _Alignof(ErrorJoin));
    if (j == NULL)
        return burrow_err_out_of_memory;

    text = (Byte *)j + sizeof(ErrorJoin);
    for (i = 0; i < kids.len; i++) {
        Str m = error_text(BURROW_AT(Error, kids, i));
        if (i > 0)
            text[off++] = (Byte)'\n';
        if (m.len > 0) {
            memcpy(text + off, m.p, (size_t)m.len);
            off += (size_t)m.len;
        }
    }

    j->errs = kids;
    j->text.p = text;
    j->text.len = (Int)total;

    return (Error){&error_join_vt, j};
}

/* Go's Join counts the non nil errors first, returns nil if there are none, and
 * copies only those into the result. The filtering is not a nicety: a tree with
 * a nil leaf in it would make errors_is walk into an error that is not there. */
Error errors_join(Alloc *a, Slice errs) {
    Slice kids;
    Int n = 0;
    Int i;

    for (i = 0; i < errs.len; i++) {
        if (BURROW_FAILED(BURROW_AT(Error, errs, i)))
            n++;
    }

    if (n == 0)
        return BURROW_NO_ERROR;

    kids = slice_make(a, TYPE_ERROR, n, n);
    if (kids.p == NULL)
        return burrow_err_out_of_memory;

    n = 0;
    for (i = 0; i < errs.len; i++) {
        Error e = BURROW_AT(Error, errs, i);
        if (BURROW_FAILED(e))
            BURROW_AT(Error, kids, n++) = e;
    }

    return join_wrap(a, kids);
}

/* Two passes over the arguments, which means two va_start calls. That is legal
 * C and it is cheaper than a temporary: the first pass only counts, so the
 * Slice comes out exactly the right size and the result owns the only
 * allocation either pass made. */
Error errors_join_v(Alloc *a, int n, ...) {
    va_list ap;
    Slice kids;
    int i;
    Int kn = 0;

    if (n <= 0)
        return BURROW_NO_ERROR;

    va_start(ap, n);
    for (i = 0; i < n; i++) {
        if (BURROW_FAILED(va_arg(ap, Error)))
            kn++;
    }
    va_end(ap);

    if (kn == 0)
        return BURROW_NO_ERROR;

    kids = slice_make(a, TYPE_ERROR, kn, kn);
    if (kids.p == NULL)
        return burrow_err_out_of_memory;

    kn = 0;
    va_start(ap, n);
    for (i = 0; i < n; i++) {
        Error e = va_arg(ap, Error);
        if (BURROW_FAILED(e))
            BURROW_AT(Error, kids, kn++) = e;
    }
    va_end(ap);

    return join_wrap(a, kids);
}

/* ------------------------------------------------------------------- retain */

/* What error_retain makes of an error that has no clone slot: its message, and
 * a retained copy of whatever it wrapped, in whichever of the two forms it
 * wrapped it. */
typedef struct ErrorRetained {
    Str text;
    Error inner;
    Slice kids; /* of Error */
} ErrorRetained;

static Str retained_message(const void *self) {
    return ((const ErrorRetained *)self)->text;
}

static Error retained_unwrap(const void *self) {
    return ((const ErrorRetained *)self)->inner;
}

static Slice retained_unwrap_multi(const void *self) {
    return ((const ErrorRetained *)self)->kids;
}

static const ErrorVT retained_vt = {
    NULL, retained_message, retained_unwrap, NULL, NULL, NULL, NULL,
};

static const ErrorVT retained_multi_vt = {
    NULL, retained_message, NULL, retained_unwrap_multi, NULL, NULL, NULL,
};

Error error_retain(Alloc *a, Error err) {
    if (BURROW_OK(err) || err.vt == &burrow_sentinel_error_vt)
        return err;
    if (err.vt->clone != NULL)
        return err.vt->clone(err.data, a);

    Str text = error_text(err);
    size_t n = text.len > 0 ? (size_t)text.len : 0;
    ErrorRetained *r = (ErrorRetained *)mem_alloc(a, sizeof(ErrorRetained) + n,
                                                  _Alignof(ErrorRetained));
    if (r == NULL)
        return burrow_err_out_of_memory;
    Byte *bytes = (Byte *)r + sizeof(ErrorRetained);
    if (n > 0)
        memcpy(bytes, text.p, n);
    r->text.p = bytes;
    r->text.len = (Int)n;

    /* The same order errors_is asks in, so a type that sets both slots, which
     * the header says not to do, is retained as the chain that errors_is
     * would have walked. */
    if (err.vt->unwrap != NULL) {
        r->inner = error_retain(a, err.vt->unwrap(err.data));
        return (Error){&retained_vt, r};
    }
    if (err.vt->unwrap_multi != NULL) {
        Slice kids = err.vt->unwrap_multi(err.data);
        r->kids = slice_make(a, TYPE_ERROR, kids.len, kids.len);
        if (kids.len > 0 && r->kids.p == NULL)
            return burrow_err_out_of_memory;
        for (Int i = 0; i < kids.len; i++)
            BURROW_AT(Error, r->kids, i) = error_retain(a, BURROW_AT(Error, kids, i));
        return (Error){&retained_multi_vt, r};
    }
    return (Error){&retained_vt, r};
}
