/* Derived from Go's src/go/types/methodset_test.go. Go source: go1.27.1.
 *
 * TestNewMethodSet and TestIssue60634 are in tests/go_types_lookup_test.c,
 * which builds its types by hand. This file has the one that needs the
 * checker to instantiate generic types.
 *
 * Copyright 2021 The Go Authors. All rights reserved.
 * Copyright 2026 The burrow Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style licence that can be found
 * in the LICENSE file. */

#include "check.h"
#include "go_types_helpers.h"

#include "burrow/burrow.h"
#include "burrow/go/types.h"
#include "burrow/mem/arena.h"

#define S(lit) BURROW_S(lit)

static void TestNewMethodSet_RecursiveGeneric(TestingT *t) {
    Str src = S("\n"
                "package pkg\n"
                "\n"
                "type Tree[T any] struct {\n"
                "\t*Node[T]\n"
                "}\n"
                "\n"
                "type Node[T any] struct {\n"
                "\t*Tree[T]\n"
                "}\n"
                "\n"
                "type Instance = *Tree[int]\n");

    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    TokenFileSet *fset = token_new_file_set(a);
    AstFile *f = must_parse(a, fset, src);

    TypesPackage *pkg = types_new_package(a, S("pkg"), f->name->name);
    Slice files = slice_append(a, slice_nil(TYPE_AST_FILE_PTR), &f, 1);
    Error err = BURROW_NO_ERROR;
    if (!types_checker_files(types_new_checker(a, NULL, fset, pkg, NULL), files, &err))
        panic_str(error_text(err));

    TypesType T =
        types_object_type(types_scope_lookup(types_package_scope(pkg), S("Instance")));
    (void)types_new_method_set(a, T); /* verify that NewMethodSet terminates */
    (void)t;
    arena_free(&ar);
}

#define TESTS(X) X(TestNewMethodSet_RecursiveGeneric)

TESTING_MAIN(TESTS)
