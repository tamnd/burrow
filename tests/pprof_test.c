/* Derived from Go's src/runtime/pprof: label_test.go, runtime_test.go,
 * proto_test.go, pprof_test.go, protomem_test.go and mprof_test.go. Go source:
 * go1.27.1.
 *
 * Copyright 2016 The Go Authors. All rights reserved.
 * Copyright 2026 The burrow Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style licence that can be found
 * in the LICENSE file. */

#include "check.h"

#include "../src/debug/pprof_internal.h"
#include "../src/debug/pprof_profile.h"
#include "../src/hash/internal.h"
#include "../src/strings/bytealg.h"
#include "burrow/burrow.h"
#include "burrow/bytes.h"
#include "burrow/chan.h"
#include "burrow/context.h"
#include "burrow/math/big.h"
#include "burrow/mcontext.h"
#include "burrow/mem/arena.h"
#include "burrow/mem/heap.h"
#include "burrow/mprof.h"
#include "burrow/os.h"
#include "burrow/proc.h"
#include "burrow/regexp.h"
#include "burrow/runtime.h"
#include "burrow/runtime/pprof.h"
#include "burrow/sched.h"
#include "burrow/slices.h"
#include "burrow/strings.h"
#include "burrow/symtab.h"
#include "burrow/time.h"

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

/* value has one entry for each sample type, location ends at the first id of
 * 0, and bytes is the "bytes" num label, or 0 for none. */
typedef struct WantSample {
    int64_t value[4];
    WantLocation location[3];
    int64_t bytes;
} WantSample;

static Int want_locations(const WantSample *w) {
    Int n = 0;
    while (n < 3 && w->location[n].id != 0)
        n++;
    return n;
}

static bool num_label_ok(const PfSample *s, int64_t bytes) {
    if (bytes == 0)
        return s->num_label.len == 0;
    if (s->num_label.len != 1)
        return false;
    const PfNumLabels *l = ((PfNumLabels *const *)s->num_label.p)[0];
    return str_eq(l->key, S("bytes")) && l->values.len == 1 &&
           ((const int64_t *)l->values.p)[0] == bytes;
}

/* sample_type is nst type and unit pairs. */
static void check_profile(TestingT *t, PfProfile *p, int64_t period, Str period_type,
                          Str period_unit, const Str *sample_type, Int nst,
                          const WantSample *samples, Int nsamples,
                          Str default_sample_type) {
    testing_t_helper(t);

    if (p->period != period)
        testing_t_errorf_v(t, "p.Period = %d, want %d", p->period, period);
    if (!value_type_eq(p->period_type, period_type, period_unit))
        testing_t_errorf_v(t, "p.PeriodType = %v\nwant = %s %s",
                           BURROW_ANY_OF(p->period_type), period_type, period_unit);
    PfValueType **st = (PfValueType **)p->sample_type.p;
    bool st_ok = p->sample_type.len == nst;
    for (Int i = 0; st_ok && i < nst; i++)
        st_ok = value_type_eq(st[i], sample_type[2 * i], sample_type[2 * i + 1]);
    if (!st_ok)
        testing_t_errorf_v(t, "p.SampleType has %d types, want %d", p->sample_type.len,
                           nst);
    if (!str_eq(default_sample_type, p->default_sample_type))
        testing_t_errorf_v(t, "p.DefaultSampleType = %v\nwant = %v",
                           p->default_sample_type, default_sample_type);
    /* Line info is not in the expected samples, so it isn't compared. */
    if (p->sample.len != nsamples)
        testing_t_fatalf_v(t, "p.Sample has %d samples, want %d", p->sample.len,
                           nsamples);
    PfSample **ps = (PfSample **)p->sample.p;
    for (Int i = 0; i < nsamples; i++) {
        const PfSample *s = ps[i];
        const WantSample *w = &samples[i];
        const int64_t *v = (const int64_t *)s->value.p;
        Int nloc = want_locations(w);
        bool ok = s->value.len == nst && s->location.len == nloc && s->label.len == 0 &&
                  num_label_ok(s, w->bytes) && s->num_unit.len == 0;
        for (Int j = 0; ok && j < nst; j++)
            ok = v[j] == w->value[j];
        PfLocation **locs = (PfLocation **)s->location.p;
        for (Int j = 0; ok && j < nloc; j++) {
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

static const Str cpu_sample_type[] = {
    BURROW_S_INIT("samples"),
    BURROW_S_INIT("count"),
    BURROW_S_INIT("cpu"),
    BURROW_S_INIT("nanoseconds"),
};

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
    check_profile(t, p, 2000 * 1000, S("cpu"), S("nanoseconds"), cpu_sample_type, 2,
                  NULL, 0, S(""));
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
        {{20, 20 * 2000 * 1000}, {{1, &maps[0], addr1}, {2, &maps[0], addr1 + 1}}, 0},
        {{40, 40 * 2000 * 1000}, {{3, &maps[1], addr2}, {4, &maps[1], addr2 + 1}}, 0},
    };
    check_profile(t, p, period, S("cpu"), S("nanoseconds"), cpu_sample_type, 2, samples,
                  NELEM(samples), S(""));
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

static Int empty_call_stack_test_run;

static void TestEmptyCallStack(TestingT *t) {
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    Str name = fmt_sprintf_v(a, "test18836_%d", empty_call_stack_test_run);
    empty_call_stack_test_run++;

    testing_t_parallel(t);
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
 * ones, most of which burrow doesn't have yet, so this does it with a profile
 * of its own: three values from two places, in both formats. */
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

    /* A run after the first, under -test.count, finds the profile already
     * there, emptied by the run before. */
    PprofProfile *p = pprof_lookup(S("burrow.test/writeto"));
    if (p == NULL) {
        p = pprof_new_profile(S("burrow.test/writeto"));
        if (pprof_lookup(S("burrow.test/writeto")) != p)
            testing_t_errorf_v(t, "Lookup did not find the new profile");
    }
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

/* Go's func1 to func4 are one line each, <-c. These also count their runs,
 * each in a field of its own, so that a compiler can't fold them into one
 * function and leave the profile one stack where Go has three, and so that the
 * receive isn't a tail call that would leave each out of its own stack. Then
 * they say they are done, so the test can wait for them before it frees the
 * labels they hold. */
typedef struct GoroutineCounts {
    Chan *c;
    SyncWaitGroup wg;
    uint32_t ran[3];
} GoroutineCounts;

static BURROW_NOINLINE void func1(void *env) {
    GoroutineCounts *g = (GoroutineCounts *)env;
    (void)chan_recv(g->c, NULL);
    sync_atomic_add_uint32(&g->ran[0], 1);
    sync_wait_group_done(&g->wg);
}

static BURROW_NOINLINE void func2(void *env) {
    GoroutineCounts *g = (GoroutineCounts *)env;
    (void)chan_recv(g->c, NULL);
    sync_atomic_add_uint32(&g->ran[1], 2);
    sync_wait_group_done(&g->wg);
}

static BURROW_NOINLINE void func3(void *env) {
    GoroutineCounts *g = (GoroutineCounts *)env;
    (void)chan_recv(g->c, NULL);
    sync_atomic_add_uint32(&g->ran[2], 3);
    sync_wait_group_done(&g->wg);
}

static void start_counted(GoroutineCounts *g, Int n) {
    for (Int i = 0; i < n; i++) {
        void (*f)(void *) = func3;
        if (i % 10 == 0)
            f = func1;
        else if (i % 2 == 0)
            f = func2;
        sync_wait_group_add(&g->wg, 1);
        if (!go(BURROW_FN(Func, f, g)))
            sync_wait_group_done(&g->wg);
        /* Let goroutines block on channel */
        for (int j = 0; j < 5; j++)
            runtime_gosched();
    }
}

static void start_counted_labelled(void *env, Context ctx) {
    (void)ctx;
    /* ... and again, with labels this time (just with fewer iterations to keep
     * sorting deterministic). */
    start_counted((GoroutineCounts *)env, 89);
}

static bool contains_in_order(Str s, const Str *all, Int n) {
    for (Int i = 0; i < n; i++) {
        Int at = strings_index(s, all[i]);
        if (at < 0)
            return false;
        s = str_from_bytes(s.p + at + all[i].len, s.len - at - all[i].len);
    }
    return true;
}

/* One entry of Go's map[int64]map[string]string, with key NULL for a count
 * that has no labels. */
typedef struct CountLabel {
    int64_t count;
    const char *key;
    const char *val;
} CountLabel;

static bool contains_counts_labels(PfProfile *prof, const CountLabel *want, Int nwant) {
    Int m[16] = {0};
    Int n[16] = {0};
    for (Int i = 0; i < nwant; i++) {
        m[i]++;
        if (want[i].key != NULL)
            n[i]++;
    }
    PfSample **ps = (PfSample **)prof->sample.p;
    for (Int i = 0; i < prof->sample.len; i++) {
        /* The count is the single value in the sample */
        if (ps[i]->value.len != 1)
            return false;
        int64_t c = ((const int64_t *)ps[i]->value.p)[0];
        for (Int j = 0; j < nwant; j++) {
            if (want[j].count == c)
                m[j]--;
        }
        PfStrLabels **ls = (PfStrLabels **)ps[i]->label.p;
        for (Int k = 0; k < ps[i]->label.len; k++) {
            const Str *vs = (const Str *)ls[k]->values.p;
            for (Int v = 0; v < ls[k]->values.len; v++) {
                Int j = 0;
                while (j < nwant && !(want[j].count == c && want[j].key != NULL &&
                                      str_eq(ls[k]->key, str_from_cstr(want[j].key)) &&
                                      str_eq(vs[v], str_from_cstr(want[j].val))))
                    j++;
                if (j == nwant)
                    return false;
                n[j]--;
            }
        }
    }
    for (Int i = 0; i < nwant; i++) {
        if (m[i] > 0 || n[i] != 0)
            return false;
    }
    return true;
}

static void TestGoroutineCounts(TestingT *t) {
    /* Setting GOMAXPROCS to 1 ensures we can force all goroutines to the
     * desired blocking point. */
    int procs = runtime_gomaxprocs(1);
    int before = runtime_numgoroutine();

    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    GoroutineCounts g;
    memset(&g, 0, sizeof g);
    g.c = chan_make(heap_allocator(), TYPE_INT, 0);
    start_counted(&g, 100);
    Context ctx = context_background();

    PprofLabel label = {S("label"), S("value")};
    PprofLabel self_label = {S("self-label"), S("self-value")};
    pprof_do(a, ctx, pprof_labels_v(a, 2, label.key, label.value),
             BURROW_FN(PprofDoFunc, start_counted_labelled, &g));

    pprof_set_goroutine_labels(
        pprof_with_labels(a, context_background(),
                          pprof_labels_v(a, 2, self_label.key, self_label.value)));

    /* Go also starts a finalizer that blocks with labels of its own, so that
     * the profile has to find the finalizer goroutine. burrow has no
     * finalizers, so the goroutine with count 1 is this one alone. */

    BytesBuffer w = BYTES_BUFFER(a);
    PprofProfile *goroutine_prof = pprof_lookup(S("goroutine"));
    if (goroutine_prof == NULL)
        testing_t_fatalf_v(t, "Lookup(\"goroutine\") = nil");

    Str label_str =
        fmt_sprintf_v(a, "\n# labels: %s",
                      labels_text(a, slice_from(&label, 1, 1, TYPE_PPROF_LABEL)));
    Str self_label_str =
        fmt_sprintf_v(a, "\n# labels: %s",
                      labels_text(a, slice_from(&self_label, 1, 1, TYPE_PPROF_LABEL)));
    Str ordered_prefix[] = {
        S("\n50 @ "), S("\n44 @"), label_str, S("\n40 @"), S("\n36 @"),    label_str,
        S("\n10 @"),  S("\n9 @"),  label_str, S("\n1 @"),  self_label_str,
    };
    /* Not in Go: runtime_gomaxprocs can't take Ps away from a scheduler that
     * is already running, so another P can steal a goroutine from above before
     * it gets to the channel, and the yields don't wait for it. With Go's one P
     * that can't happen. So this takes the profile again, for up to five
     * seconds, until every goroutine is blocked. */
    Str prof = {0};
    for (int i = 0; i < 5000; i++) {
        /* Check debug profile */
        bytes_buffer_reset(&w);
        (void)pprof_profile_write_to(goroutine_prof, bytes_buffer_as_io_writer(&w), 1);
        prof = bytes_buffer_string(&w, a);
        if (contains_in_order(prof, ordered_prefix, NELEM(ordered_prefix)))
            break;
        time_sleep(TIME_MILLISECOND);
    }
    if (!contains_in_order(prof, ordered_prefix, NELEM(ordered_prefix)))
        testing_t_errorf_v(t, "expected sorted goroutine counts with Labels:\n%s",
                           prof);

    /* Check proto profile */
    bytes_buffer_reset(&w);
    (void)pprof_profile_write_to(goroutine_prof, bytes_buffer_as_io_writer(&w), 0);
    Error err = BURROW_NO_ERROR;
    PfProfile *p = burrow__pf_parse(a, bytes_buffer_as_io_reader(&w), &err);
    if (BURROW_FAILED(err))
        testing_t_fatalf_v(t, "error parsing protobuf profile: %v", err);
    err = burrow__pf_profile_check_valid(p);
    if (BURROW_FAILED(err))
        testing_t_errorf_v(t, "protobuf profile is invalid: %v", err);
    const CountLabel expected_labels[] = {
        {50, NULL, NULL},
        {44, "label", "value"},
        {40, NULL, NULL},
        {36, "label", "value"},
        {10, NULL, NULL},
        {9, "label", "value"},
        {1, "self-label", "self-value"},
    };
    if (!contains_counts_labels(p, expected_labels, NELEM(expected_labels)))
        testing_t_errorf_v(t,
                           "expected count profile to contain goroutines with counts "
                           "and labels, got %d samples",
                           p->sample.len);

    chan_close(g.c);
    /* Go sleeps here to let the goroutines exit. These say when they have. */
    sync_wait_group_wait(&g.wg);
    /* Not in Go: a goroutine that has said it is done still holds its labels
     * until it exits, and they live in ar, which is freed below. Go's labels
     * are collected once nothing uses them. */
    for (int i = 0; i < 5000 && runtime_numgoroutine() > before; i++)
        time_sleep(TIME_MILLISECOND);
    pprof_set_goroutine_labels(context_background());
    (void)runtime_gomaxprocs(procs);
    chan_free(g.c);
    arena_free(&ar);
}

/* ----------------------------------------------------------- cpu profile */

/* Not in Go: the places the profiler has nothing to sample with. Windows and
 * wasip1 have no SIGPROF, and outside amd64, arm64 and 386 the handler cannot
 * read the interrupted pc, so every sample would be burrow__prof_system. */
#if defined(BURROW_OS_WINDOWS) || defined(BURROW_OS_WASI) ||                           \
    !(defined(BURROW_ARCH_AMD64) || defined(BURROW_ARCH_ARM64) ||                      \
      defined(BURROW_ARCH_386))
#define NO_CPU_SAMPLES 1
#endif

static void cpu_hogger(Int (*f)(Int x), Int *y, Duration dur) {
    /* We only need to get one 100 Hz clock tick, so we've got a large safety
     * buffer. But do at least 500 iterations (which should take about 100ms),
     * otherwise TestCPUProfileMultithreaded can fail if only one thread is
     * scheduled during the testing period. */
    Time t0 = time_now();
    Int accum = *y;
    for (Int i = 0; i < 500 || time_since(t0) < dur; i++)
        accum = f(accum);
    *y = accum;
}

static Int salt1 = 0;
static Int salt2 = 0;

/* Not in Go. Go's hogs are functions of the test, and the profile finds them by
 * name. The symbol table here names the library's global functions and nothing
 * in a test, so each hog spends its time in one of those instead, called from
 * another file so that it can't be inlined, and the profile looks for that. */
static const Byte hog_data[1 << 16] = {1};

/* The actual CPU hogging function. */
static Int cpu_hog1(Int x) {
    return x + (Int)burrow__adler32_update((uint32_t)x, hog_data, (Int)sizeof hog_data);
}

static Int cpu_hog2(Int x) {
    Str s = {hog_data, (Int)sizeof hog_data};
    return x + burrow__bytealg_count(s, (Byte)x);
}

/* Go's sampleMatchFunc. */
typedef bool (*SampleMatchFunc)(Str spec, uint64_t count, Slice stk, Slice labels);

/* What Go's matchAndAvoidStacks closes over. Go's avoidFunctions is empty
 * except for gccgo, so there is nothing to avoid. */
typedef struct ProfileMatch {
    SampleMatchFunc matches;
    const char *const *need;
    Int nneed;
} ProfileMatch;

static Str location_name(const PfLocation *loc) {
    if (loc->line.len == 0)
        return BURROW_STR_EMPTY;
    const PfLine *line = (const PfLine *)loc->line.p;
    return line[0].function != NULL ? line[0].function->name : BURROW_STR_EMPTY;
}

static PfProfile *parse_profile(TestingT *t, Alloc *a, BytesBuffer *val_bytes,
                                void (*f)(void *env, uint64_t count, Slice stk,
                                          Slice labels),
                                void *env) {
    Error err = BURROW_NO_ERROR;
    PfProfile *p = burrow__pf_parse(a, bytes_buffer_as_io_reader(val_bytes), &err);
    if (BURROW_FAILED(err))
        testing_t_fatal_v(t, err);
    PfSample **ps = (PfSample **)p->sample.p;
    for (Int i = 0; i < p->sample.len; i++) {
        uint64_t count = (uint64_t)((const int64_t *)ps[i]->value.p)[0];
        f(env, count, ps[i]->location, ps[i]->label);
    }
    return p;
}

/* stackContains matches if a function named spec appears anywhere in the stack
 * trace. */
static bool stack_contains(Str spec, uint64_t count, Slice stk, Slice labels) {
    (void)count;
    (void)labels;
    PfLocation **locs = (PfLocation **)stk.p;
    for (Int i = 0; i < stk.len; i++) {
        const PfLine *lines = (const PfLine *)locs[i]->line.p;
        for (Int j = 0; j < locs[i]->line.len; j++) {
            if (lines[j].function != NULL &&
                strings_contains(lines[j].function->name, spec))
                return true;
        }
    }
    return false;
}

static bool stack_contains_labeled(Str spec, uint64_t count, Slice stk, Slice labels) {
    Str kv;
    bool ok;
    Str base = strings_cut(spec, S(";"), &kv, &ok);
    if (!ok)
        panic_str(S("no semicolon in key/value spec"));
    Str v;
    Str k = strings_cut(kv, S("="), &v, &ok);
    if (!ok)
        panic_str(S("missing = in key/value spec"));
    PfStrLabels *l = burrow__pf_labels_get(labels, k);
    bool have = false;
    for (Int i = 0; l != NULL && i < l->values.len; i++)
        have = have || str_eq(((const Str *)l->values.p)[i], v);
    if (!have)
        return false;
    return stack_contains(base, count, stk, labels);
}

static void fprint_stack(Alloc *a, StringsBuilder *w, Slice stk) {
    IoWriter out = strings_builder_as_io_writer(w);
    if (stk.len == 0)
        fmt_fprintf_v(out, " (stack empty)");
    PfLocation **locs = (PfLocation **)stk.p;
    for (Int i = 0; i < stk.len; i++) {
        fmt_fprintf_v(out, " %#x", locs[i]->address);
        fmt_fprintf_v(out, " (");
        const PfLine *lines = (const PfLine *)locs[i]->line.p;
        for (Int j = 0; j < locs[i]->line.len; j++) {
            if (j > 0)
                fmt_fprintf_v(out, " ");
            Str name =
                lines[j].function != NULL ? lines[j].function->name : BURROW_STR_EMPTY;
            fmt_fprintf_v(out, "%s:%d", name, lines[j].line);
        }
        fmt_fprintf_v(out, ")");
    }
    (void)a;
}

typedef struct ProfileSamples {
    Alloc *a;
    StringsBuilder buf;
    uint64_t samples;
} ProfileSamples;

static void count_sample(void *env, uint64_t count, Slice stk, Slice labels) {
    ProfileSamples *s = (ProfileSamples *)env;
    IoWriter out = strings_builder_as_io_writer(&s->buf);
    fmt_fprintf_v(out, "%d:", count);
    fprint_stack(s->a, &s->buf, stk);
    PfStrLabels **ls = (PfStrLabels **)labels.p;
    fmt_fprintf_v(out, " labels: map[");
    for (Int i = 0; i < labels.len; i++)
        fmt_fprintf_v(out, i > 0 ? " %s:%v" : "%s:%v", ls[i]->key, ls[i]->values);
    fmt_fprintf_v(out, "]\n");
    s->samples += count;
    fmt_fprintf_v(out, "\n");
}

static bool match_and_avoid_stacks(TestingT *t, Alloc *a, const ProfileMatch *m,
                                   PfProfile *p) {
    bool ok = true;

    /* Check that profile is well formed and contains 'need'. */
    uint64_t *have = (uint64_t *)mem_alloc_array(
        a, (size_t)(m->nneed > 0 ? m->nneed : 1), sizeof(uint64_t), _Alignof(uint64_t));
    for (Int i = 0; i < m->nneed; i++)
        have[i] = 0;

    PfSample **ps = (PfSample **)p->sample.p;
    for (Int s = 0; s < p->sample.len; s++) {
        uint64_t count = (uint64_t)((const int64_t *)ps[s]->value.p)[0];
        for (Int i = 0; i < m->nneed; i++) {
            if (m->matches(str_from_cstr(m->need[i]), count, ps[s]->location,
                           ps[s]->label))
                have[i] += count;
        }
    }

    if (m->nneed == 0)
        return ok;

    uint64_t total = 0;
    for (Int i = 0; i < m->nneed; i++) {
        total += have[i];
        testing_t_logf_v(t, "found %d samples in expected function %s\n", have[i],
                         m->need[i]);
    }
    if (total == 0) {
        testing_t_logf_v(t, "no samples in expected functions");
        ok = false;
    }

    /* We'd like to check a reasonable minimum, like total / len(have) /
     * smallconstant, but this test is pretty flaky (see bug 7095). So we'll
     * just test to make sure we got at least one sample. */
    uint64_t min = 1;
    for (Int i = 0; i < m->nneed; i++) {
        if (have[i] < min) {
            testing_t_logf_v(
                t, "%s has %d samples out of %d, want at least %d, ideally %d",
                m->need[i], have[i], total, min, total / (uint64_t)m->nneed);
            ok = false;
        }
    }
    return ok;
}

static bool profile_ok(TestingT *t, Alloc *a, const ProfileMatch *matches,
                       BytesBuffer *prof, Duration duration, PfProfile **out) {
    bool ok = true;

    ProfileSamples s = {a, STRINGS_BUILDER(a), 0};
    PfProfile *p = parse_profile(t, a, prof, count_sample, &s);
    testing_t_logf_v(t, "total %d CPU profile samples collected:\n%s", s.samples,
                     strings_builder_string(&s.buf));
    *out = p;

    /* Check that we got a reasonable number of samples. We used to always
     * require at least ideal/4 samples, but that is too hard to guarantee on a
     * loaded system. Now we accept 10 or more samples, which we take to be
     * enough to show that at least some profiling is occurring. */
    uint64_t ideal = (uint64_t)(duration * 100 / TIME_SECOND);
    if (s.samples == 0 || (s.samples < ideal / 4 && s.samples < 10)) {
        testing_t_logf_v(t, "too few samples; got %d, want at least %d, ideally %d",
                         s.samples, ideal / 4, ideal);
        ok = false;
    }

    if (matches != NULL && !match_and_avoid_stacks(t, a, matches, p))
        ok = false;

    return ok;
}

/* testCPUProfile runs f under the CPU profiler, checking for some conditions
 * specified by need, as interpreted by matches, and returns the parsed
 * profile. */
static PfProfile *test_cpu_profile(TestingT *t, Alloc *a, const ProfileMatch *matches,
                                   void (*f)(void *env, Duration dur), void *env) {
#if defined(NO_CPU_SAMPLES)
    testing_t_skip_v(t, "no CPU profile samples on this platform");
#endif
#if BURROW_TSAN
    /* Not in Go. The thread sanitizer's checks, which the hogs call on every
     * byte they read, keep no frame pointer, so a sample taken in one walks
     * straight past the function the test is looking for. */
    testing_t_skip_v(t, "the thread sanitizer hides the function being sampled");
#endif
    int64_t deadline;
    if (!testing_t_deadline(t, &deadline))
        deadline = burrow_nanotime() + 10 * TIME_SECOND;

    /* If we're running a long test, start with a long duration for tests that
     * try to make sure something *doesn't* happen. */
    Duration duration = 5 * TIME_SECOND;
    if (testing_short())
        duration = 100 * TIME_MILLISECOND;

    /* Profiling tests are inherently flaky, especially on a loaded system, such
     * as when this test is running with several others under go test std. If a
     * test fails in a way that could mean it just didn't run long enough, try
     * with a longer duration. */
    for (;;) {
        /* Not in Go. The profile is written from another goroutine while f
         * runs, and f may allocate from a, so the buffer has an arena of its
         * own. */
        Arena par;
        arena_init(&par, NULL, 0);
        BytesBuffer prof = BYTES_BUFFER(arena_allocator(&par));
        Error err = pprof_start_cpu_profile(bytes_buffer_as_io_writer(&prof));
        if (BURROW_FAILED(err))
            testing_t_fatal_v(t, err);
        f(env, duration);
        pprof_stop_cpu_profile();

        PfProfile *p;
        bool ok = profile_ok(t, a, matches, &prof, duration, &p);
        arena_free(&par);
        if (ok)
            return p;

        duration *= 2;
        if (deadline - burrow_nanotime() < duration)
            break;
        testing_t_logf_v(t, "retrying with %s duration", duration_string(duration, a));
    }

    /* Ignore the failure if the tests are running in a QEMU-based emulator,
     * QEMU is not perfect at emulating everything. IN_QEMU environmental
     * variable is set by some of the Go builders. IN_QEMU=1 indicates that the
     * tests are running in QEMU. See issue 9605. */
    if (str_eq(os_getenv(a, S("IN_QEMU")), S("1")))
        testing_t_skip_v(t, "ignore the failure in QEMU; see golang.org/issue/9605");
    testing_t_fail_now(t);
}

static void hog_salt1(void *env, Duration dur) {
    (void)env;
    cpu_hogger(cpu_hog1, &salt1, dur);
}

static void TestCPUProfile(TestingT *t) {
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    static const char *const need[] = {"burrow__adler32_update"};
    ProfileMatch matches = {stack_contains, need, 1};
    (void)test_cpu_profile(t, a, &matches, hog_salt1, NULL);
    arena_free(&ar);
}

typedef struct HogEnv {
    Duration dur;
    Chan *c;
} HogEnv;

static void hog1_then_send(void *env) {
    HogEnv *h = (HogEnv *)env;
    cpu_hogger(cpu_hog1, &salt1, h->dur);
    Int one = 1;
    chan_send(h->c, &one);
}

static void hog_both(void *env, Duration dur) {
    HogEnv h = {dur, chan_make(heap_allocator(), TYPE_INT, 0)};
    if (h.c == NULL || !go(BURROW_FN(Func, hog1_then_send, &h)))
        panic_str(S("cannot start the second hog"));
    cpu_hogger(cpu_hog2, &salt2, dur);
    Int v;
    (void)chan_recv(h.c, &v);
    chan_free(h.c);
    (void)env;
}

static void TestCPUProfileMultithreaded(TestingT *t) {
    int procs = runtime_gomaxprocs(2);
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    static const char *const need[] = {"burrow__adler32_update",
                                       "burrow__bytealg_count"};
    ProfileMatch matches = {stack_contains, need, 2};
    (void)test_cpu_profile(t, a, &matches, hog_both, NULL);
    arena_free(&ar);
    (void)runtime_gomaxprocs(procs);
}

typedef struct SwitchCheck {
    TestingT *t;
    Alloc *a;
} SwitchCheck;

static void check_no_switch(void *env, uint64_t count, Slice stk, Slice labels) {
    SwitchCheck *c = (SwitchCheck *)env;
    (void)count;
    (void)labels;
    PfLocation **locs = (PfLocation **)stk.p;

    /* An entry with two frames with 'System' in its top frame exists to record
     * a PC without a traceback. Those are okay. */
    if (stk.len == 2) {
        Str name = location_name(locs[1]);
        if (str_eq(name, S("burrow__prof_system")) ||
            str_eq(name, S("burrow__prof_external_code")))
            return;
    }

    /* An entry with just one frame is OK too: it knew to stop at gogo. */
    if (stk.len == 1)
        return;

    /* Otherwise, should not see gogo. The place we'd see it would be the inner
     * most frame. Go's gogo is burrow__mcontext_switch_raw here, the part that
     * changes stacks. */
    if (stk.len > 0 &&
        str_eq(location_name(locs[0]), S("burrow__mcontext_switch_raw"))) {
        StringsBuilder buf = STRINGS_BUILDER(c->a);
        fprint_stack(c->a, &buf, stk);
        testing_t_fatalf_v(c->t, "found profile entry for gogo:\n%s",
                           strings_builder_string(&buf));
    }
}

/* Test that profiler does not observe runtime.gogo as "user" goroutine
 * execution. If it did, it would see inconsistent state and would either record
 * an incorrect stack or crash because the stack was malformed. */
static void TestGoroutineSwitch(TestingT *t) {
#if defined(NO_CPU_SAMPLES)
    testing_t_skip_v(t, "no CPU profile samples on this platform");
#endif
#if !defined(BURROW_MCONTEXT_ASM)
    /* Not in Go. Only the hand-written switch moves the stack pointer in the
     * middle of a function. The others are a C function that calls into the C
     * library to switch, and a sample in it has a whole stack, as one in any
     * other function does. */
    testing_t_skip_v(t, "the goroutine switch is not the hand-written one");
#endif
    /* How much to try. These defaults take about 1 seconds on a 2012 MacBook
     * Pro. The ones in short mode take about 0.1 seconds. */
    int tries = 10;
    Int count = 1000000;
    if (testing_short())
        tries = 1;
    for (int try_ = 0; try_ < tries; try_++) {
        Arena ar;
        arena_init(&ar, NULL, 0);
        Alloc *a = arena_allocator(&ar);
        BytesBuffer prof = BYTES_BUFFER(a);
        Error err = pprof_start_cpu_profile(bytes_buffer_as_io_writer(&prof));
        if (BURROW_FAILED(err))
            testing_t_fatal_v(t, err);
        for (Int i = 0; i < count; i++)
            runtime_gosched();
        pprof_stop_cpu_profile();

        /* Read profile to look for entries for gogo with an attempt at a
         * traceback. */
        SwitchCheck c = {t, a};
        (void)parse_profile(t, a, &prof, check_no_switch, &c);
        arena_free(&ar);
    }
}

static void big_divide(void *env, Duration duration) {
    (void)env;
    Alloc *heap = heap_allocator();
    Chan *t = time_after_chan(heap, duration);
    if (t == NULL)
        panic_str(S("out of memory"));
    BigInt pi = BIG_INT(heap);
    for (;;) {
        for (int i = 0; i < 100; i++) {
            BigInt n = BIG_INT(heap);
            BigInt d = BIG_INT(heap);
            (void)big_int_set_int64(&n, 2646693125139304345);
            (void)big_int_set_int64(&d, 842468587426513207);
            (void)big_int_div(&pi, &n, &d);
            big_int_free(&n);
            big_int_free(&d);
        }
        Time tm;
        bool ok;
        if (chan_try_recv(t, &tm, &ok))
            break;
    }
    big_int_free(&pi);
    chan_free(t);
}

/* Test that profiling of division operations is okay, especially on ARM. See
 * issue 6681. */
static void TestMathBigDivide(TestingT *t) {
    Arena ar;
    arena_init(&ar, NULL, 0);
    (void)test_cpu_profile(t, arena_allocator(&ar), NULL, big_divide, NULL);
    arena_free(&ar);
}

static void hog_labelled(void *env, Context ctx) {
    (void)ctx;
    cpu_hogger(cpu_hog1, &salt1, *(const Duration *)env);
}

typedef struct LabelHog {
    Alloc *a;
} LabelHog;

static void hog_with_label(void *env, Duration dur) {
    LabelHog *h = (LabelHog *)env;
    pprof_do(h->a, context_background(), pprof_labels_v(h->a, 2, S("key"), S("value")),
             BURROW_FN(PprofDoFunc, hog_labelled, &dur));
}

static void TestCPUProfileLabel(TestingT *t) {
    /* The label map goes into the profile as a pointer and is read when the
     * profile is written, so it comes from an arena that lasts until then. */
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    static const char *const need[] = {"burrow__adler32_update;key=value"};
    ProfileMatch matches = {stack_contains_labeled, need, 1};
    LabelHog h = {a};
    (void)test_cpu_profile(t, a, &matches, hog_with_label, &h);
    arena_free(&ar);
}

/* ------------------------------------------------------------ protomem */

typedef struct ConvertMemEnv {
    const burrow__MemProfileRecord *rec;
    Int nrec;
    int64_t rate;
    const WantSample *samples;
    Int nsamples;
    Str default_sample_type;
} ConvertMemEnv;

static const Str mem_sample_type[] = {
    BURROW_S_INIT("alloc_objects"), BURROW_S_INIT("count"),
    BURROW_S_INIT("alloc_space"),   BURROW_S_INIT("bytes"),
    BURROW_S_INIT("inuse_objects"), BURROW_S_INIT("count"),
    BURROW_S_INIT("inuse_space"),   BURROW_S_INIT("bytes"),
};

static void convert_mem_profile(void *env, TestingT *t) {
    const ConvertMemEnv *e = (const ConvertMemEnv *)env;
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);

    BytesBuffer buf = BYTES_BUFFER(a);
    Error err = burrow__ppr_write_heap_proto(bytes_buffer_as_io_writer(&buf), e->rec,
                                             e->nrec, e->rate, e->default_sample_type);
    if (BURROW_FAILED(err))
        testing_t_fatalf_v(t, "writing profile: %v", err);

    PfProfile *p = burrow__pf_parse(a, bytes_buffer_as_io_reader(&buf), &err);
    if (BURROW_FAILED(err))
        testing_t_fatalf_v(t, "profile.Parse: %v", err);

    check_profile(t, p, e->rate, S("space"), S("bytes"), mem_sample_type, 4, e->samples,
                  e->nsamples, e->default_sample_type);
    arena_free(&ar);
}

static void TestConvertMemProfile(TestingT *t) {
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);

    uint64_t addr1, addr2;
    PfMapping maps[2];
    if (!test_pcs(t, a, &addr1, &addr2, maps)) {
        arena_free(&ar);
        return;
    }
    const PfMapping *map1 = &maps[0], *map2 = &maps[1];

    /* MemProfileRecord stacks are return PCs, so add one to the addresses
     * recorded in the "profile". The proto profile locations are call PCs, so
     * conversion will subtract one from these and get back to addr1 and
     * addr2. */
    Uintptr a1 = (Uintptr)addr1 + 1, a2 = (Uintptr)addr2 + 1;
    int64_t rate = 512 * 1024;
    const Uintptr stk0[] = {a1, a2};
    const Uintptr stk1[] = {a2 + 1, a2 + 2};
    const Uintptr stk2[] = {a1 + 1, a1 + 2, a2 + 3};
    const burrow__MemProfileRecord rec[] = {
        {.object_size = 1024,
         .alloc_objects = 4,
         .free_objects = 1,
         .stack = stk0,
         .nstack = 2},
        {.object_size = 512 * 1024,
         .alloc_objects = 1,
         .free_objects = 0,
         .stack = stk1,
         .nstack = 2},
        {.object_size = 512 * 1024,
         .alloc_objects = 1,
         .free_objects = 1,
         .stack = stk2,
         .nstack = 3},
    };

    const WantSample samples[] = {
        {
            .value = {2050, 2099200, 1537, 1574400},
            .location = {{1, map1, addr1}, {2, map2, addr2}},
            .bytes = 1024,
        },
        {
            .value = {1, 829411, 1, 829411},
            .location = {{3, map2, addr2 + 1}, {4, map2, addr2 + 2}},
            .bytes = 512 * 1024,
        },
        {
            .value = {1, 829411, 0, 0},
            .location = {{5, map1, addr1 + 1},
                         {6, map1, addr1 + 2},
                         {7, map2, addr2 + 3}},
            .bytes = 512 * 1024,
        },
    };
    static const struct {
        const char *name;
        const char *default_sample_type;
    } tcs[] = {
        {"heap", ""},
        {"allocs", "alloc_space"},
    };
    for (size_t i = 0; i < NELEM(tcs); i++) {
        ConvertMemEnv e = {
            rec,     NELEM(rec),     rate,
            samples, NELEM(samples), str_from_cstr(tcs[i].default_sample_type)};
        testing_t_run(t, str_from_cstr(tcs[i].name),
                      BURROW_FN(TestingTFunc, convert_mem_profile, &e));
    }
    arena_free(&ar);
}

/* -------------------------------------------------------------- mprof */

/* Not in Go. memSink is where Go's allocations go to be dropped, and the
 * collector frees what it held. Here what it held is freed when it is
 * replaced. */
static void *mem_sink;
static size_t mem_sink_size;

static void mem_sink_set(void *p, size_t size) {
    if (mem_sink != NULL)
        mem_free(heap_allocator(), mem_sink, mem_sink_size, 1);
    mem_sink = p;
    mem_sink_size = size;
}

static void *mem_sink_alloc(void *p) {
    if (p == NULL)
        panic_str(S("out of memory"));
    return p;
}

BURROW_NOINLINE static void allocate_transient_1m(void) {
    for (int i = 0; i < 1024; i++)
        mem_sink_set(mem_sink_alloc(mem_alloc(heap_allocator(), 1024, 1)), 1024);
}

BURROW_NOINLINE static void allocate_transient_2m(void) {
    mem_sink_set(mem_sink_alloc(mem_alloc(heap_allocator(), 2 << 20, 1)), 2 << 20);
}

static inline void allocate_transient_2m_inline(void) {
    mem_sink_set(mem_sink_alloc(mem_alloc(heap_allocator(), 2 << 20, 1)), 2 << 20);
}

typedef struct Obj32 {
    struct Obj32 *link;
    Byte pad[32 - sizeof(Uintptr)];
} Obj32;

static Obj32 *persistent_mem_sink;

BURROW_NOINLINE static void allocate_persistent_1k(void) {
    for (int i = 0; i < 32; i++) {
        /* Can't use slice because that will introduce implicit allocations. */
        Obj32 *obj = (Obj32 *)mem_sink_alloc(BURROW_NEW(heap_allocator(), Obj32));
        obj->link = persistent_mem_sink;
        persistent_mem_sink = obj;
    }
}

static int memory_profiler_run = 0;

/* What Go's test looks for, with the stacks left out. The functions in a test
 * have no names here, so a record is told by its counts instead, and the
 * frame printed first has no name, which says the allocator's own frames were
 * hidden. */
typedef struct MemProfilerTest {
    int64_t value[4]; /* objects and bytes allocated, and in use */
    Str legacy;
} MemProfilerTest;

typedef struct MemProfilerEnv {
    const MemProfilerTest *tests;
    Int n;
} MemProfilerEnv;

static void memory_profiler_debug1(void *env, TestingT *t) {
    const MemProfilerEnv *e = (const MemProfilerEnv *)env;
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    BytesBuffer buf = BYTES_BUFFER(a);
    Error err = pprof_profile_write_to(pprof_lookup(S("heap")),
                                       bytes_buffer_as_io_writer(&buf), 1);
    if (BURROW_FAILED(err))
        testing_t_fatalf_v(t, "failed to write heap profile: %v", err);

    for (Int i = 0; i < e->n; i++) {
        Regexp *re = regexp_must_compile(a, e->tests[i].legacy);
        bool ok = regexp_match(re, bytes_buffer_bytes(&buf));
        regexp_free(re);
        if (!ok)
            testing_t_fatalf_v(t, "The entry did not match:\n%v\n\nProfile:\n%v\n",
                               e->tests[i].legacy, bytes_buffer_string(&buf, a));
    }
    arena_free(&ar);
}

static bool contains_sample(PfProfile *p, const int64_t value[4]) {
    PfSample **ps = (PfSample **)p->sample.p;
    for (Int i = 0; i < p->sample.len; i++) {
        const PfSample *s = ps[i];
        const int64_t *v = (const int64_t *)s->value.p;
        if (s->value.len != 4 || s->location.len == 0 ||
            memcmp(v, value, 4 * sizeof(int64_t)) != 0)
            continue;
        const PfLocation *l = ((PfLocation *const *)s->location.p)[0];
        if (location_name(l).len == 0)
            return true;
    }
    return false;
}

static void memory_profiler_proto(void *env, TestingT *t) {
    const MemProfilerEnv *e = (const MemProfilerEnv *)env;
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    BytesBuffer buf = BYTES_BUFFER(a);
    Error err = pprof_profile_write_to(pprof_lookup(S("heap")),
                                       bytes_buffer_as_io_writer(&buf), 0);
    if (BURROW_FAILED(err))
        testing_t_fatalf_v(t, "failed to write heap profile: %v", err);
    PfProfile *p = burrow__pf_parse(a, bytes_buffer_as_io_reader(&buf), &err);
    if (BURROW_FAILED(err))
        testing_t_fatalf_v(t, "failed to parse heap profile: %v", err);

    for (Int i = 0; i < e->n; i++) {
        if (!contains_sample(p, e->tests[i].value))
            testing_t_fatalf_v(t, "No matching stack entry for %v\n\nProfile:\n%s\n",
                               e->tests[i].legacy, burrow__pf_profile_string(p, a));
    }
    arena_free(&ar);
}

static void restore_mem_profile_rate(void *env) {
    runtime_set_mem_profile_rate(*(const Int *)env);
}

static void TestMemoryProfiler(TestingT *t) {
    /* Not in Go. Every record is matched by its stack, and on a system where
     * runtime_callers cannot walk the stack, wasip1 among them, they all have
     * the same empty one. */
    Uintptr pcs[4];
    if (runtime_callers(1, slice_from(pcs, 4, 4, TYPE_UINTPTR)) == 0) {
        testing_t_skip_v(t, "no stacks to tell the allocations apart by");
        return;
    }

    /* Disable sampling, otherwise it's difficult to assert anything. */
    static Int old_rate;
    old_rate = runtime_mem_profile_rate();
    runtime_set_mem_profile_rate(1);
    testing_t_cleanup(t, BURROW_FN(Func, restore_mem_profile_rate, &old_rate));

    /* Allocate a meg to ensure that mcache.nextSample is updated to 1. */
    for (int i = 0; i < 1024; i++)
        mem_sink_set(mem_sink_alloc(mem_alloc(heap_allocator(), 1024, 1)), 1024);

    /* Do the interesting allocations. */
    allocate_transient_1m();
    allocate_transient_2m();
    allocate_transient_2m_inline();
    allocate_persistent_1k();
    /* Go's allocateReflect calls a function through reflect.Call, which has
     * nothing like it here, so it is left out. */
    mem_sink_set(NULL, 0);

    memory_profiler_run++;
    int64_t r = memory_profiler_run;

    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    const MemProfilerTest tests[] = {
        {
            {32 * r, 1024 * r, 32 * r, 1024 * r},
            fmt_sprintf_v(a, "%v: %v \\[%v: %v\\] @( 0x[0-9,a-f]+)+\n#\t0x[0-9,a-f]+\n",
                          32 * r, 1024 * r, 32 * r, 1024 * r),
        },
        {
            {(1 << 10) * r, (1 << 20) * r, 0, 0},
            fmt_sprintf_v(a, "0: 0 \\[%v: %v\\] @( 0x[0-9,a-f]+)+\n#\t0x[0-9,a-f]+\n",
                          (1 << 10) * r, (1 << 20) * r),
        },
        {
            {r, (2 << 20) * r, 0, 0},
            fmt_sprintf_v(a, "0: 0 \\[%v: %v\\] @( 0x[0-9,a-f]+)+\n#\t0x[0-9,a-f]+\n",
                          r, (2 << 20) * r),
        },
    };
    MemProfilerEnv e = {tests, NELEM(tests)};

    testing_t_run(t, S("debug=1"), BURROW_FN(TestingTFunc, memory_profiler_debug1, &e));
    testing_t_run(t, S("proto"), BURROW_FN(TestingTFunc, memory_profiler_proto, &e));
    arena_free(&ar);
}

/* ------------------------------------------------ block and mutex profiles */

/* Go's profileStacks flattens each sample to the names of its lines. A
 * location with no name has no line, so here it is kept as an empty name
 * instead of being left out, since a test function has no name and its place
 * in the stack is what there is to check. */
static Slice profile_stacks(Alloc *a, PfProfile *p) {
    Slice res = slice_make(a, TYPE_UNSAFE_POINTER, 0, p->sample.len);
    PfSample **ps = (PfSample **)p->sample.p;
    for (Int i = 0; i < p->sample.len; i++) {
        PfLocation **ls = (PfLocation **)ps[i]->location.p;
        Slice *stk = (Slice *)mem_alloc(a, sizeof(Slice), _Alignof(Slice));
        *stk = slice_make(a, TYPE_STRING, 0, ps[i]->location.len);
        for (Int j = 0; j < ps[i]->location.len; j++) {
            Str name = location_name(ls[j]);
            *stk = slice_append(a, *stk, &name, 1);
        }
        res = slice_append(a, res, &stk, 1);
    }
    return res;
}

/* blockRecordStacks, with the same empty name for a frame that has none. */
static Slice block_record_stacks(Alloc *a, const RuntimeBlockProfileRecord *records,
                                 Int n) {
    Slice res = slice_make(a, TYPE_UNSAFE_POINTER, 0, n);
    for (Int i = 0; i < n; i++) {
        Int nstk = runtime_block_profile_record_stack(&records[i]);
        Slice *stk = (Slice *)mem_alloc(a, sizeof(Slice), _Alignof(Slice));
        *stk = slice_make(a, TYPE_STRING, 0, nstk);
        for (Int j = 0; j < nstk; j++) {
            RuntimeFrame frame;
            (void)burrow__ppr_frame(records[i].stack0[j], &frame);
            *stk = slice_append(a, *stk, &frame.function, 1);
        }
        res = slice_append(a, res, &stk, 1);
    }
    return res;
}

static bool contains_stack(Slice got, const Str *want, Int nwant) {
    Slice *const *stks = (Slice *const *)got.p;
    for (Int i = 0; i < got.len; i++) {
        const Slice *stk = stks[i];
        if (stk->len < nwant)
            continue;
        const Str *names = (const Str *)stk->p;
        for (Int j = 0; j < nwant; j++) {
            if (!str_eq(want[j], names[j]))
                break;
            if (j == nwant - 1)
                return true;
        }
    }
    return false;
}

/* Not in Go: the number of goroutines a stack dump shows in the given state,
 * which is what await_blocked_goroutine counts with. */
static Int goroutines_in_state(Str state) {
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    Regexp *r =
        regexp_must_compile(a, fmt_sprintf_v(a, "(?m)^goroutine \\d+ \\[%s\\]:\n",
                                             regexp_quote_meta(a, state)));
    Int size = 64 << 10;
    Int got = 0;
    for (;;) {
        Byte *buf = (Byte *)mem_alloc(a, (size_t)size, 1);
        if (buf == NULL)
            panic_str(S("out of memory"));
        Int n = runtime_stack(slice_from(buf, size, size, TYPE_BYTE), true);
        if (n == size) {
            /* Buffer wasn't large enough for a full goroutine dump. Resize it
             * and try again. */
            size *= 2;
            continue;
        }
        got = regexp_find_all(r, a, slice_from(buf, n, n, TYPE_BYTE), -1).len;
        break;
    }
    regexp_free(r);
    arena_free(&ar);
    return got;
}

/* awaitBlockedGoroutine spins on runtime_gosched until a runtime stack dump
 * shows count goroutines in the given state. Go looks for the test's own frame
 * in the goroutine, which here has no name, and a goroutine that is not
 * running only has frames where the scheduler's switch leaves one to start
 * from. So the caller counts the goroutines already in the state first, and
 * count is that and the ones it is waiting for. */
static void await_blocked_goroutine(Str state, Int count) {
    for (;;) {
        runtime_gosched();
        if (goroutines_in_state(state) >= count)
            return;
    }
}

/* What a blockXxx function shares with the goroutine it starts. The goroutine
 * says when it is done, since what it shares is on the test's stack. */
typedef struct BlockEnv {
    Chan *c;
    Chan *c2;
    SyncMutex mu;
    SyncCond cond;
    Str state;
    Int count;
    Int n;
    Duration d;
    SyncWaitGroup wg;
    SyncWaitGroup done;
} BlockEnv;

static void block_go(TestingT *t, BlockEnv *e, Str state, Int n, void (*f)(void *env)) {
    e->state = state;
    e->count = goroutines_in_state(state) + n;
    sync_wait_group_add(&e->done, 1);
    if (!go(BURROW_FN(Func, f, e)))
        testing_t_fatalf_v(t, "go failed");
}

static void block_wait(BlockEnv *e) {
    sync_wait_group_wait(&e->done);
    if (e->c != NULL)
        chan_free(e->c);
    if (e->c2 != NULL)
        chan_free(e->c2);
}

static void block_chan_recv_func1(void *env) {
    BlockEnv *e = (BlockEnv *)env;
    await_blocked_goroutine(e->state, e->count);
    bool v = true;
    chan_send(e->c, &v);
    sync_wait_group_done(&e->done);
}

static BURROW_NOINLINE void block_chan_recv(TestingT *t) {
    BlockEnv e = {0};
    e.c = chan_make(heap_allocator(), TYPE_BOOL, 0);
    block_go(t, &e, S("chan receive"), 1, block_chan_recv_func1);
    bool v;
    (void)chan_recv(e.c, &v);
    block_wait(&e);
}

static void block_chan_send_func1(void *env) {
    BlockEnv *e = (BlockEnv *)env;
    await_blocked_goroutine(e->state, e->count);
    bool v;
    (void)chan_recv(e->c, &v);
    sync_wait_group_done(&e->done);
}

static BURROW_NOINLINE void block_chan_send(TestingT *t) {
    BlockEnv e = {0};
    e.c = chan_make(heap_allocator(), TYPE_BOOL, 0);
    block_go(t, &e, S("chan send"), 1, block_chan_send_func1);
    bool v = true;
    chan_send(e.c, &v);
    block_wait(&e);
}

static void block_chan_close_func1(void *env) {
    BlockEnv *e = (BlockEnv *)env;
    await_blocked_goroutine(e->state, e->count);
    chan_close(e->c);
    sync_wait_group_done(&e->done);
}

static BURROW_NOINLINE void block_chan_close(TestingT *t) {
    BlockEnv e = {0};
    e.c = chan_make(heap_allocator(), TYPE_BOOL, 0);
    block_go(t, &e, S("chan receive"), 1, block_chan_close_func1);
    bool v;
    (void)chan_recv(e.c, &v);
    block_wait(&e);
}

enum { NUM_TRIES = 3 };

static void block_select_recv_async_func1(void *env) {
    BlockEnv *e = (BlockEnv *)env;
    for (int i = 0; i < NUM_TRIES; i++) {
        await_blocked_goroutine(e->state, e->count);
        bool v = true;
        chan_send(e->c, &v);
    }
    sync_wait_group_done(&e->done);
}

static BURROW_NOINLINE void block_select_recv_async(TestingT *t) {
    BlockEnv e = {0};
    e.c = chan_make(heap_allocator(), TYPE_BOOL, 1);
    e.c2 = chan_make(heap_allocator(), TYPE_BOOL, 1);
    block_go(t, &e, S("select"), 1, block_select_recv_async_func1);
    for (int i = 0; i < NUM_TRIES; i++) {
        SelectCase cases[] = {BURROW_RECV(e.c, NULL), BURROW_RECV(e.c2, NULL)};
        (void)chan_select(cases, 2);
    }
    block_wait(&e);
}

static void block_select_send_sync_func1(void *env) {
    BlockEnv *e = (BlockEnv *)env;
    await_blocked_goroutine(e->state, e->count);
    bool v;
    (void)chan_recv(e->c, &v);
    sync_wait_group_done(&e->done);
}

static BURROW_NOINLINE void block_select_send_sync(TestingT *t) {
    BlockEnv e = {0};
    e.c = chan_make(heap_allocator(), TYPE_BOOL, 0);
    e.c2 = chan_make(heap_allocator(), TYPE_BOOL, 0);
    block_go(t, &e, S("select"), 1, block_select_send_sync_func1);
    bool v = true;
    SelectCase cases[] = {BURROW_SEND(e.c, &v), BURROW_SEND(e.c2, &v)};
    (void)chan_select(cases, 2);
    block_wait(&e);
}

static void block_mutex_func1(void *env) {
    BlockEnv *e = (BlockEnv *)env;
    await_blocked_goroutine(e->state, e->count);
    sync_mutex_unlock(&e->mu);
    sync_wait_group_done(&e->done);
}

static BURROW_NOINLINE void block_mutex(TestingT *t) {
    BlockEnv e = {0};
    sync_mutex_lock(&e.mu);
    block_go(t, &e, S("sync.Mutex.Lock"), 1, block_mutex_func1);
    /* Note: Unlock releases mu before recording the mutex event, so it's
     * theoretically possible for this to proceed and capture the profile
     * before the event is recorded. As long as this is blocked before the
     * unlock happens, it's okay. */
    sync_mutex_lock(&e.mu);
    block_wait(&e);
}

static void block_mutex_n_func1(void *env) {
    BlockEnv *e = (BlockEnv *)env;
    await_blocked_goroutine(e->state, e->count);
    time_sleep(e->d);
    sync_mutex_unlock(&e->mu);
    sync_wait_group_done(&e->done);
}

static void block_mutex_n_func2(void *env) {
    BlockEnv *e = (BlockEnv *)env;
    sync_mutex_lock(&e->mu);
    sync_mutex_unlock(&e->mu);
    sync_wait_group_done(&e->wg);
}

static BURROW_NOINLINE void block_mutex_n(TestingT *t, Int n, Duration d) {
    BlockEnv e = {0};
    e.d = d;
    sync_mutex_lock(&e.mu);
    block_go(t, &e, S("sync.Mutex.Lock"), n, block_mutex_n_func1);
    /* Note: Unlock releases mu before recording the mutex event, so it's
     * theoretically possible for this to proceed and capture the profile
     * before the event is recorded. As long as this is blocked before the
     * unlock happens, it's okay. */
    for (Int i = 0; i < n; i++) {
        sync_wait_group_add(&e.wg, 1);
        if (!go(BURROW_FN(Func, block_mutex_n_func2, &e)))
            testing_t_fatalf_v(t, "go failed");
    }
    sync_wait_group_wait(&e.wg);
    block_wait(&e);
}

static void block_cond_func1(void *env) {
    BlockEnv *e = (BlockEnv *)env;
    await_blocked_goroutine(e->state, e->count);
    sync_mutex_lock(&e->mu);
    sync_cond_signal(&e->cond);
    sync_mutex_unlock(&e->mu);
    sync_wait_group_done(&e->done);
}

static BURROW_NOINLINE void block_cond(TestingT *t) {
    BlockEnv e = {0};
    e.cond = SYNC_COND(sync_mutex_locker(&e.mu));
    sync_mutex_lock(&e.mu);
    block_go(t, &e, S("sync.Cond.Wait"), 1, block_cond_func1);
    sync_cond_wait(&e.cond);
    sync_mutex_unlock(&e.mu);
    block_wait(&e);
}

/* Go names each stack's test functions, blockChanRecv and TestBlockProfile. A
 * test function has no name here, so those two frames are only checked to be
 * there, with no name, under the function that blocked. */
typedef struct BlockProfileCase {
    Str name;
    void (*f)(TestingT *t);
    Str stk[3];
    Str re;
} BlockProfileCase;

#define BLOCK_RE(fn)                                                                   \
    BURROW_S_INIT("\n[0-9]+ [0-9]+ @( 0x[[:xdigit:]]+)+\n"                             \
                  "#\t+0x[0-9a-f]+\t+" fn "\\+0x[0-9a-f]+\t+.*:[0-9]+\n"               \
                  "#\t+0x[0-9a-f]+\n"                                                  \
                  "#\t+0x[0-9a-f]+\n")

static const BlockProfileCase block_profile_tests[] = {
    {
        BURROW_S_INIT("chan recv"),
        block_chan_recv,
        {BURROW_S_INIT("chan_recv"), BURROW_S_INIT(""), BURROW_S_INIT("")},
        BLOCK_RE("chan_recv"),
    },
    {
        BURROW_S_INIT("chan send"),
        block_chan_send,
        {BURROW_S_INIT("chan_send"), BURROW_S_INIT(""), BURROW_S_INIT("")},
        BLOCK_RE("chan_send"),
    },
    {
        BURROW_S_INIT("chan close"),
        block_chan_close,
        {BURROW_S_INIT("chan_recv"), BURROW_S_INIT(""), BURROW_S_INIT("")},
        BLOCK_RE("chan_recv"),
    },
    {
        BURROW_S_INIT("select recv async"),
        block_select_recv_async,
        {BURROW_S_INIT("chan_select"), BURROW_S_INIT(""), BURROW_S_INIT("")},
        BLOCK_RE("chan_select"),
    },
    {
        BURROW_S_INIT("select send sync"),
        block_select_send_sync,
        {BURROW_S_INIT("chan_select"), BURROW_S_INIT(""), BURROW_S_INIT("")},
        BLOCK_RE("chan_select"),
    },
    {
        BURROW_S_INIT("mutex"),
        block_mutex,
        {BURROW_S_INIT("burrow__sync_mutex_lock_slow"), BURROW_S_INIT(""),
         BURROW_S_INIT("")},
        BLOCK_RE("burrow__sync_mutex_lock_slow"),
    },
    {
        BURROW_S_INIT("cond"),
        block_cond,
        {BURROW_S_INIT("sync_cond_wait"), BURROW_S_INIT(""), BURROW_S_INIT("")},
        BLOCK_RE("sync_cond_wait"),
    },
};

static void block_profile_debug1(void *env, TestingT *t) {
    (void)env;
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    StringsBuilder w = STRINGS_BUILDER(a);
    (void)pprof_profile_write_to(pprof_lookup(S("block")),
                                 strings_builder_as_io_writer(&w), 1);
    Str prof = strings_builder_string(&w);

    if (!strings_has_prefix(prof, S("--- contention:\ncycles/second=")))
        testing_t_fatalf_v(t, "Bad profile header:\n%v", prof);

    if (strings_has_suffix(prof, S("#\t0x0\n\n")))
        testing_t_errorf_v(t, "Useless 0 suffix:\n%v", prof);

    for (Int i = 0; i < NELEM(block_profile_tests); i++) {
        const BlockProfileCase *test = &block_profile_tests[i];
        Regexp *re = regexp_must_compile(a, test->re);
        if (!regexp_match_string(re, prof))
            testing_t_errorf_v(t, "Bad %v entry, expect:\n%v\ngot:\n%v", test->name,
                               test->re, prof);
        regexp_free(re);
    }
    arena_free(&ar);
}

static void block_profile_proto(void *env, TestingT *t) {
    (void)env;
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    /* proto format */
    BytesBuffer w = BYTES_BUFFER(a);
    (void)pprof_profile_write_to(pprof_lookup(S("block")),
                                 bytes_buffer_as_io_writer(&w), 0);
    Error err = BURROW_NO_ERROR;
    PfProfile *p = burrow__pf_parse(a, bytes_buffer_as_io_reader(&w), &err);
    if (BURROW_FAILED(err))
        testing_t_fatalf_v(t, "failed to parse profile: %v", err);
    testing_t_logf_v(t, "parsed proto: %s", burrow__pf_profile_string(p, a));
    err = burrow__pf_profile_check_valid(p);
    if (BURROW_FAILED(err))
        testing_t_fatalf_v(t, "invalid profile: %v", err);

    Slice stks = profile_stacks(a, p);
    for (Int i = 0; i < NELEM(block_profile_tests); i++) {
        const BlockProfileCase *test = &block_profile_tests[i];
        if (!contains_stack(stks, test->stk, 3))
            testing_t_errorf_v(t, "No matching stack entry for %v, want %v %v %v",
                               test->name, test->stk[0], test->stk[1], test->stk[2]);
    }
    arena_free(&ar);
}

static void reset_block_profile_rate(void *env) {
    (void)env;
    runtime_set_block_profile_rate(0);
}

static void TestBlockProfile(TestingT *t) {
    /* Not in Go. The stacks are what is checked, and without a walk every one
     * of them is empty. */
    if (!have_walk()) {
        testing_t_skip_v(t, "no stacks to check");
        return;
    }

    /* Generate block profile */
    runtime_set_block_profile_rate(1);
    testing_t_cleanup(t, BURROW_FN(Func, reset_block_profile_rate, NULL));
    for (Int i = 0; i < NELEM(block_profile_tests); i++)
        block_profile_tests[i].f(t);

    testing_t_run(t, S("debug=1"), BURROW_FN(TestingTFunc, block_profile_debug1, NULL));
    testing_t_run(t, S("proto"), BURROW_FN(TestingTFunc, block_profile_proto, NULL));
}

/* blockFrequentShort produces 100000 block events with an average duration of
 * rate / 10. */
static BURROW_NOINLINE void block_frequent_short(Int rate) {
    for (int i = 0; i < 100000; i++)
        burrow__blockevent((int64_t)(rate / 10), 1);
}

/* blockInfrequentLong produces 10000 block events with an average duration of
 * rate. */
static BURROW_NOINLINE void block_infrequent_long(Int rate) {
    for (int i = 0; i < 10000; i++)
        burrow__blockevent((int64_t)rate, 1);
}

/* Not in Go: which of the two functions above the address is in, since
 * neither has a name to look for. It is the one that starts nearest below it,
 * or 0 for an address below both. */
static int block_bias_func(Uintptr addr) {
    Uintptr fs = (Uintptr)block_frequent_short;
    Uintptr il = (Uintptr)block_infrequent_long;
    if (addr >= fs && (addr < il || fs > il))
        return 1;
    if (addr >= il)
        return 2;
    return 0;
}

/* See http://golang.org/cl/299991. */
static void TestBlockProfileBias(TestingT *t) {
    if (!have_walk()) {
        testing_t_skip_v(t, "no stacks to tell the two functions apart by");
        return;
    }

    Int rate = 1000; /* arbitrary value */
    runtime_set_block_profile_rate(rate);
    testing_t_cleanup(t, BURROW_FN(Func, reset_block_profile_rate, NULL));

    /* simulate blocking events */
    block_frequent_short(rate);
    block_infrequent_long(rate);

    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    BytesBuffer w = BYTES_BUFFER(a);
    (void)pprof_profile_write_to(pprof_lookup(S("block")),
                                 bytes_buffer_as_io_writer(&w), 0);
    Error err = BURROW_NO_ERROR;
    PfProfile *p = burrow__pf_parse(a, bytes_buffer_as_io_reader(&w), &err);
    if (BURROW_FAILED(err))
        testing_t_fatalf_v(t, "failed to parse profile: %v", err);
    testing_t_logf_v(t, "parsed proto: %s", burrow__pf_profile_string(p, a));

    double il = -1; /* blockInfrequentLong duration */
    double fs = -1; /* blockFrequentShort duration */
    PfSample **ps = (PfSample **)p->sample.p;
    for (Int i = 0; i < p->sample.len; i++) {
        const PfSample *s = ps[i];
        if (s->value.len < 2)
            testing_t_fatal_v(t, "block profile has less than 2 sample types");
        /* The first location is the function that called blockevent, and only
         * these two have one with no name. */
        if (s->location.len == 0)
            continue;
        const PfLocation *l = ((PfLocation *const *)s->location.p)[0];
        if (location_name(l).len != 0)
            continue;
        int64_t v = ((const int64_t *)s->value.p)[1];
        switch (block_bias_func((Uintptr)l->address)) {
        case 1:
            fs = (double)v;
            break;
        case 2:
            il = (double)v;
            break;
        default:
            break;
        }
    }
    if (il == -1 || fs == -1)
        testing_t_fatal_v(t, "block profile is missing expected functions");

    /* stddev of bias from 100 runs on local machine multiplied by 10x */
    const double threshold = 0.2;
    double bias = (il - fs) / il;
    if (math_abs(bias) > threshold)
        testing_t_fatalf_v(t, "bias: abs(%f) > %f", bias, threshold);
    else
        testing_t_logf_v(t, "bias: abs(%f) < %f", bias, threshold);
    arena_free(&ar);
}

enum { MUTEX_N = 100 };
#define MUTEX_D (100 * TIME_MILLISECOND)

typedef struct MutexProfileEnv {
    Duration block_mutex_n_time;
} MutexProfileEnv;

static void mutex_profile_debug1(void *env, TestingT *t) {
    (void)env;
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    StringsBuilder w = STRINGS_BUILDER(a);
    (void)pprof_profile_write_to(pprof_lookup(S("mutex")),
                                 strings_builder_as_io_writer(&w), 1);
    Str prof = strings_builder_string(&w);
    testing_t_logf_v(t, "received profile: %v", prof);

    if (!strings_has_prefix(prof, S("--- mutex:\ncycles/second=")))
        testing_t_errorf_v(t, "Bad profile header:\n%v", prof);
    prof = strings_trim(prof, S("\n"));
    Slice lines = strings_split(a, prof, S("\n"));
    if (lines.len < 6)
        testing_t_fatalf_v(t, "expected >=6 lines, got %d %q\n%s", lines.len, prof,
                           prof);
    const Str *l = (const Str *)lines.p;
    /* checking that the line is like "35258904 1 @ 0x48288d 0x47cd28 0x458931" */
    Str r2 = S("^\\d+ \\d+ @(?: 0x[[:xdigit:]]+)+");
    Regexp *re2 = regexp_must_compile(a, r2);
    if (!regexp_match_string(re2, l[3]))
        testing_t_errorf_v(t, "%q didn't match %q", l[3], r2);
    regexp_free(re2);
    /* Go's line 5 is the frame of blockMutexN.func1, which here has no name, so
     * line 4 is checked to be the unlock it called. */
    Str r4 = S("^#.*burrow__sync_mutex_unlock_slow.*$");
    Regexp *re4 = regexp_must_compile(a, r4);
    if (!regexp_match_string(re4, l[4]))
        testing_t_errorf_v(t, "%q didn't match %q", l[4], r4);
    regexp_free(re4);
    Str r3 = S("^#\t+0x[[:xdigit:]]+$");
    Regexp *re3 = regexp_must_compile(a, r3);
    if (!regexp_match_string(re3, l[5]))
        testing_t_errorf_v(t, "%q didn't match %q", l[5], r3);
    regexp_free(re3);
    testing_t_log_v(t, prof);
    arena_free(&ar);
}

static void mutex_profile_proto(void *env, TestingT *t) {
    const MutexProfileEnv *e = (const MutexProfileEnv *)env;
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    /* proto format */
    BytesBuffer w = BYTES_BUFFER(a);
    (void)pprof_profile_write_to(pprof_lookup(S("mutex")),
                                 bytes_buffer_as_io_writer(&w), 0);
    Error err = BURROW_NO_ERROR;
    PfProfile *p = burrow__pf_parse(a, bytes_buffer_as_io_reader(&w), &err);
    if (BURROW_FAILED(err))
        testing_t_fatalf_v(t, "failed to parse profile: %v", err);
    testing_t_logf_v(t, "parsed proto: %s", burrow__pf_profile_string(p, a));
    err = burrow__pf_profile_check_valid(p);
    if (BURROW_FAILED(err))
        testing_t_fatalf_v(t, "invalid profile: %v", err);

    Slice stks = profile_stacks(a, p);
    const Str want[] = {S("burrow__sync_mutex_unlock_slow"), S("")};
    if (!contains_stack(stks, want, NELEM(want)))
        testing_t_errorf_v(t, "No matching stack entry for %v %v", want[0], want[1]);

    PfValueType **st = (PfValueType **)p->sample_type.p;
    Int i = 0;
    for (; i < p->sample_type.len; i++) {
        if (str_eq(st[i]->unit, S("nanoseconds")))
            break;
    }
    if (i >= p->sample_type.len)
        testing_t_fatalf_v(t, "profile did not contain nanoseconds sample");
    int64_t total = 0;
    PfSample **ps = (PfSample **)p->sample.p;
    for (Int j = 0; j < p->sample.len; j++)
        total += ((const int64_t *)ps[j]->value.p)[i];
    /* Want d to be at least N*D, but give some wiggle-room to avoid a test
     * flaking. Set an upper-bound proportional to the total wall time spent in
     * blockMutexN. Generally speaking, the total contention time could be
     * arbitrarily high when considering OS scheduler delays, or any other
     * delays from the environment: time keeps ticking during these delays. By
     * making the upper bound proportional to the wall time in blockMutexN, in
     * theory we're accounting for all these possible delays. */
    Duration d = (Duration)total;
    Duration lo = (Duration)(MUTEX_N * MUTEX_D * 9 / 10);
    Duration hi = (Duration)MUTEX_N * e->block_mutex_n_time * 11 / 10;
    if (d < lo || d > hi) {
        for (Int j = 0; j < p->sample.len; j++)
            testing_t_logf_v(t, "sample: %dns", ((const int64_t *)ps[j]->value.p)[i]);
        testing_t_fatalf_v(t,
                           "profile samples total %dns, want within range [%dns, %dns] "
                           "(target: %dns)",
                           d, lo, hi, (Duration)(MUTEX_N * MUTEX_D));
    }
    arena_free(&ar);
}

static void mutex_profile_records(void *env, TestingT *t) {
    (void)env;
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    /* Record a mutex profile using the structured record API. */
    RuntimeBlockProfileRecord *records = NULL;
    Int n = 0;
    for (;;) {
        bool ok;
        Int got = runtime_mutex_profile(records, n, &ok);
        if (ok) {
            n = got;
            break;
        }
        n = got * 2;
        records = (RuntimeBlockProfileRecord *)mem_alloc_array(
            a, (size_t)n, sizeof(RuntimeBlockProfileRecord),
            _Alignof(RuntimeBlockProfileRecord));
        if (records == NULL)
            testing_t_fatalf_v(t, "out of memory");
    }

    /* Check that we see the same stack trace as the proto profile. Go also
     * expects a runtime.goexit root frame here, which burrow does not have. */
    Slice stks = block_record_stacks(a, records, n);
    const Str want[] = {S("burrow__sync_mutex_unlock_slow"), S("")};
    if (!contains_stack(stks, want, NELEM(want)))
        testing_t_errorf_v(t, "No matching stack entry for %v %v", want[0], want[1]);
    arena_free(&ar);
}

static void restore_mutex_profile_fraction(void *env) {
    (void)runtime_set_mutex_profile_fraction(*(const Int *)env);
}

static void TestMutexProfile(TestingT *t) {
    if (!have_walk()) {
        testing_t_skip_v(t, "no stacks to check");
        return;
    }

    /* Generate mutex profile */

    static Int old;
    old = runtime_set_mutex_profile_fraction(1);
    testing_t_cleanup(t, BURROW_FN(Func, restore_mutex_profile_fraction, &old));
    if (old != 0)
        testing_t_fatalf_v(t, "need MutexProfileRate 0, got %d", old);

    Time start = time_now();
    block_mutex_n(t, MUTEX_N, MUTEX_D);
    MutexProfileEnv e = {time_since(start)};

    testing_t_run(t, S("debug=1"), BURROW_FN(TestingTFunc, mutex_profile_debug1, &e));
    testing_t_run(t, S("proto"), BURROW_FN(TestingTFunc, mutex_profile_proto, &e));
    testing_t_run(t, S("records"), BURROW_FN(TestingTFunc, mutex_profile_records, &e));
}

/* Go counts the samples with blockMutex.func1 in them. That has no name here,
 * so this counts the ones that start at the unlock it called, which takes in
 * the ones TestMutexProfile left too. They are the same before and after, which
 * is what is being checked. */
static void read_mutex_profile(TestingT *t, int64_t *contentions, int64_t *delay) {
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    BytesBuffer w = BYTES_BUFFER(a);
    (void)pprof_profile_write_to(pprof_lookup(S("mutex")),
                                 bytes_buffer_as_io_writer(&w), 0);
    Error err = BURROW_NO_ERROR;
    PfProfile *p = burrow__pf_parse(a, bytes_buffer_as_io_reader(&w), &err);
    if (BURROW_FAILED(err))
        testing_t_fatalf_v(t, "failed to parse profile: %v", err);
    testing_t_logf_v(t, "parsed proto: %s", burrow__pf_profile_string(p, a));
    err = burrow__pf_profile_check_valid(p);
    if (BURROW_FAILED(err))
        testing_t_fatalf_v(t, "invalid profile: %v", err);

    *contentions = 0;
    *delay = 0;
    PfSample **ps = (PfSample **)p->sample.p;
    for (Int i = 0; i < p->sample.len; i++) {
        const PfSample *s = ps[i];
        if (s->location.len == 0)
            continue;
        const PfLocation *l = ((PfLocation *const *)s->location.p)[0];
        if (str_eq(location_name(l), S("burrow__sync_mutex_unlock_slow"))) {
            *contentions += ((const int64_t *)s->value.p)[0];
            *delay += ((const int64_t *)s->value.p)[1];
        }
    }
    arena_free(&ar);
}

static void TestMutexProfileRateAdjust(TestingT *t) {
    if (!have_walk()) {
        testing_t_skip_v(t, "no stacks to find the unlock in");
        return;
    }

    static Int old;
    old = runtime_set_mutex_profile_fraction(1);
    testing_t_cleanup(t, BURROW_FN(Func, restore_mutex_profile_fraction, &old));
    if (old != 0)
        testing_t_fatalf_v(t, "need MutexProfileRate 0, got %d", old);

    block_mutex(t);
    int64_t contentions, delay;
    read_mutex_profile(t, &contentions, &delay);
    if (contentions ==
        0) /* low-resolution timers can have delay of 0 in mutex profile */
        testing_t_fatal_v(t, "did not see expected function in profile");
    (void)runtime_set_mutex_profile_fraction(0);
    int64_t new_contentions, new_delay;
    read_mutex_profile(t, &new_contentions, &new_delay);
    if (new_contentions != contentions || new_delay != delay)
        testing_t_fatalf_v(t, "sample value changed: got [%d, %d], want [%d, %d]",
                           new_contentions, new_delay, contentions, delay);
}

typedef Int (*BlockCollectFn)(RuntimeBlockProfileRecord *p, Int len, bool *ok);

/* getProfileStacks, as TestMutexBlockFullAggregation calls it, with fileLine and
 * pcs both true. A frame here has no file and line to write, so each line of a
 * stack is the pc and the function's name, the name empty where the symbol
 * table has none. */
static Slice get_profile_stacks(TestingT *t, Alloc *a, BlockCollectFn collect) {
    Int n = 0;
    RuntimeBlockProfileRecord *p = NULL;
    for (;;) {
        p = (RuntimeBlockProfileRecord *)mem_alloc_array(
            a, (size_t)n, sizeof(RuntimeBlockProfileRecord),
            _Alignof(RuntimeBlockProfileRecord));
        if (p == NULL && n > 0)
            testing_t_fatalf_v(t, "out of memory");
        bool ok;
        n = collect(p, n, &ok);
        if (ok)
            break;
    }
    Slice stacks = slice_make(a, TYPE_STRING, 0, n);
    for (Int i = 0; i < n; i++) {
        Str stack = S("");
        Int nstk = runtime_block_profile_record_stack(&p[i]);
        for (Int j = 0; j < nstk; j++) {
            Uintptr pc = p[i].stack0[j];
            RuntimeFunc f = {0};
            (void)runtime_func_for_pc(pc - 1, &f);
            stack = fmt_sprintf_v(a, "%s%s%x %s", stack, j > 0 ? S("\n") : S(""), pc,
                                  f.name);
        }
        stacks = slice_append(a, stacks, &stack, 1);
    }
    return stacks;
}

static void assert_no_duplicates(TestingT *t, Str name, BlockCollectFn collect) {
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    Slice stacks = get_profile_stacks(t, a, collect);
    const Str *s = (const Str *)stacks.p;
    for (Int i = 0; i < stacks.len; i++)
        for (Int j = 0; j < i; j++)
            if (str_eq(s[i], s[j])) {
                testing_t_errorf_v(
                    t, "saw duplicate entry in %s profile with stack:\n%s", name, s[i]);
                break;
            }
    if (stacks.len == 0)
        testing_t_errorf_v(t, "did not see any samples in %s profile for this test",
                           name);
    arena_free(&ar);
}

static void full_aggregation_mutex(void *env, TestingT *t) {
    (void)env;
    assert_no_duplicates(t, S("mutex"), runtime_mutex_profile);
}

static void full_aggregation_block(void *env, TestingT *t) {
    (void)env;
    assert_no_duplicates(t, S("block"), runtime_block_profile);
}

typedef struct FullAggregationEnv {
    SyncMutex m;
    SyncWaitGroup wg;
} FullAggregationEnv;

enum { FULL_AGGREGATION_ITERS = 100, FULL_AGGREGATION_WORKERS = 2 };

static void full_aggregation_worker(void *env) {
    FullAggregationEnv *e = (FullAggregationEnv *)env;
    for (int i = 0; i < FULL_AGGREGATION_ITERS; i++) {
        sync_mutex_lock(&e->m);
        /* Wait at least 1 millisecond to pass the starvation threshold for the
         * mutex */
        time_sleep(TIME_MILLISECOND);
        sync_mutex_unlock(&e->m);
    }
    sync_wait_group_done(&e->wg);
}

static void TestMutexBlockFullAggregation(TestingT *t) {
    /* This regression test is adapted from
     * https://github.com/grafana/pyroscope-go/issues/103, authored by Tolya
     * Korniltsev */

    FullAggregationEnv e = {0};

    static Int prev;
    prev = runtime_set_mutex_profile_fraction(-1);
    testing_t_cleanup(t, BURROW_FN(Func, restore_mutex_profile_fraction, &prev));

    enum { fraction = 1 };

    (void)runtime_set_mutex_profile_fraction(fraction);
    runtime_set_block_profile_rate(1);
    testing_t_cleanup(t, BURROW_FN(Func, reset_block_profile_rate, NULL));

    sync_wait_group_add(&e.wg, FULL_AGGREGATION_WORKERS);
    for (int i = 0; i < FULL_AGGREGATION_WORKERS; i++)
        if (!go(BURROW_FN(Func, full_aggregation_worker, &e)))
            testing_t_fatalf_v(t, "go failed");
    sync_wait_group_wait(&e.wg);

    testing_t_run(t, S("mutex"), BURROW_FN(TestingTFunc, full_aggregation_mutex, NULL));
    testing_t_run(t, S("block"), BURROW_FN(TestingTFunc, full_aggregation_block, NULL));
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
    X(TestProfileWriteTo)                                                              \
    X(TestGoroutineCounts)                                                             \
    X(TestCPUProfile)                                                                  \
    X(TestCPUProfileMultithreaded)                                                     \
    X(TestGoroutineSwitch)                                                             \
    X(TestMathBigDivide)                                                               \
    X(TestCPUProfileLabel)                                                             \
    X(TestConvertMemProfile)                                                           \
    X(TestMemoryProfiler)                                                              \
    X(TestBlockProfile)                                                                \
    X(TestBlockProfileBias)                                                            \
    X(TestMutexProfile)                                                                \
    X(TestMutexProfileRateAdjust)                                                      \
    X(TestMutexBlockFullAggregation)

TESTING_MAIN(TESTS)
