/* Tests for log/slog's values, levels, records and loggers, after Go's
 * src/log/slog/value_test.go, level_test.go, record_test.go and
 * logger_test.go.
 *
 * Go finds the source of a log call from its pc. Here the _v macros say where
 * they were called from, so TestCallDepth goes through them, and the function
 * in the source is the C __func__, which has no package in front of it. The
 * tests that count allocations are left out, as there is no AllocsPerRun, and
 * TestCallDepthConnection is too, because it runs the test binary again to
 * watch log.Fatal and log.Panic exit, which the testing package here has no
 * way to do.
 *
 * Copyright 2022 The Go Authors. All rights reserved.
 * Copyright 2026 The burrow Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style licence that can be found
 * in the LICENSE file. */

#include "check.h"

#include "../src/log/slog_internal.h"

#include "burrow/burrow.h"
#include "burrow/declare.h"
#include "burrow/mem/arena.h"
#include "burrow/path/filepath.h"

#include <stdint.h>
#include <string.h>

#define S(s) BURROW_S(s)

static Slice attrs_of(SlogAttr *p, Int n) {
    return slice_from(p, n, n, TYPE_SLOG_ATTR);
}

/* The arguments as a Slice of SlogAttr, which lasts until the end of the
 * enclosing block. */
#define ATTRS(...) attrs_of((SlogAttr[]){__VA_ARGS__}, BURROW__FMT_NARGS(__VA_ARGS__))
#define NO_ATTRS slice_nil(TYPE_SLOG_ATTR)
#define NO_ARGS slice_nil(TYPE_ANY)
#define GROUP(k, ...) slog_group_attrs(S(k), ATTRS(__VA_ARGS__))
#define EMPTY_GROUP(k) slog_group_attrs(S(k), NO_ATTRS)
#define INT(k, v) slog_int(S(k), (v))
#define STRING(k, v) slog_string(S(k), S(v))

BURROW_SLICE_TYPE(AttrSlice, SlogAttr);
BURROW_PTR_TYPE(IntPtr, Int);

#define TEXT_TIME_RE                                                                   \
    "\\d{4}-\\d{2}-\\d{2}T\\d{2}:\\d{2}:\\d{2}\\.\\d{3}(Z|[+-]\\d{2}:\\d{2})"
#define JSON_TIME_RE                                                                   \
    "\\d{4}-\\d{2}-\\d{2}T\\d{2}:\\d{2}:\\d{2}(\\.\\d+)?(Z|[+-]\\d{2}:\\d{2})"

static Time test_time(void) {
    return time_date(2000, TIME_JANUARY, 2, 3, 4, 5, 0, time_utc_loc);
}

static Str buf_str(BytesBuffer *b) {
    Slice s = bytes_buffer_bytes(b);
    return str_from_bytes(s.p, s.len);
}

static Slice bytes_of(Str s) {
    return slice_from((void *)(uintptr_t)s.p, s.len, s.len, TYPE_BYTE);
}

static Str str_of(Slice b) {
    return str_from_bytes(b.p, b.len);
}

/* checkLogOutput, with clean: got without its last newline and with the
 * others as ~, matched against the whole of want. */
static void check_log_output(TestingT *t, Alloc *a, Str got, Str want) {
    if (got.len > 0 && got.p[got.len - 1] == '\n')
        got.len--;
    got = strings_replace_all(a, got, S("\n"), S("~"));
    Str re = fmt_sprintf_v(a, "^%s$", want);
    Regexp *x = regexp_must_compile(a, re);
    bool ok = regexp_match_string(x, got);
    regexp_free(x);
    if (!ok)
        testing_t_errorf_v(t, "\ngot  %s\nwant %s", got, re);
}

/* Attrs as Go's %v prints a []Attr. */
static Str attrs_string(Alloc *a, Slice as) {
    const SlogAttr *p = as.p;
    Slice b = slice_nil(TYPE_BYTE);
    b = slice_append(a, b, "[", 1);
    for (Int i = 0; i < as.len; i++) {
        if (i > 0)
            b = slice_append(a, b, " ", 1);
        Str s = slog_attr_string(p[i], a);
        b = slice_append(a, b, s.p, s.len);
    }
    b = slice_append(a, b, "]", 1);
    return str_of(b);
}

/* attrsEqual. */
static bool attrs_equal(Slice x, Slice y) {
    if (x.len != y.len)
        return false;
    const SlogAttr *p = x.p;
    const SlogAttr *q = y.p;
    for (Int i = 0; i < x.len; i++)
        if (!slog_attr_equal(p[i], q[i]))
            return false;
    return true;
}

/* attrsSlice. */
typedef struct AttrCollector {
    Alloc *a;
    Slice got;
    Int stop; /* 0 for no limit */
} AttrCollector;

static bool collect_attr(void *ctx, SlogAttr attr) {
    AttrCollector *c = ctx;
    c->got = slice_append(c->a, c->got, &attr, 1);
    return c->stop == 0 || c->got.len < c->stop;
}

static Slice attrs_slice(Alloc *a, const SlogRecord *r) {
    AttrCollector c = {a, slice_make(a, TYPE_SLOG_ATTR, 0, slog_record_num_attrs(r)),
                       0};
    slog_record_attrs(r, collect_attr, &c);
    return c.got;
}

/* x followed by more, in a new slice with no room to spare. */
static Slice attrs_concat(Alloc *a, Slice x, Slice more) {
    Slice s = slice_make(a, TYPE_SLOG_ATTR, 0, x.len + more.len);
    s = slice_append(a, s, x.p, x.len);
    return slice_append(a, s, more.p, more.len);
}

static void check_attrs(TestingT *t, Alloc *a, Slice got, Slice want) {
    if (!attrs_equal(got, want))
        testing_t_errorf_v(t, "got %s, want %s", attrs_string(a, got),
                           attrs_string(a, want));
}

/* ------------------------------------------------------- captureHandler */

typedef struct CaptureHandler {
    Alloc *a;
    SyncMutex mu;
    Context ctx;
    SlogRecord r;
    Slice attrs;
    Slice groups;
} CaptureHandler;

static const SlogHandlerVT capture_vt;

static CaptureHandler *new_capture(Alloc *a) {
    CaptureHandler *h = BURROW_NEW(a, CaptureHandler);
    memset(h, 0, sizeof *h);
    h->a = a;
    h->attrs = NO_ATTRS;
    h->groups = slice_nil(TYPE_STRING);
    return h;
}

static SlogHandler capture_handler(CaptureHandler *h) {
    return (SlogHandler){&capture_vt, h};
}

static bool capture_enabled(void *self, Context ctx, SlogLevel level) {
    (void)self;
    (void)ctx;
    (void)level;
    return true;
}

/* The record's attributes past the inline ones belong to the logger's call,
 * so they are copied to keep them. */
static Error capture_handle(void *self, Context ctx, SlogRecord r) {
    CaptureHandler *h = self;
    sync_mutex_lock(&h->mu);
    h->ctx = ctx;
    r.back = slice_append(h->a, NO_ATTRS, r.back.p, r.back.len);
    h->r = r;
    sync_mutex_unlock(&h->mu);
    return BURROW_NO_ERROR;
}

static SlogHandler capture_with_attrs(void *self, Slice as) {
    CaptureHandler *c = self;
    sync_mutex_lock(&c->mu);
    CaptureHandler *c2 = new_capture(c->a);
    c2->r = c->r;
    c2->groups = c->groups;
    c2->attrs = attrs_concat(c->a, c->attrs, as);
    sync_mutex_unlock(&c->mu);
    return capture_handler(c2);
}

static SlogHandler capture_with_group(void *self, Str name) {
    CaptureHandler *c = self;
    sync_mutex_lock(&c->mu);
    CaptureHandler *c2 = new_capture(c->a);
    c2->r = c->r;
    c2->attrs = c->attrs;
    Slice gs = slice_make(c->a, TYPE_STRING, 0, c->groups.len + 1);
    gs = slice_append(c->a, gs, c->groups.p, c->groups.len);
    c2->groups = slice_append(c->a, gs, &name, 1);
    sync_mutex_unlock(&c->mu);
    return capture_handler(c2);
}

static void capture_clear(CaptureHandler *c) {
    sync_mutex_lock(&c->mu);
    c->ctx = (Context){0};
    memset(&c->r, 0, sizeof c->r);
    sync_mutex_unlock(&c->mu);
}

/* The handlers come from an arena, so there is nothing to free one by one. */
static void capture_free(void *self) {
    (void)self;
}

static const SlogHandlerVT capture_vt = {
    NULL,
    capture_enabled,
    capture_handle,
    capture_with_attrs,
    capture_with_group,
    capture_free,
};

/* What a test that changes the default logger or log's output puts back. */
typedef struct Saved {
    SlogLogger *logger;
    IoWriter writer;
    Int flags;
    SlogLevel level;
} Saved;

static Saved save_defaults(void) {
    Saved s;
    s.logger = slog_default();
    s.writer = log_writer();
    s.flags = log_flags();
    s.level = slog_level_var_level(&burrow__slog_log_logger_level);
    return s;
}

static void restore_defaults(Saved s) {
    slog_set_default(s.logger);
    log_set_output(s.writer);
    log_set_flags(s.flags);
    slog_set_log_logger_level(s.level);
}

/* ----------------------------------------------------------- value tests */

static void TestKindString(TestingT *t) {
    Str got = slog_kind_string(SLOG_KIND_GROUP);
    if (!str_eq(got, S("Group")))
        testing_t_errorf_v(t, "got %q, want %q", got, "Group");
}

static void TestValueEqual(TestingT *t) {
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    Int x = 0;
    Int y = 0;
    Int *px = &x;
    Int *py = &y;
    SlogValue vals[] = {
        {0},
        slog_int64_value(1),
        slog_int64_value(2),
        slog_float64_value(3.5),
        slog_float64_value(3.7),
        slog_bool_value(true),
        slog_bool_value(false),
        slog_time_value(test_time()),
        slog_time_value((Time){0}),
        slog_time_value(time_date(2001, TIME_JANUARY, 2, 3, 4, 5, 0, time_utc_loc)),
        /* overflows nanoseconds */
        slog_time_value(time_date(2300, TIME_JANUARY, 1, 0, 0, 0, 0, time_utc_loc)),
        /* overflowed value */
        slog_time_value(
            time_date(1715, TIME_JUNE, 13, 0, 25, 26, 290448384, time_utc_loc)),
        slog_any_value(BURROW_ANY(TYPE_OF(IntPtr), &px)),
        slog_any_value(BURROW_ANY(TYPE_OF(IntPtr), &py)),
        slog_group_value(ATTRS(slog_bool(S("b"), true), INT("i", 3))),
        slog_group_value(ATTRS(slog_bool(S("b"), true), INT("i", 4))),
        slog_group_value(ATTRS(slog_bool(S("b"), true), INT("j", 4))),
        slog_duration_value(3 * TIME_SECOND),
        slog_duration_value(2 * TIME_SECOND),
        slog_string_value(S("foo")),
        slog_string_value(S("fuu")),
    };
    size_t n = sizeof vals / sizeof vals[0];
    for (size_t i = 0; i < n; i++) {
        for (size_t j = 0; j < n; j++) {
            bool got = slog_value_equal(vals[i], vals[j]);
            bool want = i == j;
            if (got != want)
                testing_t_errorf_v(t, "%s.Equal(%s): got %t, want %t",
                                   slog_value_string(vals[i], a),
                                   slog_value_string(vals[j], a), got, want);
        }
    }
    arena_free(&ar);
}

static void TestValueString(TestingT *t) {
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    struct {
        SlogValue v;
        const char *want;
    } tests[] = {
        {slog_int64_value(-3), "-3"},
        {slog_uint64_value(1), "1"},
        {slog_float64_value(.15), "0.15"},
        {slog_bool_value(true), "true"},
        {slog_string_value(S("foo")), "foo"},
        {slog_time_value(test_time()), "2000-01-02 03:04:05 +0000 UTC"},
        {slog_any_value(BURROW_ANY_VAL(TYPE_DURATION, Duration, 3 * TIME_SECOND)),
         "3s"},
        {slog_group_value(ATTRS(INT("a", 1), slog_bool(S("b"), true))), "[a=1 b=true]"},
    };
    for (size_t i = 0; i < sizeof tests / sizeof tests[0]; i++) {
        Str got = slog_value_string(tests[i].v, a);
        if (!str_eq(got, str_from_cstr(tests[i].want)))
            testing_t_errorf_v(t, "%d:\ngot  %q\nwant %q", (Int)i, got, tests[i].want);
    }
    arena_free(&ar);
}

static void TestAnyValue(TestingT *t) {
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    Time tt = test_time();
    struct {
        Any in;
        SlogValue want;
    } tests[] = {
        {BURROW_ANY_VAL(TYPE_INT, Int, 1), slog_int_value(1)},
        {BURROW_ANY_VAL(TYPE_FLOAT64, double, 1.5), slog_float64_value(1.5)},
        {BURROW_ANY_VAL(TYPE_FLOAT32, float, 2.5f), slog_float64_value(2.5)},
        {BURROW_ANY_VAL(TYPE_STRING, Str, S("s")), slog_string_value(S("s"))},
        {BURROW_ANY_VAL(TYPE_BOOL, bool, true), slog_bool_value(true)},
        {BURROW_ANY(TYPE_TIME, &tt), slog_time_value(tt)},
        {BURROW_ANY_VAL(TYPE_DURATION, Duration, TIME_HOUR),
         slog_duration_value(TIME_HOUR)},
        {BURROW_ANY_VAL(TYPE_OF(AttrSlice), Slice, ATTRS(INT("i", 3))),
         slog_group_value(ATTRS(INT("i", 3)))},
        {BURROW_ANY_VAL(TYPE_SLOG_VALUE, SlogValue, slog_int_value(4)),
         slog_int_value(4)},
        {BURROW_ANY_VAL(TYPE_UINT, Uint, 2), slog_uint64_value(2)},
        {BURROW_ANY_VAL(TYPE_UINT8, uint8_t, 3), slog_uint64_value(3)},
        {BURROW_ANY_VAL(TYPE_UINT16, uint16_t, 4), slog_uint64_value(4)},
        {BURROW_ANY_VAL(TYPE_UINT32, uint32_t, 5), slog_uint64_value(5)},
        {BURROW_ANY_VAL(TYPE_UINT64, uint64_t, 6), slog_uint64_value(6)},
        {BURROW_ANY_VAL(TYPE_UINTPTR, Uintptr, 7), slog_uint64_value(7)},
        {BURROW_ANY_VAL(TYPE_INT8, int8_t, 8), slog_int64_value(8)},
        {BURROW_ANY_VAL(TYPE_INT16, int16_t, 9), slog_int64_value(9)},
        {BURROW_ANY_VAL(TYPE_INT32, int32_t, 10), slog_int64_value(10)},
        {BURROW_ANY_VAL(TYPE_INT64, int64_t, 11), slog_int64_value(11)},
    };
    for (size_t i = 0; i < sizeof tests / sizeof tests[0]; i++) {
        SlogValue got = slog_any_value(tests[i].in);
        if (!slog_value_equal(got, tests[i].want))
            testing_t_errorf_v(t, "%v (%T): got %s (kind %s), want %s (kind %s)",
                               tests[i].in, tests[i].in, slog_value_string(got, a),
                               slog_kind_string(slog_value_kind(got)),
                               slog_value_string(tests[i].want, a),
                               slog_kind_string(slog_value_kind(tests[i].want)));
    }
    arena_free(&ar);
}

/* reflect.DeepEqual for what TestValueAny puts in. A []Attr comes back as a
 * slice of the package's own type, so slices of attributes are compared as
 * attributes. */
static bool any_deep_equal(Any x, Any y) {
    if (x.t == NULL || y.t == NULL)
        return x.t == y.t;
    if (x.t->kind == KIND_SLICE && y.t->kind == KIND_SLICE &&
        x.t->elem == TYPE_SLOG_ATTR && y.t->elem == TYPE_SLOG_ATTR)
        return attrs_equal(*(const Slice *)x.data, *(const Slice *)y.data);
    return x.t == y.t && any_equal(x, y);
}

static void TestValueAny(TestingT *t) {
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    Time zero = {0};
    Int x = 0;
    Int *px = &x;
    Any wants[] = {
        {NULL, NULL},
        BURROW_ANY_VAL(TYPE_SLOG_LEVEL, SlogLevel, SLOG_LEVEL_DEBUG + 100),
        BURROW_ANY(TYPE_OF(IntPtr), &px), /* Go has time.UTC, a pointer */
        BURROW_ANY_VAL(TYPE_SLOG_KIND, SlogKind, SLOG_KIND_BOOL),
        BURROW_ANY_VAL(TYPE_OF(AttrSlice), Slice, ATTRS(INT("a", 1))),
        BURROW_ANY_VAL(TYPE_INT64, int64_t, 2),
        BURROW_ANY_VAL(TYPE_UINT64, uint64_t, 3),
        BURROW_ANY_VAL(TYPE_BOOL, bool, true),
        BURROW_ANY_VAL(TYPE_DURATION, Duration, TIME_MINUTE),
        BURROW_ANY(TYPE_TIME, &zero),
        BURROW_ANY_VAL(TYPE_FLOAT64, double, 3.14),
        BURROW_ANY_VAL(TYPE_STRING, Str, S("foo")),
    };
    for (size_t i = 0; i < sizeof wants / sizeof wants[0]; i++) {
        SlogValue v = slog_any_value(wants[i]);
        Any got = slog_value_any(&v);
        if (!any_deep_equal(got, wants[i]))
            testing_t_errorf_v(t, "%d: got %v, want %v", (Int)i, got, wants[i]);
    }
    (void)a;
    arena_free(&ar);
}

/* replace, whose LogValue hands back the value it holds. */
typedef struct Replace {
    SlogValue v;
} Replace;

static SlogValue replace_log_value(void *self) {
    return ((Replace *)self)->v;
}

static const SlogLogValuerVT replace_vt = {NULL, replace_log_value};

/* panickingLogValue. */
static SlogValue panicking_log_value(void *self) {
    (void)self;
    panic_str(S("bad"));
}

static const SlogLogValuerVT panicking_vt = {NULL, panicking_log_value};

#define VALUER_ANY(lv) BURROW_ANY(TYPE_SLOG_LOG_VALUER, (lv))

static void TestLogValue(TestingT *t) {
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    Str want = S("replaced");
    Replace r = {slog_string_value(want)};
    SlogLogValuer rlv = {&replace_vt, &r};
    SlogValue v = slog_any_value(VALUER_ANY(&rlv));
    if (slog_value_kind(v) != SLOG_KIND_LOG_VALUER)
        testing_t_errorf_v(t, "got %s, want %s", slog_kind_string(slog_value_kind(v)),
                           slog_kind_string(SLOG_KIND_LOG_VALUER));
    Any lva = slog_value_log_valuer(v);
    const SlogLogValuer *lv = lva.data;
    SlogValue lvv = lv->vt->log_value(lv->data);
    Any got = slog_value_any(&lvv);
    if (got.t != TYPE_STRING || !str_eq(*(const Str *)got.data, want))
        testing_t_errorf_v(t, "got %#v, want %#v", got, want);

    /* Test Resolve. */
    SlogValue res = slog_value_resolve(v);
    got = slog_value_any(&res);
    if (got.t != TYPE_STRING || !str_eq(*(const Str *)got.data, want))
        testing_t_errorf_v(t, "got %#v, want %#v", got, want);

    /* Test Resolve max iteration. */
    r.v = slog_any_value(VALUER_ANY(&rlv)); /* create a cycle */
    res = slog_value_resolve(slog_any_value(VALUER_ANY(&rlv)));
    got = slog_value_any(&res);
    if (got.t != TYPE_ERROR)
        testing_t_errorf_v(t, "expected error, got %T", got);

    /* Groups are not recursively resolved. */
    Replace rd = {slog_string_value(S("d"))};
    SlogLogValuer rdlv = {&replace_vt, &rd};
    SlogAttr c = slog_any(S("c"), VALUER_ANY(&rdlv));
    Replace rg = {slog_group_value(ATTRS(INT("a", 1), GROUP("b", c)))};
    SlogLogValuer rglv = {&replace_vt, &rg};
    v = slog_any_value(VALUER_ANY(&rglv));
    res = slog_value_resolve(v);
    Slice got2 = slog_value_group(res);
    Slice want2 = ATTRS(INT("a", 1), GROUP("b", c));
    check_attrs(t, a, got2, want2);

    /* Verify that panics in Resolve are caught and turn into errors. */
    SlogLogValuer plv = {&panicking_vt, NULL};
    v = slog_any_value(VALUER_ANY(&plv));
    res = slog_value_resolve(v);
    got = slog_value_any(&res);
    if (got.t != TYPE_ERROR) {
        testing_t_errorf_v(t, "expected error, got %T", got);
    } else {
        /* The error should provide some context information. Go looks for
         * this function's name in the stack it carries, and a C build only
         * has names when it has a symbol table, so this looks for what comes
         * before the stack. */
        Str msg = error_text(*(const Error *)got.data);
        if (!strings_has_prefix(msg, S("LogValue panicked\n")))
            testing_t_errorf_v(t, "got %q, want prefix %q", msg, "LogValue panicked\n");
    }
    arena_free(&ar);
}

static void TestValueTime(TestingT *t) {
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    /* Validate that all representations of times work correctly. */
    Time tms[] = {
        {0},
        time_from_unix(0, INT64_C(1000000000000000)), /* UnixNanos is defined */
        /* overflows UnixNanos */
        time_date(2300, TIME_JANUARY, 1, 0, 0, 0, 0, time_utc_loc),
    };
    for (size_t i = 0; i < sizeof tms / sizeof tms[0]; i++) {
        Time got = slog_value_time(slog_time_value(tms[i]));
        if (!time_equal(got, tms[i]))
            testing_t_errorf_v(t, "got %s, want %s", time_string(got, a),
                               time_string(tms[i], a));
        if (time_location(got) != time_location(tms[i]))
            testing_t_errorf_v(t, "%s: location: got %s, want %s",
                               time_string(tms[i], a),
                               time_location_string(time_location(got)),
                               time_location_string(time_location(tms[i])));
    }
    arena_free(&ar);
}

static void TestEmptyGroup(TestingT *t) {
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    SlogValue g = slog_group_value(ATTRS(INT("a", 1), GROUP("g1", EMPTY_GROUP("g2")),
                                         GROUP("g3", GROUP("g4", INT("b", 2)))));
    Slice got = slog_value_group(g);
    Slice want = ATTRS(INT("a", 1), GROUP("g3", GROUP("g4", INT("b", 2))));
    check_attrs(t, a, got, want);
    arena_free(&ar);
}

/* ----------------------------------------------------------- level tests */

static void TestLevelString(TestingT *t) {
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    struct {
        SlogLevel in;
        const char *want;
    } tests[] = {
        {0, "INFO"},
        {SLOG_LEVEL_ERROR, "ERROR"},
        {SLOG_LEVEL_ERROR + 2, "ERROR+2"},
        {SLOG_LEVEL_ERROR - 2, "WARN+2"},
        {SLOG_LEVEL_WARN, "WARN"},
        {SLOG_LEVEL_WARN - 1, "INFO+3"},
        {SLOG_LEVEL_INFO, "INFO"},
        {SLOG_LEVEL_INFO + 1, "INFO+1"},
        {SLOG_LEVEL_INFO - 3, "DEBUG+1"},
        {SLOG_LEVEL_DEBUG, "DEBUG"},
        {SLOG_LEVEL_DEBUG - 2, "DEBUG-2"},
    };
    for (size_t i = 0; i < sizeof tests / sizeof tests[0]; i++) {
        Str got = slog_level_string(tests[i].in, a);
        if (!str_eq(got, str_from_cstr(tests[i].want)))
            testing_t_errorf_v(t, "%d: got %s, want %s", tests[i].in, got,
                               tests[i].want);
    }
    arena_free(&ar);
}

static void TestLevelVar(TestingT *t) {
    SlogLevelVar al = {0};
    if (slog_level_var_level(&al) != SLOG_LEVEL_INFO)
        testing_t_errorf_v(t, "got %d, want %d", slog_level_var_level(&al),
                           SLOG_LEVEL_INFO);
    slog_level_var_set(&al, SLOG_LEVEL_WARN);
    if (slog_level_var_level(&al) != SLOG_LEVEL_WARN)
        testing_t_errorf_v(t, "got %d, want %d", slog_level_var_level(&al),
                           SLOG_LEVEL_WARN);
    slog_level_var_set(&al, SLOG_LEVEL_INFO);
    if (slog_level_var_level(&al) != SLOG_LEVEL_INFO)
        testing_t_errorf_v(t, "got %d, want %d", slog_level_var_level(&al),
                           SLOG_LEVEL_INFO);
}

static void TestLevelMarshalJSON(TestingT *t) {
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    SlogLevel want = SLOG_LEVEL_WARN - 3;
    Str want_data = S("\"INFO+1\"");
    Error err = BURROW_NO_ERROR;
    Slice data = slog_level_marshal_json(want, a, &err);
    if (BURROW_FAILED(err)) {
        testing_t_errorf_v(t, "%v", err);
        arena_free(&ar);
        return;
    }
    if (!str_eq(str_of(data), want_data))
        testing_t_errorf_v(t, "got %s, want %s", str_of(data), want_data);
    SlogLevel got = 0;
    err = slog_level_unmarshal_json(&got, a, data);
    if (BURROW_FAILED(err))
        testing_t_errorf_v(t, "%v", err);
    else if (got != want)
        testing_t_errorf_v(t, "got %s, want %s", slog_level_string(got, a),
                           slog_level_string(want, a));
    arena_free(&ar);
}

static void TestLevelMarshalText(TestingT *t) {
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    SlogLevel want = SLOG_LEVEL_WARN - 3;
    Str want_data = S("INFO+1");
    Error err = BURROW_NO_ERROR;
    Slice data = slog_level_marshal_text(want, a, &err);
    if (BURROW_FAILED(err)) {
        testing_t_errorf_v(t, "%v", err);
        arena_free(&ar);
        return;
    }
    if (!str_eq(str_of(data), want_data))
        testing_t_errorf_v(t, "got %s, want %s", str_of(data), want_data);
    SlogLevel got = 0;
    err = slog_level_unmarshal_text(&got, a, data);
    if (BURROW_FAILED(err))
        testing_t_errorf_v(t, "%v", err);
    else if (got != want)
        testing_t_errorf_v(t, "got %s, want %s", slog_level_string(got, a),
                           slog_level_string(want, a));
    arena_free(&ar);
}

static void TestLevelAppendText(TestingT *t) {
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    Slice buf = slice_make(a, TYPE_BYTE, 4, 16);
    SlogLevel want = SLOG_LEVEL_WARN - 3;
    Str want_data = S("\x00\x00\x00\x00INFO+1");
    Error err = BURROW_NO_ERROR;
    Slice data = slog_level_append_text(want, a, buf, &err);
    if (BURROW_FAILED(err))
        testing_t_errorf_v(t, "%v", err);
    else if (!str_eq(str_of(data), want_data))
        testing_t_errorf_v(t, "got %q, want %q", str_of(data), want_data);
    arena_free(&ar);
}

/* Level.parse, which UnmarshalText calls with the text as it is. */
static Error level_parse(SlogLevel *l, Alloc *a, Str s) {
    return slog_level_unmarshal_text(l, a, bytes_of(s));
}

static void TestLevelParse(TestingT *t) {
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    struct {
        const char *in;
        SlogLevel want;
    } tests[] = {
        {"DEBUG", SLOG_LEVEL_DEBUG},       {"INFO", SLOG_LEVEL_INFO},
        {"WARN", SLOG_LEVEL_WARN},         {"ERROR", SLOG_LEVEL_ERROR},
        {"debug", SLOG_LEVEL_DEBUG},       {"iNfo", SLOG_LEVEL_INFO},
        {"INFO+87", SLOG_LEVEL_INFO + 87}, {"Error-18", SLOG_LEVEL_ERROR - 18},
        {"Error-8", SLOG_LEVEL_INFO},
    };
    for (size_t i = 0; i < sizeof tests / sizeof tests[0]; i++) {
        SlogLevel got = 0;
        Error err = level_parse(&got, a, str_from_cstr(tests[i].in));
        if (BURROW_FAILED(err)) {
            testing_t_errorf_v(t, "%q: %v", tests[i].in, err);
            continue;
        }
        if (got != tests[i].want)
            testing_t_errorf_v(t, "%q: got %s, want %s", tests[i].in,
                               slog_level_string(got, a),
                               slog_level_string(tests[i].want, a));
    }
    arena_free(&ar);
}

static void TestLevelParseError(TestingT *t) {
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    struct {
        const char *in;
        const char *want; /* error string should contain this */
    } tests[] = {
        {"", "unknown name"},
        {"dbg", "unknown name"},
        {"INFO+", "invalid syntax"},
        {"INFO-", "invalid syntax"},
        {"ERROR+23x", "invalid syntax"},
    };
    for (size_t i = 0; i < sizeof tests / sizeof tests[0]; i++) {
        SlogLevel l = 0;
        Error err = level_parse(&l, a, str_from_cstr(tests[i].in));
        if (!BURROW_FAILED(err) ||
            !strings_contains(error_text(err), str_from_cstr(tests[i].want)))
            testing_t_errorf_v(t, "%q: got %v, want string containing %q", tests[i].in,
                               err, tests[i].want);
    }
    arena_free(&ar);
}

static Slice args_of(Str *p, Int n) {
    return slice_from(p, n, n, TYPE_STRING);
}

static void TestLevelFlag(TestingT *t) {
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    FlagFlagSet *fs = flag_new_flag_set(a, S("test"), FLAG_CONTINUE_ON_ERROR);
    SlogLevel lf = SLOG_LEVEL_INFO;
    flag_flag_set_text_var(fs, BURROW_ANY(TYPE_SLOG_LEVEL, &lf), S("level"),
                           BURROW_ANY(TYPE_SLOG_LEVEL, &lf), S("set level"));
    Str args[] = {S("-level"), S("WARN+3")};
    Error err = flag_flag_set_parse(fs, args_of(args, 2));
    if (BURROW_FAILED(err))
        testing_t_errorf_v(t, "%v", err);
    else if (lf != SLOG_LEVEL_WARN + 3)
        testing_t_errorf_v(t, "got %s, want %s", slog_level_string(lf, a),
                           slog_level_string(SLOG_LEVEL_WARN + 3, a));
    flag_flag_set_free(fs);
    arena_free(&ar);
}

static void TestLevelVarMarshalText(TestingT *t) {
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    SlogLevelVar v = {0};
    slog_level_var_set(&v, SLOG_LEVEL_WARN);
    Error err = BURROW_NO_ERROR;
    Slice data = slog_level_var_marshal_text(&v, a, &err);
    if (BURROW_FAILED(err)) {
        testing_t_errorf_v(t, "%v", err);
        arena_free(&ar);
        return;
    }
    SlogLevelVar v2 = {0};
    err = slog_level_var_unmarshal_text(&v2, a, data);
    if (BURROW_FAILED(err))
        testing_t_errorf_v(t, "%v", err);
    else if (slog_level_var_level(&v2) != SLOG_LEVEL_WARN)
        testing_t_errorf_v(t, "got %s, want %s",
                           slog_level_string(slog_level_var_level(&v2), a), "WARN");
    arena_free(&ar);
}

static void TestLevelVarAppendText(TestingT *t) {
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    SlogLevelVar v = {0};
    slog_level_var_set(&v, SLOG_LEVEL_WARN);
    Slice buf = slice_make(a, TYPE_BYTE, 4, 16);
    Error err = BURROW_NO_ERROR;
    Slice data = slog_level_var_append_text(&v, a, buf, &err);
    if (BURROW_FAILED(err)) {
        testing_t_errorf_v(t, "%v", err);
        arena_free(&ar);
        return;
    }
    SlogLevelVar v2 = {0};
    err = slog_level_var_unmarshal_text(&v2, a, slice_sub(data, 4, data.len));
    if (BURROW_FAILED(err))
        testing_t_errorf_v(t, "%v", err);
    else if (slog_level_var_level(&v2) != SLOG_LEVEL_WARN)
        testing_t_errorf_v(t, "got %s, want %s",
                           slog_level_string(slog_level_var_level(&v2), a), "WARN");
    arena_free(&ar);
}

static void TestLevelVarFlag(TestingT *t) {
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    FlagFlagSet *fs = flag_new_flag_set(a, S("test"), FLAG_CONTINUE_ON_ERROR);
    SlogLevelVar v = {0};
    slog_level_var_set(&v, SLOG_LEVEL_WARN + 3);
    flag_flag_set_text_var(fs, BURROW_ANY(TYPE_SLOG_LEVEL_VAR, &v), S("level"),
                           BURROW_ANY(TYPE_SLOG_LEVEL_VAR, &v), S("set level"));
    Str args[] = {S("-level"), S("WARN+3")};
    Error err = flag_flag_set_parse(fs, args_of(args, 2));
    if (BURROW_FAILED(err))
        testing_t_errorf_v(t, "%v", err);
    else if (slog_level_var_level(&v) != SLOG_LEVEL_WARN + 3)
        testing_t_errorf_v(t, "got %s, want %s",
                           slog_level_string(slog_level_var_level(&v), a), "WARN+3");
    flag_flag_set_free(fs);
    arena_free(&ar);
}

static void TestLevelVarString(TestingT *t) {
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    SlogLevelVar v = {0};
    slog_level_var_set(&v, SLOG_LEVEL_ERROR);
    Str got = slog_level_var_string(&v, a);
    if (!str_eq(got, S("LevelVar(ERROR)")))
        testing_t_errorf_v(t, "got %q, want %q", got, "LevelVar(ERROR)");
    arena_free(&ar);
}

/* ---------------------------------------------------------- record tests */

static void TestRecordAttrs(TestingT *t) {
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    Slice as =
        ATTRS(INT("k1", 1), STRING("k2", "foo"), INT("k3", 3), slog_int64(S("k4"), -1),
              slog_float64(S("f"), 3.1), slog_uint64(S("u"), 999));
    SlogRecord r = slog_new_record(time_now(), SLOG_LEVEL_INFO, S(""), 0);
    slog_record_add_attrs(&r, a, as);
    if (slog_record_num_attrs(&r) != as.len)
        testing_t_errorf_v(t, "NumAttrs: got %d, want %d", slog_record_num_attrs(&r),
                           as.len);
    check_attrs(t, a, attrs_slice(a, &r), as);

    /* Early return. Hit both loops in Record.Attrs: front and back. */
    Int stops[] = {2, 6};
    for (size_t i = 0; i < 2; i++) {
        AttrCollector c = {a, NO_ATTRS, stops[i]};
        slog_record_attrs(&r, collect_attr, &c);
        check_attrs(t, a, c.got, slice_sub(as, 0, stops[i]));
    }
    arena_free(&ar);
}

/* callerPC. */
BURROW_NOINLINE static Uintptr caller_pc(Int depth) {
    Uintptr pcs[1] = {0};
    (void)runtime_callers(depth, slice_from(pcs, 1, 1, TYPE_UINTPTR));
    return pcs[0];
}

static void TestRecordSource(TestingT *t) {
    /* Zero call depth => no Source. Go also checks the function and file at
     * depth 1 and 2. A C build has the function only when it has a symbol
     * table and never the file, so the name is checked when there is one. */
    struct {
        Int depth;
        const char *want_function;
        bool want_nil;
    } tests[] = {
        {0, "", true},
        {-16, "", true},
        {1, "TestRecordSource", false}, /* 1: caller of NewRecord */
        {2, NULL, false},
    };
    for (size_t i = 0; i < sizeof tests / sizeof tests[0]; i++) {
        Uintptr pc = 0;
        if (tests[i].depth > 0)
            pc = caller_pc(tests[i].depth + 1);
        SlogRecord r = slog_new_record((Time){0}, 0, S(""), pc);
        SlogSource got;
        bool ok = slog_record_source(&r, &got);
        if (tests[i].want_nil) {
            if (ok)
                testing_t_errorf_v(t, "depth %d: got non-nil Source, want nil",
                                   tests[i].depth);
            continue;
        }
        if (!ok) {
            testing_t_errorf_v(t, "depth %d: got nil Source, want non-nil",
                               tests[i].depth);
            continue;
        }
        if (tests[i].want_function != NULL && got.function.len > 0 &&
            !str_eq(got.function, str_from_cstr(tests[i].want_function)))
            testing_t_errorf_v(t, "depth %d: got function %q, want %q", tests[i].depth,
                               got.function, tests[i].want_function);
    }
}

static void TestAliasingAndClone(TestingT *t) {
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    enum { N = BURROW__SLOG_NATTRS_INLINE + 1 };
    SlogAttr ints[N];
    for (Int i = 0; i < N; i++)
        ints[i] = INT("k", i);

    /* Create a record whose Attrs overflow the inline array, creating a slice
     * in r.back. */
    SlogRecord r1 = slog_new_record((Time){0}, 0, S(""), 0);
    slog_record_add_attrs(&r1, a, attrs_of(ints, N));
    /* Ensure that r1.back's capacity exceeds its length. */
    Slice b = slice_make(a, TYPE_SLOG_ATTR, r1.back.len, r1.back.len + 1);
    memcpy(b.p, r1.back.p, (size_t)r1.back.len * sizeof(SlogAttr));
    r1.back = b;
    /* Make a copy that shares state. */
    SlogRecord r2 = r1;
    /* Adding to both should insert a special Attr in the second. */
    Slice before = attrs_slice(a, &r1);
    slog_record_add_attrs(&r1, a, ATTRS(INT("p", 0)));
    slog_record_add_attrs(&r2, a, ATTRS(INT("p", 1)));
    check_attrs(t, a, attrs_slice(a, &r1), attrs_concat(a, before, ATTRS(INT("p", 0))));
    Slice r1_attrs = attrs_slice(a, &r1);
    check_attrs(
        t, a, attrs_slice(a, &r2),
        attrs_concat(a, before,
                     ATTRS(STRING("!BUG", "AddAttrs unsafely called on copy of "
                                          "Record made without using Record.Clone"),
                           INT("p", 1))));

    /* Adding to a clone is fine. */
    r2 = slog_record_clone(r1);
    check_attrs(t, a, attrs_slice(a, &r2), r1_attrs);
    slog_record_add_attrs(&r2, a, ATTRS(INT("p", 2)));
    check_attrs(t, a, attrs_slice(a, &r1), r1_attrs); /* r1 is unchanged */
    check_attrs(t, a, attrs_slice(a, &r2),
                attrs_concat(a, r1_attrs, ATTRS(INT("p", 2))));
    arena_free(&ar);
}

/* ---------------------------------------------------------- logger tests */

static SlogHandler text_handler(Alloc *a, BytesBuffer *b, const SlogHandlerOptions *o) {
    return slog_text_handler_as_handler(
        slog_new_text_handler(a, bytes_buffer_as_io_writer(b), o));
}

/* The check in TestLogTextHandler. */
static void check_text(TestingT *t, Alloc *a, BytesBuffer *buf, const char *want) {
    Str w = S("");
    if (want[0] != '\0')
        w = fmt_sprintf_v(a, "time=" TEXT_TIME_RE " %s", want);
    check_log_output(t, a, buf_str(buf), w);
    bytes_buffer_reset(buf);
}

static void TestLogTextHandler(TestingT *t) {
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    Context ctx = context_background();
    BytesBuffer buf = BYTES_BUFFER(a);
    SlogLogger *l = slog_new(a, text_handler(a, &buf, NULL));

    slog_logger_info_v(l, "msg", "a", 1, "b", 2);
    check_text(t, a, &buf, "level=INFO msg=msg a=1 b=2");

    /* By default, debug messages are not printed. */
    slog_logger_debug_v(l, "bg", INT("a", 1), "b", 2);
    check_text(t, a, &buf, "");

    slog_logger_warn_v(l, "w", slog_duration(S("dur"), 3 * TIME_SECOND));
    check_text(t, a, &buf, "level=WARN msg=w dur=3s");

    slog_logger_error_v(l, "bad", "a", 1);
    check_text(t, a, &buf, "level=ERROR msg=bad a=1");

    slog_logger_log_v(l, ctx, SLOG_LEVEL_WARN + 1, "w", INT("a", 1),
                      STRING("b", "two"));
    check_text(t, a, &buf, "level=WARN\\+1 msg=w a=1 b=two");

    slog_logger_log_attrs_v(l, ctx, SLOG_LEVEL_INFO + 1, "a b c", INT("a", 1),
                            STRING("b", "two"));
    check_text(t, a, &buf, "level=INFO\\+1 msg=\"a b c\" a=1 b=two");

    slog_logger_info_v(l, "info", "a",
                       BURROW_ANY_VAL(TYPE_OF(AttrSlice), Slice, ATTRS(INT("i", 1))));
    check_text(t, a, &buf, "level=INFO msg=info a.i=1");

    slog_logger_info_v(l, "info", "a", slog_group_value(ATTRS(INT("i", 1))));
    check_text(t, a, &buf, "level=INFO msg=info a.i=1");
    slog_logger_free(l);
    arena_free(&ar);
}

/* for a typed nil pointer */
#define AB_FIELDS(F, T)                                                                \
    F(T, Int, A, "")                                                                   \
    F(T, Int, B, "")
BURROW_STRUCT(AB, AB_FIELDS);
BURROW_PTR_TYPE(ABPtr, AB);

/* wrappingHandler. */
static bool wrap_enabled(void *self, Context ctx, SlogLevel level) {
    SlogHandler *h = self;
    return h->vt->enabled(h->data, ctx, level);
}

static Error wrap_handle(void *self, Context ctx, SlogRecord r) {
    SlogHandler *h = self;
    return h->vt->handle(h->data, ctx, r);
}

static SlogHandler wrap_with_attrs(void *self, Slice as) {
    SlogHandler *h = self;
    return h->vt->with_attrs(h->data, as);
}

static SlogHandler wrap_with_group(void *self, Str name) {
    SlogHandler *h = self;
    return h->vt->with_group(h->data, name);
}

static const SlogHandlerVT wrapping_vt = {
    NULL, wrap_enabled, wrap_handle, wrap_with_attrs, wrap_with_group, NULL,
};

static void TestConnections(TestingT *t) {
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    BytesBuffer logbuf = BYTES_BUFFER(a);
    BytesBuffer slogbuf = BYTES_BUFFER(a);

    /* Revert any changes to the default logger. This is important because
     * other tests might change the default logger using SetDefault. Also
     * ensure we restore the default logger at the end of the test. */
    Saved saved = save_defaults();
    slog_set_default(slog_new(a, burrow__slog_std_handler()));

    /* The default slog.Logger's handler uses the log package's default
     * output. */
    log_set_output(bytes_buffer_as_io_writer(&logbuf));
    log_set_flags(LOG_LSHORTFILE & ~LOG_LSTD_FLAGS);
    slog_info_v("msg", "a", 1);
    check_log_output(t, a, buf_str(&logbuf), S("slog_test.c:\\d+: INFO msg a=1"));
    bytes_buffer_reset(&logbuf);
    slog_info_v("msg", "p", ((Any){NULL, NULL}));
    check_log_output(t, a, buf_str(&logbuf), S("slog_test.c:\\d+: INFO msg p=<nil>"));
    bytes_buffer_reset(&logbuf);
    ABPtr r = NULL;
    slog_info_v("msg", "r", BURROW_ANY(TYPE_OF(ABPtr), &r));
    check_log_output(t, a, buf_str(&logbuf), S("slog_test.c:\\d+: INFO msg r=<nil>"));
    bytes_buffer_reset(&logbuf);
    slog_warn_v("msg", "b", 2);
    check_log_output(t, a, buf_str(&logbuf), S("slog_test.c:\\d+: WARN msg b=2"));
    bytes_buffer_reset(&logbuf);
    slog_error_v("msg", "err", io_eof, "c", 3);
    check_log_output(t, a, buf_str(&logbuf),
                     S("slog_test.c:\\d+: ERROR msg err=EOF c=3"));

    /* Levels below Info are not printed. */
    bytes_buffer_reset(&logbuf);
    slog_debug_v("msg", "c", 3);
    check_log_output(t, a, buf_str(&logbuf), S(""));

    /* wrap default handler: it should be possible to wrap the default handler
     * and get the right output. This works because the default handler uses
     * the source in the Record, rather than a call depth. */
    {
        SlogHandler inner = slog_logger_handler(slog_default());
        SlogLogger *logger = slog_new(a, (SlogHandler){&wrapping_vt, &inner});
        slog_logger_info_v(logger, "msg", "d", 4);
        check_log_output(t, a, buf_str(&logbuf), S("slog_test.c:\\d+: INFO msg d=4"));
        slog_logger_free(logger);
    }

    /* Once slog.SetDefault is called, the direction is reversed: the default
     * log.Logger's output goes through the handler. */
    SlogHandlerOptions src_opts = {0};
    src_opts.add_source = true;
    slog_set_default(slog_new(a, text_handler(a, &slogbuf, &src_opts)));
    log_print_v("msg2");
    check_log_output(
        t, a, buf_str(&slogbuf),
        S("time=" TEXT_TIME_RE " level=INFO source=.*slog_test.c:\\d+\"? msg=msg2"));

    /* The default log.Logger always outputs at Info level. */
    bytes_buffer_reset(&slogbuf);
    SlogHandlerOptions warn_opts = {0};
    warn_opts.level = slog_level_as_leveler(SLOG_LEVEL_WARN);
    slog_set_default(slog_new(a, text_handler(a, &slogbuf, &warn_opts)));
    log_print_v("should not appear");
    if (buf_str(&slogbuf).len != 0)
        testing_t_errorf_v(t, "got %q, want empty", buf_str(&slogbuf));

    /* Setting log's output again breaks the connection. */
    bytes_buffer_reset(&logbuf);
    bytes_buffer_reset(&slogbuf);
    log_set_output(bytes_buffer_as_io_writer(&logbuf));
    log_set_flags(LOG_LSHORTFILE & ~LOG_LSTD_FLAGS);
    log_print_v("msg3");
    check_log_output(t, a, buf_str(&logbuf), S("slog_test.c:\\d+: msg3"));
    if (buf_str(&slogbuf).len != 0)
        testing_t_errorf_v(t, "got %q, want empty", buf_str(&slogbuf));

    restore_defaults(saved);
    arena_free(&ar);
}

static void TestAttrs(TestingT *t) {
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    SlogLogger *l1 =
        slog_logger_with_v(slog_new(a, capture_handler(new_capture(a))), "a", 1);
    SlogLogger *l2 = slog_logger_with_v(slog_new(a, slog_logger_handler(l1)), "b", 2);
    slog_logger_info_v(l2, "m", "c", 3);
    CaptureHandler *h = slog_logger_handler(l2).data;
    check_attrs(t, a, h->attrs, ATTRS(INT("a", 1), INT("b", 2)));
    check_attrs(t, a, attrs_slice(a, &h->r), ATTRS(INT("c", 3)));
    arena_free(&ar);
}

/* The calls in TestCallDepth, each with the line it is on. */
#define CALL_DEPTH(call)                                                               \
    do {                                                                               \
        int line_ = __LINE__;                                                          \
        call;                                                                          \
        check_call_depth(t, h, line_);                                                 \
    } while (0)

static void check_call_depth(TestingT *t, CaptureHandler *h, int want_line) {
    SlogSource got;
    if (!slog_record_source(&h->r, &got)) {
        testing_t_error_v(t, "got nil source");
        return;
    }
    Str got_file = filepath_base(got.file);
    if (!str_eq(got.function, S("TestCallDepth")) ||
        !str_eq(got_file, S("slog_test.c")) || got.line != want_line)
        testing_t_errorf_v(t, "got (%s, %s, %d), want (%s, %s, %d)", got.function,
                           got_file, got.line, "TestCallDepth", "slog_test.c",
                           want_line);
}

static void TestCallDepth(TestingT *t) {
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    Context ctx = context_background();
    CaptureHandler *h = new_capture(a);
    Saved saved = save_defaults();
    SlogLogger *logger = slog_new(a, capture_handler(h));
    slog_set_default(logger);

    CALL_DEPTH(slog_logger_log_v(logger, ctx, SLOG_LEVEL_INFO, ""));
    CALL_DEPTH(slog_logger_log_attrs_v(logger, ctx, SLOG_LEVEL_INFO, "", INT("a", 1)));
    CALL_DEPTH(slog_logger_debug_v(logger, ""));
    CALL_DEPTH(slog_logger_info_v(logger, ""));
    CALL_DEPTH(slog_logger_warn_v(logger, ""));
    CALL_DEPTH(slog_logger_error_v(logger, ""));
    CALL_DEPTH(slog_debug_v(""));
    CALL_DEPTH(slog_info_v(""));
    CALL_DEPTH(slog_warn_v(""));
    CALL_DEPTH(slog_error_v(""));
    CALL_DEPTH(slog_log_v(ctx, SLOG_LEVEL_INFO, ""));
    CALL_DEPTH(slog_log_attrs_v(ctx, SLOG_LEVEL_INFO, "", INT("a", 1)));

    restore_defaults(saved);
    arena_free(&ar);
}

static void TestSetAttrs(TestingT *t) {
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    Str bad = BURROW__SLOG_BAD_KEY;
    struct {
        Slice args;
        Slice want;
    } tests[] = {
        {NO_ARGS, NO_ATTRS},
        {slog_args_v("a", 1), ATTRS(INT("a", 1))},
        {slog_args_v("a", 1, "b", "two"), ATTRS(INT("a", 1), STRING("b", "two"))},
        {slog_args_v("a"), ATTRS(slog_string(bad, S("a")))},
        {slog_args_v("a", 1, "b"), ATTRS(INT("a", 1), slog_string(bad, S("b")))},
        {slog_args_v("a", 1, 2, 3),
         ATTRS(INT("a", 1), slog_int(bad, 2), slog_int(bad, 3))},
    };
    for (size_t i = 0; i < sizeof tests / sizeof tests[0]; i++) {
        SlogRecord r = slog_new_record((Time){0}, 0, S(""), 0);
        slog_record_add(&r, a, tests[i].args);
        Slice got = attrs_slice(a, &r);
        if (!attrs_equal(got, tests[i].want))
            testing_t_errorf_v(t, "%v:\ngot  %s\nwant %s", tests[i].args,
                               attrs_string(a, got), attrs_string(a, tests[i].want));
    }
    arena_free(&ar);
}

typedef struct SetDefaultEnv {
    ContextCancelFunc cancel;
} SetDefaultEnv;

static void set_default_worker(void *env) {
    SetDefaultEnv *e = env;
    slog_info_v("A");
    slog_set_default(slog_default());
    slog_info_v("B");
    BURROW_CALLF0(e->cancel);
}

static void TestSetDefault(TestingT *t) {
    /* Verify that setting the default to itself does not result in
     * deadlock. */
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    SetDefaultEnv env;
    Context ctx =
        context_with_timeout(a, context_background(), TIME_SECOND, &env.cancel);
    Saved saved = save_defaults();
    log_set_output(io_discard);
    go(BURROW_FN(Func, set_default_worker, &env));
    (void)chan_recv(context_done(ctx), NULL);
    Error err = context_err(ctx);
    if (!errors_is(err, context_canceled))
        testing_t_errorf_v(t, "wanted canceled, got %v", err);
    restore_defaults(saved);
    context_release(ctx);
    arena_free(&ar);
}

/* Test defaultHandler minimum level without calling slog.SetDefault. */
static void TestLogLoggerLevelForDefaultHandler(TestingT *t) {
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    /* Revert any changes to the default logger, flags, and level of log and
     * slog. */
    Saved saved = save_defaults();
    BytesBuffer log_buf = BYTES_BUFFER(a);
    log_set_output(bytes_buffer_as_io_writer(&log_buf));
    log_set_flags(0);
    struct {
        SlogLevel log_level;
        bool debug;
        const char *want;
    } tests[] = {
        {SLOG_LEVEL_DEBUG, true, "DEBUG a"},
        {SLOG_LEVEL_DEBUG, false, "INFO a"},
        {SLOG_LEVEL_INFO, true, ""},
        {SLOG_LEVEL_INFO, false, "INFO a"},
    };
    for (size_t i = 0; i < sizeof tests / sizeof tests[0]; i++) {
        slog_set_log_logger_level(tests[i].log_level);
        if (tests[i].debug)
            slog_debug_v("a");
        else
            slog_info_v("a");
        check_log_output(t, a, buf_str(&log_buf), str_from_cstr(tests[i].want));
        bytes_buffer_reset(&log_buf);
    }
    restore_defaults(saved);
    arena_free(&ar);
}

static SlogAttr remove_time(void *ctx, Alloc *a, Slice groups, SlogAttr attr) {
    (void)ctx;
    (void)a;
    (void)groups;
    if (str_eq(attr.key, SLOG_TIME_KEY))
        return (SlogAttr){0};
    return attr;
}

/* Test handlerWriter minimum level by calling slog.SetDefault. */
static void TestLogLoggerLevelForHandlerWriter(TestingT *t) {
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    /* Revert any changes to the default logger. This is important because
     * other tests might change the default logger using SetDefault. Also
     * ensure we restore the default logger at the end of the test. */
    Saved saved = save_defaults();
    BytesBuffer log_buf = BYTES_BUFFER(a);
    log_set_output(bytes_buffer_as_io_writer(&log_buf));
    log_set_flags(0);
    slog_set_log_logger_level(SLOG_LEVEL_ERROR);
    SlogHandlerOptions opts = {0};
    opts.replace_attr = remove_time;
    slog_set_default(slog_new(a, text_handler(a, &log_buf, &opts)));
    log_print_v("error");
    check_log_output(t, a, buf_str(&log_buf), S("level=ERROR msg=error"));
    restore_defaults(saved);
    arena_free(&ar);
}

static void TestLoggerError(TestingT *t) {
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    BytesBuffer buf = BYTES_BUFFER(a);
    SlogHandlerOptions opts = {0};
    opts.replace_attr = remove_time;
    SlogLogger *l = slog_new(a, text_handler(a, &buf, &opts));
    slog_logger_error_v(l, "msg", "err", io_eof, "a", 1);
    check_log_output(t, a, buf_str(&buf), S("level=ERROR msg=msg err=EOF a=1"));
    bytes_buffer_reset(&buf);
    Slice args = slog_args_v("err", io_eof, "a");
    slog_logger_error(l, S("msg"), args);
    check_log_output(t, a, buf_str(&buf), S("level=ERROR msg=msg err=EOF !BADKEY=a"));
    slog_logger_free(l);
    arena_free(&ar);
}

static void TestNewLogLogger(TestingT *t) {
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    BytesBuffer buf = BYTES_BUFFER(a);
    SlogHandler h = text_handler(a, &buf, NULL);
    LogLogger *ll = slog_new_log_logger(a, h, SLOG_LEVEL_WARN);
    log_logger_print_v(ll, "hello");
    check_log_output(t, a, buf_str(&buf),
                     S("time=" TEXT_TIME_RE " level=WARN msg=hello"));
    slog_log_logger_free(a, ll);
    arena_free(&ar);
}

/* Go's With() and WithGroup("") hand back the receiver. Here a logger is
 * always a new one to free, so what is checked is that it has the same
 * handler. */
static void TestLoggerNoOps(TestingT *t) {
    SlogLogger *l = slog_default();
    void *h = slog_logger_handler(l).data;
    SlogLogger *w = slog_logger_with(l, NO_ARGS);
    if (slog_logger_handler(w).data != h)
        testing_t_error_v(t, "wanted receiver, didn't get it");
    slog_logger_free(w);
    w = slog_with(NO_ARGS);
    if (slog_logger_handler(w).data != h)
        testing_t_error_v(t, "wanted receiver, didn't get it");
    slog_logger_free(w);
    w = slog_logger_with_group(l, S(""));
    if (slog_logger_handler(w).data != h)
        testing_t_error_v(t, "wanted receiver, didn't get it");
    slog_logger_free(w);
}

static void TestContext(TestingT *t) {
    /* Verify that the context argument to log output methods is passed to
     * the handler. Also check the level. */
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    CaptureHandler *h = new_capture(a);
    SlogLogger *l = slog_new(a, capture_handler(h));
    Saved saved = save_defaults();
    slog_set_default(l);
    struct {
        void (*lf)(SlogLogger *l, Context ctx, Str msg, Slice args);
        void (*f)(Context ctx, Str msg, Slice args);
        SlogLevel want_level;
    } tests[] = {
        {slog_logger_debug_context, NULL, SLOG_LEVEL_DEBUG},
        {slog_logger_info_context, NULL, SLOG_LEVEL_INFO},
        {slog_logger_warn_context, NULL, SLOG_LEVEL_WARN},
        {slog_logger_error_context, NULL, SLOG_LEVEL_ERROR},
        {NULL, slog_debug_context, SLOG_LEVEL_DEBUG},
        {NULL, slog_info_context, SLOG_LEVEL_INFO},
        {NULL, slog_warn_context, SLOG_LEVEL_WARN},
        {NULL, slog_error_context, SLOG_LEVEL_ERROR},
    };
    Str key = S("L");
    Any k = BURROW_ANY(TYPE_STRING, &key);
    for (size_t i = 0; i < sizeof tests / sizeof tests[0]; i++) {
        capture_clear(h);
        Context ctx =
            context_with_value(a, context_background(), k,
                               BURROW_ANY(TYPE_SLOG_LEVEL, &tests[i].want_level));
        if (tests[i].lf != NULL)
            tests[i].lf(l, ctx, S("msg"), NO_ARGS);
        else
            tests[i].f(ctx, S("msg"), NO_ARGS);
        Any gv = h->ctx.vt != NULL ? context_value(h->ctx, k) : (Any){NULL, NULL};
        bool same = gv.t == TYPE_SLOG_LEVEL &&
                    *(const SlogLevel *)gv.data == tests[i].want_level;
        if (!same || h->r.level != tests[i].want_level)
            testing_t_errorf_v(t, "got context value %v, level %s; want %s for both",
                               gv, slog_level_string(h->r.level, a),
                               slog_level_string(tests[i].want_level, a));
        context_release(ctx);
    }
    restore_defaults(saved);
    arena_free(&ar);
}

/* panicTextAndJsonMarshaler, a type that panics in MarshalText and
 * MarshalJSON. Go calls its value methods through a nil pointer by way of a
 * wrapper that panics, and here the methods do that themselves. */
#define PANIC_FIELDS(F, T) F(T, Any, msg, "")
BURROW_STRUCT_DECL(PanicMarshaler, PANIC_FIELDS);

static Slice panic_marshal(PanicMarshaler *p) {
    if (p == NULL)
        panic_str(
            S("runtime error: invalid memory address or nil pointer dereference"));
    panic(p->msg);
}

static Slice panic_marshal_text(PanicMarshaler *p, Alloc *a, Error *err) {
    (void)a;
    (void)err;
    return panic_marshal(p);
}

static Slice panic_marshal_json(PanicMarshaler *p, Alloc *a, Error *err) {
    (void)a;
    (void)err;
    return panic_marshal(p);
}

#define PANIC_METHODS(M, T)                                                            \
    M(T, MarshalJSON, panic_marshal_json, JSONV2_SIG_MARSHAL_JSON)                     \
    M(T, MarshalText, panic_marshal_text, ENCODING_SIG_MARSHAL_TEXT)
BURROW_STRUCT_DEFINE_METHODS(PanicMarshaler, PANIC_FIELDS, PANIC_METHODS);
BURROW_PTR_TYPE(PanicMarshalerPtr, PanicMarshaler);

static const Str panicking = BURROW_S_INIT("panicking");
static const Int forty_two = 42;

static void TestPanics(TestingT *t) {
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    /* Revert any changes to the default logger. This is important because
     * other tests might change the default logger using SetDefault. Also
     * ensure we restore the default logger at the end of the test. */
    Saved saved = save_defaults();
    BytesBuffer log_buf = BYTES_BUFFER(a);
    log_set_output(bytes_buffer_as_io_writer(&log_buf));
    log_set_flags(LOG_LSHORTFILE & ~LOG_LSTD_FLAGS);

    PanicMarshalerPtr nil = NULL;
    PanicMarshaler eof = {{TYPE_ERROR, (void *)(uintptr_t)&io_err_unexpected_eof}};
    PanicMarshaler str = {{TYPE_STRING, (void *)(uintptr_t)&panicking}};
    PanicMarshaler num = {{TYPE_INT, (void *)(uintptr_t)&forty_two}};
    Any ins[] = {
        BURROW_ANY(TYPE_OF(PanicMarshalerPtr), &nil),
        BURROW_ANY(TYPE_OF(PanicMarshaler), &eof),
        BURROW_ANY(TYPE_OF(PanicMarshaler), &str),
        BURROW_ANY(TYPE_OF(PanicMarshaler), &num),
    };

    slog_set_default(slog_new(a, burrow__slog_std_handler()));
    const char *text_out[] = {
        "slog_test.c:\\d+: INFO msg p=<nil>",
        "slog_test.c:\\d+: INFO msg p=\"!PANIC: unexpected EOF\"",
        "slog_test.c:\\d+: INFO msg p=\"!PANIC: panicking\"",
        "slog_test.c:\\d+: INFO msg p=\"!PANIC: 42\"",
    };
    for (size_t i = 0; i < 4; i++) {
        slog_info_v("msg", "p", ins[i]);
        check_log_output(t, a, buf_str(&log_buf), str_from_cstr(text_out[i]));
        bytes_buffer_reset(&log_buf);
    }

    slog_set_default(slog_new(a, slog_json_handler_as_handler(slog_new_json_handler(
                                     a, bytes_buffer_as_io_writer(&log_buf), NULL))));
    const char *json_out[] = {
        "{\"time\":\"" JSON_TIME_RE "\",\"level\":\"INFO\",\"msg\":\"msg\",\"p\":null}",
        "{\"time\":\"" JSON_TIME_RE
        "\",\"level\":\"INFO\",\"msg\":\"msg\",\"p\":\"!PANIC: "
        "unexpected EOF\"}",
        "{\"time\":\"" JSON_TIME_RE
        "\",\"level\":\"INFO\",\"msg\":\"msg\",\"p\":\"!PANIC: "
        "panicking\"}",
        "{\"time\":\"" JSON_TIME_RE
        "\",\"level\":\"INFO\",\"msg\":\"msg\",\"p\":\"!PANIC: "
        "42\"}",
    };
    for (size_t i = 0; i < 4; i++) {
        slog_info_v("msg", "p", ins[i]);
        check_log_output(t, a, buf_str(&log_buf), str_from_cstr(json_out[i]));
        bytes_buffer_reset(&log_buf);
    }
    restore_defaults(saved);
    arena_free(&ar);
}

#define TESTS(X)                                                                       \
    X(TestKindString)                                                                  \
    X(TestValueEqual)                                                                  \
    X(TestValueString)                                                                 \
    X(TestAnyValue)                                                                    \
    X(TestValueAny)                                                                    \
    X(TestLogValue)                                                                    \
    X(TestValueTime)                                                                   \
    X(TestEmptyGroup)                                                                  \
    X(TestLevelString)                                                                 \
    X(TestLevelVar)                                                                    \
    X(TestLevelMarshalJSON)                                                            \
    X(TestLevelMarshalText)                                                            \
    X(TestLevelAppendText)                                                             \
    X(TestLevelParse)                                                                  \
    X(TestLevelParseError)                                                             \
    X(TestLevelFlag)                                                                   \
    X(TestLevelVarMarshalText)                                                         \
    X(TestLevelVarAppendText)                                                          \
    X(TestLevelVarFlag)                                                                \
    X(TestLevelVarString)                                                              \
    X(TestRecordAttrs)                                                                 \
    X(TestRecordSource)                                                                \
    X(TestAliasingAndClone)                                                            \
    X(TestLogTextHandler)                                                              \
    X(TestConnections)                                                                 \
    X(TestAttrs)                                                                       \
    X(TestCallDepth)                                                                   \
    X(TestSetAttrs)                                                                    \
    X(TestSetDefault)                                                                  \
    X(TestLogLoggerLevelForDefaultHandler)                                             \
    X(TestLogLoggerLevelForHandlerWriter)                                              \
    X(TestLoggerError)                                                                 \
    X(TestNewLogLogger)                                                                \
    X(TestLoggerNoOps)                                                                 \
    X(TestContext)                                                                     \
    X(TestPanics)
TESTING_MAIN(TESTS)
