/* Derived from golang.org/x/text v0.37.0's unicode/norm tests:
 * composition_test.go, forminfo_test.go, iter_test.go, normalize_test.go,
 * readwriter_test.go, transform_test.go, example_iter_test.go and
 * example_test.go. That is the version Go 1.27.1 vendors.
 *
 * The tables come from tests/xtext_norm_test_gen.h, which
 * tools/gen-xtext-norm.sh writes from the Go package itself. Besides Go's own
 * cases, it holds hashes of what Go says about a corpus of strings and about
 * every rune, and TestDifferential and TestSweep work out the same records here
 * and compare. describe and sweep_record below have to stay in step with the
 * generator. Set BURROW_XTEXT_SHOW to see the first record that differs.
 *
 * TestLinking is left out, as it is about Go's linker, and so are the
 * conformance tests in ucd_test.go, which read NormalizationTest.txt from the
 * network.
 *
 * Copyright 2011 The Go Authors. All rights reserved.
 * Copyright 2026 The burrow Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style licence that can be found
 * in the LICENSE file. */

#include "check.h"

#include "../src/xtext/norm.h"
#include "../src/xtext/norm_tables.h"

#include "burrow/burrow.h"
#include "burrow/mem/arena.h"

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

typedef struct NormFormData {
    uint8_t qc;
    bool combines_forward;
    const char *dec;
    Int dec_len;
} NormFormData;

typedef struct NormRuneData {
    Rune r;
    uint8_t ccc, n_lead, n_trail;
    NormFormData f[2];
} NormRuneData;

typedef struct NormPositionTest {
    const char *input;
    Int input_len;
    Int pos;
    const char *buffer;
    Int buffer_len;
} NormPositionTest;

typedef struct NormSpanTest {
    const char *input;
    Int input_len;
    Int at_eof;
    Int n;
    Int err;
} NormSpanTest;

typedef struct NormAppendTest {
    const char *left;
    Int left_len;
    const char *right;
    Int right_len;
    const char *out;
    Int out_len;
} NormAppendTest;

typedef struct NormRuneTest {
    int nin;
    Rune in[16];
    int nout;
    Rune out[16];
} NormRuneTest;

typedef struct NormLit {
    const char *s;
    Int len;
} NormLit;

typedef struct NormSegmentTest {
    const char *in;
    Int in_len;
    const NormLit *out;
    Int nout;
} NormSegmentTest;

typedef struct NormNextBoundaryTest {
    const char *input;
    Int input_len;
    bool at_eof;
    Int want;
} NormNextBoundaryTest;

typedef struct NormTransformTest {
    Int f;
    const char *in;
    Int in_len;
    const char *out;
    Int out_len;
    Int eof;
    Int dst_size;
    Int err;
} NormTransformTest;

#if defined(__GNUC__)
#pragma GCC diagnostic ignored "-Woverlength-strings"
#endif
#include "xtext_norm_test_gen.h"

#define LEN(x) ((Int)(sizeof(x) / sizeof((x)[0])))

static const char *const fstr[] = {"NFC", "NFD", "NFKC", "NFKD"};

/* ------------------------------------------------------------------ helpers */

static Str lit(const char *s, Int n) {
    return str_from_bytes(s, n);
}

static Slice bytes_of(Byte *p, Int len, Int cap) {
    Slice s = {p, len, cap, TYPE_BYTE};
    return s;
}

/* []byte(s), with the capacity equal to the length. */
static Slice exact(Alloc *a, Str s) {
    Byte *p = mem_alloc_nozero(a, s.len > 0 ? (size_t)s.len : 1, 1);
    if (s.len > 0)
        memcpy(p, s.p, (size_t)s.len);
    return bytes_of(p, s.len, s.len);
}

static Str cat(Alloc *a, Str x, Str y) {
    Byte *p = mem_alloc_nozero(a, (size_t)(x.len + y.len + 1), 1);
    if (x.len > 0)
        memcpy(p, x.p, (size_t)x.len);
    if (y.len > 0)
        memcpy(p + x.len, y.p, (size_t)y.len);
    return str_from_bytes(p, x.len + y.len);
}

static Str as_str(Slice b) {
    return str_from_bytes(b.p, b.len);
}

/* A byte slice that grows the way append does, near enough for a test. */
typedef struct Buf {
    Alloc *a;
    Byte *p;
    Int len, cap;
} Buf;

static void buf_write(Buf *b, const void *p, Int n) {
    if (n <= 0)
        return;
    if (b->len + n > b->cap) {
        Int c = b->cap * 2 + n + 64;
        Byte *q = mem_alloc_nozero(b->a, (size_t)c, 1);
        if (b->len > 0)
            memcpy(q, b->p, (size_t)b->len);
        b->p = q;
        b->cap = c;
    }
    memcpy(b->p + b->len, p, (size_t)n);
    b->len += n;
}

static Str buf_str(const Buf *b) {
    return str_from_bytes(b->p, b->len);
}

static Str rune_str(Alloc *a, const Rune *rs, int n) {
    Buf b = {a, NULL, 0, 0};
    for (int i = 0; i < n; i++) {
        Byte tmp[4];
        Int k = utf8_encode_rune(bytes_of(tmp, 4, 4), rs[i]);
        buf_write(&b, tmp, k);
    }
    return buf_str(&b);
}

static const Error *err_of(Int code) {
    switch (code) {
    case 1:
        return &burrow__transform_err_short_dst;
    case 2:
        return &burrow__transform_err_short_src;
    case 3:
        return &burrow__transform_err_end_of_span;
    default:
        return NULL;
    }
}

static bool err_is(Error e, Int code) {
    const Error *w = err_of(code);
    if (w == NULL)
        return !BURROW_FAILED(e);
    return burrow__transform_err_eq(e, *w);
}

/* An io.Reader over b that hands out at most n bytes a call, which is
 * bytes.Buffer when n is large. */
typedef struct ChunkReader {
    const Byte *p;
    Int len;
    Int n;
} ChunkReader;

static Int chunk_read(void *self, Slice p, Error *err) {
    ChunkReader *r = self;
    if (r->len == 0) {
        *err = io_eof;
        return 0;
    }
    Int m = p.len;
    if (r->n < m)
        m = r->n;
    if (r->len < m)
        m = r->len;
    if (m > 0)
        memcpy(p.p, r->p, (size_t)m);
    r->p += m;
    r->len -= m;
    *err = BURROW_NO_ERROR;
    return m;
}

static const Type chunk_reader_desc = {
    {(const Byte *)"chunkReader", 11},
    {(const Byte *)"golang.org/x/text/unicode/norm", 30},
    KIND_STRUCT,
    (uint32_t)sizeof(ChunkReader),
    (uint16_t)_Alignof(ChunkReader),
    0,
    0,
    NULL,
    NULL,
    NULL,
    NULL,
    0,
    0,
    NULL,
};

static const IoReaderVT chunk_reader_vt = {&chunk_reader_desc, chunk_read};

static Int buf_io_write(void *self, Slice p, Error *err) {
    buf_write(self, p.p, p.len);
    *err = BURROW_NO_ERROR;
    return p.len;
}

static const Type buf_desc = {
    {(const Byte *)"Buffer", 6},
    {(const Byte *)"bytes", 5},
    KIND_STRUCT,
    (uint32_t)sizeof(Buf),
    (uint16_t)_Alignof(Buf),
    0,
    0,
    NULL,
    NULL,
    NULL,
    NULL,
    0,
    0,
    NULL,
};

static const IoWriterVT buf_writer_vt = {&buf_desc, buf_io_write};

/* What the Reader gives for s, read with buffers of size bytes from a source
 * that hands out at most chunk bytes a call. */
static Str read_all(Alloc *a, NormForm f, Str s, Int chunk, Int size, Error *last) {
    ChunkReader cr = {s.p, s.len, chunk};
    IoReader src = {&chunk_reader_vt, &cr};
    NormReader *rd = burrow__norm_new_reader(a, f, src);
    Byte *bp = mem_alloc_nozero(a, (size_t)size, 1);
    Buf out = {a, NULL, 0, 0};
    for (;;) {
        Error err = BURROW_NO_ERROR;
        Int n = burrow__norm_reader_read(rd, bytes_of(bp, size, size), &err);
        buf_write(&out, bp, n);
        if (BURROW_FAILED(err)) {
            *last = err;
            break;
        }
    }
    burrow__norm_reader_free(rd);
    return buf_str(&out);
}

static Str write_all(Alloc *a, NormForm f, Str s, Int size) {
    Buf out = {a, NULL, 0, 0};
    IoWriter dst = {&buf_writer_vt, &out};
    NormWriter *w = burrow__norm_new_writer(a, f, dst);
    Byte *bp = mem_alloc_nozero(a, (size_t)size, 1);
    for (Int p = 0; p < s.len;) {
        Int n = s.len - p < size ? s.len - p : size;
        memcpy(bp, s.p + p, (size_t)n);
        Error err;
        burrow__norm_writer_write(w, bytes_of(bp, n, size), &err);
        p += n;
    }
    burrow__norm_writer_close(w);
    burrow__norm_writer_free(w);
    return buf_str(&out);
}

static Str iter_all(Alloc *a, NormForm f, Str s, bool as_string) {
    NormIter it;
    memset(&it, 0, sizeof it);
    if (as_string)
        burrow__norm_iter_init_string(&it, f, s);
    else
        burrow__norm_iter_init(&it, f, exact(a, s));
    Buf out = {a, NULL, 0, 0};
    while (!burrow__norm_iter_done(&it)) {
        Slice seg = burrow__norm_iter_next(&it);
        buf_write(&out, seg.p, seg.len);
    }
    return buf_str(&out);
}

/* ---------------------------------------------------------- composition_test */

static void run_tests(TestingT *t, const char *name, NormForm fm,
                      const NormRuneTest *tests, Int n) {
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    NormReorderBuffer rb;
    memset(&rb, 0, sizeof rb);
    burrow__norm_rb_init(&rb, fm, slice_nil(TYPE_BYTE));
    for (Int i = 0; i < n; i++) {
        const NormRuneTest *test = &tests[i];
        burrow__norm_rb_set_flusher(&rb, a, slice_nil(TYPE_BYTE),
                                    burrow__norm_rb_append_flush);
        for (int j = 0; j < test->nin; j++) {
            Byte b[4];
            Int k = utf8_encode_rune(bytes_of(b, 4, 4), test->in[j]);
            NormInput src = burrow__norm_input_bytes(bytes_of(b, k, 4));
            NormProperties info = burrow__norm_rb_info(&rb, src, 0);
            if (j == 0)
                burrow__norm_rb_ss_first(&rb, info);
            else
                burrow__norm_rb_ss_next(&rb, info);
            if (burrow__norm_rb_insert_flush(&rb, src, 0, info) < 0)
                testing_t_errorf_v(t, "%s:%d: insert failed for rune %d", name, i, j);
        }
        burrow__norm_rb_do_flush(&rb);
        Str was = as_str(rb.out);
        Str want = rune_str(a, test->out, test->nout);
        if (was.len != want.len)
            testing_t_errorf_v(t, "%s:%d: length = %d; want %d", name, i, was.len,
                               want.len);
        if (!str_eq(was, want))
            testing_t_errorf_v(t, "%s:%d: \nwas  %q; \nwant %q", name, i, was, want);
    }
    arena_free(&ar);
}

static void TestFlush(TestingT *t) {
    Byte buf[NORM_MAX_BUFFER_SIZE * 4];
    Str hello = BURROW_S("Hello "), world = BURROW_S("world!");
    memcpy(buf, hello.p, (size_t)hello.len);
    Int p = hello.len;
    NormReorderBuffer rb;
    memset(&rb, 0, sizeof rb);
    burrow__norm_rb_init_string(&rb, NORM_NFC, world);
    Slice out = bytes_of(buf + p, (Int)sizeof buf - p, (Int)sizeof buf - p);
    Int i = burrow__norm_rb_flush_copy(&rb, out);
    if (i != 0)
        testing_t_errorf_v(t, "wrote bytes on flush of empty buffer. (len(out) = %d)",
                           i);

    for (Int k = 0; k < world.len; k++) {
        /* No need to set streamSafe values for this test. */
        burrow__norm_rb_insert_flush(&rb, rb.src, k,
                                     burrow__norm_rb_info(&rb, rb.src, k));
        Int n = burrow__norm_rb_flush_copy(&rb, out);
        out = bytes_of((Byte *)out.p + n, out.len - n, out.cap - n);
        p += n;
    }

    Str was = str_from_bytes(buf, p);
    if (!str_eq(was, BURROW_S("Hello world!")))
        testing_t_errorf_v(t, "output after flush was \"%s\"; want \"%s\"", was,
                           BURROW_S("Hello world!"));
    if (rb.nrune != 0)
        testing_t_errorf_v(t, "non-null size of info buffer (rb.nrune == %d)",
                           rb.nrune);
    if (rb.nbyte != 0)
        testing_t_errorf_v(t, "non-null size of byte buffer (rb.nbyte == %d)",
                           (Int)rb.nbyte);
}

static void TestInsert(TestingT *t) {
    run_tests(t, "TestInsert", NORM_NFD, insert_tests, LEN(insert_tests));
}

static void TestDecomposition(TestingT *t) {
    run_tests(t, "TestDecompositionNFD", NORM_NFD, decomposition_nfd_test,
              LEN(decomposition_nfd_test));
    run_tests(t, "TestDecompositionNFKD", NORM_NFKD, decomposition_nfkd_test,
              LEN(decomposition_nfkd_test));
}

static void TestComposition(TestingT *t) {
    run_tests(t, "TestComposition", NORM_NFC, composition_test, LEN(composition_test));
}

/* ------------------------------------------------------------ forminfo_test */

static void TestProperties(TestingT *t) {
    static const char *const ck[2] = {"C", "K"};
    NormRuneData d = norm_test_data[0];
    Int k = 1;
    for (Rune r = 0; r < 0x2ffff; r++) {
        if (k < LEN(norm_test_data) && r == norm_test_data[k].r) {
            d = norm_test_data[k];
            k++;
        }
        Byte b[4];
        Int n = utf8_encode_rune(bytes_of(b, 4, 4), r);
        Str s = str_from_bytes(b, n);
        NormProperties ps[2] = {burrow__norm_properties_string(NORM_NFC, s),
                                burrow__norm_properties_string(NORM_NFKC, s)};
        for (int j = 0; j < 2; j++) {
            NormProperties p = ps[j];
            NormFormData f = d.f[j];
            if (burrow__norm_ccc(p) != d.ccc)
                testing_t_errorf_v(t, "%U: ccc(%s): was %d; want %d %X", r, ck[j],
                                   (Int)burrow__norm_ccc(p), (Int)d.ccc, (Int)p.index);
            bool yes_c = (p.flags & 0x10) == 0;
            if (yes_c != (f.qc == 0))
                testing_t_errorf_v(t, "%U: YesC(%s): was %t; want %t", r, ck[j], yes_c,
                                   f.qc == 0);
            bool back = (p.flags & 0x8) != 0;
            if (back != (f.qc == 2))
                testing_t_errorf_v(t, "%U: combines backwards(%s): was %t; want %t", r,
                                   ck[j], back, f.qc == 2);
            if (p.n_lead != d.n_lead)
                testing_t_errorf_v(t, "%U: nLead(%s): was %d; want %d", r, ck[j],
                                   (Int)p.n_lead, (Int)d.n_lead);
            if ((p.flags & 0x3) != d.n_trail)
                testing_t_errorf_v(t, "%U: nTrail(%s): was %d; want %d", r, ck[j],
                                   (Int)(p.flags & 0x3), (Int)d.n_trail);
            bool fwd = (p.flags & 0x20) != 0;
            if (fwd != f.combines_forward)
                testing_t_errorf_v(t, "%U: combines forward(%s): was %t; want %t", r,
                                   ck[j], fwd, f.combines_forward);
            /* Skip Hangul as it is algorithmically computed. */
            if (r >= 0xAC00 && r < 0xAC00 + 11172)
                continue;
            if ((p.flags & 0x4) != 0) {
                if (f.dec_len == 0)
                    testing_t_errorf_v(
                        t, "%U: hasDecomposition(%s): was true; want false", r, ck[j]);
                Str got = as_str(burrow__norm_decomposition(p));
                if (!str_eq(got, lit(f.dec, f.dec_len)))
                    testing_t_errorf_v(t, "%U: decomp(%s): was %+q; want %+q", r, ck[j],
                                       got, lit(f.dec, f.dec_len));
            }
        }
    }
}

/* ----------------------------------------------------------- normalize_test */

typedef Int (*PositionFunc)(NormReorderBuffer *rb, NormForm f, Alloc *a, Str s,
                            Str *out);

static void run_pos_tests(TestingT *t, const char *name, NormForm f, PositionFunc fn,
                          const NormPositionTest *tests, Int n) {
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    NormReorderBuffer rb;
    memset(&rb, 0, sizeof rb);
    burrow__norm_rb_init(&rb, f, slice_nil(TYPE_BYTE));
    for (Int i = 0; i < n; i++) {
        const NormPositionTest *test = &tests[i];
        Str input = lit(test->input, test->input_len);
        burrow__norm_rb_reset(&rb);
        burrow__norm_rb_set_src(&rb, burrow__norm_input_string(input));
        Str out = BURROW_STR_EMPTY;
        Int pos = fn(&rb, f, a, input, &out);
        if (pos != test->pos)
            testing_t_errorf_v(t, "%s:%d: position is %d; want %d", name, i, pos,
                               test->pos);
        Str want = lit(test->buffer, test->buffer_len);
        if (!str_eq(out, want))
            testing_t_errorf_v(t, "%s:%d: buffer \nwas  %+q; \nwant %+q", name, i, out,
                               want);
    }
    arena_free(&ar);
}

static Int decompose_segment_f(NormReorderBuffer *rb, NormForm f, Alloc *a, Str s,
                               Str *out) {
    (void)f;
    burrow__norm_rb_init_string(rb, NORM_NFD, s);
    burrow__norm_rb_set_flusher(rb, a, slice_nil(TYPE_BYTE),
                                burrow__norm_rb_append_flush);
    Int p = burrow__norm_decompose_segment(rb, 0, true);
    *out = as_str(rb->out);
    return p;
}

static void TestDecomposeSegment(TestingT *t) {
    run_pos_tests(t, "TestDecomposeSegment", NORM_NFC, decompose_segment_f,
                  decompose_segment_tests, LEN(decompose_segment_tests));
}

static Int first_boundary_f(NormReorderBuffer *rb, NormForm f, Alloc *a, Str s,
                            Str *out) {
    (void)rb;
    (void)out;
    return burrow__norm_first_boundary(f, exact(a, s));
}

static Int first_boundary_string_f(NormReorderBuffer *rb, NormForm f, Alloc *a, Str s,
                                   Str *out) {
    (void)rb;
    (void)a;
    (void)out;
    return burrow__norm_first_boundary_in_string(f, s);
}

static void TestFirstBoundary(TestingT *t) {
    run_pos_tests(t, "TestFirstBoundary", NORM_NFC, first_boundary_f,
                  first_boundary_tests, LEN(first_boundary_tests));
    run_pos_tests(t, "TestFirstBoundaryInString", NORM_NFC, first_boundary_string_f,
                  first_boundary_tests, LEN(first_boundary_tests));
}

static void TestNextBoundary(TestingT *t) {
    for (Int i = 0; i < LEN(next_boundary_tests); i++) {
        const NormNextBoundaryTest *tc = &next_boundary_tests[i];
        Str in = lit(tc->input, tc->input_len);
        Slice b = bytes_of((Byte *)(uintptr_t)in.p, in.len, in.len);
        Int nb = burrow__norm_next_boundary(NORM_NFC, b, (tc->at_eof != 0));
        Int ns = burrow__norm_next_boundary_in_string(NORM_NFC, in, (tc->at_eof != 0));
        if (nb != ns)
            testing_t_errorf_v(t, "%d: Bytes(%+q, %t) = %d; String = %d", i, in,
                               (tc->at_eof != 0), nb, ns);
        if (nb != tc->want)
            testing_t_errorf_v(t, "%d: NextBoundary(%+q, %t) = %d; want %d", i, in,
                               (tc->at_eof != 0), nb, tc->want);
    }
}

static Int decompose_to_last(NormReorderBuffer *rb, NormForm f, Alloc *a, Str s,
                             Str *out) {
    (void)f;
    burrow__norm_rb_set_flusher(rb, a, exact(a, s), burrow__norm_rb_append_flush);
    burrow__norm_decompose_to_last_boundary(rb);
    *out = as_str(burrow__norm_rb_flush(rb, a, slice_nil(TYPE_BYTE)));
    return rb->out.len;
}

static void TestDecomposeToLastBoundary(TestingT *t) {
    run_pos_tests(t, "TestDecomposeToLastBoundary", NORM_NFKC, decompose_to_last,
                  decompose_to_last_tests, LEN(decompose_to_last_tests));
}

static Int last_boundary_f(NormReorderBuffer *rb, NormForm f, Alloc *a, Str s,
                           Str *out) {
    (void)rb;
    (void)out;
    return burrow__norm_last_boundary(f, exact(a, s));
}

static void TestLastBoundary(TestingT *t) {
    run_pos_tests(t, "TestLastBoundary", NORM_NFC, last_boundary_f, last_boundary_tests,
                  LEN(last_boundary_tests));
}

static void run_span_tests(TestingT *t, const char *name, NormForm f,
                           const NormSpanTest *tests, Int n) {
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    for (Int i = 0; i < n; i++) {
        const NormSpanTest *tc = &tests[i];
        Str in = lit(tc->input, tc->input_len);
        Error err = BURROW_NO_ERROR;
        Int got = burrow__norm_span(f, exact(a, in), (tc->at_eof != 0), &err);
        if (got != tc->n || !err_is(err, tc->err)) {
            testing_t_errorf_v(t, "Bytes/%s/%d=%+q/atEOF=%t: got %d, %v; want %d, %d",
                               name, i, in, (tc->at_eof != 0), got, err, tc->n,
                               (Int)tc->err);
            continue; /* Don't do the String variant if the Bytes variant failed. */
        }
        got = burrow__norm_span_string(f, in, (tc->at_eof != 0), &err);
        if (got != tc->n || !err_is(err, tc->err))
            testing_t_errorf_v(t, "String/%s/%d=%+q/atEOF=%t: got %d, %v; want %d, %d",
                               name, i, in, (tc->at_eof != 0), got, err, tc->n,
                               (Int)tc->err);
    }
    arena_free(&ar);
}

static void TestSpan(TestingT *t) {
    run_span_tests(t, "NFD", NORM_NFD, quick_span_tests, LEN(quick_span_tests));
    run_span_tests(t, "NFD", NORM_NFD, quick_span_nfd_tests, LEN(quick_span_nfd_tests));
    run_span_tests(t, "NFC", NORM_NFC, quick_span_tests, LEN(quick_span_tests));
    run_span_tests(t, "NFC", NORM_NFC, quick_span_nfc_tests, LEN(quick_span_nfc_tests));
}

static Int is_normal_f(NormReorderBuffer *rb, NormForm f, Alloc *a, Str s, Str *out) {
    (void)rb;
    (void)out;
    return burrow__norm_is_normal(f, exact(a, s)) ? 1 : 0;
}

static Int is_normal_string_f(NormReorderBuffer *rb, NormForm f, Alloc *a, Str s,
                              Str *out) {
    (void)rb;
    (void)a;
    (void)out;
    return burrow__norm_is_normal_string(f, s) ? 1 : 0;
}

#define POS(name, f, fn, tests) run_pos_tests(t, name, f, fn, tests, LEN(tests))

static void TestIsNormal(TestingT *t) {
    POS("TestIsNormalNFD1", NORM_NFD, is_normal_f, is_normal_tests);
    POS("TestIsNormalNFD2", NORM_NFD, is_normal_f, is_normal_nfd_tests);
    POS("TestIsNormalNFC1", NORM_NFC, is_normal_f, is_normal_tests);
    POS("TestIsNormalNFC2", NORM_NFC, is_normal_f, is_normal_nfc_tests);
    POS("TestIsNormalNFKD1", NORM_NFKD, is_normal_f, is_normal_tests);
    POS("TestIsNormalNFKD2", NORM_NFKD, is_normal_f, is_normal_nfd_tests);
    POS("TestIsNormalNFKD3", NORM_NFKD, is_normal_f, is_normal_nfkx_tests);
    POS("TestIsNormalNFKC1", NORM_NFKC, is_normal_f, is_normal_tests);
    POS("TestIsNormalNFKC2", NORM_NFKC, is_normal_f, is_normal_nfc_tests);
    POS("TestIsNormalNFKC3", NORM_NFKC, is_normal_f, is_normal_nfkx_tests);
}

static void TestIsNormalString(TestingT *t) {
    POS("TestIsNormalNFD1", NORM_NFD, is_normal_string_f, is_normal_tests);
    POS("TestIsNormalNFD2", NORM_NFD, is_normal_string_f, is_normal_nfd_tests);
    POS("TestIsNormalNFC1", NORM_NFC, is_normal_string_f, is_normal_tests);
    POS("TestIsNormalNFC2", NORM_NFC, is_normal_string_f, is_normal_nfc_tests);
}

/* appendFunc: what fn makes of out followed by s, normalised to f. */
typedef struct AppendFn {
    Str (*fn)(Alloc *a, NormForm f, Slice out, Str s, Int arg);
    Int arg;
} AppendFn;

static const NormAppendTest *const norm_tests[4] = {
    append_tests_nfc, append_tests_nfd, append_tests_nfkc, append_tests_nfkd};
static const Int norm_tests_len[4] = {APPEND_TESTS_NFC_LEN, APPEND_TESTS_NFD_LEN,
                                      APPEND_TESTS_NFKC_LEN, APPEND_TESTS_NFKD_LEN};

static void run_append_tests(TestingT *t, const char *name, NormForm f, AppendFn fn) {
    const NormAppendTest *tests = norm_tests[f];
    for (Int i = 0; i < norm_tests_len[f]; i++) {
        Arena ar;
        arena_init(&ar, NULL, 0);
        Alloc *a = arena_allocator(&ar);
        const NormAppendTest *test = &tests[i];
        Str left = lit(test->left, test->left_len);
        Str right = lit(test->right, test->right_len);
        Str want = lit(test->out, test->out_len);
        Str id = cat(a, left, right);

        Str have = fn.fn(a, f, exact(a, left), right, fn.arg);
        if (have.len != want.len)
            testing_t_errorf_v(t, "%s/%s/%d: %+q: length is %d; want %d", name, fstr[f],
                               i, id, have.len, want.len);
        if (!str_eq(have, want))
            testing_t_errorf_v(t, "%s/%s/%d: %+q:\nwas  %+q; \nwant %+q", name, fstr[f],
                               i, id, have, want);

        /* Bootstrap by normalizing input. Ensures that the various variants
         * behave the same. */
        for (NormForm g = NORM_NFC; g <= NORM_NFKD; g++) {
            if (f == g)
                continue;
            Str gwant = burrow__norm_string(a, g, id);
            Slice gout = burrow__norm_append_string(a, g, slice_nil(TYPE_BYTE), left);
            Str ghave = fn.fn(a, g, gout, right, fn.arg);
            if (!str_eq(ghave, gwant))
                testing_t_errorf_v(t, "%s/%s/%d/%s: %+q:\nwas  %+q; \nwant %+q", name,
                                   fstr[f], i, fstr[g], id, ghave, gwant);
        }
        arena_free(&ar);
    }
}

static void run_norm_tests(TestingT *t, const char *name, AppendFn fn) {
    for (NormForm f = NORM_NFC; f <= NORM_NFKD; f++)
        run_append_tests(t, name, f, fn);
}

static Str append_fn(Alloc *a, NormForm f, Slice out, Str s, Int arg) {
    (void)arg;
    return as_str(burrow__norm_append(a, f, out, exact(a, s)));
}

static Str append_string_fn(Alloc *a, NormForm f, Slice out, Str s, Int arg) {
    (void)arg;
    return as_str(burrow__norm_append_string(a, f, out, s));
}

static Str bytes_fn(Alloc *a, NormForm f, Slice out, Str s, Int arg) {
    (void)arg;
    return as_str(burrow__norm_bytes(a, f, exact(a, cat(a, as_str(out), s))));
}

static Str string_fn(Alloc *a, NormForm f, Slice out, Str s, Int arg) {
    (void)arg;
    return burrow__norm_string(a, f, cat(a, as_str(out), s));
}

static void TestAppend(TestingT *t) {
    run_norm_tests(t, "Append", (AppendFn){append_fn, 0});
}

static void TestAppendString(TestingT *t) {
    run_norm_tests(t, "AppendString", (AppendFn){append_string_fn, 0});
}

static void TestBytes(TestingT *t) {
    run_norm_tests(t, "Bytes", (AppendFn){bytes_fn, 0});
}

static void TestString(TestingT *t) {
    run_norm_tests(t, "String", (AppendFn){string_fn, 0});
}

/* ---------------------------------------------------------------- iter_test */

static Str iter_string_fn(Alloc *a, NormForm f, Slice out, Str s, Int arg) {
    (void)arg;
    return iter_all(a, f, cat(a, as_str(out), s), true);
}

static Str iter_bytes_fn(Alloc *a, NormForm f, Slice out, Str s, Int arg) {
    (void)arg;
    return iter_all(a, f, cat(a, as_str(out), s), false);
}

static void TestIterNext(TestingT *t) {
    run_norm_tests(t, "IterNext", (AppendFn){iter_string_fn, 0});
    run_norm_tests(t, "IterNext", (AppendFn){iter_bytes_fn, 0});
}

static void segment_test(TestingT *t, const char *name, NormForm f,
                         const NormSegmentTest *tests, Int n) {
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    NormIter iter;
    memset(&iter, 0, sizeof iter);
    for (Int i = 0; i < n; i++) {
        const NormSegmentTest *tt = &tests[i];
        burrow__norm_iter_init_string(&iter, f, lit(tt->in, tt->in_len));
        for (Int j = 0; j < tt->nout; j++) {
            Str seg = lit(tt->out[j].s, tt->out[j].len);
            if (seg.len == 0) {
                if (!burrow__norm_iter_done(&iter)) {
                    Str res = as_str(burrow__norm_iter_next(&iter));
                    testing_t_errorf_v(
                        t, "%s:%d:%d: expected Done()==true, found segment %+q", name,
                        i, j, res);
                }
                continue;
            }
            if (burrow__norm_iter_done(&iter))
                testing_t_errorf_v(t, "%s:%d:%d: Done()==true, want false", name, i, j);
            seg = burrow__norm_string(a, f, seg);
            Str res = as_str(burrow__norm_iter_next(&iter));
            if (!str_eq(res, seg))
                testing_t_errorf_v(t, "%s:%d:%d: segment was %+q (%d); want %+q (%d)",
                                   name, i, j, res, res.len, seg, seg.len);
        }
    }
    arena_free(&ar);
}

/* Note that, by design, segmentation is equal for composing and decomposing
 * forms. */
static void TestIterSegmentation(TestingT *t) {
    segment_test(t, "SegmentTestD", NORM_NFD, segment_tests, LEN(segment_tests));
    segment_test(t, "SegmentTestC", NORM_NFC, segment_tests, LEN(segment_tests));
    segment_test(t, "SegmentTestKD", NORM_NFKD, segment_tests_k, LEN(segment_tests_k));
    segment_test(t, "SegmentTestKC", NORM_NFKC, segment_tests_k, LEN(segment_tests_k));
}

static void TestIterSeek(TestingT *t) {
    NormIter it;
    memset(&it, 0, sizeof it);
    burrow__norm_iter_init_string(&it, NORM_NFC, BURROW_S("abc"));
    Error err = BURROW_NO_ERROR;
    burrow__norm_iter_seek(&it, 0, 3, &err);
    CHECK(BURROW_FAILED(err) &&
          str_eq(error_text(err), BURROW_S("norm: invalid whence")));
    burrow__norm_iter_seek(&it, -1, 0, &err);
    CHECK(BURROW_FAILED(err) &&
          str_eq(error_text(err), BURROW_S("norm: negative position")));
    CHECK_INT_EQ(burrow__norm_iter_seek(&it, 1, 0, &err), 1);
    CHECK(!BURROW_FAILED(err));
    CHECK(str_eq(as_str(burrow__norm_iter_next(&it)), BURROW_S("b")));
    CHECK_INT_EQ(burrow__norm_iter_seek(&it, 5, 1, &err), 3);
    CHECK(burrow__norm_iter_done(&it));
}

/* ---------------------------------------------------------- readwriter_test */

static const Int buf_sizes[] = {1,   2,   3,   4,   5,    6,    7,    8,
                                100, 101, 102, 103, 4000, 4001, 4002, 4003};

static Str read_fn(Alloc *a, NormForm f, Slice out, Str s, Int size) {
    Error last = BURROW_NO_ERROR;
    return read_all(a, f, cat(a, as_str(out), s), INT64_MAX, size, &last);
}

static Str write_fn(Alloc *a, NormForm f, Slice out, Str s, Int size) {
    return write_all(a, f, cat(a, as_str(out), s), size);
}

static void TestReader(TestingT *t) {
    for (Int i = 0; i < LEN(buf_sizes); i++)
        run_norm_tests(t, "TestReader", (AppendFn){read_fn, buf_sizes[i]});
}

static void TestWriter(TestingT *t) {
    for (Int i = 0; i < LEN(buf_sizes); i++)
        run_norm_tests(t, "TestWriter", (AppendFn){write_fn, buf_sizes[i]});
}

/* ----------------------------------------------------------- transform_test */

static void TestTransform(TestingT *t) {
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    Byte b[100];
    for (Int i = 0; i < LEN(transform_tests); i++) {
        const NormTransformTest *tt = &transform_tests[i];
        Str in = lit(tt->in, tt->in_len);
        Int n_src = 0;
        Error err = BURROW_NO_ERROR;
        Int n_dst = burrow__norm_transform((NormForm)tt->f,
                                           bytes_of(b, tt->dst_size, (Int)sizeof b),
                                           exact(a, in), tt->eof != 0, &n_src, &err);
        Str out = str_from_bytes(b, n_dst);
        Str want_out = lit(tt->out, tt->out_len);
        if (!str_eq(out, want_out) || !err_is(err, tt->err))
            testing_t_errorf_v(t, "%d: was %+q (%v); want %+q (%d)", i, out, err,
                               want_out, (Int)tt->err);
        Str norm = burrow__norm_string(a, (NormForm)tt->f, in);
        if (n_dst > norm.len || !str_eq(str_from_bytes(norm.p, n_dst), out))
            testing_t_errorf_v(t, "%d: incorrect normalization: was %+q; want %+q", i,
                               out, norm);
    }
    arena_free(&ar);
}

static Str trans_fn(Alloc *a, NormForm f, Slice out, Str s, Int size) {
    Byte *buf = mem_alloc_nozero(a, (size_t)size, 1);
    Slice b = exact(a, cat(a, as_str(out), s));
    Buf acc = {a, NULL, 0, 0};
    for (Int p = 0; p < b.len;) {
        Int ns = 0;
        Error err;
        Int nd = burrow__norm_transform(f, bytes_of(buf, size, size),
                                        bytes_of((Byte *)b.p + p, b.len - p, b.cap - p),
                                        true, &ns, &err);
        p += ns;
        buf_write(&acc, buf, nd);
    }
    return buf_str(&acc);
}

static void TestTransformNorm(TestingT *t) {
    static const Int sizes[] = {
        NORM_MAX_TRANSFORM_CHUNK_SIZE, (Int)3 * NORM_MAX_TRANSFORM_CHUNK_SIZE / 2,
        (Int)2 * NORM_MAX_TRANSFORM_CHUNK_SIZE, (Int)3 * NORM_MAX_TRANSFORM_CHUNK_SIZE,
        (Int)100 * NORM_MAX_TRANSFORM_CHUNK_SIZE};
    for (Int i = 0; i < LEN(sizes); i++)
        run_norm_tests(t, "Transform", (AppendFn){trans_fn, sizes[i]});
}

static void TestTransformer(TestingT *t) {
    TransformSpanningTransformer st = burrow__norm_transformer(NORM_NFD);
    Byte b[16];
    Int n_src = 0;
    Error err = BURROW_NO_ERROR;
    Str in = BURROW_S("\xC3\xB6");
    Int n = burrow__transform_call(burrow__transform_of(st), bytes_of(b, 16, 16),
                                   bytes_of((Byte *)(uintptr_t)in.p, in.len, in.len),
                                   true, &n_src, &err);
    CHECK(!BURROW_FAILED(err));
    CHECK(str_eq(str_from_bytes(b, n), BURROW_S("o\xCC\x88")));
    CHECK_INT_EQ(n_src, 2);
    n = burrow__transform_span(st, bytes_of((Byte *)(uintptr_t)in.p, in.len, in.len),
                               true, &err);
    CHECK_INT_EQ(n, 0);
    CHECK(burrow__transform_err_eq(err, burrow__transform_err_end_of_span));
}

/* --------------------------------------------------------------- the examples */

static bool equal_simple(Str a, Str b) {
    NormIter ia, ib;
    memset(&ia, 0, sizeof ia);
    memset(&ib, 0, sizeof ib);
    burrow__norm_iter_init_string(&ia, NORM_NFKD, a);
    burrow__norm_iter_init_string(&ib, NORM_NFKD, b);
    while (!burrow__norm_iter_done(&ia) && !burrow__norm_iter_done(&ib)) {
        if (!str_eq(as_str(burrow__norm_iter_next(&ia)),
                    as_str(burrow__norm_iter_next(&ib))))
            return false;
    }
    return burrow__norm_iter_done(&ia) && burrow__norm_iter_done(&ib);
}

static Int find_prefix(Str a, Str b) {
    Int i = 0;
    for (; i < a.len && i < b.len && a.p[i] < 0x80 && a.p[i] == b.p[i]; i++) {
    }
    return i;
}

static Str str_tail(Str s, Int i) {
    if (i == 0)
        return s;
    return str_from_bytes(s.p + i, s.len - i);
}

static bool equal_opt(Str a, Str b) {
    Int n = find_prefix(a, b);
    a = str_tail(a, n);
    b = str_tail(b, n);
    NormIter ia, ib;
    memset(&ia, 0, sizeof ia);
    memset(&ib, 0, sizeof ib);
    burrow__norm_iter_init_string(&ia, NORM_NFKD, a);
    burrow__norm_iter_init_string(&ib, NORM_NFKD, b);
    while (!burrow__norm_iter_done(&ia) && !burrow__norm_iter_done(&ib)) {
        if (!str_eq(as_str(burrow__norm_iter_next(&ia)),
                    as_str(burrow__norm_iter_next(&ib))))
            return false;
        Int m = find_prefix(str_tail(a, burrow__norm_iter_pos(&ia)),
                            str_tail(b, burrow__norm_iter_pos(&ib)));
        if (m != 0) {
            Error err;
            burrow__norm_iter_seek(&ia, m, 1, &err);
            burrow__norm_iter_seek(&ib, m, 1, &err);
        }
    }
    return burrow__norm_iter_done(&ia) && burrow__norm_iter_done(&ib);
}

static void TestExampleIter(TestingT *t) {
    static const bool want[] = {true, false, true, true, true, true};
    for (Int i = 0; i < LEN(compare_tests); i++) {
        Str a = lit(compare_tests[i][0].s, compare_tests[i][0].len);
        Str b = lit(compare_tests[i][1].s, compare_tests[i][1].len);
        bool r0 = equal_simple(a, b), r1 = equal_opt(a, b);
        if (r0 != want[i] || r1 != want[i])
            testing_t_errorf_v(t, "%d: %t %t; want %t %t", i, r0, r1, want[i], want[i]);
    }
}

static void TestExampleFormNextBoundary(TestingT *t) {
    static const char *const want[] = {"M", "e\xCC\x82", "l", "e\xCC\x81", "e"};
    Arena ar;
    arena_init(&ar, NULL, 0);
    Str s = burrow__norm_string(arena_allocator(&ar), NORM_NFD,
                                BURROW_S("M\xC3\xAAl\xC3\xA9"
                                         "e"));
    Int k = 0;
    for (Int i = 0; i < s.len; k++) {
        Int d = burrow__norm_next_boundary_in_string(NORM_NFC, str_tail(s, i), true);
        Str seg = str_from_bytes(s.p + i, d);
        if (k >= LEN(want) || !str_eq(seg, str_from_cstr(want[k])))
            testing_t_errorf_v(t, "segment %d is %+q", k, seg);
        i += d;
    }
    CHECK_INT_EQ(k, LEN(want));
    arena_free(&ar);
}

/* ------------------------------------------------------------- differential */

/* A record is a run of tagged values, as the generator writes them, kept only
 * as its FNV-1a hash, with the start of it kept as text for showing. */
typedef struct Rec {
    uint64_t h;
    Byte show[4096];
    Int show_len;
} Rec;

static void rec_init(Rec *r) {
    r->h = 14695981039346656037ULL;
    r->show_len = 0;
}

static void rec_write(Rec *r, const void *p, Int n) {
    const Byte *b = p;
    for (Int i = 0; i < n; i++) {
        r->h ^= b[i];
        r->h *= 1099511628211ULL;
    }
    Int room = (Int)sizeof r->show - r->show_len;
    Int k = n < room ? n : room;
    if (k > 0) {
        memcpy(r->show + r->show_len, b, (size_t)k);
        r->show_len += k;
    }
}

static void rec_cstr(Rec *r, const char *s) {
    rec_write(r, s, (Int)strlen(s));
}

static void rec_int(Rec *r, Int n) {
    char tmp[24];
    int i = (int)sizeof tmp;
    uint64_t u = n < 0 ? (uint64_t)0 - (uint64_t)n : (uint64_t)n;
    do {
        tmp[--i] = (char)('0' + u % 10);
        u /= 10;
    } while (u != 0);
    if (n < 0)
        tmp[--i] = '-';
    rec_write(r, tmp + i, (Int)sizeof tmp - i);
}

static void rec_n(Rec *r, const char *tag, Int n) {
    rec_cstr(r, tag);
    rec_int(r, n);
    rec_cstr(r, ";");
}

static void rec_bool(Rec *r, const char *tag, bool b) {
    rec_n(r, tag, b ? 1 : 0);
}

static void rec_s(Rec *r, const char *tag, const void *p, Int n) {
    rec_cstr(r, tag);
    rec_int(r, n);
    rec_cstr(r, ":");
    rec_write(r, p, n);
}

static void rec_e(Rec *r, const char *tag, Error err) {
    rec_cstr(r, tag);
    if (!BURROW_FAILED(err)) {
        rec_cstr(r, "-;");
        return;
    }
    Str s = error_text(err);
    rec_write(r, s.p, s.len);
    rec_cstr(r, ";");
}

typedef struct Desc {
    Rec *r;
    Alloc *a;
    NormForm f;
    Str s;
    Slice b;
    bool eof;
    Int sz;
    Int split;
    Byte dst[NORM_MAX_TRANSFORM_CHUNK_SIZE];
} Desc;

/* catch runs op and records the panic, if it panics. */
static void catch (Desc *d, void (*op)(Desc *d)) {
    BURROW_TRY {
        op(d);
    }
    BURROW_CATCH(p) {
        rec_cstr(d->r, "PANIC");
        Str s = panic_text(p);
        rec_write(d->r, s.p, s.len);
        rec_cstr(d->r, ";");
    }
    BURROW_TRY_END;
}

static void op_string(Desc *d) {
    Str out = burrow__norm_string(d->a, d->f, d->s);
    rec_s(d->r, "S", out.p, out.len);
}

static void op_bytes(Desc *d) {
    Slice out = burrow__norm_bytes(d->a, d->f, d->b);
    rec_s(d->r, "B", out.p, out.len);
}

static void op_is_normal(Desc *d) {
    rec_bool(d->r, "N", burrow__norm_is_normal(d->f, d->b));
}

static void op_is_normal_string(Desc *d) {
    rec_bool(d->r, "NS", burrow__norm_is_normal_string(d->f, d->s));
}

static void op_quick_span(Desc *d) {
    rec_n(d->r, "Q", burrow__norm_quick_span(d->f, d->b));
}

static void op_quick_span_string(Desc *d) {
    rec_n(d->r, "QS", burrow__norm_quick_span_string(d->f, d->s));
}

static void op_span(Desc *d) {
    Error err = BURROW_NO_ERROR;
    Int n = burrow__norm_span(d->f, d->b, d->eof, &err);
    rec_n(d->r, "SP", n);
    rec_e(d->r, "", err);
}

static void op_span_string(Desc *d) {
    Error err = BURROW_NO_ERROR;
    Int n = burrow__norm_span_string(d->f, d->s, d->eof, &err);
    rec_n(d->r, "SPS", n);
    rec_e(d->r, "", err);
}

static void op_next_boundary(Desc *d) {
    rec_n(d->r, "NB", burrow__norm_next_boundary(d->f, d->b, d->eof));
}

static void op_next_boundary_string(Desc *d) {
    rec_n(d->r, "NBS", burrow__norm_next_boundary_in_string(d->f, d->s, d->eof));
}

static void op_first_boundary(Desc *d) {
    rec_n(d->r, "FB", burrow__norm_first_boundary(d->f, d->b));
}

static void op_first_boundary_string(Desc *d) {
    rec_n(d->r, "FBS", burrow__norm_first_boundary_in_string(d->f, d->s));
}

static void op_last_boundary(Desc *d) {
    rec_n(d->r, "LB", burrow__norm_last_boundary(d->f, d->b));
}

static void op_iter(Desc *d) {
    NormIter it;
    memset(&it, 0, sizeof it);
    burrow__norm_iter_init(&it, d->f, d->b);
    while (!burrow__norm_iter_done(&it)) {
        Slice seg = burrow__norm_iter_next(&it);
        rec_s(d->r, "I", seg.p, seg.len);
    }
}

static void op_iter_string(Desc *d) {
    NormIter it;
    memset(&it, 0, sizeof it);
    burrow__norm_iter_init_string(&it, d->f, d->s);
    while (!burrow__norm_iter_done(&it)) {
        Slice seg = burrow__norm_iter_next(&it);
        rec_s(d->r, "IS", seg.p, seg.len);
    }
}

static void op_transform(Desc *d) {
    Int n_src = 0;
    Error err = BURROW_NO_ERROR;
    Int n_dst = burrow__norm_transform(
        d->f, bytes_of(d->dst, d->sz, (Int)sizeof d->dst), d->b, d->eof, &n_src, &err);
    rec_n(d->r, "T", n_dst);
    rec_n(d->r, "", n_src);
    rec_e(d->r, "", err);
    rec_s(d->r, "", d->dst, n_dst);
}

static Slice tail(Slice b, Int i) {
    if (i == 0)
        return b;
    return bytes_of((Byte *)b.p + i, b.len - i, b.cap - i);
}

static void op_append(Desc *d) {
    Str left = str_from_bytes(d->s.p, d->split);
    Slice out = burrow__norm_bytes(d->a, d->f, exact(d->a, left));
    Slice res = burrow__norm_append(d->a, d->f, out, tail(d->b, d->split));
    rec_s(d->r, "A", res.p, res.len);
}

static void op_append_raw(Desc *d) {
    Str left = str_from_bytes(d->s.p, d->split);
    Slice res =
        burrow__norm_append(d->a, d->f, exact(d->a, left), tail(d->b, d->split));
    rec_s(d->r, "AR", res.p, res.len);
}

static void op_append_string(Desc *d) {
    Str left = str_from_bytes(d->s.p, d->split);
    Slice out = burrow__norm_bytes(d->a, d->f, exact(d->a, left));
    Slice res = burrow__norm_append_string(d->a, d->f, out, str_tail(d->s, d->split));
    rec_s(d->r, "AS", res.p, res.len);
}

static void op_reader(Desc *d) {
    Error last = BURROW_NO_ERROR;
    Str out = read_all(d->a, d->f, d->s, d->sz, d->sz, &last);
    rec_e(d->r, "RE", last);
    rec_s(d->r, "R", out.p, out.len);
}

static void op_writer(Desc *d) {
    Str out = write_all(d->a, d->f, d->s, d->sz);
    rec_s(d->r, "W", out.p, out.len);
}

static void describe(Rec *r, NormForm f, Str s) {
    Arena ar;
    arena_init(&ar, NULL, 0);
    Desc *d = mem_alloc(arena_allocator(&ar), sizeof *d, _Alignof(Desc));
    d->r = r;
    d->a = arena_allocator(&ar);
    d->f = f;
    d->s = s;
    d->b = exact(d->a, s);
    catch (d, op_string);
    catch (d, op_bytes);
    catch (d, op_is_normal);
    catch (d, op_is_normal_string);
    catch (d, op_quick_span);
    catch (d, op_quick_span_string);
    for (int e = 0; e < 2; e++) {
        d->eof = e == 1;
        catch (d, op_span);
        catch (d, op_span_string);
        catch (d, op_next_boundary);
        catch (d, op_next_boundary_string);
    }
    catch (d, op_first_boundary);
    catch (d, op_first_boundary_string);
    catch (d, op_last_boundary);
    catch (d, op_iter);
    catch (d, op_iter_string);

    static const Int dst_sizes[] = {0, 1, 2,  3,  4,
                                    5, 8, 13, 32, NORM_MAX_TRANSFORM_CHUNK_SIZE};
    for (Int i = 0; i < LEN(dst_sizes); i++) {
        for (int e = 0; e < 2; e++) {
            d->sz = dst_sizes[i];
            d->eof = e == 1;
            catch (d, op_transform);
        }
    }

    Int nsplit = s.len <= 64 ? s.len + 1 : 17;
    for (Int k = 0; k < nsplit; k++) {
        d->split = s.len <= 64 ? k : k * s.len / 16;
        catch (d, op_append);
        catch (d, op_append_raw);
        catch (d, op_append_string);
    }

    static const Int io_sizes[] = {1, 3, 7, 4000};
    for (Int i = 0; i < LEN(io_sizes); i++) {
        d->sz = io_sizes[i];
        catch (d, op_reader);
        catch (d, op_writer);
    }
    arena_free(&ar);
}

static void show(TestingT *t, const Rec *r) {
    if (getenv("BURROW_XTEXT_SHOW") != NULL)
        testing_t_logf_v(t, "record starts %q", str_from_bytes(r->show, r->show_len));
}

static void TestDifferential(TestingT *t) {
    int shown = 0;
    Arena ar;
    arena_init(&ar, NULL, 0);
    Rec *r = mem_alloc(arena_allocator(&ar), sizeof *r, _Alignof(Rec));
    for (Int i = 0; i < LEN(norm_corpus); i++) {
        Str s = lit(norm_corpus[i].s, norm_corpus[i].len);
        for (NormForm f = NORM_NFC; f <= NORM_NFKD; f++) {
            rec_init(r);
            describe(r, f, s);
            if (r->h != norm_corpus_hash[i][f]) {
                testing_t_errorf_v(t, "corpus %d/%d (%s) %+q: differs from Go", i,
                                   (Int)f, fstr[f], s);
                if (shown++ == 0)
                    show(t, r);
            }
        }
    }
    arena_free(&ar);
}

static Int sweep_strings(Rune r, Byte (*out)[16], Int *lens) {
    Byte s[4];
    Int n = utf8_encode_rune(bytes_of(s, 4, 4), r);
    memcpy(out[0], s, (size_t)n);
    lens[0] = n;
    out[1][0] = 'a';
    memcpy(out[1] + 1, s, (size_t)n);
    lens[1] = n + 1;
    memcpy(out[2], s, (size_t)n);
    memcpy(out[2] + n, "\xCC\x96\xCC\x81", 4);
    lens[2] = n + 4;
    if ((r >= 0x1100 && r < 0x1200) || (r >= 0xa960 && r < 0xa980) ||
        (r >= 0xac00 && r < 0xd800) || (r >= 0x3130 && r < 0x3190)) {
        memcpy(out[3], "\xE1\x84\x80", 3);
        memcpy(out[3] + 3, s, (size_t)n);
        memcpy(out[3] + 3 + n, "\xE1\x86\xA8", 3);
        lens[3] = n + 6;
        return 4;
    }
    return 3;
}

static void sweep_record(Rec *rec, Alloc *a, Rune r, NormForm f) {
    Byte s[4];
    Int n = utf8_encode_rune(bytes_of(s, 4, 4), r);
    NormProperties p = burrow__norm_properties_string(f, str_from_bytes(s, n));
    rec_n(rec, "P", burrow__norm_size(p));
    rec_n(rec, "", burrow__norm_ccc(p));
    rec_n(rec, "", burrow__norm_lead_ccc(p));
    rec_n(rec, "", burrow__norm_trail_ccc(p));
    rec_bool(rec, "", burrow__norm_boundary_before(p));
    rec_bool(rec, "", burrow__norm_boundary_after(p));
    Slice dec = burrow__norm_decomposition(p);
    rec_s(rec, "", dec.p, dec.len);
    Byte xs[4][16];
    Int lens[4];
    Int m = sweep_strings(r, xs, lens);
    for (Int i = 0; i < m; i++) {
        Str x = str_from_bytes(xs[i], lens[i]);
        Str out = burrow__norm_string(a, f, x);
        rec_s(rec, "S", out.p, out.len);
        rec_bool(rec, "", burrow__norm_is_normal_string(f, x));
    }
}

static void TestSweep(TestingT *t) {
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    Arena sar;
    arena_init(&sar, NULL, 0);
    Alloc *scratch = arena_allocator(&sar);
    Rec *rec = mem_alloc(a, sizeof *rec, _Alignof(Rec));
    for (Rune blk = 0; blk < 0x110; blk++) {
        for (NormForm f = NORM_NFC; f <= NORM_NFKD; f++) {
            rec_init(rec);
            for (Rune r = blk << 12; r < (blk + 1) << 12; r++) {
                if (r >= 0xd800 && r < 0xe000)
                    continue;
                sweep_record(rec, scratch, r, f);
                rec->show_len = 0;
            }
            arena_reset(&sar);
            if (rec->h != norm_sweep_hash[blk][f])
                testing_t_errorf_v(t, "block %X (%s): differs from Go", blk, fstr[f]);
        }
    }
    arena_free(&sar);
    arena_free(&ar);
}

#define TESTS(X)                                                                       \
    X(TestFlush)                                                                       \
    X(TestInsert)                                                                      \
    X(TestDecomposition)                                                               \
    X(TestComposition)                                                                 \
    X(TestProperties)                                                                  \
    X(TestDecomposeSegment)                                                            \
    X(TestFirstBoundary)                                                               \
    X(TestNextBoundary)                                                                \
    X(TestDecomposeToLastBoundary)                                                     \
    X(TestLastBoundary)                                                                \
    X(TestSpan)                                                                        \
    X(TestIsNormal)                                                                    \
    X(TestIsNormalString)                                                              \
    X(TestAppend)                                                                      \
    X(TestAppendString)                                                                \
    X(TestBytes)                                                                       \
    X(TestString)                                                                      \
    X(TestIterNext)                                                                    \
    X(TestIterSegmentation)                                                            \
    X(TestIterSeek)                                                                    \
    X(TestReader)                                                                      \
    X(TestWriter)                                                                      \
    X(TestTransform)                                                                   \
    X(TestTransformNorm)                                                               \
    X(TestTransformer)                                                                 \
    X(TestExampleIter)                                                                 \
    X(TestExampleFormNextBoundary)                                                     \
    X(TestDifferential)                                                                \
    X(TestSweep)

TESTING_MAIN(TESTS)
