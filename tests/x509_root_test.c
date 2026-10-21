/* crypto/x509 system root tests: Go's root_test.go, cert_pool_test.go and
 * platform_test.go.
 *
 * Go points certFiles and certDirectories somewhere else for a test by
 * assigning to them. Here burrow__x509_load_system_roots takes them as
 * arguments instead. Go's TestEnvVars reads test-file.crt out of the package
 * directory; here it is written to a directory of its own first. GODEBUG is
 * set with burrow__x509_godebug_set rather than through the environment.
 *
 * Some of Go's crypto/x509 tests are not anywhere in burrow's tests.
 * TestSetFallbackRoots re-runs the test binary in a new user and mount
 * namespace with an empty /etc. TestFallback here goes through the same
 * SetFallbackRoots paths without one.
 * TestNISTPKITSPolicy and TestX509Limbo read large test suites from outside
 * the package. The fips140v1.0 tests check a GODEBUG that pins an older FIPS
 * module, TestGob is about encoding/gob, and TestImports checks Go's import
 * graph.
 *
 * Copyright 2022 The Go Authors. All rights reserved.
 * Copyright 2026 The burrow Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style licence that can be found
 * in the LICENSE file. */

#include "burrow/crypto/x509.h"

#include "burrow/bytes.h"
#include "burrow/crypto/ecdsa.h"
#include "burrow/crypto/tls.h"
#include "burrow/encoding/pem.h"
#include "burrow/error.h"
#include "burrow/fmt.h"
#include "burrow/io/fs.h"
#include "burrow/math/big.h"
#include "burrow/mem/arena.h"
#include "burrow/mem/heap.h"
#include "burrow/os.h"
#include "burrow/panic.h"
#include "burrow/path/filepath.h"
#include "burrow/strings.h"
#include "burrow/testing.h"
#include "burrow/time.h"

#include "../src/crypto/x509_internal.h"
#include "check.h"
#include "x509_root_test_gen.h"

#include <stdint.h>
#include <string.h>

/* ----------------------------------------------------------------- helpers */

#define ARENA(a)                                                                       \
    Arena ar;                                                                          \
    arena_init(&ar, NULL, 0);                                                          \
    Alloc *a = arena_allocator(&ar)

#define LEN(x) ((Int)(sizeof(x) / sizeof((x)[0])))

static Slice bytes_of(Alloc *a, const void *b, Int n) {
    return slice_append(a, slice_nil(TYPE_BYTE), b, n);
}

static Slice cstr_bytes(const char *s) {
    return slice_from((void *)(uintptr_t)s, (Int)strlen(s), (Int)strlen(s), TYPE_BYTE);
}

static bool recovered(void (*f)(void *), void *env) {
    volatile bool got = false;
    BURROW_TRY {
        f(env);
    }
    BURROW_CATCH(r) {
        (void)r;
        got = true;
    }
    BURROW_TRY_END;
    return got;
}

/* A directory to remove when the test ends. The cleanup runs after the test
 * has freed its arena, so this comes from the heap with an arena of its own. */
typedef struct TempDir {
    Arena arena;
    Str path;
} TempDir;

static void remove_tree(void *env) {
    TempDir *d = env;
    (void)os_remove_all(d->path);
    arena_free(&d->arena);
    mem_free(heap_allocator(), d, sizeof *d, _Alignof(TempDir));
}

/* t.TempDir: a new directory, removed when the test ends. */
static Str temp_dir(TestingT *t, Alloc *a) {
    Error e = BURROW_NO_ERROR;
    Str path = os_mkdir_temp(a, BURROW_STR_EMPTY, BURROW_S("burrow-x509-root-*"), &e);
    if (BURROW_FAILED(e))
        testing_t_fatalf_v(t, "MkdirTemp: %s", e);
    TempDir *d = mem_alloc(heap_allocator(), sizeof *d, _Alignof(TempDir));
    if (d == NULL)
        testing_t_fatal_v(t, "out of memory");
    arena_init(&d->arena, NULL, 0);
    d->path = str_clone(arena_allocator(&d->arena), path);
    testing_t_cleanup(t, BURROW_FN(Func, remove_tree, d));
    return path;
}

/* What set_env replaced. It runs after the test has freed its arena, so it
 * has an arena of its own and comes from the heap. */
typedef struct SavedEnv {
    Arena arena;
    Str key, value;
    bool found;
} SavedEnv;

static void restore_env(void *env) {
    SavedEnv *s = env;
    if (s->found)
        (void)os_setenv(s->key, s->value);
    else
        (void)os_unsetenv(s->key);
    arena_free(&s->arena);
    mem_free(heap_allocator(), s, sizeof *s, _Alignof(SavedEnv));
}

/* t.Setenv: key set to value until the test ends. */
static void set_env(TestingT *t, Str key, Str value) {
    SavedEnv *s = mem_alloc(heap_allocator(), sizeof *s, _Alignof(SavedEnv));
    if (s == NULL)
        testing_t_fatal_v(t, "out of memory");
    arena_init(&s->arena, NULL, 0);
    Alloc *a = arena_allocator(&s->arena);
    s->key = str_clone(a, key);
    s->value = os_lookup_env(a, key, &s->found);
    testing_t_cleanup(t, BURROW_FN(Func, restore_env, s));
    Error e = os_setenv(key, value);
    if (BURROW_FAILED(e))
        testing_t_fatalf_v(t, "setenv %q failed: %v", key, e);
}

static void reset_godebug(void *env) {
    (void)env;
    burrow__x509_godebug_set(NULL);
}

/* testenv.SetGODEBUG */
static void set_godebug(TestingT *t, const char *value) {
    burrow__x509_godebug_set(value);
    testing_t_cleanup(t, BURROW_FN(Func, reset_godebug, NULL));
}

static Slice strs(Alloc *a, Int n, const Str *s) {
    return slice_append(a, slice_nil(TYPE_STRING), s, n);
}

static const Str cert_file_env = BURROW_S_INIT("SSL_CERT_FILE");
static const Str cert_dir_env = BURROW_S_INIT("SSL_CERT_DIR");

/* ---------------------------------------------------------- root_test.go */

static void set_nil_fallback_roots(void *env) {
    (void)env;
    x509_set_fallback_roots(NULL);
}

static void TestFallbackPanic(TestingT *t) {
    /* Go calls SetFallbackRoots(nil) twice, and the first call is the one that
     * panics. */
    if (!recovered(set_nil_fallback_roots, NULL))
        testing_t_fatal_v(t, "Multiple calls to SetFallbackRoots should panic");
}

typedef struct FallbackCase {
    const char *name;
    bool system_roots; /* systemRoots is a NewCertPool, not nil */
    bool system_pool;
    bool pool_content; /* poolContent holds one empty Certificate */
    bool force_fallback;
    bool returns_fallback;
} FallbackCase;

static const FallbackCase fallback_tests[] = {
    {"nil systemRoots", false, false, false, false, true},
    {"empty systemRoots", true, false, false, false, true},
    {"empty systemRoots system pool", true, true, false, false, false},
    {"filled systemRoots system pool", true, true, true, false, false},
    {"filled systemRoots", true, false, true, false, false},
    {"filled systemRoots, force fallback", true, false, true, true, true},
    {"filled systemRoot system pool, force fallback", true, true, true, true, true},
};

static void fallback_case(void *env, TestingT *t) {
    const FallbackCase *tc = &fallback_tests[(intptr_t)env];
    ARENA(a);
    static const X509Certificate empty = {0};
    burrow__x509_reset_fallbacks();
    X509CertPool *roots = tc->system_roots ? x509_new_cert_pool(a) : NULL;
    if (roots != NULL && tc->system_pool)
        burrow__x509_cert_pool_set_system(roots);
    if (tc->pool_content)
        x509_cert_pool_add_cert(roots, &empty);
    (void)burrow__x509_swap_system_roots(roots);
    burrow__x509_godebug_set(tc->force_fallback ? "x509usefallbackroots=1"
                                                : "x509usefallbackroots=0");

    X509CertPool *fallback_pool = x509_new_cert_pool(a);
    x509_set_fallback_roots(fallback_pool);

    bool system_pool_is_fallback = burrow__x509_system_roots_pool() == fallback_pool;
    burrow__x509_godebug_set(NULL);
    (void)burrow__x509_swap_system_roots(NULL);

    if (tc->returns_fallback && !system_pool_is_fallback)
        testing_t_error_v(t, "systemRoots was not set to fallback pool");
    else if (!tc->returns_fallback && system_pool_is_fallback)
        testing_t_error_v(
            t, "systemRoots was set to fallback pool when it shouldn't have been");
    arena_free(&ar);
}

static void TestFallback(TestingT *t) {
    /* Go copies the pool systemRoots points at and points it at the copy at
     * the end. The pool here is put back as it is, since nothing changes
     * it. */
    (void)burrow__x509_system_roots_pool();
    X509CertPool *original = burrow__x509_swap_system_roots(NULL);
    for (Int i = 0; i < LEN(fallback_tests); i++)
        testing_t_run(t, str_from_cstr(fallback_tests[i].name),
                      BURROW_FN(TestingTFunc, fallback_case, (void *)(intptr_t)i));
    (void)burrow__x509_swap_system_roots(original);
    burrow__x509_reset_fallbacks();
}

#define TEST_DIR_CN "test-dir"
#define TEST_FILE_CN "test-file"
#define TEST_MISSING "missing"

/* The Str fields name a file or directory that the test fills in: "file" is
 * the test-file.crt that Go reads from the package directory and "dir" is
 * tmpDir. */
typedef struct EnvVarsCase {
    const char *name;
    const char *file_env, *dir_env;
    const char *files[1];
    const char *dirs[1];
    const char *cns[2];
} EnvVarsCase;

static const EnvVarsCase env_vars_tests[] = {
    /* Environment variables override the default locations preventing fall
     * through. */
    {"override-defaults", TEST_MISSING, TEST_MISSING, {"file"}, {"dir"}, {NULL}},
    /* File environment overrides default file locations. */
    {"file", "file", "", {NULL}, {NULL}, {TEST_FILE_CN}},
    /* Directory environment overrides default directory locations. */
    {"dir", "", "dir", {NULL}, {NULL}, {TEST_DIR_CN}},
    /* File & directory environment overrides both default locations. */
    {"file+dir", "file", "dir", {NULL}, {NULL}, {TEST_FILE_CN, TEST_DIR_CN}},
    /* Environment variable empty / unset uses default locations. */
    {"empty-fall-through", "", "", {"file"}, {"dir"}, {TEST_FILE_CN, TEST_DIR_CN}},
};

typedef struct EnvVarsEnv {
    Int i;
    Str test_file, tmp_dir;
} EnvVarsEnv;

/* name as a path: "file" and "dir" are the ones made for the test. */
static Str env_path(const EnvVarsEnv *e, const char *name) {
    if (strcmp(name, "file") == 0)
        return e->test_file;
    if (strcmp(name, "dir") == 0)
        return e->tmp_dir;
    return str_from_cstr(name);
}

static Slice env_paths(Alloc *a, const EnvVarsEnv *e, const char *const *names) {
    Slice s = slice_nil(TYPE_STRING);
    if (names[0] != NULL) {
        Str p = env_path(e, names[0]);
        s = strs(a, 1, &p);
    }
    return s;
}

static void env_vars_case(void *env, TestingT *t) {
    const EnvVarsEnv *e = env;
    const EnvVarsCase *tc = &env_vars_tests[e->i];
    ARENA(a);
    set_env(t, cert_file_env, env_path(e, tc->file_env));
    set_env(t, cert_dir_env, env_path(e, tc->dir_env));

    Error err = BURROW_NO_ERROR;
    X509CertPool *r = burrow__x509_load_system_roots(a, env_paths(a, e, tc->files),
                                                     env_paths(a, e, tc->dirs), &err);
    if (BURROW_FAILED(err))
        testing_t_fatal_v(t, "unexpected failure:", err);
    if (r == NULL)
        testing_t_fatal_v(t, "nil roots");

#if defined(BURROW_OS_DARWIN) || defined(BURROW_OS_IOS) || defined(BURROW_OS_WINDOWS)
    bool want_system_pool = tc->dir_env[0] == '\0' && tc->file_env[0] == '\0';
#else
    bool want_system_pool = false;
#endif
    Int n = burrow__x509_cert_pool_len(r);
    if (want_system_pool) {
        if (!burrow__x509_cert_pool_is_system(r))
            testing_t_fatal_v(t, "expected returned cert pool to be a system pool");
        if (n != 0)
            testing_t_fatalf_v(t, "expected empty system pool, pool has %d roots", n);
        arena_free(&ar);
        return;
    }

    /* Verify that the returned certs match, otherwise report where the
     * mismatch is. */
    Int ncns = 0;
    for (Int i = 0; i < LEN(tc->cns) && tc->cns[i] != NULL; i++, ncns++) {
        Str cn = str_from_cstr(tc->cns[i]);
        if (i >= n) {
            testing_t_errorf_v(t, "missing cert %v @ %v", cn, i);
            continue;
        }
        const X509Certificate *c = burrow__x509_cert_pool_cert(r, i, NULL);
        if (!str_eq(c->subject.common_name, cn))
            testing_t_errorf_v(t, "unexpected cert common name %q, want %q",
                               c->subject.common_name, cn);
    }
    if (n > ncns)
        testing_t_errorf_v(t, "got %v certs, which is more than %v wanted", n, ncns);
    arena_free(&ar);
}

static void TestEnvVars(TestingT *t) {
    ARENA(a);
    Str tmp_dir = temp_dir(t, a);
    Str pkg_dir = temp_dir(t, a);
    Str test_file = BURROW_S("test-file.crt");
    Error err = os_write_file(filepath_join_v(a, 2, tmp_dir, test_file),
                              cstr_bytes(test_dir_crt), 0644);
    if (BURROW_FAILED(err))
        testing_t_fatalf_v(t, "failed to write test cert: %s", err);
    EnvVarsEnv e = {0, filepath_join_v(a, 2, pkg_dir, test_file), tmp_dir};
    err = os_write_file(e.test_file, cstr_bytes(test_file_crt), 0644);
    if (BURROW_FAILED(err))
        testing_t_fatalf_v(t, "failed to write test cert: %s", err);

    for (Int i = 0; i < LEN(env_vars_tests); i++) {
        e.i = i;
        testing_t_run(t, str_from_cstr(env_vars_tests[i].name),
                      BURROW_FN(TestingTFunc, env_vars_case, &e));
    }
    arena_free(&ar);
}

static Str str_cert_pool(Alloc *a, const X509CertPool *p) {
    Slice b = bytes_join(a, x509_cert_pool_subjects(p, a), cstr_bytes("\n"));
    return str_from_bytes(b.p, b.len);
}

/* Ensure that "SSL_CERT_DIR" when used as the environment variable delimited
 * by colons on Unix-like systems, and semicolons on Windows, allows
 * loadSystemRoots to load all the roots from the respective directories. See
 * https://golang.org/issue/35325. */
static void TestLoadSystemCertsLoadColonSeparatedDirs(TestingT *t) {
    ARENA(a);
    /* To prevent any other certs from being loaded in through "SSL_CERT_FILE"
     * or from known "certFiles", clear them all. */
    set_env(t, cert_file_env, BURROW_STR_EMPTY);
    set_env(t, cert_dir_env, BURROW_STR_EMPTY);

    Str tmp_dir = temp_dir(t, a);
    const char *const root_pems[] = {gts_root, google_leaf};

    Slice cert_dirs = slice_nil(TYPE_STRING);
    for (Int i = 0; i < LEN(root_pems); i++) {
        Str cert_dir = filepath_join_v(a, 2, tmp_dir, fmt_sprintf_v(a, "cert-%d", i));
        Error err = os_mkdir_all(cert_dir, 0755);
        if (BURROW_FAILED(err))
            testing_t_fatalf_v(t, "failed to create certificate dir: %v", err);
        Str cert_out_file = filepath_join_v(a, 2, cert_dir, BURROW_S("cert.crt"));
        err = os_write_file(cert_out_file, cstr_bytes(root_pems[i]), 0655);
        if (BURROW_FAILED(err))
            testing_t_fatalf_v(t, "failed to write certificate to file: %v", err);
        cert_dirs = slice_append(a, cert_dirs, &cert_dir, 1);
    }

    /* Sanity check: the number of certDirs should be equal to the number of
     * roots. */
    if (cert_dirs.len != LEN(root_pems))
        testing_t_fatalf_v(
            t,
            "failed sanity check: len(certsDir)=%d is not equal to len(rootsPEMS)=%d",
            cert_dirs.len, LEN(root_pems));

    /* Now finally concatenate them with a colon/semicolon. */
    Byte sep = FILEPATH_LIST_SEPARATOR;
    Str concat_cert_dirs = strings_join(a, cert_dirs, str_from_bytes(&sep, 1));
    Error err = os_setenv(cert_dir_env, concat_cert_dirs);
    if (BURROW_FAILED(err))
        testing_t_fatalf_v(t, "setenv: %v", err);
    X509CertPool *got_pool = burrow__x509_load_system_roots(
        a, slice_nil(TYPE_STRING), slice_nil(TYPE_STRING), &err);
    if (BURROW_FAILED(err))
        testing_t_fatalf_v(t, "failed to load system roots: %v", err);
    Slice subjects = x509_cert_pool_subjects(got_pool, a);
    /* We expect exactly len(rootPEMs) subjects back. */
    if (subjects.len != LEN(root_pems))
        testing_t_fatalf_v(t, "invalid number of subjects: got %d want %d",
                           subjects.len, LEN(root_pems));

    X509CertPool *want_pool = x509_new_cert_pool(a);
    for (Int i = 0; i < LEN(root_pems); i++)
        x509_cert_pool_append_certs_from_pem(want_pool, cstr_bytes(root_pems[i]));

    if (!x509_cert_pool_equal(got_pool, want_pool))
        testing_t_fatalf_v(t, "mismatched certPools\nGot:\n%s\n\nWant:\n%s",
                           str_cert_pool(a, got_pool), str_cert_pool(a, want_pool));
    arena_free(&ar);
}

static void TestReadUniqueDirectoryEntries(TestingT *t) {
    ARENA(a);
    Str base_tmp_dir = temp_dir(t, a);
    Error err = BURROW_NO_ERROR;
    OsFile *f =
        os_create(a, filepath_join_v(a, 2, base_tmp_dir, BURROW_S("file")), &err);
    if (BURROW_FAILED(err))
        testing_t_fatal_v(t, err);
    os_file_free(f);
    err = os_symlink(BURROW_S("target-in"),
                     filepath_join_v(a, 2, base_tmp_dir, BURROW_S("link-in")));
#if defined(BURROW_OS_WINDOWS)
    /* testenv.MustHaveSymlink */
    if (BURROW_FAILED(err))
        testing_t_skipf_v(t, "skipping test: cannot make symlinks: %v", err);
#endif
    if (BURROW_FAILED(err))
        testing_t_fatal_v(t, err);
    err = os_symlink(BURROW_S("../target-out"),
                     filepath_join_v(a, 2, base_tmp_dir, BURROW_S("link-out")));
    if (BURROW_FAILED(err))
        testing_t_fatal_v(t, err);
    Slice got = burrow__x509_read_unique_directory_entries(a, base_tmp_dir, &err);
    if (BURROW_FAILED(err))
        testing_t_fatal_v(t, err);
    Slice got_names = slice_nil(TYPE_STRING);
    for (Int i = 0; i < got.len; i++) {
        FsDirEntry fi = *(FsDirEntry *)slice_at(got, i);
        Str name = fi.vt->name(fi.data);
        got_names = slice_append(a, got_names, &name, 1);
    }
    const Str want[] = {BURROW_S("file"), BURROW_S("link-out")};
    Slice want_names = strs(a, LEN(want), want);
    bool equal = got_names.len == want_names.len;
    for (Int i = 0; equal && i < got_names.len; i++)
        equal = str_eq(((Str *)got_names.p)[i], want[i]);
    if (!equal)
        testing_t_errorf_v(t, "got %q; want %q", got_names, want_names);
    arena_free(&ar);
}

static void TestSSLCertEnvOverride(TestingT *t) {
    ARENA(a);
    set_godebug(t, "x509sslcertoverrideplatform=0");
    set_env(t, cert_file_env, BURROW_S("/tmp/nope"));
    set_env(t, cert_dir_env, BURROW_S("/tmp/nope"));

    Error err = BURROW_NO_ERROR;
    X509CertPool *p = burrow__x509_load_system_roots(a, slice_nil(TYPE_STRING),
                                                     slice_nil(TYPE_STRING), &err);
    if (BURROW_FAILED(err))
        testing_t_fatalf_v(t, "unexpected failure: %s", err);

    bool system_pool = burrow__x509_cert_pool_is_system(p);
#if defined(BURROW_OS_WINDOWS) || defined(BURROW_OS_DARWIN) || defined(BURROW_OS_IOS)
    if (!system_pool)
        testing_t_fatal_v(
            t, "x509sslcertoverrideplatform did not override SSL_CERT_{FILE,DIR}");
#else
    if (system_pool)
        testing_t_fatal_v(t, "x509sslcertoverrideplatform caused a systemPool to be "
                             "returned on OS other than windows or darwin");
#endif
    arena_free(&ar);
}

/* ----------------------------------------------------- cert_pool_test.go */

typedef struct PoolEqualCase {
    const char *name;
    int a, b; /* indexes into the pools TestCertPoolEqual makes */
    bool equal;
} PoolEqualCase;

enum {
    POOL_EMPTY,
    POOL_NON_SYSTEM_POPULATED,
    POOL_NON_SYSTEM_POPULATED_ALT,
    POOL_EMPTY_SYSTEM,
    POOL_POPULATED_SYSTEM,
    POOL_POPULATED_SYSTEM_ALT,
    POOL_NIL,
    POOL_COUNT,
};

static const PoolEqualCase pool_equal_tests[] = {
    {"two empty pools", POOL_EMPTY, POOL_EMPTY, true},
    {"one empty pool, one populated pool", POOL_EMPTY, POOL_NON_SYSTEM_POPULATED,
     false},
    {"two populated pools", POOL_NON_SYSTEM_POPULATED, POOL_NON_SYSTEM_POPULATED, true},
    {"two populated pools, different content", POOL_NON_SYSTEM_POPULATED,
     POOL_NON_SYSTEM_POPULATED_ALT, false},
    {"two empty system pools", POOL_EMPTY_SYSTEM, POOL_EMPTY_SYSTEM, true},
    {"one empty system pool, one populated system pool", POOL_EMPTY_SYSTEM,
     POOL_POPULATED_SYSTEM, false},
    {"two populated system pools", POOL_POPULATED_SYSTEM, POOL_POPULATED_SYSTEM, true},
    {"two populated pools, different content", POOL_POPULATED_SYSTEM,
     POOL_POPULATED_SYSTEM_ALT, false},
    {"two nil pools", POOL_NIL, POOL_NIL, true},
    {"one nil pool, one empty pool", POOL_NIL, POOL_EMPTY, false},
};

typedef struct PoolEqualEnv {
    Int i;
    X509CertPool *pools[POOL_COUNT];
} PoolEqualEnv;

static void pool_equal_case(void *env, TestingT *t) {
    const PoolEqualEnv *e = env;
    const PoolEqualCase *tc = &pool_equal_tests[e->i];
    bool equal = x509_cert_pool_equal(e->pools[tc->a], e->pools[tc->b]);
    if (equal != tc->equal)
        testing_t_errorf_v(t, "Unexpected Equal result: got %t, want %t", equal,
                           tc->equal);
}

static X509CertPool *system_cert_pool(TestingT *t, Alloc *a) {
    Error err = BURROW_NO_ERROR;
    X509CertPool *p = x509_system_cert_pool(a, &err);
    if (BURROW_FAILED(err))
        testing_t_fatal_v(t, err);
    return p;
}

static void TestCertPoolEqual(TestingT *t) {
    ARENA(a);
    static const Byte raw[] = {1, 2, 3}, raw_subject[] = {2};
    static const Byte other_raw[] = {9, 8, 7}, other_raw_subject[] = {8};
    X509Certificate *tc = BURROW_NEW(a, X509Certificate);
    tc->raw = bytes_of(a, raw, LEN(raw));
    tc->raw_subject = bytes_of(a, raw_subject, LEN(raw_subject));
    X509Certificate *other_tc = BURROW_NEW(a, X509Certificate);
    other_tc->raw = bytes_of(a, other_raw, LEN(other_raw));
    other_tc->raw_subject = bytes_of(a, other_raw_subject, LEN(other_raw_subject));

    PoolEqualEnv e = {0, {NULL}};
    e.pools[POOL_EMPTY] = x509_new_cert_pool(a);
    e.pools[POOL_NON_SYSTEM_POPULATED] = x509_new_cert_pool(a);
    x509_cert_pool_add_cert(e.pools[POOL_NON_SYSTEM_POPULATED], tc);
    e.pools[POOL_NON_SYSTEM_POPULATED_ALT] = x509_new_cert_pool(a);
    x509_cert_pool_add_cert(e.pools[POOL_NON_SYSTEM_POPULATED_ALT], other_tc);
    e.pools[POOL_EMPTY_SYSTEM] = system_cert_pool(t, a);
    e.pools[POOL_POPULATED_SYSTEM] = system_cert_pool(t, a);
    x509_cert_pool_add_cert(e.pools[POOL_POPULATED_SYSTEM], tc);
    e.pools[POOL_POPULATED_SYSTEM_ALT] = system_cert_pool(t, a);
    x509_cert_pool_add_cert(e.pools[POOL_POPULATED_SYSTEM_ALT], other_tc);

    for (Int i = 0; i < LEN(pool_equal_tests); i++) {
        e.i = i;
        testing_t_run(t, str_from_cstr(pool_equal_tests[i].name),
                      BURROW_FN(TestingTFunc, pool_equal_case, &e));
    }
    arena_free(&ar);
}

/* ------------------------------------------------------ platform_test.go */

/* In order to run this test suite locally, you need to insert the test root, at
 * the path below, into your trust store. This root is constrained such that it
 * should not be dangerous to local developers to trust, but care should be
 * taken when inserting it into the trust store not to give it increased
 * permissions.
 *
 * On macOS the certificate can be further constrained to only be valid for
 * 'SSL' in the certificate properties pane of the 'Keychain Access' program.
 *
 * On Windows the certificate can also be constrained to only server
 * authentication in the properties pane of the certificate in the
 * "Certificates" snap-in of mmc.exe.
 *
 * Not in Go: the root and its key are platform_root_cert and
 * platform_root_key in x509_root_test_gen.h, which tools/gen-x509-tests.sh
 * makes from Go's platform_root_cert.pem and platform_root_key.pem. Those files
 * are the ones to add to the trust store. */

typedef struct PlatformVerifier PlatformVerifier;

typedef struct PlatformVerifierTest {
    const char *name;
    X509ExtKeyUsage cert_eku;
    bool self_signed;
    const char *dns_name;
    Duration time; /* from now, and no time when 0 */
    X509ExtKeyUsage eku;
    bool has_eku;

    const char *expected_err;
    const char *windows_err;
    const char *macos_err;

    const PlatformVerifier *v;
} PlatformVerifierTest;

/* What the subtests use, which has to last until the last of them is done, as
 * they can run after the test function has returned. */
struct PlatformVerifier {
    Arena ar;
    X509Certificate *test_root;
    EcdsaPrivateKey *test_root_key;
    EcdsaPrivateKey *leaf_key;
    Time now;
    PlatformVerifierTest tests[10];
};

static void platform_verifier_free(void *env) {
    PlatformVerifier *v = env;
    arena_free(&v->ar);
    mem_free(heap_allocator(), v, sizeof *v, _Alignof(PlatformVerifier));
}

static const PlatformVerifierTest platform_verifier_tests[] = {
    {.name = "valid", .cert_eku = X509_EXT_KEY_USAGE_SERVER_AUTH},
    {.name = "valid (with name)",
     .cert_eku = X509_EXT_KEY_USAGE_SERVER_AUTH,
     .dns_name = "valid.testing.golang.invalid"},
    {.name = "valid (with time)",
     .cert_eku = X509_EXT_KEY_USAGE_SERVER_AUTH,
     .time = TIME_MINUTE * 30},
    {.name = "valid (with eku)",
     .cert_eku = X509_EXT_KEY_USAGE_SERVER_AUTH,
     .eku = X509_EXT_KEY_USAGE_SERVER_AUTH,
     .has_eku = true},
    {.name = "wrong name",
     .cert_eku = X509_EXT_KEY_USAGE_SERVER_AUTH,
     .dns_name = "invalid.testing.golang.invalid",
     .expected_err = "x509: certificate is valid for valid.testing.golang.invalid, not "
                     "invalid.testing.golang.invalid"},
    {.name = "expired (future)",
     .cert_eku = X509_EXT_KEY_USAGE_SERVER_AUTH,
     .time = TIME_HOUR * 2,
     .expected_err = "x509: certificate has expired or is not yet valid"},
    {.name = "expired (past)",
     .cert_eku = X509_EXT_KEY_USAGE_SERVER_AUTH,
     .time = TIME_HOUR * 2,
     .expected_err = "x509: certificate has expired or is not yet valid"},
    {.name = "self-signed",
     .cert_eku = X509_EXT_KEY_USAGE_SERVER_AUTH,
     .self_signed = true,
     .macos_err =
         "x509: \xe2\x80\x9cvalid.testing.golang.invalid\xe2\x80\x9d certificate "
         "is not trusted",
     .windows_err = "x509: certificate signed by unknown authority"},
    {.name = "non-specified KU",
     .cert_eku = X509_EXT_KEY_USAGE_SERVER_AUTH,
     .eku = X509_EXT_KEY_USAGE_EMAIL_PROTECTION,
     .has_eku = true,
     .expected_err = "x509: certificate specifies an incompatible key usage"},
    {.name = "non-nested KU",
     .cert_eku = X509_EXT_KEY_USAGE_EMAIL_PROTECTION,
     .macos_err =
         "x509: \xe2\x80\x9cvalid.testing.golang.invalid\xe2\x80\x9d certificate "
         "is not permitted for this usage",
     .windows_err = "x509: certificate specifies an incompatible key usage"},
};

/* Not in Go: each of Go's cases spells out the same template, apart from its
 * ExtKeyUsage, so here it is made once from the case. */
static X509Certificate platform_cert_template(Alloc *a,
                                              const PlatformVerifierTest *tc) {
    static const Str dns_name = BURROW_S_INIT("valid.testing.golang.invalid");
    X509Certificate c;
    memset(&c, 0, sizeof c);
    c.serial_number = big_new_int(a, 1);
    c.dns_names = slice_append(a, slice_nil(TYPE_STRING), &dns_name, 1);
    c.not_before = time_add(tc->v->now, -TIME_HOUR);
    c.not_after = time_add(tc->v->now, TIME_HOUR);
    c.ext_key_usage = slice_append(a, slice_nil(TYPE_INT), &tc->cert_eku, 1);
    return c;
}

static void platform_verifier_case(void *env, TestingT *t) {
    const PlatformVerifierTest *tc = env;
    const PlatformVerifier *v = tc->v;
    testing_t_parallel(t);
    ARENA(a);
    X509Certificate tmpl = platform_cert_template(a, tc);
    const X509Certificate *parent = v->test_root;
    if (tc->self_signed)
        parent = &tmpl;
    Error err = BURROW_NO_ERROR;
    Slice cert_der = x509_create_certificate(
        a, (IoReader){0}, &tmpl, parent, ecdsa_private_key_public(v->leaf_key),
        ecdsa_private_key_signer(v->test_root_key), &err);
    if (BURROW_FAILED(err)) {
        arena_free(&ar);
        testing_t_fatalf_v(t, "CreateCertificate failed: %s", err);
    }
    X509Certificate *cert = x509_parse_certificate(a, cert_der, &err);
    if (BURROW_FAILED(err)) {
        arena_free(&ar);
        testing_t_fatalf_v(t, "ParseCertificate failed: %s", err);
    }

    X509VerifyOptions opts;
    memset(&opts, 0, sizeof opts);
    if (tc->dns_name != NULL)
        opts.dns_name = str_from_cstr(tc->dns_name);
    if (tc->time != 0)
        opts.current_time = time_add(v->now, tc->time);
    if (tc->has_eku)
        opts.key_usages = slice_append(a, slice_nil(TYPE_INT), &tc->eku, 1);

    const char *expected_err = tc->expected_err;
    if (strcmp(burrow_os_name(), "darwin") == 0 && tc->macos_err != NULL)
        expected_err = tc->macos_err;
    else if (strcmp(burrow_os_name(), "windows") == 0 && tc->windows_err != NULL)
        expected_err = tc->windows_err;

    (void)x509_certificate_verify(cert, a, opts, &err);
    if (BURROW_FAILED(err) && expected_err == NULL)
        testing_t_errorf_v(t, "unexpected verification error: %s", err);
    else if (BURROW_FAILED(err) &&
             !strings_has_prefix(error_text(err), str_from_cstr(expected_err)))
        testing_t_errorf_v(t, "unexpected verification error: got %q, want %q",
                           error_text(err), str_from_cstr(expected_err));
    else if (!BURROW_FAILED(err) && expected_err != NULL)
        testing_t_errorf_v(t, "unexpected verification success: want %q",
                           str_from_cstr(expected_err));
    arena_free(&ar);
}

static void TestPlatformVerifier(TestingT *t) {
    if (strcmp(burrow_os_name(), "windows") != 0 &&
        strcmp(burrow_os_name(), "darwin") != 0) {
        testing_t_skip_v(t, "only tested on windows and darwin");
        return;
    }

    PlatformVerifier *v = BURROW_NEW(heap_allocator(), PlatformVerifier);
    if (v == NULL) {
        testing_t_fatal_v(t, "out of memory");
        return;
    }
    arena_init(&v->ar, NULL, 0);
    testing_t_cleanup(t, BURROW_FN(Func, platform_verifier_free, v));
    Alloc *a = arena_allocator(&v->ar);

    Error err = BURROW_NO_ERROR;
    PemBlock *b = pem_decode(a, cstr_bytes(platform_root_cert), NULL);
    if (b == NULL) {
        /* Not in Go: Go reads the file here, and would panic on b.Bytes if
         * there were no PEM block in it. */
        testing_t_fatal_v(t, "no PEM block in the test root");
        return;
    }
    v->test_root = x509_parse_certificate(a, b->bytes, &err);
    if (BURROW_FAILED(err)) {
        testing_t_fatalf_v(t, "failed to parse test root: %s", err);
        return;
    }

    b = pem_decode(a, cstr_bytes(platform_root_key), NULL);
    if (b == NULL) {
        /* Not in Go: Go reads the file here, and would panic on b.Bytes if
         * there were no PEM block in it. */
        testing_t_fatal_v(t, "no PEM block in the test key");
        return;
    }
    v->test_root_key = x509_parse_ec_private_key(a, b->bytes, &err);
    if (BURROW_FAILED(err)) {
        testing_t_fatalf_v(t, "failed to parse test key: %s", err);
        return;
    }

    X509VerifyOptions none;
    memset(&none, 0, sizeof none);
    (void)x509_certificate_verify(v->test_root, a, none, &err);
    if (BURROW_FAILED(err)) {
        testing_t_skipf_v(t, "test root is not in trust store, skipping (err: %q)",
                          error_text(err));
        return;
    }

    v->now = time_now();

    v->leaf_key = ecdsa_generate_key(a, elliptic_p256(), (IoReader){0}, &err);
    if (BURROW_FAILED(err)) {
        testing_t_fatalf_v(t, "ecdsa.GenerateKey failed: %s", err);
        return;
    }

    for (Int i = 0; i < LEN(platform_verifier_tests); i++) {
        v->tests[i] = platform_verifier_tests[i];
        v->tests[i].v = v;
        testing_t_run(t, str_from_cstr(v->tests[i].name),
                      BURROW_FN(TestingTFunc, platform_verifier_case, &v->tests[i]));
    }
}

/* Go's hybrid_pool_test.go. */

/* Not in Go: what TestHybridPool holds, which a fatal error must not leak. */
typedef struct {
    Arena ar;
    TlsConn *c;
} HybridPool;

static void hybrid_pool_free(void *env) {
    HybridPool *h = env;
    if (h->c != NULL) {
        (void)tls_conn_close(h->c);
        tls_conn_free(h->c);
    }
    arena_free(&h->ar);
    mem_free(heap_allocator(), h, sizeof *h, _Alignof(HybridPool));
}

static void TestHybridPool(TestingT *t) {
    testing_t_parallel(t);
    if (!(strcmp(burrow_os_name(), "windows") == 0 ||
          strcmp(burrow_os_name(), "darwin") == 0 ||
          strcmp(burrow_os_name(), "ios") == 0)) {
        testing_t_skipf_v(t, "platform verifier not available on %s",
                          str_from_cstr(burrow_os_name()));
        return;
    }
    /* Not in Go: what testenv.HasExternalNetwork asks here. */
    if (testing_short()) {
        testing_t_skip_now(t);
        return;
    }

    HybridPool *h = BURROW_NEW(heap_allocator(), HybridPool);
    if (h == NULL) {
        testing_t_fatal_v(t, "out of memory");
        return;
    }
    arena_init(&h->ar, NULL, 0);
    testing_t_cleanup(t, BURROW_FN(Func, hybrid_pool_free, h));
    Alloc *a = arena_allocator(&h->ar);
    Error err = BURROW_NO_ERROR;

    if (strcmp(burrow_os_name(), "windows") == 0) {
        /* NOTE(#51599): on the Windows builders we sometimes see that the state
         * of the root pool is not fully initialized, causing an expected
         * platform verification to fail. In part this is because Windows
         * dynamically populates roots into its local trust store at time of
         * use. We can attempt to prime the pool by attempting TLS connections
         * to google.com until it works, suggesting the pool has been properly
         * updated. If after we hit the deadline, the pool has _still_ not been
         * populated with the expected root, it's unlikely we are ever going to
         * get into a good state, and so we just fail the test. #52108 suggests
         * a better possible long term solution. */

        Time deadline = time_add(time_now(), TIME_SECOND * 10);
        Duration next_sleep = 10 * TIME_MILLISECOND;
        for (Duration i = 0;; i++) {
            err = BURROW_NO_ERROR;
            TlsConn *c = tls_dial(heap_allocator(), BURROW_S("tcp"),
                                  BURROW_S("google.com:443"), NULL, &err);
            if (!BURROW_FAILED(err)) {
                (void)tls_conn_close(c);
                tls_conn_free(c);
                break;
            }
            next_sleep = next_sleep * i;
            if (time_until(deadline) < next_sleep) {
                testing_t_fatal_v(t, "windows root pool appears to be in an "
                                     "uninitialized state (missing root that "
                                     "chains to google.com)");
                return;
            }
            time_sleep(next_sleep);
        }
    }

    /* Get the google.com chain, which should be valid on all platforms we
     * are testing */
    TlsConfig config;
    memset(&config, 0, sizeof config);
    config.insecure_skip_verify = true;
    err = BURROW_NO_ERROR;
    h->c = tls_dial(heap_allocator(), BURROW_S("tcp"), BURROW_S("google.com:443"),
                    &config, &err);
    tls_config_release(&config);
    if (BURROW_FAILED(err)) {
        h->c = NULL;
        testing_t_fatalf_v(t, "tls connection failed: %s", err);
        return;
    }
    Slice goog_chain = tls_conn_connection_state(h->c).peer_certificates;
    if (goog_chain.len == 0) {
        /* Not in Go: Go would panic on googChain[0]. */
        testing_t_fatal_v(t, "no peer certificates from google.com");
        return;
    }
    const X509Certificate *goog_leaf = *(X509Certificate *const *)goog_chain.p;

    Time now = time_now();
    X509Certificate root_tmpl;
    memset(&root_tmpl, 0, sizeof root_tmpl);
    root_tmpl.serial_number = big_new_int(a, 1);
    root_tmpl.subject.common_name = BURROW_S("Go test root");
    root_tmpl.is_ca = true;
    root_tmpl.basic_constraints_valid = true;
    root_tmpl.not_before = time_add(now, -TIME_HOUR);
    root_tmpl.not_after = time_add(now, TIME_HOUR * 10);
    EcdsaPrivateKey *k = ecdsa_generate_key(a, elliptic_p256(), (IoReader){0}, &err);
    if (BURROW_FAILED(err)) {
        testing_t_fatalf_v(t, "failed to generate test key: %s", err);
        return;
    }
    Slice root_der = x509_create_certificate(a, (IoReader){0}, &root_tmpl, &root_tmpl,
                                             ecdsa_private_key_public(k),
                                             ecdsa_private_key_signer(k), &err);
    if (BURROW_FAILED(err)) {
        testing_t_fatalf_v(t, "failed to create test cert: %s", err);
        return;
    }
    X509Certificate *root = x509_parse_certificate(a, root_der, &err);
    if (BURROW_FAILED(err)) {
        testing_t_fatalf_v(t, "failed to parse test cert: %s", err);
        return;
    }

    X509CertPool *pool = x509_system_cert_pool(a, &err);
    if (BURROW_FAILED(err)) {
        testing_t_fatalf_v(t, "SystemCertPool failed: %s", err);
        return;
    }
    X509VerifyOptions opts;
    memset(&opts, 0, sizeof opts);
    opts.roots = pool;

    (void)x509_certificate_verify(goog_leaf, a, opts, &err);
    if (BURROW_FAILED(err)) {
        testing_t_fatalf_v(
            t, "verification failed for google.com chain (system only pool): %s", err);
        return;
    }

    x509_cert_pool_add_cert(pool, root);

    (void)x509_certificate_verify(goog_leaf, a, opts, &err);
    if (BURROW_FAILED(err)) {
        testing_t_fatalf_v(
            t, "verification failed for google.com chain (hybrid pool): %s", err);
        return;
    }

    static const Str example = BURROW_S_INIT("example.com");
    X509Certificate cert_tmpl;
    memset(&cert_tmpl, 0, sizeof cert_tmpl);
    cert_tmpl.serial_number = big_new_int(a, 1);
    cert_tmpl.not_before = time_add(now, -TIME_HOUR);
    cert_tmpl.not_after = time_add(now, TIME_HOUR * 10);
    cert_tmpl.dns_names = slice_append(a, slice_nil(TYPE_STRING), &example, 1);
    Slice cert_der = x509_create_certificate(a, (IoReader){0}, &cert_tmpl, &root_tmpl,
                                             ecdsa_private_key_public(k),
                                             ecdsa_private_key_signer(k), &err);
    if (BURROW_FAILED(err)) {
        testing_t_fatalf_v(t, "failed to create test cert: %s", err);
        return;
    }
    X509Certificate *cert = x509_parse_certificate(a, cert_der, &err);
    if (BURROW_FAILED(err)) {
        testing_t_fatalf_v(t, "failed to parse test cert: %s", err);
        return;
    }

    (void)x509_certificate_verify(cert, a, opts, &err);
    if (BURROW_FAILED(err)) {
        testing_t_fatalf_v(t, "verification failed for custom chain (hybrid pool): %s",
                           err);
        return;
    }
}

#define TESTS(X)                                                                       \
    X(TestFallbackPanic)                                                               \
    X(TestFallback)                                                                    \
    X(TestEnvVars)                                                                     \
    X(TestLoadSystemCertsLoadColonSeparatedDirs)                                       \
    X(TestReadUniqueDirectoryEntries)                                                  \
    X(TestSSLCertEnvOverride)                                                          \
    X(TestCertPoolEqual)                                                               \
    X(TestPlatformVerifier)                                                            \
    X(TestHybridPool)

TESTING_MAIN(TESTS)
