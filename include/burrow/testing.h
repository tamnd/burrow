/* Copyright 2026 The burrow Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style licence that can be found
 * in the LICENSE file. */

/* testing: Go's test runner, the one `go test` drives.
 *
 * A test is a function taking a TestingT, a list of them goes to
 * TESTING_MAIN, and the binary that builds takes the same -test.* flags as
 * one built by go test and prints the same lines:
 *
 *     static void TestAbs(TestingT *t) {
 *         Int got = abs_int(-1);
 *         if (got != 1)
 *             testing_t_errorf_v(t, "abs_int(-1) = %d; want 1", got);
 *     }
 *
 *     #define TESTS(X) X(TestAbs)
 *     TESTING_MAIN(TESTS)
 *
 *     $ ./abs_test -test.v
 *     === RUN   TestAbs
 *     --- PASS: TestAbs (0.00s)
 *     PASS
 *
 * Everything Go's T does is here: subtests with testing_t_run, parallel tests,
 * cleanups, skips, the context, and failures that stop the test with
 * testing_t_fatalf. -test.run and -test.skip select tests by name the way Go
 * does, with the pattern split on slashes into one pattern per level.
 *
 * ---------------------------------------------------------------- goroutines
 *
 * Go runs every test in a goroutine of its own, and so does this. The runner
 * starts the scheduler when it is not already running, so a test can start
 * goroutines, wait on channels and call testing_t_parallel without anybody
 * having written runtime_main.
 *
 * The one kind of test that cannot live inside a goroutine is a test of the
 * scheduler itself, which has to start and stop runtime_main on its own.
 * TESTING_MAIN_BARE runs the tests on the thread that called it instead,
 * without starting anything. Subtests run in line there, and
 * testing_t_parallel returns straight away and changes nothing, which is what
 * Go does for a test that cannot run in parallel either.
 * TESTING_MAIN_BARE_WITH is the same with a TestMain, as TESTING_MAIN_WITH
 * below is for TESTING_MAIN.
 *
 * ------------------------------------------------------ stopping a test early
 *
 * testing_t_fail_now, and the fatal and skip functions that call it, end the
 * test without returning to it. Go does that with runtime.Goexit. Here it is a
 * panic that the runner catches, which runs every deferred call on the way out
 * the same as Goexit does. The one difference is that a BURROW_TRY inside the
 * test catches it too, where Go's recover would not see a Goexit. A test that
 * catches panics should let a value it did not expect carry on up with panic.
 *
 * Called from a goroutine the test started rather than from the test itself,
 * it ends that goroutine, which is what Go does too, and Go's documentation
 * says not to do it for the same reason: the test goes on running.
 *
 * ---------------------------------------------------------------- file:line
 *
 * Go prints the file and line of the call that logged. The logging functions
 * here are macros that pass __FILE__ and __LINE__ along, so they print the
 * same thing. The macros have the function's own name, and taking the address
 * of one still gives the function, which prints ???:1 as Go does when it
 * cannot find a caller.
 *
 * testing_t_helper exists and does nothing. Go finds a helper's caller by
 * walking the stack, and a C stack has no lines in it. A helper that wants its
 * caller's line reported should be a macro.
 *
 * Derived from Go's src/testing/testing.go and src/testing/match.go. */

/* burrow:package testing */

#ifndef BURROW_TESTING_H
#define BURROW_TESTING_H

#include "burrow/context.h"
#include "burrow/core.h"
#include "burrow/error.h"
#include "burrow/fmt.h"
#include "burrow/func.h"
#include "burrow/io.h"
#include "burrow/own.h"

#ifdef __cplusplus
extern "C" {
#endif

/* testing.T, which is always handled through a pointer the runner gives out. */
typedef struct TestingT TestingT;

/* testing.M, what a TestMain function receives and runs. */
typedef struct TestingM TestingM;

/* testing.B, what a benchmark gets, and testing.F, what a fuzz target gets. */
typedef struct TestingB TestingB;
typedef struct TestingF TestingF;

/* testing.PB, what testing_b_run_parallel hands each goroutine. */
typedef struct TestingPB TestingPB;

/* func(*T), which is what a test and a subtest are. The env comes first as it
 * does for every function value. TESTING_MAIN writes the adapter for a plain
 * test function, so only a subtest that needs its env ever sees it. */
BURROW_FUNC(TestingTFunc, void, TestingT *t);
BURROW_FUNC(TestingBFunc, void, TestingB *b);
BURROW_FUNC(TestingFFunc, void, TestingF *f);
BURROW_FUNC(TestingPBFunc, void, TestingPB *pb);

/* func(*T, ...), the function a fuzz target hands testing_f_fuzz. Go's takes
 * the fuzzed values as parameters of their own types, which C can only do
 * with one function type per signature, so here they come as a Slice of Any
 * in the order they were declared. testing_fuzz_arg reads one. */
BURROW_FUNC(TestingFuzzFunc, void, TestingT *t, Slice args);

/* The function Go hands MainStart for -test.run and friends, which says
 * whether a pattern matches a name. A nil one means the built in matcher,
 * described at testing_match_string. */
BURROW_FUNC(TestingMatchString, bool, Str pat, Str str, Error *err);

/* testing.InternalTest and the others, the tables go test generates. */
typedef struct TestingInternalTest {
    Str name;
    TestingTFunc f;
} TestingInternalTest;

typedef struct TestingInternalBenchmark {
    Str name;
    TestingBFunc f;
} TestingInternalBenchmark;

typedef struct TestingInternalFuzzTarget {
    Str name;
    TestingFFunc fn;
} TestingInternalFuzzTarget;

typedef struct TestingInternalExample {
    Str name;
    Func f;
    Str output;
    bool unordered;
} TestingInternalExample;

extern const Type *const TYPE_TESTING_INTERNAL_TEST;
extern const Type *const TYPE_TESTING_INTERNAL_BENCHMARK;
extern const Type *const TYPE_TESTING_INTERNAL_FUZZ_TARGET;
extern const Type *const TYPE_TESTING_INTERNAL_EXAMPLE;

/* testing.TB, the methods T, B and F share. Go keeps the set closed with an
 * unexported method, and here only this package makes the vtables. */
typedef struct TestingTBVT {
    const Type *self_type;
    void (*cleanup)(void *self, Func f);
    Context (*context)(void *self);
    void (*error)(void *self, Slice args);
    void (*errorf)(void *self, Str format, Slice args);
    void (*fail)(void *self);
    void (*fail_now)(void *self);
    bool (*failed)(void *self);
    void (*fatal)(void *self, Slice args);
    void (*fatalf)(void *self, Str format, Slice args);
    void (*helper)(void *self);
    void (*log)(void *self, Slice args);
    void (*logf)(void *self, Str format, Slice args);
    Str (*name)(void *self);
    IoWriter (*output)(void *self);
    void (*attr)(void *self, Str key, Str value);
    void (*skip)(void *self, Slice args);
    void (*skip_now)(void *self);
    void (*skipf)(void *self, Str format, Slice args);
    bool (*skipped)(void *self);
} TestingTBVT;

typedef struct TestingTB {
    const TestingTBVT *vt;
    void *data;
} TestingTB;

/* ------------------------------------------------------------ the functions
 *
 * testing.Init. Go registers the -test.* flags here and reads them from
 * os.Args later. C has no os.Args until main passes them along, so this is
 * where they come in. TESTING_MAIN calls it. Calling it again replaces the
 * command line, which only matters before testing_m_run parses it.
 *
 * The flags are Go's, with Go's usage text for -h. As on a go test command
 * line the test. prefix can be left off, so -v and -run=Foo work. The ones
 * for profiles, coverage, tracing and fuzzing are accepted and do nothing.
 * -test.shuffle takes the same values but shuffles with a generator of its
 * own, so a seed gives a different order from the one Go would give. */
void testing_init(int argc, char **argv);

/* testing.Short, testing.Verbose and testing.Testing. Short and Verbose read
 * the flags, so they panic with Go's messages when called before the run has
 * parsed them, which testing_m_run does first thing. Testing is true once
 * testing_init has been called, which in a test binary is before main does
 * anything else. */
bool testing_short(void);
bool testing_verbose(void);
bool testing_testing(void);

/* testing.CoverMode and testing.Coverage. There is no coverage
 * instrumentation, so the mode is empty and the fraction is zero, which is
 * what Go reports when -cover was not given. */
BURROW_STATIC(ret) Str testing_cover_mode(void);
double testing_coverage(void);

/* The matcher used when nobody supplies one. Go's -test.run is a regular
 * expression, and until regexp is ported this is a small one of its own that
 * reads RE2 syntax and gives Go's error messages. It understands literals and
 * escapes (\x41, \x{263a}, octal, \Q...\E), the dot, ^ and $, \A, \z, \b
 * and \B, bracketed classes with ranges, negation and the ASCII [:name:]
 * classes, \d \s \w and their capitals, groups of every kind including named
 * ones, alternation, the repetitions *, +, ? and {n,m} with their lazy forms,
 * and the flags i, m, s and U. Two things differ from Go: (?i) folds ASCII
 * letters only, and \p{...} Unicode classes are an error saying the built in
 * matcher does not support them. A pattern it cannot read is an error, so a
 * run is never silently wider than asked for. */
bool testing_match_string(Str pat, Str str, Error *err);

/* testing.MainStart, and M's one method. The tables are Slices of the
 * Internal structs above. Go leaves the M to the collector, and here
 * testing_m_free gives it back once testing_m_run has returned. TESTING_MAIN
 * does that for you. */
BURROW_OWNS(ret) TestingM *testing_main_start(TestingMatchString match, Slice tests,
                                              Slice benchmarks, Slice fuzz_targets,
                                              Slice examples);
int testing_m_run(TestingM *m);
void testing_m_free(TestingM *m);

/* Runs the tests on the calling thread rather than in goroutines. See
 * TESTING_MAIN_BARE in the comment at the top. */
void testing_m_set_bare(TestingM *m, bool bare);

/* testing.Main, which runs everything and ends the process with the status. */
BURROW_NORETURN void testing_main(TestingMatchString match, Slice tests,
                                  Slice benchmarks, Slice examples);

/* testing.RunTests, which runs a table of tests with the flags as they are
 * and reports whether they all passed. */
bool testing_run_tests(TestingMatchString match, Slice tests);

/* ---------------------------------------------------------------- T's methods
 *
 * Go's methods on *T, with the receiver first. The ones that print take an
 * operand Slice the way fmt does, and each has a _v macro that takes the
 * operands directly. */
BURROW_BORROWS(ret, t) Str testing_t_name(TestingT *t);
void testing_t_fail(TestingT *t);
bool testing_t_failed(TestingT *t);
BURROW_NORETURN void testing_t_fail_now(TestingT *t);
BURROW_NORETURN void testing_t_skip_now(TestingT *t);
bool testing_t_skipped(TestingT *t);
void testing_t_helper(TestingT *t);
void testing_t_cleanup(TestingT *t, Func f);
void testing_t_parallel(TestingT *t);
bool testing_t_run(TestingT *t, Str name, TestingTFunc f);
BURROW_BORROWS(ret, t) Context testing_t_context(TestingT *t);
bool testing_t_deadline(TestingT *t, int64_t *when);
IoWriter testing_t_output(TestingT *t);
void testing_t_attr(TestingT *t, Str key, Str value);
TestingTB testing_t_as_testing_tb(TestingT *t);

void(testing_t_log)(TestingT *t, Slice args);
void(testing_t_logf)(TestingT *t, Str format, Slice args);
void(testing_t_error)(TestingT *t, Slice args);
void(testing_t_errorf)(TestingT *t, Str format, Slice args);
BURROW_NORETURN void(testing_t_fatal)(TestingT *t, Slice args);
BURROW_NORETURN void(testing_t_fatalf)(TestingT *t, Str format, Slice args);
BURROW_NORETURN void(testing_t_skip)(TestingT *t, Slice args);
BURROW_NORETURN void(testing_t_skipf)(TestingT *t, Str format, Slice args);

/* What the macros below call, with the place the call was written. The kind
 * says what happens after the line is logged. */
typedef enum burrow__TestingLogKind {
    BURROW__TESTING_LOG,
    BURROW__TESTING_ERROR,
    BURROW__TESTING_FATAL,
    BURROW__TESTING_SKIP
} burrow__TestingLogKind;

void burrow__testing_t_logln(TestingT *t, const char *file, int line,
                             burrow__TestingLogKind kind, Slice args);
void burrow__testing_t_logf(TestingT *t, const char *file, int line,
                            burrow__TestingLogKind kind, Str format, Slice args);

#define testing_t_log(t, args)                                                         \
    burrow__testing_t_logln((t), __FILE__, __LINE__, BURROW__TESTING_LOG, (args))
#define testing_t_logf(t, format, args)                                                \
    burrow__testing_t_logf((t), __FILE__, __LINE__, BURROW__TESTING_LOG, (format),     \
                           (args))
#define testing_t_error(t, args)                                                       \
    burrow__testing_t_logln((t), __FILE__, __LINE__, BURROW__TESTING_ERROR, (args))
#define testing_t_errorf(t, format, args)                                              \
    burrow__testing_t_logf((t), __FILE__, __LINE__, BURROW__TESTING_ERROR, (format),   \
                           (args))
#define testing_t_fatal(t, args)                                                       \
    burrow__testing_t_logln((t), __FILE__, __LINE__, BURROW__TESTING_FATAL, (args))
#define testing_t_fatalf(t, format, args)                                              \
    burrow__testing_t_logf((t), __FILE__, __LINE__, BURROW__TESTING_FATAL, (format),   \
                           (args))
#define testing_t_skip(t, args)                                                        \
    burrow__testing_t_logln((t), __FILE__, __LINE__, BURROW__TESTING_SKIP, (args))
#define testing_t_skipf(t, format, args)                                               \
    burrow__testing_t_logf((t), __FILE__, __LINE__, BURROW__TESTING_SKIP, (format),    \
                           (args))

/* The operands written out, as fmt's _v macros take them:
 *
 *     testing_t_errorf_v(t, "got %d, want %d", got, want);
 *     testing_t_log_v(t, "reading", path);
 *
 * The plain forms need at least one operand, since C cannot write a variadic
 * macro that takes none, and testing_t_log(t, slice_nil(TYPE_ANY)) is the long
 * way round to an empty line. */
#define testing_t_log_v(t, ...)                                                        \
    burrow__testing_t_logln((t), __FILE__, __LINE__, BURROW__TESTING_LOG,              \
                            BURROW__FMT_ARGS(BURROW_ANY_OF, __VA_ARGS__))
#define testing_t_logf_v(t, ...)                                                       \
    burrow__testing_t_logf((t), __FILE__, __LINE__, BURROW__TESTING_LOG,               \
                           BURROW__FMT_FARGS(BURROW_ANY_OF, __VA_ARGS__))
#define testing_t_error_v(t, ...)                                                      \
    burrow__testing_t_logln((t), __FILE__, __LINE__, BURROW__TESTING_ERROR,            \
                            BURROW__FMT_ARGS(BURROW_ANY_OF, __VA_ARGS__))
#define testing_t_errorf_v(t, ...)                                                     \
    burrow__testing_t_logf((t), __FILE__, __LINE__, BURROW__TESTING_ERROR,             \
                           BURROW__FMT_FARGS(BURROW_ANY_OF, __VA_ARGS__))
#define testing_t_fatal_v(t, ...)                                                      \
    burrow__testing_t_logln((t), __FILE__, __LINE__, BURROW__TESTING_FATAL,            \
                            BURROW__FMT_ARGS(BURROW_ANY_OF, __VA_ARGS__))
#define testing_t_fatalf_v(t, ...)                                                     \
    burrow__testing_t_logf((t), __FILE__, __LINE__, BURROW__TESTING_FATAL,             \
                           BURROW__FMT_FARGS(BURROW_ANY_OF, __VA_ARGS__))
#define testing_t_skip_v(t, ...)                                                       \
    burrow__testing_t_logln((t), __FILE__, __LINE__, BURROW__TESTING_SKIP,             \
                            BURROW__FMT_ARGS(BURROW_ANY_OF, __VA_ARGS__))
#define testing_t_skipf_v(t, ...)                                                      \
    burrow__testing_t_logf((t), __FILE__, __LINE__, BURROW__TESTING_SKIP,              \
                           BURROW__FMT_FARGS(BURROW_ANY_OF, __VA_ARGS__))

/* ---------------------------------------------------------------- benchmarks
 *
 * A benchmark is a function taking a TestingB. It runs the code being
 * measured in a loop, and the runner calls it with a growing count until the
 * run takes -test.benchtime, one second unless told otherwise:
 *
 *     static void BenchmarkAbs(TestingB *b) {
 *         while (testing_b_loop(b))
 *             abs_int(-1);
 *     }
 *
 * Benchmarks go in the same list as tests and run when -test.bench matches
 * them, after the tests, one at a time. The line each prints is Go's, so
 * benchstat reads it:
 *
 *     BenchmarkAbs-10    	667417046	         1.800 ns/op
 *
 * The number after the name is the -test.cpu value, left off when it is 1.
 * The scheduler cannot change GOMAXPROCS while it runs, so every benchmark
 * runs with the largest value in the list and the name says which one this
 * pass stands for. testing_b_run_parallel starts that many goroutines, times
 * the parallelism, which is the part where the number matters.
 *
 * allocs/op and B/op count what went through heap_allocator, which is where
 * everything in burrow allocates unless handed something else. An arena counts
 * when it takes a chunk from the heap and not when it hands out a piece of
 * one, which is the point of having an arena.
 *
 * testing.BenchmarkResult. The extra metrics are sorted by unit, and the
 * result owns them: testing_benchmark_result_free gives them back. */
typedef struct TestingMetric {
    Str unit;
    double value;
} TestingMetric;

typedef struct TestingBenchmarkResult {
    Int n;               /* the number of iterations */
    int64_t t;           /* the total time taken, in nanoseconds */
    int64_t bytes;       /* bytes processed in one iteration */
    uint64_t mem_allocs; /* the total number of allocations */
    uint64_t mem_bytes;  /* the total number of bytes allocated */
    TestingMetric *extra;
    Int nextra;
} TestingBenchmarkResult;

int64_t testing_benchmark_result_ns_per_op(TestingBenchmarkResult r);
int64_t testing_benchmark_result_allocs_per_op(TestingBenchmarkResult r);
int64_t testing_benchmark_result_alloced_bytes_per_op(TestingBenchmarkResult r);
BURROW_OWNS(ret) Str testing_benchmark_result_string(Alloc *a,
                                                     TestingBenchmarkResult r);
BURROW_OWNS(ret) Str testing_benchmark_result_mem_string(Alloc *a,
                                                         TestingBenchmarkResult r);

/* r.Extra[unit], which Go reads straight out of the map. */
bool testing_benchmark_result_extra(TestingBenchmarkResult r, Str unit, double *value);
void testing_benchmark_result_free(TestingBenchmarkResult *r);

/* testing.Benchmark: runs one benchmark on its own and hands back the result,
 * for a program that wants the numbers rather than the printed line. It starts
 * the scheduler when it is not already running. -test.benchtime applies when
 * the flags have been parsed, and one second when they have not. */
TestingBenchmarkResult testing_benchmark(TestingBFunc f);

/* testing.RunBenchmarks, the table form, with the flags as they are. */
void testing_run_benchmarks(TestingMatchString match, Slice benchmarks);

/* The part of a B that testing_b_loop and testing_b_n read without a call.
 * Nothing else should touch it. */
typedef struct burrow__TestingBHead {
    uint64_t loop_n;
    uint64_t loop_i;
    Int n;
} burrow__TestingBHead;

bool burrow__testing_b_loop_slow(TestingB *b);
BURROW_BORROWS(ret, b) TestingT *burrow__testing_b_t(TestingB *b);

/* b.Loop: true while the benchmark should run another iteration. The first
 * call starts the clock afresh, so setup before the loop is not measured, and
 * the one that returns false stops it, so neither is teardown after it. The
 * common case is an increment and a compare, inline. */
static inline bool testing_b_loop(TestingB *b) {
    burrow__TestingBHead *h = (burrow__TestingBHead *)(void *)b;
    if (h->loop_i < h->loop_n) {
        h->loop_i++;
        return true;
    }
    return burrow__testing_b_loop_slow(b);
}

/* b.N, for the older form of the loop:
 *
 *     for (Int i = 0; i < testing_b_n(b); i++)
 *         abs_int(-1);
 */
static inline Int testing_b_n(TestingB *b) {
    return ((burrow__TestingBHead *)(void *)b)->n;
}

void testing_b_start_timer(TestingB *b);
void testing_b_stop_timer(TestingB *b);
void testing_b_reset_timer(TestingB *b);
void testing_b_set_bytes(TestingB *b, int64_t n);
void testing_b_report_allocs(TestingB *b);
void testing_b_report_metric(TestingB *b, double n, Str unit);
int64_t testing_b_elapsed(TestingB *b);
bool testing_b_run(TestingB *b, Str name, TestingBFunc f);
void testing_b_run_parallel(TestingB *b, TestingPBFunc body);
void testing_b_set_parallelism(TestingB *b, int p);
bool testing_pb_next(TestingPB *pb);

/* The methods B shares with T, which do what T's do. */
BURROW_BORROWS(ret, b) Str testing_b_name(TestingB *b);
void testing_b_fail(TestingB *b);
bool testing_b_failed(TestingB *b);
BURROW_NORETURN void testing_b_fail_now(TestingB *b);
BURROW_NORETURN void testing_b_skip_now(TestingB *b);
bool testing_b_skipped(TestingB *b);
void testing_b_helper(TestingB *b);
void testing_b_cleanup(TestingB *b, Func f);
BURROW_BORROWS(ret, b) Context testing_b_context(TestingB *b);
IoWriter testing_b_output(TestingB *b);
void testing_b_attr(TestingB *b, Str key, Str value);
TestingTB testing_b_as_testing_tb(TestingB *b);

#define testing_b_log(b, args)                                                         \
    burrow__testing_t_logln(burrow__testing_b_t(b), __FILE__, __LINE__,                \
                            BURROW__TESTING_LOG, (args))
#define testing_b_logf(b, format, args)                                                \
    burrow__testing_t_logf(burrow__testing_b_t(b), __FILE__, __LINE__,                 \
                           BURROW__TESTING_LOG, (format), (args))
#define testing_b_error(b, args)                                                       \
    burrow__testing_t_logln(burrow__testing_b_t(b), __FILE__, __LINE__,                \
                            BURROW__TESTING_ERROR, (args))
#define testing_b_errorf(b, format, args)                                              \
    burrow__testing_t_logf(burrow__testing_b_t(b), __FILE__, __LINE__,                 \
                           BURROW__TESTING_ERROR, (format), (args))
#define testing_b_fatal(b, args)                                                       \
    burrow__testing_t_logln(burrow__testing_b_t(b), __FILE__, __LINE__,                \
                            BURROW__TESTING_FATAL, (args))
#define testing_b_fatalf(b, format, args)                                              \
    burrow__testing_t_logf(burrow__testing_b_t(b), __FILE__, __LINE__,                 \
                           BURROW__TESTING_FATAL, (format), (args))
#define testing_b_skip(b, args)                                                        \
    burrow__testing_t_logln(burrow__testing_b_t(b), __FILE__, __LINE__,                \
                            BURROW__TESTING_SKIP, (args))
#define testing_b_skipf(b, format, args)                                               \
    burrow__testing_t_logf(burrow__testing_b_t(b), __FILE__, __LINE__,                 \
                           BURROW__TESTING_SKIP, (format), (args))

#define testing_b_log_v(b, ...)                                                        \
    burrow__testing_t_logln(burrow__testing_b_t(b), __FILE__, __LINE__,                \
                            BURROW__TESTING_LOG,                                       \
                            BURROW__FMT_ARGS(BURROW_ANY_OF, __VA_ARGS__))
#define testing_b_logf_v(b, ...)                                                       \
    burrow__testing_t_logf(burrow__testing_b_t(b), __FILE__, __LINE__,                 \
                           BURROW__TESTING_LOG,                                        \
                           BURROW__FMT_FARGS(BURROW_ANY_OF, __VA_ARGS__))
#define testing_b_error_v(b, ...)                                                      \
    burrow__testing_t_logln(burrow__testing_b_t(b), __FILE__, __LINE__,                \
                            BURROW__TESTING_ERROR,                                     \
                            BURROW__FMT_ARGS(BURROW_ANY_OF, __VA_ARGS__))
#define testing_b_errorf_v(b, ...)                                                     \
    burrow__testing_t_logf(burrow__testing_b_t(b), __FILE__, __LINE__,                 \
                           BURROW__TESTING_ERROR,                                      \
                           BURROW__FMT_FARGS(BURROW_ANY_OF, __VA_ARGS__))
#define testing_b_fatal_v(b, ...)                                                      \
    burrow__testing_t_logln(burrow__testing_b_t(b), __FILE__, __LINE__,                \
                            BURROW__TESTING_FATAL,                                     \
                            BURROW__FMT_ARGS(BURROW_ANY_OF, __VA_ARGS__))
#define testing_b_fatalf_v(b, ...)                                                     \
    burrow__testing_t_logf(burrow__testing_b_t(b), __FILE__, __LINE__,                 \
                           BURROW__TESTING_FATAL,                                      \
                           BURROW__FMT_FARGS(BURROW_ANY_OF, __VA_ARGS__))
#define testing_b_skip_v(b, ...)                                                       \
    burrow__testing_t_logln(burrow__testing_b_t(b), __FILE__, __LINE__,                \
                            BURROW__TESTING_SKIP,                                      \
                            BURROW__FMT_ARGS(BURROW_ANY_OF, __VA_ARGS__))
#define testing_b_skipf_v(b, ...)                                                      \
    burrow__testing_t_logf(burrow__testing_b_t(b), __FILE__, __LINE__,                 \
                           BURROW__TESTING_SKIP,                                       \
                           BURROW__FMT_FARGS(BURROW_ANY_OF, __VA_ARGS__))

/* ---------------------------------------------------------------- fuzz targets
 *
 * A fuzz target is a function taking a TestingF. It adds seed inputs with
 * testing_f_add and then hands testing_f_fuzz the function to run on each
 * input, along with the types that function takes:
 *
 *     static void fuzz_reverse(void *env, TestingT *t, Slice args) {
 *         (void)env;
 *         Str s = testing_fuzz_arg(args, 0, Str);
 *         Str twice = reverse(reverse(s));
 *         if (!str_eq(s, twice))
 *             testing_t_errorf_v(t, "before: %q, after: %q", s, twice);
 *     }
 *
 *     static void FuzzReverse(TestingF *f) {
 *         testing_f_add_v(f, "hello");
 *         testing_f_add_v(f, "");
 *         testing_f_fuzz_v(f, BURROW_FN(TestingFuzzFunc, fuzz_reverse, NULL),
 *                          TYPE_STRING);
 *     }
 *
 * Go takes the types from the function's signature. C has no way to ask a
 * function for its parameter types, so they are listed after it instead. The
 * ones Go allows are the ones allowed here: TYPE_BYTES, TYPE_STRING,
 * TYPE_BOOL, TYPE_BYTE, TYPE_RUNE, TYPE_FLOAT32, TYPE_FLOAT64 and every
 * signed and unsigned integer type. TYPE_BYTE and TYPE_UINT8 are the same
 * type, as they are in Go, and so are TYPE_RUNE and TYPE_INT32.
 *
 * testing_f_add_v boxes its operands the way fmt's _v macros do, so a string
 * literal is a string and an int is Go's int. Before C23 true and false are
 * ints too, so a bool seed is written (bool)true. Where one C type stands for
 * two Go types, as int64_t does for int and int64 and int32_t does for int and
 * rune, BURROW_ANY_VAL says which one is meant:
 *
 *     testing_f_add_v(f, BURROW_ANY_VAL(TYPE_INT64, int64_t, 42), (bool)true);
 *
 * A seed whose values do not match the declared types fails the target with
 * Go's message.
 *
 * Without -test.fuzz, which is the only way this runs for now, each seed is a
 * subtest named FuzzReverse/seed#0, FuzzReverse/seed#1 and so on, and -test.run
 * picks them out by those names. After those come the files in
 * testdata/fuzz/FuzzReverse under the working directory, in Go's corpus file
 * format, each a subtest named after its file. A missing directory is no seeds,
 * and a file that does not parse or does not match fails the target with Go's
 * message. A fuzz target has to call testing_f_fuzz,
 * testing_f_fail or testing_f_skip, and fails if it returns having done none
 * of them. Inside the function given to testing_f_fuzz, report through the T
 * it receives: calling most of F's methods from there panics, as in Go. */

/* f.Add. Every value is copied, so the operands can go away as soon as this
 * returns. A type fuzzing cannot use panics with Go's message. */
void testing_f_add(TestingF *f, Slice args);
#define testing_f_add_v(f, ...)                                                        \
    testing_f_add((f), BURROW__FMT_ARGS(BURROW_ANY_OF, __VA_ARGS__))

/* f.Fuzz. types is a Slice of const Type *, one per value the function takes.
 * A corpus entry that does not match is reported at the call, which is why
 * these are macros. */
void burrow__testing_f_fuzz(TestingF *f, const char *file, int line, TestingFuzzFunc ff,
                            Slice types);
#define testing_f_fuzz(f, ff, types)                                                   \
    burrow__testing_f_fuzz((f), __FILE__, __LINE__, (ff), (types))
#define testing_f_fuzz_v(f, ff, ...)                                                   \
    burrow__testing_f_fuzz(                                                            \
        (f), __FILE__, __LINE__, (ff),                                                 \
        slice_from(                                                                    \
            (void *)(const Type *[]){__VA_ARGS__},                                     \
            (Int)(sizeof((const Type *[]){__VA_ARGS__}) / sizeof(const Type *)),       \
            (Int)(sizeof((const Type *[]){__VA_ARGS__}) / sizeof(const Type *)),       \
            TYPE_UNSAFE_POINTER))

/* Value i of a fuzz function's args, as a C type: Str, Bytes, bool, Byte,
 * Rune, float, double, Int, Uint or one of the fixed width integer types.
 * Asking for a type other than the one declared panics, and so does an index
 * out of range. The value belongs to the run and should not be kept after
 * the function returns. */
#define testing_fuzz_arg(args, i, T)                                                   \
    (*(T *)burrow__testing_fuzz_arg((args), (i), TYPE_OF(T)))
BURROW_BORROWS(ret, args) void *burrow__testing_fuzz_arg(Slice args, Int i,
                                                         const Type *want);

/* The corpus file format, Go's "go test fuzz v1": a version line, then one
 * value per line written as the Go conversion that makes it, such as int(-23)
 * or []byte("hi\n"). These are what the runner uses to read testdata/fuzz,
 * and they are here rather than hidden so that the tests can reach them.
 *
 * burrow__testing_corpus_marshal is Go's marshalCorpusFile. The values have
 * to be of the types fuzzing allows, and there has to be at least one. The
 * text is allocated from a.
 *
 * burrow__testing_corpus_unmarshal is unmarshalCorpusFile. On success *vals
 * holds *n values on the heap, which burrow__testing_values_free gives back.
 * On failure it returns false and *err is Go's message, allocated from a. */
BURROW_OWNS(ret) Str burrow__testing_corpus_marshal(Alloc *a, const Any *vals, Int n);
bool burrow__testing_corpus_unmarshal(Alloc *a, Str data, Any **vals, Int *n, Str *err);
void burrow__testing_values_free(Any *vals, Int n);

/* The methods F shares with T. Go keeps the calls that would make no sense
 * inside the fuzz function from being made there, and so does this: from
 * inside it, F's fail, skip, log, cleanup and helper functions panic and say
 * to use the T instead. Name and Failed are allowed anywhere. */
BURROW_BORROWS(ret, f) Str testing_f_name(TestingF *f);
void testing_f_fail(TestingF *f);
bool testing_f_failed(TestingF *f);
BURROW_NORETURN void testing_f_fail_now(TestingF *f);
BURROW_NORETURN void testing_f_skip_now(TestingF *f);
bool testing_f_skipped(TestingF *f);
void testing_f_helper(TestingF *f);
void testing_f_cleanup(TestingF *f, Func fn);
BURROW_BORROWS(ret, f) Context testing_f_context(TestingF *f);
IoWriter testing_f_output(TestingF *f);
void testing_f_attr(TestingF *f, Str key, Str value);
TestingTB testing_f_as_testing_tb(TestingF *f);

BURROW_BORROWS(ret, f) TestingT *burrow__testing_f_t(TestingF *f);

#define testing_f_log(f, args)                                                         \
    burrow__testing_t_logln(burrow__testing_f_t(f), __FILE__, __LINE__,                \
                            BURROW__TESTING_LOG, (args))
#define testing_f_logf(f, format, args)                                                \
    burrow__testing_t_logf(burrow__testing_f_t(f), __FILE__, __LINE__,                 \
                           BURROW__TESTING_LOG, (format), (args))
#define testing_f_error(f, args)                                                       \
    burrow__testing_t_logln(burrow__testing_f_t(f), __FILE__, __LINE__,                \
                            BURROW__TESTING_ERROR, (args))
#define testing_f_errorf(f, format, args)                                              \
    burrow__testing_t_logf(burrow__testing_f_t(f), __FILE__, __LINE__,                 \
                           BURROW__TESTING_ERROR, (format), (args))
#define testing_f_fatal(f, args)                                                       \
    burrow__testing_t_logln(burrow__testing_f_t(f), __FILE__, __LINE__,                \
                            BURROW__TESTING_FATAL, (args))
#define testing_f_fatalf(f, format, args)                                              \
    burrow__testing_t_logf(burrow__testing_f_t(f), __FILE__, __LINE__,                 \
                           BURROW__TESTING_FATAL, (format), (args))
#define testing_f_skip(f, args)                                                        \
    burrow__testing_t_logln(burrow__testing_f_t(f), __FILE__, __LINE__,                \
                            BURROW__TESTING_SKIP, (args))
#define testing_f_skipf(f, format, args)                                               \
    burrow__testing_t_logf(burrow__testing_f_t(f), __FILE__, __LINE__,                 \
                           BURROW__TESTING_SKIP, (format), (args))

#define testing_f_log_v(f, ...)                                                        \
    burrow__testing_t_logln(burrow__testing_f_t(f), __FILE__, __LINE__,                \
                            BURROW__TESTING_LOG,                                       \
                            BURROW__FMT_ARGS(BURROW_ANY_OF, __VA_ARGS__))
#define testing_f_logf_v(f, ...)                                                       \
    burrow__testing_t_logf(burrow__testing_f_t(f), __FILE__, __LINE__,                 \
                           BURROW__TESTING_LOG,                                        \
                           BURROW__FMT_FARGS(BURROW_ANY_OF, __VA_ARGS__))
#define testing_f_error_v(f, ...)                                                      \
    burrow__testing_t_logln(burrow__testing_f_t(f), __FILE__, __LINE__,                \
                            BURROW__TESTING_ERROR,                                     \
                            BURROW__FMT_ARGS(BURROW_ANY_OF, __VA_ARGS__))
#define testing_f_errorf_v(f, ...)                                                     \
    burrow__testing_t_logf(burrow__testing_f_t(f), __FILE__, __LINE__,                 \
                           BURROW__TESTING_ERROR,                                      \
                           BURROW__FMT_FARGS(BURROW_ANY_OF, __VA_ARGS__))
#define testing_f_fatal_v(f, ...)                                                      \
    burrow__testing_t_logln(burrow__testing_f_t(f), __FILE__, __LINE__,                \
                            BURROW__TESTING_FATAL,                                     \
                            BURROW__FMT_ARGS(BURROW_ANY_OF, __VA_ARGS__))
#define testing_f_fatalf_v(f, ...)                                                     \
    burrow__testing_t_logf(burrow__testing_f_t(f), __FILE__, __LINE__,                 \
                           BURROW__TESTING_FATAL,                                      \
                           BURROW__FMT_FARGS(BURROW_ANY_OF, __VA_ARGS__))
#define testing_f_skip_v(f, ...)                                                       \
    burrow__testing_t_logln(burrow__testing_f_t(f), __FILE__, __LINE__,                \
                            BURROW__TESTING_SKIP,                                      \
                            BURROW__FMT_ARGS(BURROW_ANY_OF, __VA_ARGS__))
#define testing_f_skipf_v(f, ...)                                                      \
    burrow__testing_t_logf(burrow__testing_f_t(f), __FILE__, __LINE__,                 \
                           BURROW__TESTING_SKIP,                                       \
                           BURROW__FMT_FARGS(BURROW_ANY_OF, __VA_ARGS__))

/* ------------------------------------------------------------------ main
 *
 * The part go test writes for you. LIST is an X macro naming each test,
 * benchmark, fuzz target and example function. A test takes a TestingT, a
 * benchmark takes a TestingB, a fuzz target takes a TestingF and an example
 * takes nothing, and the macro tells them apart by type, the way go test
 * tells them apart by name, so a function of any other type is a compile
 * error:
 *
 *     #define TESTS(X) X(TestParse) X(TestFormat) X(BenchmarkParse) X(FuzzParse)
 *     TESTING_MAIN(TESTS)
 *
 * An example takes nothing, and its entry says what it should print. The run
 * captures what it writes to standard output, through printf, fmt or straight
 * to descriptor 1, and fails it when the two differ after trimming white space
 * from both ends. TESTING_UNORDERED is Go's "Unordered output:" comment, which
 * compares the lines in any order. An example listed with no output is
 * compiled and never run, which is what go test does with one that has no
 * output comment:
 *
 *     static void ExampleAbs(void) {
 *         fmt_println_v(abs_int(-3));
 *     }
 *     static void ExampleKeys(void) { ... prints the keys of a map ... }
 *
 *     #define TESTS(X) X(TestAbs) X(ExampleAbs, "3") \
 *                      X(ExampleKeys, TESTING_UNORDERED("a\nb\nc"))
 *
 * TESTING_MAIN_WITH also takes a TestMain, which gets the M and returns the
 * exit status, usually after doing some setup around testing_m_run.
 *
 *     static int TestMain(TestingM *m) {
 *         setup();
 *         int code = testing_m_run(m);
 *         teardown();
 *         return code;
 *     }
 *     TESTING_MAIN_WITH(TestMain, TESTS)
 */
typedef enum burrow__TestingKind {
    BURROW__TESTING_KIND_TEST = 1,
    BURROW__TESTING_KIND_BENCHMARK = 2,
    BURROW__TESTING_KIND_EXAMPLE = 3,
    BURROW__TESTING_KIND_FUZZ = 4,
} burrow__TestingKind;

typedef struct burrow__TestingEntry {
    Str name;
    int kind;
    void (*fn)(void);
    const char *output; /* NULL for an example that is not run */
    bool unordered;
} burrow__TestingEntry;

/* The second argument of an example's entry when the order of the lines does
 * not matter. */
#define TESTING_UNORDERED(output) output, true

int burrow__testing_main(int argc, char **argv, const burrow__TestingEntry *entries,
                         Int n, bool bare, int (*main_fn)(TestingM *m));

#define BURROW__TESTING_KIND(name)                                                     \
    _Generic(&(name),                                                                  \
        void (*)(TestingT *): BURROW__TESTING_KIND_TEST,                               \
        void (*)(TestingB *): BURROW__TESTING_KIND_BENCHMARK,                          \
        void (*)(TestingF *): BURROW__TESTING_KIND_FUZZ,                               \
        void (*)(void): BURROW__TESTING_KIND_EXAMPLE)
#define BURROW__TESTING_EXAMPLE_KIND(name)                                             \
    _Generic(&(name), void (*)(void): BURROW__TESTING_KIND_EXAMPLE)

/* One entry for one, two or three arguments: a name, an example's output, and
 * whether that output is unordered. */
#define BURROW__TESTING_PICK(a, b, c, d, ...) d
#define BURROW__TESTING_ENTRY(...)                                                     \
    BURROW__TESTING_PICK(__VA_ARGS__, BURROW__TESTING_ENTRY3, BURROW__TESTING_ENTRY2,  \
                         BURROW__TESTING_ENTRY1, ~)                                    \
    (__VA_ARGS__)
#define BURROW__TESTING_ENTRY1(name)                                                   \
    {BURROW_S_INIT(#name), BURROW__TESTING_KIND(name), (void (*)(void))(name), NULL,   \
     false},
#define BURROW__TESTING_ENTRY2(name, output)                                           \
    {BURROW_S_INIT(#name), BURROW__TESTING_EXAMPLE_KIND(name), (void (*)(void))(name), \
     "" output, false},
#define BURROW__TESTING_ENTRY3(name, output, unordered)                                \
    {BURROW_S_INIT(#name), BURROW__TESTING_EXAMPLE_KIND(name), (void (*)(void))(name), \
     "" output, (unordered)},

#define BURROW__TESTING_MAIN(LIST, bare, main_fn)                                      \
    int main(int argc, char **argv) {                                                  \
        static const burrow__TestingEntry burrow__entries[] = {                        \
            LIST(BURROW__TESTING_ENTRY)};                                              \
        return burrow__testing_main(                                                   \
            argc, argv, burrow__entries,                                               \
            (Int)(sizeof burrow__entries / sizeof burrow__entries[0]), (bare),         \
            main_fn);                                                                  \
    }

#define TESTING_MAIN(LIST) BURROW__TESTING_MAIN(LIST, false, testing_m_run)
#define TESTING_MAIN_BARE(LIST) BURROW__TESTING_MAIN(LIST, true, testing_m_run)
#define TESTING_MAIN_WITH(test_main, LIST) BURROW__TESTING_MAIN(LIST, false, test_main)
#define TESTING_MAIN_BARE_WITH(test_main, LIST) BURROW__TESTING_MAIN(LIST, true, test_main)

#ifdef __cplusplus
}
#endif

#endif /* BURROW_TESTING_H */
