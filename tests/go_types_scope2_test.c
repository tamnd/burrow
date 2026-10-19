/* Derived from Go's src/go/types/scope2_test.go. Go source: go1.27.1.
 *
 * Copyright 2024 The Go Authors. All rights reserved.
 * Copyright 2026 The burrow Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style licence that can be found
 * in the LICENSE file. */

#include "check.h"
#include "go_types_helpers.h"

#include "../src/go/types_internal.h"
#include "burrow/burrow.h"
#include "burrow/fmt.h"
#include "burrow/go/ast.h"
#include "burrow/go/token.h"
#include "burrow/go/types.h"
#include "burrow/map.h"
#include "burrow/mem/arena.h"
#include "burrow/regexp.h"
#include "burrow/strings.h"

#define S(lit) BURROW_S(lit)

/* ScopeComment and ObjectScopePos, from Go's util_test.go. */
static Str scope_comment(TypesScope *s) {
    return s->comment;
}

static TokenPos object_scope_pos(TypesObject obj) {
    return GT_OBJ(obj)->scope_pos;
}

/* Not in Go: strings.ToLower(strings.TrimPrefix(reflect.TypeOf(obj).String(),
 * "*types.")). */
static Str object_kind_name(TypesObject obj) {
    switch (obj->kind) {
    case TYPES_OBJECT_PKG_NAME:
        return S("pkgname");
    case TYPES_OBJECT_CONST:
        return S("const");
    case TYPES_OBJECT_TYPE_NAME:
        return S("typename");
    case TYPES_OBJECT_VAR:
        return S("var");
    case TYPES_OBJECT_FUNC:
        return S("func");
    case TYPES_OBJECT_LABEL:
        return S("label");
    case TYPES_OBJECT_BUILTIN:
        return S("builtin");
    case TYPES_OBJECT_NIL:
        return S("nil");
    default:
        return S("?");
    }
}

/* Not in Go: what %v prints for an Object, which is "<nil>" for nil. */
static Str object_string_or_nil(Alloc *a, TypesObject obj) {
    return obj == NULL ? S("<nil>") : types_object_string(a, obj, TYPES_NO_QUALIFIER);
}

static Str pos_string(Alloc *a, TokenFileSet *fset, TokenPos pos) {
    return token_position_string(token_file_set_position(fset, pos), a);
}

typedef struct {
    TestingT *t;
    TypesConfig *conf;
    TokenFileSet *fset;
    TypesInfo *info;
    Map *imports;
    Alloc *a;
} MakePkgEnv;

static TypesPackage *make_pkg(MakePkgEnv *env, Str path, AstFile *f) {
    Error err = BURROW_NO_ERROR;
    Slice files = slice_append(env->a, slice_nil(TYPE_AST_FILE_PTR), &f, 1);
    TypesPackage *pkg =
        types_config_check(env->conf, env->a, path, env->fset, files, env->info, &err);
    map_set(env->imports, &path, &pkg);
    if (BURROW_FAILED(err))
        testing_t_fatal_v(env->t, error_text(err));
    return pkg;
}

static void print_scope_tree(TestingT *t, Alloc *a, Str indent, TypesScope *s) {
    testing_t_logf_v(t, "%sscope %s %d-%d = [%s]", indent, scope_comment(s),
                     (Int)types_scope_pos(s), (Int)types_scope_end(s),
                     strings_join(a, types_scope_names(a, s), S(" ")));
    for (Int i = 0; i < types_scope_num_children(s); i++)
        print_scope_tree(t, a, fmt_sprintf_v(a, "%s  ", indent),
                         types_scope_child(s, i));
}

/* TestScopeLookupParent ensures that (*Scope).LookupParent returns the correct
 * result at various positions with the source. */
static void TestScopeLookupParent(TestingT *t) {
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    TokenFileSet *fset = token_new_file_set(a);
    Map *imports = new_test_importer(a);
    TypesConfig conf = {.importer = test_importer(imports)};
    TypesInfo info = {0};
    MakePkgEnv env = {t, &conf, fset, &info, imports, a};

    make_pkg(&env, S("lib"), must_parse(a, fset, S("package lib; var X int")));
    /* Each comment that holds name=kind:line makes the test look up the name at that
     * point and checks that it resolves to a decl of the specified kind and
     * line number. "undef" means undefined. Note that type switch case
     * clauses with an empty body (but for comments) need the ";" to ensure
     * that the recorded scope extends past the comments. */
    Str main_src = S(
        "\n"
        "/*lib=pkgname:5*/ /*X=var:1*/ /*Pi=const:8*/ /*T=typename:9*/ /*Y=var:10*/ "
        "/*F=func:12*/\n"
        "package main\n"
        "\n"
        "import \"lib\"\n"
        "import . \"lib\"\n"
        "\n"
        "const Pi = 3.1415\n"
        "type T struct{}\n"
        "var Y, _ = lib.X, X\n"
        "\n"
        "func F[T *U, U any](param1, param2 int) /*param1=undef*/ (res1 "
        "/*res1=undef*/, res2 int) /*param1=var:12*/ /*res1=var:12*/ /*U=typename:12*/ "
        "{\n"
        "\tconst pi, e = 3.1415, /*pi=undef*/ 2.71828 /*pi=const:13*/ /*e=const:13*/\n"
        "\ttype /*t=undef*/ t /*t=typename:14*/ *t\n"
        "\tprint(Y) /*Y=var:10*/\n"
        "\tx, Y := Y, /*x=undef*/ /*Y=var:10*/ Pi /*x=var:16*/ /*Y=var:16*/ ; _ = x; _ "
        "= Y\n"
        "\tvar F = /*F=func:12*/ F[*int, int] /*F=var:17*/ ; _ = F\n"
        "\n"
        "\tvar a []int\n"
        "\tfor i, x := range a /*i=undef*/ /*x=var:16*/ { _ = i; _ = x }\n"
        "\n"
        "\tvar i interface{}\n"
        "\tswitch y := i.(type) { /*y=undef*/\n"
        "\tcase /*y=undef*/ int /*y=undef*/ : /*y=var:23*/ ;\n"
        "\tcase float32, /*y=undef*/ float64 /*y=undef*/ : /*y=var:23*/ ;\n"
        "\tdefault /*y=undef*/ : /*y=var:23*/\n"
        "\t\tprintln(y)\n"
        "\t}\n"
        "\t/*y=undef*/\n"
        "\n"
        "        switch int := i.(type) {\n"
        "        case /*int=typename:0*/ int /*int=typename:0*/ : /*int=var:31*/\n"
        "        \tprintln(int)\n"
        "        default /*int=typename:0*/ : /*int=var:31*/ ;\n"
        "        }\n"
        "\n"
        "\t_ = param1\n"
        "\t_ = res1\n"
        "\treturn\n"
        "}\n"
        "/*main=undef*/\n");

    info.uses = map_make(a, TYPE_UNSAFE_POINTER, TYPE_UNSAFE_POINTER, 0);
    AstFile *f = must_parse(a, fset, main_src);
    TypesScope *main_scope = types_package_scope(make_pkg(&env, S("main"), f));
    Regexp *rx = regexp_must_compile(a, S("^/\\*(\\w*)=([\\w:]*)\\*/$"));
    for (Int i = 0; i < f->comments.len; i++) {
        AstCommentGroup *group = *(AstCommentGroup **)slice_at(f->comments, i);
        for (Int j = 0; j < group->list.len; j++) {
            AstComment *comment = *(AstComment **)slice_at(group->list, j);
            /* Parse the assertion in the comment. */
            Slice m = regexp_find_string_submatch(rx, a, comment->text);
            if (m.len == 0) {
                testing_t_errorf_v(t, "%s: bad comment: %s",
                                   pos_string(a, fset, comment->slash), comment->text);
                continue;
            }
            Str name = ((Str *)m.p)[1], want = ((Str *)m.p)[2];

            /* Look up the name in the innermost enclosing scope. */
            TypesScope *inner = types_scope_innermost(main_scope, comment->slash);
            if (inner == NULL) {
                testing_t_errorf_v(t, "%s: at %s: can't find innermost scope",
                                   pos_string(a, fset, comment->slash), comment->text);
                continue;
            }
            Str got = S("undef");
            TypesObject obj =
                types_scope_lookup_parent(inner, name, comment->slash, NULL);
            if (obj != NULL)
                got = fmt_sprintf_v(
                    a, "%s:%d", object_kind_name(obj),
                    token_file_set_position(fset, types_object_pos(obj)).line);
            if (!str_eq(got, want))
                testing_t_errorf_v(t, "%s: at %s: %s resolved to %s, want %s",
                                   pos_string(a, fset, comment->slash), comment->text,
                                   name, got, want);
        }
    }

    /* Check that for each referring identifier, a lookup of its name on the
     * innermost enclosing scope returns the correct object. */

    MapIter it = map_iter(info.uses);
    const void *k;
    void *v;
    while (map_next(&it, &k, &v)) {
        AstIdent *id = *(AstIdent *const *)k;
        TypesObject want_obj = *(TypesObject *)v;
        TypesScope *inner = types_scope_innermost(main_scope, ast_ident_pos(id));
        if (inner == NULL) {
            testing_t_errorf_v(t, "%s: can't find innermost scope enclosing %q",
                               pos_string(a, fset, ast_ident_pos(id)), id->name);
            continue;
        }

        /* Exclude selectors and qualified identifiers---lexical refs only.
         * (Ideally, we'd see if the AST parent is a SelectorExpr, but that
         * requires PathEnclosingInterval from golang.org/x/tools/go/ast/astutil.) */
        if (str_eq(id->name, S("X")))
            continue;

        TypesObject got_obj =
            types_scope_lookup_parent(inner, id->name, ast_ident_pos(id), NULL);
        if (got_obj != want_obj) {
            /* Print the scope tree of mainScope in case of error. */
            print_scope_tree(t, a, S(""), main_scope);

            testing_t_errorf_v(
                t,
                "%s: Scope(%s).LookupParent(%s@%d) got %s, want %s "
                "[scopePos=%d]",
                pos_string(a, fset, ast_ident_pos(id)), scope_comment(inner), id->name,
                (Int)ast_ident_pos(id), object_string_or_nil(a, got_obj),
                object_string_or_nil(a, want_obj), (Int)object_scope_pos(want_obj));
            continue;
        }
    }
    arena_free(&ar);
}

#define TESTS(X) X(TestScopeLookupParent)

TESTING_MAIN(TESTS)
