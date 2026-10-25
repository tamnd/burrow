/* plugin's tests, from Go's plugin_test.go, and the ones Go keeps outside the
 * package, in its testplugin directory, which needs Go plugins to build.
 *
 * Copyright 2018 The Go Authors. All rights reserved.
 * Copyright 2026 The burrow Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style licence that can be found
 * in the LICENSE file. */

#include "burrow/plugin.h"

#include "burrow/bytes.h"
#include "burrow/func.h"
#include "burrow/mem.h"
#include "burrow/mem/arena.h"
#include "burrow/os.h"
#include "burrow/path/filepath.h"
#include "burrow/strings.h"
#include "burrow/sync.h"
#include "burrow/testing.h"

#include <string.h>

static void TestPlugin(TestingT *t) {
    (void)t;
    /* This test makes sure that executable that imports plugin
     * package can actually run. See issue #28789 for details. */
}

/* Not in Go: the error for a file that is not there is Go's. */
static void TestOpenMissing(TestingT *t) {
    Str name = BURROW_S("no-such-dir/no-such-plugin.so");
    Error err = BURROW_NO_ERROR;
    Plugin *p = plugin_open(name, &err);
    if (p != NULL || !BURROW_FAILED(err))
        testing_t_fatalf_v(t, "Open(%q) did not fail", name);
    Str got = error_text(err);
    Str want =
        BURROW_S("plugin.Open(\"no-such-dir/no-such-plugin.so\"): realpath failed");
    if (!str_eq(got, want))
        testing_t_errorf_v(t, "Open(%q) = %q, want %q", name, got, want);
}

/* Not in Go: a file that is there and is not a library fails in the loader,
 * and the error is the loader's after Go's prefix. */
static void TestOpenNotLibrary(TestingT *t) {
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    Error err = BURROW_NO_ERROR;
    Str dir = os_mkdir_temp(a, BURROW_S(""), BURROW_S("plugin_test"), &err);
    if (BURROW_FAILED(err)) {
        arena_free(&ar);
        testing_t_fatalf_v(t, "MkdirTemp: %v", err);
    }
    Str name = filepath_join_v(a, 2, dir, BURROW_S("text.so"));
    err = os_write_file(name, BURROW_B("not a library\n"), 0644);
    if (BURROW_FAILED(err)) {
        os_remove_all(dir);
        arena_free(&ar);
        testing_t_fatalf_v(t, "WriteFile: %v", err);
    }

    Plugin *p = plugin_open(name, &err);
    if (p != NULL || !BURROW_FAILED(err)) {
        testing_t_errorf_v(t, "Open(%q) did not fail", name);
    } else {
        Str got = error_text(err);
        StringsBuilder b = STRINGS_BUILDER(a);
        strings_builder_write_string(&b, BURROW_S("plugin.Open(\""), NULL);
        strings_builder_write_string(&b, name, NULL);
        strings_builder_write_string(&b, BURROW_S("\"): "), NULL);
        Str prefix = strings_builder_string(&b);
        if (!strings_has_prefix(got, prefix) || got.len == prefix.len)
            testing_t_errorf_v(t, "Open(%q) = %q, want %q and the loader's reason",
                               name, got, prefix);
    }
    os_remove_all(dir);
    arena_free(&ar);
}

/* A library the test can count on finding as a file, and a function in it, or
 * an empty path where there is none. */
static Str system_library(Alloc *a, Str *sym) {
    *sym = BURROW_S("");
#if defined(BURROW_OS_LINUX)
    /* Whichever C library the test runs on, from its own map. A static test
     * binary has none. */
    *sym = BURROW_S("strlen");
    Error err = BURROW_NO_ERROR;
    Slice maps = os_read_file(a, BURROW_S("/proc/self/maps"), &err);
    if (BURROW_FAILED(err))
        return BURROW_S("");
    Str rest = str_from_bytes(maps.p, maps.len);
    while (rest.len > 0) {
        Int nl = strings_index_byte(rest, '\n');
        Str line = rest;
        if (nl >= 0) {
            line.len = nl;
            rest.p += nl + 1;
            rest.len -= nl + 1;
        } else {
            rest.len = 0;
        }
        Int slash = strings_index_byte(line, '/');
        if (slash < 0)
            continue;
        Str path = {line.p + slash, line.len - slash};
        Int base = strings_last_index_byte(path, '/');
        Str file = {path.p + base + 1, path.len - base - 1};
        if (strings_has_prefix(file, BURROW_S("libc.so")) ||
            strings_has_prefix(file, BURROW_S("libc-")) ||
            strings_has_prefix(file, BURROW_S("ld-musl-")))
            return path;
    }
    return BURROW_S("");
#elif defined(BURROW_OS_FREEBSD)
    *sym = BURROW_S("strlen");
    (void)a;
    return BURROW_S("/lib/libc.so.7");
#elif defined(BURROW_OS_WINDOWS)
    *sym = BURROW_S("GetTickCount");
    Str root = os_getenv(a, BURROW_S("SystemRoot"));
    if (root.len == 0)
        return BURROW_S("");
    return filepath_join_v(a, 3, root, BURROW_S("System32"), BURROW_S("kernel32.dll"));
#else
    (void)a;
    return BURROW_S("");
#endif
}

/* The same file, with a "." in the middle of its path. */
static Str other_spelling(Alloc *a, Str path) {
    Int i = strings_last_index_byte(path, '/');
#if defined(BURROW_OS_WINDOWS)
    Int j = strings_last_index_byte(path, '\\');
    if (j > i)
        i = j;
#endif
    Byte *p = (Byte *)mem_alloc(a, (size_t)path.len + 2, 1);
    if (p == NULL || i < 0 || i >= path.len)
        return path;
    memcpy(p, path.p, (size_t)i + 1);
    p[i + 1] = '.';
    p[i + 2] = path.p[i];
    memcpy(p + i + 3, path.p + i + 1, (size_t)(path.len - i - 1));
    Str out = {p, path.len + 2};
    return out;
}

typedef size_t (*StrlenFunc)(const char *);

/* Not in Go: Go opens a plugin it built in testplugin, and here the plugin is
 * the system's C library, which is a shared library like any other. */
static void TestOpenLookup(TestingT *t) {
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    Str sym = BURROW_S("");
    Str path = system_library(a, &sym);
    if (path.len == 0) {
        arena_free(&ar);
        Str goos = BURROW_S(BURROW_OS_NAME);
        testing_t_skipf_v(t, "no system library to open as a file on %s", goos);
    }

    Error err = BURROW_NO_ERROR;
    Plugin *p = plugin_open(path, &err);
    if (BURROW_FAILED(err) || p == NULL) {
        arena_free(&ar);
        testing_t_fatalf_v(t, "Open(%q): %v", path, err);
    }
    /* If a path has already been opened, then the existing *Plugin is
     * returned. */
    Plugin *again = plugin_open(path, &err);
    if (BURROW_FAILED(err) || again != p)
        testing_t_errorf_v(t, "second Open(%q) gave another plugin, err %v", path, err);
    Str other = other_spelling(a, path);
    Plugin *same = plugin_open(other, &err);
    if (BURROW_FAILED(err) || same != p)
        testing_t_errorf_v(t, "Open(%q) gave another plugin than Open(%q), err %v",
                           other, path, err);

    PluginSymbol f = plugin_lookup(p, sym, &err);
    if (BURROW_FAILED(err) || f == NULL) {
        testing_t_errorf_v(t, "Lookup(%q): %v", sym, err);
    } else if (str_eq(sym, BURROW_S("strlen"))) {
        union {
            PluginSymbol object;
            StrlenFunc fn;
        } u;
        u.object = f;
        size_t n = u.fn("seven..");
        if (n != 7)
            testing_t_errorf_v(t, "strlen(\"seven..\") = %d, want 7", (Int)n);
    }

    Str missing = BURROW_S("burrow_no_such_symbol");
    PluginSymbol none = plugin_lookup(p, missing, &err);
    if (none != NULL || !BURROW_FAILED(err)) {
        testing_t_errorf_v(t, "Lookup(%q) did not fail", missing);
    } else {
        Str got = error_text(err);
        Str prefix =
            BURROW_S("plugin: symbol burrow_no_such_symbol not found in plugin ");
        if (!strings_has_prefix(got, prefix) || got.len == prefix.len)
            testing_t_errorf_v(t, "Lookup(%q) = %q, want %q and the path", missing, got,
                               prefix);
    }
    arena_free(&ar);
}

typedef struct OpenJob {
    Str path;
    Plugin *got;
} OpenJob;

static void open_one(void *env) {
    OpenJob *j = (OpenJob *)env;
    Error err = BURROW_NO_ERROR;
    j->got = plugin_open(j->path, &err);
}

/* Not in Go: Open is safe for concurrent use, and every goroutine that opens
 * one file gets the same plugin back. */
static void TestOpenConcurrent(TestingT *t) {
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    Str sym = BURROW_S("");
    Str path = system_library(a, &sym);
    if (path.len == 0) {
        arena_free(&ar);
        Str goos = BURROW_S(BURROW_OS_NAME);
        testing_t_skipf_v(t, "no system library to open as a file on %s", goos);
    }
    enum { JOBS = 8 };
    OpenJob jobs[JOBS];
    SyncWaitGroup wg = {0};
    for (int i = 0; i < JOBS; i++) {
        jobs[i].path = path;
        jobs[i].got = NULL;
        sync_wait_group_go(&wg, BURROW_FN(Func, open_one, &jobs[i]));
    }
    sync_wait_group_wait(&wg);
    for (int i = 1; i < JOBS; i++)
        if (jobs[i].got == NULL || jobs[i].got != jobs[0].got)
            testing_t_errorf_v(t, "goroutine %d got another plugin", i);
    arena_free(&ar);
}

#define TESTS(X)                                                                       \
    X(TestPlugin)                                                                      \
    X(TestOpenMissing)                                                                 \
    X(TestOpenNotLibrary)                                                              \
    X(TestOpenLookup)                                                                  \
    X(TestOpenConcurrent)

TESTING_MAIN(TESTS)
