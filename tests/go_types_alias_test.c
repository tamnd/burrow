/* Derived from Go's src/go/types/alias_test.go. Go source: go1.27.1.
 *
 * Copyright 2026 The Go Authors. All rights reserved.
 * Copyright 2026 The burrow Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style licence that can be found
 * in the LICENSE file. */

#include "check.h"
#include "go_types_helpers.h"

#include "burrow/burrow.h"
#include "burrow/go/types.h"
#include "burrow/mem/arena.h"

#define S(lit) BURROW_S(lit)

static Str type_string(Alloc *a, TypesType t) {
    return types_type_string(a, t, TYPES_NO_QUALIFIER);
}

static void TestIssue74181(TestingT *t) {
    Str src =
        S("package p\n\ntype AB = A[B]\n\ntype _ struct {\n\t_ AB\n}\n\ntype B struct "
          "{\n\tf *AB\n}\n\ntype A[T any] struct{}\n");

    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    TypesPackage *pkg = must_typecheck(a, src, NULL, NULL);
    TypesType b =
        types_object_type(types_scope_lookup(types_package_scope(pkg), S("B")));
    TypesNamed *n = types_as_named(b);
    if (n != NULL) {
        TypesStruct *s = types_as_struct(types_type_underlying(b));
        if (s != NULL) {
            TypesType got = types_object_type(types_object(types_struct_field(s, 0)));
            TypesType want = types_type(
                types_new_pointer(a, types_object_type(types_scope_lookup(
                                         types_package_scope(pkg), S("AB")))));
            if (!types_identical(got, want))
                testing_t_errorf_v(t, "wrong type for f: got %s, want %s",
                                   type_string(a, got), type_string(a, want));
            arena_free(&ar);
            return;
        }
    }
    testing_t_errorf_v(t, "unexpected type for B: %s", type_string(a, b));
    arena_free(&ar);
}

static void TestPartialTypeCheckUndeclaredAliasPanic(TestingT *t) {
    Str src = S("package p\n\ntype A = B // undeclared\n");

    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    Error err = BURROW_NO_ERROR;
    TypesPackage *pkg = typecheck(a, src, NULL, NULL, &err); /* don't panic on error */
    TypesType ta =
        types_object_type(types_scope_lookup(types_package_scope(pkg), S("A")));
    TypesAlias *alias = types_as_alias(ta);
    if (alias != NULL) {
        TypesType got = types_alias_rhs(alias);
        TypesType want = types_type(types_typ[TYPES_INVALID]);

        if (!types_identical(got, want))
            testing_t_errorf_v(t, "wrong type for B: got %s, want %s",
                               type_string(a, got), type_string(a, want));
        arena_free(&ar);
        return;
    }
    testing_t_errorf_v(t, "unexpected type for A: %s", type_string(a, ta));
    arena_free(&ar);
}

#define TESTS(X)                                                                       \
    X(TestIssue74181)                                                                  \
    X(TestPartialTypeCheckUndeclaredAliasPanic)

TESTING_MAIN(TESTS)
