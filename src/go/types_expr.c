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
#include "burrow/go/ast.h"
#include "burrow/go/constant.h"
#include "burrow/go/token.h"
#include "burrow/map.h"
#include "burrow/panic.h"
#include "burrow/slice.h"

#include "types_check.h"
#include "types_internal.h"

#include <stdbool.h>
#include <string.h>

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

/* ------------------------------------------------------------- operators */

/* Go's opPredicates maps, as a switch: the predicate op needs of its
 * operands, or false in *known if op has none. */
static bool tex_op_pred(bool unary, Token op, TypesType t, bool *known) {
    *known = true;
    if (unary) {
        switch ((Int)op) {
        case TOKEN_ADD:
        case TOKEN_SUB:
            return burrow__types_all_basic(t, TYPES_IS_NUMERIC);
        case TOKEN_XOR:
            return burrow__types_all_basic(t, TYPES_IS_INTEGER);
        case TOKEN_NOT:
            return burrow__types_all_basic(t, TYPES_IS_BOOLEAN);
        default:
            break;
        }
    } else {
        switch ((Int)op) {
        case TOKEN_ADD:
            return burrow__types_all_basic(t, TYPES_IS_NUMERIC | TYPES_IS_STRING);
        case TOKEN_SUB:
        case TOKEN_MUL:
        case TOKEN_QUO:
            return burrow__types_all_basic(t, TYPES_IS_NUMERIC);
        case TOKEN_REM:
        case TOKEN_AND:
        case TOKEN_OR:
        case TOKEN_XOR:
        case TOKEN_AND_NOT:
            return burrow__types_all_basic(t, TYPES_IS_INTEGER);
        case TOKEN_LAND:
        case TOKEN_LOR:
            return burrow__types_all_basic(t, TYPES_IS_BOOLEAN);
        default:
            break;
        }
    }
    *known = false;
    return false;
}

static bool tex_op(TypesChecker *check, bool unary, GtOperand *x, Token op) {
    bool known;
    bool ok = tex_op_pred(unary, op, x->typ, &known);
    Str ops = token_string(op, check->a);
    if (!known) {
        GT_ERRORF(check, burrow__tc_at_op(x), GT_CODE_INVALID_SYNTAX_TREE,
                  "unknown operator %s", ops);
        return false;
    }
    if (!ok) {
        Str xs = burrow__tc_xs(check, x);
        GT_ERRORF(check, burrow__tc_at_op(x), GT_CODE_UNDEFINED_OP,
                  GT_INVALID_OP "operator %s not defined on %s", ops, xs);
        return false;
    }
    return true;
}

/* opPos returns the position of the operator if x is an operation; otherwise
 * it returns the start position of x. */
TokenPos burrow__tc_expr_op_pos(AstExpr x) {
    if (x == NULL)
        return TOKEN_NO_POS; /* don't crash */
    if (x->kind == AST_KIND_BINARY_EXPR)
        return ((AstBinaryExpr *)(void *)x)->op_pos;
    return ast_expr_pos(x);
}

/* opName returns the name of the operation if x is an operation that might
 * overflow; otherwise it returns the empty string. op2str2 is only used for
 * operations that may cause overflow. */
Str burrow__tc_op_name(AstExpr e) {
    if (e == NULL)
        return BURROW_S("");
    switch (e->kind) {
    case AST_KIND_BINARY_EXPR:
        switch ((Int)((AstBinaryExpr *)(void *)e)->op) {
        case TOKEN_ADD:
            return BURROW_S("addition");
        case TOKEN_SUB:
            return BURROW_S("subtraction");
        case TOKEN_XOR:
            return BURROW_S("bitwise XOR");
        case TOKEN_MUL:
            return BURROW_S("multiplication");
        case TOKEN_SHL:
            return BURROW_S("shift");
        default:
            break;
        }
        break;
    case AST_KIND_UNARY_EXPR:
        if (((AstUnaryExpr *)(void *)e)->op == TOKEN_XOR)
            return BURROW_S("bitwise complement");
        break;
    default:
        break;
    }
    return BURROW_S("");
}

/* The unary expression e may be NULL. It's passed in for better error messages
 * only. */
static void tex_unary(TypesChecker *check, GtOperand *x, AstUnaryExpr *e) {
    burrow__tc_expr(check, NULL, x, e->x);
    if (!burrow__tc_op_valid(x))
        return;

    Token op = e->op;
    switch ((Int)op) {
    case TOKEN_AND:
        /* spec: "As an exception to the addressability requirement x may also
         * be a composite literal." */
        if (ast_unparen(e->x)->kind != AST_KIND_COMPOSITE_LIT &&
            x->mode != GT_MODE_VARIABLE) {
            Str xs = burrow__tc_xs(check, x);
            GT_ERRORF(check, burrow__tc_at_op(x), GT_CODE_UNADDRESSABLE_OPERAND,
                      GT_INVALID_OP "cannot take address of %s", xs);
            burrow__tc_op_invalidate(x);
            return;
        }
        x->mode = GT_MODE_VALUE;
        x->typ = types_type(types_new_pointer(check->a, x->typ));
        return;

    case TOKEN_ARROW: {
        /* We cannot receive a value with an incomplete type; make sure it's
         * complete. */
        TypesType elem = burrow__tc_chan_elem(check, burrow__tc_at_op(x), x, true);
        if (elem != NULL && burrow__tc_is_complete(check, elem)) {
            x->mode = GT_MODE_COMMAOK;
            x->typ = elem;
            check->env.has_call_or_recv = true;
            return;
        }
        burrow__tc_op_invalidate(x);
        return;
    }

    case TOKEN_TILDE:
        /* Provide a better error position and message than what check.op
         * below would do. */
        if (!burrow__types_all_basic(x->typ, TYPES_IS_INTEGER)) {
            burrow__tc_error(check, GT_AT_NODE(e), GT_CODE_UNDEFINED_OP,
                             BURROW_S("cannot use ~ outside of interface or type "
                                      "constraint"));
            burrow__tc_op_invalidate(x);
            return;
        }
        burrow__tc_error(
            check, GT_AT_NODE(e), GT_CODE_UNDEFINED_OP,
            BURROW_S("cannot use ~ outside of interface or type constraint "
                     "(use ^ for bitwise complement)"));
        op = TOKEN_XOR;
        break;

    default:
        break;
    }

    if (!tex_op(check, true, x, op)) {
        burrow__tc_op_invalidate(x);
        return;
    }

    if (x->mode == GT_MODE_CONSTANT) {
        if (constant_value_kind(x->val) == CONSTANT_UNKNOWN)
            /* nothing to do (and don't cause an error below in the overflow
             * check) */
            return;
        Uint prec = 0;
        if (burrow__types_is_basic(x->typ, TYPES_IS_UNSIGNED))
            prec = (Uint)(burrow__types_conf_sizeof(check->conf, x->typ) * 8);
        x->val = constant_unary_op(check->a, op, x->val, prec);
        x->expr = GT_NODE(e);
        burrow__tc_overflow(check, x, burrow__tc_expr_op_pos(x->expr));
        return;
    }

    x->mode = GT_MODE_VALUE;
    /* x.typ remains unchanged */
}

/* What the commonUnder walk of chanElem closes over. */
typedef struct TexChan {
    TypesChecker *check;
    bool recv;
} TexChan;

static bool tex_chan_cond(void *env, TypesType t, TypesType u, Str *cause) {
    TexChan *c = env;
    TypesChecker *check = c->check;
    if (u == NULL) {
        *cause = BURROW_S("no specific channel type");
        return false;
    }
    TypesChan *ch = types_as_chan(u);
    if (ch == NULL) {
        Str ts = burrow__tc_ts(check, t);
        *cause = GT_SPRINTF(check, "non-channel %s", ts);
        return false;
    }
    if (c->recv && ch->dir == TYPES_SEND_ONLY) {
        Str ts = burrow__tc_ts(check, t);
        *cause = GT_SPRINTF(check, "send-only channel %s", ts);
        return false;
    }
    if (!c->recv && ch->dir == TYPES_RECV_ONLY) {
        Str ts = burrow__tc_ts(check, t);
        *cause = GT_SPRINTF(check, "receive-only channel %s", ts);
        return false;
    }
    return true;
}

/* chanElem returns the channel element type of x for a receive from x (recv
 * true) or send to x (recv false) operation. If the operation is not valid,
 * chanElem reports an error and returns NULL. */
TypesType burrow__tc_chan_elem(TypesChecker *check, GtPosn pos, const GtOperand *x,
                               bool recv) {
    TexChan c = {check, recv};
    Str cause = {0};
    TypesType u = burrow__tc_common_under(
        check, x->typ, BURROW_FN(GtUnderCond, tex_chan_cond, &c), &cause);
    if (u != NULL)
        return types_as_chan(u)->elem;

    Str xs = burrow__tc_xs(check, x);
    if (recv) {
        if (burrow__types_is_type_param(x->typ)) {
            GT_ERRORF(check, pos, GT_CODE_INVALID_RECEIVE,
                      GT_INVALID_OP "cannot receive from %s: %s", xs, cause);
        } else {
            /* In this case, only the non-channel and send-only channel error
             * are possible. */
            GT_ERRORF(check, pos, GT_CODE_INVALID_RECEIVE,
                      GT_INVALID_OP "cannot receive from %s %s", cause, xs);
        }
    } else {
        if (burrow__types_is_type_param(x->typ)) {
            GT_ERRORF(check, pos, GT_CODE_INVALID_SEND,
                      GT_INVALID_OP "cannot send to %s: %s", xs, cause);
        } else {
            /* In this case, only the non-channel and receive-only channel error
             * are possible. */
            GT_ERRORF(check, pos, GT_CODE_INVALID_SEND,
                      GT_INVALID_OP "cannot send to %s %s", cause, xs);
        }
    }
    return NULL;
}

static bool tex_is_shift(Token op) {
    return op == TOKEN_SHL || op == TOKEN_SHR;
}

bool burrow__tc_is_comparison(Token op) {
    /* Note: tokens are not ordered well to make this much easier */
    switch ((Int)op) {
    case TOKEN_EQL:
    case TOKEN_NEQ:
    case TOKEN_LSS:
    case TOKEN_LEQ:
    case TOKEN_GTR:
    case TOKEN_GEQ:
        return true;
    default:
        return false;
    }
}

/* ------------------------------------------------------ untyped operands */

/* updateExprType updates the type of x to typ and invokes itself recursively
 * for the operands of x, depending on expression kind. If typ is still an
 * untyped and not the final type, updateExprType only updates the recorded
 * untyped type for x and possibly its operands. Otherwise (i.e., typ is not an
 * untyped type anymore, or it is the final type for x), the type and value are
 * recorded. Also, if x is a constant, it must be representable as a value of
 * typ, and if x is the (formerly untyped) lhs operand of a non-constant shift,
 * it must be an integer value. */
void burrow__tc_update_expr_type(TypesChecker *check, AstExpr x, TypesType typ,
                                 bool final) {
    GtExprInfo **found = check->untyped != NULL ? map_get(check->untyped, &x) : NULL;
    if (found == NULL)
        return; /* nothing to do */
    GtExprInfo *old = *found;

    /* update operands of x if necessary. A constant has a value: Go's
     * old.val != nil. */
    bool has_val = old->mode == GT_MODE_CONSTANT;
    switch (x->kind) {
    case AST_KIND_BAD_EXPR:
    case AST_KIND_FUNC_LIT:
    case AST_KIND_COMPOSITE_LIT:
    case AST_KIND_INDEX_EXPR:
    case AST_KIND_SLICE_EXPR:
    case AST_KIND_TYPE_ASSERT_EXPR:
    case AST_KIND_STAR_EXPR:
    case AST_KIND_KEY_VALUE_EXPR:
    case AST_KIND_ARRAY_TYPE:
    case AST_KIND_STRUCT_TYPE:
    case AST_KIND_FUNC_TYPE:
    case AST_KIND_INTERFACE_TYPE:
    case AST_KIND_MAP_TYPE:
    case AST_KIND_CHAN_TYPE:
        /* These expression are never untyped - nothing to do. The respective
         * sub-expressions got their final types upon assignment or use. */
        return;

    case AST_KIND_CALL_EXPR:
        /* Resulting in an untyped constant (e.g., built-in complex). The
         * respective calls take care of calling updateExprType for the
         * arguments if necessary. */
    case AST_KIND_IDENT:
    case AST_KIND_BASIC_LIT:
    case AST_KIND_SELECTOR_EXPR:
        /* An identifier denoting a constant, a constant literal, or a
         * qualified identifier (imported untyped constant). No operands to
         * take care of. */
        break;

    case AST_KIND_PAREN_EXPR:
        burrow__tc_update_expr_type(check, ((AstParenExpr *)(void *)x)->x, typ, final);
        break;

    case AST_KIND_UNARY_EXPR:
        /* If x is a constant, the operands were constants. The operands don't
         * need to be updated since they never get "materialized" into a typed
         * value. If left in the untyped map, they will be processed at the end
         * of the type check. */
        if (has_val)
            break;
        burrow__tc_update_expr_type(check, ((AstUnaryExpr *)(void *)x)->x, typ, final);
        break;

    case AST_KIND_BINARY_EXPR: {
        if (has_val)
            break; /* see comment for unary expressions */
        AstBinaryExpr *b = (AstBinaryExpr *)(void *)x;
        if (burrow__tc_is_comparison(b->op)) {
            /* The result type is independent of operand types and the operand
             * types must have final types. */
        } else if (tex_is_shift(b->op)) {
            /* The result type depends only on lhs operand. The rhs type was
             * updated when checking the shift. */
            burrow__tc_update_expr_type(check, b->x, typ, final);
        } else {
            /* The operand types match the result type. */
            burrow__tc_update_expr_type(check, b->x, typ, final);
            burrow__tc_update_expr_type(check, b->y, typ, final);
        }
        break;
    }

    default:
        panic_str(BURROW_S("unreachable"));
    }

    /* If the new type is not final and still untyped, just update the recorded
     * type. */
    if (!final && !burrow__types_is_typed(typ)) {
        old->typ = types_as_basic(types_type_underlying(typ));
        return;
    }

    /* Otherwise we have the final (typed or untyped type). Remove it from the
     * map of yet untyped expressions. */
    map_del(check->untyped, &x);

    if (old->is_lhs) {
        /* If x is the lhs of a shift, its final type must be integer. We
         * already know from the shift check that it is representable as an
         * integer if it is a constant. */
        if (!burrow__types_all_basic(typ, TYPES_IS_INTEGER)) {
            Str es = burrow__tc_es(check, x);
            Str ts = burrow__tc_ts(check, typ);
            GT_ERRORF(check, burrow__tc_at_node(x), GT_CODE_INVALID_SHIFT_OPERAND,
                      GT_INVALID_OP "shifted operand %s (type %s) must be integer", es,
                      ts);
            return;
        }
        /* Even if we have an integer, if the value is a constant we still must
         * check that it is representable as the specific int type requested
         * (was go.dev/issue/22969). Fall through here. */
    }
    if (has_val) {
        /* If x is a constant, it must be representable as a value of typ. */
        GtOperand c = {old->mode, x, types_type(old->typ), old->val, 0};
        burrow__tc_convert_untyped(check, &c, typ);
        if (!burrow__tc_op_valid(&c))
            return;
    }

    /* Everything's fine, record final type and value for x. */
    burrow__tc_record_type_and_value(check, x, old->mode, typ, old->val);
}

/* updateExprVal updates the value of x to val. */
void burrow__tc_update_expr_val(TypesChecker *check, AstExpr x, ConstantValue val) {
    GtExprInfo **info = check->untyped != NULL ? map_get(check->untyped, &x) : NULL;
    if (info != NULL)
        (*info)->val = val;
}

/* ---------------------------------------------------- binary expressions */

/* incomparableCause returns a more specific cause why typ is not comparable.
 * If there is no more specific cause, the result is "". */
static Str tex_incomparable_cause(TypesChecker *check, TypesType typ) {
    switch (types_type_underlying(typ)->kind) {
    case TYPES_KIND_SLICE:
    case TYPES_KIND_SIGNATURE:
    case TYPES_KIND_MAP: {
        Str what = burrow__tc_composite_kind(typ);
        return GT_SPRINTF(check, "%s can only be compared to nil", what);
    }
    default:
        break;
    }
    /* see if we can extract a more specific error */
    GtIncomparable why;
    bool ok = burrow__types_comparable_type_why(typ, true, NULL, &why);
    burrow__types_assert(!ok);
    switch ((Int)why.kind) {
    case GT_INCOMPARABLE_STRUCT: {
        Str ts = burrow__tc_ts(check, why.typ);
        return GT_SPRINTF(check, "struct containing %s cannot be compared", ts);
    }
    case GT_INCOMPARABLE_ARRAY: {
        Str ts = burrow__tc_ts(check, why.typ);
        return GT_SPRINTF(check, "%s cannot be compared", ts);
    }
    case GT_INCOMPARABLE_EMPTY_SET:
        return BURROW_S("empty type set");
    case GT_INCOMPARABLE_TYPE_SET:
        return BURROW_S("incomparable types in type set");
    default:
        return BURROW_S("");
    }
}

/* If switch_case is true, the operator op is ignored. */
void burrow__tc_comparison(TypesChecker *check, GtOperand *x, GtOperand *y, Token op,
                           bool switch_case) {
    /* Avoid spurious errors if any of the operands has an invalid type
     * (go.dev/issue/54405). */
    if (!burrow__types_is_valid(x->typ) || !burrow__types_is_valid(y->typ)) {
        burrow__tc_op_invalidate(x);
        return;
    }

    if (switch_case)
        op = TOKEN_EQL;

    GtOperand *err_op = x;    /* operand for which error is reported, if any */
    Str cause = BURROW_S(""); /* specific error cause, if any */

    /* spec: "In any comparison, the first operand must be assignable to the
     * type of the second operand, or vice versa." */
    GtCode code = GT_CODE_MISMATCHED_TYPES;
    bool ok = burrow__tc_assignable_to(check, x, y->typ, NULL, NULL);
    if (!ok)
        ok = burrow__tc_assignable_to(check, y, x->typ, NULL, NULL);
    if (!ok) {
        /* Report the error on the 2nd operand since we only know after seeing
         * the 2nd operand whether we have a type mismatch. */
        err_op = y;
        Str xts = burrow__tc_ts(check, x->typ);
        Str yts = burrow__tc_ts(check, y->typ);
        cause = GT_SPRINTF(check, "mismatched types %s and %s", xts, yts);
        goto error;
    }

    /* check if comparison is defined for operands */
    code = GT_CODE_UNDEFINED_OP;
    switch ((Int)op) {
    case TOKEN_EQL:
    case TOKEN_NEQ:
        /* spec: "The equality operators == and != apply to operands that are
         * comparable." */
        if (burrow__tc_op_is_nil(x) || burrow__tc_op_is_nil(y)) {
            /* Comparison against nil requires that the other operand type has
             * nil. */
            TypesType typ = burrow__tc_op_is_nil(x) ? y->typ : x->typ;
            if (!burrow__types_has_nil(typ)) {
                /* This case should only be possible for "nil == nil". Report
                 * the error on the 2nd operand since we only know after seeing
                 * the 2nd operand whether we have an invalid comparison. */
                err_op = y;
                goto error;
            }
        } else if (!types_comparable(x->typ)) {
            err_op = x;
            cause = tex_incomparable_cause(check, x->typ);
            goto error;
        } else if (!types_comparable(y->typ)) {
            err_op = y;
            cause = tex_incomparable_cause(check, y->typ);
            goto error;
        }
        break;

    case TOKEN_LSS:
    case TOKEN_LEQ:
    case TOKEN_GTR:
    case TOKEN_GEQ:
        /* spec: The ordering operators <, <=, >, and >= apply to operands that
         * are ordered." */
        if (!burrow__types_all_basic(x->typ, TYPES_IS_ORDERED)) {
            err_op = x;
            goto error;
        }
        if (!burrow__types_all_basic(y->typ, TYPES_IS_ORDERED)) {
            err_op = y;
            goto error;
        }
        break;

    default:
        panic_str(BURROW_S("unreachable"));
    }

    /* comparison is ok */
    if (x->mode == GT_MODE_CONSTANT && y->mode == GT_MODE_CONSTANT) {
        x->val = constant_make_bool(constant_compare(x->val, op, y->val));
        /* The operands are never materialized; no need to update their
         * types. */
    } else {
        x->mode = GT_MODE_VALUE;
        /* The operands have now their final types, which at run-time will be
         * materialized. Update the expression trees. If the current types are
         * untyped, the materialized type is the respective default type. */
        burrow__tc_update_expr_type(check, x->expr, types_default(x->typ), true);
        burrow__tc_update_expr_type(check, y->expr, types_default(y->typ), true);
    }

    /* spec: "Comparison operators compare two operands and yield an untyped
     * boolean value." */
    x->typ = GT_TYP(TYPES_UNTYPED_BOOL);
    return;

error:
    /* We have an offending operand errOp and possibly an error cause. */
    if (cause.len == 0) {
        Str ops = token_string(op, check->a);
        if (burrow__types_is_type_param(x->typ) ||
            burrow__types_is_type_param(y->typ)) {
            /* TODO(gri) should report the specific type causing the problem, if
             * any */
            if (!burrow__types_is_type_param(x->typ))
                err_op = y;
            Str ts = burrow__tc_ts(check, err_op->typ);
            cause =
                GT_SPRINTF(check, "type parameter %s cannot use operator %s", ts, ops);
        } else {
            /* catch-all neither x nor y is a type parameter */
            Str what = burrow__tc_composite_kind(err_op->typ);
            if (what.len == 0)
                what = burrow__tc_ts(check, err_op->typ);
            cause = GT_SPRINTF(check, "operator %s not defined on %s", ops, what);
        }
    }
    Str xes = burrow__tc_es(check, x->expr);
    Str yes = burrow__tc_es(check, y->expr);
    if (switch_case) {
        /* error position always at 1st operand */
        GT_ERRORF(check, burrow__tc_at_op(x), code,
                  "invalid case %s in switch on %s (%s)", xes, yes, cause);
    } else {
        Str ops = token_string(op, check->a);
        GT_ERRORF(check, burrow__tc_at_op(err_op), code, GT_INVALID_OP "%s %s %s (%s)",
                  xes, ops, yes, cause);
    }
    burrow__tc_op_invalidate(x);
}

/* If e is not NULL, it must be the shift expression; it may be NULL for
 * non-constant shifts. */
static void tex_shift(TypesChecker *check, GtOperand *x, GtOperand *y, AstExpr e,
                      Token op) {
    /* TODO(gri) This function seems overly complex. Revisit. */

    ConstantValue xval = tex_nil;
    bool has_xval = false;
    if (x->mode == GT_MODE_CONSTANT) {
        xval = constant_to_int(check->a, x->val);
        has_xval = true;
    }

    if (burrow__types_all_basic(x->typ, TYPES_IS_INTEGER) ||
        (!burrow__types_is_typed(x->typ) && has_xval &&
         constant_value_kind(xval) == CONSTANT_INT)) {
        /* The lhs is of integer type or an untyped constant representable as
         * an integer. Nothing to do. */
    } else {
        /* shift has no chance */
        Str xs = burrow__tc_xs(check, x);
        GT_ERRORF(check, burrow__tc_at_op(x), GT_CODE_INVALID_SHIFT_OPERAND,
                  GT_INVALID_OP "shifted operand %s must be integer", xs);
        burrow__tc_op_invalidate(x);
        return;
    }

    /* spec: "The right operand in a shift expression must have integer type or
     * be an untyped constant representable by a value of type uint."
     *
     * Check that constants are representable by uint, but do not convert them
     * (see also go.dev/issue/47243). */
    ConstantValue yval = tex_nil;
    if (y->mode == GT_MODE_CONSTANT) {
        /* Provide a good error message for negative shift counts. */
        yval = constant_to_int(check->a, y->val); /* consider -1, 1.0, but not -1.1 */
        if (constant_value_kind(yval) == CONSTANT_INT && constant_sign(yval) < 0) {
            Str ys = burrow__tc_xs(check, y);
            GT_ERRORF(check, burrow__tc_at_op(y), GT_CODE_INVALID_SHIFT_COUNT,
                      GT_INVALID_OP "negative shift count %s", ys);
            burrow__tc_op_invalidate(x);
            return;
        }

        if (!burrow__types_is_typed(y->typ)) {
            /* Caution: Check for representability here, rather than in the
             * switch below, because isInteger includes untyped integers (was
             * bug go.dev/issue/43697). */
            burrow__tc_representable(check, y, types_typ[TYPES_UINT]);
            if (!burrow__tc_op_valid(y)) {
                burrow__tc_op_invalidate(x);
                return;
            }
        }
    } else {
        /* Check that RHS is otherwise at least of integer type. */
        if (burrow__types_all_basic(y->typ, TYPES_IS_INTEGER)) {
            if (!burrow__types_all_basic(y->typ, TYPES_IS_UNSIGNED)) {
                Str ys = burrow__tc_xs(check, y);
                if (!GT_VERIFY_VERSIONF(check, burrow__tc_at_op(y), burrow__tc_go1_13,
                                        GT_INVALID_OP "signed shift count %s", ys)) {
                    burrow__tc_op_invalidate(x);
                    return;
                }
            }
        } else if (!burrow__types_is_typed(y->typ)) {
            /* This is incorrect, but preserves pre-existing behavior. See also
             * go.dev/issue/47410. */
            burrow__tc_convert_untyped(check, y, GT_TYP(TYPES_UINT));
            if (!burrow__tc_op_valid(y)) {
                burrow__tc_op_invalidate(x);
                return;
            }
        } else {
            Str ys = burrow__tc_xs(check, y);
            GT_ERRORF(check, burrow__tc_at_op(y), GT_CODE_INVALID_SHIFT_COUNT,
                      GT_INVALID_OP "shift count %s must be integer", ys);
            burrow__tc_op_invalidate(x);
            return;
        }
    }

    if (x->mode == GT_MODE_CONSTANT) {
        if (y->mode == GT_MODE_CONSTANT) {
            /* if either x or y has an unknown value, the result is unknown */
            if (constant_value_kind(x->val) == CONSTANT_UNKNOWN ||
                constant_value_kind(y->val) == CONSTANT_UNKNOWN) {
                x->val = constant_make_unknown();
                /* ensure the correct type - see comment below */
                if (!burrow__types_is_basic(x->typ, TYPES_IS_INTEGER))
                    x->typ = GT_TYP(TYPES_UNTYPED_INT);
                return;
            }
            /* rhs must be within reasonable bounds in constant shifts; so we
             * can express smallestFloat64 (see go.dev/issue/44057) */
            const uint64_t shift_bound = 1023 - 1 + 52;
            bool ok;
            uint64_t s = constant_uint64_val(yval, &ok);
            if (!ok || s > shift_bound) {
                Str ys = burrow__tc_xs(check, y);
                GT_ERRORF(check, burrow__tc_at_op(y), GT_CODE_INVALID_SHIFT_COUNT,
                          GT_INVALID_OP "invalid shift count %s", ys);
                burrow__tc_op_invalidate(x);
                return;
            }
            /* The lhs is representable as an integer but may not be an integer
             * (e.g., 2.0, an untyped float) - this can only happen for untyped
             * non-integer numeric constants. Correct the type so that the shift
             * result is of integer type. */
            if (!burrow__types_is_basic(x->typ, TYPES_IS_INTEGER))
                x->typ = GT_TYP(TYPES_UNTYPED_INT);
            /* x is a constant so xval != nil and it must be of Int kind. */
            x->val = constant_shift(check->a, xval, op, (Uint)s);
            x->expr = e;
            burrow__tc_overflow(check, x, burrow__tc_expr_op_pos(x->expr));
            return;
        }

        /* non-constant shift with constant lhs */
        if (!burrow__types_is_typed(x->typ)) {
            /* spec: "If the left operand of a non-constant shift expression is
             * an untyped constant, the type of the constant is what it would be
             * if the shift expression were replaced by its left operand
             * alone.".
             *
             * Delay operand checking until we know the final type by marking
             * the lhs expression as lhs shift operand.
             *
             * Usually (in correct programs), the lhs expression is in the
             * untyped map. However, it is possible to create incorrect programs
             * where the same expression is evaluated twice (via a declaration
             * cycle) such that the lhs expression type is determined in the
             * first round and thus deleted from the map, and then not found in
             * the second round (double insertion of the same expr node still
             * just leads to one entry for that node, and it can only be deleted
             * once). Be cautious and check for presence of entry. Example: var
             * e, f = int(1<<""[f]) // go.dev/issue/11347 */
            GtExprInfo **info =
                check->untyped != NULL ? map_get(check->untyped, &x->expr) : NULL;
            if (info != NULL)
                (*info)->is_lhs = true;
            /* keep x's type */
            x->mode = GT_MODE_VALUE;
            return;
        }
    }

    /* non-constant shift - lhs must be an integer */
    if (!burrow__types_all_basic(x->typ, TYPES_IS_INTEGER)) {
        Str xs = burrow__tc_xs(check, x);
        GT_ERRORF(check, burrow__tc_at_op(x), GT_CODE_INVALID_SHIFT_OPERAND,
                  GT_INVALID_OP "shifted operand %s must be integer", xs);
        burrow__tc_op_invalidate(x);
        return;
    }

    x->mode = GT_MODE_VALUE;
}

/* mayConvert reports whether the operands x and y may possibly have matching
 * types after converting one untyped operand to the type of the other. If
 * mayConvert returns true, we try to convert the operands to each other's
 * types, and if that fails we report a conversion failure. If mayConvert
 * returns false, we continue without an attempt at conversion, and if the
 * operand types are not compatible, we report a type mismatch error. */
static bool tex_may_convert(const GtOperand *x, const GtOperand *y) {
    /* If both operands are typed, there's no need for an implicit
     * conversion. */
    if (burrow__types_is_typed(x->typ) && burrow__types_is_typed(y->typ))
        return false;
    /* A numeric type can only convert to another numeric type. */
    if (burrow__types_all_basic(x->typ, TYPES_IS_NUMERIC) !=
        burrow__types_all_basic(y->typ, TYPES_IS_NUMERIC))
        return false;
    /* An untyped operand may convert to its default type when paired with an
     * empty interface. TODO(gri) This should only matter for comparisons (the
     * only binary operation that is valid with interfaces), but in that case
     * the assignability check should take care of the conversion. Verify and
     * possibly eliminate this extra test. */
    if (burrow__types_is_non_type_param_interface(x->typ) ||
        burrow__types_is_non_type_param_interface(y->typ))
        return true;
    /* A boolean type can only convert to another boolean type. */
    if (burrow__types_all_basic(x->typ, TYPES_IS_BOOLEAN) !=
        burrow__types_all_basic(y->typ, TYPES_IS_BOOLEAN))
        return false;
    /* A string type can only convert to another string type. */
    if (burrow__types_all_basic(x->typ, TYPES_IS_STRING) !=
        burrow__types_all_basic(y->typ, TYPES_IS_STRING))
        return false;
    /* Untyped nil can only convert to a type that has a nil. */
    if (burrow__tc_op_is_nil(x))
        return burrow__types_has_nil(y->typ);
    if (burrow__tc_op_is_nil(y))
        return burrow__types_has_nil(x->typ);
    /* An untyped operand cannot convert to a pointer. TODO(gri) generalize to
     * type parameters */
    if (burrow__tc_is_pointer(x->typ) || burrow__tc_is_pointer(y->typ))
        return false;
    return true;
}

/* matchTypes attempts to convert any untyped types x and y such that they
 * match. If an error occurs, x.mode is set to invalid. */
void burrow__tc_match_types(TypesChecker *check, GtOperand *x, GtOperand *y) {
    if (tex_may_convert(x, y)) {
        burrow__tc_convert_untyped(check, x, y->typ);
        if (!burrow__tc_op_valid(x))
            return;
        burrow__tc_convert_untyped(check, y, x->typ);
        if (!burrow__tc_op_valid(y)) {
            burrow__tc_op_invalidate(x);
            return;
        }
    }
}

#define TEX_MODE_BIT(m) (1u << (unsigned)(m))

/* What binary does once x holds lhs. */
static void tex_binary_rest(TypesChecker *check, GtOperand *x, AstExpr e, AstExpr lhs,
                            AstExpr rhs, Token op, TokenPos op_pos) {
    GtOperand y = {0};

    burrow__tc_expr(check, NULL, &y, rhs);

    if (!burrow__tc_op_valid(x))
        return;
    if (!burrow__tc_op_valid(&y)) {
        burrow__tc_op_invalidate(x);
        x->expr = y.expr;
        return;
    }

    if (tex_is_shift(op)) {
        tex_shift(check, x, &y, e, op);
        return;
    }

    burrow__tc_match_types(check, x, &y);
    if (!burrow__tc_op_valid(x))
        return;

    if (burrow__tc_is_comparison(op)) {
        burrow__tc_comparison(check, x, &y, op, false);
        return;
    }

    if (!types_identical(x->typ, y.typ)) {
        /* only report an error if we have valid types (otherwise we had an
         * error reported elsewhere already) */
        if (burrow__types_is_valid(x->typ) && burrow__types_is_valid(y.typ)) {
            Str xts = burrow__tc_ts(check, x->typ);
            Str yts = burrow__tc_ts(check, y.typ);
            if (e != NULL) {
                Str es = burrow__tc_es(check, e);
                GT_ERRORF(check, burrow__tc_at_node(e), GT_CODE_MISMATCHED_TYPES,
                          GT_INVALID_OP "%s (mismatched types %s and %s)", es, xts,
                          yts);
            } else {
                Str ls = burrow__tc_es(check, lhs);
                Str ops = token_string(op, check->a);
                Str rs = burrow__tc_es(check, rhs);
                GT_ERRORF(check, burrow__tc_at_op(x), GT_CODE_MISMATCHED_TYPES,
                          GT_INVALID_OP "%s %s= %s (mismatched types %s and %s)", ls,
                          ops, rs, xts, yts);
            }
        }
        burrow__tc_op_invalidate(x);
        return;
    }

    if (!tex_op(check, false, x, op)) {
        burrow__tc_op_invalidate(x);
        return;
    }

    if (op == TOKEN_QUO || op == TOKEN_REM) {
        /* check for zero divisor */
        if ((x->mode == GT_MODE_CONSTANT ||
             burrow__types_all_basic(x->typ, TYPES_IS_INTEGER)) &&
            y.mode == GT_MODE_CONSTANT && constant_sign(y.val) == 0) {
            burrow__tc_error(check, burrow__tc_at_op(&y), GT_CODE_DIV_BY_ZERO,
                             BURROW_S(GT_INVALID_OP "division by zero"));
            burrow__tc_op_invalidate(x);
            return;
        }

        /* check for divisor underflow in complex division (see
         * go.dev/issue/20227) */
        if (x->mode == GT_MODE_CONSTANT && y.mode == GT_MODE_CONSTANT &&
            burrow__types_is_basic(x->typ, TYPES_IS_COMPLEX)) {
            ConstantValue re = constant_real(y.val);
            ConstantValue im = constant_imag(y.val);
            ConstantValue re2 = constant_binary_op(check->a, re, TOKEN_MUL, re);
            ConstantValue im2 = constant_binary_op(check->a, im, TOKEN_MUL, im);
            if (constant_sign(re2) == 0 && constant_sign(im2) == 0) {
                burrow__tc_error(check, burrow__tc_at_op(&y), GT_CODE_DIV_BY_ZERO,
                                 BURROW_S(GT_INVALID_OP "division by zero"));
                burrow__tc_op_invalidate(x);
                return;
            }
        }
    }

    if (x->mode == GT_MODE_CONSTANT && y.mode == GT_MODE_CONSTANT) {
        /* if either x or y has an unknown value, the result is unknown */
        if (constant_value_kind(x->val) == CONSTANT_UNKNOWN ||
            constant_value_kind(y.val) == CONSTANT_UNKNOWN) {
            x->val = constant_make_unknown();
            /* x.typ is unchanged */
            return;
        }
        /* force integer division of integer operands */
        if (op == TOKEN_QUO && burrow__types_is_basic(x->typ, TYPES_IS_INTEGER))
            op = TOKEN_QUO_ASSIGN;
        x->val = constant_binary_op(check->a, x->val, op, y.val);
        x->expr = e;
        burrow__tc_overflow(check, x, op_pos);
        return;
    }

    x->mode = GT_MODE_VALUE;
    /* x.typ is unchanged */
}

/* Not in Go: check.expr(nil, x, lhs), for an lhs that is itself a binary
 * expression, done in a loop down the left side of the tree. Go's checker
 * recurses there, once for each operator, which is fine on a stack that grows.
 * A C stack does not, and the tables in golang.org/x/text/unicode/norm have a
 * string made of nearly two thousand constants added together. Each step does
 * what binary, exprInternal, rawExpr and expr would do for that operator, in
 * the same order. */
static void tex_binary_lhs(TypesChecker *check, GtOperand *x, AstExpr lhs) {
    Int n = 0;
    for (AstExpr p = lhs; p->kind == AST_KIND_BINARY_EXPR;
         p = ((AstBinaryExpr *)(void *)p)->x)
        n++;
    if (n == 0) {
        burrow__tc_expr(check, NULL, x, lhs);
        return;
    }
    AstBinaryExpr *buf[16];
    AstBinaryExpr **chain = buf;
    if (n > (Int)(sizeof buf / sizeof buf[0]))
        chain = BURROW_NEW_N(check->a, AstBinaryExpr *, (size_t)n);
    if (chain == NULL)
        panic_str(BURROW_S("out of memory"));
    Int i = 0;
    for (AstExpr p = lhs; p->kind == AST_KIND_BINARY_EXPR;
         p = ((AstBinaryExpr *)(void *)p)->x)
        chain[i++] = (AstBinaryExpr *)(void *)p;

    burrow__tc_expr(check, NULL, x, chain[n - 1]->x);
    for (i = n - 1; i >= 0; i--) {
        AstBinaryExpr *b = chain[i];
        AstExpr e = &b->node;
        tex_binary_rest(check, x, e, b->x, b->y, b->op, b->op_pos);
        if (!burrow__tc_op_valid(x))
            burrow__tc_op_invalidate(x);
        x->expr = e;
        burrow__tc_non_generic(check, NULL, x);
        burrow__tc_record(check, x);
        burrow__tc_exclude(check, x,
                           TEX_MODE_BIT(GT_MODE_NOVALUE) |
                               TEX_MODE_BIT(GT_MODE_BUILTIN) |
                               TEX_MODE_BIT(GT_MODE_TYPEXPR));
        burrow__tc_single_value(check, x);
    }
}

/* If e is not NULL, it must be the binary expression; it may be NULL for
 * non-constant expressions (when invoked for an assignment operation where the
 * binary expression is implicit). */
void burrow__tc_binary(TypesChecker *check, GtOperand *x, AstExpr e, AstExpr lhs,
                       AstExpr rhs, Token op, TokenPos op_pos) {
    tex_binary_lhs(check, x, lhs);
    tex_binary_rest(check, x, e, lhs, rhs, op, op_pos);
}

/* ------------------------------------------------------------ expressions */

/* newTarget creates a new target for the given type and description. The
 * result is NULL if typ is not a signature. */
GtTarget *burrow__tc_new_target(Alloc *a, TypesType typ, Str desc) {
    if (typ != NULL) {
        TypesType u =
            burrow__tc_common_under(NULL, typ, (GtUnderCond){NULL, NULL}, NULL);
        TypesSignature *sig = u != NULL ? types_as_signature(u) : NULL;
        if (sig != NULL) {
            GtTarget *T = GT_NEW(a, GtTarget);
            T->sig = sig;
            T->desc = desc;
            return T;
        }
    }
    return NULL;
}

/* rawExpr typechecks expression e and initializes x with the expression value
 * or type. If an error occurred, x.mode is set to invalid. If a non-nil target
 * T is given and e is a generic function, T is used to infer the type
 * arguments for e. If hint is not NULL, it is the type of a composite literal
 * element. If allow_generic is set, the operand type may be an uninstantiated
 * parameterized type or function value. */
GtExprKind burrow__tc_raw_expr(TypesChecker *check, GtTarget *T, GtOperand *x,
                               AstExpr e, TypesType hint, bool allow_generic) {
    GtExprKind kind = burrow__tc_expr_internal(check, T, x, e, hint);

    if (!allow_generic)
        burrow__tc_non_generic(check, T, x);

    burrow__tc_record(check, x);

    return kind;
}

/* If x is a generic type, or a generic function whose type arguments cannot
 * be inferred from a non-nil target T, nonGeneric reports an error and
 * invalidates x.mode and x.typ. Otherwise it leaves x alone. */
void burrow__tc_non_generic(TypesChecker *check, GtTarget *T, GtOperand *x) {
    if (!burrow__tc_op_valid(x) || x->mode == GT_MODE_NOVALUE)
        return;
    Str what = BURROW_S("");
    switch (x->typ->kind) {
    case TYPES_KIND_ALIAS:
    case TYPES_KIND_NAMED:
        if (burrow__types_is_generic(x->typ))
            what = BURROW_S("type");
        break;
    case TYPES_KIND_SIGNATURE:
        if (types_as_signature(x->typ)->tparams != NULL) {
            if (T != NULL) {
                /* enableReverseTypeInference is true in Go */
                (void)burrow__tc_func_inst(check, T, burrow__tc_op_pos(x), x, NULL,
                                           true);
                return;
            }
            what = BURROW_S("function");
        }
        break;
    default:
        break;
    }
    if (what.len > 0) {
        Str es = burrow__tc_es(check, x->expr);
        GT_ERRORF(check, burrow__tc_at_node(x->expr), GT_CODE_WRONG_TYPE_ARG_COUNT,
                  "cannot use generic %s %s without instantiation", what, es);
        burrow__tc_op_invalidate(x);
        x->typ = GT_TYP(TYPES_INVALID);
    }
}

/* What the underIs walk of a pointer indirection closes over. */
typedef struct TexIndirect {
    TypesChecker *check;
    GtOperand *x;
    TypesType base;
} TexIndirect;

static bool tex_under_indirect(void *env, TypesType t, TypesType u) {
    (void)t;
    TexIndirect *c = env;
    TypesPointer *p = u != NULL ? types_as_pointer(u) : NULL;
    if (p == NULL) {
        Str xs = burrow__tc_xs(c->check, c->x);
        GT_ERRORF(c->check, burrow__tc_at_op(c->x), GT_CODE_INVALID_INDIRECTION,
                  GT_INVALID_OP "cannot indirect %s", xs);
        return false;
    }
    if (c->base != NULL && !types_identical(p->base, c->base)) {
        Str xs = burrow__tc_xs(c->check, c->x);
        GT_ERRORF(c->check, burrow__tc_at_op(c->x), GT_CODE_INVALID_INDIRECTION,
                  GT_INVALID_OP "pointers of %s must have identical base types", xs);
        return false;
    }
    c->base = p->base;
    return true;
}

/* The TypeAssertExpr case of exprInternal; false where Go jumps to Error. */
static bool tex_type_assert_expr(TypesChecker *check, GtOperand *x,
                                 AstTypeAssertExpr *e) {
    burrow__tc_expr(check, NULL, x, e->x);
    if (!burrow__tc_op_valid(x))
        return false;
    /* x.(type) expressions are handled explicitly in type switches */
    if (e->type == NULL) {
        /* Don't use InvalidSyntaxTree because this can occur in the AST
         * produced by go/parser. */
        burrow__tc_error(check, GT_AT_NODE(e), GT_CODE_BAD_TYPE_KEYWORD,
                         BURROW_S("use of .(type) outside type switch"));
        return false;
    }
    if (burrow__types_is_type_param(x->typ)) {
        Str xs = burrow__tc_xs(check, x);
        GT_ERRORF(check, burrow__tc_at_op(x), GT_CODE_INVALID_ASSERT,
                  GT_INVALID_OP "cannot use type assertion on type parameter value %s",
                  xs);
        return false;
    }
    if (types_type_underlying(x->typ)->kind != TYPES_KIND_INTERFACE) {
        Str xs = burrow__tc_xs(check, x);
        GT_ERRORF(check, burrow__tc_at_op(x), GT_CODE_INVALID_ASSERT,
                  GT_INVALID_OP "%s is not an interface", xs);
        return false;
    }
    TypesType T = burrow__tc_var_type(check, e->type);
    if (!burrow__types_is_valid(T))
        return false;
    /* We cannot assert to an incomplete type; make sure it's complete. */
    if (!burrow__tc_is_complete(check, T))
        return false;
    burrow__tc_type_assertion(check, GT_NODE(e), x, T, false);
    x->mode = GT_MODE_COMMAOK;
    x->typ = T;
    return true;
}

/* The StarExpr case of exprInternal; false where Go jumps to Error. */
static bool tex_star_expr(TypesChecker *check, GtOperand *x, AstStarExpr *e) {
    burrow__tc_expr_or_type(check, x, e->x, false);
    switch ((Int)x->mode) {
    case GT_MODE_INVALID:
        return false;
    case GT_MODE_TYPEXPR:
        burrow__tc_valid_var_type(check, e->x, x->typ);
        x->typ = types_type(types_new_pointer(check->a, x->typ));
        return true;
    default: {
        TexIndirect c = {check, x, NULL};
        if (!burrow__types_all(x->typ,
                               BURROW_FN(GtTypePairFunc, tex_under_indirect, &c)))
            return false;
        /* We cannot dereference a pointer with an incomplete base type; make
         * sure it's complete. */
        if (!burrow__tc_is_complete(check, c.base))
            return false;
        x->mode = GT_MODE_VARIABLE;
        x->typ = c.base;
        return true;
    }
    }
}

/* exprInternal contains the core of type checking of expressions. Must only be
 * called by rawExpr. (See rawExpr for an explanation of the parameters.) */
GtExprKind burrow__tc_expr_internal(TypesChecker *check, GtTarget *T, GtOperand *x,
                                    AstExpr e, TypesType hint) {
    /* make sure x has a valid state in case of bailout (was go.dev/issue/5770) */
    burrow__tc_op_invalidate(x);
    x->typ = GT_TYP(TYPES_INVALID);

    switch (e->kind) {
    case AST_KIND_BAD_EXPR:
        goto error; /* error was reported before */

    case AST_KIND_IDENT:
        burrow__tc_ident(check, x, (AstIdent *)(void *)e, false);
        break;

    case AST_KIND_ELLIPSIS:
        /* ellipses are handled explicitly where they are valid */
        burrow__tc_error(check, burrow__tc_at_node(e), GT_CODE_INVALID_SYNTAX_TREE,
                         BURROW_S("invalid use of ..."));
        goto error;

    case AST_KIND_BASIC_LIT:
        burrow__tc_basic_lit(check, x, (AstBasicLit *)(void *)e);
        if (!burrow__tc_op_valid(x))
            goto error;
        break;

    case AST_KIND_FUNC_LIT:
        burrow__tc_func_lit(check, x, (AstFuncLit *)(void *)e);
        if (!burrow__tc_op_valid(x))
            goto error;
        break;

    case AST_KIND_COMPOSITE_LIT:
        burrow__tc_composite_lit(check, x, (AstCompositeLit *)(void *)e, hint);
        if (!burrow__tc_op_valid(x))
            goto error;
        break;

    case AST_KIND_PAREN_EXPR: {
        /* type inference doesn't go past parentheses (target type T = nil) */
        GtExprKind kind = burrow__tc_raw_expr(
            check, NULL, x, ((AstParenExpr *)(void *)e)->x, NULL, false);
        x->expr = e;
        return kind;
    }

    case AST_KIND_SELECTOR_EXPR:
        burrow__tc_selector(check, x, (AstSelectorExpr *)(void *)e, false);
        break;

    case AST_KIND_INDEX_EXPR:
    case AST_KIND_INDEX_LIST_EXPR: {
        GtIndexedExpr ix;
        (void)burrow__tc_unpack_indexed_expr(check->a, e, &ix);
        if (burrow__tc_index_expr(check, x, &ix)) {
            /* enableReverseTypeInference is true in Go, so T stays */
            (void)burrow__tc_func_inst(check, T, ast_expr_pos(e), x, &ix, true);
        }
        if (!burrow__tc_op_valid(x))
            goto error;
        break;
    }

    case AST_KIND_SLICE_EXPR:
        burrow__tc_slice_expr(check, x, (AstSliceExpr *)(void *)e);
        if (!burrow__tc_op_valid(x))
            goto error;
        break;

    case AST_KIND_TYPE_ASSERT_EXPR:
        if (!tex_type_assert_expr(check, x, (AstTypeAssertExpr *)(void *)e))
            goto error;
        break;

    case AST_KIND_CALL_EXPR:
        return burrow__tc_call_expr(check, x, (AstCallExpr *)(void *)e);

    case AST_KIND_STAR_EXPR:
        if (!tex_star_expr(check, x, (AstStarExpr *)(void *)e))
            goto error;
        break;

    case AST_KIND_UNARY_EXPR: {
        AstUnaryExpr *u = (AstUnaryExpr *)(void *)e;
        tex_unary(check, x, u);
        if (!burrow__tc_op_valid(x))
            goto error;
        if (u->op == TOKEN_ARROW) {
            x->expr = e;
            return GT_EXPR_STATEMENT; /* receive operations may appear in statement
                                         context */
        }
        break;
    }

    case AST_KIND_BINARY_EXPR: {
        AstBinaryExpr *b = (AstBinaryExpr *)(void *)e;
        burrow__tc_binary(check, x, e, b->x, b->y, b->op, b->op_pos);
        if (!burrow__tc_op_valid(x))
            goto error;
        break;
    }

    case AST_KIND_KEY_VALUE_EXPR:
        /* key:value expressions are handled in composite literals */
        burrow__tc_error(check, burrow__tc_at_node(e), GT_CODE_INVALID_SYNTAX_TREE,
                         BURROW_S("no key:value expected"));
        goto error;

    case AST_KIND_ARRAY_TYPE:
    case AST_KIND_STRUCT_TYPE:
    case AST_KIND_FUNC_TYPE:
    case AST_KIND_INTERFACE_TYPE:
    case AST_KIND_MAP_TYPE:
    case AST_KIND_CHAN_TYPE:
        x->mode = GT_MODE_TYPEXPR;
        x->typ = burrow__tc_typ(check, e);
        /* Note: rawExpr (caller of exprInternal) will call
         * check.recordTypeAndValue even though check.typ has already called
         * it. This is fine as both times the same expression and type are
         * recorded. It is also not a performance issue because we only reach
         * here for composite literal types, which are comparatively rare. */
        break;

    default: {
        Str ps = burrow__tc_ps(check, ast_expr_pos(e));
        panic_str(GT_SPRINTF(check, "%s: unknown expression type", ps));
    }
    }

    /* everything went well */
    x->expr = e;
    return GT_EXPR_EXPRESSION;

error:
    burrow__tc_op_invalidate(x);
    x->expr = e;
    return GT_EXPR_STATEMENT; /* avoid follow-up errors */
}

/* A key for keyVal: tag, then n bytes of payload, made in a. */
static Str tex_key(Alloc *a, char tag, const void *p, Int n) {
    Byte *b = burrow__types_alloc(a, (size_t)(n + 1), 1);
    b[0] = (Byte)tag;
    if (n > 0)
        memcpy(b + 1, p, (size_t)n);
    return (Str){b, n + 1};
}

/* keyVal maps a complex, float, integer, string or boolean constant value to
 * the corresponding complex128, float64, int64, uint64, string, or bool Go
 * value if possible; otherwise it returns x. A complex constant that can be
 * represented as a float (such as 1.2 + 0i) is returned as a floating point
 * value; if a floating point value can be represented as an integer (such as
 * 1.0) it is returned as an integer value. This ensures that constants of
 * different kind but equal value (such as 1.0 + 0i, 1.0, 1) result in the
 * same value.
 *
 * Not in Go: the result is a Str that is equal for two values exactly when
 * Go's any results are, so that it can key a map. It is the Go type as a
 * letter followed by the bits of the value. The one difference is the
 * fallback: Go returns the constant.Value, and two big integers holding the
 * same number are different keys there because they are different pointers,
 * where here they are the same key. Such integers don't fit any type, so an
 * error is reported for them before keyVal is asked. */
Str burrow__tc_key_val(Alloc *a, ConstantValue x) {
    switch ((Int)constant_value_kind(x)) {
    case CONSTANT_COMPLEX: {
        ConstantValue f = constant_to_float(a, x);
        if (constant_value_kind(f) != CONSTANT_FLOAT) {
            bool exact;
            double c[2];
            c[0] = constant_float64_val(constant_real(x), &exact);
            c[1] = constant_float64_val(constant_imag(x), &exact);
            for (int i = 0; i < 2; i++) {
                if (c[i] == 0)
                    c[i] = 0; /* -0 and 0 are ==, as Go compares them */
            }
            return tex_key(a, 'c', c, (Int)sizeof c);
        }
        x = f;
    }
        /* fallthrough */
    case CONSTANT_FLOAT: {
        ConstantValue i = constant_to_int(a, x);
        if (constant_value_kind(i) != CONSTANT_INT) {
            bool exact;
            double v = constant_float64_val(x, &exact);
            if (v == 0)
                v = 0;
            return tex_key(a, 'f', &v, (Int)sizeof v);
        }
        x = i;
    }
        /* fallthrough */
    case CONSTANT_INT: {
        bool ok;
        int64_t v = constant_int64_val(x, &ok);
        if (ok)
            return tex_key(a, 'i', &v, (Int)sizeof v);
        uint64_t u = constant_uint64_val(x, &ok);
        if (ok)
            return tex_key(a, 'u', &u, (Int)sizeof u);
        break;
    }
    case CONSTANT_STRING: {
        Str s = constant_string_val(x);
        return tex_key(a, 's', s.p, s.len);
    }
    case CONSTANT_BOOL: {
        Byte b = constant_bool_val(x) ? 1 : 0;
        return tex_key(a, 'b', &b, 1);
    }
    default:
        break;
    }
    if (constant_value_kind(x) == CONSTANT_UNKNOWN)
        return tex_key(a, '?', NULL, 0);
    Str s = constant_value_exact_string(x, a);
    return tex_key(a, 'x', s.p, s.len);
}

/* typeAssertion checks x.(T). The type of x must be an interface. */
void burrow__tc_type_assertion(TypesChecker *check, AstExpr e, GtOperand *x,
                               TypesType T, bool type_switch) {
    Str cause = BURROW_S("");
    if (burrow__tc_assertable_to(check, check->a, x->typ, T, &cause))
        return; /* success */

    Str es = burrow__tc_es(check, e);
    Str ts = burrow__tc_ts(check, T);
    if (type_switch) {
        Str xs = burrow__tc_xs(check, x);
        GT_ERRORF(
            check, burrow__tc_at_node(e), GT_CODE_IMPOSSIBLE_ASSERT,
            "impossible type switch case: %s\n\t%s cannot have dynamic type %s %s", es,
            xs, ts, cause);
        return;
    }

    Str xts = burrow__tc_ts(check, x->typ);
    GT_ERRORF(check, burrow__tc_at_node(e), GT_CODE_IMPOSSIBLE_ASSERT,
              "impossible type assertion: %s\n\t%s does not implement %s %s", es, ts,
              xts, cause);
}

/* expr typechecks expression e and initializes x with the expression value.
 * If a non-nil target T is given and e is a generic function or a function
 * call, T is used to infer the type arguments for e. The result must be a
 * single value. If an error occurred, x.mode is set to invalid. */
void burrow__tc_expr(TypesChecker *check, GtTarget *T, GtOperand *x, AstExpr e) {
    (void)burrow__tc_raw_expr(check, T, x, e, NULL, false);
    burrow__tc_exclude(check, x,
                       TEX_MODE_BIT(GT_MODE_NOVALUE) | TEX_MODE_BIT(GT_MODE_BUILTIN) |
                           TEX_MODE_BIT(GT_MODE_TYPEXPR));
    burrow__tc_single_value(check, x);
}

/* genericExpr is like expr but the result may also be generic. */
void burrow__tc_generic_expr(TypesChecker *check, GtOperand *x, AstExpr e,
                             TypesType hint) {
    (void)burrow__tc_raw_expr(check, NULL, x, e, hint, true);
    burrow__tc_exclude(check, x,
                       TEX_MODE_BIT(GT_MODE_NOVALUE) | TEX_MODE_BIT(GT_MODE_BUILTIN) |
                           TEX_MODE_BIT(GT_MODE_TYPEXPR));
    burrow__tc_single_value(check, x);
}

/* A dummy identifier named name at pos, for better error messages. */
static AstIdent *tex_dummy(TypesChecker *check, Str name, TokenPos pos) {
    AstIdent *dummy = ast_new_ident(check->a, name);
    if (dummy == NULL)
        burrow__types_oom();
    dummy->name_pos = pos; /* fix position */
    return dummy;
}

static GtOperand *tex_new_operand(TypesChecker *check, GtMode mode, AstExpr expr,
                                  TypesType typ) {
    GtOperand *x = GT_NEW(check->a, GtOperand);
    x->mode = mode;
    x->expr = expr;
    x->typ = typ;
    return x;
}

/* multiExpr typechecks e and returns its value (or values) in *list, a Slice
 * of GtOperand *. If allow_comma_ok is set and e is a map index, comma-ok, or
 * comma-err expression, the result is a two-element list containing the value
 * of e, and an untyped bool value or an error value, respectively. If an error
 * occurred, list[0] is not valid. The result is Go's commaOk. */
bool burrow__tc_multi_expr(TypesChecker *check, AstExpr e, bool allow_comma_ok,
                           Slice *list) {
    *list = slice_nil(TYPE_UNSAFE_POINTER);
    GtOperand *x = tex_new_operand(check, GT_MODE_INVALID, NULL, NULL);
    (void)burrow__tc_raw_expr(check, NULL, x, e, NULL, false);
    burrow__tc_exclude(check, x,
                       TEX_MODE_BIT(GT_MODE_NOVALUE) | TEX_MODE_BIT(GT_MODE_BUILTIN) |
                           TEX_MODE_BIT(GT_MODE_TYPEXPR));

    TypesTuple *t = types_as_tuple(x->typ);
    if (t != NULL && burrow__tc_op_valid(x)) {
        /* multiple values */
        *list = burrow__types_ptrs_make(check->a, t->vars.len);
        for (Int i = 0; i < t->vars.len; i++) {
            TypesVar *v = GT_AT(t->vars, i, TypesVar *);
            /* create a dummy expression (in place of e) for better error
             * messages */
            Str name = burrow__tc_nth(check, i + 1, BURROW_S("function result"));
            AstIdent *dummy = tex_dummy(check, name, ast_expr_pos(e));
            GT_AT(*list, i, GtOperand *) =
                tex_new_operand(check, GT_MODE_VALUE, GT_NODE(dummy), GT_OBJ(v)->typ);
        }
        return false;
    }

    /* exactly one (possibly invalid or comma-ok) value */
    burrow__types_ptrs_append(check->a, list, x);
    if (allow_comma_ok && (x->mode == GT_MODE_MAPINDEX || x->mode == GT_MODE_COMMAOK ||
                           x->mode == GT_MODE_COMMAERR)) {
        Str what = BURROW_S("ok value of (comma, ok) expression");
        TypesType typ = GT_TYP(TYPES_UNTYPED_BOOL);
        if (x->mode == GT_MODE_COMMAERR) {
            what = BURROW_S("err value of (comma, err) expression");
            typ = burrow__types_universe_error();
        }
        /* create a dummy expression (in place of e) for better error messages */
        AstIdent *dummy = tex_dummy(check, what, ast_expr_pos(e));
        GtOperand *x2 = tex_new_operand(check, GT_MODE_VALUE, GT_NODE(dummy), typ);
        burrow__types_ptrs_append(check->a, list, x2);
        return true;
    }
    return false;
}

/* nth returns a string of the form "nth " + what, where nth stands for 1st,
 * 2nd, 3rd, 4th, etc. depending on n. */
Str burrow__tc_nth(TypesChecker *check, Int n, Str what) {
    Str ext;
    switch (n) {
    case 1:
        ext = BURROW_S("st");
        break;
    case 2:
        ext = BURROW_S("nd");
        break;
    case 3:
        ext = BURROW_S("rd");
        break;
    default:
        ext = BURROW_S("th");
        break;
    }
    return GT_SPRINTF(check, "%d%s %s", n, ext, what);
}

/* exprOrType typechecks expression or type e and initializes x with the
 * expression value or type. If allow_generic is set, the operand type may be
 * an uninstantiated parameterized type or function value. If an error
 * occurred, x.mode is set to invalid. */
void burrow__tc_expr_or_type(TypesChecker *check, GtOperand *x, AstExpr e,
                             bool allow_generic) {
    (void)burrow__tc_raw_expr(check, NULL, x, e, NULL, allow_generic);
    burrow__tc_exclude(check, x, TEX_MODE_BIT(GT_MODE_NOVALUE));
    burrow__tc_single_value(check, x);
}

/* exclude reports an error if x.mode is in modeset and sets x.mode to invalid.
 * The modeset may contain any of 1<<novalue, 1<<builtin, 1<<typexpr. */
void burrow__tc_exclude(TypesChecker *check, GtOperand *x, unsigned modeset) {
    if ((modeset & TEX_MODE_BIT(x->mode)) == 0)
        return;
    Str xs = burrow__tc_xs(check, x);
    switch ((Int)x->mode) {
    case GT_MODE_NOVALUE:
        if ((modeset & TEX_MODE_BIT(GT_MODE_TYPEXPR)) != 0)
            GT_ERRORF(check, burrow__tc_at_op(x), GT_CODE_TOO_MANY_VALUES,
                      "%s used as value", xs);
        else
            GT_ERRORF(check, burrow__tc_at_op(x), GT_CODE_TOO_MANY_VALUES,
                      "%s used as value or type", xs);
        break;
    case GT_MODE_BUILTIN:
        GT_ERRORF(check, burrow__tc_at_op(x), GT_CODE_UNCALLED_BUILTIN,
                  "%s must be called", xs);
        break;
    case GT_MODE_TYPEXPR:
        GT_ERRORF(check, burrow__tc_at_op(x), GT_CODE_NOT_AN_EXPR,
                  "%s is not an expression", xs);
        break;
    default:
        panic_str(BURROW_S("unreachable"));
    }
    burrow__tc_op_invalidate(x);
}

/* singleValue reports an error if x describes a tuple and sets x.mode to
 * invalid. */
void burrow__tc_single_value(TypesChecker *check, GtOperand *x) {
    if (x->mode != GT_MODE_VALUE)
        return;
    /* tuple types are never named - no need for underlying type below */
    TypesTuple *t = types_as_tuple(x->typ);
    if (t != NULL) {
        burrow__types_assert(t->vars.len != 1);
        Str xs = burrow__tc_xs(check, x);
        GT_ERRORF(check, burrow__tc_at_op(x), GT_CODE_TOO_MANY_VALUES,
                  "multiple-value %s in single-value context", xs);
        burrow__tc_op_invalidate(x);
    }
}
