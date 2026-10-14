/* Derived from Go's src/testing/slogtest/slogtest.go. See
 * include/burrow/testing/slogtest.h.
 * Go source: go1.27.1.
 *
 * Copyright 2023 The Go Authors. All rights reserved.
 * Copyright 2026 The burrow Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style licence that can be found
 * in the LICENSE file. */

#include "burrow/testing/slogtest.h"

#include "burrow/context.h"
#include "burrow/encoding/json/v2.h"
#include "burrow/fmt.h"
#include "burrow/mem/arena.h"

#include <string.h>

/* ------------------------------------------------------------------ checks */

/* check, as data. A check looks in the map it is given, or in the groups
 * named by group and group2 in turn when they are set, which is inGroup. */
typedef enum SlogtestCheckKind {
    SLOGTEST_HAS_KEY,
    SLOGTEST_MISSING_KEY,
    SLOGTEST_HAS_ATTR,
} SlogtestCheckKind;

typedef struct SlogtestCheck {
    SlogtestCheckKind kind;
    const char *key;
    const char *want; /* for SLOGTEST_HAS_ATTR */
    const char *group;
    const char *group2;
} SlogtestCheck;

#define HAS_KEY(k) {SLOGTEST_HAS_KEY, (k), NULL, NULL, NULL}
#define MISSING_KEY(k) {SLOGTEST_MISSING_KEY, (k), NULL, NULL, NULL}
#define HAS_ATTR(k, v) {SLOGTEST_HAS_ATTR, (k), (v), NULL, NULL}
#define IN_GROUP(g, kind, k, v) {(kind), (k), (v), (g), NULL}
#define IN_GROUP2(g, h, kind, k, v) {(kind), (k), (v), (g), (h)}

#define SLOGTEST_MAX_CHECKS 7

/* ------------------------------------------------------------------- cases */

/* replace, whose LogValue gives back the string it holds. */
static SlogValue slogtest_replace_log_value(void *self) {
    return slog_string_value(*(const Str *)self);
}

static const SlogLogValuerVT slogtest_replace_vt = {NULL, slogtest_replace_log_value};

static const Str slogtest_replaced = BURROW_S_INIT("replaced");
static const Str slogtest_v2 = BURROW_S_INIT("v2");

#define REPLACE(name, s)                                                               \
    SlogLogValuer name = {&slogtest_replace_vt, (void *)(uintptr_t)&(s)}
#define VALUER(lv) BURROW_ANY(TYPE_SLOG_LOG_VALUER, &(lv))
#define STR(k, v) slog_string(BURROW_S(k), BURROW_S(v))

static void slogtest_f_built_ins(SlogLogger *l) {
    slog_logger_info_v(l, "message");
}

static void slogtest_f_attrs(SlogLogger *l) {
    slog_logger_info_v(l, "message", "k", "v");
}

static void slogtest_f_empty_attr(SlogLogger *l) {
    Any nil = {NULL, NULL};
    slog_logger_info_v(l, "msg", "a", "b", "", nil, "c", "d");
}

static void slogtest_f_zero_time(SlogLogger *l) {
    slog_logger_info_v(l, "msg", "k", "v");
}

static void slogtest_f_with_attrs(SlogLogger *l) {
    SlogLogger *w = slog_logger_with_v(l, "a", "b");
    slog_logger_info_v(w, "msg", "k", "v");
    slog_logger_free(w);
}

static void slogtest_f_groups(SlogLogger *l) {
    SlogAttr c = STR("c", "d");
    SlogAttr g = slog_group_v("G", c);
    slog_logger_info_v(l, "msg", "a", "b", g, "e", "f");
}

static void slogtest_f_empty_group(SlogLogger *l) {
    SlogAttr g = slog_group_attrs(BURROW_S("G"), slice_nil(TYPE_SLOG_ATTR));
    slog_logger_info_v(l, "msg", "a", "b", g, "e", "f");
}

static void slogtest_f_inline_group(SlogLogger *l) {
    SlogAttr c = STR("c", "d");
    SlogAttr g = slog_group_v("", c);
    slog_logger_info_v(l, "msg", "a", "b", g, "e", "f");
}

static void slogtest_f_with_group(SlogLogger *l) {
    SlogLogger *g = slog_logger_with_group(l, BURROW_S("G"));
    slog_logger_info_v(g, "msg", "a", "b");
    slog_logger_free(g);
}

/* l.With("a", "b").WithGroup("G").With("c", "d").WithGroup("H"), and then I
 * too when nested is set. The loggers are freed newest first. */
static void slogtest_chain(SlogLogger *l, bool nested, bool attrs) {
    SlogLogger *ls[5];
    Int n = 0;
    ls[n++] = slog_logger_with_v(l, "a", "b");
    ls[n] = slog_logger_with_group(ls[n - 1], BURROW_S("G"));
    n++;
    ls[n] = slog_logger_with_v(ls[n - 1], "c", "d");
    n++;
    ls[n] = slog_logger_with_group(ls[n - 1], BURROW_S("H"));
    n++;
    if (nested) {
        ls[n] = slog_logger_with_group(ls[n - 1], BURROW_S("I"));
        n++;
    }
    if (attrs)
        slog_logger_info_v(ls[n - 1], "msg", "e", "f");
    else
        slog_logger_info_v(ls[n - 1], "msg");
    while (n > 0)
        slog_logger_free(ls[--n]);
}

static void slogtest_f_multi_with(SlogLogger *l) {
    slogtest_chain(l, false, true);
}

static void slogtest_f_empty_group_record(SlogLogger *l) {
    slogtest_chain(l, false, false);
}

static void slogtest_f_nested_empty_group_record(SlogLogger *l) {
    slogtest_chain(l, true, false);
}

static void slogtest_f_resolve(SlogLogger *l) {
    REPLACE(r, slogtest_replaced);
    Any v = VALUER(r);
    slog_logger_info_v(l, "msg", "k", v);
}

static void slogtest_f_resolve_groups(SlogLogger *l) {
    REPLACE(r, slogtest_v2);
    SlogAttr b = slog_any(BURROW_S("b"), VALUER(r));
    SlogAttr a = STR("a", "v1");
    SlogAttr g = slog_group_v("G", a, b);
    slog_logger_info_v(l, "msg", g);
}

static void slogtest_f_resolve_with_attrs(SlogLogger *l) {
    REPLACE(r, slogtest_replaced);
    Any v = VALUER(r);
    SlogLogger *w = slog_logger_with_v(l, "k", v);
    slog_logger_info_v(w, "msg");
    slog_logger_free(w);
}

static void slogtest_f_resolve_with_attrs_groups(SlogLogger *l) {
    REPLACE(r, slogtest_v2);
    SlogAttr b = slog_any(BURROW_S("b"), VALUER(r));
    SlogAttr a = STR("a", "v1");
    SlogAttr g = slog_group_v("G", a, b);
    SlogLogger *w = slog_logger_with_v(l, g);
    slog_logger_info_v(w, "msg");
    slog_logger_free(w);
}

static void slogtest_f_empty_pc(SlogLogger *l) {
    slog_logger_info_v(l, "message");
}

static void slogtest_mod_zero_time(SlogRecord *r) {
    memset(&r->time, 0, sizeof r->time);
}

/* Go's r.PC = 0. Here a record knows where it was made from by its pc or by
 * the source the _v macros fill in, so both go. */
static void slogtest_mod_empty_pc(SlogRecord *r) {
    r->pc = 0;
    memset(&r->source, 0, sizeof r->source);
}

/* withSource: the explanation with the file and line of the case after it. */
#define SLOGTEST_STR2(x) #x
#define SLOGTEST_STR(x) SLOGTEST_STR2(x)
#define WITH_SOURCE(s) s " (" __FILE__ ":" SLOGTEST_STR(__LINE__) ")"

/* testCase. */
typedef struct SlogtestCase {
    const char *name;
    const char *explanation;
    void (*f)(SlogLogger *l);
    void (*mod)(SlogRecord *r);
    SlogtestCheck checks[SLOGTEST_MAX_CHECKS];
} SlogtestCase;

#define TIME_KEY "time"
#define LEVEL_KEY "level"
#define MESSAGE_KEY "msg"
#define SOURCE_KEY "source"

static const SlogtestCase slogtest_cases[] = {
    {"built-ins",
     WITH_SOURCE("this test expects slog.TimeKey, slog.LevelKey and slog.MessageKey"),
     slogtest_f_built_ins,
     NULL,
     {HAS_KEY(TIME_KEY), HAS_KEY(LEVEL_KEY), HAS_ATTR(MESSAGE_KEY, "message")}},
    {"attrs",
     WITH_SOURCE("a Handler should output attributes passed to the logging function"),
     slogtest_f_attrs,
     NULL,
     {HAS_ATTR("k", "v")}},
    {"empty-attr",
     WITH_SOURCE("a Handler should ignore an empty Attr"),
     slogtest_f_empty_attr,
     NULL,
     {HAS_ATTR("a", "b"), MISSING_KEY(""), HAS_ATTR("c", "d")}},
    {"zero-time",
     WITH_SOURCE("a Handler should ignore a zero Record.Time"),
     slogtest_f_zero_time,
     slogtest_mod_zero_time,
     {MISSING_KEY(TIME_KEY)}},
    {"WithAttrs",
     WITH_SOURCE("a Handler should include the attributes from the WithAttrs method"),
     slogtest_f_with_attrs,
     NULL,
     {HAS_ATTR("a", "b"), HAS_ATTR("k", "v")}},
    {"groups",
     WITH_SOURCE("a Handler should handle Group attributes"),
     slogtest_f_groups,
     NULL,
     {HAS_ATTR("a", "b"), IN_GROUP("G", SLOGTEST_HAS_ATTR, "c", "d"),
      HAS_ATTR("e", "f")}},
    {"empty-group",
     WITH_SOURCE("a Handler should ignore an empty group"),
     slogtest_f_empty_group,
     NULL,
     {HAS_ATTR("a", "b"), MISSING_KEY("G"), HAS_ATTR("e", "f")}},
    {"inline-group",
     WITH_SOURCE("a Handler should inline the Attrs of a group with an empty key"),
     slogtest_f_inline_group,
     NULL,
     {HAS_ATTR("a", "b"), HAS_ATTR("c", "d"), HAS_ATTR("e", "f")}},
    {"WithGroup",
     WITH_SOURCE("a Handler should handle the WithGroup method"),
     slogtest_f_with_group,
     NULL,
     {HAS_KEY(TIME_KEY), HAS_KEY(LEVEL_KEY), HAS_ATTR(MESSAGE_KEY, "msg"),
      MISSING_KEY("a"), IN_GROUP("G", SLOGTEST_HAS_ATTR, "a", "b")}},
    {"multi-With",
     WITH_SOURCE("a Handler should handle multiple WithGroup and WithAttr calls"),
     slogtest_f_multi_with,
     NULL,
     {HAS_KEY(TIME_KEY), HAS_KEY(LEVEL_KEY), HAS_ATTR(MESSAGE_KEY, "msg"),
      HAS_ATTR("a", "b"), IN_GROUP("G", SLOGTEST_HAS_ATTR, "c", "d"),
      IN_GROUP2("G", "H", SLOGTEST_HAS_ATTR, "e", "f")}},
    {"empty-group-record",
     WITH_SOURCE("a Handler should not output groups if there are no attributes"),
     slogtest_f_empty_group_record,
     NULL,
     {HAS_KEY(TIME_KEY), HAS_KEY(LEVEL_KEY), HAS_ATTR(MESSAGE_KEY, "msg"),
      HAS_ATTR("a", "b"), IN_GROUP("G", SLOGTEST_HAS_ATTR, "c", "d"),
      IN_GROUP("G", SLOGTEST_MISSING_KEY, "H", NULL)}},
    {"nested-empty-group-record",
     WITH_SOURCE(
         "a Handler should not output nested groups if there are no attributes"),
     slogtest_f_nested_empty_group_record,
     NULL,
     {HAS_KEY(TIME_KEY), HAS_KEY(LEVEL_KEY), HAS_ATTR(MESSAGE_KEY, "msg"),
      HAS_ATTR("a", "b"), IN_GROUP("G", SLOGTEST_HAS_ATTR, "c", "d"),
      IN_GROUP("G", SLOGTEST_MISSING_KEY, "H", NULL),
      IN_GROUP("G", SLOGTEST_MISSING_KEY, "I", NULL)}},
    {"resolve",
     WITH_SOURCE("a Handler should call Resolve on attribute values"),
     slogtest_f_resolve,
     NULL,
     {HAS_ATTR("k", "replaced")}},
    {"resolve-groups",
     WITH_SOURCE("a Handler should call Resolve on attribute values in groups"),
     slogtest_f_resolve_groups,
     NULL,
     {IN_GROUP("G", SLOGTEST_HAS_ATTR, "a", "v1"),
      IN_GROUP("G", SLOGTEST_HAS_ATTR, "b", "v2")}},
    {"resolve-WithAttrs",
     WITH_SOURCE("a Handler should call Resolve on attribute values from WithAttrs"),
     slogtest_f_resolve_with_attrs,
     NULL,
     {HAS_ATTR("k", "replaced")}},
    {"resolve-WithAttrs-groups",
     WITH_SOURCE(
         "a Handler should call Resolve on attribute values in groups from WithAttrs"),
     slogtest_f_resolve_with_attrs_groups,
     NULL,
     {IN_GROUP("G", SLOGTEST_HAS_ATTR, "a", "v1"),
      IN_GROUP("G", SLOGTEST_HAS_ATTR, "b", "v2")}},
    {"empty-PC",
     WITH_SOURCE("a Handler should not output SourceKey if the PC is zero"),
     slogtest_f_empty_pc,
     slogtest_mod_empty_pc,
     {MISSING_KEY(SOURCE_KEY)}},
};

#define SLOGTEST_NCASES ((Int)(sizeof slogtest_cases / sizeof slogtest_cases[0]))

/* -------------------------------------------------------------- the checks */

static Str slogtest_cstr(const char *s) {
    return str_from_cstr(s);
}

/* The problem check c finds in m, or an empty Str. */
static Str slogtest_run_check(Alloc *a, const SlogtestCheck *c, Map *m) {
    const char *groups[2] = {c->group, c->group2};
    for (int i = 0; i < 2 && groups[i] != NULL; i++) {
        Str name = slogtest_cstr(groups[i]);
        Any *v = m != NULL ? map_get(m, &name) : NULL;
        if (v == NULL)
            return fmt_sprintf_v(a, "missing group %q", name);
        if (v->t == NULL || v->t->kind != KIND_MAP || v->t->key != TYPE_STRING ||
            v->t->elem != TYPE_ANY)
            return fmt_sprintf_v(a, "value for group %q is not map[string]any", name);
        m = *(Map *const *)v->data;
    }
    Str key = slogtest_cstr(c->key);
    Any *got = m != NULL ? map_get(m, &key) : NULL;
    switch (c->kind) {
    case SLOGTEST_MISSING_KEY:
        if (got != NULL)
            return fmt_sprintf_v(a, "unexpected key %q", key);
        return BURROW_STR_EMPTY;
    case SLOGTEST_HAS_KEY:
    case SLOGTEST_HAS_ATTR:
        if (got == NULL)
            return fmt_sprintf_v(a, "missing key %q", key);
        if (c->kind == SLOGTEST_HAS_KEY)
            return BURROW_STR_EMPTY;
        break;
    default:
        return BURROW_STR_EMPTY;
    }
    Str want = slogtest_cstr(c->want);
    if (got->t == TYPE_STRING && str_eq(*(const Str *)got->data, want))
        return BURROW_STR_EMPTY;
    return fmt_sprintf_v(a, "%q: got %#v, want %#v", key, *got, want);
}

/* Each problem in m for case i, passed to report. */
typedef void (*SlogtestReport)(void *ctx, Str problem, const char *explanation);

static void slogtest_check_case(Alloc *a, Int i, Map *m, SlogtestReport report,
                                void *ctx) {
    const SlogtestCase *c = &slogtest_cases[i];
    for (int j = 0; j < SLOGTEST_MAX_CHECKS && c->checks[j].key != NULL; j++) {
        Str p = slogtest_run_check(a, &c->checks[j], m);
        if (p.len > 0)
            report(ctx, p, c->explanation);
    }
}

/* ----------------------------------------------------------------- wrapper */

/* wrapper: a handler whose Handle calls mod on the record first. The rest is
 * the handler it wraps, as Go's embedding makes it, so WithAttrs and
 * WithGroup give back the wrapped handler's own. */
typedef struct SlogtestWrapper {
    SlogHandler h;
    void (*mod)(SlogRecord *r);
} SlogtestWrapper;

static bool slogtest_wrapper_enabled(void *self, Context ctx, SlogLevel level) {
    SlogtestWrapper *w = self;
    return w->h.vt->enabled(w->h.data, ctx, level);
}

static Error slogtest_wrapper_handle(void *self, Context ctx, SlogRecord r) {
    SlogtestWrapper *w = self;
    w->mod(&r);
    return w->h.vt->handle(w->h.data, ctx, r);
}

static SlogHandler slogtest_wrapper_with_attrs(void *self, Slice attrs) {
    SlogtestWrapper *w = self;
    return w->h.vt->with_attrs(w->h.data, attrs);
}

static SlogHandler slogtest_wrapper_with_group(void *self, Str name) {
    SlogtestWrapper *w = self;
    return w->h.vt->with_group(w->h.data, name);
}

static const SlogHandlerVT slogtest_wrapper_vt = {
    NULL,
    slogtest_wrapper_enabled,
    slogtest_wrapper_handle,
    slogtest_wrapper_with_attrs,
    slogtest_wrapper_with_group,
    NULL,
};

/* Logs case i through h. */
static void slogtest_log_case(Alloc *a, Int i, SlogHandler h) {
    const SlogtestCase *c = &slogtest_cases[i];
    SlogtestWrapper w = {h, c->mod};
    if (c->mod != NULL)
        h = (SlogHandler){&slogtest_wrapper_vt, &w};
    SlogLogger *l = slog_new(a, h);
    if (l == NULL)
        return;
    c->f(l);
    slog_logger_free(l);
}

/* ------------------------------------------------------------- TestHandler */

typedef struct SlogtestErrs {
    Alloc *a;
    Slice errs; /* of Error */
} SlogtestErrs;

static void slogtest_add_err(void *ctx, Str problem, const char *explanation) {
    SlogtestErrs *e = ctx;
    Error err = fmt_errorf_v("%s: %s", problem, explanation);
    e->errs = slice_append(e->a, e->errs, &err, 1);
}

Error slogtest_test_handler(SlogHandler h, SlogtestResultsFunc results) {
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);

    /* Run the handler on the test cases. */
    for (Int i = 0; i < SLOGTEST_NCASES; i++)
        slogtest_log_case(a, i, h);

    /* Collect and check the results. */
    SlogtestErrs e = {a, slice_nil(TYPE_ERROR)};
    Slice res = BURROW_CALLF0(results);
    if (res.len != SLOGTEST_NCASES) {
        arena_free(&ar);
        return fmt_errorf_v("got %d results, want %d", res.len, SLOGTEST_NCASES);
    }
    Map *const *ms = res.p;
    for (Int i = 0; i < res.len; i++)
        slogtest_check_case(a, i, ms[i], slogtest_add_err, &e);
    Error err = errors_join(error_allocator(), e.errs);
    arena_free(&ar);
    return err;
}

/* --------------------------------------------------------------------- Run */

typedef struct SlogtestSub {
    Int i;
    SlogtestNewHandlerFunc new_handler;
    SlogtestResultFunc result;
} SlogtestSub;

static void slogtest_report_t(void *ctx, Str problem, const char *explanation) {
    testing_t_errorf_v((TestingT *)ctx, "%s: %s", problem, explanation);
}

static void slogtest_sub(void *env, TestingT *t) {
    SlogtestSub *s = env;
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    SlogHandler h = BURROW_CALLF(s->new_handler, t);
    slogtest_log_case(a, s->i, h);
    Map *got = BURROW_CALLF(s->result, t);
    slogtest_check_case(a, s->i, got, slogtest_report_t, t);
    arena_free(&ar);
}

void slogtest_run(TestingT *t, SlogtestNewHandlerFunc new_handler,
                  SlogtestResultFunc result) {
    for (Int i = 0; i < SLOGTEST_NCASES; i++) {
        SlogtestSub s = {i, new_handler, result};
        testing_t_run(t, slogtest_cstr(slogtest_cases[i].name),
                      (TestingTFunc){slogtest_sub, &s});
    }
}
