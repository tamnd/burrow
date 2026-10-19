/* Derived from Go's src/go/types/lookup_test.go. Go source: go1.27.1.
 *
 * tests/go_types_lookup_test.c has the lookup tests from api_test.go and
 * methodset_test.go. This file has the benchmark from lookup_test.go, which
 * needs the source importer.
 *
 * Copyright 2022 The Go Authors. All rights reserved.
 * Copyright 2026 The burrow Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style licence that can be found
 * in the LICENSE file. */

#include "check.h"
#include "go_types_helpers.h"

#include "burrow/burrow.h"
#include "burrow/go/token.h"
#include "burrow/go/types.h"
#include "burrow/mem/arena.h"
#include "burrow/path/filepath.h"

#define S(lit) BURROW_S(lit)

typedef struct {
    Alloc *a;
    TypesPackage *pkg;
    TypesScope *scope;
    Slice names;
} LookupEnv;

/* Look up an arbitrary name for each type referenced in the package scope. */
static void lookup(LookupEnv *env) {
    for (Int i = 0; i < env->names.len; i++) {
        TypesType typ = types_object_type(
            types_scope_lookup(env->scope, BURROW_AT(Str, env->names, i)));
        (void)types_lookup_field_or_method(env->a, typ, true, env->pkg, S("m"), NULL,
                                           NULL);
    }
}

static void BenchmarkLookupFieldOrMethod(TestingB *b) {
    /* Not in Go: burrow imports from source, so it needs Go's sources. */
    if (!have_source_importer())
        testing_b_skip_v(b, "no Go sources to import from");

    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);

    /* Choose an arbitrary, large package. */
    Str path = filepath_join_v(a, 3, gtt_find_goroot(a), S("src"), S("net/http"));

    TokenFileSet *fset = token_new_file_set(a);
    Error err = BURROW_NO_ERROR;
    Slice files = pkg_files(a, fset, path, &err);
    if (BURROW_FAILED(err)) {
        testing_b_fatal_v(b, error_text(err));
        arena_free(&ar);
        return;
    }

    TypesConfig conf = {
        .importer = default_importer(fset),
    };

    TypesPackage *pkg =
        types_config_check(&conf, a, S("http"), fset, files, NULL, &err);
    if (BURROW_FAILED(err)) {
        testing_b_fatal_v(b, error_text(err));
        arena_free(&ar);
        return;
    }

    TypesScope *scope = types_package_scope(pkg);
    LookupEnv env = {a, pkg, scope, types_scope_names(a, scope)};

    /* Perform a lookup once, to ensure that any lazily-evaluated state is
     * complete. */
    lookup(&env);

    testing_b_reset_timer(b);
    for (Int i = 0; i < testing_b_n(b); i++)
        lookup(&env);
    arena_free(&ar);
}

#define TESTS(X) X(BenchmarkLookupFieldOrMethod)

TESTING_MAIN(TESTS)
