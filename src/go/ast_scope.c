/* go/ast: scopes, objects and the package resolver.
 *
 * The parser's old identifier resolution, which Go keeps for compatibility
 * and tells everyone to replace with go/types. An object's declaration is an
 * Any holding a pointer to the node, as Go's is an interface holding one, so
 * Object.Pos is a switch on the descriptor where Go has a type switch.
 *
 * Derived from Go's src/go/ast/scope.go and resolve.go.
 * Go source: go1.27.1.
 *
 * Copyright 2009 The Go Authors. All rights reserved.
 * Copyright 2026 The burrow Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style licence that can be found
 * in the LICENSE file. */

#include "burrow/go/ast.h"

#include "burrow/core.h"
#include "burrow/error.h"
#include "burrow/fmt.h"
#include "burrow/go/scanner.h"
#include "burrow/go/token.h"
#include "burrow/iface.h"
#include "burrow/map.h"
#include "burrow/mem.h"
#include "burrow/panic.h"
#include "burrow/slice.h"
#include "burrow/strconv.h"
#include "burrow/strings.h"
#include "burrow/type.h"

#include <stdbool.h>
#include <stdint.h>

BURROW_NORETURN static void as_oom(void) {
    panic_str(BURROW_S("go/ast: out of memory"));
}

/* ------------------------------------------------------------------ scopes */

AstScope *ast_new_scope(Alloc *a, AstScope *outer) {
    AstScope *s = (AstScope *)mem_alloc(a, sizeof(AstScope), _Alignof(AstScope));
    if (s == NULL) {
        return NULL;
    }
    s->outer = outer;
    s->objects = map_make(a, TYPE_OF(Str), TYPE_AST_OBJECT_PTR, 4);
    if (s->objects == NULL) {
        mem_free(a, s, sizeof(AstScope), _Alignof(AstScope));
        return NULL;
    }
    return s;
}

AstObject *ast_scope_lookup(AstScope *s, Str name) {
    AstObject **obj = (AstObject **)map_get(s->objects, &name);
    return obj == NULL ? NULL : *obj;
}

AstObject *ast_scope_insert(AstScope *s, AstObject *obj) {
    AstObject *alt = ast_scope_lookup(s, obj->name);
    if (alt == NULL && !map_set(s->objects, &obj->name, &obj)) {
        as_oom();
    }
    return alt;
}

Str ast_scope_string(AstScope *s, Alloc *a) {
    ArenaMark m = error_mark();
    Alloc *ea = error_allocator();
    StringsBuilder buf = STRINGS_BUILDER(a);
    Error err = BURROW_NO_ERROR;
    /* A local, so MSVC has no compound literal to copy for each _Generic. */
    Any sv = BURROW_ANY(TYPE_OF(AstScopePtr), &s);
    strings_builder_write_string(&buf, fmt_sprintf_v(ea, "scope %p {", sv), &err);
    if (s != NULL && map_len(s->objects) > 0) {
        strings_builder_write_string(&buf, BURROW_S("\n"), &err);
        MapIter it = map_iter(s->objects);
        const void *k = NULL;
        void *v = NULL;
        while (map_next(&it, &k, &v)) {
            AstObject *obj = *(AstObject **)v;
            strings_builder_write_string(&buf,
                                         fmt_sprintf_v(ea, "\t%s %s\n",
                                                       ast_obj_kind_string(obj->kind),
                                                       obj->name),
                                         &err);
        }
    }
    strings_builder_write_string(&buf, BURROW_S("}\n"), &err);
    error_release(m);
    if (!BURROW_OK(err)) {
        as_oom();
    }
    return strings_builder_string(&buf);
}

/* ----------------------------------------------------------------- objects */

static const Str as_obj_kind_strings[] = {
    BURROW_S_INIT("bad"),   BURROW_S_INIT("package"), BURROW_S_INIT("const"),
    BURROW_S_INIT("type"),  BURROW_S_INIT("var"),     BURROW_S_INIT("func"),
    BURROW_S_INIT("label"),
};

Str ast_obj_kind_string(AstObjKind kind) {
    if (kind < 0 || kind > AST_LBL) {
        panic_str(fmt_sprintf_v(error_allocator(),
                                "runtime error: index out of range [%d] with length 7",
                                kind));
    }
    return as_obj_kind_strings[kind];
}

AstObject *ast_new_obj(Alloc *a, AstObjKind kind, Str name) {
    AstObject *obj = (AstObject *)mem_alloc(a, sizeof(AstObject), _Alignof(AstObject));
    if (obj == NULL) {
        return NULL;
    }
    obj->kind = kind;
    obj->name = name;
    return obj;
}

static TokenPos as_ident_list_pos(Slice names, Str name) {
    for (Int i = 0; i < names.len; i++) {
        AstIdent *n = BURROW_AT(AstIdent *, names, i);
        if (str_eq(n->name, name)) {
            return ast_ident_pos(n);
        }
    }
    return TOKEN_NO_POS;
}

/* The declaration as the node pointer it holds, or NULL when it holds
 * something else, such as the scope of a predeclared object. */
static void *as_decl_node(Any decl, const Type *t) {
    if (decl.t != t || decl.data == NULL) {
        return NULL;
    }
    return *(void **)decl.data;
}

TokenPos ast_object_pos(AstObject *obj) {
    Str name = obj->name;
    AstField *field = (AstField *)as_decl_node(obj->decl, TYPE_OF(AstFieldPtr));
    if (field != NULL) {
        return as_ident_list_pos(field->names, name);
    }
    AstImportSpec *imp =
        (AstImportSpec *)as_decl_node(obj->decl, TYPE_OF(AstImportSpecPtr));
    if (imp != NULL) {
        if (imp->name != NULL && str_eq(imp->name->name, name)) {
            return ast_ident_pos(imp->name);
        }
        return ast_basic_lit_pos(imp->path);
    }
    AstValueSpec *vs =
        (AstValueSpec *)as_decl_node(obj->decl, TYPE_OF(AstValueSpecPtr));
    if (vs != NULL) {
        return as_ident_list_pos(vs->names, name);
    }
    AstTypeSpec *ts = (AstTypeSpec *)as_decl_node(obj->decl, TYPE_OF(AstTypeSpecPtr));
    if (ts != NULL) {
        return str_eq(ts->name->name, name) ? ast_ident_pos(ts->name) : TOKEN_NO_POS;
    }
    AstFuncDecl *fd = (AstFuncDecl *)as_decl_node(obj->decl, TYPE_OF(AstFuncDeclPtr));
    if (fd != NULL) {
        return str_eq(fd->name->name, name) ? ast_ident_pos(fd->name) : TOKEN_NO_POS;
    }
    AstLabeledStmt *ls =
        (AstLabeledStmt *)as_decl_node(obj->decl, TYPE_OF(AstLabeledStmtPtr));
    if (ls != NULL) {
        return str_eq(ls->label->name, name) ? ast_ident_pos(ls->label) : TOKEN_NO_POS;
    }
    AstAssignStmt *as =
        (AstAssignStmt *)as_decl_node(obj->decl, TYPE_OF(AstAssignStmtPtr));
    if (as != NULL) {
        for (Int i = 0; i < as->lhs.len; i++) {
            AstExpr x = BURROW_AT(AstExpr, as->lhs, i);
            if (x != NULL && x->kind == AST_KIND_IDENT &&
                str_eq(((AstIdent *)x)->name, name)) {
                return ast_ident_pos((AstIdent *)x);
            }
        }
    }
    /* an AstScope is a predeclared object, with nothing to do for now */
    return TOKEN_NO_POS;
}

/* ---------------------------------------------------------------- packages */

/* pkgBuilder in Go. */
typedef struct AsBuilder {
    Alloc *a;
    TokenFileSet *fset;
    GoScannerErrorList errors;
} AsBuilder;

static void as_error(AsBuilder *p, TokenPos pos, Str msg) {
    go_scanner_error_list_add(&p->errors, p->a, token_file_set_position(p->fset, pos),
                              msg);
}

static void as_declare(AsBuilder *p, AstScope *scope, AstScope *alt_scope,
                       AstObject *obj) {
    AstObject *alt = ast_scope_insert(scope, obj);
    if (alt == NULL && alt_scope != NULL) {
        /* see if there is a conflicting declaration in alt_scope */
        alt = ast_scope_lookup(alt_scope, obj->name);
    }
    if (alt == NULL) {
        return;
    }
    ArenaMark m = error_mark();
    Alloc *ea = error_allocator();
    Str prev_decl = BURROW_STR_EMPTY;
    TokenPos pos = ast_object_pos(alt);
    if (token_pos_is_valid(pos)) {
        prev_decl = fmt_sprintf_v(
            ea, "\n\tprevious declaration at %s",
            token_position_string(token_file_set_position(p->fset, pos), ea));
    }
    as_error(p, ast_object_pos(obj),
             fmt_sprintf_v(ea, "%s redeclared in this block%s", obj->name, prev_decl));
    error_release(m);
}

static bool as_resolve(AstScope *scope, AstIdent *ident) {
    for (; scope != NULL; scope = scope->outer) {
        AstObject *obj = ast_scope_lookup(scope, ident->name);
        if (obj != NULL) {
            ident->obj = obj;
            return true;
        }
    }
    return false;
}

static void as_import(AsBuilder *p, Map *imports, AstImporter importer,
                      AstScope *pkg_scope, AstScope *file_scope, AstImportSpec *spec,
                      bool *import_errors) {
    Error uerr = BURROW_NO_ERROR;
    Str path = strconv_unquote(p->a, spec->path->value, &uerr);
    (void)uerr;
    Error err = BURROW_NO_ERROR;
    AstObject *pkg = importer.f(importer.env, imports, path, &err);
    if (!BURROW_OK(err)) {
        ArenaMark m = error_mark();
        as_error(p, ast_basic_lit_pos(spec->path),
                 fmt_sprintf_v(error_allocator(), "could not import %s (%s)", path,
                               error_text(err)));
        error_release(m);
        *import_errors = true;
        return;
    }
    /* a local name overrides the imported package name */
    Str name = pkg->name;
    if (spec->name != NULL) {
        name = spec->name->name;
    }
    if (str_eq(name, BURROW_S("."))) {
        /* merge the imported scope with the file scope */
        AstScope *scope = (AstScope *)as_decl_node(pkg->data, TYPE_OF(AstScopePtr));
        if (scope == NULL) {
            return;
        }
        MapIter it = map_iter(scope->objects);
        const void *k = NULL;
        void *v = NULL;
        while (map_next(&it, &k, &v)) {
            as_declare(p, file_scope, pkg_scope, *(AstObject **)v);
        }
    } else if (!str_eq(name, BURROW_S("_"))) {
        /* Declare the imported package object in the file scope. A new object
         * rather than pkg, since the declaration differs from file to file. */
        AstObject *obj = ast_new_obj(p->a, AST_PKG, name);
        if (obj == NULL) {
            as_oom();
        }
        obj->decl = any_box(p->a, BURROW_ANY(TYPE_AST_IMPORT_SPEC_PTR, &spec));
        if (obj->decl.t == NULL) {
            as_oom();
        }
        obj->data = pkg->data;
        as_declare(p, file_scope, pkg_scope, obj);
    }
}

static void as_resolve_file(AsBuilder *p, AstFile *file, Map *imports,
                            AstImporter importer, AstScope *pkg_scope,
                            AstScope *universe) {
    /* build the file scope by processing all the imports */
    bool import_errors = false;
    AstScope *file_scope = ast_new_scope(p->a, pkg_scope);
    if (file_scope == NULL) {
        as_oom();
    }
    for (Int i = 0; i < file->imports.len; i++) {
        AstImportSpec *spec = BURROW_AT(AstImportSpec *, file->imports, i);
        if (importer.f == NULL) {
            import_errors = true;
            continue;
        }
        as_import(p, imports, importer, pkg_scope, file_scope, spec, &import_errors);
    }

    /* Resolve the identifiers. Without correct imports the universe is left
     * out, since objects in it may be shadowed by the imports and an
     * identifier could resolve to the wrong one. */
    if (import_errors) {
        pkg_scope->outer = NULL;
    }
    Int n = 0;
    for (Int i = 0; i < file->unresolved.len; i++) {
        AstIdent *ident = BURROW_AT(AstIdent *, file->unresolved, i);
        if (!as_resolve(file_scope, ident)) {
            ArenaMark m = error_mark();
            as_error(
                p, ast_ident_pos(ident),
                fmt_sprintf_v(error_allocator(), "undeclared name: %s", ident->name));
            error_release(m);
            BURROW_AT(AstIdent *, file->unresolved, n) = ident;
            n++;
        }
    }
    file->unresolved.len = n;
    pkg_scope->outer = universe; /* put the universe back */
}

AstPackage *ast_new_package(Alloc *a, TokenFileSet *fset, Map *files,
                            AstImporter importer, AstScope *universe, Error *err) {
    AsBuilder p = {a, fset, {NULL, 0, 0, NULL}};
    *err = BURROW_NO_ERROR;

    /* complete the package scope */
    Str pkg_name = BURROW_STR_EMPTY;
    AstScope *pkg_scope = ast_new_scope(a, universe);
    if (pkg_scope == NULL) {
        as_oom();
    }
    MapIter it = map_iter(files);
    const void *k = NULL;
    void *v = NULL;
    while (map_next(&it, &k, &v)) {
        AstFile *file = *(AstFile **)v;
        /* package names must match */
        Str name = file->name->name;
        if (pkg_name.len == 0) {
            pkg_name = name;
        } else if (!str_eq(name, pkg_name)) {
            ArenaMark m = error_mark();
            as_error(&p, file->package,
                     fmt_sprintf_v(error_allocator(), "package %s; expected %s", name,
                                   pkg_name));
            error_release(m);
            continue; /* ignore this file */
        }
        /* collect the top level file objects in the package scope */
        if (file->scope == NULL) {
            continue;
        }
        MapIter oit = map_iter(file->scope->objects);
        const void *ok = NULL;
        void *ov = NULL;
        while (map_next(&oit, &ok, &ov)) {
            as_declare(&p, pkg_scope, NULL, *(AstObject **)ov);
        }
    }

    /* the package wide map from imported package ids to package objects */
    Map *imports = map_make(a, TYPE_OF(Str), TYPE_AST_OBJECT_PTR, 0);
    if (imports == NULL) {
        as_oom();
    }

    /* complete the file scopes with the imports and resolve identifiers */
    it = map_iter(files);
    while (map_next(&it, &k, &v)) {
        AstFile *file = *(AstFile **)v;
        /* skip a file of another package, which has been reported already */
        if (!str_eq(file->name->name, pkg_name)) {
            continue;
        }
        as_resolve_file(&p, file, imports, importer, pkg_scope, universe);
    }

    go_scanner_error_list_sort(p.errors);
    AstPackage *pkg = (AstPackage *)ast_node_new(a, AST_KIND_PACKAGE);
    if (pkg == NULL) {
        as_oom();
    }
    pkg->name = pkg_name;
    pkg->scope = pkg_scope;
    pkg->imports = imports;
    pkg->files = files;
    *err = go_scanner_error_list_err(p.errors);
    return pkg;
}
