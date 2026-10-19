/* go/types: Instantiate and the checks it makes, Implements, Satisfies and
 * AssertableTo, and the two error types the API hands back.
 *
 * Everything here runs the way Go runs it with a nil *Checker, which is how
 * the exported functions call it: types are written with their package paths,
 * and nothing is reported anywhere but in the result.
 *
 * Derived from Go's src/go/types/instantiate.go, api_predicates.go and api.go.
 * Go source: go1.27.1.
 *
 * Copyright 2021 The Go Authors. All rights reserved.
 * Copyright 2026 The burrow Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style licence that can be found
 * in the LICENSE file. */

#include "burrow/go/types.h"

#include "burrow/core.h"
#include "burrow/error.h"
#include "burrow/fmt.h"
#include "burrow/go/token.h"
#include "burrow/mem.h"
#include "burrow/panic.h"
#include "burrow/slice.h"
#include "burrow/type.h"

#include "types_check.h"
#include "types_internal.h"

#include <stdbool.h>
#include <stdint.h>
#include <string.h>

/* ------------------------------------------------------------------ errors */

static Byte *gt_put(Byte *p, Str s) {
    if (s.len > 0)
        memcpy(p, s.p, (size_t)s.len);
    return p + s.len;
}

/* The TypesError first, so errors_as hands back a pointer to it, and the
 * message after, built once because the message slot cannot allocate. */
typedef struct GtErrorBox {
    TypesError e;
    Str message;
} GtErrorBox;

static Str gt_error_message(const void *self) {
    return ((const GtErrorBox *)self)->message;
}

static const Type gt_error_desc = {
    {(const Byte *)"Error", 5},
    {(const Byte *)"go/types", 8},
    KIND_STRUCT,
    (uint32_t)sizeof(TypesError),
    (uint16_t)_Alignof(TypesError),
    0,
    0,
    NULL,
    NULL,
    NULL,
    NULL,
    0,
    0x67746572U, /* "gter" */
    NULL,
};

const Type *const TYPE_TYPES_ERROR = &gt_error_desc;

static Error gt_error_clone(const void *self, Alloc *a);

static const ErrorVT gt_error_vt = {
    &gt_error_desc, gt_error_message, NULL, NULL, NULL, NULL, gt_error_clone,
};

/* The position part of the text: fset's position for pos, or "-" for a NULL
 * fset, which is what Go's zero Position prints as. */
static Str gt_error_where(const TypesError *e, Alloc *a) {
    TokenPosition pos = {0};
    if (e->fset != NULL)
        pos = token_file_set_position(e->fset, e->pos);
    return token_position_string(pos, a);
}

Str types_error_error(const TypesError *e, Alloc *a) {
    Str where = gt_error_where(e, a);
    return fmt_sprintf_v(a, "%s: %s", where, e->msg);
}

/* One allocation: the box, then msg and the message. */
Error types_error_as_error(const TypesError *e, Alloc *a) {
    Str where = gt_error_where(e, a);
    Int mlen = where.len + 2 + e->msg.len;
    size_t size = sizeof(GtErrorBox) + (size_t)e->msg.len + (size_t)mlen;
    GtErrorBox *b = (GtErrorBox *)mem_alloc_nozero(a, size, _Alignof(GtErrorBox));
    if (b == NULL)
        return burrow_err_out_of_memory;
    Byte *p = (Byte *)(b + 1);
    b->e = *e;
    b->e.msg = str_from_bytes(p, e->msg.len);
    p = gt_put(p, e->msg);
    b->message = str_from_bytes(p, mlen);
    p = gt_put(p, where);
    *p++ = ':';
    *p++ = ' ';
    gt_put(p, e->msg);
    return (Error){&gt_error_vt, b};
}

static Error gt_error_clone(const void *self, Alloc *a) {
    return types_error_as_error((const TypesError *)self, a);
}

/* The text of an ArgumentError is the text of the error it wraps, so there is
 * nothing to build and the box is the struct. */
static Str gt_arg_error_message(const void *self) {
    return error_text(((const TypesArgumentError *)self)->err);
}

static Error gt_arg_error_unwrap(const void *self) {
    return ((const TypesArgumentError *)self)->err;
}

static const Type gt_arg_error_desc = {
    {(const Byte *)"ArgumentError", 13},
    {(const Byte *)"go/types", 8},
    KIND_STRUCT,
    (uint32_t)sizeof(TypesArgumentError),
    (uint16_t)_Alignof(TypesArgumentError),
    0,
    0,
    NULL,
    NULL,
    NULL,
    NULL,
    0,
    0x67746165U, /* "gtae" */
    NULL,
};

const Type *const TYPE_TYPES_ARGUMENT_ERROR = &gt_arg_error_desc;

static Error gt_arg_error_clone(const void *self, Alloc *a);

static const ErrorVT gt_arg_error_vt = {
    &gt_arg_error_desc, gt_arg_error_message, gt_arg_error_unwrap, NULL, NULL, NULL,
    gt_arg_error_clone,
};

Str types_argument_error_error(const TypesArgumentError *e, Alloc *a) {
    return str_clone(a, error_text(e->err));
}

Error types_argument_error_unwrap(const TypesArgumentError *e) {
    return e->err;
}

Error types_argument_error_as_error(const TypesArgumentError *e, Alloc *a) {
    TypesArgumentError *b = (TypesArgumentError *)mem_alloc_nozero(
        a, sizeof *b, _Alignof(TypesArgumentError));
    if (b == NULL)
        return burrow_err_out_of_memory;
    *b = *e;
    return (Error){&gt_arg_error_vt, b};
}

static Error gt_arg_error_clone(const void *self, Alloc *a) {
    const TypesArgumentError *e = (const TypesArgumentError *)self;
    TypesArgumentError c = {e->index, error_retain(a, e->err)};
    return types_argument_error_as_error(&c, a);
}

/* -------------------------------------------------------------- implements */

static Str gt_tstr(Alloc *a, TypesQualifier qf, TypesType t) {
    return types_type_string(a, t, qf);
}

/* mentions: whether T mentions typ in an embedded element or term, whether or
 * not typ is in T's type set. It is only there for better error messages. */
static bool gt_mentions(TypesType T, TypesType typ) {
    switch (T == NULL ? (Int)TYPES_KIND_INVALID : T->kind) {
    case TYPES_KIND_INTERFACE: {
        TypesInterface *t = GT_KNOWN(TypesInterface, T);
        for (Int i = 0; i < t->embeddeds.len; i++) {
            if (gt_mentions(GT_AT(t->embeddeds, i, TypesType), typ))
                return true;
        }
        break;
    }
    case TYPES_KIND_UNION: {
        TypesUnion *u = GT_KNOWN(TypesUnion, T);
        for (Int i = 0; i < u->terms.len; i++) {
            if (gt_mentions(GT_AT(u->terms, i, TypesTerm *)->typ, typ))
                return true;
        }
        break;
    }
    default:
        if (types_identical(T, typ))
            return true;
    }
    return false;
}

/* What the walk over T's terms needs: V, and the first term type that V is in
 * with a ~ in front of it, for the error message. */
typedef struct GtExcludes {
    TypesType V;
    TypesType alt;
} GtExcludes;

/* Whether t leaves V out, remembering alt on the way. */
static bool gt_term_excludes(void *env, TypesTerm *t) {
    GtExcludes *x = env;
    if (burrow__types_term_includes(t, x->V))
        return false;
    /* If V is not in t.typ but is in ~t.typ, remember the type so it can be
     * suggested as an alternative in the error message. */
    if (x->alt == NULL && !t->tilde &&
        types_identical(t->typ, types_type_underlying(t->typ))) {
        TypesTerm tt = *t;
        tt.tilde = true;
        if (burrow__types_term_includes(&tt, x->V))
            x->alt = t->typ;
    }
    return true;
}

/* checkComparability, the part of implements that runs last. */
static bool gt_check_comparability(TypesChecker *check, Alloc *a, TypesQualifier qf,
                                   TypesType V, TypesInterface *Ti, const char *verb,
                                   bool constraint, Str *cause) {
    if (!types_interface_is_comparable(Ti))
        return true;
    /* If T is comparable, V must be comparable. If V is strictly comparable,
     * we're done. */
    if (burrow__types_comparable_type(V, false, NULL))
        return true;
    /* For constraint satisfaction, use dynamic (spec) comparability so that
     * ordinary, non-type parameter interfaces implement comparable. */
    if (constraint && burrow__types_comparable_type(V, true, NULL)) {
        /* V is comparable if we are at Go 1.20 or higher. */
        if (check == NULL || burrow__tc_allow_version(check, burrow__tc_go1_20))
            return true;
        if (cause != NULL) {
            Str vs = gt_tstr(a, qf, V);
            *cause = fmt_sprintf_v(a, "%s to %s comparable requires go1.20 or later",
                                   vs, verb);
        }
        return false;
    }
    if (cause != NULL) {
        Str vs = gt_tstr(a, qf, V);
        *cause = fmt_sprintf_v(a, "%s does not %s comparable", vs, verb);
    }
    return false;
}

bool burrow__types_implements(TypesChecker *check, Alloc *a, TypesType V, TypesType T,
                              bool constraint, Str *cause) {
    TypesType Vu = types_type_underlying(V);
    TypesType Tu = types_type_underlying(T);
    if (!burrow__types_is_valid(Vu) || !burrow__types_is_valid(Tu))
        return true; /* avoid follow-on errors */
    TypesPointer *p = types_as_pointer(Vu);
    if (p != NULL && !burrow__types_is_valid(types_type_underlying(p->base)))
        return true; /* avoid follow-on errors (see go.dev/issue/49541) */

    const char *verb = constraint ? "satisfy" : "implement";
    if (check != NULL)
        a = check->a;
    TypesQualifier qf = burrow__tc_qualifier(check);

    TypesInterface *Ti = types_as_interface(Tu);
    if (Ti == NULL) {
        if (cause != NULL) {
            Str vs = gt_tstr(a, qf, V), ts = gt_tstr(a, qf, T);
            Str detail = burrow__types_is_interface_ptr(Tu)
                             ? burrow__types_interface_ptr_error(a, qf, T)
                             : fmt_sprintf_v(a, "%s is not an interface", ts);
            *cause = fmt_sprintf_v(a, "%s does not %s %s (%s)", vs, verb, ts, detail);
        }
        return false;
    }

    /* Every type satisfies the empty interface. */
    if (types_interface_empty(Ti))
        return true;

    /* An interface V with an empty type set satisfies any interface. (The
     * empty set is a subset of any set.) */
    TypesInterface *Vi = types_as_interface(Vu);
    if (Vi != NULL && burrow__types_type_set_is_empty(burrow__types_iface_type_set(Vi)))
        return true;

    /* No type with non-empty type set satisfies the empty type set. */
    GtTypeSet *Ts = burrow__types_iface_type_set(Ti);
    if (burrow__types_type_set_is_empty(Ts)) {
        if (cause != NULL) {
            Str ts = gt_tstr(a, qf, T);
            *cause = fmt_sprintf_v(a, "cannot %s %s (empty type set)", verb, ts);
        }
        return false;
    }

    /* V must implement T's methods, if any. */
    if (!burrow__types_has_all_methods(check, a, V, T, true, GT_IDENTICAL_FN, cause)) {
        if (cause != NULL) {
            Str vs = gt_tstr(a, qf, V), ts = gt_tstr(a, qf, T);
            *cause = fmt_sprintf_v(a, "%s does not %s %s %s", vs, verb, ts, *cause);
        }
        return false;
    }

    /* V must also be in the set of types of T, if any. */
    if (!burrow__types_type_set_has_terms(Ts))
        return gt_check_comparability(check, a, qf, V, Ti, verb, constraint, cause);

    /* If V is itself an interface, each of its possible types must be in the
     * set of T types. */
    if (Vi != NULL) {
        if (!burrow__types_type_set_subset_of(burrow__types_iface_type_set(Vi), Ts)) {
            if (cause != NULL) {
                Str vs = gt_tstr(a, qf, V), ts = gt_tstr(a, qf, T);
                *cause = fmt_sprintf_v(a, "%s does not %s %s", vs, verb, ts);
            }
            return false;
        }
        return gt_check_comparability(check, a, qf, V, Ti, verb, constraint, cause);
    }

    /* Otherwise, V's type must be included in the iface type set. */
    GtExcludes x = {V, NULL};
    if (burrow__types_type_set_is(Ts, BURROW_FN(GtTermFunc, gt_term_excludes, &x))) {
        if (cause != NULL) {
            Str vs = gt_tstr(a, qf, V), ts = gt_tstr(a, qf, T);
            Str detail;
            if (x.alt != NULL) {
                Str alts = gt_tstr(a, qf, x.alt);
                detail = fmt_sprintf_v(a, "possibly missing ~ for %s in %s", alts, ts);
            } else if (gt_mentions(types_type(Ti), V)) {
                detail = fmt_sprintf_v(
                    a, "%s mentions %s, but %s is not in the type set of %s", ts, vs,
                    vs, ts);
            } else {
                Str terms = burrow__types_termlist_string(a, Ts->terms);
                detail = fmt_sprintf_v(a, "%s missing in %s", vs, terms);
            }
            *cause = fmt_sprintf_v(a, "%s does not %s %s (%s)", vs, verb, ts, detail);
        }
        return false;
    }

    return gt_check_comparability(check, a, qf, V, Ti, verb, constraint, cause);
}

/* ------------------------------------------------------------- instantiate */

/* verify: the index of the first type argument that does not satisfy its
 * constraint, with *cause set to why, or -1. check may be NULL; pos is used
 * only if check is not. */
static Int gt_verify(TypesChecker *check, Alloc *a, TokenPos pos, Slice tparams,
                     Slice targs, TypesContext *ctxt, Str *cause) {
    Map *smap = burrow__types_make_subst_map(a, tparams, targs);
    for (Int i = 0; i < tparams.len; i++) {
        TypesTypeParam *tpar = GT_AT(tparams, i, TypesTypeParam *);
        /* Ensure that we have a (possibly implicit) interface as type bound
         * (go.dev/issue/51048). */
        (void)burrow__types_type_param_iface(tpar);
        /* The bound is parameterized with the same type parameters as the
         * instantiated type, so it is instantiated with the same type
         * arguments before it is checked against. */
        TypesType bound =
            burrow__types_subst(check, a, pos, tpar->bound, smap, NULL, ctxt);
        if (!burrow__types_implements(check, a, GT_AT(targs, i, TypesType), bound, true,
                                      cause))
            return i;
    }
    return -1;
}

Int burrow__tc_verify(TypesChecker *check, TokenPos pos, Slice tparams, Slice targs,
                      TypesContext *ctxt, Error *err) {
    Str cause = BURROW_STR_EMPTY;
    Int i = gt_verify(check, check->a, pos, tparams, targs, ctxt, &cause);
    if (i >= 0)
        *err = errors_new(check->a, cause);
    return i;
}

/* The type parameters of orig, which must be a named type, an alias or a
 * signature. *ok is false for anything else. */
static TypesTypeParamList *gt_generic_tparams(TypesType orig, bool *ok) {
    *ok = true;
    switch (orig == NULL ? (Int)TYPES_KIND_INVALID : orig->kind) {
    case TYPES_KIND_NAMED:
        return types_named_type_params(GT_KNOWN(TypesNamed, orig));
    case TYPES_KIND_ALIAS:
        return types_alias_type_params(GT_KNOWN(TypesAlias, orig));
    case TYPES_KIND_SIGNATURE:
        return types_signature_type_params(GT_KNOWN(TypesSignature, orig));
    default:
        *ok = false;
        return NULL;
    }
}

TypesType types_instantiate(Alloc *a, TypesContext *ctxt, TypesType orig, Slice targs,
                            bool validate, Error *err) {
    if (err != NULL)
        *err = BURROW_NO_ERROR;
    if (ctxt == NULL)
        ctxt = types_new_context(a);
    bool ok;
    TypesTypeParamList *tl = gt_generic_tparams(orig, &ok);
    if (!ok) {
        Str s = gt_tstr(a, TYPES_NO_QUALIFIER, orig);
        panic_str(fmt_sprintf_v(
            a,
            "cannot instantiate non-generic %s: expected *Named, *Alias, or *Signature",
            s));
    }
    if (targs.len == 0) {
        Str s = gt_tstr(a, TYPES_NO_QUALIFIER, orig);
        panic_str(
            fmt_sprintf_v(a, "cannot instantiate %s: empty type argument list", s));
    }

    if (validate) {
        Slice tparams = burrow__types_tparam_list(tl);
        if (tparams.len == 0) {
            if (err != NULL) {
                Str s = gt_tstr(a, TYPES_NO_QUALIFIER, orig);
                *err = errors_new(
                    a, fmt_sprintf_v(a,
                                     "cannot instantiate non-generic %s: has no type "
                                     "parameters",
                                     s));
            }
            return NULL;
        }
        if (targs.len != tparams.len) {
            if (err != NULL) {
                Str s = gt_tstr(a, TYPES_NO_QUALIFIER, orig);
                *err =
                    errors_new(a, fmt_sprintf_v(a,
                                                "cannot instantiate %s: got %d type "
                                                "arguments but have %d type parameters",
                                                s, targs.len, tparams.len));
            }
            return NULL;
        }
        Str cause = BURROW_STR_EMPTY;
        Int i = gt_verify(NULL, a, TOKEN_NO_POS, tparams, targs, ctxt, &cause);
        if (i >= 0) {
            if (err != NULL) {
                TypesArgumentError ae = {i, errors_new(a, cause)};
                *err = types_argument_error_as_error(&ae, a);
            }
            return NULL;
        }
    }

    return burrow__types_instance(NULL, a, TOKEN_NO_POS, orig, targs, NULL, ctxt);
}

/* ---------------------------------------------------------- api predicates */

/* With no cause asked for, implements allocates nothing from a, so these pass
 * NULL. */

bool types_assertable_to(TypesInterface *V, TypesType T) {
    /* Checker.newAssertableTo suppresses errors for invalid types, so they
     * need handling here. */
    if (!burrow__types_is_valid(types_type_underlying(T)))
        return false;
    /* No static check is needed if T is an interface. */
    if (types_is_interface(T))
        return true;
    return burrow__types_implements(NULL, NULL, T, types_type(V), false, NULL);
}

bool types_implements(TypesType V, TypesInterface *T) {
    /* All types (even Typ[Invalid]) implement the empty interface. */
    if (types_interface_empty(T))
        return true;
    /* Checker.implements suppresses errors for invalid types, so they need
     * handling here. */
    if (!burrow__types_is_valid(types_type_underlying(V)))
        return false;
    return burrow__types_implements(NULL, NULL, V, types_type(T), false, NULL);
}

bool types_satisfies(TypesType V, TypesInterface *T) {
    return burrow__types_implements(NULL, NULL, V, types_type(T), true, NULL);
}
