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

/* testing.B and testing.F, declared so that the tables below can name them. */
typedef struct TestingB TestingB;
typedef struct TestingF TestingF;

/* func(*T), which is what a test and a subtest are. The env comes first as it
 * does for every function value. TESTING_MAIN writes the adapter for a plain
 * test function, so only a subtest that needs its env ever sees it. */
BURROW_FUNC(TestingTFunc, void, TestingT *t);
BURROW_FUNC(TestingBFunc, void, TestingB *b);
BURROW_FUNC(TestingFFunc, void, TestingF *f);

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
 * own, so a seed gives a different order from the one Go would give. Tables of
 * benchmarks, fuzz targets and examples are accepted and listed by -test.list,
 * and are not run yet. */
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

/* ------------------------------------------------------------------ main
 *
 * The part go test writes for you. LIST is an X macro naming each test
 * function, each of which takes a TestingT and returns nothing:
 *
 *     #define TESTS(X) X(TestParse) X(TestFormat)
 *     TESTING_MAIN(TESTS)
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
#define BURROW__TESTING_THUNK(name)                                                    \
    static void burrow__testing_thunk_##name(void *env, TestingT *t) {                 \
        (void)env;                                                                     \
        name(t);                                                                       \
    }
#define BURROW__TESTING_ENTRY(name)                                                    \
    {BURROW_S_INIT(#name), {burrow__testing_thunk_##name, NULL}},

#define BURROW__TESTING_MAIN(LIST, bare, main_fn)                                      \
    LIST(BURROW__TESTING_THUNK)                                                        \
    int main(int argc, char **argv) {                                                  \
        static const TestingInternalTest burrow__tests[] = {                           \
            LIST(BURROW__TESTING_ENTRY)};                                              \
        testing_init(argc, argv);                                                      \
        TestingM *m = testing_main_start(                                              \
            (TestingMatchString){NULL, NULL},                                          \
            slice_from((void *)burrow__tests,                                          \
                       (Int)(sizeof burrow__tests / sizeof burrow__tests[0]),          \
                       (Int)(sizeof burrow__tests / sizeof burrow__tests[0]),          \
                       TYPE_TESTING_INTERNAL_TEST),                                    \
            slice_nil(TYPE_TESTING_INTERNAL_BENCHMARK),                                \
            slice_nil(TYPE_TESTING_INTERNAL_FUZZ_TARGET),                              \
            slice_nil(TYPE_TESTING_INTERNAL_EXAMPLE));                                 \
        testing_m_set_bare(m, (bare));                                                 \
        int burrow__code = main_fn(m);                                                 \
        testing_m_free(m);                                                             \
        return burrow__code;                                                           \
    }

#define TESTING_MAIN(LIST) BURROW__TESTING_MAIN(LIST, false, testing_m_run)
#define TESTING_MAIN_BARE(LIST) BURROW__TESTING_MAIN(LIST, true, testing_m_run)
#define TESTING_MAIN_WITH(test_main, LIST) BURROW__TESTING_MAIN(LIST, false, test_main)

#ifdef __cplusplus
}
#endif

#endif /* BURROW_TESTING_H */
