/* go/types: collecting the package's objects, and imports.
 *
 * Derived from Go's src/go/types/resolver.go. Go source: go1.27.1.
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
#include "burrow/slice.h"
#include "burrow/sort.h"
#include "burrow/strconv.h"
#include "burrow/strings.h"
#include "burrow/unicode.h"
#include "burrow/utf8.h"

#include "types_check.h"
#include "types_internal.h"

#include <stdbool.h>
#include <stdint.h>

static Str trs_sub(Str s, Int i, Int j) {
    if (i == j)
        return BURROW_STR_EMPTY; /* s.p can be NULL, and NULL + 0 is undefined */
    return str_from_bytes(s.p + i, j - i);
}

static void trs_set(Map *m, const void *key, const void *val) {
    if (!map_set(m, key, val))
        burrow__types_oom();
}

static Map *trs_map(Alloc *a, const Type *key, const Type *val) {
    Map *m = map_make(a, key, val, 0);
    if (m == NULL)
        burrow__types_oom();
    return m;
}

GtDeclInfo *burrow__tc_obj_info(TypesChecker *check, TypesObject obj) {
    GtDeclInfo **d = map_get(check->obj_map, &obj);
    return d != NULL ? *d : NULL;
}

static GtDeclInfo *trs_new_decl_info(TypesChecker *check, TypesScope *file) {
    GtDeclInfo *d = GT_NEW(check->a, GtDeclInfo);
    d->file = file;
    d->version = check->env.version;
    d->lhs = slice_nil(TYPE_UNSAFE_POINTER);
    return d;
}

/* arityMatch checks that the lhs and rhs of a const or var decl have the
 * appropriate number of names and init exprs. For const decls, init is the
 * value spec providing the init exprs; for var decls, init is nil (the init
 * exprs are in s in this case). */
void burrow__tc_arity_match(TypesChecker *check, AstValueSpec *s, AstValueSpec *init) {
    Int l = s->names.len;
    Int r = s->values.len;
    if (init != NULL)
        r = init->values.len;

    const GtCode code = GT_CODE_WRONG_ASSIGN_COUNT;
    if (init == NULL && r == 0) {
        /* var decl w/o init expr */
        if (s->type == NULL)
            burrow__tc_error(check, GT_AT_NODE(s), code,
                             BURROW_S("missing type or init expr"));
    } else if (l < r) {
        if (l < s->values.len) {
            /* init exprs from s */
            AstExpr n = GT_AT(s->values, l, AstExpr);
            GT_ERRORF(check, burrow__tc_at_node(n), code, "extra init expr %s",
                      burrow__tc_es(check, n));
            /* TODO(gri) avoid declared and not used error here */
        } else {
            /* init exprs "inherited" */
            TokenPosition p =
                token_file_set_position(check->fset, ast_node_pos(GT_NODE(init)));
            GT_ERRORF(check, GT_AT_NODE(s), code, "extra init expr at %s",
                      token_position_string(p, check->a));
            /* TODO(gri) avoid declared and not used error here */
        }
    } else if (l > r && (init != NULL || r != 1)) {
        AstIdent *n = GT_AT(s->names, r, AstIdent *);
        GT_ERRORF(check, GT_AT_NODE(n), code, "missing init expr for %s", n->name);
    }
}

/* validatedImportPath: the unquoted path, or false and *why. */
static bool trs_validated_import_path(TypesChecker *check, Str path, Str *s, Str *why) {
    Error err = BURROW_NO_ERROR;
    *s = strconv_unquote(check->a, path, &err);
    if (BURROW_FAILED(err)) {
        *why = error_text(err);
        return false;
    }
    if (s->len == 0) {
        *why = BURROW_S("empty string");
        return false;
    }
    static const Str illegal_chars =
        BURROW_S_INIT("!\"#$%&'()*,:;<=>?[\\]^{|}`\xEF\xBF\xBD");
    for (Int i = 0; i < s->len;) {
        Int size = 0;
        Rune r = utf8_decode_rune_in_string(trs_sub(*s, i, s->len), &size);
        if (!unicode_is_graphic(r) || unicode_is_space(r) ||
            strings_contains_rune(illegal_chars, r)) {
            *why = GT_SPRINTF(check, "invalid character %#U", r);
            return false;
        }
        i += size;
    }
    return true;
}

/* declarePkgObj declares obj in the package scope, records its ident -> obj
 * mapping, and updates check.objMap. The object must not be a function or
 * method. */
static void trs_declare_pkg_obj(TypesChecker *check, AstIdent *ident, TypesObject obj,
                                GtDeclInfo *d) {
    burrow__types_assert(str_eq(ident->name, GT_OBJ(obj)->name));

    /* spec: "A package-scope or file-scope identifier with name init may only
     * be declared to be a function with this (func()) signature." */
    if (str_eq(ident->name, BURROW_S("init"))) {
        burrow__tc_error(check, GT_AT_NODE(ident), GT_CODE_INVALID_INIT_DECL,
                         BURROW_S("cannot declare init - must be func"));
        return;
    }

    /* spec: "The main package must have package name main and declare a
     * function main that takes no arguments and returns no value." */
    if (str_eq(ident->name, BURROW_S("main")) &&
        str_eq(check->pkg->name, BURROW_S("main"))) {
        burrow__tc_error(check, GT_AT_NODE(ident), GT_CODE_INVALID_MAIN_DECL,
                         BURROW_S("cannot declare main - must be func"));
        return;
    }

    burrow__tc_declare(check, check->pkg->scope, ident, obj, TOKEN_NO_POS);
    trs_set(check->obj_map, &obj, &d);
    GT_OBJ(obj)->order = (uint32_t)map_len(check->obj_map);
}

/* filename returns a filename suitable for debugging output. */
static Str trs_filename(TypesChecker *check, Int file_no) {
    AstFile *file = GT_AT(check->files, file_no, AstFile *);
    TokenPos pos = ast_node_pos(GT_NODE(file));
    if (pos != TOKEN_NO_POS) {
        TokenFile *f = token_file_set_file(check->fset, pos);
        if (f != NULL)
            return token_file_name(f);
    }
    return GT_SPRINTF(check, "file[%d]", file_no);
}

/* A package name for a failed import, from its path (heuristic). */
static Str trs_fake_name(Str path) {
    Str name = path;
    if (name.len > 0 && name.p[name.len - 1] == '/')
        name = trs_sub(name, 0, name.len - 1);
    Int i = strings_last_index(name, BURROW_S("/"));
    if (i >= 0)
        name = trs_sub(name, i + 1, name.len);
    return name;
}

static TypesPackage *trs_import_package(TypesChecker *check, GtPosn at, Str path,
                                        Str dir) {
    /* If we already have a package for the given (path, dir) pair, use it
     * instead of doing a full import. Checker.impMap only caches packages
     * that are marked Complete or fake (dummy packages for failed imports).
     * Incomplete but non-fake packages do require an import to complete
     * them. */
    Str key = GT_SPRINTF(check, "%s\x00%s", path, dir);
    TypesPackage **cached = map_get(check->imp_map, &key);
    if (cached != NULL && *cached != NULL)
        return *cached;

    /* no package yet => import it */
    TypesPackage *imp = NULL;
    if (str_eq(path, BURROW_S("C")) &&
        (check->conf->fake_import_c || check->conf->go115_uses_cgo)) {
        if (check->conf->fake_import_c && check->conf->go115_uses_cgo)
            burrow__tc_error(
                check, at, GT_CODE_BAD_IMPORT_PATH,
                BURROW_S("cannot use FakeImportC and go115UsesCgo together"));
        /* TODO(gri) shouldn't create a new one each time */
        imp = types_new_package(check->a, BURROW_S("C"), BURROW_S("C"));
        imp->fake = true; /* package scope is not populated */
        imp->cgo = check->conf->go115_uses_cgo;
    } else {
        /* ordinary import */
        bool failed = false;
        Str why = BURROW_STR_EMPTY;
        const TypesImporterVT *vt = check->conf->importer.vt;
        void *self = check->conf->importer.data;
        Error err = BURROW_NO_ERROR;
        if (vt == NULL) {
            failed = true;
            why = BURROW_S("Config.Importer not installed");
        } else if (vt->import_from != NULL) {
            imp = vt->import_from(self, path, dir, 0, &err);
            if (BURROW_FAILED(err)) {
                failed = true;
                why = error_text(err);
            } else if (imp == NULL) {
                failed = true;
                why = GT_SPRINTF(
                    check,
                    "Config.Importer.ImportFrom(%s, %s, 0) returned nil but no error",
                    path, dir);
            }
        } else {
            imp = vt->import_(self, path, &err);
            if (BURROW_FAILED(err)) {
                failed = true;
                why = error_text(err);
            } else if (imp == NULL) {
                failed = true;
                why = GT_SPRINTF(check,
                                 "Config.Importer.Import(%s) returned nil but no error",
                                 path);
            }
        }
        /* make sure we have a valid package name (errors here can only happen
         * through manipulation of packages after creation) */
        if (!failed && imp != NULL &&
            (str_eq(imp->name, BURROW_S("_")) || imp->name.len == 0)) {
            failed = true;
            why = GT_SPRINTF(check, "invalid package name: %q", imp->name);
            imp = NULL; /* create fake package below */
        }
        if (failed) {
            GT_ERRORF(check, at, GT_CODE_BROKEN_IMPORT, "could not import %s (%s)",
                      path, why);
            if (imp == NULL) {
                /* create a new fake package; come up with a sensible package
                 * name (heuristic) */
                imp = types_new_package(check->a, path, trs_fake_name(path));
            }
            /* continue to use the package as best as we can */
            imp->fake = true; /* avoid follow-up lookup failures */
        }
    }

    /* package should be complete or marked fake, but be cautious */
    if (imp->complete || imp->fake) {
        trs_set(check->imp_map, &key, &imp);
        /* Once we've formatted an error message, keep the pkgPathMap
         * up-to-date on subsequent imports. It is used for package
         * qualification in error messages. */
        if (check->pkg_path_map != NULL)
            burrow__tc_mark_imports(check, imp);
        return imp;
    }

    /* something went wrong (importer may have returned incomplete package
     * without error) */
    return NULL;
}

/* dir makes a good-faith attempt to return the directory portion of path. If
 * path is empty, the result is ".". */
static Str trs_dir(Str path) {
    Int i = strings_last_index_any(path, BURROW_S("/\\"));
    if (i > 0)
        return trs_sub(path, 0, i);
    /* i <= 0 */
    return BURROW_S(".");
}

typedef struct TrsMethodInfo {
    TypesFunc *obj; /* method */
    bool ptr;       /* true if pointer receiver */
    AstIdent *recv; /* receiver type name */
} TrsMethodInfo;

/* What the walkDecls function of collectObjects closes over. */
typedef struct TrsCollect {
    TypesPackage *pkg;
    Map *pkg_imports; /* TypesPackage * to bool */
    Slice methods;    /* of TrsMethodInfo * */
    TypesScope *file_scope;
    Str file_dir;
} TrsCollect;

static void trs_collect_import(TypesChecker *check, TrsCollect *c,
                               AstImportSpec *spec) {
    TypesPackage *pkg = c->pkg;
    TypesScope *file_scope = c->file_scope;

    /* import package */
    if (spec->path->value.len == 0)
        return; /* error reported by parser */
    Str path, why;
    if (!trs_validated_import_path(check, spec->path->value, &path, &why)) {
        GT_ERRORF(check, GT_AT_NODE(spec->path), GT_CODE_BAD_IMPORT_PATH,
                  "invalid import path (%s)", why);
        return;
    }

    TypesPackage *imp =
        trs_import_package(check, GT_AT_NODE(spec->path), path, c->file_dir);
    if (imp == NULL)
        return;

    /* local name overrides imported package name */
    Str name = imp->name;
    if (spec->name != NULL) {
        name = spec->name->name;
        if (str_eq(path, BURROW_S("C"))) {
            /* match 1.17 cmd/compile (not prescribed by spec) */
            burrow__tc_error(check, GT_AT_NODE(spec->name), GT_CODE_IMPORT_C_RENAMED,
                             BURROW_S("cannot rename import \"C\""));
            return;
        }
    }

    if (str_eq(name, BURROW_S("init"))) {
        burrow__tc_error(
            check, GT_AT_NODE(spec), GT_CODE_INVALID_INIT_DECL,
            BURROW_S("cannot import package as init - init must be a func"));
        return;
    }

    /* add package to list of explicit imports (this functionality is
     * provided as a convenience for clients; it is not needed for
     * type-checking) */
    if (map_get(c->pkg_imports, &imp) == NULL) {
        bool yes = true;
        trs_set(c->pkg_imports, &imp, &yes);
        burrow__types_ptrs_append(check->a, &pkg->imports, imp);
    }

    TypesPkgName *pkg_name =
        types_new_pkg_name(check->a, ast_node_pos(GT_NODE(spec)), pkg, name, imp);
    if (spec->name != NULL) {
        /* in a dot-import, the dot represents the package */
        burrow__tc_record_def(check, spec->name, types_object(pkg_name));
    } else {
        burrow__tc_record_implicit(check, GT_NODE(spec), types_object(pkg_name));
    }

    if (imp->fake) {
        /* match 1.17 cmd/compile (not prescribed by spec) */
        bool yes = true;
        trs_set(check->used_pkg_names, &pkg_name, &yes);
    }

    /* add import to file scope */
    burrow__types_ptrs_append(check->a, &check->imports, pkg_name);
    if (str_eq(name, BURROW_S("."))) {
        /* dot-import */
        if (check->dot_import_map == NULL)
            check->dot_import_map =
                trs_map(check->a, TYPE_UNSAFE_POINTER, TYPE_UNSAFE_POINTER);
        Map **per_scope = map_get(check->dot_import_map, &file_scope);
        Map *names;
        if (per_scope == NULL) {
            names = trs_map(check->a, TYPE_STRING, TYPE_UNSAFE_POINTER);
            trs_set(check->dot_import_map, &file_scope, &names);
        } else {
            names = *per_scope;
        }
        /* merge imported scope with file scope */
        if (imp->scope->elems == NULL)
            return;
        MapIter it = map_iter(imp->scope->elems);
        const void *k;
        void *v;
        while (map_next(&it, &k, &v)) {
            Str oname = *(const Str *)k;
            TypesObject obj = *(TypesObject *)v;
            /* A package scope may contain non-exported objects, do not import
             * them! */
            if (!ast_is_exported(oname))
                continue;
            /* declare dot-imported object (Do not use check.declare because it
             * modifies the object via Object.setScopePos, which leads to a race
             * condition; the object may be imported into more than one file
             * scope concurrently. See go.dev/issue/32154.) */
            TypesObject alt = types_scope_lookup(file_scope, oname);
            if (alt != NULL) {
                GtError *err = burrow__tc_new_error(check, GT_CODE_DUPLICATE_DECL);
                GT_ADDF(err, GT_AT_NODE(spec->name), "%s redeclared in this block",
                        GT_OBJ(alt)->name);
                burrow__tc_error_add_alt_decl(err, alt);
                burrow__tc_error_report(err);
            } else {
                burrow__types_scope_insert_raw(file_scope, oname, obj);
                trs_set(names, &oname, &pkg_name);
            }
        }
    } else {
        /* declare imported package object in file scope (no need to provide
         * s.Name since we called check.recordDef earlier) */
        burrow__tc_declare(check, file_scope, NULL, types_object(pkg_name),
                           TOKEN_NO_POS);
    }
}

static void trs_collect_func(TypesChecker *check, TrsCollect *c, AstFuncDecl *fd) {
    TypesPackage *pkg = c->pkg;
    Str name = fd->name->name;
    /* signature set later */
    TypesFunc *obj = types_new_func(check->a, fd->name->name_pos, pkg, name, NULL);
    AstField *tparam0 = NULL;
    if (ast_field_list_num_fields(fd->type->type_params) > 0)
        tparam0 = GT_AT(fd->type->type_params->list, 0, AstField *);
    if (ast_field_list_num_fields(fd->recv) == 0) {
        /* regular function */
        if (fd->recv != NULL) {
            burrow__tc_error(check, GT_AT_NODE(fd->recv), GT_CODE_BAD_RECV,
                             BURROW_S("method has no receiver"));
            /* treat as function */
        }
        bool is_main = str_eq(name, BURROW_S("main")) &&
                       str_eq(check->pkg->name, BURROW_S("main"));
        if (str_eq(name, BURROW_S("init")) || is_main) {
            /* init and main functions must not declare type and ordinary
             * parameters or results */
            GtCode code = GT_CODE_INVALID_INIT_DECL;
            if (str_eq(name, BURROW_S("main")))
                code = GT_CODE_INVALID_MAIN_DECL;
            if (tparam0 != NULL)
                GT_SOFT_ERRORF(check, GT_AT_NODE(tparam0), code,
                               "func %s must have no type parameters", name);
            AstFuncType *t = fd->type;
            if (ast_field_list_num_fields(t->params) != 0 || t->results != NULL) {
                /* TODO(rFindley) Should this be a hard error? */
                GT_SOFT_ERRORF(check, GT_AT_NODE(fd->name), code,
                               "func %s must have no arguments and no return values",
                               name);
            }
        } else if (tparam0 != NULL) {
            (void)burrow__tc_verify_version(check, GT_AT_NODE(tparam0),
                                            burrow__tc_go1_18,
                                            BURROW_S("type parameter"));
        }
        if (str_eq(name, BURROW_S("init"))) {
            /* don't declare init functions in the package scope - they are
             * invisible */
            GT_OBJ(obj)->parent = pkg->scope;
            burrow__tc_record_def(check, fd->name, types_object(obj));
            if (fd->body == NULL)
                burrow__tc_soft_error(check, burrow__tc_at_obj(types_object(obj)),
                                      GT_CODE_MISSING_INIT_BODY,
                                      BURROW_S("func init must have a body"));
        } else {
            burrow__tc_declare(check, pkg->scope, fd->name, types_object(obj),
                               TOKEN_NO_POS);
        }
    } else {
        /* method */
        AstExpr base = NULL;
        Slice unused = slice_nil(TYPE_UNSAFE_POINTER);
        AstField *f0 = GT_AT(fd->recv->list, 0, AstField *);
        bool ptr = burrow__tc_unpack_recv(check, f0->type, false, &base, &unused);
        /* (Methods with invalid receiver cannot be associated to a type, and
         * methods with blank _ names are never found; no need to collect any
         * of them. They will still be type-checked with all the other
         * functions.) */
        if (base != NULL && base->kind == AST_KIND_IDENT &&
            !str_eq(name, BURROW_S("_"))) {
            TrsMethodInfo *m = GT_NEW(check->a, TrsMethodInfo);
            m->obj = obj;
            m->ptr = ptr;
            m->recv = (AstIdent *)(void *)base;
            burrow__types_ptrs_append(check->a, &c->methods, m);
        }
        if (tparam0 != NULL)
            (void)burrow__tc_verify_version(check, GT_AT_NODE(tparam0),
                                            burrow__tc_go1_27,
                                            BURROW_S("generic method"));
        burrow__tc_record_def(check, fd->name, types_object(obj));
    }
    GtDeclInfo *info = trs_new_decl_info(check, c->file_scope);
    info->fdecl = fd;
    /* Methods are not package-level objects but we still track them in the
     * object map so that we can handle them like regular functions (if the
     * receiver is invalid); also we need their fdecl info when associating
     * them with their receiver base type, below. */
    TypesObject o = types_object(obj);
    trs_set(check->obj_map, &o, &info);
    GT_OBJ(obj)->order = (uint32_t)map_len(check->obj_map);
}

static void trs_collect_decl(TypesChecker *check, void *env, const GtDecl *d) {
    TrsCollect *c = env;
    TypesPackage *pkg = c->pkg;
    switch (d->kind) {
    case GT_DECL_IMPORT:
        trs_collect_import(check, c, d->import_spec);
        break;
    case GT_DECL_CONST: {
        /* declare all constants */
        AstValueSpec *spec = d->value_spec;
        for (Int i = 0; i < spec->names.len; i++) {
            AstIdent *name = GT_AT(spec->names, i, AstIdent *);
            TypesConst *obj = types_new_const(check->a, name->name_pos, pkg, name->name,
                                              NULL, constant_make_int64(d->iota));

            AstExpr init = NULL;
            if (i < d->init.len)
                init = GT_AT(d->init, i, AstExpr);

            GtDeclInfo *di = trs_new_decl_info(check, c->file_scope);
            di->vtyp = d->typ;
            di->init = init;
            di->inherited = d->inherited;
            trs_declare_pkg_obj(check, name, types_object(obj), di);
        }
        break;
    }
    case GT_DECL_VAR: {
        AstValueSpec *spec = d->value_spec;
        Slice lhs = burrow__types_ptrs_make(check->a, spec->names.len);
        /* If there's exactly one rhs initializer, use the same declInfo d1 for
         * all lhs variables so that each lhs variable depends on the same rhs
         * initializer (n:1 var declaration). */
        GtDeclInfo *d1 = NULL;
        if (spec->values.len == 1) {
            /* The lhs elements are only set up after the for loop below, but
             * that's ok because declareVar only collects the declInfo for a
             * later phase. */
            d1 = trs_new_decl_info(check, c->file_scope);
            d1->lhs = lhs;
            d1->vtyp = spec->type;
            d1->init = GT_AT(spec->values, 0, AstExpr);
        }

        /* declare all variables */
        for (Int i = 0; i < spec->names.len; i++) {
            AstIdent *name = GT_AT(spec->names, i, AstIdent *);
            TypesVar *obj = burrow__types_new_var(
                check->a, TYPES_PACKAGE_VAR, name->name_pos, pkg, name->name, NULL);
            GT_AT(lhs, i, TypesVar *) = obj;

            GtDeclInfo *di = d1;
            if (di == NULL) {
                /* individual assignments */
                AstExpr init = NULL;
                if (i < spec->values.len)
                    init = GT_AT(spec->values, i, AstExpr);
                di = trs_new_decl_info(check, c->file_scope);
                di->vtyp = spec->type;
                di->init = init;
            }

            trs_declare_pkg_obj(check, name, types_object(obj), di);
        }
        break;
    }
    case GT_DECL_TYPE: {
        AstTypeSpec *spec = d->type_spec;
        TypesTypeName *obj = types_new_type_name(check->a, spec->name->name_pos, pkg,
                                                 spec->name->name, NULL);
        GtDeclInfo *di = trs_new_decl_info(check, c->file_scope);
        di->tdecl = spec;
        trs_declare_pkg_obj(check, spec->name, types_object(obj), di);
        break;
    }
    case GT_DECL_FUNC:
        trs_collect_func(check, c, d->func_decl);
        break;
    default:
        break;
    }
}

/* resolveBaseTypeName returns the non-alias base type name for the given
 * name, and whether there was a pointer indirection to get to it. The base
 * type name must be declared in package scope, and there can be at most one
 * pointer indirection. Traversals through generic alias types are not
 * permitted. If no such type name exists, the returned base is nil. */
static TypesTypeName *trs_resolve_base_type_name(TypesChecker *check, bool ptr,
                                                 AstIdent *name, bool *ptr_out) {
    /* Algorithm: Starting from name, which is expected to denote a type, we
     * follow that type through non-generic alias declarations until we reach
     * a non-alias type name. */
    Map *seen = NULL;
    while (name != NULL) {
        /* name must denote an object found in the current package scope (note
         * that dot-imported objects are not in the package scope!) */
        TypesObject obj = types_scope_lookup(check->pkg->scope, name->name);
        if (obj == NULL)
            break;

        /* the object must be a type name... */
        TypesTypeName *tname = types_as_type_name(obj);
        if (tname == NULL)
            break;

        /* ... which we have not seen before */
        if (seen != NULL && map_get(seen, &tname) != NULL)
            break;

        /* we're done if tdecl describes a defined type (not an alias) */
        GtDeclInfo *d = burrow__tc_obj_info(check, obj);
        burrow__types_assert(d != NULL && d->tdecl != NULL);
        AstTypeSpec *tdecl = d->tdecl;
        if (tdecl->assign == TOKEN_NO_POS) {
            *ptr_out = ptr;
            return tname;
        }

        /* an alias must not be generic (importantly, we must not collect such
         * methods - was https://go.dev/issue/70417) */
        if (tdecl->type_params != NULL)
            break;

        /* otherwise, remember this type name and continue resolving */
        if (seen == NULL)
            seen = trs_map(check->a, TYPE_UNSAFE_POINTER, TYPE_BOOL);
        bool yes = true;
        trs_set(seen, &tname, &yes);

        /* The go/parser keeps parentheses; strip them, if any. */
        AstExpr typ = ast_unparen(tdecl->type);

        /* dereference a pointer type */
        if (typ->kind == AST_KIND_STAR_EXPR) {
            /* if we've already seen a pointer, we're done */
            if (ptr)
                break;
            ptr = true;
            /* continue with pointer base type */
            typ = ast_unparen(((AstStarExpr *)(void *)typ)->x);
        }

        /* After dereferencing, typ must be a locally defined type name.
         * Referring to other packages (qualified identifiers) or going through
         * instantiated types (index expressions) is not permitted, so we can
         * ignore those. */
        name = typ->kind == AST_KIND_IDENT ? (AstIdent *)(void *)typ : NULL;
    }

    /* no base type found */
    *ptr_out = false;
    return NULL;
}

/* collectObjects collects all file and package objects and inserts them into
 * their respective scopes. It also performs imports and associates methods
 * with receiver base type names. */
void burrow__tc_collect_objects(TypesChecker *check) {
    TypesPackage *pkg = check->pkg;

    /* pkgImports is the set of packages already imported by any package file
     * seen so far. Used to avoid duplicate entries in pkg.imports. Allocate
     * and populate it (pkg.imports may not be empty if we are checking test
     * files incrementally). Note that pkgImports is keyed by package (and
     * thus package path), not by an importKey value. Two different importKey
     * values may map to the same package which is why we cannot use the
     * check.impMap here. */
    TrsCollect c = {0};
    c.pkg = pkg;
    c.pkg_imports = trs_map(check->a, TYPE_UNSAFE_POINTER, TYPE_BOOL);
    bool yes = true;
    for (Int i = 0; i < pkg->imports.len; i++) {
        TypesPackage *imp = GT_AT(pkg->imports, i, TypesPackage *);
        trs_set(c.pkg_imports, &imp, &yes);
    }
    /* collected methods with valid receivers and non-blank _ names */
    c.methods = slice_nil(TYPE_UNSAFE_POINTER);

    /* fileScopes[i] corresponds to check.files[i] */
    Slice file_scopes = burrow__types_ptrs_make(check->a, check->files.len);
    for (Int file_no = 0; file_no < check->files.len; file_no++) {
        AstFile *file = GT_AT(check->files, file_no, AstFile *);
        Str *fv = map_get(check->versions, &file);
        check->env.version =
            burrow__tc_as_go_version(check->a, fv != NULL ? *fv : BURROW_STR_EMPTY);

        /* The package identifier denotes the current package, but there is no
         * corresponding package object. */
        burrow__tc_record_def(check, file->name, NULL);

        /* Use the actual source file extent rather than *ast.File extent since
         * the latter doesn't include comments which appear at the start or
         * end of the file. Be conservative and use the *ast.File extent if we
         * don't have a *token.File. */
        TokenPos pos = ast_node_pos(GT_NODE(file)), end = ast_node_end(GT_NODE(file));
        TokenFile *f = token_file_set_file(check->fset, pos);
        if (f != NULL) {
            pos = (TokenPos)token_file_base(f);
            end = token_file_end(f);
        }
        TypesScope *file_scope = types_new_scope(check->a, pkg->scope, pos, end,
                                                 trs_filename(check, file_no));
        GT_AT(file_scopes, file_no, TypesScope *) = file_scope;
        burrow__tc_record_scope(check, GT_NODE(file), file_scope);

        /* determine file directory, necessary to resolve imports FileName may
         * be "" (typically for tests) in which case we get "." as the
         * directory which is what we would want. */
        TokenPosition p = token_file_set_position(check->fset, file->name->name_pos);
        c.file_scope = file_scope;
        c.file_dir = trs_dir(p.filename);

        burrow__tc_walk_decls(check, file->decls, trs_collect_decl, &c);
    }

    /* verify that objects in package and file scopes have different names */
    for (Int i = 0; i < file_scopes.len; i++) {
        TypesScope *scope = GT_AT(file_scopes, i, TypesScope *);
        if (scope->elems == NULL)
            continue;
        MapIter it = map_iter(scope->elems);
        const void *k;
        void *v;
        while (map_next(&it, &k, &v)) {
            Str name = *(const Str *)k;
            TypesObject obj = *(TypesObject *)v;
            TypesObject alt = types_scope_lookup(pkg->scope, name);
            if (alt == NULL)
                continue;
            GtError *err = burrow__tc_new_error(check, GT_CODE_DUPLICATE_DECL);
            TypesPkgName *pn = types_as_pkg_name(obj);
            if (pn != NULL) {
                GT_ADDF(err, burrow__tc_at_obj(alt),
                        "%s already declared through import of %s", GT_OBJ(alt)->name,
                        types_package_string(check->a, pn->imported));
                burrow__tc_error_add_alt_decl(err, obj);
            } else {
                GT_ADDF(err, burrow__tc_at_obj(alt),
                        "%s already declared through dot-import of %s",
                        GT_OBJ(alt)->name,
                        types_package_string(check->a, GT_OBJ(obj)->pkg));
                /* TODO(gri) dot-imported objects don't have a position;
                 * addAltDecl won't print anything */
                burrow__tc_error_add_alt_decl(err, obj);
            }
            burrow__tc_error_report(err);
        }
    }

    /* Now that we have all package scope objects and all methods, associate
     * methods with receiver base type name where possible. Ignore methods that
     * have an invalid receiver. They will be type-checked later, with regular
     * functions. */
    if (c.methods.len == 0)
        return;

    check->methods = trs_map(check->a, TYPE_UNSAFE_POINTER, TYPE_UNSAFE_POINTER);
    for (Int i = 0; i < c.methods.len; i++) {
        TrsMethodInfo *m = GT_AT(c.methods, i, TrsMethodInfo *);
        /* Determine the receiver base type and associate m with it. */
        bool ptr = false;
        TypesTypeName *base = trs_resolve_base_type_name(check, m->ptr, m->recv, &ptr);
        if (base != NULL) {
            m->obj->has_ptr_recv = ptr;
            Slice **list = map_get(check->methods, &base);
            Slice *l;
            if (list == NULL) {
                l = GT_NEW(check->a, Slice);
                *l = slice_nil(TYPE_UNSAFE_POINTER);
                trs_set(check->methods, &base, &l);
            } else {
                l = *list;
            }
            burrow__types_ptrs_append(check->a, l, m->obj);
        }
    }
}

/* sortObjects sorts package-level objects by source-order for reproducible
 * processing */
void burrow__tc_sort_objects(TypesChecker *check) {
    /* Not in Go: the orders are 1 to len(objMap), each set once as the object
     * went into the map, so every object goes straight into its slot. */
    Int n = map_len(check->obj_map);
    check->obj_list = burrow__types_ptrs_make(check->a, n);
    MapIter it = map_iter(check->obj_map);
    const void *k;
    void *v;
    while (map_next(&it, &k, &v)) {
        TypesObject obj = *(TypesObject const *)k;
        uint32_t order = GT_OBJ(obj)->order;
        burrow__types_assert(order >= 1 && (Int)order <= n);
        burrow__types_assert(GT_AT(check->obj_list, order - 1, TypesObject) == NULL);
        GT_AT(check->obj_list, order - 1, TypesObject) = obj;
    }
}

/* unpackRecv unpacks a receiver type expression and returns its components:
 * ptr indicates whether rtyp is a pointer receiver, base is the receiver base
 * type expression stripped of its type parameters (if any), and tparams are
 * its type parameter names, if any. The type parameters are only unpacked if
 * unpackParams is set. For instance, given the rtyp
 *
 *	*T[A, _]
 *
 * ptr is true, base is T, and tparams is [A, _] (assuming unpackParams is
 * set). Note that base may not be a *ast.Ident for erroneous programs. */
bool burrow__tc_unpack_recv(TypesChecker *check, AstExpr rtyp, bool unpack_params,
                            AstExpr *base_out, Slice *tparams) {
    bool ptr = false;
    *tparams = slice_nil(TYPE_UNSAFE_POINTER);

    /* unpack receiver type */
    AstExpr base = ast_unparen(rtyp);
    if (base->kind == AST_KIND_STAR_EXPR) {
        ptr = true;
        base = ast_unparen(((AstStarExpr *)(void *)base)->x);
    }

    /* unpack type parameters, if any */
    GtIndexedExpr ix;
    if (burrow__tc_unpack_indexed_expr(check->a, base, &ix)) {
        base = ix.x;
        if (unpack_params) {
            for (Int i = 0; i < ix.indices.len; i++) {
                AstExpr arg = GT_AT(ix.indices, i, AstExpr);
                AstIdent *par = NULL;
                if (arg == NULL) {
                    burrow__tc_error(
                        check, burrow__tc_at_node(ix.orig), GT_CODE_INVALID_SYNTAX_TREE,
                        BURROW_S("parameterized receiver contains nil parameters"));
                } else if (arg->kind == AST_KIND_IDENT) {
                    par = (AstIdent *)(void *)arg;
                } else if (arg->kind == AST_KIND_BAD_EXPR) {
                    /* ignore - error already reported by parser */
                } else {
                    GT_ERRORF(check, burrow__tc_at_node(arg), GT_CODE_BAD_DECL,
                              "receiver type parameter %s must be an identifier",
                              burrow__tc_es(check, arg));
                }
                if (par == NULL) {
                    par = ast_new_ident(check->a, BURROW_S("_"));
                    if (par == NULL)
                        burrow__types_oom();
                    par->name_pos = arg != NULL ? ast_expr_pos(arg) : TOKEN_NO_POS;
                }
                burrow__types_ptrs_append(check->a, tparams, par);
            }
        }
    }

    *base_out = base;
    return ptr;
}

/* packageObjects typechecks all package objects, but not function bodies. */
void burrow__tc_package_objects(TypesChecker *check) {
    /* add new methods to already type-checked types (from a prior
     * Checker.Files call) */
    for (Int i = 0; i < check->obj_list.len; i++) {
        TypesObject obj = GT_AT(check->obj_list, i, TypesObject);
        TypesTypeName *tn = types_as_type_name(obj);
        if (tn != NULL && GT_OBJ(tn)->typ != NULL)
            burrow__tc_collect_methods(check, tn);
    }

    /* To avoid problems with cycles, we process non-alias type declarations
     * first, followed by alias declarations, and then everything else. This
     * appears to avoid most situations where the type of an alias is needed
     * before it is available. There may still be cases where this is not
     * good enough (see also go.dev/issue/25838). In those cases Checker.ident
     * will report an error ("invalid use of type alias"). */
    Slice alias_list = slice_nil(TYPE_UNSAFE_POINTER);
    Slice others_list =
        slice_nil(TYPE_UNSAFE_POINTER); /* everything that's not a type */
    /* phase 1: non-alias type declarations */
    for (Int i = 0; i < check->obj_list.len; i++) {
        TypesObject obj = GT_AT(check->obj_list, i, TypesObject);
        if (obj->kind == TYPES_OBJECT_TYPE_NAME) {
            GtDeclInfo *d = burrow__tc_obj_info(check, obj);
            burrow__types_assert(d != NULL && d->tdecl != NULL);
            if (d->tdecl->assign != TOKEN_NO_POS)
                burrow__types_ptrs_append(check->a, &alias_list, obj);
            else
                burrow__tc_obj_decl(check, obj);
        } else {
            burrow__types_ptrs_append(check->a, &others_list, obj);
        }
    }
    /* phase 2: alias type declarations */
    for (Int i = 0; i < alias_list.len; i++)
        burrow__tc_obj_decl(check, GT_AT(alias_list, i, TypesObject));
    /* phase 3: all other declarations */
    for (Int i = 0; i < others_list.len; i++)
        burrow__tc_obj_decl(check, GT_AT(others_list, i, TypesObject));

    /* At this point we may have a non-empty check.methods map; this means
     * that not all entries were deleted at the end of typeDecl because the
     * respective receiver base types were not found. In that case, an error
     * was reported when declaring those methods. We can now safely discard
     * this map. */
    check->methods = NULL;
}

static void trs_error_unused_pkg(TypesChecker *check, TypesPkgName *obj) {
    /* If the package was imported with a name other than the final import
     * path element, show it explicitly in the error message. Note that this
     * handles both renamed imports and imports of packages containing
     * unconventional package declarations. Note that this uses / always, even
     * on Windows, because Go import paths always use forward slashes. */
    Str path = obj->imported->path;
    Str elem = path;
    Int i = strings_last_index(elem, BURROW_S("/"));
    if (i >= 0)
        elem = trs_sub(elem, i + 1, elem.len);
    Str name = GT_OBJ(obj)->name;
    GtPosn at = burrow__tc_at_obj(types_object(obj));
    if (name.len == 0 || str_eq(name, BURROW_S(".")) || str_eq(name, elem))
        GT_SOFT_ERRORF(check, at, GT_CODE_UNUSED_IMPORT, "%q imported and not used",
                       path);
    else
        GT_SOFT_ERRORF(check, at, GT_CODE_UNUSED_IMPORT,
                       "%q imported as %s and not used", path, name);
}

/* unusedImports checks for unused imports. */
void burrow__tc_unused_imports(TypesChecker *check) {
    /* If function bodies are not checked, packages' uses are likely missing -
     * don't check. */
    if (check->conf->ignore_func_bodies)
        return;

    /* spec: "It is illegal (...) to directly import a package without
     * referring to any of its exported identifiers. To import a package
     * solely for its side-effects (initialization), use the blank identifier
     * as explicit package name." */
    for (Int i = 0; i < check->imports.len; i++) {
        TypesPkgName *obj = GT_AT(check->imports, i, TypesPkgName *);
        if (!str_eq(GT_OBJ(obj)->name, BURROW_S("_")) &&
            map_get(check->used_pkg_names, &obj) == NULL)
            trs_error_unused_pkg(check, obj);
    }
}
