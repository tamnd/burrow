/* Tests for testing/slogtest, after Go's src/testing/slogtest/run_test.go and
 * the TestSlogtest in src/log/slog/slogtest_test.go, which runs the JSON and
 * text handlers through TestHandler.
 *
 * Copyright 2023 The Go Authors. All rights reserved.
 * Copyright 2026 The burrow Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style licence that can be found
 * in the LICENSE file. */

#include "check.h"

#include "burrow/burrow.h"
#include "burrow/encoding/json/v2.h"
#include "burrow/mem/arena.h"
#include "burrow/testing/slogtest.h"

#define S(s) BURROW_S(s)

/* ------------------------------------------------------------------ TestRun */

typedef struct RunEnv {
    Alloc *a;
    BytesBuffer buf;
} RunEnv;

static SlogHandler run_new_handler(void *env, TestingT *t) {
    (void)t;
    RunEnv *e = env;
    bytes_buffer_reset(&e->buf);
    SlogJSONHandler *h =
        slog_new_json_handler(e->a, bytes_buffer_as_io_writer(&e->buf), NULL);
    return slog_json_handler_as_handler(h);
}

static Map *run_result(void *env, TestingT *t) {
    RunEnv *e = env;
    Map *m = NULL;
    Error err = json_unmarshal(e->a, bytes_buffer_bytes(&e->buf),
                               BURROW_ANY(TYPE_JSONV2_MAP_STRING_ANY, &m));
    if (BURROW_FAILED(err))
        testing_t_fatal_v(t, err);
    return m;
}

static void TestRun(TestingT *t) {
    Arena ar;
    arena_init(&ar, NULL, 0);
    RunEnv e = {arena_allocator(&ar), BYTES_BUFFER(arena_allocator(&ar))};
    slogtest_run(t, BURROW_FN(SlogtestNewHandlerFunc, run_new_handler, &e),
                 BURROW_FN(SlogtestResultFunc, run_result, &e));
    arena_free(&ar);
}

/* ------------------------------------------------------------- TestSlogtest */

/* parseJSON. */
static Error parse_json(Alloc *a, Slice line, Map **out) {
    return json_unmarshal(a, line, BURROW_ANY(TYPE_JSONV2_MAP_STRING_ANY, out));
}

/* parseText: the output of one call to the text handler's handle, as a map.
 * Like Go's, it can parse what the cases log but not quoted keys or values,
 * and it assumes exactly one space between attributes. */
static Error parse_text(Alloc *a, Slice line, Map **out) {
    Map *top = map_make(a, TYPE_STRING, TYPE_ANY, 0);
    Slice b = bytes_trim_space(line);
    Str s = str_from_bytes(b.p, b.len);
    while (s.len > 0) {
        Str rest = BURROW_STR_EMPTY, value = BURROW_STR_EMPTY;
        bool found = false;
        Str kv = strings_cut(s, S(" "), &rest, &found);
        Str k = strings_cut(kv, S("="), &value, &found);
        if (!found)
            return fmt_errorf_v("no '=' in %q", kv);
        /* Populate a tree of maps for a dotted path such as "a.b.c=x". */
        Map *m = top;
        Str key = k, after = BURROW_STR_EMPTY;
        for (;;) {
            key = strings_cut(key, S("."), &after, &found);
            if (!found)
                break;
            Any *x = map_get(m, &key);
            Map *m2 = NULL;
            if (x == NULL) {
                m2 = map_make(a, TYPE_STRING, TYPE_ANY, 0);
                Map **box = BURROW_NEW(a, Map *);
                *box = m2;
                map_set(m, &key, &(Any){TYPE_JSONV2_MAP_STRING_ANY, box});
            } else if (x->t == TYPE_JSONV2_MAP_STRING_ANY) {
                m2 = *(Map **)x->data;
            } else {
                return fmt_errorf_v(
                    "value for %q in composite key %q is not map[string]any", key, k);
            }
            m = m2;
            key = after;
        }
        Str *v = BURROW_NEW(a, Str);
        *v = value;
        map_set(m, &key, &(Any){TYPE_STRING, v});
        s = rest;
    }
    *out = top;
    return BURROW_NO_ERROR;
}

typedef Error (*ParseFunc)(Alloc *a, Slice line, Map **out);

typedef struct LinesEnv {
    TestingT *t;
    Alloc *a;
    BytesBuffer *buf;
    ParseFunc parse;
} LinesEnv;

/* parseLines, called as the results function. */
static Slice parse_lines(void *env) {
    LinesEnv *e = env;
    Slice records = slice_nil(TYPE_JSONV2_MAP_STRING_ANY);
    Slice lines = bytes_split(e->a, bytes_buffer_bytes(e->buf), BURROW_B("\n"));
    const Slice *ls = lines.p;
    for (Int i = 0; i < lines.len; i++) {
        if (ls[i].len == 0)
            continue;
        Map *m = NULL;
        Error err = e->parse(e->a, ls[i], &m);
        if (BURROW_FAILED(err))
            testing_t_fatalf_v(e->t, "%s: %v", str_from_bytes(ls[i].p, ls[i].len), err);
        records = slice_append(e->a, records, &m, 1);
    }
    return records;
}

typedef struct SlogtestCase {
    const char *name;
    bool text;
    ParseFunc parse;
} SlogtestCase;

static void slogtest_handler(void *env, TestingT *t) {
    const SlogtestCase *c = env;
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    BytesBuffer buf = BYTES_BUFFER(a);
    IoWriter w = bytes_buffer_as_io_writer(&buf);
    SlogHandler h =
        c->text ? slog_text_handler_as_handler(slog_new_text_handler(a, w, NULL))
                : slog_json_handler_as_handler(slog_new_json_handler(a, w, NULL));
    LinesEnv e = {t, a, &buf, c->parse};
    Error err =
        slogtest_test_handler(h, BURROW_FN(SlogtestResultsFunc, parse_lines, &e));
    if (BURROW_FAILED(err))
        testing_t_fatal_v(t, err);
    arena_free(&ar);
}

static void TestSlogtest(TestingT *t) {
    static const SlogtestCase cases[] = {
        {"JSON", false, parse_json},
        {"Text", true, parse_text},
    };
    for (size_t i = 0; i < sizeof cases / sizeof cases[0]; i++)
        testing_t_run(
            t, str_from_cstr(cases[i].name),
            BURROW_FN(TestingTFunc, slogtest_handler, (void *)(uintptr_t)&cases[i]));
}

/* --------------------------------------------------------- a broken handler */

/* Not in Go: what TestHandler says about a handler that gets things wrong.
 * This text handler drops the time from every record, which five of the cases
 * check for. */
static SlogAttr drop_time(void *ctx, Alloc *a, Slice groups, SlogAttr attr) {
    (void)ctx;
    (void)a;
    if (groups.len == 0 && str_eq(attr.key, SLOG_TIME_KEY))
        return (SlogAttr){0};
    return attr;
}

static Slice no_results(void *env) {
    (void)env;
    return slice_nil(TYPE_JSONV2_MAP_STRING_ANY);
}

static void TestHandlerErrors(TestingT *t) {
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    BytesBuffer buf = BYTES_BUFFER(a);
    IoWriter w = bytes_buffer_as_io_writer(&buf);
    SlogHandlerOptions opts = {0};
    opts.replace_attr = drop_time;
    SlogHandler h = slog_text_handler_as_handler(slog_new_text_handler(a, w, &opts));

    Error err =
        slogtest_test_handler(h, BURROW_FN(SlogtestResultsFunc, no_results, NULL));
    CHECK(str_eq(error_text(err), S("got 0 results, want 17")));

    bytes_buffer_reset(&buf);
    LinesEnv e = {t, a, &buf, parse_text};
    err = slogtest_test_handler(h, BURROW_FN(SlogtestResultsFunc, parse_lines, &e));
    Str got = error_text(err);
    CHECK(strings_has_prefix(got,
                             S("missing key \"time\": this test expects slog.TimeKey, "
                               "slog.LevelKey and slog.MessageKey (")));
    CHECK_INT_EQ(strings_count(got, S("missing key \"time\"")), 5);
    CHECK_INT_EQ(strings_count(got, S("\n")), 4);
    arena_free(&ar);
}

#define TESTS(X)                                                                       \
    X(TestRun)                                                                         \
    X(TestSlogtest)                                                                    \
    X(TestHandlerErrors)

TESTING_MAIN(TESTS)
