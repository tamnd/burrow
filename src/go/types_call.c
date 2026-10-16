/* go/types: typechecking of call and selector expressions.
 *
 * Derived from Go's src/go/types/call.go. Go source: go1.27.1.
 *
 * Copyright 2013 The Go Authors. All rights reserved.
 * Copyright 2026 The burrow Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style licence that can be found
 * in the LICENSE file. */

#include "burrow/go/types.h"

#include "burrow/core.h"
#include "burrow/error.h"
#include "burrow/go/ast.h"
#include "burrow/go/constant.h"
#include "burrow/go/token.h"
#include "burrow/map.h"
#include "burrow/panic.h"
#include "burrow/slice.h"
#include "burrow/strings.h"

#include "types_check.h"
#include "types_internal.h"

#include <stdbool.h>

#define TCA_MODE_BIT(m) (1u << (unsigned)(m))

static GtOperand *tca_new_operand(TypesChecker *check, GtMode mode, AstExpr expr,
                                  TypesType typ) {
    GtOperand *x = GT_NEW(check->a, GtOperand);
    x->mode = mode;
    x->expr = expr;
    x->typ = typ;
    return x;
}

static void tca_set_used(Map *m, const void *key, bool used) {
    if (!map_set(m, key, &used))
        burrow__types_oom();
}

bool burrow__tc_has_dots(const AstCallExpr *call) {
    return call->ellipsis != TOKEN_NO_POS;
}

/* funcInst type-checks a function instantiation. The incoming x must be a
 * generic function. If ix != NULL, it provides some or all of the type
 * arguments (ix->indices). If T != NULL, it may be used to infer missing type
 * arguments of x, if any. At least one of T or ix must be provided.
 *
 * There are two modes of operation:
 *
 *  1. If infer is set, funcInst infers missing type arguments as needed and
 *     instantiates the function x. The returned results are nil.
 *
 *  2. If infer is not set and ix provides all type arguments, funcInst
 *     instantiates the function x. The returned results are nil. If ix doesn't
 *     provide enough type arguments, funcInst returns the available arguments;
 *     x remains unchanged.
 *
 * If an error (other than a version error) occurs in any case, it is reported
 * and x->mode is set to invalid. */
Slice burrow__tc_func_inst(TypesChecker *check, GtTarget *T, TokenPos pos, GtOperand *x,
                           GtIndexedExpr *ix, bool infer) {
    burrow__types_assert(T != NULL || ix != NULL);
    Slice none = slice_nil(TYPE_UNSAFE_POINTER);

    GtPosn inst_err_pos;
    if (ix != NULL) {
        inst_err_pos = burrow__tc_in_node(ix->orig, ix->lbrack);
        x->expr = ix->orig; /* if we don't have an index expression, keep the
                               existing expression of x */
    } else {
        inst_err_pos = burrow__tc_at_pos(pos);
    }
    bool version_err = !burrow__tc_verify_version(
        check, inst_err_pos, burrow__tc_go1_18, BURROW_S("function instantiation"));

    /* targs and xlist are the type arguments and corresponding type
     * expressions, or nil. */
    Slice targs = none;
    Slice xlist = none;
    if (ix != NULL) {
        xlist = ix->indices;
        if (!burrow__tc_type_list(check, xlist, &targs)) {
            burrow__tc_op_invalidate(x);
            return none;
        }
        burrow__types_assert(targs.len == xlist.len);
    }

    /* Check the number of type arguments (got) vs number of type parameters
     * (want). Note that x is a function value, not a type expression, so we
     * don't need to call Underlying below. */
    TypesSignature *sig = types_as_signature(x->typ);
    burrow__types_assert(sig != NULL);
    Int got = targs.len;
    Int want = types_type_param_list_len(sig->tparams);
    if (got > want) {
        /* Providing too many type arguments is always an error. */
        burrow__types_assert(ix != NULL);
        AstExpr last = GT_AT(ix->indices, got - 1, AstExpr);
        GT_ERRORF(check, burrow__tc_at_node(last), GT_CODE_WRONG_TYPE_ARG_COUNT,
                  "got %d type arguments but want %d", got, want);
        burrow__tc_op_invalidate(x);
        return none;
    }

    if (got < want) {
        if (!infer)
            return targs;

        /* If the uninstantiated or partially instantiated function x is used in
         * an assignment (tsig != nil), infer missing type arguments by treating
         * the assignment
         *
         *    var tvar tsig = x
         *
         * like a call g(tvar) of the synthetic generic function g
         *
         *    func g[type_parameters_of_x](func_type_of_x) */
        Slice args = slice_nil(TYPE_UNSAFE_POINTER);
        Slice params = slice_nil(TYPE_UNSAFE_POINTER);
        bool reverse = false;
        if (T != NULL && sig->tparams != NULL) {
            if (!version_err && !burrow__tc_allow_version(check, burrow__tc_go1_21)) {
                if (ix != NULL)
                    burrow__tc_version_error(
                        check, inst_err_pos, burrow__tc_go1_21,
                        BURROW_S("partially instantiated function in assignment"));
                else
                    burrow__tc_version_error(
                        check, inst_err_pos, burrow__tc_go1_21,
                        BURROW_S("implicitly instantiated function in assignment"));
            }
            TypesSignature *gsig =
                types_new_signature_type(check->a, NULL, slice_nil(TYPE_UNSAFE_POINTER),
                                         slice_nil(TYPE_UNSAFE_POINTER), sig->params,
                                         sig->results, sig->variadic);
            burrow__types_ptrs_append(check->a, &params,
                                      types_new_param(check->a, burrow__tc_op_pos(x),
                                                      check->pkg, BURROW_S(""),
                                                      types_type(gsig)));
            /* The type of the argument operand is tsig, which is the type of the
             * LHS in an assignment or the result type in a return statement.
             * Create a pseudo-expression for that operand that makes sense when
             * reported in error messages from infer, below. */
            AstIdent *expr = ast_new_ident(check->a, T->desc);
            if (expr == NULL)
                burrow__types_oom();
            expr->name_pos = burrow__tc_op_pos(x); /* correct position */
            burrow__types_ptrs_append(check->a, &args,
                                      tca_new_operand(check, GT_MODE_VALUE,
                                                      GT_NODE(expr),
                                                      types_type(T->sig)));
            reverse = true;
        }

        /* Rename type parameters to avoid problems with recursive
         * instantiations. Note that types_new_tuple(params) below is NULL if
         * params is empty, as desired. */
        TypesType params2 = NULL;
        Slice tparams = burrow__tc_rename_tparams(
            check, pos, burrow__types_tparam_list(sig->tparams),
            types_type(types_new_tuple(check->a, params)), &params2);

        GtError *err = burrow__tc_new_error(check, GT_CODE_CANNOT_INFER_TYPE_ARGS);
        targs = burrow__tc_infer(check, burrow__tc_at_pos(pos), tparams, targs,
                                 types_as_tuple(params2), args, reverse, err);
        if (targs.p == NULL) {
            if (!burrow__tc_error_empty(err))
                burrow__tc_error_report(err);
            burrow__tc_op_invalidate(x);
            return none;
        }
        got = targs.len;
    }
    burrow__types_assert(got == want);

    /* instantiate function signature */
    sig = burrow__tc_instantiate_signature(check, burrow__tc_op_pos(x), x->expr, sig,
                                           targs, xlist);
    x->typ = types_type(sig);
    x->mode = GT_MODE_VALUE;
    return none;
}

/* What the delayed verification of an instantiated signature needs. */
typedef struct TcaVerify {
    TokenPos pos;
    TypesSignature *typ;
    Slice targs;
    Slice xlist;
} TcaVerify;

static void tca_verify_later(TypesChecker *check, void *env) {
    TcaVerify *c = env;
    Slice tparams = burrow__types_tparam_list(c->typ->tparams);
    /* check type constraints */
    Error err = {0};
    Int i = burrow__tc_verify(check, c->pos, tparams, c->targs,
                              burrow__tc_context(check), &err);
    if (i >= 0) {
        /* best position for error reporting */
        TokenPos pos = c->pos;
        if (i < c->xlist.len)
            pos = ast_expr_pos(GT_AT(c->xlist, i, AstExpr));
        burrow__tc_soft_error(check, burrow__tc_at_pos(pos), GT_CODE_INVALID_TYPE_ARG,
                              error_text(err));
    } else {
        burrow__tc_mono_record_instance(check, check->pkg, c->pos, tparams, c->targs,
                                        c->xlist);
    }
}

TypesSignature *burrow__tc_instantiate_signature(TypesChecker *check, TokenPos pos,
                                                 AstExpr expr, TypesSignature *typ,
                                                 Slice targs, Slice xlist) {
    burrow__types_assert(check != NULL);
    burrow__types_assert(targs.len == types_type_param_list_len(typ->tparams));

    /* For signatures, burrow__tc_instance will always succeed because the type
     * argument count is correct at this point (see assertion above); hence the
     * result is always a signature. */
    TypesSignature *inst = types_as_signature(burrow__tc_instance(
        check, pos, types_type(typ), targs, NULL, burrow__tc_context(check)));
    burrow__types_assert(inst != NULL);
    /* signature is not generic anymore */
    burrow__types_assert(types_type_param_list_len(inst->tparams) == 0);
    burrow__tc_record_instance(check, expr, targs, types_type(inst));
    burrow__types_assert(xlist.len <= targs.len);

    /* verify instantiation lazily (was go.dev/issue/50450) */
    TcaVerify *c = GT_NEW(check->a, TcaVerify);
    c->pos = pos;
    c->typ = typ;
    c->targs = targs;
    c->xlist = xlist;
    (void)burrow__tc_later(check, tca_verify_later, c);

    return inst;
}

/* The condition of callExpr's commonUnder: every type must be a function. */
static bool tca_func_cond(void *env, TypesType t, TypesType u, Str *cause) {
    TypesChecker *check = env;
    if (u != NULL && types_as_signature(u) == NULL) {
        Str ts = burrow__tc_ts(check, t);
        *cause = GT_SPRINTF(check, "%s is not a function", ts);
        return false;
    }
    return true;
}

/* The conversion T(args) of callExpr, with x holding the type T. */
static GtExprKind tca_conversion(TypesChecker *check, GtOperand *x, AstCallExpr *call) {
    burrow__tc_non_generic(check, NULL, x);
    if (!burrow__tc_op_valid(x))
        return GT_EXPR_CONVERSION;
    TypesType T = x->typ;
    burrow__tc_op_invalidate(x);
    /* We cannot convert a value to an incomplete type; make sure it's
     * complete. */
    if (!burrow__tc_is_complete(check, T)) {
        x->expr = GT_NODE(call);
        return GT_EXPR_CONVERSION;
    }
    Int n = call->args.len;
    if (n == 0) {
        Str ts = burrow__tc_ts(check, T);
        GT_ERRORF(check, burrow__tc_in_node(GT_NODE(call), call->rparen),
                  GT_CODE_WRONG_ARG_COUNT, "missing argument in conversion to %s", ts);
    } else if (n == 1) {
        AstExpr arg = GT_AT(call->args, 0, AstExpr);
        burrow__tc_expr(
            check, burrow__tc_new_target(check->a, T, BURROW_S("conversion")), x, arg);
        if (burrow__tc_op_valid(x)) {
            TypesInterface *t = types_as_interface(types_type_underlying(T));
            if (burrow__tc_has_dots(call)) {
                Str ts = burrow__tc_ts(check, T);
                GT_ERRORF(check, burrow__tc_at_node(arg),
                          GT_CODE_BAD_DOT_DOT_DOT_SYNTAX,
                          "invalid use of ... in conversion to %s", ts);
            } else if (t != NULL && !burrow__types_is_type_param(T) &&
                       !types_interface_is_method_set(t)) {
                Str ts = burrow__tc_ts(check, T);
                GT_ERRORF(
                    check, GT_AT_NODE(call), GT_CODE_MISPLACED_CONSTRAINT_IFACE,
                    "cannot use interface %s in conversion (contains specific type "
                    "constraints or is comparable)",
                    ts);
            } else {
                burrow__tc_conversion(check, x, T);
            }
        }
    } else {
        (void)burrow__tc_use(check, call->args);
        AstExpr last = GT_AT(call->args, n - 1, AstExpr);
        Str ts = burrow__tc_ts(check, T);
        GT_ERRORF(check, burrow__tc_at_node(last), GT_CODE_WRONG_ARG_COUNT,
                  "too many arguments in conversion to %s", ts);
    }
    x->expr = GT_NODE(call);
    return GT_EXPR_CONVERSION;
}

/* callExpr's way out when the call is not valid. */
static GtExprKind tca_call_error(TypesChecker *check, GtOperand *x, AstCallExpr *call) {
    (void)burrow__tc_use(check, call->args);
    burrow__tc_op_invalidate(x);
    x->expr = GT_NODE(call);
    return GT_EXPR_STATEMENT;
}

GtExprKind burrow__tc_call_expr(TypesChecker *check, GtOperand *x, AstCallExpr *call) {
    GtIndexedExpr ixv;
    GtIndexedExpr *ix = NULL;
    if (burrow__tc_unpack_indexed_expr(check->a, call->fun, &ixv)) {
        ix = &ixv;
        if (burrow__tc_index_expr(check, x, ix)) {
            /* Delay function instantiation to argument checking, where we
             * combine type and value arguments for type inference. */
            burrow__types_assert(x->mode == GT_MODE_VALUE);
        } else {
            ix = NULL;
        }
        x->expr = call->fun;
        burrow__tc_record(check, x);
    } else {
        burrow__tc_expr_or_type(check, x, call->fun, true);
    }
    /* x->typ may be generic */

    switch (x->mode) {
    case GT_MODE_INVALID:
        (void)burrow__tc_use(check, call->args);
        x->expr = GT_NODE(call);
        return GT_EXPR_STATEMENT;

    case GT_MODE_TYPEXPR:
        /* conversion */
        return tca_conversion(check, x, call);

    case GT_MODE_BUILTIN: {
        /* no need to check for non-genericity here */
        Int id = x->id;
        if (!burrow__tc_builtin(check, x, call, id))
            burrow__tc_op_invalidate(x);
        x->expr = GT_NODE(call);
        /* a non-constant result implies a function call */
        if (burrow__tc_op_valid(x) && x->mode != GT_MODE_CONSTANT)
            check->env.has_call_or_recv = true;
        return burrow__types_predeclared_funcs[id].kind == 1 ? GT_EXPR_STATEMENT
                                                             : GT_EXPR_EXPRESSION;
    }

    case GT_MODE_NOVALUE:
    case GT_MODE_CONSTANT:
    case GT_MODE_VARIABLE:
    case GT_MODE_MAPINDEX:
    case GT_MODE_VALUE:
    case GT_MODE_NILVALUE:
    case GT_MODE_COMMAOK:
    case GT_MODE_COMMAERR:
    case GT_MODE_CGOFUNC:
    default:
        break;
    }

    /* ordinary function/method call; signature may be generic */
    bool cgocall = x->mode == GT_MODE_CGOFUNC;

    /* If the operand type is a type parameter, all types in its type set must
     * have a common underlying type, which must be a signature. */
    Str cause = {0};
    TypesType u = burrow__tc_common_under(
        check, x->typ, BURROW_FN(GtUnderCond, tca_func_cond, check), &cause);
    if (u == NULL) {
        Str xs = burrow__tc_xs(check, x);
        GT_ERRORF(check, burrow__tc_at_op(x), GT_CODE_INVALID_CALL,
                  GT_INVALID_OP "cannot call %s: %s", xs, cause);
        burrow__tc_op_invalidate(x);
        x->expr = GT_NODE(call);
        return GT_EXPR_STATEMENT;
    }
    /* u must be a signature per the commonUnder condition */
    TypesSignature *sig = types_as_signature(u);
    burrow__types_assert(sig != NULL);

    /* Capture was_generic before sig is potentially instantiated below. */
    bool was_generic = types_type_param_list_len(sig->tparams) > 0;

    /* evaluate type arguments, if any */
    Slice xlist = slice_nil(TYPE_UNSAFE_POINTER);
    Slice targs = slice_nil(TYPE_UNSAFE_POINTER);
    if (ix != NULL) {
        xlist = ix->indices;
        if (!burrow__tc_type_list(check, xlist, &targs))
            return tca_call_error(check, x, call);
        burrow__types_assert(targs.len == xlist.len);

        /* check number of type arguments (got) vs number of type parameters
         * (want) */
        Int got = targs.len;
        Int want = types_type_param_list_len(sig->tparams);
        if (got > want) {
            AstExpr at = GT_AT(xlist, want, AstExpr);
            GT_ERRORF(check, burrow__tc_at_node(at), GT_CODE_WRONG_TYPE_ARG_COUNT,
                      "got %d type arguments but want %d", got, want);
            return tca_call_error(check, x, call);
        }

        /* If sig is generic and all type arguments are provided, preempt
         * function argument type inference by explicitly instantiating the
         * signature. This ensures that we record accurate type information for
         * sig, even if there is an error checking its arguments (for example,
         * if an incorrect number of arguments is supplied). */
        if (got == want && want > 0) {
            (void)burrow__tc_verify_version(check, burrow__tc_at_pos(ix->lbrack),
                                            burrow__tc_go1_18,
                                            BURROW_S("function instantiation"));
            sig = burrow__tc_instantiate_signature(check, ast_expr_pos(ix->orig),
                                                   ix->orig, sig, targs, xlist);
            /* targs have been consumed; proceed with checking arguments of the
             * non-generic signature. */
            targs = slice_nil(TYPE_UNSAFE_POINTER);
            xlist = slice_nil(TYPE_UNSAFE_POINTER);
        }
    }

    /* evaluate arguments */
    Slice atargs;
    Slice args = burrow__tc_generic_expr_list(check, call->args, &atargs);
    sig = burrow__tc_arguments(check, call, sig, targs, xlist, args, atargs);

    if (was_generic && types_type_param_list_len(sig->tparams) == 0) {
        /* Update the recorded type of call->fun to its instantiated type. */
        ConstantValue nil = {0};
        burrow__tc_record_type_and_value(check, call->fun, GT_MODE_VALUE,
                                         types_type(sig), nil);
    }

    /* determine result */
    switch (types_tuple_len(sig->results)) {
    case 0:
        x->mode = GT_MODE_NOVALUE;
        break;
    case 1: {
        x->mode = cgocall ? GT_MODE_COMMAERR : GT_MODE_VALUE;
        TypesType typ = GT_OBJ(GT_AT(sig->results->vars, 0, TypesVar *))->typ; /* unpack
                                                                                  tuple */
        /* We cannot return a value of an incomplete type; make sure it's
         * complete. */
        if (!burrow__tc_is_complete(check, typ)) {
            burrow__tc_op_invalidate(x);
            x->expr = GT_NODE(call);
            return GT_EXPR_STATEMENT;
        }
        x->typ = typ;
        break;
    }
    default:
        x->mode = GT_MODE_VALUE;
        x->typ = types_type(sig->results);
        break;
    }
    x->expr = GT_NODE(call);
    check->env.has_call_or_recv = true;

    /* if type inference failed, a parameterized result must be invalidated
     * (operands cannot have a parameterized type) */
    if (x->mode == GT_MODE_VALUE && types_type_param_list_len(sig->tparams) > 0 &&
        burrow__tc_is_parameterized(check->a, burrow__types_tparam_list(sig->tparams),
                                    x->typ))
        burrow__tc_op_invalidate(x);

    return GT_EXPR_STATEMENT;
}

/* exprList evaluates a list of expressions and returns the corresponding
 * operands. A single-element expression list may evaluate to multiple
 * operands. */
Slice burrow__tc_expr_list(TypesChecker *check, Slice elist) {
    Slice xlist = slice_nil(TYPE_UNSAFE_POINTER);
    Int n = elist.len;
    if (n == 1) {
        (void)burrow__tc_multi_expr(check, GT_AT(elist, 0, AstExpr), false, &xlist);
    } else if (n > 1) {
        /* multiple (possibly invalid) values */
        xlist = burrow__types_ptrs_make(check->a, n);
        for (Int i = 0; i < n; i++) {
            GtOperand *x = tca_new_operand(check, GT_MODE_INVALID, NULL, NULL);
            burrow__tc_expr(check, NULL, x, GT_AT(elist, i, AstExpr));
            GT_AT(xlist, i, GtOperand *) = x;
        }
    }
    return xlist;
}

/* The generic function instantiation case of genericExprList: x is the
 * generic function ix stands for. The (partial) type arguments go to *targs
 * if x was not instantiated. */
static void tca_generic_inst(TypesChecker *check, GtOperand *x, GtIndexedExpr *ix,
                             bool infer, Slice **targs) {
    Slice t = burrow__tc_func_inst(check, NULL, burrow__tc_op_pos(x), x, ix, infer);
    if (t.p != NULL) {
        /* x was not instantiated: collect the (partial) type arguments. */
        Slice *p = GT_NEW(check->a, Slice);
        *p = t;
        *targs = p;
        /* Update x->expr so that we can record the partially instantiated
         * function. */
        x->expr = ix->orig;
    } else {
        /* x was instantiated: we must record it here because we didn't use the
         * usual expression evaluators. */
        burrow__tc_record(check, x);
    }
}

/* genericExprList is like exprList but result operands may be uninstantiated
 * or partially instantiated generic functions (where constraint information is
 * insufficient to infer the missing type arguments) for Go 1.21 and later. For
 * each non-generic or uninstantiated generic operand, the corresponding
 * *targs_list elements do not exist (*targs_list is nil) or the elements are
 * NULL. For each partially instantiated generic function operand, the
 * corresponding *targs_list elements are the operand's partial type
 * arguments. */
Slice burrow__tc_generic_expr_list(TypesChecker *check, Slice elist,
                                   Slice *targs_list) {
    Slice res_list = slice_nil(TYPE_UNSAFE_POINTER);
    *targs_list = slice_nil(TYPE_UNSAFE_POINTER);

    /* Before Go 1.21, uninstantiated or partially instantiated argument
     * functions are not permitted. funcInst must infer missing type arguments
     * in that case. */
    bool infer = true; /* for -lang < go1.21 */
    Int n = elist.len;
    if (n > 0 && burrow__tc_allow_version(check, burrow__tc_go1_21))
        infer = false;

    if (n == 1) {
        /* single value (possibly a partially instantiated function), or a
         * multi-valued expression */
        AstExpr e = GT_AT(elist, 0, AstExpr);
        GtOperand *x = tca_new_operand(check, GT_MODE_INVALID, NULL, NULL);
        GtIndexedExpr ix;
        if (burrow__tc_unpack_indexed_expr(check->a, e, &ix) &&
            burrow__tc_index_expr(check, x, &ix)) {
            /* x is a generic function. */
            Slice *targs = NULL;
            tca_generic_inst(check, x, &ix, infer, &targs);
            if (targs != NULL) {
                *targs_list = burrow__types_ptrs_make(check->a, 1);
                GT_AT(*targs_list, 0, Slice *) = targs;
            }
            burrow__types_ptrs_append(check->a, &res_list, x);
        } else {
            /* x is not a function instantiation (it may still be a generic
             * function). */
            (void)burrow__tc_raw_expr(check, NULL, x, e, NULL, true);
            burrow__tc_exclude(check, x,
                               TCA_MODE_BIT(GT_MODE_NOVALUE) |
                                   TCA_MODE_BIT(GT_MODE_BUILTIN) |
                                   TCA_MODE_BIT(GT_MODE_TYPEXPR));
            TypesTuple *t = types_as_tuple(x->typ);
            if (t != NULL && burrow__tc_op_valid(x)) {
                /* x is a function call returning multiple values; it cannot be
                 * generic. */
                res_list = burrow__types_ptrs_make(check->a, t->vars.len);
                for (Int i = 0; i < t->vars.len; i++) {
                    TypesVar *v = GT_AT(t->vars, i, TypesVar *);
                    GT_AT(res_list, i, GtOperand *) =
                        tca_new_operand(check, GT_MODE_VALUE, e, GT_OBJ(v)->typ);
                }
            } else {
                /* x is exactly one value (possibly invalid or uninstantiated
                 * generic function). */
                burrow__types_ptrs_append(check->a, &res_list, x);
            }
        }
    } else if (n > 1) {
        /* multiple values */
        res_list = burrow__types_ptrs_make(check->a, n);
        *targs_list = burrow__types_ptrs_make(check->a, n);
        for (Int i = 0; i < n; i++) {
            AstExpr e = GT_AT(elist, i, AstExpr);
            GtOperand *x = tca_new_operand(check, GT_MODE_INVALID, NULL, NULL);
            GtIndexedExpr ix;
            if (burrow__tc_unpack_indexed_expr(check->a, e, &ix) &&
                burrow__tc_index_expr(check, x, &ix)) {
                /* x is a generic function. */
                Slice *targs = NULL;
                tca_generic_inst(check, x, &ix, infer, &targs);
                GT_AT(*targs_list, i, Slice *) = targs;
            } else {
                /* x is exactly one value (possibly invalid or uninstantiated
                 * generic function). */
                burrow__tc_generic_expr(check, x, e, NULL);
            }
            GT_AT(res_list, i, GtOperand *) = x;
        }
    }

    return res_list;
}

/* Appends NULL type arguments to *targs until it is as long as tparams. */
static void tca_pad_targs(TypesChecker *check, Slice *targs, Slice tparams) {
    while (targs->len < tparams.len)
        burrow__types_ptrs_append(check->a, targs, NULL);
}

/* Appends all of src to *dst, both of pointers. */
static void tca_append_all(TypesChecker *check, Slice *dst, Slice src) {
    for (Int i = 0; i < src.len; i++)
        burrow__types_ptrs_append(check->a, dst, GT_AT(src, i, void *));
}

/* arguments type-checks arguments passed to a function call with the given
 * signature. The function and its arguments may be generic, and possibly
 * partially instantiated. targs and xlist are the function's type arguments
 * (and corresponding expressions). args are the function arguments. If an
 * argument args[i] is a partially instantiated generic function, atargs[i] are
 * the corresponding type arguments. If the callee is variadic, arguments
 * adjusts its signature to match the provided arguments. The type parameters
 * and arguments of the callee and all its arguments are used together to
 * infer any missing type arguments, and the callee and argument functions are
 * instantiated as necessary. The result signature is the (possibly adjusted
 * and instantiated) function signature. If an error occurred, the result
 * signature is the incoming sig. */
TypesSignature *burrow__tc_arguments(TypesChecker *check, AstCallExpr *call,
                                     TypesSignature *sig, Slice targs, Slice xlist,
                                     Slice args, Slice atargs) {
    TypesSignature *rsig = sig;

    /* Function call argument/parameter count requirements
     *
     *               | standard call    | dotdotdot call |
     * --------------+------------------+----------------+
     * standard func | nargs == npars   | invalid        |
     * --------------+------------------+----------------+
     * variadic func | nargs >= npars-1 | nargs == npars |
     * --------------+------------------+----------------+ */

    Int nargs = args.len;
    Int npars = types_tuple_len(sig->params);
    bool ddd = burrow__tc_has_dots(call);

    /* set up parameters */
    TypesTuple *sig_params = sig->params; /* adjusted for variadic functions (may be
                                             NULL for empty parameter lists!) */
    bool adjusted = false; /* indicates if sig_params is different from sig->params */
    if (sig->variadic) {
        if (ddd) {
            /* variadic_func(a, b, c...) */
            if (call->args.len == 1 && nargs > 1) {
                /* f()... is not permitted if f() is multi-valued */
                Str es = burrow__tc_es(check, GT_AT(call->args, 0, AstExpr));
                GT_ERRORF(check, burrow__tc_in_node(GT_NODE(call), call->ellipsis),
                          GT_CODE_INVALID_DOT_DOT_DOT,
                          "cannot use ... with %d-valued %s", nargs, es);
                return rsig;
            }
        } else {
            /* variadic_func(a, b, c) */
            if (nargs >= npars - 1) {
                /* Create custom parameters for arguments: keep the first npars-1
                 * parameters and add one for each argument mapping to the ...
                 * parameter. */
                Slice vars = burrow__types_ptrs_make(check->a, npars - 1); /* npars > 0
                                                                              for
                                                                              variadic
                                                                              functions */
                for (Int i = 0; i < npars - 1; i++)
                    GT_AT(vars, i, TypesVar *) =
                        GT_AT(sig->params->vars, i, TypesVar *);
                TypesVar *last = GT_AT(sig->params->vars, npars - 1, TypesVar *);
                TypesSlice *ls = types_as_slice(GT_OBJ(last)->typ);
                burrow__types_assert(ls != NULL);
                TypesType typ = ls->elem;
                while (vars.len < nargs)
                    burrow__types_ptrs_append(
                        check->a, &vars,
                        types_new_param(check->a, GT_OBJ(last)->pos, GT_OBJ(last)->pkg,
                                        GT_OBJ(last)->name, typ));
                sig_params = types_new_tuple(check->a, vars); /* possibly NULL! */
                adjusted = true;
                npars = nargs;
            } else {
                /* nargs < npars-1 */
                npars--; /* for correct error message below */
            }
        }
    } else {
        if (ddd) {
            /* standard_func(a, b, c...) */
            Str es = burrow__tc_es(check, call->fun);
            GT_ERRORF(check, burrow__tc_in_node(GT_NODE(call), call->ellipsis),
                      GT_CODE_NON_VARIADIC_DOT_DOT_DOT,
                      "cannot use ... in call to non-variadic %s", es);
            return rsig;
        }
        /* standard_func(a, b, c) */
    }

    /* check argument count */
    if (nargs != npars) {
        GtPosn at;
        Str qualifier = BURROW_S("not enough");
        if (nargs > npars) {
            /* report at first extra argument */
            at = burrow__tc_at_node(GT_AT(args, npars, GtOperand *)->expr);
            qualifier = BURROW_S("too many");
        } else {
            at = burrow__tc_at_pos(call->rparen); /* report at closing ) */
        }
        /* take care of empty parameter lists represented by NULL tuples */
        Slice params = slice_nil(TYPE_UNSAFE_POINTER);
        if (sig->params != NULL)
            params = sig->params->vars;
        GtError *err = burrow__tc_new_error(check, GT_CODE_WRONG_ARG_COUNT);
        Str es = burrow__tc_es(check, call->fun);
        GT_ADDF(err, at, "%s arguments in call to %s", qualifier, es);
        Str have = burrow__tc_types_summary(
            check, burrow__tc_operand_types(check->a, args), false, ddd);
        GT_ADDF(err, burrow__tc_at_pos(TOKEN_NO_POS), "have %s", have);
        Str want = burrow__tc_types_summary(
            check, burrow__tc_var_types(check->a, params), sig->variadic, false);
        GT_ADDF(err, burrow__tc_at_pos(TOKEN_NO_POS), "want %s", want);
        burrow__tc_error_report(err);
        return rsig;
    }

    /* collect type parameters of callee and generic function arguments */
    Slice tparams = slice_nil(TYPE_UNSAFE_POINTER);

    /* collect type parameters of callee */
    Int n = types_type_param_list_len(sig->tparams);
    if (n > 0) {
        if (!burrow__tc_allow_version(check, burrow__tc_go1_18)) {
            GtIndexedExpr ix;
            if (burrow__tc_unpack_indexed_expr(check->a, call->fun, &ix))
                burrow__tc_version_error(
                    check, burrow__tc_in_node(call->fun, ix.lbrack), burrow__tc_go1_18,
                    BURROW_S("function instantiation"));
            else
                burrow__tc_version_error(
                    check, burrow__tc_in_node(GT_NODE(call), call->lparen),
                    burrow__tc_go1_18, BURROW_S("implicit function instantiation"));
        }
        /* rename type parameters to avoid problems with recursive calls */
        TypesType tmp = NULL;
        tparams = burrow__tc_rename_tparams(check, ast_node_pos(GT_NODE(call)),
                                            burrow__types_tparam_list(sig->tparams),
                                            types_type(sig_params), &tmp);
        sig_params = types_as_tuple(tmp);
        /* make sure targs and tparams have the same length */
        tca_pad_targs(check, &targs, tparams);
    }
    burrow__types_assert(tparams.len == targs.len);

    /* collect type parameters from generic function arguments */
    Slice generic_args = slice_nil(TYPE_INT); /* indices of generic function
                                                 arguments */
    /* enableReverseTypeInference is true in Go */
    for (Int i = 0; i < args.len; i++) {
        GtOperand *arg = GT_AT(args, i, GtOperand *);
        /* generic arguments cannot have a defined (*Named) type - no need for
         * underlying type below */
        TypesSignature *asig = types_as_signature(arg->typ);
        if (asig != NULL && types_type_param_list_len(asig->tparams) > 0) {
            /* The argument type is a generic function signature. This type is
             * pointer-identical with (it's copied from) the type of the generic
             * function argument and thus the function object. Before we change
             * the type (type parameter renaming, below), make a clone of it as
             * otherwise we implicitly modify the object's type
             * (go.dev/issues/63260). */
            TypesSignature *clone = GT_NEW(check->a, TypesSignature);
            *clone = *asig;
            asig = clone;
            /* Rename type parameters for cases like f(g, g); this gives each
             * generic function argument a unique type identity
             * (go.dev/issues/59956). */
            TypesType tmp = NULL;
            Slice atparams = burrow__tc_rename_tparams(
                check, ast_node_pos(GT_NODE(call)),
                burrow__types_tparam_list(asig->tparams), types_type(asig), &tmp);
            asig = types_as_signature(tmp);
            burrow__types_assert(asig != NULL);
            /* renameTParams doesn't touch associated type parameters */
            TypesTypeParamList *list = GT_NEW(check->a, TypesTypeParamList);
            list->tparams = atparams;
            asig->tparams = list;
            arg->typ = types_type(asig); /* new type identity for the function
                                            argument */
            tca_append_all(check, &tparams, atparams);
            /* add partial list of type arguments, if any */
            if (i < atargs.len && GT_AT(atargs, i, Slice *) != NULL)
                tca_append_all(check, &targs, *GT_AT(atargs, i, Slice *));
            /* make sure targs and tparams have the same length */
            tca_pad_targs(check, &targs, tparams);
            generic_args = slice_append(check->a, generic_args, &i, 1);
            if (generic_args.p == NULL)
                burrow__types_oom();
        }
    }
    burrow__types_assert(tparams.len == targs.len);

    /* at the moment we only support implicit instantiations of argument
     * functions */
    if (generic_args.len > 0) {
        Int first = GT_AT(generic_args, 0, Int);
        (void)burrow__tc_verify_version(
            check, burrow__tc_at_op(GT_AT(args, first, GtOperand *)), burrow__tc_go1_21,
            BURROW_S("implicitly instantiated function as argument"));
    }

    /* tparams holds the type parameters of the callee and generic function
     * arguments, if any: the first n type parameters belong to the callee,
     * followed by mi type parameters for each of the generic function
     * arguments, where mi = args[i].typ.(*Signature).TypeParams().Len(). */

    /* infer missing type arguments of callee and function arguments */
    if (tparams.len > 0) {
        GtError *err = burrow__tc_new_error(check, GT_CODE_CANNOT_INFER_TYPE_ARGS);
        targs = burrow__tc_infer(check, GT_AT_NODE(call), tparams, targs, sig_params,
                                 args, false, err);
        if (targs.p == NULL) {
            if (!burrow__tc_error_empty(err)) {
                Str es = burrow__tc_es(check, call->fun);
                Str msg = burrow__tc_error_msg(err);
                GT_ERRORF(check, burrow__tc_error_posn(err),
                          GT_CODE_CANNOT_INFER_TYPE_ARGS, "in call to %s, %s", es, msg);
            }
            return rsig;
        }

        /* update result signature: instantiate if needed */
        TokenPos pos = ast_node_pos(GT_NODE(call));
        if (n > 0) {
            rsig = burrow__tc_instantiate_signature(check, pos, call->fun, sig,
                                                    slice_sub(targs, 0, n), xlist);
            /* If the callee's parameter list was adjusted we need to update
             * (instantiate) it separately. Otherwise we can simply use the
             * result signature's parameter list. */
            if (adjusted) {
                Map *smap = burrow__types_make_subst_map(
                    check->a, slice_sub(tparams, 0, n), slice_sub(targs, 0, n));
                sig_params = types_as_tuple(
                    burrow__tc_subst(check, pos, types_type(sig_params), smap, NULL,
                                     burrow__tc_context(check)));
            } else {
                sig_params = rsig->params;
            }
        }

        /* compute argument signatures: instantiate if needed */
        Int j = n;
        for (Int gi = 0; gi < generic_args.len; gi++) {
            GtOperand *arg = GT_AT(args, GT_AT(generic_args, gi, Int), GtOperand *);
            TypesSignature *asig = types_as_signature(arg->typ);
            Int k = j + types_type_param_list_len(asig->tparams);
            /* targs[j:k] are the inferred type arguments for asig */
            arg->typ = types_type(burrow__tc_instantiate_signature(
                check, pos, arg->expr, asig, slice_sub(targs, j, k),
                slice_nil(TYPE_UNSAFE_POINTER)));
            burrow__tc_record(check, arg); /* record here because we didn't use the
                                              usual expr evaluators */
            j = k;
        }
    }

    /* check arguments */
    if (args.len > 0) {
        Str es = burrow__tc_es(check, call->fun);
        Str context = GT_SPRINTF(check, "argument to %s", es);
        for (Int i = 0; i < args.len; i++) {
            TypesVar *v = GT_AT(sig_params->vars, i, TypesVar *);
            burrow__tc_assignment(check, GT_AT(args, i, GtOperand *), GT_OBJ(v)->typ,
                                  context);
        }
    }

    return rsig;
}

static const Str tca_cgo_prefixes[] = {
    BURROW_S_INIT("_Ciconst_"),   BURROW_S_INIT("_Cfconst_"),
    BURROW_S_INIT("_Csconst_"),   BURROW_S_INIT("_Ctype_"),
    BURROW_S_INIT("_Cvar_"), /* actually a pointer to the var */
    BURROW_S_INIT("_Cfpvar_fp_"), BURROW_S_INIT("_Cfunc_"),
    BURROW_S_INIT("_Cmacro_"), /* function to evaluate the expanded expression */
};

/* The qualified identifier case of selector: e->x is an identifier denoting
 * the package pname. */
static void tca_qualified(TypesChecker *check, GtOperand *x, AstSelectorExpr *e,
                          AstIdent *ident, TypesPkgName *pname) {
    burrow__types_assert(GT_OBJ(pname)->pkg == check->pkg);
    burrow__tc_record_use(check, ident, types_object(pname));
    tca_set_used(check->used_pkg_names, &pname, true);
    TypesPackage *pkg = pname->imported;
    Str sel = e->sel->name;

    TypesObject exp = NULL;
    GtMode func_mode = GT_MODE_VALUE;
    if (pkg->cgo) {
        /* cgo special cases C.malloc: it's rewritten to _CMalloc and does not
         * support two-result calls. */
        if (str_eq(sel, BURROW_S("malloc")))
            sel = BURROW_S("_CMalloc");
        else
            func_mode = GT_MODE_CGOFUNC;
        for (size_t i = 0; i < sizeof tca_cgo_prefixes / sizeof tca_cgo_prefixes[0];
             i++) {
            /* cgo objects are part of the current package (in file
             * _cgo_gotypes.go). Use regular lookup. */
            Str name = GT_SPRINTF(check, "%s%s", tca_cgo_prefixes[i], sel);
            exp = burrow__tc_env_lookup(&check->env, name);
            if (exp != NULL)
                break;
        }
        if (exp == NULL) {
            if (burrow__types_is_valid_name(sel)) {
                Str es = burrow__tc_es(check, GT_NODE(e));
                GT_ERRORF(check, GT_AT_NODE(e->sel), GT_CODE_UNDECLARED_IMPORTED_NAME,
                          "undefined: %s", es);
            }
            goto error;
        }
        burrow__tc_obj_decl(check, exp);
    } else {
        exp = types_scope_lookup(pkg->scope, sel);
        if (exp == NULL) {
            if (!pkg->fake && burrow__types_is_valid_name(sel)) {
                /* Try to give a better error message when selector matches an
                 * object name ignoring case. */
                Slice exps = burrow__types_scope_lookup_ignoring_case(
                    check->a, pkg->scope, sel, true);
                Str es = burrow__tc_es(check, GT_NODE(e));
                if (exps.len >= 1) {
                    /* report just the first one */
                    Str name = types_object_name(GT_AT(exps, 0, TypesObject));
                    GT_ERRORF(check, GT_AT_NODE(e->sel),
                              GT_CODE_UNDECLARED_IMPORTED_NAME,
                              "undefined: %s (but have %s)", es, name);
                } else {
                    GT_ERRORF(check, GT_AT_NODE(e->sel),
                              GT_CODE_UNDECLARED_IMPORTED_NAME, "undefined: %s", es);
                }
            }
            goto error;
        }
        if (!burrow__types_is_exported(GT_OBJ(exp)->name)) {
            GT_ERRORF(check, GT_AT_NODE(e->sel), GT_CODE_UNEXPORTED_NAME,
                      "name %s not exported by package %s", sel, pkg->name);
            /* ok to continue */
        }
    }
    burrow__tc_record_use(check, e->sel, exp);

    /* Simplified version of the code for identifiers:
     * - imported objects are always fully initialized */
    switch (exp->kind) {
    case TYPES_OBJECT_CONST:
        x->mode = GT_MODE_CONSTANT;
        x->typ = GT_OBJ(exp)->typ;
        x->val = types_as_const(exp)->val;
        break;
    case TYPES_OBJECT_TYPE_NAME:
        x->mode = GT_MODE_TYPEXPR;
        x->typ = GT_OBJ(exp)->typ;
        break;
    case TYPES_OBJECT_VAR:
        x->mode = GT_MODE_VARIABLE;
        x->typ = GT_OBJ(exp)->typ;
        if (pkg->cgo && strings_has_prefix(GT_OBJ(exp)->name, BURROW_S("_Cvar_"))) {
            TypesPointer *p = types_as_pointer(x->typ);
            burrow__types_assert(p != NULL);
            x->typ = p->base;
        }
        break;
    case TYPES_OBJECT_FUNC:
        x->mode = func_mode;
        x->typ = GT_OBJ(exp)->typ;
        if (pkg->cgo && strings_has_prefix(GT_OBJ(exp)->name, BURROW_S("_Cmacro_"))) {
            TypesSignature *sig = types_as_signature(x->typ);
            burrow__types_assert(sig != NULL && sig->results != NULL);
            x->mode = GT_MODE_VALUE;
            x->typ = GT_OBJ(GT_AT(sig->results->vars, 0, TypesVar *))->typ;
        }
        break;
    case TYPES_OBJECT_BUILTIN:
        x->mode = GT_MODE_BUILTIN;
        x->typ = GT_OBJ(exp)->typ;
        x->id = types_as_builtin(exp)->id;
        break;
    default:
        panic_str(BURROW_S("unreachable"));
    }
    x->expr = GT_NODE(e);
    return;

error:
    burrow__tc_op_invalidate(x);
    x->typ = GT_TYP(TYPES_INVALID);
    x->expr = GT_NODE(e);
}

/* The method expression case of selector: x is the type and obj the
 * method. */
static bool tca_method_expr(TypesChecker *check, GtOperand *x, AstSelectorExpr *e,
                            TypesFunc *obj) {
    TypesSignature *sig = types_as_signature(GT_OBJ(obj)->typ);
    burrow__types_assert(sig != NULL);
    if (sig->recv == NULL) {
        burrow__tc_error(check, GT_AT_NODE(e), GT_CODE_INVALID_DECL_CYCLE,
                         BURROW_S("illegal cycle in method declaration"));
        return false;
    }

    /* The receiver type becomes the type of the first function argument of
     * the method expression's function type. */
    Slice params = slice_nil(TYPE_UNSAFE_POINTER);
    if (sig->params != NULL)
        params = sig->params->vars;
    /* Be consistent about named/unnamed parameters. This is not needed for
     * type-checking, but the newly constructed signature may appear in an error
     * message and then have mixed named/unnamed parameters. (An alternative
     * would be to not print parameter names in errors, but it's useful to see
     * them; this is cheap and method expressions are rare.) */
    Str name = BURROW_S("");
    if (params.len > 0 && GT_OBJ(GT_AT(params, 0, TypesVar *))->name.len != 0) {
        /* name needed */
        name = GT_OBJ(sig->recv)->name;
        if (name.len == 0)
            name = BURROW_S("_");
    }
    Slice all = burrow__types_ptrs_make(check->a, params.len + 1);
    GT_AT(all, 0, TypesVar *) = types_new_param(check->a, GT_OBJ(sig->recv)->pos,
                                                GT_OBJ(sig->recv)->pkg, name, x->typ);
    for (Int i = 0; i < params.len; i++)
        GT_AT(all, i + 1, TypesVar *) = GT_AT(params, i, TypesVar *);
    x->mode = GT_MODE_VALUE;
    TypesSignature *msig = GT_NEW(check->a, TypesSignature);
    msig->hdr.kind = TYPES_KIND_SIGNATURE;
    msig->tparams = sig->tparams;
    msig->recvold = burrow__tc_method_expr_sentinel;
    msig->params = types_new_tuple(check->a, all);
    msig->results = sig->results;
    msig->variadic = sig->variadic;
    x->typ = types_type(msig);
    return true;
}

void burrow__tc_selector(TypesChecker *check, GtOperand *x, AstSelectorExpr *e,
                         bool want_type) {
    Str sel = e->sel->name;
    /* If the identifier refers to a package, handle everything here so we
     * don't need a "package" mode for operands: package names can only appear
     * in qualified identifiers which are mapped to selector expressions. */
    if (e->x->kind == AST_KIND_IDENT) {
        AstIdent *ident = (AstIdent *)(void *)e->x;
        TypesPkgName *pname =
            types_as_pkg_name(burrow__tc_env_lookup(&check->env, ident->name));
        if (pname != NULL) {
            tca_qualified(check, x, e, ident, pname);
            return;
        }
    }

    burrow__tc_expr_or_type(check, x, e->x, false);
    switch (x->mode) {
    case GT_MODE_BUILTIN: {
        /* types2 uses the position of '.' for the error */
        Str xs = burrow__tc_xs(check, x);
        GT_ERRORF(check, GT_AT_NODE(e->sel), GT_CODE_UNCALLED_BUILTIN,
                  "invalid use of %s in selector expression", xs);
        goto error;
    }
    case GT_MODE_INVALID:
        goto error;
    case GT_MODE_NOVALUE:
    case GT_MODE_TYPEXPR:
    case GT_MODE_CONSTANT:
    case GT_MODE_VARIABLE:
    case GT_MODE_MAPINDEX:
    case GT_MODE_VALUE:
    case GT_MODE_NILVALUE:
    case GT_MODE_COMMAOK:
    case GT_MODE_COMMAERR:
    case GT_MODE_CGOFUNC:
    default:
        break;
    }

    /* We cannot select on an incomplete type; make sure it's complete. */
    if (!burrow__tc_is_complete(check, x->typ))
        goto error;

    /* Avoid crashing when checking an invalid selector in a method
     * declaration.
     *
     *   type S[T any] struct{}
     *   type V = S[any]
     *   func (fs *S[T]) M(x V.M) {}
     *
     * All codepaths below return a non-type expression. If we get here while
     * expecting a type expression, it is an error.
     *
     * See go.dev/issue/57522 for more details. */
    if (want_type) {
        Str es = burrow__tc_es(check, GT_NODE(e));
        GT_ERRORF(check, GT_AT_NODE(e->sel), GT_CODE_NOT_A_TYPE, "%s is not a type",
                  es);
        goto error;
    }

    /* Additionally, if x->typ is a pointer type, selecting implicitly
     * dereferences the value, meaning its base type must also be complete. */
    TypesPointer *p = types_as_pointer(types_type_underlying(x->typ));
    if (p != NULL && !burrow__tc_is_complete(check, p->base))
        goto error;

    Slice index;
    bool indirect;
    TypesObject obj = burrow__types_lookup_field_or_method(
        check->a, x->typ, x->mode == GT_MODE_VARIABLE, check->pkg, sel, false, &index,
        &indirect);
    if (obj == NULL) {
        /* Don't report another error if the underlying type was invalid
         * (go.dev/issue/49541). */
        if (!burrow__types_is_valid(types_type_underlying(x->typ)))
            goto error;

        if (index.p != NULL) {
            /* TODO(gri) should provide actual type where the conflict happens */
            Str es = burrow__tc_es(check, x->expr);
            GT_ERRORF(check, GT_AT_NODE(e->sel), GT_CODE_AMBIGUOUS_SELECTOR,
                      "ambiguous selector %s.%s", es, sel);
            goto error;
        }

        if (indirect) {
            Str ts = burrow__tc_ts(check, x->typ);
            if (x->mode == GT_MODE_TYPEXPR)
                GT_ERRORF(check, GT_AT_NODE(e->sel), GT_CODE_INVALID_METHOD_EXPR,
                          "invalid method expression %s.%s (needs pointer receiver "
                          "(*%s).%s)",
                          ts, sel, ts, sel);
            else
                GT_ERRORF(check, GT_AT_NODE(e->sel), GT_CODE_INVALID_METHOD_EXPR,
                          "cannot call pointer method %s on %s", sel, ts);
            goto error;
        }

        Str why;
        if (burrow__types_is_interface_ptr(x->typ)) {
            why = burrow__types_interface_ptr_error(
                check->a, burrow__tc_qualifier(check), x->typ);
        } else {
            Slice alt_index;
            bool alt_indirect;
            TypesObject alt = burrow__types_lookup_field_or_method(
                check->a, x->typ, x->mode == GT_MODE_VARIABLE, check->pkg, sel, true,
                &alt_index, &alt_indirect);
            why = burrow__tc_lookup_error(check, x->typ, sel, alt, false);
        }
        Str es = burrow__tc_es(check, x->expr);
        GT_ERRORF(check, GT_AT_NODE(e->sel), GT_CODE_MISSING_FIELD_OR_METHOD,
                  "%s.%s undefined (%s)", es, sel, why);
        goto error;
    }
    /* obj != NULL */

    switch (obj->kind) {
    case TYPES_OBJECT_VAR:
        if (x->mode == GT_MODE_TYPEXPR) {
            Str ts = burrow__tc_ts(check, x->typ);
            GT_ERRORF(check, burrow__tc_at_node(e->x), GT_CODE_MISSING_FIELD_OR_METHOD,
                      "operand for field selector %s must be value of type %s", sel,
                      ts);
            goto error;
        }

        /* field value */
        burrow__tc_record_selection(check, e, TYPES_FIELD_VAL, x->typ, obj, index,
                                    indirect);
        if (x->mode == GT_MODE_VARIABLE || indirect)
            x->mode = GT_MODE_VARIABLE;
        else
            x->mode = GT_MODE_VALUE;
        x->typ = GT_OBJ(obj)->typ;
        break;

    case TYPES_OBJECT_FUNC: {
        burrow__tc_obj_decl(check, obj); /* ensure fully set-up signature */
        burrow__tc_add_decl_dep(check, obj);

        if (x->mode == GT_MODE_TYPEXPR) {
            /* method expression */
            burrow__tc_record_selection(check, e, TYPES_METHOD_EXPR, x->typ, obj, index,
                                        indirect);
            if (!tca_method_expr(check, x, e, types_as_func(obj)))
                goto error;
        } else {
            /* method value */

            /* TODO(gri) If we needed to take into account the receiver's
             * addressability, should we report the type &(x.typ) instead? */
            burrow__tc_record_selection(check, e, TYPES_METHOD_VAL, x->typ, obj, index,
                                        indirect);

            /* Go verifies here that LookupFieldOrMethod and MethodSet.Lookup
             * agree, but only in debug mode, and that check is disabled. */

            x->mode = GT_MODE_VALUE;

            /* remove/stash receiver */
            TypesSignature *sig = GT_NEW(check->a, TypesSignature);
            *sig = *types_as_signature(GT_OBJ(obj)->typ);
            sig->recvold = sig->recv;
            sig->recv = NULL;
            x->typ = types_type(sig);
        }
        break;
    }

    default:
        panic_str(BURROW_S("unreachable"));
    }

    /* everything went well */
    x->expr = GT_NODE(e);
    return;

error:
    burrow__tc_op_invalidate(x);
    x->typ = GT_TYP(TYPES_INVALID);
    x->expr = GT_NODE(e);
}

/* use type-checks each argument. Useful to make sure expressions are
 * evaluated (and variables are "used") in the presence of other errors.
 * Arguments may be NULL. Reports if all arguments evaluated without error. */
bool burrow__tc_use(TypesChecker *check, Slice args) {
    bool ok = true;
    for (Int i = 0; i < args.len; i++) {
        if (!burrow__tc_use1(check, GT_AT(args, i, AstExpr), false))
            ok = false;
    }
    return ok;
}

/* useLHS is like use, but doesn't "use" top-level identifiers. It should be
 * called instead of use if the arguments are expressions on the lhs of an
 * assignment. */
bool burrow__tc_use_lhs(TypesChecker *check, Slice args) {
    bool ok = true;
    for (Int i = 0; i < args.len; i++) {
        if (!burrow__tc_use1(check, GT_AT(args, i, AstExpr), true))
            ok = false;
    }
    return ok;
}

bool burrow__tc_use1(TypesChecker *check, AstExpr e, bool lhs) {
    GtOperand x = {0};
    x.mode = GT_MODE_VALUE; /* anything but invalid */
    AstExpr n = e != NULL ? ast_unparen(e) : NULL;
    if (n == NULL) {
        /* nothing to do */
    } else if (n->kind == AST_KIND_IDENT) {
        AstIdent *id = (AstIdent *)(void *)n;
        /* don't report an error evaluating blank */
        if (str_eq(id->name, BURROW_S("_")))
            return burrow__tc_op_valid(&x);
        /* If the lhs is an identifier denoting a variable v, this assignment is
         * not a 'use' of v. Remember current value of v.used and restore after
         * evaluating the lhs via rawExpr. */
        TypesVar *v = NULL;
        bool v_used = false;
        if (lhs) {
            TypesObject obj = burrow__tc_env_lookup(&check->env, id->name);
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
        burrow__tc_expr_or_type(check, &x, n, true);
        if (v != NULL)
            tca_set_used(check->used_vars, &v, v_used); /* restore v.used */
    } else {
        (void)burrow__tc_raw_expr(check, NULL, &x, e, NULL, true);
    }
    return burrow__tc_op_valid(&x);
}
