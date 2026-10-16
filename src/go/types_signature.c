/* go/types: type-checking of function and method signatures.
 *
 * Derived from Go's src/go/types/signature.go. Go source: go1.27.1.
 *
 * Copyright 2021 The Go Authors. All rights reserved.
 * Copyright 2026 The burrow Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style licence that can be found
 * in the LICENSE file. */

#include "burrow/go/types.h"

#include "burrow/core.h"
#include "burrow/go/ast.h"
#include "burrow/go/token.h"
#include "burrow/path/filepath.h"
#include "burrow/slice.h"
#include "burrow/strings.h"

#include "types_check.h"
#include "types_internal.h"

#include <stdbool.h>

/* A nil constant.Value. */
static const ConstantValue tsg_nil = {0};

static TypesType tsg_unpointer(TypesType t) {
    for (;;) {
        if (t->kind != TYPES_KIND_POINTER)
            return t;
        t = types_as_pointer(t)->base;
    }
}

/* recordParenthesizedRecvTypes records parenthesized intermediate receiver
 * type expressions that all map to the same type, by recursively unpacking
 * expr and recording the corresponding type for it. Example:
 *
 *     expression  -->  type
 *     ----------------------
 *     (*(T[P]))        *T[P]
 *      *(T[P])         *T[P]
 *       (T[P])          T[P]
 *        T[P]           T[P] */
static void tsg_record_parenthesized_recv_types(TypesChecker *check, AstExpr expr,
                                                TypesType typ) {
    for (;;) {
        burrow__tc_record_type_and_value(check, expr, GT_MODE_TYPEXPR, typ, tsg_nil);
        if (expr->kind == AST_KIND_PAREN_EXPR) {
            expr = ((AstParenExpr *)(void *)expr)->x;
        } else if (expr->kind == AST_KIND_STAR_EXPR) {
            expr = ((AstStarExpr *)(void *)expr)->x;
            /* In a correct program, typ must be an unnamed pointer type. But be
             * careful and don't panic. */
            if (typ->kind != TYPES_KIND_POINTER)
                return; /* something is wrong */
            typ = types_as_pointer(typ)->base;
        } else {
            return; /* cannot unpack any further */
        }
    }
}

/* isCGoTypeObj reports whether the given type name was created by cgo. */
static bool tsg_is_cgo_type_obj(TokenFileSet *fset, TypesTypeName *obj) {
    if (strings_has_prefix(GT_OBJ(obj)->name, BURROW_S("_Ctype_")))
        return true;
    TokenFile *f = token_file_set_file(fset, GT_OBJ(obj)->pos);
    Str name = f != NULL ? token_file_name(f) : BURROW_STR_EMPTY;
    return strings_has_prefix(filepath_base(name), BURROW_S("_cgo_"));
}

/* validRecv verifies that the receiver satisfies its respective spec
 * requirements and reports an error otherwise. */
static void tsg_valid_recv(TypesChecker *check, GtPosn pos, TypesVar *recv) {
    /* spec: "The receiver type must be of the form T or *T where T is a type
     * name." */
    bool is_ptr = false;
    TypesType rtyp = burrow__types_deref(GT_OBJ(recv)->typ, &is_ptr);
    TypesType atyp = types_unalias(rtyp);
    if (!burrow__types_is_valid(atyp))
        return; /* error was reported before */

    /* spec: "The type denoted by T is called the receiver base type; it must
     * not be a pointer or interface type and it must be declared in the same
     * package as the method." */
    switch (atyp->kind) {
    case TYPES_KIND_NAMED: {
        TypesNamed *T = types_as_named(atyp);
        if (GT_OBJ(T->obj)->pkg != check->pkg ||
            tsg_is_cgo_type_obj(check->fset, T->obj)) {
            GT_ERRORF(check, pos, GT_CODE_INVALID_RECV,
                      "cannot define new methods on non-local type %s",
                      burrow__tc_ts(check, rtyp));
            break;
        }
        Str cause = BURROW_STR_EMPTY;
        TypesType u = types_type_underlying(atyp);
        switch (u->kind) {
        case TYPES_KIND_BASIC:
            /* unsafe.Pointer is treated like a regular pointer */
            if (types_as_basic(u)->kind == TYPES_UNSAFE_POINTER)
                cause = BURROW_S("unsafe.Pointer");
            break;
        case TYPES_KIND_POINTER:
        case TYPES_KIND_INTERFACE:
            cause = BURROW_S("pointer or interface type");
            break;
        case TYPES_KIND_TYPE_PARAM:
            /* The underlying type of a receiver base type cannot be a type
             * parameter: "type T[P any] P" is not a valid declaration. */
            panic_str(BURROW_S("unreachable"));
        default:
            break;
        }
        if (cause.len > 0)
            GT_ERRORF(check, pos, GT_CODE_INVALID_RECV, "invalid receiver type %s (%s)",
                      burrow__tc_ts(check, rtyp), cause);
        break;
    }
    case TYPES_KIND_BASIC:
        GT_ERRORF(check, pos, GT_CODE_INVALID_RECV,
                  "cannot define new methods on non-local type %s",
                  burrow__tc_ts(check, rtyp));
        break;
    default:
        GT_ERRORF(check, pos, GT_CODE_INVALID_RECV, "invalid receiver type %s",
                  burrow__tc_ts(check, GT_OBJ(recv)->typ));
        break;
    }
}

/* What the later function of collectRecv closes over. */
typedef struct TsgRecv {
    AstExpr rbase;
    TypesVar *recv;
} TsgRecv;

static void tsg_valid_recv_later(TypesChecker *check, void *env) {
    TsgRecv *c = env;
    tsg_valid_recv(check, burrow__tc_at_node(c->rbase), c->recv);
}

/* The receiver type when the receiver has no type parameters. */
static TypesType tsg_plain_recv_type(TypesChecker *check, AstField *rparam,
                                     AstExpr rbase) {
    /* If there are no type parameters, we can simply typecheck rparam.Type.
     * If that is a generic type, varType will complain. Further receiver
     * constraints will be checked later, with validRecv. We use rparam.Type
     * (rather than base) to correctly record pointer and parentheses in
     * types.Info (was bug, see go.dev/issue/68639). */
    TypesType recv_type = burrow__tc_var_type(check, rparam->type);
    /* Defining new methods on instantiated (alias or defined) types is not
     * permitted. Follow literal pointer/alias type chain and check. (Correct
     * code permits at most one pointer indirection, but for this check it
     * doesn't matter if we have multiple pointers.) */
    TypesType t = tsg_unpointer(recv_type); /* recvType is not generic per above */
    TypesAlias *a = t->kind == TYPES_KIND_ALIAS ? types_as_alias(t) : NULL;
    while (a != NULL) {
        TypesType base_type = tsg_unpointer(a->from_rhs);
        TypesTypeParamList *tparams = NULL;
        if (base_type->kind == TYPES_KIND_NAMED)
            tparams = types_named_type_params(types_as_named(base_type));
        else if (base_type->kind == TYPES_KIND_ALIAS)
            tparams = types_alias_type_params(types_as_alias(base_type));
        if (tparams != NULL) {
            GT_ERRORF(check, burrow__tc_at_node(rbase), GT_CODE_INVALID_RECV,
                      "cannot define new methods on instantiated type %s",
                      burrow__tc_ts(check, base_type));
            return GT_TYP(
                TYPES_INVALID); /* avoid follow-on errors by Checker.validRecv */
        }
        a = base_type->kind == TYPES_KIND_ALIAS ? types_as_alias(base_type) : NULL;
    }
    return recv_type;
}

/* The receiver base type when the receiver has type parameters: a generic
 * named type, or NULL if it is not valid. */
static TypesNamed *tsg_generic_recv_base(TypesChecker *check, AstExpr rbase) {
    /* Important: rbase must be resolved before declaring any receiver type
     * parameters (which may have the same name, see below). */
    Str cause = BURROW_STR_EMPTY;
    TypesType t = burrow__tc_generic_type(check, rbase, &cause);
    if (!burrow__types_is_valid(t)) {
        if (cause.len > 0)
            GT_ERRORF(check, burrow__tc_at_node(rbase), GT_CODE_INVALID_RECV, "%s",
                      cause);
        /* Ok to continue but do not set baseType (see comment above). */
        return NULL;
    }
    switch (t->kind) {
    case TYPES_KIND_NAMED:
        return types_as_named(t);
    case TYPES_KIND_ALIAS:
        /* Methods on generic aliases are not permitted. Only report an error if
         * the alias type is valid. */
        if (burrow__types_is_valid(t))
            GT_ERRORF(check, burrow__tc_at_node(rbase), GT_CODE_INVALID_RECV,
                      "cannot define new methods on generic alias type %s",
                      burrow__tc_ts(check, t));
        /* Ok to continue but do not set basetype in this case so that recvType
         * remains invalid (was bug, see go.dev/issue/70417). */
        return NULL;
    default:
        panic_str(BURROW_S("unreachable"));
    }
}

/* The receiver type for receiver type parameters recv_tparams on base_type,
 * after setting their bounds. */
static TypesType tsg_instantiate_recv(TypesChecker *check, AstField *rparam,
                                      AstExpr rbase, bool rptr, TypesNamed *base_type,
                                      Slice recv_tparams) {
    /* Get the type parameter bounds from the receiver base type and set them
     * for the respective (local) receiver type parameters. */
    Slice base_tparams = burrow__types_tparam_list(types_named_type_params(base_type));
    if (recv_tparams.len == base_tparams.len) {
        Map *smap = burrow__types_make_subst_map(check->a, base_tparams, recv_tparams);
        for (Int i = 0; i < recv_tparams.len; i++) {
            TypesTypeParam *recv_tpar = GT_AT(recv_tparams, i, TypesTypeParam *);
            TypesTypeParam *base_tpar = GT_AT(base_tparams, i, TypesTypeParam *);
            burrow__tc_mono_record_canon(check, recv_tpar, base_tpar);
            /* baseTPar.bound is possibly parameterized by other type parameters
             * defined by the generic base type. Substitute those parameters
             * with the receiver type parameters declared by the current
             * method. */
            recv_tpar->bound =
                burrow__tc_subst(check, GT_OBJ(recv_tpar->obj)->pos, base_tpar->bound,
                                 smap, NULL, burrow__tc_context(check));
        }
    } else {
        Str got =
            burrow__tc_measure(check, recv_tparams.len, BURROW_S("type parameter"));
        Int n = base_tparams.len;
        GT_ERRORF(check, burrow__tc_at_node(rbase), GT_CODE_BAD_RECV,
                  "receiver declares %s, but receiver base type declares %d", got, n);
    }

    /* The type parameters declared by the receiver also serve as type
     * arguments for the receiver type. Instantiate the receiver. */
    (void)burrow__tc_verify_version(check, burrow__tc_at_node(rbase), burrow__tc_go1_18,
                                    BURROW_S("type instantiation"));
    Slice targs = burrow__types_ptrs_make(check->a, recv_tparams.len);
    for (Int i = 0; i < recv_tparams.len; i++)
        GT_AT(targs, i, TypesType) =
            types_type(GT_AT(recv_tparams, i, TypesTypeParam *));
    TypesType recv_type =
        burrow__tc_instance(check, ast_node_pos(rparam->type), types_type(base_type),
                            targs, NULL, burrow__tc_context(check));
    burrow__tc_record_instance(check, rbase, targs, recv_type);

    /* Reestablish pointerness if needed (but avoid a pointer to an invalid
     * type). */
    if (rptr && burrow__types_is_valid(recv_type))
        recv_type = types_type(types_new_pointer(check->a, recv_type));

    tsg_record_parenthesized_recv_types(check, rparam->type, recv_type);
    return recv_type;
}

/* collectRecv extracts the method receiver and its type parameters (if any)
 * from rparam. It declares the type parameters (but not the receiver) in the
 * current scope, and returns the receiver variable and its type parameter list
 * (if any). */
static TypesVar *tsg_collect_recv(TypesChecker *check, AstField *rparam,
                                  TokenPos scope_pos, TypesTypeParamList **rparams) {
    /* Unpack the receiver parameter which is of the form
     *
     *     "(" [rfield] ["*"] rbase ["[" rtparams "]"] ")"
     *
     * The receiver name rname, the pointer indirection, and the receiver type
     * parameters rtparams may not be present. */
    AstExpr rbase = NULL;
    Slice rtparams;
    bool rptr = burrow__tc_unpack_recv(check, rparam->type, true, &rbase, &rtparams);

    /* Determine the receiver base type. */
    TypesType recv_type = GT_TYP(TYPES_INVALID);
    *rparams = NULL;
    if (rtparams.len == 0) {
        recv_type = tsg_plain_recv_type(check, rparam, rbase);
    } else {
        /* If there are type parameters, rbase must denote a generic base type. */
        TypesNamed *base_type = tsg_generic_recv_base(check, rbase);

        /* Collect the type parameters declared by the receiver (see also
         * Checker.collectTypeParams). The scope of the type parameter T in
         * "func (r T[T]) f() {}" starts after f, not at r, so we declare it
         * after typechecking rbase (see go.dev/issue/52038). */
        Slice recv_tparams = burrow__types_ptrs_make(check->a, rtparams.len);
        for (Int i = 0; i < rtparams.len; i++) {
            AstIdent *rp = GT_AT(rtparams, i, AstIdent *);
            TypesTypeParam *tpar = burrow__tc_declare_type_param(check, rp, scope_pos);
            GT_AT(recv_tparams, i, TypesTypeParam *) = tpar;
            /* For historic reasons, type parameters in receiver type
             * expressions are considered both definitions and uses and thus
             * must be recorded in the Info.Uses and Info.Types maps (see
             * go.dev/issue/68670). */
            burrow__tc_record_use(check, rp, types_object(tpar->obj));
            burrow__tc_record_type_and_value(check, GT_NODE(rp), GT_MODE_TYPEXPR,
                                             types_type(tpar), tsg_nil);
        }
        *rparams = burrow__types_bind_tparams(check->a, recv_tparams);

        if (base_type != NULL)
            recv_type = tsg_instantiate_recv(check, rparam, rbase, rptr, base_type,
                                             recv_tparams);
    }

    /* Make sure we have no more than one receiver name. */
    AstIdent *rname = NULL;
    Int n = rparam->names.len;
    if (n >= 1) {
        if (n > 1)
            burrow__tc_error(check, GT_AT_NODE(GT_AT(rparam->names, n - 1, AstIdent *)),
                             GT_CODE_INVALID_RECV,
                             BURROW_S("method has multiple receivers"));
        rname = GT_AT(rparam->names, 0, AstIdent *);
    }

    /* Create the receiver parameter. recvType is invalid if baseType was never
     * set. */
    TypesVar *recv;
    if (rname != NULL && rname->name.len > 0) {
        /* named receiver */
        recv = burrow__types_new_var(check->a, TYPES_RECV_VAR,
                                     ast_node_pos(GT_NODE(rname)), check->pkg,
                                     rname->name, recv_type);
        /* In this case, the receiver is declared by the caller because it must
         * be declared after any type parameters (otherwise it might shadow one
         * of them). */
    } else {
        /* anonymous receiver */
        recv = burrow__types_new_var(check->a, TYPES_RECV_VAR,
                                     ast_node_pos(GT_NODE(rparam)), check->pkg,
                                     BURROW_STR_EMPTY, recv_type);
        burrow__tc_record_implicit(check, GT_NODE(rparam), types_object(recv));
    }

    /* Delay validation of receiver type as it may cause premature expansion of
     * types the receiver type is dependent on (see go.dev/issue/51232,
     * go.dev/issue/51233). */
    TsgRecv *c = GT_NEW(check->a, TsgRecv);
    c->rbase = rbase;
    c->recv = recv;
    (void)burrow__tc_later(check, tsg_valid_recv_later, c);

    return recv;
}

/* collectParams collects (but does not declare) all parameter/result variables
 * of list into names and params, and returns whether the (parameter) list is
 * variadic. Anonymous parameters are recorded with NULL names. */
static bool tsg_collect_params(TypesChecker *check, TypesVarKind kind,
                               AstFieldList *list, Slice *names, Slice *params) {
    *names = slice_nil(TYPE_UNSAFE_POINTER);
    *params = slice_nil(TYPE_UNSAFE_POINTER);
    if (list == NULL)
        return false;

    bool variadic = false;
    bool named = false;
    bool anonymous = false;
    for (Int i = 0; i < list->list.len; i++) {
        AstField *field = GT_AT(list->list, i, AstField *);
        AstExpr ftype = field->type;
        if (ftype->kind == AST_KIND_ELLIPSIS) {
            AstEllipsis *t = (AstEllipsis *)(void *)ftype;
            ftype = t->elt;
            if (kind == TYPES_PARAM_VAR && i == list->list.len - 1 &&
                field->names.len <= 1)
                variadic = true;
            else
                burrow__tc_soft_error(check, GT_AT_NODE(t), GT_CODE_INVALID_SYNTAX_TREE,
                                      BURROW_S("invalid use of ..."));
            /* ignore ... and continue */
        }
        TypesType typ = burrow__tc_var_type(check, ftype);
        /* The parser ensures that f.Tag is nil and we don't care if a
         * constructed AST contains a non-nil tag. */
        if (field->names.len > 0) {
            /* named parameter */
            for (Int j = 0; j < field->names.len; j++) {
                AstIdent *name = GT_AT(field->names, j, AstIdent *);
                if (name->name.len == 0)
                    burrow__tc_error(check, GT_AT_NODE(name),
                                     GT_CODE_INVALID_SYNTAX_TREE,
                                     BURROW_S("anonymous parameter"));
                /* ok to continue */
                TypesVar *par =
                    burrow__types_new_var(check->a, kind, ast_node_pos(GT_NODE(name)),
                                          check->pkg, name->name, typ);
                /* named parameter is declared by caller */
                burrow__types_ptrs_append(check->a, names, name);
                burrow__types_ptrs_append(check->a, params, par);
            }
            named = true;
        } else {
            /* anonymous parameter */
            TypesVar *par = burrow__types_new_var(check->a, kind, ast_node_pos(ftype),
                                                  check->pkg, BURROW_STR_EMPTY, typ);
            burrow__tc_record_implicit(check, GT_NODE(field), types_object(par));
            burrow__types_ptrs_append(check->a, names, NULL);
            burrow__types_ptrs_append(check->a, params, par);
            anonymous = true;
        }
    }

    if (named && anonymous)
        burrow__tc_error(check, GT_AT_NODE(list), GT_CODE_INVALID_SYNTAX_TREE,
                         BURROW_S("list contains both named and anonymous parameters"));
    /* ok to continue */

    /* For a variadic function, change the last parameter's type from T to []T.
     * Since we type-checked T rather than ...T, we also need to retro-actively
     * record the type for ...T. */
    if (variadic) {
        burrow__types_assert(params->len > 0 && params->p != NULL);
        TypesVar *last = GT_AT(*params, params->len - 1, TypesVar *);
        GT_OBJ(last)->typ = types_type(types_new_slice(check->a, GT_OBJ(last)->typ));
        AstField *lf = GT_AT(list->list, list->list.len - 1, AstField *);
        burrow__tc_record_type_and_value(check, lf->type, GT_MODE_TYPEXPR,
                                         GT_OBJ(last)->typ, tsg_nil);
    }
    return variadic;
}

/* declareParams declares each named parameter in the current scope. */
static void tsg_declare_params(TypesChecker *check, Slice names, Slice params,
                               TokenPos scope_pos) {
    for (Int i = 0; i < names.len; i++) {
        AstIdent *name = GT_AT(names, i, AstIdent *);
        if (name != NULL && name->name.len > 0)
            burrow__tc_declare(check, check->env.scope, name,
                               types_object(GT_AT(params, i, TypesVar *)), scope_pos);
    }
}

/* funcType type-checks a function or method type. */
void burrow__tc_func_type(TypesChecker *check, TypesSignature *sig,
                          AstFieldList *recv_par, AstFuncType *ftyp) {
    burrow__tc_open_scope(check, GT_NODE(ftyp), BURROW_S("function"));
    check->env.scope->is_func = true;
    burrow__tc_record_scope(check, GT_NODE(ftyp), check->env.scope);
    sig->scope = check->env.scope;

    /* collect method receiver, if any */
    TypesVar *recv = NULL;
    TypesTypeParamList *rparams = NULL;
    if (recv_par != NULL && ast_field_list_num_fields(recv_par) > 0) {
        /* We have at least one receiver; make sure we don't have more than
         * one. */
        Int n = recv_par->list.len;
        if (n > 1)
            burrow__tc_error(
                check, GT_AT_NODE(GT_AT(recv_par->list, n - 1, AstField *)),
                GT_CODE_INVALID_RECV, BURROW_S("method has multiple receivers"));
        /* continue with first one */
        /* all type parameters' scopes start after the method name */
        TokenPos scope_pos = ast_node_pos(GT_NODE(ftyp));
        recv = tsg_collect_recv(check, GT_AT(recv_par->list, 0, AstField *), scope_pos,
                                &rparams);
    }

    /* collect and declare function type parameters */
    if (ftyp->type_params != NULL)
        burrow__tc_collect_type_params(check, &sig->tparams, ftyp->type_params);

    /* collect ordinary and result parameters */
    Slice pnames;
    Slice params;
    Slice rnames;
    Slice results;
    bool variadic =
        tsg_collect_params(check, TYPES_PARAM_VAR, ftyp->params, &pnames, &params);
    (void)tsg_collect_params(check, TYPES_RESULT_VAR, ftyp->results, &rnames, &results);

    /* declare named receiver, ordinary, and result parameters */
    /* all parameter's scopes start after the signature */
    TokenPos scope_pos = ast_node_end(GT_NODE(ftyp));
    if (recv != NULL && GT_OBJ(recv)->name.len > 0) {
        AstField *rf = GT_AT(recv_par->list, 0, AstField *);
        burrow__tc_declare(check, check->env.scope, GT_AT(rf->names, 0, AstIdent *),
                           types_object(recv), scope_pos);
    }
    tsg_declare_params(check, pnames, params, scope_pos);
    tsg_declare_params(check, rnames, results, scope_pos);

    sig->recv = recv;
    sig->rparams = rparams;
    sig->params = types_new_tuple(check->a, params);
    sig->results = types_new_tuple(check->a, results);
    sig->variadic = variadic;

    burrow__tc_close_scope(check);
}
