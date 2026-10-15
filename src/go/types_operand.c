/* go/types: AssignableTo and ConvertibleTo.
 *
 * Go answers both by making a non-constant operand of type V and running the
 * checker's own assignment and conversion rules on it, with a nil *Checker
 * and no cause. That is all that is done here, so none of the error text
 * those rules can build is needed, and there is no language version to hold
 * any rule back.
 *
 * Derived from Go's src/go/types/api_predicates.go, operand.go, expr.go and
 * conversions.go. Go source: go1.27.1.
 *
 * Copyright 2012 The Go Authors. All rights reserved.
 * Copyright 2026 The burrow Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style licence that can be found
 * in the LICENSE file. */

#include "burrow/go/types.h"

#include "burrow/core.h"

#include "types_internal.h"

#include <stdbool.h>

static bool gt_assignable_to(TypesType xt, TypesType T);
static bool gt_convertible_to(TypesType xt, TypesType T);

/* ----------------------------------------------------- implicitTypeAndValue */

/* Go's x.isNil() for an operand of mode value. */
static bool gt_is_nil(TypesType xt) {
    return xt == GT_TYP(TYPES_UNTYPED_NIL);
}

static bool gt_implicit_ok(TypesType xt, TypesType target);

static bool gt_under_implicit_ok(void *env, TypesType t, TypesType u) {
    (void)t;
    return u != NULL && gt_implicit_ok(*(TypesType *)env, u);
}

/* Go's implicitTypeAndValue for a non-constant operand of type xt, reporting
 * whether the type it finds is not nil. A non-constant untyped value is the
 * result of a comparison, the left side of a shift, or nil. */
static bool gt_implicit_ok(TypesType xt, TypesType target) {
    if (burrow__types_is_typed(xt) || !burrow__types_is_valid(target))
        return true;
    /* xt is untyped */

    if (!burrow__types_is_typed(target)) {
        /* both xt and target are untyped */
        return burrow__types_max_type(xt, target) != NULL;
    }

    TypesType u = types_type_underlying(target);
    switch (u == NULL ? (Int)TYPES_KIND_INVALID : u->kind) {
    case TYPES_KIND_BASIC:
        switch (types_as_basic(xt)->kind) {
        case TYPES_UNTYPED_BOOL:
            return burrow__types_is_basic(target, TYPES_IS_BOOLEAN);
        case TYPES_UNTYPED_INT:
        case TYPES_UNTYPED_RUNE:
        case TYPES_UNTYPED_FLOAT:
        case TYPES_UNTYPED_COMPLEX:
            return burrow__types_is_basic(target, TYPES_IS_NUMERIC);
        case TYPES_UNTYPED_STRING:
            /* Non-constant untyped string values are not permitted by the
             * spec and should not occur during normal type checking, but this
             * path is reachable through AssignableTo. */
            return burrow__types_is_basic(target, TYPES_IS_STRING);
        case TYPES_UNTYPED_NIL:
            /* unsafe.Pointer is a basic type that includes nil. */
            return burrow__types_has_nil(target);
        default:
            return false;
        }
    case TYPES_KIND_INTERFACE:
        if (burrow__types_is_type_param(target))
            return burrow__types_all(
                target, BURROW_FN(GtTypePairFunc, gt_under_implicit_ok, &xt));
        /* Values must have concrete dynamic types, but nil is kept untyped. */
        if (gt_is_nil(xt))
            return true;
        /* cannot assign untyped values to non-empty interfaces */
        return types_interface_empty(types_as_interface(u));
    case TYPES_KIND_POINTER:
    case TYPES_KIND_SIGNATURE:
    case TYPES_KIND_SLICE:
    case TYPES_KIND_MAP:
    case TYPES_KIND_CHAN:
        return gt_is_nil(xt);
    default:
        return false;
    }
}

/* -------------------------------------------------------------- assignableTo */

typedef struct GtOperandEnv {
    TypesType x; /* the operand's type */
    TypesType T; /* the target */
} GtOperandEnv;

/* each specific type in T's type set takes the untyped x */
static bool gt_term_implicit_ok(void *env, TypesTerm *t) {
    return t != NULL && gt_implicit_ok(((GtOperandEnv *)env)->x, t->typ);
}

/* x is assignable to the term */
static bool gt_term_assignable_from(void *env, TypesTerm *t) {
    return t != NULL && gt_assignable_to(((GtOperandEnv *)env)->x, t->typ);
}

/* the term is assignable to T */
static bool gt_term_assignable_to(void *env, TypesTerm *t) {
    return t != NULL && gt_assignable_to(t->typ, ((GtOperandEnv *)env)->T);
}

static GtTypeSet *gt_tparam_set(TypesTypeParam *p) {
    return burrow__types_iface_type_set(burrow__types_type_param_iface(p));
}

/* Go's x.assignableTo for a non-constant operand of type xt. Where Go sets
 * the operand's type to each term of V's type set, this calls itself with the
 * term's type, which is the same thing. */
static bool gt_assignable_to(TypesType xt, TypesType T) {
    if (!burrow__types_is_valid(T))
        return true; /* avoid spurious errors */

    TypesType V = types_unalias(xt);
    T = types_unalias(T);

    /* x's type is identical to T */
    if (types_identical(V, T))
        return true;

    TypesType Vu = types_type_underlying(V);
    TypesType Tu = types_type_underlying(T);
    TypesTypeParam *Vp = types_as_type_param(V);
    TypesTypeParam *Tp = types_as_type_param(T);
    GtOperandEnv env = {xt, T};

    /* x is an untyped value representable by a value of type T. */
    if (!burrow__types_is_typed(Vu)) {
        if (Tp != NULL)
            /* T is a type parameter: x is assignable to T if it is
             * representable by each specific type in the type set of T. */
            return burrow__types_type_set_is(
                gt_tparam_set(Tp), BURROW_FN(GtTermFunc, gt_term_implicit_ok, &env));
        return gt_implicit_ok(xt, T);
    }
    /* Vu is typed */

    /* x's type V and T have identical underlying types and at least one of V
     * or T is not a named type and neither V nor T is a type parameter. */
    if (types_identical(Vu, Tu) &&
        (!burrow__types_has_name(V) || !burrow__types_has_name(T)) && Vp == NULL &&
        Tp == NULL)
        return true;

    /* T is an interface type, but not a type parameter, and V implements T.
     * Also handle the case where T is a pointer to an interface. */
    if ((types_as_interface(Tu) != NULL && Tp == NULL) ||
        burrow__types_is_interface_ptr(Tu)) {
        if (burrow__types_implements(NULL, V, T, false, NULL))
            return true;
        /* V doesn't implement T but V may still be assignable to T if V is a
         * type parameter. */
        if (Vp == NULL)
            return false;
    }

    /* If V is an interface, check if a missing type assertion is the
     * problem. */
    if (types_as_interface(Vu) != NULL && Vp == NULL) {
        if (burrow__types_implements(NULL, T, V, false, NULL))
            return false; /* T implements V, so it needs a type assertion */
    }

    /* x is a bidirectional channel value, T is a channel type, x's type V and
     * T have identical element types, and at least one of V or T is not a
     * named type. */
    TypesChan *Vc = types_as_chan(Vu);
    if (Vc != NULL && types_chan_dir(Vc) == TYPES_SEND_RECV) {
        TypesChan *Tc = types_as_chan(Tu);
        if (Tc != NULL && types_identical(types_chan_elem(Vc), types_chan_elem(Tc)))
            return !burrow__types_has_name(V) || !burrow__types_has_name(T);
    }

    /* optimization: if we don't have type parameters, we're done */
    if (Vp == NULL && Tp == NULL)
        return false;

    /* x's type V is not a named type and T is a type parameter, and x is
     * assignable to each specific type in T's type set. */
    if (!burrow__types_has_name(V) && Tp != NULL)
        return burrow__types_type_set_is(
            gt_tparam_set(Tp), BURROW_FN(GtTermFunc, gt_term_assignable_from, &env));

    /* x's type V is a type parameter and T is not a named type, and values x'
     * of each specific type in V's type set are assignable to T. */
    if (Vp != NULL && !burrow__types_has_name(T))
        return burrow__types_type_set_is(
            gt_tparam_set(Vp), BURROW_FN(GtTermFunc, gt_term_assignable_to, &env));

    return false;
}

/* ------------------------------------------------------------- convertibleTo */

static bool gt_is_kind(TypesType t, TypesBasicKind kind) {
    TypesBasic *b = types_as_basic(types_type_underlying(t));
    return b != NULL && b->kind == kind;
}

static bool gt_operand_is_pointer(TypesType t) {
    return types_as_pointer(types_type_underlying(t)) != NULL;
}

static bool gt_is_bytes_or_runes(TypesType t) {
    TypesSlice *s = types_as_slice(types_type_underlying(t));
    if (s == NULL)
        return false;
    TypesBasic *b = types_as_basic(types_type_underlying(types_slice_elem(s)));
    return b != NULL && (b->kind == TYPES_BYTE || b->kind == TYPES_RUNE);
}

/* the term converts to T */
static bool gt_term_convertible_to(void *env, TypesTerm *t) {
    return t != NULL && gt_convertible_to(t->typ, ((GtOperandEnv *)env)->T);
}

/* x converts to the term */
static bool gt_term_convertible_from(void *env, TypesTerm *t) {
    return t != NULL && gt_convertible_to(((GtOperandEnv *)env)->x, t->typ);
}

typedef struct GtBothEnv {
    TypesTypeParam *Tp;
} GtBothEnv;

/* each term of Vp converts to each term of Tp */
static bool gt_term_convertible_to_all(void *env, TypesTerm *t) {
    if (t == NULL)
        return false;
    GtOperandEnv inner = {t->typ, NULL};
    return burrow__types_type_set_is(
        gt_tparam_set(((GtBothEnv *)env)->Tp),
        BURROW_FN(GtTermFunc, gt_term_convertible_from, &inner));
}

/* Go's x.convertibleTo for a non-constant operand of type xt. */
static bool gt_convertible_to(TypesType xt, TypesType T) {
    /* "x is assignable to T" */
    if (gt_assignable_to(xt, T))
        return true;

    TypesType V = types_unalias(xt);
    T = types_unalias(T);
    TypesType Vu = types_type_underlying(V);
    TypesType Tu = types_type_underlying(T);
    TypesTypeParam *Vp = types_as_type_param(V);
    TypesTypeParam *Tp = types_as_type_param(T);

    /* "V and T have identical underlying types if tags are ignored and V and
     * T are not type parameters" */
    if (types_identical_ignore_tags(Vu, Tu) && Vp == NULL && Tp == NULL)
        return true;

    /* "V and T are unnamed pointer types and their pointer base types have
     * identical underlying types if tags are ignored and their pointer base
     * types are not type parameters" */
    TypesPointer *Vptr = types_as_pointer(V);
    TypesPointer *Tptr = types_as_pointer(T);
    if (Vptr != NULL && Tptr != NULL) {
        TypesType vb = types_pointer_elem(Vptr), tb = types_pointer_elem(Tptr);
        if (types_identical_ignore_tags(types_type_underlying(vb),
                                        types_type_underlying(tb)) &&
            !burrow__types_is_type_param(vb) && !burrow__types_is_type_param(tb))
            return true;
    }

    /* "V and T are both integer or floating point types" */
    if (burrow__types_is_basic(Vu, TYPES_IS_INTEGER | TYPES_IS_FLOAT) &&
        burrow__types_is_basic(Tu, TYPES_IS_INTEGER | TYPES_IS_FLOAT))
        return true;

    /* "V and T are both complex types" */
    if (burrow__types_is_basic(Vu, TYPES_IS_COMPLEX) &&
        burrow__types_is_basic(Tu, TYPES_IS_COMPLEX))
        return true;

    /* "V is an integer or a slice of bytes or runes and T is a string type" */
    if ((burrow__types_is_basic(Vu, TYPES_IS_INTEGER) || gt_is_bytes_or_runes(Vu)) &&
        burrow__types_is_basic(Tu, TYPES_IS_STRING))
        return true;

    /* "V is a string and T is a slice of bytes or runes" */
    if (burrow__types_is_basic(Vu, TYPES_IS_STRING) && gt_is_bytes_or_runes(Tu))
        return true;

    /* package unsafe: "any pointer or value of underlying type uintptr can be
     * converted into a unsafe.Pointer" */
    if ((gt_operand_is_pointer(Vu) || gt_is_kind(Vu, TYPES_UINTPTR)) &&
        gt_is_kind(Tu, TYPES_UNSAFE_POINTER))
        return true;
    /* "and vice versa" */
    if (gt_is_kind(Vu, TYPES_UNSAFE_POINTER) &&
        (gt_operand_is_pointer(Tu) || gt_is_kind(Tu, TYPES_UINTPTR)))
        return true;

    /* "V is a slice, T is an array or pointer-to-array type, and the slice and
     * array types have identical element types." */
    TypesSlice *s = types_as_slice(Vu);
    if (s != NULL) {
        TypesArray *arr = types_as_array(Tu);
        TypesPointer *p = types_as_pointer(Tu);
        if (arr != NULL) {
            if (types_identical(types_slice_elem(s), types_array_elem(arr)))
                return true;
        } else if (p != NULL) {
            arr = types_as_array(types_type_underlying(types_pointer_elem(p)));
            if (arr != NULL &&
                types_identical(types_slice_elem(s), types_array_elem(arr)))
                return true;
        }
    }

    /* optimization: if we don't have type parameters, we're done */
    if (Vp == NULL && Tp == NULL)
        return false;

    /* generic cases with specific type terms (generic operands cannot be
     * constants) */
    GtOperandEnv env = {xt, T};
    if (Vp != NULL && Tp != NULL) {
        GtBothEnv both = {Tp};
        return burrow__types_type_set_is(
            gt_tparam_set(Vp),
            BURROW_FN(GtTermFunc, gt_term_convertible_to_all, &both));
    }
    if (Vp != NULL)
        return burrow__types_type_set_is(
            gt_tparam_set(Vp), BURROW_FN(GtTermFunc, gt_term_convertible_to, &env));
    return burrow__types_type_set_is(
        gt_tparam_set(Tp), BURROW_FN(GtTermFunc, gt_term_convertible_from, &env));
}

/* ------------------------------------------------------------------- the API */

bool types_assignable_to(TypesType V, TypesType T) {
    return gt_assignable_to(V, T);
}

bool types_convertible_to(TypesType V, TypesType T) {
    return gt_convertible_to(V, T);
}
