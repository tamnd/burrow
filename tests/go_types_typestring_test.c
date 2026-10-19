/* Derived from Go's src/go/types/typestring_test.go. Go source: go1.27.1.
 *
 * Copyright 2012 The Go Authors. All rights reserved.
 * Copyright 2026 The burrow Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style licence that can be found
 * in the LICENSE file. */

#include "check.h"
#include "go_types_helpers.h"
#include "go_types_typestring.h"

#include "burrow/burrow.h"
#include "burrow/fmt.h"
#include "burrow/go/types.h"
#include "burrow/mem/arena.h"

#define S(lit) BURROW_S(lit)
#define S_(lit) BURROW_S_INIT(lit)
#define NELEM(x) ((Int)(sizeof(x) / sizeof((x)[0])))

/* types that depend on other type declarations (src in TestTypes) */
static const TestEntry dependent_test_types[] = {
    /* interfaces */
    {S_("interface{io.Reader; io.Writer}"), S_("interface{io.Reader; io.Writer}")},
    {S_("interface{m() int; io.Writer}"), S_("interface{m() int; io.Writer}")},
    {S_("interface{m() interface{T}}"), S_("interface{m() interface{p.T}}")},
};

static void type_string_check(TestingT *t, Alloc *a, const TestEntry *test) {
    Str src = fmt_sprintf_v(a, "package p; import \"io\"; type _ io.Writer; type T %s",
                            test->src);
    Error err = BURROW_NO_ERROR;
    TypesPackage *pkg = typecheck(a, src, NULL, NULL, &err);
    if (BURROW_FAILED(err)) {
        testing_t_errorf_v(t, "%s: %s", src, error_text(err));
        return;
    }
    TypesObject obj = types_scope_lookup(types_package_scope(pkg), S("T"));
    if (obj == NULL) {
        testing_t_errorf_v(t, "%s: T not found", test->src);
        return;
    }
    TypesType typ = types_type_underlying(types_object_type(obj));
    Str got = types_type_string(a, typ, TYPES_NO_QUALIFIER);
    if (!str_eq(got, test->str))
        testing_t_errorf_v(t, "%s: got %s, want %s", test->src, got, test->str);
}

static void TestTypeString(TestingT *t) {
    /* The Go command is needed for the importer to determine the locations of
     * stdlib .a files.
     *
     * Not in Go: burrow imports io from source, so it needs Go's sources. */
    if (!have_source_importer())
        testing_t_skip_v(t, "no Go sources to import io from");

    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    for (Int i = 0; i < NELEM(independent_test_types); i++)
        type_string_check(t, a, &independent_test_types[i]);
    for (Int i = 0; i < NELEM(dependent_test_types); i++)
        type_string_check(t, a, &dependent_test_types[i]);
    arena_free(&ar);
}

static Str this_qualifier(void *env, TypesPackage *pkg) {
    if (pkg != (TypesPackage *)env)
        return types_package_name(pkg);
    return S("");
}

/* Not in Go: what %s prints for a *Package, which is "<nil>" for nil. */
static Str package_string_or_nil(Alloc *a, TypesPackage *pkg) {
    return pkg == NULL ? S("<nil>") : types_package_string(a, pkg);
}

static void TestQualifiedTypeString(TestingT *t) {
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    TypesPackage *p = must_typecheck(a, S("package p; type T int"), NULL, NULL);
    TypesPackage *q = must_typecheck(a, S("package q"), NULL, NULL);

    TypesType p_t =
        types_object_type(types_scope_lookup(types_package_scope(p), S("T")));
    TypesType ptr_p_t = types_type(types_new_pointer(a, p_t));
    struct {
        TypesType typ;
        TypesPackage *this;
        Str want;
    } tests[] = {
        {NULL, NULL, S("<nil>")}, {p_t, NULL, S("p.T")}, {p_t, p, S("T")},
        {p_t, q, S("p.T")},       {ptr_p_t, p, S("*T")}, {ptr_p_t, q, S("*p.T")},
    };
    for (Int i = 0; i < NELEM(tests); i++) {
        TypesQualifier qualifier = {this_qualifier, tests[i].this};
        Str got = types_type_string(a, tests[i].typ, qualifier);
        if (!str_eq(got, tests[i].want))
            testing_t_errorf_v(t, "TypeString(%s, %s) = %s, want %s",
                               package_string_or_nil(a, tests[i].this),
                               types_type_string(a, tests[i].typ, TYPES_NO_QUALIFIER),
                               got, tests[i].want);
    }
    arena_free(&ar);
}

#define TESTS(X)                                                                       \
    X(TestTypeString)                                                                  \
    X(TestQualifiedTypeString)

TESTING_MAIN(TESTS)
