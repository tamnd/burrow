/* Coverage guided fuzzing, end to end.
 *
 * The build compiles this file, and only this file, with the compiler's
 * coverage counters on: -fsanitize-coverage=inline-8bit-counters where the
 * compiler has them and -fsanitize-coverage=trace-pc where it does not. The
 * library under it is built as always, which is the setup the testing guide
 * tells people to use.
 *
 * FuzzDeep fails only on an input that starts with the four bytes FUZZ, one
 * nested test per byte. Blind mutation has about one chance in four billion
 * per try of getting all four at once, so it never finds that in a test's
 * time. With coverage each right byte is new code reached, the input that
 * reached it is kept, and the next byte is one guess in 256 away. Finding the
 * crash at all is the test that the guidance works.
 *
 * Copyright 2026 The burrow Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style licence that can be found
 * in the LICENSE file. */

/* popen and pclose are POSIX, not C11. */
#ifndef _WIN32
#define _POSIX_C_SOURCE 200809L
#endif

#include "burrow/testing.h"

#include "burrow/core.h"
#include "burrow/fmt.h"
#include "burrow/func.h"
#include "burrow/mem.h"
#include "burrow/mem/heap.h"
#include "burrow/pal.h"
#include "burrow/slice.h"

#include <stdio.h>
#include <string.h>

#ifdef _WIN32
#define popen _popen
#define pclose _pclose
#define SEP "\\"
#else
#include <sys/wait.h>
#define SEP "/"
#endif

/* From src/testing/fuzz.h, which is not an installed header. */
Int burrow__fuzz_coverage_len(void);

static const char *self_path;

/* ------------------------------------------------------------- the child */

/* Written between the tests so that the compiler cannot fold the four of them
 * into one four-byte compare, which would be one edge and nothing for the
 * fuzzer to climb. */
static volatile int depth;

static void deep_fn(void *env, TestingT *t, Slice args) {
    (void)env;
    Bytes b = testing_fuzz_arg(args, 0, Bytes);
    if (b.len < 4)
        return;
    const Byte *p = (const Byte *)b.p;
    if (p[0] == 'F') {
        depth = 1;
        if (p[1] == 'U') {
            depth = 2;
            if (p[2] == 'Z') {
                depth = 3;
                if (p[3] == 'Z')
                    testing_t_errorf_v(t, "found it");
            }
        }
    }
}

static void deep(void *env, TestingF *f) {
    (void)env;
    Byte seed[] = {'a', 'b', 'c', 'd'};
    testing_f_add_v(f, slice_from(seed, 4, 4, TYPE_BYTE));
    testing_f_fuzz_v(f, BURROW_FN(TestingFuzzFunc, deep_fn, NULL), TYPE_BYTES);
}

static int run_child(void) {
    char dir[1024];
    snprintf(dir, sizeof dir, "%s.cov", self_path);
    PalErrno err;
    if (!pal_mkdir(dir, 0755, &err) && err != PAL_EEXIST) {
        fprintf(stderr, "mkdir %s: %s\n", dir, pal_errno_string(err));
        return 3;
    }
    if (!pal_chdir(dir, &err)) {
        fprintf(stderr, "chdir %s: %s\n", dir, pal_errno_string(err));
        return 3;
    }
    TestingInternalFuzzTarget fuzz[] = {{BURROW_S("FuzzDeep"), {deep, NULL}}};
    TestingM *m =
        testing_main_start((TestingMatchString){NULL, NULL},
                           slice_from(NULL, 0, 0, TYPE_TESTING_INTERNAL_TEST),
                           slice_from(NULL, 0, 0, TYPE_TESTING_INTERNAL_BENCHMARK),
                           slice_from(fuzz, 1, 1, TYPE_TESTING_INTERNAL_FUZZ_TARGET),
                           slice_from(NULL, 0, 0, TYPE_TESTING_INTERNAL_EXAMPLE));
    int code = testing_m_run(m);
    testing_m_free(m);
    return code;
}

/* ------------------------------------------------------------ the parent */

typedef struct Buf {
    char *p;
    size_t len;
    size_t cap;
} Buf;

static void put(Buf *b, const char *p, size_t n) {
    if (b->len + n + 1 > b->cap) {
        size_t ncap = b->cap == 0 ? 256 : b->cap;
        while (ncap < b->len + n + 1)
            ncap *= 2;
        b->p = (char *)mem_realloc(heap_allocator(), b->p, b->cap, ncap, 1);
        b->cap = ncap;
    }
    memcpy(b->p + b->len, p, n);
    b->len += n;
    b->p[b->len] = 0;
}

static void buf_free(Buf *b) {
    if (b->p != NULL)
        mem_free(heap_allocator(), b->p, b->cap, 1);
}

static int spawn(const char *flags, Buf *out) {
    char cmd[4096];
#ifdef _WIN32
    snprintf(cmd, sizeof cmd, "\"\"%s\" %s child 2>&1\"", self_path, flags);
#else
    snprintf(cmd, sizeof cmd, "'%s' %s child 2>&1", self_path, flags);
#endif
    FILE *f = popen(cmd, "r"); /* NOLINT(cert-env33-c): it runs this binary */
    if (f == NULL)
        return -1;
    char chunk[4096];
    size_t n;
    while ((n = fread(chunk, 1, sizeof chunk, f)) > 0)
        put(out, chunk, n);
    put(out, "", 0);
    int status = pclose(f);
#ifdef _WIN32
    return status;
#else
    return WIFEXITED(status) ? WEXITSTATUS(status) : -1;
#endif
}

/* Removes the files in dir, and answers how many there were. Only files,
 * because only files are ever put there. */
static int empty_dir(const char *dir) {
    int64_t fd = pal_open(dir, PAL_O_RDONLY | PAL_O_DIRECTORY, 0, NULL);
    if (fd < 0)
        return 0;
    static PalDir d;
    d = (PalDir){.fd = fd};
    PalDirEntry e;
    PalErrno err = PAL_OK;
    int n = 0;
    while (pal_readdir(&d, &e, &err)) {
        char path[2048];
        snprintf(path, sizeof path, "%s" SEP "%s", dir, e.name);
        if (pal_unlink(path, NULL))
            n++;
    }
    pal_close(fd, NULL);
    return n;
}

static void TestCoverageGuided(TestingT *t) {
    if (self_path == NULL)
        testing_t_skip_v(t, "no path to this binary");
    if (testing_short())
        testing_t_skip_v(t, "fuzzes for up to a minute");
    if (burrow__fuzz_coverage_len() == 0)
        testing_t_skip_v(t, "this file was not built with coverage counters");

    char crashes[1024];
    char cache[1024];
    snprintf(crashes, sizeof crashes, "%s.cov" SEP "testdata" SEP "fuzz" SEP "FuzzDeep",
             self_path);
    /* Relative, since the child runs in the .cov directory, and so inside
     * it. */
    snprintf(cache, sizeof cache, "%s.cov" SEP "cache", self_path);
    char cache_target[1100];
    snprintf(cache_target, sizeof cache_target, "%s" SEP "FuzzDeep", cache);
    empty_dir(crashes);
    empty_dir(cache_target);

    char flags[2048];
    snprintf(flags, sizeof flags,
             "-test.fuzz=FuzzDeep -test.fuzztime=120s -test.parallel=2 "
             "-test.fuzzcachedir=cache");
    Buf out = {0};
    int code = spawn(flags, &out);
    if (code != 1)
        testing_t_errorf_v(t, "exit status %d, want 1", code);
    const char *want[] = {
        "gathering baseline coverage: 1/1 completed, now fuzzing with 2 workers",
        "--- FAIL: FuzzDeep",
        "found it",
        "Failing input written to testdata" SEP "fuzz" SEP "FuzzDeep" SEP,
    };
    for (size_t i = 0; i < sizeof want / sizeof want[0]; i++)
        if (out.p == NULL || strstr(out.p, want[i]) == NULL)
            testing_t_errorf_v(t, "output\n%s\nwant it to hold\n%s", out.p, want[i]);
    if (out.p != NULL && strstr(out.p, "without coverage guidance") != NULL)
        testing_t_errorf_v(t, "the coordinator did not see the counters:\n%s", out.p);

    /* Every input that reached new code on the way went to the cache. */
    int kept = empty_dir(cache_target);
    if (kept == 0)
        testing_t_errorf_v(t, "nothing was written to %s", cache_target);
    empty_dir(crashes);
    buf_free(&out);
}

#define TESTS(X) X(TestCoverageGuided)

int main(int argc, char **argv) {
    self_path = argv[0];
    if (argc >= 2 && strcmp(argv[argc - 1], "child") == 0) {
        testing_init(argc, argv);
        return run_child();
    }
    static const burrow__TestingEntry entries[] = {TESTS(BURROW__TESTING_ENTRY)};
    return burrow__testing_main(argc, argv, entries,
                                (Int)(sizeof entries / sizeof entries[0]), false,
                                testing_m_run);
}
