/* Derived from Go's src/runtime/pprof: label_test.go, runtime_test.go,
 * proto_test.go and pprof_test.go. Go source: go1.27.1.
 *
 * Copyright 2016 The Go Authors. All rights reserved.
 * Copyright 2026 The burrow Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style licence that can be found
 * in the LICENSE file. */

#include "check.h"

#include "../src/debug/pprof_internal.h"
#include "../src/debug/pprof_profile.h"
#include "burrow/burrow.h"
#include "burrow/bytes.h"
#include "burrow/chan.h"
#include "burrow/context.h"
#include "burrow/mem/arena.h"
#include "burrow/mem/heap.h"
#include "burrow/os.h"
#include "burrow/proc.h"
#include "burrow/runtime.h"
#include "burrow/runtime/pprof.h"
#include "burrow/sched.h"
#include "burrow/slices.h"
#include "burrow/strings.h"
#include "burrow/symtab.h"

#include <string.h>

#define S(lit) BURROW_S(lit)
#define NELEM(x) ((Int)(sizeof(x) / sizeof((x)[0])))

/* ------------------------------------------------------------ label_test.go */

typedef struct LabelList {
    Alloc *a;
    Slice ls;
} LabelList;

static bool collect_label(void *env, Str key, Str value) {
    LabelList *l = (LabelList *)env;
    PprofLabel lbl = {key, value};
    l->ls = slice_append(l->a, l->ls, &lbl, 1);
    return true;
}

static int compare_label_keys(void *env, const void *a, const void *b) {
    (void)env;
    return (int)strings_compare(((const PprofLabel *)a)->key,
                                ((const PprofLabel *)b)->key);
}

static Slice labels_sorted(Alloc *a, Context ctx) {
    LabelList l = {a, slice_make(a, TYPE_PPROF_LABEL, 0, 0)};
    pprof_for_labels(ctx, BURROW_FN(PprofLabelFunc, collect_label, &l));
    slices_sort_func(l.ls, BURROW_FN(SlicesCmpFunc, compare_label_keys, NULL));
    return l.ls;
}

static bool labels_equal(Slice got, const PprofLabel *want, Int n) {
    if (got.len != n)
        return false;
    const PprofLabel *g = (const PprofLabel *)got.p;
    for (Int i = 0; i < n; i++) {
        if (!str_eq(g[i].key, want[i].key) || !str_eq(g[i].value, want[i].value))
            return false;
    }
    return true;
}

static Str labels_text(Alloc *a, Slice ls) {
    PprLabelMap m = {ls};
    return burrow__ppr_label_map_string(&m, a);
}

static Str want_text(Alloc *a, const PprofLabel *want, Int n) {
    StringsBuilder b = STRINGS_BUILDER(a);
    IoWriter w = strings_builder_as_io_writer(&b);
    for (Int i = 0; i < n; i++)
        fmt_fprintf_v(w, "%s{%s %s}", i > 0 ? S(" ") : S("["), want[i].key,
                      want[i].value);
    fmt_fprintf_v(w, "]");
    return strings_builder_string(&b);
}

static void TestContextLabels(TestingT *t) {
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);

    /* Background context starts with no labels. */
    Context ctx = context_background();
    Slice labels = labels_sorted(a, ctx);
    if (labels.len != 0)
        testing_t_errorf_v(t, "labels on background context: want [], got %s ",
                           labels_text(a, labels));

    /* Add a single label. */
    ctx = pprof_with_labels(a, ctx, pprof_labels_v(a, 2, S("key"), S("value")));
    /* Retrieve it with Label. */
    Str v;
    bool ok = pprof_label(ctx, S("key"), &v);
    if (!ok || !str_eq(v, S("value")))
        testing_t_errorf_v(t, "Label(ctx, \"key\"): got %s, %t; want \"value\", ok", v,
                           ok);
    Slice got_labels = labels_sorted(a, ctx);
    static const PprofLabel want1[] = {{BURROW_S_INIT("key"), BURROW_S_INIT("value")}};
    if (!labels_equal(got_labels, want1, NELEM(want1)))
        testing_t_errorf_v(t, "(sorted) labels on context: got %s, want %s",
                           labels_text(a, got_labels),
                           want_text(a, want1, NELEM(want1)));

    /* Add a label with a different key. */
    ctx = pprof_with_labels(a, ctx, pprof_labels_v(a, 2, S("key2"), S("value2")));
    ok = pprof_label(ctx, S("key2"), &v);
    if (!ok || !str_eq(v, S("value2")))
        testing_t_errorf_v(t, "Label(ctx, \"key2\"): got %s, %t; want \"value2\", ok",
                           v, ok);
    got_labels = labels_sorted(a, ctx);
    static const PprofLabel want2[] = {
        {BURROW_S_INIT("key"), BURROW_S_INIT("value")},
        {BURROW_S_INIT("key2"), BURROW_S_INIT("value2")}};
    if (!labels_equal(got_labels, want2, NELEM(want2)))
        testing_t_errorf_v(t, "(sorted) labels on context: got %s, want %s",
                           labels_text(a, got_labels),
                           want_text(a, want2, NELEM(want2)));

    /* Add label with first key to test label replacement. */
    ctx = pprof_with_labels(a, ctx, pprof_labels_v(a, 2, S("key"), S("value3")));
    ok = pprof_label(ctx, S("key"), &v);
    if (!ok || !str_eq(v, S("value3")))
        testing_t_errorf_v(t, "Label(ctx, \"key3\"): got %s, %t; want \"value3\", ok",
                           v, ok);
    got_labels = labels_sorted(a, ctx);
    static const PprofLabel want3[] = {
        {BURROW_S_INIT("key"), BURROW_S_INIT("value3")},
        {BURROW_S_INIT("key2"), BURROW_S_INIT("value2")}};
    if (!labels_equal(got_labels, want3, NELEM(want3)))
        testing_t_errorf_v(t, "(sorted) labels on context: got %s, want %s",
                           labels_text(a, got_labels),
                           want_text(a, want3, NELEM(want3)));

    /* Labels called with two labels with the same key should pick the
     * second. */
    ctx = pprof_with_labels(
        a, ctx, pprof_labels_v(a, 4, S("key4"), S("value4a"), S("key4"), S("value4b")));
    ok = pprof_label(ctx, S("key4"), &v);
    if (!ok || !str_eq(v, S("value4b")))
        testing_t_errorf_v(t, "Label(ctx, \"key4\"): got %s, %t; want \"value4b\", ok",
                           v, ok);
    got_labels = labels_sorted(a, ctx);
    static const PprofLabel want4[] = {
        {BURROW_S_INIT("key"), BURROW_S_INIT("value3")},
        {BURROW_S_INIT("key2"), BURROW_S_INIT("value2")},
        {BURROW_S_INIT("key4"), BURROW_S_INIT("value4b")}};
    if (!labels_equal(got_labels, want4, NELEM(want4)))
        testing_t_errorf_v(t, "(sorted) labels on context: got %s, want %s",
                           labels_text(a, got_labels),
                           want_text(a, want4, NELEM(want4)));
    arena_free(&ar);
}

static void TestLabelMapStringer(TestingT *t) {
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);

    struct {
        PprLabelMap m;
        Str expected;
    } tbl[3] = {
        {
            {slice_nil(TYPE_PPROF_LABEL)}, /* empty map */
            S("{}"),
        },
        {
            {pprof_labels_v(a, 2, S("foo"), S("bar")).list},
            S("{\"foo\":\"bar\"}"),
        },
        {
            {pprof_labels_v(a, 10, S("foo"), S("bar"), S("key1"), S("value1"),
                            S("key2"), S("value2"), S("key3"), S("value3"),
                            S("key4WithNewline"), S("\nvalue4"))
                 .list},
            S("{\"foo\":\"bar\", \"key1\":\"value1\", \"key2\":\"value2\", "
              "\"key3\":\"value3\", \"key4WithNewline\":\"\\nvalue4\"}"),
        },
    };
    for (Int i = 0; i < NELEM(tbl); i++) {
        Str got = burrow__ppr_label_map_string(&tbl[i].m, a);
        if (!str_eq(tbl[i].expected, got))
            testing_t_errorf_v(t, "%d.String() = %q; want %q", i, got, tbl[i].expected);
    }
    arena_free(&ar);
}

/* ---------------------------------------------------------- runtime_test.go */

/* getProfLabel, as the labels' String form. No labels at all is "{}", the same
 * as an empty set, the way Go's getProfLabel gives an empty map for both. */
static Str get_prof_label(Alloc *a) {
    const PprLabelMap *l = (const PprLabelMap *)burrow__get_prof_label();
    if (l == NULL)
        return S("{}");
    return burrow__ppr_label_map_string(l, a);
}

/* What a child goroutine saw, sent back over sync so that only the test's own
 * goroutine calls Errorf. */
typedef struct ChildLabels {
    Alloc *a;
    Chan *sync;
    Str got;
} ChildLabels;

static void child_labels(void *env) {
    ChildLabels *c = (ChildLabels *)env;
    c->got = get_prof_label(c->a);
    Byte one = 1;
    chan_send(c->sync, &one);
}

static Str child_prof_label(ChildLabels *c) {
    c->got = BURROW_STR_EMPTY;
    if (!go(BURROW_FN(Func, child_labels, c)))
        return S("<go failed>");
    (void)chan_recv(c->sync, NULL);
    return c->got;
}

static void TestSetGoroutineLabels(TestingT *t) {
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    ChildLabels c = {a, chan_make(heap_allocator(), TYPE_UINT8, 0), BURROW_STR_EMPTY};

    Str want_labels = S("{}");
    Str got_labels = get_prof_label(a);
    if (!str_eq(got_labels, want_labels))
        testing_t_errorf_v(t,
                           "Expected parent goroutine's profile labels to be empty "
                           "before test, got %s",
                           got_labels);
    got_labels = child_prof_label(&c);
    if (!str_eq(got_labels, want_labels))
        testing_t_errorf_v(
            t,
            "Expected child goroutine's profile labels to be empty before test, got %s",
            got_labels);

    want_labels = S("{\"key\":\"value\"}");
    Context ctx = pprof_with_labels(a, context_background(),
                                    pprof_labels_v(a, 2, S("key"), S("value")));
    pprof_set_goroutine_labels(ctx);
    got_labels = get_prof_label(a);
    if (!str_eq(got_labels, want_labels))
        testing_t_errorf_v(t, "parent goroutine's profile labels: got %s, want %s",
                           got_labels, want_labels);
    got_labels = child_prof_label(&c);
    if (!str_eq(got_labels, want_labels))
        testing_t_errorf_v(t, "child goroutine's profile labels: got %s, want %s",
                           got_labels, want_labels);

    want_labels = S("{}");
    ctx = context_background();
    pprof_set_goroutine_labels(ctx);
    got_labels = get_prof_label(a);
    if (!str_eq(got_labels, want_labels))
        testing_t_errorf_v(
            t, "Expected parent goroutine's profile labels to be empty, got %s",
            got_labels);
    got_labels = child_prof_label(&c);
    if (!str_eq(got_labels, want_labels))
        testing_t_errorf_v(
            t, "Expected child goroutine's profile labels to be empty, got %s",
            got_labels);

    chan_free(c.sync);
    arena_free(&ar);
}

typedef struct DoEnv {
    TestingT *t;
    Alloc *a;
} DoEnv;

static void do_body(void *env, Context ctx) {
    (void)ctx;
    DoEnv *d = (DoEnv *)env;
    TestingT *t = d->t;
    Str want_labels = S("{\"key1\":\"value1\", \"key2\":\"value2\"}");
    Str got_labels = get_prof_label(d->a);
    if (!str_eq(got_labels, want_labels))
        testing_t_errorf_v(t, "parent goroutine's profile labels: got %s, want %s",
                           got_labels, want_labels);

    ChildLabels c = {d->a, chan_make(heap_allocator(), TYPE_UINT8, 0),
                     BURROW_STR_EMPTY};
    got_labels = child_prof_label(&c);
    if (!str_eq(got_labels, want_labels))
        testing_t_errorf_v(t, "child goroutine's profile labels: got %s, want %s",
                           got_labels, want_labels);
    chan_free(c.sync);
}

static void TestDo(TestingT *t) {
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);

    Str want_labels = S("{}");
    Str got_labels = get_prof_label(a);
    if (!str_eq(got_labels, want_labels))
        testing_t_errorf_v(
            t,
            "Expected parent goroutine's profile labels to be empty before Do, got %s",
            got_labels);

    DoEnv d = {t, a};
    pprof_do(a, context_background(),
             pprof_labels_v(a, 4, S("key1"), S("value1"), S("key2"), S("value2")),
             BURROW_FN(PprofDoFunc, do_body, &d));

    got_labels = get_prof_label(a);
    if (!str_eq(got_labels, want_labels))
        testing_t_errorf_v(
            t,
            "Expected parent goroutine's profile labels to be empty after Do, got %s",
            got_labels);
    arena_free(&ar);
}

/* ------------------------------------------------------------ proto_test.go */

static PfProfile *translate_cpu_profile(Alloc *a, const uint64_t *data, Int n,
                                        Int count, Error *err) {
    BytesBuffer buf = BYTES_BUFFER(a);
    PprBuilder *b = burrow__ppr_new_builder(bytes_buffer_as_io_writer(&buf));
    void **tags =
        (void **)mem_alloc_array(a, (size_t)count, sizeof(void *), _Alignof(void *));
    uint64_t *words =
        (uint64_t *)mem_alloc_array(a, (size_t)n, sizeof(uint64_t), _Alignof(uint64_t));
    memcpy(words, data, (size_t)n * sizeof(uint64_t));
    *err =
        burrow__ppr_add_cpu_data(b, slice_from(words, n, n, TYPE_UINT64),
                                 slice_from(tags, count, count, TYPE_UNSAFE_POINTER));
    if (BURROW_FAILED(*err)) {
        burrow__ppr_builder_free(b);
        return NULL;
    }
    *err = burrow__ppr_build(b);
    if (BURROW_FAILED(*err))
        return NULL;
    return burrow__pf_parse(a, bytes_buffer_as_io_reader(&buf), err);
}

static bool value_type_eq(const PfValueType *x, Str typ, Str unit) {
    return x != NULL && str_eq(x->type, typ) && str_eq(x->unit, unit);
}

static bool mapping_eq(const PfMapping *x, const PfMapping *y) {
    if (x == NULL || y == NULL)
        return x == y;
    return x->id == y->id && x->start == y->start && x->limit == y->limit &&
           x->offset == y->offset && str_eq(x->file, y->file) &&
           str_eq(x->build_id, y->build_id) && x->has_functions == y->has_functions &&
           x->has_filenames == y->has_filenames &&
           x->has_line_numbers == y->has_line_numbers &&
           x->has_inline_frames == y->has_inline_frames;
}

typedef struct WantLocation {
    uint64_t id;
    const PfMapping *mapping;
    uint64_t address;
} WantLocation;

typedef struct WantSample {
    int64_t value[2];
    WantLocation location[2];
} WantSample;

static void check_profile(TestingT *t, PfProfile *p, int64_t period, Str period_type,
                          Str period_unit, const WantSample *samples, Int nsamples) {
    testing_t_helper(t);

    if (p->period != period)
        testing_t_errorf_v(t, "p.Period = %d, want %d", p->period, period);
    if (!value_type_eq(p->period_type, period_type, period_unit))
        testing_t_errorf_v(t, "p.PeriodType = %v\nwant = %s %s",
                           BURROW_ANY_OF(p->period_type), period_type, period_unit);
    PfValueType **st = (PfValueType **)p->sample_type.p;
    if (p->sample_type.len != 2 || !value_type_eq(st[0], S("samples"), S("count")) ||
        !value_type_eq(st[1], S("cpu"), S("nanoseconds")))
        testing_t_errorf_v(
            t, "p.SampleType has %d types\nwant = samples/count cpu/nanoseconds",
            p->sample_type.len);
    if (p->default_sample_type.len != 0)
        testing_t_errorf_v(t,
                           "p.DefaultSampleType = %s\nwant = ", p->default_sample_type);
    /* Line info is not in the expected samples, so it isn't compared. */
    if (p->sample.len != nsamples)
        testing_t_fatalf_v(t, "p.Sample has %d samples, want %d", p->sample.len,
                           nsamples);
    PfSample **ps = (PfSample **)p->sample.p;
    for (Int i = 0; i < nsamples; i++) {
        const PfSample *s = ps[i];
        const WantSample *w = &samples[i];
        const int64_t *v = (const int64_t *)s->value.p;
        bool ok = s->value.len == 2 && v[0] == w->value[0] && v[1] == w->value[1] &&
                  s->location.len == 2 && s->label.len == 0 && s->num_label.len == 0 &&
                  s->num_unit.len == 0;
        PfLocation **locs = (PfLocation **)s->location.p;
        for (Int j = 0; ok && j < 2; j++) {
            const PfLocation *l = locs[j];
            ok = l->id == w->location[j].id && l->address == w->location[j].address &&
                 !l->is_folded && mapping_eq(l->mapping, w->location[j].mapping);
        }
        if (!ok)
            testing_t_errorf_v(t, "sample %d = %s\nwant = %v %v %#x %#x\n", i,
                               burrow__pf_profile_string(p, p->a), w->value[0],
                               w->value[1], w->location[0].address,
                               w->location[1].address);
    }
}

static void TestConvertCPUProfileNoSamples(TestingT *t) {
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);

    /* A test server with mock cpu profile data. */
    BytesBuffer buf = BYTES_BUFFER(a);

    static const uint64_t b[] = {3, 0,
                                 500}; /* empty profile at 500 Hz (2ms sample period) */
    Error err;
    PfProfile *p = translate_cpu_profile(a, b, NELEM(b), 1, &err);
    if (BURROW_FAILED(err))
        testing_t_fatalf_v(t, "translateCPUProfile: %v", err);
    err = burrow__pf_profile_write(p, bytes_buffer_as_io_writer(&buf));
    if (BURROW_FAILED(err))
        testing_t_fatalf_v(t, "writing profile: %v", err);

    p = burrow__pf_parse(a, bytes_buffer_as_io_reader(&buf), &err);
    if (BURROW_FAILED(err))
        testing_t_fatalf_v(t, "profile.Parse: %v", err);

    /* Expected PeriodType and SampleType. */
    check_profile(t, p, 2000 * 1000, S("cpu"), S("nanoseconds"), NULL, 0);
    arena_free(&ar);
}

/* Two functions for addresses, on the systems where testPCs takes them from
 * functions. Go's call themselves; these return different numbers so that no
 * linker can fold them into one. */
static BURROW_NOINLINE int f1(void) {
    return 1;
}

static BURROW_NOINLINE int f2(void) {
    return 2;
}

#if defined(BURROW_OS_LINUX) || defined(BURROW_OS_NETBSD) || defined(BURROW_OS_COSMO)
#define PROC_SELF_MAPS 1

typedef struct MappingList {
    PfMapping *m;
    Int n;
} MappingList;

static void collect_mapping(void *env, uint64_t lo, uint64_t hi, uint64_t offset,
                            Str file, Str build_id) {
    MappingList *l = (MappingList *)env;
    if (l->n == 2)
        return;
    PfMapping *m = &l->m[l->n];
    memset(m, 0, sizeof *m);
    m->id = (uint64_t)l->n + 1;
    m->start = lo;
    m->limit = hi;
    m->offset = offset;
    m->file = file;
    m->build_id = build_id;
    l->n++;
}
#endif

#if defined(BURROW_OS_COSMO) ||                                                        \
    (!defined(PROC_SELF_MAPS) && !defined(BURROW_OS_DARWIN) &&                         \
     !defined(BURROW_OS_WINDOWS))
/* The default case of testPCs. */
static bool fake_pcs(uint64_t *addr1, uint64_t *addr2, PfMapping maps[2]) {
#if defined(BURROW_OS_WASI)
    /* Not in Go. A function pointer in wasm is an index into the function
     * table, so f1 and f2 can be one apart and addr1+1 would be addr2. Two
     * places in linear memory far enough apart stand in for them. */
    static const char text[64] = {1};
    (void)f1;
    (void)f2;
    *addr1 = (uint64_t)(uintptr_t)&text[0];
    *addr2 = (uint64_t)(uintptr_t)&text[32];
#else
    *addr1 = (uint64_t)(uintptr_t)f1;
    *addr2 = (uint64_t)(uintptr_t)f2;
#endif
    /* Fake mapping. Go's has HasFunctions true because two PCs from Go will be
     * fully symbolized, which burrow's are not, having no file or line. */
    memset(maps, 0, 2 * sizeof maps[0]);
    maps[0].id = 1;
    maps[1].id = 1;
    return true;
}
#endif

/* testPCs returns two PCs and two corresponding memory mappings to use in test
 * profiles. */
static bool test_pcs(TestingT *t, Alloc *a, uint64_t *addr1, uint64_t *addr2,
                     PfMapping maps[2]) {
#if defined(PROC_SELF_MAPS)
    /* Figure out two addresses from /proc/self/maps. */
    Error err;
    Slice mmap = os_read_file(a, S("/proc/self/maps"), &err);
    if (BURROW_FAILED(err)) {
#if defined(BURROW_OS_COSMO)
        /* Not in Go. A cosmo binary has the file when it runs on Linux and
         * not elsewhere, and without it the builder fakes a mapping, so the
         * test does too. */
        return fake_pcs(addr1, addr2, maps);
#else
        testing_t_fatalf_v(t, "%v", err);
#endif
    }
#if !defined(BURROW_OS_COSMO)
    (void)f1;
    (void)f2;
#endif
    MappingList l = {maps, 0};
    burrow__ppr_parse_proc_self_maps(a, mmap, collect_mapping, &l);
    if (l.n < 2) {
        /* It is possible for a binary to only have 1 executable region of
         * memory. */
        testing_t_skipf_v(t, "need 2 or more mappings, got %d", l.n);
        return false;
    }
    *addr1 = maps[0].start;
    *addr2 = maps[1].start;
#elif defined(BURROW_OS_DARWIN) || defined(BURROW_OS_WINDOWS)
    *addr1 = (uint64_t)(uintptr_t)f1;
    *addr2 = (uint64_t)(uintptr_t)f2;

    uint64_t start, end;
    Str exe, build_id;
    Error err = burrow__ppr_read_main_module_mapping(a, &start, &end, &exe, &build_id);
    if (BURROW_FAILED(err))
        testing_t_fatalf_v(t, "%v", err);

    /* Go has HasFunctions true here, because its two PCs are fully
     * symbolized. burrow has no file or line for a function, so they never
     * are. */
    for (int i = 0; i < 2; i++) {
        memset(&maps[i], 0, sizeof maps[i]);
        maps[i].id = 1;
        maps[i].start = start;
        maps[i].limit = end;
        maps[i].file = exe;
        maps[i].build_id = build_id;
    }
#else
    (void)t;
    (void)a;
    return fake_pcs(addr1, addr2, maps);
#endif
    return true;
}

static void TestConvertCPUProfile(TestingT *t) {
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);

    uint64_t addr1, addr2;
    PfMapping maps[2];
    if (!test_pcs(t, a, &addr1, &addr2, maps)) {
        arena_free(&ar);
        return;
    }

    const uint64_t b[] = {
        3, 0, 500,                       /* hz = 500 */
        5, 0, 10,  addr1 + 1, addr1 + 2, /* 10 samples in addr1 */
        5, 0, 40,  addr2 + 1, addr2 + 2, /* 40 samples in addr2 */
        5, 0, 10,  addr1 + 1, addr1 + 2, /* 10 samples in addr1 */
    };
    Error err;
    PfProfile *p = translate_cpu_profile(a, b, NELEM(b), 4, &err);
    if (BURROW_FAILED(err))
        testing_t_fatalf_v(t, "translating profile: %v", err);
    int64_t period = 2000 * 1000;
    const WantSample samples[] = {
        {{20, 20 * 2000 * 1000}, {{1, &maps[0], addr1}, {2, &maps[0], addr1 + 1}}},
        {{40, 40 * 2000 * 1000}, {{3, &maps[1], addr2}, {4, &maps[1], addr2 + 1}}},
    };
    check_profile(t, p, period, S("cpu"), S("nanoseconds"), samples, NELEM(samples));
    arena_free(&ar);
}

static const char prof_self_maps_tests[] =
    "\n"
    "00400000-0040b000 r-xp 00000000 fc:01 787766                             "
    "/bin/cat\n"
    "0060a000-0060b000 r--p 0000a000 fc:01 787766                             "
    "/bin/cat\n"
    "0060b000-0060c000 rw-p 0000b000 fc:01 787766                             "
    "/bin/cat\n"
    "014ab000-014cc000 rw-p 00000000 00:00 0                                  [heap]\n"
    "7f7d76af8000-7f7d7797c000 r--p 00000000 fc:01 1318064                    "
    "/usr/lib/locale/locale-archive\n"
    "7f7d7797c000-7f7d77b36000 r-xp 00000000 fc:01 1180226                    "
    "/lib/x86_64-linux-gnu/libc-2.19.so\n"
    "7f7d77b36000-7f7d77d36000 ---p 001ba000 fc:01 1180226                    "
    "/lib/x86_64-linux-gnu/libc-2.19.so\n"
    "7f7d77d36000-7f7d77d3a000 r--p 001ba000 fc:01 1180226                    "
    "/lib/x86_64-linux-gnu/libc-2.19.so\n"
    "7f7d77d3a000-7f7d77d3c000 rw-p 001be000 fc:01 1180226                    "
    "/lib/x86_64-linux-gnu/libc-2.19.so\n"
    "7f7d77d3c000-7f7d77d41000 rw-p 00000000 00:00 0\n"
    "7f7d77d41000-7f7d77d64000 r-xp 00000000 fc:01 1180217                    "
    "/lib/x86_64-linux-gnu/ld-2.19.so\n"
    "7f7d77f3f000-7f7d77f42000 rw-p 00000000 00:00 0\n"
    "7f7d77f61000-7f7d77f63000 rw-p 00000000 00:00 0\n"
    "7f7d77f63000-7f7d77f64000 r--p 00022000 fc:01 1180217                    "
    "/lib/x86_64-linux-gnu/ld-2.19.so\n"
    "7f7d77f64000-7f7d77f65000 rw-p 00023000 fc:01 1180217                    "
    "/lib/x86_64-linux-gnu/ld-2.19.so\n"
    "7f7d77f65000-7f7d77f66000 rw-p 00000000 00:00 0\n"
    "7ffc342a2000-7ffc342c3000 rw-p 00000000 00:00 0                          [stack]\n"
    "7ffc34343000-7ffc34345000 r-xp 00000000 00:00 0                          [vdso]\n"
    "ffffffffff600000-ffffffffff601000 r-xp 00000090 00:00 0                  "
    "[vsyscall]\n"
    "->\n"
    "00400000 0040b000 00000000 /bin/cat\n"
    "7f7d7797c000 7f7d77b36000 00000000 /lib/x86_64-linux-gnu/libc-2.19.so\n"
    "7f7d77d41000 7f7d77d64000 00000000 /lib/x86_64-linux-gnu/ld-2.19.so\n"
    "7ffc34343000 7ffc34345000 00000000 [vdso]\n"
    "ffffffffff600000 ffffffffff601000 00000090 [vsyscall]\n"
    "\n"
    "00400000-07000000 r-xp 00000000 00:00 0\n"
    "07000000-07093000 r-xp 06c00000 00:2e 536754                             "
    "/path/to/gobench_server_main\n"
    "07093000-0722d000 rw-p 06c92000 00:2e 536754                             "
    "/path/to/gobench_server_main\n"
    "0722d000-07b21000 rw-p 00000000 00:00 0\n"
    "c000000000-c000036000 rw-p 00000000 00:00 0\n"
    "->\n"
    "07000000 07093000 06c00000 /path/to/gobench_server_main\n";

static const char prof_self_maps_tests_with_deleted[] =
    "\n"
    "00400000-0040b000 r-xp 00000000 fc:01 787766                             /bin/cat "
    "(deleted)\n"
    "0060a000-0060b000 r--p 0000a000 fc:01 787766                             /bin/cat "
    "(deleted)\n"
    "0060b000-0060c000 rw-p 0000b000 fc:01 787766                             /bin/cat "
    "(deleted)\n"
    "014ab000-014cc000 rw-p 00000000 00:00 0                                  [heap]\n"
    "7f7d76af8000-7f7d7797c000 r--p 00000000 fc:01 1318064                    "
    "/usr/lib/locale/locale-archive\n"
    "7f7d7797c000-7f7d77b36000 r-xp 00000000 fc:01 1180226                    "
    "/lib/x86_64-linux-gnu/libc-2.19.so\n"
    "7f7d77b36000-7f7d77d36000 ---p 001ba000 fc:01 1180226                    "
    "/lib/x86_64-linux-gnu/libc-2.19.so\n"
    "7f7d77d36000-7f7d77d3a000 r--p 001ba000 fc:01 1180226                    "
    "/lib/x86_64-linux-gnu/libc-2.19.so\n"
    "7f7d77d3a000-7f7d77d3c000 rw-p 001be000 fc:01 1180226                    "
    "/lib/x86_64-linux-gnu/libc-2.19.so\n"
    "7f7d77d3c000-7f7d77d41000 rw-p 00000000 00:00 0\n"
    "7f7d77d41000-7f7d77d64000 r-xp 00000000 fc:01 1180217                    "
    "/lib/x86_64-linux-gnu/ld-2.19.so\n"
    "7f7d77f3f000-7f7d77f42000 rw-p 00000000 00:00 0\n"
    "7f7d77f61000-7f7d77f63000 rw-p 00000000 00:00 0\n"
    "7f7d77f63000-7f7d77f64000 r--p 00022000 fc:01 1180217                    "
    "/lib/x86_64-linux-gnu/ld-2.19.so\n"
    "7f7d77f64000-7f7d77f65000 rw-p 00023000 fc:01 1180217                    "
    "/lib/x86_64-linux-gnu/ld-2.19.so\n"
    "7f7d77f65000-7f7d77f66000 rw-p 00000000 00:00 0\n"
    "7ffc342a2000-7ffc342c3000 rw-p 00000000 00:00 0                          [stack]\n"
    "7ffc34343000-7ffc34345000 r-xp 00000000 00:00 0                          [vdso]\n"
    "ffffffffff600000-ffffffffff601000 r-xp 00000090 00:00 0                  "
    "[vsyscall]\n"
    "->\n"
    "00400000 0040b000 00000000 /bin/cat\n"
    "7f7d7797c000 7f7d77b36000 00000000 /lib/x86_64-linux-gnu/libc-2.19.so\n"
    "7f7d77d41000 7f7d77d64000 00000000 /lib/x86_64-linux-gnu/ld-2.19.so\n"
    "7ffc34343000 7ffc34345000 00000000 [vdso]\n"
    "ffffffffff600000 ffffffffff601000 00000090 [vsyscall]\n"
    "\n"
    "00400000-0040b000 r-xp 00000000 fc:01 787766                             /bin/cat "
    "with space\n"
    "0060a000-0060b000 r--p 0000a000 fc:01 787766                             /bin/cat "
    "with space\n"
    "0060b000-0060c000 rw-p 0000b000 fc:01 787766                             /bin/cat "
    "with space\n"
    "014ab000-014cc000 rw-p 00000000 00:00 0                                  [heap]\n"
    "7f7d76af8000-7f7d7797c000 r--p 00000000 fc:01 1318064                    "
    "/usr/lib/locale/locale-archive\n"
    "7f7d7797c000-7f7d77b36000 r-xp 00000000 fc:01 1180226                    "
    "/lib/x86_64-linux-gnu/libc-2.19.so\n"
    "7f7d77b36000-7f7d77d36000 ---p 001ba000 fc:01 1180226                    "
    "/lib/x86_64-linux-gnu/libc-2.19.so\n"
    "7f7d77d36000-7f7d77d3a000 r--p 001ba000 fc:01 1180226                    "
    "/lib/x86_64-linux-gnu/libc-2.19.so\n"
    "7f7d77d3a000-7f7d77d3c000 rw-p 001be000 fc:01 1180226                    "
    "/lib/x86_64-linux-gnu/libc-2.19.so\n"
    "7f7d77d3c000-7f7d77d41000 rw-p 00000000 00:00 0\n"
    "7f7d77d41000-7f7d77d64000 r-xp 00000000 fc:01 1180217                    "
    "/lib/x86_64-linux-gnu/ld-2.19.so\n"
    "7f7d77f3f000-7f7d77f42000 rw-p 00000000 00:00 0\n"
    "7f7d77f61000-7f7d77f63000 rw-p 00000000 00:00 0\n"
    "7f7d77f63000-7f7d77f64000 r--p 00022000 fc:01 1180217                    "
    "/lib/x86_64-linux-gnu/ld-2.19.so\n"
    "7f7d77f64000-7f7d77f65000 rw-p 00023000 fc:01 1180217                    "
    "/lib/x86_64-linux-gnu/ld-2.19.so\n"
    "7f7d77f65000-7f7d77f66000 rw-p 00000000 00:00 0\n"
    "7ffc342a2000-7ffc342c3000 rw-p 00000000 00:00 0                          [stack]\n"
    "7ffc34343000-7ffc34345000 r-xp 00000000 00:00 0                          [vdso]\n"
    "ffffffffff600000-ffffffffff601000 r-xp 00000090 00:00 0                  "
    "[vsyscall]\n"
    "->\n"
    "00400000 0040b000 00000000 /bin/cat with space\n"
    "7f7d7797c000 7f7d77b36000 00000000 /lib/x86_64-linux-gnu/libc-2.19.so\n"
    "7f7d77d41000 7f7d77d64000 00000000 /lib/x86_64-linux-gnu/ld-2.19.so\n"
    "7ffc34343000 7ffc34345000 00000000 [vdso]\n"
    "ffffffffff600000 ffffffffff601000 00000090 [vsyscall]\n";

static void record_mapping(void *env, uint64_t lo, uint64_t hi, uint64_t offset,
                           Str file, Str build_id) {
    (void)build_id;
    fmt_fprintf_v(strings_builder_as_io_writer((StringsBuilder *)env),
                  "%08x %08x %08x %s\n", lo, hi, offset, file);
}

static void proc_self_maps(TestingT *t, const char *input) {
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    Slice cases = strings_split(a, str_from_cstr(input), S("\n\n"));
    const Str *tt = (const Str *)cases.p;
    for (Int tx = 0; tx < cases.len; tx++) {
        Str out;
        bool ok;
        Str in = strings_cut(tt[tx], S("->\n"), &out, &ok);
        if (!ok)
            testing_t_fatalf_v(t, "malformed test case");
        if (out.len > 0 && out.p[out.len - 1] != '\n')
            out = fmt_sprintf_v(a, "%s\n", out);
        StringsBuilder buf = STRINGS_BUILDER(a);
        Byte *bytes = (Byte *)mem_alloc(a, (size_t)in.len + 1, 1);
        memcpy(bytes, in.p, (size_t)in.len);
        burrow__ppr_parse_proc_self_maps(
            a, slice_from(bytes, in.len, in.len, TYPE_BYTE), record_mapping, &buf);
        Str have = strings_builder_string(&buf);
        if (!str_eq(have, out))
            testing_t_errorf_v(t, "#%d: have:\n%s\nwant:\n%s\n%q\n%q", tx, have, out,
                               have, out);
    }
    arena_free(&ar);
}

static void proc_self_maps_normal(void *env, TestingT *t) {
    (void)env;
    proc_self_maps(t, prof_self_maps_tests);
}

static void proc_self_maps_with_deleted_file(void *env, TestingT *t) {
    (void)env;
    proc_self_maps(t, prof_self_maps_tests_with_deleted);
}

static void TestProcSelfMaps(TestingT *t) {
    testing_t_run(t, S("Normal"), BURROW_FN(TestingTFunc, proc_self_maps_normal, NULL));
    testing_t_run(t, S("WithDeletedFile"),
                  BURROW_FN(TestingTFunc, proc_self_maps_with_deleted_file, NULL));
}

static void TestEmptyStack(TestingT *t) {
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);

    static const uint64_t b[] = {
        3, 0, 500, /* hz = 500 */
        3, 0, 10,  /* 10 samples with an empty stack trace */
    };
    Error err;
    (void)translate_cpu_profile(a, b, NELEM(b), 2, &err);
    if (BURROW_FAILED(err))
        testing_t_fatalf_v(t, "translating profile: %v", err);
    arena_free(&ar);
}

/* ------------------------------------------------------------ pprof_test.go */

static void TestEmptyCallStack(TestingT *t) {
    Str name = S("test18836_0");

    testing_t_parallel(t);
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    StringsBuilder buf = STRINGS_BUILDER(a);
    PprofProfile *p = pprof_new_profile(name);

    Str foo = S("foo");
    pprof_profile_add(p, BURROW_ANY(TYPE_STRING, &foo), 47674);
    (void)pprof_profile_write_to(p, strings_builder_as_io_writer(&buf), 1);
    pprof_profile_remove(p, BURROW_ANY(TYPE_STRING, &foo));
    Str got = strings_builder_string(&buf);
    Str prefix = fmt_sprintf_v(a, "%s profile: total 1\n", name);
    if (!strings_has_prefix(got, prefix))
        testing_t_fatalf_v(t, "got:\n\t%q\nwant prefix:\n\t%q\n", got, prefix);
    /* Go looks for lostProfileEvent. Here the name is whatever the symbol
     * table calls the C function, and there is none without a table. */
    if (burrow__symtab_len() > 0) {
        RuntimeFunc fn;
        if (!runtime_func_for_pc((Uintptr)burrow__pprof_lost_profile_event, &fn))
            testing_t_fatalf_v(t, "no name for lostProfileEvent");
        Str lostevent = fn.name;
        if (!strings_contains(got, lostevent))
            testing_t_fatalf_v(t, "got:\n\t%q\ndoes not contain:\n\t%q\n", got,
                               lostevent);
    }
    arena_free(&ar);
}

/* Not in Go. Go's tests write profiles with many stacks through the built-in
 * ones, which burrow doesn't have yet, so this does it with a profile of its
 * own: three values from two places, in both formats. */
static BURROW_NOINLINE void add_here(PprofProfile *p, Int *v) {
    pprof_profile_add(p, BURROW_ANY(TYPE_INT, v), 0);
}

static BURROW_NOINLINE void add_there(PprofProfile *p, Int *v) {
    pprof_profile_add(p, BURROW_ANY(TYPE_INT, v), 0);
}

/* Whether this build can walk a stack at all, asked the way frames_test.c
 * does, with a skip of one, since frame zero is written without a walk. */
static bool have_walk(void) {
    Uintptr buf[4];
    Slice s = {buf, 4, 4, NULL};

    return runtime_callers(1, s) > 0;
}

static void TestProfileWriteTo(TestingT *t) {
    if (!have_walk())
        testing_t_skip_v(t, "no stack walk here, so the two places look the same");
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);

    PprofProfile *p = pprof_new_profile(S("burrow.test/writeto"));
    if (pprof_lookup(S("burrow.test/writeto")) != p)
        testing_t_errorf_v(t, "Lookup did not find the new profile");
    Int vals[] = {1, 2, 3};
    add_here(p, &vals[0]);
    /* One call site for both, since a compiler can make each call to add_there
     * a jump to pprof_profile_add, which leaves the call site as the frame
     * that tells the stacks apart. The bound is volatile so the loop can't be
     * unrolled into two call sites. */
    volatile int n = 3;
    for (int i = 1; i < n; i++)
        add_there(p, &vals[i]);
    if (pprof_profile_count(p) != 3)
        testing_t_errorf_v(t, "Count = %d, want 3", pprof_profile_count(p));

    StringsBuilder buf = STRINGS_BUILDER(a);
    Error err = pprof_profile_write_to(p, strings_builder_as_io_writer(&buf), 1);
    if (BURROW_FAILED(err))
        testing_t_fatalf_v(t, "WriteTo(1): %v", err);
    Str got = strings_builder_string(&buf);
    if (!strings_has_prefix(got, S("burrow.test/writeto profile: total 3\n2 @ ")))
        testing_t_errorf_v(t, "WriteTo(1) = %q, want the stack with 2 first", got);
    if (!strings_contains(got, S("\n1 @ ")))
        testing_t_errorf_v(t, "WriteTo(1) = %q, has no stack with 1", got);

    BytesBuffer pb = BYTES_BUFFER(a);
    err = pprof_profile_write_to(p, bytes_buffer_as_io_writer(&pb), 0);
    if (BURROW_FAILED(err))
        testing_t_fatalf_v(t, "WriteTo(0): %v", err);
    PfProfile *pp = burrow__pf_parse(a, bytes_buffer_as_io_reader(&pb), &err);
    if (BURROW_FAILED(err))
        testing_t_fatalf_v(t, "profile.Parse: %v", err);
    PfValueType **st = (PfValueType **)pp->sample_type.p;
    if (pp->sample_type.len != 1 ||
        !value_type_eq(st[0], S("burrow.test/writeto"), S("count")))
        testing_t_errorf_v(t, "SampleType has %d types, want burrow.test/writeto/count",
                           pp->sample_type.len);
    if (!value_type_eq(pp->period_type, S("burrow.test/writeto"), S("count")) ||
        pp->period != 1)
        testing_t_errorf_v(t,
                           "PeriodType or Period %d is not burrow.test/writeto/count 1",
                           pp->period);
    PfSample **ps = (PfSample **)pp->sample.p;
    if (pp->sample.len != 2)
        testing_t_fatalf_v(t, "%d samples, want 2", pp->sample.len);
    if (((const int64_t *)ps[0]->value.p)[0] != 2 ||
        ((const int64_t *)ps[1]->value.p)[0] != 1)
        testing_t_errorf_v(t, "sample values %d and %d, want 2 and 1",
                           ((const int64_t *)ps[0]->value.p)[0],
                           ((const int64_t *)ps[1]->value.p)[0]);

    pprof_profile_remove(p, BURROW_ANY(TYPE_INT, &vals[1]));
    if (pprof_profile_count(p) != 2)
        testing_t_errorf_v(t, "Count after Remove = %d, want 2",
                           pprof_profile_count(p));
    pprof_profile_remove(p, BURROW_ANY(TYPE_INT, &vals[0]));
    pprof_profile_remove(p, BURROW_ANY(TYPE_INT, &vals[2]));
    arena_free(&ar);
}

#define TESTS(X)                                                                       \
    X(TestContextLabels)                                                               \
    X(TestLabelMapStringer)                                                            \
    X(TestSetGoroutineLabels)                                                          \
    X(TestDo)                                                                          \
    X(TestConvertCPUProfileNoSamples)                                                  \
    X(TestConvertCPUProfile)                                                           \
    X(TestProcSelfMaps)                                                                \
    X(TestEmptyStack)                                                                  \
    X(TestEmptyCallStack)                                                              \
    X(TestProfileWriteTo)

TESTING_MAIN(TESTS)
