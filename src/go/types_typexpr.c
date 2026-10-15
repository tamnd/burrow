/* go/types: type-checking of identifiers and type expressions.
 *
 * Derived from Go's src/go/types/typexpr.go. Go source: go1.27.1.
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
#include "burrow/panic.h"
#include "burrow/slice.h"
#include "burrow/unicode.h"

#include "types_check.h"
#include "types_internal.h"

#include <stdbool.h>
#include <stdint.h>

/* A nil constant.Value. */
static const ConstantValue tte_nil = {0};

static void tte_set_used(Map *m, const void *key) {
    bool yes = true;
    if (!map_set(m, key, &yes))
        burrow__types_oom();
}

/* isValidName reports whether s is a valid Go identifier. */
static bool tte_is_valid_name(Str s) {
    StrIter it = str_runes(s);
    Int i;
    Rune ch;
    while (str_next_rune(&it, &i, &ch)) {
        if (!(unicode_is_letter(ch) || ch == '_' || (i > 0 && unicode_is_digit(ch))))
            return false;
    }
    return true;
}

/* ident type-checks identifier e and initializes x with the value or type of
 * e. If an error occurred, x.mode is set to invalid. If wantType is set, the
 * identifier e is expected to denote a type. */
void burrow__tc_ident(TypesChecker *check, GtOperand *x, AstIdent *e, bool want_type) {
    burrow__tc_op_invalidate(x);
    x->expr = GT_NODE(e);

    TypesObject obj = NULL;
    TypesScope *scope = burrow__tc_env_lookup_scope(&check->env, e->name, &obj);
    if (obj == NULL) {
        if (str_eq(e->name, BURROW_S("_")))
            burrow__tc_error(check, GT_AT_NODE(e), GT_CODE_INVALID_BLANK,
                             BURROW_S("cannot use _ as value or type"));
        else if (tte_is_valid_name(e->name))
            GT_ERRORF(check, GT_AT_NODE(e), GT_CODE_UNDECLARED_NAME, "undefined: %s",
                      e->name);
        return;
    }
    if (obj == burrow__types_universe_any() ||
        obj == burrow__types_universe_comparable()) {
        if (!GT_VERIFY_VERSIONF(check, GT_AT_NODE(e), burrow__tc_go1_18,
                                "predeclared %s", e->name))
            return; /* avoid follow-on errors */
    }
    burrow__tc_record_use(check, e, obj);

    /* If we want a type but don't have one, stop right here and avoid
     * potential problems with missing underlying types. This also gives better
     * error messages in some cases (see go.dev/issue/65344). */
    bool got_type = obj->kind == TYPES_OBJECT_TYPE_NAME;
    if (!got_type && want_type) {
        GT_ERRORF(check, GT_AT_NODE(e), GT_CODE_NOT_A_TYPE, "%s (%s) is not a type",
                  GT_OBJ(obj)->name, burrow__types_object_kind(obj));

        /* avoid "declared but not used" errors (don't use Checker.use - we
         * don't want to evaluate too much) */
        TypesVar *v = types_as_var(obj);
        if (v != NULL && GT_OBJ(v)->pkg == check->pkg /* see Checker.use1 */)
            tte_set_used(check->used_vars, &v);
        return;
    }

    /* Type-check the object. Only call Checker.objDecl if the object doesn't
     * have a type yet (in which case we must actually determine it) or the
     * object is a TypeName from the current package and we also want a type
     * (in which case we might detect a cycle which needs to be reported).
     * Otherwise we can skip the call and avoid a possible cycle error in
     * favor of the more informative "not a type/value" error that this
     * function's caller will issue (see go.dev/issue/25790).
     *
     * Note that it is important to avoid calling objDecl on objects from
     * other packages, to avoid races: see issue #69912. */
    TypesType typ = GT_OBJ(obj)->typ;
    if (typ == NULL || (got_type && want_type && GT_OBJ(obj)->pkg == check->pkg)) {
        burrow__tc_obj_decl(check, obj);
        typ = GT_OBJ(obj)->typ; /* type must have been assigned by Checker.objDecl */
    }
    burrow__types_assert(typ != NULL);

    /* The object may have been dot-imported. If so, mark the respective
     * package as used. (This code is only needed for dot-imports. Without
     * them, we only have to mark variables, see *Var case below). */
    if (check->dot_import_map != NULL) {
        Map **names = map_get(check->dot_import_map, &scope);
        if (names != NULL) {
            TypesPkgName **pkg_name = map_get(*names, &GT_OBJ(obj)->name);
            if (pkg_name != NULL && *pkg_name != NULL)
                tte_set_used(check->used_pkg_names, pkg_name);
        }
    }

    switch (obj->kind) {
    case TYPES_OBJECT_PKG_NAME:
        GT_ERRORF(check, GT_AT_NODE(e), GT_CODE_INVALID_PKG_USE,
                  "use of package %s not in selector", GT_OBJ(obj)->name);
        return;

    case TYPES_OBJECT_CONST:
        burrow__tc_add_decl_dep(check, obj);
        if (!burrow__types_is_valid(typ))
            return;
        if (obj == burrow__types_universe_iota()) {
            if (check->env.iota == NULL) {
                burrow__tc_error(
                    check, GT_AT_NODE(e), GT_CODE_INVALID_IOTA,
                    BURROW_S("cannot use iota outside constant declaration"));
                return;
            }
            x->val = *check->env.iota;
        } else {
            x->val = types_as_const(obj)->val;
        }
        x->mode = GT_MODE_CONSTANT;
        break;

    case TYPES_OBJECT_TYPE_NAME:
        x->mode = GT_MODE_TYPEXPR;
        break;

    case TYPES_OBJECT_VAR: {
        /* It's ok to mark non-local variables, but ignore variables from other
         * packages to avoid potential race conditions with dot-imported
         * variables. */
        TypesVar *v = types_as_var(obj);
        if (GT_OBJ(v)->pkg == check->pkg)
            tte_set_used(check->used_vars, &v);
        burrow__tc_add_decl_dep(check, obj);
        if (!burrow__types_is_valid(typ))
            return;
        x->mode = GT_MODE_VARIABLE;
        break;
    }

    case TYPES_OBJECT_FUNC:
        burrow__tc_add_decl_dep(check, obj);
        x->mode = GT_MODE_VALUE;
        break;

    case TYPES_OBJECT_BUILTIN:
        x->id = types_as_builtin(obj)->id;
        x->mode = GT_MODE_BUILTIN;
        break;

    case TYPES_OBJECT_NIL:
        x->mode = GT_MODE_VALUE;
        break;

    default:
        panic_str(BURROW_S("unreachable"));
    }

    x->typ = typ;
}

/* typ type-checks the type expression e and returns its type, or
 * Typ[Invalid]. The type must not be an (uninstantiated) generic type. */
TypesType burrow__tc_typ(TypesChecker *check, AstExpr e) {
    return burrow__tc_declared_type(check, e, NULL);
}

/* varType type-checks the type expression e and returns its type, or
 * Typ[Invalid]. The type must not be an (uninstantiated) generic type and it
 * must not be a constraint interface. */
TypesType burrow__tc_var_type(TypesChecker *check, AstExpr e) {
    TypesType typ = burrow__tc_declared_type(check, e, NULL);
    burrow__tc_valid_var_type(check, e, typ);
    return typ;
}

/* What the later function of validVarType closes over. */
typedef struct TteVarType {
    AstExpr e;
    TypesType typ;
} TteVarType;

static void tte_valid_var_type_later(TypesChecker *check, void *env) {
    TteVarType *c = env;
    TypesType u = types_type_underlying(c->typ);
    if (u->kind != TYPES_KIND_INTERFACE)
        return;
    /* TODO(gri) is this the correct position? */
    GtTypeSet *tset = burrow__types_compute_interface_type_set(ast_node_pos(c->e),
                                                               types_as_interface(u));
    if (!burrow__types_type_set_is_method_set(tset)) {
        if (tset->comparable)
            GT_SOFT_ERRORF(check, burrow__tc_at_node(c->e),
                           GT_CODE_MISPLACED_CONSTRAINT_IFACE,
                           "cannot use type %s outside a type constraint: interface is "
                           "(or embeds) comparable",
                           burrow__tc_ts(check, c->typ));
        else
            GT_SOFT_ERRORF(check, burrow__tc_at_node(c->e),
                           GT_CODE_MISPLACED_CONSTRAINT_IFACE,
                           "cannot use type %s outside a type constraint: interface "
                           "contains type constraints",
                           burrow__tc_ts(check, c->typ));
    }
}

/* validVarType reports an error if typ is a constraint interface. The
 * expression e is used for error reporting, if any. */
void burrow__tc_valid_var_type(TypesChecker *check, AstExpr e, TypesType typ) {
    /* If we have a type parameter there's nothing to do. */
    if (burrow__types_is_type_param(typ))
        return;

    /* We don't want to call typ.Underlying() or complete interfaces while we
     * are in the middle of type-checking parameter declarations that might
     * belong to interface methods. Delay this check to the end of
     * type-checking. */
    TteVarType *c = GT_NEW(check->a, TteVarType);
    c->e = e;
    c->typ = typ;
    (void)burrow__tc_later(check, tte_valid_var_type_later, c);
}

static TypesType tte_typ_internal(TypesChecker *check, AstExpr e0, TypesTypeName *def);

/* declaredType is like typ but also accepts a type name def. If def != nil, e
 * is the type specification for the [Alias] or [Named] type named def, and
 * def.typ.fromRHS will be set to the [Type] of e immediately after its
 * creation. */
TypesType burrow__tc_declared_type(TypesChecker *check, AstExpr e, TypesTypeName *def) {
    TypesType typ = tte_typ_internal(check, e, def);
    burrow__types_assert(burrow__types_is_typed(typ));
    if (burrow__types_is_generic(typ)) {
        GT_ERRORF(check, burrow__tc_at_node(e), GT_CODE_WRONG_TYPE_ARG_COUNT,
                  "cannot use generic type %s without instantiation",
                  burrow__tc_ts(check, typ));
        typ = GT_TYP(TYPES_INVALID);
    }
    burrow__tc_record_type_and_value(check, e, GT_MODE_TYPEXPR, typ, tte_nil);
    return typ;
}

/* genericType is like typ but the type must be an (uninstantiated) generic
 * type. If cause is non-nil and the type expression was a valid type but not
 * generic, cause will be populated with a message describing the error.
 *
 * Note: If the type expression was invalid and an error was reported before,
 * cause will not be populated; thus cause alone cannot be used to determine if
 * an error occurred. */
TypesType burrow__tc_generic_type(TypesChecker *check, AstExpr e, Str *cause) {
    TypesType typ = tte_typ_internal(check, e, NULL);
    burrow__types_assert(burrow__types_is_typed(typ));
    if (burrow__types_is_valid(typ) && !burrow__types_is_generic(typ)) {
        if (cause != NULL)
            *cause = GT_SPRINTF(check, "%s is not a generic type",
                                burrow__tc_ts(check, typ));
        typ = GT_TYP(TYPES_INVALID);
    }
    /* TODO(gri) what is the correct call below? */
    burrow__tc_record_type_and_value(check, e, GT_MODE_TYPEXPR, typ, tte_nil);
    return typ;
}

/* The end of the Ident and SelectorExpr cases of typInternal: the type x
 * denotes, or NULL after reporting why it is none. */
static TypesType tte_operand_type(TypesChecker *check, GtOperand *x) {
    switch ((Int)x->mode) {
    case GT_MODE_TYPEXPR:
        return x->typ;
    case GT_MODE_INVALID:
        /* ignore - error reported before */
        break;
    case GT_MODE_NOVALUE:
        GT_ERRORF(check, burrow__tc_at_op(x), GT_CODE_NOT_A_TYPE, "%s used as type",
                  burrow__tc_xs(check, x));
        break;
    default:
        GT_ERRORF(check, burrow__tc_at_op(x), GT_CODE_NOT_A_TYPE, "%s is not a type",
                  burrow__tc_xs(check, x));
        break;
    }
    return NULL;
}

static TypesType tte_instantiated_type(TypesChecker *check, GtIndexedExpr *ix);
static int64_t tte_array_length(TypesChecker *check, AstExpr e);

/* What the later function of the MapType case closes over. */
typedef struct TteMapKey {
    TypesMap *typ;
    AstExpr key;
} TteMapKey;

static void tte_map_key_later(TypesChecker *check, void *env) {
    TteMapKey *c = env;
    TypesType key = c->typ->key;
    if (!types_comparable(key)) {
        Str why = BURROW_STR_EMPTY;
        if (burrow__types_is_type_param(key))
            why = BURROW_S(" (missing comparable constraint)");
        GT_ERRORF(check, burrow__tc_at_node(c->key), GT_CODE_INCOMPARABLE_MAP_KEY,
                  "invalid map key type %s%s", burrow__tc_ts(check, key), why);
    }
}

static TypesType tte_array_type(TypesChecker *check, AstArrayType *e) {
    if (e->len == NULL) {
        TypesSlice *typ = GT_NEW(check->a, TypesSlice);
        typ->hdr.kind = TYPES_KIND_SLICE;
        typ->elem = burrow__tc_var_type(check, e->elt);
        return types_type(typ);
    }

    TypesArray *typ = GT_NEW(check->a, TypesArray);
    typ->hdr.kind = TYPES_KIND_ARRAY;
    /* Provide a more specific error when encountering a [...] array rather
     * than leaving it to the handling of the ... expression. */
    if (e->len->kind == AST_KIND_ELLIPSIS) {
        burrow__tc_error(
            check, burrow__tc_at_node(e->len), GT_CODE_BAD_DOT_DOT_DOT_SYNTAX,
            BURROW_S("invalid use of [...] array (outside a composite literal)"));
        typ->len = -1;
    } else {
        typ->len = tte_array_length(check, e->len);
    }
    typ->elem = burrow__tc_var_type(check, e->elt);
    if (typ->len >= 0)
        return types_type(typ);
    /* report error if we encountered [...] */
    return NULL;
}

static TypesType tte_chan_type(TypesChecker *check, AstChanType *e) {
    TypesChan *typ = GT_NEW(check->a, TypesChan);
    typ->hdr.kind = TYPES_KIND_CHAN;

    TypesChanDir dir = TYPES_SEND_RECV;
    switch (e->dir) {
    case AST_SEND | AST_RECV:
        /* nothing to do */
        break;
    case AST_SEND:
        dir = TYPES_SEND_ONLY;
        break;
    case AST_RECV:
        dir = TYPES_RECV_ONLY;
        break;
    default: {
        Int d = e->dir;
        GT_ERRORF(check, GT_AT_NODE(e), GT_CODE_INVALID_SYNTAX_TREE,
                  "unknown channel direction %d", d);
        /* ok to continue */
        break;
    }
    }

    typ->dir = dir;
    typ->elem = burrow__tc_var_type(check, e->value);
    return types_type(typ);
}

/* typInternal drives type checking of types. Must only be called by
 * declaredType or genericType. */
static TypesType tte_typ_internal(TypesChecker *check, AstExpr e0, TypesTypeName *def) {
    TypesType t = NULL;
    switch (e0->kind) {
    case AST_KIND_BAD_EXPR:
        /* ignore - error reported before */
        break;

    case AST_KIND_IDENT: {
        GtOperand x = {0};
        burrow__tc_ident(check, &x, (AstIdent *)(void *)e0, true);
        t = tte_operand_type(check, &x);
        break;
    }

    case AST_KIND_SELECTOR_EXPR: {
        GtOperand x = {0};
        burrow__tc_selector(check, &x, (AstSelectorExpr *)(void *)e0, true);
        t = tte_operand_type(check, &x);
        break;
    }

    case AST_KIND_INDEX_EXPR:
    case AST_KIND_INDEX_LIST_EXPR: {
        GtIndexedExpr ix;
        (void)burrow__tc_unpack_indexed_expr(check->a, e0, &ix);
        (void)burrow__tc_verify_version(check, burrow__tc_in_node(e0, ix.lbrack),
                                        burrow__tc_go1_18,
                                        BURROW_S("type instantiation"));
        return tte_instantiated_type(check, &ix);
    }

    case AST_KIND_PAREN_EXPR:
        /* Generic types must be instantiated before they can be used in any
         * form. Consequently, generic types cannot be parenthesized. */
        return burrow__tc_declared_type(check, ((AstParenExpr *)(void *)e0)->x, def);

    case AST_KIND_ARRAY_TYPE:
        t = tte_array_type(check, (AstArrayType *)(void *)e0);
        break;

    case AST_KIND_ELLIPSIS:
        /* dots are handled explicitly where they are valid */
        burrow__tc_error(check, burrow__tc_at_node(e0), GT_CODE_INVALID_SYNTAX_TREE,
                         BURROW_S("invalid use of ..."));
        break;

    case AST_KIND_STRUCT_TYPE: {
        TypesStruct *typ = GT_NEW(check->a, TypesStruct);
        typ->hdr.kind = TYPES_KIND_STRUCT;
        burrow__tc_struct_type(check, typ, (AstStructType *)(void *)e0);
        return types_type(typ);
    }

    case AST_KIND_STAR_EXPR: {
        TypesPointer *typ = GT_NEW(check->a, TypesPointer);
        typ->hdr.kind = TYPES_KIND_POINTER;
        /* avoid nil base in invalid recursive type declaration */
        typ->base = GT_TYP(TYPES_INVALID);
        typ->base = burrow__tc_var_type(check, ((AstStarExpr *)(void *)e0)->x);
        /* If typ.base is invalid, it's unlikely that *base is particularly
         * useful - even a valid dereferenciation will lead to an invalid type
         * again, and in some cases we get unexpected follow-on errors (e.g.,
         * go.dev/issue/49005). Return an invalid type instead. */
        if (!burrow__types_is_valid(typ->base))
            return GT_TYP(TYPES_INVALID);
        return types_type(typ);
    }

    case AST_KIND_FUNC_TYPE: {
        TypesSignature *typ = GT_NEW(check->a, TypesSignature);
        typ->hdr.kind = TYPES_KIND_SIGNATURE;
        burrow__tc_func_type(check, typ, NULL, (AstFuncType *)(void *)e0);
        return types_type(typ);
    }

    case AST_KIND_INTERFACE_TYPE: {
        TypesInterface *typ = burrow__tc_new_interface(check);
        burrow__tc_interface_type(check, typ, (AstInterfaceType *)(void *)e0, def);
        return types_type(typ);
    }

    case AST_KIND_MAP_TYPE: {
        AstMapType *e = (AstMapType *)(void *)e0;
        TypesMap *typ = GT_NEW(check->a, TypesMap);
        typ->hdr.kind = TYPES_KIND_MAP;
        typ->key = burrow__tc_var_type(check, e->key);
        typ->elem = burrow__tc_var_type(check, e->value);

        /* spec: "The comparison operators == and != must be fully defined for
         * operands of the key type; thus the key type must not be a function,
         * map, or slice."
         *
         * Delay this check because it requires fully setup types; it is safe
         * to continue in any case (was go.dev/issue/6667). */
        TteMapKey *c = GT_NEW(check->a, TteMapKey);
        c->typ = typ;
        c->key = e->key;
        (void)burrow__tc_later(check, tte_map_key_later, c);

        return types_type(typ);
    }

    case AST_KIND_CHAN_TYPE:
        return tte_chan_type(check, (AstChanType *)(void *)e0);

    default:
        GT_ERRORF(check, burrow__tc_at_node(e0), GT_CODE_NOT_A_TYPE, "%s is not a type",
                  burrow__tc_es(check, e0));
        (void)burrow__tc_use1(check, e0, false);
        break;
    }

    if (t != NULL)
        return t;
    return GT_TYP(TYPES_INVALID);
}

/* The type parameters and the object of a type that satisfies Go's
 * genericType interface, a named type or an alias. */
static TypesTypeParamList *tte_generic_tparams(TypesType t, TypesTypeName **obj) {
    if (t->kind == TYPES_KIND_NAMED) {
        TypesNamed *n = types_as_named(t);
        *obj = n->obj;
        return types_named_type_params(n);
    }
    burrow__types_assert(t->kind == TYPES_KIND_ALIAS);
    TypesAlias *a = types_as_alias(t);
    *obj = a->obj;
    return types_alias_type_params(a);
}

/* What the later function of instantiatedType closes over. */
typedef struct TteInst {
    GtIndexedExpr *ix;
    Slice targs;
    TypesType inst;
} TteInst;

static void tte_instantiated_type_later(TypesChecker *check, void *env) {
    TteInst *c = env;
    GtIndexedExpr *ix = c->ix;
    TokenPos ix_pos = ast_node_pos(ix->x);

    /* This is an instance from the source, not from recursive substitution,
     * and so it must be resolved during type-checking so that we can report
     * errors. */
    burrow__tc_record_instance(check, ix->orig, c->targs, c->inst);

    TypesTypeName *obj;
    Slice tparams = burrow__types_tparam_list(tte_generic_tparams(c->inst, &obj));
    if (burrow__tc_validate_targ_len(check, ix_pos, GT_OBJ(obj)->name, tparams.len,
                                     c->targs.len)) {
        /* check type constraints */
        Error err = BURROW_NO_ERROR;
        Int i = burrow__tc_verify(check, ix_pos, tparams, c->targs,
                                  burrow__tc_context(check), &err);
        if (BURROW_FAILED(err)) {
            /* best position for error reporting */
            TokenPos pos = ix_pos;
            if (i < ix->indices.len)
                pos = ast_node_pos(GT_AT(ix->indices, i, AstExpr));
            burrow__tc_soft_error(check, burrow__tc_at_pos(pos),
                                  GT_CODE_INVALID_TYPE_ARG, error_text(err));
        } else {
            burrow__tc_mono_record_instance(check, check->pkg, ix_pos, tparams,
                                            c->targs, ix->indices);
        }
    }
}

static TypesType tte_instantiated_type(TypesChecker *check, GtIndexedExpr *ix) {
    Str cause = BURROW_STR_EMPTY;
    TypesType typ = burrow__tc_generic_type(check, ix->x, &cause);
    if (cause.len > 0)
        GT_ERRORF(check, burrow__tc_at_node(ix->orig), GT_CODE_NOT_A_GENERIC_TYPE,
                  GT_INVALID_OP "%s (%s)", burrow__tc_es(check, ix->orig), cause);
    if (!burrow__types_is_valid(typ))
        return typ; /* error already reported */
    /* typ must be a generic Alias or Named type (but not a *Signature) */
    if (typ->kind == TYPES_KIND_SIGNATURE)
        panic_str(BURROW_S("unexpected generic signature"));
    burrow__types_assert(typ->kind == TYPES_KIND_NAMED ||
                         typ->kind == TYPES_KIND_ALIAS);

    /* evaluate arguments */
    Slice targs;
    if (!burrow__tc_type_list(check, ix->indices, &targs))
        return GT_TYP(TYPES_INVALID);

    /* create instance The instance is not generic anymore as it has type
     * arguments, but unless instantiation failed, it still satisfies the
     * genericType interface because it has type parameters, too. */
    TypesType ityp = burrow__tc_instance(check, ast_node_pos(ix->x), typ, targs, NULL,
                                         burrow__tc_context(check));
    if (ityp->kind != TYPES_KIND_NAMED && ityp->kind != TYPES_KIND_ALIAS)
        return GT_TYP(TYPES_INVALID);

    /* For Named types, orig.tparams may not be set up, so we need to do
     * expansion later. */
    TteInst *c = GT_NEW(check->a, TteInst);
    c->ix = GT_NEW(check->a, GtIndexedExpr);
    *c->ix = *ix;
    c->targs = targs;
    c->inst = ityp;
    (void)burrow__tc_later(check, tte_instantiated_type_later, c);

    return ityp;
}

/* arrayLength type-checks the array length expression e and returns the
 * constant length >= 0, or a value < 0 to indicate an error (and thus an
 * unknown length). */
static int64_t tte_array_length(TypesChecker *check, AstExpr e) {
    /* If e is an identifier, the array declaration might be an attempt at a
     * parameterized type declaration with missing constraint. Provide an
     * error message that mentions array length. */
    if (e->kind == AST_KIND_IDENT) {
        AstIdent *name = (AstIdent *)(void *)e;
        TypesObject obj = burrow__tc_env_lookup(&check->env, name->name);
        if (obj == NULL) {
            GT_ERRORF(check, GT_AT_NODE(name), GT_CODE_INVALID_ARRAY_LEN,
                      "undefined array length %s or missing type constraint",
                      name->name);
            return -1;
        }
        if (obj->kind != TYPES_OBJECT_CONST) {
            GT_ERRORF(check, GT_AT_NODE(name), GT_CODE_INVALID_ARRAY_LEN,
                      "invalid array length %s", name->name);
            return -1;
        }
    }

    GtOperand x = {0};
    burrow__tc_expr(check, NULL, &x, e);
    if (x.mode != GT_MODE_CONSTANT) {
        if (burrow__tc_op_valid(&x))
            GT_ERRORF(check, burrow__tc_at_op(&x), GT_CODE_INVALID_ARRAY_LEN,
                      "array length %s must be constant", burrow__tc_xs(check, &x));
        return -1;
    }

    if (!burrow__types_is_typed(x.typ) ||
        burrow__types_is_basic(x.typ, TYPES_IS_INTEGER)) {
        ConstantValue val = constant_to_int(check->a, x.val);
        if (constant_value_kind(val) == CONSTANT_INT) {
            if (burrow__tc_representable_const(check->a, val, check,
                                               types_typ[TYPES_INT], NULL)) {
                bool ok = false;
                int64_t n = constant_int64_val(val, &ok);
                if (ok && n >= 0)
                    return n;
            }
        }
    }

    if (burrow__types_is_basic(x.typ, TYPES_IS_INTEGER))
        GT_ERRORF(check, burrow__tc_at_op(&x), GT_CODE_INVALID_ARRAY_LEN,
                  "invalid array length %s", burrow__tc_xs(check, &x));
    else
        GT_ERRORF(check, burrow__tc_at_op(&x), GT_CODE_INVALID_ARRAY_LEN,
                  "array length %s must be integer", burrow__tc_xs(check, &x));
    return -1;
}

/* typeList provides the list of types corresponding to the incoming
 * expression list. If an error occurred, the result is nil, but all list
 * elements were type-checked. Not in Go: nil is false here, as an empty list
 * is not nil in Go either. */
bool burrow__tc_type_list(TypesChecker *check, Slice list, Slice *res) {
    *res = burrow__types_ptrs_make(check->a, list.len);
    bool ok = true;
    for (Int i = 0; i < list.len; i++) {
        TypesType t = burrow__tc_var_type(check, GT_AT(list, i, AstExpr));
        if (!burrow__types_is_valid(t))
            ok = false;
        if (ok)
            GT_AT(*res, i, TypesType) = t;
    }
    if (!ok)
        *res = slice_nil(TYPE_UNSAFE_POINTER);
    return ok;
}
