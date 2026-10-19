/* go/types: typechecking of statements.
 *
 * Derived from Go's src/go/types/stmt.go. Go source: go1.27.1.
 *
 * Copyright 2012 The Go Authors. All rights reserved.
 * Copyright 2026 The burrow Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style licence that can be found
 * in the LICENSE file. */

#include "burrow/go/types.h"

#include "burrow/core.h"
#include "burrow/go/ast.h"
#include "burrow/go/constant.h"
#include "burrow/go/token.h"
#include "burrow/map.h"
#include "burrow/panic.h"
#include "burrow/slice.h"

#include "types_check.h"
#include "types_internal.h"

#include <stdbool.h>
#include <stdint.h>

static bool tsm_need_use(TypesVarKind kind) {
    return !(kind == TYPES_RECV_VAR || kind == TYPES_PARAM_VAR ||
             kind == TYPES_RESULT_VAR);
}

static bool tsm_is_used(TypesChecker *check, TypesVar *v) {
    const bool *used = map_get(check->used_vars, &v);
    return used != NULL && *used;
}

static void tsm_set_used(TypesChecker *check, TypesVar *v) {
    bool yes = true;
    if (!map_set(check->used_vars, &v, &yes))
        burrow__types_oom();
}

static void tsm_usage(TypesChecker *check, TypesScope *scope) {
    Slice names = types_scope_names(check->a, scope);
    Slice unused = slice_nil(TYPE_UNSAFE_POINTER);
    for (Int i = 0; i < names.len; i++) {
        TypesVar *v = types_as_var(types_scope_lookup(scope, GT_AT(names, i, Str)));
        if (v != NULL && tsm_need_use(v->kind) && !tsm_is_used(check, v))
            burrow__types_ptrs_append(check->a, &unused, v);
    }
    /* Not in Go: an insertion sort for slices.SortFunc by position */
    for (Int i = 1; i < unused.len; i++) {
        TypesVar *v = GT_AT(unused, i, TypesVar *);
        Int j = i;
        for (; j > 0 && GT_OBJ(GT_AT(unused, j - 1, TypesVar *))->pos > GT_OBJ(v)->pos;
             j--)
            GT_AT(unused, j, TypesVar *) = GT_AT(unused, j - 1, TypesVar *);
        GT_AT(unused, j, TypesVar *) = v;
    }
    for (Int i = 0; i < unused.len; i++) {
        TypesVar *v = GT_AT(unused, i, TypesVar *);
        Str name = GT_OBJ(v)->name;
        GT_SOFT_ERRORF(check, burrow__tc_at_obj(types_object(v)), GT_CODE_UNUSED_VAR,
                       "declared and not used: %s", name);
    }

    for (Int i = 0; i < types_scope_num_children(scope); i++) {
        TypesScope *child = types_scope_child(scope, i);
        /* Don't go inside function literal scopes a second time; they are
         * handled explicitly by funcBody. */
        if (!child->is_func)
            tsm_usage(check, child);
    }
}

/* decl may be NULL */
void burrow__tc_func_body(TypesChecker *check, GtDeclInfo *decl, Str name,
                          TypesSignature *sig, AstBlockStmt *body,
                          const ConstantValue *iota) {
    (void)name; /* Not in Go: there is no tracing */
    if (check->conf->ignore_func_bodies)
        panic_str(BURROW_S("function body not ignored"));

    /* save/restore current environment and set up function environment */
    GtEnv saved = check->env;
    ConstantValue *env_iota = NULL;
    if (iota != NULL) {
        env_iota = GT_NEW(check->a, ConstantValue);
        *env_iota = *iota;
    }
    check->env = (GtEnv){0};
    check->env.decl = decl;
    check->env.scope = sig->scope;
    /* TODO(adonovan): would decl.version (if decl != nil) be better? */
    check->env.version = saved.version;
    check->env.iota = env_iota;
    check->env.sig = sig;

    burrow__tc_stmt_list(check, 0, body->list);

    if (check->env.has_label)
        burrow__tc_labels(check, body);

    if (types_tuple_len(sig->results) > 0 &&
        !burrow__tc_is_terminating(check, GT_NODE(body), BURROW_STR_EMPTY))
        burrow__tc_error(check, burrow__tc_at_pos(body->rbrace), GT_CODE_MISSING_RETURN,
                         BURROW_S("missing return"));

    /* spec: "Implementation restriction: A compiler may make it illegal to
     * declare a variable inside a function body if the variable is never
     * used." */
    tsm_usage(check, sig->scope);

    check->env = saved;
}

static void tsm_simple_stmt(TypesChecker *check, AstStmt s) {
    if (s != NULL)
        burrow__tc_stmt(check, 0, s);
}

static Slice tsm_trim_trailing_empty_stmts(Slice list) {
    for (Int i = list.len; i > 0; i--) {
        if (GT_AT(list, i - 1, AstStmt)->kind != AST_KIND_EMPTY_STMT)
            return slice_sub(list, 0, i);
    }
    return slice_sub(list, 0, 0);
}

void burrow__tc_stmt_list(TypesChecker *check, GtStmtContext ctxt, Slice list) {
    bool ok = (ctxt & GT_FALLTHROUGH_OK) != 0;
    GtStmtContext inner = ctxt & ~GT_FALLTHROUGH_OK;
    /* trailing empty statements are "invisible" to fallthrough analysis */
    list = tsm_trim_trailing_empty_stmts(list);
    for (Int i = 0; i < list.len; i++) {
        GtStmtContext in = inner;
        if (ok && i + 1 == list.len)
            in |= GT_FALLTHROUGH_OK;
        burrow__tc_stmt(check, in, GT_AT(list, i, AstStmt));
    }
}

static void tsm_multiple_defaults(TypesChecker *check, Slice list) {
    AstStmt first = NULL;
    for (Int i = 0; i < list.len; i++) {
        AstStmt s = GT_AT(list, i, AstStmt);
        AstStmt d = NULL;
        Int kind = s->kind;
        switch (kind) {
        case AST_KIND_CASE_CLAUSE:
            if (((AstCaseClause *)(void *)s)->list.len == 0)
                d = s;
            break;
        case AST_KIND_COMM_CLAUSE:
            if (((AstCommClause *)(void *)s)->comm == NULL)
                d = s;
            break;
        default:
            burrow__tc_error(check, burrow__tc_at_node(s), GT_CODE_INVALID_SYNTAX_TREE,
                             BURROW_S("case/communication clause expected"));
            break;
        }
        if (d != NULL) {
            if (first != NULL) {
                Str ps = burrow__tc_ps(check, ast_node_pos(first));
                GT_ERRORF(check, burrow__tc_at_node(d), GT_CODE_DUPLICATE_DEFAULT,
                          "multiple defaults (first at %s)", ps);
            } else {
                first = d;
            }
        }
    }
}

void burrow__tc_open_scope(TypesChecker *check, AstNode node, Str comment) {
    TypesScope *scope = types_new_scope(check->a, check->env.scope, ast_node_pos(node),
                                        ast_node_end(node), comment);
    burrow__tc_record_scope(check, node, scope);
    check->env.scope = scope;
}

void burrow__tc_close_scope(TypesChecker *check) {
    check->env.scope = types_scope_parent(check->env.scope);
}

Token burrow__tc_assign_op(Token op) {
    /* token_test.go verifies the token ordering this function relies on */
    if (TOKEN_ADD_ASSIGN <= op && op <= TOKEN_AND_NOT_ASSIGN)
        return op + (TOKEN_ADD - TOKEN_ADD_ASSIGN);
    return TOKEN_ILLEGAL;
}

static void tsm_suspended_call(TypesChecker *check, Str keyword, AstCallExpr *call) {
    GtOperand x = {0};
    Str msg;
    GtCode code;
    GtExprKind kind = burrow__tc_raw_expr(check, NULL, &x, GT_NODE(call), NULL, false);
    switch (kind) {
    case GT_EXPR_CONVERSION:
        msg = BURROW_S("requires function call, not conversion");
        code = GT_CODE_INVALID_DEFER;
        if (str_eq(keyword, BURROW_S("go")))
            code = GT_CODE_INVALID_GO;
        break;
    case GT_EXPR_EXPRESSION:
        msg = BURROW_S("discards result of");
        code = GT_CODE_UNUSED_RESULTS;
        break;
    case GT_EXPR_STATEMENT:
        return;
    default:
        panic_str(BURROW_S("unreachable"));
    }
    Str xs = burrow__tc_xs(check, &x);
    GT_ERRORF(check, burrow__tc_at_op(&x), code, "%s %s %s", keyword, msg, xs);
}

/* The Go value of a case constant, which is Go's any of int64, uint64,
 * float64 or string. */
typedef enum TsmValKind {
    TSM_VAL_NONE,
    TSM_VAL_INT64,
    TSM_VAL_UINT64,
    TSM_VAL_FLOAT64,
    TSM_VAL_STRING
} TsmValKind;

typedef struct TsmVal {
    TsmValKind kind;
    int64_t i;
    uint64_t u;
    double f;
    Str s;
} TsmVal;

/* goVal returns the Go value for val, or one of kind TSM_VAL_NONE. */
static TsmVal tsm_go_val(ConstantValue val) {
    TsmVal v = {0};
    bool ok = false;
    /* Match implementation restriction of other compilers. gc only checks
     * duplicates for integer, floating-point and string values, so only create
     * Go values for these types. */
    ConstantKind kind = constant_value_kind(val);
    switch (kind) {
    case CONSTANT_INT:
        v.i = constant_int64_val(val, &ok);
        if (ok) {
            v.kind = TSM_VAL_INT64;
            return v;
        }
        v.u = constant_uint64_val(val, &ok);
        if (ok) {
            v.kind = TSM_VAL_UINT64;
            return v;
        }
        break;
    case CONSTANT_FLOAT:
        v.f = constant_float64_val(val, &ok);
        if (ok) {
            v.kind = TSM_VAL_FLOAT64;
            return v;
        }
        break;
    case CONSTANT_STRING:
        v.kind = TSM_VAL_STRING;
        v.s = constant_string_val(val);
        return v;
    default:
        break;
    }
    return (TsmVal){0};
}

/* Go's == on two map keys of type any. */
static bool tsm_val_eq(const TsmVal *x, const TsmVal *y) {
    if (x->kind != y->kind)
        return false;
    switch (x->kind) {
    case TSM_VAL_INT64:
        return x->i == y->i;
    case TSM_VAL_UINT64:
        return x->u == y->u;
    case TSM_VAL_FLOAT64:
        return x->f == y->f;
    case TSM_VAL_STRING:
        return str_eq(x->s, y->s);
    case TSM_VAL_NONE:
    default:
        return false;
    }
}

/* A valueMap maps a case value (of a basic Go type) to a list of positions
 * where the same case value appeared, together with the corresponding case
 * types. Since two case values may have the same "underlying" value but
 * different types we need to also check the value's types (e.g., byte(1) vs
 * myByte(1)) when the switch expression is of interface type.
 *
 * Not in Go: the map is a list of these, searched in the order they were
 * added, which is the order of Go's per-value lists. */
typedef struct TsmValueType {
    TsmVal val;
    TokenPos pos;
    TypesType typ;
} TsmValueType;

static void tsm_case_values(TypesChecker *check, GtOperand *x, Slice values,
                            Slice *seen) {
    for (Int i = 0; i < values.len; i++) {
        AstExpr e = GT_AT(values, i, AstExpr);
        GtOperand v = {0};
        burrow__tc_expr(check, NULL, &v, e);
        if (!burrow__tc_op_valid(x) || !burrow__tc_op_valid(&v))
            continue;
        burrow__tc_convert_untyped(check, &v, x->typ);
        if (!burrow__tc_op_valid(&v))
            continue;
        /* Order matters: By comparing v against x, error positions are at the
         * case values. */
        GtOperand res = v; /* keep original v unchanged */
        burrow__tc_comparison(check, &res, x, TOKEN_EQL, true);
        if (!burrow__tc_op_valid(&res))
            continue;
        if (v.mode != GT_MODE_CONSTANT)
            continue; /* we're done */
        /* look for duplicate values */
        TsmVal val = tsm_go_val(v.val);
        if (val.kind == TSM_VAL_NONE)
            continue;
        /* look for duplicate types for a given value (quadratic algorithm, but
         * these lists tend to be very short) */
        bool dup = false;
        for (Int j = 0; j < seen->len && !dup; j++) {
            TsmValueType *vt = GT_AT(*seen, j, TsmValueType *);
            if (tsm_val_eq(&vt->val, &val) && types_identical(v.typ, vt->typ)) {
                GtError *err = burrow__tc_new_error(check, GT_CODE_DUPLICATE_CASE);
                Str vs = burrow__tc_xs(check, &v);
                GT_ADDF(err, burrow__tc_at_op(&v),
                        "duplicate case %s in expression switch", vs);
                burrow__tc_error_add(err, burrow__tc_at_pos(vt->pos),
                                     BURROW_S("previous case"));
                burrow__tc_error_report(err);
                dup = true;
            }
        }
        if (dup)
            continue;
        TsmValueType *vt = GT_NEW(check->a, TsmValueType);
        vt->val = val;
        vt->pos = burrow__tc_op_pos(&v);
        vt->typ = v.typ;
        burrow__types_ptrs_append(check->a, seen, vt);
    }
}

/* isNil reports whether the expression e denotes the predeclared value nil. */
static bool tsm_is_nil(TypesChecker *check, AstExpr e) {
    /* The only way to express the nil value is by literally writing nil
     * (possibly in parentheses). */
    AstExpr u = ast_unparen(e);
    if (u != NULL && u->kind == AST_KIND_IDENT) {
        AstIdent *name = (AstIdent *)(void *)u;
        return types_as_nil(burrow__tc_env_lookup(&check->env, name->name)) != NULL;
    }
    return false;
}

/* A seen type of a type switch; typ is NULL for the nil case. Not in Go: the
 * map is a list of these. */
typedef struct TsmSeenType {
    TypesType typ;
    AstExpr e;
} TsmSeenType;

/* caseTypes typechecks the type expressions of a type case, checks for
 * duplicate types using the seen map, and verifies that each type is valid
 * with respect to the type of the operand x corresponding to the type switch
 * expression. If that expression is not valid, x must be nil.
 *
 *	switch <x>.(type) {
 *	case <types>: ...
 *	...
 *	}
 *
 * caseTypes returns the case-specific type for a variable v introduced
 * through a short variable declaration by the type switch:
 *
 *	switch v := <x>.(type) {
 *	case <types>: // T is the type of <v> in this case
 *	...
 *	}
 *
 * If there is exactly one type expression, T is the type of that expression.
 * If there are multiple type expressions, or if predeclared nil is among the
 * types, the result is the type of x. If x is invalid (nil), the result is the
 * invalid type. */
static TypesType tsm_case_types(TypesChecker *check, GtOperand *x, Slice types,
                                Slice *seen) {
    TypesType T = NULL;
    GtOperand dummy = {0};
    for (Int i = 0; i < types.len; i++) {
        AstExpr e = GT_AT(types, i, AstExpr);
        /* The spec allows the value nil instead of a type. */
        if (tsm_is_nil(check, e)) {
            T = NULL;
            /* run e through expr so we get the usual Info recordings */
            burrow__tc_expr(check, NULL, &dummy, e);
        } else {
            T = burrow__tc_var_type(check, e);
            if (!burrow__types_is_valid(T))
                continue;
        }
        /* look for duplicate types (quadratic algorithm, but type switches
         * tend to be reasonably small) */
        bool dup = false;
        for (Int j = 0; j < seen->len && !dup; j++) {
            TsmSeenType *other = GT_AT(*seen, j, TsmSeenType *);
            TypesType t = other->typ;
            if ((T == NULL && t == NULL) ||
                (T != NULL && t != NULL && types_identical(T, t))) {
                /* talk about "case" rather than "type" because of nil case */
                Str Ts = BURROW_S("nil");
                if (T != NULL)
                    Ts = burrow__tc_ts(check, T);
                GtError *err = burrow__tc_new_error(check, GT_CODE_DUPLICATE_CASE);
                GT_ADDF(err, burrow__tc_at_node(e), "duplicate case %s in type switch",
                        Ts);
                burrow__tc_error_add(err, burrow__tc_at_node(other->e),
                                     BURROW_S("previous case"));
                burrow__tc_error_report(err);
                dup = true;
            }
        }
        if (dup)
            continue;
        TsmSeenType *st = GT_NEW(check->a, TsmSeenType);
        st->typ = T;
        st->e = e;
        burrow__types_ptrs_append(check->a, seen, st);
        if (x != NULL && T != NULL)
            burrow__tc_type_assertion(check, e, x, T, true);
    }

    /* spec: "In clauses with a case listing exactly one type, the variable has
     * that type; otherwise, the variable has the type of the expression in the
     * TypeSwitchGuard. */
    if (types.len != 1 || T == NULL) {
        T = GT_TYP(TYPES_INVALID);
        if (x != NULL)
            T = x->typ;
    }

    burrow__types_assert(T != NULL);
    return T;
}

static void tsm_expr_stmt(TypesChecker *check, AstExprStmt *s) {
    /* spec: "With the exception of specific built-in functions, function and
     * method calls and receive operations can appear in statement context.
     * Such statements may be parenthesized." */
    GtOperand x = {0};
    GtExprKind kind = burrow__tc_raw_expr(check, NULL, &x, s->x, NULL, false);
    Str msg;
    GtCode code;
    if (x.mode == GT_MODE_BUILTIN) {
        msg = BURROW_S("must be called");
        code = GT_CODE_UNCALLED_BUILTIN;
    } else if (x.mode == GT_MODE_TYPEXPR) {
        msg = BURROW_S("is not an expression");
        code = GT_CODE_NOT_AN_EXPR;
    } else {
        if (kind == GT_EXPR_STATEMENT)
            return;
        msg = BURROW_S("is not used");
        code = GT_CODE_UNUSED_EXPR;
    }
    Str xs = burrow__tc_xs(check, &x);
    GT_ERRORF(check, burrow__tc_at_op(&x), code, "%s %s", xs, msg);
}

static void tsm_send_stmt(TypesChecker *check, AstSendStmt *s) {
    GtOperand ch = {0};
    GtOperand val = {0};
    burrow__tc_expr(check, NULL, &ch, s->chan);
    burrow__tc_generic_expr(check, &val, s->value, NULL);
    if (!burrow__tc_op_valid(&ch) || !burrow__tc_op_valid(&val))
        return;
    TypesType elem = burrow__tc_chan_elem(
        check, burrow__tc_in_node(GT_NODE(s), s->arrow), &ch, false);
    if (elem != NULL)
        burrow__tc_assignment(check, &val, elem, BURROW_S("send"));
}

static void tsm_inc_dec_stmt(TypesChecker *check, AstIncDecStmt *s) {
    Token op;
    Token tok = s->tok;
    switch (tok) {
    case TOKEN_INC:
        op = TOKEN_ADD;
        break;
    case TOKEN_DEC:
        op = TOKEN_SUB;
        break;
    default: {
        Str ts = token_string(tok, check->a);
        GT_ERRORF(check, burrow__tc_in_node(GT_NODE(s), s->tok_pos),
                  GT_CODE_INVALID_SYNTAX_TREE, "unknown inc/dec operation %s", ts);
        return;
    }
    }

    GtOperand x = {0};
    burrow__tc_expr(check, NULL, &x, s->x);
    if (!burrow__tc_op_valid(&x))
        return;
    if (!burrow__types_all_basic(x.typ, TYPES_IS_NUMERIC)) {
        Str es = burrow__tc_es(check, s->x);
        Str ts = token_string(tok, check->a);
        Str xt = burrow__tc_ts(check, x.typ);
        GT_ERRORF(check, burrow__tc_at_node(s->x), GT_CODE_NON_NUMERIC_INC_DEC,
                  GT_INVALID_OP "%s%s (non-numeric type %s)", es, ts, xt);
        return;
    }

    /* use x's position */
    AstBasicLit *Y = (AstBasicLit *)(void *)ast_node_new(check->a, AST_KIND_BASIC_LIT);
    if (Y == NULL)
        burrow__types_oom();
    Y->value_pos = ast_node_pos(s->x);
    Y->kind = TOKEN_INT;
    Y->value = BURROW_S("1");
    burrow__tc_binary(check, &x, NULL, s->x, GT_NODE(Y), op, s->tok_pos);
    if (!burrow__tc_op_valid(&x))
        return;
    burrow__tc_assign_var(check, s->x, NULL, &x, BURROW_S("assignment"));
}

static void tsm_assign_stmt(TypesChecker *check, AstAssignStmt *s) {
    Token tok = s->tok;
    if (tok == TOKEN_ASSIGN || tok == TOKEN_DEFINE) {
        if (s->lhs.len == 0) {
            burrow__tc_error(check, GT_AT_NODE(s), GT_CODE_INVALID_SYNTAX_TREE,
                             BURROW_S("missing lhs in assignment"));
            return;
        }
        if (tok == TOKEN_DEFINE)
            burrow__tc_short_var_decl(check, burrow__tc_in_node(GT_NODE(s), s->tok_pos),
                                      s->lhs, s->rhs);
        else
            /* regular assignment */
            burrow__tc_assign_vars(check, s->lhs, s->rhs);
        return;
    }

    /* assignment operations */
    if (s->lhs.len != 1 || s->rhs.len != 1) {
        Str ts = token_string(tok, check->a);
        GT_ERRORF(check, burrow__tc_in_node(GT_NODE(s), s->tok_pos),
                  GT_CODE_MULTI_VAL_ASSIGN_OP,
                  "assignment operation %s requires single-valued expressions", ts);
        return;
    }
    Token op = burrow__tc_assign_op(tok);
    if (op == TOKEN_ILLEGAL) {
        Str ts = token_string(tok, check->a);
        GT_ERRORF(check, burrow__tc_at_pos(s->tok_pos), GT_CODE_INVALID_SYNTAX_TREE,
                  "unknown assignment operation %s", ts);
        return;
    }
    GtOperand x = {0};
    AstExpr lhs0 = GT_AT(s->lhs, 0, AstExpr);
    burrow__tc_binary(check, &x, NULL, lhs0, GT_AT(s->rhs, 0, AstExpr), op, s->tok_pos);
    if (!burrow__tc_op_valid(&x))
        return;
    burrow__tc_assign_var(check, lhs0, NULL, &x, BURROW_S("assignment"));
}

static void tsm_return_stmt(TypesChecker *check, AstReturnStmt *s) {
    TypesTuple *res = check->env.sig->results;
    Int n = types_tuple_len(res);
    /* Return with implicit results allowed for function with named results.
     * (If one is named, all are named.) */
    if (s->results.len == 0 && n > 0 && GT_OBJ(types_tuple_at(res, 0))->name.len != 0) {
        /* spec: "Implementation restriction: A compiler may disallow an empty
         * expression list in a "return" statement if a different entity
         * (constant, type, or variable) with the same name as a result
         * parameter is in scope at the place of the return." */
        for (Int i = 0; i < n; i++) {
            TypesVar *obj = types_tuple_at(res, i);
            Str name = GT_OBJ(obj)->name;
            TypesObject alt = burrow__tc_env_lookup(&check->env, name);
            if (alt != NULL && alt != types_object(obj)) {
                GtError *err = burrow__tc_new_error(check, GT_CODE_OUT_OF_SCOPE_RESULT);
                GT_ADDF(err, GT_AT_NODE(s),
                        "result parameter %s not in scope at return", name);
                Str os = burrow__tc_os(check, alt);
                GT_ADDF(err, burrow__tc_at_obj(alt), "inner declaration of %s", os);
                burrow__tc_error_report(err);
                /* ok to continue */
            }
        }
        return;
    }
    Slice lhs = slice_nil(TYPE_UNSAFE_POINTER);
    if (n > 0)
        lhs = res->vars;
    burrow__tc_init_vars(check, lhs, s->results, GT_NODE(s));
}

static void tsm_branch_stmt(TypesChecker *check, GtStmtContext ctxt, AstBranchStmt *s) {
    if (s->label != NULL) {
        check->env.has_label = true;
        return; /* checked in 2nd pass (check.labels) */
    }
    Token tok = s->tok;
    switch (tok) {
    case TOKEN_BREAK:
        if ((ctxt & GT_BREAK_OK) == 0)
            burrow__tc_error(check, GT_AT_NODE(s), GT_CODE_MISPLACED_BREAK,
                             BURROW_S("break not in for, switch, or select statement"));
        break;
    case TOKEN_CONTINUE:
        if ((ctxt & GT_CONTINUE_OK) == 0)
            burrow__tc_error(check, GT_AT_NODE(s), GT_CODE_MISPLACED_CONTINUE,
                             BURROW_S("continue not in for statement"));
        break;
    case TOKEN_FALLTHROUGH:
        if ((ctxt & GT_FALLTHROUGH_OK) == 0) {
            Str msg;
            if ((ctxt & GT_FINAL_SWITCH_CASE) != 0)
                msg = BURROW_S("cannot fallthrough final case in switch");
            else if ((ctxt & GT_IN_TYPE_SWITCH) != 0)
                msg = BURROW_S("cannot fallthrough in type switch");
            else
                msg = BURROW_S("fallthrough statement out of place");
            burrow__tc_error(check, GT_AT_NODE(s), GT_CODE_MISPLACED_FALLTHROUGH, msg);
        }
        break;
    default: {
        Str ts = token_string(tok, check->a);
        GT_ERRORF(check, GT_AT_NODE(s), GT_CODE_INVALID_SYNTAX_TREE,
                  "branch statement: %s", ts);
        break;
    }
    }
}

static void tsm_if_stmt(TypesChecker *check, GtStmtContext inner, AstIfStmt *s) {
    burrow__tc_open_scope(check, GT_NODE(s), BURROW_S("if"));

    tsm_simple_stmt(check, s->init);
    GtOperand x = {0};
    burrow__tc_expr(check, NULL, &x, s->cond);
    if (burrow__tc_op_valid(&x) && !burrow__types_all_basic(x.typ, TYPES_IS_BOOLEAN))
        burrow__tc_error(check, burrow__tc_at_node(s->cond), GT_CODE_INVALID_COND,
                         BURROW_S("non-boolean condition in if statement"));
    burrow__tc_stmt(check, inner, GT_NODE(s->body));
    /* The parser produces a correct AST but if it was modified elsewhere the
     * else branch may be invalid. Check again. */
    if (s->else_ != NULL) {
        Int kind = s->else_->kind;
        switch (kind) {
        case AST_KIND_BAD_STMT:
            /* valid or error already reported */
            break;
        case AST_KIND_IF_STMT:
        case AST_KIND_BLOCK_STMT:
            burrow__tc_stmt(check, inner, s->else_);
            break;
        default:
            burrow__tc_error(check, burrow__tc_at_node(s->else_),
                             GT_CODE_INVALID_SYNTAX_TREE,
                             BURROW_S("invalid else branch in if statement"));
            break;
        }
    }

    burrow__tc_close_scope(check);
}

static void tsm_switch_stmt(TypesChecker *check, GtStmtContext inner,
                            AstSwitchStmt *s) {
    inner |= GT_BREAK_OK;
    burrow__tc_open_scope(check, GT_NODE(s), BURROW_S("switch"));

    tsm_simple_stmt(check, s->init);
    GtOperand x = {0};
    if (s->tag != NULL) {
        burrow__tc_expr(check, NULL, &x, s->tag);
        /* By checking assignment of x to an invisible temporary (as a compiler
         * would), we get all the relevant checks. */
        burrow__tc_assignment(check, &x, NULL, BURROW_S("switch expression"));
        if (burrow__tc_op_valid(&x) && !types_comparable(x.typ) &&
            !burrow__types_has_nil(x.typ)) {
            Str xs = burrow__tc_xs(check, &x);
            Str ts = burrow__tc_ts(check, x.typ);
            GT_ERRORF(check, burrow__tc_at_op(&x), GT_CODE_INVALID_EXPR_SWITCH,
                      "cannot switch on %s (%s is not comparable)", xs, ts);
            burrow__tc_op_invalidate(&x);
        }
    } else {
        /* spec: "A missing switch expression is equivalent to the boolean
         * value true." */
        x.mode = GT_MODE_CONSTANT;
        x.typ = GT_TYP(TYPES_BOOL);
        x.val = constant_make_bool(true);
        AstIdent *id = (AstIdent *)(void *)ast_node_new(check->a, AST_KIND_IDENT);
        if (id == NULL)
            burrow__types_oom();
        id->name_pos = s->body->lbrace;
        id->name = BURROW_S("true");
        x.expr = GT_NODE(id);
    }

    Slice list = s->body->list;
    tsm_multiple_defaults(check, list);

    /* map of seen case values to positions and types */
    Slice seen = slice_nil(TYPE_UNSAFE_POINTER);
    for (Int i = 0; i < list.len; i++) {
        AstStmt c = GT_AT(list, i, AstStmt);
        if (c->kind != AST_KIND_CASE_CLAUSE) {
            burrow__tc_error(check, burrow__tc_at_node(c), GT_CODE_INVALID_SYNTAX_TREE,
                             BURROW_S("incorrect expression switch case"));
            continue;
        }
        AstCaseClause *clause = (AstCaseClause *)(void *)c;
        tsm_case_values(check, &x, clause->list, &seen);
        burrow__tc_open_scope(check, c, BURROW_S("case"));
        GtStmtContext in = inner;
        if (i + 1 < list.len)
            in |= GT_FALLTHROUGH_OK;
        else
            in |= GT_FINAL_SWITCH_CASE;
        burrow__tc_stmt_list(check, in, clause->body);
        burrow__tc_close_scope(check);
    }

    burrow__tc_close_scope(check);
}

static void tsm_bad_guard(TypesChecker *check, AstTypeSwitchStmt *s) {
    burrow__tc_error(check, GT_AT_NODE(s), GT_CODE_INVALID_SYNTAX_TREE,
                     BURROW_S("incorrect form of type switch guard"));
}

static void tsm_type_switch_body(TypesChecker *check, GtStmtContext inner,
                                 AstTypeSwitchStmt *s) {
    tsm_simple_stmt(check, s->init);

    /* A type switch guard must be of the form:
     *
     *     TypeSwitchGuard = [ identifier ":=" ] PrimaryExpr "." "(" "type" ")" .
     *
     * The parser is checking syntactic correctness; remaining syntactic errors
     * are considered AST errors here.
     * TODO(gri) better factoring of error handling (invalid ASTs) */
    AstIdent *lhs = NULL; /* lhs identifier or nil */
    AstExpr rhs = NULL;
    Int guard_kind = s->assign != NULL ? s->assign->kind : AST_KIND_BAD_STMT;
    switch (guard_kind) {
    case AST_KIND_EXPR_STMT:
        rhs = ((AstExprStmt *)(void *)s->assign)->x;
        break;
    case AST_KIND_ASSIGN_STMT: {
        AstAssignStmt *guard = (AstAssignStmt *)(void *)s->assign;
        if (guard->lhs.len != 1 || guard->tok != TOKEN_DEFINE || guard->rhs.len != 1) {
            tsm_bad_guard(check, s);
            return;
        }

        AstExpr lhs0 = GT_AT(guard->lhs, 0, AstExpr);
        if (lhs0 == NULL || lhs0->kind != AST_KIND_IDENT) {
            tsm_bad_guard(check, s);
            return;
        }
        lhs = (AstIdent *)(void *)lhs0;

        if (str_eq(lhs->name, BURROW_S("_"))) {
            /* _ := x.(type) is an invalid short variable declaration */
            burrow__tc_soft_error(check, GT_AT_NODE(lhs), GT_CODE_NO_NEW_VAR,
                                  BURROW_S("no new variable on left side of :="));
            lhs = NULL; /* avoid declared and not used error below */
        } else {
            /* lhs variable is implicitly declared in each cause clause */
            burrow__tc_record_def(check, lhs, NULL);
        }

        rhs = GT_AT(guard->rhs, 0, AstExpr);
        break;
    }
    default:
        tsm_bad_guard(check, s);
        return;
    }

    /* rhs must be of the form: expr.(type) and expr must be an ordinary
     * interface */
    AstTypeAssertExpr *expr = rhs != NULL && rhs->kind == AST_KIND_TYPE_ASSERT_EXPR
                                  ? (AstTypeAssertExpr *)(void *)rhs
                                  : NULL;
    if (expr == NULL || expr->type != NULL) {
        tsm_bad_guard(check, s);
        return;
    }

    /* switch expression against which cases are compared against; nil if
     * invalid */
    GtOperand *sx = NULL;
    GtOperand x = {0};
    burrow__tc_expr(check, NULL, &x, expr->x);
    if (burrow__tc_op_valid(&x)) {
        if (burrow__types_is_type_param(x.typ)) {
            Str xs = burrow__tc_xs(check, &x);
            GT_ERRORF(check, burrow__tc_at_op(&x), GT_CODE_INVALID_TYPE_SWITCH,
                      "cannot use type switch on type parameter value %s", xs);
        } else if (types_is_interface(x.typ)) {
            sx = &x;
        } else {
            Str xs = burrow__tc_xs(check, &x);
            GT_ERRORF(check, burrow__tc_at_op(&x), GT_CODE_INVALID_TYPE_SWITCH,
                      "%s is not an interface", xs);
        }
    }

    Slice list = s->body->list;
    tsm_multiple_defaults(check, list);

    /* list of implicitly declared lhs variables */
    Slice lhs_vars = slice_nil(TYPE_UNSAFE_POINTER);
    /* map of seen types to positions */
    Slice seen = slice_nil(TYPE_UNSAFE_POINTER);
    for (Int i = 0; i < list.len; i++) {
        AstStmt c = GT_AT(list, i, AstStmt);
        if (c->kind != AST_KIND_CASE_CLAUSE) {
            burrow__tc_error(check, burrow__tc_at_node(c), GT_CODE_INVALID_SYNTAX_TREE,
                             BURROW_S("incorrect type switch case"));
            continue;
        }
        AstCaseClause *clause = (AstCaseClause *)(void *)c;
        /* Check each type in this type switch case. */
        TypesType T = tsm_case_types(check, sx, clause->list, &seen);
        burrow__tc_open_scope(check, c, BURROW_S("case"));
        /* If lhs exists, declare a corresponding variable in the case-local
         * scope. */
        if (lhs != NULL) {
            TypesVar *obj = burrow__types_new_var(
                check->a, TYPES_LOCAL_VAR, lhs->name_pos, check->pkg, lhs->name, T);
            burrow__tc_declare(check, check->env.scope, NULL, types_object(obj),
                               clause->colon);
            burrow__tc_record_implicit(check, c, types_object(obj));
            /* For the "declared and not used" error, all lhs variables act as
             * one; i.e., if any one of them is 'used', all of them are 'used'.
             * Collect them for later analysis. */
            burrow__types_ptrs_append(check->a, &lhs_vars, obj);
        }
        burrow__tc_stmt_list(check, inner, clause->body);
        burrow__tc_close_scope(check);
    }

    /* If lhs exists, we must have at least one lhs variable that was used. (We
     * can't use check.usage because that only looks at one scope; and we don't
     * want to use the same variable for all scopes and change the variable
     * type underfoot.) */
    if (lhs != NULL) {
        bool used = false;
        for (Int i = 0; i < lhs_vars.len; i++) {
            TypesVar *v = GT_AT(lhs_vars, i, TypesVar *);
            if (tsm_is_used(check, v))
                used = true;
            /* avoid usage error when checking entire function */
            tsm_set_used(check, v);
        }
        if (!used) {
            Str name = lhs->name;
            GT_SOFT_ERRORF(check, GT_AT_NODE(lhs), GT_CODE_UNUSED_VAR,
                           "%s declared and not used", name);
        }
    }
}

static void tsm_type_switch_stmt(TypesChecker *check, GtStmtContext inner,
                                 AstTypeSwitchStmt *s) {
    inner |= GT_BREAK_OK | GT_IN_TYPE_SWITCH;
    burrow__tc_open_scope(check, GT_NODE(s), BURROW_S("type switch"));
    tsm_type_switch_body(check, inner, s);
    burrow__tc_close_scope(check);
}

static void tsm_select_stmt(TypesChecker *check, GtStmtContext inner,
                            AstSelectStmt *s) {
    inner |= GT_BREAK_OK;

    Slice list = s->body->list;
    tsm_multiple_defaults(check, list);

    for (Int i = 0; i < list.len; i++) {
        AstStmt c = GT_AT(list, i, AstStmt);
        if (c->kind != AST_KIND_COMM_CLAUSE)
            continue; /* error reported before */
        AstCommClause *clause = (AstCommClause *)(void *)c;

        /* clause.Comm must be a SendStmt, RecvStmt, or default case */
        bool valid = false;
        AstExpr rhs = NULL; /* rhs of RecvStmt, or nil */
        AstStmt comm = clause->comm;
        Int kind = comm != NULL ? comm->kind : AST_KIND_SEND_STMT;
        switch (kind) {
        case AST_KIND_SEND_STMT:
            valid = true;
            break;
        case AST_KIND_ASSIGN_STMT: {
            AstAssignStmt *as = (AstAssignStmt *)(void *)comm;
            if (as->rhs.len == 1)
                rhs = GT_AT(as->rhs, 0, AstExpr);
            break;
        }
        case AST_KIND_EXPR_STMT:
            rhs = ((AstExprStmt *)(void *)comm)->x;
            break;
        default:
            break;
        }

        /* if present, rhs must be a receive operation */
        if (rhs != NULL) {
            AstExpr x = ast_unparen(rhs);
            if (x != NULL && x->kind == AST_KIND_UNARY_EXPR &&
                ((AstUnaryExpr *)(void *)x)->op == TOKEN_ARROW)
                valid = true;
        }

        if (!valid) {
            burrow__tc_error(
                check, burrow__tc_at_node(comm), GT_CODE_INVALID_SELECT_CASE,
                BURROW_S(
                    "select case must be send or receive (possibly with assignment)"));
            continue;
        }

        burrow__tc_open_scope(check, c, BURROW_S("case"));
        if (comm != NULL)
            burrow__tc_stmt(check, inner, comm);
        burrow__tc_stmt_list(check, inner, clause->body);
        burrow__tc_close_scope(check);
    }
}

static void tsm_for_stmt(TypesChecker *check, GtStmtContext inner, AstForStmt *s) {
    inner |= GT_BREAK_OK | GT_CONTINUE_OK;
    burrow__tc_open_scope(check, GT_NODE(s), BURROW_S("for"));

    tsm_simple_stmt(check, s->init);
    if (s->cond != NULL) {
        GtOperand x = {0};
        burrow__tc_expr(check, NULL, &x, s->cond);
        if (burrow__tc_op_valid(&x) &&
            !burrow__types_all_basic(x.typ, TYPES_IS_BOOLEAN))
            burrow__tc_error(check, burrow__tc_at_node(s->cond), GT_CODE_INVALID_COND,
                             BURROW_S("non-boolean condition in for statement"));
    }
    tsm_simple_stmt(check, s->post);
    /* spec: "The init statement may be a short variable declaration, but the
     * post statement must not." */
    if (s->post != NULL && s->post->kind == AST_KIND_ASSIGN_STMT) {
        AstAssignStmt *as = (AstAssignStmt *)(void *)s->post;
        if (as->tok == TOKEN_DEFINE) {
            burrow__tc_soft_error(check, GT_AT_NODE(as), GT_CODE_INVALID_POST_DECL,
                                  BURROW_S("cannot declare in post statement"));
            /* Don't call useLHS here because we want to use the lhs in this
             * erroneous statement so that we don't get errors about these lhs
             * variables being declared and not used. */
            (void)burrow__tc_use(check, as->lhs); /* avoid follow-up errors */
        }
    }
    burrow__tc_stmt(check, inner, GT_NODE(s->body));

    burrow__tc_close_scope(check);
}

static void tsm_stmt(TypesChecker *check, GtStmtContext ctxt, AstStmt s) {
    /* reset context for statements of inner blocks */
    GtStmtContext inner =
        ctxt & ~(GT_FALLTHROUGH_OK | GT_FINAL_SWITCH_CASE | GT_IN_TYPE_SWITCH);

    Int kind = s->kind;
    switch (kind) {
    case AST_KIND_BAD_STMT:
    case AST_KIND_EMPTY_STMT:
        /* ignore */
        break;

    case AST_KIND_DECL_STMT:
        burrow__tc_decl_stmt(check, ((AstDeclStmt *)(void *)s)->decl);
        break;

    case AST_KIND_LABELED_STMT:
        check->env.has_label = true;
        burrow__tc_stmt(check, ctxt, ((AstLabeledStmt *)(void *)s)->stmt);
        break;

    case AST_KIND_EXPR_STMT:
        tsm_expr_stmt(check, (AstExprStmt *)(void *)s);
        break;

    case AST_KIND_SEND_STMT:
        tsm_send_stmt(check, (AstSendStmt *)(void *)s);
        break;

    case AST_KIND_INC_DEC_STMT:
        tsm_inc_dec_stmt(check, (AstIncDecStmt *)(void *)s);
        break;

    case AST_KIND_ASSIGN_STMT:
        tsm_assign_stmt(check, (AstAssignStmt *)(void *)s);
        break;

    case AST_KIND_GO_STMT:
        tsm_suspended_call(check, BURROW_S("go"), ((AstGoStmt *)(void *)s)->call);
        break;

    case AST_KIND_DEFER_STMT:
        tsm_suspended_call(check, BURROW_S("defer"), ((AstDeferStmt *)(void *)s)->call);
        break;

    case AST_KIND_RETURN_STMT:
        tsm_return_stmt(check, (AstReturnStmt *)(void *)s);
        break;

    case AST_KIND_BRANCH_STMT:
        tsm_branch_stmt(check, ctxt, (AstBranchStmt *)(void *)s);
        break;

    case AST_KIND_BLOCK_STMT:
        burrow__tc_open_scope(check, s, BURROW_S("block"));
        burrow__tc_stmt_list(check, inner, ((AstBlockStmt *)(void *)s)->list);
        burrow__tc_close_scope(check);
        break;

    case AST_KIND_IF_STMT:
        tsm_if_stmt(check, inner, (AstIfStmt *)(void *)s);
        break;

    case AST_KIND_SWITCH_STMT:
        tsm_switch_stmt(check, inner, (AstSwitchStmt *)(void *)s);
        break;

    case AST_KIND_TYPE_SWITCH_STMT:
        tsm_type_switch_stmt(check, inner, (AstTypeSwitchStmt *)(void *)s);
        break;

    case AST_KIND_SELECT_STMT:
        tsm_select_stmt(check, inner, (AstSelectStmt *)(void *)s);
        break;

    case AST_KIND_FOR_STMT:
        tsm_for_stmt(check, inner, (AstForStmt *)(void *)s);
        break;

    case AST_KIND_RANGE_STMT: {
        AstRangeStmt *rs = (AstRangeStmt *)(void *)s;
        inner |= GT_BREAK_OK | GT_CONTINUE_OK;
        /* s.TokPos is invalid when there are no range variables (for range x
         * {}); noNewVarPos is unused in that case, but inNode asserts a valid
         * pos. */
        TokenPos tok_pos = rs->tok_pos;
        if (!token_pos_is_valid(tok_pos))
            tok_pos = rs->for_;
        burrow__tc_range_stmt(check, inner, rs, burrow__tc_in_node(s, tok_pos), rs->key,
                              rs->value, NULL, rs->x, rs->tok == TOKEN_DEFINE);
        break;
    }

    default:
        burrow__tc_error(check, burrow__tc_at_node(s), GT_CODE_INVALID_SYNTAX_TREE,
                         BURROW_S("invalid statement"));
        break;
    }
}

/* stmt typechecks statement s. */
void burrow__tc_stmt(TypesChecker *check, GtStmtContext ctxt, AstStmt s) {
    /* process collected function literals before scope changes */
    Int top = check->delayed.len;
    tsm_stmt(check, ctxt, s);
    burrow__tc_process_delayed(check, top);
}
