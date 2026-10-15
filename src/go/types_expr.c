/* go/types: type-checking of expressions.
 *
 * Derived from Go's src/go/types/expr.go. Go source: go1.27.1.
 *
 * Copyright 2012 The Go Authors. All rights reserved.
 * Copyright 2026 The burrow Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style licence that can be found
 * in the LICENSE file. */

#include "burrow/go/types.h"

#include "burrow/core.h"
#include "burrow/go/constant.h"

#include "types_check.h"
#include "types_internal.h"

#include <stdbool.h>

/* A nil constant.Value. */
static const ConstantValue tex_nil = {0};

/* What the underIs walk of implicitTypeAndValue closes over. */
typedef struct TexImplicit {
    TypesChecker *check;
    const GtOperand *x;
} TexImplicit;

static bool tex_under_implicit_ok(void *env, TypesType t, TypesType u) {
    (void)t;
    if (u == NULL)
        return false;
    TexImplicit *c = env;
    TypesType typ = NULL;
    ConstantValue val;
    bool has_val;
    (void)burrow__tc_implicit_type_and_value(c->check, c->x, u, &typ, &val, &has_val);
    return typ != NULL;
}

/* The Basic case of implicitTypeAndValue for a non-constant x. Non-constant
 * untyped values may appear as the result of comparisons (untyped bool),
 * intermediate (delayed-checked) rhs operands of shifts, and as the value
 * nil. */
static GtCode tex_implicit_basic(const GtOperand *x, TypesType target, TypesType *typ) {
    switch ((Int)types_as_basic(x->typ)->kind) {
    case TYPES_UNTYPED_BOOL:
        if (!burrow__types_is_basic(target, TYPES_IS_BOOLEAN))
            return GT_CODE_INVALID_UNTYPED_CONVERSION;
        break;
    case TYPES_UNTYPED_INT:
    case TYPES_UNTYPED_RUNE:
    case TYPES_UNTYPED_FLOAT:
    case TYPES_UNTYPED_COMPLEX:
        if (!burrow__types_is_basic(target, TYPES_IS_NUMERIC))
            return GT_CODE_INVALID_UNTYPED_CONVERSION;
        break;
    case TYPES_UNTYPED_STRING:
        /* Non-constant untyped string values are not permitted by the spec and
         * should not occur during normal typechecking passes, but this path is
         * reachable via the AssignableTo API. */
        if (!burrow__types_is_basic(target, TYPES_IS_STRING))
            return GT_CODE_INVALID_UNTYPED_CONVERSION;
        break;
    case TYPES_UNTYPED_NIL:
        /* Unsafe.Pointer is a basic type that includes nil. */
        if (!burrow__types_has_nil(target))
            return GT_CODE_INVALID_UNTYPED_CONVERSION;
        /* Preserve the type of nil as UntypedNil: see go.dev/issue/13061. */
        *typ = GT_TYP(TYPES_UNTYPED_NIL);
        return GT_CODE_NONE;
    default:
        return GT_CODE_INVALID_UNTYPED_CONVERSION;
    }
    *typ = target;
    return GT_CODE_NONE;
}

/* implicitTypeAndValue returns the implicit type of x when used in a context
 * where the target type is expected. If no such implicit conversion is
 * possible, it returns a nil Type and non-zero error code.
 *
 * If x is a constant operand, the returned constant.Value will be the
 * representation of x in this context. check may be NULL, as in Go, when x is
 * not a constant. */
GtCode burrow__tc_implicit_type_and_value(TypesChecker *check, const GtOperand *x,
                                          TypesType target, TypesType *typ,
                                          ConstantValue *val, bool *has_val) {
    *typ = NULL;
    *val = tex_nil;
    *has_val = false;
    if (!burrow__tc_op_valid(x) || burrow__types_is_typed(x->typ) ||
        !burrow__types_is_valid(target)) {
        *typ = x->typ;
        return GT_CODE_NONE;
    }
    /* x is untyped */

    if (!burrow__types_is_typed(target)) {
        /* both x and target are untyped */
        TypesType m = burrow__types_max_type(x->typ, target);
        if (m != NULL) {
            *typ = m;
            return GT_CODE_NONE;
        }
        return GT_CODE_INVALID_UNTYPED_CONVERSION;
    }

    TypesType u = types_type_underlying(target);
    switch (u->kind) {
    case TYPES_KIND_BASIC:
        if (x->mode == GT_MODE_CONSTANT) {
            ConstantValue v;
            GtCode code = burrow__tc_representation(check, x, types_as_basic(u), &v);
            if (code != GT_CODE_NONE)
                return code;
            *typ = target;
            *val = v;
            *has_val = true;
            return GT_CODE_NONE;
        }
        return tex_implicit_basic(x, target, typ);
    case TYPES_KIND_INTERFACE:
        if (burrow__types_is_type_param(target)) {
            TexImplicit c = {check, x};
            if (!burrow__types_all(
                    target, BURROW_FN(GtTypePairFunc, tex_under_implicit_ok, &c)))
                return GT_CODE_INVALID_UNTYPED_CONVERSION;
            /* keep nil untyped (was bug go.dev/issue/39755) */
            *typ = burrow__tc_op_is_nil(x) ? GT_TYP(TYPES_UNTYPED_NIL) : target;
            return GT_CODE_NONE;
        }
        /* Values must have concrete dynamic types. If the value is nil, keep it
         * untyped (this is important for tools such as go vet which need the
         * dynamic type for argument checking of say, print functions) */
        if (burrow__tc_op_is_nil(x)) {
            *typ = GT_TYP(TYPES_UNTYPED_NIL);
            return GT_CODE_NONE;
        }
        /* cannot assign untyped values to non-empty interfaces */
        if (!types_interface_empty(types_as_interface(u)))
            return GT_CODE_INVALID_UNTYPED_CONVERSION;
        *typ = types_default(x->typ);
        return GT_CODE_NONE;
    case TYPES_KIND_POINTER:
    case TYPES_KIND_SIGNATURE:
    case TYPES_KIND_SLICE:
    case TYPES_KIND_MAP:
    case TYPES_KIND_CHAN:
        if (!burrow__tc_op_is_nil(x))
            return GT_CODE_INVALID_UNTYPED_CONVERSION;
        /* Keep nil untyped - see comment for interfaces, above. */
        *typ = GT_TYP(TYPES_UNTYPED_NIL);
        return GT_CODE_NONE;
    default:
        return GT_CODE_INVALID_UNTYPED_CONVERSION;
    }
}
