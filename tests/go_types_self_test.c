/* Derived from Go's src/go/types/self_test.go. Go source: go1.27.1.
 *
 * Copyright 2013 The Go Authors. All rights reserved.
 * Copyright 2026 The burrow Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style licence that can be found
 * in the LICENSE file. */

#include "check.h"
#include "go_types_helpers.h"

#include "burrow/burrow.h"
#include "burrow/go/ast.h"
#include "burrow/go/build.h"
#include "burrow/go/parser.h"
#include "burrow/go/token.h"
#include "burrow/go/types.h"
#include "burrow/map.h"
#include "burrow/mem/arena.h"
#include "burrow/path.h"
#include "burrow/path/filepath.h"
#include "burrow/time.h"

#define S(lit) BURROW_S(lit)
#define NELEM(x) ((Int)(sizeof(x) / sizeof((x)[0])))

/* Not in Go: the directory of the package with import path p in GOROOT/src. */
static Str goroot_src(Alloc *a, Str p) {
    return filepath_join_v(a, 3, gtt_find_goroot(a), S("src"), p);
}

static void TestSelf(TestingT *t) {
    /* The Go command is needed for the importer to determine the locations of
     * stdlib .a files.
     *
     * Not in Go: burrow imports from source, so it needs Go's sources. */
    if (!have_source_importer())
        testing_t_skip_v(t, "no Go sources to import from");

    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    TokenFileSet *fset = token_new_file_set(a);
    Error err = BURROW_NO_ERROR;
    Slice files = pkg_files(a, fset, goroot_src(a, S("go/types")), &err);
    if (BURROW_FAILED(err)) {
        testing_t_fatal_v(t, error_text(err));
        arena_free(&ar);
        return;
    }

    TypesConfig conf = {.importer = default_importer(fset)};
    types_config_check(&conf, a, S("go/types"), fset, files, NULL, &err);
    if (BURROW_FAILED(err))
        testing_t_fatal_v(t, error_text(err));
    arena_free(&ar);
}

static bool count_lines(void *env, TokenFile *f) {
    *(Int *)env += token_file_line_count(f);
    return true;
}

static void runbench(TestingB *b, Str path, bool ignore_func_bodies, bool write_info) {
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    TokenFileSet *fset = token_new_file_set(a);
    Error err = BURROW_NO_ERROR;
    Slice files = pkg_files(a, fset, path, &err);
    if (BURROW_FAILED(err)) {
        testing_b_fatal_v(b, error_text(err));
        arena_free(&ar);
        return;
    }
    /* determine line count */
    Int lines = 0;
    token_file_set_iterate(fset, BURROW_FN(TokenFileFunc, count_lines, &lines));

    testing_b_reset_timer(b);
    Time start = time_now();
    for (Int i = 0; i < testing_b_n(b); i++) {
        /* Not in Go: each check gets its own arena, which goes when it is done,
         * where Go leaves the garbage to the collector. */
        Arena run;
        arena_init(&run, NULL, 0);
        Alloc *ra = arena_allocator(&run);
        TypesConfig conf = {
            .ignore_func_bodies = ignore_func_bodies,
            .importer = default_importer(fset),
        };
        TypesInfo info0 = {0};
        TypesInfo *info = NULL;
        if (write_info) {
            info0.types =
                map_make(ra, TYPE_UNSAFE_POINTER, TYPE_TYPES_TYPE_AND_VALUE, 0);
            info0.defs = map_make(ra, TYPE_UNSAFE_POINTER, TYPE_UNSAFE_POINTER, 0);
            info0.uses = map_make(ra, TYPE_UNSAFE_POINTER, TYPE_UNSAFE_POINTER, 0);
            info0.implicits = map_make(ra, TYPE_UNSAFE_POINTER, TYPE_UNSAFE_POINTER, 0);
            info0.selections =
                map_make(ra, TYPE_UNSAFE_POINTER, TYPE_UNSAFE_POINTER, 0);
            info0.scopes = map_make(ra, TYPE_UNSAFE_POINTER, TYPE_UNSAFE_POINTER, 0);
            info = &info0;
        }
        types_config_check(&conf, ra, path, fset, files, info, &err);
        arena_free(&run);
        if (BURROW_FAILED(err)) {
            testing_b_fatal_v(b, error_text(err));
            break;
        }
    }
    testing_b_stop_timer(b);
    testing_b_report_metric(
        b, (double)lines * (double)testing_b_n(b) / duration_seconds(time_since(start)),
        S("lines/s"));
    arena_free(&ar);
}

typedef struct {
    Str path;
    bool ignore_func_bodies;
} BenchEnv;

static void bench_info(void *env, TestingB *b) {
    BenchEnv *e = env;
    runbench(b, e->path, e->ignore_func_bodies, true);
}

static void bench_noinfo(void *env, TestingB *b) {
    BenchEnv *e = env;
    runbench(b, e->path, e->ignore_func_bodies, false);
}

static void bench_bodies(void *env, TestingB *b) {
    testing_b_run(b, S("info"), BURROW_FN(TestingBFunc, bench_info, env));
    testing_b_run(b, S("noinfo"), BURROW_FN(TestingBFunc, bench_noinfo, env));
}

static void bench_package(void *env, TestingB *b) {
    Str path = *(Str *)env;
    bool ignore[] = {false, true};
    for (Int i = 0; i < NELEM(ignore); i++) {
        Str name = S("funcbodies");
        if (ignore[i])
            name = S("nofuncbodies");
        BenchEnv e = {path, ignore[i]};
        testing_b_run(b, name, BURROW_FN(TestingBFunc, bench_bodies, &e));
    }
}

static void BenchmarkCheck(TestingB *b) {
    /* The Go command is needed for the importer to determine the locations of
     * stdlib .a files.
     *
     * Not in Go: burrow imports from source, so it needs Go's sources. */
    if (!have_source_importer())
        testing_b_skip_v(b, "no Go sources to import from");

    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    Str pkgs[] = {
        S("net/http"),
        S("go/parser"),
        S("go/constant"),
        S("runtime"),
        filepath_join_v(a, 3, S("go"), S("internal"), S("gcimporter")),
    };
    for (Int i = 0; i < NELEM(pkgs); i++) {
        Str path = goroot_src(a, pkgs[i]);
        testing_b_run(b, path_base(pkgs[i]),
                      BURROW_FN(TestingBFunc, bench_package, &path));
    }
    arena_free(&ar);
}

#define TESTS(X)                                                                       \
    X(TestSelf)                                                                        \
    X(BenchmarkCheck)

TESTING_MAIN(TESTS)
