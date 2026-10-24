/* cryptotest.FetchModule, and the parts of testenv and testing.T it uses, for
 * the tests that run BoringSSL's tools: TestACVP and TestBogoSuite.
 *
 * Copyright 2024 The Go Authors. All rights reserved.
 * Copyright 2026 The burrow Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style licence that can be found
 * in the LICENSE file. */

#ifndef BURROW_TESTS_FETCHMODULE_H
#define BURROW_TESTS_FETCHMODULE_H

#include "check.h"

#include "burrow/burrow.h"
#include "burrow/encoding/json.h"
#include "burrow/os/exec.h"
#include "burrow/path/filepath.h"

/* Not in Go: string(b). */
static inline Str fetchmodule_str(Slice b) {
    return str_from_bytes(b.p, b.len);
}

/* Not in Go: testenv.CPUIsSlow, for the GOARCH values it names that burrow
 * builds for. */
static inline bool cpu_is_slow(void) {
#if defined(BURROW_ARCH_ARM) || defined(BURROW_ARCH_MIPS64) || defined(BURROW_ARCH_WASM)
    return true;
#else
    return false;
#endif
}

/* testenv.SkipIfShortAndSlow */
static inline void skip_if_short_and_slow(TestingT *t) {
    if (testing_short() && cpu_is_slow())
        testing_t_skipf_v(t, "skipping test in -short mode on %s",
                          str_from_cstr(burrow_arch_name()));
}

/* testenv.MustHaveExternalNetwork */
static inline void must_have_external_network(TestingT *t) {
#if defined(BURROW_OS_WASI)
    testing_t_skipf_v(t, "skipping test: no external network on %s",
                      str_from_cstr(burrow_os_name()));
#endif
    if (testing_short())
        testing_t_skip_v(t, "skipping test: no external network in -short mode");
}

/* testenv.GoToolPath. Not in Go: a burrow program has no GOROOT of its own,
 * so the go command is the one on $PATH, and the test is skipped when there
 * is none, as MustHaveGoBuild skips it. */
static inline Str go_tool_path(TestingT *t, Alloc *a) {
    SKIP_WITHOUT_EXEC(t);
    Error err = BURROW_NO_ERROR;
    Str path = exec_look_path(a, BURROW_S("go"), &err);
    if (BURROW_FAILED(err)) {
        testing_t_skipf_v(t, "skipping test: 'go build' unavailable: %v", err);
        return BURROW_STR_EMPTY;
    }
    return path;
}

/* testenv.CleanCmdEnv, as the environment it gives the command. */
static inline Slice clean_cmd_env(Alloc *a) {
    Slice environ = os_environ(a);
    Slice env = slice_nil(TYPE_STRING);
    for (Int i = 0; i < environ.len; i++) {
        Str kv = ((const Str *)environ.p)[i];
        /* Exclude GODEBUG from the environment to prevent its output from
         * breaking tests that are trying to parse other command output. */
        if (strings_has_prefix(kv, BURROW_S("GODEBUG=")))
            continue;
        /* Exclude GOTRACEBACK for the same reason. */
        if (strings_has_prefix(kv, BURROW_S("GOTRACEBACK=")))
            continue;
        env = BURROW_APPEND(Str, a, env, kv);
    }
    return env;
}

static inline void fetchmodule_remove_all(void *env) {
    (void)os_remove_all(*(Str *)env);
}

/* Not in Go: t.TempDir. */
static inline Str fetchmodule_temp_dir(TestingT *t, Alloc *a) {
    Error err = BURROW_NO_ERROR;
    Str dir = os_mkdir_temp(a, BURROW_STR_EMPTY, testing_t_name(t), &err);
    if (BURROW_FAILED(err)) {
        testing_t_fatalf_v(t, "TempDir: %s", err);
        return BURROW_STR_EMPTY;
    }
    Str *p = BURROW_NEW(a, Str);
    *p = dir;
    testing_t_cleanup(t, BURROW_FN(Func, fetchmodule_remove_all, p));
    return dir;
}

typedef struct FetchmoduleEnvVar {
    Str key;
    Str value;
    bool found;
} FetchmoduleEnvVar;

static inline void fetchmodule_restore_env(void *env) {
    const FetchmoduleEnvVar *v = (const FetchmoduleEnvVar *)env;
    if (v->found)
        (void)os_setenv(v->key, v->value);
    else
        (void)os_unsetenv(v->key);
}

/* Not in Go: t.Setenv. */
static inline void fetchmodule_setenv(TestingT *t, Alloc *a, Str key, Str value) {
    FetchmoduleEnvVar *v = BURROW_NEW(a, FetchmoduleEnvVar);
    v->key = key;
    v->value = os_lookup_env(a, key, &v->found);
    Error err = os_setenv(key, value);
    if (BURROW_FAILED(err)) {
        testing_t_fatal_v(t, err);
        return;
    }
    testing_t_cleanup(t, BURROW_FN(Func, fetchmodule_restore_env, v));
}

/* The struct FetchModule reads 'go mod download -json' into. Not in Go: it
 * has a name, so that it can have a type. */
#define FETCHMODULE_DOWNLOAD_FIELDS(F, T) F(T, Str, Dir, "")
BURROW_STRUCT(FetchmoduleDownload, FETCHMODULE_DOWNLOAD_FIELDS);

/* cryptotest.FetchModule: FetchModule fetches the module at the given version
 * and returns the directory containing its source tree. It skips the test if
 * fetching modules is not possible in this environment. */
static inline Str fetch_module(TestingT *t, Alloc *a, Str module, Str version) {
    must_have_external_network(t);
    Str go = go_tool_path(t, a);

    /* If the default GOMODCACHE doesn't exist, use a temporary directory
     * instead. (For example, run.bash sets GOPATH=/nonexist-gopath.) */
    ExecCmd *cmd = exec_command_v(a, go, 2, BURROW_S("env"), BURROW_S("GOMODCACHE"));
    cmd->env = clean_cmd_env(a);
    Error err = BURROW_NO_ERROR;
    Slice out = exec_cmd_output(cmd, a, &err);
    exec_cmd_free(cmd);
    if (BURROW_FAILED(err)) {
        testing_t_errorf_v(t, "%s env GOMODCACHE: %v\n%s", go, err,
                           fetchmodule_str(out));
        const ExecExitError *ee =
            (const ExecExitError *)errors_as(err, TYPE_EXEC_EXIT_ERROR);
        if (ee != NULL)
            testing_t_logf_v(t, "%s", fetchmodule_str(ee->stderr_));
        testing_t_fail_now(t);
        return BURROW_STR_EMPTY;
    }
    bool modcache_ok = false;
    Str gomodcache = fetchmodule_str(bytes_trim_space(out));
    if (gomodcache.len > 0) {
        Error serr = BURROW_NO_ERROR;
        (void)os_stat(a, gomodcache, &serr);
        if (!BURROW_FAILED(serr))
            modcache_ok = true;
    }
    if (!modcache_ok) {
        fetchmodule_setenv(t, a, BURROW_S("GOMODCACHE"), fetchmodule_temp_dir(t, a));
        /* Allow t.TempDir() to clean up subdirectories. */
        Str goflags = os_getenv(a, BURROW_S("GOFLAGS"));
        fetchmodule_setenv(t, a, BURROW_S("GOFLAGS"),
                           fmt_sprintf_v(a, "%s -modcacherw", goflags));
    }

    testing_t_logf_v(t, "fetching %s@%s\n", module, version);

    cmd = exec_command_v(a, go, 4, BURROW_S("mod"), BURROW_S("download"),
                         BURROW_S("-json"), fmt_sprintf_v(a, "%s@%s", module, version));
    BytesBuffer stderr_b = BYTES_BUFFER(a);
    cmd->stderr_ = bytes_buffer_as_io_writer(&stderr_b);
    Slice output = exec_cmd_output(cmd, a, &err);
    exec_cmd_free(cmd);
    if (BURROW_FAILED(err)) {
        testing_t_fatalf_v(t,
                           "failed to download %s@%s: %s\nstdout:\n%s\nstderr:\n%s\n",
                           module, version, err, fetchmodule_str(output),
                           fetchmodule_str(bytes_buffer_bytes(&stderr_b)));
        return BURROW_STR_EMPTY;
    }
    if (bytes_buffer_len(&stderr_b) > 0)
        testing_t_logf_v(t, "go mod download stderr:\n%s",
                         fetchmodule_str(bytes_buffer_bytes(&stderr_b)));
    FetchmoduleDownload j = {0};
    err = json_unmarshal(a, output, BURROW_ANY(TYPE_OF(FetchmoduleDownload), &j));
    if (BURROW_FAILED(err)) {
        testing_t_fatalf_v(
            t, "failed to parse 'go mod download': %s\nstdout:\n%s\nstderr:\n%s\n", err,
            fetchmodule_str(output), fetchmodule_str(bytes_buffer_bytes(&stderr_b)));
        return BURROW_STR_EMPTY;
    }

    return j.Dir;
}

#endif /* BURROW_TESTS_FETCHMODULE_H */
