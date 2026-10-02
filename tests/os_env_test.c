/* Derived from Go's src/os/env_test.go, os_test.go, getwd_test.go,
 * pipe_test.go and file_test.go.
 * Go source: go1.27.1.
 *
 * The environment, Expand, the process ids, Hostname, Getwd and Chdir, the
 * user directories, Pipe, and File.Chmod, Chown and Chdir.
 *
 * Copyright 2010 The Go Authors. All rights reserved.
 * Copyright 2026 The burrow Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style licence that can be found
 * in the LICENSE file. */

#include "burrow/os.h"

#include "burrow/burrow.h"
#include "burrow/fmt.h"
#include "burrow/mem/arena.h"
#include "burrow/strings.h"
#include "burrow/syscall.h"

#include "check.h"

#include <string.h>

#define S(lit) BURROW_S(lit)

static Arena ar;
static Alloc *a;

static Str temp_dir(TestingT *t) {
    Error e = BURROW_NO_ERROR;
    Str d = os_mkdir_temp(a, S(""), S("burrow-os-env-test-*"), &e);
    if (BURROW_FAILED(e))
        testing_t_fatalf_v(t, "MkdirTemp: %s", error_text(e));
    return d;
}

static Str env_at(Slice s, Int i) {
    return *(Str *)slice_at(s, i);
}

/* -------------------------------------------------------------- Expand */

typedef struct {
    const char *in, *out;
} ExpandTest;

static const ExpandTest expand_tests[] = {
    {"", ""},
    {"$*", "all the args"},
    {"$$", "PID"},
    {"${*}", "all the args"},
    {"$1", "ARGUMENT1"},
    {"${1}", "ARGUMENT1"},
    {"now is the time", "now is the time"},
    {"$HOME", "/usr/gopher"},
    {"$home_1", "/usr/foo"},
    {"${HOME}", "/usr/gopher"},
    {"${H}OME", "(Value of H)OME"},
    {"A$$$#$1$H$home_1*B", "APIDNARGSARGUMENT1(Value of H)/usr/foo*B"},
    {"start$+middle$^end$", "start$+middle$^end$"},
    {"mixed$|bag$$$", "mixed$|bagPID$"},
    {"$", "$"},
    {"$}", "$}"},
    {"${", ""},  /* invalid syntax; eat up the characters */
    {"${}", ""}, /* invalid syntax; eat up the characters */
};

static Str test_getenv(void *env, Str s) {
    (void)env;
    static const struct {
        const char *k, *v;
    } vals[] = {
        {"*", "all the args"},  {"#", "NARGS"},          {"$", "PID"},
        {"1", "ARGUMENT1"},     {"HOME", "/usr/gopher"}, {"H", "(Value of H)"},
        {"home_1", "/usr/foo"}, {"_", "underscore"},
    };
    for (size_t i = 0; i < sizeof vals / sizeof vals[0]; i++)
        if (str_eq(s, str_from_cstr(vals[i].k)))
            return str_from_cstr(vals[i].v);
    return S("");
}

static void TestExpand(TestingT *t) {
    for (size_t i = 0; i < sizeof expand_tests / sizeof expand_tests[0]; i++) {
        const ExpandTest *tt = &expand_tests[i];
        Str result =
            os_expand(a, str_from_cstr(tt->in), BURROW_FN(StrFunc, test_getenv, NULL));
        if (!str_eq(result, str_from_cstr(tt->out)))
            testing_t_errorf_v(t, "Expand(%q)=%q; expected %q", str_from_cstr(tt->in),
                               result, str_from_cstr(tt->out));
    }
}

static void TestExpandEnv(TestingT *t) {
    CHECK(BURROW_OK(os_setenv(S("BURROW_TEST_EXPAND"), S("gopher"))));
    Str got = os_expand_env(a, S("hello ${BURROW_TEST_EXPAND}, $BURROW_TEST_EXPAND!"));
    if (!str_eq(got, S("hello gopher, gopher!")))
        testing_t_errorf_v(t, "ExpandEnv = %q", got);
    got = os_expand_env(a, S("$BURROW_TEST_NOT_SET_ANYWHERE."));
    if (!str_eq(got, S(".")))
        testing_t_errorf_v(t, "ExpandEnv of an unset name = %q, want \".\"", got);
    CHECK(BURROW_OK(os_unsetenv(S("BURROW_TEST_EXPAND"))));
}

/* --------------------------------------------------------- environment */

static void TestConsistentEnviron(TestingT *t) {
    Slice e0 = os_environ(a);
    for (int i = 0; i < 10; i++) {
        Slice e1 = os_environ(a);
        bool same = e0.len == e1.len;
        for (Int j = 0; same && j < e0.len; j++)
            same = str_eq(env_at(e0, j), env_at(e1, j));
        if (!same)
            testing_t_fatalf_v(t, "environment changed");
    }
}

static bool env_has(Str prefix) {
    Slice env = os_environ(a);
    for (Int i = 0; i < env.len; i++)
        if (strings_has_prefix(env_at(env, i), prefix))
            return true;
    return false;
}

static void TestUnsetenv(TestingT *t) {
    if (BURROW_FAILED(os_setenv(S("GO_TEST_UNSETENV"), S("1"))))
        testing_t_fatalf_v(t, "Setenv failed");
    if (!env_has(S("GO_TEST_UNSETENV=")))
        testing_t_errorf_v(t, "Setenv didn't set TestUnsetenv");
    if (BURROW_FAILED(os_unsetenv(S("GO_TEST_UNSETENV"))))
        testing_t_fatalf_v(t, "Unsetenv failed");
    if (env_has(S("GO_TEST_UNSETENV=")))
        testing_t_fatalf_v(t, "Unsetenv didn't clear TestUnsetenv");
}

static void TestLookupEnv(TestingT *t) {
    bool ok = true;
    Str value = os_lookup_env(a, S("SMALLPOX"), &ok);
    if (ok || value.len != 0)
        testing_t_fatalf_v(t, "SMALLPOX=%q", value);
    if (BURROW_FAILED(os_setenv(S("SMALLPOX"), S("virus"))))
        testing_t_fatalf_v(t, "failed to release smallpox virus");
    value = os_lookup_env(a, S("SMALLPOX"), &ok);
    if (!ok || !str_eq(value, S("virus")))
        testing_t_errorf_v(
            t, "smallpox release failed; world remains safe but LookupEnv is "
               "broken");
    CHECK(BURROW_OK(os_unsetenv(S("SMALLPOX"))));

    /* Set to the empty string is still set. */
    CHECK(BURROW_OK(os_setenv(S("BURROW_TEST_EMPTY"), S(""))));
    value = os_lookup_env(a, S("BURROW_TEST_EMPTY"), &ok);
    CHECK(ok && value.len == 0);
    CHECK(BURROW_OK(os_unsetenv(S("BURROW_TEST_EMPTY"))));
    os_lookup_env(a, S("BURROW_TEST_EMPTY"), &ok);
    CHECK(!ok);
}

/* Setenv refuses what the system cannot hold, and says so as Go does. */
static void TestSetenvInvalid(TestingT *t) {
    static const char nul_key[] = {'A', 0, 'B'};
    Str keys[] = {
        str_from_bytes((const Byte *)nul_key, 3),
#if !defined(BURROW_OS_WINDOWS)
        S(""),
        S("A=B"),
#endif
    };
    for (size_t i = 0; i < sizeof keys / sizeof keys[0]; i++) {
        Error e = os_setenv(keys[i], S("v"));
        if (!str_eq(error_text(e), S("setenv: invalid argument")))
            testing_t_errorf_v(t, "Setenv(%q) error = %q", keys[i], error_text(e));
    }
}

/* The environment as Environ gives it is what LookupEnv sees, and setting a
 * variable to what it already is works. On Windows this covers the "=C:"
 * names. */
static void TestEnvironConsistency(TestingT *t) {
    Slice env = os_environ(a);
    for (Int i = 0; i < env.len; i++) {
        Str kv = env_at(env, i);
        Str k = kv;
        Str v = S("");
        if (kv.len > 1) {
            Int j = strings_index(str_from_bytes(kv.p + 1, kv.len - 1), S("="));
            if (j >= 0) {
                k = str_from_bytes(kv.p, j + 1);
                v = str_from_bytes(kv.p + j + 2, kv.len - j - 2);
            }
        }
        bool ok = false;
        Str v2 = os_lookup_env(a, k, &ok);
        if (!ok)
            testing_t_errorf_v(
                t, "Environ contains %q, but LookupEnv(%q) reports not present", kv, k);
        else if (!str_eq(v, v2))
            testing_t_errorf_v(t, "Environ contains %q, but LookupEnv(%q) = %q", kv, k,
                               v2);
        Error e = os_setenv(k, v);
        if (BURROW_FAILED(e))
            testing_t_errorf_v(t, "Environ contains %q, but Setenv(%q, %q) = %v", kv, k,
                               v, e);
    }
}

/* Clearenv runs last, putting back what was there. */
static void TestClearenv(TestingT *t) {
    Slice orig = os_environ(a);
    if (BURROW_FAILED(os_setenv(S("GO_TEST_CLEARENV"), S("1"))))
        testing_t_fatalf_v(t, "Setenv failed");
    bool ok = false;
    os_lookup_env(a, S("GO_TEST_CLEARENV"), &ok);
    if (!ok)
        testing_t_errorf_v(t, "Setenv didn't set $GO_TEST_CLEARENV");
    os_clearenv();
    Str val = os_lookup_env(a, S("GO_TEST_CLEARENV"), &ok);
    if (ok)
        testing_t_errorf_v(
            t, "Clearenv() didn't clear $GO_TEST_CLEARENV, remained with value %q",
            val);
    for (Int i = 0; i < orig.len; i++) {
        Str kv = env_at(orig, i);
        Int j = kv.len > 1 ? strings_index(str_from_bytes(kv.p + 1, kv.len - 1), S("="))
                           : -1;
        if (j >= 0)
            (void)os_setenv(str_from_bytes(kv.p, j + 1),
                            str_from_bytes(kv.p + j + 2, kv.len - j - 2));
    }
    CHECK(os_environ(a).len == orig.len);
}

/* ------------------------------------------------------------- process */

static void TestArgs(TestingT *t) {
    Slice args = os_args();
    CHECK(args.len >= 1);
    if (args.len >= 1 && env_at(args, 0).len == 0)
        testing_t_errorf_v(t, "Args[0] is empty");
}

static void TestIds(TestingT *t) {
    CHECK(os_getpid() > 0);
    CHECK(os_getppid() != os_getpid());
#if defined(BURROW_OS_WINDOWS)
    /* The parent comes from a process snapshot here, and Go gives -1 when the
     * snapshot does not have it, so this only checks the range. */
    testing_t_logf_v(t, "Getppid = %d", os_getppid());
    CHECK(os_getppid() >= -1);
#else
    CHECK(os_getppid() > 0);
#endif
    Int ps = os_getpagesize();
    CHECK(ps >= 4096 && (ps & (ps - 1)) == 0);
    Error e = BURROW_NO_ERROR;
    Slice groups = os_getgroups(a, &e);
#if defined(BURROW_OS_WINDOWS)
    CHECK(os_getuid() == -1 && os_geteuid() == -1);
    CHECK(os_getgid() == -1 && os_getegid() == -1);
    CHECK(groups.len == 0);
    CHECK(str_eq(error_text(e), S("getgroups: not supported by windows")));
#else
    CHECK(os_getuid() >= 0 && os_geteuid() >= 0);
    CHECK(os_getgid() >= 0 && os_getegid() >= 0);
    if (BURROW_FAILED(e))
        testing_t_errorf_v(t, "Getgroups: %v", e);
    for (Int i = 0; i < groups.len; i++)
        CHECK(*(Int *)slice_at(groups, i) >= 0);
#endif
}

static void TestHostname(TestingT *t) {
    Error e = BURROW_NO_ERROR;
    Str h = os_hostname(a, &e);
    if (BURROW_FAILED(e))
        testing_t_fatalf_v(t, "Hostname: %v", e);
    if (h.len == 0)
        testing_t_fatalf_v(t, "Hostname returned an empty string and no error");
    if (memchr(h.p, 0, (size_t)h.len) != NULL)
        testing_t_fatalf_v(t, "unexpected zero byte in hostname: %q", h);
}

static void TestChdirAndGetwd(TestingT *t) {
    Error e = BURROW_NO_ERROR;
    Str orig = os_getwd(a, &e);
    if (BURROW_FAILED(e))
        testing_t_fatalf_v(t, "Getwd: %v", e);
    Str d = temp_dir(t);
    Str sub = fmt_sprintf_v(a, "%s%csub", d, (Int)OS_PATH_SEPARATOR);
    CHECK(BURROW_OK(os_mkdir(sub, 0755)));
    Str dirs[] = {d, sub, orig};
    for (size_t i = 0; i < sizeof dirs / sizeof dirs[0]; i++) {
        e = os_chdir(dirs[i]);
        if (BURROW_FAILED(e)) {
            testing_t_errorf_v(t, "Chdir %q: %v", dirs[i], e);
            continue;
        }
        Str pwd = os_getwd(a, &e);
        if (BURROW_FAILED(e)) {
            testing_t_errorf_v(t, "Getwd in %q: %v", dirs[i], e);
            continue;
        }
        OsFileInfo want = os_stat(a, dirs[i], &e);
        OsFileInfo got = os_stat(a, pwd, &e);
        if (BURROW_FAILED(e) || !os_same_file(want, got))
            testing_t_errorf_v(t, "Getwd in %q gave %q", dirs[i], pwd);
    }
    e = os_chdir(S("burrow-no-such-dir"));
    if (!os_is_not_exist(e))
        testing_t_errorf_v(t, "Chdir to a missing dir = %v", e);
    else if (!strings_has_prefix(error_text(e), S("chdir burrow-no-such-dir: ")))
        testing_t_errorf_v(t, "Chdir error = %q", error_text(e));
    CHECK(BURROW_OK(os_chdir(orig)));
    CHECK(BURROW_OK(os_remove_all(d)));
}

static void TestFileChdir(TestingT *t) {
    Error e = BURROW_NO_ERROR;
    Str orig = os_getwd(a, &e);
    if (BURROW_FAILED(e))
        testing_t_fatalf_v(t, "Getwd: %v", e);
    Str d = temp_dir(t);
    OsFile *fd = os_open(a, d, &e);
    if (BURROW_FAILED(e))
        testing_t_fatalf_v(t, "Open %q: %v", d, e);
    e = os_file_chdir(fd);
    if (BURROW_FAILED(e))
        testing_t_fatalf_v(t, "File.Chdir: %v", e);
    Str wd = os_getwd(a, &e);
    CHECK(BURROW_OK(e));
    CHECK(BURROW_OK(os_chdir(orig)));
    OsFileInfo fi1 = os_stat(a, d, &e);
    OsFileInfo fi2 = os_stat(a, wd, &e);
    if (BURROW_FAILED(e) || !os_same_file(fi1, fi2))
        testing_t_errorf_v(t, "fd.Chdir failed: got %q, want %q", wd, d);
    CHECK(BURROW_OK(os_file_close(fd)));
    e = os_file_chdir(fd);
    if (!errors_is(e, os_err_closed))
        testing_t_errorf_v(t, "Chdir after Close = %v, want ErrClosed", e);
    os_file_free(fd);
    CHECK(BURROW_OK(os_remove_all(d)));
}

static void TestUserDirs(TestingT *t) {
    Error e = BURROW_NO_ERROR;
    Str home = os_user_home_dir(a, &e);
    if (BURROW_FAILED(e)) {
        testing_t_logf_v(t, "UserHomeDir: %v", e);
    } else {
        OsFileInfo fi = os_stat(a, home, &e);
        if (BURROW_FAILED(e))
            testing_t_logf_v(t, "UserHomeDir %q is not there: %v", home, e);
        else if (!fi.vt->is_dir(fi.data))
            testing_t_errorf_v(t, "UserHomeDir %q is not a directory", home);
    }
#if !defined(BURROW_OS_WINDOWS) && !defined(BURROW_OS_ANDROID) &&                      \
    !defined(BURROW_OS_IOS)
    bool had = false;
    Str saved = os_lookup_env(a, S("HOME"), &had);
    CHECK(BURROW_OK(os_unsetenv(S("HOME"))));
    os_user_home_dir(a, &e);
    if (!str_eq(error_text(e), S("$HOME is not defined")))
        testing_t_errorf_v(t, "UserHomeDir without $HOME: %v", e);
    if (had)
        CHECK(BURROW_OK(os_setenv(S("HOME"), saved)));
#endif
#if defined(BURROW_OS_LINUX) || defined(BURROW_OS_FREEBSD)
    Str xdg = os_lookup_env(a, S("XDG_CACHE_HOME"), &had);
    CHECK(BURROW_OK(os_setenv(S("XDG_CACHE_HOME"), S("relative/dir"))));
    os_user_cache_dir(a, &e);
    if (!str_eq(error_text(e), S("path in $XDG_CACHE_HOME is relative")))
        testing_t_errorf_v(t, "UserCacheDir with a relative $XDG_CACHE_HOME: %v", e);
    CHECK(BURROW_OK(os_setenv(S("XDG_CACHE_HOME"), S("/cache/here"))));
    Str c = os_user_cache_dir(a, &e);
    if (BURROW_FAILED(e) || !str_eq(c, S("/cache/here")))
        testing_t_errorf_v(t, "UserCacheDir = %q, %v", c, e);
    if (had)
        CHECK(BURROW_OK(os_setenv(S("XDG_CACHE_HOME"), xdg)));
    else
        CHECK(BURROW_OK(os_unsetenv(S("XDG_CACHE_HOME"))));
#endif
}

/* ---------------------------------------------------------------- Pipe */

static void TestPipe(TestingT *t) {
    OsFile *w = NULL;
    Error e = BURROW_NO_ERROR;
    OsFile *r = os_pipe(a, &w, &e);
    if (BURROW_FAILED(e))
        testing_t_fatalf_v(t, "Pipe: %v", e);
    CHECK(str_eq(os_file_name(r), S("|0")));
    CHECK(str_eq(os_file_name(w), S("|1")));
    Int n = os_file_write_string(w, S("hello, pipe"), &e);
    CHECK(n == 11 && BURROW_OK(e));
    CHECK(BURROW_OK(os_file_close(w)));
    Byte buf[64];
    Int got = 0;
    while (got < (Int)sizeof buf) {
        Int room = (Int)sizeof buf - got;
        got += os_file_read(r, slice_from(buf + got, room, room, TYPE_BYTE), &e);
        if (BURROW_FAILED(e))
            break;
    }
    if (!errors_is(e, io_eof))
        testing_t_errorf_v(t, "read after the writer closed: %v, want EOF", e);
    if (!str_eq(str_from_bytes(buf, got), S("hello, pipe")))
        testing_t_errorf_v(t, "read %q", str_from_bytes(buf, got));
    CHECK(BURROW_OK(os_file_close(r)));
    os_file_free(r);
    os_file_free(w);
}

/* ------------------------------------------------- File.Chmod and Chown */

static void TestFileChmod(TestingT *t) {
    Str d = temp_dir(t);
    Str name = fmt_sprintf_v(a, "%s%cf", d, (Int)OS_PATH_SEPARATOR);
    Error e = BURROW_NO_ERROR;
    OsFile *f = os_create(a, name, &e);
    if (BURROW_FAILED(e))
        testing_t_fatalf_v(t, "Create: %v", e);
#if defined(BURROW_OS_WINDOWS)
    OsFileMode modes[] = {0444, 0666};
#else
    OsFileMode modes[] = {0456, 0123, 0600};
#endif
    for (size_t i = 0; i < sizeof modes / sizeof modes[0]; i++) {
        e = os_file_chmod(f, modes[i]);
        if (BURROW_FAILED(e)) {
            testing_t_errorf_v(t, "File.Chmod %o: %v", (Int)modes[i], e);
            continue;
        }
        OsFileInfo fi = os_stat(a, name, &e);
        CHECK(BURROW_OK(e));
        if (BURROW_OK(e) && (fi.vt->mode(fi.data) & 0777) != modes[i])
            testing_t_errorf_v(t, "File.Chmod %o gave mode %o", (Int)modes[i],
                               (Int)(fi.vt->mode(fi.data) & 0777));
    }
    e = os_file_chown(f, -1, -1);
#if defined(BURROW_OS_WINDOWS)
    if (!strings_has_suffix(error_text(e), S(": not supported by windows")))
        testing_t_errorf_v(t, "File.Chown on Windows = %v", e);
    CHECK(BURROW_OK(os_file_chmod(f, 0666)));
#else
    if (BURROW_FAILED(e))
        testing_t_errorf_v(t, "File.Chown(-1, -1): %v", e);
    e = os_file_chown(f, os_getuid(), os_getgid());
    if (BURROW_FAILED(e))
        testing_t_errorf_v(t, "File.Chown to ourselves: %v", e);
    /* Lchown on a link changes the link and leaves the target alone. */
    Str link = fmt_sprintf_v(a, "%s%clink", d, (Int)OS_PATH_SEPARATOR);
    CHECK(BURROW_OK(os_symlink(name, link)));
    e = os_lchown(link, os_getuid(), os_getgid());
    if (BURROW_FAILED(e))
        testing_t_errorf_v(t, "Lchown to ourselves: %v", e);
#endif
    CHECK(BURROW_OK(os_file_close(f)));
    e = os_file_chmod(f, 0644);
    if (!errors_is(e, os_err_closed))
        testing_t_errorf_v(t, "Chmod after Close = %v, want ErrClosed", e);
    else if (!strings_has_prefix(error_text(e), S("chmod ")))
        testing_t_errorf_v(t, "Chmod after Close: %q", error_text(e));
    os_file_free(f);
    e = os_lchown(fmt_sprintf_v(a, "%s%cmissing", d, (Int)OS_PATH_SEPARATOR), -1, -1);
#if defined(BURROW_OS_WINDOWS)
    CHECK(strings_has_suffix(error_text(e), S(": not supported by windows")));
#else
    CHECK(os_is_not_exist(e));
#endif
    CHECK(BURROW_OK(os_remove_all(d)));
}

static void setup(void) {
    arena_init(&ar, NULL, 0);
    a = arena_allocator(&ar);
}

#define TESTS(X)                                                                       \
    X(TestExpand)                                                                      \
    X(TestExpandEnv)                                                                   \
    X(TestConsistentEnviron)                                                           \
    X(TestUnsetenv)                                                                    \
    X(TestLookupEnv)                                                                   \
    X(TestSetenvInvalid)                                                               \
    X(TestEnvironConsistency)                                                          \
    X(TestArgs)                                                                        \
    X(TestIds)                                                                         \
    X(TestHostname)                                                                    \
    X(TestChdirAndGetwd)                                                               \
    X(TestFileChdir)                                                                   \
    X(TestUserDirs)                                                                    \
    X(TestPipe)                                                                        \
    X(TestFileChmod)                                                                   \
    X(TestClearenv)

static int os_env_main(TestingM *m) {
    setup();
    int r = testing_m_run(m);
    arena_free(&ar);
    return r;
}

TESTING_MAIN_WITH(os_env_main, TESTS)
