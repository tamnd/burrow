/* Derived from Go's src/go/types/resolver_test.go. Go source: go1.27.1.
 *
 * Copyright 2011 The Go Authors. All rights reserved.
 * Copyright 2026 The burrow Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style licence that can be found
 * in the LICENSE file. */

#include "check.h"
#include "go_types_helpers.h"

#include "burrow/burrow.h"
#include "burrow/fmt.h"
#include "burrow/go/ast.h"
#include "burrow/go/token.h"
#include "burrow/go/types.h"
#include "burrow/map.h"
#include "burrow/mem/arena.h"
#include "burrow/sort.h"
#include "burrow/strings.h"

#define S(lit) BURROW_S(lit)
#define NELEM(x) ((Int)(sizeof(x) / sizeof((x)[0])))

typedef struct {
    TokenFileSet *fset;
    TypesImporter importer;
    Map *imported; /* Str -> bool */
    Alloc *a;
} ResolveTestImporter;

static TypesPackage *resolve_test_import(void *self, Str path, Error *err) {
    (void)self;
    (void)path;
    (void)err;
    panic_str(S("should not be called"));
}

static TypesPackage *resolve_test_import_from(void *self, Str path, Str src_dir,
                                              TypesImportMode mode, Error *err) {
    ResolveTestImporter *imp = self;
    if (mode != 0)
        panic_str(S("mode must be 0"));
    if (imp->importer.vt == NULL) {
        imp->importer = default_importer(imp->fset);
        imp->imported = map_make(imp->a, TYPE_STRING, TYPE_BOOL, 0);
    }
    TypesPackage *pkg =
        imp->importer.vt->import_from(imp->importer.data, path, src_dir, mode, err);
    if (pkg == NULL)
        return NULL;
    bool yes = true;
    map_set(imp->imported, &path, &yes);
    return pkg;
}

static const TypesImporterVT resolve_test_importer_vt = {NULL, resolve_test_import,
                                                         resolve_test_import_from};

/* Not in Go: the position %s prints for x. */
static Str ident_pos(Alloc *a, TokenFileSet *fset, AstIdent *x) {
    return token_position_string(token_file_set_position(fset, ast_ident_pos(x)), a);
}

typedef struct {
    TestingT *t;
    Alloc *a;
    TokenFileSet *fset;
    Map *uses;
    Map *defs;
    Slice both;
} ResolveEnv;

static TypesObject lookup_object(Map *m, AstIdent *x, bool *found) {
    TypesObject *p = map_get(m, &x);
    if (found != NULL)
        *found = p != NULL;
    return p == NULL ? NULL : *p;
}

/* check that qualified identifiers are resolved */
static bool check_qualified(void *penv, AstNode n) {
    ResolveEnv *env = penv;
    if (n != NULL && n->kind == AST_KIND_SELECTOR_EXPR) {
        AstSelectorExpr *s = (AstSelectorExpr *)n;
        if (s->x != NULL && s->x->kind == AST_KIND_IDENT) {
            AstIdent *x = (AstIdent *)s->x;
            TypesObject obj = lookup_object(env->uses, x, NULL);
            if (obj == NULL) {
                testing_t_errorf_v(env->t, "%s: unresolved qualified identifier %s",
                                   ident_pos(env->a, env->fset, x), x->name);
                return false;
            }
            if (obj->kind == TYPES_OBJECT_PKG_NAME &&
                lookup_object(env->uses, s->sel, NULL) == NULL) {
                testing_t_errorf_v(env->t, "%s: unresolved selector %s",
                                   ident_pos(env->a, env->fset, s->sel), s->sel->name);
                return false;
            }
            return false;
        }
        return true;
    }
    return true;
}

/* check that each identifier in the source is found in uses or defs or both */
static bool check_ident(void *penv, AstNode n) {
    ResolveEnv *env = penv;
    if (n != NULL && n->kind == AST_KIND_IDENT) {
        AstIdent *x = (AstIdent *)n;
        int objects = 0;
        bool found;
        lookup_object(env->uses, x, &found);
        if (found) {
            objects |= 1;
            map_del(env->uses, &x);
        }
        lookup_object(env->defs, x, &found);
        if (found) {
            objects |= 2;
            map_del(env->defs, &x);
        }
        if (objects == 0)
            testing_t_errorf_v(env->t, "%s: unresolved identifier %s",
                               ident_pos(env->a, env->fset, x), x->name);
        else if (objects == 3)
            env->both = slice_append(env->a, env->both, &x->name, 1);
        return false;
    }
    return true;
}

static void TestResolveIdents(TestingT *t) {
    /* Not in Go: testenv.MustHaveGoBuild. burrow imports from source, so it
     * needs Go's sources. */
    if (!have_source_importer())
        testing_t_skip_v(t, "no Go sources to import from");

    Str sources[] = {
        S("\n\t\tpackage p\n\t\timport \"fmt\"\n\t\timport \"math\"\n\t\tconst pi = "
          "math.Pi\n\t\tfunc sin(x float64) float64 {\n\t\t\treturn "
          "math.Sin(x)\n\t\t}\n\t\tvar Println = fmt.Println\n\t\t"),
        S("\n\t\tpackage p\n\t\timport \"fmt\"\n\t\ttype errorStringer struct { "
          "fmt.Stringer; error }\n\t\tfunc f() string {\n\t\t\t_ = "
          "\"foo\"\n\t\t\treturn fmt.Sprintf(\"%d\", g())\n\t\t}\n\t\tfunc g() (x int) "
          "{ return }\n\t\t"),
        S("\n\t\tpackage p\n\t\timport . \"go/parser\"\n\t\timport \"sync\"\n\t\tfunc "
          "h() Mode { return ImportsOnly }\n\t\tvar _, x int = 1, 2\n\t\tfunc init() "
          "{}\n\t\ttype T struct{ *sync.Mutex; a, b, c int}\n\t\ttype I interface{ m() "
          "}\n\t\tvar _ = T{a: 1, b: 2, c: 3}\n\t\tfunc (_ T) m() {}\n\t\tfunc (T) _() "
          "{}\n\t\tvar i I\n\t\tvar _ = i.m\n\t\tfunc _(s []int) { for i, x := range s "
          "{ _, _ = i, x } }\n\t\tfunc _(x interface{}) {\n\t\t\tswitch x := x.(type) "
          "{\n\t\t\tcase int:\n\t\t\t\t_ = x\n\t\t\t}\n\t\t\tswitch {} // implicit "
          "'true' tag\n\t\t}\n\t\t"),
        S("\n\t\tpackage p\n\t\ttype S struct{}\n\t\tfunc (T) _() {}\n\t\tfunc (T) _() "
          "{}\n\t\t"),
        S("\n\t\tpackage p\n\t\tfunc _() {\n\t\tL0:\n\t\tL1:\n\t\t\tgoto L0\n\t\t\tfor "
          "{\n\t\t\t\tgoto L1\n\t\t\t}\n\t\t\tif true {\n\t\t\t\tgoto "
          "L2\n\t\t\t}\n\t\tL2:\n\t\t}\n\t\t"),
    };

    Str pkgnames[] = {
        S("fmt"),
        S("math"),
    };

    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);

    /* parse package files */
    TokenFileSet *fset = token_new_file_set(a);
    Slice files = slice_nil(TYPE_AST_FILE_PTR);
    for (Int i = 0; i < NELEM(sources); i++) {
        AstFile *f = must_parse(a, fset, sources[i]);
        files = slice_append(a, files, &f, 1);
    }

    /* resolve and type-check package AST */
    ResolveTestImporter importer = {.fset = fset, .a = a};
    TypesConfig conf = {.importer = {&resolve_test_importer_vt, &importer}};
    Map *uses = map_make(a, TYPE_UNSAFE_POINTER, TYPE_UNSAFE_POINTER, 0);
    Map *defs = map_make(a, TYPE_UNSAFE_POINTER, TYPE_UNSAFE_POINTER, 0);
    TypesInfo info = {.defs = defs, .uses = uses};
    Error err = BURROW_NO_ERROR;
    types_config_check(&conf, a, S("testResolveIdents"), fset, files, &info, &err);
    if (BURROW_FAILED(err)) {
        testing_t_fatal_v(t, error_text(err));
        arena_free(&ar);
        return;
    }

    /* check that all packages were imported */
    for (Int i = 0; i < NELEM(pkgnames); i++) {
        bool *ok =
            importer.imported == NULL ? NULL : map_get(importer.imported, &pkgnames[i]);
        if (ok == NULL || !*ok)
            testing_t_errorf_v(t, "package %s not imported", pkgnames[i]);
    }

    ResolveEnv env = {t, a, fset, uses, defs, slice_nil(TYPE_STRING)};
    for (Int i = 0; i < files.len; i++) {
        AstFile *f = *(AstFile **)slice_at(files, i);
        ast_inspect(&f->node, BURROW_FN(AstInspectFunc, check_qualified, &env));
    }

    MapIter it = map_iter(uses);
    const void *k;
    void *v;
    while (map_next(&it, &k, &v)) {
        if (*(TypesObject *)v == NULL) {
            AstIdent *id = *(AstIdent *const *)k;
            testing_t_errorf_v(t, "%s: Uses[%s] == nil", ident_pos(a, fset, id),
                               id->name);
        }
    }

    for (Int i = 0; i < files.len; i++) {
        AstFile *f = *(AstFile **)slice_at(files, i);
        ast_inspect(&f->node, BURROW_FN(AstInspectFunc, check_ident, &env));
    }

    /* check the expected set of idents that are simultaneously uses and defs */
    sort_strings(env.both);
    Str got = fmt_sprintf_v(a, "[%s]", strings_join(a, env.both, S(" ")));
    Str want = S("[Mutex Stringer error]");
    if (!str_eq(got, want))
        testing_t_errorf_v(t, "simultaneous uses/defs = %s, want %s", got, want);

    /* any left-over identifiers didn't exist in the source */
    it = map_iter(uses);
    while (map_next(&it, &k, &v)) {
        AstIdent *x = *(AstIdent *const *)k;
        testing_t_errorf_v(t, "%s: identifier %s not present in source",
                           ident_pos(a, fset, x), x->name);
    }
    it = map_iter(defs);
    while (map_next(&it, &k, &v)) {
        AstIdent *x = *(AstIdent *const *)k;
        testing_t_errorf_v(t, "%s: identifier %s not present in source",
                           ident_pos(a, fset, x), x->name);
    }

    /* TODO(gri) add tests to check ImplicitObj callbacks */
    arena_free(&ar);
}

#define TESTS(X) X(TestResolveIdents)

TESTING_MAIN(TESTS)
