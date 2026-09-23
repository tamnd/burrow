# Writing tests

burrow's `testing` is Go's test runner. A test binary built with it takes the same `-test.*` flags as one built by `go test` and prints the same lines, so anything that reads go test output, including `go tool test2json`, reads this too. Everything is in `burrow/testing.h`.

## A first test

A test is a function that takes a `TestingT *`. Its name starts with `Test` by convention, the same as in Go. You list the tests in an X macro and hand that to `TESTING_MAIN`, which writes `main`.

<!-- example: ../examples/testing/first.c -->
```c
#include "burrow/burrow.h"

static Int abs_int(Int v) {
    return v < 0 ? -v : v;
}

static void TestAbs(TestingT *t) {
    Int got = abs_int(-1);
    if (got != 1)
        testing_t_errorf_v(t, "abs_int(-1) = %d; want 1", got);
}

static void TestAbsZero(TestingT *t) {
    if (abs_int(0) != 0)
        testing_t_error_v(t, "abs_int(0) is not 0");
}

#define TESTS(X) X(TestAbs) X(TestAbsZero)
TESTING_MAIN(TESTS)

/* Output:
PASS
*/
```

Build it like any other program against burrow and run it. A run where every test passes prints `PASS` and exits 0. A run where one fails prints what went wrong and `FAIL`, and exits 1.

`testing_t_errorf_v` marks the test failed and keeps going. `testing_t_fatalf_v` marks it failed and ends it there. Each has a plain form without `f` that prints its operands the way `fmt_println` does, and `testing_t_log_v` and `testing_t_logf_v` print without failing anything. The `_v` macros take the operands directly, the same as fmt's. The functions without `_v` take a `Slice` of `Any`, for when you already have one.

## Subtests and -test.v

`testing_t_run` starts a subtest with a name and a function. That is how table driven tests are written: one subtest per row, each reported under its own name, and `-test.run` can pick one out.

<!-- example: ../examples/testing/verbose.c -->
```c
#include "burrow/burrow.h"

typedef struct Case {
    const char *name;
    const char *in;
    Int want;
} Case;

static const Case cases[] = {
    {"empty", "", 0},
    {"ascii", "hello", 5},
    {"accents", "h\xc3\xa9llo", 5},
};

static void run_case(void *env, TestingT *t) {
    const Case *c = env;
    Int got = utf8_rune_count_in_string(str_from_cstr(c->in));
    if (got != c->want)
        testing_t_errorf_v(t, "got %d runes, want %d", got, c->want);
}

static void TestRuneCount(TestingT *t) {
    for (size_t i = 0; i < sizeof cases / sizeof cases[0]; i++)
        testing_t_run(t, str_from_cstr(cases[i].name),
                      BURROW_FN(TestingTFunc, run_case, (void *)&cases[i]));
}

static void TestLog(TestingT *t) {
    testing_t_log_v(t, "this line only shows with -test.v or on failure");
}

/* A real test binary gets -test.v from the shell. This one has no command
 * line, so its TestMain hands testing_init one before the run parses it. */
static int TestMain(TestingM *m) {
    static char prog[] = "verbose";
    static char flag[] = "-test.v";
    static char *argv[] = {prog, flag, NULL};
    testing_init(2, argv);
    return testing_m_run(m);
}

#define TESTS(X) X(TestRuneCount) X(TestLog)
TESTING_MAIN_WITH(TestMain, TESTS)

/* Output:
=== RUN   TestRuneCount
=== RUN   TestRuneCount/empty
=== RUN   TestRuneCount/ascii
=== RUN   TestRuneCount/accents
--- PASS: TestRuneCount (0.00s)
    --- PASS: TestRuneCount/empty (0.00s)
    --- PASS: TestRuneCount/ascii (0.00s)
    --- PASS: TestRuneCount/accents (0.00s)
=== RUN   TestLog
    verbose.c:29: this line only shows with -test.v or on failure
--- PASS: TestLog (0.00s)
PASS
*/
```

A test binary normally gets `-test.v` from the shell: `./verbose_test -test.v`. You can leave the `test.` off, as with go test, so `-v`, `-run=RuneCount/ascii` and `-count=3` all work. `-h` prints every flag with Go's help text.

The line that logged is shown as `file:line`, like Go. The logging functions are macros that pass `__FILE__` and `__LINE__` along. `testing_t_helper` exists and does nothing, because a C stack has no lines in it. If you want a helper's caller reported instead of the helper, write the helper as a macro.

## Failures

Without `-test.v`, the output from passing tests is thrown away and only the failures are printed, each under the name of the test it belongs to, indented one level for each level of subtest.

<!-- example: ../examples/testing/failing.c -->
```c
#include <stdio.h>

#include "burrow/burrow.h"

static void open_config(void *env, TestingT *t) {
    testing_t_fatal_v(t, "no config file");
    testing_t_log_v(t, "never printed, Fatal ended the subtest");
}

static void parse_config(void *env, TestingT *t) {
    testing_t_errorf_v(t, "line %d: want a key", 3);
    testing_t_errorf_v(t, "line %d: want a value", 7);
}

static void TestConfig(TestingT *t) {
    testing_t_run(t, BURROW_S("open"), BURROW_FN(TestingTFunc, open_config, NULL));
    testing_t_run(t, BURROW_S("parse"), BURROW_FN(TestingTFunc, parse_config, NULL));
}

static void TestFine(TestingT *t) {
    testing_t_log_v(t, "a passing test's log is not printed");
}

/* A TestMain gets the M and returns the exit status. This one prints the
 * status and exits 0 so that the example itself counts as passing. */
static int TestMain(TestingM *m) {
    int code = testing_m_run(m);
    printf("exit status %d\n", code);
    return 0;
}

#define TESTS(X) X(TestConfig) X(TestFine)
TESTING_MAIN_WITH(TestMain, TESTS)

/* Output:
--- FAIL: TestConfig (0.00s)
    --- FAIL: TestConfig/open (0.00s)
        failing.c:6: no config file
    --- FAIL: TestConfig/parse (0.00s)
        failing.c:11: line 3: want a key
        failing.c:12: line 7: want a value
FAIL
exit status 1
*/
```

That example also shows `TESTING_MAIN_WITH`, which hands the M to a function of yours before running it. That is Go's TestMain: put setup and teardown around `testing_m_run` and return the status it gives you.

Fatal and SkipNow end the test by unwinding its stack, so deferred calls in `BURROW_SCOPE` blocks run on the way out. Go does the same with `runtime.Goexit`. The unwinding is a panic that the runner catches, so a `BURROW_TRY` in the test will catch it too. If your test catches panics, re-panic any value you did not expect.

## Cleanups and skips

`testing_t_cleanup` registers a function to run after the test and all of its subtests have finished. Cleanups run last in, first out. `testing_t_skip_v` ends the test and marks it skipped. A skipped test is not a failure.

<!-- example: ../examples/testing/cleanup.c -->
```c
#include <stdio.h>

#include "burrow/burrow.h"

static void say(void *env) {
    printf("cleanup: %s\n", (const char *)env);
}

static void TestTempFiles(TestingT *t) {
    testing_t_cleanup(t, BURROW_FN(Func, say, "remove the first file"));
    testing_t_cleanup(t, BURROW_FN(Func, say, "remove the second file"));
    printf("test body done\n");
}

static void TestNeedsNetwork(TestingT *t) {
    testing_t_skip_v(t, "no network in this example");
}

static int TestMain(TestingM *m) {
    int code = testing_m_run(m);
    printf("skipped tests do not fail the run: %d\n", code);
    return code;
}

#define TESTS(X) X(TestTempFiles) X(TestNeedsNetwork)
TESTING_MAIN_WITH(TestMain, TESTS)

/* Output:
test body done
cleanup: remove the second file
cleanup: remove the first file
PASS
skipped tests do not fail the run: 0
*/
```

The test's context, from `testing_t_context`, is cancelled just before the cleanups run. Pass it to anything the test starts that should stop when the test does.

## Parallel tests

Every test runs in a goroutine of its own, and the runner starts the scheduler, so a test can start goroutines and use channels without writing `runtime_main`. A subtest that calls `testing_t_parallel` pauses there until its parent's function returns. Then it runs alongside the other parallel subtests, with at most `-test.parallel` running at once. The default for `-test.parallel` is the number of processors. `testing_t_run` on the parent returns once all of them are done.

<!-- example: ../examples/testing/parallel.c -->
```c
#include "burrow/burrow.h"

static int32_t running;
static int32_t most;

static void slow(void *env, TestingT *t) {
    testing_t_parallel(t);
    int32_t now = sync_atomic_add_int32(&running, 1);
    for (;;) {
        int32_t seen = sync_atomic_load_int32(&most);
        if (now <= seen || sync_atomic_compare_and_swap_int32(&most, seen, now))
            break;
    }
    time_sleep(20 * TIME_MILLISECOND);
    sync_atomic_add_int32(&running, -1);
}

static void group(void *env, TestingT *t) {
    for (int i = 0; i < 4; i++)
        testing_t_run(t, BURROW_S("slow"), BURROW_FN(TestingTFunc, slow, NULL));
}

static void TestParallel(TestingT *t) {
    /* Run returns once every parallel subtest in the group has finished, so
     * this is the place to look at what they did. */
    testing_t_run(t, BURROW_S("group"), BURROW_FN(TestingTFunc, group, NULL));
    if (sync_atomic_load_int32(&running) != 0)
        testing_t_error_v(t, "a subtest was still running after Run returned");
}

#define TESTS(X) X(TestParallel)
TESTING_MAIN(TESTS)

/* Output:
PASS
*/
```

A test of the scheduler itself cannot run inside a goroutine, because it needs to start and stop `runtime_main` on its own. `TESTING_MAIN_BARE` runs the tests on the calling thread instead. Subtests run inline there, and `testing_t_parallel` does nothing.

## What is not there yet

Benchmarks, fuzz targets and examples can go in the tables and `-test.list` shows them, but they do not run yet. The flags for profiles, coverage and tracing are accepted and do nothing. `-test.run` uses a small regular expression matcher that reads RE2 syntax and reports errors with Go's messages. It folds case for ASCII only and does not know Unicode classes like `\pL`. `-test.shuffle` shuffles with its own generator, so a given seed does not give the same order it would in Go.
