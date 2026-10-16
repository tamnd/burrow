/* go/types: typechecking of range statements.
 *
 * Derived from Go's src/go/types/range.go. Go source: go1.27.1.
 *
 * Copyright 2025 The Go Authors. All rights reserved.
 * Copyright 2026 The burrow Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style licence that can be found
 * in the LICENSE file. */

#include "burrow/go/types.h"

#include "burrow/core.h"
#include "burrow/func.h"
#include "burrow/go/ast.h"
#include "burrow/go/token.h"
#include "burrow/map.h"
#include "burrow/slice.h"

#include "types_check.h"
#include "types_internal.h"

#include <stdbool.h>

/* A channel must permit receive operations. */
static bool trg_recv_cond(void *env, TypesType t, TypesType u, Str *cause) {
    TypesChecker *check = env;
    TypesChan *ch = types_as_chan(u);
    if (ch != NULL && ch->dir == TYPES_SEND_ONLY) {
        Str ts = burrow__tc_ts(check, t);
        *cause = GT_SPRINTF(check, "receive from send-only channel %s", ts);
        return false;
    }
    return true;
}

static bool trg_bad(TypesType *key, TypesType *val, Str *cause, Str why) {
    *key = GT_TYP(TYPES_INVALID);
    *val = GT_TYP(TYPES_INVALID);
    *cause = why;
    return false;
}

static TypesType trg_param_type(TypesTuple *t, Int i) {
    return GT_OBJ(types_tuple_at(t, i))->typ;
}

/* rangeKeyVal returns the key and value type produced by a range clause over
 * an expression of type orig. The language version is checked with
 * allowVersion. If the range clause is not permitted, rangeKeyVal returns
 * ok = false. When ok = false, rangeKeyVal may also return a reason in cause.
 *
 * Not in Go: allowVersion is always the checker's, and check is not NULL. */
static bool trg_range_key_val(TypesChecker *check, TypesType orig, TypesType *key,
                              TypesType *val, Str *cause) {
    *key = NULL;
    *val = NULL;
    *cause = BURROW_STR_EMPTY;

    Str err = BURROW_STR_EMPTY;
    TypesType rtyp = burrow__tc_common_under(
        check, orig, BURROW_FN(GtUnderCond, trg_recv_cond, check), &err);
    if (rtyp == NULL)
        return trg_bad(key, val, cause, err);

    TypesType typ = burrow__tc_array_ptr_deref(rtyp);
    Int kind = typ->kind;
    switch (kind) {
    case TYPES_KIND_BASIC:
        if (burrow__types_is_basic(typ, TYPES_IS_STRING)) {
            *key = GT_TYP(TYPES_INT);
            *val = types_type(burrow__types_universe_rune); /* use 'rune' name */
            return true;
        }
        if (burrow__types_is_basic(typ, TYPES_IS_INTEGER)) {
            if (!burrow__tc_allow_version(check, burrow__tc_go1_22))
                return trg_bad(key, val, cause, BURROW_S("requires go1.22 or later"));
            *key = orig;
            return true;
        }
        break;
    case TYPES_KIND_ARRAY:
        *key = GT_TYP(TYPES_INT);
        *val = GT_KNOWN(TypesArray, typ)->elem;
        return true;
    case TYPES_KIND_SLICE:
        *key = GT_TYP(TYPES_INT);
        *val = GT_KNOWN(TypesSlice, typ)->elem;
        return true;
    case TYPES_KIND_MAP:
        *key = GT_KNOWN(TypesMap, typ)->key;
        *val = GT_KNOWN(TypesMap, typ)->elem;
        return true;
    case TYPES_KIND_CHAN:
        burrow__types_assert(GT_KNOWN(TypesChan, typ)->dir != TYPES_SEND_ONLY);
        *key = GT_KNOWN(TypesChan, typ)->elem;
        return true;
    case TYPES_KIND_SIGNATURE: {
        TypesSignature *sig = GT_KNOWN(TypesSignature, typ);
        if (!burrow__tc_allow_version(check, burrow__tc_go1_23))
            return trg_bad(key, val, cause, BURROW_S("requires go1.23 or later"));
        /* check iterator arity
         * TODO(gri) error messages could be less verbose (consider rangeStmt
         * error and cause returned here) */
        if (types_tuple_len(sig->params) != 1)
            return trg_bad(
                key, val, cause,
                BURROW_S(
                    "func must be func(yield func(...) bool): wrong argument count"));
        if (types_tuple_len(sig->results) != 0)
            return trg_bad(
                key, val, cause,
                BURROW_S(
                    "func must be func(yield func(...) bool): unexpected results"));
        burrow__types_assert(sig->recv == NULL);
        /* check iterator argument type */
        Str uerr = BURROW_STR_EMPTY;
        TypesType u = burrow__tc_common_under(check, trg_param_type(sig->params, 0),
                                              (GtUnderCond){0}, &uerr);
        TypesSignature *cb = types_as_signature(u);
        if (cb == NULL) {
            /* Not in Go: a nil error is one with no text */
            if (uerr.len != 0)
                return trg_bad(key, val, cause,
                               GT_SPRINTF(check,
                                          "func must be func(yield func(...) bool): in "
                                          "yield type, %s",
                                          uerr));
            return trg_bad(
                key, val, cause,
                BURROW_S(
                    "func must be func(yield func(...) bool): argument is not func"));
        }
        if (types_tuple_len(cb->params) > 2)
            return trg_bad(
                key, val, cause,
                BURROW_S("func must be func(yield func(...) bool): yield func "
                         "has too many parameters"));
        if (types_tuple_len(cb->results) != 1 ||
            !types_identical(trg_param_type(cb->results, 0), GT_TYP(TYPES_BOOL))) {
            /* see go.dev/issues/71131, go.dev/issues/71164 */
            if (types_tuple_len(cb->results) == 1 &&
                burrow__types_is_basic(trg_param_type(cb->results, 0),
                                       TYPES_IS_BOOLEAN))
                return trg_bad(
                    key, val, cause,
                    BURROW_S("func must be func(yield func(...) bool): yield "
                             "func returns user-defined boolean, not bool"));
            return trg_bad(
                key, val, cause,
                BURROW_S("func must be func(yield func(...) bool): yield func "
                         "does not return bool"));
        }
        if (cb->variadic) {
            Str ts = burrow__tc_ts(check, types_type(cb));
            return trg_bad(
                key, val, cause,
                GT_SPRINTF(check, "yield func of type %s cannot be variadic", ts));
        }
        burrow__types_assert(cb->recv == NULL);
        /* determine key and value types, if any */
        if (types_tuple_len(cb->params) >= 1)
            *key = trg_param_type(cb->params, 0);
        if (types_tuple_len(cb->params) >= 2)
            *val = trg_param_type(cb->params, 1);
        return true;
    }
    default:
        break;
    }
    return false;
}

static bool trg_is_invalid(TypesType typ) {
    return typ == NULL || typ == GT_TYP(TYPES_INVALID);
}

static bool trg_is_integer(TypesType typ) {
    return burrow__types_is_basic(typ, TYPES_IS_INTEGER);
}

/* rangeStmt type-checks a range statement of form
 *
 *	for sKey, sValue = range rangeVar { ... }
 *
 * where sKey, sValue, sExtra may be nil. isDef indicates whether these
 * variables are assigned to only (=) or whether there is a short variable
 * declaration (:=). If the latter and there are no variables, an error is
 * reported at noNewVarPos. */
void burrow__tc_range_stmt(TypesChecker *check, GtStmtContext inner,
                           AstRangeStmt *range_stmt, GtPosn no_new_var_pos,
                           AstExpr s_key, AstExpr s_value, AstExpr s_extra,
                           AstExpr range_var, bool is_def) {
    /* check expression to iterate over */
    GtOperand x = {0};

    /* From the spec:
     *   The range expression x is evaluated before beginning the loop,
     *   with one exception: if at most one iteration variable is present
     *   and x or len(x) is constant, the range expression is not evaluated.
     * So we have to be careful not to evaluate the arg in the described
     * situation.
     *
     * Not in Go: what types2 records here is left out, as go/types does. */
    check->env.has_call_or_recv = false;
    burrow__tc_expr(check, NULL, &x, range_var);

    /* determine key/value types */
    TypesType key = NULL;
    TypesType val = NULL;
    if (burrow__tc_op_valid(&x)) {
        TypesType k = NULL;
        TypesType v = NULL;
        Str cause = BURROW_STR_EMPTY;
        bool ok = trg_range_key_val(check, x.typ, &k, &v, &cause);
        if (!ok && cause.len != 0) {
            Str xs = burrow__tc_xs(check, &x);
            GT_SOFT_ERRORF(check, burrow__tc_at_op(&x), GT_CODE_INVALID_RANGE_EXPR,
                           "cannot range over %s: %s", xs, cause);
        } else if (!ok) {
            Str xs = burrow__tc_xs(check, &x);
            GT_SOFT_ERRORF(check, burrow__tc_at_op(&x), GT_CODE_INVALID_RANGE_EXPR,
                           "cannot range over %s", xs);
        } else if (k == NULL && s_key != NULL) {
            Str xs = burrow__tc_xs(check, &x);
            GT_SOFT_ERRORF(check, burrow__tc_at_node(s_key), GT_CODE_INVALID_ITER_VAR,
                           "range over %s permits no iteration variables", xs);
        } else if (v == NULL && s_value != NULL) {
            Str xs = burrow__tc_xs(check, &x);
            GT_SOFT_ERRORF(check, burrow__tc_at_node(s_value), GT_CODE_INVALID_ITER_VAR,
                           "range over %s permits only one iteration variable", xs);
        } else if (s_extra != NULL) {
            burrow__tc_soft_error(
                check, burrow__tc_at_node(s_extra), GT_CODE_INVALID_ITER_VAR,
                BURROW_S("range clause permits at most two iteration variables"));
        }
        key = k;
        val = v;
    }

    /* Open the for-statement block scope now, after the range clause.
     * Iteration variables declared with := need to go in this scope (was
     * go.dev/issue/51437). */
    burrow__tc_open_scope(check, GT_NODE(range_stmt), BURROW_S("range"));

    /* check assignment to/declaration of iteration variables (irregular
     * assignment, cannot easily map to existing assignment checks) */

    /* lhs expressions and initialization value (rhs) types */
    AstExpr lhs[2] = {s_key, s_value}; /* sKey, sValue may be nil */
    TypesType rhs[2] = {key, val};     /* key, val may be nil */

    bool range_over_int = trg_is_integer(x.typ);

    if (is_def) {
        /* short variable declaration */
        Slice vars = slice_nil(TYPE_UNSAFE_POINTER); /* of TypesVar * */
        for (Int i = 0; i < 2; i++) {
            AstExpr e = lhs[i];
            if (e == NULL)
                continue;

            /* determine lhs variable */
            TypesVar *obj;
            if (e->kind == AST_KIND_IDENT) {
                AstIdent *ident = (AstIdent *)(void *)e;
                /* declare new variable */
                Str name = ident->name;
                obj = burrow__types_new_var(check->a, TYPES_LOCAL_VAR, ident->name_pos,
                                            check->pkg, name, NULL);
                burrow__tc_record_def(check, ident, types_object(obj));
                /* _ variables don't count as new variables */
                if (!str_eq(name, BURROW_S("_")))
                    burrow__types_ptrs_append(check->a, &vars, obj);
            } else {
                Str es = burrow__tc_es(check, e);
                GT_ERRORF(check, burrow__tc_at_node(e), GT_CODE_INVALID_SYNTAX_TREE,
                          "cannot declare %s", es);
                /* dummy variable */
                obj = burrow__types_new_var(check->a, TYPES_LOCAL_VAR, ast_node_pos(e),
                                            check->pkg, BURROW_S("_"), NULL);
            }
            burrow__types_assert(GT_OBJ(obj)->typ == NULL);

            /* initialize lhs iteration variable, if any */
            TypesType typ = rhs[i];
            if (trg_is_invalid(typ)) {
                /* typ == Typ[Invalid] can happen if allowVersion fails. */
                GT_OBJ(obj)->typ = GT_TYP(TYPES_INVALID);
                /* don't complain about unused variable */
                bool yes = true;
                if (!map_set(check->used_vars, &obj, &yes))
                    burrow__types_oom();
                continue;
            }

            if (range_over_int) {
                /* at most one iteration variable (rhs[1] == nil or
                 * Typ[Invalid] for rangeOverInt) */
                burrow__types_assert(i == 0);
                burrow__tc_init_var(check, obj, &x, BURROW_S("range clause"));
            } else {
                GtOperand y = {0};
                y.mode = GT_MODE_VALUE;
                y.expr = e; /* we don't have a better rhs expression to use here */
                y.typ = typ;
                /* error is on variable, use "assignment" not "range clause" */
                burrow__tc_init_var(check, obj, &y, BURROW_S("assignment"));
            }
            burrow__types_assert(GT_OBJ(obj)->typ != NULL);
        }

        /* declare variables */
        if (vars.len > 0) {
            TokenPos scope_pos = ast_node_pos(GT_NODE(range_stmt->body));
            for (Int i = 0; i < vars.len; i++)
                /* recordDef already called */
                burrow__tc_declare(check, check->env.scope, NULL,
                                   types_object(GT_AT(vars, i, TypesVar *)), scope_pos);
        } else {
            burrow__tc_error(check, no_new_var_pos, GT_CODE_NO_NEW_VAR,
                             BURROW_S("no new variables on left side of :="));
        }
    } else if (s_key != NULL /* lhs[0] != nil */) {
        /* ordinary assignment */
        for (Int i = 0; i < 2; i++) {
            AstExpr e = lhs[i];
            if (e == NULL)
                continue;

            /* assign to lhs iteration variable, if any */
            TypesType typ = rhs[i];
            if (trg_is_invalid(typ))
                continue;

            if (range_over_int) {
                /* at most one iteration variable (rhs[1] == nil or
                 * Typ[Invalid] for rangeOverInt) */
                burrow__types_assert(i == 0);
                burrow__tc_assign_var(check, e, NULL, &x, BURROW_S("range clause"));
                /* If the assignment succeeded, if x was untyped before, it now
                 * has a type inferred via the assignment. It must be an
                 * integer. (go.dev/issues/67027) */
                if (burrow__tc_op_valid(&x) && !trg_is_integer(x.typ)) {
                    Str ts = burrow__tc_ts(check, x.typ);
                    GT_SOFT_ERRORF(check, burrow__tc_at_node(e),
                                   GT_CODE_INVALID_RANGE_EXPR,
                                   "cannot use iteration variable of type %s", ts);
                }
            } else {
                GtOperand y = {0};
                y.mode = GT_MODE_VALUE;
                y.expr = e; /* we don't have a better rhs expression to use here */
                y.typ = typ;
                /* error is on variable, use "assignment" not "range clause" */
                burrow__tc_assign_var(check, e, NULL, &y, BURROW_S("assignment"));
            }
        }
    } else if (range_over_int) {
        /* If we don't have any iteration variables, we still need to check
         * that a (possibly untyped) integer range expression x is valid. We do
         * this by checking the assignment _ = x. This ensures that an untyped x
         * can be converted to a value of its default type (rune or int). */
        burrow__tc_assignment(check, &x, NULL, BURROW_S("range clause"));
    }

    burrow__tc_stmt(check, inner, GT_NODE(range_stmt->body));

    burrow__tc_close_scope(check);
}
