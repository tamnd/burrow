/* go/types: type-checking of conversions.
 *
 * Derived from Go's src/go/types/conversions.go. Go source: go1.27.1.
 *
 * Copyright 2012 The Go Authors. All rights reserved.
 * Copyright 2026 The burrow Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style licence that can be found
 * in the LICENSE file. */

#include "burrow/go/types.h"

#include "burrow/core.h"
#include "burrow/go/constant.h"
#include "burrow/slice.h"
#include "burrow/unicode.h"
#include "burrow/utf8.h"

#include "types_check.h"
#include "types_internal.h"

#include <stdbool.h>
#include <stdint.h>

static bool tcv_is_uintptr(TypesType typ) {
    TypesBasic *t = types_as_basic(types_type_underlying(typ));
    return t != NULL && t->kind == TYPES_UINTPTR;
}

static bool tcv_is_unsafe_pointer(TypesType typ) {
    TypesBasic *t = types_as_basic(types_type_underlying(typ));
    return t != NULL && t->kind == TYPES_UNSAFE_POINTER;
}

bool burrow__tc_is_pointer(TypesType typ) {
    return types_as_pointer(types_type_underlying(typ)) != NULL;
}

static bool tcv_is_bytes_or_runes(TypesType typ) {
    TypesSlice *s = types_as_slice(types_type_underlying(typ));
    if (s == NULL)
        return false;
    TypesBasic *t = types_as_basic(types_type_underlying(types_slice_elem(s)));
    return t != NULL && (t->kind == TYPES_BYTE || t->kind == TYPES_RUNE);
}

/* ---------------------------------------------------------------- conversion */

/* The constConvertibleTo closure of conversion: whether the constant x
 * converts to T, with the converted value in *val if val is not NULL. */
static bool tcv_const_convertible_to(TypesChecker *check, const GtOperand *x,
                                     TypesType T, ConstantValue *val) {
    TypesBasic *t = types_as_basic(types_type_underlying(T));
    if (t == NULL)
        return false; /* nothing to do */
    if (burrow__tc_representable_const(check->a, x->val, check, t, val))
        return true;
    if (burrow__types_is_basic(x->typ, TYPES_IS_INTEGER) &&
        burrow__types_is_basic(types_type(t), TYPES_IS_STRING)) {
        Rune codepoint = UNICODE_REPLACEMENT_CHAR;
        bool ok;
        uint64_t i = constant_uint64_val(x->val, &ok);
        if (ok && i <= (uint64_t)UNICODE_MAX_RUNE)
            codepoint = (Rune)i;
        if (val != NULL) {
            Slice b = utf8_append_rune(check->a, slice_nil(TYPE_BYTE), codepoint);
            if (b.p == NULL)
                burrow__types_oom();
            *val = constant_make_string(check->a, str_from_bytes(b.p, b.len));
        }
        return true;
    }
    return false;
}

/* What the underIs walk of a constant conversion to a type parameter closes
 * over. */
typedef struct TcvConstTparam {
    TypesChecker *check;
    const GtOperand *x;
    TypesType T;
    Str *cause;
} TcvConstTparam;

/* x is convertible to T if it is convertible to each specific type in the
 * type set of T. If T's type set is empty, or if it doesn't have specific
 * types, constant x cannot be converted. */
static bool tcv_under_const_convertible(void *env, TypesType t, TypesType u) {
    (void)t;
    TcvConstTparam *c = env;
    TypesChecker *check = c->check;
    const GtOperand *x = c->x;
    /* u is nil if there are no specific type terms */
    if (u == NULL) {
        Str ts = burrow__tc_ts(check, c->T);
        *c->cause = GT_SPRINTF(check, "%s does not contain specific types", ts);
        return false;
    }
    if (burrow__types_is_basic(x->typ, TYPES_IS_STRING) && tcv_is_bytes_or_runes(u))
        return true;
    if (!tcv_const_convertible_to(check, x, u, NULL)) {
        Str us = burrow__tc_ts(check, u), ts = burrow__tc_ts(check, c->T);
        if (burrow__types_is_basic(x->typ, TYPES_IS_INTEGER) &&
            burrow__types_is_basic(u, TYPES_IS_INTEGER)) {
            /* see comment above on constant conversion */
            Str vs = constant_value_string(x->val, check->a);
            *c->cause =
                GT_SPRINTF(check, "constant %s overflows %s (in %s)", vs, us, ts);
        } else {
            Str xs = burrow__tc_xs(check, x);
            *c->cause =
                GT_SPRINTF(check, "cannot convert %s to type %s (in %s)", xs, us, ts);
        }
        return false;
    }
    return true;
}

/* The conversion argument types are final. For untyped values the conversion
 * provides the type, per the spec: "A constant may be given a type explicitly
 * by a constant declaration or conversion,...". */
static void tcv_finalize_untyped(TypesChecker *check, GtOperand *x, TypesType T,
                                 bool const_arg) {
    TypesType final = T;
    /* - For conversions to interfaces, except for untyped nil arguments and
     *   isTypes2, use the argument's default type.
     * - For conversions of untyped constants to non-constant types, also use
     *   the default type (e.g., []byte("foo") should report string not []byte
     *   as type for the constant "foo").
     * - If !isTypes2, keep untyped nil for untyped nil arguments.
     * - For constant integer to string conversions, keep the argument type.
     *
     * This is go/types, so isTypes2 is false. */
    if (burrow__types_is_non_type_param_interface(T) ||
        (const_arg && !burrow__types_is_basic(T, TYPES_IS_CONST_TYPE)) ||
        burrow__tc_op_is_nil(x))
        final = types_default(x->typ); /* default type of untyped nil is untyped nil */
    else if (x->mode == GT_MODE_CONSTANT &&
             burrow__types_is_basic(x->typ, TYPES_IS_INTEGER) &&
             burrow__types_all_basic(T, TYPES_IS_STRING))
        final = x->typ;
    burrow__tc_update_expr_type(check, x->expr, final, true);
}

void burrow__tc_conversion(TypesChecker *check, GtOperand *x, TypesType T) {
    bool const_arg = x->mode == GT_MODE_CONSTANT;

    bool ok = false;
    Str cause = BURROW_STR_EMPTY;
    if (const_arg && burrow__types_is_basic(T, TYPES_IS_CONST_TYPE)) {
        /* constant conversion */
        ok = tcv_const_convertible_to(check, x, T, &x->val);
        /* A conversion from an integer constant to an integer type can only
         * fail if there's overflow. Give a concise error.
         * (go.dev/issue/63563) */
        if (!ok && burrow__types_is_basic(x->typ, TYPES_IS_INTEGER) &&
            burrow__types_is_basic(T, TYPES_IS_INTEGER)) {
            Str vs = constant_value_string(x->val, check->a);
            Str ts = burrow__tc_ts(check, T);
            GT_ERRORF(check, burrow__tc_at_op(x), GT_CODE_INVALID_CONVERSION,
                      "constant %s overflows %s", vs, ts);
            burrow__tc_op_invalidate(x);
            return;
        }
    } else if (const_arg && burrow__types_is_type_param(T)) {
        TcvConstTparam c = {check, x, T, &cause};
        ok = burrow__types_all(
            T, BURROW_FN(GtTypePairFunc, tcv_under_const_convertible, &c));
        x->mode = GT_MODE_VALUE; /* type parameters are not constants */
    } else if (burrow__tc_convertible_to(check, x, T, &cause)) {
        /* non-constant conversion */
        ok = true;
        x->mode = GT_MODE_VALUE;
    }

    if (!ok) {
        Str xs = burrow__tc_xs(check, x), ts = burrow__tc_ts(check, T);
        if (cause.len != 0)
            GT_ERRORF(check, burrow__tc_at_op(x), GT_CODE_INVALID_CONVERSION,
                      "cannot convert %s to type %s: %s", xs, ts, cause);
        else
            GT_ERRORF(check, burrow__tc_at_op(x), GT_CODE_INVALID_CONVERSION,
                      "cannot convert %s to type %s", xs, ts);
        burrow__tc_op_invalidate(x);
        return;
    }

    if (!burrow__types_is_typed(x->typ))
        tcv_finalize_untyped(check, x, T, const_arg);

    x->typ = T;
}

/* ------------------------------------------------------------- convertibleTo */

/* TODO(gri) convertibleTo checks if T(x) is valid. It assumes that the type of
 * x is fully known, but that's not the case for say string(1<<s + 1.0): Here,
 * the type of 1<<s + 1.0 will be UntypedFloat which will lead to the
 * (correct!) refusal of the conversion. But the reported error is essentially
 * "cannot convert untyped float value to string", yet the correct error (per
 * the spec) is that we cannot shift a floating-point value: 1 in 1<<s should
 * be converted to UntypedFloat because of the addition of 1.0. Fixing this is
 * tricky because we'd have to run updateExprType on the argument first.
 * (go.dev/issue/21982.) */

/* What the walks over the type sets in convertibleTo close over. */
typedef struct TcvConvert {
    TypesChecker *check;
    GtOperand x; /* a copy, so the outer x is not clobbered */
    TypesType T;
    TypesType orig_T;
    TypesTypeParam *Vp, *Tp;
    Str *cause;
} TcvConvert;

/* The errorf closure of convertibleTo. */
static void tcv_errorf(TcvConvert *c, Str msg) {
    burrow__tc_wrap_cause(c->check, c->cause, msg);
}

/* x converts to the term's type */
static bool tcv_term_convertible_from(void *env, TypesTerm *t) {
    if (t == NULL)
        return false; /* no specific types */
    TcvConvert *c = env;
    if (burrow__tc_convertible_to(c->check, &c->x, t->typ, c->cause))
        return true;
    TypesChecker *check = c->check;
    if (check != NULL && c->cause != NULL) {
        Str ts = burrow__tc_ts(check, t->typ);
        Str tps = burrow__tc_ts(check, types_type(c->Tp));
        if (c->Vp != NULL) {
            Str vs = burrow__tc_ts(check, c->x.typ);
            Str vps = burrow__tc_ts(check, types_type(c->Vp));
            tcv_errorf(c,
                       GT_SPRINTF(check, "cannot convert %s (in %s) to type %s (in %s)",
                                  vs, vps, ts, tps));
        } else {
            Str xs = burrow__tc_ts(check, c->x.typ);
            tcv_errorf(c, GT_SPRINTF(check, "cannot convert %s to type %s (in %s)", xs,
                                     ts, tps));
        }
    }
    return false;
}

/* each term of Tp takes x, with x's type set to the term's type */
static bool tcv_term_convertible_to_all(void *env, TypesTerm *t) {
    if (t == NULL)
        return false; /* no specific types */
    TcvConvert *c = env;
    c->x.typ = t->typ;
    return burrow__types_type_param_is(
        c->Tp, BURROW_FN(GtTermFunc, tcv_term_convertible_from, c));
}

/* values of the term's type convert to T */
static bool tcv_term_convertible_to(void *env, TypesTerm *t) {
    if (t == NULL)
        return false; /* no specific types */
    TcvConvert *c = env;
    c->x.typ = t->typ;
    if (burrow__tc_convertible_to(c->check, &c->x, c->T, c->cause))
        return true;
    TypesChecker *check = c->check;
    if (check != NULL && c->cause != NULL) {
        Str vs = burrow__tc_ts(check, t->typ);
        Str vps = burrow__tc_ts(check, types_type(c->Vp));
        Str ts = burrow__tc_ts(check, c->orig_T);
        tcv_errorf(
            c, GT_SPRINTF(check, "cannot convert %s (in %s) to type %s", vs, vps, ts));
    }
    return false;
}

/* "V is a slice, T is an array or pointer-to-array type, and the slice and
 * array types have identical element types." *found is set if the rule
 * applies, and the result is whether the language version allows it. */
static bool tcv_slice_to_array(TypesChecker *check, TypesType Vu, TypesType Tu,
                               Str *cause, bool *found) {
    *found = false;
    TypesSlice *s = types_as_slice(Vu);
    if (s == NULL)
        return false;
    TypesArray *a = types_as_array(Tu);
    if (a != NULL) {
        if (!types_identical(types_slice_elem(s), types_array_elem(a)))
            return false;
        *found = true;
        if (check == NULL || burrow__tc_allow_version(check, burrow__tc_go1_20))
            return true;
        /* check != nil */
        if (cause != NULL)
            /* TODO(gri) consider restructuring versionErrorf so we can use it
             * here and below */
            *cause = BURROW_S("conversion of slice to array requires go1.20 or later");
        return false;
    }
    TypesPointer *p = types_as_pointer(Tu);
    if (p == NULL)
        return false;
    a = types_as_array(types_type_underlying(types_pointer_elem(p)));
    if (a == NULL || !types_identical(types_slice_elem(s), types_array_elem(a)))
        return false;
    *found = true;
    if (check == NULL || burrow__tc_allow_version(check, burrow__tc_go1_17))
        return true;
    /* check != nil */
    if (cause != NULL)
        *cause =
            BURROW_S("conversion of slice to array pointer requires go1.17 or later");
    return false;
}

bool burrow__tc_convertible_to(TypesChecker *check, const GtOperand *x, TypesType T,
                               Str *cause) {
    /* "x is assignable to T" */
    if (burrow__tc_assignable_to(check, x, T, cause, NULL))
        return true;

    TypesType orig_T = T;
    TypesType V = types_unalias(x->typ);
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
    if ((burrow__types_is_basic(Vu, TYPES_IS_INTEGER) || tcv_is_bytes_or_runes(Vu)) &&
        burrow__types_is_basic(Tu, TYPES_IS_STRING))
        return true;

    /* "V is a string and T is a slice of bytes or runes" */
    if (burrow__types_is_basic(Vu, TYPES_IS_STRING) && tcv_is_bytes_or_runes(Tu))
        return true;

    /* package unsafe: "any pointer or value of underlying type uintptr can be
     * converted into a unsafe.Pointer" */
    if ((burrow__tc_is_pointer(Vu) || tcv_is_uintptr(Vu)) && tcv_is_unsafe_pointer(Tu))
        return true;
    /* "and vice versa" */
    if (tcv_is_unsafe_pointer(Vu) && (burrow__tc_is_pointer(Tu) || tcv_is_uintptr(Tu)))
        return true;

    bool found;
    bool ok = tcv_slice_to_array(check, Vu, Tu, cause, &found);
    if (found)
        return ok;

    /* optimization: if we don't have type parameters, we're done */
    if (Vp == NULL && Tp == NULL)
        return false;

    /* generic cases with specific type terms (generic operands cannot be
     * constants, so we can ignore x.val) */
    TcvConvert c = {check, *x, T, orig_T, Vp, Tp, cause};
    if (Vp != NULL && Tp != NULL)
        return burrow__types_type_param_is(
            Vp, BURROW_FN(GtTermFunc, tcv_term_convertible_to_all, &c));
    if (Vp != NULL)
        return burrow__types_type_param_is(
            Vp, BURROW_FN(GtTermFunc, tcv_term_convertible_to, &c));
    return burrow__types_type_param_is(
        Tp, BURROW_FN(GtTermFunc, tcv_term_convertible_from, &c));
}
