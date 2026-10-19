/* Derived from Go's src/internal/profile/proto_test.go. Go source: go1.27.1.
 *
 * Copyright 2016 The Go Authors. All rights reserved.
 * Copyright 2026 The burrow Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style licence that can be found
 * in the LICENSE file. */

#include "check.h"

#include "../src/debug/pprof_profile.h"
#include "burrow/burrow.h"
#include "burrow/bytes.h"
#include "burrow/mem/arena.h"
#include "burrow/slices.h"

#define S(lit) BURROW_S(lit)
#define NELEM(x) ((Int)(sizeof(x) / sizeof((x)[0])))

typedef struct PackedInts {
    Slice uint64s;
    Slice int64s;
} PackedInts;

static Error packed_ints_dec_uint64s(PfBuffer *b, void *m) {
    return burrow__pf_decode_uint64s(b, &((PackedInts *)m)->uint64s);
}

static Error packed_ints_dec_int64s(PfBuffer *b, void *m) {
    return burrow__pf_decode_int64s(b, &((PackedInts *)m)->int64s);
}

static void packed_ints_encode(PfBuffer *b, const void *m) {
    const PackedInts *u = m;
    burrow__pf_encode_uint64s(b, 1, u->uint64s);
    burrow__pf_encode_int64s(b, 2, u->int64s);
}

static const PfDecoder packed_ints_decoder[] = {
    NULL,
    packed_ints_dec_uint64s,
    packed_ints_dec_int64s,
};

static const PfMessage packed_ints_msg = {
    packed_ints_decoder,
    NELEM(packed_ints_decoder),
    packed_ints_encode,
};

static void TestPackedEncoding(TestingT *t) {
    static uint64_t u0[] = {0, 1, 10, 100, 1000, 10000};
    static int64_t i0[] = {1000, 0, 1000};
    static Byte e0[] = {10, 8,  0, 1,   10, 100, 232, 7, 144,
                        78, 18, 5, 232, 7,  0,   232, 7};
    static uint64_t u1[] = {10000};
    static Byte e1[] = {8, 144, 78};
    static int64_t i2[] = {-10000};
    static Byte e2[] = {16, 240, 177, 255, 255, 255, 255, 255, 255, 255, 1};
    struct {
        Slice uint64s;
        Slice int64s;
        Slice encoded;
    } tests[] = {
        {
            {u0, NELEM(u0), NELEM(u0), TYPE_UINT64},
            {i0, NELEM(i0), NELEM(i0), TYPE_INT64},
            {e0, NELEM(e0), NELEM(e0), TYPE_BYTE},
        },
        {
            {u1, NELEM(u1), NELEM(u1), TYPE_UINT64},
            slice_nil(TYPE_INT64),
            {e1, NELEM(e1), NELEM(e1), TYPE_BYTE},
        },
        {
            slice_nil(TYPE_UINT64),
            {i2, NELEM(i2), NELEM(i2), TYPE_INT64},
            {e2, NELEM(e2), NELEM(e2), TYPE_BYTE},
        },
    };
    for (Int i = 0; i < NELEM(tests); i++) {
        Arena ar;
        arena_init(&ar, NULL, 0);
        Alloc *a = arena_allocator(&ar);

        PackedInts source = {tests[i].uint64s, tests[i].int64s};
        Slice got = burrow__pf_marshal(a, &packed_ints_msg, &source);
        if (!slices_equal(got, tests[i].encoded))
            testing_t_errorf_v(t, "failed encode %d, got %v, want %v", i, got,
                               tests[i].encoded);

        PackedInts dest = {slice_nil(TYPE_UINT64), slice_nil(TYPE_INT64)};
        Error err = burrow__pf_unmarshal(a, tests[i].encoded, &packed_ints_msg, &dest);
        if (BURROW_FAILED(err)) {
            testing_t_errorf_v(t, "failed decode %d: %v", i, err);
            arena_free(&ar);
            continue;
        }
        if (!slices_equal(dest.uint64s, tests[i].uint64s))
            testing_t_errorf_v(t, "failed decode uint64s %d, got %v, want %v", i,
                               dest.uint64s, tests[i].uint64s);
        if (!slices_equal(dest.int64s, tests[i].int64s))
            testing_t_errorf_v(t, "failed decode int64s %d, got %v, want %v", i,
                               dest.int64s, tests[i].int64s);
        arena_free(&ar);
    }
}

/* A small profile with two sample types, one mapping, two functions, an
 * inlined location and labelled samples, for the tests below. */
static PfProfile *small_profile(Alloc *a) {
    PfProfile *p = burrow__pf_new_profile(a);
    PfValueType *st0 = BURROW_NEW(a, PfValueType);
    *st0 = (PfValueType){.type = S("samples"), .unit = S("count")};
    PfValueType *st1 = BURROW_NEW(a, PfValueType);
    *st1 = (PfValueType){.type = S("cpu"), .unit = S("nanoseconds")};
    p->sample_type = BURROW_APPEND(void *, a, p->sample_type, st0);
    p->sample_type = BURROW_APPEND(void *, a, p->sample_type, st1);
    p->period_type = BURROW_NEW(a, PfValueType);
    *p->period_type = (PfValueType){.type = S("cpu"), .unit = S("nanoseconds")};
    p->period = 10000000;
    p->duration_nanos = 2000000000;

    PfMapping *m = BURROW_NEW(a, PfMapping);
    *m = (PfMapping){.id = 1,
                     .start = 0x400000,
                     .limit = 0x500000,
                     .file = S("/bin/prog"),
                     .build_id = S("abc"),
                     .has_functions = true};
    p->mapping = BURROW_APPEND(void *, a, p->mapping, m);

    PfFunction *f1 = BURROW_NEW(a, PfFunction);
    *f1 = (PfFunction){.id = 1,
                       .name = S("main.main"),
                       .system_name = S("main.main"),
                       .filename = S("main.go"),
                       .start_line = 10};
    PfFunction *f2 = BURROW_NEW(a, PfFunction);
    *f2 = (PfFunction){.id = 2,
                       .name = S("main.work"),
                       .system_name = S("main.work"),
                       .filename = S("work.go"),
                       .start_line = 3};
    p->function = BURROW_APPEND(void *, a, p->function, f1);
    p->function = BURROW_APPEND(void *, a, p->function, f2);

    PfLocation *l1 = BURROW_NEW(a, PfLocation);
    *l1 = (PfLocation){.id = 1, .mapping = m, .address = 0x401000};
    l1->line = slice_make(a, burrow__pf_line_type(), 1, 1);
    BURROW_AT(PfLine, l1->line, 0) = (PfLine){.function = f1, .line = 12};
    PfLocation *l2 = BURROW_NEW(a, PfLocation);
    *l2 = (PfLocation){.id = 2, .mapping = m, .address = 0x402000};
    l2->line = slice_make(a, burrow__pf_line_type(), 2, 2);
    BURROW_AT(PfLine, l2->line, 0) = (PfLine){.function = f2, .line = 5};
    BURROW_AT(PfLine, l2->line, 1) = (PfLine){.function = f1, .line = 14};
    p->location = BURROW_APPEND(void *, a, p->location, l1);
    p->location = BURROW_APPEND(void *, a, p->location, l2);

    static const int64_t v0[] = {1, 10000000};
    static const int64_t v1[] = {3, 30000000};
    PfLocation *const stacks[][2] = {{l2, l1}, {l1, NULL}};
    const int64_t *values[] = {v0, v1};
    for (Int i = 0; i < 2; i++) {
        PfSample *s = BURROW_NEW(a, PfSample);
        s->location = slice_nil(TYPE_UNSAFE_POINTER);
        for (Int j = 0; j < 2 && stacks[i][j] != NULL; j++)
            s->location = BURROW_APPEND(void *, a, s->location, stacks[i][j]);
        s->value = slice_make(a, TYPE_INT64, 2, 2);
        BURROW_AT(int64_t, s->value, 0) = values[i][0];
        BURROW_AT(int64_t, s->value, 1) = values[i][1];
        s->label = slice_nil(TYPE_UNSAFE_POINTER);
        s->num_label = slice_nil(TYPE_UNSAFE_POINTER);
        s->num_unit = slice_nil(TYPE_UNSAFE_POINTER);
        if (i == 0) {
            burrow__pf_labels_add(a, &s->label, S("key"), S("value"));
            burrow__pf_num_labels_add(a, &s->num_label, S("bytes"), 512);
        }
        p->sample = BURROW_APPEND(void *, a, p->sample, s);
    }
    return p;
}

/* Not in Go. Writing a profile and parsing it back gives the same profile,
 * as String prints it. */
static void TestWriteParse(TestingT *t) {
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);

    PfProfile *p = small_profile(a);
    Error err = burrow__pf_profile_check_valid(p);
    if (BURROW_FAILED(err))
        testing_t_fatalf_v(t, "CheckValid: %v", err);
    Str want = burrow__pf_profile_string(p, a);

    BytesBuffer buf = BYTES_BUFFER(a);
    err = burrow__pf_profile_write(p, bytes_buffer_as_io_writer(&buf));
    if (BURROW_FAILED(err))
        testing_t_fatalf_v(t, "Write: %v", err);
    PfProfile *q = burrow__pf_parse(a, bytes_buffer_as_io_reader(&buf), &err);
    if (BURROW_FAILED(err))
        testing_t_fatalf_v(t, "Parse: %v", err);
    Str got = burrow__pf_profile_string(q, a);
    if (!str_eq(got, want))
        testing_t_errorf_v(t, "Parse(Write(p)):\n%s\nwant:\n%s", got, want);

    PfStrLabels *l = burrow__pf_labels_get(
        ((PfSample *)BURROW_AT(void *, q->sample, 0))->label, S("key"));
    if (l == NULL || l->values.len != 1 ||
        !str_eq(BURROW_AT(Str, l->values, 0), S("value")))
        testing_t_errorf_v(t, "label key did not survive the round trip");
    arena_free(&ar);
}

/* Not in Go. Merging a profile with itself keeps one copy of each sample with
 * the values added, and an empty input or an incompatible one is an error. */
static void TestMergeSelf(TestingT *t) {
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);

    PfProfile *p = small_profile(a);
    Slice srcs = slice_nil(TYPE_UNSAFE_POINTER);
    srcs = BURROW_APPEND(void *, a, srcs, p);
    srcs = BURROW_APPEND(void *, a, srcs, p);
    Error err = BURROW_NO_ERROR;
    PfProfile *m = burrow__pf_merge(a, srcs, &err);
    if (BURROW_FAILED(err))
        testing_t_fatalf_v(t, "Merge: %v", err);
    if (m->sample.len != 2 || m->location.len != 2 || m->function.len != 2 ||
        m->mapping.len != 1)
        testing_t_errorf_v(
            t, "Merge: got %d samples, %d locations, %d functions, %d mappings",
            m->sample.len, m->location.len, m->function.len, m->mapping.len);
    for (Int i = 0; i < m->sample.len && i < p->sample.len; i++) {
        Slice gv = ((PfSample *)BURROW_AT(void *, m->sample, i))->value;
        Slice pv = ((PfSample *)BURROW_AT(void *, p->sample, i))->value;
        for (Int j = 0; j < gv.len; j++) {
            if (BURROW_AT(int64_t, gv, j) != 2 * BURROW_AT(int64_t, pv, j))
                testing_t_errorf_v(t, "sample %d value %d: got %d, want %d", i, j,
                                   BURROW_AT(int64_t, gv, j),
                                   2 * BURROW_AT(int64_t, pv, j));
        }
    }
    if (m->duration_nanos != 2 * p->duration_nanos)
        testing_t_errorf_v(t, "DurationNanos: got %d, want %d", m->duration_nanos,
                           2 * p->duration_nanos);
    err = burrow__pf_profile_check_valid(m);
    if (BURROW_FAILED(err))
        testing_t_errorf_v(t, "CheckValid(merged): %v", err);

    (void)burrow__pf_merge(a, slice_nil(TYPE_UNSAFE_POINTER), &err);
    if (!BURROW_FAILED(err) || !str_eq(error_text(err), S("no profiles to merge")))
        testing_t_errorf_v(t, "Merge(nil): got %v", err);

    PfProfile *other = small_profile(a);
    ((PfValueType *)BURROW_AT(void *, other->sample_type, 1))->unit = S("seconds");
    BURROW_AT(void *, srcs, 1) = other;
    (void)burrow__pf_merge(a, srcs, &err);
    if (!BURROW_FAILED(err))
        testing_t_errorf_v(t, "Merge with a different sample unit: no error");
    arena_free(&ar);
}

/* Not in Go. Parse reports empty input with ErrNoData wrapped, and a bad
 * profile through CheckValid. */
static void TestParseErrors(TestingT *t) {
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);

    BytesBuffer empty = BYTES_BUFFER(a);
    Error err = BURROW_NO_ERROR;
    PfProfile *p = burrow__pf_parse(a, bytes_buffer_as_io_reader(&empty), &err);
    if (p != NULL || !errors_is(err, burrow__pf_err_no_data))
        testing_t_errorf_v(t, "Parse(empty): got %v", err);
    else if (!str_eq(error_text(err), S("parsing profile: empty input file")))
        testing_t_errorf_v(t, "Parse(empty): got %q", error_text(err));

    p = small_profile(a);
    ((PfMapping *)BURROW_AT(void *, p->mapping, 0))->id = 0;
    err = burrow__pf_profile_check_valid(p);
    if (!BURROW_FAILED(err) ||
        !str_eq(error_text(err), S("found mapping with reserved ID=0")))
        testing_t_errorf_v(t, "CheckValid with mapping ID 0: got %v", err);
    arena_free(&ar);
}

/* Not in Go. Scale multiplies each value and Prune cuts stacks at the frames
 * DropFrames names. */
static void TestScalePrune(TestingT *t) {
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);

    PfProfile *p = small_profile(a);
    burrow__pf_profile_scale(p, 0.5);
    Slice v = ((PfSample *)BURROW_AT(void *, p->sample, 1))->value;
    if (BURROW_AT(int64_t, v, 0) != 1 || BURROW_AT(int64_t, v, 1) != 15000000)
        testing_t_errorf_v(t, "Scale(0.5): got %v", v);

    p->drop_frames = S("main\\.work");
    Error err = burrow__pf_profile_remove_uninteresting(p);
    if (BURROW_FAILED(err))
        testing_t_fatalf_v(t, "RemoveUninteresting: %v", err);
    PfLocation *l2 = BURROW_AT(void *, p->location, 1);
    if (l2->line.len != 1 || BURROW_AT(PfLine, l2->line, 0).line != 14)
        testing_t_errorf_v(t, "Prune: location 2 has %d lines", l2->line.len);

    p->drop_frames = S("(");
    err = burrow__pf_profile_remove_uninteresting(p);
    if (!BURROW_FAILED(err))
        testing_t_errorf_v(t, "RemoveUninteresting with a bad regexp: no error");
    arena_free(&ar);
}

#define TESTS(X)                                                                       \
    X(TestPackedEncoding)                                                              \
    X(TestWriteParse)                                                                  \
    X(TestMergeSelf)                                                                   \
    X(TestParseErrors)                                                                 \
    X(TestScalePrune)

TESTING_MAIN(TESTS)
