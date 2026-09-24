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
#include "burrow/pal.h"
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

/* The examples the child runs. One prints through stdio and fmt both, since
 * the capture has to catch the two. */
static void child_example_hello(void *env) {
    (void)env;
    printf("hello from printf\n");
    fmt_println_v(BURROW_S("hello from fmt"));
}

static void child_example_wrong(void *env) {
    (void)env;
    fmt_println_v(BURROW_S("one"));
}

static void child_example_lines(void *env) {
    (void)env;
    fmt_print_v(BURROW_S("c\nb\na\n"));
}

static void child_example_lines_wrong(void *env) {
    (void)env;
    fmt_print_v(BURROW_S("c\na\n"));
}

static void child_example_panic(void *env) {
    (void)env;
    fmt_println_v(BURROW_S("before"));
    panic_str(BURROW_S("boom"));
}

/* The fuzz targets the child runs, the same ones as a Go program whose
 * output the scenarios below were checked against. */
static void child_fuzz_good_fn(void *env, TestingT *t, Slice args) {
    (void)env;
    testing_t_log_v(t, "got", testing_fuzz_arg(args, 0, Str),
                    testing_fuzz_arg(args, 1, Int));
}

static void child_fuzz_good(void *env, TestingF *f) {
    (void)env;
    testing_f_add_v(f, "hello", (Int)5);
    testing_f_add_v(f, "", (Int)0);
    testing_f_fuzz_v(f, BURROW_FN(TestingFuzzFunc, child_fuzz_good_fn, NULL),
                     TYPE_STRING, TYPE_INT);
}

static void child_fuzz_bad_fn(void *env, TestingT *t, Slice args) {
    (void)env;
    Str s = testing_fuzz_arg(args, 0, Str);
    if (s.len > 0 && s.p[0] == 'x')
        testing_t_errorf_v(t, "bad %q", s);
}

static void child_fuzz_bad(void *env, TestingF *f) {
    (void)env;
    testing_f_add_v(f, "abc");
    testing_f_add_v(f, "xyz");
    testing_f_fuzz_v(f, BURROW_FN(TestingFuzzFunc, child_fuzz_bad_fn, NULL),
                     TYPE_STRING);
}

static void child_fuzz_no_call(void *env, TestingF *f) {
    (void)env;
    testing_f_log_v(f, "setup");
}

static void child_fuzz_nothing(void *env, TestingT *t, Slice args) {
    (void)env;
    (void)t;
    (void)args;
}

static void child_fuzz_mismatch(void *env, TestingF *f) {
    (void)env;
    testing_f_add_v(f, (Int)1);
    testing_f_fuzz_v(f, BURROW_FN(TestingFuzzFunc, child_fuzz_nothing, NULL),
                     TYPE_STRING);
}

static void child_fuzz_count(void *env, TestingF *f) {
    (void)env;
    testing_f_add_v(f, (Int)1, (Int)2);
    testing_f_fuzz_v(f, BURROW_FN(TestingFuzzFunc, child_fuzz_nothing, NULL), TYPE_INT);
}

static void child_fuzz_skip(void *env, TestingF *f) {
    (void)env;
    testing_f_skip_v(f, "not today");
}

static void child_fuzz_empty(void *env, TestingF *f) {
    (void)env;
    testing_f_fuzz_v(f, BURROW_FN(TestingFuzzFunc, child_fuzz_nothing, NULL),
                     TYPE_BYTES);
}

/* Calls F's Log from inside the fuzz function, which Go stops with a panic. */
static void child_fuzz_inside_fn(void *env, TestingT *t, Slice args) {
    (void)t;
    (void)args;
    testing_f_log_v((TestingF *)env, "no");
}

static void child_fuzz_inside(void *env, TestingF *f) {
    (void)env;
    testing_f_add_v(f, (bool)true);
    testing_f_fuzz_v(f, BURROW_FN(TestingFuzzFunc, child_fuzz_inside_fn, f), TYPE_BOOL);
}

/* The targets -test.fuzz runs, in the fuzzgen scenario. FuzzLong fails on
 * any input longer than three bytes, which the mutator finds at once and the
 * minimizer can only bring down to four. */
static void child_fuzz_long_fn(void *env, TestingT *t, Slice args) {
    (void)env;
    Bytes b = testing_fuzz_arg(args, 0, Bytes);
    if (b.len > 3)
        testing_t_errorf_v(t, "too long: %d bytes", b.len);
}

static void child_fuzz_long(void *env, TestingF *f) {
    (void)env;
    Byte seed[] = {'a', 'b'};
    testing_f_add_v(f, slice_from(seed, 2, 2, TYPE_BYTE));
    testing_f_fuzz_v(f, BURROW_FN(TestingFuzzFunc, child_fuzz_long_fn, NULL),
                     TYPE_BYTES);
}

static void child_fuzz_panic_fn(void *env, TestingT *t, Slice args) {
    (void)env;
    (void)t;
    Str s = testing_fuzz_arg(args, 0, Str);
    if (s.len > 5)
        panic_str(BURROW_S("too much"));
}

static void child_fuzz_panic(void *env, TestingF *f) {
    (void)env;
    testing_f_add_v(f, "abc");
    testing_f_fuzz_v(f, BURROW_FN(TestingFuzzFunc, child_fuzz_panic_fn, NULL),
                     TYPE_STRING);
}

/* Takes the worker down with it, which the coordinator has to notice. _Exit
 * and not exit, so that a leak check at exit under a sanitizer does not turn
 * the status into its own. */
static void child_fuzz_exit_fn(void *env, TestingT *t, Slice args) {
    (void)env;
    (void)t;
    Str s = testing_fuzz_arg(args, 0, Str);
    if (s.len > 5)
        _Exit(3);
}

static void child_fuzz_exit(void *env, TestingF *f) {
    (void)env;
    testing_f_add_v(f, "abc");
    testing_f_fuzz_v(f, BURROW_FN(TestingFuzzFunc, child_fuzz_exit_fn, NULL),
                     TYPE_STRING);
}

static void child_fuzz_fine_fn(void *env, TestingT *t, Slice args) {
    (void)env;
    (void)t;
    (void)testing_fuzz_arg(args, 0, Str);
    (void)testing_fuzz_arg(args, 1, Int);
}

static void child_fuzz_fine(void *env, TestingF *f) {
    (void)env;
    testing_f_add_v(f, "x", (Int)1);
    testing_f_fuzz_v(f, BURROW_FN(TestingFuzzFunc, child_fuzz_fine_fn, NULL),
                     TYPE_STRING, TYPE_INT);
}

/* The targets that read testdata/fuzz, which fuzz_dir sets up. */
static void child_fuzz_seeds_fn(void *env, TestingT *t, Slice args) {
    (void)env;
    testing_t_log_v(t, "got", testing_fuzz_arg(args, 0, Str),
                    testing_fuzz_arg(args, 1, Int));
}

static void child_fuzz_seeds(void *env, TestingF *f) {
    (void)env;
    testing_f_add_v(f, "hello", (Int)5);
    testing_f_fuzz_v(f, BURROW_FN(TestingFuzzFunc, child_fuzz_seeds_fn, NULL),
                     TYPE_STRING, TYPE_INT);
}

static void child_fuzz_ran(void *env, TestingT *t, Slice args) {
    (void)env;
    testing_t_log_v(t, "ran", testing_fuzz_arg(args, 0, Str));
}

static void child_fuzz_broken(void *env, TestingF *f) {
    (void)env;
    testing_f_fuzz_v(f, BURROW_FN(TestingFuzzFunc, child_fuzz_ran, NULL), TYPE_STRING);
}

static void child_fuzz_no_dir(void *env, TestingF *f) {
    (void)env;
    testing_f_add_v(f, "only");
    testing_f_fuzz_v(f, BURROW_FN(TestingFuzzFunc, child_fuzz_ran, NULL), TYPE_STRING);
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
    TestingInternalExample examples[5];
    Int ne = 0;
    TestingInternalFuzzTarget fuzz[8];
    Int nf = 0;
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
    } else if (strcmp(scenario, "examples") == 0) {
        tests[n++] = (TestingInternalTest){BURROW_S("TestPass"), {child_pass, NULL}};
        examples[ne++] =
            (TestingInternalExample){BURROW_S("ExampleHello"),
                                     {child_example_hello, NULL},
                                     BURROW_S("hello from printf\nhello from fmt\n"),
                                     false};
        examples[ne++] = (TestingInternalExample){BURROW_S("ExampleWrong"),
                                                  {child_example_wrong, NULL},
                                                  BURROW_S("two"),
                                                  false};
        examples[ne++] = (TestingInternalExample){BURROW_S("ExampleLines"),
                                                  {child_example_lines, NULL},
                                                  BURROW_S("a\nb\nc"),
                                                  true};
        examples[ne++] = (TestingInternalExample){BURROW_S("ExampleLinesWrong"),
                                                  {child_example_lines_wrong, NULL},
                                                  BURROW_S("a\nb"),
                                                  true};
        examples[ne++] = (TestingInternalExample){BURROW_S("ExamplePanic"),
                                                  {child_example_panic, NULL},
                                                  BURROW_S("before"),
                                                  false};
    } else if (strcmp(scenario, "fuzz") == 0) {
        fuzz[nf++] =
            (TestingInternalFuzzTarget){BURROW_S("FuzzGood"), {child_fuzz_good, NULL}};
        fuzz[nf++] =
            (TestingInternalFuzzTarget){BURROW_S("FuzzBad"), {child_fuzz_bad, NULL}};
        fuzz[nf++] = (TestingInternalFuzzTarget){BURROW_S("FuzzNoCall"),
                                                 {child_fuzz_no_call, NULL}};
        fuzz[nf++] = (TestingInternalFuzzTarget){BURROW_S("FuzzMismatch"),
                                                 {child_fuzz_mismatch, NULL}};
        fuzz[nf++] = (TestingInternalFuzzTarget){BURROW_S("FuzzCount"),
                                                 {child_fuzz_count, NULL}};
        fuzz[nf++] =
            (TestingInternalFuzzTarget){BURROW_S("FuzzSkip"), {child_fuzz_skip, NULL}};
        fuzz[nf++] = (TestingInternalFuzzTarget){BURROW_S("FuzzEmpty"),
                                                 {child_fuzz_empty, NULL}};
        fuzz[nf++] = (TestingInternalFuzzTarget){BURROW_S("FuzzInside"),
                                                 {child_fuzz_inside, NULL}};
    } else if (strcmp(scenario, "fuzzdir") == 0) {
        char dir[1024];
        snprintf(dir, sizeof dir, "%s.fuzz", self_path);
        PalErrno err;
        if (!pal_chdir(dir, &err)) {
            fprintf(stderr, "chdir %s: %s\n", dir, pal_errno_string(err));
            return 3;
        }
        fuzz[nf++] = (TestingInternalFuzzTarget){BURROW_S("FuzzSeeds"),
                                                 {child_fuzz_seeds, NULL}};
        fuzz[nf++] = (TestingInternalFuzzTarget){BURROW_S("FuzzBroken"),
                                                 {child_fuzz_broken, NULL}};
        fuzz[nf++] = (TestingInternalFuzzTarget){BURROW_S("FuzzNoDir"),
                                                 {child_fuzz_no_dir, NULL}};
    } else if (strcmp(scenario, "fuzzgen") == 0) {
        char dir[1024];
        snprintf(dir, sizeof dir, "%s.gen", self_path);
        PalErrno err;
        if (!pal_mkdir(dir, 0755, &err) && err != PAL_EEXIST) {
            fprintf(stderr, "mkdir %s: %s\n", dir, pal_errno_string(err));
            return 3;
        }
        if (!pal_chdir(dir, &err)) {
            fprintf(stderr, "chdir %s: %s\n", dir, pal_errno_string(err));
            return 3;
        }
        fuzz[nf++] =
            (TestingInternalFuzzTarget){BURROW_S("FuzzLong"), {child_fuzz_long, NULL}};
        fuzz[nf++] = (TestingInternalFuzzTarget){BURROW_S("FuzzPanic"),
                                                 {child_fuzz_panic, NULL}};
        fuzz[nf++] =
            (TestingInternalFuzzTarget){BURROW_S("FuzzFine"), {child_fuzz_fine, NULL}};
        fuzz[nf++] =
            (TestingInternalFuzzTarget){BURROW_S("FuzzExit"), {child_fuzz_exit, NULL}};
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
        slice_from(fuzz, nf, nf, TYPE_TESTING_INTERNAL_FUZZ_TARGET),
        slice_from(examples, ne, ne, TYPE_TESTING_INTERNAL_EXAMPLE));
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

/* Writes the testdata/fuzz tree the fuzzdir scenario runs in, next to this
 * binary so that nothing lands in the source tree. */
static bool put_file(const char *path, const char *text, PalErrno *err) {
    int64_t fd = pal_open(path, PAL_O_WRONLY | PAL_O_CREATE | PAL_O_TRUNC, 0644, err);
    if (fd < 0)
        return false;
    int64_t n = (int64_t)strlen(text);
    bool ok = pal_write(fd, text, n, err) == n;
    return pal_close(fd, err) && ok;
}

static bool fuzz_dir(TestingT *t) {
    static const char *const dirs[] = {"", "/testdata", "/testdata/fuzz",
                                       "/testdata/fuzz/FuzzSeeds",
                                       "/testdata/fuzz/FuzzBroken"};
    static const char *const files[][2] = {
        {"/testdata/fuzz/FuzzSeeds/aaa",
         "go test fuzz v1\nstring(\"from file\")\nint(7)\n"},
        {"/testdata/fuzz/FuzzBroken/bad", "go test fuzz v1\nstring(\"x\" +)\n"},
        {"/testdata/fuzz/FuzzBroken/wrong", "go test fuzz v1\nint(1)\n"},
        {"/testdata/fuzz/FuzzBroken/zok", "go test fuzz v1\nstring(\"fine\")\n"},
    };
    char path[1024];
    PalErrno err = PAL_OK;
    for (size_t i = 0; i < sizeof dirs / sizeof dirs[0]; i++) {
        snprintf(path, sizeof path, "%s.fuzz%s", self_path, dirs[i]);
        if (!pal_mkdir(path, 0755, &err) && err != PAL_EEXIST) {
            testing_t_errorf_v(t, "mkdir %s: %s", path, pal_errno_string(err));
            return false;
        }
    }
    for (size_t i = 0; i < sizeof files / sizeof files[0]; i++) {
        snprintf(path, sizeof path, "%s.fuzz%s", self_path, files[i][0]);
        if (!put_file(path, files[i][1], &err)) {
            testing_t_errorf_v(t, "write %s: %s", path, pal_errno_string(err));
            return false;
        }
    }
    return true;
}

#ifdef _WIN32
#define FUZZ_DIR "testdata\\\\fuzz\\\\FuzzBroken\\\\"
#else
#define FUZZ_DIR "testdata/fuzz/FuzzBroken/"
#endif

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
    {"-test.v -test.skip=Panic", "examples", 1,
     "=== RUN   TestPass\n"
     "    testing_test.c:N: hello\n"
     "--- PASS: TestPass (0.00s)\n"
     "=== RUN   ExampleHello\n"
     "--- PASS: ExampleHello (0.00s)\n"
     "=== RUN   ExampleWrong\n"
     "--- FAIL: ExampleWrong (0.00s)\n"
     "got:\n"
     "one\n"
     "want:\n"
     "two\n"
     "=== RUN   ExampleLines\n"
     "--- PASS: ExampleLines (0.00s)\n"
     "=== RUN   ExampleLinesWrong\n"
     "--- FAIL: ExampleLinesWrong (0.00s)\n"
     "got:\n"
     "c\n"
     "a\n"
     "\n"
     "want (unordered):\n"
     "a\n"
     "b\n"
     "FAIL\n",
     false},
    {"-test.run=Hello", "examples", 0, "PASS\n", false},
    {"-test.run=Nothing", "examples", 0, "testing: warning: no tests to run\nPASS\n",
     false},
    {"-test.v=test2json -test.run=Hello", "examples", 0,
     "\x16=== RUN   ExampleHello\n"
     "\x16--- PASS: ExampleHello (0.00s)\n"
     "\x16=== NAME   \n"
     "\x16PASS\n",
     false},
    {"-test.run=Panic", "examples", 2,
     "--- FAIL: ExamplePanic (0.00s)\n"
     "panic: boom",
     true},
    {"-test.skip=FuzzInside", "fuzz", 1,
     "--- FAIL: FuzzBad (0.00s)\n"
     "    --- FAIL: FuzzBad/seed#1 (0.00s)\n"
     "        testing_test.c:N: bad \"xyz\"\n"
     "--- FAIL: FuzzNoCall (0.00s)\n"
     "    testing_test.c:N: setup\n"
     "    testing.c:N: returned without calling F.Fuzz, F.Fail, or F.Skip\n"
     "--- FAIL: FuzzMismatch (0.00s)\n"
     "    testing_test.c:N: mismatched types in corpus entry: [int], want [string]\n"
     "--- FAIL: FuzzCount (0.00s)\n"
     "    testing_test.c:N: wrong number of values in corpus entry: 2, want 1\n"
     "FAIL\n",
     false},
    {"-test.v -test.skip=FuzzInside", "fuzz", 1,
     "=== RUN   FuzzGood\n"
     "=== RUN   FuzzGood/seed#0\n"
     "    testing_test.c:N: got hello 5\n"
     "=== RUN   FuzzGood/seed#1\n"
     "    testing_test.c:N: got  0\n"
     "--- PASS: FuzzGood (0.00s)\n"
     "    --- PASS: FuzzGood/seed#0 (0.00s)\n"
     "    --- PASS: FuzzGood/seed#1 (0.00s)\n"
     "=== RUN   FuzzBad\n"
     "=== RUN   FuzzBad/seed#0\n"
     "=== RUN   FuzzBad/seed#1\n"
     "    testing_test.c:N: bad \"xyz\"\n"
     "--- FAIL: FuzzBad (0.00s)\n"
     "    --- PASS: FuzzBad/seed#0 (0.00s)\n"
     "    --- FAIL: FuzzBad/seed#1 (0.00s)\n"
     "=== RUN   FuzzNoCall\n"
     "    testing_test.c:N: setup\n"
     "    testing.c:N: returned without calling F.Fuzz, F.Fail, or F.Skip\n"
     "--- FAIL: FuzzNoCall (0.00s)\n"
     "=== RUN   FuzzMismatch\n"
     "    testing_test.c:N: mismatched types in corpus entry: [int], want [string]\n"
     "--- FAIL: FuzzMismatch (0.00s)\n"
     "=== RUN   FuzzCount\n"
     "    testing_test.c:N: wrong number of values in corpus entry: 2, want 1\n"
     "--- FAIL: FuzzCount (0.00s)\n"
     "=== RUN   FuzzSkip\n"
     "    testing_test.c:N: not today\n"
     "--- SKIP: FuzzSkip (0.00s)\n"
     "=== RUN   FuzzEmpty\n"
     "--- PASS: FuzzEmpty (0.00s)\n"
     "FAIL\n",
     false},
    {"-test.run=FuzzBad/seed#1 -test.v", "fuzz", 1,
     "=== RUN   FuzzBad\n"
     "=== RUN   FuzzBad/seed#1\n"
     "    testing_test.c:N: bad \"xyz\"\n"
     "--- FAIL: FuzzBad (0.00s)\n"
     "    --- FAIL: FuzzBad/seed#1 (0.00s)\n"
     "FAIL\n",
     false},
    {"-test.run=FuzzGood -test.count=2", "fuzz", 0, "PASS\n", false},
    {"-test.run=Nothing", "fuzz", 0, "testing: warning: no tests to run\nPASS\n",
     false},
    {"-test.run=FuzzInside", "fuzz", 2,
     "--- FAIL: FuzzInside (0.00s)\n"
     "    --- FAIL: FuzzInside/seed#0 (0.00s)\n"
     "panic: testing: f.Log was called inside the fuzz target, use t.Log instead",
     true},
    {"-test.v", "fuzzdir", 1,
     "=== RUN   FuzzSeeds\n"
     "=== RUN   FuzzSeeds/seed#0\n"
     "    testing_test.c:N: got hello 5\n"
     "=== RUN   FuzzSeeds/aaa\n"
     "    testing_test.c:N: got from file 7\n"
     "--- PASS: FuzzSeeds (0.00s)\n"
     "    --- PASS: FuzzSeeds/seed#0 (0.00s)\n"
     "    --- PASS: FuzzSeeds/aaa (0.00s)\n"
     "=== RUN   FuzzBroken\n"
     "    testing_test.c:N: \"" FUZZ_DIR "bad\": unmarshal: malformed line "
     "\"string(\\\"x\\\" +)\": (test):1:13: expected operand, found ')'\n"
     "        \"" FUZZ_DIR "wrong\": mismatched types in corpus entry: [int], want "
     "[string]\n"
     "--- FAIL: FuzzBroken (0.00s)\n"
     "=== RUN   FuzzNoDir\n"
     "=== RUN   FuzzNoDir/seed#0\n"
     "    testing_test.c:N: ran only\n"
     "--- PASS: FuzzNoDir (0.00s)\n"
     "    --- PASS: FuzzNoDir/seed#0 (0.00s)\n"
     "FAIL\n",
     false},
    {"-test.run=FuzzSeeds/aaa", "fuzzdir", 0, "PASS\n", false},
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
    if (!fuzz_dir(t))
        return;
    check_scenarios(t, scenarios, sizeof scenarios / sizeof scenarios[0]);
}

/* -test.fuzz, for real: the child coordinates, its workers run the inputs,
 * and a failing input ends up minimized in testdata. What the engine logs
 * depends on the machine, so only the parts that do not are checked. */
#ifdef _WIN32
#define GEN_SEP "\\"
#else
#define GEN_SEP "/"
#endif

static bool has(const Buf *out, const char *want) {
    return out->p != NULL && strstr(out->p, want) != NULL;
}

/* The file a crash went to, relative to the child's directory, which the
 * test removes so that the next run starts clean. */
static void forget_crash(const Buf *out, const char *target) {
    static const char intro[] = "Failing input written to ";
    const char *p = out->p != NULL ? strstr(out->p, intro) : NULL;
    char rel[512];
    if (p != NULL) {
        p += sizeof intro - 1;
        size_t n = strcspn(p, "\n");
        snprintf(rel, sizeof rel, "%.*s", (int)n, p);
    } else {
        snprintf(rel, sizeof rel, "testdata" GEN_SEP "fuzz" GEN_SEP "%s", target);
    }
    char path[1024];
    snprintf(path, sizeof path, "%s.gen" GEN_SEP "%s", self_path, rel);
    pal_unlink(path, NULL);
}

static bool read_back(const char *path, char *buf, size_t cap) {
    int64_t fd = pal_open(path, PAL_O_RDONLY, 0, NULL);
    if (fd < 0)
        return false;
    int64_t n = pal_read(fd, buf, (int64_t)cap - 1, NULL);
    pal_close(fd, NULL);
    if (n < 0)
        return false;
    buf[n] = '\0';
    return true;
}

static void TestFuzzing(TestingT *t) {
    if (self_path == NULL)
        testing_t_skip_v(t, "no path to this binary");
    if (testing_short())
        testing_t_skip_v(t, "starts fuzz workers");
    char flags[2048];
    char cache[1024];
    snprintf(cache, sizeof cache, "%s.cache", self_path);

    /* A failing input: found, minimized to the four bytes the target still
     * fails on, and written where a plain run picks it up as a seed. */
    snprintf(flags, sizeof flags, "-test.fuzz=FuzzLong -test.fuzzcachedir=\"%s\"",
             cache);
    Buf out = {0};
    int code = spawn(flags, "fuzzgen", &out);
    if (code != 1)
        testing_t_errorf_v(t, "FuzzLong: exit status %d, want 1", code);
    const char *want[] = {
        "--- FAIL: FuzzLong (0.00s)\n"
        "    --- FAIL: FuzzLong (0.00s)\n"
        "        testing_test.c:N: too long: 4 bytes\n",
        "    Failing input written to testdata" GEN_SEP "fuzz" GEN_SEP
        "FuzzLong" GEN_SEP "06ba4bdb19de593e\n",
        "    To re-run:\n",
        "-test.run=FuzzLong/06ba4bdb19de593e\nFAIL\n",
    };
    for (size_t i = 0; i < sizeof want / sizeof want[0]; i++)
        if (!has(&out, want[i]))
            testing_t_errorf_v(t, "FuzzLong: output\n%s\nwant it to hold\n%s", out.p,
                               want[i]);
    char path[1024];
    char text[256];
    snprintf(path, sizeof path,
             "%s.gen" GEN_SEP "testdata" GEN_SEP "fuzz" GEN_SEP "FuzzLong" GEN_SEP
             "06ba4bdb19de593e",
             self_path);
    if (!read_back(path, text, sizeof text))
        testing_t_errorf_v(t, "no crash file at %s", path);
    else if (strcmp(text, "go test fuzz v1\n[]byte(\"0000\")\n") != 0)
        testing_t_errorf_v(t, "crash file holds %q", text);

    /* Now a seed, which fails the run before any fuzzing starts. */
    Buf again = {0};
    code = spawn("-test.run=FuzzLong", "fuzzgen", &again);
    if (code != 1 || !has(&again, "    --- FAIL: FuzzLong/06ba4bdb19de593e (0.00s)\n"))
        testing_t_errorf_v(t, "rerun: exit status %d, output\n%s", code, again.p);
    buf_free(&again);
    forget_crash(&out, "FuzzLong");
    buf_free(&out);

    /* A panic fails the input, stack and all, rather than the worker. */
    snprintf(flags, sizeof flags, "-test.fuzz=FuzzPanic -test.fuzzcachedir=\"%s\"",
             cache);
    out = (Buf){0};
    code = spawn(flags, "fuzzgen", &out);
    if (code != 1 || !has(&out, "    --- FAIL: FuzzPanic (0.00s)\n") ||
        !has(&out, ": panic: too much\n") || !has(&out, "goroutine "))
        testing_t_errorf_v(t, "FuzzPanic: exit status %d, output\n%s", code, out.p);
    forget_crash(&out, "FuzzPanic");
    buf_free(&out);

    /* A worker that exits takes the input it was on down with it, and that
     * input is written out as it was, since there is nothing to minimize in. */
    snprintf(flags, sizeof flags, "-test.fuzz=FuzzExit -test.fuzzcachedir=\"%s\"",
             cache);
    out = (Buf){0};
    code = spawn(flags, "fuzzgen", &out);
    if (code != 1 ||
        !has(&out,
             "fuzzing process hung or terminated unexpectedly: exit status 3\n") ||
        !has(&out, "Failing input written to testdata"))
        testing_t_errorf_v(t, "FuzzExit: exit status %d, output\n%s", code, out.p);
    forget_crash(&out, "FuzzExit");
    buf_free(&out);

    /* A target that holds up, for a fixed number of inputs. */
    snprintf(flags, sizeof flags,
             "-test.fuzz=FuzzFine -test.fuzztime=300x -test.parallel=2 "
             "-test.fuzzcachedir=\"%s\"",
             cache);
    out = (Buf){0};
    code = spawn(flags, "fuzzgen", &out);
    if (code != 0 || !has(&out, "now fuzzing with 2 workers\n") ||
        !has(&out, "execs: 300 (") || !has(&out, "\nPASS\n"))
        testing_t_errorf_v(t, "FuzzFine: exit status %d, output\n%s", code, out.p);
    buf_free(&out);

    snprintf(flags, sizeof flags, "-test.fuzz=Fuzz -test.fuzzcachedir=\"%s\"", cache);
    out = (Buf){0};
    code = spawn(flags, "fuzzgen", &out);
    if (code != 1 ||
        strcmp(out.p, "testing: will not fuzz, -fuzz matches more than one fuzz test: "
                      "[FuzzLong FuzzPanic FuzzFine FuzzExit]\nFAIL\n") != 0)
        testing_t_errorf_v(t, "-fuzz=Fuzz: exit status %d, output\n%s", code, out.p);
    buf_free(&out);

    snprintf(flags, sizeof flags, "-test.fuzz=Nothing -test.fuzzcachedir=\"%s\"",
             cache);
    out = (Buf){0};
    code = spawn(flags, "fuzzgen", &out);
    if (code != 0 ||
        strcmp(out.p, "testing: warning: no fuzz tests to fuzz\nPASS\n") != 0)
        testing_t_errorf_v(t, "-fuzz=Nothing: exit status %d, output\n%s", code, out.p);
    buf_free(&out);
}

/* Examples in this binary's own list, which run in process with its standard
 * output captured, the way every test binary's examples do. */
static void ExampleCapture(void) {
    printf("printf\n");
    fflush(stdout);
    fmt_printf_v("fmt %d\n", 2);
    fmt_println_v(BURROW_S("  trailing space goes  "));
}

static void ExampleUnordered(void) {
    for (int i = 3; i > 0; i--)
        printf("%d\n", i);
}

/* Listed without output, so it is compiled and never run. */
static void ExampleNeverRun(void) {
    abort();
}

/* ------------------------------------------------------------- fuzz targets */

/* The text of the panic the last guarded call raised, or nothing. */
static char caught_text[200];

static void keep_caught_text(Any p) {
    caught_text[0] = '\0';
    if (p.t != TYPE_STRING)
        return;
    Str s = *(const Str *)p.data;
    size_t n =
        s.len < (Int)sizeof caught_text - 1 ? (size_t)s.len : sizeof caught_text - 1;
    memcpy(caught_text, s.p, n);
    caught_text[n] = '\0';
}

/* Calls fn and keeps the text of the panic it raises. The calls are
 * functions of their own so that nothing the setjmp can clobber lives here. */
static void guard(void (*fn)(void *), void *env) {
    caught_text[0] = '\0';
    BURROW_TRY {
        fn(env);
    }
    BURROW_CATCH(p) {
        keep_caught_text(p);
    }
    BURROW_TRY_END;
}

static void call_f_fail(void *f) {
    testing_f_fail((TestingF *)f);
}

static void call_f_skipped(void *f) {
    (void)testing_f_skipped((TestingF *)f);
}

static void call_f_context(void *f) {
    (void)testing_f_context((TestingF *)f);
}

static void call_arg_wrong_type(void *args) {
    (void)testing_fuzz_arg(*(Slice *)args, 0, Int);
}

static void call_arg_past_end(void *args) {
    (void)testing_fuzz_arg(*(Slice *)args, 15, Int);
}

static void fuzz_types_fn(void *env, TestingT *t, Slice args);

static void call_add_uintptr(void *f) {
    testing_f_add_v((TestingF *)f, BURROW_ANY_VAL(TYPE_UINTPTR, Uintptr, 1));
}

static void call_fuzz_no_types(void *f) {
    testing_f_fuzz((TestingF *)f, BURROW_FN(TestingFuzzFunc, fuzz_types_fn, NULL),
                   slice_nil(TYPE_UNSAFE_POINTER));
}

static void call_fuzz_int(void *f) {
    testing_f_fuzz_v((TestingF *)f, BURROW_FN(TestingFuzzFunc, fuzz_types_fn, NULL),
                     TYPE_INT);
}

static void call_fuzz_uintptr(void *f) {
    testing_f_fuzz_v((TestingF *)f, BURROW_FN(TestingFuzzFunc, fuzz_types_fn, NULL),
                     TYPE_UINTPTR);
}

static int32_t fuzz_seeds_run;

/* Every type a fuzz function can take, two seeds of them, the second all
 * zeroes. The seeds go parallel, which keeps the F alive past its target. */
static void fuzz_types_fn(void *env, TestingT *t, Slice args) {
    TestingF *f = (TestingF *)env;
    guard(call_f_fail, f);
    if (strcmp(
            caught_text,
            "testing: f.Fail was called inside the fuzz target, use t.Fail instead") !=
        0)
        testing_t_errorf_v(t, "f.Fail inside: %q", caught_text);
    guard(call_f_skipped, f);
    if (strcmp(caught_text, "testing: f.Skipped was called inside the fuzz target, use "
                            "t.Skipped instead") != 0)
        testing_t_errorf_v(t, "f.Skipped inside: %q", caught_text);
    guard(call_f_context, f);
    if (strcmp(caught_text, "testing: f.Context was called inside the fuzz target, use "
                            "t.Context instead") != 0)
        testing_t_errorf_v(t, "f.Context inside: %q", caught_text);
    testing_t_parallel(t);
    bool first = testing_fuzz_arg(args, 2, bool);
    Str s = testing_fuzz_arg(args, 0, Str);
    Bytes b = testing_fuzz_arg(args, 1, Bytes);
    Byte by = testing_fuzz_arg(args, 3, Byte);
    Rune r = testing_fuzz_arg(args, 4, Rune);
    float f32 = testing_fuzz_arg(args, 5, float);
    double f64 = testing_fuzz_arg(args, 6, double);
    Int i = testing_fuzz_arg(args, 7, Int);
    int8_t i8 = testing_fuzz_arg(args, 8, int8_t);
    int16_t i16 = testing_fuzz_arg(args, 9, int16_t);
    int64_t i64 = testing_fuzz_arg(args, 10, int64_t);
    Uint u = testing_fuzz_arg(args, 11, Uint);
    uint16_t u16 = testing_fuzz_arg(args, 12, uint16_t);
    uint32_t u32 = testing_fuzz_arg(args, 13, uint32_t);
    uint64_t u64 = testing_fuzz_arg(args, 14, uint64_t);
    if (first) {
        if (!str_eq(s, BURROW_S("h\xc3\xa9llo")) || b.len != 3 ||
            memcmp(b.p, "raw", 3) != 0 || by != 'x' || r != 0x263A || f32 != 1.5f ||
            f64 != -2.25 || i != -7 || i8 != -8 || i16 != 300 || i64 != INT64_MIN ||
            u != 7 || u16 != 65535 || u32 != 70000 || u64 != UINT64_MAX)
            testing_t_errorf_v(
                t, "first seed came back as %q %q %v %v %v %v %v %v %v %v %v %v %v %v",
                s, b, by, r, f32, f64, i, i8, i16, i64, u, u16, u32, u64);
    } else if (s.len != 0 || b.len != 0 || by != 0 || r != 0 || f32 != 0 || f64 != 0 ||
               i != 0 || i8 != 0 || i16 != 0 || i64 != 0 || u != 0 || u16 != 0 ||
               u32 != 0 || u64 != 0) {
        testing_t_error_v(t, "second seed is not all zeroes");
    }

    guard(call_arg_wrong_type, &args);
    if (strcmp(caught_text, "testing: fuzz argument 0 is string, not int") != 0)
        testing_t_errorf_v(t, "asking for the wrong type: %q", caught_text);
    guard(call_arg_past_end, &args);
    if (strcmp(caught_text, "testing: fuzz argument 15 out of range with 15 values") !=
        0)
        testing_t_errorf_v(t, "asking past the end: %q", caught_text);
    sync_atomic_add_int32(&fuzz_seeds_run, 1);
}

static void seed_count_cleanup(void *env) {
    TestingT *t = (TestingT *)env;
    if (sync_atomic_load_int32(&fuzz_seeds_run) != 2)
        testing_t_errorf_v(t, "%d seeds ran, want 2",
                           sync_atomic_load_int32(&fuzz_seeds_run));
}

static void FuzzTypes(TestingF *f) {
    Byte raw[] = {'r', 'a', 'w'};
    testing_f_add_v(f, "h\xc3\xa9llo", slice_from(raw, 3, 3, TYPE_BYTE), (bool)true,
                    (Byte)'x', BURROW_ANY_VAL(TYPE_RUNE, Rune, 0x263A), 1.5f, -2.25,
                    (Int)-7, (int8_t)-8, (int16_t)300,
                    BURROW_ANY_VAL(TYPE_INT64, int64_t, INT64_MIN), (Uint)7,
                    (uint16_t)65535, BURROW_ANY_VAL(TYPE_UINT32, uint32_t, 70000),
                    BURROW_ANY_VAL(TYPE_UINT64, uint64_t, UINT64_MAX));
    /* The seed is a copy, so this does not change it. */
    raw[0] = 'X';
    testing_f_add_v(f, "", slice_nil(TYPE_BYTE), (bool)false, (Byte)0,
                    BURROW_ANY_VAL(TYPE_RUNE, Rune, 0), 0.0f, 0.0, (Int)0, (int8_t)0,
                    (int16_t)0, BURROW_ANY_VAL(TYPE_INT64, int64_t, 0), (Uint)0,
                    (uint16_t)0, BURROW_ANY_VAL(TYPE_UINT32, uint32_t, 0),
                    BURROW_ANY_VAL(TYPE_UINT64, uint64_t, 0));
    sync_atomic_store_int32(&fuzz_seeds_run, 0);
    testing_f_cleanup(f, BURROW_FN(Func, seed_count_cleanup, burrow__testing_f_t(f)));
    testing_f_fuzz_v(f, BURROW_FN(TestingFuzzFunc, fuzz_types_fn, f), TYPE_STRING,
                     TYPE_BYTES, TYPE_BOOL, TYPE_BYTE, TYPE_RUNE, TYPE_FLOAT32,
                     TYPE_FLOAT64, TYPE_INT, TYPE_INT8, TYPE_INT16, TYPE_INT64,
                     TYPE_UINT, TYPE_UINT16, TYPE_UINT32, TYPE_UINT64);
    if (!str_eq(testing_f_name(f), BURROW_S("FuzzTypes")))
        testing_f_errorf_v(f, "name %q", testing_f_name(f));
}

/* The misuse Go stops with a panic, each with its message. */
static void FuzzMisuse(TestingF *f) {
    guard(call_add_uintptr, f);
    if (strcmp(caught_text, "testing: unsupported type to Add uintptr") != 0)
        testing_f_errorf_v(f, "adding a uintptr: %q", caught_text);
    guard(call_fuzz_no_types, f);
    if (strcmp(caught_text,
               "testing: fuzz target must receive at least two arguments, where "
               "the first argument is a *T") != 0)
        testing_f_errorf_v(f, "no types: %q", caught_text);
    guard(call_fuzz_int, f);
    if (strcmp(caught_text, "testing: F.Fuzz called more than once") != 0)
        testing_f_errorf_v(f, "a second call: %q", caught_text);
}

static void FuzzUnsupported(TestingF *f) {
    guard(call_fuzz_uintptr, f);
    if (strcmp(caught_text, "testing: unsupported type for fuzzing uintptr") != 0)
        testing_f_errorf_v(f, "a uintptr: %q", caught_text);
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
    X(TestBenchmarkOutput)                                                             \
    X(TestFuzzing)                                                                     \
    X(FuzzTypes)                                                                       \
    X(FuzzMisuse)                                                                      \
    X(FuzzUnsupported)                                                                 \
    X(ExampleCapture, "printf\nfmt 2\n  trailing space goes")                          \
    X(ExampleUnordered, TESTING_UNORDERED("1\n2\n3"))                                  \
    X(ExampleNeverRun)

int main(int argc, char **argv) {
    self_path = argv[0];
    if (argc >= 3 && strcmp(argv[argc - 2], "child") == 0) {
        testing_init(argc, argv);
        return run_child(argv[argc - 1]);
    }
    static const burrow__TestingEntry entries[] = {TESTS(BURROW__TESTING_ENTRY)};
    return burrow__testing_main(argc, argv, entries,
                                (Int)(sizeof entries / sizeof entries[0]), false,
                                testing_m_run);
}
