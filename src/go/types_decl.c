/* go/types: declarations of constants, variables, types and functions.
 *
 * Derived from Go's src/go/types/decl.go. Go source: go1.27.1.
 *
 * Copyright 2014 The Go Authors. All rights reserved.
 * Copyright 2026 The burrow Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style licence that can be found
 * in the LICENSE file. */

#include "burrow/go/types.h"

#include "burrow/bytes.h"
#include "burrow/core.h"
#include "burrow/go/ast.h"
#include "burrow/go/constant.h"
#include "burrow/go/token.h"
#include "burrow/map.h"
#include "burrow/panic.h"
#include "burrow/slice.h"
#include "burrow/type.h"

#include "types_check.h"
#include "types_internal.h"

#include <stdbool.h>
#include <stdint.h>

void burrow__tc_declare(TypesChecker *check, TypesScope *scope, AstIdent *id,
                        TypesObject obj, TokenPos pos) {
    /* spec: "The blank identifier, represented by the underscore character _,
     * may be used in a declaration like any other identifier but the
     * declaration does not introduce a new binding." */
    if (!str_eq(GT_OBJ(obj)->name, BURROW_S("_"))) {
        TypesObject alt = types_scope_insert(scope, obj);
        if (alt != NULL) {
            GtError *err = burrow__tc_new_error(check, GT_CODE_DUPLICATE_DECL);
            GT_ADDF(err, burrow__tc_at_obj(obj), "%s redeclared in this block",
                    GT_OBJ(obj)->name);
            burrow__tc_error_add_alt_decl(err, alt);
            burrow__tc_error_report(err);
            return;
        }
        GT_OBJ(obj)->scope_pos = pos;
    }
    if (id != NULL)
        burrow__tc_record_def(check, id, obj);
}

static bool tcd_valid_cycle(TypesChecker *check, TypesObject obj);
static void tcd_const_decl(TypesChecker *check, TypesConst *obj, AstExpr typ,
                           AstExpr init, bool inherited);
static void tcd_var_decl(TypesChecker *check, TypesVar *obj, Slice lhs, AstExpr typ,
                         AstExpr init);
static void tcd_type_decl(TypesChecker *check, TypesTypeName *obj, AstTypeSpec *tdecl);
static void tcd_func_decl(TypesChecker *check, TypesFunc *obj, GtDeclInfo *decl);

/* objDecl type-checks the declaration of obj in its respective (file)
 * environment. */
void burrow__tc_obj_decl(TypesChecker *check, TypesObject obj) {
    /* Checking the declaration of an object means determining its type (and
     * also its value for constants). An object (and thus its type) may be in
     * 1 of 3 states:
     *
     * - not in Checker.objPathIdx and type == nil : type is not yet known (white)
     * -     in Checker.objPathIdx                 : type is pending       (grey)
     * - not in Checker.objPathIdx and type != nil : type is known         (black)
     *
     * During type-checking, an object changes from white to grey to black.
     * Predeclared objects start as black (their type is known without
     * checking).
     *
     * A black object may only depend on (refer to) to other black objects.
     * White and grey objects may depend on white or black objects. A
     * dependency on a grey object indicates a (possibly invalid) cycle.
     *
     * When an object is marked grey, it is pushed onto the object path (a
     * stack) and its index in the path is recorded in the path index map. It
     * is popped and removed from the map when its type is determined (and
     * marked black). */

    /* If this object is grey, we have a (possibly invalid) cycle. This is
     * signaled by a non-nil type for the object, except for constants and
     * variables whose type may be non-nil (known), or nil if it depends on a
     * not-yet known initialization value.
     *
     * In the former case, set the type to Typ[Invalid] because we have an
     * initialization cycle. The cycle error will be reported later, when
     * determining initialization order.
     *
     * TODO(gri) Report cycle here and simplify initialization order code. */
    if (check->obj_path_idx != NULL && map_get(check->obj_path_idx, &obj) != NULL) {
        switch (obj->kind) {
        case TYPES_OBJECT_CONST:
        case TYPES_OBJECT_VAR:
            if (!tcd_valid_cycle(check, obj) || GT_OBJ(obj)->typ == NULL)
                GT_OBJ(obj)->typ = GT_TYP(TYPES_INVALID);
            break;
        case TYPES_OBJECT_TYPE_NAME:
            if (!tcd_valid_cycle(check, obj))
                GT_OBJ(obj)->typ = GT_TYP(TYPES_INVALID);
            break;
        case TYPES_OBJECT_FUNC:
            /* Don't set type to Typ[Invalid]; plenty of code asserts that
             * functions have a *Signature type. Instead, leave the type as an
             * empty signature, which makes it impossible to initialize a
             * variable with the function. */
            (void)tcd_valid_cycle(check, obj);
            break;
        default:
            panic_str(BURROW_S("unreachable"));
        }

        burrow__types_assert(GT_OBJ(obj)->typ != NULL);
        return;
    }

    if (GT_OBJ(obj)->typ != NULL) /* black, meaning it's already type-checked */
        return;

    /* white, meaning it must be type-checked */

    burrow__tc_push(check, obj); /* mark as grey */

    GtDeclInfo *d = burrow__tc_obj_info(check, obj);
    burrow__types_assert(d != NULL);

    /* save/restore current environment and set up object environment */
    GtEnv saved = check->env;
    check->env = (GtEnv){0};
    check->env.scope = d->file;
    check->env.version = d->version;

    /* Const and var declarations must not have initialization cycles. We
     * track them by remembering the current declaration in check.decl.
     * Initialization expressions depending on other consts, vars, or
     * functions, add dependencies to the current check.decl. */
    switch (obj->kind) {
    case TYPES_OBJECT_CONST:
        check->env.decl = d; /* new package-level const decl */
        tcd_const_decl(check, types_as_const(obj), d->vtyp, d->init, d->inherited);
        break;
    case TYPES_OBJECT_VAR:
        check->env.decl = d; /* new package-level var decl */
        tcd_var_decl(check, types_as_var(obj), d->lhs, d->vtyp, d->init);
        break;
    case TYPES_OBJECT_TYPE_NAME:
        /* invalid recursive types are detected via path */
        tcd_type_decl(check, types_as_type_name(obj), d->tdecl);
        /* methods can only be added to top-level types */
        burrow__tc_collect_methods(check, types_as_type_name(obj));
        break;
    case TYPES_OBJECT_FUNC:
        /* functions may be recursive - no need to track dependencies */
        tcd_func_decl(check, types_as_func(obj), d);
        break;
    default:
        panic_str(BURROW_S("unreachable"));
    }

    /* Go defers these two. A bailout never comes back here, and nothing
     * looks at the environment or the path after one. */
    check->env = saved;
    burrow__tc_pop(check);
}

/* validCycle checks if the cycle starting with obj is valid and reports an
 * error if it is not. */
static bool tcd_valid_cycle(TypesChecker *check, TypesObject obj) {
    /* Count cycle objects. */
    Int *idx = map_get(check->obj_path_idx, &obj);
    burrow__types_assert(idx != NULL);
    Int start = *idx;
    Slice cycle = slice_sub(check->obj_path, start, check->obj_path.len);
    burrow__types_assert(cycle.len > 0 && cycle.p != NULL); /* obj is in it */
    bool tpar_cycle = false; /* if set, the cycle is through a type parameter list */
    Int nval = 0;            /* number of (constant or variable) values in the cycle */
    Int ndef = 0;            /* number of type definitions in the cycle */
    for (Int i = 0; i < cycle.len && !tpar_cycle; i++) {
        TypesObject o = GT_AT(cycle, i, TypesObject);
        switch (o->kind) {
        case TYPES_OBJECT_CONST:
        case TYPES_OBJECT_VAR:
            nval++;
            break;
        case TYPES_OBJECT_TYPE_NAME:
            /* If we reach a generic type that is part of a cycle and we are
             * in a type parameter list, we have a cycle through a type
             * parameter list. */
            if (check->env.in_tparam_list && burrow__types_is_generic(GT_OBJ(o)->typ)) {
                tpar_cycle = true;
                break;
            }

            if (!types_type_name_is_alias(types_as_type_name(o)))
                ndef++;
            break;
        case TYPES_OBJECT_FUNC:
            /* ignored for now */
            break;
        default:
            panic_str(BURROW_S("unreachable"));
        }
    }

    /* Cycles through type parameter lists are ok (go.dev/issue/68162). */
    if (tpar_cycle)
        return true;

    /* A cycle involving only constants and variables is invalid but we ignore
     * them here because they are reported via the initialization cycle
     * check. */
    if (nval == cycle.len)
        return true;

    /* A cycle involving only types (and possibly functions) must have at
     * least one type definition to be permitted: If there is no type
     * definition, we have a sequence of alias type names which will expand ad
     * infinitum. */
    if (nval == 0 && ndef > 0)
        return true;

    burrow__tc_cycle_error(check, cycle, burrow__tc_first_in_src(cycle));
    return false;
}

/* name in cycleError: the (possibly qualified) object name. This is needed
 * because with generic types, cycles may refer to imported types. See
 * go.dev/issue/50788. */
static Str tcd_cycle_name(TypesChecker *check, TypesObject obj) {
    /* include any type arguments in the reported error message */
    TypesNamed *n = burrow__types_as_named(GT_OBJ(obj)->typ);
    if (n != NULL && n->inst != NULL)
        return types_type_string(check->a, types_type(n), burrow__tc_qualifier(check));
    BytesBuffer buf = BYTES_BUFFER(check->a);
    burrow__types_write_package_prefix(&buf, GT_OBJ(obj)->pkg,
                                       burrow__tc_qualifier(check));
    burrow__types_buf_str(&buf, GT_OBJ(obj)->name);
    return burrow__types_buf_done(&buf, check->a);
}

/* cycleError reports a declaration cycle starting with the object at
 * cycle[start]. */
void burrow__tc_cycle_error(TypesChecker *check, Slice cycle, Int start) {
    /* If obj is a type alias, mark it as valid (not broken) in order to avoid
     * follow-on errors. */
    TypesObject obj = GT_AT(cycle, start, TypesObject);
    TypesTypeName *tname = types_as_type_name(obj);
    if (tname != NULL) {
        TypesType t = GT_OBJ(tname)->typ;
        if (t != NULL && t->kind == TYPES_KIND_ALIAS)
            types_as_alias(t)->from_rhs = GT_TYP(TYPES_INVALID);
    }

    /* report a more concise error for self references */
    if (cycle.len == 1) {
        if (tname != NULL)
            GT_ERRORF(check, burrow__tc_at_obj(obj), GT_CODE_INVALID_DECL_CYCLE,
                      "invalid recursive type: %s refers to itself",
                      tcd_cycle_name(check, obj));
        else
            GT_ERRORF(check, burrow__tc_at_obj(obj), GT_CODE_INVALID_DECL_CYCLE,
                      "invalid cycle in declaration: %s refers to itself",
                      tcd_cycle_name(check, obj));
        return;
    }

    GtError *err = burrow__tc_new_error(check, GT_CODE_INVALID_DECL_CYCLE);
    if (tname != NULL)
        GT_ADDF(err, burrow__tc_at_obj(obj), "invalid recursive type %s",
                tcd_cycle_name(check, obj));
    else
        GT_ADDF(err, burrow__tc_at_obj(obj), "invalid cycle in declaration of %s",
                tcd_cycle_name(check, obj));
    /* "cycle[i] refers to cycle[j]" for (i,j) = (s,s+1), (s+1,s+2), ...,
     * (n-1,0), (0,1), ..., (s-1,s) for len(cycle) = n, s = start. */
    for (Int i = 0; i < cycle.len; i++) {
        TypesObject next = GT_AT(cycle, (start + i + 1) % cycle.len, TypesObject);
        GT_ADDF(err, burrow__tc_at_obj(obj), "%s refers to %s",
                tcd_cycle_name(check, obj), tcd_cycle_name(check, next));
        obj = next;
    }
    burrow__tc_error_report(err);
}

/* firstInSrc reports the index of the object with the "smallest" source
 * position in path. path must not be empty. */
Int burrow__tc_first_in_src(Slice path) {
    Int fst = 0;
    TokenPos pos = GT_OBJ(GT_AT(path, 0, TypesObject))->pos;
    for (Int i = 1; i < path.len; i++) {
        TokenPos p = GT_OBJ(GT_AT(path, i, TypesObject))->pos;
        if (p < pos) {
            fst = i;
            pos = p;
        }
    }
    return fst;
}

/* Go's %T of a node, such as *ast.BadDecl. */
static Str tcd_node_type(TypesChecker *check, AstNode n) {
    const Type *t = ast_node_type(n);
    if (t == NULL)
        return BURROW_S("<nil>");
    return GT_SPRINTF(check, "*ast.%s", t->name);
}

void burrow__tc_walk_decls(TypesChecker *check, Slice decls, GtDeclFn f, void *env) {
    for (Int i = 0; i < decls.len; i++)
        burrow__tc_walk_decl(check, GT_AT(decls, i, AstDecl), f, env);
}

static void tcd_walk_value_spec(TypesChecker *check, AstGenDecl *d, AstValueSpec *s,
                                Int iota, AstValueSpec **last, GtDeclFn f, void *env) {
    GtDecl decl = {0};
    switch (d->tok) {
    case TOKEN_CONST: {
        /* determine which initialization expressions to use */
        bool inherited = true;
        if (s->type != NULL || s->values.len > 0) {
            *last = s;
            inherited = false;
        } else if (*last == NULL) {
            /* make sure last exists */
            *last = (AstValueSpec *)(void *)ast_node_new(check->a, AST_KIND_VALUE_SPEC);
            if (*last == NULL)
                burrow__types_oom();
            inherited = false;
        }
        burrow__tc_arity_match(check, s, *last);
        decl.kind = GT_DECL_CONST;
        decl.value_spec = s;
        decl.iota = iota;
        decl.typ = (*last)->type;
        decl.init = (*last)->values;
        decl.inherited = inherited;
        f(check, env, &decl);
        break;
    }
    case TOKEN_VAR:
        burrow__tc_arity_match(check, s, NULL);
        decl.kind = GT_DECL_VAR;
        decl.value_spec = s;
        f(check, env, &decl);
        break;
    default:
        GT_ERRORF(check, GT_AT_NODE(s), GT_CODE_INVALID_SYNTAX_TREE, "invalid token %s",
                  token_string(d->tok, check->a));
        break;
    }
}

void burrow__tc_walk_decl(TypesChecker *check, AstDecl d, GtDeclFn f, void *env) {
    switch (d->kind) {
    case AST_KIND_BAD_DECL:
        /* ignore */
        break;
    case AST_KIND_GEN_DECL: {
        AstGenDecl *g = (AstGenDecl *)(void *)d;
        AstValueSpec *last = NULL; /* last ValueSpec with type or init exprs seen */
        for (Int iota = 0; iota < g->specs.len; iota++) {
            AstSpec s = GT_AT(g->specs, iota, AstSpec);
            GtDecl decl = {0};
            switch (s->kind) {
            case AST_KIND_IMPORT_SPEC:
                decl.kind = GT_DECL_IMPORT;
                decl.import_spec = (AstImportSpec *)(void *)s;
                f(check, env, &decl);
                break;
            case AST_KIND_VALUE_SPEC:
                tcd_walk_value_spec(check, g, (AstValueSpec *)(void *)s, iota, &last, f,
                                    env);
                break;
            case AST_KIND_TYPE_SPEC:
                decl.kind = GT_DECL_TYPE;
                decl.type_spec = (AstTypeSpec *)(void *)s;
                f(check, env, &decl);
                break;
            default:
                GT_ERRORF(check, burrow__tc_at_node(s), GT_CODE_INVALID_SYNTAX_TREE,
                          "unknown ast.Spec node %s", tcd_node_type(check, s));
                break;
            }
        }
        break;
    }
    case AST_KIND_FUNC_DECL: {
        GtDecl decl = {0};
        decl.kind = GT_DECL_FUNC;
        decl.func_decl = (AstFuncDecl *)(void *)d;
        f(check, env, &decl);
        break;
    }
    default:
        GT_ERRORF(check, burrow__tc_at_node(d), GT_CODE_INVALID_SYNTAX_TREE,
                  "unknown ast.Decl node %s", tcd_node_type(check, d));
        break;
    }
}

static void tcd_const_decl(TypesChecker *check, TypesConst *obj, AstExpr typ,
                           AstExpr init, bool inherited) {
    burrow__types_assert(GT_OBJ(obj)->typ == NULL);

    /* use the correct value of iota */
    ConstantValue *saved_iota = check->env.iota;
    bool saved_has_errpos = check->env.has_errpos;
    GtPosn saved_errpos = check->env.errpos;
    ConstantValue *iota = GT_NEW(check->a, ConstantValue);
    *iota = obj->val;
    check->env.iota = iota;
    check->env.has_errpos = false;

    /* provide valid constant value under all circumstances */
    obj->val = constant_make_unknown();

    /* determine type, if any */
    bool ok = true;
    if (typ != NULL) {
        TypesType t = burrow__tc_typ(check, typ);
        if (!burrow__types_is_basic(t, TYPES_IS_CONST_TYPE)) {
            /* don't report an error if the type is an invalid C (defined) type
             * (go.dev/issue/22090) */
            if (burrow__types_is_valid(types_type_underlying(t)))
                GT_ERRORF(check, burrow__tc_at_node(typ), GT_CODE_INVALID_CONST_TYPE,
                          "invalid constant type %s", burrow__tc_ts(check, t));
            GT_OBJ(obj)->typ = GT_TYP(TYPES_INVALID);
            ok = false;
        } else {
            GT_OBJ(obj)->typ = t;
        }
    }

    if (ok) {
        /* check initialization */
        GtOperand x = {0};
        if (init != NULL) {
            if (inherited) {
                /* The initialization expression is inherited from a previous
                 * constant declaration, and (error) positions refer to that
                 * expression and not the current constant declaration. Use
                 * the constant identifier position for any errors during init
                 * expression evaluation since that is all we have (see issues
                 * go.dev/issue/42991, go.dev/issue/42992). */
                check->env.has_errpos = true;
                check->env.errpos = burrow__tc_at_pos(GT_OBJ(obj)->pos);
            }
            burrow__tc_expr(check, NULL, &x, init);
        }
        burrow__tc_init_const(check, obj, &x);
    }

    check->env.iota = saved_iota;
    check->env.has_errpos = saved_has_errpos;
    check->env.errpos = saved_errpos;
}

static void tcd_var_decl(TypesChecker *check, TypesVar *obj, Slice lhs, AstExpr typ,
                         AstExpr init) {
    burrow__types_assert(GT_OBJ(obj)->typ == NULL);

    /* determine type, if any */
    if (typ != NULL) {
        GT_OBJ(obj)->typ = burrow__tc_var_type(check, typ);
        /* We cannot spread the type to all lhs variables if there are more
         * than one since that would mark them as checked (see
         * Checker.objDecl) and the assignment of init exprs, if any, would
         * not be checked.
         *
         * TODO(gri) If we have no init expr, we should distribute a given
         * type otherwise we need to re-evaluate the type expr for each lhs
         * variable, leading to duplicate work. */
    }

    /* check initialization */
    if (init == NULL) {
        if (typ == NULL) {
            /* error reported before by arityMatch */
            GT_OBJ(obj)->typ = GT_TYP(TYPES_INVALID);
        }
        return;
    }

    if (lhs.len <= 1) {
        burrow__types_assert(lhs.len == 0 || GT_AT(lhs, 0, TypesVar *) == obj);
        GtOperand x = {0};
        burrow__tc_expr(
            check, burrow__tc_new_target(check->a, GT_OBJ(obj)->typ, GT_OBJ(obj)->name),
            &x, init);
        burrow__tc_init_var(check, obj, &x, BURROW_S("variable declaration"));
        return;
    }

    /* We have multiple variables on the lhs and one init expr. Make sure all
     * variables have been given the same type if one was specified, otherwise
     * they assume the type of the init expression values (was
     * go.dev/issue/15755). */
    if (typ != NULL) {
        for (Int i = 0; i < lhs.len; i++)
            GT_OBJ(GT_AT(lhs, i, TypesVar *))->typ = GT_OBJ(obj)->typ;
    }

    Slice rhs = burrow__types_ptrs_make(check->a, 1);
    GT_AT(rhs, 0, AstExpr) = init;
    burrow__tc_init_vars(check, lhs, rhs, NULL);
}

/* isImportedConstraint reports whether typ is an imported type constraint. */
bool burrow__tc_is_imported_constraint(TypesChecker *check, TypesType typ) {
    TypesNamed *named = burrow__types_as_named(typ);
    if (named == NULL || GT_OBJ(named->obj)->pkg == check->pkg ||
        GT_OBJ(named->obj)->pkg == NULL)
        return false;
    TypesType u = types_type_underlying(types_type(named));
    return u->kind == TYPES_KIND_INTERFACE &&
           !types_interface_is_method_set(types_as_interface(u));
}

/* What the later function of typeDecl closes over. */
typedef struct TcdTypeDecl {
    TypesTypeName *obj;
    AstTypeSpec *tdecl;
    TypesType rhs;
    bool version_err; /* only report a version error if we have not reported one */
} TcdTypeDecl;

static void tcd_type_decl_later(TypesChecker *check, void *env) {
    TcdTypeDecl *c = env;
    TypesNamed *t = burrow__types_as_named(GT_OBJ(c->obj)->typ);
    if (t != NULL) /* type may be invalid */
        burrow__tc_valid_type(check, t);
    /* If typ is local, an error was already reported where typ is
     * specified/defined. */
    if (!c->version_err && burrow__tc_is_imported_constraint(check, c->rhs))
        (void)GT_VERIFY_VERSIONF(check, burrow__tc_at_node(c->tdecl->type),
                                 burrow__tc_go1_18, "using type constraint %s",
                                 burrow__tc_ts(check, c->rhs));
}

static void tcd_alias_decl(TypesChecker *check, TcdTypeDecl *c, AstField *tparam0) {
    TypesTypeName *obj = c->obj;
    AstTypeSpec *tdecl = c->tdecl;

    /* Report highest version requirement first so that fixing a version issue
     * avoids possibly two -lang changes (first to Go 1.9 and then to Go
     * 1.23). */
    if (!c->version_err && tparam0 != NULL &&
        !burrow__tc_verify_version(check, GT_AT_NODE(tparam0), burrow__tc_go1_23,
                                   BURROW_S("generic type alias")))
        c->version_err = true;
    if (!c->version_err &&
        !burrow__tc_verify_version(check, burrow__tc_at_pos(tdecl->assign),
                                   burrow__tc_go1_9, BURROW_S("type alias")))
        c->version_err = true;

    TypesAlias *alias = burrow__tc_new_alias(check, obj, NULL);

    /* If we could not type the RHS, set it to invalid. This should only ever
     * happen if we panic before setting. Not in Go: the check is deferred
     * there, and here the alias waits on a list that a bailout goes through
     * instead. */
    burrow__types_ptrs_append(check->a, &check->bail_aliases, alias);

    /* handle type parameters even if not allowed (Alias type is supported) */
    if (tparam0 != NULL) {
        burrow__tc_open_scope(check, GT_NODE(tdecl), BURROW_S("type parameters"));
        burrow__tc_collect_type_params(check, &alias->tparams, tdecl->type_params);
    }

    c->rhs = burrow__tc_declared_type(check, tdecl->type, obj);
    burrow__types_assert(c->rhs != NULL);
    alias->from_rhs = c->rhs;

    /* spec: In an alias declaration the given type cannot be a type parameter
     * declared in the same declaration." (see also go.dev/issue/75884,
     * go.dev/issue/#75885) */
    TypesTypeParam *tpar = types_as_type_param(c->rhs);
    if (tpar != NULL && alias->tparams != NULL) {
        Slice list = alias->tparams->tparams;
        for (Int i = 0; i < list.len; i++) {
            if (GT_AT(list, i, TypesTypeParam *) == tpar) {
                burrow__tc_error(
                    check, burrow__tc_at_node(tdecl->type),
                    GT_CODE_MISPLACED_TYPE_PARAM,
                    BURROW_S(
                        "cannot use type parameter declared in alias declaration as "
                        "RHS"));
                alias->from_rhs = GT_TYP(TYPES_INVALID);
                break;
            }
        }
    }

    if (tparam0 != NULL)
        burrow__tc_close_scope(check);
    check->bail_aliases.len--;
}

static void tcd_type_decl(TypesChecker *check, TypesTypeName *obj, AstTypeSpec *tdecl) {
    burrow__types_assert(GT_OBJ(obj)->typ == NULL);

    TcdTypeDecl *c = GT_NEW(check->a, TcdTypeDecl);
    c->obj = obj;
    c->tdecl = tdecl;
    (void)burrow__tc_later(check, tcd_type_decl_later, c);

    /* First type parameter, or nil. */
    AstField *tparam0 = NULL;
    if (ast_field_list_num_fields(tdecl->type_params) > 0)
        tparam0 = GT_AT(tdecl->type_params->list, 0, AstField *);

    /* alias declaration */
    if (tdecl->assign != TOKEN_NO_POS) {
        tcd_alias_decl(check, c, tparam0);
        return;
    }

    /* type definition or generic type declaration */
    if (!c->version_err && tparam0 != NULL &&
        !burrow__tc_verify_version(check, GT_AT_NODE(tparam0), burrow__tc_go1_18,
                                   BURROW_S("type parameter")))
        c->version_err = true;

    TypesNamed *named =
        burrow__tc_new_named(check, obj, NULL, slice_nil(TYPE_UNSAFE_POINTER));
    if (tdecl->type_params != NULL) {
        burrow__tc_open_scope(check, GT_NODE(tdecl), BURROW_S("type parameters"));
        burrow__tc_collect_type_params(check, &named->tparams, tdecl->type_params);
    }

    c->rhs = burrow__tc_declared_type(check, tdecl->type, obj);
    burrow__types_assert(c->rhs != NULL);
    named->from_rhs = c->rhs;

    /* spec: "In a type definition the given type cannot be a type parameter."
     * (See also go.dev/issue/45639.) */
    if (burrow__types_is_type_param(c->rhs)) {
        burrow__tc_error(
            check, burrow__tc_at_node(tdecl->type), GT_CODE_MISPLACED_TYPE_PARAM,
            BURROW_S("cannot use a type parameter as RHS in type declaration"));
        named->from_rhs = GT_TYP(TYPES_INVALID);
    }

    if (tdecl->type_params != NULL)
        burrow__tc_close_scope(check);
}

static TypesType tcd_bound(TypesChecker *check, AstExpr x);

void burrow__tc_collect_type_params(TypesChecker *check, TypesTypeParamList **dst,
                                    AstFieldList *list) {
    Slice tparams = slice_nil(TYPE_UNSAFE_POINTER);
    /* Declare type parameters up-front, with empty interface as type bound.
     * The scope of type parameters starts at the beginning of the type
     * parameter list (so we can have mutually recursive parameterized
     * interfaces). */
    TokenPos scope_pos = ast_node_pos(GT_NODE(list));
    for (Int i = 0; i < list->list.len; i++) {
        AstField *f = GT_AT(list->list, i, AstField *);
        for (Int j = 0; j < f->names.len; j++)
            burrow__types_ptrs_append(
                check->a, &tparams,
                burrow__tc_declare_type_param(check, GT_AT(f->names, j, AstIdent *),
                                              scope_pos));
    }

    /* Set the type parameters before collecting the type constraints because
     * the parameterized type may be used by the constraints
     * (go.dev/issue/47887). Example: type T[P T[P]] interface{} */
    *dst = burrow__types_bind_tparams(check->a, tparams);

    /* Signal to cycle detection that we are in a type parameter list. We can
     * only be inside one type parameter list at any given time: function
     * closures may appear inside a type parameter list but they cannot be
     * generic, and their bodies are processed in delayed and sequential
     * fashion. Note that with each new declaration, we save the existing
     * environment and restore it when done; thus inTPList is true exactly
     * only when we are in a specific type parameter list. */
    burrow__types_assert(!check->env.in_tparam_list);
    check->env.in_tparam_list = true;

    Int index = 0;
    for (Int i = 0; i < list->list.len; i++) {
        AstField *f = GT_AT(list->list, i, AstField *);
        TypesType bound;
        /* NOTE: we may be able to assert that f.Type != nil here, but this is
         * not an invariant of the AST, so we are cautious. */
        if (f->type != NULL) {
            bound = tcd_bound(check, f->type);
            if (burrow__types_is_type_param(bound)) {
                /* We may be able to allow this since it is now well-defined
                 * what the underlying type and thus type set of a type
                 * parameter is. But we may need some additional form of cycle
                 * detection within type parameter lists. */
                burrow__tc_error(check, burrow__tc_at_node(f->type),
                                 GT_CODE_MISPLACED_TYPE_PARAM,
                                 BURROW_S("cannot use a type parameter as constraint"));
                bound = GT_TYP(TYPES_INVALID);
            }
        } else {
            bound = GT_TYP(TYPES_INVALID);
        }
        for (Int j = 0; j < f->names.len; j++)
            GT_AT(tparams, index + j, TypesTypeParam *)->bound = bound;
        index += f->names.len;
    }

    check->env.in_tparam_list = false;
}

static TypesType tcd_bound(TypesChecker *check, AstExpr x) {
    /* A type set literal of the form ~T and A|B may only appear as
     * constraint; embed it in an implicit interface so that only interface
     * type-checking needs to take care of such type expressions. */
    bool wrap = false;
    if (x->kind == AST_KIND_UNARY_EXPR)
        wrap = ((AstUnaryExpr *)(void *)x)->op == TOKEN_TILDE;
    else if (x->kind == AST_KIND_BINARY_EXPR)
        wrap = ((AstBinaryExpr *)(void *)x)->op == TOKEN_OR;
    if (wrap) {
        AstField *f = (AstField *)(void *)ast_node_new(check->a, AST_KIND_FIELD);
        AstFieldList *l =
            (AstFieldList *)(void *)ast_node_new(check->a, AST_KIND_FIELD_LIST);
        AstInterfaceType *it =
            (AstInterfaceType *)(void *)ast_node_new(check->a, AST_KIND_INTERFACE_TYPE);
        if (f == NULL || l == NULL || it == NULL)
            burrow__types_oom();
        f->type = x;
        l->list = burrow__types_ptrs_make(check->a, 1);
        GT_AT(l->list, 0, AstField *) = f;
        it->methods = l;
        TypesType t = burrow__tc_typ(check, GT_NODE(it));
        /* mark t as implicit interface if all went well */
        if (t->kind == TYPES_KIND_INTERFACE)
            types_as_interface(t)->implicit = true;
        return t;
    }
    return burrow__tc_typ(check, x);
}

TypesTypeParam *burrow__tc_declare_type_param(TypesChecker *check, AstIdent *name,
                                              TokenPos scope_pos) {
    /* Use Typ[Invalid] for the type constraint to ensure that a type is
     * present even if the actual constraint has not been assigned yet.
     * TODO(gri) Need to systematically review all uses of type parameter
     * constraints to make sure we don't rely on them if they are not properly
     * set yet. */
    TypesTypeName *tname =
        types_new_type_name(check->a, name->name_pos, check->pkg, name->name, NULL);
    /* assigns type to tname as a side-effect */
    TypesTypeParam *tpar =
        burrow__tc_new_type_param(check, tname, GT_TYP(TYPES_INVALID));
    burrow__tc_declare(check, check->env.scope, name, types_object(tname), scope_pos);
    return tpar;
}

static void tcd_check_field_uniqueness(TypesChecker *check, void *env) {
    TypesNamed *base = env;
    TypesType u = types_type_underlying(types_type(base));
    if (u->kind != TYPES_KIND_STRUCT)
        return;
    TypesStruct *t = types_as_struct(u);
    GtObjset mset = {check->a, NULL};
    for (Int i = 0; i < types_named_num_methods(base); i++) {
        TypesFunc *m = types_named_method(base, i);
        burrow__types_assert(!str_eq(GT_OBJ(m)->name, BURROW_S("_")));
        burrow__types_assert(burrow__types_objset_insert(&mset, types_object(m)) ==
                             NULL);
    }

    /* Check that any non-blank field names of base are distinct from its
     * method names. */
    for (Int i = 0; i < t->fields.len; i++) {
        TypesVar *fld = GT_AT(t->fields, i, TypesVar *);
        if (str_eq(GT_OBJ(fld)->name, BURROW_S("_")))
            continue;
        TypesObject alt = burrow__types_objset_insert(&mset, types_object(fld));
        if (alt == NULL)
            continue;
        /* Struct fields should already be unique, so we should only encounter
         * an alternate via collision with a method name. */
        burrow__types_assert(alt->kind == TYPES_OBJECT_FUNC);

        /* For historical consistency, we report the primary error on the
         * method, and the alt decl on the field. */
        GtError *err = burrow__tc_new_error(check, GT_CODE_DUPLICATE_FIELD_AND_METHOD);
        GT_ADDF(err, burrow__tc_at_obj(alt), "field and method with the same name %s",
                GT_OBJ(fld)->name);
        burrow__tc_error_add_alt_decl(err, types_object(fld));
        burrow__tc_error_report(err);
    }
}

void burrow__tc_collect_methods(TypesChecker *check, TypesTypeName *obj) {
    /* get associated methods (Checker.collectObjects only collects methods
     * with non-blank names; Checker.resolveBaseTypeName ensures that obj is
     * not an alias name if it has attached methods.) */
    if (check->methods == NULL)
        return;
    Slice **list = map_get(check->methods, &obj);
    if (list == NULL)
        return;
    Slice methods = **list;
    map_del(check->methods, &obj);
    /* don't use TypeName.IsAlias (requires fully set up object) */
    burrow__types_assert(burrow__tc_obj_info(check, types_object(obj))->tdecl->assign ==
                         TOKEN_NO_POS);

    /* use an objset to check for name conflicts */
    GtObjset mset = {check->a, NULL};

    /* spec: "If the base type is a struct type, the non-blank method and field
     * names must be distinct." */
    TypesNamed *base = burrow__types_as_named(GT_OBJ(obj)->typ); /* be conservative */
    if (base != NULL) {
        /* collectMethods should not be called on an instantiated type */
        burrow__types_assert(types_type_list_len(types_named_type_args(base)) == 0);

        /* See go.dev/issue/52529: we must delay the expansion of underlying
         * here, as base may not be fully set-up. */
        (void)burrow__tc_later(check, tcd_check_field_uniqueness, base);

        /* Checker.Files may be called multiple times; additional package
         * files may add methods to already type-checked types. Add
         * pre-existing methods so that we can detect redeclarations. */
        for (Int i = 0; i < types_named_num_methods(base); i++) {
            TypesFunc *m = types_named_method(base, i);
            burrow__types_assert(!str_eq(GT_OBJ(m)->name, BURROW_S("_")));
            burrow__types_assert(burrow__types_objset_insert(&mset, types_object(m)) ==
                                 NULL);
        }
    }

    /* add valid methods */
    for (Int i = 0; i < methods.len; i++) {
        TypesFunc *m = GT_AT(methods, i, TypesFunc *);
        /* spec: "For a base type, the non-blank names of methods bound to it
         * must be unique." */
        burrow__types_assert(!str_eq(GT_OBJ(m)->name, BURROW_S("_")));
        TypesObject alt = burrow__types_objset_insert(&mset, types_object(m));
        if (alt != NULL) {
            if (GT_OBJ(alt)->pos != TOKEN_NO_POS)
                GT_ERRORF(check, burrow__tc_at_obj(types_object(m)),
                          GT_CODE_DUPLICATE_METHOD,
                          "method %s.%s already declared at %s", GT_OBJ(obj)->name,
                          GT_OBJ(m)->name, burrow__tc_ps(check, GT_OBJ(alt)->pos));
            else
                GT_ERRORF(check, burrow__tc_at_obj(types_object(m)),
                          GT_CODE_DUPLICATE_METHOD, "method %s.%s already declared",
                          GT_OBJ(obj)->name, GT_OBJ(m)->name);
            continue;
        }

        if (base != NULL)
            types_named_add_method(base, m);
    }
}

/* What the later function of funcDecl closes over. */
typedef struct TcdFuncBody {
    GtDeclInfo *decl;
    TypesFunc *obj;
    TypesSignature *sig;
} TcdFuncBody;

static void tcd_func_body_later(TypesChecker *check, void *env) {
    TcdFuncBody *c = env;
    burrow__tc_func_body(check, c->decl, GT_OBJ(c->obj)->name, c->sig,
                         c->decl->fdecl->body, NULL);
}

static void tcd_func_decl(TypesChecker *check, TypesFunc *obj, GtDeclInfo *decl) {
    burrow__types_assert(GT_OBJ(obj)->typ == NULL);

    /* func declarations cannot use iota */
    burrow__types_assert(check->env.iota == NULL);

    TypesSignature *sig = GT_NEW(check->a, TypesSignature);
    sig->hdr.kind = TYPES_KIND_SIGNATURE;
    GT_OBJ(obj)->typ = types_type(sig); /* guard against cycles */

    AstFuncDecl *fdecl = decl->fdecl;
    burrow__tc_func_type(check, sig, fdecl->recv, fdecl->type);

    /* types2 handles go:nointerface pragma here by setting obj.nointerface.
     * go/types currently doesn't handle pragmas. */

    /* Set the scope's extent to the complete "func (...) { ... }" so that
     * Scope.Innermost works correctly. */
    sig->scope->pos = ast_node_pos(GT_NODE(fdecl));
    sig->scope->end = ast_node_end(GT_NODE(fdecl));

    if (ast_field_list_num_fields(fdecl->type->type_params) > 0 && fdecl->body == NULL)
        burrow__tc_soft_error(check, GT_AT_NODE(fdecl->name), GT_CODE_BAD_DECL,
                              BURROW_S("generic function is missing function body"));

    /* function body must be type-checked after global declarations (functions
     * implemented elsewhere have no body) */
    if (!check->conf->ignore_func_bodies && fdecl->body != NULL) {
        TcdFuncBody *c = GT_NEW(check->a, TcdFuncBody);
        c->decl = decl;
        c->obj = obj;
        c->sig = sig;
        (void)burrow__tc_later(check, tcd_func_body_later, c);
    }
}

static void tcd_decl_stmt_const(TypesChecker *check, const GtDecl *d) {
    Int top = check->delayed.len;

    /* declare all constants */
    AstValueSpec *spec = d->value_spec;
    Slice lhs = burrow__types_ptrs_make(check->a, spec->names.len);
    for (Int i = 0; i < spec->names.len; i++) {
        AstIdent *name = GT_AT(spec->names, i, AstIdent *);
        TypesConst *obj =
            types_new_const(check->a, name->name_pos, check->pkg, name->name, NULL,
                            constant_make_int64(d->iota));
        GT_AT(lhs, i, TypesConst *) = obj;

        AstExpr init = NULL;
        if (i < d->init.len)
            init = GT_AT(d->init, i, AstExpr);

        tcd_const_decl(check, obj, d->typ, init, d->inherited);
    }

    /* process function literals in init expressions before scope changes */
    burrow__tc_process_delayed(check, top);

    /* spec: "The scope of a constant or variable identifier declared inside a
     * function begins at the end of the ConstSpec or VarSpec (ShortVarDecl
     * for short variable declarations) and ends at the end of the innermost
     * containing block." */
    TokenPos scope_pos = ast_node_end(GT_NODE(spec));
    for (Int i = 0; i < spec->names.len; i++)
        burrow__tc_declare(check, check->env.scope, GT_AT(spec->names, i, AstIdent *),
                           GT_AT(lhs, i, TypesObject), scope_pos);
}

static void tcd_decl_stmt_var(TypesChecker *check, const GtDecl *d) {
    Int top = check->delayed.len;

    AstValueSpec *spec = d->value_spec;
    Slice lhs0 = burrow__types_ptrs_make(check->a, spec->names.len);
    for (Int i = 0; i < spec->names.len; i++) {
        AstIdent *name = GT_AT(spec->names, i, AstIdent *);
        GT_AT(lhs0, i, TypesVar *) = burrow__types_new_var(
            check->a, TYPES_LOCAL_VAR, name->name_pos, check->pkg, name->name, NULL);
    }

    /* initialize all variables */
    for (Int i = 0; i < lhs0.len; i++) {
        TypesVar *obj = GT_AT(lhs0, i, TypesVar *);
        Slice lhs = slice_nil(TYPE_UNSAFE_POINTER);
        AstExpr init = NULL;
        /* Go's switch has the case where lhs and rhs match first; it takes
         * the same init as the default one does. */
        if (spec->values.len == 1 && spec->names.len != 1) {
            /* rhs is expected to be a multi-valued expression */
            lhs = lhs0;
            init = GT_AT(spec->values, 0, AstExpr);
        } else if (i < spec->values.len) {
            init = GT_AT(spec->values, i, AstExpr);
        }
        tcd_var_decl(check, obj, lhs, spec->type, init);
        if (spec->values.len == 1) {
            /* If we have a single lhs variable we are done either way. If we
             * have a single rhs expression, it must be a multi-valued
             * expression, in which case handling the first lhs variable will
             * cause all lhs variables to have a type assigned, and we are done
             * as well. */
            break;
        }
    }

    /* process function literals in init expressions before scope changes */
    burrow__tc_process_delayed(check, top);

    /* declare all variables (only at this point are the variable scopes
     * (parents) set) */
    TokenPos scope_pos = ast_node_end(GT_NODE(spec)); /* see constant declarations */
    for (Int i = 0; i < spec->names.len; i++) {
        /* see constant declarations */
        burrow__tc_declare(check, check->env.scope, GT_AT(spec->names, i, AstIdent *),
                           GT_AT(lhs0, i, TypesObject), scope_pos);
    }
}

static AstNode tcd_decl_node(const GtDecl *d) {
    switch (d->kind) {
    case GT_DECL_IMPORT:
        return GT_NODE(d->import_spec);
    case GT_DECL_CONST:
    case GT_DECL_VAR:
        return GT_NODE(d->value_spec);
    case GT_DECL_TYPE:
        return GT_NODE(d->type_spec);
    case GT_DECL_FUNC:
        return GT_NODE(d->func_decl);
    default:
        return NULL;
    }
}

static void tcd_decl_stmt_fn(TypesChecker *check, void *env, const GtDecl *d) {
    (void)env;
    switch ((Int)d->kind) {
    case GT_DECL_CONST:
        tcd_decl_stmt_const(check, d);
        break;
    case GT_DECL_VAR:
        tcd_decl_stmt_var(check, d);
        break;
    case GT_DECL_TYPE: {
        AstTypeSpec *spec = d->type_spec;
        TypesTypeName *obj = types_new_type_name(check->a, spec->name->name_pos,
                                                 check->pkg, spec->name->name, NULL);
        /* spec: "The scope of a type identifier declared inside a function
         * begins at the identifier in the TypeSpec and ends at the end of the
         * innermost containing block." */
        TokenPos scope_pos = spec->name->name_pos;
        burrow__tc_declare(check, check->env.scope, spec->name, types_object(obj),
                           scope_pos);
        burrow__tc_push(check, types_object(obj)); /* mark as grey */
        tcd_type_decl(check, obj, spec);
        burrow__tc_pop(check);
        break;
    }
    default: {
        AstNode n = tcd_decl_node(d);
        GT_ERRORF(check, burrow__tc_at_node(n), GT_CODE_INVALID_SYNTAX_TREE,
                  "unknown ast.Decl node %s", tcd_node_type(check, n));
        break;
    }
    }
}

void burrow__tc_decl_stmt(TypesChecker *check, AstDecl d) {
    burrow__tc_walk_decl(check, d, tcd_decl_stmt_fn, NULL);
}
