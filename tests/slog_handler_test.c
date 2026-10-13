/* Tests for log/slog's handlers, after Go's src/log/slog/handler_test.go,
 * text_handler_test.go, json_handler_test.go and multi_handler_test.go.
 *
 * Go's Source case expects its own function, log/slog.TestJSONAndTextHandlers.
 * Here the record's source is set by hand from __func__, which has no package
 * in front of it. The tests that count allocations are left out, as there is
 * no AllocsPerRun to count them with, and the ones that call unexported
 * helpers go through a handler instead.
 *
 * Copyright 2022 The Go Authors. All rights reserved.
 * Copyright 2026 The burrow Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style licence that can be found
 * in the LICENSE file. */

#include "check.h"

#include "../src/log/slog_internal.h"

#include "burrow/burrow.h"
#include "burrow/declare.h"
#include "burrow/encoding/json/v2.h"
#include "burrow/mem/arena.h"
#include "burrow/path/filepath.h"

#include <stdint.h>

#define S(s) BURROW_S(s)

static Slice attrs_of(SlogAttr *p, Int n) {
    return slice_from(p, n, n, TYPE_SLOG_ATTR);
}

/* The arguments as a Slice of SlogAttr, which lasts until the end of the
 * enclosing block. */
#define ATTRS(...) attrs_of((SlogAttr[]){__VA_ARGS__}, BURROW__FMT_NARGS(__VA_ARGS__))
#define NO_ATTRS slice_nil(TYPE_SLOG_ATTR)
#define GROUP(k, ...) slog_group_attrs(S(k), ATTRS(__VA_ARGS__))
#define EMPTY_GROUP(k) slog_group_attrs(S(k), NO_ATTRS)
#define INT(k, v) slog_int(S(k), (v))
#define STRING(k, v) slog_string(S(k), S(v))

BURROW_SLICE_TYPE(AttrSlice, SlogAttr);
BURROW_SLICE_TYPE(StrSlice, Str);
BURROW_MAP_TYPE(StrIntMap, Str, Int);

static Time test_time(void) {
    return time_date(2000, TIME_JANUARY, 2, 3, 4, 5, 0, time_utc_loc);
}

static Str buf_str(BytesBuffer *b) {
    Slice s = bytes_buffer_bytes(b);
    return str_from_bytes(s.p, s.len);
}

/* strings.TrimSuffix(s, "\n"). */
static Str trim_newline(Str s) {
    if (s.len > 0 && s.p[s.len - 1] == '\n')
        s.len--;
    return s;
}

static SlogHandler with_attrs(SlogHandler h, Slice as) {
    return h.vt->with_attrs(h.data, as);
}

static SlogHandler with_group(SlogHandler h, Str name) {
    return h.vt->with_group(h.data, name);
}

static Error handle(SlogHandler h, SlogRecord r) {
    return h.vt->handle(h.data, context_background(), r);
}

static SlogHandler text_handler(Alloc *a, BytesBuffer *b, const SlogHandlerOptions *o) {
    return slog_text_handler_as_handler(
        slog_new_text_handler(a, bytes_buffer_as_io_writer(b), o));
}

static SlogHandler json_handler(Alloc *a, BytesBuffer *b, const SlogHandlerOptions *o) {
    return slog_json_handler_as_handler(
        slog_new_json_handler(a, bytes_buffer_as_io_writer(b), o));
}

/* The regexp's search memory is on the heap whatever a is, so it is freed
 * here. */
static bool matches(Alloc *a, Str pattern, Str s) {
    Regexp *re = regexp_must_compile(a, pattern);
    bool ok = regexp_match_string(re, s);
    regexp_free(re);
    return ok;
}

#define TEXT_TIME_RE                                                                   \
    "\\d{4}-\\d{2}-\\d{2}T\\d{2}:\\d{2}:\\d{2}\\.\\d{3}(Z|[+-]\\d{2}:\\d{2})"
#define JSON_TIME_RE                                                                   \
    "\\d{4}-\\d{2}-\\d{2}T\\d{2}:\\d{2}:\\d{2}(\\.\\d+)?(Z|[+-]\\d{2}:\\d{2})"

/* checkLogOutput: got, without its last newline, matched against the whole of
 * want. */
static void check_log_output(TestingT *t, Alloc *a, Str got, const char *want) {
    got = trim_newline(got);
    got = strings_replace_all(a, got, S("\n"), S("~"));
    Str re = fmt_sprintf_v(a, "^%s$", want);
    if (!matches(a, re, got))
        testing_t_errorf_v(t, "\ngot  %s\nwant %s", got, re);
}

/* ---------------------------------------------------------- replace funcs */

/* Keys for remove_keys, which takes a NULL terminated list of them as its
 * context. The lists are constant and remove_keys only reads them. */
static const char *const time_level_keys[] = {"time", "level", NULL};
static const char *const builtin_keys[] = {"time", "level", "msg", NULL};
static const char *const time_key[] = {"time", NULL};
static const char *const level_key[] = {"level", NULL};
static const char *const level_msg_keys[] = {"level", "msg", NULL};
static const char *const partial_a_keys[] = {"time", "level", "msg", "a", NULL};
static const char *const partial_ab_keys[] = {"time", "level", "msg", "a", "b", NULL};
static const char *const partial_ac_keys[] = {"time", "level", "msg", "a", "c", NULL};

static void *keys(const char *const *k) {
    return (void *)(uintptr_t)k;
}

/* removeKeys. */
static SlogAttr remove_keys(void *ctx, Alloc *a, Slice groups, SlogAttr attr) {
    (void)a;
    (void)groups;
    for (const char *const *k = ctx; *k != NULL; k++)
        if (str_eq(attr.key, str_from_cstr(*k)))
            return (SlogAttr){0};
    return attr;
}

static SlogAttr remove_all(void *ctx, Alloc *a, Slice groups, SlogAttr attr) {
    (void)ctx;
    (void)a;
    (void)groups;
    (void)attr;
    return (SlogAttr){0};
}

/* upperCaseKey. */
static SlogAttr upper_case_key(void *ctx, Alloc *a, Slice groups, SlogAttr attr) {
    (void)ctx;
    (void)groups;
    attr.key = strings_to_upper(a, attr.key);
    return attr;
}

/* The Source case's: the file cut down to its base name. */
static SlogAttr base_source(void *ctx, Alloc *a, Slice groups, SlogAttr attr) {
    (void)ctx;
    if (str_eq(attr.key, SLOG_SOURCE_KEY)) {
        Any x = slog_value_any(&attr.value);
        SlogSource *s = x.data;
        s->file = filepath_base(s->file);
        return slog_any(attr.key, x);
    }
    return remove_keys(keys(time_level_keys), a, groups, attr);
}

/* "replace built-in with group". */
static SlogAttr time_as_group(void *ctx, Alloc *a, Slice groups, SlogAttr attr) {
    (void)ctx;
    (void)groups;
    if (str_eq(attr.key, SLOG_TIME_KEY))
        return slog_group(a, SLOG_TIME_KEY, slog_args_v("mins", 3, "secs", 2));
    if (str_eq(attr.key, SLOG_LEVEL_KEY))
        return (SlogAttr){0};
    return attr;
}

/* "replace resolved group". */
static SlogAttr groups_bad(void *ctx, Alloc *a, Slice groups, SlogAttr attr) {
    (void)ctx;
    if (attr.value.kind == SLOG_KIND_GROUP)
        return slog_int(S("bad"), 1);
    return remove_keys(keys(builtin_keys), a, groups, attr);
}

/* ------------------------------------------------------------ log valuers */

/* logValueName. The group's attributes live in the value itself, so that
 * what LogValue returns lasts as long as the value does. */
typedef struct LogValueName {
    Str first;
    Str last;
    SlogAttr attrs[2];
} LogValueName;

static SlogValue log_value_name(void *self) {
    LogValueName *n = self;
    n->attrs[0] = slog_string(S("first"), n->first);
    n->attrs[1] = slog_string(S("last"), n->last);
    return slog_group_value(attrs_of(n->attrs, 2));
}

static const SlogLogValuerVT log_value_name_vt = {NULL, log_value_name};

#define NAME_VALUER(f, l)                                                              \
    BURROW_ANY(TYPE_SLOG_LOG_VALUER,                                                   \
               (&(SlogLogValuer){&log_value_name_vt,                                   \
                                 &(LogValueName){.first = S(f), .last = S(l)}}))

/* replace, the LogValuer that hands back the value it holds. */
static SlogValue replace_log_value(void *self) {
    return *(SlogValue *)self;
}

static const SlogLogValuerVT replace_vt = {NULL, replace_log_value};

/* ------------------------------------------------------- TestDefaultHandle */

static Error capture_output(void *ctx, Str file, Int line, Str s) {
    (void)file;
    (void)line;
    BytesBuffer *b = ctx;
    bytes_buffer_reset(b);
    (void)bytes_buffer_write_string(b, s, NULL);
    return BURROW_NO_ERROR;
}

static SlogHandler dh_pre(SlogHandler h) {
    return with_attrs(h, ATTRS(INT("pre", 0)));
}

static SlogHandler dh_group(SlogHandler h) {
    return with_group(dh_pre(h), S("s"));
}

static SlogHandler dh_preformatted_groups(SlogHandler h) {
    h = with_group(with_attrs(h, ATTRS(INT("p1", 1))), S("s1"));
    return with_group(with_attrs(h, ATTRS(INT("p2", 2))), S("s2"));
}

static SlogHandler dh_two_groups(SlogHandler h) {
    h = with_group(with_attrs(h, ATTRS(INT("p1", 1))), S("s1"));
    return with_group(h, S("s2"));
}

typedef SlogHandler (*WithFunc)(SlogHandler h);

static void TestDefaultHandle(TestingT *t) {
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    struct {
        const char *name;
        WithFunc with;
        Slice attrs;
        const char *want;
    } tests[] = {
        {"no attrs", NULL, NO_ATTRS, "INFO message"},
        {"attrs", NULL, ATTRS(INT("a", 1), STRING("b", "two")),
         "INFO message a=1 b=two"},
        {"preformatted", dh_pre, ATTRS(INT("a", 1), STRING("b", "two")),
         "INFO message pre=0 a=1 b=two"},
        {"groups", NULL,
         ATTRS(INT("a", 1),
               GROUP("g", INT("b", 2), GROUP("h", INT("c", 3)), INT("d", 4)),
               INT("e", 5)),
         "INFO message a=1 g.b=2 g.h.c=3 g.d=4 e=5"},
        {"group", dh_group, ATTRS(INT("a", 1), STRING("b", "two")),
         "INFO message pre=0 s.a=1 s.b=two"},
        {"preformatted groups", dh_preformatted_groups,
         ATTRS(INT("a", 1), STRING("b", "two")),
         "INFO message p1=1 s1.p2=2 s1.s2.a=1 s1.s2.b=two"},
        {"two with-groups", dh_two_groups, ATTRS(INT("a", 1), STRING("b", "two")),
         "INFO message p1=1 s1.s2.a=1 s1.s2.b=two"},
    };
    for (size_t i = 0; i < sizeof tests / sizeof tests[0]; i++) {
        BytesBuffer got = BYTES_BUFFER(a);
        SlogHandler h = burrow__slog_new_default_handler(a, capture_output, &got);
        if (tests[i].with != NULL)
            h = tests[i].with(h);
        SlogRecord r = slog_new_record((Time){0}, SLOG_LEVEL_INFO, S("message"), 0);
        slog_record_add_attrs(&r, a, tests[i].attrs);
        Error err = handle(h, r);
        if (BURROW_FAILED(err)) {
            testing_t_errorf_v(t, "%s: %v", tests[i].name, err);
            continue;
        }
        if (!str_eq(buf_str(&got), str_from_cstr(tests[i].want)))
            testing_t_errorf_v(t, "%s:\ngot  %s\nwant %s", tests[i].name, buf_str(&got),
                               tests[i].want);
    }
    arena_free(&ar);
}

/* -------------------------------------------------------- TestConcurrentWrites */

typedef struct WriteJob {
    SlogHandler sub1;
    SlogHandler sub2;
    Int i;
    bool failed;
} WriteJob;

static void concurrent_write(void *env) {
    WriteJob *j = env;
    SlogRecord r1 =
        slog_new_record((Time){0}, SLOG_LEVEL_INFO, S("hello from sub1"), 0);
    r1.front[0] = INT("i", j->i);
    r1.nfront = 1;
    SlogRecord r2 =
        slog_new_record((Time){0}, SLOG_LEVEL_INFO, S("hello from sub2"), 0);
    r2.front[0] = INT("i", j->i);
    r2.nfront = 1;
    if (BURROW_FAILED(handle(j->sub1, r1)) || BURROW_FAILED(handle(j->sub2, r2)))
        j->failed = true;
}

static void TestConcurrentWrites(TestingT *t) {
    enum { COUNT = 1000 };
    for (int json = 0; json < 2; json++) {
        Arena ar;
        arena_init(&ar, NULL, 0);
        Alloc *a = arena_allocator(&ar);
        BytesBuffer buf = BYTES_BUFFER(a);
        SlogHandler h =
            json ? json_handler(a, &buf, NULL) : text_handler(a, &buf, NULL);
        SlogHandler sub1 = with_attrs(h, ATTRS(slog_bool(S("sub1"), true)));
        SlogHandler sub2 = with_attrs(h, ATTRS(slog_bool(S("sub2"), true)));
        WriteJob *jobs = mem_alloc(a, sizeof(WriteJob) * COUNT, _Alignof(WriteJob));
        SyncWaitGroup wg = {0};
        for (Int i = 0; i < COUNT; i++) {
            jobs[i] = (WriteJob){sub1, sub2, i, false};
            sync_wait_group_go(&wg, BURROW_FN(Func, concurrent_write, &jobs[i]));
        }
        sync_wait_group_wait(&wg);
        for (Int i = 0; i < COUNT; i++)
            if (jobs[i].failed)
                testing_t_errorf_v(t, "handle %d failed", i);
        for (int i = 1; i <= 2; i++) {
            Str want = fmt_sprintf_v(a, "hello from sub%d", i);
            Int n = strings_count(buf_str(&buf), want);
            if (n != COUNT)
                testing_t_errorf_v(t, "%s: want %d occurrences of %q, got %d",
                                   json ? "json" : "text", COUNT, want, n);
        }
        arena_free(&ar);
    }
}

/* ------------------------------------------------- TestJSONAndTextHandlers */

static SlogHandler w_pre(SlogHandler h) {
    return with_attrs(h, ATTRS(INT("pre", 3), STRING("x", "y")));
}

static SlogHandler w_pre_group(SlogHandler h) {
    return with_group(w_pre(h), S("s"));
}

static SlogHandler w_p1_s1_p2_s2_p3(SlogHandler h) {
    h = with_group(with_attrs(h, ATTRS(INT("p1", 1))), S("s1"));
    h = with_group(with_attrs(h, ATTRS(INT("p2", 2))), S("s2"));
    return with_attrs(h, ATTRS(INT("p3", 3)));
}

static SlogHandler w_p1_s1_s2(SlogHandler h) {
    h = with_group(with_attrs(h, ATTRS(INT("p1", 1))), S("s1"));
    return with_group(h, S("s2"));
}

static SlogHandler w_x_y(SlogHandler h) {
    return with_group(with_group(h, S("x")), S("y"));
}

static SlogHandler w_x_empty_y(SlogHandler h) {
    h = with_attrs(with_group(h, S("x")), ATTRS(EMPTY_GROUP("g")));
    return with_group(h, S("y"));
}

static SlogHandler w_x_a_y(SlogHandler h) {
    h = with_attrs(with_group(h, S("x")), ATTRS(INT("a", 1)));
    return with_group(h, S("y"));
}

static SlogHandler w_g(SlogHandler h) {
    return with_group(h, S("g"));
}

static SlogHandler w_g_a(SlogHandler h) {
    return with_attrs(with_group(h, S("g")), ATTRS(INT("a", 1)));
}

static SlogHandler w_g_a_h_b(SlogHandler h) {
    h = with_attrs(with_group(h, S("g")), ATTRS(INT("a", 1)));
    return with_attrs(with_group(h, S("h")), ATTRS(INT("b", 2)));
}

static SlogHandler w_g_a_n_h_b(SlogHandler h) {
    h = with_attrs(with_group(h, S("g")), ATTRS(INT("a", 1)));
    h = with_attrs(h, ATTRS(INT("n", 4)));
    return with_attrs(with_group(h, S("h")), ATTRS(INT("b", 2)));
}

static SlogHandler w_g_x_a_n_h_b(SlogHandler h) {
    h = with_attrs(with_group(h, S("g")), ATTRS(INT("x", 0)));
    h = with_attrs(h, ATTRS(INT("a", 1)));
    h = with_attrs(h, ATTRS(INT("n", 4)));
    return with_attrs(with_group(h, S("h")), ATTRS(INT("b", 2)));
}

typedef struct HandlerCase {
    const char *name;
    SlogReplaceAttrFunc replace;
    void *replace_ctx;
    bool add_source;
    WithFunc with;
    Slice attrs;
    const char *want_text;
    const char *want_json;
} HandlerCase;

#define BASE_ATTRS STRING("a", "one"), INT("b", 2), slog_any(S(""), (Any){NULL, NULL})
#define STR_ANY(s) BURROW_ANY_VAL(TYPE_STRING, Str, S(s))
#define BYTES_ANY(t, ...)                                                              \
    BURROW_ANY_VAL(t, Slice,                                                           \
                   slice_from((Byte[]){__VA_ARGS__}, BURROW__FMT_NARGS(__VA_ARGS__),   \
                              BURROW__FMT_NARGS(__VA_ARGS__), TYPE_BYTE))

static void TestJSONAndTextHandlers(TestingT *t) {
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    void *tl = keys(time_level_keys);
    HandlerCase tests[] = {
        {"basic", NULL, NULL, false, NULL, ATTRS(BASE_ATTRS),
         "time=2000-01-02T03:04:05.000Z level=INFO msg=message a=one b=2",
         "{\"time\":\"2000-01-02T03:04:05Z\",\"level\":\"INFO\",\"msg\":\"message\","
         "\"a\":\"one\",\"b\":2}"},
        {"empty key", NULL, NULL, false, NULL,
         ATTRS(BASE_ATTRS, slog_any(S(""), STR_ANY("v"))),
         "time=2000-01-02T03:04:05.000Z level=INFO msg=message a=one b=2 \"\"=v",
         "{\"time\":\"2000-01-02T03:04:05Z\",\"level\":\"INFO\",\"msg\":\"message\","
         "\"a\":\"one\",\"b\":2,\"\":\"v\"}"},
        {"cap keys", upper_case_key, NULL, false, NULL, ATTRS(BASE_ATTRS),
         "TIME=2000-01-02T03:04:05.000Z LEVEL=INFO MSG=message A=one B=2",
         "{\"TIME\":\"2000-01-02T03:04:05Z\",\"LEVEL\":\"INFO\",\"MSG\":\"message\","
         "\"A\":\"one\",\"B\":2}"},
        {"remove all", remove_all, NULL, false, NULL, ATTRS(BASE_ATTRS), "", "{}"},
        {"preformatted", NULL, NULL, false, w_pre, ATTRS(BASE_ATTRS),
         "time=2000-01-02T03:04:05.000Z level=INFO msg=message pre=3 x=y a=one b=2",
         "{\"time\":\"2000-01-02T03:04:05Z\",\"level\":\"INFO\",\"msg\":\"message\","
         "\"pre\":3,\"x\":\"y\",\"a\":\"one\",\"b\":2}"},
        {"preformatted cap keys", upper_case_key, NULL, false, w_pre, ATTRS(BASE_ATTRS),
         "TIME=2000-01-02T03:04:05.000Z LEVEL=INFO MSG=message PRE=3 X=y A=one B=2",
         "{\"TIME\":\"2000-01-02T03:04:05Z\",\"LEVEL\":\"INFO\",\"MSG\":\"message\","
         "\"PRE\":3,\"X\":\"y\",\"A\":\"one\",\"B\":2}"},
        {"preformatted remove all", remove_all, NULL, false, w_pre, ATTRS(BASE_ATTRS),
         "", "{}"},
        {"remove built-in", remove_keys, keys(builtin_keys), false, NULL,
         ATTRS(BASE_ATTRS), "a=one b=2", "{\"a\":\"one\",\"b\":2}"},
        {"preformatted remove built-in", remove_keys, keys(builtin_keys), false, w_pre,
         ATTRS(BASE_ATTRS), "pre=3 x=y a=one b=2",
         "{\"pre\":3,\"x\":\"y\",\"a\":\"one\",\"b\":2}"},
        {"groups", remove_keys, tl, false, NULL,
         ATTRS(INT("a", 1),
               GROUP("g", INT("b", 2), GROUP("h", INT("c", 3)), INT("d", 4)),
               INT("e", 5)),
         "msg=message a=1 g.b=2 g.h.c=3 g.d=4 e=5",
         "{\"msg\":\"message\",\"a\":1,\"g\":{\"b\":2,\"h\":{\"c\":3},\"d\":4},\"e\":"
         "5}"},
        {"empty group", remove_keys, tl, false, NULL,
         ATTRS(EMPTY_GROUP("g"), GROUP("h", INT("a", 1))), "msg=message h.a=1",
         "{\"msg\":\"message\",\"h\":{\"a\":1}}"},
        {"nested empty group", remove_keys, tl, false, NULL,
         ATTRS(GROUP("g", GROUP("h", EMPTY_GROUP("i"), EMPTY_GROUP("j")))),
         "msg=message", "{\"msg\":\"message\"}"},
        {"nested non-empty group", remove_keys, tl, false, NULL,
         ATTRS(GROUP("g", GROUP("h", EMPTY_GROUP("i"), GROUP("j", INT("a", 1))))),
         "msg=message g.h.j.a=1",
         "{\"msg\":\"message\",\"g\":{\"h\":{\"j\":{\"a\":1}}}}"},
        {"escapes", remove_keys, tl, false, NULL,
         ATTRS(STRING("a b", "x\t\n\000y"),
               GROUP(" b.c=\"\\x2E\t", STRING("d=e", "f.g\""), INT("m.d", 1))),
         "msg=message \"a b\"=\"x\\t\\n\\x00y\" \" "
         "b.c=\\\"\\\\x2E\\t.d=e\"=\"f.g\\\"\" "
         "\" b.c=\\\"\\\\x2E\\t.m.d\"=1",
         "{\"msg\":\"message\",\"a b\":\"x\\t\\n\\u0000y\",\" "
         "b.c=\\\"\\\\x2E\\t\":{\"d=e\":"
         "\"f.g\\\"\",\"m.d\":1}}"},
        {"LogValuer", remove_keys, tl, false, NULL,
         ATTRS(INT("a", 1), slog_any(S("name"), NAME_VALUER("Ren", "Hoek")),
               INT("b", 2)),
         "msg=message a=1 name.first=Ren name.last=Hoek b=2",
         "{\"msg\":\"message\",\"a\":1,\"name\":{\"first\":\"Ren\",\"last\":\"Hoek\"},"
         "\"b\":2}"},
        /* Resolution when there is no ReplaceAttr function. */
        {"resolve", NULL, NULL, false, NULL,
         ATTRS(slog_any(S(""),
                        BURROW_ANY(TYPE_SLOG_LOG_VALUER,
                                   (&(SlogLogValuer){&replace_vt, &(SlogValue){0}}))),
               slog_any(S("name"), NAME_VALUER("Ren", "Hoek"))),
         "time=2000-01-02T03:04:05.000Z level=INFO msg=message name.first=Ren "
         "name.last=Hoek",
         "{\"time\":\"2000-01-02T03:04:05Z\",\"level\":\"INFO\",\"msg\":\"message\","
         "\"name\":{\"first\":\"Ren\",\"last\":\"Hoek\"}}"},
        {"with-group", remove_keys, tl, false, w_pre_group, ATTRS(BASE_ATTRS),
         "msg=message pre=3 x=y s.a=one s.b=2",
         "{\"msg\":\"message\",\"pre\":3,\"x\":\"y\",\"s\":{\"a\":\"one\",\"b\":2}}"},
        {"preformatted with-groups", remove_keys, tl, false, w_p1_s1_p2_s2_p3,
         ATTRS(BASE_ATTRS), "msg=message p1=1 s1.p2=2 s1.s2.p3=3 s1.s2.a=one s1.s2.b=2",
         "{\"msg\":\"message\",\"p1\":1,\"s1\":{\"p2\":2,\"s2\":{\"p3\":3,\"a\":"
         "\"one\","
         "\"b\":2}}}"},
        {"two with-groups", remove_keys, tl, false, w_p1_s1_s2, ATTRS(BASE_ATTRS),
         "msg=message p1=1 s1.s2.a=one s1.s2.b=2",
         "{\"msg\":\"message\",\"p1\":1,\"s1\":{\"s2\":{\"a\":\"one\",\"b\":2}}}"},
        {"empty with-groups", remove_keys, tl, false, w_x_y, NO_ATTRS, "msg=message",
         "{\"msg\":\"message\"}"},
        {"empty with-groups, no non-empty attrs", remove_keys, tl, false, w_x_empty_y,
         NO_ATTRS, "msg=message", "{\"msg\":\"message\"}"},
        {"one empty with-group", remove_keys, tl, false, w_x_a_y,
         ATTRS(GROUP("g", EMPTY_GROUP("h"))), "msg=message x.a=1",
         "{\"msg\":\"message\",\"x\":{\"a\":1}}"},
        {"GroupValue as Attr value", remove_keys, tl, false, NULL,
         ATTRS(
             ((SlogAttr){S("v"), slog_any_value(BURROW_ANY_VAL(
                                     TYPE_SLOG_VALUE, SlogValue, slog_int_value(3)))})),
         "msg=message v=3", "{\"msg\":\"message\",\"v\":3}"},
        {"byte slice", remove_keys, tl, false, NULL,
         ATTRS(slog_any(S("bs"), BYTES_ANY(TYPE_BYTES, 1, 2, 3, 4))),
         "msg=message bs=\"\\x01\\x02\\x03\\x04\"",
         "{\"msg\":\"message\",\"bs\":\"AQIDBA==\"}"},
        {"json.RawMessage", remove_keys, tl, false, NULL,
         ATTRS(slog_any(S("bs"), BYTES_ANY(TYPE_JSON_RAW_MESSAGE, '1', '2', '3', '4'))),
         "msg=message bs=\"1234\"", "{\"msg\":\"message\",\"bs\":1234}"},
        {"inline group", remove_keys, tl, false, NULL,
         ATTRS(INT("a", 1), GROUP("", INT("b", 2), INT("c", 3)), INT("d", 4)),
         "msg=message a=1 b=2 c=3 d=4",
         "{\"msg\":\"message\",\"a\":1,\"b\":2,\"c\":3,\"d\":4}"},
        {"Source", base_source, NULL, true, NULL, NO_ATTRS,
         "source=slog_handler_test.c:$LINE msg=message",
         "{\"source\":{\"function\":\"TestJSONAndTextHandlers\",\"file\":"
         "\"slog_handler_test.c\",\"line\":$LINE},\"msg\":\"message\"}"},
        {"replace built-in with group", time_as_group, NULL, false, NULL, NO_ATTRS,
         "time.mins=3 time.secs=2 msg=message",
         "{\"time\":{\"mins\":3,\"secs\":2},\"msg\":\"message\"}"},
        {"replace empty", remove_all, NULL, false, NULL, ATTRS(GROUP("g", INT("a", 1))),
         "", "{}"},
        {"replace empty 1", remove_all, NULL, false, w_g_a,
         ATTRS(GROUP("h", INT("b", 2))), "", "{}"},
        {"replace empty 2", remove_all, NULL, false, w_g_a_h_b,
         ATTRS(GROUP("i", INT("c", 3))), "", "{}"},
        {"replace empty 3", remove_all, NULL, false, w_g, ATTRS(INT("a", 1)), "", "{}"},
        {"replace empty inline", remove_all, NULL, false, w_g_a_h_b,
         ATTRS(GROUP("", INT("c", 3))), "", "{}"},
        {"replace partial empty attrs 1", remove_keys, keys(partial_a_keys), false,
         w_g_a_h_b, ATTRS(GROUP("i", INT("c", 3))), "g.h.b=2 g.h.i.c=3",
         "{\"g\":{\"h\":{\"b\":2,\"i\":{\"c\":3}}}}"},
        {"replace partial empty attrs 2", remove_keys, keys(partial_ab_keys), false,
         w_g_a_n_h_b, ATTRS(GROUP("i", INT("c", 3))), "g.n=4 g.h.i.c=3",
         "{\"g\":{\"n\":4,\"h\":{\"i\":{\"c\":3}}}}"},
        {"replace partial empty attrs 3", remove_keys, keys(partial_ac_keys), false,
         w_g_x_a_n_h_b, ATTRS(GROUP("i", INT("c", 3))), "g.x=0 g.n=4 g.h.b=2",
         "{\"g\":{\"x\":0,\"n\":4,\"h\":{\"b\":2}}}"},
        {"replace resolved group", groups_bad, NULL, false, NULL,
         ATTRS(slog_any(S("name"), NAME_VALUER("Perry", "Platypus"))),
         "name.first=Perry name.last=Platypus",
         "{\"name\":{\"first\":\"Perry\",\"last\":\"Platypus\"}}"},
        {"group and key (or both) needs quoting", remove_keys, tl, false, NULL,
         ATTRS(GROUP("prefix", STRING(" needs quoting ", "v"),
                     STRING("NotNeedsQuoting", "v")),
               GROUP("prefix needs quoting", STRING(" needs quoting ", "v"),
                     STRING("NotNeedsQuoting", "v"))),
         "msg=message \"prefix. needs quoting \"=v prefix.NotNeedsQuoting=v \"prefix "
         "needs "
         "quoting. needs quoting \"=v \"prefix needs quoting.NotNeedsQuoting\"=v",
         "{\"msg\":\"message\",\"prefix\":{\" needs quoting "
         "\":\"v\",\"NotNeedsQuoting\":"
         "\"v\"},\"prefix needs quoting\":{\" needs quoting "
         "\":\"v\",\"NotNeedsQuoting\":"
         "\"v\"}}"},
    };
    for (size_t i = 0; i < sizeof tests / sizeof tests[0]; i++) {
        const HandlerCase *tc = &tests[i];
        SlogRecord r = slog_new_record(test_time(), SLOG_LEVEL_INFO, S("message"), 0);
        r.source = (SlogSource){str_from_cstr(__func__), S(__FILE__), __LINE__};
        Str line = strconv_itoa(a, r.source.line);
        slog_record_add_attrs(&r, a, tc->attrs);
        SlogHandlerOptions opts = {0};
        opts.add_source = tc->add_source;
        opts.replace_attr = tc->replace;
        opts.replace_attr_ctx = tc->replace_ctx;
        BytesBuffer buf = BYTES_BUFFER(a);
        struct {
            const char *name;
            SlogHandler h;
            const char *want;
        } handlers[] = {
            {"text", text_handler(a, &buf, &opts), tc->want_text},
            {"json", json_handler(a, &buf, &opts), tc->want_json},
        };
        for (size_t j = 0; j < 2; j++) {
            SlogHandler h = handlers[j].h;
            if (tc->with != NULL)
                h = tc->with(h);
            bytes_buffer_reset(&buf);
            Error err = handle(h, r);
            if (BURROW_FAILED(err)) {
                testing_t_errorf_v(t, "%s/%s: %v", tc->name, handlers[j].name, err);
                continue;
            }
            Str want = strings_replace_all(a, str_from_cstr(handlers[j].want),
                                           S("$LINE"), line);
            Str got = trim_newline(buf_str(&buf));
            if (!str_eq(got, want))
                testing_t_errorf_v(t, "%s/%s:\ngot  %s\nwant %s", tc->name,
                                   handlers[j].name, got, want);
        }
    }
    arena_free(&ar);
}

/* ------------------------------------------------------ TestHandlerEnabled */

static void TestHandlerEnabled(TestingT *t) {
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    SlogLevelVar zero = {0};
    SlogLevelVar warn = {0};
    slog_level_var_set(&warn, SLOG_LEVEL_WARN);
    SlogLevelVar debug = {0};
    slog_level_var_set(&debug, SLOG_LEVEL_DEBUG);
    struct {
        const char *name;
        SlogLeveler leveler;
        bool want;
    } tests[] = {
        {"nil", {NULL, NULL}, true},
        {"LevelWarn", slog_level_as_leveler(SLOG_LEVEL_WARN), false},
        {"&LevelVar{}", slog_level_var_as_leveler(&zero), true}, /* defaults to Info */
        {"levelVar(LevelWarn)", slog_level_var_as_leveler(&warn), false},
        {"LevelDebug", slog_level_as_leveler(SLOG_LEVEL_DEBUG), true},
        {"levelVar(LevelDebug)", slog_level_var_as_leveler(&debug), true},
    };
    for (size_t i = 0; i < sizeof tests / sizeof tests[0]; i++) {
        SlogHandlerOptions opts = {0};
        opts.level = tests[i].leveler;
        SlogTextHandler *h = slog_new_text_handler(a, io_discard, &opts);
        bool got = slog_text_handler_enabled(h, context_background(), SLOG_LEVEL_INFO);
        if (got != tests[i].want)
            testing_t_errorf_v(t, "%s: got %t, want %t", tests[i].name, got,
                               tests[i].want);
    }
    arena_free(&ar);
}

static void TestJSONAndTextHandlersWithUnavailableSource(TestingT *t) {
    /* Verify that a record with no source does not cause a panic, and that
     * the source is left out. */
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    BytesBuffer buf = BYTES_BUFFER(a);
    SlogHandlerOptions opts = {0};
    opts.replace_attr = remove_keys;
    opts.replace_attr_ctx = keys(level_key);
    opts.add_source = true;
    struct {
        const char *name;
        SlogHandler h;
        const char *want;
    } tests[] = {
        {"text", text_handler(a, &buf, &opts), "msg=message"},
        {"json", json_handler(a, &buf, &opts), "{\"msg\":\"message\"}"},
    };
    for (size_t i = 0; i < 2; i++) {
        bytes_buffer_reset(&buf);
        SlogRecord r = slog_new_record((Time){0}, SLOG_LEVEL_INFO, S("message"), 0);
        Error err = handle(tests[i].h, r);
        if (BURROW_FAILED(err)) {
            testing_t_errorf_v(t, "%s: %v", tests[i].name, err);
            continue;
        }
        Str got = strings_trim_space(buf_str(&buf));
        if (!str_eq(got, str_from_cstr(tests[i].want)))
            testing_t_errorf_v(t, "%s:\ngot  %s\nwant %s", tests[i].name, got,
                               tests[i].want);
    }
    arena_free(&ar);
}

static void TestSecondWith(TestingT *t) {
    /* Verify that a second call to Logger.With does not corrupt the
     * original. */
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    BytesBuffer buf = BYTES_BUFFER(a);
    SlogHandlerOptions opts = {0};
    opts.replace_attr = remove_keys;
    opts.replace_attr_ctx = keys(time_key);
    SlogLogger *base = slog_new(a, text_handler(a, &buf, &opts));
    SlogLogger *logger =
        slog_logger_with_v(base, STRING("app", "playground"), STRING("role", "tester"),
                           INT("data_version", 2));
    SlogLogger *app_logger = slog_logger_with_v(logger, "type", "log");
    SlogLogger *other = slog_logger_with_v(logger, "type", "metric");
    slog_logger_info_v(app_logger, "foo");
    Str got = strings_trim_space(buf_str(&buf));
    const char *want = "level=INFO msg=foo app=playground role=tester data_version=2 "
                       "type=log";
    if (!str_eq(got, str_from_cstr(want)))
        testing_t_errorf_v(t, "\ngot  %s\nwant %s", got, want);
    slog_logger_free(other);
    slog_logger_free(app_logger);
    slog_logger_free(logger);
    slog_logger_free(base);
    arena_free(&ar);
}

/* What TestReplaceAttrGroups records of each call to its ReplaceAttr. */
typedef struct GroupCall {
    Str groups;
    Str key;
    Str val;
} GroupCall;

typedef struct GroupCalls {
    Alloc *a;
    GroupCall got[16];
    Int n;
} GroupCalls;

static SlogAttr record_groups(void *ctx, Alloc *a, Slice groups, SlogAttr attr) {
    (void)a;
    GroupCalls *c = ctx;
    Str v = slog_value_string(attr.value, c->a);
    if (str_eq(attr.key, SLOG_TIME_KEY))
        v = S("<now>");
    if (c->n < 16)
        c->got[c->n++] = (GroupCall){strings_join(c->a, groups, S(",")), attr.key, v};
    return attr;
}

static void TestReplaceAttrGroups(TestingT *t) {
    /* Verify that ReplaceAttr is called with the correct groups. */
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    GroupCalls calls = {0};
    calls.a = a;
    SlogHandlerOptions opts = {0};
    opts.replace_attr = record_groups;
    opts.replace_attr_ctx = &calls;
    SlogTextHandler *h = slog_new_text_handler(a, io_discard, &opts);
    SlogLogger *l = slog_new(a, slog_text_handler_as_handler(h));
    l = slog_logger_with_v(l, INT("a", 1));
    l = slog_logger_with_group(l, S("g1"));
    l = slog_logger_with_v(l, INT("b", 2));
    l = slog_logger_with_group(l, S("g2"));
    l = slog_logger_with_v(l, INT("c", 3), slog_group_v("g3", INT("d", 4)),
                           INT("e", 5));
    slog_logger_info_v(l, "m", INT("f", 6), slog_group_v("g4", INT("h", 7)),
                       INT("i", 8));

    static const struct {
        const char *groups;
        const char *key;
        const char *val;
    } want[] = {
        {"", "a", "1"},         {"g1", "b", "2"},    {"g1,g2", "c", "3"},
        {"g1,g2,g3", "d", "4"}, {"g1,g2", "e", "5"}, {"", "time", "<now>"},
        {"", "level", "INFO"},  {"", "msg", "m"},    {"g1,g2", "f", "6"},
        {"g1,g2,g4", "h", "7"}, {"g1,g2", "i", "8"},
    };
    Int nwant = (Int)(sizeof want / sizeof want[0]);
    bool same = calls.n == nwant;
    for (Int i = 0; same && i < nwant; i++)
        same = str_eq(calls.got[i].groups, str_from_cstr(want[i].groups)) &&
               str_eq(calls.got[i].key, str_from_cstr(want[i].key)) &&
               str_eq(calls.got[i].val, str_from_cstr(want[i].val));
    if (!same) {
        for (Int i = 0; i < calls.n; i++)
            testing_t_errorf_v(t, "got {%s %s %s}", calls.got[i].groups,
                               calls.got[i].key, calls.got[i].val);
        testing_t_errorf_v(t, "want %d calls, got %d", nwant, calls.n);
    }
    arena_free(&ar);
}

static void TestWriteTimeRFC3339(TestingT *t) {
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    Time times[] = {
        time_date(2000, TIME_JANUARY, 2, 3, 4, 5, 0, time_utc_loc),
        time_date(2000, TIME_JANUARY, 2, 3, 4, 5, 400, time_local_loc),
        time_date(2000, TIME_NOVEMBER, 12, 3, 4, 500, 50000000, time_utc_loc),
    };
    SlogHandlerOptions opts = {0};
    opts.replace_attr = remove_keys;
    opts.replace_attr_ctx = keys(level_msg_keys);
    BytesBuffer buf = BYTES_BUFFER(a);
    SlogHandler h = text_handler(a, &buf, &opts);
    for (size_t i = 0; i < sizeof times / sizeof times[0]; i++) {
        bytes_buffer_reset(&buf);
        SlogRecord r = slog_new_record(times[i], SLOG_LEVEL_INFO, S(""), 0);
        (void)handle(h, r);
        Str got = trim_newline(buf_str(&buf));
        Str want = fmt_sprintf_v(
            a, "time=%s", time_format(times[i], a, S("2006-01-02T15:04:05.000Z07:00")));
        if (!str_eq(got, want))
            testing_t_errorf_v(t, "got %s, want %s", got, want);
    }
    arena_free(&ar);
}

static void TestDiscardHandler(TestingT *t) {
    (void)t;
    /* Just ensure nothing panics during normal usage. */
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    Context ctx = context_background();
    SlogLogger *l = slog_new(a, slog_discard_handler);
    slog_logger_info_v(l, "msg", "a", 1, "b", 2);
    slog_logger_debug_v(l, "bg", INT("a", 1), "b", 2);
    slog_logger_warn_v(l, "w", slog_duration(S("dur"), 3 * TIME_SECOND));
    slog_logger_error_v(l, "bad", "a", 1);
    slog_logger_log_v(l, ctx, SLOG_LEVEL_WARN + 1, "w", INT("a", 1),
                      STRING("b", "two"));
    slog_logger_log_attrs_v(l, ctx, SLOG_LEVEL_INFO + 1, "a b c", INT("a", 1),
                            STRING("b", "two"));
    slog_logger_info_v(l, "info", "a",
                       BURROW_ANY_VAL(TYPE_OF(AttrSlice), Slice, ATTRS(INT("i", 1))));
    slog_logger_info_v(l, "info", "a", slog_group_value(ATTRS(INT("i", 1))));
    slog_logger_free(l);
    arena_free(&ar);
}

/* --------------------------------------------------------- text handler */

/* for testing fmt.Sprint */
#define NAME_FIELDS(F, T)                                                              \
    F(T, Str, First, "")                                                               \
    F(T, Str, Last, "")
BURROW_STRUCT_DECL(Name, NAME_FIELDS);

/* n.Last + ", " + n.First. String methods get no allocator, and the tests
 * run one at a time, so the result goes in a buffer of its own. */
static Str name_string(Name *n) {
    static Byte buf[64];
    Int len = 0;
    Str parts[3] = {n->Last, S(", "), n->First};
    for (int i = 0; i < 3; i++)
        for (Int j = 0; j < parts[i].len && len < (Int)sizeof buf; j++)
            buf[len++] = parts[i].p[j];
    return str_from_bytes(buf, len);
}

#define NAME_SIG_String(IN, OUT) OUT(Str)
#define NAME_METHODS(M, T) M(T, String, name_string, NAME_SIG_String)
BURROW_STRUCT_DEFINE_METHODS(Name, NAME_FIELDS, NAME_METHODS);

/* for testing a struct through a pointer, &{A:1 b:2} */
#define AB_FIELDS(F, T)                                                                \
    F(T, Int, A, "")                                                                   \
    F(T, Int, b, "")
BURROW_STRUCT(AB, AB_FIELDS);
BURROW_PTR_TYPE(ABPtr, AB);

/* for testing TextMarshaler */
#define TEXT_FIELDS(F, T) F(T, Str, s, "")
BURROW_STRUCT_DECL(Text, TEXT_FIELDS);

static Str text_string(Text *x) {
    return x->s; /* should be ignored */
}

static Slice text_marshal_text(Text *x, Alloc *a, Error *err) {
    if (x->s.len == 0) {
        BURROW_OUT(err, errors_new(error_allocator(), S("text: empty string")));
        return slice_nil(TYPE_BYTE);
    }
    BURROW_OUT(err, BURROW_NO_ERROR);
    return fmt_appendf_v(a, slice_nil(TYPE_BYTE), "text{%q}", x->s);
}

#define TEXT_SIG_String(IN, OUT) OUT(Str)
#define TEXT_METHODS(M, T)                                                             \
    M(T, MarshalText, text_marshal_text, ENCODING_SIG_MARSHAL_TEXT)                    \
    M(T, String, text_string, TEXT_SIG_String)
BURROW_STRUCT_DEFINE_METHODS(Text, TEXT_FIELDS, TEXT_METHODS);

/* for testing TextAppender */
#define TEXT_APPEND_FIELDS(F, T) F(T, Str, s, "")
BURROW_STRUCT_DECL(TextAppend, TEXT_APPEND_FIELDS);

static Str text_append_string(TextAppend *x) {
    return x->s;
}

static Slice text_append_marshal_text(TextAppend *x, Alloc *a, Error *err) {
    BURROW_OUT(err, BURROW_NO_ERROR);
    return slice_append(a, slice_nil(TYPE_BYTE), x->s.p, x->s.len);
}

static Slice text_append_append_text(TextAppend *x, Alloc *a, Slice b, Error *err) {
    if (x->s.len == 0) {
        BURROW_OUT(err, errors_new(error_allocator(), S("textAppend: empty string")));
        return slice_nil(TYPE_BYTE);
    }
    BURROW_OUT(err, BURROW_NO_ERROR);
    b = slice_append(a, b, "textAppend{\"", 12);
    b = slice_append(a, b, x->s.p, x->s.len);
    return slice_append(a, b, "\"}", 2);
}

#define TEXT_APPEND_SIG_String(IN, OUT) OUT(Str)
#define TEXT_APPEND_METHODS(M, T)                                                      \
    M(T, AppendText, text_append_append_text, ENCODING_SIG_APPEND_TEXT)                \
    M(T, MarshalText, text_append_marshal_text, ENCODING_SIG_MARSHAL_TEXT)             \
    M(T, String, text_append_string, TEXT_APPEND_SIG_String)
BURROW_STRUCT_DEFINE_METHODS(TextAppend, TEXT_APPEND_FIELDS, TEXT_APPEND_METHODS);

/* marshalerASCII, the bytes 0 to 127. */
static Str marshaler_ascii(void) {
    static Byte b[128];
    for (int i = 0; i < 128; i++)
        b[i] = (Byte)i;
    return str_from_bytes(b, 128);
}

static void TestTextHandler(TestingT *t) {
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    Str ascii = marshaler_ascii();
    ABPtr ab = &(AB){1, 2};
    struct {
        const char *name;
        SlogAttr attr;
        Str want_key;
        Str want_val;
    } tests[] = {
        {"unquoted", INT("a", 1), S("a"), S("1")},
        {"quoted", STRING("x = y", "qu\"o"), S("\"x = y\""), S("\"qu\\\"o\"")},
        {"String method",
         slog_any(S("name"), BURROW_ANY(TYPE_OF(Name), (&(Name){S("Ren"), S("Hoek")}))),
         S("name"), S("\"Hoek, Ren\"")},
        {"struct", slog_any(S("x"), BURROW_ANY(TYPE_OF(ABPtr), &ab)), S("x"),
         S("\"&{A:1 b:2}\"")},
        {"TextMarshaler",
         slog_any(S("t"), BURROW_ANY(TYPE_OF(Text), &(Text){S("abc")})), S("t"),
         S("\"text{\\\"abc\\\"}\"")},
        {"TextMarshaler escapes",
         slog_any(S("t"), BURROW_ANY(TYPE_OF(Text), &(Text){ascii})), S("t"),
         strconv_quote(a, fmt_sprintf_v(a, "text{%q}", ascii))},
        {"TextMarshaler error",
         slog_any(S("t"), BURROW_ANY(TYPE_OF(Text), &(Text){S("")})), S("t"),
         S("\"!ERROR:text: empty string\"")},
        {"TextAppender",
         slog_any(S("t"), BURROW_ANY(TYPE_OF(TextAppend), &(TextAppend){S("abc")})),
         S("t"), S("\"textAppend{\\\"abc\\\"}\"")},
        {"TextAppender error",
         slog_any(S("t"), BURROW_ANY(TYPE_OF(TextAppend), &(TextAppend){S("")})),
         S("t"), S("\"!ERROR:textAppend: empty string\"")},
        {"nil value", slog_any(S("a"), (Any){NULL, NULL}), S("a"), S("<nil>")},
    };
    for (size_t i = 0; i < sizeof tests / sizeof tests[0]; i++) {
        for (int upper = 0; upper < 2; upper++) {
            SlogHandlerOptions opts = {0};
            if (upper)
                opts.replace_attr = upper_case_key;
            const char *prefix =
                upper ? "TIME=2000-01-02T03:04:05.000Z LEVEL=INFO MSG=\"a message\""
                      : "time=2000-01-02T03:04:05.000Z level=INFO msg=\"a message\"";
            BytesBuffer buf = BYTES_BUFFER(a);
            SlogHandler h = text_handler(a, &buf, &opts);
            SlogRecord r =
                slog_new_record(test_time(), SLOG_LEVEL_INFO, S("a message"), 0);
            slog_record_add_attrs(&r, a, ATTRS(tests[i].attr));
            Error err = handle(h, r);
            if (BURROW_FAILED(err)) {
                testing_t_errorf_v(t, "%s: %v", tests[i].name, err);
                continue;
            }
            Str got = trim_newline(buf_str(&buf));
            Str key =
                upper ? strings_to_upper(a, tests[i].want_key) : tests[i].want_key;
            Str want = fmt_sprintf_v(a, "%s %s=%s", prefix, key, tests[i].want_val);
            if (!str_eq(got, want))
                testing_t_errorf_v(t, "%s/%s:\ngot  %s\nwant %s", tests[i].name,
                                   upper ? "replace" : "none", got, want);
        }
    }
    arena_free(&ar);
}

static void TestTextHandlerPreformatted(TestingT *t) {
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    BytesBuffer buf = BYTES_BUFFER(a);
    SlogHandler h = text_handler(a, &buf, NULL);
    h = with_attrs(
        h, ATTRS(slog_duration(S("dur"), TIME_MINUTE), slog_bool(S("b"), true)));
    /* Also test omitting time. */
    SlogRecord r = slog_new_record((Time){0}, 0 /* 0 Level is INFO */, S("m"), 0);
    slog_record_add_attrs(&r, a, ATTRS(INT("a", 1)));
    Error err = handle(h, r);
    if (BURROW_FAILED(err))
        testing_t_errorf_v(t, "%v", err);
    Str got = trim_newline(buf_str(&buf));
    const char *want = "level=INFO msg=m dur=1m0s b=true a=1";
    if (!str_eq(got, str_from_cstr(want)))
        testing_t_errorf_v(t, "got %s, want %s", got, want);
    arena_free(&ar);
}

/* needsQuoting, seen from outside: whether the text handler quotes a string
 * value. */
static void TestNeedsQuoting(TestingT *t) {
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    struct {
        Str in;
        bool want;
    } tests[] = {
        {S(""), true},
        {S("ab"), false},
        {S("a=b"), true},
        {S("\"ab\""), true},
        {S("\a\b"), true},
        {S("a\tb"), true},
        {S("\xc2\xb5\xc3\xa5\xcf\x80"), false},
        {S("a b"), true},
        {S("badutf8\xF6"), true},
    };
    SlogHandlerOptions opts = {0};
    opts.replace_attr = remove_keys;
    opts.replace_attr_ctx = keys(builtin_keys);
    BytesBuffer buf = BYTES_BUFFER(a);
    SlogHandler h = text_handler(a, &buf, &opts);
    for (size_t i = 0; i < sizeof tests / sizeof tests[0]; i++) {
        bytes_buffer_reset(&buf);
        SlogRecord r = slog_new_record((Time){0}, SLOG_LEVEL_INFO, S(""), 0);
        slog_record_add_attrs(&r, a, ATTRS(slog_string(S("v"), tests[i].in)));
        (void)handle(h, r);
        Str got = buf_str(&buf);
        bool quoted = got.len > 2 && got.p[2] == '"';
        if (quoted != tests[i].want)
            testing_t_errorf_v(t, "%q: got %t, want %t", tests[i].in, quoted,
                               tests[i].want);
    }
    arena_free(&ar);
}

/* --------------------------------------------------------- JSON handler */

static void TestJSONHandler(TestingT *t) {
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    struct {
        const char *name;
        SlogReplaceAttrFunc replace;
        const char *want;
    } tests[] = {
        {"none", NULL,
         "{\"time\":\"2000-01-02T03:04:05Z\",\"level\":\"INFO\",\"msg\":\"m\",\"a\":1,"
         "\"m\":{\"b\":2}}"},
        {"replace", upper_case_key,
         "{\"TIME\":\"2000-01-02T03:04:05Z\",\"LEVEL\":\"INFO\",\"MSG\":\"m\",\"A\":1,"
         "\"M\":{\"b\":2}}"},
    };
    StrIntMap m = map_make(a, TYPE_STRING, TYPE_INT, 0);
    BURROW_MAP_SET(Str, Int, m, S("b"), 2);
    for (size_t i = 0; i < 2; i++) {
        SlogHandlerOptions opts = {0};
        opts.replace_attr = tests[i].replace;
        BytesBuffer buf = BYTES_BUFFER(a);
        SlogHandler h = json_handler(a, &buf, &opts);
        SlogRecord r = slog_new_record(test_time(), SLOG_LEVEL_INFO, S("m"), 0);
        slog_record_add_attrs(
            &r, a,
            ATTRS(INT("a", 1), slog_any(S("m"), BURROW_ANY(TYPE_OF(StrIntMap), &m))));
        Error err = handle(h, r);
        if (BURROW_FAILED(err)) {
            testing_t_errorf_v(t, "%s: %v", tests[i].name, err);
            continue;
        }
        Str got = trim_newline(buf_str(&buf));
        if (!str_eq(got, str_from_cstr(tests[i].want)))
            testing_t_errorf_v(t, "%s:\ngot  %s\nwant %s", tests[i].name, got,
                               tests[i].want);
    }
    arena_free(&ar);
}

/* for testing json.Marshaler */
#define JSON_MARSHALER_FIELDS(F, T) F(T, Str, s, "")
BURROW_STRUCT_DECL(JsonMarshaler, JSON_MARSHALER_FIELDS);

static Str json_marshaler_string(JsonMarshaler *j) {
    return j->s; /* should be ignored */
}

static Slice json_marshaler_marshal_json(JsonMarshaler *j, Alloc *a, Error *err) {
    if (j->s.len == 0) {
        BURROW_OUT(err, errors_new(error_allocator(), S("json: empty string")));
        return slice_nil(TYPE_BYTE);
    }
    Slice ss = slice_from(&j->s, 1, 1, TYPE_STRING);
    return json_marshal(a, BURROW_ANY(TYPE_OF(StrSlice), &ss), err);
}

#define JSON_MARSHALER_SIG_String(IN, OUT) OUT(Str)
#define JSON_MARSHALER_METHODS(M, T)                                                   \
    M(T, MarshalJSON, json_marshaler_marshal_json, JSONV2_SIG_MARSHAL_JSON)            \
    M(T, String, json_marshaler_string, JSON_MARSHALER_SIG_String)
BURROW_STRUCT_DEFINE_METHODS(JsonMarshaler, JSON_MARSHALER_FIELDS,
                             JSON_MARSHALER_METHODS);

/* jsonMarshalerError, a jsonMarshaler that is an error too. Go gets the
 * MarshalJSON by embedding, and here it is listed again. */
#define JSON_MARSHALER_ERR_FIELDS(F, T) F(T, Str, s, "")
BURROW_STRUCT_DECL(JsonMarshalerErr, JSON_MARSHALER_ERR_FIELDS);

static Slice json_marshaler_err_marshal_json(JsonMarshalerErr *j, Alloc *a,
                                             Error *err) {
    return json_marshaler_marshal_json((JsonMarshaler *)(void *)j, a, err);
}

static Str json_marshaler_err_error(JsonMarshalerErr *j) {
    (void)j;
    return S("oops");
}

#define JSON_MARSHALER_ERR_SIG_Error(IN, OUT) OUT(Str)
#define JSON_MARSHALER_ERR_METHODS(M, T)                                               \
    M(T, Error, json_marshaler_err_error, JSON_MARSHALER_ERR_SIG_Error)                \
    M(T, MarshalJSON, json_marshaler_err_marshal_json, JSONV2_SIG_MARSHAL_JSON)
BURROW_STRUCT_DEFINE_METHODS(JsonMarshalerErr, JSON_MARSHALER_ERR_FIELDS,
                             JSON_MARSHALER_ERR_METHODS);

#define M_FIELDS(F, T) F(T, StrSlice, M, "json:\"m\"")
BURROW_STRUCT(MRecord, M_FIELDS);

static void TestJSONHandlerMarshalerEscaping(TestingT *t) {
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    Str ascii = marshaler_ascii();
    BytesBuffer buf = BYTES_BUFFER(a);
    SlogHandler h = json_handler(a, &buf, NULL);
    SlogRecord r = slog_new_record(test_time(), SLOG_LEVEL_INFO, S("m"), 0);
    slog_record_add_attrs(&r, a,
                          ATTRS(slog_any(S("m"), BURROW_ANY(TYPE_OF(JsonMarshaler),
                                                            &(JsonMarshaler){ascii}))));
    Error err = handle(h, r);
    if (BURROW_FAILED(err)) {
        testing_t_errorf_v(t, "%v", err);
        arena_free(&ar);
        return;
    }
    Str got = trim_newline(buf_str(&buf));
    Slice gotb = slice_from((void *)(uintptr_t)got.p, got.len, got.len, TYPE_BYTE);
    if (!json_valid(gotb)) {
        testing_t_errorf_v(t, "output is not valid JSON: %q", got);
        arena_free(&ar);
        return;
    }
    const char *escapes[] = {"\\\"", "\\\\", "\\t", "\\n", "\\u0000"};
    for (size_t i = 0; i < sizeof escapes / sizeof escapes[0]; i++)
        if (!strings_contains(got, str_from_cstr(escapes[i])))
            testing_t_errorf_v(t, "output %q does not contain JSON escape %q", got,
                               escapes[i]);
    for (Int i = 0; i < got.len; i++)
        if (got.p[i] < 0x20)
            testing_t_errorf_v(t, "output %q contains unescaped control character %#x",
                               got, got.p[i]);
    MRecord rec = {0};
    err = json_unmarshal(a, gotb, BURROW_ANY(TYPE_OF(MRecord), &rec));
    if (BURROW_FAILED(err)) {
        testing_t_errorf_v(t, "%v", err);
    } else if (rec.M.len != 1 || !str_eq(((const Str *)rec.M.p)[0], ascii)) {
        testing_t_errorf_v(t, "marshaled value has %d strings, want [%q]", rec.M.len,
                           ascii);
    }
    arena_free(&ar);
}

/* jsonValueString: what the JSON handler writes for a value, which is the
 * line for a record with only that value in it, minus the {"v": and the }. */
static SlogHandlerOptions value_only_opts(void) {
    SlogHandlerOptions opts = {0};
    opts.replace_attr = remove_keys;
    opts.replace_attr_ctx = keys(builtin_keys);
    return opts;
}

static Str json_value_string(Alloc *a, Any v) {
    SlogHandlerOptions opts = value_only_opts();
    BytesBuffer buf = BYTES_BUFFER(a);
    SlogHandler h = json_handler(a, &buf, &opts);
    SlogRecord r = slog_new_record((Time){0}, SLOG_LEVEL_INFO, S(""), 0);
    slog_record_add_attrs(&r, a, ATTRS(slog_any(S("v"), v)));
    (void)handle(h, r);
    Str got = trim_newline(buf_str(&buf));
    got = strings_trim_prefix(got, S("{\"v\":"));
    return strings_trim_suffix(got, S("}"));
}

/* marshalJSON. */
static Str marshal_json(Alloc *a, Any x, Error *err) {
    BytesBuffer buf = BYTES_BUFFER(a);
    JsonEncoder *enc = json_new_encoder(a, bytes_buffer_as_io_writer(&buf));
    json_encoder_set_escape_html(enc, false);
    Error e = json_encoder_encode(enc, x);
    json_encoder_free(enc);
    BURROW_OUT(err, e);
    if (BURROW_FAILED(e))
        return BURROW_STR_EMPTY;
    return strings_trim_space(buf_str(&buf));
}

static void TestAppendJSONValue(TestingT *t) {
    /* The JSON handler should always agree with json.Marshal. */
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    Time tt = test_time();
    Any values[] = {
        STR_ANY("hello\r\n\t\a"),
        STR_ANY("\"[{escape}]\""),
        STR_ANY("<escapeHTML&>"),
        /* \u2028\u2029 is an edge case in JavaScript vs JSON. \xF6 is an
         * incomplete encoding. */
        STR_ANY("\xce\xb8\xe2\x80\xa8\xe2\x80\xa9\xef\xbf\xbf\xF6"),
        STR_ANY("-123"),
        BURROW_ANY_VAL(TYPE_INT64, int64_t, INT64_C(-9200123456789123456)),
        BURROW_ANY_VAL(TYPE_UINT64, uint64_t, UINT64_C(9200123456789123456)),
        BURROW_ANY_VAL(TYPE_FLOAT64, double, -12.75),
        BURROW_ANY_VAL(TYPE_FLOAT64, double, 1.23e-9),
        BURROW_ANY_VAL(TYPE_BOOL, bool, false),
        BURROW_ANY_VAL(TYPE_DURATION, Duration, TIME_MINUTE),
        BURROW_ANY(TYPE_TIME, &tt),
        BURROW_ANY(TYPE_OF(JsonMarshaler), &(JsonMarshaler){S("xyz")}),
        BURROW_ANY(TYPE_OF(JsonMarshalerErr), &(JsonMarshalerErr){S("pqr")}),
        BURROW_ANY_VAL(TYPE_SLOG_LEVEL, SlogLevel, SLOG_LEVEL_WARN),
    };
    for (size_t i = 0; i < sizeof values / sizeof values[0]; i++) {
        Str got = json_value_string(a, values[i]);
        Error err = BURROW_NO_ERROR;
        Str want = marshal_json(a, values[i], &err);
        if (BURROW_FAILED(err)) {
            testing_t_errorf_v(t, "%v: %v", values[i], err);
            continue;
        }
        if (!str_eq(got, want)) {
            testing_t_errorf_v(t, "%v: got %s, want %s", values[i], got, want);
            testing_t_errorf_v(t, "%v: got %x, want %x", values[i], got, want);
        }
    }
    arena_free(&ar);
}

static void TestJSONAppendAttrValueSpecial(TestingT *t) {
    /* Attr values that render differently from json.Marshal. */
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    struct {
        Any value;
        const char *want;
    } tests[] = {
        {BURROW_ANY_VAL(TYPE_FLOAT64, double, math_nan()),
         "\"!ERROR:json: unsupported value: NaN\""},
        {BURROW_ANY_VAL(TYPE_FLOAT64, double, math_inf(1)),
         "\"!ERROR:json: unsupported value: +Inf\""},
        {BURROW_ANY_VAL(TYPE_FLOAT64, double, math_inf(-1)),
         "\"!ERROR:json: unsupported value: -Inf\""},
        {BURROW_ANY(TYPE_ERROR, (void *)(uintptr_t)&io_eof), "\"EOF\""},
    };
    for (size_t i = 0; i < sizeof tests / sizeof tests[0]; i++) {
        Str got = json_value_string(a, tests[i].value);
        if (!str_eq(got, str_from_cstr(tests[i].want)))
            testing_t_errorf_v(t, "%v: got %s, want %s", tests[i].value, got,
                               tests[i].want);
    }
    arena_free(&ar);
}

/* -------------------------------------------------------- multi handler */

/* mockFailingHandler: a handler that hands the record on and then fails. */
typedef struct MockFailing {
    SlogHandler h;
    Error err;
} MockFailing;

static bool mock_enabled(void *self, Context ctx, SlogLevel level) {
    MockFailing *m = self;
    return m->h.vt->enabled(m->h.data, ctx, level);
}

static Error mock_handle(void *self, Context ctx, SlogRecord r) {
    MockFailing *m = self;
    (void)m->h.vt->handle(m->h.data, ctx, r);
    return m->err;
}

static SlogHandler mock_with_attrs(void *self, Slice attrs) {
    MockFailing *m = self;
    return with_attrs(m->h, attrs);
}

static SlogHandler mock_with_group(void *self, Str name) {
    MockFailing *m = self;
    return with_group(m->h, name);
}

static const SlogHandlerVT mock_failing_vt = {
    NULL, mock_enabled, mock_handle, mock_with_attrs, mock_with_group, NULL,
};

static SlogHandler multi(SlogHandler *hs, Int n) {
    return slog_multi_handler_as_handler(
        slog_new_multi_handler(heap_allocator(), hs, n));
}

static void TestMultiHandler(TestingT *t) {
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    Context ctx = context_background();

    /* Handle sends log to all handlers */
    {
        BytesBuffer buf1 = BYTES_BUFFER(a);
        BytesBuffer buf2 = BYTES_BUFFER(a);
        SlogHandler hs[2] = {text_handler(a, &buf1, NULL),
                             json_handler(a, &buf2, NULL)};
        SlogHandler m = multi(hs, 2);
        SlogLogger *logger = slog_new(a, m);
        slog_logger_info_v(logger, "hello world", "user", "test");
        check_log_output(t, a, buf_str(&buf1),
                         "time=" TEXT_TIME_RE
                         " level=INFO msg=\"hello world\" user=test");
        check_log_output(
            t, a, buf_str(&buf2),
            "{\"time\":\"" JSON_TIME_RE
            "\",\"level\":\"INFO\",\"msg\":\"hello world\",\"user\":\"test\"}");
        slog_logger_free(logger);
        slog_handler_free(m);
    }

    /* Enabled returns true if any handler is enabled */
    {
        SlogHandlerOptions err_opts = {0};
        err_opts.level = slog_level_as_leveler(SLOG_LEVEL_ERROR);
        SlogHandlerOptions info_opts = {0};
        info_opts.level = slog_level_as_leveler(SLOG_LEVEL_INFO);
        BytesBuffer b1 = BYTES_BUFFER(a);
        BytesBuffer b2 = BYTES_BUFFER(a);
        SlogHandler hs[2] = {text_handler(a, &b1, &err_opts),
                             text_handler(a, &b2, &info_opts)};
        SlogMultiHandler *m = slog_new_multi_handler(heap_allocator(), hs, 2);
        if (!slog_multi_handler_enabled(m, ctx, SLOG_LEVEL_INFO))
            testing_t_error_v(t,
                              "Enabled should be true for INFO level, but got false");
        if (!slog_multi_handler_enabled(m, ctx, SLOG_LEVEL_ERROR))
            testing_t_error_v(t,
                              "Enabled should be true for ERROR level, but got false");

        /* Enabled returns false if no handlers are enabled */
        if (slog_multi_handler_enabled(m, ctx, SLOG_LEVEL_DEBUG))
            testing_t_error_v(t,
                              "Enabled should be false for DEBUG level, but got true");
        slog_handler_free(slog_multi_handler_as_handler(m));
    }

    /* WithAttrs propagates attributes to all handlers */
    {
        BytesBuffer buf1 = BYTES_BUFFER(a);
        BytesBuffer buf2 = BYTES_BUFFER(a);
        SlogHandler hs[2] = {text_handler(a, &buf1, NULL),
                             json_handler(a, &buf2, NULL)};
        SlogHandler m = multi(hs, 2);
        SlogHandler mw = with_attrs(m, ATTRS(STRING("request_id", "123")));
        SlogLogger *logger = slog_new(a, mw);
        slog_logger_info_v(logger, "request processed");
        check_log_output(t, a, buf_str(&buf1),
                         "time=" TEXT_TIME_RE
                         " level=INFO msg=\"request processed\" request_id=123");
        check_log_output(t, a, buf_str(&buf2),
                         "{\"time\":\"" JSON_TIME_RE "\",\"level\":\"INFO\",\"msg\":"
                         "\"request processed\",\"request_id\":\"123\"}");
        slog_logger_free(logger);
        slog_handler_free(mw);
        slog_handler_free(m);
    }

    /* WithGroup propagates group to all handlers */
    {
        BytesBuffer buf1 = BYTES_BUFFER(a);
        BytesBuffer buf2 = BYTES_BUFFER(a);
        SlogHandlerOptions opts = {0};
        opts.add_source = false;
        SlogHandler hs[2] = {text_handler(a, &buf1, &opts),
                             json_handler(a, &buf2, &opts)};
        SlogHandler m = multi(hs, 2);
        SlogHandler mg = with_group(m, S("req"));
        SlogLogger *logger = slog_new(a, mg);
        slog_logger_info_v(logger, "user login", "user_id", 42);
        check_log_output(t, a, buf_str(&buf1),
                         "time=" TEXT_TIME_RE
                         " level=INFO msg=\"user login\" req.user_id=42");
        check_log_output(t, a, buf_str(&buf2),
                         "{\"time\":\"" JSON_TIME_RE "\",\"level\":\"INFO\",\"msg\":"
                         "\"user login\",\"req\":{\"user_id\":42}}");
        slog_logger_free(logger);
        slog_handler_free(mg);
        slog_handler_free(m);
    }

    /* Handle propagates errors from handlers */
    {
        Error err_fail = errors_new(error_allocator(), S("mock failing"));
        BytesBuffer buf1 = BYTES_BUFFER(a);
        BytesBuffer buf2 = BYTES_BUFFER(a);
        MockFailing mock = {json_handler(a, &buf2, NULL), err_fail};
        SlogHandler hs[2] = {{&mock_failing_vt, &mock}, text_handler(a, &buf1, NULL)};
        SlogMultiHandler *m = slog_new_multi_handler(heap_allocator(), hs, 2);
        Error err = slog_multi_handler_handle(
            m, ctx, slog_new_record(time_now(), SLOG_LEVEL_INFO, S("test message"), 0));
        if (!errors_is(err, err_fail))
            testing_t_errorf_v(t, "Expected error: %v, but got: %v", err_fail, err);
        check_log_output(t, a, buf_str(&buf1),
                         "time=" TEXT_TIME_RE " level=INFO msg=\"test message\"");
        check_log_output(t, a, buf_str(&buf2),
                         "{\"time\":\"" JSON_TIME_RE
                         "\",\"level\":\"INFO\",\"msg\":\"test message\"}");
        slog_handler_free(slog_multi_handler_as_handler(m));
    }

    /* Handle with no handlers */
    {
        SlogMultiHandler *m = slog_new_multi_handler(heap_allocator(), NULL, 0);
        SlogLogger *logger = slog_new(a, slog_multi_handler_as_handler(m));
        slog_logger_info_v(logger, "nothing");
        Error err = slog_multi_handler_handle(
            m, ctx, slog_new_record(time_now(), SLOG_LEVEL_INFO, S("test"), 0));
        if (BURROW_FAILED(err))
            testing_t_errorf_v(
                t, "Handle with no sub-handlers should return nil, but got: %v", err);
        slog_logger_free(logger);
        slog_handler_free(slog_multi_handler_as_handler(m));
    }
    arena_free(&ar);
}

/* Test that slog_new_multi_handler copies the handlers it is given and is
 * insulated from changes to them later. */
static void TestNewMultiHandlerCopy(TestingT *t) {
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    BytesBuffer buf1 = BYTES_BUFFER(a);
    SlogHandler hs[1] = {text_handler(a, &buf1, NULL)};
    SlogMultiHandler *m = slog_new_multi_handler(heap_allocator(), hs, 1);
    hs[0] = (SlogHandler){NULL, NULL};
    Error err = slog_multi_handler_handle(
        m, context_background(),
        slog_new_record(time_now(), SLOG_LEVEL_INFO, S("test message"), 0));
    if (BURROW_FAILED(err))
        testing_t_errorf_v(t, "Expected nil error, but got: %v", err);
    check_log_output(t, a, buf_str(&buf1),
                     "time=" TEXT_TIME_RE " level=INFO msg=\"test message\"");
    slog_handler_free(slog_multi_handler_as_handler(m));
    arena_free(&ar);
}

#define TESTS(X)                                                                       \
    X(TestDefaultHandle)                                                               \
    X(TestConcurrentWrites)                                                            \
    X(TestJSONAndTextHandlers)                                                         \
    X(TestHandlerEnabled)                                                              \
    X(TestJSONAndTextHandlersWithUnavailableSource)                                    \
    X(TestSecondWith)                                                                  \
    X(TestReplaceAttrGroups)                                                           \
    X(TestWriteTimeRFC3339)                                                            \
    X(TestDiscardHandler)                                                              \
    X(TestTextHandler)                                                                 \
    X(TestTextHandlerPreformatted)                                                     \
    X(TestNeedsQuoting)                                                                \
    X(TestJSONHandler)                                                                 \
    X(TestJSONHandlerMarshalerEscaping)                                                \
    X(TestAppendJSONValue)                                                             \
    X(TestJSONAppendAttrValueSpecial)                                                  \
    X(TestMultiHandler)                                                                \
    X(TestNewMultiHandlerCopy)
TESTING_MAIN(TESTS)
