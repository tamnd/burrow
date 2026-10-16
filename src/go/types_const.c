/* go/types: functions for untyped constant operands.
 *
 * Derived from Go's src/go/types/const.go. Go source: go1.27.1.
 *
 * Copyright 2023 The Go Authors. All rights reserved.
 * Copyright 2026 The burrow Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style licence that can be found
 * in the LICENSE file. */

#include "burrow/go/types.h"

#include "burrow/core.h"
#include "burrow/go/constant.h"
#include "burrow/go/token.h"

#include "types_check.h"
#include "types_internal.h"

#include <math.h>
#include <stdbool.h>
#include <stdint.h>

/* overflow checks that the constant x is representable by its type. For
 * untyped constants, it checks that the value doesn't become arbitrarily
 * large. */
void burrow__tc_overflow(TypesChecker *check, GtOperand *x, TokenPos op_pos) {
    burrow__types_assert(x->mode == GT_MODE_CONSTANT);

    if (constant_value_kind(x->val) == CONSTANT_UNKNOWN) {
        /* TODO(gri) We should report exactly what went wrong. At the moment we
         * don't have the (go/constant) API for that. See also TODO in
         * go/constant/value.go. */
        burrow__tc_error(check, burrow__tc_at_pos(op_pos), GT_CODE_INVALID_CONST_VAL,
                         BURROW_S("constant result is not representable"));
        return;
    }

    /* Typed constants must be representable in their type after each constant
     * operation. x.typ cannot be a type parameter (type parameters cannot be
     * constant types). */
    if (burrow__types_is_typed(x->typ)) {
        burrow__tc_representable(check, x,
                                 types_as_basic(types_type_underlying(x->typ)));
        return;
    }

    /* Untyped integer values must not grow arbitrarily. */
    enum { PREC = 512 }; /* 512 is the constant precision */
    if (constant_value_kind(x->val) == CONSTANT_INT &&
        constant_bit_len(x->val) > PREC) {
        Str op = burrow__tc_op_name(x->expr);
        if (op.len > 0)
            op = GT_SPRINTF(check, "%s ", op);
        GT_ERRORF(check, burrow__tc_at_pos(op_pos), GT_CODE_INVALID_CONST_VAL,
                  "constant %soverflow", op);
        x->val = constant_make_unknown();
        return;
    }

    /* String values must not become arbitrarily long (go.dev/issue/78346). */
    const int64_t max_len = 2000000000; /* cmd/internal/obj.MaxSymSize */
    if (constant_value_kind(x->val) == CONSTANT_STRING) {
        int64_t len = constant_string_len(x->val);
        if (len > max_len) {
            GT_ERRORF(check, burrow__tc_at_pos(op_pos), GT_CODE_INVALID_CONST_VAL,
                      "constant string too long (%d bytes > %d bytes)", len, max_len);
            x->val = constant_make_unknown();
            return;
        }
    }
}

/* Config.sizeof. Not in Go: a NULL check uses the standard sizes, where Go
 * would dereference a nil Config. */
static int64_t tco_sizeof(TypesChecker *check, TypesBasic *T) {
    return burrow__types_conf_sizeof(check != NULL ? check->conf : NULL, types_type(T));
}

/* Whether the int64 value x fits in an integer of kind k. */
static bool tco_int64_fits(TypesChecker *check, TypesBasic *typ, int64_t x) {
    switch ((Int)typ->kind) {
    case TYPES_INT: {
        uint64_t s = (uint64_t)tco_sizeof(check, typ) * 8;
        int64_t max = (int64_t)(((uint64_t)1 << (s - 1)) - 1);
        return -max - 1 <= x && x <= max;
    }
    case TYPES_INT8:
        return INT8_MIN <= x && x <= INT8_MAX;
    case TYPES_INT16:
        return INT16_MIN <= x && x <= INT16_MAX;
    case TYPES_INT32:
        return INT32_MIN <= x && x <= INT32_MAX;
    case TYPES_INT64:
    case TYPES_UNTYPED_INT:
        return true;
    case TYPES_UINT:
    case TYPES_UINTPTR: {
        uint64_t s = (uint64_t)tco_sizeof(check, typ) * 8;
        if (s < 64)
            return 0 <= x && x <= (int64_t)(((uint64_t)1 << s) - 1);
        return 0 <= x;
    }
    case TYPES_UINT8:
        return 0 <= x && x <= UINT8_MAX;
    case TYPES_UINT16:
        return 0 <= x && x <= UINT16_MAX;
    case TYPES_UINT32:
        return 0 <= x && x <= (int64_t)UINT32_MAX;
    case TYPES_UINT64:
        return 0 <= x;
    default:
        panic_str(BURROW_S("unreachable"));
    }
}

/* Whether the integer x, which does not fit into int64, fits in typ. */
static bool tco_big_fits(TypesChecker *check, TypesBasic *typ, ConstantValue x) {
    Int n = constant_bit_len(x);
    switch ((Int)typ->kind) {
    case TYPES_UINT:
    case TYPES_UINTPTR: {
        int64_t s = tco_sizeof(check, typ) * 8;
        return constant_sign(x) >= 0 && n <= s;
    }
    case TYPES_UINT64:
        return constant_sign(x) >= 0 && n <= 64;
    case TYPES_UNTYPED_INT:
        return true;
    default:
        return false;
    }
}

static bool tco_fits_float32(ConstantValue x) {
    bool exact;
    double f = (double)constant_float32_val(x, &exact);
    return !isinf(f);
}

/* roundFloat32: false where Go returns nil. */
static bool tco_round_float32(Alloc *a, ConstantValue x, ConstantValue *r) {
    bool exact;
    double f = (double)constant_float32_val(x, &exact);
    if (!isinf(f)) {
        *r = constant_make_float64(a, f);
        return true;
    }
    return false;
}

static bool tco_fits_float64(ConstantValue x) {
    bool exact;
    double f = constant_float64_val(x, &exact);
    return !isinf(f);
}

/* roundFloat64: false where Go returns nil. */
static bool tco_round_float64(Alloc *a, ConstantValue x, ConstantValue *r) {
    bool exact;
    double f = constant_float64_val(x, &exact);
    if (!isinf(f)) {
        *r = constant_make_float64(a, f);
        return true;
    }
    return false;
}

static bool tco_representable_float(Alloc *a, ConstantValue x, TypesBasic *typ,
                                    ConstantValue *rounded) {
    x = constant_to_float(a, x);
    if (constant_value_kind(x) != CONSTANT_FLOAT)
        return false;
    switch ((Int)typ->kind) {
    case TYPES_FLOAT32:
        if (rounded == NULL)
            return tco_fits_float32(x);
        return tco_round_float32(a, x, rounded);
    case TYPES_FLOAT64:
        if (rounded == NULL)
            return tco_fits_float64(x);
        return tco_round_float64(a, x, rounded);
    case TYPES_UNTYPED_FLOAT:
        return true;
    default:
        panic_str(BURROW_S("unreachable"));
    }
}

static bool tco_representable_complex(Alloc *a, ConstantValue x, TypesBasic *typ,
                                      ConstantValue *rounded) {
    x = constant_to_complex(a, x);
    if (constant_value_kind(x) != CONSTANT_COMPLEX)
        return false;
    ConstantValue re;
    ConstantValue im;
    switch ((Int)typ->kind) {
    case TYPES_COMPLEX64:
        if (rounded == NULL)
            return tco_fits_float32(constant_real(x)) &&
                   tco_fits_float32(constant_imag(x));
        if (tco_round_float32(a, constant_real(x), &re) &&
            tco_round_float32(a, constant_imag(x), &im)) {
            *rounded = constant_binary_op(a, re, TOKEN_ADD, constant_make_imag(a, im));
            return true;
        }
        return false;
    case TYPES_COMPLEX128:
        if (rounded == NULL)
            return tco_fits_float64(constant_real(x)) &&
                   tco_fits_float64(constant_imag(x));
        if (tco_round_float64(a, constant_real(x), &re) &&
            tco_round_float64(a, constant_imag(x), &im)) {
            *rounded = constant_binary_op(a, re, TOKEN_ADD, constant_make_imag(a, im));
            return true;
        }
        return false;
    case TYPES_UNTYPED_COMPLEX:
        return true;
    default:
        panic_str(BURROW_S("unreachable"));
    }
}

/* representableConst reports whether x can be represented as value of the
 * given basic type and for the configuration provided (only needed for
 * int/uint sizes).
 *
 * If rounded != nil, *rounded is set to the rounded value of x for
 * representable floating-point and complex values, and to an Int value for
 * integer values; it is left alone otherwise. It is ok to provide the address
 * of the first argument for rounded.
 *
 * The check parameter may be nil if representableConst is invoked
 * (indirectly) through an exported API call (AssignableTo, ConvertibleTo)
 * because we don't need the Checker's config for those calls. Not in Go: new
 * values are made with a. */
bool burrow__tc_representable_const(Alloc *a, ConstantValue x, TypesChecker *check,
                                    TypesBasic *typ, ConstantValue *rounded) {
    if (constant_value_kind(x) == CONSTANT_UNKNOWN)
        return true; /* avoid follow-up errors */

    TypesType t = types_type(typ);
    if (burrow__types_is_basic(t, TYPES_IS_INTEGER)) {
        x = constant_to_int(a, x);
        if (constant_value_kind(x) != CONSTANT_INT)
            return false;
        if (rounded != NULL)
            *rounded = x;
        bool ok = false;
        int64_t v = constant_int64_val(x, &ok);
        if (ok)
            return tco_int64_fits(check, typ, v);
        /* x does not fit into int64 */
        return tco_big_fits(check, typ, x);
    }
    if (burrow__types_is_basic(t, TYPES_IS_FLOAT))
        return tco_representable_float(a, x, typ, rounded);
    if (burrow__types_is_basic(t, TYPES_IS_COMPLEX))
        return tco_representable_complex(a, x, typ, rounded);
    if (burrow__types_is_basic(t, TYPES_IS_STRING))
        return constant_value_kind(x) == CONSTANT_STRING;
    if (burrow__types_is_basic(t, TYPES_IS_BOOLEAN))
        return constant_value_kind(x) == CONSTANT_BOOL;
    return false;
}

/* representation returns the representation of the constant operand x as the
 * basic type typ in *v. If no such representation is possible, it returns a
 * non-zero error code. */
GtCode burrow__tc_representation(TypesChecker *check, const GtOperand *x,
                                 TypesBasic *typ, ConstantValue *v) {
    burrow__types_assert(x->mode == GT_MODE_CONSTANT);
    *v = x->val;
    if (!burrow__tc_representable_const(check->a, x->val, check, typ, v)) {
        TypesType t = types_type(typ);
        if (burrow__types_is_basic(x->typ, TYPES_IS_NUMERIC) &&
            burrow__types_is_basic(t, TYPES_IS_NUMERIC)) {
            /* numeric conversion : error msg
             *
             * integer -> integer : overflows
             * integer -> float   : overflows (actually not possible)
             * float   -> integer : truncated
             * float   -> float   : overflows */
            if (!burrow__types_is_basic(x->typ, TYPES_IS_INTEGER) &&
                burrow__types_is_basic(t, TYPES_IS_INTEGER))
                return GT_CODE_TRUNCATED_FLOAT;
            return GT_CODE_NUMERIC_OVERFLOW;
        }
        return GT_CODE_INVALID_CONST_VAL;
    }
    return GT_CODE_NONE;
}

/* representable checks that a constant operand is representable in the given
 * basic type. */
void burrow__tc_representable(TypesChecker *check, GtOperand *x, TypesBasic *typ) {
    ConstantValue v;
    GtCode code = burrow__tc_representation(check, x, typ, &v);
    if (code != GT_CODE_NONE) {
        burrow__tc_invalid_conversion(check, code, x, types_type(typ));
        burrow__tc_op_invalidate(x);
        return;
    }
    x->val = v;
}

void burrow__tc_invalid_conversion(TypesChecker *check, GtCode code, const GtOperand *x,
                                   TypesType target) {
    Str xs = burrow__tc_xs(check, x);
    Str ts = burrow__tc_ts(check, target);
    Str msg;
    switch ((Int)code) {
    case GT_CODE_TRUNCATED_FLOAT:
        msg = GT_SPRINTF(check, "%s truncated to %s", xs, ts);
        break;
    case GT_CODE_NUMERIC_OVERFLOW:
        msg = GT_SPRINTF(check, "%s overflows %s", xs, ts);
        break;
    default:
        msg = GT_SPRINTF(check, "cannot convert %s to type %s", xs, ts);
        break;
    }
    burrow__tc_error(check, burrow__tc_at_op(x), code, msg);
}

/* convertUntyped attempts to set the type of an untyped value to the target
 * type. */
void burrow__tc_convert_untyped(TypesChecker *check, GtOperand *x, TypesType target) {
    TypesType new_type = NULL;
    ConstantValue val;
    bool has_val = false;
    GtCode code =
        burrow__tc_implicit_type_and_value(check, x, target, &new_type, &val, &has_val);
    if (code != GT_CODE_NONE) {
        TypesType t = target;
        if (!burrow__types_is_type_param(target))
            t = burrow__types_safe_underlying(target);
        burrow__tc_invalid_conversion(check, code, x, t);
        burrow__tc_op_invalidate(x);
        return;
    }
    if (has_val) {
        x->val = val;
        burrow__tc_update_expr_val(check, x->expr, val);
    }
    if (new_type != x->typ) {
        x->typ = new_type;
        burrow__tc_update_expr_type(check, x->expr, new_type, false);
    }
}
