/* Tests for log, after Go's src/log/log_test.go.
 *
 * Go's patterns name log_test.go and the two line numbers its Printf and
 * Println calls are on. Here the file is log_test.c, it may come with a
 * directory or without one depending on how the compiler was called, and the
 * line is the one AT records for the call it makes. TestCallDepth runs the _v
 * macros, which know their file and line, and the plain functions, which print
 * "???" until runtime_caller learns file names.
 *
 * Copyright 2009 The Go Authors. All rights reserved.
 * Copyright 2026 The burrow Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style licence that can be found
 * in the LICENSE file. */

#include "check.h"

#include "burrow/bytes.h"
#include "burrow/error.h"
#include "burrow/fmt.h"
#include "burrow/io.h"
#include "burrow/log.h"
#include "burrow/mem/arena.h"
#include "burrow/mem/heap.h"
#include "burrow/os.h"
#include "burrow/os/exec.h"
#include "burrow/regexp.h"
#include "burrow/strconv.h"
#include "burrow/strings.h"
#include "burrow/sync.h"
#include "burrow/time.h"

#include <stdint.h>

#define S(s) BURROW_S(s)

#define RDATE "[0-9][0-9][0-9][0-9]/[0-9][0-9]/[0-9][0-9]"
#define RTIME "[0-9][0-9]:[0-9][0-9]:[0-9][0-9]"
#define RMICROSECONDS "\\.[0-9][0-9][0-9][0-9][0-9][0-9]"
#define RLINE "@LINE@:" /* replaced with the line the call is on */
#define RLONGFILE "(.*[/\\\\])?[A-Za-z0-9_\\-]+\\.c:" RLINE
#define RSHORTFILE "[A-Za-z0-9_\\-]+\\.c:" RLINE

/* Runs call and sets line to the line it is written on. */
#define AT(line, call)                                                                 \
    do {                                                                               \
        (line) = __LINE__;                                                             \
        call;                                                                          \
    } while (0)

typedef struct Tester {
    Int flag;
    const char *prefix;
    const char *pattern; /* what the output must match; ^ and the text$ are added */
} Tester;

static const Tester tests[] = {
    /* individual pieces: */
    {0, "", ""},
    {0, "XXX", "XXX"},
    {LOG_LDATE, "", RDATE " "},
    {LOG_LTIME, "", RTIME " "},
    {LOG_LTIME | LOG_LMSGPREFIX, "XXX", RTIME " XXX"},
    {LOG_LTIME | LOG_LMICROSECONDS, "", RTIME RMICROSECONDS " "},
    {LOG_LMICROSECONDS, "", RTIME RMICROSECONDS " "}, /* microsec implies time */
    {LOG_LLONGFILE, "", RLONGFILE " "},
    {LOG_LSHORTFILE, "", RSHORTFILE " "},
    {LOG_LLONGFILE | LOG_LSHORTFILE, "", RSHORTFILE " "}, /* shortfile overrides */
    /* everything at once: */
    {LOG_LDATE | LOG_LTIME | LOG_LMICROSECONDS | LOG_LLONGFILE, "XXX",
     "XXX" RDATE " " RTIME RMICROSECONDS " " RLONGFILE " "},
    {LOG_LDATE | LOG_LTIME | LOG_LMICROSECONDS | LOG_LSHORTFILE, "XXX",
     "XXX" RDATE " " RTIME RMICROSECONDS " " RSHORTFILE " "},
    {LOG_LDATE | LOG_LTIME | LOG_LMICROSECONDS | LOG_LLONGFILE | LOG_LMSGPREFIX, "XXX",
     RDATE " " RTIME RMICROSECONDS " " RLONGFILE " XXX"},
    {LOG_LDATE | LOG_LTIME | LOG_LMICROSECONDS | LOG_LSHORTFILE | LOG_LMSGPREFIX, "XXX",
     RDATE " " RTIME RMICROSECONDS " " RSHORTFILE " XXX"},
};

static Str buf_str(BytesBuffer *b) {
    Slice s = bytes_buffer_bytes(b);
    return (Str){(const Byte *)s.p, s.len};
}

static bool matches(Alloc *a, Str pattern, Str s) {
    return regexp_match_string(regexp_must_compile(a, pattern), s);
}

/* Test using log_println_v("hello", 23, "world") or using
 * log_printf_v("hello %d world", 23). */
static void test_print(TestingT *t, Int flag, const char *prefix, const char *pattern,
                       bool use_format) {
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);

    BytesBuffer buf = BYTES_BUFFER(a);
    log_set_output(bytes_buffer_as_io_writer(&buf));
    log_set_flags(flag);
    log_set_prefix(str_from_cstr(prefix));
    int at = 0;
    if (use_format)
        AT(at, log_printf_v("hello %d world", 23));
    else
        AT(at, log_println_v("hello", 23, "world"));
    Str line = buf_str(&buf);
    line = (Str){line.p, line.len > 0 ? line.len - 1 : 0};
    Str pat = fmt_sprintf_v(a, "^%shello 23 world$", pattern);
    pat = strings_replace_all(a, pat, S("@LINE@"), strconv_itoa(a, at));
    if (!matches(a, pat, line))
        testing_t_errorf_v(t, "log output should match %q is %q", pat, line);
    log_set_output(os_file_as_io_writer(os_stderr));
    arena_free(&ar);
}

static void TestDefault(TestingT *t) {
    LogLogger *d = log_default();
    if (d == NULL || d != log_default())
        testing_t_error_v(t, "Default should be the same logger every time");
    log_set_prefix(S("std:"));
    if (!str_eq(log_logger_prefix(d), S("std:")))
        testing_t_errorf_v(t, "Default's prefix is %q, want %q", log_logger_prefix(d),
                           S("std:"));
    log_set_prefix(BURROW_STR_EMPTY);
}

static void TestAll(TestingT *t) {
    for (size_t i = 0; i < sizeof tests / sizeof tests[0]; i++) {
        test_print(t, tests[i].flag, tests[i].prefix, tests[i].pattern, false);
        test_print(t, tests[i].flag, tests[i].prefix, tests[i].pattern, true);
    }
    log_set_flags(LOG_LSTD_FLAGS);
    log_set_prefix(BURROW_STR_EMPTY);
}

static void TestOutput(TestingT *t) {
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    BytesBuffer b = BYTES_BUFFER(a);
    LogLogger *l = log_new(a, bytes_buffer_as_io_writer(&b), BURROW_STR_EMPTY, 0);
    log_logger_println_v(l, "test");
    if (!str_eq(buf_str(&b), S("test\n")))
        testing_t_errorf_v(t, "log output should match %q is %q", S("test\n"),
                           buf_str(&b));
    log_logger_free(a, l);
    arena_free(&ar);
}

static void TestNonNewLogger(TestingT *t) {
    (void)t;
    Arena ar;
    arena_init(&ar, NULL, 0);
    BytesBuffer b = BYTES_BUFFER(arena_allocator(&ar));
    LogLogger l = {0};
    log_logger_set_output(&l, bytes_buffer_as_io_writer(&b)); /* minimal work */
    log_logger_print_v(&l, "hello");
    arena_free(&ar);
}

static void output_race_one(void *env) {
    LogLogger *l = env;
    log_logger_set_flags(l, 0);
    (void)log_logger_output(l, 0, BURROW_STR_EMPTY);
}

static void TestOutputRace(TestingT *t) {
    (void)t;
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    BytesBuffer b = BYTES_BUFFER(a);
    LogLogger *l = log_new(a, bytes_buffer_as_io_writer(&b), BURROW_STR_EMPTY, 0);
    SyncWaitGroup wg = {0};
    for (int i = 0; i < 100; i++)
        sync_wait_group_go(&wg, BURROW_FN(Func, output_race_one, l));
    sync_wait_group_wait(&wg);
    log_logger_free(a, l);
    arena_free(&ar);
}

static void TestFlagAndPrefixSetting(TestingT *t) {
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    BytesBuffer b = BYTES_BUFFER(a);
    LogLogger *l =
        log_new(a, bytes_buffer_as_io_writer(&b), S("Test:"), LOG_LSTD_FLAGS);
    Int f = log_logger_flags(l);
    if (f != LOG_LSTD_FLAGS)
        testing_t_errorf_v(t, "Flags 1: expected %x got %x", (Int)LOG_LSTD_FLAGS, f);
    log_logger_set_flags(l, f | LOG_LMICROSECONDS);
    f = log_logger_flags(l);
    if (f != (LOG_LSTD_FLAGS | LOG_LMICROSECONDS))
        testing_t_errorf_v(t, "Flags 2: expected %x got %x",
                           (Int)(LOG_LSTD_FLAGS | LOG_LMICROSECONDS), f);
    Str p = log_logger_prefix(l);
    if (!str_eq(p, S("Test:")))
        testing_t_errorf_v(t, "Prefix: expected \"Test:\" got %q", p);
    log_logger_set_prefix(l, S("Reality:"));
    p = log_logger_prefix(l);
    if (!str_eq(p, S("Reality:")))
        testing_t_errorf_v(t, "Prefix: expected \"Reality:\" got %q", p);
    /* Verify a log message looks right, with our prefix and microseconds
     * present. */
    log_logger_print_v(l, "hello");
    Str pattern = S("^Reality:" RDATE " " RTIME RMICROSECONDS " hello\n");
    if (!matches(a, pattern, buf_str(&b)))
        testing_t_error_v(t, "message did not match pattern");

    /* Ensure that a newline is added only if the buffer lacks a newline
     * suffix. */
    bytes_buffer_reset(&b);
    log_logger_set_flags(l, 0);
    log_logger_set_prefix(l, S("\n"));
    (void)log_logger_output(l, 0, BURROW_STR_EMPTY);
    if (!str_eq(buf_str(&b), S("\n")))
        testing_t_errorf_v(t, "message mismatch:\ngot  %q\nwant %q", buf_str(&b),
                           S("\n"));
    log_logger_free(a, l);
    arena_free(&ar);
}

static Str utc_line(Alloc *a, Time now) {
    TimeDateRet d = time_date_of(now);
    TimeClockRet c = time_clock(now);
    return fmt_sprintf_v(a, "Test:%d/%.2d/%.2d %.2d:%.2d:%.2d hello\n", d.year,
                         (Int)d.month, d.day, c.hour, c.min, c.sec);
}

static void TestUTCFlag(TestingT *t) {
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    BytesBuffer b = BYTES_BUFFER(a);
    LogLogger *l =
        log_new(a, bytes_buffer_as_io_writer(&b), S("Test:"), LOG_LSTD_FLAGS);
    log_logger_set_flags(l, LOG_LDATE | LOG_LTIME | LOG_LUTC);
    /* Verify a log message looks right in the right time zone. Quantize to the
     * second only. */
    Time now = time_utc(time_now());
    log_logger_print_v(l, "hello");
    Str got = buf_str(&b);
    Str want = utc_line(a, now);
    if (!str_eq(got, want)) {
        /* It's possible we crossed a second boundary between getting now and
         * logging, so add a second and try again. This should very nearly
         * always work. */
        want = utc_line(a, time_add(now, TIME_SECOND));
        if (!str_eq(got, want))
            testing_t_errorf_v(t, "got %q; want %q", got, want);
    }
    log_logger_free(a, l);
    arena_free(&ar);
}

static void TestEmptyPrintCreatesLine(TestingT *t) {
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    BytesBuffer b = BYTES_BUFFER(a);
    LogLogger *l =
        log_new(a, bytes_buffer_as_io_writer(&b), S("Header:"), LOG_LSTD_FLAGS);
    log_logger_print(l, slice_nil(TYPE_ANY));
    log_logger_println_v(l, "non-empty");
    Str output = buf_str(&b);
    Int n = strings_count(output, S("Header"));
    if (n != 2)
        testing_t_errorf_v(t, "expected 2 headers, got %d", n);
    n = strings_count(output, S("\n"));
    if (n != 2)
        testing_t_errorf_v(t, "expected 2 lines, got %d", n);
    log_logger_free(a, l);
    arena_free(&ar);
}

static uint64_t heap_allocs(void) {
    uint64_t n = 0, bytes = 0;
    burrow__heap_counts(&n, &bytes);
    return n;
}

/* Go allows one allocation a call, for the slice Printf's arguments arrive
 * in. That slice is on the stack here, so the answer is none. */
static void TestDiscard(TestingT *t) {
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    LogLogger *l = log_new(a, io_discard, BURROW_STR_EMPTY, 0);
    Str s = strings_repeat(a, S("a"), 102400);
    burrow__heap_count(true);
    uint64_t before = heap_allocs();
    for (int i = 0; i < 100; i++)
        log_logger_printf_v(l, "%s", s);
    uint64_t c = heap_allocs() - before;
    burrow__heap_count(false);
    if (c != 0)
        testing_t_errorf_v(t, "got %d allocs in 100 calls, want 0", (Int)c);
    log_logger_free(a, l);
    arena_free(&ar);
}

/* Go's TestCallDepth cases, one function each. The macro puts the line it is
 * written on in an enum next to the function, which is the line the _v macros
 * report. */
#define CALL_DEPTH(n, call)                                                            \
    enum { cd_line_##n = __LINE__ };                                                   \
    static void cd_##n(void) {                                                         \
        call;                                                                          \
    }

CALL_DEPTH(fatal, log_fatal_v("Fatal"))
CALL_DEPTH(fatalf, log_fatalf_v("Fatalf"))
CALL_DEPTH(fatalln, log_fatalln_v("Fatalln"))
CALL_DEPTH(output, (void)log_output(1, S("Output")))
CALL_DEPTH(panic_, log_panic_v("Panic"))
CALL_DEPTH(panicf, log_panicf_v("Panicf"))
CALL_DEPTH(panicln, log_panicln_v("Panicln"))
CALL_DEPTH(d_fatal, log_logger_fatal_v(log_default(), "Default.Fatal"))
CALL_DEPTH(d_fatalf, log_logger_fatalf_v(log_default(), "Default.Fatalf"))
CALL_DEPTH(d_fatalln, log_logger_fatalln_v(log_default(), "Default.Fatalln"))
CALL_DEPTH(d_output, (void)log_logger_output(log_default(), 1, S("Default.Output")))
CALL_DEPTH(d_panic, log_logger_panic_v(log_default(), "Default.Panic"))
CALL_DEPTH(d_panicf, log_logger_panicf_v(log_default(), "Default.Panicf"))
CALL_DEPTH(d_panicln, log_logger_panicln_v(log_default(), "Default.Panicln"))

typedef struct CallDepthCase {
    const char *name;
    void (*log)(void);
    int line;
    bool plain; /* no file and line from the call site */
} CallDepthCase;

static const CallDepthCase call_depth_cases[] = {
    {"Fatal", cd_fatal, cd_line_fatal, false},
    {"Fatalf", cd_fatalf, cd_line_fatalf, false},
    {"Fatalln", cd_fatalln, cd_line_fatalln, false},
    {"Output", cd_output, cd_line_output, true},
    {"Panic", cd_panic_, cd_line_panic_, false},
    {"Panicf", cd_panicf, cd_line_panicf, false},
    {"Panicln", cd_panicln, cd_line_panicln, false},
    {"Default.Fatal", cd_d_fatal, cd_line_d_fatal, false},
    {"Default.Fatalf", cd_d_fatalf, cd_line_d_fatalf, false},
    {"Default.Fatalln", cd_d_fatalln, cd_line_d_fatalln, false},
    {"Default.Output", cd_d_output, cd_line_d_output, true},
    {"Default.Panic", cd_d_panic, cd_line_d_panic, false},
    {"Default.Panicf", cd_d_panicf, cd_line_d_panicf, false},
    {"Default.Panicln", cd_d_panicln, cd_line_d_panicln, false},
};

static void call_depth_one(void *env, TestingT *t) {
    Int i = (Int)(intptr_t)env;
    const CallDepthCase *tt = &call_depth_cases[i];
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);

    Error err = BURROW_NO_ERROR;
    Str exe = os_executable(a, &err);
    if (BURROW_FAILED(err)) {
        testing_t_fatalf_v(t, "Executable failed: %s", error_text(err));
        return;
    }
    /* This program again, running TestCallDepth with the case's index in the
     * environment, which makes it log that case and exit. */
    ExecCmd *cmd =
        exec_command_v(a, exe, 2, S("-test.run=^TestCallDepth$"), S("-test.count=1"));
    Slice env_ = os_environ(a);
    env_ = BURROW_APPEND(Str, a, env_, fmt_sprintf_v(a, "LOGTEST_CALL_DEPTH=%d", i));
    cmd->env = env_;

    Slice out = exec_cmd_combined_output(cmd, a, &err);
    if (errors_as(err, TYPE_EXEC_EXIT_ERROR) == NULL) {
        testing_t_fatalf_v(t, "expected exec.ExitError: %s", error_text(err));
        return;
    }
    Str got = {(const Byte *)out.p, out.len};
    Int nl = strings_index_byte(got, '\n');
    if (nl >= 0)
        got = (Str){got.p, nl};
    if (got.len > 0 && got.p[got.len - 1] == '\r')
        got.len--;

    Str want = fmt_sprintf_v(a, "log_test.c:%d: %s", tt->line, tt->name);
    /* The plain functions get their file and line from runtime_caller, which
     * does not know file names yet and leaves Go's "???" in their place. */
    Str plain = fmt_sprintf_v(a, "???:0: %s", tt->name);
    if (!str_eq(got, want) && !(tt->plain && str_eq(got, plain)))
        testing_t_errorf_v(t, "output from %s() mismatch:\n\t got: %s\n\twant: %s",
                           tt->name, got, want);
    exec_cmd_free(cmd);
    arena_free(&ar);
}

static void TestCallDepth(TestingT *t) {
    Arena ar;
    arena_init(&ar, NULL, 0);
    Str which = os_getenv(arena_allocator(&ar), S("LOGTEST_CALL_DEPTH"));
    if (which.len > 0) {
        Error err = BURROW_NO_ERROR;
        Int i = strconv_atoi(which, &err);
        Int n = (Int)(sizeof call_depth_cases / sizeof call_depth_cases[0]);
        if (BURROW_OK(err) && i >= 0 && i < n) {
            log_set_flags(LOG_LSHORTFILE);
            call_depth_cases[i].log();
        }
        os_exit(1);
    }
    arena_free(&ar);

    if (testing_short())
        testing_t_skip_v(t, "skipping test in short mode");
    SKIP_WITHOUT_EXEC(t);

    for (size_t i = 0; i < sizeof call_depth_cases / sizeof call_depth_cases[0]; i++) {
        const CallDepthCase *tt = &call_depth_cases[i];
        (void)testing_t_run(
            t, str_from_cstr(tt->name),
            BURROW_FN(TestingTFunc, call_depth_one, (void *)(intptr_t)i));
    }
}

#define TESTS(X)                                                                       \
    X(TestDefault)                                                                     \
    X(TestAll)                                                                         \
    X(TestOutput)                                                                      \
    X(TestNonNewLogger)                                                                \
    X(TestOutputRace)                                                                  \
    X(TestFlagAndPrefixSetting)                                                        \
    X(TestUTCFlag)                                                                     \
    X(TestEmptyPrintCreatesLine)                                                       \
    X(TestDiscard)                                                                     \
    X(TestCallDepth)
TESTING_MAIN(TESTS)
