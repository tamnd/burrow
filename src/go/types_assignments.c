/* go/types: initialization and assignment checks.
 *
 * Derived from Go's src/go/types/assignments.go. Go source: go1.27.1.
 *
 * Copyright 2013 The Go Authors. All rights reserved.
 * Copyright 2026 The burrow Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style licence that can be found
 * in the LICENSE file. */

#include "burrow/go/types.h"

#include "burrow/core.h"
#include "burrow/go/ast.h"
#include "burrow/go/constant.h"
#include "burrow/go/token.h"
#include "burrow/map.h"
#include "burrow/slice.h"
#include "burrow/strings.h"
#include "burrow/type.h"

#include "types_check.h"
#include "types_internal.h"

#include <stdbool.h>

/* assignment reports whether x can be assigned to a variable of type T, if
 * necessary by attempting to convert untyped values to the appropriate type.
 * context describes the context in which the assignment takes place. Use
 * T == NULL to indicate assignment to an untyped blank identifier. If the
 * assignment check fails, x->mode is set to invalid. */
void burrow__tc_assignment(TypesChecker *check, GtOperand *x, TypesType T,
                           Str context) {
    burrow__tc_single_value(check, x);

    switch (x->mode) {
    case GT_MODE_INVALID:
        return; /* error reported before */
    case GT_MODE_CONSTANT:
    case GT_MODE_VARIABLE:
    case GT_MODE_MAPINDEX:
    case GT_MODE_VALUE:
    case GT_MODE_COMMAOK:
    case GT_MODE_COMMAERR:
        /* ok */
        break;
    case GT_MODE_NOVALUE:
    case GT_MODE_BUILTIN:
    case GT_MODE_TYPEXPR:
    case GT_MODE_NILVALUE: /* only used by types2 */
    case GT_MODE_CGOFUNC:
    default: {
        /* we may get here because of other problems (go.dev/issue/39634,
         * crash 12) */
        Str xs = burrow__tc_xs(check, x);
        Str ts = burrow__tc_ts(check, T);
        GT_ERRORF(check, burrow__tc_at_op(x), GT_CODE_INCOMPATIBLE_ASSIGN,
                  "cannot assign %s to %s in %s", xs, ts, context);
        burrow__tc_op_invalidate(x);
        return;
    }
    }

    if (!burrow__types_is_typed(x->typ)) {
        TypesType target = T;
        /* spec: "If an untyped constant is assigned to a variable of interface
         * type or the blank identifier, the constant is first converted to
         * type bool, rune, int, float64, complex128 or string respectively,
         * depending on whether the value is a boolean, rune, integer,
         * floating-point, complex, or string constant." */
        if (T == NULL || burrow__types_is_non_type_param_interface(T)) {
            if (T == NULL && x->typ == GT_TYP(TYPES_UNTYPED_NIL)) {
                GT_ERRORF(check, burrow__tc_at_op(x), GT_CODE_UNTYPED_NIL_USE,
                          "use of untyped nil in %s", context);
                burrow__tc_op_invalidate(x);
                return;
            }
            target = types_default(x->typ);
        }
        TypesType new_type = NULL;
        ConstantValue val = {0};
        bool has_val = false;
        GtCode code = burrow__tc_implicit_type_and_value(check, x, target, &new_type,
                                                         &val, &has_val);
        if (code != 0) {
            Str xs = burrow__tc_xs(check, x);
            Str ts = burrow__tc_ts(check, target);
            Str msg =
                GT_SPRINTF(check, "cannot use %s as %s value in %s", xs, ts, context);
            if (code == GT_CODE_TRUNCATED_FLOAT) {
                msg = GT_SPRINTF(check, "%s (truncated)", msg);
            } else if (code == GT_CODE_NUMERIC_OVERFLOW) {
                msg = GT_SPRINTF(check, "%s (overflows)", msg);
            } else {
                code = GT_CODE_INCOMPATIBLE_ASSIGN;
            }
            burrow__tc_error(check, burrow__tc_at_op(x), code, msg);
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
    /* x.typ is typed */

    /* A generic (non-instantiated) function value cannot be assigned to a
     * variable. */
    burrow__tc_non_generic(check, burrow__tc_new_target(check->a, T, context), x);
    if (!burrow__tc_op_valid(x))
        return;

    /* spec: "If a left-hand side is the blank identifier, any typed or
     * non-constant value except for the predeclared identifier nil may be
     * assigned to it." */
    if (T == NULL)
        return;

    Str cause = {0};
    GtCode code = 0;
    if (!burrow__tc_assignable_to(check, x, T, &cause, &code)) {
        Str xs = burrow__tc_xs(check, x);
        Str ts = burrow__tc_ts(check, T);
        if (cause.len != 0)
            GT_ERRORF(check, burrow__tc_at_op(x), code,
                      "cannot use %s as %s value in %s: %s", xs, ts, context, cause);
        else
            GT_ERRORF(check, burrow__tc_at_op(x), code,
                      "cannot use %s as %s value in %s", xs, ts, context);
        burrow__tc_op_invalidate(x);
    }
}

void burrow__tc_init_const(TypesChecker *check, TypesConst *lhs, GtOperand *x) {
    if (!burrow__tc_op_valid(x) || !burrow__types_is_valid(x->typ) ||
        !burrow__types_is_valid(GT_OBJ(lhs)->typ)) {
        if (GT_OBJ(lhs)->typ == NULL)
            GT_OBJ(lhs)->typ = GT_TYP(TYPES_INVALID);
        return;
    }

    /* rhs must be a constant */
    if (x->mode != GT_MODE_CONSTANT) {
        Str xs = burrow__tc_xs(check, x);
        GT_ERRORF(check, burrow__tc_at_op(x), GT_CODE_INVALID_CONST_INIT,
                  "%s is not constant", xs);
        if (GT_OBJ(lhs)->typ == NULL)
            GT_OBJ(lhs)->typ = GT_TYP(TYPES_INVALID);
        return;
    }
    burrow__types_assert(burrow__types_is_basic(x->typ, TYPES_IS_CONST_TYPE));

    /* If the lhs doesn't have a type yet, use the type of x. */
    if (GT_OBJ(lhs)->typ == NULL)
        GT_OBJ(lhs)->typ = x->typ;

    burrow__tc_assignment(check, x, GT_OBJ(lhs)->typ, BURROW_S("constant declaration"));
    if (!burrow__tc_op_valid(x))
        return;

    lhs->val = x->val;
}

/* initVar checks the initialization lhs = x in a variable declaration. If lhs
 * doesn't have a type yet, it is given the type of x, or Typ[Invalid] in case
 * of an error. If the initialization check fails, x->mode is set to
 * invalid. */
void burrow__tc_init_var(TypesChecker *check, TypesVar *lhs, GtOperand *x,
                         Str context) {
    if (!burrow__tc_op_valid(x) || !burrow__types_is_valid(x->typ) ||
        !burrow__types_is_valid(GT_OBJ(lhs)->typ)) {
        if (GT_OBJ(lhs)->typ == NULL)
            GT_OBJ(lhs)->typ = GT_TYP(TYPES_INVALID);
        burrow__tc_op_invalidate(x);
        return;
    }

    /* If lhs doesn't have a type yet, use the type of x. */
    if (GT_OBJ(lhs)->typ == NULL) {
        TypesType typ = x->typ;
        if (!burrow__types_is_typed(typ)) {
            /* convert untyped types to default types */
            if (typ == GT_TYP(TYPES_UNTYPED_NIL)) {
                GT_ERRORF(check, burrow__tc_at_op(x), GT_CODE_UNTYPED_NIL_USE,
                          "use of untyped nil in %s", context);
                GT_OBJ(lhs)->typ = GT_TYP(TYPES_INVALID);
                burrow__tc_op_invalidate(x);
                return;
            }
            typ = types_default(typ);
        }
        GT_OBJ(lhs)->typ = typ;
    }

    burrow__tc_assignment(check, x, GT_OBJ(lhs)->typ, context);
}

static void tas_set_used(Map *m, TypesVar *v, bool used) {
    if (!map_set(m, &v, &used))
        burrow__types_oom();
}

/* lhsVar checks a lhs variable in an assignment and returns its type. lhsVar
 * takes care of not counting a lhs identifier as a "use" of that identifier.
 * The result is NULL if it is the blank identifier, and Typ[Invalid] if it is
 * an invalid lhs expression. */
static TypesType tas_lhs_var(TypesChecker *check, AstExpr lhs) {
    /* Determine if the lhs is a (possibly parenthesized) identifier. */
    AstExpr un = ast_unparen(lhs);
    AstIdent *ident =
        un != NULL && un->kind == AST_KIND_IDENT ? (AstIdent *)(void *)un : NULL;

    /* Don't evaluate lhs if it is the blank identifier. */
    if (ident != NULL && str_eq(ident->name, BURROW_S("_"))) {
        burrow__tc_record_def(check, ident, NULL);
        return NULL;
    }

    /* If the lhs is an identifier denoting a variable v, this reference is not
     * a 'use' of v. Remember current value of v.used and restore after
     * evaluating the lhs via check.expr. */
    TypesVar *v = NULL;
    bool v_used = false;
    if (ident != NULL) {
        TypesObject obj = burrow__tc_env_lookup(&check->env, ident->name);
        if (obj != NULL) {
            /* It's ok to mark non-local variables, but ignore variables from
             * other packages to avoid potential race conditions with
             * dot-imported variables. */
            TypesVar *w = types_as_var(obj);
            if (w != NULL && GT_OBJ(w)->pkg == check->pkg) {
                v = w;
                const bool *used = map_get(check->used_vars, &v);
                v_used = used != NULL && *used;
            }
        }
    }

    GtOperand x = {0};
    burrow__tc_expr(check, NULL, &x, lhs);

    if (v != NULL)
        tas_set_used(check->used_vars, v, v_used); /* restore v.used */

    if (!burrow__tc_op_valid(&x) || !burrow__types_is_valid(x.typ))
        return GT_TYP(TYPES_INVALID);

    /* spec: "Each left-hand side operand must be addressable, a map index
     * expression, or the blank identifier. Operands may be parenthesized." */
    switch (x.mode) {
    case GT_MODE_INVALID:
        return GT_TYP(TYPES_INVALID);
    case GT_MODE_VARIABLE:
    case GT_MODE_MAPINDEX:
        /* ok */
        break;
    case GT_MODE_NOVALUE:
    case GT_MODE_BUILTIN:
    case GT_MODE_TYPEXPR:
    case GT_MODE_CONSTANT:
    case GT_MODE_VALUE:
    case GT_MODE_NILVALUE:
    case GT_MODE_COMMAOK:
    case GT_MODE_COMMAERR:
    case GT_MODE_CGOFUNC:
    default: {
        if (x.expr != NULL && x.expr->kind == AST_KIND_SELECTOR_EXPR) {
            AstSelectorExpr *sel = (AstSelectorExpr *)(void *)x.expr;
            GtOperand op = {0};
            burrow__tc_expr(check, NULL, &op, sel->x);
            if (op.mode == GT_MODE_MAPINDEX) {
                Str es = burrow__tc_es(check, x.expr);
                GT_ERRORF(check, burrow__tc_at_op(&x),
                          GT_CODE_UNADDRESSABLE_FIELD_ASSIGN,
                          "cannot assign to struct field %s in map", es);
                return GT_TYP(TYPES_INVALID);
            }
        }
        Str es = burrow__tc_es(check, x.expr);
        GT_ERRORF(
            check, burrow__tc_at_op(&x), GT_CODE_UNASSIGNABLE_OPERAND,
            "cannot assign to %s (neither addressable nor a map index expression)", es);
        return GT_TYP(TYPES_INVALID);
    }
    }

    return x.typ;
}

/* assignVar checks the assignment lhs = rhs (if x == NULL), or lhs = x (if
 * x != NULL). If x != NULL, it must be the evaluation of rhs (and rhs will be
 * ignored). If the assignment check fails and x != NULL, x->mode is set to
 * invalid. */
void burrow__tc_assign_var(TypesChecker *check, AstExpr lhs, AstExpr rhs, GtOperand *x,
                           Str context) {
    TypesType T = tas_lhs_var(check, lhs); /* NULL if lhs is _ */
    if (!burrow__types_is_valid(T)) {
        if (x != NULL)
            burrow__tc_op_invalidate(x);
        else
            (void)burrow__tc_use1(check, rhs, false);
        return;
    }

    if (x == NULL) {
        GtTarget *target = NULL;
        /* avoid calling ExprString if not needed */
        if (T != NULL && types_as_signature(types_type_underlying(T)) != NULL)
            target = burrow__tc_new_target(check->a, T, burrow__tc_es(check, lhs));
        x = GT_NEW(check->a, GtOperand);
        burrow__tc_expr(check, target, x, rhs);
    }

    if (T == NULL && str_eq(context, BURROW_S("assignment")))
        context = BURROW_S("assignment to _ identifier");
    burrow__tc_assignment(check, x, T, context);
}

Slice burrow__tc_operand_types(Alloc *a, Slice list) {
    Slice res = slice_nil(TYPE_UNSAFE_POINTER);
    for (Int i = 0; i < list.len; i++)
        burrow__types_ptrs_append(a, &res, GT_AT(list, i, GtOperand *)->typ);
    return res;
}

Slice burrow__tc_var_types(Alloc *a, Slice list) {
    Slice res = slice_nil(TYPE_UNSAFE_POINTER);
    for (Int i = 0; i < list.len; i++)
        burrow__types_ptrs_append(a, &res, GT_OBJ(GT_AT(list, i, TypesVar *))->typ);
    return res;
}

/* typesSummary returns a string of the form "(t1, t2, ...)" where the ti's are
 * user-friendly string representations for the given types. If variadic is
 * set and the last type is a slice, its string is of the form "...E" where E
 * is the slice's element type. If has_dots is set, the last argument string is
 * of the form "T..." where T is the last type. Only one of variadic and
 * has_dots may be set. */
Str burrow__tc_types_summary(TypesChecker *check, Slice list, bool variadic,
                             bool has_dots) {
    burrow__types_assert(!(variadic && has_dots));
    Slice res = slice_nil(TYPE_STRING);
    for (Int i = 0; i < list.len; i++) {
        TypesType t = GT_AT(list, i, TypesType);
        Str s;
        if (t == NULL || !burrow__types_is_valid(t)) {
            /* t == NULL should not happen but be cautious */
            s = BURROW_S("unknown type");
        } else if (!burrow__types_is_typed(t)) { /* => *Basic */
            if (burrow__types_is_basic(t, TYPES_IS_NUMERIC)) {
                /* Do not imply a specific type requirement: "have number, want
                 * float64" is better than "have untyped int, want float64" or
                 * "have int, want float64". */
                s = BURROW_S("number");
            } else {
                /* If we don't have a number, omit the "untyped" qualifier for
                 * compactness. */
                TypesBasic *b = types_as_basic(t);
                burrow__types_assert(b != NULL);
                s = strings_replace_all(check->a, b->name, BURROW_S("untyped "),
                                        BURROW_S(""));
            }
        } else {
            s = burrow__tc_ts(check, t);
        }
        /* handle ... parameters/arguments */
        if (i == list.len - 1) {
            if (variadic) {
                /* In correct code, the parameter type is a slice, but be
                 * careful. */
                TypesSlice *st = types_as_slice(t);
                if (st != NULL)
                    s = burrow__tc_ts(check, st->elem);
                s = GT_SPRINTF(check, "...%s", s);
            } else if (has_dots) {
                s = GT_SPRINTF(check, "%s...", s);
            }
        }
        res = slice_append(check->a, res, &s, 1);
        if (res.p == NULL)
            burrow__types_oom();
    }
    Str joined = strings_join(check->a, res, BURROW_S(", "));
    return GT_SPRINTF(check, "(%s)", joined);
}

Str burrow__tc_measure(TypesChecker *check, Int x, Str unit) {
    if (x != 1)
        unit = GT_SPRINTF(check, "%ss", unit);
    return GT_SPRINTF(check, "%d %s", x, unit);
}

static void tas_assign_error(TypesChecker *check, Slice rhs, Int l, Int r) {
    Str vars = burrow__tc_measure(check, l, BURROW_S("variable"));
    Str vals = burrow__tc_measure(check, r, BURROW_S("value"));
    AstExpr rhs0 = GT_AT(rhs, 0, AstExpr);

    if (rhs.len == 1) {
        AstExpr un = ast_unparen(rhs0);
        if (un != NULL && un->kind == AST_KIND_CALL_EXPR) {
            AstCallExpr *call = (AstCallExpr *)(void *)un;
            Str fs = burrow__tc_es(check, call->fun);
            GT_ERRORF(check, burrow__tc_at_node(rhs0), GT_CODE_WRONG_ASSIGN_COUNT,
                      "assignment mismatch: %s but %s returns %s", vars, fs, vals);
            return;
        }
    }
    GT_ERRORF(check, burrow__tc_at_node(rhs0), GT_CODE_WRONG_ASSIGN_COUNT,
              "assignment mismatch: %s but %s", vars, vals);
}

static void tas_return_error(TypesChecker *check, GtPosn at, Slice lhs, Slice rhs) {
    Int l = lhs.len, r = rhs.len;
    Str qualifier = BURROW_S("not enough");
    if (r > l) {
        at = burrow__tc_at_op(GT_AT(rhs, l, GtOperand *)); /* report at first extra
                                                              value */
        qualifier = BURROW_S("too many");
    } else if (r > 0) {
        at =
            burrow__tc_at_op(GT_AT(rhs, r - 1, GtOperand *)); /* report at last value */
    }
    GtError *err = burrow__tc_new_error(check, GT_CODE_WRONG_RESULT_COUNT);
    GT_ADDF(err, at, "%s return values", qualifier);
    Str have = burrow__tc_types_summary(check, burrow__tc_operand_types(check->a, rhs),
                                        false, false);
    GT_ADDF(err, burrow__tc_at_pos(TOKEN_NO_POS), "have %s", have);
    Str want = burrow__tc_types_summary(check, burrow__tc_var_types(check->a, lhs),
                                        false, false);
    GT_ADDF(err, burrow__tc_at_pos(TOKEN_NO_POS), "want %s", want);
    burrow__tc_error_report(err);
}

/* tas_is_single_call: whether list is a single, possibly parenthesized, call. */
static bool tas_is_single_call(Slice list) {
    if (list.len != 1)
        return false;
    AstExpr un = ast_unparen(GT_AT(list, 0, AstExpr));
    return un != NULL && un->kind == AST_KIND_CALL_EXPR;
}

/* ensure that LHS variables have a type */
static void tas_ensure_types(Slice lhs) {
    for (Int i = 0; i < lhs.len; i++) {
        TypesVar *v = GT_AT(lhs, i, TypesVar *);
        if (GT_OBJ(v)->typ == NULL)
            GT_OBJ(v)->typ = GT_TYP(TYPES_INVALID);
    }
}

/* initVars type-checks assignments of initialization expressions orig_rhs to
 * variables lhs. If return_stmt is non-NULL, initVars type-checks the implicit
 * assignment of result expressions orig_rhs to function result parameters
 * lhs. */
void burrow__tc_init_vars(TypesChecker *check, Slice lhs, Slice orig_rhs,
                          AstStmt return_stmt) {
    Int l = lhs.len, r = orig_rhs.len;

    Str context = BURROW_S("assignment");
    if (return_stmt != NULL)
        context = BURROW_S("return statement");
    else if (l > 1)
        context = BURROW_S("multiple assignment");

    /* If l == 1 and the rhs is a single call, for a better error message don't
     * handle it as n:n mapping below. */
    bool is_call = tas_is_single_call(orig_rhs);

    /* If we have a n:n mapping from lhs variable to rhs expression, each value
     * can be assigned to its corresponding variable. */
    if (l == r && !is_call) {
        GtOperand x = {0};
        for (Int i = 0; i < l; i++) {
            TypesVar *v = GT_AT(lhs, i, TypesVar *);
            Str desc = GT_OBJ(v)->name;
            if (return_stmt != NULL && desc.len == 0)
                desc = BURROW_S("result variable");
            burrow__tc_expr(check,
                            burrow__tc_new_target(check->a, GT_OBJ(v)->typ, desc), &x,
                            GT_AT(orig_rhs, i, AstExpr));
            burrow__tc_init_var(check, v, &x, context);
        }
        return;
    }

    /* If we don't have an n:n mapping, the rhs must be a single expression
     * resulting in 2 or more values; otherwise we have an assignment
     * mismatch. */
    if (r != 1) {
        /* Only report a mismatch error if there are no other errors on the
         * rhs. */
        if (burrow__tc_use(check, orig_rhs)) {
            if (return_stmt != NULL) {
                Slice rhs = burrow__tc_expr_list(check, orig_rhs);
                tas_return_error(check, burrow__tc_at_node(return_stmt), lhs, rhs);
            } else {
                tas_assign_error(check, orig_rhs, l, r);
            }
        }
        tas_ensure_types(lhs);
        return;
    }

    Slice rhs;
    bool comma_ok = burrow__tc_multi_expr(check, GT_AT(orig_rhs, 0, AstExpr),
                                          l == 2 && return_stmt == NULL, &rhs);
    r = rhs.len;
    if (l == r) {
        for (Int i = 0; i < l; i++)
            burrow__tc_init_var(check, GT_AT(lhs, i, TypesVar *),
                                GT_AT(rhs, i, GtOperand *), context);
        /* Only record comma-ok expression if both initializations succeeded
         * (go.dev/issue/59371). */
        if (comma_ok && GT_AT(rhs, 0, GtOperand *)->mode != GT_MODE_INVALID &&
            GT_AT(rhs, 1, GtOperand *)->mode != GT_MODE_INVALID)
            burrow__tc_record_comma_ok_types(check, GT_AT(orig_rhs, 0, AstExpr),
                                             GT_AT(rhs, 0, GtOperand *),
                                             GT_AT(rhs, 1, GtOperand *));
        return;
    }

    /* In all other cases we have an assignment mismatch. Only report a
     * mismatch error if there are no other errors on the rhs. */
    if (GT_AT(rhs, 0, GtOperand *)->mode != GT_MODE_INVALID) {
        if (return_stmt != NULL)
            tas_return_error(check, burrow__tc_at_node(return_stmt), lhs, rhs);
        else
            tas_assign_error(check, orig_rhs, l, r);
    }
    tas_ensure_types(lhs);
    /* orig_rhs[0] was already evaluated */
}

/* assignVars type-checks assignments of expressions orig_rhs to variables
 * lhs. */
void burrow__tc_assign_vars(TypesChecker *check, Slice lhs, Slice orig_rhs) {
    Int l = lhs.len, r = orig_rhs.len;

    Str context = BURROW_S("assignment");
    if (l > 1)
        context = BURROW_S("multiple assignment");

    /* If l == 1 and the rhs is a single call, for a better error message don't
     * handle it as n:n mapping below. */
    bool is_call = tas_is_single_call(orig_rhs);

    /* If we have a n:n mapping from lhs variable to rhs expression, each value
     * can be assigned to its corresponding variable. */
    if (l == r && !is_call) {
        for (Int i = 0; i < l; i++)
            burrow__tc_assign_var(check, GT_AT(lhs, i, AstExpr),
                                  GT_AT(orig_rhs, i, AstExpr), NULL, context);
        return;
    }

    /* If we don't have an n:n mapping, the rhs must be a single expression
     * resulting in 2 or more values; otherwise we have an assignment
     * mismatch. */
    if (r != 1) {
        /* Only report a mismatch error if there are no other errors on the lhs
         * or rhs. */
        bool ok_lhs = burrow__tc_use_lhs(check, lhs);
        bool ok_rhs = burrow__tc_use(check, orig_rhs);
        if (ok_lhs && ok_rhs)
            tas_assign_error(check, orig_rhs, l, r);
        return;
    }

    Slice rhs;
    bool comma_ok =
        burrow__tc_multi_expr(check, GT_AT(orig_rhs, 0, AstExpr), l == 2, &rhs);
    r = rhs.len;
    if (l == r) {
        for (Int i = 0; i < l; i++)
            burrow__tc_assign_var(check, GT_AT(lhs, i, AstExpr), NULL,
                                  GT_AT(rhs, i, GtOperand *), context);
        /* Only record comma-ok expression if both assignments succeeded
         * (go.dev/issue/59371). */
        if (comma_ok && GT_AT(rhs, 0, GtOperand *)->mode != GT_MODE_INVALID &&
            GT_AT(rhs, 1, GtOperand *)->mode != GT_MODE_INVALID)
            burrow__tc_record_comma_ok_types(check, GT_AT(orig_rhs, 0, AstExpr),
                                             GT_AT(rhs, 0, GtOperand *),
                                             GT_AT(rhs, 1, GtOperand *));
        return;
    }

    /* In all other cases we have an assignment mismatch. Only report a
     * mismatch error if there are no other errors on the rhs. */
    if (GT_AT(rhs, 0, GtOperand *)->mode != GT_MODE_INVALID)
        tas_assign_error(check, orig_rhs, l, r);
    (void)burrow__tc_use_lhs(check, lhs);
    /* orig_rhs[0] was already evaluated */
}

void burrow__tc_short_var_decl(TypesChecker *check, GtPosn pos, Slice lhs,
                               Slice orig_rhs) {
    Int top = check->delayed.len;
    TypesScope *scope = check->env.scope;

    /* collect lhs variables */
    Map *seen = map_make(check->a, TYPE_STRING, TYPE_BOOL, lhs.len);
    if (seen == NULL)
        burrow__types_oom();
    Slice vars = burrow__types_ptrs_make(check->a, lhs.len);
    Slice new_vars = slice_nil(TYPE_UNSAFE_POINTER);
    bool has_err = false;
    for (Int i = 0; i < lhs.len; i++) {
        AstExpr e = GT_AT(lhs, i, AstExpr);
        AstIdent *ident = e->kind == AST_KIND_IDENT ? (AstIdent *)(void *)e : NULL;
        if (ident == NULL) {
            (void)burrow__tc_use_lhs(check, slice_sub(lhs, i, i + 1));
            /* TODO(gri) This is redundant with a go/parser error. Consider
             * omitting in go/types? */
            Str es = burrow__tc_es(check, e);
            GT_ERRORF(check, burrow__tc_at_node(e), GT_CODE_BAD_DECL,
                      "non-name %s on left side of :=", es);
            has_err = true;
            continue;
        }

        Str name = ident->name;
        if (!str_eq(name, BURROW_S("_"))) {
            if (map_get(seen, &name) != NULL) {
                Str es = burrow__tc_es(check, e);
                GT_ERRORF(check, burrow__tc_at_node(e), GT_CODE_REPEATED_DECL,
                          "%s repeated on left side of :=", es);
                has_err = true;
                continue;
            }
            bool yes = true;
            if (!map_set(seen, &name, &yes))
                burrow__types_oom();
        }

        /* Use the correct obj if the ident is redeclared. The variable's scope
         * starts after the declaration; so we must use Scope.Lookup here and
         * call Scope.Insert (via check.declare) later. */
        TypesObject alt = types_scope_lookup(scope, name);
        if (alt != NULL) {
            burrow__tc_record_use(check, ident, alt);
            /* redeclared object must be a variable */
            TypesVar *obj = types_as_var(alt);
            if (obj != NULL) {
                GT_AT(vars, i, TypesVar *) = obj;
            } else {
                Str es = burrow__tc_es(check, e);
                GT_ERRORF(check, burrow__tc_at_node(e), GT_CODE_UNASSIGNABLE_OPERAND,
                          "cannot assign to %s", es);
                has_err = true;
            }
            continue;
        }

        /* declare new variable */
        TypesVar *obj = burrow__types_new_var(check->a, TYPES_LOCAL_VAR,
                                              ident->name_pos, check->pkg, name, NULL);
        GT_AT(vars, i, TypesVar *) = obj;
        if (!str_eq(name, BURROW_S("_")))
            burrow__types_ptrs_append(check->a, &new_vars, obj);
        burrow__tc_record_def(check, ident, types_object(obj));
    }

    /* create dummy variables where the lhs is invalid */
    for (Int i = 0; i < vars.len; i++) {
        if (GT_AT(vars, i, TypesVar *) == NULL)
            GT_AT(vars, i, TypesVar *) = burrow__types_new_var(
                check->a, TYPES_LOCAL_VAR, ast_expr_pos(GT_AT(lhs, i, AstExpr)),
                check->pkg, BURROW_S("_"), NULL);
    }

    burrow__tc_init_vars(check, vars, orig_rhs, NULL);

    /* process function literals in rhs expressions before scope changes */
    burrow__tc_process_delayed(check, top);

    if (new_vars.len == 0 && !has_err) {
        burrow__tc_soft_error(check, pos, GT_CODE_NO_NEW_VAR,
                              BURROW_S("no new variables on left side of :="));
        return;
    }

    /* declare new variables */
    /* spec: "The scope of a constant or variable identifier declared inside a
     * function begins at the end of the ConstSpec or VarSpec (ShortVarDecl for
     * short variable declarations) and ends at the end of the innermost
     * containing block." */
    TokenPos scope_pos = ast_node_end(GT_AT(orig_rhs, orig_rhs.len - 1, AstExpr));
    for (Int i = 0; i < new_vars.len; i++)
        burrow__tc_declare(check, scope, NULL,
                           types_object(GT_AT(new_vars, i, TypesVar *)),
                           scope_pos); /* id = nil: recordDef already called */
}
