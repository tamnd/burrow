/* Derived from Go's src/go/types/named_test.go. Go source: go1.27.1.
 *
 * Copyright 2022 The Go Authors. All rights reserved.
 * Copyright 2026 The burrow Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style licence that can be found
 * in the LICENSE file. */

#include "check.h"
#include "go_types_helpers.h"

#include "burrow/burrow.h"
#include "burrow/fmt.h"
#include "burrow/go/types.h"
#include "burrow/mem/arena.h"

#define S(lit) BURROW_S(lit)
#define NELEM(x) ((Int)(sizeof(x) / sizeof((x)[0])))

static TypesType must_instantiate(TestingB *b, Alloc *a, TypesType orig,
                                  TypesType targ) {
    Error err = BURROW_NO_ERROR;
    Slice targs = slice_append(a, slice_nil(TYPE_UNSAFE_POINTER), &targ, 1);
    TypesType inst = types_instantiate(a, NULL, orig, targs, true, &err);
    if (BURROW_FAILED(err))
        testing_b_fatal_v(b, error_text(err));
    return inst;
}

typedef struct {
    Str name;
    TypesType typ;
} NamedBench;

typedef struct {
    Alloc *a;
    NamedBench tests[4];
    Int i;
} NamedBenchEnv;

static void named_underlying_one(void *env, TestingB *b) {
    NamedBenchEnv *e = env;
    TypesType typ = e->tests[e->i].typ;
    /* Access underlying once, to trigger any lazy calculation. */
    (void)types_type_underlying(typ);
    testing_b_reset_timer(b);
    for (Int i = 0; i < testing_b_n(b); i++)
        (void)types_type_underlying(typ);
}

static void named_underlying(void *env, TestingB *b) {
    NamedBenchEnv *e = env;
    for (e->i = 0; e->i < NELEM(e->tests); e->i++)
        testing_b_run(b, e->tests[e->i].name,
                      BURROW_FN(TestingBFunc, named_underlying_one, e));
}

static void named_method_set_one(void *env, TestingB *b) {
    NamedBenchEnv *e = env;
    TypesType typ = e->tests[e->i].typ;
    /* Access underlying once, to trigger any lazy calculation. */
    Arena ar;
    arena_init(&ar, NULL, 0);
    (void)types_new_method_set(arena_allocator(&ar), typ);
    arena_free(&ar);
    testing_b_reset_timer(b);
    for (Int i = 0; i < testing_b_n(b); i++) {
        /* Not in Go: each set goes in an arena that is freed at once, where Go
         * leaves them to the collector. */
        arena_init(&ar, NULL, 0);
        (void)types_new_method_set(arena_allocator(&ar), typ);
        arena_free(&ar);
    }
}

static void named_method_set(void *env, TestingB *b) {
    NamedBenchEnv *e = env;
    for (e->i = 0; e->i < NELEM(e->tests); e->i++)
        testing_b_run(b, e->tests[e->i].name,
                      BURROW_FN(TestingBFunc, named_method_set_one, e));
}

static void BenchmarkNamed(TestingB *b) {
    Str src =
        S("\npackage p\n\ntype T struct {\n\tP int\n}\n\nfunc (T) M(int) {}\nfunc "
          "(T) N() (i int) { return }\n\ntype G[P any] struct {\n\tF P\n}\n\nfunc "
          "(G[P]) M(P) {}\nfunc (G[P]) N() (p P) { return }\n\ntype Inst = "
          "G[int]\n\t");
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    TypesPackage *pkg = must_typecheck(a, src, NULL, NULL);
    TypesScope *scope = types_package_scope(pkg);
    TypesType g = types_object_type(types_scope_lookup(scope, S("G")));
    NamedBenchEnv env = {
        a,
        {
            {S("nongeneric"), types_object_type(types_scope_lookup(scope, S("T")))},
            {S("generic"), g},
            {S("src instance"),
             types_object_type(types_scope_lookup(scope, S("Inst")))},
            {S("user instance"),
             must_instantiate(b, a, g, types_type(types_typ[TYPES_INT]))},
        },
        0,
    };

    testing_b_run(b, S("Underlying"), BURROW_FN(TestingBFunc, named_underlying, &env));
    testing_b_run(b, S("NewMethodSet"),
                  BURROW_FN(TestingBFunc, named_method_set, &env));
    arena_free(&ar);
}

static TypesNamed *first_field_type(TypesNamed *n) {
    TypesStruct *s = types_as_struct(types_type_underlying(types_type(n)));
    TypesType ft = types_object_type(types_object(types_struct_field(s, 0)));
    return types_as_named(types_pointer_elem(types_as_pointer(ft)));
}

/* Test that types do not expand infinitely, as in go.dev/issue/52715. */
static void TestFiniteTypeExpansion(TestingT *t) {
    Str src =
        S("\npackage p\n\ntype Tree[T any] struct {\n\t*Node[T]\n}\n\nfunc "
          "(*Tree[R]) N(r R) R { return r }\n\ntype Node[T any] struct {\n\t*Tree[T]"
          "\n}\n\nfunc (Node[Q]) M(Q) {}\n\ntype Inst = *Tree[int]\n");

    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    TokenFileSet *fset = token_new_file_set(a);
    AstFile *f = must_parse(a, fset, src);
    TypesPackage *pkg = types_new_package(a, S("p"), f->name->name);
    Error err = BURROW_NO_ERROR;
    Slice files = slice_append(a, slice_nil(TYPE_AST_FILE_PTR), &f, 1);
    if (!types_checker_files(types_new_checker(a, NULL, fset, pkg, NULL), files, &err))
        testing_t_fatal_v(t, error_text(err));

    TypesType inst_t = types_unalias(
        types_object_type(types_scope_lookup(types_package_scope(pkg), S("Inst"))));
    TypesNamed *inst = types_as_named(types_pointer_elem(types_as_pointer(inst_t)));
    TypesNamed *node = first_field_type(inst);
    TypesNamed *tree = first_field_type(node);
    if (!types_identical(types_type(inst), types_type(tree)))
        testing_t_fatalf_v(t, "Not a cycle: got %s, want %s",
                           types_type_string(a, types_type(tree), TYPES_NO_QUALIFIER),
                           types_type_string(a, types_type(inst), TYPES_NO_QUALIFIER));
    if (inst != tree)
        testing_t_errorf_v(
            t, "Duplicate instances in cycle: %s (%p) -> %s (%p) -> %s (%p)",
            types_type_string(a, types_type(inst), TYPES_NO_QUALIFIER), (void *)inst,
            types_type_string(a, types_type(node), TYPES_NO_QUALIFIER), (void *)node,
            types_type_string(a, types_type(tree), TYPES_NO_QUALIFIER), (void *)tree);
    arena_free(&ar);
}

/* TestMethodOrdering is a simple test verifying that the indices of methods of
 * a named type remain the same as long as the same source and AddMethod calls
 * are presented to the type checker in the same order (go.dev/issue/61298). */
static void TestMethodOrdering(TestingT *t) {
    Str src =
        S("\npackage p\n\ntype T struct{}\n\nfunc (T) a() {}\nfunc (T) c() {}\nfunc "
          "(T) b() {}\n");

    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    /* should get the same method order each time */
    Slice methods = slice_nil(TYPE_STRING);
    for (Int i = 0; i < 5; i++) {
        /* collect T methods as provided in src */
        TypesPackage *pkg = must_typecheck(a, src, NULL, NULL);
        TypesNamed *tn = types_as_named(
            types_object_type(types_scope_lookup(types_package_scope(pkg), S("T"))));

        /* add a few more methods manually */
        static const Str names[] = {BURROW_S_INIT("foo"), BURROW_S_INIT("bar"),
                                    BURROW_S_INIT("bal")};
        for (Int k = 0; k < NELEM(names); k++) {
            TypesFunc *m = types_new_func(a, TOKEN_NO_POS, pkg, names[k],
                                          NULL /* don't care about signature */);
            types_named_add_method(tn, m);
        }

        /* check method order */
        if (i == 0) {
            /* first round: collect methods in given order */
            for (Int j = 0; j < types_named_num_methods(tn); j++) {
                Str name = types_object_name(types_object(types_named_method(tn, j)));
                methods = slice_append(a, methods, &name, 1);
            }
        } else {
            /* successive rounds: methods must appear in the same order */
            Int got = types_named_num_methods(tn);
            if (got != methods.len) {
                testing_t_errorf_v(t, "got %d methods, want %d", got, methods.len);
                continue;
            }
            for (Int j = 0; j < methods.len; j++) {
                Str m = ((Str *)methods.p)[j];
                Str name = types_object_name(types_object(types_named_method(tn, j)));
                if (!str_eq(name, m))
                    testing_t_errorf_v(t, "got method %s, want %s", name, m);
            }
        }
    }
    arena_free(&ar);
}

#define TESTS(X)                                                                       \
    X(BenchmarkNamed)                                                                  \
    X(TestFiniteTypeExpansion)                                                         \
    X(TestMethodOrdering)

TESTING_MAIN(TESTS)
