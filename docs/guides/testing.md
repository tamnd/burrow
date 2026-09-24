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

A test of the scheduler itself cannot run inside a goroutine, because it needs to start and stop `runtime_main` on its own. `TESTING_MAIN_BARE` runs the tests on the calling thread instead. Subtests run inline there, and `testing_t_parallel` does nothing. `TESTING_MAIN_BARE_WITH` is the same thing with a TestMain.

## Benchmarks

A benchmark is a function that takes a `TestingB` and whose name starts with `Benchmark`. It goes in the same list as the tests, and `TESTING_MAIN` tells the two apart by the type of the function. Benchmarks only run when `-test.bench` is given a pattern, and they run after the tests, one at a time.

The loop is `while (testing_b_loop(b))`. The runner calls the function once, and `testing_b_loop` keeps saying yes until the loop has run for `-test.benchtime`, which is one second unless you say otherwise. Setup before the loop and cleanup after it are not timed. The older form, a `for` loop up to `testing_b_n(b)`, works too, with the runner calling the function again with a larger count each time.

<!-- example: ../examples/testing/bench.c#bench -->
```c
static void BenchmarkJoin(TestingB *b) {
    Arena ar;
    arena_init(&ar, NULL, 0);
    while (testing_b_loop(b)) {
        join(arena_allocator(&ar), 100);
        arena_reset(&ar);
    }
    arena_free(&ar);
}

static void join_size(void *env, TestingB *b) {
    Int n = *(const Int *)env;
    testing_b_report_allocs(b);
    testing_b_set_bytes(b, n * 5);
    while (testing_b_loop(b)) {
        Str s = join(heap_allocator(), n);
        mem_free(heap_allocator(), (void *)(uintptr_t)s.p, (size_t)s.len, 1);
    }
}

static void BenchmarkJoinSizes(TestingB *b) {
    static const Int sizes[] = {10, 1000};
    testing_b_run(b, BURROW_S("10"),
                  BURROW_FN(TestingBFunc, join_size, (void *)&sizes[0]));
    testing_b_run(b, BURROW_S("1000"),
                  BURROW_FN(TestingBFunc, join_size, (void *)&sizes[1]));
}
```

Each benchmark prints one line in Go's format, so benchstat and anything else that reads `go test -bench` output reads it too. Running the program above with `-test.bench=.` prints something like this:

```
goos: darwin
goarch: arm64
cpu: Apple M4
BenchmarkJoin-10         	  272800	       390.3 ns/op
BenchmarkJoinSizes/10-10 	 2650298	        46.38 ns/op	1077.97 MB/s	      50 B/op	       1 allocs/op
BenchmarkJoinSizes/1000-10         	   42828	      2910 ns/op	1718.20 MB/s	    5000 B/op	       1 allocs/op
PASS
```

The number after the name is the `-test.cpu` value. `testing_b_run` starts a sub-benchmark, and a benchmark with sub-benchmarks reports theirs and none of its own. `testing_b_set_bytes` adds the MB/s column. `testing_b_report_allocs`, or `-test.benchmem` for every benchmark, adds B/op and allocs/op, which count what went through `heap_allocator` while the timer ran. An arena only counts when it takes a new chunk from the heap, which is why `BenchmarkJoin` shows none. `testing_b_report_metric` adds a column of your own, and `testing_b_run_parallel` spreads the iterations over goroutines.

`testing_benchmark` runs one benchmark function outside a test binary and gives back the numbers, which is how a program can measure something and then decide what to do about it.

<!-- example: ../examples/testing/benchresult.c#body -->
```c
static void copy_name(void *env, TestingB *b) {
    (void)env;
    while (testing_b_loop(b)) {
        Str s = str_clone(heap_allocator(), BURROW_S("gopher"));
        mem_free(heap_allocator(), (void *)(uintptr_t)s.p, (size_t)s.len, 1);
    }
}

int main(void) {
    TestingBenchmarkResult r =
        testing_benchmark(BURROW_FN(TestingBFunc, copy_name, NULL));
    printf("ran it %s times\n", r.n > 1000 ? "plenty of" : "too few");
    printf("%lld B/op, %lld allocs/op\n",
           (long long)testing_benchmark_result_alloced_bytes_per_op(r),
           (long long)testing_benchmark_result_allocs_per_op(r));
    testing_benchmark_result_free(&r);
    return 0;
}
```

GOMAXPROCS cannot change while the scheduler runs, so all the benchmarks run with the largest `-test.cpu` value and the name says which value that pass is for. It only really matters to `testing_b_run_parallel`, which starts that many goroutines. Under `TESTING_MAIN_BARE` there are no goroutines, so benchmarks run on the calling thread and the bodies passed to `testing_b_run_parallel` take turns.

## Examples

An example is a function that takes nothing and prints something. In Go the expected output is an `// Output:` comment at the end of the function, which C cannot read back, so here it is the second argument of the example's entry in the list. The runner captures what the example writes to standard output, whether through `printf`, fmt or straight to descriptor 1, and the example fails when that differs from the expected output. White space at either end does not count.

<!-- example: ../examples/testing/examples.c#examples -->
```c
static void ExampleStrClone(void) {
    Str s = str_clone(heap_allocator(), BURROW_S("gopher"));
    fmt_println_v(s, s.len);
    mem_free(heap_allocator(), (void *)(uintptr_t)s.p, (size_t)s.len, 1);
}

static void ExampleFruit(void) {
    printf("banana\n");
    printf("apple\n");
    printf("cherry\n");
}

static void ExampleSum(void) {
    fmt_println_v(2 + 2);
}

static void ExampleDraft(void) {
    printf("not checked yet\n");
}

#define TESTS(X)                                                                       \
    X(ExampleStrClone, "gopher 6")                                                     \
    X(ExampleFruit, TESTING_UNORDERED("apple\nbanana\ncherry"))                        \
    X(ExampleSum, "5")                                                                 \
    X(ExampleDraft)
```

`TESTING_UNORDERED` is Go's `// Unordered output:`, for output whose lines can come in any order, like the keys of a map. An example listed with no output is compiled and never run, which is what go test does with an example that has no output comment. It still has to build, so it cannot go stale without anybody noticing.

Examples run after the tests and before the benchmarks, and `-test.run` and `-test.skip` pick them by name the same way they pick tests. The program above prints this, with the failing example showing what it printed and what it should have printed:

```
--- FAIL: ExampleSum (0.00s)
got:
4
want:
5
FAIL
```

An example that panics fails, and then the panic carries on and ends the run with its stack trace, the way it does in Go.

## Fuzz targets

A fuzz target takes a `TestingF`. It adds seed inputs with `testing_f_add_v` and then passes `testing_f_fuzz_v` the function to run on each input. Go reads the input types from the function's parameters, but C has no way to ask a function what it takes, so the types go after the function instead. The function receives the values as a `Slice`, and `testing_fuzz_arg` reads each one back as its C type.

<!-- example: ../examples/testing/fuzz.c#fuzz -->
```c
/* Reverses s byte by byte, which is wrong for any rune longer than a byte. */
static Str reverse(Alloc *a, Str s) {
    Byte *p = (Byte *)mem_alloc(a, (size_t)s.len + 1, 1);
    for (Int i = 0; i < s.len; i++)
        p[i] = s.p[s.len - 1 - i];
    return str_from_bytes(p, s.len);
}

static void fuzz_reverse(void *env, TestingT *t, Slice args) {
    (void)env;
    Str s = testing_fuzz_arg(args, 0, Str);
    Arena ar;
    arena_init(&ar, NULL, 0);
    Str rev = reverse(arena_allocator(&ar), s);
    if (utf8_valid_string(s) && !utf8_valid_string(rev))
        testing_t_errorf_v(t, "reverse(%q) = %q, not valid UTF-8", s, rev);
    arena_free(&ar);
}

/* Takes a []byte and an int, and skips the inputs it has no use for. */
static void fuzz_count(void *env, TestingT *t, Slice args) {
    (void)env;
    Bytes b = testing_fuzz_arg(args, 0, Bytes);
    Int max = testing_fuzz_arg(args, 1, Int);
    if (b.len > max)
        testing_t_skip_v(t, "longer than", max);
    Int n = utf8_rune_count(b);
    if (n > b.len)
        testing_t_errorf_v(t, "%d runes in %d bytes", n, b.len);
}

static void FuzzReverse(TestingF *f) {
    testing_f_add_v(f, "gopher");
    testing_f_add_v(f, "h\xc3\xa9llo");
    testing_f_fuzz_v(f, BURROW_FN(TestingFuzzFunc, fuzz_reverse, NULL), TYPE_STRING);
}

static void FuzzCount(TestingF *f) {
    Byte data[] = {'a', 0xE2, 0x98, 0xBA, 'b'};
    testing_f_add_v(f, slice_from(data, 5, 5, TYPE_BYTE), 16);
    testing_f_add_v(f, slice_from(data, 5, 5, TYPE_BYTE), 2);
    testing_f_fuzz_v(f, BURROW_FN(TestingFuzzFunc, fuzz_count, NULL), TYPE_BYTES,
                     TYPE_INT);
}

#define TESTS(X) X(FuzzReverse) X(FuzzCount)
```

Each seed runs as a subtest named after its place in the corpus, so `-test.run=FuzzReverse/seed#1` runs just that one. The program above prints this:

```
--- FAIL: FuzzReverse (0.00s)
    --- FAIL: FuzzReverse/seed#1 (0.00s)
        fuzz.c:22: reverse("héllo") = "oll\xa9\xc3h", not valid UTF-8
FAIL
```

The types allowed are Go's: `TYPE_STRING`, `TYPE_BYTES`, `TYPE_BOOL`, `TYPE_BYTE`, `TYPE_RUNE`, the two float types and every sized and unsized integer type. `testing_f_add_v` boxes values the way fmt's `_v` macros do, so a literal `16` is Go's int. Before C23, `true` is an int too, so a bool seed is written `(bool)true`. Some C types stand for two Go types, as `int64_t` does for int and int64. For those, `BURROW_ANY_VAL(TYPE_INT64, int64_t, 42)` says which type is meant. A seed that does not match the declared types fails the target with Go's message.

Seeds can also live in files. After the seeds added in code, a target runs every file in `testdata/fuzz/<target name>` under the working directory, each as a subtest named after the file. The files are Go's own format, so a corpus that `go test` wrote works unchanged:

```
go test fuzz v1
string("héllo")
int(7)
```

A missing directory is no seeds and no error. A file that does not parse, or whose values do not match the declared types, fails the target with the same message Go gives, down to the `go/parser` error for a malformed line.

A target has to call `testing_f_fuzz`, `testing_f_fail` or `testing_f_skip`, and fails if it returns without doing any of them. Inside the fuzz function, report through the `TestingT` it gets. Calling F's log, fail, skip or cleanup functions from there panics and tells you to use the T, as Go does.

### Generating inputs

`-test.fuzz` takes a pattern, and when it matches exactly one fuzz target, the program stops running seeds and starts looking for new failing inputs, the way `go test -fuzz` does. It runs the tests and seeds first as usual, then starts copies of itself as workers, one for each of `-test.parallel`, which defaults to the number of CPUs. The workers mutate the corpus with Go's mutators and random source. `-test.fuzztime` stops fuzzing after a duration, or after a count of inputs when written like `1000x`. Without it, fuzzing runs until an input fails or you press Ctrl-C. `-test.fuzzminimizetime` limits how long a failing input is shrunk for, and `-test.fuzzcachedir` says where inputs found along the way are kept between runs.

When an input fails, a worker shrinks it to the smallest input that still fails, and the program writes it to `testdata/fuzz/<target name>` and says how to run it again. With the failing seed taken out of the example above, the fuzzer finds a two byte rune within a second:

```
$ ./fuzz_test -test.fuzz=FuzzReverse -test.fuzztime=10s
warning: the test binary was not built with coverage instrumentation, so fuzzing will run without coverage guidance and may be inefficient
fuzz: elapsed: 0s, testing seed corpus: 0/1 completed
fuzz: elapsed: 0s, testing seed corpus: 1/1 completed, now fuzzing with 10 workers
fuzz: minimizing 33-byte failing input file
fuzz: elapsed: 0s, minimizing
--- FAIL: FuzzReverse (0.37s)
    --- FAIL: FuzzReverse (0.00s)
        fuzz.c:22: reverse("ɳ") = "\xb3\xc9", not valid UTF-8
    
    Failing input written to testdata/fuzz/FuzzReverse/4532eca23d537359
    To re-run:
    ./fuzz_test -test.run=FuzzReverse/4532eca23d537359
FAIL
```

That file is a seed from then on, so a plain run of the program fails on it until the bug is fixed. A panic in the fuzz function fails the input with the panic and its stack, as Go reports it. A worker that exits or crashes fails the input it was running, and that input is written out as it was, since the worker is gone and cannot shrink it.

Ctrl-C stops fuzzing the way it does in Go. The workers finish the input they are on, the program prints a last stats line and PASS, and it exits 0, since nothing failed. If a failing input was being shrunk when you pressed it, shrinking stops, the input is written out as it was found, and the run fails, as it does in Go. On Windows, Ctrl-C still ends the program at once, as burrow does not catch console events yet.

### Coverage guidance

Go builds the package under test with coverage counters, and the fuzzer keeps any input that reaches code no input reached before. Those inputs join the corpus, get mutated in turn, and go to the cache directory so the next run starts from them. That is what lets a fuzzer get past a check that blind mutation would almost never pass.

In C the compiler keeps the counters, so you ask it for them. Build the code you want guidance on with `-fsanitize-coverage=inline-8bit-counters` if you use clang, or with `-fsanitize-coverage=trace-pc` if you use gcc. burrow reads either kind. Build the test file and the code it tests that way, and leave burrow itself as it is, since coverage of the fuzzing engine only adds noise, the same way Go only instruments the package under test:

```
$ clang -std=c11 -O2 -fsanitize-coverage=inline-8bit-counters parse_test.c parse.c burrow.c -o parse_test
```

With the counters in, the run first gathers baseline coverage from every input in the corpus, seeds and cache alike, where it would otherwise only run the seeds. Then the stats line counts the inputs that found new code. Here is a target that only fails when its input starts with the four bytes `FUZZ`, tested one byte at a time. Built without counters, it ran four million inputs in ten seconds and found nothing. With them, each right byte is new code, so it gets there a byte at a time:

```
$ ./cover_test -test.fuzz=FuzzDeep -test.fuzztime=60s -test.parallel=2
fuzz: elapsed: 0s, gathering baseline coverage: 0/1 completed
fuzz: elapsed: 0s, gathering baseline coverage: 1/1 completed, now fuzzing with 2 workers
fuzz: minimizing 32-byte failing input file
fuzz: elapsed: 0s, minimizing
--- FAIL: FuzzDeep (0.16s)
    --- FAIL: FuzzDeep (0.00s)
        testing_cover_test.c:72: found it
    
    Failing input written to testdata/fuzz/FuzzDeep/10a65938dce505e4
    To re-run:
    ./cover_test -test.run=FuzzDeep/10a65938dce505e4
FAIL
```

An input that reaches new code is shrunk before it is kept, as in Go, down to the smallest input that still reaches it. A longer run prints `new interesting: N (total: M)` on each stats line, where the total counts the baseline inputs too.

Some things to know. burrow defines the functions the compiler calls into, and so does libFuzzer. To link burrow into a libFuzzer binary, build burrow with `-DBURROW_NO_FUZZ_HOOKS`, and `-test.fuzz` in that program runs without guidance. The trace-pc counters are a table of 16384 entries picked by a hash of where each call came from, so two edges can share an entry, which costs a little guidance and nothing else. Both kinds only count code in the program itself, and a shared library loaded at a different address in each worker would not line up, so link the code under test statically. MSVC's `/fsanitize-coverage` is not supported yet.

## Porting a test from Go

Most of Go's tests are tables of cases and a loop over them, and the tables are the tedious part to copy by hand. `tools/burrow-gen tests` does that part. Give it one of Go's test files and it writes a C test file with every table translated, the structs they are made of declared, and every `Test` function stubbed out with its Go left in a comment:

```
$ tools/burrow-gen tests $(go env GOROOT)/src/strconv/quote_test.go -o tests/strconv_quote_test.c
gen-tests: 6 tables, 9 loops, 11 tests to translate by hand, 0 values to fill in by hand
```

The loop over a table is written out, with the case bound to the name Go gave it, and the body is left as Go for you to port:

```
static void TestQuote(TestingT *t) {
    for (size_t i = 0; i < sizeof quotetests / sizeof quotetests[0]; i++) {
        const QuoteTest *tt = &quotetests[i];
        (void)tt;
        /* Go, with tt as a pointer now:
         *     if out := Quote(tt.in); out != tt.out {
         *         t.Errorf("Quote(%s) = %s, want %s", tt.in, out, tt.out)
         *     }
         ...
         */
    }
    testing_t_skip_v(t, "burrow-gen tests: the loop body is still Go");
}
```

The file compiles and runs as it comes out, with every test skipping, so you can port one test at a time and delete its skip when it is done. Package names are resolved to burrow's C names through the same tables `tools/burrow-coverage` uses, so `ErrSyntax` becomes `&strconv_err_syntax` and `Quote` becomes `strconv_quote`. A value the tool cannot translate, like a call to a function or an interface holding something, compiles as a zero with a `/* by hand: ... */` comment next to it, and the count on the last line of the output says how many there are. It needs Go, since it reads the file with Go's own parser and type checker, and it runs `clang-format` over what it writes when there is one on the path.

`tools/gen-tests/testdata/strconv/fixture_test.go` has one of every shape the tool knows, and what the tool writes for it is checked in beside it. `make check` regenerates it to make sure it has not drifted, and `make test` builds and runs it with the rest.

## What is not there yet

The flags for profiles, coverage and tracing are accepted and do nothing. `-test.run` uses a small regular expression matcher that reads RE2 syntax and reports errors with Go's messages. It folds case for ASCII only and does not know Unicode classes like `\pL`. `-test.shuffle` shuffles with its own generator, so a given seed does not give the same order it would in Go.
