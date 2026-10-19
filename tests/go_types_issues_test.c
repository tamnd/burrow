/* Derived from Go's src/go/types/issues_test.go. Go source: go1.27.1.
 *
 * TestIssue43124 and TestIssue59944 are in go_types_check_test.c, next to the
 * testFiles they call, and TestIssue50646 is in go_types_operand_test.c.
 *
 * Copyright 2013 The Go Authors. All rights reserved.
 * Copyright 2026 The burrow Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style licence that can be found
 * in the LICENSE file. */

#include "check.h"
#include "go_types_importer.h"

#include "burrow/burrow.h"
#include "burrow/fmt.h"
#include "burrow/go/ast.h"
#include "burrow/go/parser.h"
#include "burrow/go/token.h"
#include "burrow/go/types.h"
#include "burrow/map.h"
#include "burrow/mem/arena.h"
#include "burrow/panic.h"
#include "burrow/regexp.h"
#include "burrow/slices.h"
#include "burrow/strings.h"

#define S(lit) BURROW_S(lit)
#define S_(lit) BURROW_S_INIT(lit)
#define NELEM(x) ((Int)(sizeof(x) / sizeof((x)[0])))

/* --------------------------------------------- the helpers from api_test.go */

/* pkgName extracts the package name from src, which must contain a package
 * header. */
static Str pkg_name(Str src) {
    Str kw = S("package ");
    Int i = strings_index(src, kw);
    if (i >= 0) {
        Str after = str_from_bytes(src.p + i + kw.len, src.len - i - kw.len);
        Int n = after.len;
        Int j = strings_index_any(after, S("\n\t ;/"));
        if (j >= 0)
            n = j;
        return str_from_bytes(after.p, n);
    }
    panic_str(S("missing package header"));
}

static AstFile *must_parse(Alloc *a, TokenFileSet *fset, Str src) {
    Error err = BURROW_NO_ERROR;
    AstFile *f =
        parser_parse_file(a, fset, pkg_name(src), BURROW_ANY(TYPE_STRING, &src),
                          PARSER_PARSE_COMMENTS | PARSER_SKIP_OBJECT_RESOLUTION, &err);
    if (BURROW_FAILED(err))
        panic_str(error_text(err)); /* so we don't need to pass *testing.T */
    return f;
}

static void ignore_error(void *env, const TypesError *e) {
    (void)env;
    (void)e;
}

static Slice one_file(Alloc *a, AstFile *f) {
    return slice_append(a, slice_nil(TYPE_AST_FILE_PTR), &f, 1);
}

static TypesPackage *typecheck(Alloc *a, Str src, TypesConfig *conf, TypesInfo *info,
                               Error *err) {
    TokenFileSet *fset = token_new_file_set(a);
    AstFile *f = must_parse(a, fset, src);
    TypesConfig conf0 = {0};
    if (conf == NULL) {
        conf0.error = (TypesErrorFunc){ignore_error, NULL}; /* collect all errors */
        conf0.importer = default_importer(fset);
        conf = &conf0;
    }
    return types_config_check(conf, a, f->name->name, fset, one_file(a, f), info, err);
}

static TypesPackage *must_typecheck(Alloc *a, Str src, TypesConfig *conf,
                                    TypesInfo *info) {
    Error err = BURROW_NO_ERROR;
    TypesPackage *pkg = typecheck(a, src, conf, info, &err);
    if (BURROW_FAILED(err))
        panic_str(error_text(err)); /* so we don't need to pass *testing.T */
    return pkg;
}

/* Not in Go: what %v prints for an error, which is <nil> for none. */
static Str err_string(Error err) {
    return BURROW_OK(err) ? S("<nil>") : error_text(err);
}

static Str type_string(Alloc *a, TypesType t) {
    return types_type_string(a, t, TYPES_NO_QUALIFIER);
}

static Str object_string(Alloc *a, TypesObject obj) {
    return types_object_string(a, obj, TYPES_NO_QUALIFIER);
}

static TypesType typ(TypesBasicKind kind) {
    return types_type(types_typ[kind]);
}

static TypesType universe_type(Str name) {
    return types_object_type(types_scope_lookup(types_universe(), name));
}

static Map *new_types_map(Alloc *a) {
    return map_make(a, TYPE_UNSAFE_POINTER, TYPE_TYPES_TYPE_AND_VALUE, 0);
}

static Map *new_objects_map(Alloc *a) {
    return map_make(a, TYPE_UNSAFE_POINTER, TYPE_UNSAFE_POINTER, 0);
}

/* The object m has for id, or NULL. */
static TypesObject object_for(Map *m, const AstIdent *id) {
    TypesObject obj = NULL;
    (void)map_get2(m, &id, &obj);
    return obj;
}

/* --------------------------------------------------------------- the tests */

static void TestIssue5770(TestingT *t) {
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    Error err = BURROW_NO_ERROR;
    (void)typecheck(a, S("package p; type S struct{T}"), NULL, NULL, &err);
    Str want = S("undefined: T");
    if (BURROW_OK(err) || !strings_contains(error_text(err), want))
        testing_t_errorf_v(t, "got: %v; want: %s", err, want);
    arena_free(&ar);
}

static void TestIssue5849(TestingT *t) {
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    Str src =
        S("\npackage p\nvar (\n\ts uint\n\t_ = uint8(8)\n\t_ = uint16(16) << s\n\t_ = "
          "uint32(32 << s)\n\t_ = uint64(64 << s + s)\n\t_ = "
          "(interface{})(\"foo\")\n\t_ = (interface{})(nil)\n)");
    TypesInfo info = {0};
    info.types = new_types_map(a);
    (void)must_typecheck(a, src, NULL, &info);

    const void *k;
    void *v;
    for (MapIter it = map_iter(info.types); map_next(&it, &k, &v);) {
        AstExpr x = *(AstExpr const *)k;
        const TypesTypeAndValue *tv = v;
        TypesType want = NULL;
        if (x->kind == AST_KIND_BASIC_LIT) {
            Str value = ((AstBasicLit *)x)->value;
            if (str_eq(value, S("8")))
                want = typ(TYPES_UINT8);
            else if (str_eq(value, S("16")))
                want = typ(TYPES_UINT16);
            else if (str_eq(value, S("32")))
                want = typ(TYPES_UINT32);
            else if (str_eq(value, S("64")))
                want = typ(TYPES_UINT); /* because of "+ s", s is of type uint */
            else if (str_eq(value, S("\"foo\"")))
                want = typ(TYPES_STRING);
        } else if (x->kind == AST_KIND_IDENT) {
            if (str_eq(((AstIdent *)x)->name, S("nil")))
                want = typ(TYPES_UNTYPED_NIL);
        }
        if (want != NULL && !types_identical(tv->type, want))
            testing_t_errorf_v(t, "got %s; want %s", type_string(a, tv->type),
                               type_string(a, want));
    }
    arena_free(&ar);
}

static void TestIssue6413(TestingT *t) {
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    /* check_test.go's fset, which typecheck does not use, so as in Go the
     * positions below are not in it. */
    TokenFileSet *fset = token_new_file_set(a);
    Str src = S("\npackage p\nfunc f() int {\n\tdefer f()\n\tgo f()\n\treturn 0\n}\n");
    TypesInfo info = {0};
    info.types = new_types_map(a);
    (void)must_typecheck(a, src, NULL, &info);

    TypesType want = typ(TYPES_INT);
    Int n = 0;
    const void *k;
    void *v;
    for (MapIter it = map_iter(info.types); map_next(&it, &k, &v);) {
        AstExpr x = *(AstExpr const *)k;
        const TypesTypeAndValue *tv = v;
        if (x->kind == AST_KIND_CALL_EXPR) {
            if (tv->type != want) {
                Str pos = token_position_string(
                    token_file_set_position(fset, ast_node_pos(x)), a);
                testing_t_errorf_v(t, "%s: got %s; want %s", pos,
                                   type_string(a, tv->type), type_string(a, want));
            }
            n++;
        }
    }

    if (n != 2)
        testing_t_errorf_v(t, "got %d CallExprs; want 2", n);
    arena_free(&ar);
}

static void TestIssue7245(TestingT *t) {
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    TokenFileSet *fset = token_new_file_set(a);
    Str src = S("\npackage p\nfunc (T) m() (res bool) { return }\ntype T struct{} // "
                "receiver type after method declaration\n");
    AstFile *f = must_parse(a, fset, src);

    TypesConfig conf = {0};
    TypesInfo info = {0};
    info.defs = new_objects_map(a);
    Error err = BURROW_NO_ERROR;
    (void)types_config_check(&conf, a, f->name->name, fset, one_file(a, f), &info,
                             &err);
    if (BURROW_FAILED(err))
        testing_t_fatal_v(t, error_text(err));

    AstFuncDecl *m = BURROW_AT(AstFuncDecl *, f->decls, 0);
    TypesFunc *fn = types_as_func(object_for(info.defs, m->name));
    TypesVar *res1 =
        types_tuple_at(types_signature_results(types_func_signature(fn)), 0);
    AstField *field = BURROW_AT(AstField *, m->type->results->list, 0);
    TypesVar *res2 =
        types_as_var(object_for(info.defs, BURROW_AT(AstIdent *, field->names, 0)));

    if (res1 != res2)
        testing_t_errorf_v(t, "got %s (%p) != %s (%p)",
                           object_string(a, types_object(res1)), (void *)res2,
                           object_string(a, types_object(res1)), (void *)res2);
    arena_free(&ar);
}

static void log_error(void *env, const TypesError *e) {
    TestingT *t = env;
    Arena ar;
    arena_init(&ar, NULL, 0);
    testing_t_log_v(t, types_error_error(e, arena_allocator(&ar)));
    arena_free(&ar);
}

/* This tests that uses of existing vars on the LHS of an assignment are Uses,
 * not Defs; and also that the (illegal) use of a non-var on the LHS of an
 * assignment is a Use nonetheless. */
static void TestIssue7827(TestingT *t) {
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    Str src =
        S("\npackage p\nfunc _() {\n\tconst w = 1        // defs w\n        x, y := "
          "2, 3       // defs x, y\n        w, x, z := 4, 5, 6 // uses w, x, defs "
          "z; error: cannot assign to w\n        _, _, _ = x, y, z  // uses x, y, "
          "z\n}\n");

    /* We need a specific fileset in this test below for positions. Cannot use
     * typecheck helper. */
    TokenFileSet *fset = token_new_file_set(a);
    AstFile *f = must_parse(a, fset, src);

    Str want =
        S("L3 defs func p._()\nL4 defs const w untyped int\nL5 defs var x "
          "int\nL5 defs var y int\nL6 defs var z int\nL6 uses const w untyped "
          "int\nL6 uses var x int\nL7 uses var x int\nL7 uses var y int\nL7 uses "
          "var z int");

    /* don't abort at the first error */
    TypesConfig conf = {0};
    conf.error = (TypesErrorFunc){log_error, t};
    TypesInfo info = {0};
    info.defs = new_objects_map(a);
    info.uses = new_objects_map(a);
    Error err = BURROW_NO_ERROR;
    (void)types_config_check(&conf, a, f->name->name, fset, one_file(a, f), &info,
                             &err);
    Str s = err_string(err);
    if (!strings_has_suffix(s, S("cannot assign to w")))
        testing_t_errorf_v(t, "Check: unexpected error: %s", s);

    Slice facts = slice_nil(TYPE_STRING);
    const void *k;
    void *v;
    for (MapIter it = map_iter(info.defs); map_next(&it, &k, &v);) {
        const AstIdent *id = *(AstIdent *const *)k;
        TypesObject obj = *(TypesObject *)v;
        if (obj != NULL) {
            Str fact = fmt_sprintf_v(a, "L%d defs %s",
                                     token_file_set_position(fset, id->name_pos).line,
                                     object_string(a, obj));
            facts = slice_append(a, facts, &fact, 1);
        }
    }
    for (MapIter it = map_iter(info.uses); map_next(&it, &k, &v);) {
        const AstIdent *id = *(AstIdent *const *)k;
        TypesObject obj = *(TypesObject *)v;
        Str fact = fmt_sprintf_v(a, "L%d uses %s",
                                 token_file_set_position(fset, id->name_pos).line,
                                 object_string(a, obj));
        facts = slice_append(a, facts, &fact, 1);
    }
    slices_sort(facts);

    Str got = strings_join(a, facts, S("\n"));
    if (!str_eq(got, want))
        testing_t_errorf_v(t, "Unexpected defs/uses\ngot:\n%s\nwant:\n%s", got, want);
    arena_free(&ar);
}

/* This tests that the package associated with the types.Object.Pkg method is
 * the type's package independent of the order in which the imports are
 * listed in the sources src1, src2 below. The actual issue is in
 * go/internal/gcimporter which has a corresponding test; we leave this test
 * here to verify correct behavior at the go/types level. */
static void TestIssue13898(TestingT *t) {
    /* Not in Go: in place of testenv.MustHaveGoBuild, the importer here reads
     * Go's sources, and these need go/types and go/importer from them. */
    if (!have_source_importer())
        testing_t_skip_v(t, "no go1.27 sources to import go/types from");

    static const Str srcs[3] = {
        S_("\npackage main\n\nimport \"go/types\"\n\nfunc main() {\n\tvar info "
           "types.Info\n\tfor _, obj := range info.Uses {\n\t\t_ = "
           "obj.Pkg()\n\t}\n}\n"),
        /* like src0, but also imports go/importer */
        S_("\npackage main\n\nimport (\n\t\"go/types\"\n\t_ \"go/importer\"\n)\n\nfunc "
           "main() {\n\tvar info types.Info\n\tfor _, obj := range info.Uses {\n\t\t_ "
           "= obj.Pkg()\n\t}\n}\n"),
        /* like src1 but with different import order (used to fail with this
         * issue) */
        S_("\npackage main\n\nimport (\n\t_ \"go/importer\"\n\t\"go/types\"\n)\n\nfunc "
           "main() {\n\tvar info types.Info\n\tfor _, obj := range info.Uses {\n\t\t_ "
           "= obj.Pkg()\n\t}\n}\n"),
    };
    static const char *const names[3] = {"src0", "src1", "src2"};

    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    for (Int i = 0; i < NELEM(srcs); i++) {
        TypesInfo info = {0};
        info.uses = new_objects_map(a);
        (void)must_typecheck(a, srcs[i], NULL, &info);

        TypesPackage *pkg = NULL;
        Int count = 0;
        const void *k;
        void *v;
        for (MapIter it = map_iter(info.uses); map_next(&it, &k, &v);) {
            const AstIdent *id = *(AstIdent *const *)k;
            if (str_eq(id->name, S("Pkg"))) {
                pkg = types_object_pkg(*(TypesObject *)v);
                count++;
            }
        }
        if (count != 1) {
            arena_free(&ar);
            testing_t_fatalf_v(t, "%s: got %d entries named Pkg; want 1", names[i],
                               count);
        }
        if (!str_eq(types_package_name(pkg), S("types"))) {
            testing_t_errorf_v(t, "%s: got %s; want package types", names[i],
                               types_package_string(a, pkg));
            arena_free(&ar);
            return;
        }
    }
    arena_free(&ar);
}

typedef struct {
    Alloc *a;
    Str got;
} GotErrors;

static void append_error(void *env, const TypesError *e) {
    GotErrors *g = env;
    g->got = fmt_sprintf_v(g->a, "%s%s\n", g->got, types_error_error(e, g->a));
}

static void TestIssue22525(TestingT *t) {
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    Str src = S("package p; func f() { var a, b, c, d, e int }");

    GotErrors g = {a, S("\n")};
    TypesConfig conf = {0};
    conf.error = (TypesErrorFunc){append_error, &g};
    Error err = BURROW_NO_ERROR;
    (void)typecheck(a, src, &conf, NULL, &err); /* do not crash */
    Str want = S("\np:1:27: declared and not used: a\np:1:30: declared and not used: "
                 "b\np:1:33: declared and not used: c\np:1:36: declared and not used: "
                 "d\np:1:39: declared and not used: e\n");
    if (!str_eq(g.got, want))
        testing_t_errorf_v(t, "got: %swant: %s", g.got, want);
    arena_free(&ar);
}

typedef struct {
    TestingT *t;
    TypesInfo *info;
    Str src;
} Issue25627;

static bool issue25627_inspect(void *env, AstNode n) {
    Issue25627 *e = env;
    if (n != NULL && n->kind == AST_KIND_TYPE_SPEC) {
        AstTypeSpec *spec = (AstTypeSpec *)n;
        TypesTypeAndValue tv;
        if (map_get2(e->info->types, &spec->type, &tv) &&
            str_eq(spec->name->name, S("T"))) {
            Int want = strings_count(e->src, S(";")) + 1;
            Int got = types_struct_num_fields(types_as_struct(tv.type));
            if (got != want)
                testing_t_errorf_v(e->t, "%s: got %d fields; want %d", e->src, got,
                                   want);
        }
    }
    return true;
}

static void TestIssue25627(TestingT *t) {
    static const Str prefix = S_("package p; import \"unsafe\"; type P *struct{}; type "
                                 "I interface{}; type T ");
    /* The src strings (without prefix) are constructed such that the number of
     * semicolons plus one corresponds to the number of fields expected in the
     * respective struct. */
    static const Str srcs[] = {
        S_("struct { x Missing }"),
        S_("struct { Missing }"),
        S_("struct { *Missing }"),
        S_("struct { unsafe.Pointer }"),
        S_("struct { P }"),
        S_("struct { *I }"),
        S_("struct { a int; b Missing; *Missing }"),
    };

    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    TokenFileSet *fset = token_new_file_set(a);
    for (Int i = 0; i < NELEM(srcs); i++) {
        Str src = srcs[i];
        AstFile *f = must_parse(a, fset, fmt_sprintf_v(a, "%s%s", prefix, src));

        TypesConfig cfg = {0};
        cfg.importer = default_importer(fset);
        cfg.error = (TypesErrorFunc){ignore_error, NULL};
        TypesInfo info = {0};
        info.types = new_types_map(a);
        Error err = BURROW_NO_ERROR;
        (void)types_config_check(&cfg, a, f->name->name, fset, one_file(a, f), &info,
                                 &err);
        if (BURROW_FAILED(err) && errors_as(err, TYPE_TYPES_ERROR) == NULL) {
            testing_t_error_v(t, error_text(err));
            arena_free(&ar);
            return;
        }

        Issue25627 env = {t, &info, src};
        ast_inspect(&f->node, BURROW_FN(AstInspectFunc, issue25627_inspect, &env));
    }
    arena_free(&ar);
}

static void TestIssue28005(TestingT *t) {
    /* method names must match defining interface name for this test (see last
     * comment in this function) */
    static const Str sources[3] = {
        S_("package p; type A interface{ A() }"),
        S_("package p; type B interface{ B() }"),
        S_("package p; type X interface{ A; B }"),
    };

    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    TokenFileSet *fset = token_new_file_set(a);

    /* compute original file ASTs */
    AstFile *orig[3];
    for (Int i = 0; i < NELEM(sources); i++)
        orig[i] = must_parse(a, fset, sources[i]);

    /* run the test for all order permutations of the incoming files */
    static const Int perms[6][3] = {
        {0, 1, 2}, {0, 2, 1}, {1, 0, 2}, {1, 2, 0}, {2, 0, 1}, {2, 1, 0},
    };
    for (Int p = 0; p < NELEM(perms); p++) {
        const Int *perm = perms[p];
        Str perm_str = fmt_sprintf_v(a, "[%d %d %d]", perm[0], perm[1], perm[2]);

        /* create file order permutation */
        Slice files = slice_nil(TYPE_AST_FILE_PTR);
        for (Int i = 0; i < 3; i++)
            files = slice_append(a, files, &orig[perm[i]], 1);

        /* type-check package with given file order permutation */
        TypesConfig conf = {0};
        TypesInfo info = {0};
        info.defs = new_objects_map(a);
        Error err = BURROW_NO_ERROR;
        (void)types_config_check(&conf, a, S(""), fset, files, &info, &err);
        if (BURROW_FAILED(err)) {
            testing_t_error_v(t, error_text(err));
            arena_free(&ar);
            return;
        }

        /* look for interface object X */
        TypesObject obj = NULL;
        const void *k;
        void *v;
        for (MapIter it = map_iter(info.defs); map_next(&it, &k, &v);) {
            const AstIdent *name = *(AstIdent *const *)k;
            if (str_eq(name->name, S("X"))) {
                obj = *(TypesObject *)v;
                break;
            }
        }
        if (obj == NULL) {
            arena_free(&ar);
            testing_t_fatal_v(t, "object X not found");
        }
        /* object X must be an interface */
        TypesInterface *iface =
            types_as_interface(types_type_underlying(types_object_type(obj)));

        /* Each iface method m is embedded; and m's receiver base type name must
         * match the method's name per the choice in the source file. */
        for (Int i = 0; i < types_interface_num_methods(iface); i++) {
            TypesFunc *m = types_interface_method(iface, i);
            TypesVar *recv = types_signature_recv(types_func_signature(m));
            TypesNamed *named = types_as_named(types_object_type(types_object(recv)));
            Str recv_name = types_object_name(types_object(types_named_obj(named)));
            Str m_name = types_object_name(types_object(m));
            if (!str_eq(recv_name, m_name))
                testing_t_errorf_v(t, "perm %s: got recv %s; want %s", perm_str,
                                   recv_name, m_name);
        }
    }
    arena_free(&ar);
}

/* Not in Go: a []*T or []Type of one or two elements, for the constructors. */
static Slice ptrs1(Alloc *a, const void *p) {
    return slice_append(a, slice_nil(TYPE_UNSAFE_POINTER), &p, 1);
}

static Slice ptrs2(Alloc *a, const void *p, const void *q) {
    const void *pq[2] = {p, q};
    return slice_append(a, slice_nil(TYPE_UNSAFE_POINTER), pq, 2);
}

static void TestIssue28282(TestingT *t) {
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);

    /* create type interface { error } */
    TypesType et = universe_type(S("error"));
    TypesInterface *it =
        types_new_interface_type(a, slice_nil(TYPE_UNSAFE_POINTER), ptrs1(a, et));
    (void)types_interface_complete(it);
    Str its = type_string(a, types_type(it));
    Str ets = type_string(a, et);
    /* verify that after completing the interface, the embedded method remains
     * unchanged */
    TypesFunc *want =
        types_interface_method(types_as_interface(types_type_underlying(et)), 0);
    Str wants = object_string(a, types_object(want));
    TypesFunc *got = types_interface_method(it, 0);
    if (got != want) {
        testing_t_errorf_v(t, "%s.Method(0): got %q (%p); want %q (%p)", its,
                           object_string(a, types_object(got)), (void *)got, wants,
                           (void *)want);
        arena_free(&ar);
        return;
    }
    /* verify that lookup finds the same method in both interfaces (redundant
     * check) */
    TypesObject obj =
        types_lookup_field_or_method(a, et, false, NULL, S("Error"), NULL, NULL);
    if (obj != types_object(want)) {
        testing_t_errorf_v(t, "%s.Lookup: got %q (%p); want %q (%p)", ets,
                           object_string(a, obj), (void *)obj, wants, (void *)want);
        arena_free(&ar);
        return;
    }
    obj = types_lookup_field_or_method(a, types_type(it), false, NULL, S("Error"), NULL,
                                       NULL);
    if (obj != types_object(want))
        testing_t_errorf_v(t, "%s.Lookup: got %q (%p); want %q (%p)", its,
                           object_string(a, obj), (void *)obj, wants, (void *)want);
    arena_free(&ar);
}

/* printInfo prints the *Func definitions recorded in info, one *Func per
 * line. */
static Str print_info(Alloc *a, TypesInfo *info) {
    Str buf = S("");
    const void *k;
    void *v;
    for (MapIter it = map_iter(info->defs); map_next(&it, &k, &v);) {
        TypesObject obj = *(TypesObject *)v;
        if (obj != NULL && types_as_func(obj) != NULL)
            buf = fmt_sprintf_v(a, "%s%s\n", buf, object_string(a, obj));
    }
    return buf;
}

static void TestIssue29029(TestingT *t) {
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    TokenFileSet *fset = token_new_file_set(a);
    AstFile *f1 = must_parse(a, fset, S("package p; type A interface { M() }"));
    AstFile *f2 = must_parse(a, fset, S("package p; var B interface { A }"));

    /* The *Func (method) definitions for package p must be the same
     * independent on whether f1 and f2 are type-checked together, or
     * incrementally. */

    /* type-check together */
    TypesConfig conf = {0};
    TypesInfo info = {0};
    info.defs = new_objects_map(a);
    TypesChecker *check =
        types_new_checker(a, &conf, fset, types_new_package(a, S(""), S("p")), &info);
    Error err = BURROW_NO_ERROR;
    Slice both = slice_append(a, one_file(a, f1), &f2, 1);
    if (!types_checker_files(check, both, &err)) {
        testing_t_error_v(t, error_text(err));
        arena_free(&ar);
        return;
    }
    Str want = print_info(a, &info);

    /* type-check incrementally */
    info = (TypesInfo){0};
    info.defs = new_objects_map(a);
    check =
        types_new_checker(a, &conf, fset, types_new_package(a, S(""), S("p")), &info);
    if (!types_checker_files(check, one_file(a, f1), &err) ||
        !types_checker_files(check, one_file(a, f2), &err)) {
        testing_t_error_v(t, error_text(err));
        arena_free(&ar);
        return;
    }
    Str got = print_info(a, &info);

    if (!str_eq(got, want))
        testing_t_errorf_v(t, "\ngot : %swant: %s", got, want);
    arena_free(&ar);
}

typedef struct {
    TypesPackage *pkg;
    TypesImporter fallback;
} ImportHelper;

static TypesPackage *import_helper_import(void *self, Str path, Error *err) {
    ImportHelper *h = self;
    if (str_eq(path, types_package_path(h->pkg)))
        return h->pkg;
    if (h->fallback.vt == NULL) {
        *err = fmt_errorf_v("got package path %q; want %q", path,
                            types_package_path(h->pkg));
        return NULL;
    }
    return h->fallback.vt->import_(h->fallback.data, path, err);
}

static const TypesImporterVT import_helper_vt = {NULL, import_helper_import, NULL};

static TypesImporter import_helper(ImportHelper *h) {
    return (TypesImporter){&import_helper_vt, h};
}

static void TestIssue34151(TestingT *t) {
    (void)t;
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    Str asrc =
        S("package a; type I interface{ M() }; type T struct { F interface { I } }");
    Str bsrc =
        S("package b; import \"a\"; type T struct { F interface { a.I } }; var _ = "
          "a.T(T{})");

    TypesPackage *pa = must_typecheck(a, asrc, NULL, NULL);

    ImportHelper h = {pa, {0}};
    TypesConfig conf = {0};
    conf.importer = import_helper(&h);
    (void)must_typecheck(a, bsrc, &conf, NULL);
    arena_free(&ar);
}

/* TestIssue34921 verifies that we don't update an imported type's underlying
 * type when resolving an underlying type. Specifically, when determining the
 * underlying type of b.T (which is the underlying type of a.T, which is int)
 * we must not set the underlying type of a.T again since that would lead to a
 * race condition if package b is imported elsewhere, in a package that is
 * concurrently type-checked. */
static void TestIssue34921(TestingT *t) {
    static const Str sources[] = {
        S_("package a; type T int"),
        S_("package b; import \"a\"; type T a.T"),
    };

    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    BURROW_TRY {
        TypesPackage *pkg = NULL;
        for (Int i = 0; i < NELEM(sources); i++) {
            ImportHelper h = {pkg, {0}};
            TypesConfig conf = {0};
            conf.importer = import_helper(&h);
            /* pkg imported by the next package in this test */
            pkg = must_typecheck(a, sources[i], &conf, NULL);
        }
    }
    BURROW_CATCH(r) {
        testing_t_error_v(t, panic_text(r));
    }
    BURROW_TRY_END;
    arena_free(&ar);
}

static void TestIssue43088(TestingT *t) {
    (void)t;
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    /* type T1 struct {
     *         _ T2
     * }
     *
     * type T2 struct {
     *         _ struct {
     *                 _ T2
     *         }
     * } */
    Slice none = slice_nil(TYPE_UNSAFE_POINTER);
    Slice no_tags = slice_nil(TYPE_STRING);
    TypesTypeName *n1 = types_new_type_name(a, TOKEN_NO_POS, NULL, S("T1"), NULL);
    TypesNamed *T1 = types_new_named(a, n1, NULL, none);
    TypesTypeName *n2 = types_new_type_name(a, TOKEN_NO_POS, NULL, S("T2"), NULL);
    TypesNamed *T2 = types_new_named(a, n2, NULL, none);
    TypesStruct *s1 = types_new_struct(
        a,
        ptrs1(a, types_new_field(a, TOKEN_NO_POS, NULL, S("_"), types_type(T2), false)),
        no_tags);
    types_named_set_underlying(T1, types_type(s1));
    TypesStruct *s2 = types_new_struct(
        a,
        ptrs1(a, types_new_field(a, TOKEN_NO_POS, NULL, S("_"), types_type(T2), false)),
        no_tags);
    TypesStruct *s3 = types_new_struct(
        a,
        ptrs1(a, types_new_field(a, TOKEN_NO_POS, NULL, S("_"), types_type(s2), false)),
        no_tags);
    types_named_set_underlying(T2, types_type(s3));

    /* These calls must terminate (no endless recursion). */
    (void)types_comparable(types_type(T1));
    (void)types_comparable(types_type(T2));
    arena_free(&ar);
}

static Str qualify_unsafe_as_foo(void *env, TypesPackage *pkg) {
    (void)env;
    if (pkg == types_unsafe())
        return S("foo");
    return S("");
}

static void TestIssue44515(TestingT *t) {
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    TypesType ptr = types_object_type(
        types_scope_lookup(types_package_scope(types_unsafe()), S("Pointer")));

    Str got = types_type_string(a, ptr, TYPES_NO_QUALIFIER);
    Str want = S("unsafe.Pointer");
    if (!str_eq(got, want))
        testing_t_errorf_v(t, "got %q; want %q", got, want);

    TypesQualifier qf = {qualify_unsafe_as_foo, NULL};
    got = types_type_string(a, ptr, qf);
    want = S("foo.Pointer");
    if (!str_eq(got, want))
        testing_t_errorf_v(t, "got %q; want %q", got, want);
    arena_free(&ar);
}

static void new_variadic_sig(Alloc *a, TypesType typ) {
    TypesVar *par = types_new_param(a, TOKEN_NO_POS, NULL, S(""), typ);
    TypesTuple *params = types_new_tuple(a, ptrs1(a, par));
    (void)types_new_signature_type(a, NULL, slice_nil(TYPE_UNSAFE_POINTER),
                                   slice_nil(TYPE_UNSAFE_POINTER), params, NULL, true);
}

/* makeSig makes the signature func(typ...). If valid is not set, making that
 * signature is expected to panic. */
static void make_sig(Alloc *a, TypesType typ, bool valid) {
    if (valid) {
        new_variadic_sig(a, typ);
        return;
    }
    volatile bool panicked = false;
    volatile bool is_string = false;
    BURROW_TRY {
        new_variadic_sig(a, typ);
    }
    BURROW_CATCH(r) {
        panicked = true;
        is_string = r.t == TYPE_STRING;
    }
    BURROW_TRY_END;
    if (!panicked)
        panic_str(S("NewSignatureType panic expected"));
    if (!is_string)
        panic_str(S("NewSignatureType string panic expected"));
}

static TypesType type_param(Alloc *a, Str name, TypesType embedded) {
    TypesTypeName *P = types_new_type_name(a, TOKEN_NO_POS, NULL, name, NULL);
    TypesInterface *c =
        types_new_interface_type(a, slice_nil(TYPE_UNSAFE_POINTER), ptrs1(a, embedded));
    return types_type(types_new_type_param(a, P, types_type(c)));
}

static void TestIssue55030(TestingT *t) {
    (void)t;
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);

    /* makeSig must not panic for the following (example) types: */
    /* []int */
    make_sig(a, types_type(types_new_slice(a, typ(TYPES_INT))), true);

    /* string */
    make_sig(a, typ(TYPES_STRING), true);

    /* P where P's common underlying type is string */
    make_sig(a, type_param(a, S("P"), typ(TYPES_STRING)), true); /* [P string] */

    /* P where P's common underlying type is an (unnamed) slice */
    make_sig(a, type_param(a, S("P"), types_type(types_new_slice(a, typ(TYPES_INT)))),
             true); /* [P []int] */

    /* P where P's type set contains strings and []byte */
    {
        TypesTerm *t1 = types_new_term(a, true, typ(TYPES_STRING)); /* ~string */
        TypesTerm *t2 = types_new_term(
            a, false, types_type(types_new_slice(a, typ(TYPES_BYTE)))); /* []byte */
        TypesUnion *u = types_new_union(a, ptrs2(a, t1, t2)); /* ~string | []byte */
        make_sig(a, type_param(a, S("P"), types_type(u)),
                 true); /* [P ~string | []byte] */
    }

    /* makeSig must panic for the following (example) types: */
    /* int */
    make_sig(a, typ(TYPES_INT), false);

    /* P where P's type set doesn't have any specific types */
    make_sig(a, type_param(a, S("P"), universe_type(S("any"))), false); /* [P any] */

    /* P where P's type set doesn't have any slice or string types */
    make_sig(a, type_param(a, S("P"), typ(TYPES_INT)), false); /* [P any] */
    arena_free(&ar);
}

static void TestIssue51093(TestingT *t) {
    /* Each test stands for a conversion of the form P(val) where P is a type
     * parameter with typ as constraint. The test ensures that P(val) has the
     * correct type P and is not a constant. */
    static const struct {
        Str typ;
        Str val;
    } tests[] = {
        {S_("bool"), S_("false")},
        {S_("int"), S_("-1")},
        {S_("uint"), S_("1.0")},
        {S_("rune"), S_("'a'")},
        {S_("float64"), S_("3.5")},
        {S_("complex64"), S_("1.25")},
        {S_("string"), S_("\"foo\"")},

        /* some more complex constraints */
        {S_("~byte"), S_("1")},
        {S_("~int | ~float64 | complex128"), S_("1")},
        {S_("~uint64 | ~rune"), S_("'X'")},
    };

    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    for (Int i = 0; i < NELEM(tests); i++) {
        Str src = fmt_sprintf_v(a, "package p; func _[P %s]() { _ = P(%s) }",
                                tests[i].typ, tests[i].val);
        TypesInfo info = {0};
        info.types = new_types_map(a);
        (void)must_typecheck(a, src, NULL, &info);

        Int n = 0;
        const void *k;
        void *v;
        for (MapIter it = map_iter(info.types); map_next(&it, &k, &v);) {
            AstExpr x = *(AstExpr const *)k;
            const TypesTypeAndValue *tv = v;
            if (x->kind != AST_KIND_CALL_EXPR)
                continue;
            /* there must be exactly one CallExpr which is the P(val)
             * conversion */
            n++;
            TypesTypeParam *tpar = types_as_type_param(tv->type);
            if (tpar == NULL) {
                testing_t_errorf_v(t, "%s: got type %s, want type parameter",
                                   types_expr_string(a, x), type_string(a, tv->type));
                arena_free(&ar);
                return;
            }
            Str name = types_object_name(types_object(types_type_param_obj(tpar)));
            if (!str_eq(name, S("P"))) {
                testing_t_errorf_v(t, "%s: got type parameter name %s, want P",
                                   types_expr_string(a, x), name);
                arena_free(&ar);
                return;
            }
            /* P(val) must not be constant. Not in Go: the zero ConstantValue, of
             * kind unknown, is Go's nil Value. */
            if (constant_value_kind(tv->value) != CONSTANT_UNKNOWN) {
                Str value = constant_value_string(tv->value, a);
                testing_t_errorf_v(t,
                                   "%s: got constant value %s (%s), want no constant",
                                   types_expr_string(a, x), value, value);
            }
        }

        if (n != 1) {
            testing_t_errorf_v(t, "%s: got %d CallExpr nodes; want 1", src, (Int)1);
            arena_free(&ar);
            return;
        }
    }
    arena_free(&ar);
}

static void TestIssue54258(TestingT *t) {
    static const struct {
        Str main, b, want;
    } tests[] = {
        {/* --------------------------------------------------------------- */
         S_("package main\nimport \"b\"\ntype I0 interface {\n\tM0(w struct{ f string "
            "})\n}\nvar _ I0 = b.S{}\n"),
         S_("package b\ntype S struct{}\nfunc (S) M0(struct{ f string }) {}\n"),
         S_("6:12: cannot use b[.]S{} [(]value of struct type b[.]S[)] as I0 value in "
            "variable declaration: b[.]S does not implement I0 [(]wrong type for "
            "method "
            "M0[)]\n.*have M0[(]struct{f string /[*] package b [*]/ }[)]\n.*want "
            "M0[(]struct{f string /[*] package main [*]/ }[)]")},

        {/* --------------------------------------------------------------- */
         S_("package main\nimport \"b\"\ntype I1 interface {\n\tM1(struct{ string "
            "})\n}\nvar _ I1 = b.S{}\n"),
         S_("package b\ntype S struct{}\nfunc (S) M1(struct{ string }) {}\n"),
         S_("6:12: cannot use b[.]S{} [(]value of struct type b[.]S[)] as I1 value in "
            "variable declaration: b[.]S does not implement I1 [(]wrong type for "
            "method "
            "M1[)]\n.*have M1[(]struct{string /[*] package b [*]/ }[)]\n.*want "
            "M1[(]struct{string /[*] package main [*]/ }[)]")},

        {/* --------------------------------------------------------------- */
         S_("package main\nimport \"b\"\ntype I2 interface {\n\tM2(y struct{ f struct{ "
            "f string } })\n}\nvar _ I2 = b.S{}\n"),
         S_("package b\ntype S struct{}\nfunc (S) M2(struct{ f struct{ f string } }) "
            "{}\n"),
         S_("6:12: cannot use b[.]S{} [(]value of struct type b[.]S[)] as I2 value in "
            "variable declaration: b[.]S does not implement I2 [(]wrong type for "
            "method "
            "M2[)]\n.*have M2[(]struct{f struct{f string} /[*] package b [*]/ "
            "}[)]\n.*want M2[(]struct{f struct{f string} /[*] package main [*]/ }[)]")},

        {/* --------------------------------------------------------------- */
         S_("package main\nimport \"b\"\ntype I3 interface {\n\tM3(z struct{ F struct{ "
            "f string } })\n}\nvar _ I3 = b.S{}\n"),
         S_("package b\ntype S struct{}\nfunc (S) M3(struct{ F struct{ f string } }) "
            "{}\n"),
         S_("6:12: cannot use b[.]S{} [(]value of struct type b[.]S[)] as I3 value in "
            "variable declaration: b[.]S does not implement I3 [(]wrong type for "
            "method "
            "M3[)]\n.*have M3[(]struct{F struct{f string /[*] package b [*]/ "
            "}}[)]\n.*want M3[(]struct{F struct{f string /[*] package main [*]/ "
            "}}[)]")},

        {/* --------------------------------------------------------------- */
         S_("package main\nimport \"b\"\ntype I4 interface {\n\tM4(_ struct { *string "
            "})\n}\nvar _ I4 = b.S{}\n"),
         S_("package b\ntype S struct{}\nfunc (S) M4(struct { *string }) {}\n"),
         S_("6:12: cannot use b[.]S{} [(]value of struct type b[.]S[)] as I4 value in "
            "variable declaration: b[.]S does not implement I4 [(]wrong type for "
            "method "
            "M4[)]\n.*have M4[(]struct{[*]string /[*] package b [*]/ }[)]\n.*want "
            "M4[(]struct{[*]string /[*] package main [*]/ }[)]")},

        {/* --------------------------------------------------------------- */
         S_("package main\nimport \"b\"\ntype t struct{ A int }\ntype I5 interface "
            "{\n\tM5(_ struct {b.S;t})\n}\nvar _ I5 = b.S{}\n"),
         S_("package b\ntype S struct{}\ntype t struct{ A int }\nfunc (S) M5(struct "
            "{S;t}) {}\n"),
         S_("7:12: cannot use b[.]S{} [(]value of struct type b[.]S[)] as I5 value in "
            "variable declaration: b[.]S does not implement I5 [(]wrong type for "
            "method "
            "M5[)]\n.*have M5[(]struct{b[.]S; b[.]t}[)]\n.*want M5[(]struct{b[.]S; "
            "t}[)]")},
    };

    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    TokenFileSet *fset = token_new_file_set(a);
    for (Int i = 0; i < NELEM(tests); i++) {
        Str want = tests[i].want;
        Regexp *re = regexp_must_compile(a, want);
        TypesPackage *bpkg = must_typecheck(a, tests[i].b, NULL, NULL);
        AstFile *mast = must_parse(a, fset, tests[i].main);
        ImportHelper h = {bpkg, {0}};
        TypesConfig conf = {0};
        conf.importer = import_helper(&h);
        Error err = BURROW_NO_ERROR;
        (void)types_config_check(&conf, a, mast->name->name, fset, one_file(a, mast),
                                 NULL, &err);
        if (BURROW_OK(err)) {
            testing_t_error_v(t, "Expected failure, but it did not");
        } else {
            Str got = error_text(err);
            if (!regexp_match_string(re, got))
                testing_t_errorf_v(t, "Wanted match for\n\t%s\n but got\n\t%s", want,
                                   got);
            else if (testing_verbose())
                testing_t_logf_v(t, "Saw expected\n\t%s", got);
        }
        regexp_free(re);
    }
    arena_free(&ar);
}

static void TestIssue61931(TestingT *t) {
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    Str src =
        S("\npackage p\n\nfunc A(func(any), ...any) {}\nfunc B[T any](T)          "
          "{}\n\nfunc _() {\n\tA(B, nil // syntax error: missing ',' before newline "
          "in argument list\n}\n");
    TokenFileSet *fset = token_new_file_set(a);
    Error err = BURROW_NO_ERROR;
    AstFile *f =
        parser_parse_file(a, fset, pkg_name(src), BURROW_ANY(TYPE_STRING, &src),
                          PARSER_SKIP_OBJECT_RESOLUTION, &err);
    if (BURROW_OK(err)) {
        arena_free(&ar);
        testing_t_fatal_v(t, "expected syntax error");
    }

    TypesConfig conf = {0};
    Error cerr = BURROW_NO_ERROR;
    /* must not panic */
    (void)types_config_check(&conf, a, f->name->name, fset, one_file(a, f), NULL,
                             &cerr);
    arena_free(&ar);
}

static void TestIssue61938(TestingT *t) {
    (void)t;
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    Str src = S("\npackage p\n\nfunc f[T any]() {}\nfunc _()        { f() }\n");
    /* no error handler provided (this issue) */
    TypesConfig conf = {0};
    Error err = BURROW_NO_ERROR;
    (void)typecheck(a, src, &conf, NULL, &err); /* must not panic */

    /* with error handler (sanity check) */
    conf.error = (TypesErrorFunc){ignore_error, NULL};
    err = BURROW_NO_ERROR;
    (void)typecheck(a, src, &conf, NULL, &err); /* must not panic */
    arena_free(&ar);
}

static void TestIssue63260(TestingT *t) {
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    Str src =
        S("\npackage p\n\nfunc _() {\n        use(f[*string])\n}\n\nfunc use(func()) "
          "{}\n\nfunc f[I *T, T any]() {\n        var v T\n        _ = v\n}");

    TypesInfo info = {0};
    info.defs = new_objects_map(a);
    TypesPackage *pkg = must_typecheck(a, src, NULL, &info);

    /* get type parameter T in signature of f */
    TypesSignature *sig = types_as_signature(
        types_object_type(types_scope_lookup(types_package_scope(pkg), S("f"))));
    TypesTypeParam *T = types_type_param_list_at(types_signature_type_params(sig), 1);
    if (!str_eq(types_object_name(types_object(types_type_param_obj(T))), S("T"))) {
        testing_t_errorf_v(t, "got type parameter %s, want T",
                           type_string(a, types_type(T)));
        arena_free(&ar);
        return;
    }

    /* get type of variable v in body of f */
    TypesObject v = NULL;
    const void *k;
    void *val;
    for (MapIter it = map_iter(info.defs); map_next(&it, &k, &val);) {
        const AstIdent *name = *(AstIdent *const *)k;
        if (str_eq(name->name, S("v"))) {
            v = *(TypesObject *)val;
            break;
        }
    }
    if (v == NULL) {
        arena_free(&ar);
        testing_t_fatal_v(t, "variable v not found");
    }

    /* type of v and T must be pointer-identical */
    if (types_object_type(v) != types_type(T))
        testing_t_errorf_v(t, "types of v and T are not pointer-identical: %p != %p",
                           (void *)types_as_type_param(types_object_type(v)),
                           (void *)T);
    arena_free(&ar);
}

static void TestIssue44410(TestingT *t) {
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    Str src = S("\npackage p\n\ntype A = []int\ntype S struct{ A }\n");

    TypesPackage *pkg = must_typecheck(a, src, NULL, NULL);

    TypesObject s = types_scope_lookup(types_package_scope(pkg), S("S"));
    if (s == NULL) {
        arena_free(&ar);
        testing_t_fatal_v(t, "object S not found");
    }

    Str got = object_string(a, s);
    Str want = S("type p.S struct{p.A}");
    if (!str_eq(got, want))
        testing_t_errorf_v(t, "got %q; want %q", got, want);
    arena_free(&ar);
}

static void TestIssue59831(TestingT *t) {
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);

    /* Package a exports a type S with an unexported method m; the tests check
     * the error messages when m is not found. */
    Str asrc = S("package a; type S struct{}; func (S) m() {}");
    TypesPackage *apkg = must_typecheck(a, asrc, NULL, NULL);

    /* Package b exports a type S with an exported method m; the tests check
     * the error messages when M is not found. */
    Str bsrc = S("package b; type S struct{}; func (S) M() {}");
    TypesPackage *bpkg = must_typecheck(a, bsrc, NULL, NULL);

    /* Not in Go: imported is 'a' for apkg, 'b' for bpkg and 0 for nothing. */
    static const struct {
        char imported;
        Str src, err;
    } tests[] = {
        /* tests importing a (or nothing) */
        {'a', S_("package a1; import \"a\"; var _ interface { M() } = a.S{}"),
         S_("a.S does not implement interface{M()} (missing method M) have m() want "
            "M()")},

        {'a', S_("package a2; import \"a\"; var _ interface { m() } = a.S{}"),
         S_("a.S does not implement interface{m()} (unexported method m)")}, /* test for
                                                                                issue */

        {0,
         S_("package a3; type S struct{}; func (S) m(); var _ interface { M() } = S{}"),
         S_("S does not implement interface{M()} (missing method M) have m() want "
            "M()")},

        {0,
         S_("package a4; type S struct{}; func (S) m(); var _ interface { m() } = S{}"),
         S_("")}, /* no error expected */

        {0,
         S_("package a5; type S struct{}; func (S) m(); var _ interface { n() } = S{}"),
         S_("S does not implement interface{n()} (missing method n)")},

        /* tests importing b (or nothing) */
        {'b', S_("package b1; import \"b\"; var _ interface { m() } = b.S{}"),
         S_("b.S does not implement interface{m()} (missing method m) have M() want "
            "m()")},

        {'b', S_("package b2; import \"b\"; var _ interface { M() } = b.S{}"),
         S_("")}, /* no error expected */

        {0,
         S_("package b3; type S struct{}; func (S) M(); var _ interface { M() } = S{}"),
         S_("")}, /* no error expected */

        {0,
         S_("package b4; type S struct{}; func (S) M(); var _ interface { m() } = S{}"),
         S_("S does not implement interface{m()} (missing method m) have M() want "
            "m()")},

        {0,
         S_("package b5; type S struct{}; func (S) M(); var _ interface { n() } = S{}"),
         S_("S does not implement interface{n()} (missing method n)")},
    };

    for (Int i = 0; i < NELEM(tests); i++) {
        TypesPackage *imported = NULL;
        if (tests[i].imported == 'a')
            imported = apkg;
        else if (tests[i].imported == 'b')
            imported = bpkg;

        /* typecheck test source */
        ImportHelper h = {imported, {0}};
        TypesConfig conf = {0};
        conf.importer = import_helper(&h);
        Error err = BURROW_NO_ERROR;
        TypesPackage *pkg = typecheck(a, tests[i].src, &conf, NULL, &err);
        Str name = types_package_name(pkg);
        if (BURROW_OK(err)) {
            if (tests[i].err.len != 0)
                testing_t_errorf_v(t, "package %s: got no error, want %q", name,
                                   tests[i].err);
            continue;
        }
        if (tests[i].err.len == 0)
            testing_t_errorf_v(t, "package %s: got %q, want not error", name,
                               error_text(err));

        /* flatten reported error message */
        Str errmsg = strings_replace_all(a, error_text(err), S("\n"), S(" "));
        errmsg = strings_replace_all(a, errmsg, S("\t"), S(""));

        /* verify error message */
        if (!strings_contains(errmsg, tests[i].err))
            testing_t_errorf_v(t, "package %s: got %q, want %q", name, errmsg,
                               tests[i].err);
    }
    arena_free(&ar);
}

static void TestIssue64759(TestingT *t) {
    (void)t;
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    Str src =
        S("\n//go:build go1.18\npackage p\n\nfunc f[S ~[]E, E any](S) {}\n\nfunc _() "
          "{\n\tf([]string{})\n}\n");
    /* Per the go:build directive, the source must typecheck even though the
     * (module) Go version is set to go1.17. */
    TypesConfig conf = {0};
    conf.go_version = S("go1.17");
    (void)must_typecheck(a, src, &conf, NULL);
    arena_free(&ar);
}

static void TestIssue68334(TestingT *t) {
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    Str src =
        S("\npackage p\n\nfunc f(x int) {\n\tfor i, j := range x {\n\t\t_, _ = i, "
          "j\n\t}\n\tvar a, b int\n\tfor a, b = range x {\n\t\t_, _ = a, b\n\t}\n}\n");

    GotErrors g = {a, S("")};
    TypesConfig conf = {0};
    conf.go_version = S("go1.21"); /* #68334 requires GoVersion <= 1.21 */
    conf.error = (TypesErrorFunc){append_error, &g}; /* #68334 requires Error != nil */
    Error err = BURROW_NO_ERROR;
    (void)typecheck(a, src, &conf, NULL, &err); /* do not crash */

    Str want =
        S("p:5:20: cannot range over x (variable of type int): requires go1.22 or "
          "later\np:9:19: cannot range over x (variable of type int): requires "
          "go1.22 or later\n");
    if (!str_eq(g.got, want))
        testing_t_errorf_v(t, "got: %s want: %s", g.got, want);
    arena_free(&ar);
}

static void TestIssue68877(TestingT *t) {
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    Str src = S("\npackage p\n\ntype (\n\tS struct{}\n\tA = S\n\tT A\n)");

    TypesPackage *pkg = must_typecheck(a, src, NULL, NULL);
    TypesTypeName *T =
        types_as_type_name(types_scope_lookup(types_package_scope(pkg), S("T")));
    Str got = object_string(a, types_object(T)); /* this must not panic (was issue) */
    Str want = S("type p.T struct{}");
    if (!str_eq(got, want))
        testing_t_errorf_v(t, "got %s, want %s", got, want);
    arena_free(&ar);
}

static void TestIssue69092(TestingT *t) {
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    Str src = S("\npackage p\n\nvar _ = T{{x}}\n");

    TokenFileSet *fset = token_new_file_set(a);
    AstFile *file = must_parse(a, fset, src);
    TypesConfig conf = {0};
    conf.error = (TypesErrorFunc){ignore_error, NULL}; /* ignore errors */
    TypesInfo info = {0};
    info.types = new_types_map(a);
    Error err = BURROW_NO_ERROR;
    (void)types_config_check(&conf, a, S("p"), fset, one_file(a, file), &info, &err);

    /* look for {x} expression */
    AstGenDecl *decl = BURROW_AT(AstGenDecl *, file->decls, 0);
    AstValueSpec *spec = BURROW_AT(AstValueSpec *, decl->specs, 0);
    AstCompositeLit *outer = BURROW_AT(AstCompositeLit *, spec->values, 0); /* T{{x}} */
    AstExpr inner = BURROW_AT(AstExpr, outer->elts, 0);                     /* {x} */

    /* type of {x} must have been recorded */
    TypesTypeAndValue tv;
    if (!map_get2(info.types, &inner, &tv)) {
        arena_free(&ar);
        testing_t_fatal_v(t, "no type found for {x}");
    }
    if (tv.type != typ(TYPES_INVALID))
        testing_t_errorf_v(t, "unexpected type for {x}: %s", type_string(a, tv.type));
    arena_free(&ar);
}

static void TestIssue72978(TestingT *t) {
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    Str src =
        S("\npackage p\n\ntype (\n\tgenericG[T, U any] struct { x T; y U }\n\tG1 "
          "genericG[int, string]\n\tG2 = G1\n)\n\nfunc genericF[T, U any]() {}\nvar "
          "f = genericF[string, float64]\n");

    TypesPackage *pkg = must_typecheck(a, src, NULL, NULL);
    static const Str names[] = {S_("G1"), S_("G2"), S_("f")};
    for (Int i = 0; i < NELEM(names); i++) {
        Str name = names[i];
        TypesType ty =
            types_object_type(types_scope_lookup(types_package_scope(pkg), name));
        Error err = BURROW_NO_ERROR;
        (void)types_instantiate(a, NULL, ty, ptrs2(a, typ(TYPES_BOOL), typ(TYPES_INT)),
                                true, &err);
        if (BURROW_OK(err)) {
            testing_t_errorf_v(t, "%s[bool, int]: got no error", name);
            continue;
        }
        Str want = fmt_sprintf_v(
            a, "cannot instantiate non-generic %s: has no type parameters",
            type_string(a, ty));
        if (!str_eq(error_text(err), want))
            testing_t_errorf_v(t, "%s[bool, int]: got %q, want %q", name,
                               error_text(err), want);
    }
    arena_free(&ar);
}

#define TESTS(X)                                                                       \
    X(TestIssue5770)                                                                   \
    X(TestIssue5849)                                                                   \
    X(TestIssue6413)                                                                   \
    X(TestIssue7245)                                                                   \
    X(TestIssue7827)                                                                   \
    X(TestIssue13898)                                                                  \
    X(TestIssue22525)                                                                  \
    X(TestIssue25627)                                                                  \
    X(TestIssue28005)                                                                  \
    X(TestIssue28282)                                                                  \
    X(TestIssue29029)                                                                  \
    X(TestIssue34151)                                                                  \
    X(TestIssue34921)                                                                  \
    X(TestIssue43088)                                                                  \
    X(TestIssue44515)                                                                  \
    X(TestIssue55030)                                                                  \
    X(TestIssue51093)                                                                  \
    X(TestIssue54258)                                                                  \
    X(TestIssue61931)                                                                  \
    X(TestIssue61938)                                                                  \
    X(TestIssue63260)                                                                  \
    X(TestIssue44410)                                                                  \
    X(TestIssue59831)                                                                  \
    X(TestIssue64759)                                                                  \
    X(TestIssue68334)                                                                  \
    X(TestIssue68877)                                                                  \
    X(TestIssue69092)                                                                  \
    X(TestIssue72978)

TESTING_MAIN(TESTS)
