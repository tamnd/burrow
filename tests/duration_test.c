/* Tests for time_parse_duration and duration_string.
 *
 * The tables are Go's, from time_test.go, in the same order.
 *
 * Copyright 2009 The Go Authors. All rights reserved.
 * Copyright 2026 The burrow Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style licence that can be found
 * in the LICENSE file. */

#include "check.h"

#include "burrow/error.h"
#include "burrow/mem/heap.h"
#include "burrow/strings.h"
#include "burrow/time.h"

#include <stdint.h>
#include <string.h>

static Str cs(const char *s) {
    return str_from_cstr(s);
}

static void sfree(Alloc *a, Str s) {
    mem_free(a, (void *)(uintptr_t)s.p, (size_t)s.len, 1);
}

#define NS TIME_NANOSECOND
#define US TIME_MICROSECOND
#define MS TIME_MILLISECOND
#define SEC TIME_SECOND
#define MIN TIME_MINUTE
#define HOUR TIME_HOUR

static const struct {
    const char *str;
    Duration d;
} duration_tests[] = {
    {"0s", 0},
    {"1ns", 1 * NS},
    {"1.1\xC2\xB5s", 1100 * NS},
    {"2.2ms", 2200 * US},
    {"3.3s", 3300 * MS},
    {"4m5s", 4 * MIN + 5 * SEC},
    {"4m5.001s", 4 * MIN + 5001 * MS},
    {"5h6m7.001s", 5 * HOUR + 6 * MIN + 7001 * MS},
    {"8m0.000000001s", 8 * MIN + 1 * NS},
    {"2562047h47m16.854775807s", INT64_MAX},
    {"-2562047h47m16.854775808s", INT64_MIN},
};

static void TestDurationString(TestingT *t) {
    Alloc *a = heap_allocator();
    for (size_t i = 0; i < sizeof duration_tests / sizeof duration_tests[0]; i++) {
        Duration d = duration_tests[i].d;
        Str want = cs(duration_tests[i].str);
        Str got = duration_string(d, a);
        if (!str_eq(got, want))
            testing_t_errorf_v(t, "Duration(%v).String() = %s, want %s", d, got, want);
        sfree(a, got);
        if (d > 0) {
            Byte buf[DURATION_STRING_MAX];
            Int n = duration_format(-d, buf);
            Str neg = str_from_bytes(buf, n);
            if (neg.len != want.len + 1 || neg.p[0] != '-' ||
                memcmp(neg.p + 1, want.p, (size_t)want.len) != 0)
                testing_t_errorf_v(t, "Duration(%v).String() = %s, want -%s", -d, neg,
                                   want);
        }
    }
}

static const struct {
    const char *in;
    Duration want;
} parse_duration_tests[] = {
    /* simple */
    {"0", 0},
    {"5s", 5 * SEC},
    {"30s", 30 * SEC},
    {"1478s", 1478 * SEC},
    /* sign */
    {"-5s", -5 * SEC},
    {"+5s", 5 * SEC},
    {"-0", 0},
    {"+0", 0},
    /* decimal */
    {"5.0s", 5 * SEC},
    {"5.6s", 5 * SEC + 600 * MS},
    {"5.s", 5 * SEC},
    {".5s", 500 * MS},
    {"1.0s", 1 * SEC},
    {"1.00s", 1 * SEC},
    {"1.004s", 1 * SEC + 4 * MS},
    {"1.0040s", 1 * SEC + 4 * MS},
    {"100.00100s", 100 * SEC + 1 * MS},
    /* different units */
    {"10ns", 10 * NS},
    {"11us", 11 * US},
    {"12\xC2\xB5s", 12 * US}, /* U+00B5 */
    {"12\xCE\xBCs", 12 * US}, /* U+03BC */
    {"13ms", 13 * MS},
    {"14s", 14 * SEC},
    {"15m", 15 * MIN},
    {"16h", 16 * HOUR},
    /* composite durations */
    {"3h30m", 3 * HOUR + 30 * MIN},
    {"10.5s4m", 4 * MIN + 10 * SEC + 500 * MS},
    {"-2m3.4s", -(2 * MIN + 3 * SEC + 400 * MS)},
    {"1h2m3s4ms5us6ns", 1 * HOUR + 2 * MIN + 3 * SEC + 4 * MS + 5 * US + 6 * NS},
    {"39h9m14.425s", 39 * HOUR + 9 * MIN + 14 * SEC + 425 * MS},
    /* large value */
    {"52763797000ns", 52763797000 * NS},
    /* more than 9 digits after decimal point, see golang.org/issue/6617 */
    {"0.3333333333333333333h", 20 * MIN},
    /* 9007199254740993 = 1<<53+1 cannot be stored precisely in a float64 */
    {"9007199254740993ns", (((Duration)1 << 53) + 1) * NS},
    /* largest duration that can be represented by int64 in nanoseconds */
    {"9223372036854775807ns", INT64_MAX},
    {"9223372036854775.807us", INT64_MAX},
    {"9223372036s854ms775us807ns", INT64_MAX},
    {"-9223372036854775808ns", INT64_MIN},
    {"-9223372036854775.808us", INT64_MIN},
    {"-9223372036s854ms775us808ns", INT64_MIN},
    /* largest negative value */
    {"-9223372036854775808ns", INT64_MIN},
    /* largest negative round trip value, see golang.org/issue/48629 */
    {"-2562047h47m16.854775808s", INT64_MIN},
    /* huge string, golang.org/issue/15011 */
    {"0.100000000000000000000h", 6 * MIN},
    /* This value tests the first overflow check in leadingFraction. */
    {"0.830103483285477580700h", 49 * MIN + 48 * SEC + 372539827 * NS},
};

static void TestParseDuration(TestingT *t) {
    for (size_t i = 0; i < sizeof parse_duration_tests / sizeof parse_duration_tests[0];
         i++) {
        Error err;
        Str in = cs(parse_duration_tests[i].in);
        Duration d = time_parse_duration(in, &err);
        if (!BURROW_OK(err) || d != parse_duration_tests[i].want)
            testing_t_errorf_v(t, "ParseDuration(%q) = %v, %v, want %v, nil", in, d,
                               err, parse_duration_tests[i].want);
    }
}

static const struct {
    const char *in;
    const char *expect;
} parse_duration_error_tests[] = {
    /* invalid */
    {"", "\"\""},
    {"3", "\"3\""},
    {"-", "\"-\""},
    {"s", "\"s\""},
    {".", "\".\""},
    {"-.", "\"-.\""},
    {".s", "\".s\""},
    {"+.s", "\"+.s\""},
    {"1d", "\"1d\""},
    {"\x85\x85", "\"\\x85\\x85\""},
    {"\xff"
     "ff",
     "\"\\xffff\""},
    {"hello \xff"
     "ff world",
     "\"hello \\xffff world\""},
    {"\xEF\xBF\xBD", "\"\\xef\\xbf\\xbd\""}, /* utf8.RuneError */
    {"\xEF\xBF\xBD hello \xEF\xBF\xBD world",
     "\"\\xef\\xbf\\xbd hello \\xef\\xbf\\xbd world\""}, /* utf8.RuneError */
    /* overflow */
    {"9223372036854775810ns", "\"9223372036854775810ns\""},
    {"9223372036854775808ns", "\"9223372036854775808ns\""},
    {"-9223372036854775809ns", "\"-9223372036854775809ns\""},
    {"9223372036854776us", "\"9223372036854776us\""},
    {"3000000h", "\"3000000h\""},
    {"9223372036854775.808us", "\"9223372036854775.808us\""},
    {"9223372036854ms775us808ns", "\"9223372036854ms775us808ns\""},
};

static void TestParseDurationErrors(TestingT *t) {
    for (size_t i = 0;
         i < sizeof parse_duration_error_tests / sizeof parse_duration_error_tests[0];
         i++) {
        Error err;
        Str in = cs(parse_duration_error_tests[i].in);
        Str expect = cs(parse_duration_error_tests[i].expect);
        (void)time_parse_duration(in, &err);
        if (BURROW_OK(err))
            testing_t_errorf_v(t, "ParseDuration(%q) = _, nil, want _, non-nil", in);
        else if (!strings_contains(error_text(err), expect))
            testing_t_errorf_v(t,
                               "ParseDuration(%q) = _, %q, error does not contain %q",
                               in, error_text(err), expect);
    }
}

/* The whole message, which the table above only checks the end of. */
static void TestParseDurationErrorText(TestingT *t) {
    static const struct {
        const char *in;
        const char *want;
    } cases[] = {
        {"1d", "time: unknown unit \"d\" in duration \"1d\""},
        {"3", "time: missing unit in duration \"3\""},
        {"", "time: invalid duration \"\""},
        {"1\xC3\xA9", "time: unknown unit \"\\xc3\\xa9\" in duration \"1\\xc3\\xa9\""},
        {"1\"s", "time: unknown unit \"\\\"s\" in duration \"1\\\"s\""},
    };
    for (size_t i = 0; i < sizeof cases / sizeof cases[0]; i++) {
        Error err;
        Str in = cs(cases[i].in);
        (void)time_parse_duration(in, &err);
        if (!str_eq(error_text(err), cs(cases[i].want)))
            testing_t_errorf_v(t, "ParseDuration(%q) error = %q, want %q", in,
                               error_text(err), cs(cases[i].want));
    }
}

/* An input long enough that its quoted form does not fit the stack buffer the
 * message is usually built in. Every byte is escaped, so it quotes to four. */
static void TestParseDurationLongErrorText(TestingT *t) {
    Byte in[200];
    memset(in, 0xFF, sizeof in);
    Error err;
    (void)time_parse_duration(str_from_bytes(in, sizeof in), &err);
    Str text = error_text(err);
    Str prefix = BURROW_S("time: invalid duration \"");
    if (text.len != prefix.len + 4 * (Int)sizeof in + 1 ||
        !strings_has_prefix(text, prefix) ||
        !strings_has_suffix(text, BURROW_S("\\xff\\xff\"")))
        testing_t_errorf_v(t, "error for 200 bytes of 0xff is %v bytes: %q", text.len,
                           text);
}

static void round_trip(TestingT *t, Alloc *a, Duration d0) {
    Error err;
    Str s = duration_string(d0, a);
    Duration d1 = time_parse_duration(s, &err);
    if (!BURROW_OK(err) || d0 != d1)
        testing_t_errorf_v(t, "round-trip failed: %v => %q => %v, %v", d0, s, d1, err);
    sfree(a, s);
}

static void TestParseDurationRoundTrip(TestingT *t) {
    Alloc *a = heap_allocator();
    round_trip(t, a, INT64_MAX);
    round_trip(t, a, INT64_MIN);

    /* Go takes 100 values from rand.Int31. A fixed generator gives the same
     * coverage and the same numbers every run. Resolutions finer than
     * milliseconds would not round-trip exactly. */
    uint32_t x = 2463534242U;
    for (int i = 0; i < 100; i++) {
        x ^= x << 13;
        x ^= x >> 17;
        x ^= x << 5;
        round_trip(t, a, (Duration)(x >> 1) * MS);
    }
}

static void TestDurationFormatAllocatesNothing(TestingT *t) {
    Byte buf[DURATION_STRING_MAX];
    Int n = duration_format(INT64_MIN, buf);
    if (n != 25)
        testing_t_errorf_v(t, "the longest duration took %v bytes, want 25", n);
}

static void BenchmarkParseDuration(TestingB *b) {
    for (Int i = 0; i < testing_b_n(b); i++) {
        (void)time_parse_duration(BURROW_S("9007199254.740993ms"), NULL);
        (void)time_parse_duration(BURROW_S("9007199254740993ns"), NULL);
    }
}

/* The errors go in the goroutine's error arena, so the loop hands them back
 * the way a long running caller would, instead of growing the arena for the
 * length of the run. */
static void BenchmarkParseDurationError(TestingB *b) {
    ArenaMark m = error_mark();
    for (Int i = 0; i < testing_b_n(b); i++) {
        (void)time_parse_duration(BURROW_S("9223372036854775810ns"),
                                  NULL); /* overflow */
        (void)time_parse_duration(BURROW_S("9007199254.740993"),
                                  NULL); /* missing unit */
        error_release(m);
    }
}

static void BenchmarkDurationString(TestingB *b) {
    Byte buf[DURATION_STRING_MAX];
    for (Int i = 0; i < testing_b_n(b); i++)
        (void)duration_format(5 * HOUR + 6 * MIN + 7001 * MS, buf);
}

#define TESTS(X)                                                                       \
    X(TestDurationString)                                                              \
    X(TestParseDuration)                                                               \
    X(TestParseDurationErrors)                                                         \
    X(TestParseDurationErrorText)                                                      \
    X(TestParseDurationLongErrorText)                                                  \
    X(TestParseDurationRoundTrip)                                                      \
    X(TestDurationFormatAllocatesNothing)                                              \
    X(BenchmarkParseDuration)                                                          \
    X(BenchmarkParseDurationError)                                                     \
    X(BenchmarkDurationString)

TESTING_MAIN(TESTS)
