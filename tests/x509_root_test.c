/* crypto/x509 system root tests: Go's root_test.go and cert_pool_test.go.
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
 * SetFallbackRoots paths without one. TestHybridPool and TestPlatformVerifier
 * need the macOS and Windows platform verifiers, which are not done yet.
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
#include "burrow/error.h"
#include "burrow/fmt.h"
#include "burrow/io/fs.h"
#include "burrow/mem/arena.h"
#include "burrow/mem/heap.h"
#include "burrow/os.h"
#include "burrow/panic.h"
#include "burrow/path/filepath.h"
#include "burrow/strings.h"
#include "burrow/testing.h"

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

static void remove_tree(void *env) {
    (void)os_remove_all(*(Str *)env);
}

/* t.TempDir: a new directory, removed when the test ends. */
static Str temp_dir(TestingT *t, Alloc *a) {
    Error e = BURROW_NO_ERROR;
    Str *d = BURROW_NEW(a, Str);
    *d = os_mkdir_temp(a, BURROW_STR_EMPTY, BURROW_S("burrow-x509-root-*"), &e);
    if (BURROW_FAILED(e))
        testing_t_fatalf_v(t, "MkdirTemp: %s", e);
    testing_t_cleanup(t, BURROW_FN(Func, remove_tree, d));
    return *d;
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

#define TESTS(X)                                                                       \
    X(TestFallbackPanic)                                                               \
    X(TestFallback)                                                                    \
    X(TestEnvVars)                                                                     \
    X(TestLoadSystemCertsLoadColonSeparatedDirs)                                       \
    X(TestReadUniqueDirectoryEntries)                                                  \
    X(TestSSLCertEnvOverride)                                                          \
    X(TestCertPoolEqual)

TESTING_MAIN(TESTS)
