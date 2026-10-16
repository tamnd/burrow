/* go/types: the checker itself, what it records, and the API around it.
 *
 * Derived from Go's src/go/types/check.go, api.go, recording.go and
 * version.go. Go source: go1.27.1.
 *
 * Copyright 2011 The Go Authors. All rights reserved.
 * Copyright 2026 The burrow Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style licence that can be found
 * in the LICENSE file. */

#include "burrow/go/types.h"

#include "burrow/bytes.h"
#include "burrow/core.h"
#include "burrow/fmt.h"
#include "burrow/go/ast.h"
#include "burrow/go/constant.h"
#include "burrow/go/token.h"
#include "burrow/go/version.h"
#include "burrow/map.h"
#include "burrow/panic.h"
#include "burrow/slice.h"
#include "burrow/type.h"

#include "types_check.h"
#include "types_internal.h"

#include <stdbool.h>
#include <stdint.h>

/* ------------------------------------------------------------- descriptors */

static const Type tc_type_and_value_desc = {
    {(const Byte *)"TypeAndValue", 12},
    {(const Byte *)"go/types", 8},
    KIND_STRUCT,
    (uint32_t)sizeof(TypesTypeAndValue),
    (uint16_t)_Alignof(TypesTypeAndValue),
    0,
    0,
    NULL,
    NULL,
    NULL,
    NULL,
    0,
    0x67747476U, /* "gttv" */
    NULL,
};

const Type *const TYPE_TYPES_TYPE_AND_VALUE = &tc_type_and_value_desc;

static const Type tc_instance_desc = {
    {(const Byte *)"Instance", 8},
    {(const Byte *)"go/types", 8},
    KIND_STRUCT,
    (uint32_t)sizeof(TypesInstance),
    (uint16_t)_Alignof(TypesInstance),
    0,
    0,
    NULL,
    NULL,
    NULL,
    NULL,
    0,
    0x6774696eU, /* "gtin" */
    NULL,
};

const Type *const TYPE_TYPES_INSTANCE = &tc_instance_desc;

static void tc_set(Map *m, const void *key, const void *val) {
    if (!map_set(m, key, val))
        burrow__types_oom();
}

static Map *tc_map(Alloc *a, const Type *key, const Type *val) {
    Map *m = map_make(a, key, val, 0);
    if (m == NULL)
        burrow__types_oom();
    return m;
}

/* -------------------------------------------------------------------- api */

bool types_type_and_value_is_void(TypesTypeAndValue tv) {
    return tv.mode == GT_MODE_NOVALUE;
}

bool types_type_and_value_is_type(TypesTypeAndValue tv) {
    return tv.mode == GT_MODE_TYPEXPR;
}

bool types_type_and_value_is_builtin(TypesTypeAndValue tv) {
    return tv.mode == GT_MODE_BUILTIN;
}

bool types_type_and_value_is_value(TypesTypeAndValue tv) {
    switch (tv.mode) {
    case GT_MODE_CONSTANT:
    case GT_MODE_VARIABLE:
    case GT_MODE_MAPINDEX:
    case GT_MODE_VALUE:
    case GT_MODE_COMMAOK:
    case GT_MODE_COMMAERR:
        return true;
    default:
        return false;
    }
}

bool types_type_and_value_is_nil(TypesTypeAndValue tv) {
    return tv.mode == GT_MODE_VALUE && tv.type == GT_TYP(TYPES_UNTYPED_NIL);
}

bool types_type_and_value_addressable(TypesTypeAndValue tv) {
    return tv.mode == GT_MODE_VARIABLE;
}

bool types_type_and_value_assignable(TypesTypeAndValue tv) {
    return tv.mode == GT_MODE_VARIABLE || tv.mode == GT_MODE_MAPINDEX;
}

bool types_type_and_value_has_ok(TypesTypeAndValue tv) {
    return tv.mode == GT_MODE_COMMAOK || tv.mode == GT_MODE_MAPINDEX;
}

Str types_initializer_string(Alloc *a, const TypesInitializer *init) {
    BytesBuffer buf = BYTES_BUFFER(a);
    for (Int i = 0; i < init->lhs.len; i++) {
        if (i > 0)
            burrow__types_buf_str(&buf, BURROW_S(", "));
        burrow__types_buf_str(&buf, GT_OBJ(GT_AT(init->lhs, i, TypesVar *))->name);
    }
    burrow__types_buf_str(&buf, BURROW_S(" = "));
    types_write_expr(&buf, init->rhs);
    return burrow__types_buf_done(&buf, a);
}

TypesInfo types_info_all(Alloc *a) {
    TypesInfo info = {0};
    info.types = tc_map(a, TYPE_UNSAFE_POINTER, TYPE_TYPES_TYPE_AND_VALUE);
    info.instances = tc_map(a, TYPE_UNSAFE_POINTER, TYPE_TYPES_INSTANCE);
    info.defs = tc_map(a, TYPE_UNSAFE_POINTER, TYPE_UNSAFE_POINTER);
    info.uses = tc_map(a, TYPE_UNSAFE_POINTER, TYPE_UNSAFE_POINTER);
    info.implicits = tc_map(a, TYPE_UNSAFE_POINTER, TYPE_UNSAFE_POINTER);
    info.selections = tc_map(a, TYPE_UNSAFE_POINTER, TYPE_UNSAFE_POINTER);
    info.scopes = tc_map(a, TYPE_UNSAFE_POINTER, TYPE_UNSAFE_POINTER);
    info.file_versions = tc_map(a, TYPE_UNSAFE_POINTER, TYPE_STRING);
    return info;
}

static TypesObject tc_info_obj(Map *m, const void *key) {
    if (m == NULL)
        return NULL;
    TypesObject *obj = map_get(m, &key);
    return obj != NULL ? *obj : NULL;
}

TypesType types_info_type_of(TypesInfo *info, AstExpr e) {
    if (info->types != NULL) {
        TypesTypeAndValue *t = map_get(info->types, &e);
        if (t != NULL)
            return t->type;
    }
    if (e != NULL && e->kind == AST_KIND_IDENT) {
        TypesObject obj = types_info_object_of(info, (AstIdent *)(void *)e);
        if (obj != NULL)
            return types_object_type(obj);
    }
    return NULL;
}

TypesObject types_info_object_of(TypesInfo *info, AstIdent *id) {
    TypesObject obj = tc_info_obj(info->defs, id);
    if (obj != NULL)
        return obj;
    return tc_info_obj(info->uses, id);
}

TypesPkgName *types_info_pkg_name_of(TypesInfo *info, AstImportSpec *imp) {
    TypesObject obj;
    if (imp->name != NULL)
        obj = tc_info_obj(info->defs, imp->name);
    else
        obj = tc_info_obj(info->implicits, imp);
    return types_as_pkg_name(obj);
}

/* ---------------------------------------------------------------- versions */

const Str burrow__tc_go1_9 = BURROW_S_INIT("go1.9");
const Str burrow__tc_go1_13 = BURROW_S_INIT("go1.13");
const Str burrow__tc_go1_14 = BURROW_S_INIT("go1.14");
const Str burrow__tc_go1_17 = BURROW_S_INIT("go1.17");
const Str burrow__tc_go1_18 = BURROW_S_INIT("go1.18");
const Str burrow__tc_go1_20 = BURROW_S_INIT("go1.20");
const Str burrow__tc_go1_21 = BURROW_S_INIT("go1.21");
const Str burrow__tc_go1_22 = BURROW_S_INIT("go1.22");
const Str burrow__tc_go1_23 = BURROW_S_INIT("go1.23");
const Str burrow__tc_go1_26 = BURROW_S_INIT("go1.26");
const Str burrow__tc_go1_27 = BURROW_S_INIT("go1.27");

/* The Go version this is a port of, which is what go_current is in Go. */
const Str burrow__tc_go_current = BURROW_S_INIT("go1.27");

Str burrow__tc_as_go_version(Alloc *a, Str v) {
    return version_lang(a, v);
}

bool burrow__tc_allow_version(TypesChecker *check, Str want) {
    return check->env.version.len == 0 ||
           version_compare(check->env.version, want) >= 0;
}

bool burrow__tc_verify_version(TypesChecker *check, GtPosn at, Str v, Str msg) {
    if (!burrow__tc_allow_version(check, v)) {
        burrow__tc_version_error(check, at, v, msg);
        return false;
    }
    return true;
}

/* ---------------------------------------------------------- environments */

TypesScope *burrow__tc_env_lookup_scope(GtEnv *env, Str name, TypesObject *obj) {
    for (TypesScope *s = env->scope; s != NULL; s = s->parent) {
        TypesObject o = types_scope_lookup(s, name);
        if (o != NULL &&
            (env->expr_pos == TOKEN_NO_POS || GT_OBJ(o)->scope_pos <= env->expr_pos)) {
            *obj = o;
            return s;
        }
    }
    *obj = NULL;
    return NULL;
}

TypesObject burrow__tc_env_lookup(GtEnv *env, Str name) {
    TypesObject obj;
    (void)burrow__tc_env_lookup_scope(env, name, &obj);
    return obj;
}

bool burrow__tc_decl_has_initializer(const GtDeclInfo *d) {
    return d->init != NULL || (d->fdecl != NULL && d->fdecl->body != NULL);
}

void burrow__tc_decl_add_dep(TypesChecker *check, GtDeclInfo *d, TypesObject obj) {
    if (d->deps == NULL)
        d->deps = tc_map(check->a, TYPE_UNSAFE_POINTER, TYPE_BOOL);
    bool yes = true;
    tc_set(d->deps, &obj, &yes);
}

/* ---------------------------------------------------------------- checker */

/* addDeclDep adds the dependency edge (check.decl -> to) if check.decl
 * exists. */
void burrow__tc_add_decl_dep(TypesChecker *check, TypesObject to) {
    GtDeclInfo *from = check->env.decl;
    if (from == NULL)
        return; /* not in a package-level init expression */
    if (map_get(check->obj_map, &to) == NULL)
        return; /* to is not a package-level object */
    burrow__tc_decl_add_dep(check, from, to);
}

void burrow__tc_remember_untyped(TypesChecker *check, AstExpr e, bool lhs, GtMode mode,
                                 TypesBasic *typ, ConstantValue val) {
    if (check->untyped == NULL)
        check->untyped = tc_map(check->a, TYPE_UNSAFE_POINTER, TYPE_UNSAFE_POINTER);
    GtExprInfo *info = GT_NEW(check->a, GtExprInfo);
    info->is_lhs = lhs;
    info->mode = mode;
    info->typ = typ;
    info->val = val;
    tc_set(check->untyped, &e, &info);
}

/* later pushes f on to the stack of actions that will be processed later;
 * either at the end of the current statement, or in case of a local constant
 * or variable declaration, before the constant or variable is in scope (so
 * that f still sees the scope before any new declarations). */
GtAction *burrow__tc_later(TypesChecker *check, GtActionFn f, void *env) {
    GtAction *act = GT_NEW(check->a, GtAction);
    act->version = check->env.version;
    act->f = f;
    act->env = env;
    burrow__types_ptrs_append(check->a, &check->delayed, act);
    return act;
}

/* push pushes obj onto the object path and records its index in the path
 * index map. */
void burrow__tc_push(TypesChecker *check, TypesObject obj) {
    if (check->obj_path_idx == NULL)
        check->obj_path_idx = tc_map(check->a, TYPE_UNSAFE_POINTER, TYPE_INT);
    Int n = check->obj_path.len;
    tc_set(check->obj_path_idx, &obj, &n);
    burrow__types_ptrs_append(check->a, &check->obj_path, obj);
}

/* pop pops an object from the object path and removes it from the path index
 * map. */
void burrow__tc_pop(TypesChecker *check) {
    Int i = check->obj_path.len - 1;
    TypesObject obj = GT_AT(check->obj_path, i, TypesObject);
    GT_AT(check->obj_path, i, TypesObject) = NULL;
    check->obj_path.len = i;
    map_del(check->obj_path_idx, &obj);
}

/* needsCleanup records the types that need a final cleanup at the end of type
 * checking: named types, aliases, interfaces and type parameters. */
void burrow__tc_needs_cleanup(TypesChecker *check, TypesType c) {
    burrow__types_ptrs_append(check->a, &check->cleaners, c);
}

TypesChecker *types_new_checker(Alloc *a, TypesConfig *conf, TokenFileSet *fset,
                                TypesPackage *pkg, TypesInfo *info) {
    TypesChecker *check = GT_NEW(a, TypesChecker);
    check->a = a;

    /* make sure we have a configuration */
    check->conf = conf != NULL ? conf : &check->conf0;

    /* make sure we have an info struct */
    check->info = info != NULL ? info : &check->info0;

    /* Note: clients may call NewChecker with the Unsafe package, which is
     * globally shared and must not be mutated. Therefore NewChecker must not
     * mutate *pkg. */

    check->ctxt = check->conf->context;
    check->fset = fset;
    check->pkg = pkg;
    check->obj_map = tc_map(a, TYPE_UNSAFE_POINTER, TYPE_UNSAFE_POINTER);
    check->imp_map = tc_map(a, TYPE_STRING, TYPE_UNSAFE_POINTER);
    check->used_vars = tc_map(a, TYPE_UNSAFE_POINTER, TYPE_BOOL);
    check->used_pkg_names = tc_map(a, TYPE_UNSAFE_POINTER, TYPE_BOOL);
    return check;
}

static Str tc_version_max(Str a, Str b) {
    if (version_compare(a, b) < 0)
        return b;
    return a;
}

/* initFiles initializes the files-specific portion of checker. The provided
 * files must all belong to the same package. */
static void tc_init_files(TypesChecker *check, Slice files) {
    /* start with a clean slate (check.Files may be called multiple times) */
    Slice nil = slice_nil(TYPE_UNSAFE_POINTER);
    check->files = nil;
    check->imports = nil;
    check->dot_import_map = NULL;

    check->has_first_err = false;
    check->methods = NULL;
    check->untyped = NULL;
    check->delayed = nil;
    check->obj_path = nil;
    check->obj_path_idx = NULL;
    check->cleaners = nil;
    check->bail_aliases = nil;

    /* We must initialize usedVars and usedPkgNames both here and in
     * NewChecker, because initFiles is not called in the CheckExpr or Eval
     * codepaths, yet we want to free this memory at the end of Files ('used'
     * predicates are only needed in the context of a given file). */
    check->used_vars = tc_map(check->a, TYPE_UNSAFE_POINTER, TYPE_BOOL);
    check->used_pkg_names = tc_map(check->a, TYPE_UNSAFE_POINTER, TYPE_BOOL);

    /* determine package name and collect valid files */
    TypesPackage *pkg = check->pkg;
    for (Int i = 0; i < files.len; i++) {
        AstFile *file = GT_AT(files, i, AstFile *);
        Str name = file->name->name;
        if (pkg->name.len == 0) {
            if (!str_eq(name, BURROW_S("_")))
                pkg->name = name;
            else
                burrow__tc_error(check, GT_AT_NODE(file->name), GT_CODE_BLANK_PKG_NAME,
                                 BURROW_S("invalid package name _"));
            burrow__types_ptrs_append(check->a, &check->files, file);
        } else if (str_eq(name, pkg->name)) {
            burrow__types_ptrs_append(check->a, &check->files, file);
        } else {
            GT_ERRORF(check, burrow__tc_at_pos(file->package),
                      GT_CODE_MISMATCHED_PKG_NAME, "package %s; expected package %s",
                      name, pkg->name);
            /* ignore this file */
        }
    }

    /* reuse Info.FileVersions if provided */
    Map *versions = check->info->file_versions;
    if (versions == NULL)
        versions = tc_map(check->a, TYPE_UNSAFE_POINTER, TYPE_STRING);
    check->versions = versions;

    Str pkg_version = burrow__tc_as_go_version(check->a, check->conf->go_version);
    if (pkg_version.len > 0 && files.len > 0 &&
        version_compare(pkg_version, burrow__tc_go_current) > 0) {
        AstFile *f0 = GT_AT(files, 0, AstFile *);
        GT_ERRORF(check, GT_AT_NODE(f0), GT_CODE_TOO_NEW,
                  "package requires newer Go version %v (application built with %v)",
                  pkg_version, burrow__tc_go_current);
    }

    /* determine Go version for each file */
    for (Int i = 0; i < check->files.len; i++) {
        AstFile *file = GT_AT(check->files, i, AstFile *);
        /* use unaltered Config.GoVersion by default (This version string may
         * contain dot-release numbers as in go1.20.1, unlike file versions
         * which are Go language versions only, if valid.) */
        Str v = check->conf->go_version;

        /* If the file specifies a version, use max(fileVersion, go1.21). */
        Str file_version = burrow__tc_as_go_version(check->a, file->go_version);
        if (file_version.len > 0) {
            /* Go 1.21 introduced the feature of setting the go.mod go line to
             * an early version of Go and allowing //go:build lines to set the
             * Go version in a given file. Versions Go 1.21 and later can be
             * set backwards compatibly as that was the first version files
             * with go1.21 or later build tags could be built with.
             *
             * Set the version to max(fileVersion, go1.21): That will allow a
             * downgrade to a version before go1.22, where the for loop
             * semantics change was made, while being backwards compatible with
             * versions of go before the new //go:build semantics were
             * introduced. */
            v = tc_version_max(file_version, burrow__tc_go1_21);

            /* Report a specific error for each tagged file that's too new.
             * (Normally the build system will have filtered files by version,
             * but clients can present arbitrary files to the type checker.) */
            if (version_compare(file_version, burrow__tc_go_current) > 0) {
                /* Use position of 'package [p]' for types/types2 consistency.
                 * (Ideally we would use the //build tag itself.) */
                GT_ERRORF(check, GT_AT_NODE(file->name), GT_CODE_TOO_NEW,
                          "file requires newer Go version %v (application built with "
                          "%v)",
                          file_version, burrow__tc_go_current);
            }
        }
        tc_set(versions, &file, &v);
    }
}

/* checkFiles type-checks the specified files. Errors are reported as a side
 * effect, not by returning early, to ensure that well-formed syntax is
 * properly type annotated even in a package containing errors. */
static void tc_check_files(TypesChecker *check, Slice files) {
    tc_init_files(check, files);
    burrow__tc_collect_objects(check);
    burrow__tc_sort_objects(check);
    burrow__tc_direct_cycles(check);
    burrow__tc_package_objects(check);
    burrow__tc_process_delayed(check, 0); /* incl. all functions */
    burrow__tc_cleanup(check);
    burrow__tc_init_order(check);

    if (!check->conf->disable_unused_import_check)
        burrow__tc_unused_imports(check);

    burrow__tc_record_untyped(check);

    if (!check->has_first_err) {
        /* TODO(mdempsky): Ensure monomorph is safe when errors exist. */
        burrow__tc_monomorph(check);
    }

    check->pkg->go_version = check->conf->go_version;
    check->pkg->complete = true;

    /* no longer needed - release memory */
    Slice nil = slice_nil(TYPE_UNSAFE_POINTER);
    check->imports = nil;
    check->dot_import_map = NULL;
    check->pkg_path_map = NULL;
    check->seen_pkg_map = NULL;
    check->broken_aliases = NULL;
    check->union_type_sets = NULL;
    check->used_vars = NULL;
    check->used_pkg_names = NULL;
    check->ctxt = NULL;
}

/* The part of Files that can bail out, kept small so that nothing the catch
 * block reads is a local of the frame the jump lands in. */
/* Not in Go: what the deferred function of typeDecl does for each alias
 * declaration a bailout leaves half done. */
void burrow__tc_bail_aliases(TypesChecker *check) {
    for (Int i = check->bail_aliases.len - 1; i >= 0; i--) {
        TypesAlias *alias = GT_AT(check->bail_aliases, i, TypesAlias *);
        if (alias->from_rhs == NULL) {
            alias->from_rhs = GT_TYP(TYPES_INVALID);
            (void)burrow__types_unalias(alias);
        }
    }
    check->bail_aliases.len = 0;
}

static void tc_files_guarded(TypesChecker *check, Slice files) {
    check->bailing = false;
    check->bail_aliases.len = 0;
    BURROW_TRY {
        tc_check_files(check, files);
    }
    BURROW_CATCH(r) {
        if (!check->bailing)
            panic(r);
        burrow__tc_bail_aliases(check);
    }
    BURROW_TRY_END;
    check->bailing = false;
}

bool types_checker_files(TypesChecker *check, Slice files, Error *err) {
    if (err != NULL)
        *err = BURROW_NO_ERROR;
    if (check->pkg == types_unsafe()) {
        /* Defensive handling for Unsafe, which cannot be type checked, and
         * must not be mutated. See https://go.dev/issue/61212 for an example of
         * where Unsafe is passed to NewChecker. */
        return true;
    }

    /* Avoid early returns here! Nearly all errors can be localized to a piece
     * of syntax and needn't prevent type-checking of the rest of the
     * package. */
    tc_files_guarded(check, files);
    if (!check->has_first_err)
        return true;
    if (err != NULL)
        *err = types_error_as_error(&check->first_err, check->a);
    return false;
}

TypesPackage *types_config_check(TypesConfig *conf, Alloc *a, Str path,
                                 TokenFileSet *fset, Slice files, TypesInfo *info,
                                 Error *err) {
    TypesPackage *pkg = types_new_package(a, path, BURROW_STR_EMPTY);
    if (pkg == NULL)
        burrow__types_oom();
    TypesChecker *check = types_new_checker(a, conf, fset, pkg, info);
    (void)types_checker_files(check, files, err);
    return pkg;
}

/* processDelayed processes all delayed actions pushed after top. */
void burrow__tc_process_delayed(TypesChecker *check, Int top) {
    /* If each delayed action pushes a new action, the stack will continue to
     * grow during this loop. However, it is only processing functions (which
     * are processed in a delayed fashion) that may add more actions (such as
     * nested functions), so this is a sufficiently bounded process. */
    Str saved_version = check->env.version;
    for (Int i = top; i < check->delayed.len; i++) {
        GtAction *act = GT_AT(check->delayed, i, GtAction *);
        check->env.version = act->version; /* reestablish the effective Go version */
        act->f(check, act->env);           /* may append to check->delayed */
    }
    burrow__types_assert(top <= check->delayed.len); /* stack must not have shrunk */
    check->delayed.len = top;
    check->env.version = saved_version;
}

/* cleanup runs cleanup for all collected cleaners. */
void burrow__tc_cleanup(TypesChecker *check) {
    /* Don't use a range clause since Named.cleanup may add more cleaners. */
    for (Int i = 0; i < check->cleaners.len; i++) {
        TypesType c = GT_AT(check->cleaners, i, TypesType);
        switch (c->kind) {
        case TYPES_KIND_NAMED:
            burrow__types_named_cleanup(GT_KNOWN(TypesNamed, c));
            break;
        case TYPES_KIND_ALIAS:
            (void)burrow__types_unalias(GT_KNOWN(TypesAlias, c));
            break;
        case TYPES_KIND_INTERFACE:
            burrow__types_interface_cleanup(GT_KNOWN(TypesInterface, c));
            break;
        case TYPES_KIND_TYPE_PARAM:
            burrow__types_type_param_cleanup(GT_KNOWN(TypesTypeParam, c));
            break;
        default:
            burrow__types_assert(false);
        }
    }
    check->cleaners = slice_nil(TYPE_UNSAFE_POINTER);
}

TypesContext *burrow__tc_context(TypesChecker *check) {
    if (check->ctxt == NULL)
        check->ctxt = types_new_context(check->a);
    return check->ctxt;
}

/* instantiatedIdent determines the identifier of the type instantiated in
 * expr. Helper function for recordInstance. */
AstIdent *burrow__tc_instantiated_ident(TypesChecker *check, AstExpr expr) {
    AstExpr sel_or_ident = NULL;
    switch (expr->kind) {
    case AST_KIND_INDEX_EXPR:
        sel_or_ident = ((AstIndexExpr *)(void *)expr)->x;
        break;
    case AST_KIND_INDEX_LIST_EXPR: /* only exists in go/ast, not syntax */
        sel_or_ident = ((AstIndexListExpr *)(void *)expr)->x;
        break;
    case AST_KIND_SELECTOR_EXPR:
    case AST_KIND_IDENT:
        sel_or_ident = expr;
        break;
    default:
        break;
    }
    if (sel_or_ident != NULL) {
        if (sel_or_ident->kind == AST_KIND_IDENT)
            return (AstIdent *)(void *)sel_or_ident;
        if (sel_or_ident->kind == AST_KIND_SELECTOR_EXPR)
            return ((AstSelectorExpr *)(void *)sel_or_ident)->sel;
    }

    /* extra debugging of go.dev/issue/63933 */
    Str es = burrow__tc_es(check, expr);
    panic_str(
        fmt_sprintf_v(check->a, "instantiated ident not found; please report: %s", es));
}

/* -------------------------------------------------------------- recording */

/* Not in Go: Go records a nil *Tuple for an expression with no value, which is
 * still a non-nil Type. A NULL TypesType can't say that, so this empty tuple
 * stands in. It prints as "()" and has length 0, like the nil *Tuple. */
static TypesTuple tc_novalue = {.hdr = {TYPES_KIND_TUPLE}};

void burrow__tc_record(TypesChecker *check, const GtOperand *x) {
    /* convert x into a user-friendly set of values */
    TypesType typ;
    ConstantValue val = {0};
    switch ((Int)x->mode) {
    case GT_MODE_INVALID:
        typ = GT_TYP(TYPES_INVALID);
        break;
    case GT_MODE_NOVALUE:
        typ = types_type(&tc_novalue);
        break;
    case GT_MODE_CONSTANT:
        typ = x->typ;
        val = x->val;
        break;
    default:
        typ = x->typ;
        break;
    }
    burrow__types_assert(x->expr != NULL && typ != NULL);

    if (!burrow__types_is_typed(typ)) {
        /* delay type and value recording until we know the type or until the
         * end of type checking */
        burrow__tc_remember_untyped(check, x->expr, false, x->mode,
                                    GT_KNOWN(TypesBasic, typ), val);
    } else {
        burrow__tc_record_type_and_value(check, x->expr, x->mode, typ, val);
    }
}

void burrow__tc_record_untyped(TypesChecker *check) {
    if (check->info->types == NULL || check->untyped == NULL)
        return; /* nothing to do */

    MapIter it = map_iter(check->untyped);
    const void *k;
    void *v;
    while (map_next(&it, &k, &v)) {
        AstExpr x = *(const AstExpr *)k;
        GtExprInfo *info = *(GtExprInfo **)v;
        burrow__tc_record_type_and_value(check, x, info->mode, types_type(info->typ),
                                         info->val);
    }
}

void burrow__tc_record_type_and_value(TypesChecker *check, AstExpr x, GtMode mode,
                                      TypesType typ, ConstantValue val) {
    burrow__types_assert(x != NULL);
    burrow__types_assert(typ != NULL);
    if (mode == GT_MODE_INVALID)
        return; /* omit */
    if (mode == GT_MODE_CONSTANT) {
        burrow__types_assert(!burrow__types_is_valid(typ) ||
                             burrow__types_all_basic(typ, TYPES_IS_CONST_TYPE));
    }
    Map *m = check->info->types;
    if (m != NULL) {
        TypesTypeAndValue tv = {(Int)mode, typ, val};
        tc_set(m, &x, &tv);
    }
}

void burrow__tc_record_builtin_type(TypesChecker *check, AstExpr f,
                                    TypesSignature *sig) {
    /* f must be a (possibly parenthesized, possibly qualified) identifier
     * denoting a built-in (built-ins don't have types). Record the signature
     * for f and possible children. */
    ConstantValue none = {0};
    for (;;) {
        burrow__tc_record_type_and_value(check, f, GT_MODE_BUILTIN, types_type(sig),
                                         none);
        switch (f->kind) {
        case AST_KIND_IDENT:
        case AST_KIND_SELECTOR_EXPR:
            return; /* we're done */
        case AST_KIND_PAREN_EXPR:
            f = ((AstParenExpr *)(void *)f)->x;
            break;
        default:
            burrow__types_assert(false);
        }
    }
}

/* recordCommaOkTypes updates recorded types to reflect that x is used in a
 * commaOk context (and therefore has tuple type). */
void burrow__tc_record_comma_ok_types(TypesChecker *check, AstExpr x, GtOperand *a0,
                                      GtOperand *a1) {
    burrow__types_assert(x != NULL);
    if (a0->mode == GT_MODE_INVALID)
        return;
    TypesType t0 = a0->typ, t1 = a1->typ;
    burrow__types_assert(burrow__types_is_typed(t0) && burrow__types_is_typed(t1) &&
                         (burrow__types_all_basic(t1, TYPES_IS_BOOLEAN) ||
                          t1 == burrow__types_universe_error()));
    Map *m = check->info->types;
    if (m != NULL) {
        for (;;) {
            TypesTypeAndValue *tv = map_get(m, &x);
            burrow__types_assert(tv != NULL && tv->type != NULL); /* recorded already */
            TokenPos pos = ast_expr_pos(x);
            Slice vars = burrow__types_ptrs_make(check->a, 2);
            GT_AT(vars, 0, TypesVar *) = burrow__types_new_var(
                check->a, TYPES_LOCAL_VAR, pos, check->pkg, BURROW_STR_EMPTY, t0);
            GT_AT(vars, 1, TypesVar *) = burrow__types_new_var(
                check->a, TYPES_LOCAL_VAR, pos, check->pkg, BURROW_STR_EMPTY, t1);
            tv->type = types_type(types_new_tuple(check->a, vars));
            if (x->kind != AST_KIND_PAREN_EXPR)
                break;
            x = ((AstParenExpr *)(void *)x)->x;
        }
    }
}

/* methodExprSentinel: what a method expression puts in recvold, to say the
 * receiver is params[0]. */
static TypesVar tc_method_expr_sentinel;
TypesVar *const burrow__tc_method_expr_sentinel = &tc_method_expr_sentinel;

/* recordInstance records instantiation information into check.Info, if the
 * Instances map is non-nil. The given expr must be an ident, selector, or
 * index (list) expr with ident or selector operand. */
void burrow__tc_record_instance(TypesChecker *check, AstExpr expr, Slice targs,
                                TypesType typ) {
    AstIdent *ident = burrow__tc_instantiated_ident(check, expr);
    burrow__types_assert(ident != NULL);
    burrow__types_assert(typ != NULL);
    Map *m = check->info->instances;
    if (m == NULL)
        return;
    TypesInstance inst = {burrow__types_new_type_list(check->a, targs), typ};
    TypesSignature *sig = types_as_signature(typ);
    if (sig != NULL && sig->recvold != NULL) {
        TypesSignature *copy = GT_NEW(check->a, TypesSignature);
        *copy = *sig;
        copy->recvold = NULL; /* not strictly necessary */
        if (sig->recvold == burrow__tc_method_expr_sentinel) {
            TypesTuple *pars = GT_NEW(check->a, TypesTuple);
            *pars = *copy->params;
            copy->recv = GT_AT(pars->vars, 0, TypesVar *);
            pars->vars = slice_sub(pars->vars, 1, pars->vars.len);
            copy->params = pars;
        } else {
            copy->recv = sig->recvold;
        }
        inst.type = types_type(copy);
    }
    tc_set(m, &ident, &inst);
}

void burrow__tc_record_def(TypesChecker *check, AstIdent *id, TypesObject obj) {
    burrow__types_assert(id != NULL);
    Map *m = check->info->defs;
    if (m != NULL)
        tc_set(m, &id, &obj);
}

void burrow__tc_record_use(TypesChecker *check, AstIdent *id, TypesObject obj) {
    burrow__types_assert(id != NULL);
    burrow__types_assert(obj != NULL);
    Map *m = check->info->uses;
    if (m != NULL)
        tc_set(m, &id, &obj);
}

void burrow__tc_record_implicit(TypesChecker *check, AstNode node, TypesObject obj) {
    burrow__types_assert(node != NULL);
    burrow__types_assert(obj != NULL);
    Map *m = check->info->implicits;
    if (m != NULL)
        tc_set(m, &node, &obj);
}

void burrow__tc_record_selection(TypesChecker *check, AstSelectorExpr *x,
                                 TypesSelectionKind kind, TypesType recv,
                                 TypesObject obj, Slice index, bool indirect) {
    burrow__types_assert(obj != NULL && (recv == NULL || index.len > 0));
    burrow__tc_record_use(check, x->sel, obj);
    Map *m = check->info->selections;
    if (m != NULL) {
        TypesSelection *sel =
            burrow__types_new_selection(check->a, kind, recv, obj, index, indirect);
        tc_set(m, &x, &sel);
    }
}

void burrow__tc_record_scope(TypesChecker *check, AstNode node, TypesScope *scope) {
    burrow__types_assert(node != NULL);
    burrow__types_assert(scope != NULL);
    Map *m = check->info->scopes;
    if (m != NULL)
        tc_set(m, &node, &scope);
}
