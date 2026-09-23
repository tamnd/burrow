/* Tests for testing, written with testing.
 *
 * Most of what the package does is print, so most of these run this same
 * binary again with a scenario named on the end of the command line and compare
 * what it printed with what go test prints for the same tests. Durations and
 * line numbers are the only things that differ from one run to the next and
 * both are rewritten before the comparison.
 *
 * The rest run in process: the regexp the matcher uses, the names subtests get,
 * cleanups, skips and parallel subtests.
 *
 * Copyright 2026 The burrow Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style licence that can be found
 * in the LICENSE file. */

/* popen and pclose are POSIX, not C11, so they need asking for before the
 * first include or they come out implicitly declared and returning int. */
#ifndef _WIN32
#define _POSIX_C_SOURCE 200809L
#endif

#include "burrow/testing.h"

#include "burrow/context.h"
#include "burrow/core.h"
#include "burrow/error.h"
#include "burrow/fmt.h"
#include "burrow/func.h"
#include "burrow/mem.h"
#include "burrow/mem/heap.h"
#include "burrow/panic.h"
#include "burrow/slice.h"
#include "burrow/sync/atomic.h"
#include "burrow/time.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#define popen _popen
#define pclose _pclose
#else
#include <sys/wait.h>
#endif

static const char *self_path;

/* ------------------------------------------------------------------ regexp */

typedef struct MatchCase {
    const char *pat;
    const char *str;
    bool want;
} MatchCase;

static const MatchCase match_cases[] = {
    {"", "anything", true},
    {"abc", "xxabcxx", true},
    {"^abc$", "xxabcxx", false},
    {"^abc$", "abc", true},
    {"a.c", "abc", true},
    {"a.c", "a\nc", false},
    {"(?s)a.c", "a\nc", true},
    {"a|b", "b", true},
    {"^(foo|bar)$", "bar", true},
    {"^(foo|bar)$", "baz", false},
    {"^a*$", "", true},
    {"^a+$", "", false},
    {"^a?b$", "b", true},
    {"^a{2,3}$", "aaa", true},
    {"^a{2,3}$", "aaaa", false},
    {"^a{2}$", "aa", true},
    {"^a{2,}$", "aaaaa", true},
    {"^[a-c]+$", "abcabc", true},
    {"^[^a-c]+$", "xyz", true},
    {"^[^a-c]+$", "xaz", false},
    {"^\\d+$", "12345", true},
    {"^\\w+$", "Test_1", true},
    {"\\bfoo\\b", "a foo b", true},
    {"\\bfoo\\b", "afoob", false},
    {"(?i)^test", "TeSt", true},
    {"^[[:alpha:]]+$", "abcXYZ", true},
    {"^\\Qa.b\\E$", "a.b", true},
    {"^\\Qa.b\\E$", "axb", false},
    {"^\\x41$", "A", true},
    {"^.$", "\xc3\xa9", true},
    {"(?m)^b$", "a\nb\nc", true},
    {"^b$", "a\nb\nc", false},
    {"^(?P<name>ab)+$", "abab", true},
    {"^(a*)*$", "aaaa", true},
    {"^(a|ab)(c|bcd)$", "abcd", true},
    {"\\Aab\\z", "ab", true},
    {"\\Aab\\z", "abc", false},
    {"^a+?$", "aaa", true},
    {"^\\x{263a}$", "\xe2\x98\xba", true},
    {"^\\101$", "A", true},
    {"^\\S\\s\\D$", "x 1", false},
    {"(?U)^a+$", "aaa", true},
    {"TestFoo/bar", "TestFoo/bar", true},
};

static void TestMatchString(TestingT *t) {
    for (size_t i = 0; i < sizeof match_cases / sizeof match_cases[0]; i++) {
        const MatchCase *c = &match_cases[i];
        Error err = {0};
        bool got =
            testing_match_string(str_from_cstr(c->pat), str_from_cstr(c->str), &err);
        if (err.vt != NULL) {
            testing_t_errorf_v(t, "%q: unexpected error %v", c->pat, err);
            continue;
        }
        if (got != c->want)
            testing_t_errorf_v(t, "match(%q, %q) = %v, want %v", c->pat, c->str, got,
                               c->want);
    }
}

typedef struct ErrorCase {
    const char *pat;
    const char *want;
} ErrorCase;

static const ErrorCase error_cases[] = {
    {"a(", "error parsing regexp: missing closing ): `a(`"},
    {"a)", "error parsing regexp: unexpected ): `a)`"},
    {"*", "error parsing regexp: missing argument to repetition operator: `*`"},
    {"a**", "error parsing regexp: invalid nested repetition operator: `**`"},
    {"[a", "error parsing regexp: missing closing ]: `[a`"},
    {"[z-a]", "error parsing regexp: invalid character class range: `z-a`"},
    {"a{1001}", "error parsing regexp: invalid repeat count: `{1001}`"},
    {"\\8", "error parsing regexp: invalid escape sequence: `\\8`"},
    {"\\pL", "error parsing regexp: unsupported by the built in matcher: `\\p`"},
    {"(?z)", "error parsing regexp: invalid or unsupported Perl syntax: `(?z`"},
};

static void TestMatchStringErrors(TestingT *t) {
    for (size_t i = 0; i < sizeof error_cases / sizeof error_cases[0]; i++) {
        const ErrorCase *c = &error_cases[i];
        Error err = {0};
        testing_match_string(str_from_cstr(c->pat), BURROW_S("x"), &err);
        if (err.vt == NULL) {
            testing_t_errorf_v(t, "%q: no error", c->pat);
            continue;
        }
        if (!str_eq(error_text(err), str_from_cstr(c->want)))
            testing_t_errorf_v(t, "%q: error %q, want %q", c->pat, error_text(err),
                               c->want);
    }
}

/* ------------------------------------------------------------------ names */

static Str names[8];
static int nnames;

static void record_name(void *env, TestingT *t) {
    (void)env;
    if (nnames < 8)
        names[nnames] = str_clone(heap_allocator(), testing_t_name(t));
    nnames++;
}

static void TestSubtestNames(TestingT *t) {
    static const char *const in[] = {"plain", "a b", "dup", "dup", "dup#01", "bell\a"};
    static const char *const want[] = {
        "TestSubtestNames/plain",     "TestSubtestNames/a_b",
        "TestSubtestNames/dup",       "TestSubtestNames/dup#01",
        "TestSubtestNames/dup#01#01", "TestSubtestNames/bell\\a",
    };
    nnames = 0;
    for (int i = 0; i < 6; i++)
        testing_t_run(t, str_from_cstr(in[i]),
                      BURROW_FN(TestingTFunc, record_name, NULL));
    if (nnames != 6)
        testing_t_fatalf_v(t, "ran %d subtests, want 6", nnames);
    for (int i = 0; i < 6; i++) {
        if (!str_eq(names[i], str_from_cstr(want[i])))
            testing_t_errorf_v(t, "subtest %d is named %q, want %q", i, names[i],
                               want[i]);
        mem_free(heap_allocator(), (void *)(uintptr_t)names[i].p, (size_t)names[i].len,
                 1);
    }
}

/* --------------------------------------------------------------- cleanups */

static char order[16];
static int norder;

static void push_order(void *env) {
    order[norder++] = *(const char *)env;
}

static void cleanup_body(void *env, TestingT *t) {
    (void)env;
    static const char a = 'a';
    static const char b = 'b';
    static const char c = 'c';
    testing_t_cleanup(t, BURROW_FN(Func, push_order, (void *)(uintptr_t)&a));
    testing_t_cleanup(t, BURROW_FN(Func, push_order, (void *)(uintptr_t)&b));
    testing_t_cleanup(t, BURROW_FN(Func, push_order, (void *)(uintptr_t)&c));
}

static void TestCleanupOrder(TestingT *t) {
    norder = 0;
    testing_t_run(t, BURROW_S("sub"), BURROW_FN(TestingTFunc, cleanup_body, NULL));
    if (norder != 3 || memcmp(order, "cba", 3) != 0)
        testing_t_errorf_v(t, "cleanups ran as %q, want \"cba\"",
                           str_from_bytes((const Byte *)order, norder));
}

/* ------------------------------------------------------------------- skip */

static bool after_skip;

static void skip_body(void *env, TestingT *t) {
    (void)env;
    testing_t_skip_v(t, "not today");
    after_skip = true;
}

static void TestSkip(TestingT *t) {
    after_skip = false;
    bool ok =
        testing_t_run(t, BURROW_S("skipper"), BURROW_FN(TestingTFunc, skip_body, NULL));
    if (!ok)
        testing_t_error_v(t, "a skipped subtest made Run return false");
    if (after_skip)
        testing_t_error_v(t, "Skip returned");
    if (testing_t_failed(t))
        testing_t_error_v(t, "a skipped subtest failed its parent");
}

/* --------------------------------------------------------------- parallel */

static int32_t par_done;
static int32_t par_live;
static int32_t par_peak;

static void parallel_body(void *env, TestingT *t) {
    (void)env;
    testing_t_parallel(t);
    int32_t live = sync_atomic_add_int32(&par_live, 1);
    for (;;) {
        int32_t peak = sync_atomic_load_int32(&par_peak);
        if (live <= peak || sync_atomic_compare_and_swap_int32(&par_peak, peak, live))
            break;
    }
    time_sleep(5 * TIME_MILLISECOND);
    sync_atomic_add_int32(&par_live, -1);
    sync_atomic_add_int32(&par_done, 1);
}

static void parallel_group(void *env, TestingT *t) {
    (void)env;
    for (int i = 0; i < 6; i++)
        testing_t_run(t, BURROW_S("p"), BURROW_FN(TestingTFunc, parallel_body, NULL));
    if (sync_atomic_load_int32(&par_done) != 0)
        testing_t_error_v(t, "a parallel subtest ran before its parent returned");
}

static void TestParallel(TestingT *t) {
    par_done = 0;
    testing_t_run(t, BURROW_S("group"), BURROW_FN(TestingTFunc, parallel_group, NULL));
    if (sync_atomic_load_int32(&par_done) != 6)
        testing_t_errorf_v(t,
                           "%d parallel subtests finished before Run returned, want 6",
                           sync_atomic_load_int32(&par_done));
}

/* ------------------------------------------------------------- scenarios
 *
 * The tests the child runs. The line numbers in their messages are rewritten
 * to N by normalise, so moving them around does not break anything. */

static void child_pass(void *env, TestingT *t) {
    (void)env;
    testing_t_log_v(t, "hello");
}

static void child_fail(void *env, TestingT *t) {
    (void)env;
    testing_t_errorf_v(t, "bad %d", 1);
}

static void child_ok(void *env, TestingT *t) {
    (void)env;
    (void)t;
}

static void child_fatal(void *env, TestingT *t) {
    (void)env;
    testing_t_fatal_v(t, "stop");
}

static void child_skip(void *env, TestingT *t) {
    (void)env;
    testing_t_skip_v(t, "later");
}

static void child_sub(void *env, TestingT *t) {
    (void)env;
    testing_t_run(t, BURROW_S("ok"), BURROW_FN(TestingTFunc, child_ok, NULL));
    testing_t_run(t, BURROW_S("fail"), BURROW_FN(TestingTFunc, child_fatal, NULL));
    testing_t_run(t, BURROW_S("skip"), BURROW_FN(TestingTFunc, child_skip, NULL));
}

static void child_multi(void *env, TestingT *t) {
    (void)env;
    testing_t_log_v(t, "one\ntwo");
}

static void child_panic(void *env, TestingT *t) {
    (void)env;
    (void)t;
    panic_str(BURROW_S("boom"));
}

static void child_sleep(void *env, TestingT *t) {
    (void)env;
    (void)t;
    time_sleep(10 * TIME_SECOND);
}

/* The benchmarks the child runs. Their timings differ from run to run and
 * normalise turns every ns/op figure into N, so what is left to compare is the
 * shape of the output and the counts that -test.benchtime=Nx fixes. */

/* Enough work per op that no clock rounds it down to nothing, which would drop
 * the ns/op column the way Go drops it for a zero. */
static void spin(void) {
    volatile Int sum = 0;
    for (Int i = 0; i < 2000; i++)
        sum += i;
    (void)sum;
}

static void child_bench_plain(void *env, TestingB *b) {
    (void)env;
    for (Int i = 0; i < testing_b_n(b); i++)
        spin();
}

static void child_bench_loop(void *env, TestingB *b) {
    (void)env;
    while (testing_b_loop(b))
        spin();
}

static void child_bench_metric(void *env, TestingB *b) {
    (void)env;
    while (testing_b_loop(b))
        spin();
    testing_b_report_metric(b, 42, BURROW_S("widgets/op"));
    testing_b_set_bytes(b, 0);
}

static void child_bench_sub(void *env, TestingB *b) {
    (void)env;
    testing_b_run(b, BURROW_S("a"), BURROW_FN(TestingBFunc, child_bench_metric, NULL));
    testing_b_run(b, BURROW_S("b"), BURROW_FN(TestingBFunc, child_bench_plain, NULL));
}

static void child_pb_body(void *env, TestingPB *pb) {
    (void)env;
    while (testing_pb_next(pb))
        spin();
}

static void child_bench_parallel(void *env, TestingB *b) {
    (void)env;
    testing_b_set_parallelism(b, 2);
    testing_b_run_parallel(b, BURROW_FN(TestingPBFunc, child_pb_body, NULL));
}

static void child_bench_fail(void *env, TestingB *b) {
    (void)env;
    testing_b_error_v(b, "broken");
}

static void child_bench_skip(void *env, TestingB *b) {
    (void)env;
    testing_b_skip_v(b, "not today");
}

static void child_bench_log(void *env, TestingB *b) {
    (void)env;
    testing_b_log_v(b, "noted");
    for (Int i = 0; i < testing_b_n(b); i++)
        spin();
}

static void child_bench_break(void *env, TestingB *b) {
    (void)env;
    while (testing_b_loop(b))
        break;
}

static void child_bench_alloc(void *env, TestingB *b) {
    (void)env;
    testing_b_report_allocs(b);
    for (Int i = 0; i < testing_b_n(b); i++) {
        void *p = mem_alloc(heap_allocator(), 16, 8);
        if (p == NULL)
            testing_b_fatal_v(b, "out of memory");
        spin();
        mem_free(heap_allocator(), p, 16, 8);
    }
}

/* testing.Benchmark on its own, outside any test binary's main. */
static int run_benchfunc(void) {
    TestingBenchmarkResult r =
        testing_benchmark(BURROW_FN(TestingBFunc, child_bench_alloc, NULL));
    fmt_printf_v("n=%d allocs=%d bytes=%d\n", r.n,
                 testing_benchmark_result_allocs_per_op(r),
                 testing_benchmark_result_alloced_bytes_per_op(r));
    testing_benchmark_result_free(&r);
    return 0;
}

static int run_child(const char *scenario) {
    TestingInternalTest tests[4];
    Int n = 0;
    TestingInternalBenchmark benchmarks[10];
    Int nb = 0;
    if (strcmp(scenario, "benchfunc") == 0)
        return run_benchfunc();
    if (strcmp(scenario, "bench") == 0 || strcmp(scenario, "benchbare") == 0) {
        tests[n++] = (TestingInternalTest){BURROW_S("TestPass"), {child_pass, NULL}};
        benchmarks[nb++] = (TestingInternalBenchmark){BURROW_S("BenchmarkPlain"),
                                                      {child_bench_plain, NULL}};
        benchmarks[nb++] = (TestingInternalBenchmark){BURROW_S("BenchmarkLoop"),
                                                      {child_bench_loop, NULL}};
        benchmarks[nb++] = (TestingInternalBenchmark){BURROW_S("BenchmarkSub"),
                                                      {child_bench_sub, NULL}};
        benchmarks[nb++] = (TestingInternalBenchmark){BURROW_S("BenchmarkParallel"),
                                                      {child_bench_parallel, NULL}};
        benchmarks[nb++] = (TestingInternalBenchmark){BURROW_S("BenchmarkFail"),
                                                      {child_bench_fail, NULL}};
        benchmarks[nb++] = (TestingInternalBenchmark){BURROW_S("BenchmarkSkip"),
                                                      {child_bench_skip, NULL}};
        benchmarks[nb++] = (TestingInternalBenchmark){BURROW_S("BenchmarkLog"),
                                                      {child_bench_log, NULL}};
        benchmarks[nb++] = (TestingInternalBenchmark){BURROW_S("BenchmarkBreak"),
                                                      {child_bench_break, NULL}};
        benchmarks[nb++] = (TestingInternalBenchmark){BURROW_S("BenchmarkAlloc"),
                                                      {child_bench_alloc, NULL}};
    } else if (strcmp(scenario, "panic") == 0) {
        tests[n++] = (TestingInternalTest){BURROW_S("TestPass"), {child_pass, NULL}};
        tests[n++] = (TestingInternalTest){BURROW_S("TestPanic"), {child_panic, NULL}};
    } else if (strcmp(scenario, "sleep") == 0) {
        tests[n++] = (TestingInternalTest){BURROW_S("TestSleep"), {child_sleep, NULL}};
    } else {
        tests[n++] = (TestingInternalTest){BURROW_S("TestPass"), {child_pass, NULL}};
        tests[n++] = (TestingInternalTest){BURROW_S("TestFail"), {child_fail, NULL}};
        tests[n++] = (TestingInternalTest){BURROW_S("TestSub"), {child_sub, NULL}};
        tests[n++] = (TestingInternalTest){BURROW_S("TestMulti"), {child_multi, NULL}};
    }
    TestingM *m = testing_main_start(
        (TestingMatchString){NULL, NULL},
        slice_from(tests, n, n, TYPE_TESTING_INTERNAL_TEST),
        slice_from(benchmarks, nb, nb, TYPE_TESTING_INTERNAL_BENCHMARK),
        slice_nil(TYPE_TESTING_INTERNAL_FUZZ_TARGET),
        slice_nil(TYPE_TESTING_INTERNAL_EXAMPLE));
    testing_m_set_bare(m, strcmp(scenario, "bare") == 0 ||
                              strcmp(scenario, "benchbare") == 0);
    int code = testing_m_run(m);
    testing_m_free(m);
    return code;
}

/* ------------------------------------------------------------- the parent */

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

static bool is_digit(char c) {
    return c >= '0' && c <= '9';
}

static const char *const machine_lines[] = {"goos: ", "goarch: "};

/* The output with every "(1.23s)" made "(0.00s)", every line number after a
 * file name made N and every \r dropped, which is what Windows adds. For
 * benchmarks, every ns/op figure is made N, the goos and goarch lines lose
 * what comes after the colon and the cpu line goes, since not every machine
 * has a name to give it. */
static void normalise(Buf *out, const char *s) {
    static const char file[] = "testing_test.c:";
    static const char lib[] = "testing.c:";
    size_t flen = sizeof file - 1;
    size_t llen = sizeof lib - 1;
    bool bol = true;
    while (*s != 0) {
        if (*s == '\r') {
            s++;
            continue;
        }
        if (bol && strncmp(s, "cpu: ", 5) == 0) {
            while (*s != 0 && *s != '\n')
                s++;
            if (*s == '\n')
                s++;
            continue;
        }
        if (bol) {
            for (size_t i = 0; i < sizeof machine_lines / sizeof machine_lines[0];
                 i++) {
                size_t n = strlen(machine_lines[i]);
                if (strncmp(s, machine_lines[i], n) == 0) {
                    put(out, machine_lines[i], n);
                    put(out, "X", 1);
                    while (*s != 0 && *s != '\n')
                        s++;
                    break;
                }
            }
        }
        bol = *s == '\n';
        if (*s == '\t') {
            const char *e = s + 1;
            while (*e == ' ')
                e++;
            const char *d = e;
            while (is_digit(*e) || *e == '.')
                e++;
            if (e > d && strncmp(e, " ns/op", 6) == 0) {
                put(out, "\tN ns/op", 8);
                s = e + 6;
                continue;
            }
        }
        if (strncmp(s, file, flen) == 0 && is_digit(s[flen])) {
            put(out, file, flen);
            put(out, "N", 1);
            s += flen;
            while (is_digit(*s))
                s++;
            continue;
        }
        if (strncmp(s, lib, llen) == 0 && is_digit(s[llen])) {
            put(out, lib, llen);
            put(out, "N", 1);
            s += llen;
            while (is_digit(*s))
                s++;
            continue;
        }
        if (*s == '(' && is_digit(s[1])) {
            const char *e = s + 1;
            while (is_digit(*e))
                e++;
            if (*e == '.' && is_digit(e[1]) && is_digit(e[2]) && e[3] == 's' &&
                e[4] == ')') {
                put(out, "(0.00s)", 7);
                s = e + 5;
                continue;
            }
        }
        put(out, s, 1);
        s++;
    }
}

/* Runs this binary on a scenario, with the flags in front. */
static int spawn(const char *flags, const char *scenario, Buf *out) {
    char cmd[1024];
#ifdef _WIN32
    snprintf(cmd, sizeof cmd, "\"\"%s\" %s child %s 2>&1\"", self_path, flags,
             scenario);
#else
    snprintf(cmd, sizeof cmd, "'%s' %s child %s 2>&1", self_path, flags, scenario);
#endif
    FILE *f = popen(cmd, "r");
    if (f == NULL)
        return -1;
    Buf raw = {0};
    char chunk[4096];
    size_t n;
    while ((n = fread(chunk, 1, sizeof chunk, f)) > 0)
        put(&raw, chunk, n);
    int status = pclose(f);
    put(&raw, "", 0);
    normalise(out, raw.p);
    buf_free(&raw);
    put(out, "", 0);
#ifdef _WIN32
    return status;
#else
    return WIFEXITED(status) ? WEXITSTATUS(status) : -1;
#endif
}

typedef struct Scenario {
    const char *flags;
    const char *name;
    int code;
    const char *want;
    bool prefix; /* want is only the start of the output */
} Scenario;

static const Scenario scenarios[] = {
    {"", "basic", 1,
     "--- FAIL: TestFail (0.00s)\n"
     "    testing_test.c:N: bad 1\n"
     "--- FAIL: TestSub (0.00s)\n"
     "    --- FAIL: TestSub/fail (0.00s)\n"
     "        testing_test.c:N: stop\n"
     "FAIL\n",
     false},
    {"-test.v", "basic", 1,
     "=== RUN   TestPass\n"
     "    testing_test.c:N: hello\n"
     "--- PASS: TestPass (0.00s)\n"
     "=== RUN   TestFail\n"
     "    testing_test.c:N: bad 1\n"
     "--- FAIL: TestFail (0.00s)\n"
     "=== RUN   TestSub\n"
     "=== RUN   TestSub/ok\n"
     "=== RUN   TestSub/fail\n"
     "    testing_test.c:N: stop\n"
     "=== RUN   TestSub/skip\n"
     "    testing_test.c:N: later\n"
     "--- FAIL: TestSub (0.00s)\n"
     "    --- PASS: TestSub/ok (0.00s)\n"
     "    --- FAIL: TestSub/fail (0.00s)\n"
     "    --- SKIP: TestSub/skip (0.00s)\n"
     "=== RUN   TestMulti\n"
     "    testing_test.c:N: one\n"
     "        two\n"
     "--- PASS: TestMulti (0.00s)\n"
     "FAIL\n",
     false},
    {"", "bare", 1,
     "--- FAIL: TestFail (0.00s)\n"
     "    testing_test.c:N: bad 1\n"
     "--- FAIL: TestSub (0.00s)\n"
     "    --- FAIL: TestSub/fail (0.00s)\n"
     "        testing_test.c:N: stop\n"
     "FAIL\n",
     false},
    {"-test.run=Pass -test.shuffle=5", "basic", 0, "-test.shuffle 5\nPASS\n", false},
    {"-test.run=Pass -test.v=test2json", "basic", 0,
     "\x16=== RUN   TestPass\n"
     "    testing_test.c:N: hello\n"
     "\x16--- PASS: TestPass (0.00s)\n"
     "\x16=== NAME  \n"
     "\x16PASS\n",
     false},
    {"-test.run=Sub/ok -v", "basic", 0,
     "=== RUN   TestSub\n"
     "=== RUN   TestSub/ok\n"
     "--- PASS: TestSub (0.00s)\n"
     "    --- PASS: TestSub/ok (0.00s)\n"
     "PASS\n",
     false},
    {"-test.run=Pass -test.skip=Pass", "basic", 0,
     "testing: warning: no tests to run\n"
     "PASS\n",
     false},
    {"-test.failfast", "basic", 1,
     "--- FAIL: TestFail (0.00s)\n"
     "    testing_test.c:N: bad 1\n"
     "FAIL\n",
     false},
    {"-test.list=Sub", "basic", 0, "TestSub\n", false},
    {"\"-test.list=a(\"", "basic", 1,
     "testing: invalid regexp in -test.list (\"a(\"): error parsing regexp: missing "
     "closing ): "
     "`a(`\n",
     false},
    {"\"-test.run=a(\"", "basic", 1,
     "testing: invalid regexp for element 0 of -test.run (\"a(\"): error parsing "
     "regexp: "
     "missing closing ): `a(`\n",
     false},
    {"-test.count=x", "basic", 2,
     "invalid value \"x\" for flag -test.count: parse error\nUsage of ", true},
    {"-test.nope", "basic", 2, "flag provided but not defined: -test.nope\nUsage of ",
     true},
    {"-test.parallel=0", "basic", 2,
     "testing: -parallel can only be given a positive integer\nUsage of ", true},
    {"-test.shuffle=x", "basic", 2,
     "testing: -shuffle should be \"off\", \"on\", or a valid integer: "
     "strconv.ParseInt: parsing "
     "\"x\": invalid syntax\n",
     false},
    {"-test.cpu=0", "basic", 1, "testing: invalid value \"0\" for -test.cpu\n", false},
    {"-test.v=bad", "basic", 2,
     "invalid boolean value \"bad\" for -test.v: invalid flag -test.v=bad\nUsage of ",
     true},
    {"", "panic", 2,
     "--- FAIL: TestPanic (0.00s)\n"
     "panic: boom",
     true},
    {"-test.timeout=300ms -test.v", "sleep", 2,
     "=== RUN   TestSleep\n"
     "panic: test timed out after 300ms\n"
     "running tests:\n"
     "\tTestSleep (0s)\n",
     false},
};

/* The same, for benchmarks. */
static const Scenario bench_scenarios[] = {
    {"-test.bench=. -test.benchtime=10x -test.cpu=1", "bench", 1,
     "goos: X\n"
     "goarch: X\n"
     "BenchmarkPlain    \t      10\tN ns/op\n"
     "BenchmarkLoop     \t      10\tN ns/op\n"
     "BenchmarkSub/a    \t      10\tN ns/op\t        42.00 widgets/op\n"
     "BenchmarkSub/b    \t      10\tN ns/op\n"
     "BenchmarkParallel \t      10\tN ns/op\n"
     "--- FAIL: BenchmarkFail\n"
     "    testing_test.c:N: broken\n"
     "BenchmarkLog      \t      10\tN ns/op\n"
     "--- BENCH: BenchmarkLog\n"
     "    testing_test.c:N: noted\n"
     "    testing_test.c:N: noted\n"
     "--- FAIL: BenchmarkBreak\n"
     "    testing.c:N: benchmark function returned without B.Loop() == false (break or "
     "return in loop?)\n"
     "BenchmarkAlloc    \t      10\tN ns/op\t      16 B/op\t       1 allocs/op\n"
     "FAIL\n",
     false},
    {"-test.bench=. -test.benchtime=10x -test.cpu=2 -test.v", "bench", 1,
     "=== RUN   TestPass\n"
     "    testing_test.c:N: hello\n"
     "--- PASS: TestPass (0.00s)\n"
     "goos: X\n"
     "goarch: X\n"
     "BenchmarkPlain\n"
     "BenchmarkPlain-2      \t      10\tN ns/op\n"
     "BenchmarkLoop\n"
     "BenchmarkLoop-2       \t      10\tN ns/op\n"
     "BenchmarkSub\n"
     "BenchmarkSub/a\n"
     "BenchmarkSub/a-2      \t      10\tN ns/op\t        42.00 widgets/op\n"
     "BenchmarkSub/b\n"
     "BenchmarkSub/b-2      \t      10\tN ns/op\n"
     "BenchmarkParallel\n"
     "BenchmarkParallel-2   \t      10\tN ns/op\n"
     "BenchmarkFail\n"
     "    testing_test.c:N: broken\n"
     "--- FAIL: BenchmarkFail\n"
     "BenchmarkSkip\n"
     "    testing_test.c:N: not today\n"
     "--- SKIP: BenchmarkSkip\n"
     "BenchmarkLog\n"
     "    testing_test.c:N: noted\n"
     "    testing_test.c:N: noted\n"
     "BenchmarkLog-2        \t      10\tN ns/op\n"
     "BenchmarkBreak\n"
     "    testing.c:N: benchmark function returned without B.Loop() == false (break or "
     "return in loop?)\n"
     "--- FAIL: BenchmarkBreak\n"
     "BenchmarkAlloc\n"
     "BenchmarkAlloc-2      \t      10\tN ns/op\t      16 B/op\t       1 allocs/op\n"
     "FAIL\n",
     false},
    {"-test.run=X -test.bench=Alloc -test.benchtime=10x -test.cpu=1 -test.benchmem",
     "bench", 0,
     "goos: X\n"
     "goarch: X\n"
     "BenchmarkAlloc \t      10\tN ns/op\t      16 B/op\t       1 allocs/op\n"
     "PASS\n",
     false},
    {"-test.run=X -test.bench=Plain -test.benchtime=3x -test.cpu=1,2 -test.count=2",
     "bench", 0,
     "goos: X\n"
     "goarch: X\n"
     "BenchmarkPlain     \t       3\tN ns/op\n"
     "BenchmarkPlain     \t       3\tN ns/op\n"
     "BenchmarkPlain-2   \t       3\tN ns/op\n"
     "BenchmarkPlain-2   \t       3\tN ns/op\n"
     "PASS\n",
     false},
    {"-test.run=Pass -test.bench=Sub/a -test.benchtime=3x -test.cpu=1 "
     "-test.v=test2json",
     "bench", 0,
     "\x16=== RUN   TestPass\n"
     "    testing_test.c:N: hello\n"
     "\x16--- PASS: TestPass (0.00s)\n"
     "\x16=== NAME  \n"
     "goos: X\n"
     "goarch: X\n"
     "\x16=== RUN   BenchmarkSub\n"
     "BenchmarkSub\n"
     "\x16=== RUN   BenchmarkSub/a\n"
     "BenchmarkSub/a\n"
     "BenchmarkSub/a         \t       3\tN ns/op\t        42.00 widgets/op\n"
     "\x16=== NAME  \n"
     "\x16PASS\n",
     false},
    {"-test.bench=Sub -test.benchtime=5x -test.cpu=1", "benchbare", 0,
     "goos: X\n"
     "goarch: X\n"
     "BenchmarkSub/a         \t       5\tN ns/op\t        42.00 widgets/op\n"
     "BenchmarkSub/b         \t       5\tN ns/op\n"
     "PASS\n",
     false},
    {"-test.run=X -test.bench=X", "bench", 0, "PASS\n", false},
    {"-test.benchtime=7x", "benchfunc", 0, "n=7 allocs=1 bytes=16\n", false},
};

static void check_scenarios(TestingT *t, const Scenario *list, size_t n) {
    for (size_t i = 0; i < n; i++) {
        const Scenario *sc = &list[i];
        Buf out = {0};
        int code = spawn(sc->flags, sc->name, &out);
        size_t wlen = strlen(sc->want);
        bool same = sc->prefix ? out.len >= wlen && memcmp(out.p, sc->want, wlen) == 0
                               : out.len == wlen && memcmp(out.p, sc->want, wlen) == 0;
        if (code != sc->code)
            testing_t_errorf_v(t, "%s %s: exit status %d, want %d", sc->flags, sc->name,
                               code, sc->code);
        if (!same)
            testing_t_errorf_v(t, "%s %s: output\n%s\nwant\n%s", sc->flags, sc->name,
                               str_from_bytes((const Byte *)out.p, (Int)out.len),
                               sc->want);
        buf_free(&out);
    }
}

static void TestBenchmarkOutput(TestingT *t) {
    if (self_path == NULL)
        testing_t_skip_v(t, "no path to this binary");
    check_scenarios(t, bench_scenarios,
                    sizeof bench_scenarios / sizeof bench_scenarios[0]);
}

static void TestOutput(TestingT *t) {
    if (self_path == NULL)
        testing_t_skip_v(t, "no path to this binary");
    check_scenarios(t, scenarios, sizeof scenarios / sizeof scenarios[0]);
}

static void TestHelpers(TestingT *t) {
    if (!testing_testing())
        testing_t_error_v(t, "Testing() is false inside a test");
    if (testing_short())
        testing_t_error_v(t, "Short() is true without -test.short");
    if (!str_eq(testing_cover_mode(), BURROW_S("")))
        testing_t_error_v(t, "CoverMode() is not empty");
    int64_t when;
    if (testing_t_deadline(t, &when))
        testing_t_error_v(t, "a deadline without -test.timeout");
    if (context_err(testing_t_context(t)).vt != NULL)
        testing_t_error_v(t, "the context is done while the test runs");
}

#define TESTS(X)                                                                       \
    X(TestMatchString)                                                                 \
    X(TestMatchStringErrors)                                                           \
    X(TestSubtestNames)                                                                \
    X(TestCleanupOrder)                                                                \
    X(TestSkip)                                                                        \
    X(TestParallel)                                                                    \
    X(TestHelpers)                                                                     \
    X(TestOutput)                                                                      \
    X(TestBenchmarkOutput)

int main(int argc, char **argv) {
    if (argc >= 3 && strcmp(argv[argc - 2], "child") == 0) {
        testing_init(argc, argv);
        return run_child(argv[argc - 1]);
    }
    self_path = argv[0];
    static const burrow__TestingEntry entries[] = {TESTS(BURROW__TESTING_ENTRY)};
    return burrow__testing_main(argc, argv, entries,
                                (Int)(sizeof entries / sizeof entries[0]), false,
                                testing_m_run);
}
